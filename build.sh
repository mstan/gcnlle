#!/usr/bin/env bash
# gcnrecomp build helper.
#
# Builds the recompiler (and, once it exists, the runtime). The msys2 mingw64
# gcc needs its own bin dir on PATH for its runtime DLLs, or CMake's
# try-compile fails with a bare "is not able to compile a simple test program"
# — so we export it here rather than leave it to the caller's shell.
set -euo pipefail

MINGW="${MINGW_BIN:-/c/msys64/mingw64/bin}"
export PATH="$MINGW:$PATH"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GEN="${CMAKE_GENERATOR:-Ninja}"

build_dir() {  # <src-subdir>
  local sub="$1"
  [ -f "$ROOT/$sub/CMakeLists.txt" ] || { echo "skip $sub (no CMakeLists yet)"; return 0; }
  echo "=== building $sub ==="
  cmake -S "$ROOT/$sub" -B "$ROOT/$sub/build" -G "$GEN"
  cmake --build "$ROOT/$sub/build"
}

build_dir recompiler
build_dir runtime

echo "=== done ==="
