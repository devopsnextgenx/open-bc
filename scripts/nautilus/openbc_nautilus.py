"""OpenBC entries in the Nautilus (GNOME Files) context menu (needs python3-nautilus / nautilus-python)."""
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
            if directories[0]:
                return [
                    self._item("Select Left Folder", "--select-left-folder", paths),
                    self._item("Compare to Left Folder", "--compare-to-left-folder", paths),
                ]
            return [
                self._item("Select Left File", "--select-left-file", paths),
                self._item("Compare to Left File", "--compare-to-left-file", paths),
            ]

        if len(paths) == 2 and directories[0] == directories[1]:
            return [self._item("Compare", None, paths)]
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
