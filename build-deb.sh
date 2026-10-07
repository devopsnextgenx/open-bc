#!/usr/bin/env bash
set -e

# Usage: ./deb.sh [-c]    (-c = clean rebuild of the CMake/cargo build dir)
CLEAN=false
while getopts "c" opt; do
  case $opt in
    c) CLEAN=true ;;
    \?) echo "Usage: $0 [-c]"; exit 1 ;;
  esac
done

PACKAGE_NAME="openbc"
VERSION="1.0.0"
ARCH="amd64"
BUILD_DIR="build_deb"
DEB_STAGING="$BUILD_DIR/${PACKAGE_NAME}_${VERSION}_${ARCH}"
OUTPUT_DEB="$BUILD_DIR/${PACKAGE_NAME}_${VERSION}_${ARCH}.deb"

# Cache compiled Rust crates across builds if sccache is installed
if command -v sccache >/dev/null 2>&1; then
  export RUSTC_WRAPPER=sccache
fi

# Only wipe the build dir when explicitly asked
if [ "$CLEAN" = true ]; then
  echo "Cleaning build directory..."
  rm -rf "$BUILD_DIR"
fi

echo "=== 1. Building openbc target ==="
# Configure only when needed; later runs perform fast incremental builds
if [ ! -f "$BUILD_DIR/build.ninja" ] && [ ! -f "$BUILD_DIR/Makefile" ]; then
  if command -v ninja >/dev/null 2>&1; then
    cmake -B "$BUILD_DIR" -G "Ninja" -DCMAKE_BUILD_TYPE=Release
  else
    cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
  fi
fi

cmake --build "$BUILD_DIR" --target openbc

echo "=== 2. Creating standard Debian filesystem hierarchy ==="
# Clean and recreate staging folder inside build_deb
rm -rf "$DEB_STAGING"
mkdir -p "$DEB_STAGING/usr/bin"
mkdir -p "$DEB_STAGING/DEBIAN"

cp "$BUILD_DIR/openbc" "$DEB_STAGING/usr/bin/"

echo "=== 3. Creating DEBIAN control file ==="
cat << EOF > "$DEB_STAGING/DEBIAN/control"
Package: ${PACKAGE_NAME}
Version: ${VERSION}
Section: utils
Priority: optional
Architecture: ${ARCH}
Depends: libqt6widgets6 (>= 6.0), libqt6opengl6 (>= 6.0), zlib1g, liblzma5, libssl3
Maintainer: OpenBC Team <info@example.com>
Description: OpenBC File and Directory Comparison Tool
 OpenBC is an open-source comparison tool integrated with Rust backends and Qt6 GUI.
EOF

echo "=== 4. Building .deb package ==="
dpkg-deb --build "$DEB_STAGING" "$OUTPUT_DEB"

echo "=== Package Created: ${OUTPUT_DEB} ==="