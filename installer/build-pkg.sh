#!/bin/bash
# Build double-click installer and uninstaller packages:
#   build/FastTrack24-<version>.pkg            installs the plug-in and an uninstaller app in
#                                              /Applications/Utilities, restarts Core Audio
#   build/Uninstall-FastTrack24-<version>.pkg  removes it and gives the card back to macOS
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
HERE="$ROOT/installer"
OUT="$ROOT/build"
ID=com.github.nclr.fasttrack24
VERSION=$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$ROOT/driver/Info.plist")

make -C "$ROOT/driver" >/dev/null
make -C "$ROOT/tools" ftconfig >/dev/null
W="$OUT/.work"
rm -rf "$W"
mkdir -p "$W"
trap 'rm -rf "$W"' EXIT

# ---- installer ----
# Two component packages, each installed straight into its folder, so the payloads hold
# only our bundles and never touch the ownership or permissions of system folders.
APP="Uninstall Fast Track Pro 24-bit.app"
mkdir -p "$W/driver-root" "$W/app-root" "$W/scripts" "$W/res" "$W/pkgs"
cp -R "$ROOT/driver/FastTrack24.driver" "$W/driver-root/"

# Uninstaller app for /Applications/Utilities: an AppleScript applet that runs the same
# script as the uninstaller package, with ftconfig next to it.
osacompile -o "$W/app-root/$APP" "$HERE/uninstall-app/uninstall.applescript"
cp "$HERE/scripts-uninstall/postinstall" "$W/app-root/$APP/Contents/Resources/uninstall.sh"
cp "$ROOT/tools/ftconfig" "$W/app-root/$APP/Contents/Resources/"
chmod 755 "$W/app-root/$APP/Contents/Resources/"{uninstall.sh,ftconfig}
/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier $ID.uninstaller" "$W/app-root/$APP/Contents/Info.plist" 2>/dev/null ||
  /usr/libexec/PlistBuddy -c "Add :CFBundleIdentifier string $ID.uninstaller" "$W/app-root/$APP/Contents/Info.plist"
codesign --force --deep --sign - "$W/app-root/$APP" 2>/dev/null

cp "$HERE/scripts-install/postinstall" "$W/scripts/"
chmod 755 "$W/scripts/postinstall"

# Component package: not relocatable (Installer would otherwise update any other copy of
# the bundle it finds, such as a build folder) and always replaced, even at the same version.
component() { # root install-location identifier output [scripts]
  pkgbuild --analyze --root "$1" "$W/component.plist" >/dev/null
  local n=0
  while /usr/libexec/PlistBuddy -c "Print :$n" "$W/component.plist" >/dev/null 2>&1; do
    /usr/libexec/PlistBuddy -c "Delete :$n:BundleIsRelocatable" "$W/component.plist" 2>/dev/null || true
    /usr/libexec/PlistBuddy -c "Add :$n:BundleIsRelocatable bool false" \
      -c "Set :$n:BundleIsVersionChecked false" "$W/component.plist"
    n=$((n + 1))
  done
  pkgbuild --quiet --root "$1" --component-plist "$W/component.plist" ${5:+--scripts "$5"} \
    --identifier "$3" --version "$VERSION" --install-location "$2" --ownership recommended "$4"
}
component "$W/app-root" /Applications/Utilities "$ID.uninstaller" "$W/pkgs/UninstallerApp.pkg"
# The plug-in goes last: its postinstall restarts Core Audio.
component "$W/driver-root" /Library/Audio/Plug-Ins/HAL "$ID" "$W/pkgs/FastTrack24.pkg" "$W/scripts"

cp "$HERE/resources/welcome.html" "$W/res/"
cp "$ROOT/LICENSE" "$W/res/LICENSE.txt"
cat > "$W/dist.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Fast Track Pro 24-bit</title>
    <welcome file="welcome.html"/>
    <license file="LICENSE.txt"/>
    <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_anywhere="false" enable_currentUserHome="false" enable_localSystem="true"/>
    <volume-check>
        <allowed-os-versions><os-version min="12.0"/></allowed-os-versions>
    </volume-check>
    <choices-outline>
        <line choice="plugin"/>
    </choices-outline>
    <choice id="plugin" visible="false">
        <pkg-ref id="$ID.uninstaller"/>
        <pkg-ref id="$ID"/>
    </choice>
    <pkg-ref id="$ID.uninstaller" version="$VERSION" onConclusion="none">UninstallerApp.pkg</pkg-ref>
    <pkg-ref id="$ID" version="$VERSION" onConclusion="none">FastTrack24.pkg</pkg-ref>
</installer-gui-script>
EOF
productbuild --quiet --distribution "$W/dist.xml" --resources "$W/res" --package-path "$W/pkgs" \
  "$OUT/FastTrack24-$VERSION.pkg"

# ---- uninstaller: no payload, the script does the work ----
mkdir -p "$W/uscripts" "$W/ures" "$W/upkgs"
cp "$HERE/scripts-uninstall/postinstall" "$W/uscripts/"
cp "$ROOT/tools/ftconfig" "$W/uscripts/"
chmod 755 "$W/uscripts/postinstall" "$W/uscripts/ftconfig"
pkgbuild --quiet --nopayload --scripts "$W/uscripts" --identifier "$ID.uninstall" --version "$VERSION" \
  "$W/upkgs/Uninstall.pkg"
cp "$HERE/resources/uninstall-welcome.html" "$W/ures/welcome.html"
cat > "$W/udist.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Uninstall Fast Track Pro 24-bit</title>
    <welcome file="welcome.html"/>
    <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_anywhere="false" enable_currentUserHome="false" enable_localSystem="true"/>
    <choices-outline>
        <line choice="uninstall"/>
    </choices-outline>
    <choice id="uninstall" visible="false">
        <pkg-ref id="$ID.uninstall"/>
    </choice>
    <pkg-ref id="$ID.uninstall" version="$VERSION" onConclusion="none">Uninstall.pkg</pkg-ref>
</installer-gui-script>
EOF
productbuild --quiet --distribution "$W/udist.xml" --resources "$W/ures" --package-path "$W/upkgs" \
  "$OUT/Uninstall-FastTrack24-$VERSION.pkg"

echo "Built:"
ls -1 "$OUT"/*"$VERSION".pkg
