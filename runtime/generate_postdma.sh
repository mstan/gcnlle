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

# /x/path -> X:/path so the Windows exe's fopen accepts the dump path.
to_win() { echo "$1" | sed -E 's|^/([a-z])/|\U\1:/|'; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

echo "[1/4] descramble IPL (full image + BS2 payload)"
"$DESC" "$IPL" -o "$TMP/descr.bin" --bs2 "$TMP/bs2.bin"

echo "[2/4] run stage-1 to DMA stage-2, dump post-DMA MEM1 code image"
GCN_IPL_ROM="$(to_win "$TMP/descr.bin")" \
GCN_MEM_DUMP="0x81200000:0x270000:$(to_win "$TMP/postdma.bin")" \
  "$BOOT" "$TMP/bs2.bin" 5000000 >/dev/null 2>&1 || true   # stops at first sc; that's fine
[ -f "$TMP/postdma.bin" ] || { echo "error: no post-DMA dump produced"; exit 1; }

echo "[3/4] recompile the post-DMA image (both stages, base 0x81200000)"
"$DOL" --gamecube-ipl "$TMP/postdma.bin" "$TMP/out" --base 0x81200000 --entry 0x81200150 -j8 >/dev/null

echo "[4/4] install into runtime/generated/"
GEN="$(dirname "$(find "$TMP/out" -name generated.h | head -1)")"
rm -rf "$ROOT/runtime/generated"; cp -r "$GEN" "$ROOT/runtime/generated"
echo "done -> runtime/generated ($(find "$ROOT/runtime/generated/chunks" -name '*.c' | wc -l) chunks, stage-1 + stage-2)"
echo "now rebuild gcn_boot: cmake --build runtime/build-boot --target gcn_boot"
