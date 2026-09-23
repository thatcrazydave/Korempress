# Measured results

Every figure here was produced on one machine and every compressed output was
decompressed and compared against the original. Nothing is quoted from a vendor
benchmark. Sizes are bytes.

## The container, on a real 399,649-byte PDF

| approach | bytes | ratio | pack time | rebuild |
|---|---:|---:|---:|---|
| the PDF as-is | 399,649 | — | — | — |
| gzip -9, whole file | 293,161 | 1.36× | — | — |
| brotli -11, whole file | 276,234 | 1.45× | — | — |
| xz -9e, whole file | 272,984 | 1.46× | — | — |
| container + brotli -11 | 127,805 | 3.13× | 6 s | sha256 identical |
| **container + `kl -9`** | **103,551** | **3.86×** | 74 s | sha256 identical |

Two separate effects, and it is worth keeping them apart:

- **The container is worth 2.2×** over the best whole-file codec (276,234 →
  127,805). That comes from understanding PDF structure, not from entropy coding.
- **Our codec is worth a further 19%** over brotli inside the same container
  (127,805 → 103,551), at roughly 12× the pack time.

## The codec alone, against brotli at maximum

Same input bytes to every codec; `kl` measured in-process so it is not charged
for process startup.

| corpus | `kl` | brotli -11 | ratio |
|---|---:|---:|---:|
| **PDF content streams, 2.18 MB** | **102,080** | 127,197 | **0.80×** |
| text (prose), 400 kB | 130,651 | 121,579 | 1.07× |
| image pixels (raw), 16.9 MB | 4,253,652 | 3,912,478 | 1.09× |
| image residuals (filtered), 16.9 MB | 3,137,343 | 2,811,310 | 1.12× |

We win on document streams by 19.7% and trail by 7–12% elsewhere.

## Against zstd

zstd is far faster in both directions — 15–40× on compression, 2–4× on
decompression — and `zstd -19` beats our best output on the three corpora where
brotli also does. On size at comparable settings we are ahead of its mid ladder;
on speed we are not remotely close, and no amount of tuning changes that without
rewriting the match finder.

## What is not good

- **Encode speed.** Level 9 runs several configurations and is slow by design.
  Level 3 is the practical setting when time matters.
- **Images.** The ~330 kB gap to brotli on the two image corpora has never been
  investigated. Nobody has looked at image data at all.
- **Why we now beat brotli on documents is unexplained.** The ~1,900 bytes of
  Huffman tables and context maps brotli must transmit, which our adaptive
  models never send, is the plausible source — but that is reasoning, not a
  measurement, and it is listed here as an open question rather than an answer.
