#!/bin/bash
# Which byte of a 24-bit sample does the card treat as most significant?
# Keep the headphone level LOW. Expect ONE phase to be a clear (slightly buzzy) tone.
D=$(cd "$(dirname "$0")" && pwd)
"$D/ftconfig" claim >/dev/null || { echo "could not claim the card"; exit 1; }
sleep 1
phase() { echo; echo ">>> PHASE $1"; "$D/usbtone" 2 "$2" 4 2>&1 | grep -iE 'error|0xe|not found'; sleep 1.5; }
phase "1: tone in byte 0 only" b0
phase "2: tone in byte 1 only" b1
phase "3: tone in byte 2 only" b2
echo; echo "done. Give the card back to macOS with: $D/ftconfig release"
