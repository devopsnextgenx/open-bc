#!/bin/sh
# Usage: build_and_run.sh <dir-with-libffi_harness.a>   (Qt 6 headless; real Rust session API)
set -e
HERE=$(cd "$(dirname "$0")" && pwd); CPP=$(cd "$HERE/../.." && pwd)
LIBDIR=${1:?path to directory containing the Rust staticlib}
OUT=${OUT:-/tmp/sht_build}; mkdir -p "$OUT"
g++ -std=c++17 -fPIC -I"$CPP" $(pkg-config --cflags Qt6Widgets Qt6Gui Qt6Core) \
  "$HERE/syntax_highlighter_test.cpp" "$LIBDIR"/libffi_harness.a \
  $(pkg-config --libs Qt6Widgets Qt6Gui Qt6Core) -lpthread -ldl -lm -o "$OUT/syntax_highlighter_test"
QT_QPA_PLATFORM=offscreen "$OUT/syntax_highlighter_test"
