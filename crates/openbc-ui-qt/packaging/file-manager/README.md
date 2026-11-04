> Legacy reference. The maintained scripts (Nautilus Scripts + nautilus-python, Dolphin `.desktop.in`, MSI registry in `scripts/build.sh`) use the same `openbc:` names and the same app-managed "Compare To Left" mechanism; see `cpp/app/context_menu_state.*`.

# File-manager integration (Nautilus, Dolphin, Windows Explorer)

The app already understands the command lines these menus run (`cli_options.cpp`, `handleLaunch` in
`main_window.h`; the left item is held in memory only):

| Menu entry | Command | Shown |
|---|---|---|
| openbc: Select Left Folder / File | `openbc --select-left-folder PATH` / `--select-left-file PATH` | always |
| openbc: Compare To `<left item name>` | `openbc --compare-to-left-folder PATH` / `--compare-to-left-file PATH` | only after **Select Left Folder / File**; shown on matching items |
| openbc: Compare (2 items selected) | `openbc LEFT RIGHT` | two items of the same kind |

"Select Left" opens no tab: the running OpenBC (started on demand, Home tab only) keeps the path in memory.
The compare menu is labeled with the selected left item's name and appears only after that selection. The first
tab appears when the compare action supplies the right side (an empty tab is reused). OpenBC publishes
the selection so the file manager can hide "Compare To Left" until it exists (`app/context_menu_state.*`):
Windows writes/removes the `OpenBC.CompareToLeft` verb in HKCU; Linux writes `$XDG_RUNTIME_DIR/openbc/left-selection`
(Nautilus extension reads it) and `~/.local/share/kio/servicemenus/openbc-compare-to-left-*.desktop` (Dolphin).

This folder holds what has to be INSTALLED so the file managers show them. Nothing here is wired into
the root build yet (CMakeLists.txt, scripts/build.sh, release.yml live outside `crates/`). Untested on real desktops.

## Linux

Files in `linux/`: `openbc-fm` (launcher), `openbc-nautilus.py` (Nautilus), three Dolphin service menus
(`openbc-compare-*.desktop`, `@FM_LAUNCHER@` is replaced at install), `install-file-manager-integration.sh`.

| Package | Install | Notes |
|---|---|---|
| deb / rpm | `openbc-fm` -> `/usr/bin`; `openbc-nautilus.py` -> `/usr/share/nautilus-python/extensions/`; `.desktop` -> `/usr/share/kio/servicemenus/` (with `@FM_LAUNCHER@` = `/usr/bin/openbc-fm`, mode 0755) | Add `Recommends: python3-nautilus` (deb) / `nautilus-python` (rpm) |
| Flatpak | cannot register with host file managers from inside the sandbox | user runs `install-file-manager-integration.sh --exec "flatpak run io.github.devopsnextgenx.open-bc"`; the app needs filesystem access to the paths (`--filesystem=host` or similar) |
| AppImage | same limitation | `install-file-manager-integration.sh --exec /path/to/OpenBC.AppImage` |

Nautilus needs `nautilus -q` after installing. Dolphin picks service menus up on reopen.

CMake sketch (adapt to the real file):

```cmake
install(PROGRAMS crates/openbc-ui-qt/packaging/file-manager/linux/openbc-fm DESTINATION bin)
install(FILES crates/openbc-ui-qt/packaging/file-manager/linux/openbc-nautilus.py
        DESTINATION share/nautilus-python/extensions)
foreach(n openbc-compare-folders openbc-compare-files openbc-compare-two)
  configure_file(crates/openbc-ui-qt/packaging/file-manager/linux/${n}.desktop
                 ${CMAKE_BINARY_DIR}/${n}.desktop @ONLY)  # @FM_LAUNCHER@ -> /usr/bin/openbc-fm
  install(PROGRAMS ${CMAKE_BINARY_DIR}/${n}.desktop DESTINATION share/kio/servicemenus)
endforeach()
```
(Set `FM_LAUNCHER` to `/usr/bin/openbc-fm` before `configure_file`.)

## Windows

`windows/openbc-explorer.reg` shows the verbs (classic menu, under "Show more options" on Windows 11;
the new Windows 11 menu needs an `IExplorerCommand` shell extension, not done). For the MSI, the same keys
go into the installer as registry entries under `HKCR` of the exe component (CPack WiX: a
`CPACK_WIX_PATCH_FILE` fragment on the openbc.exe component). Two-item "Compare" uses `MultiSelectModel=Player` (untested; it also appears for a single item, which opens a one-sided compare).
