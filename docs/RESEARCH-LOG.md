# Compression research — working code

Experimental. Nothing here is wired into the platform. Kept in the repository
because the work is iterative and the sandbox it was built in is ephemeral.

The findings behind these files are written up in [`R&D.md`](../../R&D.md);
this directory is the code those measurements were taken with.

## What is here

| file | what it is |
|---|---|
| `image_format_cases.py` | Rebuilds the same PDF with JPEG / PNG-predictor images to show how much the container's gain depends on how the images were stored. |
| `pdf_container_own_codecs.py` | The same container wired to `kl`/`cm` only — no brotli, no WebP. The honest head-to-head. |
| `pdf_container.py` | PDF-aware lossless container. Inflates every Flate stream, PNG-filters the rasters, recompresses, and reproduces the original **byte for byte** from a recorded zlib recipe. |
| `cm.c` | Context-mixing entropy coder. Binary arithmetic coder driven by a logistic mixture of six models. Maximum ratio, very slow. |
| `kl.c` | LZ77 + canonical Huffman codec. Lazy parsing, repeat-offsets, context-selected literal tables. Built for decode speed. |
| `brotli_stress.py` | Eight corpora chosen to find where brotli loses. |
| `solid_test.py` | Whether a document's images share enough structure for cross-image matching to pay. |
| `bench.py` | Ratio and throughput against gzip / brotli / xz / zstd. |

## Building

```bash
gcc -O3 -march=native -o cm cm.c -lm
gcc -O3 -march=native -o kl kl.c

./kl c in out 9     # compress    ./kl d out back    # decompress
./kl b in /dev/null 9   # in-process decode benchmark
./kl t in /dev/null 9   # self-test: encode, decode, report first mismatch
```

Build with `-fsanitize=address,undefined` before trusting any change; both
codecs are verified clean on empty, single-byte, all-zero, random, text and
image inputs.

## Measured, on this machine

Decode throughput is in-process, so it excludes process start and file I/O.

| codec | filtered image, 16.9 MB | vs brotli | decode |
|---|---:|---:|---:|
| gzip -9 | 3,397,842 | +20.9% | 312 MB/s |
| **kl** (LZ77+Huffman) | **3,160,053** | **+12.4%** | **285 MB/s** |
| brotli -11 | 2,811,310 | — | 201 MB/s |
| **cm** (context mixing) | **2,472,753** | −12.0% | 3.6 MB/s |

The two home-made codecs sit at opposite ends on purpose. `cm` beats brotli on
ratio by 12% and loses 79× on speed; `kl` beats brotli on speed and loses 12%
on ratio. That trade is the point — there is no single setting that wins both,
so a pipeline picks per tier rather than per codec.

### Closing kl's ratio gap

`kl` started 22.6% behind brotli. Instrumenting where the bytes went settled
the direction: **82% of the output was the sequence stream, not the literals**,
and offsets dominated it — 655k matches over a 16 MB window at ~22 bits each.
The offsets are also almost perfectly scattered (the most common is 1.2% of
matches), which is what LZ77 looks like when it is the wrong tool for the data.

| change | bytes | gap |
|---|---:|---:|
| lazy parsing, repeat-offsets, context literals | 3,447,183 | +22.6% |
| drop the per-sequence "has match" flag | 3,343,073 | +18.9% |
| two-pass **cost-aware parser** | 3,243,645 | +15.4% |
| price-aware match choice, iterated prices | 3,214,166 | +14.3% |
| 16 MB window | 3,200,560 | +13.8% |
| three repeat-offset slots, deeper search | **3,160,053** | **+12.4%** |

The cost-aware parser is most of it. Pricing each candidate in real bits and
rejecting matches that cost more than the literals they replace dropped 21% of
the matches and made the file *smaller*. Decode speed did not move — all the
work is in the encoder, which is where it belongs.

### How much of this depends on how the images were stored

Same document, images re-encoded three ways, same container, all byte-exact:

| the PDF's images are | pdf | container | ratio |
|---|---:|---:|---:|
| raw RGB + Flate, **no predictor** | 11,947,867 | 6,503,476 | **1.84×** |
| Flate **with** a PNG predictor | 9,954,125 | 7,513,571 | 1.32× |
| JPEG q90 (DCTDecode) | 6,253,282 | 4,793,822 | 1.30× |

The big win needs a producer that left the rasters unfiltered. Where the
images were already packed properly the container still returns ~30%, from the
non-image streams and from recompressing what is left.

**AVIF is not an answer here.** Pillow's AVIF at `quality=100` is *not*
pixel-exact — verified on the largest image, twice. It is an excellent lossy
codec (22× at q50 against 1.93× for WebP lossless) and the wrong tool for a
pipeline that must return the original bytes.

## Does it hold on a realistic document?

The 11.9 MB test PDF is 93% raster images — a worst case. A 400 kB text PDF
with **no images at all** (16 content streams, 396 kB stored, 2.18 MB decoded)
is much closer to what a platform actually receives:

| approach | bytes | ratio |
|---|---:|---:|
| the PDF as-is | 399,649 | — |
| gzip -9 | 293,161 | 1.36× |
| xz -9e (best generic) | 272,984 | 1.46× |
| **this container** | **127,805** | **3.13×** |

**The container is 2.1× better than the best generic codec**, byte-identical.
That is the whole case for being format-aware: un-deflating first exposes
2.18 MB of highly repetitive operator streams that a generic pass never sees,
because from outside they are already-compressed noise.

The same trick applies to DOCX and PPTX, which are ZIPs of XML. On a
DOCX-shaped archive: compressing the file gets 1.79×, unzipping first and
recompressing the members together gets **2.79×**. Note that `unzip + gzip` is
*worse* than gzipping the archive — the exposed redundancy spans more than
gzip's 32 kB window, so the trick only pays with a codec that can reach it.

### Our codecs alone, on the same document

No brotli, no WebP anywhere. Byte-identical either way:

| approach | bytes | ratio | inner codec |
|---|---:|---:|---|
| the PDF as-is | 399,649 | — | |
| gzip -9 | 293,161 | 1.36× | |
| xz -9e | 272,984 | 1.46× | |
| brotli -11 | 276,234 | 1.45× | |
| **container, OUR codecs only** | **314,098** | **1.27×** | kl/cm |
| **container + brotli inside** | **127,805** | **3.13×** | brotli |

Our codecs lose, and the reason is specific rather than general. Brotli's own
quality ladder on this data:

    brotli -5    297,826
    brotli -9    285,035
    brotli -11   127,197     <- 2.2x better than its own level 9
    kl -9        312,484

### The optimal parse, and where kl stands now

`kl` was brotli-9 class. Adding a Zopfli-style **optimal parse** — a forward
dynamic program over each segment, pricing every route in real bits, iterated
so the parse and the code lengths converge on each other — took it past:

| corpus | kl | brotli-9 | brotli-11 | vs 9 | vs 11 |
|---|---:|---:|---:|---:|---:|
| PDF content streams, 2.18 MB | **257,097** | 285,035 | 127,197 | **−9.8%** | +102% |
| filtered image, 16.9 MB | 3,149,902 | 3,138,115 | 2,811,310 | +0.4% | +12.0% |
| text, 400 kB | 134,728 | 133,944 | 121,579 | +0.6% | +10.8% |

**Matches or beats brotli-9 on all three.** On documents it beats it by 10%.

The optimal parse is not universally better, which is why the encoder runs
**both parsers every round and keeps the smaller output**. It wins by 18% on
document data and contributes nothing on image residuals.

The sampling was suspected and cleared. Rewriting candidate generation to
enumerate (offset, longest-length) pairs — giving each length its nearest
offset, which is what the old code did through a 66,000-entry table cleared at
every position — made the parse **2.3× faster** (114s → 50s on the document
corpus) at the same ratio, and removed the sampling. It still does not help
images: on the 16.9 MB corpus, 41 minutes of parse returns 3,149,902, the
identical byte count the greedy parse reaches in seconds. The greedy priced
parse is already finding essentially the best decomposition there, so it is
capped at 8 MB and the greedy result stands.

Bounding it mattered. The first version never finished on all-zeros — scanning
every match to full length at every position is quadratic — so match scans cap
at 4096 bytes and long lengths are enumerated coarsely.

### Making it faster, and fixing the ladder

Three changes to the decoder, none touching the format: the bit reader refills
with one 64-bit load instead of a byte-at-a-time loop, every literal is decoded
up front in one pass so the sequence loop copies rather than decodes, and
matches copy 8 bytes at a time. **Decode is 1.3–1.5× faster on every corpus at
every level.** The encoder got word-at-a-time match extension, a four-byte
rejection before extending a repeat-offset candidate, and tables sized to the
input rather than a fixed 64 MB chain and a 1 MB clear.

The literal tables are now chosen by measurement: the encoder prices one table
against four and keeps the cheaper. The four-way context was buying under 0.3%
while costing 7–14% of decode speed, and on text it was worse on **both** axes.

**The level ladder had two real defects.** Levels 2/3 and 4/5/6 produced
identical output, and level 7 was dominated outright — its deep-chain greedy
gave 315,735 at 2.76 MB/s where the dynamic program with a *shallow* chain gave
271,356 at 3.33 MB/s, better on both axes. The 4096-deep chain was costing 55×
for 6%; the parse was buying 15% for 1.3×. Every rung is now a measured point
on the frontier.

Three bugs surfaced while rebuilding it, none reachable from the old ladder:

- **Parser choice was smuggled in as a level number.** The round loop called its
  "priced greedy" pass with a hardcoded level 7, which only produced greedy
  because the old ladder gated the parse on `level >= 8`. It is an argument now.
- **A segfault above 8 MB at levels 4+.** The encoder and the parse checked the
  parse's size cap in different places, so above it one disabled the parse and
  the other still ran it. They share one predicate now, and the corpus set
  gained a 9.4 MB case — every existing test file was under the cap.
- **Chain depth is not convex.** 64 and 4096 both beat 256 on document streams,
  which made level 8 compress 1.5% *worse* than level 7. Levels 8 and 9 now run
  several (chain, rounds) configurations and keep the smallest, each set
  containing the one below it. Levels 4–7 keep one configuration: their residual
  inversions are ~0.2% and covering those cost 5× the encode time.

Against the previous build, same corpora, same protocol:

| corpus | before | after | speed |
|---|---:|---:|---|
| PDF streams, level 7 | 312,484 | **265,332** | 0.25 → **0.59 MB/s** |
| image residuals, level 6 | 3,441,673 | **3,249,991** | — |
| text, best reachable | 134,824 @ 0.22 MB/s | **134,615** | level 5 now reaches 134,783 at 1.77 MB/s |

The first two rows are strict wins — smaller **and** faster. Verified clean under
`-fsanitize=address,undefined` over **85 round trips** — nine corpora × nine
levels, plus a 9.4 MB case above the parse's size cap, which is the case that
caught the segfault. The ladder no longer inverts on any corpus.

### Beating brotli-11 on document streams

`kl` now compresses the PDF content streams to **102,077 bytes** where brotli at
quality 11 reaches 127,197 — **19.7% smaller**, round-trip byte-exact.

| corpus | kl | brotli-11 | ratio |
|---|---:|---:|---:|
| **PDF streams, 2.18 MB** | **102,077** | 127,197 | **0.80×** |
| text, 400 kB | 130,651 | 121,579 | 1.07× |
| image pixels, 16.9 MB | 4,253,652 | 3,912,478 | 1.09× |
| image residuals, 16.9 MB | 3,137,343 | 2,811,310 | 1.12× |

**The 81,482-byte deficit decomposed exactly**, by handing our unchanged coder
brotli's own command stream and re-scoring both parses under one cost model:

| cause | bytes | share |
|---|---:|---:|
| the parse chose distances the cache could not reuse | 54,814 | 67% |
| our distance alphabet was too narrow — 3 exact slots, no ±1/±2/±3 | 26,668 | 33% |
| literal context width (`LC_RC` 3→4) | 766 | ~1% |
| brotli's 122 kB static dictionary | 136 | 0.2% |
| header tables — **in our favour**, brotli sends 1,908 bytes we never do | −1,908 | −2% |

**The two causes are multiplicative, not additive, which is why every
single-lever attempt failed.** Measured, each alone is a REGRESSION: the widened
alphabet on the old parse costs +6,548, cache-aware pricing on the old alphabet
costs +24,675. Together they save 106,606. The interaction is worth ~46,000
bytes. The distance-cache hit rate went 52.5% → 80.8%; brotli's is 93.9%.

So the earlier 217,311 → 238,644 regression was not a mystery — it was the first
of those two bullets. The explanation offered for it at the time (an optimal
parse pricing against the Huffman model while the output was range-coded) is
**wrong**: the regression reproduces with the parse frozen bit-for-bit, and the
working fix contains no range-coder pricing at all.

### Failing loudly

A range decoder reads zeros past the end of its input, so a truncated block used
to decode into a full-length buffer of plausible garbage and report success —
the worst failure a compressor has. The header now carries a format version and
four bytes of FNV-1a over the original, checked on decode:

| input | before | now |
|---|---|---|
| truncated | full buffer of garbage, exit 0 | refused, exit 3 |
| bit-flipped payload | undetected | refused, exit 3 |
| older format | 0 bytes, exit 0 | refused, exit 3 |
| garbage | 0 bytes, exit 0 | refused, exit 3 |

Cost: four bytes. Binaries built before this carry the old silent behaviour and
cannot be fixed retroactively.

### A range coder, and what reading brotli actually showed

Huffman spends a whole bit on anything, so a symbol that is 90% likely still
costs one. `kl` now carries a second entropy back end — an LZMA-style binary
range coder with adaptive per-context models, matched-literal coding and a
twelve-state token machine — chosen per block against the Huffman one by a tag
byte, so it is used only where it actually produces fewer bytes.

| corpus | Huffman | range coder | brotli-11 | gap |
|---|---:|---:|---:|---:|
| text, 400 kB | 134,615 | **130,802** | 121,579 | 1.08× |
| PDF streams, 2.18 MB | 257,106 | **208,679** | 127,197 | 1.64× |
| image residuals, 16.9 MB | 3,249,991 | **3,138,228** | 2,811,310 | 1.12× |
| image pixels, 16.9 MB | 4,585,637 | **4,259,097** | 3,912,478 | 1.09× |

Image residuals now land within **113 bytes of brotli-9**. The cost is decode
speed, roughly 600 → 155 MB/s, which is why the Huffman back end stays and why
the range coder is only offered from level 4 up.

**The notebook's standing theory about the brotli-11 gap was wrong.** It named
the combined insert-and-copy symbol and literal context modelling as the
remaining suspects. Brotli was cloned, built and ablated feature by feature on
the PDF streams, and on that corpus its 127,197 decomposes as:

| feature removed | bytes | cost |
|---|---:|---:|
| literal context modelling | 127,985 | +0.6% |
| block splitting (lgblock 16 / 24) | 127,601 / 126,744 | ±0.3% |
| mode TEXT / FONT | 127,197 / 127,303 | ~0% |
| **the Zopfli parse (quality 10)** | 185,304 | **+45.7%** |
| **the 16 MB window (lgwin 16)** | 257,454 | **+102%** |

Context modelling is worth 0.6% here. It is the parse and long-range matching.

Instrumenting brotli's own parse settles where the rest goes. The two parses are
the same shape — brotli 180,363 commands covering 97.8% at 11.84 bytes average,
ours 174,923 covering 97.2% at 12.1 — but brotli hits its distance cache on
**93.9%** of matches against our **48.6%**, so it spends about 4.3 bits a
command where we spend 10.4. Its mechanism is sixteen short distance codes
(`kDistanceCacheIndex`/`kDistanceCacheOffset` in `c/enc/backward_references_hq.c`):
the last four distances, plus the top two at ±1, ±2, ±3. Our offsets cluster
exactly that way — 298 at 13.7%, 296 at 9.3%, 289 at 4.0%.

**Implementing those sixteen codes made it worse** — 217,311 → 238,644, and a
cheaper one-bit escape for the commonest code gave 238,885. The cause is not the
coding: the optimal parse prices every route against the **Huffman** model while
the winning output is **range-coded**, so widening the candidate set only lets it
make more confident wrong choices. Closing the rest of the gap means the parse
and the entropy model sharing one cost function, which is what gives brotli its
94% cache-hit rate. That is a redesign, not a feature.

### Against brotli and zstd, honestly

Everything round-trip verified, `kl` measured in-process so none of the three is
charged for process start.

| corpus | our best | zstd -19 | brotli -9 | brotli -11 |
|---|---:|---:|---:|---:|
| text, 400 kB | 134,615 | **128,854** | 133,944 | **121,579** |
| PDF streams, 2.18 MB | 257,106 | 190,493 | 285,035 | **127,197** |
| image residuals, 16.9 MB | 3,249,991 | 3,034,769 | 3,138,115 | **2,811,310** |
| image pixels, 16.9 MB | 4,585,637 | 4,398,306 | 4,500,521 | **3,912,478** |

**We beat zstd on size at comparable settings and brotli beats us almost
everywhere.** Against zstd's mid ladder we win: kl -4 on PDF streams is 271,357
against zstd -12's 276,487, and on raw pixels 2.8% smaller. But zstd packs
15–40× faster and unpacks 2–4×, and **zstd -19 beats our best on all four
corpora**. Against brotli it is worse: brotli -9 is smaller *and* faster than
our best on text (41× the speed), on image residuals (3.4% smaller, 10×), and
on raw pixels (1.9% smaller, 4.4×).

The one corpus where our whole ladder beats zstd's fast ladder is **raw image
pixels** — kl -3 at 4,610,298 against zstd -5's 4,835,556. Row-filtered
residuals are the opposite: filtering is already a compression step, and what
it leaves behind has little for an LZ77 matcher to find.

### Against brotli-11 and zstd at maximum, honestly

Ratio and both speeds, on the PDF content streams. Decode is in-process for
every codec, so none is charged for process start.

| codec | bytes | ratio | compress MB/s | decompress MB/s |
|---|---:|---:|---:|---:|
| kl -6 | 332,785 | 6.56 | **6.57** | — |
| kl -9 | 257,105 | 8.49 | 0.04 | 372 |
| brotli -11 | **127,197** | **17.16** | 0.44 | 552 |
| zstd -19 | 190,493 | 11.46 | 1.86 | 921 |
| zstd -22 | 190,230 | 11.47 | 1.02 | 843 |
| xz -9e | 181,152 | 12.05 | 0.83 | 138 |

**kl loses on all three axes here**, and it is not close: half brotli-11's
ratio, 11× slower to compress, and slower to decode than both.

Decode across corpora, which is where the picture is least uniform:

| corpus | kl | brotli -11 | zstd -22 |
|---|---:|---:|---:|
| filtered image, 16.9 MB | **285 MB/s** | 169 | 601 |
| PDF streams, 2.18 MB | 372 | 552 | 843 |
| text, 400 kB | 203 | 275 | 712 |

An earlier note here claimed kl decodes faster than brotli. **That holds only
on image residuals.** On documents and text brotli is ahead, and zstd is ahead
of both everywhere. The claim was drawn from the image corpus and generalised
without checking, which was wrong.

### What still separates kl from brotli-11, and what it is not

Six hypotheses were tested and killed:

| suspect | test | result |
|---|---|---|
| hash table too small | 18/20/22/24 bits | byte-identical output |
| search too shallow | 24→512 candidates, 4k→64k chain | 0.5% |
| match scan capped | 4096 → 65535 | 13 bytes |
| literal context mode | brotli GENERIC/TEXT/FONT | identical |
| block splitting | 64 kB/256 kB/1 MB chunks | worse for *both* codecs |
| pure context modelling | order-0..4 conditional entropy | order-4 floor is 325,489 — worse than kl already achieves |

The last row is the interesting one. brotli-11 reaches **0.466 bits/byte** on
this data, far below the order-4 context floor of 1.193, while exact long
repeats cover only 0.7% of it. So brotli is not winning on context modelling
and not on long matches.

It matters what this data *is*: not prose but **vector path coordinates written
as ASCII decimals** — `514.58562 333.851526 514.498458 ... c` — 98.6%
printable, and the commonest bytes are all digits. Numbers that are nearly but
never exactly alike, which is why kl's matches average 12 bytes, about one
coordinate. The remaining suspect is brotli's combined insert-and-copy symbol,
which codes the (literal run, match length) pair as one of 704 symbols rather
than two independent codes; at 175k sequences, a few bits each is most of the
gap. Untested.

### Who is actually doing the work

On that text PDF's decoded streams:

| codec | bytes | vs the PDF's own Flate |
|---|---:|---:|
| **brotli -11** | **127,197** | **3.11×** |
| xz -9e | 181,152 | 2.19× |
| zstd -19 | 190,493 | 2.08× |
| `kl` | 356,279 | 1.11× |
| `cm` | 400,535 | 0.99× |

**Brotli, not the home-made codecs.** `cm` is worse than gzip here because it
has no LZ stage and this data is long exact repeats; `kl` has one but a weaker
match finder and no static dictionary, and brotli's 122 kB dictionary of common
text is aimed squarely at ASCII operator streams. The container's contribution
is the format-awareness, not the entropy coding — `cm` earns its place only on
image residuals, and `kl` only where decode speed matters more than ratio.

## Where brotli is weak

From `brotli_stress.py`. Percentages are how much larger brotli's output is
than the best of zstd-22 / xz-9e on the same input.

| case | brotli loses by |
|---|---:|
| newline JSON, one shared schema | **118%** |
| uint32 column table | 10.4% |
| 2 MB repeated across a 20 MB gap | 9.1% |
| sorted uint32 ids | 2.8% |

The pattern: brotli is excellent on short-range text and correctly refuses
already-compressed input, and it falls behind whenever redundancy is further
apart than its 16 MB window, or the data is numeric rather than textual. The
Python binding caps `lgwin` at 24, so the large-window mode the C library has
is not reachable from Python at all.
