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
#
# BOOTSTRAP MODE (GCN_POSTDMA_NO_LOWMEM=1) -- breaking the chicken-and-egg.
#
# Normally this script dumps postdma.bin AND lowmem.bin in one run, then
# requires lowmem's 0xC00 syscall vector to be populated. But BS2 only INSTALLS
# those handlers if stage-2 actually executes, and stage-2 only executes if the
# running gcn_boot already has correct stage-2 -- which is what this script
# exists to produce. From a cold tree (one built by generate.sh alone, or by
# WindWakerRecomp/tools/build.sh, which skips this script) every binary decodes
# 0x81300000 as `psq_lu f27, -31(r29)` and dies with dar=0xFFFFFFE1.
#
# Stage-2 correctness does not depend on lowmem, so the cycle breaks in two
# passes:
#
#   GCN_POSTDMA_NO_LOWMEM=1 ./runtime/generate_postdma.sh   # pass A
#   cmake --build runtime/build-boot --target gcn_boot      #   -> correct stage-2
#   ./runtime/generate_postdma.sh                           # pass B+C: BS2 now
#   cmake --build runtime/build-boot --target gcn_boot      #      runs and the
#                                                           #      real tree lands
#
# Pass A emits no low-memory chunk, which is correct: nothing has installed
# handlers yet. Do not ship a pass-A tree -- it cannot service a syscall.
# See beads-u2x.11.
set -euo pipefail
export PATH="/c/msys64/mingw64/bin:$PATH"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIOS_DIR="${GCN_BIOS_DIR:-$ROOT/bios}"
DESC="$ROOT/tools/ipl_descramble/build/ipl_descramble.exe"
DOL="$ROOT/recompiler/build/dolrecomp.exe"
BOOT="${GCN_BOOTSTRAP_BOOT:-$ROOT/runtime/build-boot/gcn_boot.exe}"
IPL="$BIOS_DIR/ipl.bin"
DSP_ROM="$BIOS_DIR/dsp_rom.bin"
DSP_COEF="$BIOS_DIR/dsp_coef.bin"
BUILD_JOBS="${GCN_BUILD_JOBS:-2}"
for f in "$DESC" "$DOL" "$BOOT" "$IPL" "$DSP_ROM" "$DSP_COEF"; do
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
if [ "${GCN_POSTDMA_NO_LOWMEM:-0}" = 1 ]; then
  echo "  (bootstrap mode: dumping postdma only, no lowmem -- see header)"
  DUMP_SPEC="0x81200000:0x270000:$TMP/postdma.bin"
else
  DUMP_SPEC="0x81200000:0x270000:$TMP/postdma.bin;0x80000000:0x3000:$TMP/lowmem.bin"
fi
GCN_IPL_ROM="$TMP/descr.bin" \
GCN_MEM_DUMP="$DUMP_SPEC" \
GCN_DSP_ROM="$DSP_ROM" \
GCN_DSP_COEF="$DSP_COEF" \
  "$BOOT" "$TMP/bs2.bin" 5000000 >"$TMP/phase1.log" 2>&1 || true
[ -f "$TMP/postdma.bin" ] || {
  echo "error: no post-DMA dump produced"
  tail -80 "$TMP/phase1.log"
  exit 1
}
if [ "${GCN_POSTDMA_NO_LOWMEM:-0}" != 1 ]; then
  [ -f "$TMP/lowmem.bin" ]  || {
    echo "error: no low-mem dump produced"
    tail -80 "$TMP/phase1.log"
    exit 1
  }
fi

# The first syscall below enters physical 0x00000C00, dispatched through the
# cached 0x80000C00 alias. A stale/early dump can be the right size yet contain
# zeroes here, producing generated `.long 0` fallthrough all the way to
# 0x80003000. Reject that tree before it can replace runtime/generated/.
if [ "${GCN_POSTDMA_NO_LOWMEM:-0}" != 1 ]; then
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
if [ $? -ne 0 ]; then
  echo ""
  echo "  BS2 never installed its handlers, which means the gcn_boot used here"
  echo "  ($BOOT) does not have correct stage-2 yet."
  echo "  Recover with the two-pass bootstrap (see this script's header):"
  echo "    GCN_POSTDMA_NO_LOWMEM=1 ./runtime/generate_postdma.sh"
  echo "    cmake --build runtime/build-boot --target gcn_boot"
  echo "    ./runtime/generate_postdma.sh"
  exit 1
fi
fi

echo "[3/4] recompile the post-DMA image + low-mem handlers into one table"
# Primary image = stage-1/2 (base 0x81200000). Extra segment = low-memory
# exception handlers at 0x80000000; the 0xC00 syscall vector routes there via
# the dispatch's physical-PC alias (0xC00 -> 0x80000C00).
if [ "${GCN_POSTDMA_NO_LOWMEM:-0}" = 1 ]; then
  "$DOL" --gamecube-ipl "$TMP/postdma.bin" "$TMP/out" --base 0x81200000 --entry 0x81200150 \
    --chunk-instructions 1024 -j "$BUILD_JOBS" >/dev/null
else
  "$DOL" --gamecube-ipl "$TMP/postdma.bin" "$TMP/out" --base 0x81200000 --entry 0x81200150 \
    --segment "0x80000000:$TMP/lowmem.bin" \
    --chunk-instructions 1024 -j "$BUILD_JOBS" >/dev/null
fi

echo "[4/4] install into runtime/generated/"
GEN="$(dirname "$(find "$TMP/out" -name generated.h | head -1)")"
[ -n "$GEN" ] || { echo "error: recompiler produced no generated.h"; exit 1; }
if [ "${GCN_POSTDMA_NO_LOWMEM:-0}" != 1 ]; then
  grep -Rqs '80000C00: mfhid0' "$GEN/chunks" || {
    echo "error: generated low-memory chunk did not decode the syscall vector"
    exit 1
  }
fi
rm -rf "$ROOT/runtime/generated"; cp -r "$GEN" "$ROOT/runtime/generated"
echo "done -> runtime/generated ($(find "$ROOT/runtime/generated/chunks" -name '*.c' | wc -l) chunks, stage-1 + stage-2)"
echo "now rebuild gcn_boot: cmake --build runtime/build-boot --target gcn_boot"
