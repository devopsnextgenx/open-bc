#!/usr/bin/env bash
set -e

PACKAGE_NAME="openbc"
VERSION="1.0.0"
ARCH="amd64"
DEB_DIR="${PACKAGE_NAME}_${VERSION}_${ARCH}"

# 1. Build project
cmake -B build_deb -DCMAKE_BUILD_TYPE=Release
cmake --build build_deb --target openbc

# 2. Create standard Debian filesystem hierarchy
rm -rf "$DEB_DIR"
mkdir -p "$DEB_DIR/usr/bin"
mkdir -p "$DEB_DIR/DEBIAN"

cp build_deb/openbc "$DEB_DIR/usr/bin/"

# 3. Create control file with system dependencies
cat << EOF > "$DEB_DIR/DEBIAN/control"
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

# 4. Build .deb file
dpkg-deb --build "$DEB_DIR"

echo "=== Package Created: ${DEB_DIR}.deb ==="