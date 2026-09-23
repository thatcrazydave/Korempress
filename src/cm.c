/*
 * A context-mixing entropy coder, written from scratch.
 *
 * Binary arithmetic coder (carryless, LZMA-style renormalisation) driven by a
 * logistic mixture of several context models. Each byte is coded as 8 binary
 * decisions down a 256-leaf tree; every model predicts the next bit from its
 * own context, and a learned mixer combines them in the logistic domain.
 *
 * No LZ stage, deliberately: this is the entropy half only, so it can be
 * compared against another entropy coder rather than against a whole format.
 *
 *   cm c <in> <out> [stride]    stride>0 enables the two image contexts
 *   cm d <in> <out>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/* ---------- logistic helpers ---------- */
static short STRETCH[4096];
static int   SQUASH[4096];        /* index: x+2048 -> 12-bit probability */

static int squash_calc(int x) {
    if (x <= -2047) return 1;
    if (x >=  2047) return 4095;
    double v = 4096.0 / (1.0 + exp(-x / 256.0));
    int r = (int)(v + 0.5);
    return r < 1 ? 1 : (r > 4095 ? 4095 : r);
}
static void tables_init(void) {
    for (int x = -2048; x < 2048; x++) SQUASH[x + 2048] = squash_calc(x);
    int pi = 0;
    for (int x = -2047; x <= 2047; x++) {          /* stretch = inverse squash */
        int v = squash_calc(x);
        for (; pi <= v && pi < 4096; pi++) STRETCH[pi] = (short)x;
    }
    for (; pi < 4096; pi++) STRETCH[pi] = 2047;
}
static inline int squash(int x) {
    if (x < -2047) x = -2047;
    if (x >  2047) x =  2047;
    return SQUASH[x + 2048];
}

/* ---------- binary arithmetic coder ---------- */
typedef struct {
    FILE *f;
    uint64_t low;
    uint32_t range, code;
    uint8_t  cache;
    int64_t  carry_count;
    int      first;
} RC;

static void rc_enc_init(RC *r, FILE *f) {
    r->f = f; r->low = 0; r->range = 0xFFFFFFFFu;
    r->cache = 0; r->carry_count = 0; r->first = 1;
}
static void rc_shift_low(RC *r) {
    if ((uint32_t)(r->low >> 32) || (uint32_t)r->low < 0xFF000000u) {
        if (!r->first) fputc(r->cache + (uint8_t)(r->low >> 32), r->f);
        r->first = 0;
        for (; r->carry_count; r->carry_count--)
            fputc(0xFF + (uint8_t)(r->low >> 32), r->f);
        r->cache = (uint8_t)(r->low >> 24);
    } else {
        r->carry_count++;
    }
    r->low = (r->low << 8) & 0xFFFFFFFFull;
}
/* p1 is P(bit==1) on a 12-bit scale; the coder never updates it. */
static inline void rc_encode(RC *r, int bit, int p1) {
    if (p1 < 1) p1 = 1;
    if (p1 > 4095) p1 = 4095;
    uint32_t bound = (r->range >> 12) * (uint32_t)(4096 - p1);
    if (!bit) {
        r->range = bound;
    } else {
        r->low += bound;
        r->range -= bound;
    }
    while (r->range < (1u << 24)) { rc_shift_low(r); r->range <<= 8; }
}
static void rc_enc_flush(RC *r) { for (int i = 0; i < 5; i++) rc_shift_low(r); }

static void rc_dec_init(RC *r, FILE *f) {
    r->f = f; r->range = 0xFFFFFFFFu; r->code = 0;
    for (int i = 0; i < 4; i++) { int c = fgetc(f); r->code = (r->code << 8) | (uint32_t)(c < 0 ? 0 : c); }
}
static inline int rc_decode(RC *r, int p1) {
    if (p1 < 1) p1 = 1;
    if (p1 > 4095) p1 = 4095;
    uint32_t bound = (r->range >> 12) * (uint32_t)(4096 - p1);
    int bit;
    if (r->code < bound) { r->range = bound; bit = 0; }
    else { r->code -= bound; r->range -= bound; bit = 1; }
    while (r->range < (1u << 24)) {
        int c = fgetc(r->f);
        r->code = (r->code << 8) | (uint32_t)(c < 0 ? 0 : c);
        r->range <<= 8;
    }
    return bit;
}

/* ---------- models ---------- */
#define NIN      6                 /* o0,o1,o2,o3,above,left-channel */
#define H2BITS   22
#define H3BITS   22
#define HABITS   22
#define HLBITS   22
#define TSIZE(b) (1u << (b))

static uint16_t *t0, *t1, *t2, *t3, *ta, *tl;
static int      *wts;              /* mixer weights, 16.16, one set per tree node */

static inline uint32_t hsh(uint32_t a, uint32_t b, uint32_t bits) {
    uint32_t h = a * 2654435761u ^ b * 0x9E3779B1u;
    h ^= h >> 15; h *= 0x85EBCA6Bu; h ^= h >> 13;
    return h & (TSIZE(bits) - 1);
}
static uint16_t *alloc_tab(uint32_t n) {
    uint16_t *t = (uint16_t *)malloc((size_t)n * sizeof(uint16_t));
    for (uint32_t i = 0; i < n; i++) t[i] = 2048;
    return t;
}

typedef struct { uint16_t *p[NIN]; int st[NIN]; int node; } Ctx;

/* Single pass over one byte, shared by encoder and decoder. */
static inline int code_byte(RC *rc, int encoding, int byte,
                            uint32_t c1, uint32_t c2, uint32_t c3,
                            uint32_t cabove, uint32_t cleft)
{
    int node = 1;
    for (int i = 7; i >= 0; i--) {
        uint16_t *pp[NIN];
        pp[0] = &t0[node];
        pp[1] = &t1[(c1 << 8) | (uint32_t)node];
        pp[2] = &t2[hsh(c2, (uint32_t)node, H2BITS)];
        pp[3] = &t3[hsh(c3, (uint32_t)node, H3BITS)];
        pp[4] = &ta[hsh(cabove, (uint32_t)node, HABITS)];
        pp[5] = &tl[hsh(cleft, (uint32_t)node, HLBITS)];

        int st[NIN];
        int64_t dot = 0;
        int *w = &wts[node * NIN];
        for (int k = 0; k < NIN; k++) { st[k] = STRETCH[*pp[k]]; dot += (int64_t)w[k] * st[k]; }
        int pm = squash((int)(dot >> 16));

        int bit;
        if (encoding) { bit = (byte >> i) & 1; rc_encode(rc, bit, pm); }
        else            bit = rc_decode(rc, pm);

        /* Gradient step in the logistic domain. The shift is the learning
           rate: too large and the weights diverge and output EXPANDS. */
        int err = ((bit << 12) - pm) * 7;
        for (int k = 0; k < NIN; k++) {
            int nw = w[k] + ((st[k] * err + 0x8000) >> 16);
            if (nw >  (1 << 20)) nw =  (1 << 20);
            if (nw < -(1 << 20)) nw = -(1 << 20);
            w[k] = nw;
        }
        for (int k = 0; k < NIN; k++) {             /* counter updates */
            int c = *pp[k];
            *pp[k] = (uint16_t)(c + (((bit << 12) - c) >> 5));
        }
        node = (node << 1) | bit;
    }
    return node & 0xFF;
}

static void models_init(void) {
    t0 = alloc_tab(256);
    t1 = alloc_tab(256 * 256);
    t2 = alloc_tab(TSIZE(H2BITS));
    t3 = alloc_tab(TSIZE(H3BITS));
    ta = alloc_tab(TSIZE(HABITS));
    tl = alloc_tab(TSIZE(HLBITS));
    wts = (int *)malloc(256 * NIN * sizeof(int));
    for (int i = 0; i < 256 * NIN; i++) wts[i] = (1 << 16) / NIN;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "cm c|d in out [stride]\n"); return 2; }
    tables_init(); models_init();
    int stride = (argc > 4) ? atoi(argv[4]) : 0;

    FILE *in = fopen(argv[2], "rb"), *out = fopen(argv[3], "wb");
    if (!in || !out) { perror("open"); return 1; }

    if (argv[1][0] == 'c') {
        fseek(in, 0, SEEK_END); long n = ftell(in); fseek(in, 0, SEEK_SET);
        uint8_t *buf = (uint8_t *)malloc((size_t)n);
        if (fread(buf, 1, (size_t)n, in) != (size_t)n) { perror("read"); return 1; }
        fwrite(&n, sizeof(long), 1, out);
        fwrite(&stride, sizeof(int), 1, out);
        RC rc; rc_enc_init(&rc, out);
        for (long i = 0; i < n; i++) {
            uint32_t b1 = i >= 1 ? buf[i-1] : 0, b2 = i >= 2 ? buf[i-2] : 0, b3 = i >= 3 ? buf[i-3] : 0;
            uint32_t ab = (stride > 0 && i >= stride) ? buf[i-stride] : 0;
            uint32_t lf = (stride > 0 && i >= stride+1) ? buf[i-stride-1] : 0;
            code_byte(&rc, 1, buf[i], b1, (b1<<8)|b2, (b1<<16)|(b2<<8)|b3, (ab<<8)|b1, (ab<<8)|lf);
        }
        rc_enc_flush(&rc);
    } else {
        long n; int st;
        if (fread(&n, sizeof(long), 1, in) != 1) return 1;
        if (fread(&st, sizeof(int), 1, in) != 1) return 1;
        stride = st;
        uint8_t *buf = (uint8_t *)malloc((size_t)n);
        RC rc; rc_dec_init(&rc, in);
        for (long i = 0; i < n; i++) {
            uint32_t b1 = i >= 1 ? buf[i-1] : 0, b2 = i >= 2 ? buf[i-2] : 0, b3 = i >= 3 ? buf[i-3] : 0;
            uint32_t ab = (stride > 0 && i >= stride) ? buf[i-stride] : 0;
            uint32_t lf = (stride > 0 && i >= stride+1) ? buf[i-stride-1] : 0;
            buf[i] = (uint8_t)code_byte(&rc, 0, 0, b1, (b1<<8)|b2, (b1<<16)|(b2<<8)|b3, (ab<<8)|b1, (ab<<8)|lf);
        }
        fwrite(buf, 1, (size_t)n, out);
    }
    fclose(in); fclose(out);
    return 0;
}
