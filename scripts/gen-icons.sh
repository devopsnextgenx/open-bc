#!/usr/bin/env bash
# gen-icons.sh - one-off: derive every icon the packages need from a single square-ish PNG.
#   Linux : resources/icons/hicolor/<N>x<N>/apps/io.openbc.OpenBC.png  (48,64,128,256,512)
#   Windows: resources/icons/openbc.ico (16..256, used by the .rc, the MSI shortcuts and Add/Remove Programs)
# Usage: scripts/gen-icons.sh [source.png]     (default: resources/icons/fevicon.png; best if >= 512px)
# Requires ImageMagick (magick or convert). Commit the generated files.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${1:-$ROOT/resources/icons/fevicon.png}"
NAME="io.openbc.OpenBC"
[ -f "$SRC" ] || { echo "source icon not found: $SRC" >&2; exit 1; }
if   command -v magick  >/dev/null 2>&1; then IM=(magick)
elif command -v convert >/dev/null 2>&1; then IM=(convert)
else echo "ImageMagick not found (apt install imagemagick / pacman -S mingw-w64-ucrt-x86_64-imagemagick)" >&2; exit 1; fi

for s in 48 64 128 256 512; do
  d="$ROOT/resources/icons/hicolor/${s}x${s}/apps"; mkdir -p "$d"
  "${IM[@]}" "$SRC" -background none -resize "${s}x${s}" -gravity center -extent "${s}x${s}" "PNG32:$d/$NAME.png"
  echo "  $d/$NAME.png"
done
"${IM[@]}" "$SRC" -background none -resize 256x256 -gravity center -extent 256x256 \
  -define icon:auto-resize=256,128,64,48,32,24,16 "$ROOT/resources/icons/openbc.ico"
echo "  $ROOT/resources/icons/openbc.ico"