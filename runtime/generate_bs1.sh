#!/usr/bin/env bash
# M1 — real BS1 boot from the raw scrambled bios/ipl.bin (docs/M1_PLAN.md).
#
# Produces the MERGED runtime/generated/ tree that serves BOTH boot paths from
# ONE dispatch table (no duplication, no symbol collision — the address
# ranges are disjoint):
#   - M0 default (unchanged): func_81200000 (BS1-at-RAM, the Dolphin-HLE
#     landing-point copy) -> func_81300000 (BS2)
#   - M1 (GCN_BOOT_BS1=1):    func_FFF00100 (BS1-at-ROM, TRUE reset vector,
#     recompiled from the SAME descrambled bytes at a different base — BS1 has
#     no self-referential absolute addressing, confirmed by the full
#     instruction walk in docs/M1_PLAN.md §3) -> its OWN real EXI DMA -> the
#     SAME func_81300000 BS2 the default path uses.
#
# Three phases:
#  Phase 1 (regenerate M0's known-good inputs): re-run the CURRENT gcn_boot
#           (unchanged M0 binary) to reproduce postdma.bin/lowmem.bin exactly
#           as the checked-in runtime/generated/ tree was built from
#           (generate_postdma.sh's own mechanism, one level up) — this is what
#           lets the FINAL merged recompile in phase 3 reuse them without
#           needing to keep phase-2's temp files across script runs.
#  Phase 2 (the real M1 validation): recompile ONLY the BS1 ROM slice
#           (0xFFF00100), swap it into a SCRATCH generated/ tree, build a
#           scratch gcn_boot from it, and run it from the TRUE hardware reset
#           vector with EXI backed by the RAW SCRAMBLED bios/ipl.bin. BS1's
#           own recompiled code does its own HID0/BAT/MSR bring-up + EXI DMA
#           of BS2 through the transparent in-CPU/EXI descrambler. gcn_boot's
#           own GCN_BS1_REFERENCE integrity check (boot.c) prints PASS/FAIL.
#  Phase 3 (the final merged tree): ONE dolrecomp invocation — primary =
#           postdma.bin (base 0x81200000, entry 0x81200150, byte-identical to
#           today) + --segment 0x80000000:lowmem.bin (unchanged) + NEW
#           --segment 0xFFF00100:bs1_rom.bin. Installs into runtime/generated/,
#           the tree BOTH boot paths ship from.
#
# Prerequisites: build.sh already ran (recompiler + tools/ipl_descramble
# built), and the CURRENT runtime/generated/ + a built gcn_boot at
# runtime/build-boot/gcn_boot.exe exist (i.e. generate_postdma.sh has been run
# at least once before — this script re-derives its inputs, it does not
# replace it).
set -euo pipefail
export PATH="/c/msys64/mingw64/bin:$PATH"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DESC="$ROOT/tools/ipl_descramble/build/ipl_descramble.exe"
DOL="$ROOT/recompiler/build/dolrecomp.exe"
BOOT="$ROOT/runtime/build-boot/gcn_boot.exe"
IPL="$ROOT/bios/ipl.bin"
for f in "$DESC" "$DOL" "$BOOT" "$IPL"; do
  [ -f "$f" ] || { echo "error: missing $f (build phase-1 first)"; exit 1; }
done
[ -f "$ROOT/runtime/generated/generated.h" ] || {
  echo "error: runtime/generated/ is empty. Run generate_postdma.sh first."
  exit 1; }

TMP_UNIX="$(mktemp -d)"
TMP="$(cygpath -w "$TMP_UNIX" | sed 's|\\|/|g')"
trap 'rm -rf "$TMP_UNIX"' EXIT

echo "=================================================================="
echo "[1/8] descramble IPL: full reference image + the M0 bs2.bin input"
echo "=================================================================="
"$DESC" "$IPL" -o "$TMP/descr.bin" --bs2 "$TMP/bs2.bin"

echo "=================================================================="
echo "[2/8] slice BS1 ROM body: descr.bin[0x100:0x800) -> bs1_rom.bin"
echo "=================================================================="
dd if="$TMP/descr.bin" of="$TMP/bs1_rom.bin" bs=1 skip=256 count=1792 status=none
ls -la "$TMP/bs1_rom.bin"

echo "=================================================================="
echo "[3/8] regenerate postdma.bin + lowmem.bin (re-derive generate_postdma's"
echo "      own inputs fresh, from the CURRENT/unchanged M0 gcn_boot)"
echo "=================================================================="
GCN_IPL_ROM="$TMP/descr.bin" \
GCN_MEM_DUMP="0x81200000:0x270000:$TMP/postdma.bin;0x80000000:0x3000:$TMP/lowmem.bin" \
  "$BOOT" "$TMP/bs2.bin" 5000000 > "$TMP/phase1.log" 2>&1 || true
[ -f "$TMP/postdma.bin" ] || { echo "error: no postdma.bin (see $TMP/phase1.log)"; cat "$TMP/phase1.log"; exit 1; }
[ -f "$TMP/lowmem.bin" ]  || { echo "error: no lowmem.bin (see $TMP/phase1.log)";  cat "$TMP/phase1.log"; exit 1; }
tail -5 "$TMP/phase1.log"

echo "=================================================================="
echo "[4/8] recompile BS1-at-ROM STANDALONE (scratch, base/entry 0xFFF00100)"
echo "=================================================================="
"$DOL" --gamecube-ipl "$TMP/bs1_rom.bin" "$TMP/stage_a" --base 0xFFF00100 --entry 0xFFF00100 -j8
GEN_A="$(dirname "$(find "$TMP/stage_a" -name generated.h | head -1)")"
[ -n "$GEN_A" ] || { echo "error: stage-A recompile produced no generated.h"; exit 1; }

echo "=================================================================="
echo "[5/8] swap in the scratch BS1-at-ROM tree + build a scratch gcn_boot"
echo "=================================================================="
rm -rf "$ROOT/runtime/generated"
cp -r "$GEN_A" "$ROOT/runtime/generated"
cmake -S "$ROOT/runtime" -B "$ROOT/runtime/build-bs1-stageA" -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGCN_WITH_GENERATED=ON
cmake --build "$ROOT/runtime/build-bs1-stageA" --target gcn_boot -j4

echo "=================================================================="
echo "[6/8] RUN BS1-at-ROM from the TRUE hardware reset vector"
echo "      (real EXI DMA of BS2 through the transparent descrambler)"
echo "=================================================================="
GCN_BOOT_BS1=1 \
GCN_BS1_REFERENCE="$TMP/descr.bin" \
GCN_MEM_DUMP="0x81300000:0x1B0000:$TMP/postdma_bs1.bin" \
  "$ROOT/runtime/build-bs1-stageA/gcn_boot.exe" "$IPL" 5000000 2>&1 | tee "$TMP/stageA.log"
[ -f "$TMP/postdma_bs1.bin" ] || { echo "error: no post-DMA dump produced (see above)"; exit 1; }

echo "=================================================================="
echo "[7/8] independent cross-check (Python CRC/byte-compare, same bytes)"
echo "=================================================================="
# 0x16FFE0: BS1's real EXI-DMA bulk-copy size, measured empirically this
# session (NOT IPL_SCRAMBLE_END-0x820=0x1AF6E0 -- see boot.c's GCN_BS1_BS2_DMA_LEN
# comment). Everything past this offset is other data BS1's bulk copy never
# touches (confirmed all-zero on a fresh true-reset run).
set +e
python3 - "$TMP/postdma_bs1.bin" "$TMP/descr.bin" <<'PYEOF'
import sys, zlib
dump = open(sys.argv[1], "rb").read()
ref  = open(sys.argv[2], "rb").read()
LEN = 0x16FFE0
dump_bs2 = dump[:LEN]
ref_bs2  = ref[0x820:0x820+LEN]
ok = dump_bs2 == ref_bs2
print(f"dump  CRC32 = 0x{zlib.crc32(dump_bs2) & 0xFFFFFFFF:08X}  ({len(dump_bs2)} bytes)")
print(f"ref   CRC32 = 0x{zlib.crc32(ref_bs2)  & 0xFFFFFFFF:08X}  ({len(ref_bs2)} bytes)")
print("INDEPENDENT PYTHON CHECK: " + ("PASS -- byte-identical" if ok else "FAIL -- MISMATCH"))
sys.exit(0 if ok else 1)
PYEOF
PYCHECK_RC=$?
set -e

echo "=================================================================="
echo "[8/8] recompile the FINAL merged tree (M0 primary+lowmem + NEW"
echo "      0xFFF00100 BS1-ROM segment) and install into runtime/generated/"
echo "=================================================================="
"$DOL" --gamecube-ipl "$TMP/postdma.bin" "$TMP/out" --base 0x81200000 --entry 0x81200150 \
  --segment "0x80000000:$TMP/lowmem.bin" \
  --segment "0xFFF00100:$TMP/bs1_rom.bin" -j8
GEN_FINAL="$(dirname "$(find "$TMP/out" -name generated.h | head -1)")"
[ -n "$GEN_FINAL" ] || { echo "error: final recompile produced no generated.h"; exit 1; }
rm -rf "$ROOT/runtime/generated"
cp -r "$GEN_FINAL" "$ROOT/runtime/generated"
echo "done -> runtime/generated ($(find "$ROOT/runtime/generated/chunks" -name '*.c' | wc -l) chunks, BS1-at-RAM + BS1-at-ROM + BS2 + lowmem)"
echo "now rebuild: cmake --build runtime/build-boot --target gcn_boot"
echo
echo "PYTHON CROSS-CHECK EXIT CODE: $PYCHECK_RC (0 = PASS)"
