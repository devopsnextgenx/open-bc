#!/bin/bash
export PATH="/c/msys64/usr/bin:$PATH"
export PATH="/c/msys64/ucrt64/bin:$PATH"
export OPENSSL_DIR=/c/msys64/ucrt64

# Parse command line flags
CLEAN=false

while getopts "c" opt; do
  case $opt in
    c)
      CLEAN=true
      ;;
    \?)
      echo "Usage: $0 [-c]"
      exit 1
      ;;
  esac
done

# Perform clean rebuild if -c flag is passed
if [ "$CLEAN" = true ]; then
  echo "Cleaning and reconfiguring build directory..."
  rm -rf build
  cmake -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release
fi

# Build project
cmake --build build

# Run the built executable
./build/openbc
