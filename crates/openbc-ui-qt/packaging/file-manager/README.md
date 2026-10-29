# File-manager integration (Nautilus, Dolphin, Windows Explorer)

The app already understands the command lines these menus run (`cli_options.cpp`, `handleLaunch` in
`main_window.h`; the left item is remembered in preferences `context/leftPath`):

| Menu entry | Command |
|---|---|
| Select Left Folder / File | `openbc --select-left-folder PATH` / `--select-left-file PATH` |
| Compare to Selected Folder / File | `openbc --compare-to-left-folder PATH` / `--compare-to-left-file PATH` |
| Compare with OpenBC (2 items selected) | `openbc LEFT RIGHT` |

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
`CPACK_WIX_PATCH_FILE` fragment on the openbc.exe component). Two-item "Compare" is not offered on
Windows, because static verbs run once per selected item.
