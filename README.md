# CYMA-1

A playable Chladni plate: original firmware for the M-VAVE FM-1 pocket synth.

Each key strikes, bows or rains on a virtual metal plate. The sand on the 240×240 screen settles into the nodal figure you hear, because the picture and the sound come from the same modal simulation:

- key pressure bows harder;
- SELECT morphs the plate from a circle through hexagon, star, guitar and bar;
- SEQ turns the keybed itself into the plate.

## Screens

These are frames of the real 240×240 device UI, rendered by the same C code that runs on the FM-1 (the browser build and the hardware draw identical pixels).

| | | |
|---|---|---|
| ![Handpan: steel plate, sand on the nodal rings](media/screen-handpan.png) | ![Singing bowl, leaning in](media/screen-singing-bowl.png) | ![Steel rain](media/screen-steel-rain.png) |
| **02 HANDPAN**: sand on the nodal rings of a steel plate | **01 SINGING BOWL**: pressing harder bows harder | **04 STEEL RAIN**: the rain exciter |

| Morphing the plate from circle to star | Steel rain swells |
|---|---|
| ![Circle-to-star morph](media/morph-circle-to-star.gif) | ![Steel rain swells](media/steel-rain.gif) |

[Every plate shape and its first eight vibration patterns](media/plates-and-modes.png) is the reference sheet the engine is checked against: the sand lands on the white nodal lines.

## What's a Chladni plate?

In 1787 the physicist and musician Ernst Chladni scattered sand on a thin brass plate and drew a violin bow along its edge. The plate rang, and the sand jumped into a sharp geometric figure.

A vibrating plate doesn't move everywhere at once. Some regions swing hard while certain lines stay perfectly still (the *nodal lines*). The sand is thrown off the moving parts and piles up on the still lines, so the figure is the shape of the sound. Every note draws its own figure.

Why it's worth a look:

- It makes sound visible.
- Napoleon saw it in 1808 and backed a prize for explaining it. Sophie Germain won it in 1816 with work that helped found the theory of how solid materials vibrate.
- Violin and guitar makers still read Chladni figures on glitter-dusted top plates to carve them until they ring right.
- It's the same experiment as today's "cymatics" videos.

CYMA-1 makes it playable:

- The picture and the sound come from the same simulation of the plate's vibration patterns.
- Morphing the plate changes the figure and the overtones together.
- Pressing harder bows harder.

The plates are Chladni-like rather than laboratory-exact: they are modelled as stiffened membranes.

## Play it

The browser version is in `docs/`. It is the same C code as the FM-1 firmware, compiled to WebAssembly.

To play it locally in Chrome, where Web MIDI works too:

```sh
cd docs && python3 -m http.server 8137
# then open http://localhost:8137
```

Or use `python3 instrument/tools/cyma_local.py` after a local web build (see `instrument/BUILDING.md`).

To play:

- **Keys:** the letter keys in the Ableton layout (A = C4).
- **Lean in:** hold Space.
- **Shape:** ← →.
- **Exciter:** [ ].
- **Presets:** ↑ ↓.
- **Big screen:** B.
- **Sounds** (below the instrument): save, restore, export and share your sounds.
- **Capture:** saves the last 60 s as 24-bit WAV + MIDI.

## Layout

| Path | What |
|---|---|
| `instrument/` | The CYMA-1 app (MPL-2.0): C engine (`src/`), UI (`ui/`), browser page (`web/emu/`), tests and tools |
| `docs/` | The packaged browser build (static files) |
| `reference/fm1-base` | [fm1-base](https://github.com/charlesvestal/fm1-base) (MPL-2.0), as a git submodule: the platform layer, simulator and build tools |
| `tools/build_device.sh` | Device build in the pinned JieLi toolchain container |
| `media/` | Screenshots and animations of the device UI |
| `notes/` | The install runbook, the FM-1 hardware atlas and the next release's spec |

## Build

```sh
git submodule update --init --recursive
export DYLD_LIBRARY_PATH=/opt/homebrew/lib     # Pillow + libraqm on macOS, for the fonts
python3 reference/fm1-base/tools/build.py instrument --host   # native simulator
python3 reference/fm1-base/tools/build.py instrument --web    # browser build
python3 instrument/tools/package_web.py                       # -> instrument/dist/web
instrument/tools/run_all.sh                                   # every test and gate
```

Full details are in `instrument/BUILDING.md`.

## Status and safety

- Verified in the native simulator, the browser build, and the JieLi chip emulator.
- **Runs on a real FM-1** (first install 2026-10-29, tag `v1.0-hw`): the screen, all 27 keys, pressure, the CPU meter, saving and the USB Sounds and backup tools all work. The installed package was built from this tag with fm1-base `086cd0c` (sha256 `1a3b809f…7fc5`).
- No firmware package (`.fwsc`) is included in this repo.
- Before installing any custom firmware:
  - back up the unit's full flash;
  - keep the official V15 image for recovery;
  - have the RP2040 "Transporter" recovery dongle ready.

## Milestones

| Date | Milestone |
|---|---|
| 2026-10-24 | Built in one night by agent teams. Research covered about 45 community repos and the chip's own documents. A design contest of 5 concepts and 3 judges picked the Chladni plate, 8 agents built the modules in parallel, and an adversarial review confirmed 115 findings (98 fixed, 13 partly fixed) before release. |
| 2026-10-24 | The browser version plays the same C code compiled to WebAssembly. |
| 2026-10-24 | **Sounds**: save, restore, export and share sounds as short codes, and send them to the FM-1 over USB-MIDI. A hardening round followed. |
| 2026-10-28 | **Hardware prep**: a safety re-check of the firmware, a byte-for-byte reproducible rebuild, the Mac recovery tools vetted, and a step-by-step install runbook ([notes/CYMA-1-install-runbook.md](notes/CYMA-1-install-runbook.md)). |
| 2026-10-28 | **Hardware atlas**: the FM-1 mapped down to the chip, from silicon blocks and all 48 pins to the board, the stock firmware and its protocols. A skeptic agent checked every claim, and 42 earlier community claims were corrected ([notes/FM-1-hardware-atlas.md](notes/FM-1-hardware-atlas.md)). |
| 2026-10-28 | **Next release designed**: VOICE, the talking plate (your voice over USB shapes the plate's resonances), and export as an Ableton instrument ([notes/CYMA-1-voice-spec.md](notes/CYMA-1-voice-spec.md)). |
| 2026-10-29 | **First install on a real FM-1.** A full flash backup was taken from macOS through the chip's ROM mode, with two identical reads; as far as we know, the first report of that route working on an M4 Mac with macOS 26. The install took about 2 minutes. On hardware, the colour screen, the smooth frame rate, all 27 keys, pressure, the CPU meter, saving, and the USB Sounds and backup tools all work. Tag `v1.0-hw`. |

## Credits

- Built on [fm1-base](https://github.com/charlesvestal/fm1-base) by Charles Vestal, which builds on Leo Kuroshita's felucca-bsp (MPL-2.0).
- Fonts: Barlow and Terminus (SIL OFL 1.1).
- CYMA-1 is MPL-2.0.
