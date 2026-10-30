# Installing CYMA-1 on an FM-1, from a Mac

This guide installs CYMA-1 on an M-VAVE FM-1 over USB from a Mac. You don't need the RP2040 "Transporter" dongle or a
Linux machine. It covers:

1. [the first install from stock V15](#3-first-install-from-stock-v15), with a full backup of the unit's flash;
2. [updating from an earlier CYMA-1](#4-updating-from-an-earlier-cyma-1);
3. [going back to stock V15](#5-going-back-to-stock-v15);
4. [recovery](#6-recovery), if something goes wrong;
5. [what never to do](#7-never-whatever-a-tool-offers).

CYMA-1 was installed this way on a real FM-1 on 2026-10-29:
- v1.0 went on first, from stock V15, after a full backup read twice from macOS 26 on Apple silicon;
- v1.1.1, v1.1.2 and v1.1.3 went on later that day, as updates;
- the install itself takes about 2 minutes, and the user sounds on the unit survived every update.

**Tested on a real FM-1 so far:** the first install from stock V15 (section 3) and updates (section 4). **Going back
to stock V15 (section 5) follows the platform's own tools, but we have not yet run it on a real unit.** We will test
it on a second unit and update this page.

**V15 only.** CYMA-1 installs on an FM-1 running M-VAVE's **V15** firmware. M-VAVE released V16 on 2026-10-21, and
its own updater now installs V16. V16 changed the bootloader and the flash layout, and no way back from V16 has been
shown. If your unit reports `FM-1_016` (section 2), don't install CYMA-1 on it, and don't update a V15 unit to V16.

**How to use this page:**
- Read [section 0](#0-before-anything-the-rules) and [section 6](#6-recovery) once, before you start.
- On the day, work through your section in order.
- If anything differs from an **Expect:** line, stop and go to section 6.

Paths are examples. `~/FM1-backup` is the session folder, and `~/cyma-1` is a clone of this repository.

---

## 0. Before anything: the rules

1. **Never switch the FM-1 off while `UPDATING` is on its screen, or while any tool is writing.** A pulled cable, a
   sleeping Mac or a crashed tool is *not* a power cut. The FM-1 keeps running on its battery and waits. Run the same
   command again and it finishes.
2. **Answer `y` only when the package and the device are exactly the ones this page names.** For anything else,
   answer `N`. The installer does not refuse a V16 unit by itself.
3. **One FM-1, one cable, no MIDI apps.**
   - Connect exactly one FM-1, on a known-good USB-C data cable, straight into the Mac. No hub and no dock.
   - Quit Ableton Live and every other app that uses MIDI.
   - Close every browser tab with Web MIDI, including CYMA-1's own page.
4. **Keep the Mac awake.** Keep it on its charger, run `caffeinate`, and leave the lid open.
5. **Press Return after the last pasted line.** When you paste a block of commands into Terminal, the last line can sit
   at the prompt without running. Press Return, then wait for the prompt to come back before you read the result.

### What can go wrong, and what it costs

fm1-base's update loader has **no protection against a write that gets cut off**. These windows come from fm1-base's own
loader code, run in a model that cuts the power at each of the 2,432 flash operations of an install:

| Event | When | Result | Fix |
|---|---|---|---|
| Cable pulled, Mac sleeps or crashes, the tool stops | Any time | **Nothing lost.** The FM-1 waits in update mode on its battery | Replug and run the same command again: it finishes |
| The FM-1 loses power or resets | Before the write pass (the first climb of the percentage) | The old firmware boots | Start again |
| The same | **During the write pass**, its first few seconds | **Dark: black screen, nothing on USB** | Only the Transporter dongle |
| The same | Write pass, the middle | Restarts by itself a few times, then lands in **ROM mode** (USB `4C4A:8057`) | From the Mac with fm1-recover ([R5](#r5-rom-mode-4c4a8057)), if your backup proved it works |
| The same | Write pass, the last seconds | CYMA-1 boots | Run the install again |
| CYMA-1 crashes at start-up | After the install | **SAFE MODE** after 2 failed boots, USB still on | Back to V15 over USB ([R6](#r6-cyma-1-safe-mode)) |

A power loss during the write needs the battery to collapse or the chip to reset. With a charged battery that is
unlikely, but it is not impossible. **Flashing over USB cannot damage the hardware**: it only changes what is stored in
the flash chip.

What the install never writes:
- the boot area `0x0000–0x3FFF`, so the Transporter's way into ROM mode always stays open;
- the Bluetooth data at `0xE9000`;
- the factory voices at `0xEA000–0xFBFFF`;
- the key and MAC sector at `0xFF000`.

CYMA-1's loader writes only the app area, `0x4000–0x93000`. It does overwrite parts of V15's settings area, which your
backup keeps a copy of.

---

## 1. What you need

| Item | Version | sha256 | Where |
|---|---|---|---|
| **CYMA-1 package** | v1.1.3, identity `FM-1_94010103`, 611,496 B | `1c61abd4d1754a5c181cd60ed8895cf98b6d7970b1e42f3c84ddc480f0f413d3` | The release's `cyma-1-v1.1.3.fwsc`, or your own build ([BUILDING.md](../BUILDING.md): the same bytes) |
| **Stock V15** (the only image to go back to) | `FM-1_015` | `db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a` | M-VAVE's [fm1_open](https://github.com/m-vave/fm1_open): `sdk/firmware/FM-1_015.fwsc` |
| **This repository** | with its submodule: `git submodule update --init --recursive` | | `~/cyma-1` |
| Installer | fm1-base `086cd0c`, `bsp/tools/fm1_install.py` | `689e91bd558839a4636f5755e65840d43b4202b7bf172847e5c9fe831a491b6d` | `reference/fm1-base/bsp/tools/` in this repository. Use this one, never fm1-base's `tools/fm1_install.py` |
| Python | Python 3 (3.12 was used), with mido 1.3.3 and python-rtmidi 1.5.8 | | a virtual environment (1.1 below) |
| For the first install: **fm1-recover** (backup, ROM-mode rescue) | Melodee 1.5.1, macOS arm64 | zip `79239b682d00c8a277f23c06304d5ffb4f58648bf27bbcf26cf0482d84f6671a`, binary `958c986589a4c21b0c0068931e02ea33ee0037fffb54af50b8b30963ea6f0310` | [keremimo/melodee, release v1.5.1](https://github.com/keremimo/melodee/releases/tag/v1.5.1) |
| For the first install: **MvaveFM1Unbricker** (ROM key, dump analyser) | 1.2.0, commit `707470a` | `fm1_unbrick.py` `24c313181927199bb6abcbdb4fe5f20815a32aa1fe0ce85ccc5c2b0217dd613a`, `fm1_transporter_recover.py` `cd09c78c1acc6b03747d2890734443a76ad3618c9d29ebb6d285334750568a4f` | [Quixotic7/MvaveFM1Unbricker](https://github.com/Quixotic7/MvaveFM1Unbricker) |
| Optional: **M-Flasher** (a third read; an exact-factory restore later) | 0.6.1 | DMG `7c7a07f085ba2ff51fc8e14f460c1ebf4f21425a67780d5c2c811fe0cddba66a` | [jvitkauskas/m-flasher, release v0.6.1](https://github.com/jvitkauskas/m-flasher/releases/tag/v0.6.1) |

**Never** take a firmware from M-VAVE's own download link, M-UPGRADE or the Workbench: they serve V16 now
([section 7](#7-never-whatever-a-tool-offers)).

### 1.1 The session folder

Once, in Terminal:

```sh
mkdir -p ~/FM1-backup/firmware ~/FM1-backup/fm1-recover ~/FM1-backup/m-flasher ~/FM1-backup/recovery ~/FM1-tools
python3 -m venv ~/FM1-tools/venv
~/FM1-tools/venv/bin/pip install mido==1.3.3 python-rtmidi==1.5.8
cat > ~/FM1-backup/env.sh <<'EOF'
# CYMA-1 install session: run `source ~/FM1-backup/env.sh` in EVERY Terminal tab
export CYMA1="$HOME/cyma-1"                                    # your clone of this repository
export PY="$HOME/FM1-tools/venv/bin/python"
export INSTALL="$CYMA1/reference/fm1-base/bsp/tools/fm1_install.py"
export BACKUP="$CYMA1/reference/fm1-base/tools/fm1_backup.py"
export SOUNDS="$CYMA1/instrument/tools/cyma_sounds.py"
export UNBRICK="$HOME/FM1-tools/MvaveFM1Unbricker/fm1_unbrick.py"
export ANALYSE="$HOME/FM1-tools/MvaveFM1Unbricker/fm1_transporter_recover.py"
export R="$HOME/fm1-recover/fm1-recover-1.5.1-macos-arm64/fm1-recover"
export MF="/Applications/M-Flasher.app/Contents/MacOS/M-Flasher"
export FW="$HOME/FM1-backup/firmware"
export CYMA="$FW/cyma-1_1c61abd4.fwsc"
export V15="$FW/FM-1_V15_official.fwsc"
cd "$HOME/FM1-backup"
EOF
```

### 1.2 Pin the packages

Copy both packages into the session folder under fixed names, and check them:

```sh
source ~/FM1-backup/env.sh
cp ~/Downloads/cyma-1-v1.1.3.fwsc "$CYMA"        # or ~/cyma-1/instrument/build/cyma-1-1.1.3.fwsc from your own build
cp ~/Downloads/FM-1_015.fwsc "$V15"
cd "$FW" && shasum -a 256 -c <<'EOF'
1c61abd4d1754a5c181cd60ed8895cf98b6d7970b1e42f3c84ddc480f0f413d3  cyma-1_1c61abd4.fwsc
db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a  FM-1_V15_official.fwsc
EOF
```

**Expect:** `cyma-1_1c61abd4.fwsc: OK` and `FM-1_V15_official.fwsc: OK`.

- Nothing authenticates a `.fwsc`: the FM-1 installs whatever it is given, so the hash is your only proof.
- From here on, every command uses these pinned copies. A rebuild or a download later can't change what gets installed.
- **Keep the CYMA-1 package you are updating from** pinned next to it, for example `cyma-1_1554752c.fwsc` for
  v1.1.2. It is your known-good rescue image.

### 1.3 The room

- [ ] The FM-1 is **fully charged**. CYMA-1 shows no battery level, so charge before you install.
- [ ] Nothing is in the FM-1's TRS MIDI jack.
- [ ] If the FM-1 was ever paired over Bluetooth MIDI, disconnect it in Audio MIDI Setup → MIDI Studio → Bluetooth.
- [ ] Terminal.app has **three tabs**, each with `source ~/FM1-backup/env.sh`:
  - **Tab K** keeps the Mac awake for the whole session: `caffeinate -dims`. Press Ctrl+C at the very end;
  - **Tab A** runs the tools that need `sudo` (fm1-recover, M-Flasher);
  - **Tab B** runs the MIDI tools (`fm1_install.py`, `fm1_unbrick.py`, `cyma_sounds.py`).

---

## 2. What is running now

Switch the FM-1 on, wait for its screen, and plug it in. In Tab B:

```sh
"$PY" "$INSTALL" --info
```

It sends only the identity request `F0 00 32 45 00 00 00 40 7F F7`.

| It prints | What it means | Go to |
|---|---|---|
| `FM-1_015  [running]  port: FM-1` | Stock V15 | [Section 3](#3-first-install-from-stock-v15) |
| `FM-1_9400`, `FM-1_94010100`, `FM-1_94010101` or `FM-1_94010102  [running]  port: CYMA-1 FM-1` | CYMA-1 v1.0, v1.1, v1.1.1 or v1.1.2 | [Section 4](#4-updating-from-an-earlier-cyma-1) |
| `FM-1_94010103  [running]` | CYMA-1 v1.1.3 already | Nothing to do |
| `FM-1_016 …` | V16, or a firmware that reports V16 | **Stop for good on this unit.** Nobody has shown a way back from V16 |
| `ota-FM-1_900  [update loader (update not finished)]` | A CYMA-1 install did not finish | [R2](#r2-interrupted-while-the-fm-1-shows-updating) |
| `ota-FM-1_015 …` (or another `ota-FM-1…`) | M-VAVE's own updater did not finish | [R2](#r2-interrupted-while-the-fm-1-shows-updating), with the V15 file |
| `error: FM-1 not found …` | The cable, or another app has the port | Check the cable, close the apps, then retry with `--port "<the name Audio MIDI Setup shows>"` |

---

## 3. First install, from stock V15

**Time:** about 5 minutes of checks, 20 minutes of backup, 3 minutes of install, and 30 minutes of first-boot checks.

The install depends on four gates. Pass each one before you go on:

| Gate | Pass | If it fails |
|---|---|---|
| **G1 Package** | Both pinned files say `OK` (1.2) | Stop. Get the files again |
| **G2 Identity** | `--info` prints `FM-1_015  [running]` | `FM-1_016`: stop for good. `not found`: the cable, or close apps |
| **G3 Full backup** | Two 1 MiB reads with identical sha256, flash chip `0x856014`, and the analysis says `head == V15` | No backup possible: wait for the Transporter. Wrong flash chip: don't install CYMA-1 on this unit at all |
| **G4 Session** | FM-1 charged, Mac on its charger with `caffeinate`, MIDI apps closed, one FM-1 on a direct cable | Fix these first |

### 3.1 The tools, checked offline

fm1-recover:

```sh
source ~/FM1-backup/env.sh
mkdir -p ~/fm1-recover && cd ~/fm1-recover
shasum -a 256 ~/Downloads/fm1-recover-1.5.1-macos-arm64.zip      # 79239b68…
unzip -q ~/Downloads/fm1-recover-1.5.1-macos-arm64.zip
shasum -a 256 "$R"                                                # 958c9865…
"$R" --version
"$R" "$V15" --check
"$R" "$CYMA" --check
```

**Expect:**
- `FM-1 Recovery 1.5.1`;
- `Verified official M-VAVE V15.` for the V15 file;
- `Package checksums, chip key and application boundary verified.` for both packages.

`--check` never touches USB. If macOS blocks the binary, use **System Settings → Privacy & Security → Allow Anyway**.
Do that only after the sha256 matched.

MvaveFM1Unbricker:

```sh
git clone https://github.com/Quixotic7/MvaveFM1Unbricker ~/FM1-tools/MvaveFM1Unbricker
git -C ~/FM1-tools/MvaveFM1Unbricker checkout 707470a
shasum -a 256 "$UNBRICK" "$ANALYSE"                               # 24c31318…, cd09c78c…
"$PY" "$UNBRICK" softkey --dry-run --yes
```

**Expect:** `DRY RUN: using a MOCK FM-1 on MIDI` … `Key sent  F0 22 24 35 7D F7` … `DRY RUN: a real FM-1 would now
be in ROM boot mode`.

### 3.2 The full flash backup (gate G3)

V15's own ROM key puts the FM-1 into the chip's ROM mode, and fm1-recover, run with `sudo`, reads all 1 MiB of the
flash. Without `--write`, fm1-recover **never erases or writes**.

The dump is the only copy of this unit's own flash: V15's patches and settings, its factory app build, its Bluetooth
data and its factory voices. The run also proves that a ROM-mode tool works on your Mac, which turns "stuck in ROM mode"
from "wait for the dongle" into "fixable tonight".

**Read 1.** In **Tab A** (start it first: it waits up to 60 s):

```sh
sudo -v
sudo "$R" "$V15" --wait 60 --backup-dir ~/FM1-backup/fm1-recover
```

**Expect:** `Waiting for WL82 ROM UBOOT USB (4c4a:8057)...`

Then, within those 60 s, in **Tab B**:

```sh
"$PY" "$UNBRICK" softkey
```

**Expect:**
1. `Identity    FM-1_015 = M-VAVE stock firmware V15`.
2. `Type UBOOT to continue`: type **UBOOT** and press Return.
3. `Key sent    F0 22 24 35 7D F7`.
4. A line saying "This Mac cannot continue … move the cable". **Ignore it and leave the cable where it is:** fm1-recover
   in Tab A is the macOS continuation.

The FM-1's screen goes dark or freezes. If macOS says *"The disk you attached was not readable by this computer"*,
click **Ignore**. **Never click Initialize.**

**Tab A then shows** (in under a minute):

```
Connected to WL82 / UBOOT1.00. Loading recovery program into RAM...
Reading flash: 0%
…
Full flash backup verified on disk: …/FM1-backup/fm1-recover/2026…/full-flash-before.bin
143 application sectors differ; protected regions will be retained.
Backup complete. No flash changes. Power-cycle before using USB again; add --write to recover.
```

- The number of differing sectors may be lower, even 0.
- **Never add `--write` here.**

**Power-cycle.** Unplug, switch the FM-1 off, wait 5 s, switch it on, plug it back in, and run `"$PY" "$INSTALL" --info`.
**Expect:** `FM-1_015  [running]`.

**Read 2.** Repeat read 1 exactly, then power-cycle again and check `--info`.

**Compare:**

```sh
cd ~/FM1-backup && sudo chown -R "$USER" ~/FM1-backup
ls -l fm1-recover/*/full-flash-before.bin
shasum -a 256 fm1-recover/*/full-flash-before.bin
grep -h '"chip_key"' fm1-recover/*/backup.json
```

**Expect:** both files exactly `1048576` bytes, **the two sha256 values identical**, and `"chip_key": "0x980f"`.
- If they differ only between `0x93000` and `0xE8FFF`, V15 rewrote its own settings between the reads. Take a third
  read: you pass when two reads in a row match.
- If they differ anywhere else, try another cable or port and read twice again. **Don't install** until two reads match.

**Analyse the dump** (it only reads the file):

```sh
A=$(ls ~/FM1-backup/fm1-recover/*/full-flash-before.bin | head -1)
"$PY" "$ANALYSE" analyse "$A" --v15 "$V15" --package "$CYMA"
```

**Expect** `head == V15`, `no torn sector` and `erased sectors: 0`. **If the head differs from V15, stop.** That unit
has a boot area none of these tools was checked against.

**Optional, read 3 with M-Flasher.** This is a second, independent tool that checks its read against a CRC32 computed
by the chip. It is also the only Mac route that can later put back your **whole** flash, settings included. Install
the DMG and open M-Flasher once, then quit its window without touching any button. From then on, use it only from
Terminal:

```sh
sudo -v
"$MF" download-mode && sudo "$MF" backup ~/FM1-backup/m-flasher
```

**Expect** `Checked: the chip's CRC32 of the flash matches the read`, `flash 0x856014` and `Backup saved: …`. Then
power-cycle and check `--info`. Its `.bin` should have the same sha256 as the fm1-recover reads, unless V15 rewrote its
settings in between (the same rule as above).

**Keep three copies:**

```sh
sudo chown -R "$USER" ~/FM1-backup
cd ~/FM1-backup && find . -type f ! -name SHA256SUMS ! -path './logs/*' -print0 | xargs -0 shasum -a 256 > SHA256SUMS
cd ~ && zip -qr "FM1-backup-$(date +%Y%m%d).zip" FM1-backup
```

- Copy 1 stays in `~/FM1-backup`.
- Copy 2 goes to an external drive.
- Copy 3 goes to cloud storage.
- On each copy, run `shasum -a 256 -c SHA256SUMS`. Every line must say `OK`.
- **Never restore one unit's dump to another unit.**

**If the backup fails:**

| Message | What to do |
|---|---|
| `ROM UBOOT device not found` | The key came too late. Power-cycle, start Tab A again, then send the key |
| macOS grabbed the disk and the unit fell back to V15 | Make sure Tab A was already waiting, then retry |
| A USB error (access denied, busy, timeout) | Check `sudo`. Power-cycle, try another port and cable, and retry up to 3 times |
| **`Unexpected flash type or capacity; refusing recovery`** | **A second-source flash chip. Don't install CYMA-1 on this unit:** it could save nothing, and the recovery tools refuse it |
| `Chip-key mismatch` or `Unexpected ROM identity` | Stop. This is not the hardware these tools were checked against |

No backup at all from this Mac? **Wait for the Transporter.** Installing without one loses V15's patches, settings and
factory app build for good.

### 3.3 The install

Final checklist:
- [ ] G1: both pinned files say `OK`.
- [ ] G2: `--info` says `FM-1_015  [running]` **now**, after the last power-cycle.
- [ ] G3: two matching dumps, `head == V15`, three copies checked.
- [ ] The FM-1 is charged and switched on, on the direct cable, the only FM-1 connected.
- [ ] Tab K's `caffeinate -dims` is running.
- [ ] You can leave the FM-1, the cable and the Mac untouched for 3 minutes.

In Tab B:

```sh
source ~/FM1-backup/env.sh
shasum -a 256 "$CYMA"        # 1c61abd4d1754a5c181cd60ed8895cf98b6d7970b1e42f3c84ddc480f0f413d3
caffeinate -dims "$PY" "$INSTALL" "$CYMA"
```

**The prompt:**

```
package: FM-1_94010103  (…/FM1-backup/firmware/cyma-1_1c61abd4.fwsc)
device:  FM-1_015  (FM-1)
Install? Do not unplug the FM-1 while writing. [y/N]
```

Answer **y** only if it says exactly `package: FM-1_94010103` **and** `device:  FM-1_015`. Never add `--yes` or
`--force`.

### 3.4 What you see: two passes

| Stage | Terminal | FM-1 screen | Time | Safe to stop? |
|---|---|---|---|---|
| 1. The old firmware gets ready | `starting the update on …`, then `checking the package (N reads)` | The old firmware's screen | 1–3 s | Yes |
| 2. Switch to the loader | `switching to update mode` | Black, **`UPDATING`**, **`DO NOT UNPLUG`** and an empty bar | 3–30 s | Yes |
| 3. **Check pass** (reads only) | `writing   0%` climbing to about `99%` | The bar fills to about **half** | about 20–45 s | Yes: nothing is erased yet |
| 4. **Write pass: the risk window** | **The number drops back** to a few percent and climbs again to `99%` | The bar goes on **past half** to full | about 30–50 s | **Never switch off.** A pulled cable or a sleeping Mac is still fine: run the command again |
| 5. Finish | `writing 100%`, then `restarting` | CYMA-1 starts: the sand credit and Team EG's bell | a few s | The writing is done |
| 6. Check | **`done: the FM-1 runs FM-1_94010103`** | The CYMA-1 screen | up to 40 s | Done |

**The percentage runs twice** because it shows how far into the package the loader is reading. The loader reads the
whole package once to check it, then again, sector by sector, while it writes. The FM-1's own bar runs once over both
passes, and the real write starts at about half of it.

**The rule that matters:** once the terminal number has dropped back, or the FM-1's bar is past half, **don't touch
the power switch** until `done:` and the CYMA-1 screen appear. If anything stops, go to [R2](#r2-interrupted-while-the-fm-1-shows-updating).

### 3.5 First-boot checks

Use headphones with **MASTER low** at first. Don't hold REC, or OCT− and OCT+ together, while you switch on.

| # | Check | Expect |
|---|---|---|
| 1 | `"$PY" "$INSTALL" --info` | `FM-1_94010103  [running]  port: CYMA-1 FM-1` (macOS may show a cached port name; that is harmless) |
| 2 | Watch a boot | The sand spells TEAM EG & LIZARD_MAN as Team EG's bell rings, holds, bursts into the CYMA-1 title, then flows into the sand table and the tour (`PLAY A KEY`) |
| 3 | Play C4 on **01 SINGING BOWL**, then F3 to G5, soft and hard | A bronze bowl; all 27 keys sound, and velocity matters |
| 4 | Hold a key in the bottom 20 (F3–C5) and lean in | The pressure column moves; on BOW, the bow grips and sings. The top 7 keys have velocity only, by design |
| 5 | SELECT, ALGORITHM, PRESETS, K1–K4, MASTER | The shape morphs, the exciters and presets change, MASTER sets the volume |
| 6 | **GLO**: LOAD at the bottom of the right rail, with an 8-key chord on **21 AETHER** | The measured audio load. No crackle |
| 7 | SAVE → `SAVE TO U01?` → SAVE; switch off and on | U01 is there, and the last preset comes back |
| 8 | `"$PY" "$SOUNDS" info` | It names CYMA-1 and lists the slots |
| 9 | `"$PY" "$BACKUP" backup ~/FM1-backup/cyma-1-first.bak --magic CYM1` | A `.bak` file, exit 0 |

---

## 4. Updating from an earlier CYMA-1

This is the flow used for every update so far. **An update has the same risk windows as a first install**, because the
loader rewrites almost every code sector. Recovery is the same too ([section 6](#6-recovery)).

**1. Save CYMA-1's data** (Tab B):

```sh
source ~/FM1-backup/env.sh
"$PY" "$SOUNDS" backup                                                        # your sounds: ~/Music/CYMA-1/backups/
"$PY" "$BACKUP" backup ~/FM1-backup/cyma-1-presets-$(date +%Y%m%d-%H%M).bak --magic CYM1   # presets and settings
```

**2. Pin the new package and check it** (1.2). Keep the package you are updating from pinned too.

**3. Check what is running:**

```sh
"$PY" "$INSTALL" --info
```

**Expect** your current CYMA-1, for example `FM-1_94010102  [running]  port: CYMA-1 FM-1`.

**4. Install.** Run through the conditions in 1.3 (charged, one FM-1, apps closed, `caffeinate` in Tab K), then:

```sh
caffeinate -dims "$PY" "$INSTALL" "$CYMA"
```

```
package: FM-1_94010103  (…/FM1-backup/firmware/cyma-1_1c61abd4.fwsc)
device:  FM-1_94010102  (CYMA-1 FM-1)
Install? Do not unplug the FM-1 while writing. [y/N]
```

If you pasted the command and it sits on the command line without running, **press Return** to start it. Answer **y**
only if the package is `FM-1_94010103` and the device is the CYMA-1 `--info` just showed.

**5. Watch the two passes** (3.4). The number climbs to about 99 %, drops back, and climbs again. **From the drop until
`done:`, don't touch the power switch.**

**6. Check:**
- `"$PY" "$INSTALL" --info` → `FM-1_94010103  [running]`.
- `"$PY" "$SOUNDS" info` lists your sounds. An update keeps the user presets and settings, but you have the backups
  from step 1 if not.
- The boot plays the bell and the credit.

**What to check after v1.1.3 in particular:**

| # | Check | Expect |
|---|---|---|
| 1 | Turn MASTER fully down, then switch off and on | **Silence** through the whole boot. v1.1.2 played a short burst of the bell here |
| 2 | MASTER up a little, then switch off and on | The bell at the knob's level from its first strike |
| 3 | Press EDIT twice (MESH), wait about 40 s for the autosave, then switch off and on | The splash's sand flows into the **sand table** (SAND), not a cut to MESH. EDIT still reaches FIELD and MESH |
| 4 | Unplug and replug the USB cable while CYMA-1 runs, then `"$PY" "$INSTALL" --info` | It answers: USB comes back |

---

## 5. Going back to stock V15

**5.1 Save CYMA-1's data first**, as in section 4, step 1.

**5.2 The standard way, over USB** (from a running CYMA-1, or from its SAFE MODE screen). Use it only if your dump's
analysis said `head == V15`.

```sh
caffeinate -dims "$PY" "$INSTALL" "$V15"
```

**Expect** `package: FM-1_015` and `device:  FM-1_94010103`: answer `y`. It ends with **`done: the FM-1 runs
FM-1_015`**.

- **The write is done by M-VAVE's own closed loader.** CYMA-1 only hands over to it.
- The percentage may count down instead of up.
- That loader also rewrites the boot data below `0x4000`, with the same bytes when `head == V15`. Treat the whole run
  like an install: hands off until V15's screen appears.
- If it is interrupted, `--info` shows `ota-FM-1_015`: resume with the **V15** file.
- Afterwards V15 may show garbled patches or settings, because CYMA-1's store sits inside V15's settings area. Use
  **System → Reset Patches** on V15.

**5.3 Back to the exact factory state** (your V15 patches and settings), only if M-Flasher read the unit in 3.2.
Start from a running CYMA-1 and put it into ROM mode with its REC key, not the ROM key: switch the FM-1 off, then hold
**REC** while you switch it on, until the screen says `UPDATE MODE (REC)` and goes dark. Click **Ignore** on any disk
dialog. Then, in Tab A:

```sh
sudo -v
sudo "$MF" restore ~/FM1-backup/m-flasher/fm1-YYYYMMDD-HHMMSS.bin
```

Before you type anything, read the line `Writes N sectors (…)`:
- every range must start at `0x04000` or above;
- none may include `0xFF000`;
- if either check fails, type `no`. Otherwise type `yes`.

**5.4 The other way back, through ROM mode** (if the head differed, or the USB route fails). It never writes below
`0x4000`:
1. In Tab A, start `sudo "$R" "$V15" --write --wait 60 --backup-dir ~/FM1-backup/recovery`.
2. Within those 60 s, switch the FM-1 off, then hold **REC** while you switch it on, until the screen says
   `UPDATE MODE (REC)` and goes dark. That is fm1-base's own way into the chip's ROM mode, so the ROM key isn't needed.
3. Power-cycle when Tab A prints `RESTORE VERIFIED`.
4. Use Reset Patches on V15, as in 5.2.

---

## 6. Recovery

### R0. Five rules first

1. **Never switch the FM-1 off while `UPDATING` is on its screen, or while a tool is writing.**
2. **Resume with the same file you started with.**
   - `--info` says `ota-FM-1_900` (fm1-base's loader): resume with the CYMA-1 file.
   - It says `ota-FM-1_015` or another `ota-FM-1…`: that is M-VAVE's loader, so resume with the V15 file.
3. In ROM mode, click **Ignore** on macOS's disk dialog. **Never Initialize.**
4. **If it restarts by itself, don't power-cycle.** Wait 2 minutes. After 2 failed boots it goes to SAFE MODE, and after
   4 to ROM mode. Switching it off resets that count and starts the loop over.
5. Photograph the screen and copy the terminal output before you do anything else.

### Where to go

| You see | Go to |
|---|---|
| The installer stopped before `switching to update mode`; the old firmware still runs | R1 |
| `UPDATING / DO NOT UNPLUG` on the FM-1, and the installer stopped, the Mac slept or the cable came out | R2 |
| `writing 100%`, then `the FM-1 did not come back: power-cycle it` | R3 |
| The FM-1 went black, restarted or lost power **during** the write, or keeps restarting | R4 |
| USB shows `4C4A:8057` (`UBOOT1.00`), or macOS says the disk is unreadable | R5 |
| `CYMA-1 SAFE MODE … IT CRASHED TWICE WHILE STARTING` | R6 |
| CYMA-1 runs but something is wrong, or `RESTARTED AFTER A CRASH` | R7 |
| Black, and **nothing** of the FM-1 on USB for 2 minutes | R8 |
| `HARDWARE CALIBRATION`, `PRESS FX` | R9 |

To see what is on USB: `ioreg -p IOUSB -l -w0 | grep -E '"USB Product Name"|"idVendor"|"idProduct"'`.

| What is running | `idVendor` | `idProduct` | Name |
|---|---|---|---|
| Stock V15 | 19530 (`0x4C4A`) | 51029 (`0xC755`) | FM-1 … |
| CYMA-1 | 4617 (`0x1209`) | 1 | `CYMA-1 FM-1` |
| fm1-base's update loader | 4617 | 2 | `Felucca Update` |
| M-VAVE's update loader | 19786 (`0x4D4A`) | 16725 (`0x4155`) | `ota-FM-1…` |
| ROM mode | 19530 | 32855 (`0x8057`) | `UBOOT1.00` |

### R1. It stopped before the switch

Nothing was written. Fix the cause and run the install again.
- `FM-1 not found`: the cable, an app holding the port, or a cached port name (`--port`).
- `the FM-1 stopped answering … nothing was written`: replug and run it again.
- `the update loader did not appear`: if the FM-1 shows `UPDATING`, go to R2; otherwise run it again.
- A wrong-model or wrong-file message: stop, and check `shasum -a 256 "$CYMA"`.

### R2. Interrupted while the FM-1 shows UPDATING

1. **Don't switch the FM-1 off.** The loader runs from RAM and waits as long as it has power.
2. Replug the cable, then run `"$PY" "$INSTALL" --info`. **Expect** `ota-FM-1_900  [update loader (update not finished)]`.
3. Run the **same** install command again. It says `in update mode: the write will be finished`. Answer `y`. Sectors
   that already match are skipped, and it ends with `done: the FM-1 runs …`.
4. If `--info` finds nothing: wait 10 s and retry with `--port Felucca`, then try another port or cable. Restarting the
   Mac is fine too: the FM-1 stays in update mode on its battery. If the battery may be low, power the FM-1 from a USB
   charger meanwhile.

### R3. "writing 100%", then "did not come back"

The write finished and was verified; only the restart check timed out. Watch the screen for 2 minutes.
- If CYMA-1 shows, run `--info`.
- If it is still black with nothing on USB, **one** power-cycle is now safe, because the write is complete.

### R4. Power lost or the unit reset during the write

Don't touch it for 2 minutes. If the battery died, connect USB to charge and switch it on once. Then:
- **CYMA-1 boots:** run the same install again. It finishes the remaining sectors.
- **SAFE MODE:** R6.
- **ROM mode** (it gets there by itself after about 4 crash-restarts, 20–40 s): R5.
- **Black with nothing on USB after 2 minutes:** R8.

### R5. ROM mode (4C4A:8057)

Click **Ignore** on any disk dialog.

**A. Finish the install, or go back to V15, with fm1-recover** (try this first). It refuses a wrong chip, and it can't
touch the boot area. In Tab A:

```sh
sudo -v
sudo "$R" "$CYMA" --write --wait 60 --backup-dir ~/FM1-backup/recovery      # finish CYMA-1
# or, to go back to stock instead:
sudo "$R" "$V15"  --write --wait 60 --backup-dir ~/FM1-backup/recovery
```

Pick CYMA-1 if the cut came from outside (battery, power). Pick V15 if CYMA-1 itself seemed to be the problem.

**Expect:**
1. `Full flash backup verified on disk` (a fresh backup is saved **before** anything is erased).
2. `Restored and verified 1/N sectors` … `N/N`.
3. **`RESTORE VERIFIED. Bootloader and saved data unchanged. Power-cycle the FM-1 to start it.`**

Then switch it off, wait 5 s, switch it on and run `--info`.
- This write is unprotected too. If it stops during writing, don't switch off: run the same command again.
- If the unit drops out of ROM mode before the tool grabs it, start the tool first with `--wait 60`, then switch the
  FM-1 on.

**B. If fm1-recover can't claim it but M-Flasher read the unit in 3.2:** restore your own dump with
`sudo "$MF" restore ~/FM1-backup/m-flasher/fm1-YYYYMMDD-HHMMSS.bin`, with the plan-line check of 5.3.

**C. If neither tool can reach it,** it waits for the Transporter. Switch it off to save the battery.

### R6. CYMA-1 SAFE MODE

The screen says `CYMA-1 SAFE MODE`, `IT CRASHED TWICE WHILE STARTING. NO SOUND. USB IS ON`, and maybe shows a
`PC xxxxxxxx` line.
1. Photograph it, including the PC line.
2. CYMA-1 has no web installer: use the Mac. `--info` should show the CYMA-1 identity.
3. Then choose one:
   - **PLAY** tries CYMA-1 once more;
   - go back to V15 over USB (5.2), the recommended route after a real crash loop;
   - reinstall the previous, known-good CYMA-1 package;
   - **REC** enters ROM mode (R5). Use it only if USB-MIDI can't reach the unit.

### R7. It boots, but something is wrong

- `RESTARTED AFTER A CRASH` with a PC: it crashed once and recovered. Note the PC.
- Wrong sound, screen or keys: hold **SAVE + HOME while you switch on**. That boots with default settings and keeps
  your user presets. If it is still wrong, go back to the previous package or to V15.

### R8. Dark: black, nothing on USB

1. Check `ioreg` for all the IDs above. Try another cable and port.
2. Switch off, wait 10 s, switch on **once**, and watch for 2 minutes.
3. Still nothing: **stop.** Only the Transporter can reach a chip that never shows up on USB. The boot area was never
   written, so its route into ROM mode works. Wire it to D+, D− and GND, **never VBUS**, and follow MvaveFM1Unbricker's
   Transporter guide.

### R9. HARDWARE CALIBRATION

OCT− and OCT+ were held at power-on. That pair is also CYMA-1's PANIC chord. Do what the screen asks, or touch nothing
for 30 s and it keeps the old layout.

---

## 7. Never, whatever a tool offers

1. **V16, in any form.** That includes anything that downloads `FM-1.fwsc` from M-VAVE, which now serves V16:
   M-UPGRADE, the Workbench and rescue scripts that fetch the latest image.
2. **Anything that writes the boot area `0x0000–0x3FFF`.** No USB-only tool can repair it. That means:
   - M-Flasher's `bootloader`, `firmware`, `--with-bootloader` and `--wipe-settings`, and its window buttons;
   - the "also install the bootloader" option of any web installer;
   - the Workbench's Force flash;
   - M-UPGRADE's force mode.
3. Switching off during `UPDATING`, or while fm1-recover or M-Flasher writes.
4. `fm1_install.py --yes` or `--force`. Also fm1-base's other copy, `tools/fm1_install.py`: use `bsp/tools/fm1_install.py`.
5. `fm1-recover --write` outside R5 and 5.4; `M-Flasher --yes`; `run-ram`.
6. Restoring another unit's dump, or this unit's dump to another unit.
7. Clicking Initialize on the `UBOOT1.00` disk.
8. Hand-sent SysEx. `F0 22 24 35 7F F7` (update) and `F0 22 24 35 7D F7` (ROM key) differ by one byte. The ROM key is
   sent only for the backup in 3.2: by `softkey`, and by M-Flasher's `download-mode` for the optional third read.
   Everywhere else, this guide reaches ROM mode with CYMA-1's REC key: held at power-on, or on the SAFE MODE screen.
9. Holding REC, or OCT− and OCT+ together, while switching on, unless you mean to (as in 5.3 and 5.4). You get ROM
   mode or calibration. Both are harmless, but confusing in the middle of a session.
10. Two FM-1s connected at once.

---

## 8. Credits

- The installer, the update loader and `fm1_backup.py` are fm1-base's, by Charles Vestal, on felucca-bsp.
- The backup and the ROM-mode rescue are Melodee's fm1-recover.
- The ROM key and the dump analyser are MvaveFM1Unbricker's.
- The optional exact-factory restore is M-Flasher's.
- The power-cut windows in section 0 come from fm1-base's own loader code, compiled into a model that cuts the power at
  every flash operation of a V15-to-CYMA-1 install.
