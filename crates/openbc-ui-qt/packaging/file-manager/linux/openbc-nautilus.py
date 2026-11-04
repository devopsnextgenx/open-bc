"""OpenBC entries in the Nautilus (GNOME Files) context menu.

Needs nautilus-python (Debian/Ubuntu: python3-nautilus, Fedora: nautilus-python).
Works with Nautilus 43+ (extension API 4.0) and older releases (API 3.0).

  * two items of the same kind selected  -> "openbc: Compare"
  * one item selected                    -> "openbc: Select Left Folder/File", and, only while a left item
    is selected in the running OpenBC, "openbc: Compare To <left item name>"
  * right-click on the folder background -> the same entries for the folder being shown

OpenBC keeps the left item in memory and publishes it to $XDG_RUNTIME_DIR/openbc/left-selection
("folder" or "file" on the first line); the file is removed when the selection is cleared or OpenBC exits.
"""
import os
import shutil
import subprocess

import gi

try:
    gi.require_version("Nautilus", "4.0")
except ValueError:
    gi.require_version("Nautilus", "3.0")
from gi.repository import GObject, Nautilus  # noqa: E402

_LAUNCHER = "openbc-fm"


def _left_selection():
    """(kind, path) when OpenBC currently holds a left item, else None."""
    runtime = os.environ.get("XDG_RUNTIME_DIR") or "/tmp"
    try:
        with open(os.path.join(runtime, "openbc", "left-selection"), encoding="utf-8") as handle:
            kind = handle.readline().strip()
            path = handle.readline().rstrip("\r\n")
    except OSError:
        return None
    return (kind, path) if kind in ("folder", "file") and path else None


def _launcher():
    found = shutil.which(_LAUNCHER)
    if found:
        return found
    local = os.path.expanduser("~/.local/bin/" + _LAUNCHER)
    return local if os.path.exists(local) else None


def _run(args):
    launcher = _launcher()
    if launcher is None:
        return
    subprocess.Popen([launcher] + list(args), stdin=subprocess.DEVNULL,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                     start_new_session=True)


def _local_path(file_info):
    """Local path of a Nautilus file, or None for remote / virtual locations."""
    location = file_info.get_location()
    return location.get_path() if location is not None else None


class OpenBCMenuProvider(GObject.GObject, Nautilus.MenuProvider):
    @staticmethod
    def _item(name, label, callback):
        item = Nautilus.MenuItem(name="OpenBC::" + name, label=label, tip="")
        item.connect("activate", callback)
        return item

    def _single(self, path, is_dir, prefix):
        kind = "folder" if is_dir else "file"
        title = kind.capitalize()
        items = [self._item(prefix + "SelectLeft", "openbc: Select Left " + title,
                            lambda *_: _run(["--select-left-" + kind, path]))]
        left = _left_selection()
        if left is not None and left[0] == kind:
            left_name = os.path.basename(os.path.normpath(left[1])) or left[1]
            left_name = left_name.replace("\r", " ").replace("\n", " ")
            items.append(self._item(prefix + "CompareToLeft", "openbc: Compare To " + left_name,
                                    lambda *_: _run(["--compare-to-left-" + kind, path])))
        return items

    # Nautilus 4.0: get_file_items(files); 3.0: get_file_items(window, files).
    def get_file_items(self, *args):
        files = args[-1]
        entries = [(_local_path(f), f.is_directory()) for f in files]
        if not entries or any(path is None for path, _ in entries):
            return []
        if len(entries) == 2 and entries[0][1] == entries[1][1]:
            first, second = entries[0][0], entries[1][0]
            return [self._item("Compare", "openbc: Compare", lambda *_: _run([first, second]))]
        if len(entries) == 1:
            return self._single(entries[0][0], entries[0][1], "Item")
        return []

    # Nautilus 4.0: get_background_items(folder); 3.0: get_background_items(window, folder).
    def get_background_items(self, *args):
        path = _local_path(args[-1])
        return self._single(path, True, "Background") if path else []
