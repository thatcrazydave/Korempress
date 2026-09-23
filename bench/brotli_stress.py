"""Where does brotli actually lose? Each case targets one suspected weakness."""
import brotli, zlib, lzma, numpy as np, time, os, sys
import zstandard as zstd
rng=np.random.default_rng(42)

def mk_cases():
    c={}
    # 1. long-range repeats beyond the window
    a=rng.integers(0,256,(1<<20,),dtype=np.uint8).tobytes()
    c["long-range: 1MB block repeated 8x"]=a*8
    # 2. repeats just past the 16MB default window
    b=rng.integers(0,256,(2<<20,),dtype=np.uint8).tobytes()
    c["far repeat: 2MB, 20MB gap, 2MB"]=b+rng.integers(0,256,(20<<20,),dtype=np.uint8).tobytes()+b
    # 3. fixed-width numeric records (columnar, no byte repeats)
    n=400000
    arr=np.stack([np.arange(n)%1000, (np.arange(n)*7)%65536, np.arange(n)//3],axis=1).astype(np.uint32)
    c["numeric table (uint32 x3)"]=arr.tobytes()
    # 4. float64 time series
    t=np.arange(300000); c["float64 sine series"]=(np.sin(t/97.0)*1000).astype(np.float64).tobytes()
    # 5. highly-structured text with long-distance repeats
    para=("the quick brown fox jumps over the lazy dog. "*40).encode()
    c["text, 4000 paragraphs, 200 distinct"]=b"".join(para+str(i%200).encode() for i in range(4000))
    # 6. sorted integers (delta-friendly)
    c["sorted uint32 ids"]=np.sort(rng.integers(0,1<<31,(500000,),dtype=np.uint32)).tobytes()
    # 7. many tiny records with shared prefix (JSON-ish)
    c["json lines, shared schema"]=b"".join(b'{"id":%d,"name":"user_%d","role":"member","active":true}\n'%(i,i) for i in range(30000))
    # 8. already compressed
    c["already-compressed (zstd blob)"]=zstd.ZstdCompressor(level=10).compress(rng.integers(0,256,(4<<20,),dtype=np.uint8).tobytes())
    return c

cz19=zstd.ZstdCompressor(level=19); cz22=zstd.ZstdCompressor(level=22,write_content_size=False)
def row(name,d):
    n=len(d); out={}
    for label,fn in [("brotli-11/w24",lambda: brotli.compress(d,quality=11,lgwin=24)),
                     
                     ("zstd-19",lambda: cz19.compress(d)),
                     ("zstd-22",lambda: cz22.compress(d)),
                     ("xz-9e",lambda: lzma.compress(d,preset=9|lzma.PRESET_EXTREME))]:
        t0=time.time(); b=fn(); out[label]=(len(b),time.time()-t0)
    best=min(out.items(), key=lambda kv: kv[1][0])
    br=out["brotli-11/w24"][0]
    print(f"\n{name}  ({n:,} bytes)")
    for k,(sz,tt) in out.items():
        mark=" <-- best" if k==best[0] else ""
        gap=f"  brotli is {sz/br:.2f}x" if k!="brotli-11/w24" else ""
        print(f"   {k:<16}{sz:>12,}  {n/sz:>7.1f}:1  {n/1e6/tt:>6.1f} MB/s{mark}")
    if best[0]!="brotli-11/w24":
        print(f"   >>> brotli LOSES here by {100*(br/best[1][0]-1):.1f}%")

for name,d in mk_cases().items(): row(name,d)
