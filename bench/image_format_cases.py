"""What happens when the PDF's images are JPEG (DCTDecode) instead of raw Flate?

The claim to test: 'if the images were jpeg or png, those issues will arise'.
Build the same document three ways and run the same lossless container on each.
"""
import io, os, subprocess, sys
import numpy as np
import pikepdf
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import vlc

SRC = os.path.join(HERE, "paper.pdf")


def rebuild(dst, mode):
    """mode: 'jpeg' (DCTDecode q90), 'png' (Flate + PNG predictor), 'avif-lossless'."""
    pdf = pikepdf.open(SRC)
    for o in list(pdf.objects):
        if not isinstance(o, pikepdf.Stream) or str(o.get("/Subtype", "")) != "/Image":
            continue
        if o.get("/DecodeParms") is not None:
            continue
        cs = str(o.get("/ColorSpace", ""))
        nc = 3 if cs == "/DeviceRGB" else (1 if cs == "/DeviceGray" else 0)
        if nc == 0 or int(o.get("/BitsPerComponent") or 8) != 8:
            continue
        w, h = int(o.get("/Width")), int(o.get("/Height"))
        b = o.read_bytes()
        if len(b) < w * h * nc:
            continue
        arr = np.frombuffer(b[:w * h * nc], dtype=np.uint8).reshape(h, w, nc)
        im = Image.fromarray(arr.squeeze() if nc == 1 else arr)
        if mode == "jpeg":
            buf = io.BytesIO()
            im.convert("RGB" if nc == 3 else "L").save(buf, "JPEG", quality=90, optimize=True)
            o.write(buf.getvalue(), filter=pikepdf.Name("/DCTDecode"))
            o["/ColorSpace"] = pikepdf.Name("/DeviceRGB" if nc == 3 else "/DeviceGray")
        elif mode == "png":
            # Flate WITH a PNG Up-predictor, which is what a careful producer emits.
            filt = vlc.filter_rows(b, w, h, nc)
            import zlib
            o.write(zlib.compress(filt, 9), filter=pikepdf.Name("/FlateDecode"))
            o["/DecodeParms"] = pikepdf.Dictionary(
                Predictor=15, Colors=nc, BitsPerComponent=8, Columns=w)
    pdf.save(dst, compress_streams=True,
             object_stream_mode=pikepdf.ObjectStreamMode.generate)


def container(path):
    out = path + ".vlc"
    subprocess.run([sys.executable, os.path.join(HERE, "vlc.py"), "c", path, out, "11"],
                   check=True, capture_output=True)
    back = path + ".back"
    subprocess.run([sys.executable, os.path.join(HERE, "vlc.py"), "d", out, back],
                   check=True, capture_output=True)
    ok = open(path, "rb").read() == open(back, "rb").read()
    n = os.path.getsize(out)
    os.remove(back)
    return n, ok


print(f"{'variant':<26}{'pdf bytes':>12}{'container':>12}{'ratio':>8}  exact")
orig = os.path.getsize(SRC)
n, ok = container(SRC)
print(f"{'raw Flate, no predictor':<26}{orig:>12,}{n:>12,}{orig/n:>8.2f}  {ok}")

for mode, label in [("jpeg", "JPEG q90 inside (DCTDecode)"), ("png", "Flate + PNG predictor")]:
    dst = os.path.join(HERE, f"variant_{mode}.pdf")
    rebuild(dst, mode)
    sz = os.path.getsize(dst)
    n, ok = container(dst)
    print(f"{label:<26}{sz:>12,}{n:>12,}{sz/n:>8.2f}  {ok}")

# And what AVIF would do to ONE image, lossless vs lossy — a different product.
pdf = pikepdf.open(SRC)
big = None
for o in pdf.objects:
    if isinstance(o, pikepdf.Stream) and str(o.get("/Subtype", "")) == "/Image" \
       and o.get("/DecodeParms") is None and str(o.get("/ColorSpace", "")) == "/DeviceRGB":
        if big is None or len(o.read_raw_bytes()) > len(big.read_raw_bytes()):
            big = o
w, h = int(big.get("/Width")), int(big.get("/Height"))
arr = np.frombuffer(big.read_bytes()[:w * h * 3], dtype=np.uint8).reshape(h, w, 3)
im = Image.fromarray(arr)
stored = len(big.read_raw_bytes())
print(f"\nbiggest image {w}x{h}, {stored:,} bytes as stored in the PDF")


def enc(fmt, **kw):
    b = io.BytesIO()
    im.save(b, fmt, **kw)
    return b.getvalue()


for label, fmt, kw, lossless in [
    ("WebP lossless", "WEBP", dict(lossless=True, quality=100, method=6), True),
    ("AVIF lossless", "AVIF", dict(quality=100, speed=4), True),
    ("AVIF q80 (LOSSY)", "AVIF", dict(quality=80, speed=4), False),
    ("JPEG q90 (LOSSY)", "JPEG", dict(quality=90, optimize=True), False),
]:
    try:
        blob = enc(fmt, **kw)
        exact = ""
        if lossless:
            back = np.asarray(Image.open(io.BytesIO(blob)).convert("RGB"))
            exact = "  pixel-exact" if np.array_equal(back, arr) else "  NOT pixel-exact"
        print(f"  {label:<20}{len(blob):>10,}  {stored/len(blob):>5.2f}x vs stored{exact}")
    except Exception as e:
        print(f"  {label:<20}  failed: {e}")
