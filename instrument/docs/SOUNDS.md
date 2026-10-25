# CYMA-1 Sounds: save, restore, share

You shape a sound on CYMA-1 until you like it. This guide shows how to keep it, come back to it, back it up, send it
to someone else, and later put it on your FM-1.

The technical definition of everything here (file formats, codes, the USB protocol, the tests) is
[CYMA-1-sounds-spec.md](../../docs/CYMA-1-sounds-spec.md). This page is the user's guide.

> **Status, plainly.** Saving, loading, files, codes and links work in the browser page, and the tests check them.
> The FM-1 side (sending sounds to the device over USB) is built and tested against the firmware's own code running
> as a virtual FM-1, but **CYMA-1 has never been installed on a real FM-1**, so nobody has sent a sound to real
> hardware yet. The [checklist](#once-cyma-1-is-on-your-fm-1-a-first-time-checklist) at the end is how you confirm it
> when you install it.

---

## What a "sound" is

A sound is the plate as you set it up: its shape, exciter, material, decay, position, room and so on. That is
**40 settings and a name**. It is the same thing the FM-1 stores in a user preset.

- **Names** are up to 14 characters: `A–Z`, `0–9`, space and `- . ' & + / ! ? # ( ) :`. Lower case becomes capitals
  and other characters are dropped, because that is what the FM-1's screen can show. `Glass rain` becomes
  `GLASS RAIN`; `Café: ¿qué?` becomes `CAFE: QUE?`.
- **Not in a sound:** the player's settings on the GLOBAL page (velocity curve, pressure sensitivity, tuning and the
  rest). Those belong to you and your instrument, not to a sound you send someone.

## My Sounds: your 32 slots

**My Sounds** is the page's list of your sounds. It has 32 slots, U01 to U32, the same as the FM-1's user presets.
It is the same bank as the panel's **SAVE** button, so a sound saved either way appears in both places. On the panel,
turn **PRESETS** past 24 to reach them.

My Sounds lives **in this browser** (its local storage). It survives a reload and closing the tab. It does **not**
travel to another browser, another computer, or a private window. The page at claude.ai and the copy on your Mac at
`localhost` keep separate lists. Files and codes are how you move sounds between them (below).

## Save a sound

1. Play until you like it.
2. Press **Save sound** in the bar (or **⌘S**, or **Ctrl+S**).
3. Type a name. The counter shows how many of the 14 characters you have used.
4. Pick a slot: the first empty one is chosen for you. A used slot says `(replace)`. (⌘S on a sound you loaded from
   My Sounds picks that sound's own slot instead, so your edits go back into it; pick another slot, or use
   **Save as new…**, to keep both.)
5. Press **Save to U08** (or **Replace U03**).

The bar confirms it: `Saved GLASS RAIN to U08.` If you replaced a sound, `Saved GLASS RAIN over U03 (was DUSK BOWL).`
comes with an **Undo**.

In the **Sounds** section, under **NOW PLAYING**:
- **Save** writes your edits back into the slot you loaded (it lights up only when there is something to save).
- **Save as new…** keeps the original and saves your version in another slot.
- **Revert** throws your edits away and goes back to the stored sound.
- **Share** makes a code or a link for exactly what you hear now, edits included.

When all 32 slots are used, the page says so and asks you to pick one to replace, or to delete a sound first.

## Come back to a sound

- **Load:** click a row in **Mine**. It glides there in 300 ms, the same as turning PRESETS, so you can click through
  sounds while a note rings. Arrow keys move along the list, Enter loads.
- **Rename:** the row's **⋯** menu, then **Rename** (or F2). Enter saves, Esc cancels.
- **Delete:** **⋯**, then **Delete…**. It asks first (`Delete U03 GLASS RAIN?`). After it, **Undo** brings it back.
- **Delete all:** the **⋯** next to the count, then **Delete all my sounds…**. It offers **Save as file first**. Undo
  works here too.
- **Factory** tab: the 24 factory sounds. **Save a copy to My Sounds** starts your own version of one.

**Factory reset** (at the bottom of the page) forgets the settings saved in this browser and **keeps My Sounds**.

## Share a sound

Press **Share** (in the bar, under NOW PLAYING, or in a row's ⋯ menu). You get three ways:

| Way | What it is | Works where |
|---|---|---|
| **Copy code** | A line of 126–140 characters starting `cy1.` | **Always, everywhere.** Paste it into any chat, email or note. Any copy of CYMA-1 opens it (**Paste a code or link**), and so does the command-line tool |
| **Copy link** | The page's address, then `#` and the sound's short code, `cy2.…` | Opens CYMA-1 with the sound loaded, in one click. The person needs access to the page. Inside claude.ai only short links arrive (below), so a very detailed sound gets no link. On the page at `localhost` it is the claude.ai page's link when the build carries that address (the published build does); a build without it makes a link that works only on your computer, and Share says so: send the code instead |
| **Save as file** | `GLASS RAIN.cyma1.json` | A file to attach, archive or keep with a project |

The factory sound SINGING BOWL as a code, and as the part of a link after the `#`:

```
cy1.AQEoDFNJTkdJTkcgQk9XTAAgAAAAAMzMzMwAACCaAAAAgGZm___NTEIdmZlmZmZmzUx6lKNwZiYAAACAmhkmp5mZmVnCswAAMzMAcACAVOWaGQAAAIAAAP__AAAAAJrUbDbQ3w
cy2.KAxIGkaHAFNJTkdJTkcgQk9XTMzMIJpmZs1MQh16lKNwmhmZmZlZwrNU5bRADLs
```

- **Credit (optional)** puts your name into files you save. Codes and links carry no name but the sound's own. A
  credit is a label, not a signature: anyone can type any name, so treat the credit in a file you receive as
  unverified.
- A code survives being wrapped onto several lines by a chat app: paste the whole thing and CYMA-1 finds it.
- A damaged or cut-off code is refused, never half-loaded: `This link's sound code is damaged or cut off, so nothing
  was loaded.`

### Links and codes: which to send

**A code always works. A link is a shortcut that works when it is short enough.**

- claude.ai passes the part of a link after the `#` into the page only when it is **at most 128 characters**. That
  was measured in the real claude.ai page on 2026-10-24: 64, 100 and 128 characters arrived, 145 never did (lengths in
  between were not tried).
- A code is 126–140 characters, so most codes are too long for that. A link therefore carries a shorter form of the
  same sound, `cy2.`, which leaves out every setting still at its default. Every factory sound's link fits (50–74
  characters).
- A sound you have changed a lot may not fit: with a 14-character name, a link fits while at most 34 of the 40
  settings differ from their defaults (a shorter name leaves room for a few more). For a sound that doesn't fit, the
  Share popover offers no link and says to share the code. The code and the file still carry it exactly.
- A link from before this change (`#cy1.…`) still opens wherever it arrives, for example the page on your Mac. Inside
  claude.ai most of them are too long to arrive: CYMA-1 then opens as if there were no link. If someone's link opens
  CYMA-1 with no sound, ask them for the code.
- Both forms carry exactly the same 40 settings and the name. Neither carries your credit.

### What the person you send it to sees

When they open your link (or paste your code, or drop your file), CYMA-1 **plays the sound but saves nothing**. A
banner says, for example, `Playing GLASS RAIN from a shared link. It isn't in My Sounds yet.` with two buttons:

- **Keep it** saves it into their first free slot (`Kept GLASS RAIN in U08.`). If they already have it, the page says
  so instead of making a copy.
- **Undo** goes back to what they were playing before.

Before they press Switch on, the page says `Switch on to hear GLASS RAIN, shared with you`.

## Files: archive, back up, move

- **Save as file** (a row's ⋯ menu, or Share) saves one sound: `NAME.cyma1.json`.
- **Save all my sounds as a file** (in **Import and export**) saves the whole bank:
  `CYMA-1 My Sounds 2026-10-24.cyma1-bank.json`.

Both are plain JSON text. They are your archive, like folders in Ableton's User Library: keep them with your projects,
put them in a cloud folder, send them to someone.

**Import** a file by dropping it anywhere on the page or with **Choose file…** in **Import and export**. Paste a code or
link into **Paste a code or link** and press **Open**.
- One sound plays with the Keep it / Undo banner, as a link does.
- A bank file opens a sheet: `BANK.cyma1-bank.json has 12 sounds. 3 are already in My Sounds.`
  - **Add 9 to My Sounds** fills empty slots and skips sounds you already have.
  - **Replace My Sounds…** replaces the whole list (it asks first, and Undo works).
- A text file full of codes works too: one code plays, several open the bank sheet.

Files from other versions of CYMA-1 load as far as they can, and the page says what happened:
- `Made with a newer CYMA-1: 2 of its settings aren't known here and were left out (FLUTTER, GRAIN).`
- `2 settings were out of range and were brought inside it (DECAY, LEVEL).`
- `This file was made with a newer CYMA-1. Open the newest CYMA-1 page to load it.` (nothing loaded)

## Back up your sounds

Browser storage is convenient, not permanent. It disappears if you clear the browser's site data, and a private window
forgets everything when it closes. So:

1. **Save all my sounds as a file** whenever you have made something you care about. It takes a second.
2. Always do it **before you update CYMA-1 or install other firmware** on the FM-1.
3. Keep the file somewhere backed up (a project folder, iCloud Drive, Dropbox).

The page warns you when storage is not being kept:
- `This browser isn't keeping storage for this page, so My Sounds lasts only until you close it. Save them as a file
  to keep them.`
- `My Sounds was changed in another tab. Reload this page before you save here.` (two tabs of CYMA-1 open: reload
  the older one so neither overwrites the other).

On the FM-1 itself, fm1-base's **USB backup** saves the whole device (settings and all 32 user presets) and puts it
back later. It is the safety net for the device; the bank file is the safety net for your sounds.

---

## Put your sounds on the FM-1 (once CYMA-1 is installed)

Your FM-1 needs CYMA-1 installed first (see the README's
[Before you flash anything](../README.md#before-you-flash-anything)). Then two routes reach it over USB:

| Route | Use it for |
|---|---|
| **The page on your Mac**, in Chrome: `python3 instrument/tools/cyma_local.py` | Clicking: Connect, Send My Sounds, Get sounds, Play current sound |
| **The command-line tool**: `instrument/tools/cyma_sounds.py` | Scripting, backups, a bank file in one command |

Why not the page at claude.ai? Sending sounds to a MIDI device needs **Web MIDI**, and claude.ai runs the page in a
protected frame with no access to MIDI devices. The page there says so and points here. Your sounds get from one page
to the other as a bank file (save it there, import it in the local page) or as codes.

### The FM-1 always asks first

Nothing from USB goes into your **user presets (U01–U32)** until **you press SAVE on the FM-1**. When a sound or a bank
arrives, its screen asks, for example:

```
USB: SAVE 3 SOUNDS?
SAVE=YES HOME=NO
```

- **SAVE** keeps them. The screen says `SAVED` / `3 SOUNDS`.
- **HOME** (or any page button, SEL+SAVE, turning PRESETS, or PANIC) says no: `USB` / `NOTHING CHANGED`.
- After 30 seconds with no answer, nothing changes either.

Other questions you may see: `USB: SAVE TO U07?`, `USB: REPLACE U07?`, `USB: DELETE U07?`, `USB: CHANGE 12 SLOTS?`.

**Play current sound on FM-1** (the page) or `audition` (the tool) only plays the sound on the FM-1, like turning
PRESETS: the footer shows `FROM USB` and the name. It goes into no user slot: SAVE then keeps it in the first free
slot, and SEL+SAVE goes back.

What the FM-1 does remember on its own is **the sound you are playing**, exactly as it does after a knob turn: its
settings save themselves once nothing has sounded for 3 seconds (at most once every 30 seconds). So a sound you only
auditioned is still there after a power-off, as an edit of the preset that was loaded (under that preset's name, so
SAVE offers that slot and you can see it on the screen). Your user presets are not touched by that.

### The page on your Mac

```sh
python3 instrument/tools/cyma_local.py         # serves the page at http://localhost:8137 and opens Chrome
python3 instrument/tools/cyma_local.py "<a link or code you were sent>"   # opens it in that page (also a long #cy1. link claude.ai won't pass)
```

1. In **FM-1 over USB**, press **Connect to FM-1**. Chrome asks to let the page "control and reprogram your MIDI
   devices". CYMA-1 only reads and writes sounds: it never updates the firmware.
2. `Connected: CYMA-1 on the FM-1 · 9 of 32 slots used.`
3. **Get sounds from FM-1** opens the FM-1's bank in the import sheet. **Send My Sounds to FM-1…** replaces the FM-1's
   32 slots with yours (it warns you, and offers **Get theirs first**). A row's **Send to FM-1** sends one sound to a
   slot you pick. Then answer on the FM-1.

### The command-line tool

```sh
PY=.venv312/bin/python                          # (mido and python-rtmidi are installed there)
$PY instrument/tools/cyma_sounds.py info                                   # what is on the FM-1 (read only)
$PY instrument/tools/cyma_sounds.py get all                                # the FM-1's sounds -> a bank file
$PY instrument/tools/cyma_sounds.py put "BOWED GLASS.cyma1.json" --slot U07
$PY instrument/tools/cyma_sounds.py push-bank "Mark Pads.cyma1-bank.json"  # into free slots
$PY instrument/tools/cyma_sounds.py push-bank "Mark Pads.cyma1-bank.json" --replace --dry-run   # the plan, nothing sent
$PY instrument/tools/cyma_sounds.py audition cy1.AQEo…                     # play a code on the FM-1, save nothing
$PY instrument/tools/cyma_sounds.py erase U07
```

Offline (no FM-1 needed):

```sh
$PY instrument/tools/cyma_sounds.py encode "GLASS RAIN.cyma1.json" [--base URL]   # the cy1. code; with --base, its links (cy2. too when it fits 128)
$PY instrument/tools/cyma_sounds.py decode "cy1.AQEo…"                            # a code or link (cy1. or cy2.) -> a .cyma1.json
$PY instrument/tools/cyma_sounds.py validate *.cyma1.json                         # check files before you share them
```

- `push-bank --replace` first saves a **safety backup** of what is on the FM-1, in
  `~/Music/CYMA-1/backups/FM-1 YYYY-MM-DD HH-MM-SS.cyma1-bank.json`.
- `--yes` skips the computer's own question. **The FM-1 still asks.**
- Files are never overwritten unless you add `--force`.
- Exit codes: 0 done; 1 declined, timed out or cancelled; 2 a bad argument, file or code; 3 no FM-1 found; 4 the
  connection was lost; 5 the FM-1 is busy; 6 wrong firmware, protocol, or a locked bank.
- `--virtual` runs the tool against a virtual FM-1 (the real firmware, in node) for trying things without hardware.

### Ableton Live and USB

- On a Mac, Live can stay open while you send sounds: macOS shares the FM-1's MIDI port. If Live sends notes during a
  transfer, a frame may need a retry, which the tools do on their own.
- **Never** send sounds while a firmware install or update is running.

### What keeps your FM-1 safe

- The tools first send the standard identity question that firmware installers send. Only an FM-1 that answers as
  CYMA-1 gets anything else. A stock FM-1, one in update mode, or one running other firmware receives that single
  question and nothing more, and the tool tells you which it is:
  `This FM-1 runs M-VAVE's own firmware (FM-1_015). Install CYMA-1 to send sounds.`
- The tools can only send that identity question and CYMA-1's own sound messages. They cannot send the update or
  bootloader commands.
- CYMA-1's sound messages never use, block or confuse the FM-1's update, backup or recovery channel. Recovery
  (holding REC at power-on) never depends on CYMA-1's code.
- Your user presets are never written from USB without a SAVE press on the device. The only other memory write a
  transfer can lead to is the settings autosave described above, the same one a knob turn causes. It goes to the
  settings area, well away from the firmware and the update area.

---

## When something goes wrong

| You see | It means | Do this |
|---|---|---|
| `That isn't a CYMA-1 code. Codes start with cy1.` | The text has no code in it | Copy the whole code, from `cy1.` to the end |
| `This link's sound code is damaged or cut off, so nothing was loaded.` | Part of the code went missing | Ask for the link again, or for the code |
| A link opens CYMA-1 at claude.ai, but no sound arrives and nothing is said | The part after `#` was over 128 characters (an older `#cy1.…` link, for example), so claude.ai never passed it in | Ask for the code |
| `This sound is too detailed for a link here; share the code instead.` (in Share) | Even the short form of this sound is over 128 characters | Copy the code, or save it as a file |
| `This sound needs a newer CYMA-1 page. Nothing was loaded.` | It was made with a newer CYMA-1 | Open the newest CYMA-1 page |
| `That file isn't a CYMA-1 sound or bank.` | Wrong file | Use a `.cyma1.json` or `.cyma1-bank.json` file |
| `That file is too big to be a CYMA-1 bank (over 1 MB).` | Not a CYMA-1 file | As above |
| `My Sounds in this browser was saved by a newer CYMA-1 page, so this page can't read or change it.` | A newer page wrote this browser's list | Open the newest CYMA-1 page |
| `Saving files isn't available in this view.` | The page cannot save a file here | Use **Copy code**, or copy the file's text it shows you |
| `Your browser blocked copying. The code is selected: press ⌘C (Ctrl+C) to copy it.` | The browser refused the clipboard | Press ⌘C |
| `No FM-1 found. Plug it in with USB, switch it on, and try again.` | No FM-1 port | Check the cable and that the FM-1 is on |
| `This FM-1 runs other firmware (…), not CYMA-1.` | Not CYMA-1 | Install CYMA-1 first |
| `The FM-1 is busy (a SAVE question or COMPARE is open). Finish it, then try again.` | The FM-1 is in the middle of something | Answer or close it on the FM-1 |
| `The FM-1 is sending a backup. Try again when it's done.` | A USB backup is running | Wait for it |
| `You pressed HOME on the FM-1, so nothing on it was changed.` | You said no on the device | Send again and press SAVE |
| `Nobody answered on the FM-1 within 30 s, so nothing on it was changed.` | The question timed out | Send again and answer within 30 s |
| `The FM-1 stopped answering, so nothing on it was changed.` | The cable or the FM-1 went away mid-transfer | Check the cable and try again |
| `The FM-1 couldn't write its memory (SAVE FAILED).` | Its flash write failed | The sounds stay in its RAM until it is switched off: try again |

---

## For people building on it

Everything is open (MPL-2.0) and specified, so other tools can read and write CYMA-1 sounds:

- **Sound file** (`format: "cyma1-sound"`, version 1): the 40 settings by name, in display units (`"decay": 1.6`,
  `"lfo_wave": "DRIFT"`). Readers decide by `format`, never by the file's extension. Unknown settings are skipped
  with a note, missing ones take their defaults. [Spec §3.3](../../docs/CYMA-1-sounds-spec.md).
- **Bank file** (`format: "cyma1-bank"`): a list of sounds, each with an optional `slot` 1–32. Spec §3.5.
- **Code**: `cy1.` + base64url of a 96-byte record plus its CRC-32. Spec §4.
- **Link code**: `cy2.` + base64url of the same sound with every value still at its default left out (a bitmap says
  which are stored), plus its CRC-32: 50–74 characters for the factory sounds, so a link fits the 128 characters
  claude.ai passes in. Its table of defaults is frozen forever. Spec revision 2026-10-24b (§4a).
- **USB**: SysEx `F0 7D 43 59 …`, nine commands, confirmed on the device. Spec §5.
- **The parameter table** every tool uses: `instrument/tools/cyma1_params.json` (generated from the firmware).
- **Test vectors**: `instrument/tests/sounds/vectors.json` (its `cy2` section has every factory sound's link code, its
  length and whether it fits 128). Any new implementation should reproduce them byte for byte.
  `cyma_sounds.py validate --strict FILE` checks a file.

**Publishing the page with share links.** A link needs the page's own address, which claude.ai gives only after the
first publish. So: publish `dist/artifact` once, then repackage with
`python3 instrument/tools/package_web.py --share-base https://claude.ai/artifact/<id>` and publish again to the same
artifact (its address does not change). Without a share base, the page inside claude.ai hides **Copy link** and offers
the code. With one, Copy link gives `https://claude.ai/artifact/<id>#cy2.…`, and only for a sound whose link code fits
128 characters. `run_all.sh` packages without one, so repackage with `--share-base` after a test run and before you
publish.

How it is tested (`instrument/tools/run_all.sh`, the steps named `sounds_*`, `unit_xfer` and `xfer_asan`):

| Test | What it checks |
|---|---|
| `tests/sounds/gen_vectors.py` | Re-derives every frozen vector from the firmware's table; the exhaustive JSON round trip; the robustness of both code forms (cy1 and cy2); 100,000 frames of the update-safety fuzz; that this guide quotes the vectors exactly |
| `tests/unit/test_xfer.c` | The device's codec against the vectors, plus a 10⁶-buffer fuzz under ASan/UBSan |
| `tests/sounds_sysex.txt`, `tests/sounds_safety.txt` | The USB protocol in the host simulator: every reply's bytes, the SAVE-press rule, timeouts, declines, flash failure; that the FM-1's update, backup, mirror and identity frames always reach the platform untouched, also under a flood of 240 requests sent without waiting |
| `tools/sounds_smoke.mjs` | The browser firmware's Sounds functions in node, the whole USB flow, a locked bank, GET_DIR's bank CRC after every page write, and a 10⁵-frame fuzz |
| `tools/sounds_chrome_test.mjs`, `tools/artifact_test.mjs` | The page in headless Chrome: save, rename, delete, links (both forms), files, Keep it / Undo, two tabs, phone width (every popover and menu target), a fake FM-1; and in claude.ai's frame, with its 128-character anchor rule |
| `tools/test_cyma_sounds.py` | The command-line tool, offline and against the virtual FM-1, including the identity check, a cable pulled mid-transfer, the link code's vectors, and credits with invisible characters |

---

## Once CYMA-1 is on your FM-1: a first-time checklist

This is the one part only you can do (the tests never touch hardware). Do it once, after installing CYMA-1, with a
bank file of your sounds saved first.

1. **Stock unit first (optional).** Before installing, `cyma_sounds.py info` against the stock FM-1 should stop at
   once with `This FM-1 runs M-VAVE's own firmware (FM-1_0…)`, having sent only the identity question.
2. `cyma_sounds.py info`: it names CYMA-1 and lists your slots.
3. `cyma_sounds.py audition` a code while you hold a note: the FM-1 glides to the new sound and shows `FROM USB`.
4. `cyma_sounds.py put FILE --slot free`: the FM-1 asks; press **SAVE**; `get` that slot back and compare.
5. The same with **HOME** (nothing changes), and once with no answer for 30 s (nothing changes).
6. `push-bank BANK --replace`: check the safety backup appears in `~/Music/CYMA-1/backups/` before the FM-1 asks.
7. Pull the USB cable in the middle of a `push-bank`: the tool says the FM-1 stopped answering, and after you plug
   it back in, `info` shows the bank unchanged.
8. Do one transfer with Live open on the same port.

If anything differs from what this page says, keep the bank file and the tool's output, and stop there.
