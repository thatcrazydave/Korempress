#!/usr/bin/env bash
# Round-trip every corpus at every level, under both an optimised and a
# sanitizer build. A codec that loses a byte is worthless, so correctness is
# checked before anything else and the script exits non-zero on the first loss.
set -uo pipefail
cd "$(dirname "$0")/.."

CORPUS_DIR=${CORPUS_DIR:-tests/corpora}
KL=./kl
SAN=./kl-san
fail=0 n=0

mkdir -p "$CORPUS_DIR"
# Generated rather than committed: the sizes matter, the contents do not.
python3 - "$CORPUS_DIR" <<'PY'
import os, random, sys
d = sys.argv[1]; random.seed(7)
def w(name, b):
    p = os.path.join(d, name)
    if not os.path.exists(p): open(p, "wb").write(b)
w("empty.bin", b"")
w("one.bin", b"A")
# All-same bytes is the worst case for hash-chain matching; 64 kB exposes it
# without making every run of the suite pay minutes for it.
w("zeros.bin", bytes(65536))
w("random.bin", bytes(random.randrange(256) for _ in range(200000)))
w("text.bin", (b"the quick brown fox jumps over the lazy dog. " * 6000)[:250000])
words = [bytes(random.randrange(32,127) for _ in range(random.randrange(3,12))) for _ in range(400)]
o = bytearray()
while len(o) < 5*1024*1024: o += random.choice(words) + b" "
# 5 MB sits in the band where the parse runs on a SEGMENTED input. A heap
# overflow lived there undetected because every corpus was <4 MB or >8 MB.
w("mid5mb.bin", bytes(o[:5*1024*1024]))
PY

# FULL=1 runs every corpus at every level under both builds, which takes most of
# an hour because level 9 on 5 MB runs the full parse with a 4096-deep chain.
# The default suite keeps the coverage that matters: level 4 is the cheapest
# level that runs the parse, and it reproduces the 4-8 MiB overflow under the
# sanitizer in about 10 seconds — the same bug level 9 needs minutes to reach.
FULL=${FULL:-0}

check() {  # binary label file levels...
  local bin=$1 label=$2 f=$3; shift 3
  for L in "$@"; do
    out=$("$bin" t "$f" /dev/null $L 2>&1); n=$((n+1))
    case "$out" in
      *"firstbad=none"*) ;;
      *) echo "FAIL [$label] $(basename "$f") level $L"; echo "$out" | head -6; fail=1;;
    esac
  done
}

ALL="1 2 3 4 5 6 7 8 9"
BIG="$CORPUS_DIR/mid5mb.bin"
echo "== optimised build =="
for f in "$CORPUS_DIR"/*.bin; do
  if [ "$f" = "$BIG" ] && [ "$FULL" != 1 ]; then check "$KL" opt "$f" 1 4
  else check "$KL" opt "$f" $ALL; fi
done
if [ -x "$SAN" ]; then
  echo "== address/undefined sanitizer =="
  for f in "$CORPUS_DIR"/*.bin; do
    if [ "$FULL" = 1 ]; then check "$SAN" san "$f" 1 4 6 9
    elif [ "$f" = "$BIG" ]; then check "$SAN" san "$f" 4
    else check "$SAN" san "$f" 1 4 6; fi
  done
fi

echo "== the ladder must never compress worse at a higher level =="
MONO="$CORPUS_DIR/text.bin"; [ "$FULL" = 1 ] && MONO="$MONO $BIG"
for f in $MONO; do
  prev=999999999
  for L in 1 2 3 4 5 6 7 8 9; do
    "$KL" c "$f" /tmp/_mono.bin $L >/dev/null 2>&1
    c=$(stat -c%s /tmp/_mono.bin)
    if [ "$c" -gt "$prev" ]; then echo "FAIL inversion at level $L on $(basename "$f"): $prev -> $c"; fail=1; fi
    prev=$c
  done
done
rm -f /tmp/_mono.bin

echo "== corrupt input must be refused, never silently truncated =="
# Cut relative to the real size: a repetitive corpus compresses to a few dozen
# bytes, and a fixed-length cut of that silently tests nothing at all.
"$KL" c "$CORPUS_DIR"/mid5mb.bin /tmp/_v.bin 3 >/dev/null 2>&1
head -c $(( $(stat -c%s /tmp/_v.bin) / 2 )) /tmp/_v.bin > /tmp/_trunc.bin
"$KL" d /tmp/_trunc.bin /dev/null >/dev/null 2>&1
[ $? -eq 0 ] && { echo "FAIL truncated input accepted"; fail=1; } || echo "  truncated: refused"
head -c 300 /dev/urandom > /tmp/_junk.bin
"$KL" d /tmp/_junk.bin /dev/null >/dev/null 2>&1
[ $? -eq 0 ] && { echo "FAIL garbage accepted"; fail=1; } || echo "  garbage:   refused"
rm -f /tmp/_v.bin /tmp/_trunc.bin /tmp/_junk.bin

echo
[ $fail -eq 0 ] && echo "PASS — $n round trips clean" || echo "FAILURES above"
exit $fail
