#!/usr/bin/env bash
set -e
export PATH="/c/msys64/opt/wix:$PATH"

cmake -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd build && cpack