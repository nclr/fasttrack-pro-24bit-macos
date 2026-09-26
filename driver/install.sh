#!/bin/bash
# Build and install the Fast Track Pro 24-bit Core Audio plug-in.
set -euo pipefail
D=$(cd "$(dirname "$0")" && pwd)
make -C "$D"
echo "Installing to /Library/Audio/Plug-Ins/HAL. macOS will ask for your password."
# The plug-in runs in its own helper process, which can outlive coreaudiod for a few
# seconds. Remember the old one so it can be stopped without touching the new one.
old_helper=$(pgrep -f "Core Audio Driver \\(FastTrack24.driver\\)" || true)
sudo rm -rf /Library/Audio/Plug-Ins/HAL/FastTrack24.driver
sudo cp -R "$D/FastTrack24.driver" /Library/Audio/Plug-Ins/HAL/
sudo chown -R root:wheel /Library/Audio/Plug-Ins/HAL/FastTrack24.driver
echo "Restarting Core Audio (sound drops for a few seconds)..."
# Kill the old helper first, in the same command, so it cannot re-claim the card.
sudo sh -c "${old_helper:+kill -9 $old_helper 2>/dev/null;} killall coreaudiod"
echo "Done. Select \"Fast Track Pro 24-bit\" in System Settings > Sound > Output."
