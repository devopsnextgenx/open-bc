#!/bin/sh
# Per-user Dolphin (KDE) menu entries for installs that cannot register them system-wide
# (AppImage, Flatpak, tarball). deb and rpm install them to /usr/share/kio/servicemenus themselves.
#
#   install-dolphin.sh [--exec "COMMAND"] [--uninstall]
#   --exec  how to start OpenBC, e.g. "/home/me/OpenBC.AppImage" or
#           "flatpak run io.github.devopsnextgenx.open-bc"   (default: openbc)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
# Templates live next to this script in the source tree, or in ../share/openbc/dolphin when installed.
src="$here"; [ -f "$src/openbc-compare-files.desktop.in" ] || src="$here/../share/openbc/dolphin"
dest="${XDG_DATA_HOME:-$HOME/.local/share}/kio/servicemenus"
cmd="openbc"; uninstall=0
while [ $# -gt 0 ]; do
    case "$1" in
        --exec) cmd="$2"; shift ;;
        --uninstall) uninstall=1 ;;
        -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
names="openbc-compare-folders openbc-compare-files openbc-compare-two"
if [ "$uninstall" = 1 ]; then
    for n in $names; do rm -f "$dest/$n.desktop"; done
    echo "Removed."; exit 0
fi
mkdir -p "$dest"
for n in $names; do
    sed "s|@OPENBC_FM_EXEC@|$cmd|g" "$src/$n.desktop.in" > "$dest/$n.desktop"
    chmod 0755 "$dest/$n.desktop"   # KDE only trusts executable service menus under $HOME
done
echo "Installed in $dest - reopen Dolphin."
