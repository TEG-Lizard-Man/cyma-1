<!-- Draft text for the GitHub release. Tag: v1.1.3. Title: CYMA-1 v1.1.3.
     Attach when publishing: cyma-1-v1.1.3.fwsc (611,496 bytes),
     sha256 1c61abd4d1754a5c181cd60ed8895cf98b6d7970b1e42f3c84ddc480f0f413d3. -->

# CYMA-1 v1.1.3

A playable Chladni plate for the M-VAVE FM-1. This release fixes the first two bugs found on a real unit, and rounds
off the v1.1 series: no more crackle on big bowed chords, tighter MIDI timing, and Team EG's bronze bell at every boot.

![The boot, from power-on](https://raw.githubusercontent.com/TEG-Lizard-Man/cyma-1/v1.1.3/media/boot-intro.gif)

## What's new in v1.1.3

- **A silent boot when MASTER is at zero.** Before, the boot bell played a short burst at half volume before it
  settled to the knob's level. The platform now reads MASTER before the audio starts.
- **Every boot lands on the sand table.** The splash's sand now flows straight into the SAND view, instead of cutting to
  the last view you used. EDIT still reaches FIELD and MESH.

## New since v1.0

- **No more crackle on big bowed chords** (v1.1). The CPU governor bows at most two notes at once, and the hottest
  loops run from RAM. On the unit, held AETHER chords now barely move the LOAD meter.
- **Tighter MIDI timing** (v1.1.1). The Team EG platform no longer stalls the main loop once a second when no computer
  is attached. In the fm1-emu emulator, MIDI-to-sound p90 went from 39.9 ms to 7.4 ms.
- **Team EG's sonic logo** (v1.1.1). The engine plays a bronze bell, live, at every boot: E, G, then a flick as the
  sand locks into the credit. **BOOT SOUND on/off:** hold GLO for half a second on the GLOBAL page.
- **Time to read the credit and the title** (v1.1.1, v1.1.2). The TEAM EG & LIZARD_MAN credit holds about 4 s and the
  title over 3 s; any key skips.
- **Layer B with a tap** (v1.1). Tap a page's button again to latch its layer B, where THEME lives on GLOBAL.

Every change: [CHANGELOG.md](https://github.com/TEG-Lizard-Man/cyma-1/blob/v1.1.3/CHANGELOG.md).

## The package

| File | Identity | Size | sha256 |
|---|---|---|---|
| `cyma-1-v1.1.3.fwsc` | `FM-1_94010103` | 611,496 bytes | `1c61abd4d1754a5c181cd60ed8895cf98b6d7970b1e42f3c84ddc480f0f413d3` |

- **Check the hash before you install.** Nothing authenticates a `.fwsc`: the FM-1 installs whatever it is given.
- **It is reproducible.** A fresh clone of this tag that follows [BUILDING.md](https://github.com/TEG-Lizard-Man/cyma-1/blob/v1.1.3/BUILDING.md)
  builds the same bytes: the pinned toolchain image, `tools/get_platform.sh`, then
  `tools/build_device.sh instrument --no-ble --release 1.1.3`.
- It reads the settings and user presets of every earlier CYMA-1, so an update keeps your sounds.

## Install

Follow [notes/INSTALL.md](https://github.com/TEG-Lizard-Man/cyma-1/blob/v1.1.3/notes/INSTALL.md):
- **From stock V15:** take a full flash backup first, read twice, and keep three copies. Then install.
- **From an earlier CYMA-1:** back up your sounds and presets, check the package's sha256, run the installer, and
  answer `y` only for `package: FM-1_94010103` and your current CYMA-1 as the device. Once the percentage drops back
  for the second pass, don't touch the power switch until `done:`.
- **Never install V16**, and never use a tool that writes the boot area.

## Status

- v1.0, v1.1.1 and v1.1.2 have run on a real FM-1 since 2026-10-29.
- v1.1.3 on the FM-1: **confirmed on 2026-10-29.** The boot is silent with MASTER at zero, the bell plays in full
  with the knob up, and the boot lands on the sand table even after MESH was the last view. The user sounds were kept.
- **Known limits:**
  - The panel keys reach the sound in about 20–22 ms (p90 in the emulator), against about 8 ms for MIDI. That is the
    velocity sensing.
  - Bowed chords stay at two notes on the FM-1.
  - Emulator figures are not measurements on the FM-1.

## Credits

Produced by **Team EG** & **Lizard_Man**. Built on fm1-base by Charles Vestal and Leo Kuroshita's felucca-bsp
(MPL-2.0), with two Team EG patches. CYMA-1 is MPL-2.0. The names Team EG, Lizard_Man and CYMA-1, and their logos and
boot artwork, are not licensed under MPL-2.0.
