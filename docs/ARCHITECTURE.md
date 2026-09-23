# How `kl` works

An LZ77 front end feeding one of two entropy back ends, chosen per block by
whichever actually produces fewer bytes.

## Front end: finding the repeats

A 4-byte hash indexes a chain of earlier positions. Two parsers run:

- **Greedy, priced.** Walk forward, take the best match at each position, with a
  one-byte lazy look-ahead. Cheap.
- **Optimal.** A forward dynamic program over each segment that prices every
  route in real bits, carries the literal-run length and the distance cache
  along each path, and is iterated so the parse and the code lengths converge on
  each other. This is the Zopfli idea.

Both run and the smaller output wins. The optimal parse is not universally
better — it loses on image residuals — so it is never assumed.

## Distances: the part that mattered most

A four-entry cache of recent distances, addressed by **sixteen short codes**:
the four cached values, plus the two most recent each at ±1, ±2, ±3. This is
brotli's `kDistanceCacheIndex`/`kDistanceCacheOffset` table.

It exists because real distances cluster with small perturbations. On a PDF
content-stream corpus the four commonest offsets were 298 (13.7% of matches),
296 (9.3%), 244730 (6.1%) and 24 (5.4%) — near-misses of each other. An
exact-match cache hits 52.5% of matches; the short codes hit 80.8%.

**Widening the alphabet alone makes things worse** (+6,548 bytes), and so does
cache-aware pricing alone (+24,675). Together they save 106,606. The parse has
to be hunting for reusable distances *and* able to name them cheaply; either one
without the other is a regression. This is the single most important fact about
the codec and it cost several failed attempts to learn.

## Back end A: canonical Huffman

Separate code tables for literals, literal-run lengths, match lengths and
distance buckets. Literal tables are adaptive — the encoder prices one table
against four context-selected ones and keeps the cheaper, because the four-way
context was measured to buy under 0.3% while costing 7–14% of decode speed.

Decoding is built for speed: one 64-bit refill instead of a byte-at-a-time loop,
all literals decoded up front in a single tight pass so the sequence loop copies
rather than decodes, and 8-byte-wide match copies.

## Back end B: binary range coder

LZMA-shaped. 11-bit adaptive probabilities, a twelve-state token machine,
matched-literal coding against the byte the last match would have repeated, and
bit-tree coding of lengths and distance buckets.

Huffman spends a whole bit on anything, so a symbol that is 90% likely still
costs one. A range coder charges the true −log2(p). It wins on structured data
and costs roughly 4× decode speed, so it is only offered from level 4 up and
only used when it actually produces a smaller block.

## Levels

Nine rungs, each a measured point on the ratio-versus-speed frontier, not a
guess. Levels 1–3 are single-pass; 4–9 iterate parse and prices. Levels 8–9 run
several (chain depth, round count) configurations and keep the smallest, because
**chain depth is not convex** — 64 and 4096 both beat 256 on document streams,
and a single hand-picked value inverted the ladder.

## Format

    [version<<4 | backend][u32 original length][u32 FNV-1a of the original][payload]

The version nibble lets a decoder refuse what it does not understand. The hash
is there because a range decoder reads zeros past the end of its input, so a
truncated block would otherwise decode into a full-length buffer of plausible
garbage and report success. Four bytes to turn silent corruption into a refusal.
