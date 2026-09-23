"""The container, using ONLY codecs we built. No brotli, no WebP, no zlib
for the payloads — zlib appears only where the PDF format itself demands it,
to reproduce the original Flate bytes exactly.

Inner codec is chosen per block: kl (LZ77+Huffman) or cm (context mixing),
whichever is smaller, recorded per block so extraction knows which to run.

  python3 vlc_own.py c in.pdf out.own [level]
  python3 vlc_own.py d out.own back.pdf
"""
import hashlib, json, os, struct, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pdf_container as vlc  # find_streams, find_recipe, redeflate, filters

ROOT = os.path.dirname(HERE)
KL = os.path.join(ROOT, "kl")   # built by `make`
CM = os.path.join(ROOT, "cm")
MAGIC = b"OWN1"


def _run(tool, mode, data, extra=None):
    with tempfile.TemporaryDirectory() as td:
        i, o = os.path.join(td, "i"), os.path.join(td, "o")
        open(i, "wb").write(data)
        cmd = [tool, mode, i, o] + ([str(extra)] if extra is not None else [])
        subprocess.run(cmd, check=True, capture_output=True)
        return open(o, "rb").read()


def enc_best(data, level=9):
    """Smallest of our two codecs. Tag 1 = kl, 2 = cm."""
    if not data:
        return 1, b""
    a = _run(KL, "c", data, level)
    # cm is quadratic-ish in wall time; only ask it about blocks it can finish.
    if len(data) <= 8 << 20:
        b = _run(CM, "c", data, 0)
        if len(b) < len(a):
            return 2, b
    return 1, a


def dec_one(tag, blob, declen):
    if not blob:
        return b""
    return _run(KL if tag == 1 else CM, "d", blob)


def compress(src, dst, level=9):
    buf = open(src, "rb").read()
    dims = vlc.image_dims(src)
    streams = vlc.find_streams(buf, dims)
    entries, cursor, skeleton, solid_parts, img_jobs = [], 0, bytearray(), [], []
    for start, length, dec, dm in streams:
        recipe = vlc.find_recipe(buf[start:start + length], dec)
        if recipe is None:
            continue
        skeleton += buf[cursor:start]
        cursor = start + length
        e = {"gap": len(skeleton), "recipe": recipe, "declen": len(dec),
             "dims": list(dm) if dm else None}
        if dm:
            e["solid"] = 0
            entries.append(e)
            img_jobs.append((len(entries) - 1, dec, dm))
        else:
            e["solid"] = 1
            entries.append(e)
            solid_parts.append(dec)
    skeleton += buf[cursor:]

    payloads = {}
    for idx, dec, dm in img_jobs:
        w, h, nc = dm
        cands = [(dec, 0)]
        if w * h * nc <= len(dec):
            cands.append((vlc.filter_rows(dec, w, h, nc), 1))
            if nc == 3:
                cands.append((vlc.filter_rows(vlc.sub_green(dec, w, h), w, h, 3), 2))
        best = None
        for payload, mode in cands:
            tag, blob = enc_best(payload, level)
            if best is None or len(blob) < len(best[2]):
                best = (mode, tag, blob)
        entries[idx]["filtered"], entries[idx]["tag"] = best[0], best[1]
        payloads[idx] = best[2]

    solid_tag, solid = enc_best(b"".join(solid_parts), level) if solid_parts else (1, b"")
    skel_tag, skel = enc_best(bytes(skeleton), level)
    man_tag, manifest = enc_best(json.dumps(entries).encode(), level)

    with open(dst, "wb") as f:
        f.write(MAGIC)
        f.write(hashlib.sha256(buf).digest())
        f.write(struct.pack("<QIIIBBB", len(buf), len(manifest), len(skel), len(solid),
                            man_tag, skel_tag, solid_tag))
        f.write(manifest); f.write(skel); f.write(solid)
        for i, e in enumerate(entries):
            if e["solid"]:
                continue
            b = payloads[i]
            f.write(struct.pack("<I", len(b))); f.write(b)
    return len(buf), os.path.getsize(dst), len(entries), len(streams)


def decompress(src, dst):
    raw = open(src, "rb").read()
    assert raw[:4] == MAGIC
    digest = raw[4:36]
    total, mlen, slen, sdlen, mt, st, sot = struct.unpack("<QIIIBBB", raw[36:59])
    off = 59
    manifest = dec_one(mt, raw[off:off + mlen], 0); off += mlen
    entries = json.loads(manifest)
    skeleton = dec_one(st, raw[off:off + slen], 0); off += slen
    solid = dec_one(sot, raw[off:off + sdlen], 0) if sdlen else b""
    off += sdlen

    out, scur, spos = bytearray(), 0, 0
    for e in entries:
        if e["solid"]:
            dec = solid[spos:spos + e["declen"]]; spos += e["declen"]
        else:
            n, = struct.unpack("<I", raw[off:off + 4]); off += 4
            blob = raw[off:off + n]; off += n
            payload = dec_one(e["tag"], blob, e["declen"])
            mode = e["filtered"]
            if mode == 0:
                dec = payload
            else:
                w, h, nc = e["dims"]
                if mode == 1:
                    dec = vlc.unfilter_rows(payload, w, h, nc)[:e["declen"]]
                else:
                    dec = vlc.un_sub_green(vlc.unfilter_rows(payload, w, h, 3), w, h)[:e["declen"]]
        if len(dec) < e["declen"]:
            dec += bytes(e["declen"] - len(dec))
        out += skeleton[scur:e["gap"]]
        scur = e["gap"]
        out += vlc.redeflate(dec, tuple(e["recipe"]))
    out += skeleton[scur:]
    data = bytes(out)
    open(dst, "wb").write(data)
    return hashlib.sha256(data).digest() == digest, len(data), total


if __name__ == "__main__":
    cmd = sys.argv[1]
    t0 = time.time()
    if cmd == "c":
        a, b, n, tot = compress(sys.argv[2], sys.argv[3],
                                int(sys.argv[4]) if len(sys.argv) > 4 else 9)
        print(f"{a} -> {b} bytes  ({a/b:.2f}x)  {n}/{tot} streams  {time.time()-t0:.0f}s")
    else:
        ok, n, exp = decompress(sys.argv[2], sys.argv[3])
        print(f"rebuilt {n} bytes (expected {exp}) sha256 match: {ok}  {time.time()-t0:.0f}s")
