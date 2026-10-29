#!/usr/bin/env bash
# Per-user Nautilus "Scripts" entries (right-click > Scripts). Works on every Nautilus version.
#   install-nautilus.sh [--exec "COMMAND"]
#   --exec  how to start OpenBC when it is not `openbc` on PATH, e.g. "/home/me/OpenBC.AppImage" or
#           "flatpak run io.github.devopsnextgenx.open-bc" (stored in ~/.config/openbc/launcher)
set -euo pipefail

exec_cmd=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --exec) exec_cmd="${2:?--exec needs a command}"; shift ;;
        -h|--help) sed -n '2,6p' "$0"; exit 0 ;;
        *) printf 'unknown option: %s\n' "$1" >&2; exit 2 ;;
    esac
    shift
done

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source_dir=${OPENBC_NAUTILUS_SOURCE:-/usr/share/openbc/nautilus}
destination="${XDG_DATA_HOME:-$HOME/.local/share}/nautilus/scripts"
helper="$source_dir/openbc-nautilus-action"
# Not installed system-wide (AppImage, source tree): use the helper next to this script.
[[ -x "$helper" ]] || helper="$here/openbc-nautilus-action"

if [[ ! -x "$helper" ]]; then
    printf 'OpenBC Nautilus scripts were not found at %s\n' "$helper" >&2
    exit 1
fi

install -d "$destination"
for name in "Compare" "Select Left Folder" "Compare to Left Folder" \
            "Select Left File" "Compare to Left File"; do
    install -m 0755 "$helper" "$destination/$name"
done
if [[ -n "$exec_cmd" ]]; then
    config="${XDG_CONFIG_HOME:-$HOME/.config}/openbc"
    install -d "$config"
    printf '%s\n' "$exec_cmd" > "$config/launcher"
fi
printf 'Installed OpenBC actions in %s\n' "$destination"
printf 'Restart Nautilus (nautilus -q) or open a new file-manager window to load them.\n'
