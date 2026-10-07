#!/usr/bin/env bash
set -e

# Usage: ./msi.sh [-c]    (-c = clean rebuild of the CMake/cargo build dir)
CLEAN=false
while getopts "c" opt; do
  case $opt in
    c) CLEAN=true ;;
    \?) echo "Usage: $0 [-c]"; exit 1 ;;
  esac
done

# 1. Temporary directories in a writable MSYS2 location
export TMPDIR="/tmp"
export TEMP="/tmp"
export TMP="/tmp"
mkdir -p /tmp

# 2. Environment
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:/c/msys64/opt/wix:$PATH"
export OPENSSL_DIR="/c/msys64/ucrt64"

# Cache compiled crates across builds if sccache is installed
if command -v sccache >/dev/null 2>&1; then
  export RUSTC_WRAPPER=sccache
fi

# Same build dir as win-build.sh so cargo's target/ is shared and reused
BUILD_DIR="build"
STAGING_DIR="$BUILD_DIR/staging"
WIX_DIR="$BUILD_DIR/wix"
OUTPUT_MSI="$BUILD_DIR/openbc-installer.msi"

# Only wipe the build dir when explicitly asked
if [ "$CLEAN" = true ]; then
  echo "Cleaning build directory..."
  rm -rf "$BUILD_DIR"
fi

echo "=== 1. Building openbc target ==="
# Configure only when needed; later runs go straight to the incremental build
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  cmake -B "$BUILD_DIR" -G "Ninja" -DCMAKE_BUILD_TYPE=Release
fi
cmake --build "$BUILD_DIR" --target openbc

# Clean and set up staging & WiX work directories
rm -rf "$STAGING_DIR" "$WIX_DIR"
mkdir -p "$STAGING_DIR" "$WIX_DIR"
cp "$BUILD_DIR/openbc.exe" "$STAGING_DIR/"

echo "=== 2. Collecting Qt Dependencies via windeployqt ==="
windeployqt --no-compiler-runtime --no-translations "$STAGING_DIR/openbc.exe"

echo "=== 3. Copying MinGW/third-party DLLs (via ldd) ==="
# Repeat until no new DLLs appear, since copied DLLs have dependencies of their own
while :; do
  added=0
  while IFS= read -r dll; do
    [ -z "$dll" ] && continue
    name=$(basename "$dll")
    if [ ! -f "$STAGING_DIR/$name" ]; then
      cp "$dll" "$STAGING_DIR/"
      echo "   + $name"
      added=1
    fi
  done < <(find "$STAGING_DIR" -type f \( -name '*.dll' -o -name '*.exe' \) -print0 \
           | xargs -0 ldd 2>/dev/null \
           | grep -i '/ucrt64/' | awk '{print $3}' | sort -u)
  [ "$added" -eq 0 ] && break
done

echo "=== 4. Generating WiX Installer XML ==="
cat << 'EOF' > "$WIX_DIR/openbc.wxs"
<?xml version="1.0" encoding="UTF-8"?>
<Wix xmlns="http://schemas.microsoft.com/wix/2006/wi">
  <Product Id="*" Name="OpenBC" Language="1033" Version="1.0.0.0" Manufacturer="OpenBC Team" UpgradeCode="a1b2c3d4-e5f6-7890-abcd-ef1234567890">
    <Package InstallerVersion="200" Compressed="yes" InstallScope="perMachine" Platform="x64" />
    <MediaTemplate EmbedCab="yes" />

    <!-- Enables seamless in-place updates when version is incremented -->
    <MajorUpgrade DowngradeErrorMessage="A newer version of [ProductName] is already installed." />

    <Directory Id="TARGETDIR" Name="SourceDir">
      <!-- Installation Directory: C:\Program Files\OpenBC -->
      <Directory Id="ProgramFiles64Folder">
        <Directory Id="INSTALLFOLDER" Name="OpenBC" />
      </Directory>

      <!-- Start Menu Directory -->
      <Directory Id="ProgramMenuFolder">
        <Directory Id="ApplicationProgramsFolder" Name="OpenBC"/>
      </Directory>

      <!-- Desktop Folder Target -->
      <Directory Id="DesktopFolder" Name="Desktop" />
    </Directory>

    <!-- Start Menu Shortcut Component -->
    <DirectoryRef Id="ApplicationProgramsFolder">
      <Component Id="ApplicationShortcut" Guid="b2c3d4e5-f6a7-8901-bcde-f23456789012">
        <Shortcut Id="ApplicationStartMenuShortcut"
                  Name="OpenBC"
                  Description="OpenBC File &amp; Folder Comparison"
                  Target="[INSTALLFOLDER]openbc.exe"
                  WorkingDirectory="INSTALLFOLDER"/>
        <RemoveFolder Id="CleanUpShortCut" On="uninstall"/>
        <RegistryValue Root="HKCU" Key="Software\OpenBC" Name="installed" Type="integer" Value="1" KeyPath="yes"/>
      </Component>
    </DirectoryRef>

    <!-- Desktop Shortcut Component -->
    <DirectoryRef Id="DesktopFolder">
      <Component Id="DesktopShortcut" Guid="c3d4e5f6-a7b8-9012-cdef-345678901234">
        <Shortcut Id="ApplicationDesktopShortcut"
                  Name="OpenBC"
                  Description="OpenBC File &amp; Folder Comparison"
                  Target="[INSTALLFOLDER]openbc.exe"
                  WorkingDirectory="INSTALLFOLDER"/>
        <RegistryValue Root="HKCU" Key="Software\OpenBC" Name="desktop_shortcut" Type="integer" Value="1" KeyPath="yes"/>
      </Component>
    </DirectoryRef>

    <!-- Feature Tree including App Files and Shortcuts -->
    <Feature Id="MainFeature" Title="OpenBC Core" Level="1">
      <ComponentGroupRef Id="AppFiles" />
      <ComponentRef Id="ApplicationShortcut" />
      <ComponentRef Id="DesktopShortcut" />
    </Feature>
  </Product>
</Wix>
EOF

echo "=== 5. Harvesting staging directory & building MSI ==="
heat dir "$STAGING_DIR" -cg AppFiles -dr INSTALLFOLDER \
     -scom -sreg -sfrag -srd -gg \
     -var var.StagingDir \
     -out "$WIX_DIR/files.wxs"

candle -arch x64 -dStagingDir="$STAGING_DIR" \
       -out "$WIX_DIR/" \
       "$WIX_DIR/openbc.wxs" "$WIX_DIR/files.wxs"

light -ext WixUIExtension \
      -b "$STAGING_DIR" \
      "$WIX_DIR/openbc.wixobj" "$WIX_DIR/files.wixobj" \
      -o "$OUTPUT_MSI"

echo "=== Build Complete: $OUTPUT_MSI ==="