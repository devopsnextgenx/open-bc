#!/usr/bin/env bash
set -euo pipefail

source_dir=${OPENBC_NAUTILUS_SOURCE:-/usr/share/openbc/nautilus}
destination="${XDG_DATA_HOME:-$HOME/.local/share}/nautilus/scripts"
helper="$source_dir/openbc-nautilus-action"

if [[ ! -x "$helper" ]]; then
    printf 'OpenBC Nautilus scripts were not found at %s\n' "$helper" >&2
    exit 1
fi

install -d "$destination"
for name in "Compare" "Select Left Folder" "Compare to Left Folder" \
            "Select Left File" "Compare to Left File"; do
    install -m 0755 "$helper" "$destination/$name"
done
printf 'Installed OpenBC actions in %s\n' "$destination"
printf 'Restart Nautilus or open a new file-manager window to load them.\n'