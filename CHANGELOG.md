# Changelog

Every CYMA-1 release, newest first. Each one has its own package identity, so `fm1_install.py --info` tells them
apart. Device timings come from the fm1-emu chip emulator at the FM-1's 205 MHz unless a line says otherwise. They are
not measurements on an FM-1.

| Version | Identity | Package sha256 | Platform | On the FM-1 |
|---|---|---|---|---|
| [v1.1.3](#v113-2026-10-29) | `FM-1_94010103` | `1c61abd4d1754a5c181cd60ed8895cf98b6d7970b1e42f3c84ddc480f0f413d3` | Team EG platform, with the MASTER seed | Installed and confirmed 2026-10-29 |
| [v1.1.2](#v112-2026-10-29) | `FM-1_94010102` | `1554752c5d5694b9c23df3f67c70c9c3512e37f1e66b78daa4a1e7b1b7ef0e54` | Team EG platform | Installed 2026-10-29 |
| [v1.1.1](#v111-2026-10-29) | `FM-1_94010101` | `a84d55e14730824d6c4301ba6bdfb337cec49491dbee529b070294684f07d3fe` | Team EG platform | Installed 2026-10-29 |
| [v1.1](#v11-2026-10-29) | `FM-1_94010100` | not released on its own | upstream fm1-base `086cd0c` | Shipped as part of v1.1.1 |
| [v1.0](#v10-2026-10-24) | `FM-1_9400` | `1a3b809f5469b303eb2791eb225c17afb1121fbcf079e4ba20d60b94e60c7fc5` | upstream fm1-base `086cd0c` | First install 2026-10-29 (tag `v1.0-hw`) |

Every version reads the settings and user presets of the ones before it, so an update keeps your sounds.

## v1.1.3 (2026-10-29)

Two fixes for the first bugs found in the field, on a unit running v1.1.2.

- **No boot sound with MASTER at zero.**
  - What happened: with the knob at zero, the boot bell played a short burst, then went silent.
  - Why: fm1-base started its MASTER level at half. Its smoothing then took about 0.25 s to reach the knob on a busy
    boot, and the bell strikes 34 ms in.
  - The fix is in the platform (`platform/patches/fm1-base/0002`). The firmware reads the MASTER pot before the audio
    starts, so the first block already plays at the knob's level. Without a reading it behaves as before.
  - fm1-emu can't show the old burst: it has no flash-cache model, so its main loop reaches the pot before the bell.
    The proof is on the unit: boot with MASTER at zero and expect silence.
- **Every boot lands on the sand table.**
  - What happened: the splash's sand flowed into the sand table, then the screen cut to the last view used (MESH).
  - Now the boot always lands on SAND, so the sand flows straight into it. EDIT still reaches FIELD and MESH.
  - The last view is still recorded in the settings, but it is no longer applied at boot.
- **Tests:** `tests/splash.txt` gains the case "use MESH, wait for the autosave, reboot: the view is SAND, and EDIT gives
  FIELD". `tests/storage.txt` checks the view after a reboot. `run_all.sh --device --perf` passes, except the two
  panel-key latency gates that have been red since v1.0 (21.8 and 20.4 ms).
- **Reproducible from this repository alone.** The Team EG platform is now patch files in `platform/`, and
  `tools/get_platform.sh` applies them to the pinned upstream. A fresh clone that follows [BUILDING.md](BUILDING.md)
  builds this package byte for byte.
- Confirmed on the FM-1 the same night. The boot is silent with MASTER at zero, the bell plays in full with the knob up,
  and the boot lands on the sand table after MESH was the last view.

## v1.1.2 (2026-10-29)

- **The credit holds long enough to read.** After v1.1.1's hardware test, the TEAM EG & LIZARD_MAN credit stays whole
  and still for 3.95 s, instead of 0.95 s. Everything after it moves 3 s later:
  - the burst at 4.5 s;
  - the CYMA-1 title whole at 5.3 s;
  - the sprinkle into the plate at 8.65 s;
  - the plate's controls at 9.15 s.
- The bell is unchanged: it rings out by 2 s, and the credit holds its last 2.5 s in silence.
- Any key, button or knob still skips, and the skipping key still sounds.
- **On the FM-1:** installed on 2026-10-29 as an update from v1.1.1, with the user sounds kept. It showed the two bugs
  that v1.1.3 fixes.

## v1.1.1 (2026-10-29)

- **Team EG's sonic logo.** At every boot the engine plays a bronze bell, live, not from a recording:
  - a soft E5, then a full G5 (Team EG's letters);
  - then a four-note flick, E6 G6 B6 E7, which lands as the sand locks into the credit.
  - It is in tune to within about 3 cents, and it rings out by itself by about 2 s, with no fade.
  - It plays at MASTER's level, through the same room and limiter as everything else.
  - Then the engine hands back to your preset as if the bell had never played: a key afterwards is bit for bit the
    key without it, for 23 of the 24 factory presets as the boot preset. On 21 AETHER it is within −123 dBFS.
  - Nothing of it reaches your settings, your sounds or the CPU governor.
  - Any key, button, knob or MIDI message stops it with a fade under 10 ms, then acts as usual. A key played in the
    bell sounds about 15 ms late, on that one touch.
- **BOOT SOUND on/off:** hold **GLO** for half a second on the GLOBAL page. The footer says `BOOT SOUND OFF` or `ON`,
  and the choice is saved with the settings. Off gives the old silent boot.
- **The browser plays the bell too**, in place of its old soft "ting".
- **The CYMA-1 title and its tagline hold 3 s longer**, for over 3 s in all. Any key, button or knob skips to the
  plate.
- **Built on the Team EG platform.** With no computer attached (on battery, or on a charger without data), fm1-base
  retried USB once a second. Each try held the main loop for 29–41 ms, and keys, TRS MIDI and the screen waited.
  - The retry is now two steps on separate passes, timed instead of waited (`platform/patches/felucca-bsp/0001`).
  - MIDI-to-sound p90 went from **39.9 to 7.4 ms** in the emulator's play scenario (7.8 ms in v1.1.3's run).
  - The worst case over 203 notes went from 47.4 to 12.6 ms, with 0 xruns before and after.
  - The host and browser builds are unchanged, bit for bit, since they don't compile that file.
- **On the FM-1:** installed on 2026-10-29 as an update from v1.0, the first update on hardware.
  - It boots cleanly, and the bell sounds right.
  - Held AETHER chords barely load the CPU: v1.1's crackle fix works on the chip.
  - After an unplug and replug, USB comes back and the tools answer: the new USB retry works on the chip too.
  - The user sounds were kept.

## v1.1 (2026-10-29)

Built and tested. It reached the FM-1 as part of v1.1.1.

- **No more crackle on bowed chords.** On the first unit, an 8-key chord on 21 AETHER read 90–95 % on the LOAD meter
  and crackled, because the chip runs CYMA-1's audio about 1.2–1.6× slower than the emulator's instruction counts.
  - The governor now lets at most two notes bow at once, since a bow's cost is its friction solver. A bowed chord's
    first two notes sound exactly as before and its other notes don't start; a melody still hands the bow to each new
    note.
  - When the chip runs short on a bowed patch, released tails fade out early (over 50 ms, the quietest first) and ring
    again once the playing has been lighter for about 2 s.
  - The render loops that ran from flash on bowed chords moved into RAM.
  - Mallet and rain presets keep v1.0's behaviour. The host and browser sound is unchanged, bit for bit.
- **Layer B with a tap.** Tap the button of the page you are on to latch its layer B (the SEL LED stays lit, and the knob
  cells show a notch). Tap it again, or tap SEL, to go back. Holding SEL works as before.
- **The boot opens with the credit.** The sand springs into **TEAM EG & LIZARD_MAN PRESENT**, bursts, and forms the
  CYMA-1 logo. The keys play through it.

## v1.0 (2026-10-24)

The first release: a playable Chladni plate for the M-VAVE FM-1.

- A modal engine: 8 voices of up to 32 partials, struck, bowed or rained on a plate that morphs from a circle through a
  hexagon, a star, a guitar body and a bar.
- 24 factory presets, user presets, and pressure on the bottom 20 keys.
- Three views of the same vibration (SAND, FIELD, MESH) and two colour themes (CYMATIC, PHOSPHOR).
- **Sounds**: save, restore, export and share sounds as short codes, and send them to the FM-1 over USB-MIDI.
- The browser version: the same C code compiled to WebAssembly.
- **On the FM-1:** first installed on 2026-10-29 from stock V15, after a full flash backup taken from macOS (tag
  `v1.0-hw`). The screen, all 27 keys, pressure, the CPU meter, saving, and the USB Sounds and backup tools all worked.
