import subprocess, time, os, zlib, brotli, lzma, sys
def t(fn):
    a=time.time(); r=fn(); return r, time.time()-a
def mine(path, stride):
    a=time.time(); subprocess.run(["./cm","c",path,path+".cm",str(stride)],check=True); ct=time.time()-a
    n=os.path.getsize(path+".cm")
    a=time.time(); subprocess.run(["./cm","d",path+".cm",path+".back"],check=True); dt=time.time()-a
    ok = open(path,'rb').read()==open(path+".back",'rb').read()
    os.remove(path+".back")
    return n, ct, dt, ok

CASES=[("filtered image (3260x1726 RGB)","img_filtered.bin",9781),
       ("raw image pixels","img_raw.bin",9780),
       ("text (RD.md)","text.bin",0)]
for label,path,stride in CASES:
    data=open(path,'rb').read(); n0=len(data)
    print(f"\n=== {label} — {n0:,} bytes ===")
    rows=[]
    # entropy-coder-only reference: Huffman, no LZ
    h,ht=t(lambda: zlib.compress(data,9) if False else None)
    co=zlib.compressobj(9,zlib.DEFLATED,-15,9,zlib.Z_HUFFMAN_ONLY)
    a=time.time(); hb=co.compress(data)+co.flush(); ht=time.time()-a
    rows.append(("zlib Huffman-only (entropy only)",len(hb),ht,None,True))
    a=time.time(); g=zlib.compress(data,9); gt=time.time()-a
    rows.append(("gzip -9 (LZ77+Huffman)",len(g),gt,None,True))
    a=time.time(); b=brotli.compress(data,quality=11,lgwin=24); bt=time.time()-a
    rows.append(("brotli -11 (LZ77+CM entropy)",len(b),bt,None,True))
    a=time.time(); x=lzma.compress(data,preset=9|lzma.PRESET_EXTREME); xt=time.time()-a
    rows.append(("xz -9e (LZ77+range coder)",len(x),xt,None,True))
    n,ct,dt,ok = mine(path,stride)
    rows.append((f"MINE: cm (entropy only, stride={stride})",n,ct,dt,ok))
    print(f"{'codec':<38}{'bytes':>11}{'ratio':>8}{'enc MB/s':>10}{'dec MB/s':>10}{'rt':>5}")
    for name,sz,et,dt2,ok in rows:
        dd = f"{n0/1e6/dt2:>10.1f}" if dt2 else f"{'-':>10}"
        print(f"{name:<38}{sz:>11,}{n0/sz:>8.2f}{n0/1e6/et:>10.1f}{dd}{('ok' if ok else 'FAIL'):>5}")
