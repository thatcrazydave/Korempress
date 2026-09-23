# Findings, including the ones that were wrong

Kept because the dead ends cost more time than the successes, and re-walking
them is the expensive mistake.

## What closed the gap to brotli

The deficit on PDF content streams was 81,482 bytes. Handing our unchanged coder
brotli's own command stream, and re-scoring both parses under one cost model,
decomposes it exactly:

| cause | bytes | share |
|---|---:|---:|
| the parse chose distances the cache could not reuse | 54,814 | 67% |
| our distance alphabet was too narrow | 26,668 | 33% |
| literal context width | 766 | ~1% |
| brotli's 122 kB static dictionary | 136 | 0.2% |
| header tables — **in our favour** | −1,908 | −2% |

**The two causes multiply, they do not add.** Each alone is a regression:
widening the alphabet on the old parse costs +6,548, cache-aware pricing on the
old alphabet costs +24,675. Together they save 106,606 — an interaction worth
about 46,000 bytes.

## Theories that were tested and are false

1. **"The gap is literal context modelling / the insert-and-copy symbol."**
   Ablating brotli feature by feature: literal context modelling is worth +0.6%
   on this corpus, block splitting ±0.3%, mode 0%. Meanwhile quality 10 instead
   of 11 costs +45.7% and a 64 kB window costs +102%. It was the parse and
   long-range matching all along.

2. **"Brotli's short distance codes do not help us."** Measured at −391 bytes
   with the parse frozen — a correct measurement of the wrong quantity. They are
   worth ~27.8 kB alone once the parse can use them, and ~106.6 kB combined.

3. **"The regression came from the parse pricing against Huffman while the
   output was range-coded."** Plausible, and wrong. The regression reproduces
   with the parse frozen bit-for-bit, and the fix that works contains no
   range-coder pricing at all.

4. **"Deeper hash chains always help."** Chain depth is not convex: 64 and 4096
   both beat 256 on document streams. One hand-picked value inverted the level
   ladder.

5. **"Widen the distance cache from 3 slots to 4."** Actively harmful at equal
   short-code count: +9,007 bytes.

6. **"Sampling in the optimal parse loses long image matches."** With no
   sampling and 41 minutes of parse, the image corpus returns the identical byte
   count the greedy parse reaches in seconds.

## Two bugs, both found by adversarial review rather than by the gates

**Heap-buffer-overflow in the optimal parse.** The dynamic program guarded its
loop with the whole input length but capped match scanning at the segment end,
so on a segmented input the last three positions underflowed a `size_t` and the
scan ran off a 117 MB allocation. It fires for any input between 4 MiB and 8 MiB
at the parse levels.

It survived 85 clean sanitizer round trips because every corpus was either under
4 MB or over 8 MB. The band where the parse runs on a segmented input had no
coverage at all. **A gate that reports "all clean" over a corpus set with a hole
in it is worse than no gate, because it buys false confidence.** `tests/` now
includes a 5 MB case for exactly this reason.

**Silent truncation.** A range decoder reads zeros past the end of its input, so
a truncated block decoded into a full-length buffer of plausible garbage and
exited 0. No investigation raised it; it turned up while probing failure modes
deliberately. A version byte does not catch it — it needed a checksum over the
original, verified on decode. Four bytes.

## A regression the gap-closing change introduced

Pricing **every** length a short distance reaches is what let the parse hunt
for reusable distances — and it made all-zero input 8× slower. On zeros, all
sixteen short codes match 4,096-byte runs at every position, so each position
relaxed hundreds of edges per code:

| 200 kB of zeros | before | after the change | after the fix |
|---|---:|---:|---:|
| level 6 | 6.5 s | 53.6 s | 6.3 s |
| level 9 | 32.7 s | 270.9 s | 31.2 s |

It passed every verification sweep because no corpus was all-zeros. The fix is
brotli's own rule, read from its source: above `MAX_ZOPFLI_LEN_QUALITY_11 = 325`
a match is priced at its full length only (`backward_references_hq.c`: *"if the
maximum length is long enough, try only one maximum length"*). Cost: 3 bytes on
the document corpus, 102,077 → 102,080.

**A codec for user uploads needs a degenerate-input corpus in its gate**, because
a pathological slowdown on attacker-choosable input is a denial of service.
`tests/` now carries one.

## Open

- The ~330 kB gap to brotli on the two image corpora. Untouched.
- Why we now beat brotli on documents. Unexplained.
- Encode speed at the top levels. Zeros at level 9 still take 31 s for 200 kB —
  pre-existing, not introduced by the fix above, but worth a skip-ahead for long
  runs of the kind brotli uses at `BROTLI_LONG_COPY_QUICK_STEP`.
