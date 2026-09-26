#!/bin/bash
# Remove the plug-in and give the Fast Track Pro back to the macOS driver.
set -euo pipefail
D=$(cd "$(dirname "$0")" && pwd)
make -C "$D/../tools" ftconfig >/dev/null
echo "Removing /Library/Audio/Plug-Ins/HAL/FastTrack24.driver. macOS will ask for your password."
sudo rm -rf /Library/Audio/Plug-Ins/HAL/FastTrack24.driver
echo "Restarting Core Audio (sound drops for a few seconds)..."
# The plug-in runs in its own helper process, which can outlive coreaudiod for a few
# seconds. Kill it first so it cannot re-claim the card after it is released below.
sudo sh -c 'pkill -9 -f "Core Audio Driver \\(FastTrack24.driver\\)"; killall coreaudiod'
sleep 2
"$D/../tools/ftconfig" release
# Installed by the .pkg installer, if that was used
sudo rm -rf "/Applications/Utilities/Uninstall Fast Track Pro 24-bit.app"
for id in com.github.nclr.fasttrack24 com.github.nclr.fasttrack24.uninstaller; do
  sudo pkgutil --forget "$id" >/dev/null 2>&1 || true
done
echo "Done. The Fast Track Pro is back as the normal 16-bit macOS device."
