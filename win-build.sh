#!/bin/bash
#
# Build script for Windows using CMake and Ninja
rm -rf build && cmake -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release && cmake --build build

# run the built executable
./build/OpenBC