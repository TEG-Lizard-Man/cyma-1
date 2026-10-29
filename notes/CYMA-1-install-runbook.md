# CYMA-1 first install on a real FM-1, from this Mac, without the Transporter

> **Outcome (2026-10-29):** this plan ran as written. The backup took two identical reads with fm1-recover 1.5.1 on an M4 Mac with macOS 26, the install took about 2 minutes, and CYMA-1 booted and passed the first-boot checks. Local paths in this copy are shortened to `<project>` and `~`.

2026-10-28. This is for the first CYMA-1 install on **FM-1 unit A** (stock V15), from the M4 Max MacBook (macOS 26.7),
with **no RP2040 Transporter** and no Linux machine. Nothing here was run against hardware: every command was checked
by reading its source, and by running the tool offline, in a mock or in a simulator.

**How to use this page:** read sections 0 and 6 once before the session. On the day, work through sections 1 to 5 in
order and tick each box. If anything differs from an **Expect:** line, stop and go to section 6.

For a dongle-free install this page replaces steps 3 and 5 of `instrument/README.md` ("Before you flash anything"),
which assume the Transporter. One correction to that README: its figure "2,414 of 2,432 cut points end in ROM mode"
belongs to TransitFM1's boot stub. fm1-base, and so CYMA-1, has no such stub (see 0.2).

---

## 0. Go / no-go

### 0.1 Recommendation: GO, with conditions

None of the five prep tracks found anything that blocks the install. The install depends on four gates, and you pass
or fail each one during the session:

| Gate | Pass | If it fails |
|---|---|---|
| **G1 Package** | `cyma-1.fwsc` has sha256 `1a3b809f5469b303eb2791eb225c17afb1121fbcf079e4ba20d60b94e60c7fc5` | Stop. Don't rebuild it yourself; ask. |
| **G2 Identity** | `fm1_install.py --info` prints `FM-1_015  [running]` | `FM-1_016`: stop for good on this unit. `not found`: fix the cable or close apps (section 2). |
| **G3 Full backup from this Mac** | Two 1 MiB reads with identical sha256, the flash chip is `0x856014`, the analysis says `head == V15` | No backup possible: **wait for the Transporter** (3.9). Wrong flash chip: **don't install CYMA-1 on this unit at all** (3.9). |
| **G4 Session conditions** | FM-1 fully charged, Mac on its charger with sleep blocked, MIDI apps closed, one FM-1 on a direct cable | Fix these first. |

**What each track found, in one line each:**
- **Build.** A clean rebuild reproduced the reviewed package byte for byte (`1a3b809f…7fc5`, 611,496 B, identity
  `FM-1_9400`). In the fm1-emu chip emulator it boots to the full UI and plays clean audio. The only flaw is two dropouts of
  about 3 ms each, in a synthetic extreme: a full-force bowed chord with DRIVE 24 dB and ROOM 100 % (`instrument/build/hwprep/`).
- **Safety.** The routing matrix, 1–2 million-frame fuzzes, the boot path, the safe mode and the flash-write windows
  all passed. One robustness test failed: a computer that floods requests **without waiting for replies** can make
  fm1-base's own replies wait for up to 200 ms. That wait is bounded, the watchdog is fed and audio keeps running. Our tools wait for every
  reply, so this cannot happen in this session. Don't run any other MIDI tools. The claim in REVIEW_REPORT §H.7 that CYMA-1
  "never makes the platform wait" is wrong; the send gate gets fixed in a later firmware round
  (`build-test/recheck-1028/`).
- **Recovery.** A full flash backup without the dongle is possible from this Mac. V15's own ROM key (sent by
  `fm1_unbrick.py softkey`) puts the unit into ROM mode, and Melodee's **fm1-recover 1.5.1** reads the flash. The
  same tool can later finish or undo an install from ROM mode. This was simulated end to end
  (`build-test/mac-recovery-vetting/`).
- **Loader.** fm1-base has **no protection against a write that gets cut off**. A pulled cable is harmless: you run the install again and it resumes. A
  power loss during the write is not; the windows are measured in 0.2 below (`build-test/install-risk/`).
- **Intel.** On 2026-10-28 someone recovered an FM-1 on macOS with fm1-recover 1.5.1 (github.com/charlesvestal/fm1-x0x/issues/38),
  but nobody has reported it on an M4 or on macOS 26, so **your backup run is the test**. Three new risks: a second
  flash-chip supplier, tools that write the boot area, and macOS caching MIDI port names. M-VAVE's URL still serves
  V16; never use it.

**Considered and not recommended:**
- **Going through TransitFM1 first.** Its first hop out of V15 is just as unprotected, it is third-party, and nobody has
  reported a TransitFM1-to-fm1-base install.
- **Rebuilding against the newer fm1-base (4587fda).** That would be an untested image, and its 18 new commits don't
  change the Python install path.
- **Installing through M-Flasher or any web installer.** Some of them can write the boot area.

### 0.2 The honest risk picture

**What should happen:** the backup takes about 20 minutes and changes nothing. The install itself takes about 2 minutes.
CYMA-1 starts with its sand splash and a "PLAY A KEY" tour.

**What can go wrong, and what it costs:**

| Event | When | Result | Fix |
|---|---|---|---|
| Cable pulled, Mac sleeps or crashes, the tool stops | Any time | **Nothing lost.** The FM-1 runs on its battery and waits in update mode | Replug and run the same command again: it finishes (93 of 93 simulated cable pulls finished bit-exact) |
| The FM-1 loses power or resets | Before the real write starts (about the first 30–60 s after you answer `y`, until the terminal percentage drops back) | V15 boots as before | Start again |
| The same | **During the write pass (about 25–50 s)**, first ~4–8 s | **Dark: black screen, nothing on USB** | **Only the Transporter.** Use unit B until it arrives |
| The same | Write pass, next ~14–30 s | The unit restarts by itself about 4 times, then lands in **ROM mode** (USB `4C4A:8057`) | From this Mac with fm1-recover, **if the backup proved it works** (R5) |
| The same | Write pass, last ~6–13 s | CYMA-1 boots | Run the install again |
| CYMA-1 crashes at start-up | After the install | **SAFE MODE** after 2 failed boots (USB still works) | Back to V15 over USB (R6) |

Those windows come from fm1-base's own loader code, compiled into a model that cuts power at each of the 2,432 flash
operations of a V15-to-CYMA-1 install. The model had 372 dark cut points, 1,428 that end in ROM mode and 631 where
CYMA-1 boots (`build-test/install-risk/out_A.summary`). A power loss during that window needs the battery to collapse or the chip to
reset. With a charged battery that is unlikely, but the risk is not zero.

**Worst realistic outcome:** the FM-1 sits dark, or in ROM mode with no Mac tool that can reach it, until the Transporter
arrives. You use unit B meanwhile.

**Why the Transporter can always fix it:** CYMA-1's update loader writes only the app area
`0x4000–0x93000`. It never writes the boot area `0x0000–0x3FFF`, so the dongle's route into ROM mode stays open.
Sources: `reference/fm1-base/bsp/loader/loader.c:19-21` and `ldr_core.c:15-19`.

**What the install never writes:**
- the Bluetooth data at `0xE9000`;
- the factory voices at `0xEA000–0xFBFFF`;
- the key and MAC sector at `0xFF000`.

**What it does overwrite:** parts of V15's own settings area. Your backup holds a copy of that.

**No physical or electrical damage is possible from flashing over USB.** Flashing only changes what is stored in the
flash chip. CYMA-1 sets the same supply voltages as the stock firmware (`fm1_power_stock`,
`reference/fm1-base/device/main_fm1.c:775-780`).

**What made other people's units dark, and how this page avoids each one:**
- **Unplugging, then switching off, mid-install** (fm1-x0x #38): rule R0.1.
- **A laptop that hibernated mid-install, then the unit switched off**: `caffeinate`.
- **A low battery**: charge it fully first.
- **V16, and boot-area writers** (Unbricker #6): forbidden list, section 8.

---

## 1. Before the session (nothing connected)

**Time:** about 30 minutes, the day before if you can.

### 1.1 What you need, with exact versions

| Item | Version | sha256 | Where it is |
|---|---|---|---|
| CYMA-1 package | `FM-1_9400`, built from fm1-base `086cd0c` + bsp `b413cd8` | `1a3b809f5469b303eb2791eb225c17afb1121fbcf079e4ba20d60b94e60c7fc5` | `instrument/build/hwprep/cyma-1.fwsc` (the same bytes as `instrument/build/cyma-1.fwsc`) |
| Stock V15 (the only restore image; M-VAVE no longer serves it) | `FM-1_015` | `db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a` | `firmware/stock/FM-1_V15_official.fwsc` |
| fm1-recover (backup and ROM-mode rescue) | Melodee 1.5.1, macOS arm64 (tag v1.5.1 = `6a608ba`) | zip `79239b682d00c8a277f23c06304d5ffb4f58648bf27bbcf26cf0482d84f6671a`, binary `958c986589a4c21b0c0068931e02ea33ee0037fffb54af50b8b30963ea6f0310` | `build-test/mac-recovery-vetting/downloads/` (from github.com/keremimo/melodee/releases/tag/v1.5.1; the hash matches its SHA256SUMS) |
| M-Flasher (**optional**: a third read, and an exact-factory restore later) | 0.6.1 (tag v0.6.1 = `67858a6`) | DMG `7c7a07f085ba2ff51fc8e14f460c1ebf4f21425a67780d5c2c811fe0cddba66a` (GitHub's digest; the release publishes no checksum file) | `build-test/mac-recovery-vetting/downloads/` (from github.com/jvitkauskas/m-flasher/releases/tag/v0.6.1) |
| Installer | fm1-base `086cd0c`, `bsp/tools/fm1_install.py` | `689e91bd558839a4636f5755e65840d43b4202b7bf172847e5c9fe831a491b6d` | `reference/fm1-base/` (**do not `git pull` it**) |
| ROM key sender | MvaveFM1Unbricker 1.2.0 (`707470a`), `fm1_unbrick.py` | `24c313181927199bb6abcbdb4fe5f20815a32aa1fe0ce85ccc5c2b0217dd613a` | `reference/MvaveFM1Unbricker/` |
| Dump analyser (offline) | the same clone, `fm1_transporter_recover.py` | `cd09c78c1acc6b03747d2890734443a76ad3618c9d29ebb6d285334750568a4f` | the same |
| Python | `.venv312`: Python 3.12.8, mido 1.3.3, python-rtmidi 1.5.8 | — | the project |

Nothing else needs installing. Homebrew, colima, Docker and emcc are not used on the day.

### 1.2 Set up the session folder (copy and paste)

```sh
mkdir -p ~/FM1-backup/firmware ~/FM1-backup/fm1-recover ~/FM1-backup/m-flasher ~/FM1-backup/recovery
cat > ~/FM1-backup/env.sh <<'EOF'
# CYMA-1 install session: run `source ~/FM1-backup/env.sh` in EVERY Terminal tab
export P="<project>"
export PY="$P/.venv312/bin/python"
export INSTALL="$P/reference/fm1-base/bsp/tools/fm1_install.py"
export UNBRICK="$P/reference/MvaveFM1Unbricker/fm1_unbrick.py"
export ANALYSE="$P/reference/MvaveFM1Unbricker/fm1_transporter_recover.py"
export R="$HOME/fm1-recover/fm1-recover-1.5.1-macos-arm64/fm1-recover"
export MF="/Applications/M-Flasher.app/Contents/MacOS/M-Flasher"
export FW="$HOME/FM1-backup/firmware"
export CYMA="$FW/cyma-1_1a3b809f.fwsc"
export V15="$FW/FM-1_V15_official.fwsc"
cd "$HOME/FM1-backup"
EOF
source ~/FM1-backup/env.sh
cp "$P/instrument/build/hwprep/cyma-1.fwsc" "$CYMA"
cp "$P/firmware/stock/FM-1_V15_official.fwsc" "$V15"
cd "$FW" && shasum -a 256 -c <<'EOF'
1a3b809f5469b303eb2791eb225c17afb1121fbcf079e4ba20d60b94e60c7fc5  cyma-1_1a3b809f.fwsc
db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a  FM-1_V15_official.fwsc
EOF
```

**Expect:** `cyma-1_1a3b809f.fwsc: OK` and `FM-1_V15_official.fwsc: OK`. This is **gate G1**. From here on, every command
uses these pinned copies, so a later rebuild in the project can't change what gets installed.

### 1.3 Unpack fm1-recover and check it offline

```sh
source ~/FM1-backup/env.sh
mkdir -p ~/fm1-recover && cd ~/fm1-recover
shasum -a 256 "$P/build-test/mac-recovery-vetting/downloads/fm1-recover-1.5.1-macos-arm64.zip"
unzip -q "$P/build-test/mac-recovery-vetting/downloads/fm1-recover-1.5.1-macos-arm64.zip"
shasum -a 256 "$R"
"$R" --version
"$R" "$V15" --check
"$R" "$CYMA" --check
```

**Expect** (these exact lines were produced on this Mac on 2026-10-28; `--check` never touches USB,
`reference/melodee/tools/fm1_recover.py:340-345`):

```
79239b682d00c8a277f23c06304d5ffb4f58648bf27bbcf26cf0482d84f6671a  …zip
958c986589a4c21b0c0068931e02ea33ee0037fffb54af50b8b30963ea6f0310  …/fm1-recover
FM-1 Recovery 1.5.1
Package: FM-1_015; SHA-256 db1642b2…db8a
Verified official M-VAVE V15. Recovery preserves saved-data bytes; stock firmware may change them after boot.
Package checksums, chip key and application boundary verified. Recovery loader checksum verified.
Package: FM-1_9400; SHA-256 1a3b809f…7fc5
Package checksums, chip key and application boundary verified. Recovery loader checksum verified.
```

If macOS blocks the binary ("cannot be opened"), go to **System Settings → Privacy & Security → Allow Anyway**. Only
after the sha256 matched, you can also run `xattr -dr com.apple.quarantine ~/fm1-recover`.

### 1.4 Rehearse the ROM key without hardware

```sh
source ~/FM1-backup/env.sh
"$PY" "$UNBRICK" softkey --dry-run --yes
```

**Expect:** `DRY RUN: using a MOCK FM-1 on MIDI (no real MIDI port is opened)` … `Identity  FM-1_015 = M-VAVE stock
firmware V15` … `Key sent  F0 22 24 35 7D F7` … `DRY RUN: a real FM-1 would now be in ROM boot mode`. It writes a log
under `~/FM1-backup/logs/`.

### 1.5 Optional: M-Flasher

Open `build-test/mac-recovery-vetting/downloads/M-Flasher-0.6.1-macos.dmg` and drag M-Flasher to Applications. Start it
once. If macOS blocks it, use **Privacy & Security → Open Anyway**, then **quit the window** without touching any button.

Then run `"$MF" --version`. **Expect:** `0.6.1`.

From here on you use M-Flasher **only from Terminal**, and only the commands this page names.

### 1.6 The FM-1, the Mac, the room

- [ ] **Label the units.** Unit **A** gets CYMA-1; unit **B** is the spare. On the day, only A is connected; B is
  switched off and away.
- [ ] **Charge unit A fully** beforehand. CYMA-1 shows no battery level (`instrument/README.md`, Before you flash
  anything, step 1), and the one documented mid-write reset was suspected to be a brown-out
  (`reference/ChoralRootFM1/docs/TRANSPORTER-HANDOFF.md`).
- [ ] **Cable:** a known-good USB-C **data** cable, the one that already worked with the FM-1, straight into a port on the
  Mac. No hub, no dock, no monitor passthrough. Nothing in the FM-1's TRS MIDI jack.
- [ ] **Mac:** on its charger, lid open, no macOS update waiting to restart. Turn on a Focus mode so notifications stay out of the way.
- [ ] **Quit:** Ableton Live and any other DAW, MIDI Monitor, the M-VAVE app and Workbench, every Chrome tab with Web MIDI
  (including CYMA-1's own page), and fm1-base's web pages (installer, backup, screen reader). Audio MIDI Setup may stay
  open: it only looks.
- [ ] **Bluetooth MIDI:** if you ever paired the FM-1 over Bluetooth MIDI, disconnect it in Audio MIDI Setup →
  MIDI Studio → Bluetooth. There must be exactly one FM-1 MIDI port.
- [ ] **Terminal:** use Terminal.app on the Mac itself (not over SSH). Open **three tabs** and run
  `source ~/FM1-backup/env.sh` in each:
  - **Tab K** keeps the Mac awake for the whole session: `caffeinate -dims`. Leave it running and press Ctrl+C at the
    very end. `-s` works only on mains power, so keep the charger in.
  - **Tab A** runs the root tools (fm1-recover, M-Flasher).
  - **Tab B** runs the MIDI tools (fm1_install.py, fm1_unbrick.py).
- [ ] Have your phone ready to photograph the screen if anything looks wrong.

**Time on the day:** about 5 min of checks, 20 min of backup, 3 min of install and 30 min of first-boot checks.

---

## 2. Read-only checks with the device

Switch unit A **on**, wait for its normal V15 screen, then plug it into the Mac.

**2.1 USB identity** (it reads the Mac's own USB list; nothing is sent to the FM-1):

```sh
ioreg -p IOUSB -l -w0 | grep -E '"USB Product Name"|"idVendor"|"idProduct"'
```

The list includes the Mac's own devices. Look for the FM-1 with these numbers:

| What is running | `idVendor` | `idProduct` | Name |
|---|---|---|---|
| **Stock V15 (expected now)** | 19530 (`0x4C4A`) | 51029 (`0xC755`) | FM-1 … |
| CYMA-1 | 4617 (`0x1209`) | 1 | `CYMA-1 FM-1` |
| fm1-base update loader (during the install) | 4617 | 2 | `Felucca Update` |
| M-VAVE update loader (when going back to V15) | 19786 (`0x4D4A`) | 16725 (`0x4155`) | `ota-FM-1…` |
| **ROM mode** | 19530 | 32855 (`0x8057`) | `WL80UBOOT1.00` / `UBOOT1.00` |

On macOS 26 the System Information equivalent is `system_profiler SPUSBHostDataType`; the old `SPUSBDataType` no longer
exists there. Or use **Apple menu → System Information → Hardware → USB**. Sources for the IDs:
`docs/research-ecosystem-2026-10-24.md` §5, `reference/fm1-base/bsp/loader/loader.c:5-14`, fm1-x0x #38.

**2.2 MIDI ports.** In Audio MIDI Setup (Window → Show MIDI Studio) you should see one device, `FM-1`. **Write down the
exact port name.** If any tool later says `not found`, pass that name with `--port`. The Python installer still lacks
the Mac port-name fix that fm1-base added to its web page on 2026-10-26.

**2.3 The firmware identity: gate G2.** In Tab B:

```sh
"$PY" "$INSTALL" --info
```

It sends only the identity request `F0 00 32 45 00 00 00 40 7F F7` (`bsp/tools/fm1_install.py:30,419-426`).

| It prints | Meaning |
|---|---|
| `FM-1_015  [running]  port: FM-1` | **Pass.** Carry on. |
| `FM-1_016 …` | **Stop.** V16 (or Echomatter's firmware). The installer would **not** refuse it, so never continue. |
| `error: FM-1 not found (USB data cable? another app using it?)` | Check the cable and close apps. Retry with `--port "<the name from 2.2>"`. |

**2.4 Optional, the Sounds tool against stock:**

```sh
"$PY" "$P/instrument/tools/cyma_sounds.py" info
```

**Expect:** `This FM-1 runs M-VAVE's own firmware (FM-1_015). Install CYMA-1 to send sounds.` It sends only the identity
request (`instrument/docs/SOUNDS.md`, checklist item 1).

---

## 3. Full flash backup from macOS: gate G3

**The method:** V15's own ROM key puts the FM-1 into the chip's ROM mode, and **fm1-recover 1.5.1** (run with sudo)
takes the device away from macOS's disk driver and reads all 1 MiB. Without `--write` it **never erases or writes**:
`recover()` returns right after saving the backup (`reference/melodee/tools/fm1_recover.py:271-292`). In simulation it
made 0 erases and 0 programs, and two sessions gave identical dumps
(`build-test/mac-recovery-vetting/evidence/melodee-sim-transcript.txt`, M1 and M2).

**Why it matters twice:** the dump is the only copy of this unit's own flash. It holds:
- V15's patches and settings;
- the factory 015 app build, which differs from the published V15 file;
- the Bluetooth data and the factory voices.

The run also **proves that a ROM-mode tool works on this exact Mac**. That turns the "ROM mode" row in 0.2 from "stuck
until the dongle" into "fixable tonight".

Do two reads in two sessions, with a power-cycle between them, then compare them.

### 3.1 Read 1

**Tab A** (start this first; it waits up to 60 s):

```sh
sudo -v
sudo "$R" "$V15" --wait 60 --backup-dir ~/FM1-backup/fm1-recover
```

**Expect:**
```
Package: FM-1_015; SHA-256 db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a
Verified official M-VAVE V15. Recovery preserves saved-data bytes; stock firmware may change them after boot.
Recovery session: ~/FM1-backup/fm1-recover/2026…Z-xxxxxxxx
Waiting for WL82 ROM UBOOT USB (4c4a:8057)...
```

**Tab B**, within those 60 s:

```sh
"$PY" "$UNBRICK" softkey
```

**Expect:**
- `FM-1 port   out 'FM-1', in 'FM-1'`
- `Identity    FM-1_015 = M-VAVE stock firmware V15`
- the warning text (`…Nothing is written to it. Switching it off and on brings it back.`)
- `Type UBOOT to continue (anything else cancels):`. Type **UBOOT** and press Return.
- `Key sent    F0 22 24 35 7D F7`
- then a line saying "This Mac cannot continue … move the cable to a Windows or Linux machine". **Ignore it and don't
  move the cable:** fm1-recover in Tab A is the macOS continuation. (Source: `fm1_unbrick.py:1928-2000`.)

**The FM-1:** its screen goes dark or freezes, and the keys stop responding. If macOS says *"The disk you attached was
not readable by this computer"*, click **Ignore**. **Never** click Initialize.

**Tab A then shows** (under a minute):
```
Connected to WL82 / UBOOT1.00. Loading recovery program into RAM...
Reading flash: 0%
…
Reading flash: 87%
Full flash backup verified on disk: ~/FM1-backup/fm1-recover/2026…/full-flash-before.bin
143 application sectors differ; protected regions will be retained.
Backup complete. No flash changes. Power-cycle before using USB again; add --write to recover.
```
- `143 … differ` is normal: the factory 015 build is not byte-equal to the published V15 file. A smaller number, even 0, is
  fine too.
- **Never add `--write` here.**
- The FM-1 may restart into V15 by itself a few seconds later. That is fine.

### 3.2 Power-cycle

Unplug the cable, switch the FM-1 off, wait 5 s, switch it on, and plug the cable back in. Then in Tab B run
`"$PY" "$INSTALL" --info`. **Expect:** `FM-1_015  [running]  port: FM-1`.

### 3.3 Read 2

Repeat 3.1 exactly. It creates a second session folder. Then power-cycle again as in 3.2 and check `--info`.

### 3.4 Compare the two reads

```sh
cd ~/FM1-backup
sudo chown -R "$USER" ~/FM1-backup
ls -l fm1-recover/*/full-flash-before.bin
shasum -a 256 fm1-recover/*/full-flash-before.bin
grep -h '"chip_key"\|"backup_sha256"' fm1-recover/*/backup.json
```

**Expect:**
- both files are exactly `1048576` bytes;
- **the two sha256 values are identical**;
- `"chip_key": "0x980f"`.

**If the hashes differ,** find out where:

```sh
"$PY" - fm1-recover/*/full-flash-before.bin <<'EOF'
import sys
a, b = (open(p, 'rb').read() for p in sys.argv[1:3])
d = [i for i in range(0, max(len(a), len(b)), 4096) if a[i:i+4096] != b[i:i+4096]]
print(len(a), len(b), 'differing 4 KiB sectors:', ' '.join(hex(x) for x in d) or 'none')
EOF
```

- **Differences only between `0x93000` and `0xE8FFF`** (V15's settings area, which V15 can rewrite when it boots between
  the two reads): take a third read (3.1 + 3.2). You pass when two consecutive reads match.
- **Differences anywhere else:** try another cable or port, then repeat both reads. **Don't install** until two reads
  match.

### 3.5 Check the dump offline

```sh
A=$(ls ~/FM1-backup/fm1-recover/*/full-flash-before.bin | head -1)
"$PY" "$ANALYSE" analyse "$A" --v15 "$V15" --package "$CYMA"
```

It reads the file only (`fm1_transporter_recover.py:1538-1547`). **Expect** under (a) `head == V15`; under (b)
`no torn sector` and `erased sectors: 0`; and under (f) Verdict `head == V15` and `no torn sector`.

**If it says the head differs from V15, stop.** The unit then has a boot area that none of the tools was checked
against (V16 changed the bootloader). Ask before going further.

### 3.6 Optional, recommended: read 3 with M-Flasher

This gives you a read from a second, independent tool that checks itself against a CRC32 computed by the chip. It also proves the only Mac route
that can put back your **whole** flash, settings included (R5-B, 7.3). Skip it if M-Flasher misbehaves; it is not
needed for the install.

**Tab A:**
```sh
"$MF" find
```
**Expect:** `midi	FM-1 (running; download-mode switches it)`.

```sh
sudo -v
"$MF" download-mode && sudo "$MF" backup ~/FM1-backup/m-flasher
```

**Expect:**
- `Sent the FM-1 the download-mode message over USB MIDI; waiting for it`
- `In download mode: …	WL82 UBOOT1.00 …`
- `Starting the M-Flasher loader 1.2`, then `Reading the flash`
- **`Checked: the chip's CRC32 of the flash matches the read`**
- `model FM-1`, `bootloader stock (M-VAVE)`, `firmware FM-1 015 (stock)`, `app OK`, `chip key 0x980F`,
  **`flash 0x856014`** and `unit id <16 hex digits>`
- `Backup saved: …/fm1-YYYYMMDD-HHMMSS.bin`, with a `.json` beside it.

macOS may ask whether Terminal may access a removable volume: click **Allow**. If it says the disk is unreadable, click
**Ignore**. Afterwards, unplug the FM-1, switch it off and on, and check `--info`. Then compare the files:
`shasum -a 256 ~/FM1-backup/m-flasher/*.bin` should equal the fm1-recover hash. If V15 rewrote its settings between the
reads, use the 3.4 rule.

If `download-mode` prints "did not come back in download mode within 20 s", or M-Flasher can't claim the unit,
power-cycle the FM-1 and drop M-Flasher for this session. Note what it said.

M-Flasher's simulated run is in `build-test/mac-recovery-vetting/evidence/mflasher-sim-transcript.txt`, scenario S1.

### 3.7 Keep three copies

```sh
sudo chown -R "$USER" ~/FM1-backup
cd ~/FM1-backup && find . -type f ! -name SHA256SUMS ! -path './logs/*' -print0 | xargs -0 shasum -a 256 > SHA256SUMS
cd ~ && zip -qr "FM1-backup-unitA-$(date +%Y%m%d).zip" FM1-backup
```

1. **Copy 1** stays in `~/FM1-backup` on the Mac.
2. **Copy 2** goes to an external drive or USB stick.
3. **Copy 3** goes to cloud storage (iCloud Drive or similar).

On each copy, unzip it and run `shasum -a 256 -c SHA256SUMS`; every line must say `OK`. Keep each `.json` next to its
`.bin`.

**Never restore this dump to unit B, and never restore another unit's dump to A.** M-Flasher treats a foreign dump
without its `.json` as the same unit (simulation S5).

### 3.8 If something goes wrong during the backup

Every retry is harmless: the backup never writes, and a power-cycle brings V15 back.

| Message or symptom | What to do |
|---|---|
| `Recovery stopped: ROM UBOOT device not found…` | The key came too late, or the unit didn't switch. Power-cycle, start Tab A again, then send the key. |
| `softkey` refuses: several FM-1 ports, or no identity answer | Close the other apps, disconnect Bluetooth MIDI, and pass `--port "FM-1"`. |
| macOS shows the unreadable-disk dialog and the unit drops back to V15 by itself | macOS grabbed it first. Make sure Tab A was **already waiting**, then retry. |
| `Recovery stopped:` with a USB error (access denied, busy, pipe, timeout) | Check that you used `sudo`. Power-cycle, try another port and cable, and retry up to 3 times. Then try 3.6 (M-Flasher). |
| **`Recovery stopped: Unexpected flash type or capacity; refusing recovery`** | **A second-source flash chip** (`0xB36014`; sloop-fm1 #228). **Don't install CYMA-1 on this unit:** CYMA-1 would save nothing ("RAM only", `main_fm1.c:782`), and fm1-recover and both Transporter tools refuse such a unit. If you like, take an M-Flasher backup (3.6) for safekeeping; it only treats the ID as a hint. Then go to unit B and start again at section 2. |
| `Recovery stopped: Chip-key mismatch…` or `Unexpected ROM identity` | Stop. This is not the hardware everything was checked against. |
| `Multiple ROM devices found` | Only one FM-1 may be connected. |

### 3.9 The decision

| Outcome | Decision |
|---|---|
| Two matching reads, `0x856014`, `head == V15`, 3 copies verified | **G3 passed: go to section 4.** |
| No full backup possible from this Mac (neither fm1-recover nor M-Flasher could read it) | **Recommendation: wait for the Transporter** (see below). |
| Wrong flash chip | Don't install CYMA-1 on this unit. Try unit B. |
| Head differs from V15 | Stop and ask. |

**Why wait if there is no backup?** It costs a few days.
- Installing without a backup **permanently** loses V15's patches and settings on this unit (the install overwrites parts
  of that area) and the factory app build.
- If no ROM tool works on this Mac, about 15–30 s of the install's write window would end "stuck until the Transporter"
  instead of "fixable tonight".
- The Transporter makes the same backup in about 3 seconds (`instrument/README.md` step 3).

If you still choose to go ahead, write down that you accepted those two losses.

---

## 4. The install

### 4.1 Final checklist (all must be ticked)

- [ ] G1: both pinned files say `OK` (1.2).
- [ ] G2: `--info` says `FM-1_015  [running]` **now**, after the last power-cycle.
- [ ] G3: two matching dumps, 3 copies verified.
- [ ] FM-1 battery full, power switch ON, on the direct data cable, the only FM-1 connected.
- [ ] Tab K's `caffeinate -dims` is running, the Mac is on its charger and the lid is open.
- [ ] Every MIDI app is closed, and there is no Bluetooth MIDI connection to the FM-1.
- [ ] You have read **R0 and R2** in section 6.
- [ ] You can leave the FM-1, the cable and the Mac untouched for 3 minutes.

### 4.2 The command (Tab B)

```sh
source ~/FM1-backup/env.sh
shasum -a 256 "$CYMA"        # must print 1a3b809f5469b303eb2791eb225c17afb1121fbcf079e4ba20d60b94e60c7fc5
caffeinate -dims "$PY" "$INSTALL" "$CYMA"
```

- **Never** add `--yes` or `--force`.
- Use **this** installer, `bsp/tools/fm1_install.py`. fm1-base's other copy, `tools/fm1_install.py`, would accept V16
  without `--force` (installer simulation, case I).

**The prompt:**
```
package: FM-1_9400  (~/FM1-backup/firmware/cyma-1_1a3b809f.fwsc)
device:  FM-1_015  (FM-1)
Install? Do not unplug the FM-1 while writing. [y/N]
```

Answer **y** only if it says exactly `package: FM-1_9400` **and** `device:  FM-1_015`. For anything else, answer **N**
(it then prints `cancelled` and nothing is sent but the identity request). The installer does **not** refuse an
`FM-1_016` device (`build-test/install-risk/verify_only_mock.py`).

### 4.3 What you see, stage by stage

| Stage | Terminal | FM-1 screen | Time (default model; slow host in brackets) | Safe to stop? |
|---|---|---|---|---|
| 1. V15 gets ready | `starting the update on FM-1_015`, then `checking the package (N reads)` counting up | V15's own screen (closed code; it may not change) | ~1–3 s | Yes. A cut here boots V15 |
| 2. Switch to the loader | `switching to update mode` | Black, **`UPDATING`**, **`DO NOT UNPLUG`** and an empty bar (`bsp/loader/ldr_screen.c:125-127`) | 3–30 s | Yes |
| 3. Loader **check pass** (reads only, writes nothing) | `writing   0%` climbing to about `99%` | The bar fills to about **half** | ~21 s (43 s) | Yes: nothing is erased yet. A power cut boots V15; a pulled cable resumes |
| 4. Loader **write pass: THE RISK WINDOW** | **The number drops back to a few percent** and climbs again to `99%` | The bar goes on **past half** to full | ~30 s (51 s) | **Never switch off.** A pulled cable or a sleeping Mac is still fine: run the command again |
| 5. Finish | `writing 100%`, then `restarting` | CYMA-1 starts: black, the sand splash, the `CYMA-1` title, the plate fading in | a few s | Writing is complete |
| 6. Check | **`done: the FM-1 runs FM-1_9400`** | The CYMA-1 UI with the tour chip **`PLAY A KEY`** | ≤ 40 s | Done |

**Total:** about 1.5–2.5 minutes. Sources: `build-test/install-risk/out_A*.summary` and the loader track's timeline;
`bsp/tools/fm1_install.py:310-352`.

**Why the percentage runs twice:** it shows how far into the package the loader is reading
(`fm1_install.py:336`). The loader reads the whole package once to check it, then again sector by sector while writing.
The FM-1's own bar runs once over both passes (`ldr_core.c:29`), and the real write begins at about **51 %** of it.

**The rule that matters:** once the terminal number has dropped back, or the FM-1's bar is past half, **don't touch
the power switch** until `done:` and the CYMA-1 screen appear. If anything stops, go to R2. Don't switch off.

(If you really must abort, do it only during the **first** climb, with the bar under half: press Ctrl+C, then switch the
FM-1 off and on, and V15 comes back. It is better not to.)

### 4.4 Right after `done:`

In Tab B run `"$PY" "$INSTALL" --info`. **Expect:** `FM-1_9400  [running]  port: CYMA-1 FM-1`. The port name may
differ if macOS cached another name, and that is harmless. The `ioreg` line from 2.1 should show `CYMA-1 FM-1`,
`idVendor` 4617 and `idProduct` 1.

CYMA-1 is not affected by the first-boot-after-V15 bug in felucca-bsp #17: its boot-guard check is a full 32-bit compare
(`instrument/build/cyma-1.dis` around `0x2015a92`).

---

## 5. First-boot checks (about 30 minutes)

Use headphones with **MASTER low** at first. MASTER comes after the limiter, so turning it down never changes the sound's
character. **Don't** hold REC, or OCT− and OCT+ together, while switching on (8.1).

| # | Check | How | Expect |
|---|---|---|---|
| 1 | Screen | Watch the boot | The sand splash, the `CYMA-1` title, then the plate and the tour (`PLAY A KEY`, then `LEAN IN: PRESS HARDER`, `TURN SELECT: SHAPE`…). The sand is **sparser than in the browser on purpose** (1,200 grains against 2,000; `src/params.c:70-76`) |
| 2 | Sound | Play C4 on preset **01 SINGING BOWL** | A bronze bowl. C4 measured 261.5 Hz in the emulator |
| 3 | All keys | Play F3 to G5, soft and hard | Every one of the 27 keys sounds; velocity matters |
| 4 | **Pressure** | Hold a key in the **bottom 20 (F3–C5)** and lean in. Then ALGORITHM → **BOW** and lean in | Preset 01 bends a little sharp; the pressure column in the right rail moves; the bow "grips and sings". The **top 7 keys have velocity only**: that is by design |
| 5 | Encoders | SELECT, ALGORITHM, PRESETS, K1–K4, MASTER | Shape morphs (e.g. `HEX › SQUARE 40%`); MALLET → BOW → RAIN; presets 01–24; values change; volume |
| 6 | Buttons | HOME ENV FX LFO GLO, SEQ (SURFACE), PLAY (FREEZE), ARP (LATCH), EDIT (view), OCT± (both together: PANIC) | As in `instrument/README.md`, Buttons |
| 7 | **LOAD / CPU** | Press **GLO** (the GLOBAL page). The figure is at the **bottom of the right rail**, e.g. `34%`. Read it at idle, with one note, with an 8-key chord on **21 AETHER**, and pressing hard on **08 STEEL RAIN** | It is the measured audio load. It also shows on any page once it reaches 70 %, and turns red from 90 % (`ui/chrome.c:276-296,354-358`). The emulator gave a mean of about 49 % in play and slowest blocks up to 89 %. The real chip can only be slower (no cache model) |
| 8 | Worst case (optional) | **23 BOWED SQUARE**: FX → ROOM 100, SIZE 12 s, DRIVE 24 dB; play a full-force chord | The emulator showed 2 late blocks (~3 ms each) at the bow's onset. Note whether you hear clicks |
| 9 | Save and reboot | SAVE → `SAVE TO U01?` → SAVE. Switch off and on; turn PRESETS past 24 | U01 is there, and the last preset comes back. This proves the flash store works |
| 10 | **Sounds over USB (CLI)** | `"$PY" "$P/instrument/tools/cyma_sounds.py" info`, then `get all` | It names CYMA-1 and lists the slots; a bank file is written |
| 11 | Sounds: audition | Hold a note, then `"$PY" "$P/instrument/tools/cyma_sounds.py" audition <a cy1. code>`. Use one from the page's Share, or run `encode` on a saved file | The FM-1 glides to it, and the footer shows `FROM USB` |
| 12 | **Sounds: the localhost page** | `python3 "$P/instrument/tools/cyma_local.py"`: Chrome opens `http://localhost:8137`. **Connect to FM-1**, and allow MIDI when Chrome asks | `Connected: CYMA-1 on the FM-1 · N of 32 slots used.` |
| 13 | Sounds: send, save and decline | In the page: Save sound (⌘S) as `TEST ONE`, then on its row choose **Send to FM-1** → U02. On the FM-1, `USB: SAVE TO U02?` / `SAVE=YES HOME=NO`: press **SAVE** (`SAVED`). Send it again and press **HOME** this time | The first gives `SAVED` and **Get sounds from FM-1** shows it; the second gives `USB` / `NOTHING CHANGED` (`instrument/docs/SOUNDS.md`) |
| 14 | USB audio | Audio MIDI Setup → Audio Devices | Whether a 2-in `CYMA-1 FM-1` input appears. It is reported to work on macOS 26 (fm1-loom #7) |
| 15 | Back up CYMA-1's state | `"$PY" "$P/reference/fm1-base/tools/fm1_backup.py" backup ~/FM1-backup/cyma-1-first.bak --magic CYM1` | A `.bak` file (exit 0) |

Rules for steps 10–15:
- Keep Live **closed** for this first test.
- Use only `cyma_sounds.py`, the localhost page and `fm1_backup.py`. They all wait for each reply.
- Don't open fm1-base's screen-reader page or any flood tool (0.1, Safety).

**What to report back** (paste this and fill it in; attach photos and videos of anything odd):

```
Unit A, CYMA-1 1a3b809f, installed YYYY-MM-DD
Backup: tool(s) that worked = fm1-recover yes/no, M-Flasher yes/no/skipped; dump sha256 = …; flash = 0x856014; head == V15 yes/no
Install: total time …; anything unusual in the terminal …; exit code …
--info after install: …
Screen: colours / tearing / glitches …
Keys: all 27 sound yes/no; velocity feel …; key-to-sound delay feels OK / laggy
Pressure (bottom 20 keys): works yes/no; feel …
LOAD %: idle … / one note … / 8-key AETHER chord … / STEEL RAIN pressed … / BOWED SQUARE worst case … (any red?)
Clicks or dropouts: none / when …
Save + reboot: U01 kept yes/no
Sounds CLI: info …; audition …; get all …   Page: connect …; send+SAVE …; HOME declines …
USB audio input appears: yes/no
Any "RESTARTED AFTER A CRASH" (PC …) or SAFE MODE (PC …): …
Battery / charging behaviour: …
```

---

## 6. Recovery decision tree

### R0. Five rules first

1. **Never switch the FM-1 off while `UPDATING` is on its screen, or while any tool is writing.** A pulled cable, a
   sleeping Mac or a crashed tool is **not** a power cut: the FM-1 keeps running on its battery.
2. **Resume with the same file you started with.** If `--info` says `ota-FM-1_900`, resume with the CYMA-1 file; if it
   says `ota-FM-1_015` (or any other `ota-FM-1…` that is not `_900`: M-VAVE's own loader), with the V15 file.
3. In ROM mode, click **Ignore** on macOS's disk dialog. **Never Initialize.**
4. **If it restarts by itself, don't power-cycle.** Wait 2 minutes. The boot guard walks it to SAFE MODE after 2 failed
   boots and to ROM mode after 4 (`main_fm1.c:950-964`). Switching off clears that count (it lives in RAM) and starts the
   loop over.
5. Photograph the screen and copy the terminal output before you do anything else.

### What you see, and where to go

| You see | Go to |
|---|---|
| The installer stopped before `switching to update mode`; the FM-1 shows V15 | **R1** |
| `UPDATING / DO NOT UNPLUG` on the FM-1, and the installer stopped, the Mac slept or the cable came out | **R2** |
| `writing 100%` was printed, then `the FM-1 did not come back: power-cycle it` (exit 5) | **R3** |
| The FM-1 went black, restarted or lost power **during** the write, or keeps restarting | **R4** |
| The `ioreg` line shows 19530 / 32855 (`4C4A:8057`, `UBOOT1.00`), or macOS says the disk is unreadable | **R5** |
| `CYMA-1 SAFE MODE … IT CRASHED TWICE WHILE STARTING` | **R6** |
| CYMA-1 runs but something is wrong, or `RESTARTED AFTER A CRASH` | **R7** |
| Black, and **nothing** of the FM-1 on USB for 2 minutes | **R8** |
| `HARDWARE CALIBRATION`, `PRESS FX` | **R9** |
| `UPDATE MODE (REC)` or `UBOOT (USB)`, then black, on a unit that was fine | ROM mode on purpose or by accident. Switch off and on: nothing was written (`fm1_sys.h:225-240`) |

### R1. It stopped before the switch

Nothing was written. Fix the cause and run 4.2 again:

| Message | Meaning and fix |
|---|---|
| `error: FM-1 not found …` (exit 3) | The cable, or another app has the port, or a cached port name. Use `--port "<name from 2.2>"` |
| `the device is …, the package is for …` (exit 6) | Wrong model. Stop |
| `… no update loader of this family in this package …` (exit 2) | Wrong file. Check `shasum -a 256 "$CYMA"` |
| `the FM-1 stopped answering after N requests; nothing was written` (exit 4) | Replug and run it again |
| `the update loader did not appear. Replug the USB cable and run the install again …` (exit 5) | If the FM-1 shows `UPDATING`, go to **R2**. If it shows V15, run it again |

### R2. Interrupted while the FM-1 shows UPDATING

`bsp/tools/fm1_install.py:338-342` prints `error: the loader was disconnected after N requests. Replug the USB cable and
run the install again to finish` (exit 4).

1. **Don't switch the FM-1 off.** The loader runs from RAM with its watchdog fed. It waits as long as it has power
   (`bsp/loader/loader.c:85-116`).
2. Replug the cable (direct, the same port). In Tab B:
   ```sh
   "$PY" "$INSTALL" --info
   ```
   **Expect:** `ota-FM-1_900  [update loader (update not finished)]  port: Felucca Update`.
3. Run the **same** install command:
   ```sh
   caffeinate -dims "$PY" "$INSTALL" "$CYMA"
   ```
   **Expect:** `device:  ota-FM-1_900  (Felucca Update)  in update mode: the write will be finished`. Answer `y`. Sectors
   that already match are skipped, then `writing … 100%`, `restarting`, `done: the FM-1 runs FM-1_9400`.
   Simulated: `build-test/mac-recovery-vetting/evidence/installer-sim-transcript.txt` D, E and E2.
4. If `--info` finds nothing:
   - check that `ioreg` shows 4617 / 2;
   - wait 10 s, then retry with `--port Felucca`;
   - try another port or cable.

   Restarting the Mac is OK too: the FM-1 stays in update mode on its battery. Keep the screen showing `UPDATING`, and
   if the battery may be low, power the FM-1 from a USB charger meanwhile.

### R3. "writing 100%", then "did not come back"

The write finished and was verified. Only the restart check timed out.
1. Watch the screen for 2 minutes. If you see the CYMA-1 UI, run `--info`. SAFE MODE: R6. ROM mode: R5.
2. If it is still black with nothing on USB, **one** power-cycle is now safe, because the write is complete. Then watch again.
   If it stays dark: R8.
3. Exit 6, `written, but the FM-1 reports X`: copy the message and stop.

### R4. Power lost or the unit reset during the write

Don't touch it for 2 minutes. If the battery died, connect USB to charge and switch it on once. Then see which state it is
in (0.2):
- **CYMA-1 boots** (a cut late in the write): run the same install command again. It finishes the remaining sectors.
- **SAFE MODE**: R6.
- **ROM mode**: it typically gets there by itself after about 4 crash-restarts, 20–40 s. Go to R5.
- **Black with nothing on USB after 2 minutes**: R8.

### R5. ROM mode (4C4A:8057)

Click **Ignore** on any disk dialog.

**A. Finish the install, or go back to V15, with fm1-recover (try this first).** It is field-proven on macOS, it
refuses a wrong chip key or flash chip, and its write window is hard-coded to `0x4000–0x93000`, so it can't touch the
boot area (`fm1_recover.py:257-259,296-302`). Tab A:

```sh
sudo -v
sudo "$R" "$CYMA" --write --wait 60 --backup-dir ~/FM1-backup/recovery      # finish CYMA-1
# or, to go back to stock instead:
sudo "$R" "$V15"  --write --wait 60 --backup-dir ~/FM1-backup/recovery
```

Pick CYMA-1 if the cut came from outside (battery, power). Pick V15 if CYMA-1 itself seemed to be the problem.

`--write` saves a fresh full backup **before** it erases anything. **Expect:**
- `Connected to WL82 / UBOOT1.00…` and the reading progress;
- `Full flash backup verified on disk: …`;
- `N application sectors differ; protected regions will be retained.`;
- `Restored and verified 1/N sectors (0x…)` … `N/N`, then another read;
- **`RESTORE VERIFIED. Bootloader and saved data unchanged. Power-cycle the FM-1 to start it.`**

Then switch off, wait 5 s, switch on, and run `--info`. Simulated: `melodee-sim-transcript.txt` M3 (V15) and M4 (CYMA-1).

- This write is unprotected too. Keep the same rules until `RESTORE VERIFIED`.
- `Recovery stopped:` **during** writing: **don't switch off.** Run the same command again; finished sectors are skipped.
- The unit drops out of ROM mode before the tool grabs it (macOS's disk driver can reset it, and it then crash-loops back
  into ROM mode): leave the tool waiting with `--wait 60`, or start the tool first and then switch the FM-1 on.

**B. If fm1-recover can't claim it but M-Flasher worked in 3.6:** restore your own dump.

```sh
sudo "$MF" restore ~/FM1-backup/m-flasher/fm1-YYYYMMDD-HHMMSS.bin
```

Read the line `Writes N sectors (…): …` **before** typing anything:
- every range must start at `0x04000` or above;
- none may include `0xFF000`;
- if either check fails, type `no`.

Otherwise type `yes`. **Expect:** `Done: everything written is verified (the chip's CRC32 matches)`. Power-cycle: the unit
is back exactly as it was, on factory V15 with your patches (simulation S2).

**C. Neither tool can reach it:** it waits for the Transporter. Switch it off to save the battery. When the dongle arrives,
run `fm1_transporter_recover.py restore "$V15" --ref <your dump>` (`instrument/README.md`, step 5).

### R6. CYMA-1 SAFE MODE

The screen shows `CYMA-1 SAFE MODE`, `IT CRASHED TWICE WHILE STARTING. NO SOUND. USB IS ON: REINSTALL FROM THE WEB
INSTALLER.`, `PLAY: TRY AGAIN`, `REC: UPDATE MODE`, and maybe a `PC xxxxxxxx` line (`main_fm1.c:677-737`).

1. Photograph it, including the PC line.
2. CYMA-1 has **no** web installer: use the Mac. Run `"$PY" "$INSTALL" --info`; **expect** `FM-1_9400  [running]`.
3. Choose one:
   - **PLAY** tries CYMA-1 once more. If it then runs, do section 5 and report the crash.
   - **Back to V15** over USB, the recommended way after a real crash loop: follow 7.1–7.2.
   - **REC** enters ROM mode (R5). Use it only if the USB-MIDI install can't reach the unit.

### R7. It boots, but something is wrong

- **`RESTARTED AFTER A CRASH` with a PC:** a crash happened and the unit recovered. Note the PC, keep testing, report it.
- **Wrong sound, screen or keys:** take a photo or video. Then hold **SAVE + HOME while switching on**: that boots with
  default settings and keeps your user presets. If it is still wrong, report it, and stay on CYMA-1 or go back to V15
  (section 7).

### R8. Dark: black, nothing on USB

1. Check `ioreg` (2.1) for all four IDs. Try another cable and port.
2. Switch off, wait 10 s, switch on **once**, and watch for 2 minutes.
3. If still nothing: **stop here.** This is the case only the Transporter fixes, because no Mac tool can reach a chip that
   never shows up on USB. Switch it off and label it "dark".
   - The boot area was never written, so the dongle's route into ROM mode works.
   - When it arrives: `python3 "$P/reference/MvaveFM1Unbricker/fm1_transporter_recover.py" wizard --v15 "$V15"`. Switch
     the FM-1 off, wire the dongle (D+, D− and GND; **never VBUS**), start the wizard, switch the FM-1 on, then dump,
     dry-run, type `WRITE` and verify (`instrument/README.md`, step 5;
     `reference/MvaveFM1Unbricker/TRANSPORTER-GUIDE.md`).
4. Meanwhile, use unit B. **Don't** install CYMA-1 on unit B until you know what happened to A.

### R9. HARDWARE CALIBRATION

OCT− and OCT+ were held at power-on; that pair is also CYMA-1's PANIC chord. Do what the screen asks, or touch nothing
for 30 s and it keeps the old layout.

### What waits for the Transporter, and what doesn't

- **Waits for it:**
  - a dark unit (R8);
  - ROM mode that neither fm1-recover nor M-Flasher can reach on this Mac (R5-C);
  - putting back the exact factory flash (V15 patches, settings, factory app build), if M-Flasher didn't work here.
- **Doable from this Mac today:**
  - resuming an interrupted install (R2);
  - finishing or undoing an install from ROM mode (R5-A);
  - safe mode back to V15 (R6);
  - going back to V15 (section 7);
  - updating CYMA-1.

---

## 7. Going back to stock V15, and updating CYMA-1 later

### 7.1 Before either one: save CYMA-1's data

```sh
"$PY" "$P/instrument/tools/cyma_sounds.py" backup        # → ~/Music/CYMA-1/backups/FM-1 … .cyma1-bank.json
"$PY" "$P/reference/fm1-base/tools/fm1_backup.py" backup ~/FM1-backup/cyma-1-presets-$(date +%Y%m%d).bak --magic CYM1
```

Use the same conditions as the first install: G4, Tab K's `caffeinate`, apps closed, one FM-1.

### 7.2 Back to V15, the standard way (USB-MIDI, from a running CYMA-1 or from SAFE MODE)

Only take this route if 3.5 said `head == V15`.

```sh
caffeinate -dims "$PY" "$INSTALL" "$V15"
```

**Expect:**
- `package: FM-1_015`, `device:  FM-1_9400`, `Install? … [y/N]`: answer `y`;
- `starting the update on FM-1_9400` … `switching to update mode` … `writing …` … `restarting`;
- **`done: the FM-1 runs FM-1_015`** (installer simulation, case F).

The installer accepts the unmodified V15 file without `--force` because it checks the hash
(`bsp/tools/fm1_install.py:38,378`).

**Know this about this route:**
- **The write is done by M-VAVE's own closed loader,** the one M-UPGRADE uses. CYMA-1 only stages it; the staging was
  checked offline (`build-test/install-risk/out_R.summary`).
- **The percentage may count down** instead of up. One captured stock update writes top-down
  (`reference/FM-1-RE/tools/tests/fixtures/fm1-v15-reflash-2026-09-04.json:53`).
- **Its behaviour on a power cut is not known,** and Melodee's docs say it also rewrites the boot data below `0x4000`
  (`reference/melodee/docs/RECOVERY.md:152-160`). On your unit that rewrite would put back the same bytes, since
  `head == V15`, but treat the whole run like an install: hands off until V15's screen appears.
- **If it is interrupted,** `--info` shows `ota-FM-1_015`. Resume with the V15 file (R0.2). A unit left in M-VAVE's
  updater stays there across power cycles (`reference/mvave-fm1-open-firmware/docs/03-update-protocol.md:172-176`).

**Afterwards, V15 may show garbled patches or settings.** CYMA-1's store (`0xDA000–0xDDFFF` and `0xFC000–0xFDFFF`) and
fm1-base's staging area (from `0xE0000`) sit inside V15's settings regions. Try **System → Reset Patches** on V15 first.

### 7.3 Back to the exact factory state (optional)

This puts back your V15 patches and settings and the factory app build. It needs M-Flasher to have worked in 3.6.

From a running CYMA-1 or V15:

```sh
"$MF" download-mode && sudo "$MF" restore ~/FM1-backup/m-flasher/fm1-YYYYMMDD-HHMMSS.bin
```

Use the same plan-line check as R5-B. M-Flasher's soft key also reaches CYMA-1, because its port `CYMA-1 FM-1` matches.
Without M-Flasher, this waits for the Transporter (`fm1_transporter_recover.py restore … --ref <dump>`).

### 7.4 Alternative way back: ROM mode with fm1-recover

Use this if 3.5 said the head differs, or if the USB-MIDI route fails. It never writes below `0x4000`:

1. send the ROM key from CYMA-1: run `"$PY" "$UNBRICK" softkey` in Tab B (it accepts `FM-1_9400`), with Tab A already
   running `sudo "$R" "$V15" --write --wait 60 --backup-dir ~/FM1-backup/recovery`;
2. power-cycle when it prints `RESTORE VERIFIED`;
3. Reset Patches on V15, as in 7.2.

### 7.5 Updating CYMA-1 later

1. Build the new package (`tools/build_device.sh instrument --no-ble`, or `run_all.sh --device`). Note its sha256 from
   `instrument/build/SHA256SUMS`, and copy it to `~/FM1-backup/firmware/cyma-1_<first 8 hex>.fwsc`.
   - A package built against a **newer fm1-base** needs `git submodule update` (4587fda requires bsp `2cd696a`), plus the
     emulator smoke test and safety re-check again before it goes on hardware.
   - **Keep the previous, known-good `.fwsc`.** It is your fm1-recover rescue image.
2. Do 7.1, then the checklist in 4.1 (minus the backup, which you already have).
3. Run `caffeinate -dims "$PY" "$INSTALL" ~/FM1-backup/firmware/cyma-1_<hash>.fwsc` from the running CYMA-1. **Expect**
   `device:  FM-1_9400`, then the same stages as 4.3.
4. **An update has the same risk windows as the first install.** There is no torn-write protection: even a 112-byte
   change rewrote 105 of the 106 code sectors, starting at `0x4000` (`build-test/install-risk/out_B_*.summary`).
   Recovery is the same, section 6. A boot stub in a future fm1-base or CYMA-1 round would shrink this window.
5. The firmware fix queued from the safety re-check: put the 21-packet rule on lines said during a USB frame (tested in
   `build-test/recheck-1028/root/instrument_fixcand/`), and correct REVIEW_REPORT §H.7 and the in-tree `sounds_ring`
   test.

---

## 8. Never, whatever a tool offers

1. Never hold **REC**, or **OCT− and OCT+** together, while switching on. You would get ROM mode or calibration. Both are harmless, but confusing
   mid-session.
2. Never switch off during `UPDATING`, or while fm1-recover or M-Flasher is writing.
3. **V16, in any form or tool.** Also nothing that downloads `FM-1.fwsc` from M-VAVE, which now serves V16: that includes
   `fm1_rescue.sh`, M-UPGRADE and the Workbench.
4. **Anything that writes the boot area `0x0000–0x3FFF`:**
   - M-Flasher `bootloader`, `firmware`, `--with-bootloader` and `--wipe-settings`;
   - M-Flasher's window buttons;
   - the "Also install m-bootloader" option in fm1-base's web installer;
   - the Workbench's Force flash;
   - M-UPGRADE's Y+U+I+O force mode.

   No USB-only tool can repair the boot area (Unbricker #6 is an open case of a black unit with a changed boot area).
5. `fm1_install.py --yes` or `--force`; fm1-base's `tools/fm1_install.py` (use `bsp/tools/`); `fm1-recover --write`
   outside R5 and 7.4; `M-Flasher --yes`; `run-ram`.
6. Restoring another unit's dump, or unit A's dump to unit B.
7. Initialize on the `WL82 UBOOT1.00` disk.
8. Hand-sent SysEx. `F0 22 24 35 7F F7` (update) and `F0 22 24 35 7D F7` (ROM key) differ by one byte.
9. Before the install: no `git pull` in `reference/fm1-base`, no rebuild of the package, no fm1-base web pages and no
   screen-reader page.
10. Never connect two FM-1s at once.

---

## 9. Sources

- **Track evidence (2026-10-28):**
  - safety: `build-test/recheck-1028/` (`root/build-test/verify-sounds-safety/r2/results/r2_full.txt`, `rp.txt`,
    `r3/results/store_sim_O2.txt`);
  - build: `instrument/build/hwprep/` (`device_build.log`, `verify_pkg.txt`, `emu/report.txt`, `audio.txt`,
    `late_blocks.txt`);
  - recovery: `build-test/mac-recovery-vetting/evidence/` (`downloads.txt`, `installer-sim-transcript.txt`,
    `melodee-sim-transcript.txt`, `mflasher-sim-transcript.txt`, `unbricker-dryrun-transcript.txt`,
    `dump-analyse-transcript.txt`);
  - loader: `build-test/install-risk/` (`out_A*.summary`, `out_B_*.summary`, `out_R.summary`, `emu/sweep.tsv`,
    `verify_only_mock.py`).
- **Code:**
  - `reference/fm1-base/bsp/tools/fm1_install.py`;
  - `bsp/loader/loader.c`, `ldr_core.c`, `ldr_screen.c`;
  - `bsp/chip/ota.c:215-259` (boot-area check before staging a foreign loader);
  - `device/main_fm1.c:660-737` (safe mode), `775-846` (boot, flash ID, REC and calibration keys), `950-964` (boot guard);
  - `reference/melodee/tools/fm1_recover.py`;
  - `reference/MvaveFM1Unbricker/fm1_unbrick.py:1928-2000` (softkey), `fm1_transporter_recover.py:1538` (analyse);
  - `reference/jvitkauskas_m-flasher/src/cli.rs`, `docs/usage.md`.
- **Project docs:**
  - `instrument/README.md` (Before you flash anything; Buttons; The screen);
  - `instrument/docs/SOUNDS.md` (FM-1 transfer, first-time checklist);
  - `instrument/build/REVIEW_REPORT.md` §H, §10;
  - `docs/research-briefing-2026-10-24.md` §4 and §8;
  - `docs/research-ecosystem-2026-10-24.md` §4 and §5;
  - `firmware/stock/README.md`.
- **Community:**
  - github.com/charlesvestal/fm1-x0x/issues/38 (macOS ROM recovery with fm1-recover; a dark unit after unplug + power-off);
  - github.com/isod89/sloop-fm1/issues/228 (second-source flash);
  - github.com/Quixotic7/MvaveFM1Unbricker/issues/6 (boot area changed after an M-Flasher install);
  - github.com/hugelton/felucca-bsp/issues/17 (not affecting CYMA-1);
  - `reference/ChoralRootFM1/docs/BOOT-SAFETY.md:36` (the stock SPL ignores the flash update record on a cold boot,
    which is why a power cut mid-write doesn't resume).
