#!/bin/sh
# Builds and runs text_settings_test (Qt 6, headless). No Rust library needed: the highlight bridge is stubbed.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CPP=$(cd "$HERE/../.." && pwd)
OUT=${OUT:-/tmp/tst_build}; mkdir -p "$OUT"
g++ -std=c++17 -fPIC -Wall -Wextra -I"$CPP" $(pkg-config --cflags Qt6Core Qt6Gui Qt6Widgets) \
  "$HERE/text_settings_test.cpp" -o "$OUT/text_settings_test" $(pkg-config --libs Qt6Core Qt6Gui Qt6Widgets)
QT_QPA_PLATFORM=offscreen "$OUT/text_settings_test"
