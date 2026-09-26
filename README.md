# Fast Track Pro 24-bit on macOS

24-bit playback for the **M-Audio Fast Track Pro** (USB `0763:2012`) on macOS, where
the built-in USB audio driver only produces loud static in 24-bit mode.

It installs a Core Audio plug-in that adds a **"Fast Track Pro 24-bit"** output device.
Select it in System Settings like any other sound output.

## Building and installing

Requirements: a Mac with macOS 12 or later (Apple silicon or Intel) and an administrator
password.

### Installer package (double-click)

1. Get `FastTrack24-<version>.pkg`: download it from the repository's Releases page, or
   build it yourself (see below).
2. Double-click it and follow the steps. Sound stops for a few seconds at the end while
   Core Audio restarts.
3. Open **System Settings → Sound → Output** and select **Fast Track Pro 24-bit**.
   The regular "FastTrack Pro" entry disappears while the plug-in owns the card.

The package also installs **Uninstall Fast Track Pro 24-bit** in Applications → Utilities
(see [Uninstalling](#uninstalling)).

The package is not signed with an Apple Developer ID, so macOS blocks it the first time if
it was **downloaded**: it says it cannot verify the developer. Click **Done**, then open
**System Settings → Privacy & Security**, scroll down and click **Open Anyway** next to the
message about `FastTrack24-<version>.pkg`. A package you built on the same Mac opens directly.

### Building from source

Install the Xcode command line tools once with `xcode-select --install`, then:

```sh
git clone https://github.com/nclr/fasttrack-pro-24bit-macos.git
cd fasttrack-pro-24bit-macos
make pkg        # builds build/FastTrack24-<version>.pkg and build/Uninstall-FastTrack24-<version>.pkg
```

Double-click the package in `build/`, or install straight from the terminal instead:

```sh
make            # builds driver/FastTrack24.driver and tools/
make install    # copies the plug-in and restarts Core Audio (asks for your password)
```

The plug-in is ad-hoc signed during the build; no Apple developer account is needed.

Optional checks:

```sh
make test       # loads the plug-in outside coreaudiod and streams silence (do this before installing)
FT_TONE=1 make -C driver test   # same, with 440 Hz on outputs 1-2 and 660 Hz on 3-4
/usr/bin/log stream --predicate 'subsystem == "com.github.nclr.fasttrack24"'   # live plug-in log
```

The plug-in logs a health line 30 s after it starts streaming and afterwards only when
something goes wrong. Healthy playback looks like
`last 30 s: 0 retries, 0 gaps, 0 underruns, 0 errors`.

## The problem

The Fast Track Pro boots in USB configuration 1 (16-bit, class compliant). Its 24-bit
modes live in configuration 2. When macOS drives those modes, every 24-bit format
plays as harsh noise.

The cause is the sample byte order. On output interfaces 2 and 3, alt setting 2
(24-bit, 44.1/48 kHz, adaptive), a freshly powered-up card takes each 3-byte sample as:

| byte 0 | byte 1 | byte 2 |
|--------|--------|--------|
| high   | low    | middle |

That is neither little-endian (what macOS sends) nor big-endian (what the Linux
`snd-usb-audio` quirk assumes for this card), and both of those play as noise. The layout
was determined by ear with a byte-exact USB streamer, see [How it was found](#how-it-was-found).

The card does not realign to sample boundaries. It behaves like little-endian read from
the wrong starting byte, and a transfer that is not a whole number of samples shifts that
starting byte for good: the order that plays clean changes and stays changed until the
card is powered up or re-enumerated again. A single 1-byte transfer is enough.

The macOS driver cannot be told to use this layout: it rejects big-endian physical
formats, refuses integer virtual formats and does not allow mixing to be disabled.

## The solution: a Core Audio plug-in

`driver/` builds `FastTrack24.driver`, an Audio Server plug-in that runs inside
`coreaudiod` and drives the card directly:

```
apps -> "Fast Track Pro 24-bit" (plug-in) -> USB isochronous -> Fast Track Pro outputs 1-2 and 3-4
```

The card has two independent stereo outputs, and the device shows both:

| Channels | Stream | USB interface | Where it comes out |
|----------|--------|---------------|--------------------|
| 1–2 | Outputs 1-2 (headphones A) | 2 | main outputs 1–2; headphones with the A/B button **out** |
| 3–4 | Outputs 3-4 (headphones B, S/PDIF) | 3 | outputs 3–4 and S/PDIF out; headphones with the A/B button **in** |

Ordinary apps (Music, browsers, system sounds) play on channels 1–2. A DAW can send a
separate mix, such as a cue or monitor mix, to channels 3–4. The A/B button chooses which
of the two the headphones hear.

1. When the card is plugged in (or the plug-in starts, or streaming fails), the plug-in
   re-enumerates it, which puts the byte alignment back to the power-up state whatever
   was sent to the card before. It then re-selects USB configuration 2 with interface
   matching off, so the macOS driver (`usbaudiod`) does not attach.
2. It opens output interfaces 2 and 3, selects alt 2 and sets the endpoint sample rate.
3. Each output stream is written into a ring indexed by sample time, so the two pairs stay
   sample-aligned. Audio is converted from float to 24-bit in the card's byte order and
   streamed with isochronous writes. The endpoints are adaptive, so instead of
   resampling the plug-in sends 44–45 or 47–49 samples per 1 ms USB frame to follow the
   device clock. At full volume samples pass through unaltered.
4. When the card is unplugged, the device disappears; when it comes back, it is
   claimed again.

Features:

- 24-bit at **44.1 kHz and 48 kHz** (set in Audio MIDI Setup or by the playing app)
- Volume and mute controls, so the keyboard volume keys work. Full volume is bit-perfect.
- Reports its latency (about 50 ms) to Core Audio, so video stays in sync
- Four outputs as two streams, with channel names ("Output 1" … "Output 4") for DAWs
- No virtual loopback device (such as BlackHole), no background app, no microphone permission

Limitations:

- Playback only. The card's inputs are not supported: on the card this was developed with,
  they send corrupted data (near full-scale values made of one repeated byte) with nothing
  plugged in, even through Apple's own driver in the standard 16-bit mode, so there was
  nothing valid to build on. `tools/usbrec` and `tools/carec` reproduce the test.
- MIDI is unavailable while the plug-in owns the card
- The 88.2/96 kHz mode (alt 3) is untested and not offered
- Audio MIDI Setup shows the stream as 32-bit float: that is the mix format Core Audio
  hands to the plug-in; the card receives 24-bit integers

## How it was found

The tools in `tools/` drive the card directly. Uninstall the plug-in before using them:
both would claim the card.

| Tool | Purpose |
|------|---------|
| `probe` | Lists Core Audio devices, streams and formats |
| `desc` | Dumps the card's USB configuration descriptors |
| `ftconfig status\|claim\|claim1\|release\|reset` | Show the USB configuration, detach the macOS driver (configuration 2, or 1 with `claim1`), give the card back (configuration 1), or re-enumerate it (`reset`: back to the power-up byte alignment) |
| `usbtone <alt> <layout> <seconds> [interface] [Hz] [pad]` | Byte-exact tone on interface 2 (outputs 1–2) or 3 (outputs 3–4). `pad` sends that many bytes as a packet of their own first; `FT_WAV=file.wav` plays a mono 16-bit 48 kHz recording (for example from `say -o f.wav --data-format=LEI16@48000`) instead of the tone |
| `usbrec <interface> <seconds> [file] [alt]` | Raw capture from input interface 4 or 5, with a byte-layout smoothness check |
| `carec <seconds>` | Records the inputs through the macOS driver and reports level and repeated-byte samples |
| `session.sh` | A/B button test (440 Hz on outputs 1–2, 660 Hz on 3–4), then a `carec` recording |
| `usbdiag.sh` | 16-bit LE/BE controls vs 24-bit LE/BE |
| `bytediag.sh` | 8-bit tone in one byte position at a time |
| `permdiag.sh` | Resets the card, then plays a full 24-bit tone in each byte rotation (`HLM`, `MHL`, `LMH`) |

Results with alt 2 at 48 kHz (the first ones on a card in an unknown state, see below):

- 16-bit little-endian: clean tone; 16-bit big-endian: noise. The streamer is correct.
- 24-bit little-endian and big-endian: both noise.
- Tone in byte 0 only: audible. Byte 1 only: louder. Byte 2 only: silence.
  So byte 1 is most significant, byte 0 middle, byte 2 least.
- Full 24-bit tone as `[middle, high, low]`: clean.
- After the card was unplugged and plugged in again, `[middle, high, low]` played as noise
  and `[high, low, middle]` was clean, the same order read one byte later. That stayed
  the same across configuration switches and stream restarts, and after every power-up
  or re-enumeration (`ftconfig reset`). One 1-byte packet (`usbtone 2 HLM 4 2 440 1`)
  turned it into noise for that and later streams until the next re-enumeration. So the
  earlier sessions ran on a card that stray bytes had already shifted, probably from the
  macOS 16-bit driver, whose 44.1 kHz packets (176 bytes) are not a multiple of 3.
- 440 Hz on outputs 1–2 and 660 Hz on outputs 3–4 at the same time: the low tone with the
  A/B button out, the high tone with it in.

## Repository layout

```
driver/     Core Audio plug-in (FastTrack24.c), install/uninstall scripts, test host
installer/  .pkg installer and uninstaller, and the uninstaller app
tools/      diagnostics used to find the byte layout
```

## Uninstalling

Open **Applications → Utilities → Uninstall Fast Track Pro 24-bit**, confirm and enter your
password. It is installed by the package; it removes itself when done.

Alternatively double-click `Uninstall-FastTrack24-<version>.pkg` (built next to the installer
by `make pkg`, or from the Releases page), or from a clone of the repository run:

```sh
make uninstall
```

All three run the same script (`installer/scripts-uninstall/postinstall`), which undoes
everything the installer and the plug-in did:

- removes the plug-in from `/Library/Audio/Plug-Ins/HAL`, the uninstaller app and the
  package receipts, and stops the plug-in's helper process
- removes what Core Audio saved for the plug-in (volume, mute, sample rate) and for the
  "Fast Track Pro 24-bit" device, including its place in the preferred output devices,
  then restarts Core Audio
- re-enumerates the card, as if it were unplugged and plugged in again. It comes back in
  USB configuration 1 as the normal 16-bit macOS device, with its byte alignment reset:
  only switching the configuration back would leave the alignment the plug-in left, and
  the macOS driver would play loud noise.

If the card is not plugged in while uninstalling, there is nothing to reset: it starts
fresh the next time it is plugged in.

## License

MIT, see [LICENSE](LICENSE).
