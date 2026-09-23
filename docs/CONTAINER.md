# The container: what it does and why it exists

## The problem it solves

A PDF is not a blob of data. It is a skeleton of object definitions and
cross-reference tables wrapped around embedded **streams** — page content, font
programs, images — and almost every one of those streams is *already* compressed,
usually with Flate (zlib/deflate).

So running a general-purpose compressor over a PDF means compressing data that
has already been compressed. There is almost nothing left to find. Measured on a
399,649-byte text PDF:

| whole-file codec | bytes | ratio |
|---|---:|---:|
| gzip -9 | 293,161 | 1.36× |
| brotli -11 | 276,234 | 1.45× |
| xz -9e | 272,984 | 1.46× |

Brotli at its maximum setting recovers 1.45×. That is the ceiling for treating a
PDF as opaque bytes, and no amount of entropy coding moves it, because the
redundancy was consumed by the producer's deflate pass years before we saw it.

## What the container does instead

1. **Parse** the PDF and locate every stream.
2. **Un-deflate** each stream back to the bytes the producer actually had.
3. **Recompress** those original bytes with a far stronger codec.
4. **Record a recipe** — the exact zlib parameters (level, strategy, memLevel,
   windowBits) that reproduce the original Flate bytes from the original data.
5. Store the skeleton, the recipes, and the recompressed payloads.

Decoding runs it backwards: decompress each payload, re-deflate it with the
recorded recipe, and slot the result back into the skeleton.

## Why step 4 is the whole design

Without it, you have a *different PDF*. You could recompress the streams with
something better and emit a valid document, but its bytes would not match the
original, its hash would not match, and any signature over it would break. For
stored user documents that is not lossless, whatever the pixels look like.

The recipe search is what makes the container lossless at the **file** level
rather than merely the stream level. A stream whose recipe cannot be found is
left exactly as it was — never re-encoded on a guess. Every result in this
repository is verified by SHA-256 over the rebuilt file, not by eyeballing it.

## Two more things it does

- **Solid packing.** Non-image streams are concatenated and compressed as one
  block, so they share a dictionary. Separately compressing 18 small streams
  wastes a fresh model on each.
- **Image filtering.** Image streams get PNG-style adaptive row filtering, and
  three-channel images are additionally tried with a subtract-green transform.
  YCoCg-R was rejected: it is not byte-exact at 8 bits, because its chroma needs
  9. Subtract-green is reversible exactly.

## What it is for

Stored user documents. A PDF held at 3× less size is 3× less storage and 3× less
egress, and the reconstruction is byte-identical, so hashes, signatures and
deduplication all still work. It is a storage-layer transform, not a viewer
format — nothing downstream needs to understand it, because what comes out is
the original file.

## What it is not good at

Image-heavy documents. On an 11.9 MB paper full of figures the container reaches
1.84× against 3.13× on a text PDF, because the image payloads dominate the byte
count and resist compression once filtered. The container's advantage comes from
understanding PDF structure, and a scanned page has very little structure to
understand.
