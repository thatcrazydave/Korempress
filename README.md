# korelearn-compression

A lossless compression toolkit written from scratch: an LZ77 codec with two
entropy back ends, and a PDF-aware container that rebuilds files byte-identically.

On a real 399,649-byte PDF the container plus our codec reaches **103,551 bytes
(3.86×)** where brotli at its maximum setting applied to the whole file reaches
276,234 (1.45×). The rebuilt file is SHA-256 identical to the original.

## Two pieces, and they do different jobs

**`kl` — the codec.** LZ77 matching feeding either canonical Huffman or a binary
range coder, whichever produces fewer bytes for that block. On PDF content
streams it compresses to **102,080** against brotli -11's 127,197 — 19.7%
smaller. On prose and image data it trails brotli by 7–12%.

**The container — a PDF-aware transform.** A PDF's streams are already
Flate-compressed, so a general codec has nothing left to find. The container
un-deflates each stream, recompresses the original bytes, and records the exact
zlib parameters needed to reproduce the original Flate bytes on the way back.
That last step is what makes it lossless at the *file* level rather than just the
stream level. See [docs/CONTAINER.md](docs/CONTAINER.md).

The container is worth 2.2× on its own; the codec is worth a further 19% inside
it. They are independent wins and worth keeping separate in your head.

## Quickstart

```sh
make                       # builds ./kl and ./cm
./kl c input output 9      # compress, level 1-9
./kl d output restored     # decompress
./kl m input /dev/null 9   # measure in-process: size, encode/decode seconds
./kl t input /dev/null 9   # self-test round trip

make test                  # round trips, sanitizers, ladder, corruption refusal (~5 min)
make test-full             # every corpus at every level under both builds (~1 hour)
```

The container needs `pikepdf`:

```sh
python3 container/pdf_container_own_codecs.py c in.pdf out.klc 9   # our codec
python3 container/pdf_container_own_codecs.py d out.klc back.pdf
python3 container/pdf_container.py c in.pdf out.klc 11             # brotli inside
```

## Honest limits

- **Degenerate input is slow at high levels.** 200 kB of zeros takes 31 s at
  level 9. Do not expose levels 7–9 to untrusted input without a time limit.
- **Encode speed.** Level 9 runs several configurations and is slow on purpose.
  Level 3 is the setting to use when time matters. zstd compresses 15–40× faster
  than we do and always will without a new match finder.
- **`zstd -19` and `brotli -11` still beat us** on prose and both image corpora.
  Documents are the one place we are ahead.
- **Images are the weak spot.** The ~330 kB gap there has never been
  investigated.
- **Why we beat brotli on documents is not established** — only that we
  reproducibly do.

Full numbers in [docs/RESULTS.md](docs/RESULTS.md). The design is in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). The dead ends, including three
confidently-held theories that turned out false, are in
[docs/FINDINGS.md](docs/FINDINGS.md) — that file is the most useful one if you
are picking this up to develop further.

## Benchmark corpora are not included

The figures in `docs/` were measured on streams extracted from two private
PDFs, so those files are deliberately not in this repository — publishing them
would publish the documents. `bench/` expects them beside the scripts; point it
at your own. `tests/` generates its own corpora and needs nothing external.

## Correctness

Correctness is the only property that matters absolutely; a codec that loses a
byte is worthless however small its output. `make test` round-trips every corpus
at every level under an optimised build and an address/undefined sanitizer
build, checks that a higher level never compresses worse, and checks that
truncated and corrupt input is *refused* rather than silently decoded into
garbage.

Two bugs have shipped in this codec and both were found by adversarial review
rather than by the test suite — a heap overflow reachable only between 4 MiB and
8 MiB, and silent truncation. Both are described in
[docs/FINDINGS.md](docs/FINDINGS.md), with the corpus hole that hid the first
one. Regression cases for both are in `tests/`.

## Layout

```
src/kl.c          the codec
src/cm.c          a context-mixing coder — an earlier experiment, kept for reference
container/        the PDF-aware container
bench/            benchmark harnesses against brotli, zstd, xz, gzip
tests/            round-trip, sanitizer, ladder and corruption tests
docs/             architecture, container rationale, results, findings
```

`src/cm.c` predates `kl` and is not used by anything. It is kept because it is
the reference point that showed pure context mixing loses to LZ77 plus entropy
coding on this data.

## Licence

MIT — see [LICENSE](LICENSE).

The sixteen-entry short-distance table in `src/kl.c` (`KDCI`/`KDCO`) is the same
table as brotli's `kDistanceCacheIndex`/`kDistanceCacheOffset`, from
[google/brotli](https://github.com/google/brotli), which is also MIT-licensed.
It is sixteen small integers; it is credited here because it is where the idea
came from, not because the licence requires it.
