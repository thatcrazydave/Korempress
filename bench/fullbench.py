import subprocess, time, os, brotli, lzma, tempfile
import zstandard as zstd
def best(fn, r):
    b=1e9
    for _ in range(r):
        t=time.time(); fn(); b=min(b,time.time()-t)
    return b
d=open("doc6_decoded.bin","rb").read(); n=len(d)
print(f"corpus: PDF content streams, {n:,} bytes\n")
rows=[]
for lvl in (6,7,9):
    with tempfile.TemporaryDirectory() as td:
        i=os.path.join(td,'i'); o=os.path.join(td,'o'); b2=os.path.join(td,'b')
        open(i,'wb').write(d)
        t0=time.time(); subprocess.run(["./kl","c",i,o,str(lvl)],check=True); ct=time.time()-t0
        sz=os.path.getsize(o)
        dt=best(lambda: subprocess.run(["./kl","d",o,b2],check=True), 3)
        rows.append((f"kl -{lvl}", sz, ct, dt))
cz19=zstd.ZstdCompressor(level=19); cz22=zstd.ZstdCompressor(level=22,write_content_size=False)
dz=zstd.ZstdDecompressor()
for lbl,comp,dec in [("brotli -11", lambda: brotli.compress(d,quality=11,lgwin=24), brotli.decompress),
                     ("zstd -19", lambda: cz19.compress(d), lambda b: dz.decompress(b, max_output_size=n)),
                     ("zstd -22", lambda: cz22.compress(d), lambda b: dz.decompress(b, max_output_size=n)),
                     ("xz -9e", lambda: lzma.compress(d,preset=9|lzma.PRESET_EXTREME), lzma.decompress)]:
    t0=time.time(); blob=comp(); ct=time.time()-t0
    dt=best(lambda: dec(blob), 5)
    rows.append((lbl, len(blob), ct, dt))
print(f"{'codec':<12}{'bytes':>10}{'ratio':>8}{'comp MB/s':>11}{'decomp MB/s':>13}")
for lbl,sz,ct,dt in rows:
    print(f"{lbl:<12}{sz:>10,}{n/sz:>8.2f}{n/1e6/ct:>11.2f}{n/1e6/dt:>13.0f}")
