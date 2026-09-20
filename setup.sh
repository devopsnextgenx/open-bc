#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

install_packages() {
    local manager="$1"
    shift
    case "$manager" in
        apt)
            sudo apt-get update
            sudo apt-get install -y "$@"
            ;;
        dnf)
            sudo dnf install -y "$@"
            ;;
        pacman)
            sudo pacman -Sy --needed --noconfirm "$@"
            ;;
    esac
}

if command -v apt-get >/dev/null 2>&1; then
    install_packages apt build-essential clang cmake pkg-config qt6-base-dev libssl-dev libssh2-1-dev libvulkan-dev vulkan-tools
elif command -v dnf >/dev/null 2>&1; then
    install_packages dnf gcc-c++ clang cmake pkgconf-pkg-config qt6-qtbase-devel openssl-devel libssh2-devel vulkan-loader-devel vulkan-tools
elif command -v pacman >/dev/null 2>&1; then
    install_packages pacman base-devel clang cmake pkgconf qt6-base openssl libssh2 vulkan-headers vulkan-tools
else
    echo "Unsupported Linux package manager. Install Qt6, CMake, a C++17 toolchain, OpenSSL, libssh2, and Vulkan SDK manually." >&2
    exit 1
fi

command -v cargo >/dev/null 2>&1 || {
    echo "Rust/Cargo is required. Install rustup from https://rustup.rs/ and rerun setup.sh." >&2
    exit 1
}

cargo install cargo-xtask --locked 2>/dev/null || true
printf 'OpenBC setup complete. Try: cargo xtask setup && cargo xtask build\n'
