#!/bin/sh
# Builds and runs virtual_editor_test against the Rust text engine (static library built from
# src/ffi/text_engine.rs). Usage:  build_and_run.sh <path-to-libffi_harness.a-dir> [extra g++ args]
# Requires Qt 6 (Widgets) and a C++17 compiler (no moc: the code is Q_OBJECT-free like the app). Runs headless (offscreen platform).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CPP=$(cd "$HERE/../.." && pwd)
LIBDIR=${1:?path to directory containing the Rust staticlib}
shift
OUT=${OUT:-/tmp/vet_build}
mkdir -p "$OUT"
QT="$(pkg-config --cflags --libs Qt6Widgets Qt6Gui Qt6Core)"
g++ -std=c++17 -fPIC -O1 -g -Wall -Wextra -I"$CPP" -I"$OUT" \
  "$HERE/virtual_editor_test.cpp" "$HERE/backend_stubs.cpp" "$CPP/viewers/text/virtual/virtual_text_editor.cpp" "$CPP/viewers/text/virtual/virtual_text_compare_viewer.cpp" \
  "$LIBDIR"/libffi_harness.a $QT -lpthread -ldl -lm -o "$OUT/virtual_editor_test" "$@"
QT_QPA_PLATFORM=offscreen "$OUT/virtual_editor_test"
