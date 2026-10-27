#!/usr/bin/env bash
# build.sh - unified build + packaging entry point (replaces win-build.sh, build-deb.sh,
# build-msi.sh, build-pkg.sh).

#
#   ./scripts/build.sh [-c] [-p win|lin] [-s deb,rpm,flatpak,appimage,msi] [-r] [-n]
#
#   -c            clean: wipe the build directory first
#   -p PLATFORM   win | lin. Without -s this only BUILDS (no package). With -p and no -s the default
#                 package is chosen: win -> msi, lin -> deb. Omit -p to just build for this host.
#   -s FORMATS    comma-separated package formats to create (deb, rpm, flatpak, appimage, msi); implies -p
#                 from the format when -p is absent (msi -> win, others -> lin)
#   -r            run the app after building (what win-build.sh used to do)
#   -n            dry run: print what would happen, execute nothing
#
# Examples:
#   ./scripts/build.sh -p lin                 # build + .deb
#   ./scripts/build.sh -c -p lin -s deb,rpm   # clean, then .deb and .rpm
#   ./scripts/build.sh -p lin -s appimage     # build + portable .AppImage (linuxdeploy + Qt plugin)
#   ./scripts/build.sh -p win                 # (MSYS2 UCRT64 shell) build + .msi
#   ./scripts/build.sh -r                     # incremental build, then run
set -euo pipefail

CLEAN=false; RUN=false; DRY=false; PLATFORM=""; FORMATS=""
usage() { sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-1}"; }
while getopts "cp:s:rnh" opt; do
  case $opt in
    c) CLEAN=true ;;
    p) PLATFORM="$OPTARG" ;;
    s) FORMATS="$OPTARG" ;;
    r) RUN=true ;;
    n) DRY=true ;;
    h) usage 0 ;;
    *) usage 1 ;;
  esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# ---- host / platform resolution -------------------------------------------------------------
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) HOST=win ;;
  Linux)                HOST=lin ;;
  *) echo "Unsupported host: $(uname -s)" >&2; exit 1 ;;
esac

# -s alone implies the platform from its formats
if [ -z "$PLATFORM" ] && [ -n "$FORMATS" ]; then
  case ",$FORMATS," in
    *,msi,*) PLATFORM=win ;;
    *)       PLATFORM=lin ;;
  esac
fi
# -p without -s -> default package for that platform
if [ -n "$PLATFORM" ] && [ -z "$FORMATS" ]; then
  case "$PLATFORM" in win) FORMATS=msi ;; lin) FORMATS=deb ;; esac
fi
[ -z "$PLATFORM" ] && PLATFORM="$HOST"

case "$PLATFORM" in win|lin) ;; *) echo "-p must be 'win' or 'lin' (got '$PLATFORM')" >&2; exit 1 ;; esac
if [ "$PLATFORM" != "$HOST" ]; then
  echo "Requested platform '$PLATFORM' but this machine is '$HOST'. Run the script on the target OS" >&2
  echo "(Windows: MSYS2 UCRT64 shell). Cross-packaging is not supported." >&2
  exit 1
fi

IFS=',' read -r -a FORMAT_LIST <<< "$FORMATS"
for f in "${FORMAT_LIST[@]:-}"; do
  [ -z "$f" ] && continue
  case "$f" in
    deb|rpm|flatpak|appimage) [ "$PLATFORM" = lin ] || { echo "'$f' is a Linux format (use -p lin)" >&2; exit 1; } ;;
    msi)             [ "$PLATFORM" = win ] || { echo "'msi' is a Windows format (use -p win)" >&2; exit 1; } ;;
    *) echo "Unknown package format '$f' (deb, rpm, flatpak, appimage, msi)" >&2; exit 1 ;;
  esac
done

run() { echo "+ $*"; $DRY || "$@"; }

# ---- environment ------------------------------------------------------------------------------
if [ "$PLATFORM" = win ]; then
  export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:/c/msys64/opt/wix:$PATH"
  export OPENSSL_DIR="/c/msys64/ucrt64"
  export TMPDIR=/tmp TEMP=/tmp TMP=/tmp
fi
if command -v sccache >/dev/null 2>&1; then export RUSTC_WRAPPER=sccache; fi

BUILD_DIR="build/$PLATFORM"      # separate dirs: a Linux and a Windows tree can share a checkout
OUT_DIR="$BUILD_DIR/packages"
VERSION="$(tr -d '\r' < CMakeLists.txt | sed -n 's/^set(CPACK_PACKAGE_VERSION "\(.*\)")/\1/p' | head -1)"
VERSION="${VERSION:-1.0.0}"

# ---- clean + build --------------------------------------------------------------------------
if $CLEAN; then echo "=== Cleaning $BUILD_DIR"; run rm -rf "$BUILD_DIR"; fi

# The host build feeds deb/rpm/msi and -r. Flatpak compiles from source INSIDE its own sandbox against the
# KDE runtime (different Qt/OpenSSL/glibc), so it can never reuse these binaries - skip the host build
# when flatpak is the only thing requested.
NEED_HOST_BUILD=false
[ -z "$FORMATS" ] && NEED_HOST_BUILD=true          # plain build, no packaging
$RUN && NEED_HOST_BUILD=true
for f in "${FORMAT_LIST[@]:-}"; do
  [ -n "$f" ] && [ "$f" != flatpak ] && NEED_HOST_BUILD=true
done

if $NEED_HOST_BUILD; then
  echo "=== Building openbc ($PLATFORM, Release)"
  if [ ! -f "$BUILD_DIR/build.ninja" ] && [ ! -f "$BUILD_DIR/Makefile" ]; then
    if command -v ninja >/dev/null 2>&1; then GEN=(-G Ninja); else GEN=(); fi
    run cmake -B "$BUILD_DIR" "${GEN[@]}" -DCMAKE_BUILD_TYPE=Release
  fi
  run cmake --build "$BUILD_DIR" --target openbc openbc-depcheck
else
  echo "=== Skipping host build (flatpak compiles inside its own sandbox)"
fi
$DRY || mkdir -p "$OUT_DIR"

# ---- deploy Windows runtime DLLs -------------------------------------------------------------

deploy_win_runtime() {
  echo "=== Copying MinGW/UCRT runtime DLLs into $BUILD_DIR"
  $DRY && return 0
  "$BUILD_DIR/openbc-depcheck.exe" "$BUILD_DIR/openbc.exe" | tr -d '\r' \
    | sed -n 's/^  ok  *\(.*\.dll\) -> [Cc]:\\msys64\\ucrt64\\bin\\.*/\1/p' \
    | while read -r dll; do cp -n "/c/msys64/ucrt64/bin/$dll" "$BUILD_DIR/" && echo "   + $dll"; done
}
if [ "$PLATFORM" = win ]; then deploy_win_runtime; fi

# ---- package builders -------------------------------------------------------------------------
pkg_deb() {
  local arch stage="$BUILD_DIR/stage_deb" out
  arch="$(dpkg --print-architecture 2>/dev/null || echo amd64)"
  out="$OUT_DIR/openbc_${VERSION}_${arch}.deb"
  echo "=== deb -> $out"
  run rm -rf "$stage"
  run mkdir -p "$stage/usr/bin" "$stage/DEBIAN" \
    "$stage/usr/share/applications" "$stage/usr/share/metainfo" \
    "$stage/usr/share/icons/hicolor/512x512/apps" \
    "$stage/usr/share/openbc/nautilus" "$stage/usr/share/nautilus-python/extensions"

  # Single source of truth: CMake's install() rules (binaries, .desktop, metainfo, hicolor icons)
  # - identical to what the Flatpak build installs.
  run cmake --install "$BUILD_DIR" --prefix "$stage/usr"
  run install -m 0755 scripts/nautilus/openbc-nautilus-action "$stage/usr/share/openbc/nautilus/"
  run install -m 0755 scripts/nautilus/install-nautilus.sh "$stage/usr/bin/openbc-install-nautilus"
  run install -m 0644 scripts/nautilus/openbc_nautilus.py "$stage/usr/share/nautilus-python/extensions/openbc.py"

  # refresh icon / desktop caches so the launcher icon shows up immediately after install
  for hook in postinst postrm; do
    $DRY || cat > "$stage/DEBIAN/$hook" <<'HOOK'
#!/bin/sh
set -e
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
  gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
fi
if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database -q /usr/share/applications || true
fi
exit 0
HOOK
    $DRY || chmod 0755 "$stage/DEBIAN/$hook"
  done

  $DRY || cat > "$stage/DEBIAN/control" <<CTRL
Package: openbc
Version: ${VERSION}
Section: utils
Priority: optional
Architecture: ${arch}
Depends: libqt6widgets6 (>= 6.0), libqt6network6 (>= 6.0), libqt6opengl6 (>= 6.0), libqt6openglwidgets6 (>= 6.0), libqt6multimedia6 (>= 6.0), libqt6multimediawidgets6 (>= 6.0), python3-nautilus, zlib1g, liblzma5, libssl3
Maintainer: OpenBC Team <devopsnexegenx@gmail.com>
Description: OpenBC File and Directory Comparison Tool
 OpenBC is an open-source comparison tool integrated with Rust backends and Qt6 GUI.
CTRL
  run dpkg-deb --root-owner-group --build "$stage" "$out"
}

pkg_rpm() {
  local top="$ROOT/$BUILD_DIR/rpmbuild"
  echo "=== rpm -> $OUT_DIR"
  $DRY || command -v rpmbuild >/dev/null 2>&1 || { echo "rpmbuild not found" >&2; return 1; }
  run rm -rf "$top"
  run mkdir -p "$top"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
  $DRY || cat > "$top/SPECS/openbc.spec" <<SPEC
%global debug_package %{nil}
Name:           openbc
Version:        ${VERSION}
Release:        1
Summary:        OpenBC File and Directory Comparison Tool
License:        Proprietary
AutoReqProv:    yes
Requires:       qt6-qtbase-gui, openssl-libs, zlib, xz-libs, qt6-qtmultimedia, hicolor-icon-theme

%description
OpenBC is a comparison tool with Rust backends and a Qt6 GUI.

%install
cmake --install $ROOT/$BUILD_DIR --prefix %{buildroot}/usr

%post
gtk-update-icon-cache -q -t -f %{_datadir}/icons/hicolor >/dev/null 2>&1 || :
update-desktop-database -q %{_datadir}/applications >/dev/null 2>&1 || :

%postun
gtk-update-icon-cache -q -t -f %{_datadir}/icons/hicolor >/dev/null 2>&1 || :
update-desktop-database -q %{_datadir}/applications >/dev/null 2>&1 || :

%files
/usr/bin/openbc
/usr/bin/openbc-depcheck
/usr/share/applications/io.openbc.OpenBC.desktop
/usr/share/metainfo/io.openbc.OpenBC.metainfo.xml
/usr/share/icons/hicolor/*/apps/io.openbc.OpenBC.png
SPEC
  run rpmbuild -bb --define "_topdir $top" "$top/SPECS/openbc.spec"
  $DRY || find "$top/RPMS" -name '*.rpm' -exec cp {} "$OUT_DIR/" \;
}

pkg_flatpak() {
  echo "=== flatpak -> $OUT_DIR"
  $DRY || command -v flatpak-builder >/dev/null 2>&1 || { echo "flatpak-builder not found" >&2; return 1; }

  local tmpl="scripts/packaging/io.openbc.OpenBC.yml.in"
  # Rendered next to the template so relative sources (cargo-sources.json) still resolve.
  local manifest="scripts/packaging/io.openbc.OpenBC.generated.yml"
  [ -f "$tmpl" ] || { echo "missing $tmpl" >&2; return 1; }

  # Peeled commit SHA for a tag. For annotated tags, ls-remote lists the tag object first and
  # the real commit second (refs/tags/X^{}); tail -1 picks the commit in both cases.
  tag_commit() {
    git ls-remote "$1" "refs/tags/$2" "refs/tags/$2^{}" | tail -1 | cut -f1
  }

  # Local vs release mode. CI (GITHUB_ACTIONS/CI set) builds the pushed tag from the GitHub remote.
  # Anywhere else the tag usually is not pushed yet, so build from this checkout's HEAD instead.
  # Force with OPENBC_FLATPAK_LOCAL=1 (local) or =0 (remote tag).
  local local_src=false
  if [ "${OPENBC_FLATPAK_LOCAL:-}" = 1 ]; then local_src=true
  elif [ -z "${OPENBC_FLATPAK_LOCAL:-}" ] && [ -z "${GITHUB_ACTIONS:-}${CI:-}" ]; then local_src=true; fi

  local app_tag="${OPENBC_TAG:-v$VERSION}" app_url="https://github.com/devopsnextgenx/open-bc.git"
  local app_commit corro_commit
  corro_commit="$(tag_commit https://github.com/corrosion-rs/corrosion.git v0.6.1)"
  [ -n "$corro_commit" ] || { echo "corrosion v0.6.1 not found" >&2; return 1; }

  local sed_args=()
  if $local_src; then
    app_url="file://$ROOT"
    app_commit="$(git rev-parse HEAD)"
    echo "--- flatpak sources: LOCAL checkout $ROOT @ $app_commit (tag lookup skipped; uncommitted changes are NOT included)"
    # The local commit may carry no tag, so drop any 'tag:' pin; the commit alone identifies the source.
    sed_args+=(-e '/^[[:space:]]*tag:[[:space:]]*@APP_TAG@[[:space:]]*$/d'
               -e "s|https://github.com/devopsnextgenx/open-bc.git|$app_url|g")
  else
    app_commit="$(tag_commit "$app_url" "$app_tag")"
    [ -n "$app_commit" ] || { echo "tag $app_tag not found on open-bc remote (push the tag first, or set OPENBC_FLATPAK_LOCAL=1)" >&2; return 1; }
    echo "--- flatpak sources: open-bc $app_tag @ $app_commit"
  fi
  echo "--- corrosion v0.6.1 @ $corro_commit"

  # Offline Cargo sources are generated by CI from Cargo.lock; locally they must exist already.
  $DRY || [ -s scripts/packaging/cargo-sources.json ] || {
    echo "missing scripts/packaging/cargo-sources.json - generate it first:" >&2
    echo "  python3 scripts/packaging/flatpak-cargo-generator.py Cargo.lock -o scripts/packaging/cargo-sources.json" >&2
    return 1; }

  $DRY || sed "${sed_args[@]}" \
               -e "s|@APP_TAG@|$app_tag|g" \
               -e "s|@APP_COMMIT@|$app_commit|g" \
               -e "s|@CORROSION_COMMIT@|$corro_commit|g" "$tmpl" > "$manifest"

  run flatpak-builder --force-clean --ccache --user --install-deps-from=flathub \
      --repo="$BUILD_DIR/flatpak-repo" "$BUILD_DIR/flatpak-build" "$manifest"
  run flatpak build-bundle "$BUILD_DIR/flatpak-repo" "$OUT_DIR/openbc-${VERSION}.flatpak" io.openbc.OpenBC
}

pkg_appimage() {
  local arch appdir work tools out n url f qmake
  arch="$(uname -m)"
  case "$arch" in
    x86_64|aarch64) ;;
    *) echo "AppImage: unsupported architecture '$arch'" >&2; return 1 ;;
  esac
  appdir="$ROOT/$BUILD_DIR/AppDir"
  work="$ROOT/$BUILD_DIR/appimage-work"
  # linuxdeploy tools are cached outside the build tree so `-c` does not re-download them.
  tools="${OPENBC_APPIMAGE_TOOLS:-${XDG_CACHE_HOME:-$HOME/.cache}/openbc/appimage-tools}"
  out="$ROOT/$OUT_DIR/openbc-${VERSION}-${arch}.AppImage"
  echo "=== appimage -> $out"

  # linuxdeploy + its Qt and AppImage plugins. The plugins must sit next to (or on PATH of) linuxdeploy.
  # They are the upstream "continuous" builds; pin a release here if you need reproducible packages.
  run mkdir -p "$tools"
  for n in linuxdeploy linuxdeploy-plugin-qt linuxdeploy-plugin-appimage; do
    f="$tools/$n-$arch.AppImage"
    url="https://github.com/linuxdeploy/$n/releases/download/continuous/$n-$arch.AppImage"
    if [ ! -x "$f" ]; then
      echo "--- downloading $n"
      run curl -fL --retry 3 -o "$f.part" "$url"
      run chmod +x "$f.part"
      run mv "$f.part" "$f"
    fi
  done

  # linuxdeploy-plugin-qt asks qmake where the Qt plugins live.
  qmake=""
  for c in qmake6 qmake-qt6 qmake; do
    command -v "$c" >/dev/null 2>&1 && { qmake="$(command -v "$c")"; break; }
  done
  $DRY || [ -n "$qmake" ] || { echo "qmake6 not found (apt: qmake6, dnf: qt6-qtbase-devel)" >&2; return 1; }

  # Same install() rules as deb/rpm/flatpak: binaries, .desktop, metainfo, hicolor icons.
  run rm -rf "$appdir" "$work"
  run mkdir -p "$appdir" "$work"
  run cmake --install "$BUILD_DIR" --prefix "$appdir/usr"

  (
    $DRY || cd "$work"
    # FUSE is often missing (containers, CI); let the AppImage-based tools unpack themselves instead.
    export APPIMAGE_EXTRACT_AND_RUN=1
    export ARCH="$arch" QMAKE="${qmake:-qmake6}"
    export PATH="$tools:$PATH"
    export DEPLOY_PLATFORM_THEMES=1                      # bundle platform themes (GTK/KDE look)
    # Bundle the Wayland platform plugins only when this Qt actually has them (Ubuntu: qt6-wayland,
    # Fedora: qt6-qtwayland). Asking for a missing one makes the Qt plugin abort the whole deploy.
    local pdir="" wl="" p
    [ -n "$qmake" ] && pdir="$("$qmake" -query QT_INSTALL_PLUGINS 2>/dev/null || true)/platforms"
    for p in libqwayland-generic.so libqwayland-egl.so; do
      [ -f "$pdir/$p" ] && wl="${wl:+$wl;}$p"
    done
    if [ -n "$wl" ]; then
      export EXTRA_PLATFORM_PLUGINS="$wl"
    else
      echo "--- Wayland platform plugins not found: AppImage will be X11/XWayland only (install qt6-wayland to add them)"
    fi
    export LINUXDEPLOY_OUTPUT_VERSION="$VERSION"
    # Optional delta updates: export LDAI_UPDATE_INFORMATION="gh-releases-zsync|devopsnextgenx|open-bc|latest|openbc-*-$arch.AppImage.zsync"
    run "$tools/linuxdeploy-$arch.AppImage" \
      --appdir "$appdir" \
      --executable "$appdir/usr/bin/openbc" \
      --desktop-file "$appdir/usr/share/applications/io.openbc.OpenBC.desktop" \
      --icon-file "$appdir/usr/share/icons/hicolor/512x512/apps/io.openbc.OpenBC.png" \
      --plugin qt \
      --output appimage
  )

  if ! $DRY; then
    local built
    built="$(find "$work" -maxdepth 1 -name '*.AppImage' -print -quit)"
    [ -n "$built" ] || { echo "linuxdeploy produced no .AppImage in $work" >&2; return 1; }
    mv "$built" "$out"
    chmod +x "$out"
    find "$work" -maxdepth 1 -name '*.AppImage.zsync' -exec mv {} "$OUT_DIR/" \; || true

    # Smoke test: every shared library of the bundled binary must resolve (system libs or the AppDir).
    echo "--- ldd check"
    if LD_LIBRARY_PATH="$appdir/usr/lib" ldd "$appdir/usr/bin/openbc" | grep 'not found'; then
      echo "ERROR: AppImage would be missing the libraries listed above." >&2
      return 1
    fi
  fi
}

pkg_msi() {
  local stage="$BUILD_DIR/staging" wix="$BUILD_DIR/wix" out="$OUT_DIR/openbc-installer.msi"
  echo "=== msi -> $out"
  run rm -rf "$stage" "$wix"
  run mkdir -p "$stage" "$wix"
  run cp "$BUILD_DIR/openbc.exe" "$BUILD_DIR/openbc-depcheck.exe" "$stage/"

  echo "--- windeployqt (Qt DLLs + platforms/qwindows.dll)"
  local WDQ=""
  for c in windeployqt6 windeployqt-qt6 windeployqt; do
    command -v "$c" >/dev/null 2>&1 && { WDQ="$c"; break; }
  done
  $DRY || [ -n "$WDQ" ] || { echo "windeployqt not found (pacman -S mingw-w64-ucrt-x86_64-qt6-tools)" >&2; return 1; }
  run "${WDQ:-windeployqt6}" --no-compiler-runtime --no-translations "$stage/openbc.exe"

  echo "--- copying remaining MinGW/UCRT DLLs (scans openbc.exe AND every Qt plugin / DLL in the stage)"
  if ! $DRY; then
    local UCRT=/c/msys64/ucrt64/bin SYS32=/c/Windows/System32
    # case-insensitive "is this DLL available in dir?" (PE import names and file names often differ in case)
    have_dll() { [ -n "$(find "$1" -maxdepth 1 -iname "$2" -print -quit 2>/dev/null)" ]; }
    is_builtin() { case "${1,,}" in api-ms-*|ext-ms-*) return 0 ;; esac; have_dll "$SYS32" "$1"; }
    list_imports() { objdump -p "$1" 2>/dev/null | tr -d '\r' | sed -n 's/^[[:space:]]*DLL Name: //p'; }

    for _ in $(seq 1 15); do
      copied=0
      while IFS= read -r bin; do
        while IFS= read -r dll; do
          [ -z "$dll" ] && continue
          is_builtin "$dll" && continue
          if ! have_dll "$stage" "$dll"; then
            src="$(find "$UCRT" -maxdepth 1 -iname "$dll" -print -quit 2>/dev/null)"
            [ -n "$src" ] && cp "$src" "$stage/" && echo "   + $(basename "$src")" && copied=1
          fi
        done < <(list_imports "$bin")
      done < <(find "$stage" -type f \( -iname '*.exe' -o -iname '*.dll' \))
      [ "$copied" = 0 ] && break
    done

    echo "--- final dependency report (everything must resolve inside the stage or System32)"
    PATH="$SYS32" "$stage/openbc-depcheck.exe" "$stage/openbc.exe" || {
      echo "ERROR: the package would be missing the DLLs listed above." >&2; exit 2; }
    unresolved=0
    while IFS= read -r bin; do
      while IFS= read -r dll; do
        [ -z "$dll" ] && continue
        is_builtin "$dll" && continue
        have_dll "$stage" "$dll" || { echo "  MISSING $dll (needed by ${bin#$stage/})" >&2; unresolved=1; }
      done < <(list_imports "$bin")
    done < <(find "$stage" -type f \( -iname '*.exe' -o -iname '*.dll' \))
    [ "$unresolved" = 0 ] || { echo "ERROR: unresolved DLL imports in plugins/libraries (see above)." >&2; exit 2; }
  fi

  # ---- icon: used for Start-menu/Desktop shortcuts, Add/Remove Programs and context-menu entries ----
  local ico="$ROOT/resources/icons/openbc.ico"
  $DRY || [ -f "$ico" ] || { echo "missing $ico - run scripts/gen-icons.sh and commit the result" >&2; exit 2; }
  winpath() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else echo "$1"; fi; }

  echo "--- WiX"
  $DRY || cat > "$wix/openbc.wxs" <<WXS
<?xml version="1.0" encoding="UTF-8"?>
<Wix xmlns="http://schemas.microsoft.com/wix/2006/wi">
  <Product Id="*" Name="OpenBC" Language="1033" Version="${VERSION}.0" Manufacturer="OpenBC Team" UpgradeCode="a1b2c3d4-e5f6-7890-abcd-ef1234567890">
    <Package InstallerVersion="200" Compressed="yes" InstallScope="perMachine" Platform="x64" />
    <MediaTemplate EmbedCab="yes" />
    <MajorUpgrade DowngradeErrorMessage="A newer version of [ProductName] is already installed." />
    <Icon Id="openbc.ico" SourceFile="\$(var.IconFile)" />
    <Property Id="ARPPRODUCTICON" Value="openbc.ico" />
    <Directory Id="TARGETDIR" Name="SourceDir">
      <Directory Id="ProgramFiles64Folder"><Directory Id="INSTALLFOLDER" Name="OpenBC" /></Directory>
      <Directory Id="ProgramMenuFolder"><Directory Id="ApplicationProgramsFolder" Name="OpenBC"/></Directory>
      <Directory Id="DesktopFolder" Name="Desktop" />
    </Directory>
    <DirectoryRef Id="ApplicationProgramsFolder">
      <Component Id="ApplicationShortcut" Guid="b2c3d4e5-f6a7-8901-bcde-f23456789012">
        <Shortcut Id="ApplicationStartMenuShortcut" Name="OpenBC" Description="OpenBC File &amp; Folder Comparison" Target="[INSTALLFOLDER]openbc.exe" WorkingDirectory="INSTALLFOLDER" Icon="openbc.ico"/>
        <RemoveFolder Id="CleanUpShortCut" On="uninstall"/>
        <RegistryValue Root="HKCU" Key="Software\OpenBC" Name="installed" Type="integer" Value="1" KeyPath="yes"/>
      </Component>
    </DirectoryRef>
    <DirectoryRef Id="DesktopFolder">
      <Component Id="DesktopShortcut" Guid="c3d4e5f6-a7b8-9012-cdef-345678901234">
        <Shortcut Id="ApplicationDesktopShortcut" Name="OpenBC" Description="OpenBC File &amp; Folder Comparison" Target="[INSTALLFOLDER]openbc.exe" WorkingDirectory="INSTALLFOLDER" Icon="openbc.ico"/>
        <RegistryValue Root="HKCU" Key="Software\OpenBC" Name="desktop_shortcut" Type="integer" Value="1" KeyPath="yes"/>
      </Component>
    </DirectoryRef>
    <DirectoryRef Id="TARGETDIR">
      <Component Id="OpenBCFolderSelectLeft" Guid="*">
        <RegistryKey Root="HKCR" Key="Directory\shell\OpenBC.SelectLeftFolder">
          <RegistryValue Name="MUIVerb" Type="string" Value="Select Left Folder" KeyPath="yes" />
          <RegistryValue Name="Icon" Type="string" Value="[INSTALLFOLDER]openbc.exe,0" />
          <RegistryKey Key="command"><RegistryValue Type="string" Value="&quot;[INSTALLFOLDER]openbc.exe&quot; --select-left-folder &quot;%1&quot;" /></RegistryKey>
        </RegistryKey>
      </Component>
      <Component Id="OpenBCFolderCompareToLeft" Guid="*">
        <RegistryKey Root="HKCR" Key="Directory\shell\OpenBC.CompareToLeftFolder">
          <RegistryValue Name="MUIVerb" Type="string" Value="Compare to Left Folder" KeyPath="yes" />
          <RegistryValue Name="Icon" Type="string" Value="[INSTALLFOLDER]openbc.exe,0" />
          <RegistryKey Key="command"><RegistryValue Type="string" Value="&quot;[INSTALLFOLDER]openbc.exe&quot; --compare-to-left-folder &quot;%1&quot;" /></RegistryKey>
        </RegistryKey>
      </Component>
      <Component Id="OpenBCFileSelectLeft" Guid="*">
        <RegistryKey Root="HKCR" Key="*\shell\OpenBC.SelectLeftFile">
          <RegistryValue Name="MUIVerb" Type="string" Value="Select Left File" KeyPath="yes" />
          <RegistryValue Name="Icon" Type="string" Value="[INSTALLFOLDER]openbc.exe,0" />
          <RegistryKey Key="command"><RegistryValue Type="string" Value="&quot;[INSTALLFOLDER]openbc.exe&quot; --select-left-file &quot;%1&quot;" /></RegistryKey>
        </RegistryKey>
      </Component>
      <Component Id="OpenBCFileCompareToLeft" Guid="*">
        <RegistryKey Root="HKCR" Key="*\shell\OpenBC.CompareToLeftFile">
          <RegistryValue Name="MUIVerb" Type="string" Value="Compare to Left File" KeyPath="yes" />
          <RegistryValue Name="Icon" Type="string" Value="[INSTALLFOLDER]openbc.exe,0" />
          <RegistryKey Key="command"><RegistryValue Type="string" Value="&quot;[INSTALLFOLDER]openbc.exe&quot; --compare-to-left-file &quot;%1&quot;" /></RegistryKey>
        </RegistryKey>
      </Component>
      <Component Id="OpenBCFolderCompareSelection" Guid="*">
        <RegistryKey Root="HKCR" Key="Directory\shell\OpenBC.Compare">
          <RegistryValue Name="MUIVerb" Type="string" Value="Compare" KeyPath="yes" />
          <RegistryValue Name="Icon" Type="string" Value="[INSTALLFOLDER]openbc.exe,0" />
          <RegistryValue Name="MultiSelectModel" Type="string" Value="Player" />
          <RegistryKey Key="command"><RegistryValue Type="string" Value="&quot;[INSTALLFOLDER]openbc.exe&quot; %*" /></RegistryKey>
        </RegistryKey>
      </Component>
      <Component Id="OpenBCFileCompareSelection" Guid="*">
        <RegistryKey Root="HKCR" Key="*\shell\OpenBC.Compare">
          <RegistryValue Name="MUIVerb" Type="string" Value="Compare" KeyPath="yes" />
          <RegistryValue Name="Icon" Type="string" Value="[INSTALLFOLDER]openbc.exe,0" />
          <RegistryValue Name="MultiSelectModel" Type="string" Value="Player" />
          <RegistryKey Key="command"><RegistryValue Type="string" Value="&quot;[INSTALLFOLDER]openbc.exe&quot; %*" /></RegistryKey>
        </RegistryKey>
      </Component>
    </DirectoryRef>
    <Feature Id="MainFeature" Title="OpenBC Core" Level="1">
      <ComponentGroupRef Id="AppFiles" />
      <ComponentRef Id="ApplicationShortcut" />
      <ComponentRef Id="DesktopShortcut" />
      <ComponentRef Id="OpenBCFolderSelectLeft" />
      <ComponentRef Id="OpenBCFolderCompareToLeft" />
      <ComponentRef Id="OpenBCFileSelectLeft" />
      <ComponentRef Id="OpenBCFileCompareToLeft" />
      <ComponentRef Id="OpenBCFolderCompareSelection" />
      <ComponentRef Id="OpenBCFileCompareSelection" />
    </Feature>
  </Product>
</Wix>
WXS
  run heat dir "$stage" -cg AppFiles -dr INSTALLFOLDER -scom -sreg -sfrag -srd -gg -var var.StagingDir -out "$wix/files.wxs"
  run candle -arch x64 -dStagingDir="$stage" -dIconFile="$(winpath "$ico")" -out "$wix/" "$wix/openbc.wxs" "$wix/files.wxs"
  run light -ext WixUIExtension -b "$stage" "$wix/openbc.wixobj" "$wix/files.wixobj" -o "$out"
}

for f in "${FORMAT_LIST[@]:-}"; do
  [ -z "$f" ] && continue
  "pkg_$f"
done

if $RUN; then
  if [ "$PLATFORM" = win ]; then run "$BUILD_DIR/openbc.exe"; else run "$BUILD_DIR/openbc"; fi
fi
echo "=== Done ($PLATFORM${FORMATS:+, packages: $FORMATS})"