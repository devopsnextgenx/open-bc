#!/usr/bin/env bash
# Builds and runs i18n_test with plain g++ + pkg-config (no CMake needed). Extra args are passed to the test.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
cpp="$here/../.."                       # include root: cpp/
out="${TMPDIR:-/tmp}/openbc-i18n-test"
g++ -std=c++17 -fPIC -Wall -Wextra -I"$cpp" $(pkg-config --cflags Qt6Core) \
    "$here/i18n_test.cpp" -o "$out" $(pkg-config --libs Qt6Core)
exec "$out" "$@"
