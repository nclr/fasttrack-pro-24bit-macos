#!/bin/bash
# Full 24-bit tone in each byte rotation, from the power-up alignment. Keep the headphone level LOW.
D=$(cd "$(dirname "$0")" && pwd)
"$D/ftconfig" reset >/dev/null || { echo "could not reset the card"; exit 1; }
sleep 3
"$D/ftconfig" claim >/dev/null || { echo "could not claim the card"; exit 1; }
sleep 1
phase() { echo; echo ">>> PHASE $1"; "$D/usbtone" 2 "$2" 4 2>&1 | grep -iE 'error|0xe|not found'; sleep 1.5; }
phase "1: 24-bit, layout [high, low, mid] (expected clean)" HLM
phase "2: 24-bit, layout [mid, high, low] (expected noise)" MHL
phase "3: 24-bit, layout [low, mid, high] (plain little-endian, expected near silence)" LMH
echo; echo "done. Give the card back to macOS with: $D/ftconfig release"
