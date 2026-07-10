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
IPL="$ROOT/bios/ipl.bin"
DESC="$ROOT/tools/ipl_descramble/build/ipl_descramble.exe"
DOL="$ROOT/recompiler/build/dolrecomp.exe"
OUT="$ROOT/runtime/generated"

[ -f "$IPL" ]  || { echo "error: missing $IPL (supply your own IPL dump)"; exit 1; }
[ -f "$DESC" ] || { echo "error: missing $DESC (run ./build.sh first)"; exit 1; }
[ -f "$DOL" ]  || { echo "error: missing $DOL (run ./build.sh first)"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "[1/3] descrambling IPL -> plaintext BS2"
"$DESC" "$IPL" --bs2 "$TMP/bs2.bin"

echo "[2/3] recompiling BS2 -> split C"
"$DOL" --gamecube-ipl "$TMP/bs2.bin" "$TMP/out" -j8

echo "[3/3] installing into runtime/generated/"
GENDIR="$(dirname "$(find "$TMP/out" -name generated.h | head -1)")"
[ -n "$GENDIR" ] || { echo "error: recompiler produced no generated.h"; exit 1; }
rm -rf "$OUT"
cp -r "$GENDIR" "$OUT"

echo "done -> $OUT ($(find "$OUT/chunks" -name '*.c' | wc -l) chunks)"
