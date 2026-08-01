#!/usr/bin/env bash
# Regenerate the recompiled BS2 C from bios/ipl.bin into runtime/generated/.
#
# Full M0 pipeline: descramble the IPL -> slice out plaintext BS2 -> recompile
# it to split C via the forked DolRecomp. Output lands in runtime/generated/
# (gitignored). Requires the descrambler and recompiler to be built first
# (run ./build.sh at the repo root), and a user-supplied bios/ipl.bin.
set -euo pipefail
export PATH="/c/msys64/mingw64/bin:$PATH"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIOS_DIR="${GCN_BIOS_DIR:-$ROOT/bios}"
IPL="$BIOS_DIR/ipl.bin"
DESC="$ROOT/tools/ipl_descramble/build/ipl_descramble.exe"
DOL="$ROOT/recompiler/build/dolrecomp.exe"
OUT="$ROOT/runtime/generated"
BUILD_JOBS="${GCN_BUILD_JOBS:-2}"

[ -f "$IPL" ]  || { echo "error: missing $IPL (supply your own IPL dump)"; exit 1; }
[ -f "$DESC" ] || { echo "error: missing $DESC (run ./build.sh first)"; exit 1; }
[ -f "$DOL" ]  || { echo "error: missing $DOL (run ./build.sh first)"; exit 1; }

TMP_UNIX="$(mktemp -d)"
TMP="$(cygpath -w "$TMP_UNIX" | sed 's|\\|/|g')"
trap 'rm -rf "$TMP_UNIX"' EXIT

echo "[1/3] descrambling IPL -> plaintext BS2"
"$DESC" "$IPL" --bs2 "$TMP/bs2.bin"

echo "[2/3] recompiling BS2 -> split C (base/entry oracle-corrected)"
"$DOL" --gamecube-ipl "$TMP/bs2.bin" "$TMP/out" --base 0x81200000 --entry 0x81200150 \
  --chunk-instructions 1024 -j "$BUILD_JOBS"

echo "[3/3] installing into runtime/generated/"
GENDIR="$(dirname "$(find "$TMP/out" -name generated.h | head -1)")"
[ -n "$GENDIR" ] || { echo "error: recompiler produced no generated.h"; exit 1; }
rm -rf "$OUT"
cp -r "$GENDIR" "$OUT"

echo "done -> $OUT ($(find "$OUT/chunks" -name '*.c' | wc -l) chunks)"
