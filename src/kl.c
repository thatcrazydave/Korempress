/*
 * KL — a fast lossless codec: LZ77 matching plus canonical Huffman.
 *
 * Built for DECODE SPEED. The context-mixing coder (cm.c) predicts every bit
 * and pays for it in both directions; this one does the opposite trade — find
 * repeats, entropy-code the tokens, and decode through a lookup table so the
 * hot loop is a table read and a memory copy.
 *
 * Format, per block:
 *   [u32 rawLen][u32 nSeq][u32 litLen][huff tables][sequences][literals]
 * A sequence is (run of literals, match length, match offset). Offsets use a
 * bucket code plus raw bits, so the alphabet stays small enough to decode from
 * one table while the window stays large.
 *
 *   kl c <in> <out> [level]     level 1..9, default 6
 *   kl d <in> <out>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

/* Each rung is a measured point on the ratio-vs-speed frontier. Before this
   table levels 2/3 and 4/5/6 were duplicates and level 7 was dominated: its
   deep-chain greedy cost more than the dynamic program and compressed less. */
static const struct { int chain, nice, lazy, dp; } LEVELS[10] = {
    {   0,   0, 0, 0 },
    {   4,  32, 0, 0 },  {  16,  32, 0, 0 },  {  64, 128, 1, 0 },
    {  16, 128, 1, 1 },  {  32, 128, 1, 1 },  {  64, 128, 1, 1 },
    {  64, 258, 1, 1 },  { 256, 258, 1, 1 },  {4096, 258, 1, 1 },
};

/* Chain depth is not convex on real data: 64 and 4096 both beat 256 on
   document streams, which made level 8 compress 1.5% WORSE than level 7. The
   top two levels therefore run several (chain, rounds) configurations and keep
   the smallest, each set containing the one below it so they cannot lose.
   Levels 4-7 keep a single configuration: their residual inversions are ~0.2%
   and covering them cost 5x the encode time, which is the worse trade. Each
   configuration restarts the price iteration, because seeding it from a
   shallower chain settles on a worse fixed point (257,105 -> 266,307). */
static const struct { int chain, rounds; } CFG[10][3] = {
    {{0,0}}, {{0,0}}, {{0,0}}, {{0,0}},
    {{ 16,1}},
    {{ 32,2}},
    {{ 64,3}},
    {{ 64,4}},
    {{ 64,4},{256,3},{4096,3}},
    {{ 64,4},{256,3},{4096,8}},
};
static const int NCFG[10] = { 0,0,0,0, 1,1,1,1, 3,3 };

/* A 4096-deep chain over a 16 MB input costs far more than it returns. */
#define DEEP_CAP 512

/* Above this the dynamic program is not affordable, so `deep` carries the
   effort instead. Both columns were set from measured frontiers, not guessed. */
#define DP_MAX_BYTES (8u << 20)
#define USE_DP(level, n) (LEVELS[level].dp && (size_t)(n) <= DP_MAX_BYTES)

#define WINDOW_LOG   24                 /* 16 MB window */
#define WINDOW       (1u << WINDOW_LOG)
#define HASH_LOG     18
#define HASH_SIZE    (1u << HASH_LOG)
#define MIN_MATCH    4
#define BLOCK_SIZE   (4u << 20)

#define NLIT   256
#ifndef NCTX
#define NCTX   4                     /* literal tables, chosen by the previous literal */
#endif
#define LCTX(prev) (NCTX == 1 ? 0 : ((prev) >> (8 - NCTX_BITS)))
#define NCTX_BITS (NCTX == 1 ? 0 : (NCTX == 2 ? 1 : 2))
#define NLL    64                        /* literal-run-length codes */
#define NML    64                        /* match-length codes */
/* Bumped whenever the bitstream changes. A decoder that does not know the
   version must REFUSE the input — the previous format carried only a back-end
   tag, so an older build read a newer file as a Huffman block, produced zero
   bytes and exited 0. Silent truncation is the worst possible failure for a
   compressor, so the version is checked and the exit status is meaningful. */
#define KL_FORMAT 2

/* A range decoder reads zeros past the end of its input, so a truncated block
   decodes to a full-length buffer of plausible garbage and reports success.
   Four bytes of FNV-1a over the original make that detectable. */
static uint32_t kl_hash(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}
#define KL_TAG(backend) ((uint8_t)((KL_FORMAT << 4) | (backend)))

#define NREP   4                         /* recent offsets kept, brotli-style ring */
#define NSD    16                        /* brotli short distance codes */
#define NOFF   (NSD + 69)                /* short codes then bucket codes */
#define MAXSYM 256

static const int8_t KDCI[NSD] = {0,1,2,3,0,0,0,0,0,0,1,1,1,1,1,1};
static const int8_t KDCO[NSD] = {0,0,0,0,-1,1,-2,2,-3,3,-1,1,-2,2,-3,3};
#define SDINIT { 4, 11, 15, 16 }
static inline int sd_find(const int32_t *r, uint32_t off) {
    for (int c = 0; c < NSD; c++) if ((int32_t)off == r[KDCI[c]] + KDCO[c]) return c;
    return -1;
}
static inline void sd_push(int32_t *r, uint32_t off, int code) {
    if (code == 0) return;
    r[3] = r[2]; r[2] = r[1]; r[1] = r[0]; r[0] = (int32_t)off;
}
static inline uint32_t ld32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t ld64(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* Bytes shared by a and b, up to cap. Little-endian: the low set bit of the
   XOR names the first byte that differs. */
static inline size_t match_len(const uint8_t *a, const uint8_t *b, size_t cap) {
    size_t l = 0;
    while (l + 8 <= cap) {
        uint64_t x = ld64(a + l) ^ ld64(b + l);
        if (x) return l + ((size_t)__builtin_ctzll(x) >> 3);
        l += 8;
    }
    while (l < cap && a[l] == b[l]) l++;
    return l;
}
static inline void st32(void *p, uint32_t v) { memcpy(p, &v, 4); }

/* ---------------- bit I/O ---------------- */
typedef struct { uint8_t *p, *end; uint64_t acc; int nbits; } BW;
static void bw_init(BW *w, uint8_t *buf, size_t n) { w->p = buf; w->end = buf + n; w->acc = 0; w->nbits = 0; }
static inline void bw_put(BW *w, uint32_t v, int n) {
    w->acc |= (uint64_t)v << w->nbits; w->nbits += n;
    while (w->nbits >= 8) { if (w->p < w->end) *w->p++ = (uint8_t)w->acc; w->acc >>= 8; w->nbits -= 8; }
}
static size_t bw_done(BW *w, uint8_t *base) { if (w->nbits && w->p < w->end) *w->p++ = (uint8_t)w->acc; return (size_t)(w->p - base); }

typedef struct { const uint8_t *p, *end; uint64_t acc; int nbits; } BR;
static void br_init(BR *r, const uint8_t *buf, size_t n) { r->p = buf; r->end = buf + n; r->acc = 0; r->nbits = 0; }
static inline void br_fill(BR *r) {
    if (r->nbits > 56) return;
    if (r->p + 8 <= r->end) {                 /* one load beats eight branches */
        uint64_t v; memcpy(&v, r->p, 8);
        r->acc = (r->acc & ((1ull << r->nbits) - 1)) | (v << r->nbits);
        int c = (63 - r->nbits) >> 3;
        r->p += c; r->nbits += c << 3;
        return;
    }
    while (r->nbits <= 56 && r->p < r->end) { r->acc |= (uint64_t)(*r->p++) << r->nbits; r->nbits += 8; }
}
static inline uint32_t br_peek(BR *r, int n) { return (uint32_t)(r->acc & ((1ull << n) - 1)); }
static inline void br_skip(BR *r, int n) { r->acc >>= n; r->nbits -= n; }
static inline uint32_t br_get(BR *r, int n) { if (!n) return 0; br_fill(r); uint32_t v = br_peek(r, n); br_skip(r, n); return v; }

/* ---------------- canonical Huffman ---------------- */
typedef struct { uint8_t len[MAXSYM]; uint16_t code[MAXSYM]; int n; } HEnc;
typedef struct { uint16_t sym[1 << 11]; uint8_t len[1 << 11]; int maxlen; } HDec;

/* Package-merge-free: a simple length-limited build via frequency sorting. */
static void huff_lengths(const uint32_t *freq, int n, uint8_t *len, int maxlen) {
    if (n <= 0) return;
    typedef struct { uint64_t w; int a, b, sym; } Node;
    Node *nd = (Node *)calloc((size_t)2 * n + 2, sizeof(Node));
    int cnt = 0;
    for (int i = 0; i < n; i++) if (freq[i]) { nd[cnt].w = freq[i]; nd[cnt].a = nd[cnt].b = -1; nd[cnt].sym = i; cnt++; }
    memset(len, 0, (size_t)n);
    if (cnt == 0) { free(nd); return; }
    if (cnt == 1) { len[nd[0].sym] = 1; free(nd); return; }
    int total = cnt;
    int *live = (int *)malloc(sizeof(int) * (size_t)cnt);
    for (int i = 0; i < cnt; i++) live[i] = i;
    int nlive = cnt;
    while (nlive > 1) {
        int m1 = 0, m2 = 1;
        if (nd[live[m2]].w < nd[live[m1]].w) { int t = m1; m1 = m2; m2 = t; }
        for (int i = 2; i < nlive; i++) {
            if (nd[live[i]].w < nd[live[m1]].w) { m2 = m1; m1 = i; }
            else if (nd[live[i]].w < nd[live[m2]].w) m2 = i;
        }
        int a = live[m1], b = live[m2];
        nd[total].w = nd[a].w + nd[b].w; nd[total].a = a; nd[total].b = b; nd[total].sym = -1;
        int hi = m1 > m2 ? m1 : m2, lo = m1 < m2 ? m1 : m2;
        live[lo] = total; live[hi] = live[nlive - 1]; nlive--; total++;
    }
    /* walk depths */
    int *stack = (int *)malloc(sizeof(int) * (size_t)total * 2);
    int *depth = (int *)malloc(sizeof(int) * (size_t)total * 2);
    int sp = 0; stack[sp] = live[0]; depth[sp] = 0; sp++;
    while (sp) {
        sp--; int u = stack[sp], d = depth[sp];
        if (nd[u].sym >= 0) { len[nd[u].sym] = (uint8_t)(d > maxlen ? maxlen : (d ? d : 1)); continue; }
        stack[sp] = nd[u].a; depth[sp] = d + 1; sp++;
        stack[sp] = nd[u].b; depth[sp] = d + 1; sp++;
    }
    free(stack); free(depth); free(live); free(nd);

    /* Kraft repair: clamping above can break the inequality, so push it back. */
    for (;;) {
        double k = 0;
        for (int i = 0; i < n; i++) if (len[i]) k += 1.0 / (double)(1u << len[i]);
        if (k <= 1.0000001) break;
        int worst = -1;
        for (int i = 0; i < n; i++) if (len[i] && len[i] < maxlen && (worst < 0 || freq[i] < freq[worst])) worst = i;
        if (worst < 0) break;
        len[worst]++;
    }
}

static void huff_codes(HEnc *e, int n) {
    uint16_t next[32]; uint32_t cntl[32];
    memset(cntl, 0, sizeof(cntl));
    for (int i = 0; i < n; i++) cntl[e->len[i]]++;
    cntl[0] = 0;
    uint16_t code = 0;
    for (int b = 1; b < 32; b++) { code = (uint16_t)((code + cntl[b - 1]) << 1); next[b] = code; }
    /* Store REVERSED: the writer emits LSB-first, while canonical codes are
       defined MSB-first, and the decode table is built on the reversed form. */
    for (int i = 0; i < n; i++) {
        if (!e->len[i]) continue;
        uint16_t c = next[e->len[i]]++, rev = 0;
        for (int b = 0; b < e->len[i]; b++) if (c & (1u << b)) rev |= (uint16_t)(1u << (e->len[i] - 1 - b));
        e->code[i] = rev;
    }
    e->n = n;
}

static uint64_t huff_cost(const uint32_t *f, const uint8_t *len, int n) {
    uint64_t b = 0;
    for (int i = 0; i < n; i++) b += (uint64_t)f[i] * len[i];
    return b;
}

static void huff_dec_build(HDec *d, const uint8_t *len, int n) {
    int maxlen = 0;
    for (int i = 0; i < n; i++) if (len[i] > maxlen) maxlen = len[i];
    if (maxlen == 0) maxlen = 1;
    d->maxlen = maxlen;
    uint32_t cntl[32]; memset(cntl, 0, sizeof(cntl));
    for (int i = 0; i < n; i++) cntl[len[i]]++;
    cntl[0] = 0;
    uint16_t next[32], code = 0;
    for (int b = 1; b < 32; b++) { code = (uint16_t)((code + cntl[b - 1]) << 1); next[b] = code; }
    size_t tsize = (size_t)1 << maxlen;
    for (size_t i = 0; i < tsize; i++) { d->sym[i] = 0; d->len[i] = 0; }
    for (int i = 0; i < n; i++) {
        if (!len[i]) continue;
        uint16_t c = next[len[i]]++;
        /* canonical codes are MSB-first; the reader is LSB-first, so reverse */
        uint16_t rev = 0;
        for (int b = 0; b < len[i]; b++) if (c & (1u << b)) rev |= (uint16_t)(1u << (len[i] - 1 - b));
        for (size_t f = rev; f < tsize; f += ((size_t)1 << len[i])) { d->sym[f] = (uint16_t)i; d->len[f] = len[i]; }
    }
}
static inline int huff_dec_nf(BR *r, const HDec *d) {
    uint32_t idx = br_peek(r, d->maxlen);
    int l = d->len[idx];
    br_skip(r, l ? l : 1);
    return d->sym[idx];
}
static inline int huff_dec(BR *r, const HDec *d) { br_fill(r); return huff_dec_nf(r, d); }

/* ---------------- code buckets ---------------- */
static inline int bucket_of(uint32_t v, int *extra, uint32_t *rest) {
    if (v < 16) { *extra = 0; *rest = 0; return (int)v; }
    int hb = 31 - __builtin_clz(v);
    int b = 16 + ((hb - 4) * 2) + ((v >> (hb - 1)) & 1);
    *extra = hb - 1;
    *rest = v & ((1u << *extra) - 1);
    return b;
}
static inline uint32_t bucket_base(int b, int *extra) {
    if (b < 16) { *extra = 0; return (uint32_t)b; }
    int k = (b - 16) / 2, odd = (b - 16) & 1;
    int hb = k + 4;
    *extra = hb - 1;
    return (uint32_t)((1u << hb) | ((uint32_t)odd << (hb - 1)));
}

/* ---------------- encoder ---------------- */
/*
 * Bit prices from a first parse. A match at a far offset can cost MORE than
 * the literals it replaces; without prices the parser cannot know that, and
 * on this data most offsets are far.
 */
typedef struct {
    uint8_t lit[NCTX][MAXSYM], ll[NLL], ml[NML], off[NOFF];
    int valid;
} Prices;
#define PBITS(t, i) ((t)[i] ? (int)(t)[i] : 20)

typedef struct { uint32_t litRun, mlen, off; int repIdx; } Seq;  /* repIdx -1 = explicit */

/* ---------------- binary range coder ----------------
 * Huffman spends a whole bit on anything, so a symbol that is 90% likely
 * still costs one. A range coder charges the true -log2(p), and adaptive
 * per-context models learn those probabilities as they go. That is the whole
 * of the gap to brotli-11 on document streams, and it is what LZMA does.
 */
#define PBITS_RC 11
#define PMAX_RC  (1 << PBITS_RC)
#define PMOVE_RC 5
typedef uint16_t Prob;

typedef struct { uint8_t *p, *end; uint64_t low; uint32_t range; uint8_t cache; int cacheSize; } REnc;
typedef struct { const uint8_t *p, *end; uint32_t range, code; } RDec;

static void renc_init(REnc *e, uint8_t *buf, size_t n) {
    e->p = buf; e->end = buf + n; e->low = 0; e->range = 0xFFFFFFFFu;
    e->cache = 0; e->cacheSize = 1;
}
static void renc_shift(REnc *e) {
    if ((uint32_t)e->low < 0xFF000000u || (int)(e->low >> 32) != 0) {
        uint8_t c = e->cache;
        do { if (e->p < e->end) *e->p++ = (uint8_t)(c + (uint8_t)(e->low >> 32)); c = 0xFF; }
        while (--e->cacheSize);
        e->cache = (uint8_t)((uint32_t)e->low >> 24);
    }
    e->cacheSize++;
    e->low = (uint32_t)e->low << 8;
}
static inline void renc_bit(REnc *e, Prob *pr, int bit) {
    uint32_t bound = (e->range >> PBITS_RC) * (uint32_t)(*pr);
    if (!bit) { e->range = bound; *pr = (Prob)(*pr + ((PMAX_RC - *pr) >> PMOVE_RC)); }
    else { e->low += bound; e->range -= bound; *pr = (Prob)(*pr - (*pr >> PMOVE_RC)); }
    while (e->range < (1u << 24)) { renc_shift(e); e->range <<= 8; }
}
static inline void renc_direct(REnc *e, uint32_t v, int nbits) {
    while (nbits-- > 0) {
        e->range >>= 1;
        if ((v >> nbits) & 1) e->low += e->range;
        while (e->range < (1u << 24)) { renc_shift(e); e->range <<= 8; }
    }
}
static size_t renc_flush(REnc *e, uint8_t *base) {
    for (int i = 0; i < 5; i++) renc_shift(e);
    return (size_t)(e->p - base);
}

static void rdec_init(RDec *d, const uint8_t *buf, size_t n) {
    d->p = buf; d->end = buf + n; d->range = 0xFFFFFFFFu; d->code = 0;
    for (int i = 0; i < 5; i++) d->code = (d->code << 8) | (d->p < d->end ? *d->p++ : 0);
}
static inline int rdec_bit(RDec *d, Prob *pr) {
    uint32_t bound = (d->range >> PBITS_RC) * (uint32_t)(*pr);
    int bit;
    if (d->code < bound) { d->range = bound; *pr = (Prob)(*pr + ((PMAX_RC - *pr) >> PMOVE_RC)); bit = 0; }
    else { d->code -= bound; d->range -= bound; *pr = (Prob)(*pr - (*pr >> PMOVE_RC)); bit = 1; }
    while (d->range < (1u << 24)) {
        d->code = (d->code << 8) | (d->p < d->end ? *d->p++ : 0);
        d->range <<= 8;
    }
    return bit;
}
static inline uint32_t rdec_direct(RDec *d, int nbits) {
    uint32_t v = 0;
    while (nbits-- > 0) {
        d->range >>= 1;
        d->code -= d->range;
        uint32_t t = 0 - (d->code >> 31);      /* branch-free borrow, as LZMA does */
        d->code += d->range & t;
        v = (v << 1) + (t + 1);
        while (d->range < (1u << 24)) {
            d->code = (d->code << 8) | (d->p < d->end ? *d->p++ : 0);
            d->range <<= 8;
        }
    }
    return v;
}

static void enc_tree(REnc *e, Prob *pr, int nbits, uint32_t sym) {
    uint32_t m = 1;
    for (int i = nbits; i-- > 0;) { int b = (sym >> i) & 1; renc_bit(e, &pr[m], b); m = (m << 1) | (uint32_t)b; }
}
static uint32_t dec_tree(RDec *d, Prob *pr, int nbits) {
    uint32_t m = 1;
    for (int i = 0; i < nbits; i++) m = (m << 1) | (uint32_t)rdec_bit(d, &pr[m]);
    return m - ((uint32_t)1 << nbits);
}
static void enc_rtree(REnc *e, Prob *pr, int nbits, uint32_t sym) {
    uint32_t m = 1;
    for (int i = 0; i < nbits; i++) { int b = (int)(sym & 1); sym >>= 1; renc_bit(e, &pr[m], b); m = (m << 1) | (uint32_t)b; }
}
static uint32_t dec_rtree(RDec *d, Prob *pr, int nbits) {
    uint32_t m = 1, v = 0;
    for (int i = 0; i < nbits; i++) { uint32_t b = (uint32_t)rdec_bit(d, &pr[m]); m = (m << 1) | b; v |= b << i; }
    return v;
}

/* LZMA's state machine: which of the last tokens were matches decides both the
   is-match context and whether a literal is worth coding against the byte the
   last match would have repeated. */
static const uint8_t ST_LIT[12] = {0,0,0,0,1,2,3,4,5,6,4,5};
static const uint8_t ST_MATCH[12] = {7,7,7,7,7,7,7,10,10,10,10,10};
static const uint8_t ST_REP[12] = {8,8,8,8,8,8,8,11,11,11,11,11};

#define LC_RC 3                       /* literal contexts, on the previous byte */
typedef struct {
    Prob isMatch[12], isRep[12], isRepG0[12], isRepG1[12];
    Prob lit[1 << LC_RC][0x300];
    Prob mlen[1 << 7], rlen[1 << 7];  /* bucket trees, match and rep lengths */
    Prob slot[4][1 << 7];             /* offset bucket, by length class */
    Prob align[16];
    Prob sdc[NSD];                    /* short distance code tree */
} RModels;

static void rm_init(RModels *m) {
    Prob *p = (Prob *)m;
    for (size_t i = 0; i < sizeof(RModels) / sizeof(Prob); i++) p[i] = PMAX_RC / 2;
}
#define LENCLASS(ml) ((ml) - MIN_MATCH < 3 ? (int)((ml) - MIN_MATCH) : 3)

static size_t kl_rc_encode(const uint8_t *src, size_t n, const Seq *seqs, size_t nseq,
                           const uint8_t *lits, size_t nlit, uint32_t tailLit,
                           uint8_t *dst, size_t cap) {
    RModels *m = (RModels *)malloc(sizeof(RModels));
    if (!m) return 0;
    rm_init(m);
    dst[0] = KL_TAG(1);
    st32(dst + 1, (uint32_t)n);
    st32(dst + 5, kl_hash(src, n));
    REnc e; renc_init(&e, dst + 9, cap - 9);

    int32_t rep[NREP] = SDINIT;
    int state = 0;
    size_t pos = 0, lp = 0;
    uint8_t prev = 0;

    for (size_t k = 0; k <= nseq; k++) {
        uint32_t run = (k < nseq) ? seqs[k].litRun : tailLit;
        for (uint32_t j = 0; j < run && lp < nlit; j++) {
            uint8_t c = lits[lp++];
            renc_bit(&e, &m->isMatch[state], 0);
            Prob *lt = m->lit[prev >> (8 - LC_RC)];
            if (state >= 7) {
                uint8_t mb = src[pos - (uint32_t)rep[0]];
                uint32_t sym = 1, offs = 0x100;
                for (int i = 7; i >= 0; i--) {
                    uint32_t bm = (uint32_t)((mb >> i) & 1), bit = (uint32_t)((c >> i) & 1);
                    renc_bit(&e, &lt[offs + (bm << 8) + sym], (int)bit);
                    sym = (sym << 1) | bit;
                    if (bm != bit) offs = 0;
                }
            } else enc_tree(&e, lt, 8, c);
            state = ST_LIT[state];
            prev = c; pos++;
        }
        if (k == nseq) break;

        const Seq *s = &seqs[k];
        renc_bit(&e, &m->isMatch[state], 1);
        int isrep = s->repIdx >= 0;
        renc_bit(&e, &m->isRep[state], isrep);
        int e1; uint32_t r1;
        int lb = bucket_of(s->mlen - MIN_MATCH, &e1, &r1);
        if (isrep) {
            enc_tree(&e, m->sdc, 4, (uint32_t)s->repIdx);
            enc_tree(&e, m->rlen, 7, (uint32_t)lb);
            if (e1) renc_direct(&e, r1, e1);
            sd_push(rep, s->off, s->repIdx);
            state = ST_REP[state];
        } else {
            enc_tree(&e, m->mlen, 7, (uint32_t)lb);
            if (e1) renc_direct(&e, r1, e1);
            int e2; uint32_t r2;
            int ob = bucket_of(s->off - 1, &e2, &r2);
            enc_tree(&e, m->slot[LENCLASS(s->mlen)], 7, (uint32_t)ob);
            if (e2 > 4) { renc_direct(&e, r2 >> 4, e2 - 4); enc_rtree(&e, m->align, 4, r2 & 15); }
            else if (e2) renc_direct(&e, r2, e2);
            sd_push(rep, s->off, -1);
            state = ST_MATCH[state];
        }
        pos += s->mlen;
        prev = src[pos - 1];
    }
    size_t sz = renc_flush(&e, dst + 9) + 9;
    free(m);
    return (sz < cap) ? sz : 0;
}

static size_t kl_rc_decode(const uint8_t *src, size_t n, uint8_t *dst) {
    uint32_t rawLen = ld32(src + 1), want = ld32(src + 5);
    RModels *m = (RModels *)malloc(sizeof(RModels));
    if (!m) return 0;
    rm_init(m);
    RDec d; rdec_init(&d, src + 9, n - 9);

    int32_t rep[NREP] = SDINIT;
    int state = 0; uint8_t prev = 0;
    size_t pos = 0;
    while (pos < rawLen) {
        if (!rdec_bit(&d, &m->isMatch[state])) {
            Prob *lt = m->lit[prev >> (8 - LC_RC)];
            uint32_t sym;
            if (state >= 7) {
                if (rep[0] <= 0 || (size_t)rep[0] > pos) { free(m); return 0; }
                uint8_t mb = dst[pos - (uint32_t)rep[0]];
                uint32_t s2 = 1, offs = 0x100;
                for (int i = 7; i >= 0; i--) {
                    uint32_t bm = (uint32_t)((mb >> i) & 1);
                    uint32_t bit = (uint32_t)rdec_bit(&d, &lt[offs + (bm << 8) + s2]);
                    s2 = (s2 << 1) | bit;
                    if (bm != bit) offs = 0;
                }
                sym = s2 & 0xFF;
            } else sym = dec_tree(&d, lt, 8);
            dst[pos++] = (uint8_t)sym; prev = (uint8_t)sym;
            state = ST_LIT[state];
        } else {
            uint32_t off, mlen;
            int isrep = rdec_bit(&d, &m->isRep[state]);
            if (isrep) {
                int code = (int)dec_tree(&d, m->sdc, 4);
                int e1; uint32_t base = bucket_base((int)dec_tree(&d, m->rlen, 7), &e1);
                mlen = base + (e1 ? rdec_direct(&d, e1) : 0) + MIN_MATCH;
                int32_t sd = rep[KDCI[code]] + KDCO[code];
                if (sd <= 0 || (size_t)sd > pos) { free(m); return 0; }
                off = (uint32_t)sd;
                sd_push(rep, off, code);
                state = ST_REP[state];
            } else {
                int e1; uint32_t base = bucket_base((int)dec_tree(&d, m->mlen, 7), &e1);
                mlen = base + (e1 ? rdec_direct(&d, e1) : 0) + MIN_MATCH;
                int e2; uint32_t obase = bucket_base((int)dec_tree(&d, m->slot[LENCLASS(mlen)], 7), &e2);
                uint32_t rest = 0;
                if (e2 > 4) rest = (rdec_direct(&d, e2 - 4) << 4) | dec_rtree(&d, m->align, 4);
                else if (e2) rest = rdec_direct(&d, e2);
                off = obase + rest + 1;
                sd_push(rep, off, -1);
                state = ST_MATCH[state];
            }
            if (off == 0 || off > pos) { free(m); return 0; }
            if (pos + mlen > rawLen) mlen = (uint32_t)(rawLen - pos);
            for (uint32_t j = 0; j < mlen; j++) { dst[pos] = dst[pos - off]; pos++; }
            prev = dst[pos - 1];
        }
    }
    free(m);
    return (pos == rawLen && kl_hash(dst, pos) == want) ? pos : 0;
}


static size_t kl_pass(const uint8_t *src, size_t n, uint8_t *dst, int level,
                      const Prices *pin, Prices *pout, int useDP, int chain) {
    int hlog = HASH_LOG;
    while (hlog > 12 && ((size_t)1 << hlog) > n) hlog--;
    const int hshift = 32 - hlog;
    size_t wsize = 1; while (wsize < n && wsize < WINDOW) wsize <<= 1;
    const size_t wmask = wsize - 1;
    uint32_t *head = (uint32_t *)malloc(sizeof(uint32_t) << hlog);
    uint32_t *prev = (uint32_t *)malloc(sizeof(uint32_t) * wsize);
    if (!head || !prev) { free(head); free(prev); return 0; }
    memset(head, 0xFF, sizeof(uint32_t) << hlog);
    const int maxchain = chain > 0 ? chain : LEVELS[level].chain;
    const int nice = LEVELS[level].nice;

    Seq *seqs = (Seq *)malloc(sizeof(Seq) * (n / 2 + 16));
    uint8_t *lits = (uint8_t *)malloc(n + 16);
    size_t nseq = 0, nlit = 0, litRun = 0;
    size_t i = 0;

    int32_t rep[NREP] = SDINIT;
    int avgLit = 8;
    if (pin && pin->valid) {
        int sum = 0, cnt = 0;
        for (int c = 0; c < NCTX; c++) for (int v = 0; v < MAXSYM; v++)
            if (pin->lit[c][v]) { sum += pin->lit[c][v]; cnt++; }
        if (cnt) avgLit = sum / cnt;
        if (avgLit < 2) avgLit = 2;
    }

    /* find the best match at position q; returns length, sets *off */
    #define FIND(q, outLen, outOff) do {                                            \
        outLen = 0; outOff = 0; int _bestScore = -(1 << 28);                        \
        if ((q) + MIN_MATCH <= n) {                                                 \
            uint32_t _h = (uint32_t)((ld32(src + (q)) * 2654435761u) >> hshift); \
            uint32_t _c = head[_h]; int _ch = maxchain;                             \
            while (_c != UINT32_MAX && _ch-- > 0) {                                 \
                size_t _o = (q) - _c;                                               \
                if (_o == 0 || _o > WINDOW) break;                                  \
                if (ld32(src + _c) == ld32(src + (q))) {                            \
                    size_t _cap = n - (q); if (_cap > 65535) _cap = 65535;           \
                    size_t _l = 4 + match_len(src + _c + 4, src + (q) + 4, _cap - 4); \
                    if (pin && pin->valid) {                                        \
                        int _e1, _e2; uint32_t _r1, _r2;                            \
                        int _bm = bucket_of((uint32_t)_l - MIN_MATCH, &_e1, &_r1);  \
                        int _cost = PBITS(pin->ml, _bm) + _e1;                      \
                        int _ri2 = sd_find(rep, (uint32_t)_o);                      \
                        if (_ri2 >= 0) _cost += PBITS(pin->off, _ri2);              \
                        else { int _bo = bucket_of((uint32_t)_o - 1, &_e2, &_r2) + NSD; \
                               _cost += PBITS(pin->off, _bo) + _e2; }               \
                        int _score = (int)_l * avgLit - _cost;                      \
                        if (_score > _bestScore) { _bestScore = _score;             \
                            outLen = (uint32_t)_l; outOff = (uint32_t)_o;           \
                            if ((int)_l >= nice) break; }                           \
                    } else if (_l > outLen) {                                       \
                        outLen = (uint32_t)_l; outOff = (uint32_t)_o;               \
                        if ((int)_l >= nice) break;                                 \
                    }                                                               \
                }                                                                   \
                _c = prev[_c & wmask];                                       \
            }                                                                       \
            /* recent offsets are nearly free to encode, so try each of them */   \
            for (int _ri = 0; _ri < NSD; _ri++) {                                   \
                int32_t _rs = rep[KDCI[_ri]] + KDCO[_ri];                           \
                if (_rs <= 0 || (size_t)_rs > (q)) continue;                        \
                uint32_t _ro = (uint32_t)_rs;                                       \
                if ((q) + MIN_MATCH > n) continue;                                  \
                if (ld32(src + (q) - _ro) != ld32(src + (q))) continue;              \
                size_t _rc = n - (q); if (_rc > 65535) _rc = 65535;                 \
                size_t _l = 4 + match_len(src + (q) - _ro + 4, src + (q) + 4, _rc - 4); \
                int _e1; uint32_t _r1;                                              \
                int _bm = bucket_of((uint32_t)_l - MIN_MATCH, &_e1, &_r1);          \
                int _cost = (pin && pin->valid)                                     \
                          ? PBITS(pin->ml, _bm) + _e1 + PBITS(pin->off, _ri) : 4;   \
                int _score = (int)_l * avgLit - _cost;                              \
                if ((pin && pin->valid) ? (_score > _bestScore)                     \
                                        : (_l + 2 >= outLen)) {                     \
                    _bestScore = _score; outLen = (uint32_t)_l; outOff = _ro; }     \
            }                                                                       \
        }                                                                           \
    } while (0)

    #define INSERT(q) do {                                                          \
        if ((q) + MIN_MATCH <= n) {                                                 \
            uint32_t _hh = (uint32_t)((ld32(src + (q)) * 2654435761u) >> hshift); \
            prev[(q) & wmask] = head[_hh]; head[_hh] = (uint32_t)(q);         \
        }                                                                           \
    } while (0)

    if (!(pin && pin->valid && useDP))
    while (i + MIN_MATCH <= n) {
        uint32_t best, bestoff;
        FIND(i, best, bestoff);

        if (best >= MIN_MATCH && LEVELS[level].lazy && i + 1 + MIN_MATCH <= n) {
            /* Lazy: a longer match one byte later beats taking this one now. */
            uint32_t nb, nbo; (void)nbo;
            int32_t savedRep[NREP];
            for (int t = 0; t < NREP; t++) savedRep[t] = rep[t];
            FIND(i + 1, nb, nbo);
            for (int t = 0; t < NREP; t++) rep[t] = savedRep[t];
            if (nb > best + 1) {
                INSERT(i);
                lits[nlit++] = src[i]; litRun++; i++;
                continue;
            }
        }

        if (best >= MIN_MATCH && pin && pin->valid) {
            /* Price the match against the literals it would replace. */
            int e1, e2; uint32_t r1, r2;
            int bml = bucket_of(best - MIN_MATCH, &e1, &r1);
            int costM = PBITS(pin->ml, bml) + e1;
            int ri = sd_find(rep, bestoff);
            if (ri >= 0) costM += PBITS(pin->off, ri);
            else { int bo = bucket_of(bestoff - 1, &e2, &r2) + NSD; costM += PBITS(pin->off, bo) + e2; }
            int costL = 0; uint8_t pv = (nlit ? lits[nlit - 1] : 0);
            for (uint32_t k = 0; k < best; k++) {
                uint8_t v = src[i + k];
                costL += PBITS(pin->lit[LCTX(pv)], v);
                pv = v;
                if (costL > costM) break;
            }
            if (costL <= costM) {            /* literals are cheaper — skip the match */
                INSERT(i);
                lits[nlit++] = src[i]; litRun++; i++;
                continue;
            }
        }
        if (best >= MIN_MATCH) {
            seqs[nseq].litRun = (uint32_t)litRun;
            seqs[nseq].mlen = best;
            seqs[nseq].off = bestoff;
            int ri = sd_find(rep, bestoff);
            seqs[nseq].repIdx = ri;
            nseq++;
            sd_push(rep, bestoff, ri);
            litRun = 0;
            for (uint32_t k = 0; k < best; k++) INSERT(i + k);
            i += best;
        } else {
            INSERT(i);
            lits[nlit++] = src[i]; litRun++; i++;
        }
    }
    if (!(pin && pin->valid && useDP))
        while (i < n) { lits[nlit++] = src[i]; litRun++; i++; }


    /*
     * Optimal parse. The greedy walk above commits to the best match at each
     * position; this one prices every route through the block and keeps the
     * cheapest, which is what brotli turns on at quality 10-11 and the only
     * remaining reason it beats this codec.
     *
     * Forward dynamic program over a segment: cost[k] is the fewest bits to
     * reach offset k. Literal-run length lives in the state because the format
     * codes it, and the recent-offset cache is carried along the winning path.
     */
    if (pin && pin->valid && useDP) {
        const size_t SEG = 1u << 22;
        /* An optimal parse must be bounded on repetitive input: scanning every
           match to its full length, at every position, is quadratic. */
        const size_t MAXSCAN = 4096;
        #define NCAND 24
        /* Above this a match is priced at its full length only, never at every
           shorter one. Same rule and same value as brotli's quality-11
           MAX_ZOPFLI_LEN: without it, all-zero input put every short code on a
           4096-byte run at every position and 200 kB took 4.5 minutes. */
        #define LONG_MATCH 325
        #define SCAN_ROOM(end_, q_, have_) \
            ((size_t)((end_) - (q_)) > (size_t)(have_)                       \
                ? (((size_t)((end_) - (q_)) < (size_t)MAXSCAN                \
                        ? (size_t)((end_) - (q_)) : (size_t)MAXSCAN)         \
                   > (size_t)(have_)                                         \
                   ? (((size_t)((end_) - (q_)) < (size_t)MAXSCAN             \
                        ? (size_t)((end_) - (q_)) : (size_t)MAXSCAN)         \
                      - (size_t)(have_))                                     \
                   : (size_t)0)                                              \
                : (size_t)0)
        typedef struct {
            uint32_t cost, mlen, moff, litrun;
            int32_t rep[NREP];
        } Opt;
        Opt *opt = (Opt *)malloc(sizeof(Opt) * (SEG + 2));
        if (opt) {
            size_t base = 0;
            int32_t carry[NREP] = SDINIT;
            while (base < n) {
                size_t end = base + SEG; if (end > n) end = n;
                size_t span = end - base;
                for (size_t k = 0; k <= span; k++) opt[k].cost = UINT32_MAX;
                opt[0].cost = 0; opt[0].mlen = 0; opt[0].litrun = 0;
                for (int t = 0; t < NREP; t++) opt[0].rep[t] = carry[t];

                for (size_t k = 0; k < span; k++) {
                    if (opt[k].cost == UINT32_MAX) continue;
                    size_t q = base + k;
                    /* literal */
                    {
                        uint8_t pv = (k && opt[k].litrun) ? src[q - 1] : 0;
                        uint32_t c = opt[k].cost + (uint32_t)PBITS(pin->lit[LCTX(pv)], src[q]);
                        if (c < opt[k + 1].cost) {
                            opt[k + 1].cost = c; opt[k + 1].mlen = 0;
                            opt[k + 1].litrun = opt[k].litrun + 1;
                            for (int t = 0; t < NREP; t++) opt[k + 1].rep[t] = opt[k].rep[t];
                        }
                    }
                    if (q + MIN_MATCH > end) { INSERT(q); continue; }

                    uint32_t llBits = (uint32_t)PBITS(pin->ll, bucket_of(opt[k].litrun, &(int){0}, &(uint32_t){0}));
                    { int e0; uint32_t r0; int b0 = bucket_of(opt[k].litrun, &e0, &r0);
                      llBits = (uint32_t)PBITS(pin->ll, b0) + (uint32_t)e0; }

                    uint32_t maxLen = 0;
                    /* candidates: the sixteen short distances, then the hash chain */
                    for (int ri = 0; ri < NSD; ri++) {
                        int32_t rs = opt[k].rep[KDCI[ri]] + KDCO[ri];
                        if (rs <= 0 || q < (size_t)rs) continue;
                        uint32_t ro = (uint32_t)rs;
                        size_t L = 0;
                        L += match_len(src + q - ro + L, src + q + L, SCAN_ROOM(end, q, L));
                        if (L < MIN_MATCH) continue;
                        /* Price every length this short code reaches, not only its
                           longest: a shorter cheap-distance edge can beat a longer
                           expensive one, which is the whole point of the +/-k codes. */
                        uint32_t sdBits = (uint32_t)PBITS(pin->off, ri);
                        for (size_t Lx = (L > LONG_MATCH ? L : MIN_MATCH); Lx <= L; Lx++) {
                            if (Lx != L && L - Lx > 64 && (Lx & 7)) continue;
                            int em; uint32_t rm;
                            int bm = bucket_of((uint32_t)Lx - MIN_MATCH, &em, &rm);
                            uint32_t c = opt[k].cost + llBits + (uint32_t)PBITS(pin->ml, bm)
                                       + (uint32_t)em + sdBits;
                            if (c < opt[k + Lx].cost) {
                                opt[k + Lx].cost = c; opt[k + Lx].mlen = (uint32_t)Lx;
                                opt[k + Lx].moff = ro; opt[k + Lx].litrun = 0;
                                for (int t = 0; t < NREP; t++) opt[k + Lx].rep[t] = opt[k].rep[t];
                                sd_push(opt[k + Lx].rep, ro, ri);
                            }
                        }
                    }
                    {
                        /*
                         * Enumerate CANDIDATES, not lengths. Each chain hit
                         * gives one (offset, longest length) pair; adding an
                         * edge per pair costs ~24 edges a position instead of
                         * thousands, and samples nothing away — which is what
                         * lost the long image matches before.
                         */
                        uint32_t h = (uint32_t)((ld32(src + q) * 2654435761u) >> hshift);
                        uint32_t cand = head[h]; int chain = maxchain;
                        uint32_t cOff[NCAND], cLen[NCAND]; int nc = 0;
                        while (cand != UINT32_MAX && chain-- > 0 && nc < NCAND) {
                            size_t off = q - cand;
                            if (off == 0 || off > WINDOW) break;
                            if (ld32(src + cand) == ld32(src + q)) {
                                size_t L = 4;
                                L += match_len(src + cand + L, src + q + L, SCAN_ROOM(end, q, L));
                                if (L > maxLen) maxLen = (uint32_t)L;
                                if (L >= MIN_MATCH) { cOff[nc] = (uint32_t)off; cLen[nc] = (uint32_t)L; nc++; }
                            }
                            cand = prev[cand & wmask];
                        }
                        /* The chain walks most-recent-first, so candidates
                           arrive nearest-offset-first. Give each LENGTH the
                           nearest offset that reaches it by only covering
                           lengths no earlier candidate already reached. */
                        uint32_t covered = MIN_MATCH - 1;
                        for (int ci = 0; ci < nc; ci++) {
                            if (cLen[ci] <= covered) continue;
                            uint32_t off = cOff[ci];
                            int eo; uint32_t ro2;
                            int bo = bucket_of(off - 1, &eo, &ro2) + NSD;
                            uint32_t offBits = (uint32_t)PBITS(pin->off, bo) + (uint32_t)eo;
                            uint32_t from = (cLen[ci] > LONG_MATCH) ? cLen[ci] : covered + 1;
                            for (uint32_t L = from; L <= cLen[ci]; L++) {
                                /* dense where it matters, sampled once the run is long */
                                if (L != cLen[ci] && L - covered > 64 && (L & 7)) continue;
                                int em; uint32_t rm;
                                int bm = bucket_of(L - MIN_MATCH, &em, &rm);
                                uint32_t c = opt[k].cost + llBits + (uint32_t)PBITS(pin->ml, bm)
                                           + (uint32_t)em + offBits;
                                if (c < opt[k + L].cost) {
                                    opt[k + L].cost = c; opt[k + L].mlen = L; opt[k + L].moff = off;
                                    opt[k + L].litrun = 0;
                                    for (int t = 0; t < NREP; t++) opt[k + L].rep[t] = opt[k].rep[t];
                                    sd_push(opt[k + L].rep, off, -1);
                                }
                            }
                            covered = cLen[ci];
                        }
                    }
                    INSERT(q);
                }

                /* backtrack the chosen route, then replay it forwards */
                size_t *path = (size_t *)malloc(sizeof(size_t) * (span + 2));
                size_t np = 0, k = span;
                while (k > 0) {
                    path[np++] = k;
                    k -= opt[k].mlen ? opt[k].mlen : 1;
                }
                for (size_t t = np; t-- > 0;) {
                    size_t kk = path[t];
                    size_t from = kk - (opt[kk].mlen ? opt[kk].mlen : 1);
                    if (!opt[kk].mlen) { lits[nlit++] = src[base + from]; litRun++; continue; }
                    uint32_t off = opt[kk].moff;
                    int ri = -1;
                    ri = sd_find(rep, off);
                    seqs[nseq].litRun = (uint32_t)litRun;
                    seqs[nseq].mlen = opt[kk].mlen;
                    seqs[nseq].off = off;
                    seqs[nseq].repIdx = ri;
                    nseq++; litRun = 0;
                    sd_push(rep, off, ri);
                }
                free(path);
                for (int t = 0; t < NREP; t++) carry[t] = rep[t];
                base = end;
            }
            i = n;
        }
        free(opt);
    }

    uint32_t tailLit = (uint32_t)litRun;   /* every stored sequence now HAS a match */

    /* histograms */
    uint32_t fl[NCTX][MAXSYM]; memset(fl, 0, sizeof(fl));
    uint32_t fll[MAXSYM] = {0}, fml[MAXSYM] = {0}, foff[MAXSYM] = {0};
    { uint8_t prev = 0;
      for (size_t k = 0; k < nlit; k++) { fl[LCTX(prev)][lits[k]]++; prev = lits[k]; } }
    for (size_t k = 0; k < nseq; k++) {
        int e; uint32_t r;
        fll[bucket_of(seqs[k].litRun, &e, &r)]++;
        fml[bucket_of(seqs[k].mlen - MIN_MATCH, &e, &r)]++;
        foff[seqs[k].repIdx >= 0 ? seqs[k].repIdx : bucket_of(seqs[k].off - 1, &e, &r) + NSD]++;
    }
    HEnc el[NCTX]; memset(el, 0, sizeof(el));
    HEnc ell = {{0}}, eml = {{0}}, eoff = {{0}};
    for (int c = 0; c < NCTX; c++) { huff_lengths(fl[c], NLIT, el[c].len, 11); huff_codes(&el[c], NLIT); }

    /* The per-context tables cost NLIT header bytes each and were measured to
       buy under 0.3%. Price both and keep the cheaper; one table also shortens
       the decoder's dependency chain. */
    int nLitTab = NCTX;
    {
        uint32_t f1[MAXSYM] = {0};
        for (int c = 0; c < NCTX; c++) for (int v = 0; v < NLIT; v++) f1[v] += fl[c][v];
        HEnc e1; memset(&e1, 0, sizeof(e1));
        huff_lengths(f1, NLIT, e1.len, 11); huff_codes(&e1, NLIT);
        uint64_t bitsN = 0;
        for (int c = 0; c < NCTX; c++) bitsN += huff_cost(fl[c], el[c].len, NLIT);
        if (huff_cost(f1, e1.len, NLIT) / 8 + NLIT <= bitsN / 8 + (uint64_t)NCTX * NLIT) {
            nLitTab = 1; for (int c = 0; c < NCTX; c++) el[c] = e1;
        }
    }
    huff_lengths(fll, NLL, ell.len, 11);  huff_codes(&ell, NLL);
    huff_lengths(fml, NML, eml.len, 11);  huff_codes(&eml, NML);
    huff_lengths(foff, NOFF, eoff.len, 11); huff_codes(&eoff, NOFF);

    if (pout) {
        for (int c = 0; c < NCTX; c++) memcpy(pout->lit[c], el[c].len, MAXSYM);
        memcpy(pout->ll, ell.len, NLL);
        memcpy(pout->ml, eml.len, NML);
        memcpy(pout->off, eoff.len, NOFF);
        pout->valid = 1;
    }

    uint8_t *p = dst;
    *p++ = KL_TAG(0);
    st32(p, (uint32_t)n); p += 4;
    st32(p, kl_hash(src, n)); p += 4;
    st32(p, (uint32_t)nseq); p += 4;
    st32(p, (uint32_t)nlit); p += 4;
    st32(p, tailLit); p += 4;
    *p++ = (uint8_t)nLitTab;
    for (int c = 0; c < nLitTab; c++) { memcpy(p, el[c].len, NLIT); p += NLIT; }
    memcpy(p, ell.len, NLL); p += NLL;
    memcpy(p, eml.len, NML); p += NML;
    memcpy(p, eoff.len, NOFF); p += NOFF;

    uint8_t *seqStart = p + 4;
    BW w; bw_init(&w, seqStart, (size_t)(n * 2));
    for (size_t k = 0; k < nseq; k++) {
        int e; uint32_t r; int b;
        b = bucket_of(seqs[k].litRun, &e, &r); bw_put(&w, ell.code[b], ell.len[b]); if (e) bw_put(&w, r, e);
        b = bucket_of(seqs[k].mlen - MIN_MATCH, &e, &r); bw_put(&w, eml.code[b], eml.len[b]); if (e) bw_put(&w, r, e);
        if (seqs[k].repIdx >= 0) { b = seqs[k].repIdx; bw_put(&w, eoff.code[b], eoff.len[b]); }
        else { b = bucket_of(seqs[k].off - 1, &e, &r) + NSD; bw_put(&w, eoff.code[b], eoff.len[b]);
               if (e) bw_put(&w, r, e); }
    }
    size_t seqBytes = bw_done(&w, seqStart);
    st32(p, (uint32_t)seqBytes); p += 4 + seqBytes;

    BW wl; bw_init(&wl, p, (size_t)(n * 2));
    { uint8_t prev = 0;
      for (size_t k = 0; k < nlit; k++) {
          int c = (nLitTab == 1) ? 0 : LCTX(prev);
          bw_put(&wl, el[c].code[lits[k]], el[c].len[lits[k]]);
          prev = lits[k];
      } }
    size_t litBytes = bw_done(&wl, p);
    p += litBytes;

    if (getenv("KL_OFFS")) {
        uint64_t reps = 0; uint64_t nm = 0;
        /* how concentrated are the offsets? top-16 by frequency */
        size_t cap = 1u << 20; uint32_t *tab = (uint32_t *)calloc(cap, 4);
        uint32_t *val = (uint32_t *)calloc(cap, 4);
        for (size_t k = 0; k < nseq; k++) {
            if (!seqs[k].mlen) continue;
            nm++;
            if (seqs[k].repIdx >= 0) reps++;
            uint32_t o = seqs[k].off; size_t h = (o * 2654435761u) & (cap - 1);
            while (tab[h] && val[h] != o) h = (h + 1) & (cap - 1);
            val[h] = o; tab[h]++;
        }
        /* pick the 12 most common offsets */
        for (int r = 0; r < 12; r++) {
            size_t bi = 0; uint32_t bc = 0;
            for (size_t h = 0; h < cap; h++) if (tab[h] > bc) { bc = tab[h]; bi = h; }
            if (!bc) break;
            fprintf(stderr, "    offset %-9u used %8u  (%.1f%% of matches)\n",
                    val[bi], bc, 100.0 * bc / (double)nm);
            tab[bi] = 0;
        }
        fprintf(stderr, "    matches=%llu  rep-offset hits=%llu (%.1f%%)\n",
                (unsigned long long)nm, (unsigned long long)reps, 100.0 * reps / (double)nm);
        free(tab); free(val);
    }
    if (getenv("KL_STATS")) {
        size_t hdr = 16 + (size_t)NCTX * NLIT + NLL + NML + NOFF + 4;
        uint64_t mtot = 0, ltot = 0;
        for (size_t k = 0; k < nseq; k++) { mtot += seqs[k].mlen; ltot += seqs[k].litRun; }
        fprintf(stderr,
            "  in=%zu out=%zu | hdr=%zu seq=%zu lit=%zu | nseq=%zu nlit=%zu "
            "| covered-by-matches=%.1f%% avg-match=%.1f lit-bits/byte=%.2f\n",
            n, (size_t)(p - dst), hdr, seqBytes, litBytes, nseq, nlit,
            100.0 * (double)mtot / (double)n, nseq ? (double)mtot / (double)nseq : 0.0,
            nlit ? 8.0 * (double)litBytes / (double)nlit : 0.0);
    }
    size_t huffSize = (size_t)(p - dst);
    /* The range coder costs decode speed, so it only competes where ratio is
       the point, and it only wins if it actually produces fewer bytes. */
    if (level >= 4 && n >= 4096) {
        uint8_t *rcbuf = (uint8_t *)malloc(n * 2 + 4096);
        if (rcbuf) {
            size_t rcSize = kl_rc_encode(src, n, seqs, nseq, lits, nlit, tailLit,
                                         rcbuf, n * 2 + 4096);
            if (rcSize && rcSize < huffSize) { memcpy(dst, rcbuf, rcSize); huffSize = rcSize; }
            free(rcbuf);
        }
    }
    free(head); free(prev); free(seqs); free(lits);
    return huffSize;
}

static size_t kl_rounds(const uint8_t *src, size_t n, uint8_t *dst, int level,
                        int chain, int rounds, int tryOpt) {
    Prices pr; memset(&pr, 0, sizeof(pr));
    uint8_t *tmp = (uint8_t *)malloc(n * 2 + 4096);
    if (!tmp) return kl_pass(src, n, dst, level, NULL, NULL, 0, chain);

    size_t best = kl_pass(src, n, dst, level, NULL, &pr, 0, chain);
    for (int r = 0; r < rounds; r++) {
        Prices ng; memset(&ng, 0, sizeof(ng));
        size_t g = kl_pass(src, n, tmp, level, &pr, &ng, 0, chain);
        Prices best_next = ng;
        if (g < best) { memcpy(dst, tmp, g); best = g; }
        if (tryOpt) {
            Prices no; memset(&no, 0, sizeof(no));
            size_t o = kl_pass(src, n, tmp, level, &pr, &no, 1, chain);
            if (o < best) { memcpy(dst, tmp, o); best = o; }
            if (o <= g && no.valid) best_next = no;
        }
        if (!best_next.valid) break;
        pr = best_next;
    }
    free(tmp);
    return best;
}

static size_t kl_compress(const uint8_t *src, size_t n, uint8_t *dst, int level) {
    if (!LEVELS[level].dp || n < 4096) return kl_pass(src, n, dst, level, NULL, NULL, 0, 0);

    const int tryOpt = USE_DP(level, n);
    uint8_t *cand = (uint8_t *)malloc(n * 2 + 4096);
    if (!cand) return kl_pass(src, n, dst, level, NULL, NULL, 0, 0);

    size_t best = 0;
    for (int i = 0; i < NCFG[level]; i++) {
        int ch = CFG[level][i].chain;
        if (n > DP_MAX_BYTES) {
            /* No parse up here to make up for a shallow search, so never look
               less far than level 3 does. */
            if (ch < LEVELS[3].chain) ch = LEVELS[3].chain;
            if (ch > DEEP_CAP) ch = DEEP_CAP;
        }
        size_t sz = kl_rounds(src, n, cand, level, ch, CFG[level][i].rounds, tryOpt);
        if (i == 0 || sz < best) { memcpy(dst, cand, sz); best = sz; }
    }
    free(cand);
    return best;
}

/* One decoder workspace, reused across calls, the way a real library does. */
static uint8_t *g_lit = NULL; static size_t g_litcap = 0;
static void kl_release(void) { free(g_lit); g_lit = NULL; g_litcap = 0; }

static size_t kl_decompress(const uint8_t *src, size_t n, uint8_t *dst) {
    if (n < 9) return 0;
    if ((src[0] >> 4) != KL_FORMAT) return 0;      /* not ours, or a newer format */
    if ((src[0] & 0x0F) == 1) return kl_rc_decode(src, n, dst);
    if ((src[0] & 0x0F) != 0) return 0;
    const uint8_t *p = src + 1;
    uint32_t rawLen = ld32(p); p += 4;
    uint32_t want = ld32(p); p += 4;
    uint32_t nseq   = ld32(p); p += 4;
    uint32_t nlit   = ld32(p); p += 4;
    uint32_t tailLit = ld32(p); p += 4;
    int nLitTab = *p++;
    if (nLitTab != 1 && nLitTab != NCTX) return 0;
    HDec dl[NCTX], dll, dml, doff;
    for (int c = 0; c < nLitTab; c++) { huff_dec_build(&dl[c], p, NLIT); p += NLIT; }
    for (int c = nLitTab; c < NCTX; c++) dl[c] = dl[0];
    const int lshift = (nLitTab == 1) ? 8 : 6;
    huff_dec_build(&dll, p, NLL);   p += NLL;
    huff_dec_build(&dml, p, NML);   p += NML;
    huff_dec_build(&doff, p, NOFF); p += NOFF;
    uint32_t seqBytes = ld32(p); p += 4;
    const uint8_t *seqp = p; p += seqBytes;
    const uint8_t *litp = p;

    if ((size_t)nlit + 32 > g_litcap) {
        free(g_lit); g_litcap = (size_t)nlit + 32; g_lit = (uint8_t *)malloc(g_litcap);
        if (!g_lit) { g_litcap = 0; return 0; }
    }
    uint8_t *lit = g_lit;

    /* Literals first, in one tight loop. The sequence loop then copies instead
       of decoding, so the match copies can run 8 bytes wide. */
    {
        BR rl; br_init(&rl, litp, (size_t)(src + n - litp));
        uint32_t j = 0, v;
        if (nLitTab == 1) {
            /* One table means no dependency on the previous symbol, so the
               loads run ahead of each other instead of in single file. */
            const HDec *d0 = &dl[0];
            while (j + 4 <= nlit && rl.p + 8 <= rl.end) {
                br_fill(&rl);
                lit[j++] = (uint8_t)huff_dec_nf(&rl, d0);
                lit[j++] = (uint8_t)huff_dec_nf(&rl, d0);
                lit[j++] = (uint8_t)huff_dec_nf(&rl, d0);
                lit[j++] = (uint8_t)huff_dec_nf(&rl, d0);
            }
            for (; j < nlit; j++) lit[j] = (uint8_t)huff_dec(&rl, d0);
        } else {
            uint8_t pv = 0;
            while (j + 4 <= nlit && rl.p + 8 <= rl.end) {
                br_fill(&rl);                  /* 4 codes of <=11 bits fit one refill */
                v = (uint32_t)huff_dec_nf(&rl, &dl[(pv) >> lshift]); lit[j++] = (uint8_t)v; pv = (uint8_t)v;
                v = (uint32_t)huff_dec_nf(&rl, &dl[(pv) >> lshift]); lit[j++] = (uint8_t)v; pv = (uint8_t)v;
                v = (uint32_t)huff_dec_nf(&rl, &dl[(pv) >> lshift]); lit[j++] = (uint8_t)v; pv = (uint8_t)v;
                v = (uint32_t)huff_dec_nf(&rl, &dl[(pv) >> lshift]); lit[j++] = (uint8_t)v; pv = (uint8_t)v;
            }
            for (; j < nlit; j++) { v = (uint32_t)huff_dec(&rl, &dl[(pv) >> lshift]); lit[j] = (uint8_t)v; pv = (uint8_t)v; }
        }
    }

    uint8_t *o = dst; uint8_t *oend = dst + rawLen;
    int32_t rep[NREP] = SDINIT; uint32_t lpos = 0;
    BR rs; br_init(&rs, seqp, seqBytes);
    for (uint32_t k = 0; k < nseq; k++) {
        int e; uint32_t base;
        br_fill(&rs);
        int b = huff_dec_nf(&rs, &dll);
        base = bucket_base(b, &e);
        uint32_t lrun = base + (e ? br_get(&rs, e) : 0);
        if (lrun > nlit - lpos) lrun = nlit - lpos;
        if ((size_t)(oend - o) >= (size_t)lrun + 32) { memcpy(o, lit + lpos, lrun); o += lrun; }
        else { for (uint32_t j = 0; j < lrun && o < oend; j++) *o++ = lit[lpos + j]; }
        lpos += lrun;

        br_fill(&rs);
        b = huff_dec_nf(&rs, &dml); base = bucket_base(b, &e);
        uint32_t mlen = base + (e ? br_get(&rs, e) : 0) + MIN_MATCH;
        b = huff_dec(&rs, &doff);
        uint32_t off;
        if (b < NSD) {
            int32_t sd = rep[KDCI[b]] + KDCO[b];
            if (sd <= 0) return 0;
            off = (uint32_t)sd;
            sd_push(rep, off, b);
        } else {
            base = bucket_base(b - NSD, &e);
            off = base + (e ? br_get(&rs, e) : 0) + 1;
            sd_push(rep, off, -1);
        }
        const uint8_t *m = o - off;
        if (m < dst) return 0;
        /* Wide copy needs a non-overlapping stride and room to overshoot. */
        if (off >= 8 && (size_t)(oend - o) >= (size_t)mlen + 32) {
            uint8_t *stop = o + mlen;
            do { memcpy(o, m, 8); o += 8; m += 8; } while (o < stop);
            o = stop;
        } else {
            for (uint32_t j = 0; j < mlen && o < oend; j++) *o++ = *m++;
        }
    }
    {
        uint32_t t = tailLit; if (t > nlit - lpos) t = nlit - lpos;
        if ((size_t)(oend - o) >= (size_t)t) { memcpy(o, lit + lpos, t); o += t; }
        else { for (uint32_t j = 0; j < t && o < oend; j++) *o++ = lit[lpos + j]; }
    }
    if ((size_t)(o - dst) != (size_t)rawLen || kl_hash(dst, (size_t)(o - dst)) != want) return 0;
    return (size_t)(o - dst);
}

static double now_s(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "kl c|d|m|b|t in out [level]\n"); return 2; }
    int level = argc > 4 ? atoi(argv[4]) : 6;
    if (level < 1) level = 1;
    if (level > 9) level = 9;
    FILE *in = fopen(argv[2], "rb"); if (!in) { perror("in"); return 1; }
    fseek(in, 0, SEEK_END); long n = ftell(in); fseek(in, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)n + 64);
    if (n && fread(buf, 1, (size_t)n, in) != (size_t)n) { perror("read"); return 1; }
    fclose(in);
    FILE *out = fopen(argv[3], "wb"); if (!out) { perror("out"); return 1; }

    if (argv[1][0] == 'm') {                      /* measure in-process: exec and file I/O are not the codec */
        uint8_t *tmp = (uint8_t *)malloc((size_t)n * 2 + 4096);
        uint8_t *back = (uint8_t *)malloc((size_t)n + 64);
        size_t m = kl_compress(buf, (size_t)n, tmp, level);
        double bc = 1e30, bd = 1e30, t0 = now_s(), a, e;
        do { a = now_s(); m = kl_compress(buf, (size_t)n, tmp, level);
             e = now_s() - a; if (e < bc) bc = e; } while (now_s() - t0 < 2.0);
        kl_decompress(tmp, m, back);
        t0 = now_s();
        do { a = now_s(); kl_decompress(tmp, m, back);
             e = now_s() - a; if (e < bd) bd = e; } while (now_s() - t0 < 2.0);
        int ok = n == 0 || memcmp(back, buf, (size_t)n) == 0;
        printf("raw=%ld comp=%zu enc=%.6f dec=%.6f rt=%s\n", n, m, bc, bd, ok ? "ok" : "FAIL");
        free(tmp); free(back); free(buf); fclose(out); kl_release();
        return ok ? 0 : 1;
    }
    if (argv[1][0] == 'b') {                      /* in-process decode benchmark */
        uint8_t *tmp = (uint8_t *)malloc((size_t)n * 2 + 4096);
        size_t m = kl_compress(buf, (size_t)n, tmp, level);
        uint8_t *back = (uint8_t *)malloc((size_t)n + 64);
        int reps = (int)(400000000ull / (unsigned long long)(n ? n : 1)) + 1;
        struct timespec a, b2;
        clock_gettime(CLOCK_MONOTONIC, &a);
        for (int k = 0; k < reps; k++) kl_decompress(tmp, m, back);
        clock_gettime(CLOCK_MONOTONIC, &b2);
        double secs = (double)(b2.tv_sec - a.tv_sec) + 1e-9 * (double)(b2.tv_nsec - a.tv_nsec);
        printf("raw=%ld comp=%zu ratio=%.2f reps=%d decode=%.0f MB/s\n",
               n, m, (double)n / (double)m, reps, (double)n * reps / 1e6 / secs);
        free(tmp); free(back); free(buf); fclose(out); kl_release();
        return 0;
    }
    if (argv[1][0] == 't') {                      /* self-test: encode then decode in-process */
        uint8_t *tmp = (uint8_t *)malloc((size_t)n * 2 + 4096);
        size_t m = kl_compress(buf, (size_t)n, tmp, level);
        uint8_t *back = (uint8_t *)calloc((size_t)n + 64, 1);
        size_t r = kl_decompress(tmp, m, back);
        size_t bad = (size_t)-1;
        for (size_t k = 0; k < (size_t)n; k++) if (back[k] != buf[k]) { bad = k; break; }
        printf("in=%ld comp=%zu out=%zu firstbad=%s%zu\n", n, m, r,
               bad == (size_t)-1 ? "none " : "", bad == (size_t)-1 ? (size_t)0 : bad);
        if (bad != (size_t)-1) {
            printf("  at %zu: want", bad);
            for (int k = 0; k < 12 && bad + k < (size_t)n; k++) printf(" %02x", buf[bad + k]);
            printf("\n  at %zu: got ", bad);
            for (int k = 0; k < 12 && bad + k < (size_t)n; k++) printf(" %02x", back[bad + k]);
            printf("\n");
        }
        free(tmp); free(back); free(buf); fclose(out); kl_release();
        return bad == (size_t)-1 ? 0 : 1;
    }
    uint8_t *dst;
    if (argv[1][0] == 'c') {
        dst = (uint8_t *)malloc((size_t)n * 2 + 4096);
        size_t m = kl_compress(buf, (size_t)n, dst, level);
        fwrite(dst, 1, m, out);
    } else {
        uint32_t rawLen = n >= 5 ? ld32(buf + 1) : 0;
        dst = (uint8_t *)malloc((size_t)rawLen + 64);
        size_t m = kl_decompress(buf, (size_t)n, dst);
        if (n > 0 && m == 0) {
            fprintf(stderr, "kl: refusing to decode (bad or unsupported format)\n");
            fclose(out); free(dst); free(buf); kl_release();
            return 3;
        }
        if (m != (size_t)rawLen) {            /* truncated or corrupt payload */
            fprintf(stderr, "kl: short decode, got %zu of %u bytes\n", m, rawLen);
            fclose(out); free(dst); free(buf); kl_release();
            return 4;
        }
        fwrite(dst, 1, m, out);
    }
    fclose(out);
    free(dst); free(buf); kl_release();
    return 0;
}
