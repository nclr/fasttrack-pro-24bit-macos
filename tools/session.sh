#!/bin/bash
# Listening + recording session: A/B button test on the outputs, then a recording through
# the macOS driver. Run from Terminal (it needs Terminal's Microphone permission).
D=$(cd "$(dirname "$0")" && pwd)
make -C "$D" >/dev/null
echo "=== Part 1: A/B button (headphone level LOW) ==="
echo "Outputs 1-2 play a LOW tone (440 Hz), outputs 3-4 a HIGH tone (660 Hz), for 15 s."
echo "Press the A/B button a few times and note which tone you hear in each position."
"$D/ftconfig" reset >/dev/null || exit 1
sleep 3
"$D/ftconfig" claim >/dev/null || exit 1
sleep 1
"$D/usbtone" 2 HLM 15 2 440 >/dev/null &
"$D/usbtone" 2 HLM 15 3 660 >/dev/null &
wait
echo
echo "=== Part 2: recording through the macOS driver ==="
"$D/ftconfig" release >/dev/null
sleep 4
echo "If a mic or instrument is plugged into input 1, make some sound for 5 s now."
"$D/carec" 5
