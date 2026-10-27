import gi

try:
    gi.require_version("Nautilus", "4.1")
except ValueError:
    gi.require_version("Nautilus", "4.0")

from gi.repository import Gio, GLib, GObject, Nautilus


OPENBC = "/usr/bin/openbc"


class OpenBCMenuProvider(GObject.GObject, Nautilus.MenuProvider):
    def get_file_items(self, files):
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
        arguments = ([action, paths[0]] if action else paths)
        item.connect("activate", self._activate, arguments)
        return item

    @staticmethod
    def _activate(_item, arguments):
        try:
            Gio.Subprocess.new([OPENBC, *arguments], Gio.SubprocessFlags.NONE)
        except GLib.Error as error:
            print("OpenBC could not be started: {}".format(error), flush=True)