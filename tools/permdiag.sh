#!/bin/bash
# Full 24-bit tone in two byte layouts. Keep the headphone level LOW.
D=$(cd "$(dirname "$0")" && pwd)
"$D/ftconfig" claim >/dev/null || { echo "could not claim the card"; exit 1; }
sleep 1
phase() { echo; echo ">>> PHASE $1"; "$D/usbtone" 2 "$2" 4 2>&1 | grep -iE 'error|0xe|not found'; sleep 1.5; }
phase "1: 24-bit, layout [mid, high, low]" MHL
phase "2: 24-bit, layout [high, mid, low] (plain big-endian, expected noise)" HML
echo; echo "done. Give the card back to macOS with: $D/ftconfig release"
