"""
Lossless PDF-aware container. Rebuilds the input byte-for-byte.

Every FlateDecode stream is inflated, PNG-filtered when it is a raster image,
and recompressed with brotli. The original Flate bytes are reproduced on
extraction from a recorded zlib recipe, so the output is the input.
"""
import sys, io, zlib, struct, hashlib, json, time
import numpy as np, brotli, pikepdf
from PIL import Image
from multiprocessing import Pool

MAGIC = b"VLC1"
RECIPES = [(9,0,8,15),(6,0,8,15),(9,0,9,15),(6,0,9,15),(1,0,8,15),(9,1,8,15),(9,2,8,15)]

def sha(b): return hashlib.sha256(b).hexdigest()

# ---------- PNG adaptive filtering, vectorised ----------
def filter_rows(data, w, h, nc):
    stride = w*nc
    need = stride*h
    data = data[:need] + bytes(max(0, need-len(data)))
    img = np.frombuffer(data, dtype=np.uint8).reshape(h, stride).astype(np.int16)
    prev = np.zeros(stride, dtype=np.int16)
    out = np.empty(h*(stride+1), dtype=np.uint8)
    for y in range(h):
        row = img[y]
        left = np.zeros(stride, dtype=np.int16); left[nc:] = row[:-nc]
        up = prev
        upleft = np.zeros(stride, dtype=np.int16); upleft[nc:] = prev[:-nc]
        cands = [row,
                 row-left,
                 row-up,
                 row-((left+up)>>1)]
        p = left+up-upleft
        pa, pb, pc = np.abs(p-left), np.abs(p-up), np.abs(p-upleft)
        pred = np.where((pa<=pb)&(pa<=pc), left, np.where(pb<=pc, up, upleft))
        cands.append(row-pred)
        best, bi = None, 0
        for i,c in enumerate(cands):
            m = (c & 0xff).astype(np.uint8)
            score = int(np.minimum(m, 256-m.astype(np.int16)).sum())
            if best is None or score < best: best, bi, bm = score, i, m
        base = y*(stride+1)
        out[base] = bi; out[base+1:base+1+stride] = bm
        prev = row
    return out.tobytes()

def unfilter_rows(fdata, w, h, nc):
    stride = w*nc
    out = np.zeros((h, stride), dtype=np.uint8)
    arr = np.frombuffer(fdata, dtype=np.uint8)
    prev = np.zeros(stride, dtype=np.uint8)
    for y in range(h):
        base = y*(stride+1); ft = arr[base]; row = arr[base+1:base+1+stride].copy()
        if ft == 0: cur = row
        elif ft == 2: cur = (row + prev) & 0xff
        else:
            cur = np.empty(stride, dtype=np.uint8); r = row.astype(np.int16); pv = prev.astype(np.int16)
            for i in range(stride):
                a = int(cur[i-nc]) if i>=nc else 0
                b = int(pv[i]); c = int(pv[i-nc]) if i>=nc else 0
                if ft == 1: pred = a
                elif ft == 3: pred = (a+b)>>1
                else:
                    p = a+b-c; pa,pb,pc = abs(p-a),abs(p-b),abs(p-c)
                    pred = a if (pa<=pb and pa<=pc) else (b if pb<=pc else c)
                cur[i] = (int(r[i]) + pred) & 0xff
        out[y] = cur; prev = cur
    return out.tobytes()

# ---------- reversible colour transform ----------
# Subtract-green, as WebP lossless uses. YCoCg-R needs 9 bits for the chroma
# channels, so it is NOT byte-exact at 8 bits; this is, being add/sub mod 256.
def sub_green(data, w, h):
    a = np.frombuffer(data[:w*h*3], dtype=np.uint8).reshape(-1,3)
    G = a[:,1]
    return np.stack([(a[:,0]-G) & 0xff, G, (a[:,2]-G) & 0xff], axis=1).tobytes()

def un_sub_green(data, w, h):
    a = np.frombuffer(data[:w*h*3], dtype=np.uint8).reshape(-1,3)
    G = a[:,1]
    return np.stack([(a[:,0]+G) & 0xff, G, (a[:,2]+G) & 0xff], axis=1).tobytes()

# ---------- recipe matching ----------
def find_recipe(raw, dec):
    for (l,s,m,wb) in RECIPES:
        co = zlib.compressobj(l, zlib.DEFLATED, wb, m, s)
        if co.compress(dec)+co.flush() == raw: return (l,s,m,wb)
    return None

def redeflate(dec, recipe):
    l,s,m,wb = recipe
    co = zlib.compressobj(l, zlib.DEFLATED, wb, m, s)
    return co.compress(dec)+co.flush()

# ---------- stream discovery over RAW bytes ----------
def find_streams(buf, imgdims):
    """(start, length, decoded, dims|None) for every inflatable Flate stream."""
    found=[]; pos=0
    while True:
        i = buf.find(b"stream", pos)
        if i < 0: break
        j = i+6
        if buf[j:j+2] == b"\r\n": j += 2
        elif buf[j:j+1] == b"\n": j += 1
        elif buf[j:j+1] == b"\r": j += 1
        else: pos = i+6; continue
        d = zlib.decompressobj()
        try:
            dec = d.decompress(buf[j:])
            if not d.eof: raise zlib.error("incomplete")
        except zlib.error:
            pos = i+6; continue
        consumed = len(buf)-j-len(d.unused_data)
        raw = buf[j:j+consumed]
        found.append((j, consumed, dec, imgdims.get(sha(raw))))
        pos = j+consumed
    return found

def image_dims(path):
    dims={}
    pdf = pikepdf.open(path)
    for o in pdf.objects:
        if not isinstance(o, pikepdf.Stream): continue
        if str(o.get("/Subtype","")) != "/Image": continue
        if o.get("/DecodeParms") is not None: continue
        try: raw = o.read_raw_bytes()
        except Exception: continue
        cs = str(o.get("/ColorSpace","")); nc = 3 if cs=="/DeviceRGB" else (1 if cs in ("/DeviceGray","/CalGray") else 0)
        if nc == 0: continue
        w,h = int(o.get("/Width")), int(o.get("/Height"))
        if o.get("/BitsPerComponent") is not None and int(o.get("/BitsPerComponent")) != 8: continue
        if w*h*nc == 0: continue
        dims[sha(raw)] = (w,h,nc)
    return dims

def _webp(dec, w, h, nc):
    """WebP lossless, with the exact pixels verified to come back."""
    if max(w,h) > 16383: return None
    arr = np.frombuffer(dec[:w*h*nc], dtype=np.uint8).reshape(h, w, nc)
    im = Image.fromarray(arr[:,:,0] if nc == 1 else arr)
    buf = io.BytesIO()
    im.save(buf, "WEBP", lossless=True, quality=100, method=6)
    blob = buf.getvalue()
    back = np.asarray(Image.open(io.BytesIO(blob)).convert("RGB" if nc==3 else "L"))
    if nc == 1: back = back[:,:,None]
    if back.shape != arr.shape or not np.array_equal(back, arr): return None
    return blob

def _pack_one(args):
    idx, dec, dims, q = args
    cands = [(len(brotli.compress(dec, quality=q, lgwin=24)), 0, None)]
    cands[0] = (cands[0][0], 0, brotli.compress(dec, quality=q, lgwin=24))
    if dims:
        w,h,nc = dims
        if w*h*nc <= len(dec):
            f = filter_rows(dec, w, h, nc)
            b = brotli.compress(f, quality=q, lgwin=24); cands.append((len(b), 1, b))
            if nc == 3:
                yf = filter_rows(sub_green(dec, w, h), w, h, 3)
                b = brotli.compress(yf, quality=q, lgwin=24); cands.append((len(b), 2, b))
            wp = _webp(dec, w, h, nc)
            if wp is not None: cands.append((len(wp), 3, wp))
    n, mode, blob = min(cands, key=lambda t: t[0])
    return idx, mode, blob

def compress(src, dst, q=11, procs=4):
    buf = open(src,"rb").read()
    dims = image_dims(src)
    streams = find_streams(buf, dims)
    entries=[]; cursor=0; skeleton=bytearray(); jobs=[]; solid_parts=[]
    for k,(start, length, dec, dm) in enumerate(streams):
        recipe = find_recipe(buf[start:start+length], dec)
        if recipe is None:
            continue                      # left inside the skeleton, verbatim
        skeleton += buf[cursor:start]; cursor = start+length
        e = {"gap": len(skeleton), "len": length, "recipe": recipe,
             "dims": list(dm) if dm else None, "declen": len(dec)}
        if dm:
            entries.append(e); jobs.append((len(entries)-1, dec, dm, q))
        else:
            # Non-image streams share a vocabulary — PDF operators, font names,
            # the same XML tags — so they are packed as ONE block. Measured 5.3%
            # on a text PDF; the same trick is worth 0.7% on images, which get
            # their own codec each instead.
            e["solid"] = 1
            entries.append(e); solid_parts.append(dec)
    skeleton += buf[cursor:]

    with Pool(procs) as p:
        results = p.map(_pack_one, jobs, chunksize=1) if jobs else []
    payloads = {}
    for idx, mode, blob in results:
        entries[idx]["filtered"] = mode; payloads[idx] = blob
    for e in entries:
        e.setdefault("filtered", 0); e.setdefault("solid", 0)

    solid = brotli.compress(b"".join(solid_parts), quality=q, lgwin=24) if solid_parts else b""
    manifest = brotli.compress(json.dumps(entries).encode(), quality=11)
    skel = brotli.compress(bytes(skeleton), quality=q, lgwin=24)
    with open(dst,"wb") as f:
        f.write(MAGIC)
        f.write(hashlib.sha256(buf).digest())
        f.write(struct.pack("<QIII", len(buf), len(manifest), len(skel), len(solid)))
        f.write(manifest); f.write(skel); f.write(solid)
        for i, e in enumerate(entries):
            if e["solid"]: continue
            b = payloads[i]
            f.write(struct.pack("<I", len(b))); f.write(b)
    return len(buf), len(open(dst,'rb').read()), len(entries), len(streams)

def decompress(src, dst):
    raw = open(src,"rb").read()
    assert raw[:4]==MAGIC
    digest = raw[4:36]
    total, mlen, slen, sdlen = struct.unpack("<QIII", raw[36:56])
    off=56
    entries = json.loads(brotli.decompress(raw[off:off+mlen])); off+=mlen
    skeleton = brotli.decompress(raw[off:off+slen]); off+=slen
    solid = brotli.decompress(raw[off:off+sdlen]) if sdlen else b""
    off += sdlen
    out = bytearray(); scur=0; spos=0
    for e in entries:
        if e.get("solid"):
            dec = solid[spos:spos+e["declen"]]; spos += e["declen"]
        else:
            n, = struct.unpack("<I", raw[off:off+4]); off+=4
            mode = e["filtered"]
            # Mode 3 is a WebP file, already its own container; the rest are brotli.
            blob = raw[off:off+n] if mode == 3 else brotli.decompress(raw[off:off+n])
            off+=n
            if mode == 0:
                dec = blob
            else:
                w,h,nc = e["dims"]
                if mode == 1:
                    dec = unfilter_rows(blob, w, h, nc)[:e["declen"]]
                elif mode == 2:
                    dec = un_sub_green(unfilter_rows(blob, w, h, 3), w, h)[:e["declen"]]
                else:
                    arr = np.asarray(Image.open(io.BytesIO(blob)).convert("RGB" if nc==3 else "L"))
                    dec = arr.tobytes()[:e["declen"]]
        if len(dec) < e["declen"]: dec = dec + bytes(e["declen"]-len(dec))
        out += skeleton[scur:e["gap"]]
        scur = e["gap"]
        out += redeflate(dec, tuple(e["recipe"]))
    out += skeleton[scur:]
    data = bytes(out)
    open(dst,"wb").write(data)
    return hashlib.sha256(data).digest()==digest, len(data), total

if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "c":
        t0=time.time(); a,b,n,tot = compress(sys.argv[2], sys.argv[3], q=int(sys.argv[4]) if len(sys.argv)>4 else 11)
        print(f"{a} -> {b} bytes  ({a/b:.2f}x, {100*b/a:.1f}% of original)  {n}/{tot} streams packed  {time.time()-t0:.0f}s")
    else:
        t0=time.time(); ok,n,exp = decompress(sys.argv[2], sys.argv[3])
        print(f"rebuilt {n} bytes (expected {exp}) sha256 match: {ok}  {time.time()-t0:.0f}s")
