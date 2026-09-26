# Fast Track Pro 24-bit on macOS

24-bit playback for the **M-Audio Fast Track Pro** (USB `0763:2012`) on macOS, where
the built-in USB audio driver only produces loud static in 24-bit mode.

## The problem

The Fast Track Pro boots in USB configuration 1 (16-bit, class compliant). Its 24-bit
modes live in configuration 2. When macOS drives those modes, every 24-bit format
plays as harsh noise.

The cause is the sample byte order. On output interfaces 2 and 3, alt setting 2
(24-bit, 48 kHz, adaptive), each 3-byte sample is laid out as:

| byte 0 | byte 1 | byte 2 |
|--------|--------|--------|
| middle | high   | low    |

That is neither little-endian (what macOS sends) nor big-endian (what the Linux
`snd-usb-audio` quirk assumes for this card). Both of those play as noise. The layout
was determined by ear with a byte-exact USB streamer (`tools/usbtone`), see
[How it was found](#how-it-was-found).

The macOS driver cannot be told to use this layout: it rejects big-endian physical
formats, refuses integer virtual formats and does not allow mixing to be disabled.

## The solution: `ft24`

`ft24` bypasses the macOS driver and streams to the card itself:

```
apps -> BlackHole 2ch -> ft24 -> USB isochronous -> Fast Track Pro outputs 1-2 and 3-4
```

1. Re-selects USB configuration 2 with interface matching off, so the macOS driver
   (`usbaudiod`) does not attach. No sudo needed.
2. Opens output interfaces 2 and 3, selects alt 2 and sets 48 kHz on the endpoints.
3. Reads BlackHole 2ch, converts float to 24-bit in the card's byte order and sends
   it with isochronous writes. The endpoints are adaptive, so instead of resampling
   `ft24` sends 47–49 samples per 1 ms USB frame to follow BlackHole's clock. Samples
   pass through unaltered.
4. On Ctrl-C, returns the card to configuration 1 (the normal 16-bit macOS device)
   and restores the previous default output.

Both output pairs get the same stereo signal, so the headphone jack works with the
front-panel A/B button in either position.

### Requirements

- macOS with Xcode command line tools (`xcode-select --install`)
- [BlackHole 2ch](https://github.com/ExistentialAudio/BlackHole)
- Microphone permission for the terminal app that runs `ft24`. Without it macOS
  delivers silence from every input device, BlackHole included. `ft24` checks this at
  startup and asks for it.

### Build and run

```sh
make -C ft24
./ft24/ft24
```

`ft24` switches the system output to BlackHole 2ch. Play audio from any app. Every
5 s it prints a status line:

```
level 0.412  buffer  21 ms  clock   +62 ppm  underruns 0  usb errors 0
```

Press **Ctrl-C** to stop and give the card back to macOS. If `ft24` is killed without
cleaning up, unplug and replug the card, or run:

```sh
./ft24/ft24 --release
```

Options:

- `--a-only` streams only outputs 1–2 (headphone button A).
- `--release` returns the card to macOS and exits.

### Limitations

- The card's inputs and MIDI are unavailable while `ft24` runs.
- Fixed at 48 kHz. The 96 kHz mode (alt 3) is untested.
- About 50 ms of added latency.

## How it was found

The tools in `tools/` (build with `make -C tools`) drive the card directly:

| Tool | Purpose |
|------|---------|
| `probe` | Lists Core Audio devices, streams and formats |
| `desc` | Dumps the card's USB configuration descriptors |
| `ft-config2` | `claim`: configuration 2 with the macOS driver detached. `release`: give it back |
| `usbtone <alt> <layout> <seconds>` | Byte-exact tone on interface 2 |
| `usbdiag.sh` | 16-bit LE/BE controls vs 24-bit LE/BE |
| `bytediag.sh` | 8-bit tone in one byte position at a time |
| `permdiag.sh` | Full 24-bit tone in `MHL` vs `HML` layout |

Results with alt 2 at 48 kHz:

- 16-bit little-endian: clean tone; 16-bit big-endian: noise. The streamer is correct.
- 24-bit little-endian and big-endian: both noise.
- Tone in byte 0 only: audible. Byte 1 only: louder. Byte 2 only: silence.
  So byte 1 is most significant, byte 0 middle, byte 2 least.
- Full 24-bit tone as `[middle, high, low]`: clean.

## License

MIT, see [LICENSE](LICENSE).
