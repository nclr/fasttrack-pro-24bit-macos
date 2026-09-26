#!/bin/bash
# Byte-exact listening test through our own USB streamer (not the macOS driver).
# Keep the headphone level LOW. Button A, mix knob toward playback.
D=$(cd "$(dirname "$0")" && pwd)
"$D/ftconfig" claim >/dev/null || { echo "could not claim the card"; exit 1; }
sleep 1
phase() { echo; echo ">>> PHASE $1"; "$D/usbtone" "$2" "$3" 4 2>&1 | grep -iE 'error|0xe|not found'; sleep 1.5; }
phase "1: 16-bit little-endian (control: should be a clean tone)" 1 le
phase "2: 16-bit big-endian   (control: should be noise)"         1 be
phase "3: 24-bit big-endian"                                       2 be
phase "4: 24-bit little-endian"                                    2 le
echo; echo "done. Card is still detached from macOS; run: $D/ftconfig release"
