#!/usr/bin/env bash
# Phase 2 of IPL recompilation — the "modify-before-recomp" step for the
# DMA-loaded stage-2 (see docs/DYNAMIC-CODE.md).
#
# The IPL boots in two stages: stage-1 (loader) at 0x81200000 DMA-copies
# stage-2 (the menu) from the EXI mask-ROM into 0x81300000+. A single static
# recompile of the descrambled image gets stage-2 wrong (the bytes at
# 0x81300000 differ from what the DMA lands). So we let stage-1 RUN (its EXI
# model performs the real DMA, oracle-validated), DUMP the post-DMA MEM1 image,
# and recompile THAT — both stages then have correct func_.
#
# Prerequisites: phase-1 gcn_boot already built (runtime/generate.sh, then a
# -DGCN_WITH_GENERATED=ON build) and bios/ipl.bin present.
set -euo pipefail
export PATH="/c/msys64/mingw64/bin:$PATH"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DESC="$ROOT/tools/ipl_descramble/build/ipl_descramble.exe"
DOL="$ROOT/recompiler/build/dolrecomp.exe"
BOOT="$ROOT/runtime/build-boot/gcn_boot.exe"
IPL="$ROOT/bios/ipl.bin"
for f in "$DESC" "$DOL" "$BOOT" "$IPL"; do
  [ -f "$f" ] || { echo "error: missing $f (build phase-1 first: build.sh, generate.sh, then -DGCN_WITH_GENERATED build)"; exit 1; }
done

# The mingw exes (dolrecomp / gcn_boot) fopen native Windows paths, not git-bash
# mounts like /tmp/... — so make the work dir a real Windows-form path (via
# cygpath) and hand it to every tool verbatim. rm from git-bash accepts it too.
TMP_UNIX="$(mktemp -d)"
TMP="$(cygpath -w "$TMP_UNIX" | sed 's|\\|/|g')"
trap 'rm -rf "$TMP_UNIX"' EXIT

echo "[1/4] descramble IPL (full image + BS2 payload)"
"$DESC" "$IPL" -o "$TMP/descr.bin" --bs2 "$TMP/bs2.bin"

echo "[2/4] run stage-1 to DMA stage-2, dump post-DMA image + low-mem handlers"
# Two regions: the DMA-loaded stage-2 (0x81200000) AND the BS2 exception handlers
# BS2 installs in low memory (0x80000000..0x80003000) before the first `sc`. The
# runtime stops at that `sc`, at which point both are present in MEM1.
GCN_IPL_ROM="$TMP/descr.bin" \
GCN_MEM_DUMP="0x81200000:0x270000:$TMP/postdma.bin;0x80000000:0x3000:$TMP/lowmem.bin" \
  "$BOOT" "$TMP/bs2.bin" 5000000 >/dev/null 2>&1 || true   # runs through the sc to the block budget
[ -f "$TMP/postdma.bin" ] || { echo "error: no post-DMA dump produced"; exit 1; }
[ -f "$TMP/lowmem.bin" ]  || { echo "error: no low-mem dump produced"; exit 1; }

# The first syscall below enters physical 0x00000C00, dispatched through the
# cached 0x80000C00 alias. A stale/early dump can be the right size yet contain
# zeroes here, producing generated `.long 0` fallthrough all the way to
# 0x80003000. Reject that tree before it can replace runtime/generated/.
python3 - "$TMP/lowmem.bin" <<'PYEOF'
import sys

data = open(sys.argv[1], "rb").read()
vector = data[0xC00:0xD00]
if len(data) != 0x3000 or not vector or vector in (bytes(len(vector)), b"\xff" * len(vector)):
    raise SystemExit("error: low-memory syscall vector is empty/invalid")
if bytes.fromhex("4c000064") not in vector:  # rfi
    raise SystemExit("error: low-memory syscall vector contains no rfi")
print("low-memory syscall vector: PASS")
PYEOF

echo "[3/4] recompile the post-DMA image + low-mem handlers into one table"
# Primary image = stage-1/2 (base 0x81200000). Extra segment = low-memory
# exception handlers at 0x80000000; the 0xC00 syscall vector routes there via
# the dispatch's physical-PC alias (0xC00 -> 0x80000C00).
"$DOL" --gamecube-ipl "$TMP/postdma.bin" "$TMP/out" --base 0x81200000 --entry 0x81200150 \
  --segment "0x80000000:$TMP/lowmem.bin" -j8 >/dev/null

echo "[4/4] install into runtime/generated/"
GEN="$(dirname "$(find "$TMP/out" -name generated.h | head -1)")"
[ -n "$GEN" ] || { echo "error: recompiler produced no generated.h"; exit 1; }
grep -Rqs '80000C00: mfhid0' "$GEN/chunks" || {
  echo "error: generated low-memory chunk did not decode the syscall vector"
  exit 1
}
rm -rf "$ROOT/runtime/generated"; cp -r "$GEN" "$ROOT/runtime/generated"
echo "done -> runtime/generated ($(find "$ROOT/runtime/generated/chunks" -name '*.c' | wc -l) chunks, stage-1 + stage-2)"
echo "now rebuild gcn_boot: cmake --build runtime/build-boot --target gcn_boot"
