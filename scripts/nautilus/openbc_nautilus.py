"""OpenBC entries in the Nautilus (GNOME Files) context menu (needs python3-nautilus / nautilus-python).

"openbc: Compare To <left item name>" is offered only while the running OpenBC holds a left item. OpenBC
publishes its kind and path to $XDG_RUNTIME_DIR/openbc/left-selection and deletes the file when the
selection is cleared or OpenBC exits.
"""
import os
import shlex

import gi

# Nautilus 46+ -> 4.1, Nautilus 43-45 -> 4.0, older (e.g. Ubuntu 22.04's 42) -> 3.0.
for _version in ("4.1", "4.0", "3.0"):
    try:
        gi.require_version("Nautilus", _version)
        break
    except ValueError:
        continue

from gi.repository import Gio, GLib, GObject, Nautilus  # noqa: E402


def _openbc_command():
    """Same lookup as the Scripts helper: ~/.config/openbc/launcher, else openbc."""
    config = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    try:
        with open(os.path.join(config, "openbc", "launcher"), encoding="utf-8") as handle:
            command = shlex.split(handle.readline())
            if command:
                return command
    except OSError:
        pass
    return ["/usr/bin/openbc"]


def _left_selection():
    """(kind, path) while OpenBC holds a left item, else None."""
    runtime = os.environ.get("XDG_RUNTIME_DIR") or "/tmp"
    try:
        with open(os.path.join(runtime, "openbc", "left-selection"), encoding="utf-8") as handle:
            kind = handle.readline().strip()
            path = handle.readline().rstrip("\r\n")
    except OSError:
        return None
    return (kind, path) if kind in ("folder", "file") and path else None


class OpenBCMenuProvider(GObject.GObject, Nautilus.MenuProvider):
    # Nautilus 4.x calls get_file_items(files); 3.0 calls get_file_items(window, files).
    def get_file_items(self, *args):
        files = args[-1]
        paths = []
        directories = []
        for file_info in files:
            path = Gio.File.new_for_uri(file_info.get_uri()).get_path()
            if path is None:
                return []
            paths.append(path)
            directories.append(file_info.is_directory())

        if len(paths) == 1:
            kind = "folder" if directories[0] else "file"
            title = kind.capitalize()
            items = [self._item("openbc: Select Left " + title, "--select-left-" + kind, paths)]
            left = _left_selection()
            if left is not None and left[0] == kind:
                left_name = os.path.basename(os.path.normpath(left[1])) or left[1]
                left_name = left_name.replace("\r", " ").replace("\n", " ")
                items.append(self._item("openbc: Compare To " + left_name,
                                        "--compare-to-left-" + kind, paths))
            return items

        if len(paths) == 2 and directories[0] == directories[1]:
            return [self._item("openbc: Compare", None, paths)]
        return []

    def _item(self, label, action, paths):
        item = Nautilus.MenuItem(
            name="OpenBC::" + label.replace(" ", ""),
            label=label,
            tip="Compare with OpenBC",
            icon="",
        )
        arguments = [action, paths[0]] if action else paths
        item.connect("activate", self._activate, arguments)
        return item

    @staticmethod
    def _activate(_item, arguments):
        try:
            Gio.Subprocess.new([*_openbc_command(), *arguments], Gio.SubprocessFlags.NONE)
        except GLib.Error as error:
            print("OpenBC could not be started: {}".format(error), flush=True)
