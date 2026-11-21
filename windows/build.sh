#!/bin/sh
# Builds the release exe (x86 -> windows/build-x86/CodeTrans.exe, runs on 32- and
# 64-bit Windows) with llvm-mingw (msvcrt) and runs the tests.
# Usage: windows/build.sh [x86|x64|all] [--no-test]   (default x86; x64 = dev build)
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
ARCHES=${1:-x86}
[ "$ARCHES" = "all" ] && ARCHES="x64 x86"
TEST=1
[ "$2" = "--no-test" ] && TEST=0

NINJA=$(command -v ninja || true)
if [ -n "$NINJA" ] && [ -L "$NINJA" ]; then NINJA=$(readlink -f "$NINJA"); fi
[ -z "$NINJA" ] && { echo "ninja not found"; exit 1; }

for A in $ARCHES; do
  B="windows/build-$A"
  if [ ! -f "$B/build.ninja" ]; then
    cmake -S windows -B "$B" -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA" \
      --toolchain "$ROOT/windows/cmake/mingw-$A.cmake" > /dev/null
  fi
  "$NINJA" -C "$B"
  if [ $TEST = 1 ]; then
    (cd "$B" && ./cpwin_tests.exe && ./core/cp_core_tests.exe --quick)
  fi
  if [ "$A" = "x86" ]; then ls -l "$B/CodeTrans.exe"; else ls -l "$B/CodeTrans-$A.exe"; fi
done
