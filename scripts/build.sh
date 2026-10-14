#!/usr/bin/env bash
# build.sh - unified build + packaging entry point (replaces win-build.sh, build-deb.sh,
# build-msi.sh, build-pkg.sh).
#
#   ./scripts/build.sh [-c] [-p win|lin] [-s deb,rpm,flatpak,msi] [-r] [-n]
#
#   -c            clean: wipe the build directory first
#   -p PLATFORM   win | lin. Without -s this only BUILDS (no package). With -p and no -s the default
#                 package is chosen: win -> msi, lin -> deb. Omit -p to just build for this host.
#   -s FORMATS    comma-separated package formats to create (deb, rpm, flatpak, msi); implies -p
#                 from the format when -p is absent (msi -> win, others -> lin)
#   -r            run the app after building (what win-build.sh used to do)
#   -n            dry run: print what would happen, execute nothing
#
# Examples:
#   ./scripts/build.sh -p lin                 # build + .deb
#   ./scripts/build.sh -c -p lin -s deb,rpm   # clean, then .deb and .rpm
#   ./scripts/build.sh -p win                 # (MSYS2 UCRT64 shell) build + .msi
#   ./scripts/build.sh -r                     # incremental build, then run
set -euo pipefail

CLEAN=false; RUN=false; DRY=false; PLATFORM=""; FORMATS=""
usage() { sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-1}"; }
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
    deb|rpm|flatpak) [ "$PLATFORM" = lin ] || { echo "'$f' is a Linux format (use -p lin)" >&2; exit 1; } ;;
    msi)             [ "$PLATFORM" = win ] || { echo "'msi' is a Windows format (use -p win)" >&2; exit 1; } ;;
    *) echo "Unknown package format '$f' (deb, rpm, flatpak, msi)" >&2; exit 1 ;;
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

echo "=== Building openbc ($PLATFORM, Release)"
if [ ! -f "$BUILD_DIR/build.ninja" ] && [ ! -f "$BUILD_DIR/Makefile" ]; then
  if command -v ninja >/dev/null 2>&1; then GEN=(-G Ninja); else GEN=(); fi
  run cmake -B "$BUILD_DIR" "${GEN[@]}" -DCMAKE_BUILD_TYPE=Release
fi
run cmake --build "$BUILD_DIR" --target openbc openbc-depcheck
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
    "$stage/usr/share/applications" "$stage/usr/share/icons/hicolor/512x512/apps" \
    "$stage/usr/share/openbc/nautilus" "$stage/usr/share/nautilus-python/extensions"
  run cp "$BUILD_DIR/openbc" "$BUILD_DIR/openbc-depcheck" "$stage/usr/bin/"
  run install -m 0755 scripts/nautilus/openbc-nautilus-action "$stage/usr/share/openbc/nautilus/"
  run install -m 0755 scripts/nautilus/install-nautilus.sh "$stage/usr/bin/openbc-install-nautilus"
  run install -m 0644 scripts/nautilus/openbc_nautilus.py "$stage/usr/share/nautilus-python/extensions/openbc.py"
  run cp resources/io.openbc.OpenBC.desktop "$stage/usr/share/applications/"
  run cp resources/icons/fevicon.png "$stage/usr/share/icons/hicolor/512x512/apps/openbc.png"
  $DRY || cat > "$stage/DEBIAN/control" <<CTRL
Package: openbc
Version: ${VERSION}
Section: utils
Priority: optional
Architecture: ${arch}
Depends: libqt6widgets6 (>= 6.0), libqt6network6 (>= 6.0), libqt6opengl6 (>= 6.0), libqt6openglwidgets6 (>= 6.0), python3-nautilus, zlib1g, liblzma5, libssl3
Maintainer: OpenBC Team <info@example.com>
Description: OpenBC File and Directory Comparison Tool
 OpenBC is an open-source comparison tool integrated with Rust backends and Qt6 GUI.
CTRL
  run dpkg-deb --root-owner-group --build "$stage" "$out"
}

pkg_rpm() {
  local top="$ROOT/$BUILD_DIR/rpmbuild"
  echo "=== rpm -> $OUT_DIR"
  $DRY || command -v rpmbuild >/dev/null 2>&1 || { echo "rpmbuild not found (install rpm-build)" >&2; return 1; }
  run rm -rf "$top"
  run mkdir -p "$top"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
  $DRY || cat > "$top/SPECS/openbc.spec" <<SPEC
Name:           openbc
Version:        ${VERSION}
Release:        1
Summary:        OpenBC File and Directory Comparison Tool
License:        Proprietary
AutoReqProv:    yes
Requires:       qt6-qtbase-gui, openssl-libs, zlib, xz-libs

%description
OpenBC is a comparison tool with Rust backends and a Qt6 GUI.

%install
mkdir -p %{buildroot}/usr/bin
install -m 0755 $ROOT/$BUILD_DIR/openbc %{buildroot}/usr/bin/openbc
install -m 0755 $ROOT/$BUILD_DIR/openbc-depcheck %{buildroot}/usr/bin/openbc-depcheck
mkdir -p %{buildroot}/usr/share/openbc/nautilus
install -m 0755 $ROOT/scripts/nautilus/openbc-nautilus-action %{buildroot}/usr/share/openbc/nautilus/
install -m 0755 $ROOT/scripts/nautilus/install-nautilus.sh %{buildroot}/usr/bin/openbc-install-nautilus

%files
/usr/bin/openbc
/usr/bin/openbc-depcheck
/usr/bin/openbc-install-nautilus
/usr/share/openbc/nautilus/openbc-nautilus-action
SPEC
  run rpmbuild -bb --define "_topdir $top" "$top/SPECS/openbc.spec"
  $DRY || find "$top/RPMS" -name '*.rpm' -exec cp {} "$OUT_DIR/" \;
}

pkg_flatpak() {
  echo "=== flatpak -> $OUT_DIR"
  $DRY || command -v flatpak-builder >/dev/null 2>&1 || { echo "flatpak-builder not found" >&2; return 1; }
  local manifest="scripts/packaging/io.openbc.OpenBC.yml"
  [ -f "$manifest" ] || { echo "missing $manifest" >&2; return 1; }
  run flatpak-builder --force-clean --user --install-deps-from=flathub \
      --repo="$BUILD_DIR/flatpak-repo" "$BUILD_DIR/flatpak-build" "$manifest"
  run flatpak build-bundle "$BUILD_DIR/flatpak-repo" "$OUT_DIR/openbc-${VERSION}.flatpak" io.openbc.OpenBC
}

pkg_msi() {
  local stage="$BUILD_DIR/staging" wix="$BUILD_DIR/wix" out="$OUT_DIR/openbc-installer.msi"
  echo "=== msi -> $out"
  run rm -rf "$stage" "$wix"
  run mkdir -p "$stage" "$wix"
  run cp "$BUILD_DIR/openbc.exe" "$BUILD_DIR/openbc-depcheck.exe" "$stage/"

  echo "--- windeployqt (Qt DLLs + platforms/qwindows.dll)"
  run windeployqt --no-compiler-runtime --no-translations "$stage/openbc.exe"

  echo "--- copying remaining MinGW/UCRT DLLs until openbc-depcheck reports nothing missing"
  # openbc-depcheck is the same checker as 'openbc --dependency-check'; it reads the PE import
  # tables, so this loop no longer depends on ldd output formats.
  # if ! $DRY; then
  #   for _ in 1 2 3 4 5 6 7 8; do
  #     missing="$("$stage/openbc-depcheck.exe" "$stage/openbc.exe" --dir /c/msys64/ucrt64/bin | sed -n 's/^  MISSING  \(.*\.dll\) (needed by.*/\1/p' || true)"
  #     [ -z "$missing" ] && break
  #     while IFS= read -r dll; do
  #       [ -f "/c/msys64/ucrt64/bin/$dll" ] && cp "/c/msys64/ucrt64/bin/$dll" "$stage/" && echo "   + $dll"
  #     done <<< "$missing"
  #   done
  #   echo "--- final dependency report"
  #   # Search ONLY the staging folder (plus Windows itself) so the report reflects what the MSI ships.
  #   PATH="/c/Windows/System32" "$stage/openbc-depcheck.exe" "$stage/openbc.exe" || {
  #     echo "ERROR: the package would be missing the DLLs listed above." >&2; exit 2; }
  # fi

  echo "--- copying remaining MinGW/UCRT DLLs"
  if ! $DRY; then
    UCRT=/c/msys64/ucrt64/bin
    for _ in $(seq 1 10); do
      copied=0
      while IFS= read -r bin; do
        while IFS= read -r dll; do
          [ -z "$dll" ] && continue
          if [ ! -f "$stage/$dll" ] && [ -f "$UCRT/$dll" ]; then
            cp "$UCRT/$dll" "$stage/" && echo "   + $dll" && copied=1
          fi
        done < <(objdump -p "$bin" 2>/dev/null | tr -d '\r' | sed -n 's/^[[:space:]]*DLL Name: //p')
      done < <(find "$stage" -type f \( -iname '*.exe' -o -iname '*.dll' \))
      [ "$copied" = 0 ] && break
    done
    echo "--- final dependency report"
    PATH="/c/Windows/System32" "$stage/openbc-depcheck.exe" "$stage/openbc.exe" || {
      echo "ERROR: the package would be missing the DLLs listed above." >&2; exit 2; }
  fi

  echo "--- WiX"
  $DRY || cat > "$wix/openbc.wxs" <<WXS
<?xml version="1.0" encoding="UTF-8"?>
<Wix xmlns="http://schemas.microsoft.com/wix/2006/wi">
  <Product Id="*" Name="OpenBC" Language="1033" Version="${VERSION}.0" Manufacturer="OpenBC Team" UpgradeCode="a1b2c3d4-e5f6-7890-abcd-ef1234567890">
    <Package InstallerVersion="200" Compressed="yes" InstallScope="perMachine" Platform="x64" />
    <MediaTemplate EmbedCab="yes" />
    <MajorUpgrade DowngradeErrorMessage="A newer version of [ProductName] is already installed." />
    <Directory Id="TARGETDIR" Name="SourceDir">
      <Directory Id="ProgramFiles64Folder"><Directory Id="INSTALLFOLDER" Name="OpenBC" /></Directory>
      <Directory Id="ProgramMenuFolder"><Directory Id="ApplicationProgramsFolder" Name="OpenBC"/></Directory>
      <Directory Id="DesktopFolder" Name="Desktop" />
    </Directory>
    <DirectoryRef Id="ApplicationProgramsFolder">
      <Component Id="ApplicationShortcut" Guid="b2c3d4e5-f6a7-8901-bcde-f23456789012">
        <Shortcut Id="ApplicationStartMenuShortcut" Name="OpenBC" Description="OpenBC File &amp; Folder Comparison" Target="[INSTALLFOLDER]openbc.exe" WorkingDirectory="INSTALLFOLDER"/>
        <RemoveFolder Id="CleanUpShortCut" On="uninstall"/>
        <RegistryValue Root="HKCU" Key="Software\OpenBC" Name="installed" Type="integer" Value="1" KeyPath="yes"/>
      </Component>
    </DirectoryRef>
    <DirectoryRef Id="DesktopFolder">
      <Component Id="DesktopShortcut" Guid="c3d4e5f6-a7b8-9012-cdef-345678901234">
        <Shortcut Id="ApplicationDesktopShortcut" Name="OpenBC" Description="OpenBC File &amp; Folder Comparison" Target="[INSTALLFOLDER]openbc.exe" WorkingDirectory="INSTALLFOLDER"/>
        <RegistryValue Root="HKCU" Key="Software\OpenBC" Name="desktop_shortcut" Type="integer" Value="1" KeyPath="yes"/>
      </Component>
    </DirectoryRef>
    <DirectoryRef Id="TARGETDIR">
      <Component Id="OpenBCFolderSelectLeft" Guid="*">
        <RegistryKey Root="HKCR" Key="Directory\shell\OpenBC.SelectLeftFolder">
          <RegistryValue Name="MUIVerb" Type="string" Value="Select Left Folder" KeyPath="yes" />
          <RegistryKey Key="command"><RegistryValue Type="string" Value="&quot;[INSTALLFOLDER]openbc.exe&quot; --select-left-folder &quot;%1&quot;" /></RegistryKey>
        </RegistryKey>
      </Component>
      <Component Id="OpenBCFolderCompareToLeft" Guid="*">
        <RegistryKey Root="HKCR" Key="Directory\shell\OpenBC.CompareToLeftFolder">
          <RegistryValue Name="MUIVerb" Type="string" Value="Compare to Left Folder" KeyPath="yes" />
          <RegistryKey Key="command"><RegistryValue Type="string" Value="&quot;[INSTALLFOLDER]openbc.exe&quot; --compare-to-left-folder &quot;%1&quot;" /></RegistryKey>
        </RegistryKey>
      </Component>
      <Component Id="OpenBCFileSelectLeft" Guid="*">
        <RegistryKey Root="HKCR" Key="*\shell\OpenBC.SelectLeftFile">
          <RegistryValue Name="MUIVerb" Type="string" Value="Select Left File" KeyPath="yes" />
          <RegistryKey Key="command"><RegistryValue Type="string" Value="&quot;[INSTALLFOLDER]openbc.exe&quot; --select-left-file &quot;%1&quot;" /></RegistryKey>
        </RegistryKey>
      </Component>
      <Component Id="OpenBCFileCompareToLeft" Guid="*">
        <RegistryKey Root="HKCR" Key="*\shell\OpenBC.CompareToLeftFile">
          <RegistryValue Name="MUIVerb" Type="string" Value="Compare to Left File" KeyPath="yes" />
          <RegistryKey Key="command"><RegistryValue Type="string" Value="&quot;[INSTALLFOLDER]openbc.exe&quot; --compare-to-left-file &quot;%1&quot;" /></RegistryKey>
        </RegistryKey>
      </Component>
      <Component Id="OpenBCFolderCompareSelection" Guid="*">
        <RegistryKey Root="HKCR" Key="Directory\shell\OpenBC.Compare">
          <RegistryValue Name="MUIVerb" Type="string" Value="Compare" KeyPath="yes" />
          <RegistryValue Name="MultiSelectModel" Type="string" Value="Player" />
          <RegistryKey Key="command"><RegistryValue Type="string" Value="&quot;[INSTALLFOLDER]openbc.exe&quot; %*" /></RegistryKey>
        </RegistryKey>
      </Component>
      <Component Id="OpenBCFileCompareSelection" Guid="*">
        <RegistryKey Root="HKCR" Key="*\shell\OpenBC.Compare">
          <RegistryValue Name="MUIVerb" Type="string" Value="Compare" KeyPath="yes" />
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
  run candle -arch x64 -dStagingDir="$stage" -out "$wix/" "$wix/openbc.wxs" "$wix/files.wxs"
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
