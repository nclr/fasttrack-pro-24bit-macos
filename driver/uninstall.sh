#!/bin/bash
# Remove the plug-in and give the Fast Track Pro back to the macOS driver, with the same
# script the uninstaller package and app run.
set -euo pipefail
D=$(cd "$(dirname "$0")" && pwd)
make -C "$D/../tools" ftconfig >/dev/null
echo "Removing the Fast Track Pro 24-bit plug-in. macOS will ask for your password."
echo "Sound stops for a few seconds while Core Audio restarts and the card is reset."
sudo env FTCONFIG="$D/../tools/ftconfig" sh "$D/../installer/scripts-uninstall/postinstall"
echo "Done. The Fast Track Pro is back as the normal 16-bit macOS device."
