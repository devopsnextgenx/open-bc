#!/bin/sh
# Installs the OpenBC file-manager menu entries for the CURRENT USER (or system-wide with --system).
#
#   install-file-manager-integration.sh [--exec "COMMAND"] [--system] [--uninstall]
#
# --exec  how to start OpenBC when it is not simply `openbc` on PATH, e.g.
#           --exec "flatpak run io.github.devopsnextgenx.open-bc"   (Flatpak)
#           --exec "/home/me/Apps/OpenBC-x86_64.AppImage"           (AppImage)
# deb / rpm packages install these same files themselves (see ../README.md); this script is for
# Flatpak, AppImage and tarball installs, which cannot register anything with the host file managers.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
exec_cmd=""
system=0
uninstall=0
while [ $# -gt 0 ]; do
    case "$1" in
        --exec) exec_cmd="$2"; shift ;;
        --system) system=1 ;;
        --uninstall) uninstall=1 ;;
        -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

if [ "$system" = 1 ]; then
    bin_dir=/usr/bin
    kio_dir=/usr/share/kio/servicemenus
    naut_dir=/usr/share/nautilus-python/extensions
else
    data="${XDG_DATA_HOME:-$HOME/.local/share}"
    bin_dir="$HOME/.local/bin"
    kio_dir="$data/kio/servicemenus"
    naut_dir="$data/nautilus-python/extensions"
fi
cfg_dir="${XDG_CONFIG_HOME:-$HOME/.config}/openbc"

if [ "$uninstall" = 1 ]; then
    rm -f "$bin_dir/openbc-fm" "$naut_dir/openbc-nautilus.py" "$cfg_dir/launcher" \
          "$kio_dir/openbc-compare-folders.desktop" "$kio_dir/openbc-compare-files.desktop" \
          "$kio_dir/openbc-compare-two.desktop"
    echo "OpenBC file-manager integration removed. Restart Nautilus with: nautilus -q"
    exit 0
fi

mkdir -p "$bin_dir" "$kio_dir" "$naut_dir"
install -m 0755 "$here/openbc-fm" "$bin_dir/openbc-fm"
install -m 0644 "$here/openbc-nautilus.py" "$naut_dir/openbc-nautilus.py"
for f in openbc-compare-folders openbc-compare-files openbc-compare-two; do
    sed "s|@FM_LAUNCHER@|$bin_dir/openbc-fm|g" "$here/$f.desktop" > "$kio_dir/$f.desktop"
    chmod 0755 "$kio_dir/$f.desktop"   # KDE only trusts executable service menus in the user's home
done
if [ -n "$exec_cmd" ]; then
    mkdir -p "$cfg_dir"
    printf '%s\n' "$exec_cmd" > "$cfg_dir/launcher"
fi

echo "Installed. Restart the file manager to see the entries:"
echo "  Nautilus: nautilus -q      (needs python3-nautilus / nautilus-python)"
echo "  Dolphin:  reopen it"
