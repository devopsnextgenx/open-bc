#!/bin/bash
set -e

# Define required MSYS2 directories
REQUIRED_PATHS=(
    "/c/msys64/ucrt64/bin"
    "/c/msys64/ucrt64/share/qt6/bin"
    "/c/msys64/usr/bin"
)

# Helper function to check and prepend missing paths
add_to_path_if_missing() {
    local target_path="$1"
    local win_slash="C:/msys64/${target_path#/c/msys64/}"
    local win_bslash="C:\\msys64\\${target_path#/c/msys64/}"

    case ":$PATH:" in
        *":$target_path:"* | *":$win_slash:"* | *":$win_bslash:"*)
            ;;
        *)
            export PATH="$target_path:$PATH"
            ;;
    esac
}

# 1. Prepend MSYS2 paths
for (( i=${#REQUIRED_PATHS[@]}-1; i>=0; i-- )); do
    add_to_path_if_missing "${REQUIRED_PATHS[$i]}"
done

# 2. Prevent Git Bash / System DLL conflicts by stripping non-essential Windows paths
# Retain Windows system binaries and Rustup/Cargo paths
export PATH="/c/msys64/ucrt64/bin:/c/msys64/ucrt64/share/qt6/bin:/c/msys64/usr/bin:/c/Windows/system32:/c/Windows:$HOME/.cargo/bin:$PATH"

# Clean stale build cache
rm -rf build

# Configure and Build using UCRT64 toolchain
cmake -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run executable
./build/OpenBC.exe