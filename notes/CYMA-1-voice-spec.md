# CYMA-1 VOICE: the talking plate, USB voice in, export as instrument (build spec v1)

**Date:** 2026-10-28.
**Status:** the build spec for the next CYMA-1 release. The interfaces in §8.2 are frozen when the modules start.
**Licence:** MPL-2.0, like the rest of CYMA-1. The fm1-base patch touches MPL-2.0 files of fm1-base and felucca-bsp (§3.13).

**The user's requests** (a music producer who works mostly in Ableton):
- "be ambitious... find a better way to take advantage of the hardware for synth work in a unique way... what if you used the headphone jack so you could speak into headphones like a microphone (old school trick) and combine that into a high-end vocoder function";
- "add the preset export feature". Per-sound and bank `.json` export, `cy1` codes, `cy2` links and send-to-FM-1 shipped in the Sounds release (docs/CYMA-1-sounds-spec.md). This release adds **Export as instrument**: a DAW multisample kit.

**What this merges.** Four research tracks (audio-in, hidden-hw, vocoder, export) and three judges. Every conflict between them is settled in §1. Everything below was built, simulated or measured in `build-test/voice-design/` unless it is marked **[est]** (an estimate) or **[I]** (an inference).

**Ground truth.**
- `instrument/` as of 2026-10-28: the Sounds and hardening rounds merged (instrument/build/REVIEW_REPORT.md §H.4).
- `reference/fm1-base` @ 086cd0c, with bsp @ b413cd8.
- Code is cited as file:line where the line was read for this spec. Line numbers drift.

**Hard rules for every module.**
- Never flash, never touch a USB device, never send MIDI to a real port. Device builds are compiled and run in fm1-emu only. A first flash is the user's own decision (§3.18 G-HW).
- No hardware modifications and no soldering in any plan (the user's standing rule). Ideas that need them are listed as out of policy (§0.4, §5.3).
- `reference/` is read-only. The fm1-base patch lives in a pinned fork (§3.13).
- The default CYMA-1 image keeps today's USB descriptors and behaviour. USB voice input ships as a second, opt-in image (§3.14).

---

## 0. The answer for the user: can I talk into headphones plugged into the FM-1?

### 0.1 Verdict

**No, not on the FM-1's own jack, with any firmware or any cable.** The FM-1's 3.5 mm jack is wired only to an output stage, and none of the chip's microphone inputs reach any jack. **Yes, one step upstream:** plug the headphones into the mic input of your Mac, phone or audio interface, send that voice to CYMA-1 over USB, and the plate talks. That is the headline feature of this release.

### 0.2 Why (the evidence in a few sentences)

The old trick works on a phone or laptop because their 4-pole jack has a microphone contact with bias voltage, a preamp and an ADC behind it. A headphone driver is a small moving coil, so speaking into it makes a few hundred µV to a few mV, and that input digitises it.

On the FM-1:
- The jack (J7) is fed by an output stage only: a 10-lead DAC with no control bus (U9, CS4344-class, which is a DAC only) into a headphone amp (U5). PCB photos: `reference/mvave-fm1-open-firmware/photos/2026-09-29/3-top.jpg`; crop at `build-test/voice-design/hw/audio-in/photo_top_jacks_U5_U9_PC1.png`. CS4344 datasheet: https://statics.cirrus.com/pubs/proDatasheet/CS4344-45-48_F2.pdf.
- M-VAVE's own board header says so in plain words: "FM1 has no connected analogue input" (`reference/fm1_open/app/board/fm1_audio.h:9`).
- The SoC does have 4 microphone amplifiers and an audio ADC, but every pin they could use is already wired to something else on this board: the shift-register lines, the backlight/LED rail, an LED row and the TRS MIDI input (§2, table H2).
- The TRS MIDI IN jack sits behind a logic optocoupler (PC1, "OCIC P2362", TLP2362-class). It needs milliamps to switch, so it passes at most a 1-bit on/off version of a hot line signal, never a microphone.

Changing any of this means opening the case and reworking the board, which is off the table.

### 0.3 What does work, best first

| # | Input path | What you do | Status in this release |
|---|---|---|---|
| 1 | **Mac over USB (Ableton)** | Talk into any mic the Mac sees, including passive headphones in the mic leg of a TRRS splitter or an interface mic input at 50–60 dB gain. In Live, an **External Audio Effect** sends the voice to "CYMA-1 FM-1" outputs 1/2 and takes the talking plate back on inputs 1/2, latency-compensated. | **Headline** (VOX image) |
| 2 | **Phone over USB-C** | An iPhone (USB-C) or Android phone sees CYMA-1 as a class-compliant 2-in/2-out audio device. A monitoring app (AUM, Loopy Pro, GarageBand; on Android an Oboe app) sends the phone mic to the FM-1. No laptop needed. | Same firmware as #1; documented use case, untested on phones |
| 3 | **Browser** | On the local page (`tools/cyma_local.py`, localhost), the computer's microphone. In the claude.ai artifact, a voice file you drop in, or the built-in demo voice. The artifact frame grants no microphone permission (§3.15). | **Ships** (0 device bytes) |
| 4 | **Built-in demo voice** | A formant synthesiser written for this project (no recorded speech, no TTS) says a few phrases, so the plate can talk with nothing plugged in. | Ships (host, browser, and 2 phrases on the device) |

Not now, with the reason:
- **FM-1 as USB host to a USB-C headset or adapter.** This is the only way to plug a headset with a mic into the FM-1 itself. It needs new isochronous host code that no FM-1 firmware has, a powered hub (the FM-1 supplies no VBUS), and it drops the computer link while hosting (`reference/felucca-bsp/chip/usb_host.c:1-24`, "physical validation pending"). Research-grade.
- **AirPods or a Bluetooth headset as the mic (HFP).** The Bluetooth stack that can do it (JieLi classic, about 165 KB flash and 40 KB RAM, `reference/optimist/docs/BT-CLASSIC.md:293-294`) does not fit next to CYMA-1 (148,668 B of flash free; 21,484 B of pool free). The clean-room stack has no HFP or eSCO.
- **Bluetooth A2DP into the FM-1.** Doesn't fit the pool, and 150–300 ms of latency rules out live vocoding (`BT-CLASSIC.md:296-297`).
- **Wi-Fi audio.** Needs about 276 KB of pool against 21 KB free (`reference/fm1-base/README.md:94-104`).

### 0.4 Out of policy (hardware mods): documented, never planned

- Rewiring the TRS MIDI jack to the chip's audio ADC (pin 19 is bonded to PH8+PH7, which are MIC1 and AUX1).
- An optical S/PDIF input on the LCD's chip-select pin.
- A resistor cable that turns the USB-C port into a TRS MIDI out.
- Feeding hot line audio into the MIDI optocoupler for a 1-bit pitch follower. This needs an inline resistor to protect PC1, whose input resistor value is unread.

### 0.5 The two-sentence verdict

The FM-1's headphone jack is wired only to its DAC and headphone amp, and every one of the chip's microphone pins is already used by the keys, LEDs and MIDI input, so speaking into headphones plugged into the FM-1 cannot work with any firmware. Do the old trick one step upstream instead: plug the headphones into your Mac's, phone's or interface's mic input, stream the voice to CYMA-1 over USB, and the plate becomes the vocoder; in the browser, use the local page's microphone or drop a voice file into the artifact.

---

## 1. Decisions (the conflicts, resolved)

| # | Question | Decision | Why |
|---|---|---|---|
| D1 | Which input API? Three were proposed: `plat_usb_audio_read(lr,n)`, `plat_audio_in(l,r,n)` and an `APP_UAC_IN` hook | **`uint32_t plat_usb_audio_read(float *lr, uint32_t n)`**, called inside `app_render`, as built | It is the one that was built with the pinned toolchain and simulated. Every consumer (TALK, RING, later MIRROR) uses it. |
| D2 | Ship the v1 patch as built? | **No: patch v2** (§3.13). It adds a 4-point Catmull-Rom resampler, a stale-ring re-prime, a browser ring with no new WASI imports, the `APP_GAIN_F` hook and corrected provenance. Built and simulated here. | Judges' must-fixes: 17.5 Hz top-octave flutter, a stale ring after a skipped read, `getenv`/`fopen` leaking into `app.wasm`, and a header that said "NOT compiled". |
| D3 | The zero-patch hook (the app reads usb.c's static ring) | **Rejected** | In simulation `uac_play` still primed and mixed 1–4 times per 10 minutes (`fm1-base-patch/sim_uac.out`). That is the post-limiter crackle that made CYMA-1 turn USB audio out off (`instrument/fm1.toml:11-12`). |
| D4 | One image or two? | **Two from one tree.** `cyma-1.fwsc` keeps today's descriptors (`usb_audio_out = false`). `cyma-1-vox.fwsc` adds the UAC OUT stream read by the app. | UAC OUT has never run on CYMA-1's descriptors or any verified hardware. The default image must not change under people who have it. |
| D5 | Which voice modes ship? | **TALK** (the headline) and **RING** (a cheaper sub-mode that absorbs PLATE INSERT). **ROBOT/HARMONY** is deferred. **VOWEL MAP** ships but is the first thing cut. | ROBOT's saved results do not match its proposed configuration (`out/extra_metrics.json` pitch_hysteresis_notes has 13 notes with an octave error; the "exactly 7 notes" run has no saved artifact). |
| D6 | Where do VOICE settings live? | **OBJ_SETTINGS v3**: a 32 B VOICE block appended, with v2 → v3 migration. Not in presets, and not a new store object. | `cy_user_bank_t` is frozen (sounds-spec R8). Settings are versioned and already migrate (`app.c` `APP_SET_VERSION 2`). They are already in the backup set (`app.h` `APP_BACKUP_OBJS`). No new flash sectors. |
| D7 | Where is the VOICE page? | **Hold FX for 500 ms.** The press still opens SPACE at once; holding switches to VOICE. | All 12 buttons are bound, and "Hold" on FX is free (spec §5.3). SEL+FX already opens SPACE (`app.c:1214-1226`). |
| D8 | CPU: trust the governor? | **No: a fixed VOICE rule.** While TALK or RING is on, the mode ceiling is 80 (64 when the exciter is BOW), and TALK polyphony is 4. The governor stays as a backstop with a floor of 64 while VOICE is on. | The governor sheds the highest modes of the oldest voices (`app.c:1737`; `engine.c` keeps each slot's lowest modes). Under TALK those modes are the formant bands, cut mid-phrase. |
| D9 | The excited-and-ramping mode loop | **New `EXCITED_GR` kernel** (excited input, gains ramping, coefficients steady) is mandatory before TALK ships on the device | Today such a mode falls into the general loop: 27 instr/mode-sample against 19 steady (`src/voice.c:16-20`, `:43-57`). That would add +8 on every strike onset and every bowed or rain carrier. |
| D10 | Hardware SRC and hardware FFT | **Not used in this release.** The software resampler is in uac_read; there is no FFT on the device. | JL_SRC: prototypes only. JL_FFT: the local fm1-emu clone treats unknown SFRs as plain storage (`reference/fm1-emu/crates/fm1-soc/src/periph.rs:747`), so a done-poll spins forever. Both are next-after (§5). **Update 2026-10-28 (docs/FM-1-hardware-atlas.md):** upstream fm1-emu now models FFT, EQ and SRC from FM-1 measurements, which removes the emulator blocker once the clone is updated. The 44,117.6 Hz ratio is exactly 2500/2499, so the SRC may express it if it accepts small integer pairs (untested). |
| D11 | VOICE on at boot? | **MODE always boots OFF.** The other VOICE values persist. | TALK at AMOUNT 100 % with no input is silent by design. A synth that powers up silent looks broken. |
| D12 | Export: engine-only render or the whole device? | **The whole device**, in its own `app.wasm` instance in a Worker, through the existing exports | The prototype proves it: Standard kits in 0.3–12.8 s on an M4 Max (`daw-export/proto/render_kit.mjs`, `proto/kits/report.json`). It needs no new firmware exports, and LFO, orbit and morph behave exactly as when played. The engine-only path (2–3× faster) is a later speed-up. |
| D13 | Export `.adv` skeleton | **Generated from Live's schema defaults** by our own tool; Creator "CYMA-1". Never ship or derive from Ableton's own `Sampler.adv` or `SmoothSynthKeys.xml`. | The prototype read the user's Live files (`render_kit.mjs:29-30`) and stamped `Creator="Ableton Live 11.3.22"`. |
| D14 | One latency figure | **TALK about 20 ms, RING about 15 ms** from mouth to FM-1 output, with Live at 64-sample buffers [est] (§3.17) | The tracks quoted 15–20 and 22–23 ms with different assumptions. |
| D15 | Recording level into Live | **VOX image: GLOBAL "USB REC" = FULL by default**, via `plat_usb_audio_full(1)`. The new `APP_GAIN_F` hook keeps CYMA-1's squared, smoothed MASTER taper on the headphones. | Today the USB tap follows MASTER (`device/main_fm1.c:45-79`), so the External Audio Effect return would depend on the headphone volume. CYMA-1 never calls `plat_usb_audio_full` (grep: no reference in `instrument/`). |
| D16 | Claiming "high-end" | **Only after a real-speech evaluation** (§3.18 G-Q). It uses CC BY 4.0 recordings, ESTOI against a 20-band reference vocoder, and a blind A/B with the user. | Every accuracy number so far comes from our own all-pole Klatt voice, which flatters order-10 LPC by construction. |

---

## 2. Hardware facts this release relies on

| # | Fact | Evidence |
|---|---|---|
| H1 | The audio jack J7 is output-only: U9 (10-lead, no control bus, CS4344-class DAC) → U5 (QFN amp-class part, likely a TPA6132A2-type capless headphone amp [I]) → ferrites FB4/FB5 → J7. No jack-detect or mute line reaches the SoC. | photos `2026-09-29/3-top.jpg`, `2-bottom.jpg`; crops in `build-test/voice-design/hw/audio-in/`; `reference/Felucca/firmware/src/menu_items.c:127-130` ("no amp enable or mute line is known") |
| H2 | Audio-ADC pins per the SDK: MIC0 P/N = PA2/PA1, MIC1 P/N = PH9/PH8 (corrected per the hardware atlas; the `ladc.h` comment is reversed), MIC2 P/N = PA3/PA4, MIC3 P/N = PH6/PH5; AUX0 PA0, AUX1 PH7, AUX2 PA5, AUX3 PH4. On the FM-1 all are used: PA1/PA3/PA4 = 74HC595 latch/clock/data, PA2 = backlight and LED-rail enable ("never toggle"), PA0/PA5 = matrix rows, PH9 = LED row, PH8+PH7 (bonded, pin 19) = TRS MIDI IN, PH6/PH4 (pin 20) = LED row. | `reference/fw-AC79_AIoT_SDK/include_lib/driver/cpu/wl82/asm/ladc.h:24-45`; `reference/fm1-custom-fw/REVERSE_ENGINEERING.md:81-101,128`; `reference/fm1-base/bsp/chip/fm1_uart.h:36-38` |
| H3 | TRS MIDI IN passes through PC1 ("OCIC P2362", TLP2362-class: logic output, IFHL ≤ 5 mA). Audio there is a 1-bit threshold signal at best. | `reference/mvave-fm1-open-firmware/notes/2026-09-29-baudgirl-fm1va-and-pcb-photos.md:118`; https://toshiba.semicon-storage.com/us/semiconductor/product/isolators-solid-state-relays/detail.TLP2362.html |
| H4 | fm1-base already receives host playback: UAC1, 2 ch, 16-bit, 44,100 Hz only, EP4 OUT isochronous adaptive, 1 ms. Packets land in `uo_ring[2048]` from `uo_service`, nested in TIMER5. `UO_TGT = 2 × HALF_FRAMES`, and `HALF_FRAMES = APP_BLOCK` = 128 in CYMA-1, so the target is 256 frames (5.8 ms). `uac_play` adds it after `app_render`, unclamped. | `reference/fm1-base/bsp/chip/usb.c:154-165`, `:1389-1415`, `:1420-1462`; `device/unity.c:66`; `device/main_fm1.c:62-68` |
| H5 | No fm1-base API hands that stream to the app. CYMA-1 turned it off because the unclamped add wrapped the 24-bit DAC word. | `include/plat.h:180-182`; `instrument/fm1.toml:11-12`; REVIEW_REPORT.md §3 ("Dropping USB audio out saved 7.9 KB") |
| H6 | The update channel is SysEx over USB-MIDI. The OTA and UBOOT keys are caught in the ISR before any frame exists; REC at power-on enters UBOOT before the app starts. | docs/CYMA-1-sounds-spec.md §5.6 P1, P5 |
| H7 | Device budgets now (built, never flashed): image 432,896 of 581,564 B (148,668 free); RAM `.data+.bss` 36,280 of 98,304 B; pool 322,580 of 344,064 B (21,484 free, 13,292 usable after the 8 KB spare rule); `.ram_text` 16,096 of 32,768 B. | REVIEW_REPORT.md §H.4 (lines 206-214) and §3 |
| H8 | Device time in fm1-emu at 205 instr/µs (4,650 instr/sample = 100 % of a block): preset 01 play 64 % slowest; AETHER 4-note bowed 81 %; AETHER 8-note bowed stress 83 %; CATHEDRAL GONG 16 strikes 70 %; a synthetic worst case 102 % with 2 xruns. Gate ≤ 85 %. fm1-emu has no cache model. | REVIEW_REPORT.md §4 (lines 738-765) |
| H9 | Mode kernels per mode-sample: FREE 16, EXCITED steady 19 (27 with steps), RAMP gains-only 18, glide-only 18, both 21. Bowed coupled modes: 29 static, about 35 moving. | `instrument/src/voice.c:16-20`; REVIEW_REPORT.md §4 |
| H10 | fm1-emu does not model USB audio (no UAC or isochronous code in its crates). Device perf runs must drive VOICE from the built-in DEMO source. | grep of `reference/fm1-emu` (only a README mention of Dinghy's USB audio) |
| H11 | The second core is unused by CYMA-1 (no `cpu2` in `fm1.toml`, no `plat_cpu2_*` call). Some units' core 1 misreads shared RAM under load. | `reference/fm1-base/include/plat.h:214-234`; `bsp/chip/fm1_cpu1.h:24-35` |

**Corrections to earlier notes** (so no doc repeats them):
- MIC2N is PA4 and MIC3P is PH6 (SDK `ladc.h:28-31`). One track said PA5 and another PH7; those are AUX2 and AUX1.
- MIC1P is PH9 and MIC1N is PH8: the datasheet and the SDK driver's own pin table agree, and only the comment in `ladc.h:27-28` is reversed (docs/FM-1-hardware-atlas.md, Appendix A). Both are taken anyway.
- `REVERSE_ENGINEERING.md:128` says PH7 carries UART1_TXA. The datasheet lists PH7 as UART1_RXA and PH6 as UART1_TXA.
- M-VAVE's header lists "PC3 clock/RX" as an ALNK receive channel used only as the interrupt clock (`fm1_audio.h:7-9`). The QFN-48 pin table is not reliable for this board (the FM-1 uses PB6 and PB7, which it does not list). **Updated 2026-10-28:** the B8 re-bond is now known to touch only pins 37–41, so the datasheet's port-C table applies and "PC3/PC4 are not bonded" is a strong inference (docs/FM-1-hardware-atlas.md, headline correction 14).
- Only bits 13–15 of the 74HC595 chain are never driven (`reference/fm1-base/bsp/boards/fm1/fm1_input.h:372`), not five.

---

## 3. VOICE: the talking plate (the headline)

### 3.1 What the player does

**In Live** (VOX image):
1. In Audio MIDI Setup, make an Aggregate Device of your interface plus "CYMA-1 FM-1", with drift correction on the FM-1. Or use the FM-1 as Live's only device.
2. Set Live to 44.1 kHz: the FM-1 advertises only 44,100 Hz (`usb.c:328-329`). Use a 64-sample buffer.
3. On the voice track (mic input, monitoring Off), insert **External Audio Effect**: Audio To = CYMA-1 1/2, Audio From = CYMA-1 1/2. Set Hardware Latency from Live's ping measurement.
4. On the FM-1: hold FX → VOICE page → MODE = TALK. Hold a chord and talk. Leaning on a key lets more voice into that note. PLAY freezes the vowel.
5. Record the External Audio Effect return. The FM-1's USB recording is at full level whatever MASTER says (D15).

**Pocket rig** (VOX image): phone ↔ USB-C ↔ FM-1. The phone app routes the built-in mic to the "CYMA-1 FM-1" output; listen on the FM-1's headphones.

**Browser:** the page's new **Voice** panel offers *Demo phrase*, *Open voice file…* and *Use microphone*. The last appears on the local page only (§3.15).

### 3.2 Signal path (one 128-sample block, inside `app_render`, audio ISR)

```
Mac / phone (16-bit 44.1 kHz, isochronous) ─► uo_service (TIMER5) ─► uo_ring[2048]
                                                                         │
app_render(lr, 128, master_q12)                                          ▼
 ├─ got = plat_usb_audio_read(in, 128)   (v2: Catmull-Rom, fill servo, stale re-prime)   [VOX image]
 │     or the DEMO formant voice (vin_demo.c)            or, in the browser, the page's ring (web_uin_*)
 ├─ f = cy_vin_block(in, got, 128)        src/vin.c ───────────────────────────────────────────────┐
 │     mono (L+R | L | R) → TRIM → 30 Hz DC block → dropout fades (2 ms) → meters                  │
 │     TALK:  4 kHz HP biquad → ESS signal + high-band power                                       │
 │            2× allpass halfbands (44.1 → 11.025 kHz) → pre-emphasis → asym window 128 → 11 lags   │
 │            → lag window + 1e-4 → Levinson p=10 → 48-point log envelope (40 LPC + 8 high band)   │
 │            → dB follower (3 ms attack, SMOOTH release) → gate (−55 dBFS, 5 ms ramp)             │
 │     RING:  LOWCUT HP → fixed −21 dB trim → x[] (mono excitation)                                │
 ├─ cy_engine_vin(f)                       engine.c: this render's VOICE frame ◄───────────────────┘
 └─ cy_engine_render(lr, 128, master)
       per slot, per sub-block (32):
         cy_slot_ctl: frequencies → per-mode envelope position cached when w_k is refreshed
         TALK: target gain g_k = (1−a)+a·gate·level·10^((env(f_k·ratio)+tilt_k)/20), ramped over the block
               (HOLD lengthens T60 while held); the existing mic gains × g_k → RAMP / EXCITED_GR kernels
         RING: b_k = g_in·sqrt(1−r_k²)·φ_k(POS)/rms(φ) on held slots; x = the voice → EXCITED kernel;
               per-slot RMS governor
       + ESS (high-passed voice × unvoiced detector × ESS) into both bus channels
       → DRIVE → TILT → FDN → limiter → MASTER (as today) → I2S, and uac_tap → USB to Live
```

### 3.3 Input stage (`src/vin.c`, audio ISR)

1. **Read.** `got = plat_usb_audio_read(in, 128)` on the VOX image when INPUT ≠ DEMO. It returns 128, or 0 with zeros. In host builds it reads `FM1_HOST_USB_IN`; in the browser it reads `web_uin_*`; on the default image it always returns 0.
2. **Mono.** INPUT = L+R gives `0.5(L+R)`; L or R takes that channel.
3. **TRIM** −12…+24 dB, then a one-pole DC block at 30 Hz.
4. **Dropout fades.** On got 0→128, fade in over 88 samples (2 ms). On 128→0, synthesise a 2 ms linear ramp from the last sample to 0 in place of the missing block. Count each in `tel.dropouts`. The gate must not treat a dropout as silence followed by an onset: the follower holds its last value for one block.
5. **Meters.** Input peak and RMS in dBFS, published to telemetry.
6. **When to read.** Only while MODE ≠ OFF. The v2 stale re-prime makes skipping safe: in simulation an app that stops reading for 2 s re-primes once and resumes at the 256-frame target (`fm1-base-patch-v2/sim_uac_v2.out`). Reading only while VOICE is on keeps VOICE OFF at zero cost.

**DEMO source** (`src/vin_demo.c`): a Klatt (1980) cascade formant synthesiser with Peterson & Barney vowel targets, written for this project (prototype `build-test/voice-design/tools/formant_voice.py`; C kernel `vin_fsynth`, 115 instr/sample counted).
- Device: 2 phrases, "wow, I see you" and "a e i o u".
- Host and browser: also "she sees you" and a sung line.
- It loops while selected, replacing the USB read. It is what fm1-emu perf runs use (H10).

### 3.4 The EAR: voice analysis (TALK; `src/vin.c`)

Constants are frozen from the measured design (`build-test/voice-design/kernels/vin.c`, `out/ear_choice.json`, `out/c_kernel_check.json`). Chosen: order-10 LPC on an asymmetric 128-sample window at fs/4, recomputed every block.

| Step | Detail | Counted cost (instr/block, pinned clang, `kernels/vin_p10.dis`) |
|---|---|---|
| High band | 4 kHz biquad high-pass (vin_hf): the ESS signal, plus high-band and total power | 2,590 |
| Decimate | 2 polyphase-allpass halfband stages, 2+2 coefficients (`tools/design_halfband.py --ncoef 4 --tbw 0.1`, −70 dB), 44.1 → 11.025 kHz | 2,740 |
| Autocorrelation | pre-emphasis 0.9; window 128 samples (104 rising, 24 falling = 11.6 ms) applied to the **signal** (always positive definite); lags 0–10, 4-way unrolled | 7,690 |
| LPC | 60 Hz Gaussian lag window, ×(1+1e-4) on r0; Levinson-Durbin p = 10 with \|k\| ≤ 0.999; then the coefficients' own autocorrelation ra_m | 1,900 |
| Grid | 40 log-spaced points 80–4,200 Hz: 10·log10(G/\|A\|²) via Clenshaw on cos(ω), with pre-emphasis removed. Plus 8 points 4.6–11 kHz from the high-band power with a fixed slope. Then subtract the voiced-band mean (80 Hz–4.2 kHz), so env is shape only. | 3,500 |
| Follower | per point, in dB: attack 3 ms; release = SMOOTH (5–80 ms) | in glue |
| Unvoiced detector | high-band share of total power: ≤ −20 dB voiced, ≥ −10 dB unvoiced, linear between; 2 ms / 20 ms smoothing | in glue |
| Gate | input RMS (10 ms) against −55 dBFS with 6 dB hysteresis; 5 ms ramp. Quiet input gives exact digital silence from the VOICE path. | in glue |
| Level | `level = clamp(rms_in / rms_nominal, 0, 4)` with rms_nominal = −18 dBFS, smoothed 20 ms | in glue |
| Glue (ESS mix, gate, follower, level) | | 1,400 [est] |

Rejected, by measurement:
- A recursive window on the lag products: not positive definite. One /e/ frame had a reflection coefficient of 5.4.
- Updating every second block: energy above 0.45 fs rose from −96.3 to −75.6 dB.

The grid ends at 4.2 kHz because the decimator's transition starts at 4.41 kHz.

### 3.5 TALK: the plate is the vocoder's filterbank (`src/voice.c`, `src/engine.c`)

The keys choose pitches; each held key is a plate whose modes are the synthesis bands. The voice sets each sounding mode's loudness at that mode's own frequency. Because the gains act on the output, articulation does not depend on T60: a 10 s bronze can still say "t".

**Per-mode gain** (once per block per live slot, in the engine):

```
pos_k   = grid position of f_k · formant_ratio        cy_vin.h's formula, computed inline and cached when
                                                        cy_slot_ctl refreshes w_k (no per-block log, no call)
tilt_k  = 3 dB · log2(f_k / 1000 Hz)                   cached with pos_k
a_s     = AMOUNT + (1 − AMOUNT) · P_s                   P_s: the slot's pressure 0..1 (lean = more voice);
                                                        0 for MIDI notes without aftertouch
e_k     = interp(env_db, pos_k) + tilt_k + MAKEUP_DB    linear between grid points; clamp −40 … +12 dB
g_k     = (1 − a_s) + a_s · gate · level · dB2lin(e_k) dB2lin: 65-entry LUT in 0.75 dB steps, linearly
                                                        interpolated (no 0.75 dB flicker on sustained modes)
```

- `formant_ratio = 2^(−FORMANT/12)`, so the envelope is sampled at f·2^(−s/12). FORMANT +12 makes the voice smaller, −12 larger.
- MAKEUP_DB is a fixed constant tuned so that TALK on the demo phrase matches VOICE OFF within ±3 dB loudness (§3.18 G-LVL). The model renders sat 15–25 dB low, e.g. `out/talk_circle_wow_i_see_you_120.wav` peaks at −21.6 dBFS.

**Ramping.**
- Each block, `g_k` becomes the target. `cy_slot_gains` multiplies its existing target (mic gains, edge taper, kill fade) by `g_k` and ramps over the whole 128-sample block: one `dgl/dgr` per block, not one per sub-block. It recomputes inside the block only when another gain event (fade, kill, mic move) fires.
- Its early-return test (`a == sl->gsig && o->seq == sl->gseq …`, `src/voice.c` `cy_slot_gains`) gains `&& !e->vin_talk`.

**Kernels.**
- FREE modes become RAMP: 18 instead of 16.
- Steady EXCITED modes go to the new **EXCITED_GR** loop (excited input, gains ramping, coefficients steady): ~21 [est] instead of 19. Without it they would take the 27-instruction general loop.
- Bowed coupled modes get a ramp-only path in `cy_bow_block` (~31 [est] instead of 29 static). Today ramping forces the ~35 "moving" path (`src/bow.c:172`, `:226`).

**HOLD (sustain, 0–100 %).** While a slot is held (key down, latched or frozen) and TALK is on, its per-mode decay rate δ_k (r_k = e^(−δ_k)) becomes δ_k·(1 − HOLD)³. At 100 % that gives r_k = 1: a steady 32-partial tone per key at FREE-loop cost. The coefficient renormalisation `kn` in `cy_slot_coeffs` keeps |rotation| at 1. Release returns the slot to the normal material decay. HOLD is the recommended TALK carrier, because a bowed carrier costs the junction (~150 instr per sample per bowed slot).

**Exciters under TALK.**
- MALLET: the strike starts the modes, HOLD sustains them, the voice shapes them (a talking gong). In TALK, MALLET's pressure roles (TENSION bend and PALM) are suspended, so the lean is pure voice depth.
- BOW: bowed talking glass, under the BOW ceiling (D8). Pressure keeps its BOW role (force), and depth comes from AMOUNT only.
- RAIN: a whisper. Pressure keeps its RAIN role (density).

**FREEZE (PLAY).** As today, the sounding slots freeze (spec §3.4: r = 0.9999999, energy governor). In TALK, FREEZE also holds `env_db`, `gate` and `level` at the moment of the press. Every slot, including notes played afterwards, takes the frozen vowel. PLAY again releases both.

**ESS.**
- The 4 kHz high-passed voice × the unvoiced detector × ESS (0–100 %), added equally to both bus channels before DRIVE.
- It needs the v2 resampler: linear interpolation would tremolo the sibilance at 17.5 Hz.
- Without ESS, the sibilant contour correlation was −0.34; with it, 0.60 (`out/extra_metrics.json`).

**Telemetry.** `mode_energy` is published as e_k·g_k², so the sand, the ladder and the LEDs show what you hear (§3.10).

**SURFACE.** TALK works in SURFACE too: the surface slot's gains make the key LEDs map the voice across the keybed (spec §5.5 vibration map). TALK polyphony is irrelevant there.

### 3.6 RING: the voice excites the plate

- Each **held** slot (key down or latched) runs its modes through the existing EXCITED kernel with the voice as x[n].
- Input gain per mode: `b_k = g_in · sqrt(1 − r_k²) · φ_k(POS) / rms(φ)`. That is unit noise gain per mode, weighted by where the voice touches the plate (EXCITE page POS X/Y).
- r_k follows the material law, scaled by **DECAY** (×0.05–×1, default ×0.35). Short makes a talking resonator; long a bowl you sing into.
- Input chain: LOWCUT high-pass (40–400 Hz, default 80 Hz), then a fixed −21 dB trim. Measured in the model: speech comes out +21.4 dB hotter than it goes in at DECAY ×0.35 (`out/extra_metrics.json`).
- **RMS governor** (per slot, every sub-block, mandatory in the first build):
  - E_target = the input's RMS² over 300 ms × the slot's unit-gain reference;
  - when the slot energy E > 4·E_target, multiply its state by 0.98 per sub-block (−0.18 dB per 0.73 ms);
  - release when E < 2·E_target;
  - a 50 ms hold stops it pumping on vowel boundaries.
  - It exists because an in-tune sustained sine builds +36.7 dB, which is +15.3 dB over speech (`extra_metrics.json`). The −0.3 dBFS limiter is not the safety net (§3.18 G-LVL).
- MALLET strikes add on top (true superposition). BOW adds friction at its usual cost. RAIN adds drops.
- Released slots ring out normally (FREE). Up to 8 voices.
- ESS works as in TALK. RING needs no EAR, so VOWEL MAP is unavailable in RING and the screen shows PHYSICAL.
- The model's spectral shape correlation is 0.92–0.96 on a C3-E3-G3-C4 chord (`out/talking_metrics.json`). The pre-trim peak is +25.9 dBFS.

### 3.7 Engine changes (exact list; module V2)

All are additive. `src/cy_engine.h` (frozen) is untouched; the one new entry point is declared in the new `src/cy_vin.h`.

| File | Change |
|---|---|
| `src/engine.c` | `cy_engine_vin(const cy_vin_frame_t *f)`: store the pointer for this render (NULL or mode OFF → nothing changes, bit-exact). In `cy_engine_render`: (1) the VOICE rule's budget ceiling (`e->budget = min(e->budget, f ? ceiling : …)`) and the TALK polyphony cap of 4; (2) RING: set `exc` and `b_k` for held slots and pass `x = f->x`; (3) add `f->ess` to `bus_l/bus_r` before `cy_fx_process`; (4) the CPU model (`e->est`) counts the new kernels. |
| `src/voice.c` | `cy_slot_coeffs`: when `w_k` is refreshed, cache `pos_k` and `tilt_k` per slot (`cy_vin_mc_t`, .bss). HOLD scales δ_k. `cy_slot_gains`: multiply targets by `g_k` and ramp over the block. `cy_modes_block`: the `EXCITED_GR` loop. `cy_slot_post`: energies × g² for telemetry; the RING governor. |
| `src/bow.c` | `cy_bow_block`: a ramp-only path for coupled modes (gains move, coefficients steady). |
| `src/cy_engine_int.h` | `vin_*` fields in `cy_eng_t`; `cy_vin_mc_t vin_mc[CY_NVOICES + 1][CY_N]` (TUNED voices + SURFACE; fade slots keep their last gains for their 5 ms). |

Gates: VOICE OFF is bit-exact (all 24 `demo_hash` values unchanged, host and wasm). `.ram_text` stays call-free.

### 3.8 Parameters (globals in OBJ_SETTINGS v3; not in presets)

| Param | Range | Default | Curve, detents | Notes |
|---|---|---|---|---|
| MODE | OFF · TALK · RING | OFF (always at boot) | enum, 1 step/detent | D11 |
| AMOUNT | 0–100 % | 100 % | lin, 64 | 0 = the plain plate; lean adds the rest |
| FORMANT | −12…+12 st | 0 | lin, 48, bipolar, sticky zero | TALK |
| HOLD | 0–100 % | 60 % | lin, 64 | TALK sustain while held |
| SMOOTH | 5–80 ms | 20 ms | log, 48 | TALK envelope release |
| ESS | 0–100 % | 50 % | lin, 64 | both modes |
| INPUT | USB L+R · USB L · USB R · DEMO | L+R (VOX image); DEMO (default image) | enum | in the browser "USB" means the page's voice source |
| TRIM | −12…+24 dB | 0 dB | lin, 72 (0.5 dB) | before everything |
| DECAY | ×0.05–×1 | ×0.35 | log, 48 | RING |
| LOWCUT | 40–400 Hz | 80 Hz | log, 48 | RING |
| LEAN | 0–100 % | 50 % | lin, 64 | RING: how much key pressure raises that slot's g_in (g_in × (1 + 3·LEAN·P)) |
| DEMO PHRASE | 1…2 device / 1…4 host and web | 1 | enum | shown when INPUT = DEMO, in place of TRIM |
| USB REC | FULL · MASTER | FULL (VOX image) | enum, GLOBAL page layer B | D15; shown only in the VOX image |

**Settings v3 layout** (I-V4): `app_settings_t` grows from 160 B to 192 B (`app.c` `app_assert_settings` → 192). APP_SET_VERSION 3. Appended:

```c
typedef struct {                 /* VOICE (docs/CYMA-1-voice-spec.md §3.8): norm x 65535, mode not kept (boots OFF) */
    uint16_t amount, formant, hold, smooth, ess, trim, decay, lowcut, lean;
    uint8_t input, phrase, usb_rec, rsv;
    uint8_t rsv2[10];
} app_voice_set_t;               /* 32 B */
```

Migration: a v2 record (160 B, version 2) loads with VOICE defaults and is marked dirty. A v1 record goes v1 → v2 as today, then → v3. A v3 record restored into an older firmware is rejected by its magic/version check and loads defaults (`app.c:2073`). That is safe, and the backup format (`APP_BACKUP_VERSION 1`) is unchanged.

### 3.9 Panel mapping

**Getting there.** Hold FX for 500 ms. The press opens SPACE at once, as today; at 500 ms still held, the page becomes VOICE and the footer says `VOICE`. Tap FX again for SPACE. VOICE then behaves like any page: SEL held shows layer B, K1–K4 edit, GLO returns to the previous page.

| Layer | TALK | RING | OFF |
|---|---|---|---|
| A | MODE · AMOUNT · FORMANT · HOLD | MODE · AMOUNT · DECAY · LOWCUT | MODE · — · — · — |
| B | INPUT · TRIM · SMOOTH · ESS | INPUT · TRIM · LEAN · ESS | INPUT · TRIM · — · — |

With INPUT = DEMO, TRIM's cell becomes DEMO PHRASE.

**Buttons in VOICE** (TALK or RING on; every other button is unchanged):

| Button | Behaviour |
|---|---|
| PLAY | FREEZE, plus the frozen vowel in TALK (§3.5) |
| REC (hold) | DAMP, as today |
| OCT−+OCT+ | PANIC, as today, and it also clears the VOICE follower and the RING governors |
| SEL+PLAY (new, VOICE page only) | VOICE MODE OFF ⇄ the last mode: a quick bypass |

**LEDs** (spec §5.5 levels):
- FX = 3 while VOICE is the page.
- With MODE ≠ OFF, FX pulses 1↔2 at 1 Hz whenever another page shows, so VOICE is never on invisibly.
- PLAY's green LED stays FREEZE.
- In TALK SURFACE the key LEDs show the voice across the keybed.

**Two-press audit:** any VOICE parameter is hold-FX + turn = 1, or + SEL = 2.

### 3.10 The 240×240 screen: the sand shows the voice

The plate is still one live screen (spec §6). VOICE adds:

| Element | Spec |
|---|---|
| Knob strip | VOICE page cells in a new hue, **lime #D4F06B (RGB565 0xD78D)**. It is distinct from teal, amber, violet, magenta, silver, clip and ice (spec §6.2). V4 checks contrast on void. B layer notch as usual. |
| Sand (PHYSICAL view, default) | Unchanged `ui/sand.c`. Because telemetry mode energies include g², the sand forms the figure of the modes your voice is opening, and it moves as you talk. Honest limit: under the physical rule some vowels draw the same figure (max pairwise figure correlation 0.99–1.0 across the 5 vowels; `out/vowel_map_metrics.json`). |
| Partial ladder (x 0–27) | Already shows mode levels (now voice-shaped). Adds the voice envelope as a 1 px lime polyline: y = 216 − 29·log2(f/f0_newest) for each grid point inside the ladder's range, x = 5 + clamp((env_dB+30)/30·20, 0, 20). Formants and the modes they open line up visibly. |
| Meters (x 212–239) | While MODE ≠ OFF, the pressure column (x 227–236) becomes the **voice input** column: a lime fill for input RMS (dBFS+60)/60, a 1 px ink tick at the gate threshold, and the max held pressure kept as a 1 px magenta line. The voice ticks stay. |
| Footer badge | `VOX` in lime, priority FRZ > VOX > LAT > DMP > octave. When MODE ≠ OFF and no input for 1 s: the footer says `VOICE` / `NO INPUT` (VOX image) or `VOICE` / `DEMO OR VOX BUILD` (default image with INPUT = USB). |
| **VOWEL MAP** view | EDIT cycles SAND → FIELD → MESH → **VOWEL MAP** (only while TALK is on). A "VOWEL MAP" pill shows 1.2 s on entry, and a 2-letter `VM` tag stays at the plate box's top-right. Details below. |

**How VOWEL MAP works.**
1. The main loop (not the ISR) reads the published LPC coefficients (11 floats, seqlock).
2. It evaluates |A|² on 64 points from 200 to 3,000 Hz, picks F1 and F2 with parabolic refinement, and holds the last pick in noise.
3. It maps (log F1, log F2) onto a 3×3 table of mode-group figures for the current shape. Rows are F1 at 280/480/760 Hz; columns are F2 at 850/1,400/2,300 Hz.
4. It blends the 4 nearest figures bilinearly into the sand's E(p).

The 9 groups per shape are chosen offline for maximum difference (greedy minimax correlation of ψ² maps): `tools/gen_vowel_map.py` → `src/vowel_map_data.c`, 81 B. Measured: max figure correlation 0.46–0.75 on 8 of 9 shapes. BAR stays at 1.0 (5 groups only), so on BAR the view shows PHYSICAL with a `1-D` tag. Cost: about 5.4k instructions per UI frame, ~26 µs against the 8 ms frame budget. It is stylised, not physics; the label says so (the project's honesty rule).

### 3.11 Device CPU budget (VOX image) and what is traded

Basis: 4,650 instr/sample = 100 % of a block (205 instr/µs). "Counted" means loop bodies from the pinned JieLi clang 4.0.1. The method reproduces voice.c's RAMP 18 (`build-test/voice-design/tools/count_loops.py`).

| Component | instr/sample | Source |
|---|---|---|
| `uac_read` v2 (Catmull-Rom, stereo) | **86** | counted, `hw/audio-in/devbuild-v2/usbin/build/template.dis` (v1 linear: 31) |
| DEMO voice instead (when INPUT = DEMO) | 115 | counted (`vin_fsynth`) |
| Input conditioning (mono, trim, DC, fades, meters) | 8 | [est] |
| EAR (hf 20.2 + decim 21.4 + acorr 60.1 + lpc 14.8 + grid 27.3) | 144 | counted |
| Per-mode gain lookup + LUT interpolation (96 modes; 80: 25) | 30 | counted 35/mode/block + 5 [est] |
| Glue (ESS, gate, follower, level) | 11 | [est] |
| Slot gain targets, once per block | 20 | [est]; the V2 gate is ≤ 20 counted |
| Mode kernels: +2 per mode-sample (RAMP 18 vs FREE 16; EXCITED_GR ~21 vs 19; bow ramp-only ~31 vs 29) | +192 at 96 / +160 at 80 / +128 at 64 | counted 18 vs 16; the other two [est] |
| **TALK total** | **~491 at 96 (10.6 %); ~451 at 80 (9.7 %)** | |
| **RING total** (read 86 + cond 8 + lowcut/trim 6 + ESS high-pass 20 + EXCITED vs FREE +3/mode + governor 2) | **~362 at 80 (7.8 %)** | +3/mode counted (19 vs 16) |

This is higher than the vocoder track's 403. The judges' corrections are included: the EXCITED-with-ramp path, the slot gain targets and the measured cubic read.

**The VOICE rule (D8)**, applied while MODE ≠ OFF:
- **Mode ceiling 80**: −16 modes × ~18 ≈ −290 instr ≈ −6.2 % on struck patches.
- **64 with BOW.** Bowed patches use fewer than 48 modes (`app.c` governor comment above `app_gov_shed`), so the ceiling acts through the bow's coupled share, `bow_ccap = budget / (2 × bowed slots)` (`src/engine.c:1222-1227`). With 4 bowed slots that is 12 → 8 coupled modes per slot: −16 coupled modes × ~29 ≈ −460 instr ≈ −10 %.
- **TALK polyphony 4.** With 4 voices at 80 modes, each keeps its lowest 20 by water-filling (`engine.c` `cy_budget`).
- **Governor floor 64.**

| Scenario (fm1-emu) | Measured today | TALK, VOICE rule [est] | RING, VOICE rule [est] |
|---|---|---|---|
| preset 01: 4-note struck chords + lean | 64 % | 64 − 6.2 + 9.7 ≈ **68 %** | 64 − 6.2 + 7.8 ≈ **66 %** |
| CATHEDRAL GONG: 16 strikes round robin (onsets) | 70 % | ≈ 74 % | ≈ 72 % |
| AETHER: 4-note bowed chord | 81 % | 81 − 10 + 7.5 ≈ **78 %** (TALK on ~40 modes: read, EAR, +2/mode) | 81 − 10 + 5.2 ≈ **76 %** |
| AETHER: 8-note bowed stress (4 bowed + 4 ringing) | 83 % | n/a (TALK polyphony 4) → as above | 83 − 10 + 5.2 ≈ 78 % |
| Synthetic worst (102 % today) | 102 % | not claimed; it fails today without VOICE | not claimed |

**What is traded, in plain words.** While VOICE is on:
- the plate uses 80 modes instead of 96;
- TALK plays up to 4 notes;
- bowed voices couple fewer modes and sound darker. REVIEW_REPORT §4 measured −10 to −21 dB at 2–8 kHz when bowed budgets were pre-cut, which is why this applies only while VOICE is on and HOLD is the recommended TALK carrier.

VOICE OFF costs nothing: no read, no EAR, kernels bit-exact. fm1-emu has no cache model. All VOICE hot code goes in `.ram_text`. `uac_read` stays in flash, as `uac_play` is today; moving it to RAM is a v3 patch option if hardware shows misses.

### 3.12 Memory and flash budget

| Item | Flash | RAM `.data+.bss` | Pool | `.ram_text` |
|---|---|---|---|---|
| fm1-base patch v2 (UAC OUT on + uac_read) | +1,084 B (template A/B: 81,268 → 82,352 B) | +8,432 B (uo_ring + ep4rx; measured) | 0 | 0 |
| vin.c kernels | 3,684 B (counted .text, `kernels/vin_p10.o` + `vin_opt`) | vin_t 2,020 B | 0 | ~2,400 B (hf, decim, acorr, lpc, grid) |
| vin_demo.c (synth + 2 phrases) | ~2,000 B [est] | 148 B | 0 | 0 |
| Engine hooks (EXCITED_GR, bow ramp path, lookup, HOLD, RING, governor) | ~2,000 B [est] | vin_mc 9 × 32 × 16 B = 4,608 B | 0 | ~700 B [est] |
| Buffers, dB LUT, telemetry (2 copies) | — | 1,024 + 260 + 540 B | 0 | 0 |
| app_voice.c, VOICE page, settings v3, words | ~3,500 B [est] | +32 B (settings) | 0 | 0 |
| UI (voice_ui.c, ladder curve, meter, VOWEL MAP) + vowel_map_data | ~3,000 B [est] + 81 B | ~300 B | 0 | 0 |
| **Total** | **~15.3 KB** | **~17.4 KB** | **0** | **~3.1 KB** |
| **After** | 448 KB of 581,564 B (133 KB free) | 53.7 KB of 98,304 B (44.6 KB free) | 322,580 B (21,484 free; the 8 KB spare kept) | ~19.2 KB of 32 KB |
| **Gate (REVIEW_REPORT row, its own)** | ≤ +16,384 B | ≤ +20,480 B | **+0 B** | ≤ +4,096 B, call-free |

The default image (no UAC OUT) carries the VOICE code without the patch's RAM: about +14 KB flash and +9 KB RAM. The Sounds gate (196 B left) is not touched; VOICE has its own gate row.

### 3.13 The fm1-base patch v2 (exact, minimal, MPL-clean)

**File:** `build-test/voice-design/hw/audio-in/fm1-base-patch-v2/fm1base-usb-audio-read-v2.diff` (375 lines with its header). It supersedes `fm1-base-patch/fm1base-usb-audio-read.diff`. Base: fm1-base @ 086cd0c (bsp @ b413cd8). It applies cleanly (dry run).

| File (licence) | Change |
|---|---|
| `bsp/chip/usb.c` (felucca-bsp, MPL-2.0) | `FELUCCA_UAC_OUT_APP` (default 0). `uac_read(float *lr, uint32_t n)`: the same fill servo as `uac_play`; a 4-point Catmull-Rom with integer differences (`UAC_CR`); re-prime when fill > UO_TGT + 4 blocks (`uo_stale`); returns n, or 0 with zeros. Default builds compile it out. |
| `device/main_fm1.c` | Skips the post-render `uac_play` when OUT_APP. In full mode, `APP_GAIN_F(m)` (if the app defines it) gives the speaker gain as a float ramped across the block; default unchanged. |
| `device/plat_fm1.c` | `plat_usb_audio_read` → `uac_read`, or zeros. |
| `device/unity.c` | `FM1B_UAC_OUT_APP` (default 0) → `FELUCCA_UAC_OUT_APP`; documented. |
| `include/plat.h` | The `plat_usb_audio_read` contract (§8.2 I-V1) and the `APP_GAIN_F` hook text. |
| `host/plat_host.c` | `FM1_HOST_USB_IN=file.raw` reader for host builds only. PLAT_WEB reads `web_uin_take`. `APP_GAIN_F` parity in the host render. |
| `web/emu/emu_web.c` | The page's input ring (4,096 frames) with `web_uin_buf()` / `web_uin_push(n)` exports. Two-block prime, re-prime past six blocks. |
| `tools/build.py` | `usb_audio_out = "app"` → `-DFM1B_UAC_OUT_APP=1`; `FM1B_UAC_OUT_APP` also from the environment. |

**Evidence (this spec's run).**
- **Device build** (`tools/build_device.sh` with the pinned image `fm1-toolchain:20250324.1-raqm`; fm1-base's template app with `usb_audio_out = "app"`; `hw/audio-in/devbuild-v2/`):
  - every check ok;
  - `uac_read` linked and called from `app_render`, `uac_play` absent;
  - loop 86 instr/frame, no calls;
  - image 82,352 B; RAM 29,892 B;
  - the package was deleted, never flashed.
- **Host and web builds** of the same app compile. `app.wasm` imports only `wasi_snapshot_preview1.clock_time_get` and exports `web_uin_buf` and `web_uin_push` (checked with `WebAssembly.Module.imports`).
- **Simulation** (`fm1-base-patch-v2/sim_uac_v2.c`, uac_read extracted verbatim; `sim_uac_v2.out`) at CYMA-1's UO_TGT 256:
  - 0 underruns and 0 overruns over 600 s at 0, ±300 and ±1000 ppm and render loads 5–98 %;
  - ring wait 5.07–6.55 ms;
  - flutter: 10 kHz 0.74 dB (linear: 2.42 dB), 15 kHz 3.51 dB (linear: 6.33 dB), 4 kHz 0.08 dB;
  - an app skipping 2 s: 1 re-prime, resumes at the target;
  - a 50 ms host pause: 1 underrun, 18 silent blocks, then recovery.

**MPL obligations.**
- The patch modifies MPL-2.0 files. A distributed CYMA-1 binary built with them must publish those changed files under MPL-2.0 (MPL 2.0 §3.1–3.2). CYMA-1's own files are unaffected (`reference/fm1-base/LICENSING.md`).
- Upstream as one fm1-base PR plus one felucca-bsp PR (usb.c). The PR descriptions carry the evidence above.

**Where it lives.** A fork at `vendor/fm1-base-cyma/`: a git clone of `reference/fm1-base` at 086cd0c with the bsp submodule initialised, and one commit, the v2 patch. `instrument/FM1_BASE.lock` holds that path and commit hash. `run_all.sh` defaults `FM1_BASE` to the fork when the lock exists, checks the hash, and re-runs `sounds_ring` (its `extract.py` copies usb.c verbatim and fails when usb.c changes, `run_all.sh:211`).

### 3.14 Two images, one tree

| Image | How it is built | USB | VOICE inputs |
|---|---|---|---|
| `build/cyma-1.fwsc` (default) | `fm1.toml` unchanged (`usb_audio_out = false`) | as today: UAC IN + USB-MIDI | DEMO only on the device; the page's sources in the browser |
| `build/cyma-1-vox.fwsc` | `FM1B_UAC=1 FM1B_UAC_OUT=1 FM1B_UAC_OUT_APP=1 tools/build_device.sh instrument --no-ble`, output renamed by V6's step; `APP_VERSION` suffix `-vox` | + the UAC OUT stream (fm1-base's default descriptor set) read by the app | USB L+R / L / R and DEMO; USB REC FULL |

The app code is identical in both. It calls `plat_usb_audio_read`, which returns 0 on the default image, and uses `#if FM1B_UAC_OUT_APP` only for the footer text and the USB REC item. Both images have identity FM-1_9400 and the same store map, so settings and user presets survive switching (settings v3 either way).

### 3.15 Browser parts (module V5)

- **Worklet input.**
  - `new AudioWorkletNode(ac, "cyma-1", { numberOfInputs: 1, … })` (it is 0 today, `instrument/web/emu/index.html:1898`).
  - In `process()`: take input channel 0 (mono), resample from `ac.sampleRate` to the device rate with a second instance of the worklet's Kaiser-sinc resampler run in the other direction, write into `web_uin_buf`, call `web_uin_push(n)`, then `web_render` as today (`worklet.js:360`).
  - With no source connected, push nothing; the firmware sees 0 and the VOICE path is silent.
- **Sources** (the Voice panel, under the keybed):
  - **Demo phrase**: sets INPUT = DEMO through the firmware. No audio assets, no page weight.
  - **Open voice file…** and drag-and-drop: `decodeAudioData` → a looping `AudioBufferSourceNode` → the node's input. This works in the claude.ai artifact.
  - **Use microphone**: `getUserMedia({audio:{echoCancellation:false, noiseSuppression:false, autoGainControl:false}})` → `MediaStreamAudioSourceNode`. Show it only when `isSecureContext` holds and a trial call does not reject with `NotAllowedError`. The artifact frame grants `allow="autoplay"` only (modelled at `instrument/tools/artifact_test.mjs:85`), and the microphone Permissions-Policy defaults to `'self'` (https://developer.mozilla.org/en-US/docs/Web/HTTP/Reference/Headers/Permissions-Policy/microphone).
  - Where the mic is blocked, the panel says: `Microphone works on the local page: python3 tools/cyma_local.py`.
- **Feedback guard.** Choosing the microphone shows `Use headphones: the speakers will feed back into the mic.` once per session. In RING, the input meter turns clip-coloured above −6 dBFS.
- **Telemetry.** New `web_vin.c` (included from `web.c`) exports `web_cy_vin_tel()`, a pointer to a `cy_vin_tel_t` copy. The page draws a 48-band analyser and an input meter in the panel (the same data as the device's screen).
- **Latency readout.** The panel shows `≈ N ms`, where N = 5.8 (ring prime) + 5.5 (EAR, TALK only) + (`ac.baseLatency` + `ac.outputLatency`)·1000, measured live.
- **Capture** (the existing 24-bit WAV + `.mid` zip) records the result unchanged.
- **Not now:** a localhost bridge that sends the mic to a real FM-1 through `AudioContext.setSinkId`. It would touch a real device, which this workflow may not test.

### 3.16 Safety

| # | Claim | Why it holds | Locked by |
|---|---|---|---|
| S1 | The USB voice path cannot disturb the update channel or MIDI. | `uac_read` touches only `uo_ring`, `uo_w/uo_r`, `uo` and `uo_stale`. The OTA and UBOOT keys are caught in the USB ISR before any frame exists (sounds-spec P1). REC-at-boot UBOOT runs before the app (P5). The safe-mode loop never calls `app_render`. USB-MIDI endpoints and the SysEx path are unchanged. | fw call-graph check: `uac_read` and `cy_vin_block` reach no MIDI, OTA, flash or blocking function; sounds_ring and the P1–P8 tests re-run |
| S2 | The default image is unchanged on the wire. | `cyma-1.fwsc` keeps `usb_audio_out = false`. | V6 diffs the descriptor bytes against today's package |
| S3 | Host audio never reaches the DAC unprocessed or unclamped. | With OUT_APP, `uac_play` is not linked (devbuild-v2 disassembly). Everything goes through the engine, the FX and the limiter. | V0 acceptance: `uac_play` absent in `cyma-1-vox` |
| S4 | No click on stream start, stop or underrun. | 2 ms fades in `vin.c`; the follower holds one block. | G-DROP |
| S5 | RING cannot run away. | Fixed −21 dB trim, the per-slot RMS governor, the existing NaN/runaway reset in `cy_slot_post`. | G-LVL |
| S6 | No flash write in the audio path; VOICE settings autosave under the existing rules (3 s of silence, 30 s gap). | `app.c` autosave conditions | review |
| S7 | Feedback warnings. | Page text; the README section "Headphones, not speakers". | V5 tests |
| S8 | No new radio, pool or second-core use. | Unchanged `fm1.toml` keys | build checks |

### 3.17 Latency: the one published figure

From mouth to FM-1 output, Live at 64-sample buffers, VOX image [est]:

| Stage | ms |
|---|---|
| Mac: converter + 64-sample input + 64-sample output buffers + Core Audio safety offsets | ~5 [I] |
| USB isochronous frame | 1 |
| uo_ring wait (simulated mean at UO_TGT 256) | 5.8 (5.1–6.6) |
| EAR: voice onset to plate onset (model; TALK only) | 5.5 |
| Output half (the block plays during the next I2S half) | 2.9 |
| DAC and amp | ~0.3 |
| **TALK** | **≈ 20 ms** |
| **RING** (no EAR) | **≈ 15 ms** |

The round trip back into Live adds the UAC IN path and Live's input buffer. Live compensates it once Hardware Latency is set from a ping. README and the page state these figures with "estimate; measure on your setup". Browser: shown live (§3.15), typically 20–40 ms.

### 3.18 Test metrics and acceptance gates (all must pass to ship)

| Gate | Metric | Threshold | How |
|---|---|---|---|
| G-OFF | VOICE OFF bit-exact | all 24 `demo_hash` unchanged, host and wasm; golden unchanged | run_all |
| G-EAR | C EAR vs Python reference on demo vowels (F0 100–250 Hz) | envelope error ≤ 2.5 dB median below 4.2 kHz (now 1.9–2.1) | `tests/unit/test_vin.c`, `tools/vin_ref.py` |
| G-TALK | Log ERB-band shape correlation, voiced frames, "wow I see you" and "she sees you", C3-E3-G3-C4, CIRCLE/SQUARE/BAR, **at the device rule (budget 80, 4 voices, HOLD 60 %)**, not budget 128 | median ≥ 0.85 (the 20-band noise-vocoder reference is 0.87–0.92 at budget 128) | `tools/voice_golden.py` on host renders |
| G-Q | Real speech: ≥ 6 clips from LibriSpeech test-clean and VCTK (both CC BY 4.0, attributed; https://www.openslr.org/12): low male, female, breathy, nasal, and two with pink noise at 15 dB SNR | TALK ESTOI ≥ reference 20-band vocoder ESTOI − 0.05, per clip median; plus a blind A/B with the user, reported, not gating | `tools/voice_eval.py` with pystoi `extended=True` (MIT, https://github.com/mpariente/pystoi) |
| G-ESS | 4.5–10 kHz contour correlation on "she sees you" | ≥ 0.5 with ESS 50 % | voice_golden |
| G-SPILL | Energy above 0.45 fs (vowel switches) | ≤ −90 dB | voice_golden |
| G-GATE | Input below −60 dBFS for 50 ms | VOICE path output exactly 0 | host scenario `voice_gate.txt` |
| G-DROP | Truncated `FM1_HOST_USB_IN` file and a 50 ms gap | no step: max \|Δ²\| of the output < the clean-tone reference × 2; `tel.dropouts` counts each | `voice_drop.txt` |
| G-LVL | (a) TALK vs VOICE OFF loudness on the demo phrase. (b) RING with a synthetic in-tune sustained sung vowel at −6 dBFS into a C3-E3-G3-C4 BRONZE chord, DECAY ×1. | (a) within ±3 dB LUFS. (b) peak before the limiter ≤ −1 dBFS, limiter GR ≤ 3 dB, no governor pumping (gain change ≤ 1 dB per 100 ms after 300 ms) | voice_golden |
| G-PERF | fm1-emu `--perf`, DEMO source: `voice_play` (preset 01 + TALK), `voice_gong` (CATHEDRAL GONG strikes + TALK), `voice_aether` (AETHER bowed + TALK), `voice_ring` (8-note RING) | slowest block ≤ 85 %, 0 xruns; 75–85 % flagged "hardware check needed" | `tools/emu_perf.py` new scenarios on `cyma-1-vox.fwsc` |
| G-MEM | §3.12 gates | flash ≤ +16,384 B, RAM ≤ +20,480 B, pool +0, `.ram_text` ≤ +4,096 B and call-free | device build checks |
| G-SAFE | S1–S3 | call graph clean; descriptors of the default image byte-identical; `uac_play` absent in VOX | V6 |
| G-WEB | wasm imports unchanged (`clock_time_get`, `fd_write`); node has 1 input; artifact mode hides the mic and file/demo work; local mode mic test | pass | `wasm_smoke.mjs`; `chrome_test.mjs` with `--use-fake-device-for-media-stream --use-file-for-fake-audio-capture=demo.wav` |
| G-HW | **Only after the user explicitly approves a flash:** first a loopback image (host audio → `plat_usb_audio_read` → straight to the output and UAC IN, with level, underrun and stale counters on screen); then TALK | counters stay 0 for 10 min at Live 64 and 128 buffers; then the user's ears | user-run; never by this workflow |

### 3.19 Host commands and expects (frozen, I-V5)

| Command | Meaning |
|---|---|
| `vin mode off\|talk\|ring` | VOICE MODE |
| `vin src lr\|l\|r\|demo` | INPUT |
| `vin phrase N` | DEMO PHRASE |
| `vin set NAME VALUE` | AMOUNT, FORMANT, HOLD, SMOOTH, ESS, TRIM, DECAY, LOWCUT, LEAN in display units |
| env `FM1_HOST_USB_IN=file.raw` | the USB input (16-bit stereo LE, 44.1 kHz), read once from the start |

| Expect | Value |
|---|---|
| `vin_in_db` | input RMS dBFS (10 ms) |
| `vin_gate` | 0..1 |
| `vin_dropouts` | count |
| `vin_mode` | 0..2 |
| `vin_env_db K` | envelope grid point K (0..47) |
| `set_version` | 3 |

---

## 4. Export as instrument (the DAW multisample export)

### 4.1 Format choice, with evidence

- **Ableton Sampler `.adv`** is the only way a multisample loads natively in Live:
  - Live 11 and later cannot import `.nki`, `.exs`, `.sf2` or `.gig` into Sampler, and SFZ was never native (Ableton, "Supported sample formats", https://help.ableton.com/hc/en-us/articles/115001134410-Supported-sample-formats, confirmed by search on 2026-10-28; the direct fetch returned 403).
  - An `.adv` is gzip-compressed XML. This Mac's Live 12.1.10 ships its schema history (193 files, 8.1_220 to 12.0_12120) in `/Applications/Ableton Live 12 Suite.app/Contents/App-Resources/Schema`.
- **Two `.adv` variants.**
  - Schema **11.0_11300**: the newest Live 11 schema; opens in 11.3 and every 12.x. Round robins are stored as SelectorRange slices.
  - Schema **12.0_12120** with native RoundRobin, written only when the kit has round robins. Round robin first appears in 12.0_12001.
  - The Sampler trees differ by exactly 8 elements: RoundRobin, RoundRobinMode, RoundRobinResetPeriod, RoundRobinRandomSeed, BreakoutIsExpanded, MpePitchBendUsesTuning, SourceContext/LocalFiltersJson, SampleRef/SamplesToAutoWarp.
  - Never write a schema newer than 12.0_12120: Live cannot open files from a newer version (https://forum.ableton.com/viewtopic.php?t=245878).
- **Simpler `.adv`** (one zone: the middle root at the middle velocity) for Live Intro and Standard, because Sampler is Suite-only. OriginalSimpler uses the same SimplerPlayer/MultiSampleMap classes (Schema 12.0_12120.txt:5575).
- **SFZ** (`seq_length`/`seq_position`, https://sfzformat.com/opcodes/seq_length/) and **Decent Sampler** (`.dspreset`, https://decentsampler-developers-guide.readthedocs.io/en/latest/the-groups-element.html) for any DAW.
- **Samples:** 24-bit WAV by default (Live reads WAV/AIFF/FLAC/OGG at 8/16/24-bit; https://help.ableton.com/hc/en-us/articles/211427589-Supported-Audio-File-Formats). FLAC is optional and unverified for Sampler zones.
- **Reference implementation for semantics only:** ConvertWithMoss (LGPLv3; manual §6.3 and changelog 11.3.0, where a millisecond LastModDate made Live 12 refuse files). We follow its documented behaviour and copy none of its code or templates.

### 4.2 The render pipeline (browser only: 0 device bytes)

- **Engine.** A module Worker with its **own `app.wasm` instance**, never the worklet's. It drives the whole simulated device through existing exports, exactly as `daw-export/proto/render_kit.mjs` does:
  - `web_boot`, `web_master(4096)`;
  - `web_cy_load`/`web_cy_box`/`web_cy_audition` to load a copy of the sound with P_KEYMODE forced to TUNED, and ROOM 0 unless "Print room";
  - `web_midi_in` for notes and CC120;
  - `web_render` in 128-frame blocks;
  - the device clock is simulated, so renders are deterministic.

  Measured: `app.wasm` instantiates in 12.4 ms in a Chrome 156 Worker; the whole-device path runs 71× realtime there (node 96–105×); the determinism hash 0x8A60F65A reproduces in the Worker (`daw-export/bench/bench_out.json`).
- **Load order.** A published supporting file `export/export-worker.js`, else a Blob URL, else a main-thread render time-sliced in ≤ 20 ms slices. The claude.ai CSP for workers is unverified, so the fallback is mandatory.
- **Sources:** a factory preset, My Sounds, a pasted `cy1`/`cy2` code, or (local page) the FM-1's edit buffer via the Sounds SysEx `GET_EDIT`.

### 4.3 Zone plan and capture rules

| Tier | Roots | Velocity layers (render → zone) | Tail cap |
|---|---|---|---|
| Sketch | 9: every 6 st, C2–C6 | 100 → 1–127 | 6 s |
| **Standard** (default) | 17: every 3 st, C2–C6 | 40/80/120 → 1–60, 61–100, 101–127, with 6-step velocity crossfades in SFZ/DS (Sampler: Fade In/Out on the velocity ranges) | 8 s |
| Deep | 25: every 3 st, C1–C7 | 4 layers | 20 s |

- **Round robins: automatic.** Render one note twice and null-test the takes.
  - Under −15 dB → 1 round robin (MARIMBA −19.9 dB).
  - Otherwise 2, or 3 for BOW/RAIN (SINGING BOWL +7.3, HANDPAN +5.7, CATHEDRAL GONG +4.7, STEEL RAIN +1.0, BOWED GLASS −0.8 dB; `proto/rr_null.mjs`).
  - Each round robin is the device's next strike: the RNG, LFO and WANDER state move on. These are new physical strikes, not copies.
- **Capture per zone** (as `render_kit.mjs`):
  - wait for digital silence;
  - MALLET: hold until −66 dB under the zone's peak or the cap;
  - BOW/RAIN: hold 3 s, release, and record the tail;
  - onset = the first nonzero int24 sample;
  - the tail is cut at −60 dB under the peak with a 30 ms fade, or a 400 ms fade at the cap.
- **Baked loops** for BOW/RAIN zones: loop end at a rising zero crossing ~50 ms before note-off; loop start at the best normalised cross-correlation 0.6–1.6 s earlier (measured median 1.00 bowed, 0.91 rain); a 4,096-sample equal-power crossfade baked into the file. Loops are seamless in every player with no player crossfade. Written as SustainLoop Mode 1 / ReleaseLoop Mode 0 in `.adv`, `loop_mode=loop_sustain` in SFZ, `loopEnabled` in DS.
- **Level.**
  - Samples are the engine's exact int24.
  - One kit gain (peak → −1 dBFS) goes into each preset (Sampler Volume, SFZ `volume=`, DS `volume`), so key and velocity balance stay intact.
  - "Bake into files" is a deterministic two-pass option.
  - Measured: single-note peaks ≤ −6.6 dBFS across 24 presets × 5 notes × 2 velocities, so the limiter never shapes a zone. The worker flags any zone above −1 dBFS.
- **Release.** DAMPER 0 gives a release longer than any sample; otherwise the measured ring after note-off. Pitch bend ±2 st.
- **SURFACE presets** export as a TUNED kit, with README text saying so. The 27-point SURFACE kit mode is later (§5.3).

### 4.4 Writers

- **`.adv`.**
  - The MultiSampler skeleton comes from `export/adv_skeleton.json`. It is generated by `tools/adv_skeleton.py` from the schema files' defaults (element order, types and default values: format facts), committed with the schema version it was built from.
  - Header: `<Ableton MajorVersion="5" MinorVersion="11.0_11300" Creator="CYMA-1 <APP_VERSION>" Revision="">` (12.0_12120 for the Live 12 file).
  - Each MultiSamplePart carries the schema's members in schema order: KeyRange, VelocityRange, SelectorRange, RootKey, sample start/end, loops.
  - FileRef: RelativePathType 1 (relative to the preset's folder), RelativePath `Samples/<file>`, OriginalFileSize, LastModDate **in seconds**, no OriginalCrc.
  - If type 1 fails the Live gate (G-E1), the fallback is the User Library layout: RelativePathType 6, `Samples/Imported/CYMA-1/<NAME>/`, `Presets/Instruments/Sampler/CYMA-1/`.
- **SFZ:** `<control> default_path=Samples/`; `amp_veltrack`, `ampeg_release`; per-layer `<group>` with `lovel/hivel` and `seq_length`; `<region>` with `lokey/hikey/pitch_keycenter/seq_position`, loops.
- **Decent Sampler:** `<DecentSampler><groups release ampVelTrack seqMode="round_robin"><group loVel hiVel><sample path rootNote loNote hiNote seqPosition seqLength loopEnabled loopStart loopEnd/>`.
- **`<NAME>.cyma1.json`:** the source sound, exactly as the Sounds export writes it, so the kit carries its recipe.
- **README.txt:**
  - what each file is and which Live edition opens it;
  - render settings (tier, dry/room, kit gain, the share code);
  - "chords from samples skip the bus drive and limiter; play the FM-1 or the page for the live plate";
  - MPL note: the samples are the user's.

### 4.5 Zip layout and naming (frozen, I-E2)

```
CYMA-1 <NAME> (Standard).zip                       stored (no compression), Blob parts, via downloads.save
└── CYMA-1 <NAME>/
    ├── <NAME> (Live 11.3+).adv                    Sampler, schema 11.0_11300
    ├── <NAME> (Live 12).adv                       only with round robins > 1; schema 12.0_12120
    ├── <NAME> (Simpler).adv                       one zone
    ├── <NAME>.sfz
    ├── <NAME>.dspreset
    ├── <NAME>.cyma1.json
    ├── README.txt
    └── Samples/<NAME>_<note><octave>_v<vel>[_rr<n>].wav     (or .flac)
```

- `<NAME>` follows the Sounds name rule (sounds-spec §3.4); file names are ASCII.
- The zip is the only container: the downloads capability allowlists `zip` but not `wav`, `flac`, `sfz` or `adv` (artifact-capabilities `downloads.d.ts:19-36`).
- Above 200 MiB the sheet warns (the Android app's save cap). Standard sizes: MARIMBA 10.7 MB, BOWED GLASS 92 MB, SINGING BOWL 105 MB, CATHEDRAL GONG 108 MB, STEEL RAIN (2 RR) 179 MB of WAV. FLAC −5 is 22–51 % of that.

### 4.6 UI

- *Export as instrument…* appears in the ⋯ menu, the Share sheet and NOW PLAYING.
- The sheet:
  - Target: Live (all) · SFZ · Decent Sampler · All;
  - Tier: Sketch / Standard / Deep;
  - Room: Dry / Print room;
  - Samples: WAV 24 / FLAC;
  - Gain: in preset / baked.
- A size and time estimate comes from a 1-note probe render. Progress shows per zone, with Cancel.
- Done: `Saved CYMA-1 MARIMBA (Standard).zip — drag "MARIMBA (Live 11.3+).adv" onto a MIDI track in Live.`
- The Capture sheet gets *Make an instrument of this sound*, which hands the current sound to the same worker.

### 4.7 Export gates

| Gate | Threshold |
|---|---|
| G-E1 | **The user drags** `proto/kits/*/… (Live 11.3+).adv` and `(Live 12).adv` into Live 12.1.10 (and 11.3 if available) and confirms the samples resolve with RelativePathType 1 and round robins alternate. This takes about 2 minutes. Until then, nothing claims "loads in Live". This workflow never touches the user's Live session. |
| G-E2 | `tools/validate_adv.py` against Live's own schema: only the members Live's own Core Library presets also omit (KeyMidi, RemoteSelectionKeyMidi) and OriginalCrc may be missing. |
| G-E3 | Determinism: a frozen 3-zone mini kit (MARIMBA C3/C4/C5 at velocity 100) hashes identically in node and Chrome, twice from fresh instances (`export_test.mjs`). |
| G-E4 | No Ableton-owned bytes: the generated `.adv` contains no string from `Sampler_default.xml`/`SmoothSynthKeys.xml` beyond schema element names; Creator = CYMA-1. `build-test/voice-design/daw-export/live/` is never published. |
| G-E5 | The Worker path, and the time-sliced fallback with the Worker disabled, both produce the G-E3 hash. The page stays responsive (no main-thread task over 50 ms while the Worker renders). |
| G-E6 | Standard MARIMBA renders in ≤ 3 s on the M4 Max (measured 0.3 s); the zip opens in macOS Archive Utility. |

---

## 5. Further hardware-exploiting features (next-after: not now)

### 5.1 DEEP PLATE: the idle second core (next release; the strongest headroom lever)

- **What it is.** Render half the voice slots on core 1. Turn on `cpu2 = true, cpu2_fallback = true`; `plat_cpu2_set(1)` in `app_init` (`include/plat.h:214-234`).
- **Each render:**
  - core 0 applies events and reads `cy_geom_object()` once;
  - `plat_cpu2_run(render_hi)`: core 1 renders slots 4–7 into its own L/R sub-bus (2 × 128 floats, .bss);
  - core 0 renders 0–3 + the fade pool + SURFACE, then `plat_cpu2_wait()`;
  - the sub-buses are summed in a fixed order, then the FX.
  - Host and wasm always sum two sub-buses, so `demo_hash` changes once and is then identical split or not.
- **Gain.** Modes are ~1,540–1,730 of ~2,900 instr/sample at budget 96 (spec §3.14). Moving half frees ~800–900 instr/sample on core 0, a realistic 1.6–1.8× (ChoralRoot's dual-core worst case is still 72 % of a half). That would lift the VOICE rule (budget 96, 8 TALK voices) or give 8 bowed voices with full mode sets.
- **Why not now.**
  - Some units' core 1 misreads shared RAM, and data "can go bad too, where no check reaches" (`bsp/chip/fm1_cpu1.h:24-35`). A long-T60 mode state could carry a bad value for seconds.
  - The 8 ms job timeout is longer than a 2.9 ms block, so a hung job costs an xrun before fallback.
  - fm1-emu models neither caches nor contention.
  - The rule therefore holds: **every VOICE feature must fit on one core** (it does, §3.11), and DEEP PLATE multiplies quality only.
- **Gates when built:** sanity-check sub-bus energies and mode masks at the join; `fm1-emu --flaky-core1 RATE` passes with graceful single-core degradation; budgets fall back by 32 on a retire.

### 5.2 MODAL MIRROR: any sound in, a playable plate out (browser first; device after a bench test)

- **What it is.** Drop a WAV (a vocal chop, a bell, a stab) on the page. Offline analysis (decodeAudioData, an 8,192+ point FFT, a partial tracker, plus ESPRIT or matrix-pencil on the top partials to separate the beating doublets that define bells) gives a 128 B mode set: 32 × {ratio in u16 cents, amplitude u8 dB, T60 u8 log}.
- **How it plays.** The set travels by the Sounds SysEx (`cy_xfer`) or a share-code extension into a **new store object** with a separate MIRROR flag. It never extends P_SHAPE: SHAPE is a continuous 0..8 morph (`src/params.c:494`), and a 10th slot would remap every saved sound. Strike and mic positions still come from the current SHAPE, so the sand keeps forming figures (stylised, and labelled so).
- **The device path later.** It uses the same `plat_usb_audio_read` hook, analysis in the main loop, the software FFT as the reference, and JL_FFT (0x1EE0400, IRQ 57; protocol in the AC79 SDK `apps/common/jl_math/jl_fft.c:295-336`; register sketch at `build-test/voice-design/hw/jl_fft_regs.h`) only behind a timeout, after a bench test. It would be the first FM-1 firmware to use the FFT block. Main-loop cost ≤ 1.3 % of a core; ~20 KB .bss; 0 pool.
- **Why not now.** It needs a tonalness check ("not tonal enough" message), a new store object, and real-material validation. Sustained sources have no meaningful T60.

### 5.3 Queued, not designed here

| Idea | Why later |
|---|---|
| ROBOT / HARMONY / voice-to-MIDI | Re-measure the proposed YIN configuration (fs/8, 80 samples, every 4 blocks) and save the output first. Octave errors below 100 Hz; 100–170 ms quantised latency. |
| SCALE CATCHER (key LEDs follow the song) | Needs an FFT and an fm1-base API for the 64-level LED mode. Lit LEDs shift pressure ADC readings by ~25 counts. |
| MIDI clock sync + RAIN SYNC; CC map with knob CC out; MPE out | Producer wins from the export track. Clock is already passed (`usb.c:973-975`); Song Position is dropped and needs a ~10-line upstream fix. Each needs its own flash gate. |
| SURFACE 27-point kit export; vowel multisamples; capture upgrades (tempo `.mid`, recipe in the WAV) | Browser-only follow-ons to §4 |
| USB host headset, HFP headset mic, A2DP sink | §0.3 |
| Melodee-style explicit-feedback 24-bit UAC OUT (no resampler) | Better than any interpolator, but needs a free IN endpoint and descriptor work (`reference/melodee/firmware/src/usb_audio_stream.c:1-9`) |
| Pin-19 analog input, S/PDIF in, TRS out via USB-C, opto pitch follower | Out of policy (§0.4) |

---

## 6. Risks

| Risk | Mitigation |
|---|---|
| UAC OUT has never run on CYMA-1's descriptors or any verified hardware | Separate VOX image (D4); loopback-first hardware gate (G-HW); the default image is unchanged |
| Device CPU is static counts plus estimates; fm1-emu has no cache model | The VOICE rule (D8); G-PERF with DEMO; hot code in `.ram_text`; the 75–85 % band flagged for hardware |
| Intelligibility on real voices differs from the Klatt model | G-Q before any "high-end" claim; HOLD and FORMANT defaults tuned on real speech |
| High chords talk poorly (C5–G5: correlation 0.55–0.73 in the model) | Docs: "speak low, play low"; FORMANT down helps |
| TALK at budget 80 / 4 voices may lose F2–F3 coverage | G-TALK is measured at the device rule; if it fails, TALK polyphony drops to 3 before the budget rises |
| RING pumps on sung vowels, or runs hot | Governor time constants specified; G-LVL with a sustained in-tune vowel |
| Phone input routing (iOS picks CYMA-1 as input) | Documented app settings; a later output-only descriptor variant |
| The aggregate device drifts | Drift correction on the FM-1; the servo absorbs ±1000 ppm (simulated) |
| The patch needs maintaining until upstreamed | Pinned fork + lock file; upstream PRs carry the evidence |
| `.adv` rejected by Live | G-E1 by the user; the type 6 fallback layout; static validator |
| claude.ai CSP blocks the Worker | The time-sliced fallback (G-E5) |
| Export kits lose pressure, morph and bow interaction | Stated in README; the instrument stays the real thing |

---

## 7. Cut order (cut from the top; never cut the last block)

1. VOWEL MAP view (V4's `gen_vowel_map.py`, `vowel_map_data.c`, view code).
2. FLAC, Decent Sampler and Simpler outputs in the export.
3. RING mode (ship TALK alone).
4. USB REC FULL / `APP_GAIN_F` (docs instead: "set MASTER to max when recording into Live").
5. DEMO voice on the device (keep it in host and wasm for tests). G-PERF then needs a `FM1_HOST_USB_IN`-like emulator injection, so this cut moves G-PERF to estimates only and blocks the VOX image.
6. The Catmull-Rom read (back to v1 linear, 31 instr/frame; ESS dims at 10 kHz). The re-prime, web ring and provenance fixes stay.
7. The VOX device image entirely: ship the browser VOICE (mic, file, demo) and Export as instrument only.

**Never cut:** VOICE OFF bit-exactness (G-OFF); the EXCITED_GR kernel whenever TALK ships on the device; the gate and dropout fades; the RING governor if RING ships; the default image unchanged (S2); the hardware gate wording.

---

## 8. Module breakdown (for parallel implementation)

**Precondition.** Start from `instrument/` after the current build/test workflow has finished; rebase if it is still landing. No module edits a file another module owns. At kickoff the integrator (V3) commits:
- the frozen headers `src/cy_vin.h` and `ui/ui_voice.h` exactly as in §8.2;
- the two include lines in `web.c` (`#include "web_vin.c"`) and `index.html` (`<script type="module" src="export/export-ui.js">` plus an empty `#voice-panel` container);
- empty stub files for every new path.

### 8.1 Modules and files (disjoint)

| ID | Module | Files owned | Depends on | Acceptance tests |
|---|---|---|---|---|
| **V0** | fm1-base fork + patch v2 | `vendor/fm1-base-cyma/` (whole fork); `build-test/voice-design/hw/audio-in/fm1-base-patch-v2/*`; upstream PR texts | — | Patch applies to 086cd0c; template A/B build ok (uac_read linked, uac_play absent, loop ≤ 90 instr/frame, no calls); `sim_uac_v2` all rows as in §3.13; host + web builds; wasm imports unchanged; default builds (`usb_audio_out = true` and `false`): `uac_play` path disassembly identical to unpatched fm1-base, image within +64 B; **CYMA-1 VOX build re-measures the §3.12 patch row** |
| **V1** | EAR + VOICE DSP | `src/vin.c`, `src/vin_demo.c`, `src/cy_vin.h` (frozen content), `tests/unit/test_vin.c`, `tools/vin_ref.py` | I-V3 | G-EAR; DEMO phrases deterministic (hash); gate silence exact; dropout fades; loop counts in pinned clang ≤ §3.11 rows; `.ram_text` subset call-free |
| **V2** | Engine VOICE hooks | `src/engine.c`, `src/voice.c`, `src/bow.c`, `src/cy_engine_int.h`, `tests/unit/test_engine_vin.c` | I-V3 | G-OFF (bit-exact); EXCITED_GR ≤ 22 instr counted; bow ramp-only ≤ 32; slot gain targets ≤ 20/sample at 96 modes; RING governor unit test (sustained sine bounded); host `est_max` with VOICE ≤ gates |
| **V3** | App integration (integrator) | `app.c` (hooks only), `app_voice.c` (new, included at the end like `app_sounds.c`), `words.tsv`, `README.md` (VOICE section, "Headphones as a mic: do it at the computer", Live setup, latency), `FM1_BASE.lock` | V1, V2 interfaces | Settings v3 migration (v1 → v3, v2 → v3, v3 into a v2 build → defaults); hold-FX page; SEL+PLAY bypass; MODE boots OFF; VOICE rule applied and released; FREEZE holds the vowel; host commands I-V5; USB REC FULL calls `plat_usb_audio_full` and `APP_GAIN_F` |
| **V4** | UI | `ui/voice_ui.c`, `ui/ui_voice.h` (frozen content), hooks in `ui/chrome.c`, `ui/views.c`, `ui/sand.c`, `ui/palette.h`; `tools/gen_vowel_map.py`, `src/vowel_map_data.c` (generated, committed) | I-V6 | Contact sheet with VOICE page A/B, ladder curve, meter, `VOX` badge, VOWEL MAP; UI frame ≤ 8 ms device (fm1-emu) with VOICE on; vowel-map distinctness ≤ 0.75 on 8 shapes (re-measured from the generated table) |
| **V5** | Browser VOICE | `web/emu/index.html` (Voice panel; the export include line is V3's), `web/emu/worklet.js`, `web.c`, `web_vin.c`, `tools/cyma_local.py` (mic hint only) | V0 web ring, V1 | G-WEB; the file source works in the artifact frame model; mic flow on localhost with a fake device; latency readout present |
| **V6** | Tests, perf, eval, images | `tools/run_all.sh`, `tests/voice/*.txt`, `tools/voice_golden.py`, `tools/voice_eval.py`, `tools/emu_perf.py` (new scenarios only), `tools/wasm_smoke.mjs`, `tools/chrome_test.mjs` (voice cases), `build-test/voice-design/eval/` (clips + attribution) | all | Builds both images; G-TALK, G-Q, G-ESS, G-SPILL, G-GATE, G-DROP, G-LVL, G-PERF, G-MEM, G-SAFE; REVIEW_REPORT gets a VOICE budget row |
| **E1** | Export render | `web/emu/export/export-worker.js`, `kit_plan.js`, `capture.js` (zone render, tail, loop bake, RR null test), `wav24.js`, `flac.js` (optional), `zip_stream.js` | existing exports only | G-E3; G-E6; loop crossfade seamless (no step at the loop point); peaks flagged above −1 dBFS |
| **E2** | Export formats | `web/emu/export/adv_writer.js`, `sfz_writer.js`, `ds_writer.js`, `readme_writer.js`, `tools/adv_skeleton.py` → `web/emu/export/adv_skeleton.json`, `tools/validate_adv.py` | I-E1, I-E3 | G-E2; G-E4; SFZ and DS parse (sfizz CLI if available, else an XML/opcode schema check) |
| **E3** | Export UI + packaging | `web/emu/export/export-ui.js`, `tools/package_web.py`, `tools/export_test.mjs` | E1, E2 | G-E5; the artifact build includes the worker as a supporting file; Cancel works; the size estimate is within ±30 % of the actual |

### 8.2 Frozen interfaces

**I-V1 `plat_usb_audio_read`** (fm1-base `include/plat.h`, as in the v2 diff):

```c
/* the computer's audio to the app (fm1.toml usb_audio_out = "app": FM1B_UAC_OUT_APP): it is not mixed after the
 * render; app_render reads it, once per call, n = the call's frames: resampled to the I2S clock (~44,117.6 Hz), L R
 * interleaved floats, full scale +-1.0 (the host sends 16-bit stereo at 44.1 kHz). Returns n, or 0 with lr zeroed
 * while the computer is not playing (or the ring primes or ran dry). Call it every render while the input matters:
 * left unread for more than four blocks, the ring starts again at its target (the old audio is dropped, never played
 * late). Audio interrupt (app_render) only. Host builds: the raw file named by FM1_HOST_USB_IN (16-bit stereo
 * little-endian at 44.1 kHz), else silence; the browser: what the page pushes (web_uin_buf, web_uin_push) */
uint32_t plat_usb_audio_read(float *lr, uint32_t n);
/* APP_GAIN_F(m): with plat_usb_audio_full on, the speaker's gain from MASTER's m as a float (0..1), ramped */
```

CYMA-1 defines `#define APP_GAIN_F(m) ((float)(m) * (float)(m) * (1.0f / 16777216.0f))`, the same square fx.c applies.

**I-V2 web input** (fm1-base `web/emu/emu_web.c`): `float *web_uin_buf(void)` (2 × 1,024 floats, L R interleaved, at the device rate); `void web_uin_push(uint32_t n)` (n ≤ 1,024; call before `web_render` for the same quantum).

**I-V3 `src/cy_vin.h`** (new; V1 implements the `cy_vin_*` functions, V2 implements `cy_engine_vin`):

```c
/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 src/cy_vin.h: the VOICE layer. FROZEN CONTRACT: docs/CYMA-1-voice-spec.md §8.2 I-V3. */
#pragma once
#include "cy_config.h"
#define CY_VIN_GRID 48u                  /* 40 LPC points 80 Hz..4.2 kHz (log) + 8 high-band points 4.6..11 kHz */
#define CY_VIN_NLO 40u
enum { CY_VIN_OFF, CY_VIN_TALK, CY_VIN_RING, CY_VIN_NMODE };
enum { CY_VIN_SRC_LR, CY_VIN_SRC_L, CY_VIN_SRC_R, CY_VIN_SRC_DEMO, CY_VIN_NSRC };
typedef struct {                         /* the VOICE settings in display units (main loop; cy_vin_set copies them) */
    uint8_t mode, src, phrase, freeze;
    float amount, formant_st, hold, smooth_ms, ess, trim_db, ring_decay, lowcut_hz, lean;
} cy_vin_ctl_t;
typedef struct {                         /* one render's VOICE (audio ISR): built by cy_vin_block, read by the engine */
    uint8_t mode;                        /* CY_VIN_OFF: the engine changes nothing (bit-exact) */
    uint8_t frozen;                      /* env_db, gate and level held (FREEZE in TALK) */
    float amount, hold, ring_decay, lean;
    float formant_ratio;                 /* 2^(-formant/12): the envelope is sampled at f x this */
    float env_db[CY_VIN_GRID];           /* TALK: followed envelope, shape only (voiced-band mean removed), dB */
    float level;                         /* TALK: input loudness / -18 dBFS, 0..4 */
    float gate;                          /* 0..1 at the block's end (the engine ramps from the last block's) */
    const float *x;                      /* RING: CY_BLOCK mono excitation (trimmed, low-cut), else 0 */
    const float *ess;                    /* CY_BLOCK mono ESS signal (gated, scaled) added to both channels, else 0 */
    uint32_t seq;
} cy_vin_frame_t;
typedef struct {                         /* telemetry (seqlock; the UI and the page read it in the main loop) */
    uint32_t seq;
    float in_db, gate, unvoiced, f1_hz, f2_hz;
    float env_db[CY_VIN_GRID], lpc[11];
    uint16_t dropouts;
    uint8_t mode, src, have_input, frozen;
} cy_vin_tel_t;
void cy_vin_init(void);
void cy_vin_set(const cy_vin_ctl_t *c);                                    /* main loop */
const cy_vin_frame_t *cy_vin_block(const float *lr, uint32_t got, uint32_t n);   /* audio: in app_render, before
                                                                           * cy_engine_render; lr may be 0 (DEMO) */
void cy_engine_vin(const cy_vin_frame_t *f);                               /* audio: this render's frame (engine.c) */
int cy_vin_tel_get(cy_vin_tel_t *t);                                       /* main loop: 1 = new since last */
/* the grid (both sides use exactly this): point i < 40 sits at 80 Hz x 2^(i x CY_VIN_LO_OCT); point 40 + j at
 * 4.6 kHz x 2^(j x CY_VIN_HI_OCT). A frequency's position p (0..47, fractional): p = log2(hz / 80) / CY_VIN_LO_OCT
 * up to 4.2 kHz; 39 from 4.2 to 4.6 kHz; 40 + log2(hz / 4600) / CY_VIN_HI_OCT above; clamped to 0..47. The engine
 * computes it inline at a mode's frequency refresh with cy_i_log2f (cy_engine_int.h), no call (.ram_text) */
#define CY_VIN_LO_HZ 80.0f
#define CY_VIN_LO_OCT (5.714246f / 39.0f)      /* log2(4200 / 80) / 39 */
#define CY_VIN_HI_HZ 4600.0f
#define CY_VIN_HI_OCT (1.257797f / 7.0f)       /* log2(11000 / 4600) / 7 */
```

**I-V4 settings v3:** `app_voice_set_t`, 32 B, appended to `app_settings_t`; size 192; `APP_SET_VERSION 3`; migration as in §3.8.

**I-V5 host commands and expects:** §3.19.

**I-V6 `ui/ui_voice.h`** (the frozen `ui/ui.h` is untouched; page value 5 means VOICE in `cy_ui_state_t.page`, whose field is a `uint8_t` with room):

```c
/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 ui/ui_voice.h: the VOICE page and views. FROZEN CONTRACT: docs/CYMA-1-voice-spec.md §8.2 I-V6. */
#pragma once
#include "src/cy_vin.h"
#define CY_PG_VOICE 5                    /* cy_ui_state_t.page == 5: the VOICE page (PG_N stays 5) */
#define CY_VIEW_VOWEL 3                  /* cy_ui_state_t.view == 3 while TALK: VOWEL MAP (CY_NVIEW stays 3) */
typedef struct {
    const cy_vin_tel_t *tel;             /* the latest telemetry copy */
    uint8_t mode, layer, focus_knob;     /* focus_knob 0..3, 0xFF none */
    char label[4][8], value[4][12];      /* the four cells, formatted by app_voice.c */
    float bar[4];                        /* 0..1 value bars (bipolar: -1..1) */
    uint8_t bipolar_mask, vox_build, no_input;
} cy_ui_voice_t;
void ui_voice_state(const cy_ui_voice_t *v);   /* the app, before ui_frame, every frame while VOICE matters */
```

**I-E1 export sources:** the existing exports (`web_boot`, `web_master`, `web_render`, `web_out_l/r`, `web_midi_in`, `web_cy_load`, `web_cy_box`, `web_cy_audition`, `web_cy_cur`, `web_cy_info`). No new firmware export. Worker messages: `{type:'kit', sound:<94 B>, plan}` → `{type:'zone', i, n}` progress → `{type:'done', blob}` or `{type:'error', text}`.

**I-E2 zip layout and naming:** §4.5.

**I-E3 `adv_skeleton.json`:** `{schema:"11.0_11300"|"12.0_12120", root:<element tree with ordered children and default attribute values>, part:<MultiSamplePart template>}`, produced by `tools/adv_skeleton.py --schema DIR`.

### 8.3 Order

1. **Day 0:** V3 commits the frozen headers, stubs and include lines. V0 builds the fork and the CYMA-1 VOX image skeleton (VOICE stubs), and re-measures §3.12's patch row.
2. **In parallel:** V1, V2, V4, V5, E1, E2. V6 writes the scenarios and eval harness against the stubs.
3. V3 integrates V1 + V2 (host first: G-OFF, G-EAR, G-TALK), then V4 and V5.
4. V6 runs G-PERF on the VOX image with DEMO. If anything is over 85 %, the VOICE rule tightens (TALK polyphony 3, then budget 72) before the release; the governor is never the fix.
5. E3 packages; G-E1 waits for the user's 2-minute drag test.
6. Release notes: two images, the honest headphone answer, the latency figure, "never flashed by us".

---

## Appendix A. Evidence index (this spec's own runs)

| What | Where |
|---|---|
| fm1-base patch v2 (diff, header with the build and sim results) | `build-test/voice-design/hw/audio-in/fm1-base-patch-v2/fm1base-usb-audio-read-v2.diff` |
| Its staged trees (a = 086cd0c, b = v1 + v2) | `…/fm1-base-patch-v2/a/`, `…/b/` |
| v2 simulation source and output | `…/fm1-base-patch-v2/sim_uac_v2.c`, `extracted_uac_v2.c`, `sim_uac_v2.out` |
| v2 device build (template app, usb_audio_out = "app"), disassembly, log; package deleted | `build-test/voice-design/hw/audio-in/devbuild-v2/usbin/build/template.dis`, `devbuild-v2/build.log` |
| v2 host and web builds; wasm import check | `devbuild-v2/host.log`, `web.log`, `usbin/build/emu/app.wasm` |
| Resampler flutter computation (linear / Catmull-Rom / 8-tap sinc) | reproduced by `sim_uac_v2.out`; linear at 10 kHz −2.42 dB, Catmull-Rom −0.74 dB, an 8-tap Kaiser sinc −0.09 dB (a later option) |
| Vocoder kernels, counts, model metrics, renders | `build-test/voice-design/kernels/`, `tools/`, `out/` (`device_budget.json`, `talking_metrics.json`, `extra_metrics.json`, `ear_choice.json`, `c_kernel_check.json`, `vowel_map_metrics.json`) |
| Export prototype, kits, benches | `build-test/voice-design/daw-export/proto/`, `bench/` |

Nothing in this work touched a USB device, sent MIDI to a real port, flashed anything, or touched the user's running Live session. The only container used was the pinned toolchain image for compilation.
