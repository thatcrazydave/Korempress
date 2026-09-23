"""Do the 22 images share structure? Compress separately vs as one solid stream.

If they do, a shared window / cross-image matching is worth real bytes and the
container should stop treating each image as an island.
"""
import os, pickle, sys, time
import pikepdf, brotli
import zstandard as zstd

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vlc

CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "filt.pkl")
PDF = os.path.join(os.path.dirname(os.path.abspath(__file__)), "paper.pdf")

if os.path.exists(CACHE):
    filt = pickle.load(open(CACHE, "rb"))
else:
    pdf = pikepdf.open(PDF)
    imgs = []
    for o in pdf.objects:
        if not isinstance(o, pikepdf.Stream):
            continue
        if str(o.get("/Subtype", "")) != "/Image" or o.get("/DecodeParms") is not None:
            continue
        cs = str(o.get("/ColorSpace", ""))
        nc = 3 if cs == "/DeviceRGB" else (1 if cs == "/DeviceGray" else 0)
        if nc == 0 or int(o.get("/BitsPerComponent") or 8) != 8:
            continue
        imgs.append((len(o.read_raw_bytes()), int(o.get("/Width")), int(o.get("/Height")), nc, o))
    imgs.sort(key=lambda t: -t[0])
    filt = [vlc.filter_rows(o.read_bytes(), w, h, nc) for _, w, h, nc, o in imgs]
    pickle.dump(filt, open(CACHE, "wb"), protocol=4)

joined = b"".join(filt)
print(f"{len(filt)} images, {len(joined):,} filtered bytes", flush=True)


def zparams(level, ldm, wlog):
    return zstd.ZstdCompressor(
        compression_params=zstd.ZstdCompressionParameters.from_level(
            level, enable_ldm=ldm, window_log=wlog))


TESTS = [
    ("zstd-12            ", lambda d: zparams(12, False, 23).compress(d)),
    ("zstd-12 ldm win=28 ", lambda d: zparams(12, True, 28).compress(d)),
    ("zstd-19 ldm win=28 ", lambda d: zparams(19, True, 28).compress(d)),
    ("brotli-9  win=24   ", lambda d: brotli.compress(d, quality=9, lgwin=24)),
]

for name, fn in TESTS:
    t0 = time.time(); sep = sum(len(fn(f)) for f in filt); ts = time.time() - t0
    t0 = time.time(); sol = len(fn(joined)); tj = time.time() - t0
    verdict = f"SOLID WINS by {sep - sol:,}" if sol < sep else f"separate wins by {sol - sep:,}"
    print(f"{name} separate {sep:>11,} ({ts:>5.0f}s)   solid {sol:>11,} ({tj:>5.0f}s)   {verdict}", flush=True)
