"""OpenBC entries in the Nautilus (GNOME Files) context menu.

Needs nautilus-python (Debian/Ubuntu: python3-nautilus, Fedora: nautilus-python).
Works with Nautilus 43+ (extension API 4.0) and older releases (API 3.0).

  * two items of the same kind selected  -> "Compare with OpenBC"
  * one item selected                    -> "OpenBC: Select Left ..." / "OpenBC: Compare to Selected ..."
    (OpenBC itself remembers the left item, like Beyond Compare)
  * right-click on the folder background -> the same two entries for the folder being shown
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
        return [
            self._item(prefix + "SelectLeft", "OpenBC: Select Left " + title,
                       lambda *_: _run(["--select-left-" + kind, path])),
            self._item(prefix + "CompareTo", "OpenBC: Compare to Selected " + title,
                       lambda *_: _run(["--compare-to-left-" + kind, path])),
        ]

    # Nautilus 4.0: get_file_items(files); 3.0: get_file_items(window, files).
    def get_file_items(self, *args):
        files = args[-1]
        entries = [(_local_path(f), f.is_directory()) for f in files]
        if not entries or any(path is None for path, _ in entries):
            return []
        if len(entries) == 2 and entries[0][1] == entries[1][1]:
            first, second = entries[0][0], entries[1][0]
            return [self._item("Compare", "Compare with OpenBC", lambda *_: _run([first, second]))]
        if len(entries) == 1:
            return self._single(entries[0][0], entries[0][1], "Item")
        return []

    # Nautilus 4.0: get_background_items(folder); 3.0: get_background_items(window, folder).
    def get_background_items(self, *args):
        path = _local_path(args[-1])
        return self._single(path, True, "Background") if path else []
