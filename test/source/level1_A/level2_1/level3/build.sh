#!/bin/bash
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
export PATH="/c/msys64/ucrt64/bin:$PATH"
export OPENSSL_DIR=/c/msys64/ucrt64


echo "B"
echo "C"
echo "D"
#

# Build script for Windows using CMake and Ninja

# rm -rf build && cmake -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release && cmake --build build
cmake --build build



# run the built executable
./build/openbc