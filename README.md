# CYMA-1

A playable Chladni plate: original firmware for the M-VAVE FM-1 pocket synth.

Each key strikes, bows or rains on a virtual metal plate. The sand on the 240×240 screen settles into the nodal figure you hear, because the picture and the sound come from the same modal simulation:

- key pressure bows harder;
- SELECT morphs the plate from a circle through hexagon, star, guitar and bar;
- SEQ turns the keybed itself into the plate.

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
| `tools/build_device.sh` | Device build in the pinned JieLi toolchain container (built, never flashed) |

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
- **Not yet run on a real FM-1.** No firmware package (`.fwsc`) is included in this repo.
- Before installing any custom firmware:
  - back up the unit's full flash;
  - keep the official V15 image for recovery;
  - have the RP2040 "Transporter" recovery dongle ready.

## Credits

- Built on [fm1-base](https://github.com/charlesvestal/fm1-base) by Charles Vestal, which builds on Leo Kuroshita's felucca-bsp (MPL-2.0).
- Fonts: Barlow and Terminus (SIL OFL 1.1).
- CYMA-1 is MPL-2.0.
