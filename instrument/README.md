# CYMA-1: a playable Chladni plate

CYMA-1 is a firmware instrument for the **M-VAVE FM-1**. It turns the keyboard into a vibrating metal plate. Each key
strikes, bows or rains on the plate, and the screen shows sand settling into the nodal figures you hear. The picture
and the sound come from the same modal simulation, so what you see is what is ringing.

The name comes from the Greek κῦμα, "wave". In 1787 Ernst Chladni drew a bow across sand-covered brass plates and
showed people the shapes of sound. CYMA-1 makes that playable on a pocket keyboard: press a key harder and you bow
harder, the plate starts to sing, and the sand jumps off the antinodes and snaps into a crisp figure. Turn SELECT and
a circle melts into a hexagon and then a star. The overtones glide with it, and the sand flows into the new shape.

Produced by **Team EG & Lizard_Man**. It is built on [fm1-base](../reference/fm1-base) (MPL-2.0) and is MPL-2.0
itself.

> **Status, plainly.** CYMA-1 runs in the **host simulator** and in the **browser**. Both builds compile the same C
> code, and that code is what the test harness exercises. **v1.0 has run on a real FM-1 since 2026-10-29.** That
> unit showed what **v1.1** fixes: 21 AETHER chords read 90–95 % on the LOAD meter and crackled, because the chip
> runs CYMA-1's audio about 1.2–1.6× slower than the emulator's instruction counts. **v1.1.1 and v1.1.2 ran on the
> same unit that evening:** the boot and the bell, AETHER without the crackle, and USB coming back after a replug were
> confirmed there. v1.1.2 showed the two field bugs that **this tree, v1.1.3** (`cyma-1.fwsc`, identity
> `FM-1_94010103`), fixes; v1.1.3 is built and tested, and **its own hardware check is still to come**
> <!-- CONFIRM: v1.1.3 on hardware -->. The sound and
> screen figures are **algorithmic, from the host build**. The CPU figures closest to the device come from
> running that exact package in the fm1-emu chip emulator at the FM-1's clock (`run_all.sh --perf`), and v1.1 was also
> run with the emulator's clock slowed to 1.4× and 1.6× to stand in for the chip. That is still **not a measurement
> on the FM-1**. The current pass/fail state of every test, the package's SHA-256 and the device-time figures all live
> in [docs/SPEC.md](docs/SPEC.md), generated from the latest run; every release's changes are in
> [../CHANGELOG.md](../CHANGELOG.md).
> Read [Before you flash anything](#before-you-flash-anything) before you put it on a device.
>
> **What changed in v1.1:** bowed chords no longer overrun the chip. The device's CPU governor now limits how many
> notes bow at once to two, because a bow's cost is its friction solver, which v1.0's mode cuts never reached. On the
> FM-1 a 21 AETHER chord sounds its first two notes bowed (each exactly as before) and the chord's other notes do not
> start; a melody still hands the bow to each new note. Expect bowed chords to stay at two notes: a third comes back
> only on a unit with clear headroom, after a two-note hold, and not while chords keep arriving (the emulator's
> stand-ins for a slower chip never gave it back). When the chip runs short on a bowed patch, released notes' tails
> fade out early (over 50 ms, the quietest first) and ring again once the playing has been lighter for about 2 s;
> when it cannot carry a new bowed chord and the last one's ringing tails together, the tails fade quickly (5 ms) as
> the chord begins and its second note follows about 6 ms after its first. Mallet and rain presets keep v1.0's
> behaviour. The render loops that ran from flash on bowed chords moved into RAM. A page's layer B can now be latched
> by tapping that page's button again (tap it again, or tap SEL, to leave). The boot screen now opens with the credit:
> the sand springs into **TEAM EG & LIZARD_MAN PRESENT**, bursts, and forms the CYMA-1 logo (3.15 s in all; the keys
> play through it). The host and browser sound is unchanged, bit for bit.
>
> **What changed in v1.1.1:** CYMA-1 now says hello. At every boot the engine itself plays **Team EG's sonic logo**,
> live, not from a recording: a bronze bell's soft E, then a full G, then a quick flick as the sand locks into the
> credit. It rings out on its own by about 2 s, at the level MASTER sets. It never touches your sound: the preset you
> boot into plays as it would without it, and nothing of the bell is saved. Any key, button, knob or MIDI message
> stops it at once (a fade under 10 ms) and then does what it always does (a key's note follows the fade, about 15 ms
> late on that one touch). To switch it off, **hold GLO for half a second on the GLOBAL page**: the footer says
> `BOOT SOUND OFF`, it is saved with the settings, and the boot is silent again (hold GLO again to bring it back).
> The browser plays it too, in place of its old soft "ting". The
> logo and its tagline now hold for over 3 s, and any key, button or knob skips to the plate. And v1.1.1 is built on
> the Team EG platform, a pinned fork of fm1-base (`platform/`, made by `tools/get_platform.sh`), whose main loop no
> longer stalls once a second when no USB host is attached (on battery, or on a charger): that stall held keys, TRS
> MIDI and the screen up to about 45 ms.
>
> **What changed in v1.1.2:** the TEAM EG & LIZARD_MAN credit holds about 3 s longer, so it can be read (9.15 s in
> all; any key still skips).
>
> **What changed in v1.1.3:** the two bugs v1.1.2 showed on the unit. With MASTER at zero, the boot bell no longer
> plays a short burst: the platform now reads MASTER before the audio starts, so the first block plays at the knob's
> level (fm1-base's `device/main_fm1.c`, in `platform/patches`). And every boot now lands on the SAND view, so the
> splash's sand flows straight into the sand table; EDIT still reaches FIELD and MESH.

---

## What it is, in a little more detail

- **The plates.** There are nine shapes: DRUM (a clamped circle), CIRCLE, HEX, SQUARE, TRIANGLE, STAR, GUITAR,
  STADIUM and BAR. Each one's vibration modes were solved offline with scipy and committed as C data. Between shapes,
  the modes were tracked through intermediate outlines, so a morph glides each partial instead of jumping.
- **The sound.** Each voice is a bank of up to 32 resonating modes (complex one-pole filters, all float). Eight
  voices can play at once, and SURFACE mode adds one more slot. The output goes through drive (oversampled 2×,
  alias-suppressed), a tilt EQ, an 8-line reverb, a DC blocker and a look-ahead limiter. It reaches the DAC (the
  headphone and line out) as clean 24-bit words. The USB audio input is 16-bit: see [Recording it](#recording-it).
  There are no oscillators to alias, and by design no mode is synthesised above 0.45 of the sample rate. The test
  harness measures what reaches up there anyway (the strike transients), against a -100 dB gate, in
  [docs/SPEC.md](docs/SPEC.md).
- **The picture.** The sand, the FIELD view (the plate's displacement, with nodal lines drawn in ink) and the MESH
  view (a slow-motion 3-D surface) all read the same mode tables and energies as the audio engine.
- **The physics, honestly.** The modes are membrane (Laplacian) eigenmodes plus a stiffness law, not true biharmonic
  free-plate modes. So the figures are **Chladni-like**: recognisable, but not a lab measurement of a brass plate.

## How to play

### Keys

There are 27 keys, F3 to G5. Every key senses velocity. The bottom **20 keys (F3 to C5) also sense pressure**: keep
leaning after the key goes down and the plate responds. The top seven keys (C♯5 to G5) have velocity only.

What leaning in does depends on the **exciter**, which you choose with ALGORITHM:

| Exciter | A key press | Leaning in (pressure) |
|---|---|---|
| **MALLET** | a strike (HARDNESS sets felt to steel) | TENSION bends the pitch up, PALM mutes it |
| **BOW** | a bow on the plate at POS | bow force: press harder and the bow grips and sings |
| **RAIN** | a drizzle of tiny impacts while held | the rain pours (the drop rate follows pressure) |

With every exciter, pressure also drives STRETCH: lean in and the metal tightens.

### The encoders

| Control | What it does |
|---|---|
| **SELECT** | **SHAPE**, on every page. 16 detents per shape, with a short dwell on each pure shape. The readout shows e.g. `HEX › SQUARE 40%` |
| **ALGORITHM** | **Exciter**: MALLET → BOW → RAIN |
| **PRESETS** | Browse 01–24, then your saved U01–U32. The preset loads with a 300 ms glide, so ringing notes slide into the new sound. Browsing away from an edited preset keeps its edits, one preset deep: land on it again and they come back (`EDITS KEPT`). Edits also survive switching off |
| **K1–K4** | The four parameters shown along the top of the screen, for the current page and layer |
| **MASTER** | Volume. It comes after the limiter, so it never changes the sound's character |

### Pages (layer B: hold SEL, or tap the page's button again)

| Button | Page | Layer A | Layer B (SEL held, or the page's button tapped again) |
|---|---|---|---|
| HOME | **PLATE** | MATERIAL · STIFFNESS · CARVE · DECAY | TARGET · BRIGHT · SHIMMER · MODES |
| ENV | **EXCITE** | HARDNESS / FORCE / DENSITY · POS X · POS Y · TENSION / GRIT / SPREAD | NOISE · DAMPER · BLOOM · PALM / SWELL |
| FX | **SPACE** | MICS · ROOM · SIZE · DRIVE | PREDELAY · DAMPING · TILT · LEVEL |
| LFO | **MOTION** | WANDER · MORPH · LFO RATE · STRETCH | LFO WAVE · LFO>POS · VIBRATO · VIB RATE |
| GLO | **GLOBAL** | VEL CURVE · PRESS SENS · TUNE · VOICES | THEME · GRAINS · MOTION · DETAIL |

On EXCITE, the first and last knobs follow the exciter. The slashes above list MALLET, then BOW, then RAIN.

There are two ways to reach a page's layer B:
- **Hold SEL.** Layer B shows while SEL is held and goes back to A when you let go.
- **Tap the button of the page you are on** (new in v1.1). Layer B then stays without holding anything: the SEL LED
  stays lit, each of the four knob cells along the top of the screen shows a small notch in its top-right corner (and
  their labels change), and the footer says `LAYER B` for a moment. It goes back to A when you tap that button again,
  tap **SEL** on its own, or press another page button. The tap counts when you let go: a press held longer than
  about 0.4 s, or one that lands together with SEL (SEL+HOME is COMPARE), does not toggle. Holding SEL still works as
  before.

**Changing the colour theme:** press **GLO** for the GLOBAL page, then tap **GLO** until the first knob cell reads
**THEME** (the footer says `GLOBAL LAYER B`). Turn K1 to switch between CYMATIC (the material's colours) and PHOSPHOR
(green on black). Tap GLO or SEL to go back to layer A. (Or hold SEL on the GLOBAL page and turn K1.) THEME is a
setting, not part of the preset, so it stays as you leave it.

A few parameters worth knowing:
- **MATERIAL** runs from rubber through wood, glass, steel and bronze to "aether". It sets how long the plate rings,
  how quickly the high modes die, and the screen's colours.
- **STIFFNESS** runs from a skin (0) through a plate (80) to a stretched bell (100).
- **CARVE** pulls the overtones onto a **TARGET** tuning: HARMONIC (handpans, tabla), BELL (hum, prime, tierce) or
  BAR (marimba, 1:4:10).
- **BLOOM** makes the pitch fall as a note decays (positive: kick drums, toms) or rise (negative: an opera gong).
- **POS X/Y** is where you strike. Hit a nodal line and that mode goes quiet.

### Buttons

| Button | Tap | With SEL held |
|---|---|---|
| **SEQ** | **SURFACE ⇄ TUNED** (see below) | tap: **SPRINKLE** fresh sand; hold 1 s: **SWEEP** the plate clean |
| **PLAY** | **FREEZE**: the notes sounding now hold forever (PLAY glows green, the plate turns ice-blue) | |
| **ARP** | **LATCH**: notes keep sounding after you let go; bowed notes keep bowing, for drones. In SURFACE it holds the touched points | |
| **REC** | hold: **DAMP**, a palm on the plate | |
| **EDIT** | view: **SAND → FIELD → MESH** | |
| **SAVE** | save. On a factory preset the footer asks `SAVE TO U07?` (the first free slot; PRESETS picks another). A slot that already holds a preset asks `REPLACE U05?` in red, with that preset's name; a full bank starts on U32. Press SAVE again to write it; a page button cancels, and an untouched question closes after 15 s. A save over ringing notes fades the output for about 0.1 s instead of clicking | **REVERT** to the stored preset. SEL+SAVE again within 5 s undoes the revert |
| **HOME** | page PLATE; on PLATE, layer B ⇄ A | **COMPARE**: hear the stored preset while held |
| **ENV · FX · LFO** | pages EXCITE, SPACE, MOTION; on that page, layer B ⇄ A | |
| **GLO** | page GLOBAL; on GLOBAL, layer B ⇄ A (THEME · GRAINS · MOTION · DETAIL). In v1.0 a second tap went back to the page before. On GLOBAL, **held 0.5 s: BOOT SOUND** on ⇄ off (v1.1.1: the logo at boot; saved with the settings) | replay the **tour** |
| **SEL** | hold: layer B while held; tap on its own while layer B is latched: back to layer A | (the combos in this column) |
| **OCT− / OCT+** | octave −3…+3; **both together: PANIC** (all notes off; freeze, latch and damp cleared) | transpose ±1 semitone (in SURFACE: the plate's root) |
| **SEL + PRESETS** | **morph-scrub** between this preset and its neighbour, 1/16 per detent. Let go of SEL to keep the in-between sound | |

Nothing is buried in a menu. Any setting is at most two presses away.

### SURFACE mode: the keybed becomes the plate

Press **SEQ** and the 27 keys stop being notes. Each one becomes a **strike point on a single plate**, laid out the
way the keys sit on the panel. Centre keys boom and edge keys shimmer. The **key LEDs show the live vibration map**,
so keys on a nodal line go dark. A key played softly (velocity under 24) doesn't strike; it **touches**. Resting a
finger on a dark key damps every mode except those with a node there, and a pure harmonic rings out: the
flageolet trick, on a gong. POS X/Y slides the keybed over the plate, and OCT moves the plate's root. Preset 22,
SURFACE GONG, starts in this mode.

### The screen

The plate fills the middle of the screen, always live. Along the top are the four knob cells for the current page.
The left rail is the **partial ladder**, showing every mode's level at its pitch (CARVE's targets appear as amber
ticks). The right rail holds the L/R meters, gain reduction and a pressure column; the CPU load shows there on the
GLOBAL page, and on any page once it passes 70 %. The footer shows the exciter's glyph, the preset's number and name,
badges for FREEZE, LATCH, DAMP, the octave and the transpose, and the voices. When you turn a knob, its value appears
large at the plate's top-left, with a one-line hint for the first few touches. While SEL+PRESETS morphs, the two
presets' names show at the plate's foot and their numbers on the footer's scrubber. On first boot, a five-step tour
points off the screen at the controls you need.

### Recording it

There are two ways to get CYMA-1 into Live, and they are not equal.

- **The headphone/line out into an audio interface: the best path.** It carries the DAC's full 24-bit words.
  MASTER sets its level after the limiter, so the sound's character doesn't change with the volume.
- **Over the USB cable, as an audio input.** The FM-1 shows up as a 2-channel, 44.1 kHz USB audio **input**, named
  after `usb_product` in `fm1.toml` ("CYMA-1 FM-1"). It is input only: the Mac cannot play audio through the FM-1
  (fm1-base would mix that after CYMA-1's limiter, unclamped, and the 24-bit word could wrap into loud crackle). The
  input is **16-bit**, rounded without dither, and taken **after MASTER**. If you turn MASTER down for your
  headphones, the recording drops with it and loses resolution: −20 dB on MASTER leaves about 13 effective bits, and
  quiet reverb tails turn grainy. So when you record over USB, **set MASTER fully up** and lower the level in Live
  instead.

### MIDI

CYMA-1 listens on **every channel** of USB MIDI (and TRS):

| Message | What it does |
|---|---|
| Note on / off | plays, like the keys. In SURFACE, notes F3–G5 (moved by OCT) are the strike points |
| Poly aftertouch | that note's lean (pressure) |
| Channel pressure, CC1 (mod wheel) | lean for the notes played **from MIDI**, only while they are held; the FM-1's own keys keep their own pressure |
| Pitch bend | ±2 semitones |
| CC64 (sustain pedal) | LATCH, separate from ARP's: a DAW's pedal-off never clears the panel's LATCH |
| CC120 All Sound Off | a full PANIC: latched and frozen notes stop too |
| CC121 Reset All Controllers | bend, pressure, mod wheel and the pedal back to rest |
| CC122 Local Control | 0: the keys still send MIDI but do not sound (for Live echoing the track back); 127, PANIC or a reboot: on again |
| CC123 All Notes Off | MIDI's notes let go |
| Program Change | 0–23 load presets 01–24; 24–55 load U01–U32 |

It sends on USB channel 1: the keys' notes (one note-on and one note-off per note number) and their poly aftertouch,
thinned so it never crowds out a note-off. There is no CC parameter map, clock sync or channel select yet.

### The 24 factory presets

| # | Preset | | # | Preset | |
|---|---|---|---|---|---|
| 01 | SINGING BOWL | bronze bowl, soft mallet; lean in and it bends a little sharp | 13 | CHAOS STADIUM | rain in a chaotic billiard |
| 02 | HANDPAN | steel tuned to fundamental, octave and fifth | 14 | FELT PLATE | muted wood and felt: lo-fi keys |
| 03 | GLASS LOTUS | a five-armed glass star | 15 | WATER GONG | a gong dipped in water: the pitch falls |
| 04 | MARIMBA | rosewood bar undercut to 1:4:10 | 16 | MEMBRANE KICK | skin kick with real pitch-drop physics (play at OCT −2) |
| 05 | VIBES | aluminium bars; let go and the felt damps them | 17 | LOOSE TOM | a half-clamped tom |
| 06 | TABLA SKIN | loaded skin; lean in to bend it like a bayan heel | 18 | THUNDER SHEET | a shaken steel sheet, driven |
| 07 | CATHEDRAL GONG | huge bronze gong whose pitch rises as it decays | 19 | ICE | hard glass triangle, crystalline |
| 08 | STEEL RAIN | rain on a steel hexagon; press harder and it pours | 20 | KALIMBA TINE | a steel tine plucked near its tip |
| 09 | BOWED GLASS PAD | the classic Chladni demo: a bowed glass square | 21 | AETHER | endless bowed drone, morphing circle ↔ hexagon |
| 10 | GAMELAN STAR | bronze star with ombak beating | 22 | SURFACE GONG | the keybed is one gong |
| 11 | PRISM BELLS | triangle carved to bell tuning | 23 | BOWED SQUARE | raw steel square bowed at the edge |
| 12 | GUITAR BODY | a knock on a spruce guitar body | 24 | TEMPLE BELL | a bonshō struck by a wooden beam |

## In the browser

The browser build runs the identical firmware compiled to WebAssembly in an AudioWorklet, including the screen,
the LEDs and the saved presets. Build and open it:

```sh
export DYLD_LIBRARY_PATH=/opt/homebrew/lib
python3 reference/fm1-base/tools/build.py instrument --web     # instrument/build/emu/
python3 instrument/tools/package_web.py                         # instrument/dist/web/ (one folder, any static host)
                                                                # and instrument/dist/artifact/ (the claude.ai Artifact)
```

Serve it with `python3 -m http.server -d instrument/dist/web`, open the page and press **Switch on**.

- **Computer keyboard** (Ableton layout): `A W S E D F T G Y H U J K O L P ; '` play C4–F5. `Z`/`X` are OCT−/OCT+,
  `C`/`V` change the velocity, and **hold Space to lean in** on every held key. Shift is SEL. `1`–`6` are
  HOME ENV FX LFO GLO EDIT, `7 8 9 0 -` are SEQ ARP PLAY REC SAVE. `←`/`→` turn SELECT, `↑`/`↓` turn PRESETS and
  `[`/`]` turn ALGORITHM.
- **Mouse or touch:** click higher on a key for a softer note. Drag down while holding to press harder; pen and touch
  pressure work too. Shift-click, Alt-click or right-click latches a key or a button; Escape lets go of everything.
  Click the screen (or press `B`) for the big screen.
- **Web MIDI** in and out where the browser supports it: play it from a controller, or play into Live over IAC.
- **Capture:** the last 60 seconds are always kept. One click saves them as a `.zip`: a 24-bit WAV and the `.mid`
  of the same minute (the keys, MIDI in and the phrase), both starting at the same instant, so they line up when
  dropped at the same bar.
  **Play me something** performs a short handpan groove through the MIDI input.

## Sounds: save, restore, share

When you like what you hear, keep it. The full guide is [docs/SOUNDS.md](docs/SOUNDS.md); in short:

- **Save:** **Save sound** in the bar (or ⌘S / Ctrl+S), type a name (up to 14 characters, shown in capitals as the
  FM-1 shows them), pick a slot. Your sounds go into **My Sounds**: 32 slots, U01–U32, the same bank as the panel's
  SAVE button. On the panel they follow the 24 factory presets when you turn PRESETS.
- **Restore:** click a sound in **Mine** to load it (it glides there, as PRESETS does). Rename, delete (with Undo) and
  Revert are in the **Sounds** section.
- **Share:** **Share** gives a **code** (`cy1.…`, 126–140 characters, for any chat), a **link** that opens CYMA-1
  with the sound ready to play, or a **file** (`NAME.cyma1.json`). Whoever opens it hears the sound first; nothing is saved
  until they press **Keep it**, and **Undo** takes them back. **A code always works.** A link carries a shorter form
  (`#cy2.…`, 50–74 characters for the factory sounds), because claude.ai passes the part after the `#` into the page
  only when it is at most 128 characters (measured); a sound changed so much that even that form is longer gets no
  link, only its code and file.
- **Files:** **Save all my sounds as a file** writes one `.cyma1-bank.json` with every sound. Drop it (or a sound file,
  or a text file of codes) on the page to import it.
- **Back up:** My Sounds lives in this browser's storage, which a cleared browser or a private window loses, and the
  claude.ai page and the page on your Mac keep separate lists. Save all your sounds as a file now and then, and
  **always before you update CYMA-1 or install other firmware**. Factory reset keeps My Sounds.
- **To the FM-1, later:** once CYMA-1 is installed, send sounds over USB from the page on your Mac
  (`python3 instrument/tools/cyma_local.py`, then Chrome) or with the command-line tool
  (`instrument/tools/cyma_sounds.py`). The FM-1 asks on its own screen first (`SAVE=YES HOME=NO`): nothing from USB
  goes into your user presets (U01–U32) without a press of SAVE there. A sound you only play on it is remembered by
  the settings autosave, as a knob turn is. The tools talk only to an FM-1 that answers as CYMA-1, and never touch
  the update channel. The page at claude.ai cannot reach MIDI devices, so it explains this route instead.
  **Not yet tried on a real FM-1**: [docs/SOUNDS.md](docs/SOUNDS.md) ends with the checklist for the first time.

The formats (sound and bank files, codes and the short link code, the USB protocol) are specified in
[CYMA-1-sounds-spec.md](../docs/CYMA-1-sounds-spec.md) (its revision 2026-10-24b adds the link code `cy2` and the
measured 128-character anchor limit), with frozen test vectors in `tests/sounds/vectors.json`, so other tools can read
and write CYMA-1 sounds. `run_all.sh` tests them in the steps `sounds_vectors`, `unit_xfer`,
`xfer_asan`, `sounds_scen`, `sounds_smoke` and `sounds_cli`, plus `sounds_chrome` with `--chrome` and the Sounds checks
of `--artifact`.

## Building and testing

Run everything from the project root. `DYLD_LIBRARY_PATH` lets Pillow find libraqm for the fonts.

```sh
tools/get_platform.sh                                           # the Team EG platform -> platform/fm1-base-teg (once)
export DYLD_LIBRARY_PATH=/opt/homebrew/lib
export FM1_BASE="$PWD/platform/fm1-base-teg"                    # v1.1.1 and later: the Team EG platform (its TEG_FORK.md)
python3 "$FM1_BASE/tools/build.py" instrument --host            # host simulator: instrument/build/host/cyma-1_host
python3 "$FM1_BASE/tools/build.py" instrument --web             # browser emulator: instrument/build/emu/
tools/build_device.sh instrument --no-ble --release 1.1.3       # device package (build/cyma-1-1.1.3.fwsc), NEVER flashed
instrument/tools/run_all.sh                                     # everything below, with a summary
```

`run_all.sh` takes fm1-base from `FM1_BASE`; unset, it uses `platform/fm1-base-teg` when it is there (v1.1.1 and
later are built on it), else `reference/fm1-base`, and hands the same tree to `tools/build_device.sh`. The steps from
a fresh clone to the release's exact package are in [../BUILDING.md](../BUILDING.md).

`instrument/tools/run_all.sh` runs, in order:
1. the harness's own selftests and a lint of every script;
2. both builds;
3. the modules' unit tests;
4. every scenario script and golden gate, including the wasm smoke test;
5. `SPEC.json` and `docs/SPEC.md`;
6. the screens' contact sheet, the demos and the web package.

Flags add the slower suites:

| Flag | Adds | Time |
|---|---|---|
| `--device` | The device package, built and never flashed, as release `1.1.3` (`fm1.toml`'s `release`: `build.py --release 1.1.3`, identity `FM-1_94010103`, renamed `build/cyma-1.fwsc`). Every check must print ok, the image must fit the app slot (581,564 B) and the identity must be `FM-1_94…`. It writes `build/SHA256SUMS`, which `docs/SPEC.md` quotes after checking it against the files | about 15 s |
| `--perf` | `--device`, then the package in fm1-emu at 205 MHz (the FM-1's measured throughput), timed from breakpoints on the firmware's own functions. It gates the slowest audio block, xruns, the screen's frame time, key-to-sound latency and idle redraws (`tools/emu_perf.py`) | about 3 min |
| `--chrome` | The page in headless Chrome. Every check is asserted: switch-on, keys, mouse, lean, knobs, capture, MIDI, themes, 1280 / 2× / 390 px, a sandboxed frame, 48 kHz, `file://`, the dev build, and the error paths (`tools/chrome_test.mjs`) | about 40 s |
| `--artifact` | The page as its publishing target frames it: converted to the artifact shape, in a cross-site sandboxed frame under the host's CSP, with the host's downloads runtime stubbed (`tools/artifact_test.mjs`). Then the same suite again on the package that gets published, built with the artifact's own URL as its share base (`artifact_sb`: Copy link gives `#cy2.…` within claude.ai's 128-character anchor; `dist/` is left as it was) | about 45 s |
| `--quick` | Only smoke and controls. The gates they cannot feed show as SKIP, not FAIL | |

`--copy DIR` tests a private copy instead of this tree. Each step writes its own log. A summary lists PASS, FAIL or
SKIP for each step, plus every failing gate and the reason.

| Tool | What it does |
|---|---|
| `tests/*.txt` | Scenario scripts in fm1-base's host-script language: smoke, controls (every button, combo and encoder, with LED checks), presets, bow sweep, SURFACE, morph, freeze, silence, limiter, noise floor, MIDI, storage (save, reboot, reload), screens, the review round's app fixes (`app_fixes`, `app_store`: REPLACE, undo, edit memory, the MIDI controllers, the governor, settings across reboots, backup and restore) and perf. `tests/ui_stub/regress.txt` checks the screen's redraw budgets on the UI stub |
| `tools/golden.py` | Runs every scenario, then measures what came out: mode ratios against Bessel and Euler, energy above 0.45 fs, sample peaks, NaN, clicks, reverb T60, determinism (host = host = wasm), the CPU model and the UI frame time. Each has a pass/fail gate. `golden.py selftest` checks each measurement against synthetic signals; `golden.py lint` checks the scripts themselves |
| `tools/wasm_smoke.mjs` | Runs `app.wasm` in node with scripted keys and knobs, then checks the sound, the screen, the LEDs, the info JSON and the demo hash |
| `tools/spec.py` | Writes `build/spec/SPEC.json` (the page's spec card) and [docs/SPEC.md](docs/SPEC.md), with the package's hash from `build/SHA256SUMS` and the device-time figures when they exist |
| `tools/emu_perf.py` | Runs the unmodified `.fwsc` in fm1-emu at 205 MHz and gates what the chip would do. `emu_perf.py selftest` checks its analysis against synthetic breakpoint logs; `build-emu` builds the emulator with its two small patches |
| `tools/chrome_test.mjs`, `tools/artifact_test.mjs` | The browser suites behind `--chrome` and `--artifact` |
| `tests/sounds/ring/` | The `sounds_ring` step: fm1-base's own USB SysEx ring, receive slot and main-loop dispatch (extracted verbatim from `reference/fm1-base` at test time) around CYMA-1's real `app_sysex`, flooded at every host read rate and judged against the same platform without CYMA-1. It is the only test of the send gate that keeps CYMA-1's replies from crowding out the platform's own (about 25 s) |
| `tools/render_demos.sh` | Renders the eight `tests/demos/*.txt` performances to a 24-bit WAV; an MP4 with the screen and the sound in sync (8 frames a second, AAC 256 kbps from the WAV); a 320 kbps mp3; and a GIF of the screen |
| `tools/contact_sheet.py` | Lays out every screenshot from `tests/screens.txt` on one page |

The demos are real performances, scripted note by note, played by the same code a device would run:
- a singing bowl leaned into;
- a handpan groove;
- a circle melting into a star under an arpeggio;
- steel rain swells;
- tabla bends;
- a frozen gong drone;
- SURFACE harmonics;
- Chladni's bowed glass square.

The demo films and MP3s are loudness-matched listening copies (−18 LUFS integrated, true peak at most −1 dBTP; only
the tabla demo needed a transient limiter, about 5 dB at most). The 24-bit WAVs in `build/demos` are the engine's
exact output, untouched.

## Before you flash anything

**Installing from a Mac without the Transporter**, as every CYMA-1 install so far was done, is a separate guide:
[../notes/INSTALL.md](../notes/INSTALL.md). It covers the first install from stock V15 with a full backup, updates
from an earlier CYMA-1, going back to V15 and recovery. This section is the Transporter route.

**v1.1.3 has not been confirmed on an FM-1 yet** <!-- CONFIRM: v1.1.3 on hardware --> (v1.0, v1.1.1 and v1.1.2 have run on one since 2026-10-29). The `.fwsc` builds cleanly and passes the build's static checks: no
calls from RAM-resident code, no soft-double maths, no risky 64-bit multiplies, and the memory pools fit. It also runs
in the fm1-emu chip emulator at the FM-1's clock (`run_all.sh --perf`). None of that proves it boots, sounds right or
keeps up on the real chip. fm1-base also has **no torn-install protection**: an install cut off halfway can leave the
unit in ROM recovery mode. The first install from stock V15 is the riskiest moment there is. In TransitFM1's power-cut
model, 2,414 of 2,432 cut points ended in ROM update mode. Every case below is recoverable, but only if the backup and
the recovery tools are ready **before** you start.

Work through the steps in order, from the project root (`cd "…/M-VAVE Synth Experiment"`). Steps 0–3 change nothing
on the unit. The full background is in
[docs/research-briefing-2026-10-24.md, §4](../docs/research-briefing-2026-10-24.md). That briefing is generic,
though: where it disagrees with this page (its tool paths, its power-on keys), this page is the one that is right for
CYMA-1.

**0. Check the package.** Nothing authenticates a `.fwsc`: the FM-1 installs whatever it is given. The hash is your
only proof that the file is the one this tree built. `run_all.sh --device` writes it, and the build is reproducible: a
clean rebuild in another folder gives the identical file.

```sh
shasum -a 256 instrument/build/cyma-1.fwsc        # must equal the cyma-1.fwsc line of:
cat instrument/build/SHA256SUMS
```

**1. Prepare the unit and the Mac.**
- Charge the battery and switch the FM-1 **on**. CYMA-1 shows no battery level: fm1-base cannot read the battery
  while the key sensing owns the converter, so charge before installing or saving, not when a warning appears.
- Use a USB **data** cable, plugged **directly** into the Mac: no hub, no dock. Leave nothing in the TRS MIDI jack.
- **Quit Ableton Live** and every other app that uses MIDI: other DAWs, MIDI Monitor, browser tabs with Web MIDI,
  including this project's own page. Live keeps the FM-1's MIDI port open while it runs, and can send clock, notes or
  SysEx to the unit in the middle of an install.

**2. The read-only pre-check.** It sends one identity request (`F0 00 32 45 00 00 00 40 7F F7`) and prints what is
running. The project's `.venv312` has the `mido` and `python-rtmidi` it needs.

```sh
.venv312/bin/python reference/fm1-base/bsp/tools/fm1_install.py --info
```

| It prints | What it means |
|---|---|
| `FM-1_015  [running]` | Stock V15: carry on. |
| `FM-1_9400  [running]` | CYMA-1 v1.0: carry on (this is an update). Back up your user presets and settings first (step 6's `fm1_backup.py backup`); v1.1.3 reads the same settings and user bank. |
| `FM-1_94010100  [running]` | CYMA-1 v1.1: carry on (this is an update), with the same backup first. v1.1.3 reads the same settings (BOOT SOUND uses a byte v1.1 kept reserved, at 0: on) and user bank. |
| `FM-1_94010101` or `FM-1_94010102  [running]` | CYMA-1 v1.1.1 or v1.1.2: carry on (this is an update), with the same backup first. Same settings and user bank. |
| `FM-1_016` | **Stop.** V16 and Echomatter's custom firmware both report this identity, and nobody has shown a way back from V16. |
| `ota-…` or `[update loader …]` | An earlier install never finished. Go to step 5. |
| `FM-1 not found` | Check the cable, and that no other app has the port open. |

**3. Back up the whole flash.** It holds unit-unique Bluetooth data, the key and MAC address, and the factory voices,
and **cannot be recreated**. Use the Transporter: a Seeed XIAO **RP2040** (not an RP2350), wired to D+, D− and GND,
never VBUS ([its guide](../reference/MvaveFM1Unbricker/TRANSPORTER-GUIDE.md)). Rehearse the whole thing once
without hardware by adding `--dry-run` to the command.

```sh
python3 reference/MvaveFM1Unbricker/fm1_transporter_recover.py dump ~/FM1-backup \
    --v15 firmware/stock/FM-1_V15_official.fwsc       # reads the 1 MiB twice and compares
shasum -a 256 firmware/stock/FM-1_V15_official.fwsc  # db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a
```

- Keep three copies of the dump, plus a `SHA256SUMS`, off the Mac.
- Never restore another unit's dump.
- `firmware/stock/FM-1_V15_official.fwsc` is the recovery image. It is byte-identical to M-VAVE's `FM-1_015.fwsc`
  from fm1_open, and `fm1_install.py` recognises it as stock V15.
- Leave the Transporter wired up for the install.

**4. Install.** The tool prints the package (`FM-1_94010103`, CYMA-1 1.1.3; v1.1.2's was `FM-1_94010102`, v1.1.1's
`FM-1_94010101`, v1.1's `FM-1_94010100`, v1.0's `FM-1_9400`) and the device, then asks before it writes. From then on,
hands off: do not touch the FM-1, the cable or the Mac until CYMA-1's splash screen appears (its boot sound plays as
the credit forms). The unit shows an update screen and restarts by itself, and the tool then checks that
`FM-1_94010103` is running.

```sh
.venv312/bin/python reference/fm1-base/bsp/tools/fm1_install.py instrument/build/cyma-1.fwsc
```

**5. If something goes wrong,** stop at the first row that matches. The Transporter commands below are
`python3 reference/MvaveFM1Unbricker/fm1_transporter_recover.py …`, and each one is a dry run until you add `--write`
or type `WRITE`.

| What you see | What it means | What to do |
|---|---|---|
| The update screen stays up, or the tool stopped or lost the unit | The install was interrupted | Replug the cable and run step 4 again: it finishes the write. Do not power-cycle again and again. |
| `CYMA-1 SAFE MODE`, saying "IT CRASHED TWICE WHILE STARTING … REINSTALL FROM THE WEB INSTALLER" | CYMA-1 crashed twice at start-up. There is no sound, but USB still works | CYMA-1 has **no web installer**: use `fm1_install.py` from the Mac. **PLAY** tries CYMA-1 once more. To go back to stock, run step 6 while this screen is up. **REC** enters update mode (next row). |
| `UPDATE MODE (REC)`, then a black screen. System Information lists `4C4A:8057` | The chip's ROM update mode, from REC held at power-on or REC in safe mode | macOS may offer to initialise an "unreadable disk": click **Ignore**, never Initialize. Got here by accident? Switch off and on. Otherwise restore with the Transporter: `fm1_transporter_recover.py restore firmware/stock/FM-1_V15_official.fwsc --ref ~/FM1-backup/<dump>.bin`. |
| `HARDWARE CALIBRATION`, asking for `PRESS FX` | OCT− and OCT+ were held at power-on. That is CYMA-1's PANIC chord, so it is easy to do | Press each button and turn each knob as the screen names it: this re-teaches the panel. Or touch nothing for 30 s: it gives up and keeps the old layout. |
| A black screen, and nothing on USB | Nothing answers | Transporter: `fm1_transporter_recover.py wizard --v15 firmware/stock/FM-1_V15_official.fwsc`. Switch the FM-1 off, wire it up, start the wizard, switch the FM-1 on, then dump, dry-run, type `WRITE` and verify. |
| CYMA-1 boots, but the sound or the screen is wrong | A bug, not a failed install | Note what you saw, then return to stock (step 6). |
| CYMA-1 crashes or misbehaves only with your saved state | A setting it stored | Hold **SAVE and HOME** while you switch on: it boots with default settings (preset 01, the globals, the octave) and keeps your user presets. |

Two power-on combinations belong to the platform, not to CYMA-1. **REC** held while you switch on enters ROM update
mode. **OCT− and OCT+** held enter hardware calibration. Let go of PANIC before you switch the unit on.

**6. Back to stock V15.** First back up your CYMA-1 user presets and settings over USB, because V15 will overwrite
them (the user bank at 0xDA000 is inside V15's settings region). The firmware can keep playing; a restore later
asks on CYMA-1's screen before it changes anything.

```sh
.venv312/bin/python reference/fm1-base/tools/fm1_backup.py backup ~/cyma-1-presets.bak --magic CYM1
```

Then install V15. `fm1_install.py` accepts the unmodified V15 file without `--force`, because it checks the
file's hash.

```sh
.venv312/bin/python reference/fm1-base/bsp/tools/fm1_install.py firmware/stock/FM-1_V15_official.fwsc
```

V15 keeps its settings at 0x93000–0xE8FFF and 0xFC000–0xFEFFF. CYMA-1's saved state (0xDA000–0xDDFFF, 0xFC000) and
fm1-base's update staging (from 0xE0000) overwrite parts of both regions. So V15 may come back with garbled patches
or settings. First try **System > Reset Patches** on V15. A deeper repair restores those regions from **your own**
dump: `fm1_unbrick.py repair-settings`, which runs on Linux or Windows only.

**Hard rules, whatever a tool offers:**
- never write below 0x4000 or at 0xFF000;
- never erase the whole chip;
- never write a raw `.fwsc` with a region-based tool;
- never "restore" V16 with a region-based tool. V16 has a different flash map, and those tools would truncate it.

**Before a public release:** the USB identity is still the pid.codes test VID:PID `1209:0001`, which every
fm1-base firmware shares, so a computer cannot tell CYMA-1 from another fm1-base build. It needs its own PID (and
fm1-base a `usb_pid` key in `fm1.toml`) before anyone else installs it.

The tools in this project never flash, install, OTA-update or send MIDI to hardware. Building the package is as far
as they go: every command above is one you run yourself.

## Layout

```
instrument/
  app.c app.h fm1.toml words.tsv web.c     app glue, the fm1-base contract, the page's exports
  app_sounds.c                             Sounds: the USB SysEx handler and the shared save/audition accessors
                                           (a unity part of app.c)
  src/      DSP and data, each an -O2 unit: plates_data.c (the solved modes), geom.c, engine.c voice.c bow.c, fx.c,
            params.c presets.c, cy_xfer.c (the Sounds codec: CRC-32, pack7, the record, names); the frozen
            interfaces cy_*.h
  ui/       the screen: plate, sand, views, chrome, overlays
  web/emu/  the browser page and its AudioWorklet
  tools/    gen_plates.py gen_fonts.py design_halfband.py package_web.py; Sounds: cyma_sounds.py (the CLI),
            cyma_local.py (the page on localhost), sx_bridge.mjs (the virtual FM-1), cyma1_params.json (generated);
            the test harness (golden.py, spec.py, emu_perf.py, wasm_smoke.mjs, chrome_test.mjs, artifact_test.mjs,
            sounds_smoke.mjs, sounds_chrome_test.mjs, test_cyma_sounds.py, render_demos.sh, contact_sheet.py,
            run_all.sh)
  tests/    scenario scripts (sounds_sysex.txt and sounds_safety.txt among them), demos/, unit/, plates/, sounds/
            (the frozen vectors and their generator; ring/, the USB send-ring harness)
  docs/     SPEC.md (generated), SOUNDS.md (the Sounds guide)
```

The design is specified in [docs/CYMA-1-spec.md](../docs/CYMA-1-spec.md).

## Licence and credits

**Produced by Team EG & Lizard_Man.**

CYMA-1 is MPL-2.0, file by file. It is built on **fm1-base** (MPL-2.0) and its felucca-bsp board support. The fonts
are **Barlow** and **Terminus**, both under the SIL Open Font License 1.1. No GPL code is included. Ernst Chladni
published *Entdeckungen über die Theorie des Klanges* in 1787.
