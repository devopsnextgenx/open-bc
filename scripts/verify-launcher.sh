#!/usr/bin/env bash
# verify-launcher.sh - checks that the installed launcher, binary and icon line up.
# Usage: scripts/verify-launcher.sh [app-id] [prefix]     (defaults: io.github.devopsnextgenx.open-bc, /usr)
# For a Flatpak install use: PREFIX=/var/lib/flatpak/app/io.github.devopsnextgenx.open-bc/current/active/files  (or ~/.local/share/flatpak/...)
ID="${1:-io.github.devopsnextgenx.open-bc}"; PREFIX="${2:-${PREFIX:-/usr}}"
DESK="$PREFIX/share/applications/$ID.desktop"
ok(){ echo "  [ OK ] $*"; }; bad(){ echo "  [FAIL] $*"; FAILED=1; }; FAILED=0

echo "== 1. desktop file"
[ -f "$DESK" ] && ok "$DESK" || { bad "missing $DESK"; echo "   installed desktop files:"; ls "$PREFIX"/share/applications | grep -i bc; exit 1; }
ICON="$(sed -n 's/^Icon=//p' "$DESK" | head -1)"; EXEC="$(sed -n 's/^Exec=//p' "$DESK" | head -1)"
WMC="$(sed -n 's/^StartupWMClass=//p' "$DESK" | head -1)"
echo "     Icon=$ICON"; echo "     Exec=$EXEC"; echo "     StartupWMClass=${WMC:-<none>}"
[ -n "$ICON" ] && ok "Icon key present" || bad "no Icon= line"
[ "$ICON" = "$ID" ] || [ "${ICON#/}" != "$ICON" ] || bad "Icon=$ICON does not match installed file name '$ID' (name must equal the PNG basename)"

echo "== 2. Exec target"
BIN="${EXEC%% *}"; BIN="${BIN//\"/}"
if [ "${BIN#/}" != "$BIN" ]; then [ -x "$BIN" ] && ok "$BIN" || bad "$BIN not executable/missing"
else command -v "$BIN" >/dev/null && ok "$BIN -> $(command -v "$BIN")" || bad "'$BIN' not on PATH"; fi

echo "== 3. icon files (theme lookup name: '$ICON')"
if [ "${ICON#/}" != "$ICON" ]; then [ -f "$ICON" ] && ok "$ICON" || bad "$ICON missing"
else
  FOUND=0
  for f in "$PREFIX"/share/icons/hicolor/*/apps/"$ICON".{png,svg}; do
    [ -f "$f" ] || continue; FOUND=1
    echo "     $f  $(file -b "$f" | cut -d, -f1-2)"
  done
  [ $FOUND = 1 ] && ok "icon found in hicolor" || bad "no $ICON.png/svg under $PREFIX/share/icons/hicolor/*/apps/"
  ls -d "$PREFIX"/share/icons/hicolor/{64x64,128x128}/apps/"$ICON".png >/dev/null 2>&1 && ok "64/128 px present (AppStream)" || echo "  [warn] no 64x64/128x128 copy - run scripts/gen-icons.sh"
  command -v gtk-update-icon-cache >/dev/null && [ -f "$PREFIX/share/icons/hicolor/icon-theme.cache" ] && \
    echo "     icon-theme.cache: $(stat -c %y "$PREFIX/share/icons/hicolor/icon-theme.cache" | cut -c1-19)"
fi

echo "== 4. validators"
command -v desktop-file-validate >/dev/null && { desktop-file-validate "$DESK" && ok "desktop-file-validate" || bad "desktop-file-validate"; } || echo "  [skip] desktop-file-validate (apt install desktop-file-utils)"
MI="$PREFIX/share/metainfo/$ID.metainfo.xml"
[ -f "$MI" ] && ok "$MI" || bad "metainfo missing"
command -v appstreamcli >/dev/null && [ -f "$MI" ] && { appstreamcli validate --no-net "$MI" | tail -5; }

echo "== 5. does the desktop environment see it?"
command -v gio >/dev/null && gio info "$DESK" 2>/dev/null | grep -E 'standard::icon|display name' | sed 's/^/     /'
echo "     (run: gtk-launch $ID   - launches it exactly the way the app grid does)"
echo "== 6. window <-> launcher match (running app shows generic icon if this differs)"
echo "     Wayland app_id / X11 WM_CLASS must equal '$ID' (or StartupWMClass). With the app running:"
echo "       X11: xprop WM_CLASS   (click the window)      GNOME: Alt+F2 -> lg -> Windows tab"
echo "     In main(): QGuiApplication::setDesktopFileName(\"$ID\");"
exit $FAILED