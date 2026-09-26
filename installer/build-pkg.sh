#!/bin/bash
# Build double-click installer and uninstaller packages:
#   build/FastTrack24-<version>.pkg            installs the plug-in, restarts Core Audio
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
mkdir -p "$W/root/Library/Audio/Plug-Ins/HAL" "$W/scripts" "$W/res" "$W/pkgs"
cp -R "$ROOT/driver/FastTrack24.driver" "$W/root/Library/Audio/Plug-Ins/HAL/"
cp "$HERE/scripts-install/postinstall" "$W/scripts/"
chmod 755 "$W/scripts/postinstall"
# Not relocatable: otherwise Installer would update any other copy of the bundle it finds
# (such as a build folder) instead of installing into /Library/Audio/Plug-Ins/HAL.
pkgbuild --analyze --root "$W/root" "$W/component.plist" >/dev/null
/usr/libexec/PlistBuddy -c 'Add :0:BundleIsRelocatable bool false' \
  -c 'Set :0:BundleIsVersionChecked false' "$W/component.plist" # always replace, even the same version
pkgbuild --quiet --root "$W/root" --component-plist "$W/component.plist" --scripts "$W/scripts" \
  --identifier "$ID" --version "$VERSION" --install-location / --ownership recommended "$W/pkgs/FastTrack24.pkg"

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
        <pkg-ref id="$ID"/>
    </choice>
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
