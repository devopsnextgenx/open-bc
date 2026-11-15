#!/usr/bin/env bash
# Builds and runs theme_test with plain g++ + pkg-config (no CMake needed).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
cpp="$here/../.."
out="${TMPDIR:-/tmp}/openbc-theme-test"
g++ -std=c++17 -fPIC -Wall -Wextra -I"$cpp" $(pkg-config --cflags Qt6Widgets) \
    "$here/theme_test.cpp" -o "$out" $(pkg-config --libs Qt6Widgets)
QT_QPA_PLATFORM=offscreen exec "$out" "$@"
