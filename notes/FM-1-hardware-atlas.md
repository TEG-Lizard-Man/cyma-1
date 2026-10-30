# FM-1 hardware atlas: what the M-VAVE FM-1 can physically do

**Date:** 2026-10-28.
**Status:** reference document for CYMA-1 (modal "Chladni plate" firmware on fm1-base, MPL-2.0). It merges six research tracks (SoC peripherals, pins, board, stock firmware, protocols, experiments) and an independent verification pass over each one. Where a verifier corrected a claim, this atlas uses the corrected version. Refuted claims are left out of the tables and listed in Appendix A so no later document repeats them.
**Supersedes:** the "hardware ceiling" table in `docs/research-briefing-2026-10-24.md` §2 and the pin notes in `reference/fm1-custom-fw/REVERSE_ENGINEERING.md` §3, wherever the two disagree with this atlas.
**Consistent with:** `docs/CYMA-1-voice-spec.md` §0 (the audio-input verdict). §2 below refines it with new evidence and corrects three of its hardware notes (listed there).

## How to read this

**Confidence labels:**
- **V (verified):** read directly in code, a datasheet or a disassembly, or measured, by two independent passes.
- **S (strong inference):** several consistent sources, but no direct test on an FM-1.
- **P (speculative):** plausible, with gaps in the evidence.
- **↓** marks a claim that verification **downgraded or corrected**. The corrected statement is the one shown.

**Path prefixes used in evidence cells:**

| Prefix | Path (relative to the project root) |
|---|---|
| `R/` | `reference/` |
| `SDK/` | `reference/fw-AC79_AIoT_SDK/` (Apache-2.0) |
| `ASM/` | `reference/fw-AC79_AIoT_SDK/include_lib/driver/cpu/wl82/asm/` |
| `JLM/` | `reference/fw-AC79_AIoT_SDK/apps/common/jl_math/` |
| `DS` | AC7911B datasheet V1.3 (PDF under `SDK/doc/datasheet/AC791N规格书/datasheet/`; text at `build-test/re/soc-peripheral-atlas/AC7911B_Datasheet_V1.3_20230303.txt`) |
| `SEL` | JieLi selection sheet `SDK/doc/datasheet/AC791N规格书/datasheet/AC791N选型表231122.xlsx` |
| `[soc]` `[soc-v]` | `build-test/re/soc-peripheral-atlas/`, `build-test/re/soc-verify/` |
| `[pins]` `[pins-v]` | `build-test/re/pins/`, `build-test/re/pins-verify/` |
| `[board]` `[board-v]` | `build-test/re/board-teardown/`, `build-test/re/board-verify/` |
| `[fw]` `[fw-v]` | `build-test/re/stock-fw-internals/`, `build-test/re/firmware-verify/` |
| `[proto]` `[proto-v]` | `build-test/re/protocols-connectivity/`, `build-test/re/protocols-verify/` |
| `[exp]` | `build-test/re/experiments/` |
| `UPEMU` | https://github.com/joakimeriksson/fm1-emu master as of 2026-10-28: `crates/fm1-soc/src/hwdsp.rs`, `hweq.rs`, `hwsrc.rs`, `tests/hwdsp/fm1.txt`. These models were fitted to measurements on a real FM-1 made with fm1-dock's `dsp-probes` branch (`probes/hwdsp.c`, `probes/hweq.c`). I re-fetched `hwdsp.rs` and `hweq.rs` while writing this atlas, and their module notes match what is quoted here. |

**Firmware addresses:** V15 `app.bin` loads at 0x2000120 and V16 at 0x2000140. FM-1-RE's V13/V14 addresses are 0x120 too low.

**Rules this atlas follows:**
- **No hardware modifications.** The user will not solder or modify the FM-1. Anything that needs a mod appears only in clearly marked "out of policy" rows, for completeness.
- **No writes without asking.** Never flash or write to the FM-1 without the user's explicit OK for each step (`notes/INSTALL.md`). The research itself touched no USB device, no MIDI port and no unit.
- **Licences.**
  - `R/fm1_open/sdk/lib/libfm1_engine.a`: its terms grant use, linking and redistribution on the FM-1 only. They contain no explicit ban on reverse engineering, but grant no permission for it either. No track disassembled it; only fm1_open's public headers and its GPL app sources were read. Those headers expose no FFT, EQ, SRC or CORDIC.
  - **Stock V16:** proprietary. It was studied for interoperability only, and nothing was copied.
  - **Stock V15:** the recovery image.
  - **JieLi SDK libraries:** Apache-2.0. Their LLVM bitcode was turned into IR to read register sequences.
  - **Never commit or redistribute** the listings and unpacked images under `[fw]`, `[proto]/fw` and `[soc]/dis`.
  - **CYMA-1 code** must re-express register sequences from Apache-2.0 headers and IR, never M-VAVE code. GPL-3.0 community code (Melodee, Optimist) cannot go into the MPL-2.0 tree, except felucca-bsp's MPL-2.0 port of Melodee's USB host, which Melodee's authors permitted.

## Headline corrections to earlier notes

These change what earlier project documents (and much community folklore) say. Each is backed in the tables below.

1. **The chip is an AC7911B8, and pins 37-41 are PB7, PB6, PB5, HUSBDM and HUSBDP.** They are not the external-flash pins. The flash is inside the package.
   - REVERSE_ENGINEERING.md:115-121 has this wrong.
   - U12 (SOIC-8) is the charger, not flash.
   - "All 48 pins are used" is unproven. The board routing of PB5 (pin 39), HUSBDM/HUSBDP (pins 40/41) and DACL (pin 13) is unknown.
   - (V for PB6/PB7; S for PB5/HUSB.)
2. **Stock firmware has been dual-core since V15.**
   - V15's second core renders the whole 12-voice engine. It runs at 360 MHz, with VDC14 and SYSVDD set above the datasheet maxima.
   - V16 splits 20 voices 16:4: a bare-metal loop on core 1 and an RTOS task on core 0. V16's bootloader drops the clock to 320 MHz.
   - The community belief that "stock is single-core" (`R/ChoralRootFM1/docs/DUALCORE.md:445-446`) is wrong. (V)
3. **V15 is not a 16-bit path.** Both V15 and V16 quantise to full-scale Q23, then apply a 512-segment tanh soft clipper. (V)
4. **Two instruction-set myths.**
   - Stock does not use packed 16-bit SIMD: every hit is in data tables.
   - Stock does use a single-instruction float multiply-accumulate. JieLi's clang 4.0.1 never emits it, even with `-ffp-contract=fast`; inline asm does. (V)
5. **The FFT, vector, EQ and SRC accelerators have been measured on a real FM-1, but no shipping firmware uses them.**
   - The measurements were made by a third party on 2026-10-28 (UPEMU).
   - V16 contains FFT and vector code, apparently dormant.
   - Melodee has an opt-in hardware-EQ path that has never been validated. (V)
6. **The stock FM-1 is a Bluetooth audio receiver (A2DP sink).**
   - It registers one SDP record, `JL_A2DP` AudioSink 0x110B, and drives the hardware SBC decoder.
   - Its BLE service 0xAE40 is a binary command channel: writes to handle 0x62 carry the same `00 59` frames as USB SysEx. (V)
7. **The DAC wire format is standard Philips I2S:** 24-bit data in 64-BCLK frames, master mode. That points to a CS4344-class DAC, not a CS4345. (S)
8. **Audio-ADC pins** (CYMA-1-voice-spec §2 H2 needs this correction):
   - MIC1P = PH9 (pin 18) and MIC1N = PH8 (pin 19). The SDK driver's own pin table agrees with the datasheet; only the comment in `ladc.h` is reversed.
   - MIC2N = PA4, MIC3P = PH6, AUX1 = PH7, AUX2 = PA5, AUX3 = PH4.
   - PA6 (the top-7-key row) has no analog function at all.
   - JieLi's selection sheet lists only **2** MIC/AUX channels for the B8 variant (4 for the B0). (V for the pins; S for the channel count)
9. **UART1 pins:** PH7 is UART1_RXA and PH6 (an LED line) is UART1_TXA. DACL is analog-only and cannot carry a UART. (V)
10. **Only 74HC595 bits 13-15 can be spare,** not five. (V on the firmware side)
11. **48 kHz USB audio already ships in community firmware:** Felucca 1.1.5+, SLOOP 2.5+ and others. `uac_fir.h` exists in several repos. (V)
12. **An MPL-2.0 USB-MIDI host already exists:** felucca-bsp has a permitted port of Melodee's (`FELUCCA_USB_HOST`). (V)
13. **The FCC grant 2ARCP-FM1 covers Bluetooth BR/EDR (DSS) only.** It has no Wi-Fi line, and no BLE (DTS) line is visible. Shipping Wi-Fi features sits outside the documented US certification at any power. (V for the filing contents)
14. **No I2S audio input exists.** The ALNK receive data pads PC3 and PC4 are not bonded. Now that the B8 re-bond is known to touch only pins 37-41, the datasheet's port-C table applies. So the "RX on PC3" in every FM-1 HAL is only an interrupt-cadence source. (S, upgraded from "unproven" in CYMA-1-voice-spec §2)
15. **The core-0 cycle counter `C0_TL_CKCNT` does not count while `DBG_CON = 0`.** X0X measured this on a unit. Profiling must use TIMER4 or opt in to DBG_CON. (V)

---

## 1. Executive summary for a music producer: ten things this box can do that nobody uses yet

"Nobody uses" means no shipping firmware: stock, M-VAVE's open SDK, or any of the ~60 community firmwares. Where a community member has probed a feature without shipping it, that is noted.

1. **A built-in resonator chip.**
   - **What it is.** The processor has a filter coprocessor that runs banks of two-pole filters in floating point, on its own, while both CPU cores keep working. A Chladni plate mode is exactly a two-pole resonator, so this block is a second, free modal bank.
   - **Measured on a real FM-1.** Up to 64 filter sections and 15 output channels per run. Each mode costs about 0.11-0.14 µs per sample of coprocessor time. The CPU only mixes the outputs, at an estimated 4-5 instructions per mode-sample against 16-19 today.
   - **Musically:** an estimated 60-150 extra ringing modes for long decays, gongs and bells, on top of today's 96 (see §4 rank 2).
   - **Who uses it:** nobody ships it. Melodee has an untested opt-in.
   - *Confidence:* block and timing **V** (third-party hardware measurement); suitability for very long, high-Q plate decays **S** (precision at Q > 1000 is untested).
2. **A hardware spectrum engine (FFT plus vector maths).**
   - **Measured speed:** a 512-point FFT in 16.6 µs and a 1024-point in 40 µs, beside the CPU.
   - **Musically:**
     - a live spectrum and nodal-energy display that really analyses the sound;
     - "play any sample as a plate" (MODAL MIRROR) on the device;
     - better voice analysis for the talking plate;
     - convolution with recorded plates;
     - additive layers with hundreds of partials.
   - **Who uses it:** M-VAVE's V16 contains FFT code that seems unused. No shipping firmware uses it.
   - *Confidence:* **V** (SDK, V16 disassembly and third-party measurement).
3. **Native 48 kHz and 96 kHz output.**
   - **Why it works:** the chip has a second audio clock, and stock V15 even links an (unused) routine that switches to 48/64/88.2/96/176.4/192 kHz.
   - **48 kHz** matches DAW sessions and removes resampling for USB at 48k.
   - **96 kHz** is built-in 2× oversampling for bowed and driven sounds and for crisp top modes. It halves the CPU per sample.
   - **Status:** never tried on an FM-1.
   - *Confidence:* **S** (the DAC part is not read yet; CS4344-class parts accept these clock ratios).
4. **Your computer or phone as the plate's microphone, in real time.**
   - **How:** the FM-1 is already a USB sound card in both directions, so host audio can drive the plate: a vocoder, a sympathetic resonator, a resonant effects box.
   - **Who uses it:** this is CYMA-1 VOX's headline. No other firmware feeds host audio into synthesis live; PIGEON records it as samples only.
   - *Confidence:* **V** for the USB path (stock and fm1-base implement it); the CYMA-1 VOX image has not run on hardware.
5. **Both cores, the way M-VAVE does it.**
   - **What stock does:** it has always rendered its synth on the second core (V15 included). V16 runs a lean bare-metal render loop there, with an 80/20 voice split and live lateness counters.
   - **CYMA-1 today:** it leaves the second core idle. Using it is the biggest headroom lever: about 1.6-1.8× the modes or voices.
   - *Confidence:* **V** that stock does this; per-unit reliability is the risk, because some units' core 1 misreads RAM.
6. **Hidden fast-maths instructions.**
   - **Float multiply-accumulate:** stock uses this one-instruction "multiply and add". JieLi's compiler cannot emit it, but hand-written assembly can. Estimate: about 20% cheaper modal kernels, which means more modes.
   - **CORDIC "maths engine":** an instruction for sin/cos/exp/log/sqrt/tanh that no one has ever executed on this chip.
   - *Confidence:* multiply-accumulate **V** (whether its result is rounded once or twice is unknown); CORDIC **P** to **S**.
7. **It is secretly a Bluetooth speaker.**
   - **What stock does:** it advertises as a Bluetooth audio receiver and decodes in hardware.
   - **Musically:** a phone could stream a song, a drone or field recordings into the plate as an exciter or backing.
   - **Limits:** latency is 100-250 ms, so this is for textures, not playing. The Bluetooth stack does not fit next to CYMA-1's current memory budget.
   - *Confidence:* **V** for the capability; **low feasibility** today.
8. **Hardware varispeed and compressed sample memory.**
   - **Hardware sample-rate converter:** measured at about 7× real time in mono. It can repitch mallet, bow and tap samples smoothly.
   - **Hardware SBC codec:** could store several times more exciter or impulse material in the 1 MB of flash, with almost no CPU.
   - *Confidence:* SRC **V** (measured); SBC **S**.
9. **Your own wireless channel.**
   - **Phone link:** stock shows the pattern, a private Bluetooth LE service next to BLE-MIDI. A Web Bluetooth page on a phone could act as a live "sand" visualiser and editor, with binary frames up to ~1 KB.
   - **Plate ensembles:** two FM-1s can already link over BLE-MIDI in fm1-base. Timestamped clock sync between them is unclaimed.
   - **Cost:** the Bluetooth stack's memory.
   - *Confidence:* pattern **V**; fit in CYMA-1's budget **P**.
10. **The chip's own senses.**
    - **Die-temperature sensor:** slow "analog warm-up" drift.
    - **USB-power and battery rails:** power-aware modes.
    - **Hardware random-number generator:** non-repeating strike jitter.
    - **Status:** all are on-chip and unused for sound.
    - *Confidence:* random generator **V**; temperature and rail readings **S** (they need an analog-mux step that only JieLi's closed driver performs today).

**Honourable mentions (not top ten, with the reason):**
- **USB-C as an accessory port** (MIDI out, expression pedal, I2C CV) with no case opening. **S**. It needs a home-made adapter cable, which CYMA-1-voice-spec §0.4 classed as out of policy; the user should decide.
- **USB-MIDI 2.0** high-resolution per-note pressure. **S**; nobody does it, but host support varies.
- **Multichannel USB stems and "CV over audio" lanes.** **V**; Melodee and JIANT already do stems.
- **A WebUSB vendor channel** at ~830 KB/s. **V**; RemoteFM1 already does it.
- **Raw 802.11 peer-to-peer.** **S**; not covered by the FCC filing.
- **BLE 5 broadcast sync.** **P**.
- **Graded LED brightness through MCPWM.** **V**; Melodee does it.

---

## 2. The audio-input question, settled

### 2.1 Verdict

**On an unmodified FM-1, no microphone or analog signal can reach the chip's audio ADC.** That rules out:
- the headphone jack, which is output-only;
- the TRS MIDI jack, which sits behind a logic optocoupler;
- every internal pin, because each one that can reach the audio ADC is already wired to keys, LEDs, the shift registers or MIDI IN;
- the I2S bus, whose receive pads are not bonded.

**Every working microphone path is digital.** In order:
1. **USB audio from a computer or phone.** This works today and is the right path for the talking plate.
2. **Bluetooth A2DP.** The stock firmware proves the hardware, but the latency is 100-250 ms and the stack does not fit CYMA-1's memory today.
3. **USB host to a USB microphone.** Research-grade.
4. **Wi-Fi streaming.** Does not fit, and is not certified.

Analog inputs exist only as board modifications, which are out of policy. They are documented in §2.4 so the question never has to be researched again.

This agrees with `docs/CYMA-1-voice-spec.md` §0 and tightens it in three places:
- **MIC1 polarity.** The SDK driver table confirms the datasheet: PH9 = MIC1P, PH8 = MIC1N.
- **PC3 is not bonded.** Strong inference, no longer "unproven".
- **The B8 variant may expose only two audio-ADC channels.** Selection sheet.

### 2.2 Why each physical route is closed

| Route | What is physically there | Confidence | Evidence |
|---|---|---|---|
| Headphone jack J7 | U9 (10-lead I2S DAC, no ADC and no control port) → U5 (QFN-16 headphone driver) → FB4/FB5 → J7. The jack has 6 terminals and a normally-closed detect leaf that mutes the speaker amp in hardware. It has no mic contact, no bias and no path to any SoC pin. | V (topology), S (part identities) | `[board]/crops/top-audio.jpg`, `a1-u9-u5.jpg`, `j7-u5.jpg`; `R/fm1_open/app/board/fm1_audio.h:7-9` ("FM1 has no connected analogue input"); manual p.11 (`[board]/ds/manual_all.txt:572-591`) |
| TRS MIDI IN J3 | The jack drives the LED of PC1 ("OCIC P2362", TLP2362-class, open-collector logic output, IFHL ≤ 5 mA). The opto output drives PH8 on pin 19. Audio through it is a 1-bit threshold at best. | S | `[board]/crops/j3-opto.jpg`; https://toshiba.semicon-storage.com/ap-en/semiconductor/product/isolators-solid-state-relays/detail.TLP2362.html |
| Audio ADC (MIC0-3, AUX0-3) | Every input pad has another job: MIC0 = PA2/PA1 (backlight and rail enable / 595 latch); MIC1 = PH9 (LED line) and PH8 (MIDI opto); MIC2 = PA3/PA4 (595 clock / data); MIC3P = PH6 (LED line); AUX0 = PA0 (encoder row); AUX1 = PH7 (pin 19, with MIDI); AUX2 = PA5 (button row); AUX3 = PH4 (pin 20, with an LED line). PH5 (MIC3N) is not bonded. | V | `ASM/ladc.h:24-45`; `[pins]/sdk_objs/adc.ll:10-75` (`@mic_io_port` = PA2,PA1,PH9,PH8,PA3,PA4,PH6,PH5; `@linein_io_port` = PA0,PH7,PA5,PH4); DS pp.5-6; `[pins]/fm1_ac7911b8_pins.csv` |
| I2S receive (ALNK0 DAT0-2) | DAT0A/PC3 and DAT1A/PC4 are not bonded on QFN48. DAT2A/PC5 shares pin 34 with LRCK. The PA4-PA10 group is the key matrix and LEDs. | S ↓ | DS p.7 (`[pins]/ds-08.png`); `[fw-v]` ALNK verdict; `[pins]/fm1_ac7911b8_pins.csv` rows 33-34 |
| PDM mic link, S/PDIF receiver | No free pins on fixed functions. PB5 (PDM DAT1) is unproven and needs a solder joint. | V | `ASM/plnk.h:10-35`; `ASM/spdif.h:6-14` |

### 2.3 Ranked options that respect the no-mods rule

| Rank | Route | How it works | What is lost | Latency | Status and confidence | Risk |
|---|---|---|---|---|---|---|
| **1** | **USB Audio Class playback from a computer** (CYMA-1 VOX) | Any mic on the Mac (including passive headphones on a TRRS splitter's mic leg) feeds an Ableton External Audio Effect, which plays to the FM-1's 2-ch USB output. CYMA-1 reads the stream before rendering (`plat_usb_audio_read`, fm1-base patch v2). | Nothing. USB-MIDI stays. Costs ~86 instr/sample to read plus ~360-490 for RING/TALK, and +8.4 KB RAM. | ~15-20 ms mouth to output [est] | Path **V**: stock declares UAC OUT 24-bit; fm1-base runs 16-bit UAC OUT. VOX image never run on hardware. | Low. Separate opt-in image (voice spec D4). |
| 1a | Upgrade: native 48 kHz UAC | Host picks 48k; device resamples 160/147 (shipped in Felucca 1.1.5+, SLOOP 2.5+). With native 48 kHz I2S (§3.1 row ALNK), there would be no resampler. | ~100 instr/sample for the FIR, unless native 48k | +0.5 ms | **V** (community), **S** (native) | Low |
| 1b | Upgrade: 24-bit UAC OUT with explicit feedback | Melodee pattern: no resampler, host-locked rate. | One IN endpoint for feedback | lower | **V** (Melodee) | Medium (descriptor work) |
| **2** | **Phone over USB-C** | iPhone (USB-C) or Android as USB host; an app such as AUM or Loopy Pro sends the mic to the FM-1. | Same as 1 | phone-dependent | **S** (class-compliant; untested on phones) | Low. iOS input routing needs settings. |
| **3** | **Bluetooth A2DP sink** | The phone routes its mic or a song to Bluetooth output; the FM-1 receives SBC. Stock proves the radio, profile and hardware decoder. | ~40 KB RAM, roughly 165 KB flash (JieLi classic stack estimate) and pool CYMA-1 does not have (21 KB pool free). BLE-MIDI shares the radio. | 100-250 ms | **V** (stock capability); CYMA-1 fit: **not today** | Medium. Pairing writes link keys to flash. Route M (JieLi binaries). |
| **4** | **USB host + class-compliant USB mic or USB-C headset adapter** | SDK has host primitives, a UAC host class (`host/audio.c`) and isochronous host reads (`usb_h_iso_read`). felucca-bsp's MPL MIDI host is a starting point. | The computer link while hosting. Needs a powered hub or OTG Y-adapter (the FM-1 supplies no VBUS). | ~10 ms + buffer [est] | **P**: nobody has done isochronous host on an FM-1 | Medium. Never boot as host. |
| **5** | **Wi-Fi stream** from another device (UDP, or raw 802.11; JieLi's DevKit demo streams PCM between two AC79 boards) | Wi-Fi build plus lwIP or raw frames | ~276 KB pool (impossible next to CYMA-1); BLE excluded | 5-30 ms | **P** | Not FCC-covered. Not recommended. |
| 6 | **S/PDIF via a USB-C dock** (no case opening) | A powered TOSLINK receiver drives USB-C D−. The input crossbar routes D− (remap index 61) to the S/PDIF receiver (IOMAP modes in `spdif.h`). | USB while in use. 16-bit SRC or software resampling to 44,117.6 Hz. | low | **P**: four unverified links | Needs a home-made adapter (policy question) |
| 7 | **SAR ADC on USB-C D− (ADC12)** via an adapter with a preamp | 10-bit, a few kHz at best, shared with the key scan | USB while in use | — | **P** | Envelope or strike-trigger follower only, not audio. Adapter policy question. |

**For the talking plate:** route 1 is exactly the modulator path the VOX spec builds, and nothing beats it without hardware. Two hardware blocks in §3.1 would make that path stronger:
- **JL_FFT** could carry the voice-envelope analysis (the "EAR"). It needs about 0.6% of each 128-sample block in accelerator time, plus an estimated 20-40 instr/sample of CPU windowing and bin pooling, instead of the counted 144 instr/sample of order-10 LPC.
- **JL_EQ** could add extra filterbank bands, beyond the VOICE rule's 80-mode ceiling.

See §4.

### 2.4 Out of policy (hardware mods): the exact changes, documented once, never planned

| Mod | Exact change | Gives | Loses | Risk | Confidence |
|---|---|---|---|---|---|
| **M-A: MIDI jack → line or contact-mic input on pin 19** | 1. Lift or remove the PC1 opto output from pin 19, and any PH8 pull-up. 2. Wire J3 tip through ~1 µF to pin 19, with a bias network (JieLi's MIC block: DACVDD via 3.3 kΩ, 1 µF). 3. Ground the sleeve if it is floating. 4. Use **AUX1 (PH7)** line-in and keep PH8 high-impedance. Using PH8 = MIC1N single-ended is unproven. | 16-bit, 8-48 kHz, 87 dB DR line input through the existing jack | TRS MIDI IN (USB and BLE MIDI remain) | Medium: rework next to the QFN | S. Channel availability on the B8 is uncertain (SEL lists MIC/AUX = 2). |
| **M-B: mic on pin 18 (MIC1P / PH9)** | Cut the LED-line trace at pin 18 (row PA8's LEDs). AC-couple an electret with bias from DACVDD (~3 V per the design guide). | A true mic preamp (gain index 0-100) | 11 key LEDs | Medium | S |
| **M-C: mic on pin 20 (MIC3P / PH6, AUX3 / PH4)** | The same at pin 20 (row PA7's LEDs). Single-ended MIC3 is JieLi's own story-machine reference design. | As M-B | 11 key LEDs | Medium | S |
| **M-D: PDM MEMS mic** | Clock: PB5 via output channel CH0/CH1 (PLNK SCLK). Data: an HS-USB pad (remap 62/63, unproven) or USB-C D± (remap 60/61). | A clean digital mic, no analog front end | Nothing, if PB5 is truly free | Medium-high: fine pitch; PB5 unproven | S/P |
| **M-E: S/PDIF on PC7** | Tie the LCD's CS low to free PC7 = SPDIF_A; fit a TOSLINK receiver. | Digital audio in, 32-48 kHz | LCD CS robustness (a desync needs a power cycle). Excludes sharing SPI1 with a DAC. | Medium-high | S |
| **M-F: hot line audio into the MIDI opto** | A series resistor; a 1-bit zero-cross signal on PH8 | A pitch follower only | Nothing | Low-medium | S |

The headphone jack cannot become an input, even with mods, short of rewiring J7 to one of the pins above.

---

## 3. The atlas tables

### 3.1 SoC blocks (JieLi AC7911B8, WL82 family)

**Column guide:**
- **Status:** what stock / community firmware does with the block today.
- **How CYMA-1 reaches it:**
  - *fm1-base API:* already exposed.
  - *fm1-base patch:* needs a BSP change.
  - *Register code:* small app-side code.
  - *Mod:* out of policy.
- **Cost / risk:** CPU, memory and operational risk.

| Block | Status on the FM-1 | How CYMA-1 reaches it | Cost / risk | Evidence | Conf. |
|---|---|---|---|---|---|
| **CPU complex.** 2× pi32v2 (r3). Single-precision FPU (no hardware sqrt, doubles in software). 64-bit MAC builtins. `rep` loops ≤ 32 B. 128 vectored IRQs (DS says 4 priority levels; SDK API takes 0-7). I- and D-cache 32 KB each, 32-B lines. 578 KB SRAM total. **No SDRAM on the B8.** | Used by all. Stock V15 runs 360 MHz; V16's bootloader sets 320 MHz. ~205 instr/µs measured at 360 MHz. | fm1-base API (`-mcpu=r3 -mfprev1`) | 360 MHz is above the datasheet's 320 MHz. An inlined 32×32→64 multiply next to 16-bit loads flips bits at 360 MHz (erratum). | DS p.1; SEL row AC7911B8; `R/felucca-bsp/chip/fm1_clock.h:3-17`; `R/felucca-bsp/docs/CPU.md:1-33`; `R/ChoralRootFM1/docs/FX.md:316-320`; `ASM/hwi.h:133-137` | V |
| **Second core (CPU1)** | **Stock V15 renders all 12 voices on CPU1** (0x1C01C22: voice stride 476 B, ×2^-24, float ping-pong). **V16 renders 16 of 20 voices** in a bare-metal CPU1 loop (entry 0x2000244 → 0x1C040B2). Melodee, X0X and ChoralRoot use it with check words. **CYMA-1: unused.** | fm1-base API: `cpu2 = true`, `cpu2_fallback = true`, `plat_cpu2_*` | Some units misread RAM on CPU1 (bit 21; core 0 bit 24); this depends on the VDC14 rail. The 8 ms job timeout is longer than a 2.9 ms block. Flash writes must pause the other core. | `[fw]/dis/v163.objdump.txt` 0x2000244, 0x205897A, 0x1C040B2-0x1C04398; `[fw-v]` CPU1 verdict (`v153.objdump.txt:198299-198950`); `R/felucca-bsp/chip/fm1_cpu1.h:24-41`; `R/ChoralRootFM1/docs/DUALCORE.md:445-500` | V |
| **Float multiply-accumulate** `rd += rs*rt (f)` / `rd -= …` (fprev1) | Stock uses it: V16 122 MAC / 26 MSC; V15 17/18. **Community: unused, because JieLi clang 4.0.1 never emits it**, even with `-ffp-contract=fast` or `-ffast-math`. | Register code: inline asm `"%0 += %1 * %2 (f)"` assembles to `3f e5 17 02` | Fused (single rounding) or not is unknown. If fused, device renders differ bit-wise from host goldens (CYMA-1 §3.12 determinism). | `[fw]/dis/v163.objdump.txt` 0x2019018; `[fw-v]/fmac.c`, `fmac_asm.c` → `fa.o`; `R/mvave-fm1-open-firmware/notes/2026-10-02-jieli-compile-check.md:54-55,203-208` | V ↓ |
| **Other FPU / ALU forms:** fmin/fmax, ftoi (trunc/floor/round-to-even), float divide, smin/smax, sat8/sat16 | Used by stock V16 (Q23 via ftoi-even, phase wrap via floor, fmax peak tracking) and emitted by the compiler | Compiler / inline asm | Low | `[fw]/dis/v163.objdump.txt` 0x1C0ECDE, 0x1C0978A, 0x1C037DA, 0x1C0A73E | V |
| **CORDIC instruction** `rD_rC = copex(rA_rB) (sel)`: sin/cos, atan2, abs, exp, ln, sqrt, sinh/cosh, atanh, float mul/div/add | **Never executed** by stock, SDK demos or community. Its only SDK caller is marked "暂时不用该函数" (not currently used). Two conflicting selector maps exist in the SDK (flat op/op+16 vs `(op<<2)\|3`). | Register code: inline asm (assembles at `-mcpu=r3` without `+cordic`) | May trap as illegal. Latency and precision are unknown. Probe both encodings. | `[soc]/sdklib/ll/cpu/math_fast_funtion.c.ll`, `math.c.ll:18-221`; `JLM/nn_function_vec.c:530-557`; https://doc.zh-jieli.com/AC79/zh-cn/master/getting_started/overview/index.html ("数学运算加速引擎") | S (exists), P (behaviour) |
| **Packed 16-bit SIMD** (`*\|*`, `+\|+`, ssat/savg) | SDK source uses it (`jl_math.c`). **Stock: no evidence** (every hit is in data regions) ↓. Optimist's SIMD build crackled on hardware and was withdrawn. | Register code: inline asm | Semantics undocumented | `JLM/jl_math.c:734-839`; `R/optimist/firmware/hal/fm1_simd.h:3-27`; `[soc-v]` density analysis (52/56 V15 hits in 0x2044000-0x2057FFF data) | S (exists), P (semantics) |
| **JL_FFT, FFT mode.** 0x1EE0400 (CON/CADR/CONST/TEST1), IRQ 57. 4-word descriptors {cfg, in, out, 0}, chainable via CON[15:8]. int32 interleaved. Complex N = 10…2048 incl. radix 3/5. Real N = 20…2048 (not 10/15/32). **Measured on an FM-1** (UPEMU): forward unscaled; inverse ÷2^floor(log2 N); outputs from a 29-bit datapath that wraps into ±2^28; a DC/Nyquist packing quirk in the real inverse. **R512 = 398 TIMER4 ticks = 16.6 µs, R1024 = 40.1 µs, C1024 = 64.2 µs** (job alone). Buffers in internal RAM. | **Stock V16:** dormant RAM code (CPU1 mailbox 0x1C11400 with no poster; config tables at 0x20452FC = SDK `REAL_FFTn_CONFIG`). V13-V15: none. **No shipping firmware uses it.** fm1-dock probes measured it. | Register code: bounded poll of CON bit 7 (re-express from `JLM/jl_fft.c:296-337,674-699`); IRQ 57 needs an fm1-base IRQ attach | It writes RAM wherever the descriptor points: a bad pointer crashes, but cannot reach flash. One shared block, so cores need a lock. The SDK's software fallback is compiled only under `KISS_FFT`. Wrap, not saturation: scale input to ~2^15 as JieLi's float wrapper does. | `ASM/csfr.h:83-91`; `ASM/hwi.h:102`; `JLM/fftvec_math_drv.h:11-87`; `JLM/flfft_core_pi32v2.c:27,130-146`; `[fw]/dis/v163.objdump.txt:192304-192533`; `[exp]/host/fft_golden_check.py` (rms 0.36-0.55 LSB vs exact DFT); UPEMU `hwdsp.rs`, `tests/hwdsp/fm1.txt` | V |
| **JL_FFT, vector mode** (CON bit 3, 8-word descriptor): real/complex add, sub, mul, conj-mul, square, MAC32/MSC32, dot products, const-mul, min/max vs CONST, ReLU; strided `nloop × nlen` = matrix × vector | **Stock V16:** dormant module 0x1C07C76-0x1C09548 (complex multiply 0xA82 and complex MAC32 0x8A82: frequency-domain convolution). No community use. **Measured** (UPEMU): `>>q` rounds half away from zero; add/sub wrap at 32 bits; products saturate; dot products accumulate 64-bit wrapping. ~0.18 µs fixed cost, ~11 ns per real element, ~23 ns per complex multiply. | Register code | About CPU speed, so its value is concurrency, not speed. Integer Q-format management. Shares the lock with the FFT. | `JLM/fftvec_math_drv.h:93-204`; `JLM/jl_fft.c:819-978`; `JLM/nn_function_vec.c:9-67`; `[fw]/dis/v163.objdump.txt:197223-197264`; UPEMU | V |
| **JL_EQ: hardware biquad engine.** Registers at 0x1E000, coefficient/state RAM at 0x1E800 (208 words), IRQ 60. **Measured on an FM-1** (UPEMU, re-fetched):<br>- **Section:** transposed direct form II, H = g(1 + c4 z⁻¹ + c2 z⁻²)/(1 − c3 z⁻¹ − c1 z⁻²), computed wider than f32 and rounded once.<br>- **Capacity:** EQ RAM holds 20 (channel, section) slots. Further slots spill to `FLT_ADR` in system RAM at the same speed. Up to 64 sections and 15 output channels per run.<br>- **Run modes:** 1 (1→N) and 2 (2→4) both work.<br>- **Formats:** short/int/float I/O.<br>- **Cost:** ≈ 100 + frames × channels × (8 + 26 × sections) cycles (240 MHz-equivalent), off the CPU. | **Stock: unused** (V15 ships `eq_cfg_hw.bin` but never touches the block). **Melodee:** opt-in `MELODEE_HW_EQ=1`, off by default, unvalidated (4-biquad float cascade with a CPU fallback). fm1-dock probes measured it. | Register code: re-express from `ASM/eq.h` and the measurements. Do not copy Melodee (GPL-3.0). | Glitches on coefficient update and precision at Q > 1000 are untested. The CPU must write coefficients. A modal bank needs run mode 1 with one section per output channel. | `ASM/WL82.h:1090-1110`; `ASM/eq.h:8-174`; `ASM/hwi.h:105`; `R/melodee/firmware/hal/fm1_eq.h:18-46`, `R/melodee/BUILDING.md:718-755`; UPEMU `hweq.rs`, `tests/hwdsp/fm1.txt` (eq/f32/20sec = 13,376 ticks for 256 frames) | V ↓ |
| **JL_SRC: hardware resampler.** 0x14300, IRQ 58. s16 only, 1-8 channels. **Measured** (UPEMU): CON0[2:0] = ch−1, phase in [24:8], history offset [30:26]; bit 3 start, 7 done, 6 clear; CON1 = out_rate<<16 \| in_rate; CON2/CON3 = strides. 24-tap low-pass, DC gain 0.999. ≈ 240 cycles per output + 440 per output channel + 40 per input, so ~3 µs per mono output (~7× real time). | **Stock V13-V15:** full driver plus IRQ handler (USB/BT ASRC with a ±4 drift servo); V16 dropped it. **Community: unused.** | Register code | 16-bit only. A few voices at most. QEMU fm1-emulator faults on any start; upstream fm1-emu models it. | `ASM/src.h:11-37`; `ASM/WL82.h:884-897`; `R/FM-1-RE/docs/io/03-audio-dac.md:57,392-401`; `[soc]/dis/v152.full.txt` 0x203aba8, 0x203c774; UPEMU `hwsrc.rs` | V ↓ |
| **ALNK0/1 I2S and the audio clock tree.**<br>- **Format:** 2 modules × 4 data lines; 16/24-bit; master/slave; IIS/LJ/RJ/DSP.<br>- **MCLK:** 192M/17 = 11.294 MHz, or 160M/13 = 12.308 MHz. Selected by CLK_CON2 bit 8 (ALNK1: bit 10) plus PLL_CON1 bit 25 (320M OE).<br>- **LRCK:** MCLK / {64, 128, 192, 256, 384, 512, 768}.<br>- **Rates:** 44,117.6 (current), 48,077, 64,103, 88,235, 96,154, 176,471, 192,308 Hz. | ALNK0 is used by all at 44,117.6 Hz: PC0 MCLK, PC1 BCLK, PC2 LRCK, PC6 DAT3 TX; PC3 RX is cadence only. **Stock V15 links the SDK routine that switches families** (`audio_sample_rate_set` 0x0203e03c), but `board_init` hard-codes 44118. **No firmware has run another rate.** ALNK1 has no pins. | fm1-base patch: make Fs a platform parameter | The DAC's lock at 128× (96k) and 64× (192k) is untested. Tables and fm1-base timing assume 44118. At 96 kHz: ~2,130 instr/sample/core. 48,077 Hz is +0.16% (2.8 cents) off 48k. | `SDK/include_lib/driver/cpu/wl82/clock_hw.h:377-389,444-448`; `[soc]/sdklib/ll/cpu/iis.c.ll:1065-1105`; `R/FM-1-RE/analysis/shards/shard_0203967c_0203e720.txt:6534-6720`; `R/fm1_open/app/board/audio.c:92-109`; `R/felucca-bsp/boards/fm1/fm1_audio.h:47-72` | V (current), S (other rates) |
| **Internal DAC → DACL (pin 13).** One 16-bit channel; 99 dB SNR (A); −72 dB THD+N; 0.9 Vrms; ≥ 15 mW into 32 Ω; 8-48 kHz | Unused. Pin 13's routing is unknown (the antenna wire covers it in every photo). V15 links the SDK DAC driver (boilerplate); V16 keeps 1 read. | Register code, only if pin 13 is already routed | If it feeds an amp, an untested enable could be loud: start at analog gain 0 | DS p.12 Table 2-5, p.6 pin 13; `ASM/dac.h:7-16,88-135`; `[board]/crops/soc-bottom-pins.jpg` | S (block), P (routing) |
| **Audio ADC (LADC).** 4 × 16-bit per DS (**SEL lists 2 for the B8**), 4 mic preamps (gain index 0-100), 4 AUX; 87 dB DR; 8-48 kHz | **Not connected:** every input pad has a digital job (§2.2). V15 links the SDK driver (FM-1-RE mislabels its ADC_CON at 0x12F40 as "DAC DMA ch B"). | **Mod only** (out of policy) | — | `ASM/ladc.h:24-45`; `[pins]/sdk_objs/adc.ll:10-75`; DS p.12 Table 2-6; SEL rows 7-8 | V |
| **PDM mic link** PLNK0/1 (0x12D00/0x15200, IRQ 38/39), 2 mics each | Unused. No free fixed pins; PB5 (DAT1) is unproven. | Mod only | — | `ASM/plnk.h:10-35`; `[pins]/sdk_objs/plnk.ll:22` | V |
| **S/PDIF receiver** (fixed PC7/PC8/PA7/PA8; IOMAP input modes 8-11) | Unused; fixed pads are the LCD and key rows | Mod, or the speculative USB-C dock remap (§2.3 row 6) | Clock-domain bridging needed | `ASM/spdif.h:6-58` | V (fixed), P (remap) |
| **SAR ADC, 10-bit, external channels:** ch0 PA7, ch1 PA8, ch2 PA10, ch3 PB1, ch4 PB6, ch5 PB7, ch6-9 PC0/PC1/PC9/PC10, ch12 D−, ch13 D+ | ch0/1/2: key velocity and pressure (fm1-velocity, fm1-base touch). ch3: battery. ch4: MASTER. **ch5/12/13: unused.** | fm1-base API (the converter belongs to the 10 kHz scan tick) | Lit LEDs shift readings by ~25 counts. 26-60 µs per conversion (reasoned bound, not measured). | `ASM/adc_api.h:8-27`; `R/felucca-bsp/boards/fm1/fm1_adc.h:1-30`; `R/fm1-velocity/README.md:1-10` | V |
| **SAR ADC internal channels** via ch15: VBG, VDC14, SYSVDD, **VTEMP** (die temperature), PROGF, VBAT/4, **LDO5V/4** (USB 5 V), WVDD; audio VCM/VOUTL/VOUTR/DACVDD (internal-codec nodes) | Unused by the community | Register code, **plus** the PMU/analog sub-mux write (`adc_pmu_detect_en`, only in closed `cpu.a`; IR is available). Not in fm1-base. | VTEMP is uncalibrated: relative readings only. P33 writes need care. | `ASM/adc_api.h:22-60`; `SDK/cpu/wl82/key/adc_api.c:93-118,253-262`; `ASM/p33.h:445-456` | S ↓ |
| **FUSB 1.1 OTG** (USB0, pins 47/48): 5 hardware endpoints, isochronous ≤ 1023 B, IRQ 8/9 | **Device:** used by all. **Host:** Melodee (input reported working on hardware); felucca-bsp has an **MPL-2.0 opt-in port** (`FELUCCA_USB_HOST`). Not in fm1-base yet. The PHY sources no VBUS. | fm1-base API (device); sync felucca-bsp for host | Host needs a powered hub. Enumeration can block for seconds. Never boot as host. | `ASM/usb.h:19-45`; `R/felucca-bsp/chip/usb_host.c:1-12`, `R/felucca-bsp/NOTICE`, `R/felucca-bsp/docs/USB_HOST.md`; `R/melodee/BUILDING.md:199-203` | V |
| **HUSB 2.0 OTG** (USB1, pins 40/41): SIE 0x16000, PHY 0x16A00, IRQ 53/54 | FM-1 routing unknown. Stock's 0x16A0C writes are the generic SDK GPIO driver (pins 150/151), not proof of use. **The sibling SMK-37 drives these pads as GPIO LED commons.** | If routed: pull-ups and readback (0x16A0C/0x16A10), giving 2 slow GPIOs. A second USB port is a mod. | Unknown routing | `[pins]/sch_b8_note.png`; SEL (HSUSB = 1); `ASM/usb.h:64-117`; `R/jvitkauskas_m-bootloader/docs/SPEC.md:676-680`; `R/obervave/firmware/hal/smk_input.h:16-17,45` | S |
| **USB-C data pins as an accessory port:**<br>- FUSBDP: UART1_TXD, SPI2_CLKB, IIC_SCL_A, ADC13, SDTAP_CLKB.<br>- FUSBDM: UART1_RXD, SPI2_DOB, IIC_SDA_A, ADC12, SDTAP_DATB.<br>- Pulls: 1.5 kΩ up / 15 kΩ down on both. | Peripheral alternates unused. **Stock samples D− as GPIO 149** about every 512 ms as a charge/USB flag. | Register code plus a custom adapter (a policy question) | Never with a host or charger attached (a BC1.2 charger shorts D+ to D−). UART1 RX is already MIDI IN (PH8 via input channel 1), so a second RX needs UART0/2 via remap 61. Write a mode handshake. | DS p.9, p.11 Table 2-4; `ASM/usb.h:46-61`; `R/FM-1-RE/docs/io/07-input.md:175-178`; `R/FM-1-RE/firmware-images/v14/decomp/app_pi32v2_objdump.txt` @0x24A6A-0x24ACC | S ↓ |
| **Output-channel crossbar:** 4 channels to any PA-PH pin (UART TX, Timer0-5 PWM, MCPWM H/L, PLNK SCLK, 12/24 MHz). **Input remap:** a 64-entry index incl. USB D± (60-63); 4 input channels shared by UART RX, PLNK and wake events. | Output channels: Melodee uses all 4 for LED PWM. **SMK-37 stock sends UART1 TX through CH1 on PC7 for TRS MIDI out** (hardware proof on this chip family). Input ch1 = MIDI IN everywhere. | Register code (`[pins]/sdk_objs/gpio.ll:2038-2113`) | A pin in channel mode cannot be read back. Never route onto PA2. Only one wake/edge input is free while MIDI IN holds ch1. | `ASM/gpio.h:219-287`; `ASM/WL82.h:306-320`; `R/melodee/firmware/hal/fm1_led_pwm.h:35-107`; `R/Vector37/firmware/hal/smk_uart_tx.h:3-40`; `SDK/cpu/wl82/port_waked_up.c:30-39` | V ↓ |
| **Timers:** TIMER0-5, per-core TTMR, LRCT, watchdog | TIMER4 = 24 MHz free-running base. TIMER5 = 10 kHz scan tick (fm1-base). TIMER1 = stock `usr_timer`. **TIMER0-3: free.** Timer capture is not shown in the SDK headers. | fm1-base API (TIMER4) / register code | Low | `ASM/WL82.h:385-409,762-765`; `R/fm1-base/bsp/chip/fm1_timer.h:3-6`; `R/fm1-emulator/src/hw/pi32v2/fm1-lrct.c:1-8` | V (capture P) |
| **MCPWM** (8 timers, 8 complementary channels) | Melodee uses it for 64-step LED dimming via the output channels. Fixed pads are all occupied. | fm1-base patch | Enabling MCPWM fixed pads on PA7/PA8 makes phantom key presses | `R/melodee/firmware/hal/fm1_led_pwm.h:1-45`; JieLi PWM doc https://doc.zh-jieli.com/AC79/zh-cn/release_v1.0.3/module_example/peripherals/pwm.html | V |
| **GPCNT** clock/pulse counter (0x11100, IRQ 26) | Unused | Register code | Low value: Fs is already an exact integer relation (24 MHz / 544 = 44,117.647 Hz) | `SDK/include_lib/driver/cpu/wl82/clock_hw.h:298-309`; `R/Felucca/firmware/src/audio.c:9` | S |
| **DCP** memory-to-memory DMA (0x41300, IRQ 48) | Unused | Register code | Unknown whether it can read XIP flash | `ASM/dma.h`; `[soc]/sdklib/ll/cpu/dma.c.ll` | S |
| **JL_RAND** 64-bit (0x13B00) | Stock reads it (19-20 sites: OS, BT, app). Optimist's BLE stack uses it. **Audio: nobody.** | Register code | Quality and refresh rate unknown (TRNG or LFSR?) | `ASM/WL82.h:777-785`; `R/optimist/firmware/hal/fm1_ble.h:68-69`; `R/fm1-emu/crates/fm1-soc/src/lib.rs:866-877` | V ↓ |
| **CRC16/32, AES-128/256, SHA-1/256, SFCENC, eFuse** | Stock uses CRC, AES and SFCENC (chip key 0x980F); SHA is unused. The community computes CRC in software. | Register code | Never write eFuse | `ASM/WL82.h:46-60,123,459-462,754-757`; `R/FM-1-RE/docs/architecture.md:79-88` | V |
| **Hardware SBC codec** (0x13D00, IRQ 46) | Stock: A2DP decode, IRQ on CPU1; the encoder is programmed too. Community: none (Optimist uses software SBC). | Register code (`cpu.a` `hw_sbc_init` IR) | Register semantics are only names. Lossy. | `ASM/WL82.h:796-810`; `ASM/sbc.h:17-30`; `[proto]/nm_cpu.a.txt`; `[proto]/dis/app_V15.dis` 0x3e250-0x3e5e4 | S |
| **Clock generation:** 24 MHz crystal (JieLi requires ±10 ppm); PLL taps 480/320/192/137/107 MHz; integer-N in 2 MHz steps; CLKOUT on PC0/PA4 (both used pins) | Stock V15 SPL: PLL 540 → CPU 360 / HSB 180 / LSB 60. V16 bootloader: 320 MHz. fm1-base forces 360. Fractional-N and DSM appear only in inactive debug macros. | fm1-base patch | Everything derived moves with the clock (SPI, UART, SFC, USB, audio) | `SDK/include_lib/driver/cpu/wl82/clock_hw.h:101-342`; `[fw]` isd_config decode; `[board]/hwguide.txt:398-416,1091-1096` | S |
| **PMU and rails:** VBAT 2.2-5.5 V; DCDC DCVDD14 max 1.55 V; DVDD max 1.32 V. The internal charger is not bonded on this package (the board uses U12). | Stock apps raise **VDC14 = 7 (1.60 V)** and **SYSVDD = 15 (1.38 V)**, both above the datasheet maxima if the community's step formulas are right. fm1-base copies these. | fm1-base patch | Never go above V15's levels. Watchdog, LP timer and soft power-off are available. | DS p.10 Tables 2-1/2-2; `[fw]/dis/v163.objdump.txt:40686-40735`; `R/melodee/firmware/hal/fm1_power.h:13-15` | V (values), S (out-of-spec) |
| **Debug and protection:** write-limit windows, PC limits, EMU exceptions (incl. the FPU "tiny" flag), ETM last 4 PCs, watchpoint WP0, breakpoints BP0-3, SDTAP (on the USB pins) | **Used:** fm1-base and felucca use write limits, PC limits, the EMU stack guard and ETM in the crash record (so do ~20 community HALs). **Unused:** WP0, BP0-3 and the FPU tiny flag. | fm1-base patch / register code | A wrong window faults (the boot guard handles it) | `ASM/csfr.h:140-337`; `R/fm1-base/bsp/chip/fm1_irq.h:29-32,90,141`; `R/fm1-base/bsp/chip/fm1_guard.h:36-110` | V ↓ |
| **corex2 performance counters:** TL_CKCNT (64-bit cycles), IF/RD/WR counts | **TL_CKCNT does not count while DBG_CON = 0** (X0X, measured on a unit). fm1-x0x and fm1-velocity use `fm1_perf.h`. | fm1-base API (`fm1_perf_*`), opt-in DBG_CON | DBG_CON may arm fault exceptions | `ASM/csfr.h:105-142`; `R/fm1-x0x/docs/plans/2026-10-04-dual-core-and-clock.md:101-108`; `R/felucca-bsp/chip/fm1_perf.h:1-49` | S ↓ |
| **Cross-core signalling:** IRQ_SOFT0-7, ILAT_SET, other-core halt (C0/C1_CON bit 2, acknowledge bit 4), `sevg`/`wfe` | Stock: remote-call mailbox 0x1C11640 and halt around flash work. V16 has exactly one genuine `sevg` and **no genuine `wfe` anywhere**. | Register code | Event-wait semantics unproven; polling is the proven path | `[fw]/dis/v163.objdump.txt:189190,189227,192498`; `ASM/csfr.h:44-51,314-324` | S (halt, IPI), P (event wait) |
| **Cache-as-RAM:** free cache ways at 0x1F28000 (28 KB) | Melodee uses it (`MELODEE_CACHE_RAM`). Stock V16 keeps a 6,144-sample int16 USB/BT FIFO at 0x1F28020. | Linker / fm1-base | It shrinks the D-cache, so flash data reads get slower. Whether accelerator DMA can reach it is unknown. | `SDK/cpu/wl82/sdk_ld_sfc.c:24-40`; `R/mvave-fm1-open-firmware/notes/2026-10-23-devkit-audio-performance.md:1672`; `[fw]/dis/v163.objdump.txt:56893` | V |
| **2.4 GHz radio:** Wi-Fi b/g/n + BT (DS: BLE 5.3; SDK README: 5.0). 34 mm wire antenna: 3.04 dBi at 2400 MHz falling to −0.17 dBi at 2483.5 MHz. | BLE-MIDI: stock, fm1-base, Optimist. Wi-Fi: fm1-radio, COSMOS. Certified power 1.44 mW (BR/EDR). **The FCC filing has no Wi-Fi line.** | fm1-base API | BLE start-up RF trim draws high current and can corrupt flash reads. Upper channels are ~3 dB weaker. | `[board]/fcc/` antenna and BT reports; https://fccid.co/fcc/2ARCP-FM1; DS pp.1-2 | V |
| **Video/display blocks** (ISC/ISP, JPEG, IMD/OSD, EMI) | Unused. Camera and parallel pins are the matrix/595/LEDs; the LCD is serial. | Impossible, except perhaps the JPEG decoder from RAM | Large drivers | `ASM/WL82.h:148-242`; `[soc]/sdklib/ll/cpu/imd.c.ll:1903-1960` | S (JPEG P) |
| **CTMU touch, RDEC, IR filter, UART0/2, SPI3, IIC, SD hosts** | Unused: pins taken. SD0 group A = PB7/PB6/PB5 (E6 row, MASTER, unknown PB5). RDEC and S/PDIF may take remapped inputs (header-level only). | Mod (or speculative remap) | — | `ASM/rdec.h`; `[pins]/fm1_ac7911b8_pins.csv`; SEL (B8 SD = 1) | V (P for remap) |
| **JL_ANC** (0x1F000, 2 KB RAM; declared in the shared WL82 header) | Unknown whether present on the AC7911B | A read-only presence probe behind a breadcrumb | May bus-fault | `ASM/WL82.h:1112-1140` | P |
| **MMU/TLB** (for the SDRAM window) | The B8 has no SDRAM. Double-mapping RAM pages for wrap-free delay lines is untested. | Register code | Unknown | `SDK/cpu/wl82/sdk_ld_sfc.c:16-19` | P |

### 3.2 Pins and alternate functions (AC7911B8 as used on "DX7 MB V07")

**Package.** The chip is marked `C188612-11B8` / `C156211-11B8`. JieLi documents the 11B8 only as **QFN48 6×6 mm, 0.4 mm pitch**.
- The board track measured a ~6 mm body and 0.40 mm pitch against the 8.94 mm USB-C shell.
- My own rough re-measure of `[board]/crops/usb-soc-scale.jpg` gives the same: shell ≈ 410 px, body ≈ 285 px ≈ 6.2 mm, pitch ≈ 0.43 mm.
- The pins verifier read the visible terminals as LQFP gull-wings. They protrude only ~0.3-0.5 mm (`[pins-v]/soc_crop.jpg`), which fits QFN side terminals with solder fillets better than ~1 mm LQFP feet.
- The question stays open; a low-angle photo settles it (§5.4).
- The pin numbering is the same either way and agrees with every landmark: crystal at pins 26/27, antenna at 24, DC-DC inductor L2 at 45, I2S lines toward U9. (S)

**Re-bonding on the B8.** Pins 37-41 carry PB7, PB6, PB5, HUSBDM and HUSBDP instead of the B0's flash pins.
- JieLi's B0/B8 reference-schematic note and the AC7913A6 datasheet both say so.
- PB6/PB7 are **V**: every firmware uses them, and they do not exist on the B0 bond.
- PB5 and the HUSB pads are **S**.

| Pin | Pad(s) | FM-1 use | Alternates that matter | Software-only verdict for CYMA-1 | Conf. |
|---|---|---|---|---|---|
| 1, 28 | IOVDD | Power | — | — | V |
| 2 | PA10 | LED line (LEDs of row PA6) + top-7-key pressure probe (ADC2) | ADC2, SD0_D1B, ALNK DAT3B1, TMR7CK | Keep (already double duty) | V |
| 3 | PA9 | LED line (row PA5) | SD0_D0B, ALNK DAT2B1, TMR6CK | Keep. MCPWM dimming via output channels is possible (Melodee). | V |
| 4 | PA8 | Key row 4 + ADC1 (velocity/pressure) | ADC1, IIC_SDA_B, SPDIF_D, PWMCH1L, Wakeup4, SDTAP_D | **Never repurpose** | V |
| 5 | PA7 | Key row 3 + ADC0 | ADC0, IIC_SCL_B, SPDIF_C, PWMCH1H, TMR0 clock-in, Wakeup3 | **Never** | V |
| 6 | PA6 | Key row 2 (top 7 keys) | UART0_RXA, ALNK LRCKB1, FPIN0. **No analog function.** | **Never** | V |
| 7 | PA5 | Button/encoder row 1 | AUX2, UART0_TXA, CAP3 | **Never** | V |
| 8 | PA4 | 74HC595 SER (data) | MIC2N, UART0_RXC, CLKOUT1, PWMCH0L | **Never** | V |
| 9 | PA3 | 74HC595 SRCLK | MIC2P, UART0_TXC, PWMCH0H | **Never** | V |
| 10 | PA2 | Backlight and the switched 595/LED rail (Q2 AO3401A high-side switch, or 595 OE#) | MIC0P, SPI2_DIC | **Never toggle or PWM** (it kills the key scan) | V |
| 11 | PA1 | 74HC595 RCLK (latch) | MIC0N, PWM0 | **Never** | V |
| 12 | PA0 | Encoder row 0 | AUX0, TMR1CK, Wakeup2 | **Never** | V |
| 13 | DACL | Internal-DAC headphone output; **routing unknown** | Analog only (no GPIO, no UART) | Read-only/low-gain probe (§5) | P |
| 14-17 | VCM, HPVSS, DACVDD, AHVDD | Internal-codec reference and supplies (decoupling fitted at the corner) | DACVDD can bias a mic (~3 V) | — | S |
| 18 | PH9 | LED line (row PA8) | MIC1P | Keep (audio in is mod-only) | V |
| 19 | **PH8 + PH7** (double-bonded) | TRS MIDI IN: opto → PH8 → input channel 1 (remap 49) → UART1 RX | PH8: MIC1N. PH7: AUX1, UART1_RXA, PWMCH5L, Wakeup13. | **Never make either an output.** Never select UART1 fixed group A. | V |
| 20 | **PH6 + PH4** (double-bonded) | LED line (row PA7) | PH6: MIC3P, UART1_TXA, PWM2. PH4: AUX3. | Keep. Twin pad high-impedance. | V |
| 21-23, 25 | DVDD and RF supplies | — | — | — | V |
| 24 | WFRF | Antenna pad P1 (34 mm wire) | — | — | V |
| 26-27 | XOSCI/XOSCO | 24.000 MHz crystal | — | — | V |
| 29 | PC10 | LCD MOSI (SPI1-B DO) | ADC9, UART2_RXB, PWM3, TOUCH10, Q-decoder1 | Keep. A write-only expander could share it, given a CS source (mod). | V |
| 30 | PC9 | LCD SCK | ADC8, UART2_TXB, TOUCH9, Q-decoder0 | Keep | V |
| 31 | PC8 | LCD D/C (also SPI1-B MISO, so full-duplex SPI1 reads are impossible) | SPI1_DIB, SPDIF_B, CAP5, TOUCH8 | **Never** (D/C is mandatory) | V |
| 32 | PC7 | LCD CS | SPDIF_A, CAP4, PWMCH2H, TOUCH7, SD0_DAT1D | Keep. Freeing it needs the CS-tie mod (out of policy). | V |
| 33 | PC6 | I2S DAT3 → U9 | ALNK0 DAT3A | **Never** | V |
| 34 | PC5 + PC2 | I2S LRCK (PC2); PC5 = DAT2A twin | — | **Never** | V |
| 35 | PC1 | I2S BCLK | PWM1 | **Never** | V |
| 36 | PB8 + PC0 | I2S MCLK (PC0) | CLKOUT0, ADC6 | **Never** | V |
| 37 | PB7 | Matrix row 5: only encoder E6 (columns 0/1) | ADC5, UART2_RXC, SPI1_DOA, SD0_CLK, PWMCH7L, UART1_RTS, SDTAP_C | Keep. Its 9 empty intersections need a mod. ch5 analog is readable. | V |
| 38 | PB6 | MASTER pot (ADC4; volume applied digitally) | UART2_TXC, SPI1_CLKA, SD0_CMD, PWMCH7H, UART1_CTS, Wakeup9 | **Remap MASTER as a macro knob in firmware** (no mod needed) | V |
| 39 | PB5 | **No known use; routing unknown** | PLNK0/1_DAT1, SPI1_DIA, SD0_DAT0A, FPIN2, CAP0; 8 mA; allowed as the ROM's UART-update input (stock config has none) | Read-only float probe only | P |
| 40, 41 | HUSBDM, HUSBDP | **No known FM-1 use; routing unknown** | HS-USB PHY; GPIO through PHY pulls (the SMK-37 does this); remap inputs 62/63 | Read-only line-state probe only | S |
| 42 | PB1 | Battery divider (ADC3); **8 s low = chip reset** (`isd_config RESET=PB01_08_0`) | — | **Never drive** | V |
| 43-46 | DCVDD14, VBAT, SW, PGND | PMU; SW → L2 10 µH | — | — | V |
| 47 | FUSBDM | USB-C D− (stock also samples it as GPIO 149) | UART1_RXD(D), SPI2_DOB, IIC_SDA_A, ADC12, SDTAP_DATB; remap 61 | USB. Accessory mode is the adapter policy question. | V |
| 48 | FUSBDP | USB-C D+ | UART1_TXD(D), SPI2_CLKB, IIC_SCL_A, ADC13, SDTAP_CLKB; remap 60 | As above | V |
| PAD | VSS | Ground | — | — | V |

**The crossbar in one paragraph:**
- **Output channels.** Any regular PA-PH pin can carry one of 4 output channels: UART TX, timer PWM, MCPWM, PLNK clock or 12/24 MHz. Attach a pin by setting PU = PD = 1 and DIR = out, with (OUT, DIE) choosing the channel. The source goes in IOMAP CON1[11:8] for CH0 and CON3[31:20] for CH1-3.
- **Input remap.** Any pin, including the four USB pads (indices 60-63), can feed the 4 input channels, which serve UART RX, PDM data, the quadrature decoder, CTS and wake events. MIDI IN holds channel 1.
- **What this means on the FM-1.** Every regular pin already has a job, so on an unmodified FM-1 the crossbar is useful only for internal tricks, such as MCPWM LED dimming on the LED lines (Melodee).
- Evidence: `[pins]/sdk_objs/gpio.ll:447-466,2038-2113,2153-2462,2624-2700`; `ASM/WL82.h:306-320`.

**74HC595 chain.**
- **Structure.** 16 bits, shifted MSB first and latched by PA1 every 100 µs. Bits 0-10 are the 11 columns.
- **Bits 11-12** are pulled low together with columns 0/1, probably E6's select lines.
- **Bits 13-15** (QF/QG/QH of the downstream 595, SOIC pins 5-7) are held high by every firmware. Whether they connect to anything is unknown.
- **Software-only value:** none. Using them as gates would need wires.
- Evidence: `R/fm1_open/app/board/fm1_input.h:47-56`; `R/felucca-bsp/boards/fm1/fm1_input.h:252-277,372`. (V firmware side, P board side)

**Empty matrix intersections.** Row PB7 columns 2-10, plus column 10 of rows PA0 and PA5: 11 intersections that every firmware already scans. They would only matter with added switches, which is a mod. (V)

### 3.3 Board components and the audio chain

| Designator | Package / marking | Identity | Role | Conf. | Evidence |
|---|---|---|---|---|---|
| SoC | QFN48 6×6 (see §3.2), `JL C188612-11B8` | JieLi AC7911B8: 2 cores, 1 MiB in-package flash (JEDEC 0x856014), FS-USB on 47/48, HS-USB on 40/41 | Everything | V (marking), S (bond details) | `[board]/crops/soc-full.jpg`; SEL; `[pins]/sch_b8_note.png` |
| Y | 3225, "24.000 MHz" | System and RF crystal (JieLi requires ±10 ppm; on-die load trim via `wfrf_set_xosc`) | Fs = 24 MHz / 544 = 44,117.647 Hz exactly | V | `[board-v]/soc_closeup_small.jpg`; `[board]/hwguide.txt:398-416` |
| L2 | ~4×4 mm "100" (10 µH) | Inductor for the SoC's internal DC-DC (DCVDD14) | Core rail | V | DS pins 43-46 |
| **U9** | 10-lead MSOP/TSSOP, **marking unread** | Stereo 24-bit I2S DAC with no control port and no ADC: CS4344 / CJC4344 / ES7144LV class | The only path to analog audio | S | `[board]/crops/top-audio.jpg`, `a1-u9-u5.jpg`; `[board]/ds/cs4344.txt` |
| **U5** | QFN-16 3×3, **marking unread** | Headphone/line driver. A ground-referenced charge-pump type (TPA6132A2-class) is a guess. | U9 → U5 → FB4/FB5 → J7 | S (role), P (part) ↓ | `[board]/crops/j7-u5.jpg` |
| U6 | Small regulator (read as SOT-23-5 by one track, SOT-89 by another), unread | Likely an LDO for the analog audio rail (sets the DAC's VA, hence full scale) | Audio supply | P | `[board]/crops/top-audio.jpg` |
| **Speaker amp** | ESOP-10, marking **SL5316D / 6253KD** (also read as "SLS316D") | Mono class-D/AB with inductorless 6.5 V charge-pump boost; ~4.2 W (1%) into 4 Ω; 4 AGC modes; AB/D select and shutdown via one-wire CTRL | Drives J13 (speaker). PVDD/VBAT reservoirs C112/C113 (16 V 100 µF on the "J14" holes); EMI filter FB1/FB2 + C108/C109; input R93/R94 + C91/C92. | S (V marking) | `[board]/crops/top-sls316d-region.jpg`, `bot-j13.jpg`; `[board]/ds/SL5316.pdf` pp.2-3 |
| J7 | 3.5 mm TRS, 6 terminals incl. a normally-closed leaf | Audio out (headphone/line). **Plugging in mutes the speaker in hardware;** firmware cannot switch it. TRRS plugs may fail to mute. | Output only | V (behaviour), S (mechanism) | manual p.11; `R/Felucca/firmware/src/menu_items.c:127-130` |
| J3 + PC1 | TRS jack + SO6 "OCIC P2362 2623" | TRS MIDI IN, Type A (reported). TLP2362-class 10 MBd open-collector opto → PH8 (internal 10 kΩ pull-up). | MIDI in only | S | `[board]/crops/j3-opto.jpg` |
| J13 + speaker | 2-pin 1.25 mm on the bottom (+S/−S) | ~25-28 mm speaker (photo estimate), impedance unread | Mono; L+R sum assumed | V / P | `[board]/crops/bot-j13.jpg` |
| J6 | USB-C 16-pin | Wired to FUSB (pins 47/48), full speed. Two small resistors (CC pull-downs?) beside it. | USB-MIDI, UAC, power | V (P CC values) | `[board]/crops/usb-soc-scale.jpg` |
| SOT-23 + R21 | Between J6 and the SoC, unread | ESD array or power-path part | USB protection? | P | `[board]/crops/a1-usb-sot23.jpg` |
| U2, U3 | SOIC-16 74HC595D | Column drivers (16-bit chain) | Keys/encoders/buttons | V | `[board]/crops/a1-u2u3.jpg` |
| Q2 + R73 | SOT-23 "A19T" (AO3401A-class P-MOSFET), 68 Ω | PA2-gated, active-low high-side switch: backlight and LED/595 rail | Why PA2 must never toggle | S | `[board]/crops/a1-q2-u11.jpg` |
| Q1, Q3 | SOT-23 (Q1 "A19T") | Unknown load switches | — | P | `[board]/crops/top-q1-q3.jpg` |
| U11 + L1 | SOT-23-6 + 4.7 µH "4R7"; feedback R83/R84; R78 = 1.00 kΩ | External DC-DC, most likely the 3.3 V system buck | System rail | S | `[board]/crops/top-l1-u11-q2.jpg` |
| **U12** | Bottom SOP-8, unread, with R32 = 0.1 Ω (2512) | **The charger (not flash).** TP4056-class is doubtful (TP4056 sets current with RPROG, not a 0.1 Ω shunt). | Battery charging; no status line to the SoC | S (role), P (part) | `[board]/crops/bot-u12.jpg`; `R/fm1-base/device/plat_fm1.c:45-50` |
| LED2 | Bicolour, R22/R77 | Charge indicator | — | S | `[board]/crops/top-usb-switch.jpg` |
| J5 | Slide switch | Main power (unit invisible on USB when off) | — | V | `R/mvave-fm1-open-firmware/notes/2026-09-06-bench.md` |
| Q4, D54, R37, R88, R89 | Labels near J5/J6 | Probably the USB/battery power path (PMOS + Schottky, as in JieLi's reference) | — | P | `[board]/crops/top-usb-switch.jpg`, `b8-power-*.png` |
| Battery | DTP704060, 3.7 V 2000 mAh 7.4 Wh, 3-wire (+, −, NTC) | Li-Po | — | V | `[board-v]/bottom_small.jpg` |
| J12 | ~12-pin 0.5 mm FPC | ST7789V 240×240 on SPI1-B (PC7-PC10). No MCU reset; no TE line known. | Display | S | `R/mvave-fm1-open-firmware/docs/01-hardware.md:158` |
| RW1, encoders, keys | Pot; 7 detented encoders in the matrix; 27 keys on carbon-pill domes | MASTER (PB6/ADC4); keys read as resistive dividers on ADC0/1 (velocity and polyphonic pressure) | Performance controls | V | `R/fm1-velocity/README.md:1-10`; `R/fm1-base/lib/sense.c:57,152` |
| P1 + wire | 34 mm × 2.5 mm wire antenna | 3.04 / 1.75 / −0.06 / −0.17 dBi at 2400 / 2440 / 2480 / 2483.5 MHz | Radio | V | `[board]/fcc/img/antH-4.png`, `antH-5.png` |
| Debug | None. The 3-hole "header" is the battery connector's leads; J14 is electrolytic leads; round pads are fiducials. | — | No JTAG/SWD; SDTAP only via the USB pins | V | `[board]` overlay check |

**Same PCB, other brands** (FCC exhibits list the trade names): DONNER, GEAR4MUSIC, GOKKO and CASON, sold as DX-7, SK15, POCKET FM, Opix 7 and GOW-7/GOR-7/GOB-7. Teardown photos of any of these apply to the FM-1. The FCC internal photos stay confidential until about February 2027.

**The audio chain:**

```
SoC ALNK0 (master, Philips I2S, 24-bit data in 64-BCLK frames)
  PC0 MCLK 11.294 MHz (256 fs)   PC1 BCLK 2.82 MHz   PC2 LRCK 44,117.6 Hz   PC6 DATA (channel 3)
        |
        v
U9  CS4344-class DAC (no control port, no ADC; full scale ≈ 0.65·VA Vpp ≈ 0.76 Vrms if VA = 3.3 V)
        |-- L/R --> U5 QFN-16 headphone driver --> FB4/FB5 --> J7 (TRS; detect leaf mutes the speaker amp)
        '-- (L+R, assumed) --> R93/R94, C91/C92 --> SL5316D boosted class-D/AB --> FB1/FB2 --> J13 speaker

Not in the analog path:
  MASTER (RW1 → PB6/ADC4): digital gain in firmware.
  Internal DAC (DACL, pin 13): routing unknown.
  Audio ADC: no input reaches it (§2).
Audio into the FM-1 arrives only digitally: USB UAC OUT, or Bluetooth A2DP (stock).
```

Evidence: `R/fm1_open/app/board/audio.c:92-109`; `[board-v]/ac792n_audio_link.h` (ALINK_MD_IIS = 1, 64-SCLK frame); `[board]/ds/cs4344.txt:196-242,556-575`.

The I2S reading is **S**: it comes from the register semantics of the WL83/BR23 families and has not been scoped. The **CS4345** (left-justified) **is ruled out** if that reading holds.

### 3.4 Stock firmware internals (V15 = FM-1_015; V16 = FM-1_016 of 2026-10-21)

Method:
- **Unpacking:** both `.fwsc` images were unpacked with kagaimiq's jl-misctools `fwunpack_newfw.py`.
- **Disassembly:** JieLi's pinned objdump inside `fm1-toolchain:20250324.1-raqm`, with `-mcpu=r3 -mattr=+fprev1,+simd,+cordic,+instrev2`. Flash code was linked at its real XIP address and the `.data` RAM code at 0x1C00000.
- **Peripheral map:** a 968-register SFR map, generated from the SDK headers.

Outputs are in `[fw]` (local only: never commit).

| Item | V15 | V16 | What it means for CYMA-1 | Evidence | Conf. |
|---|---|---|---|---|---|
| **Partitions** | `app.bin` 581,564 B @0x120 (abs 0x4120). VM 352,256 B @0x93000/0x97000. BTIF @0xE9000/0xED000. USR 72 KB @0xEA000/0xEE000. key_mac at abs 0xFF000. | `app.bin` 597,928 B @0x140. **DX7.bin** 73,226 B (256 raw DX7 voices, then an unexplained 7,690-B block at 0x10000). VM 128 KB @0xBA000. SEQ64 72 KB @0xDA000. BTIF @0xEC000/0xF0000. EXT_RESERVED, PRCT. Chip string "AC791N", PID "AC791N_STORY". | **Never apply V15 region tools to V16.** Never write below 0x4000, BTIF or key_mac. Keep the unit on V15. | jl-misctools listing re-run by `[fw-v]`; `[fw]/fw/v16.fwsc_unpack/files/DX7.bin` | V |
| **Bootloader config** (`isd_config.ini`) | SYS_CLK 360 MHz; VLVD 4; UTBD 1,000,000; RESET PB01_08_0 | **SYS_CLK 320 MHz**; VDDIO 4; DVDD 11; DCDC14 3 | M-VAVE dropped to 320 MHz when V16 moved more work onto CPU1. A 320 MHz CYMA-1 option is stock-proven. | `[fw]/scripts/isdcfg.py` on both `top/isd_config.ini` | V |
| **App rail raise** (identical) | P33: CON1 = 0x20, CON5 = 0x40, CON6 = 12, CON7 = 0xA5, CON8 = 0x96, CON9 \|= 0x10. VDDIOM 4 (3.2 V), **VDC14 7 (1.60 V)**, VDDIOW 3, **SYSVDD 15 (1.38 V)**. | same | fm1-base already forces these levels. Do not go higher. | `[fw]/dis/v163.objdump.txt:40686-40735`; `v153.objdump.txt:6867-6869` | V ↓ |
| **CPU1 role** | **Renders the whole 12-voice engine** (state 2 → 0x1C01C22; DC blocker; float ping-pong at eng+8812). CPU0 swaps blocks. | **Bare-metal loop at 0x1C040B2** renders the 16-voice engine (pump 0x1C03BBA), serves the FFT mailbox 0x1C11400 and a 2-channel stream service. IRQs are on: TIMER4 and SBC ISRs (in flash) pre-empt it. | Dual-core is the stock norm. Copy the pattern: asymmetric split, per-job late/overrun counters, a one-block pipeline. | `[fw-v]` CPU1 verdict; `[fw]/dis/v163.objdump.txt` 0x1C040B2-0x1C04398, 0x201B5D2 | V ↓ |
| **Engine** | 12 voices × 476 B; 64-sample blocks; mono float render | 2 msfa engines: `engine_alloc(4)` (RTOS task "dx7_cpu0", prio 30) + `engine_alloc(16)` (CPU1); n × 432 + 4591 B each; Fs 44118 stored; one global float-built `freqlut` (1025); 8 parts × 348 B; 64-sample control-rate linear ramps; per-voice fmax peaks; 256-frame jobs; float two-engine sum | A blueprint: float buses, control-rate ramps, peak tracking for stealing and meters, one master quantiser | `[fw]/dis/v163.objdump.txt` 0x20206F4, 0x201FEA8, 0x1C0362C-0x1C03A94, 0x1C01960 | S (V counts) |
| **Output stage** | **Q23 (×8,388,607, trunc) + 512-segment tanh soft clip** (tables 0x1C081BC/0x1C089BC), mono → L = R | ×0.75, then Q23 with round-to-even, then the same **tanh PWL over ±16× full scale** (tables 0x1C11660/0x1C11E60, output <<2); ~16 instr/sample | Both versions send full-scale 24-bit words. A tanh table clipper is cheap (2-3 KB). It is gain-1 at zero but nonlinear everywhere. | `[fw-v]` output-stage verdict; `[fw]/dis/v163.objdump.txt:200520,206542`; `v153.objdump.txt:206413-206440` | V ↓ |
| **ALNK0 setup** | CON3 MSRC = PLL, MDIV ÷1, LRDIV 256 fs; CON0 MOE\|SOE, DSPE = 0, 64 SCLK; CON1 ch3 TX and ch0 RX, mode 1, 24-bit; LEN 128 words (64 frames); IRQ 11 prio 3, RAM ISR | Same; frames 64 or 256 (`alnk_open` 0x202124C rejects others); IRQ 11 prio 4, flash ISR | 64-frame halves for low latency, as V15 does | `[fw]/dis/v163.objdump.txt:46271-46495`; `v153` 0x20057BC-0x200587A | V (S for "mode 1 = I2S") |
| **Sample-rate switch routine** | `audio_sample_rate_set` (0x0203e03c, linked from the SDK) handles 8-192 kHz: CLK_CON2 bit 8, PLL_CON1 bit 25, LRDIV codes | (not traced) | Proof that the 48k-family clock path exists in shipped code. Runtime stays at 44118. | `R/FM-1-RE/analysis/shards/shard_0203967c_0203e720.txt:6534-6720` | V |
| **JL_FFT / vector unit** | None | RAM code present but **no static caller or poster** (FFT mailbox, frequency-domain convolution module 0x1C07C76-0x1C09548) | A working blueprint; dormant in stock | `[fw-v]` FFT and vector verdicts (call-target scan of 13,278 targets) | V |
| **Float MAC** | 17 MAC / 18 MSC | 122 / 26 | M-VAVE's compiler emits it; ours cannot (§3.1) | `[fw-v]/fmac*.c` | V |
| **Packed SIMD** | No genuine use | No genuine use | Do not cite stock as proof of SIMD semantics | `[soc-v]`, `[fw-v]` density checks | V |
| **IRQ routing** | ALNK 11 (core 0, RAM ISR); SRC 58, SS 47, PLNK 38/39, ALNK 11/12 (SDK path) on CPU1; AUDIO 13 core 0 | SYSCALL 2; EXCEPTION 1 (both cores); OSA 28; TIMER1 5; LRCT 44; PMU 49/50; USB 9; ALNK 11; SPI2 37 (key/LED shift DMA); SPI1 16 (LCD); UART1 20 (MIDI); SARADC 24; **TIMER4 62 and SBC 46 on CPU1**; SOFT2-5; BLE 45/29; BT 40/41 | A custom CPU1 loop must mask or service whatever is routed to it | `[fw]` `request_irq` enumeration; `ASM/hwi.h:38-121` | V |
| **SRAM layout** | `.data` 41,052 B; `.bss` 103,872 B | `.data` + RAM code 86,088 B; `.bss` 213,968 B (end 0x1C49418). Per-sample kernels, scheduler and CPU1 loop in RAM; ISR, FIFO mixer and control code in flash. 6,144-sample int16 USB/BT FIFO in cache-RAM at 0x1F28020. | ~86 KB of RAM code is affordable when the pool is small | `[fw]/dis/v163.objdump.txt` 0x20001BC-0x20001EC, :56893 | V ↓ |
| **Other drivers** | SRC (full + IRQ), 13 JL_AUDIO functions (internal DAC and audio ADC), PLNK, SS. **EQ: unused** (`eq_cfg_hw.bin` shipped but never loaded). | SRC, PLNK and ALNK1 dropped; 1 DAC_CON read; EQ unused | Present in silicon, unused by stock audio | `[fw]/out/v15.periph.txt:419-433,620-622`; `v16.periph.txt:361-362` | V |
| **RNG, CRC, AES, LRCT** | Used | Used (RAND in 14 functions: OS task-create, BT, app) | Hardware random exists | `[fw]/out/v16.periph.txt` | V ↓ |
| **USB identity** | Template 4C4A:4155 with **byte 11 patched to 0xC7**, giving the runtime **4C4A:C755**. bcdUSB 2.00. UAC1 2-in/2-out, **24-bit, 44,100 Hz only**. EP2 OUT iso adaptive 288 B (sized for 48 frames). EP3 IN iso async 288 B. Sampling-frequency control. MIDI EP4 bulk 64 B. | Runtime-built **3654:7756**, bcdUSB 1.10, bcdDevice from version digits. C755 only if flag [ENG+760] is set. Audio on EP3 both ways (274/270 B). No sampling-frequency control. SU8 added. | No MSC/CDC/HID. Serial = chip ID in hex. Declares 44,100 Hz while I2S runs at 44,117.6 (+0.04%), absorbed in the FIFO. **Host tools must not rely on PIDs.** | `[proto]/find_desc.py`, `find2.py`; `[proto]/dis/app_V15.dis` 0x86ef6; `app_V16.dis` 0x27de4-0x27e86 | V |
| **BLE GATT** (identical) | 0x1800 Device Name; **0xAE40**: AE41 write-without-response (value 0x62), AE42 notify (0x64, CCCD 0x65); **BLE-MIDI** 03B80E5A… at 0x70-0x73. No DIS or battery service. | same | AE41 carries raw `00 59` syscmd frames (≥ 7 B, len + 7 ≤ 1046). **Never write AE41 on a stock unit.** | `[proto]/gatt.py`; `[proto]/dis/app_V15.dis` 0x12c2-0x15b4 (tbh on handle − 0x62) | V |
| **Bluetooth Classic** | One SDP record: **"JL_A2DP", AudioSink 0x110B**, AVDTP PSM 0x19, profile 0x110D v1.3. No HFP, AVRCP or SPP records. Hardware SBC decode, IRQ on CPU1. | same; task "bt_sbc_dec" | The stock FM-1 is a Bluetooth speaker | `[proto]/fw` V15 app+0x73e78, V16 +0x70f34; `[proto]/dis/app_V15.dis` 0x3e250-0x3e5e4 | V |
| **Vendor syscmd** (USB SysEx 7-bit packed; BLE AE41 raw). Frame: `[00 59][cmd][len u24 LE][payload][~sum8]`, LSB-first 8→7 packing over SysEx. | **17 (0x11)** device info, 27 B (the handshake `F0 00 32 45 00 00 00 40 7F F7` unpacks to cmd 0x11, not "parameter 0x40"). **18:** 13-B descriptor. **21:** drain ring. **33-36:** callback-object ops (null statically; dangerous). **48 (0x30):** data/OTA pull with "success\0" at 0xE0000000/0xF0000000. Others: no-ops. tbh at 0x2028B1E. | Same set, as an if-chain at 0x20237E8. **No new command class.** | Keep compatibility only where recovery needs it (0x11/0x30 framing). **Never fuzz 33-36 or 48.** | `[fw]/dis/v153.objdump.txt` 0x2028B1E-0x2028B38; `[fw]/out/v16_syscmd.txt`; `R/FM-1-RE/docs/io/11-ota-protocol.md:29-67,105-140` | V |
| **Hidden triggers** | USB-MIDI `F0 22 24 35 7F F7` = **enter OTA upgrade**. `F0 22 24 35 7D F7` = **ROM UBOOT soft key** (writes "usb_update_mode" to NVRAM 0x1C7FD80, then resets via P3_PR_PWR bit 4). `F0 35 59 xx xx xx F7` arms the vendor channel. | Same. V16 tests `(b \| 2) == 0x7F`, so it accepts both 7F and 7D. | **Red lines:** never send 7F or 7D outside the install guide's one sanctioned backup step | `[fw]/dis/v163.objdump.txt` 0x2024D28-0x2024D6E, 0x201A76E-0x201A78E | V |
| **Factory / test modes** | **None found:** no factory or test menu, shell or AT-console strings | None found | The only "service mode" is the mask-ROM UBOOT (MSC device 4C4A:8057 with vendor SCSI 0xFB/0xFC/0xFD) | `[fw]/strings/`; `R/jl-uboot-tool/docs/usb-protocol.md:1-25` | S (absence) |
| **DX7 SysEx** | 32-voice bulk `F0 43 0n 09 20 00`; single voice `F0 43 0n 00 1B`; parameter change `F0 43 1n` | Adds bank send/receive in the UI ("Sending Bank %c") | Not relevant to CYMA-1 | `[fw]` strings and handlers | V |
| **User records** | (V15 VM/USR) | Headers 'D7Q4'/'D7R2' with u16 version and size, data CRC and header CRC. A/B 36 KB slots at 0xDA000/0xE3000; 12 KB slots from 0xED000 (outside the JLFS table). | A torn-write-safe pattern worth copying (CYMA-1 already versions its objects) | `[fw]/dis/v163.objdump.txt` 0x201A582-0x201A6A2 | V |
| **Audio calibration** | None for the output path. FM-1-RE's "DAC trim" VM slot is RF/analog trim. | none | Nothing to preserve for audio | `[fw]` summary | S |
| **UART1 TX DMA** | Kicked from the MIDI route/thru pump | Same (0x2024F66) | No pad is known to carry it on the FM-1 | `[fw]/out/v1{5,6}.periph.txt` | P |

### 3.5 Protocols and connectivity

**Budget context for every row below.** CYMA-1 today (`docs/CYMA-1-voice-spec.md` §2 H7, built, never flashed):
- image 432,896 of 581,564 B, so 148,668 B free;
- pool 21,484 B free;
- `.ram_text` 16 KB free.

Anything that needs a radio stack must fit inside those numbers.

| Interface | What exists | Status | CYMA-1 route | Cost / risk | Evidence | Conf. |
|---|---|---|---|---|---|---|
| **UAC device rates and format** | One iso endpoint carries ≤ 1023 B per 1 ms frame. That fits 2-ch 24-bit at 96 kHz (≤ 582 B). 48 kHz capture via SET_CUR plus a 160/147 FIR. | **48 kHz shipped** in Felucca 1.1.5+, SLOOP 2.5+ (USB RATE menu since 2.7), Clipper, Golana and others. `uac_fir.h` exists in fm-106, dinghy and ChoralRootFM1. fm1-base keeps it off (`FM1B_UAC_48K 0`). | fm1-base patch: enable it and add `uac_fir.h` | ~100 instr/sample FIR at 48k. SLOOP warns some phones record 48k distorted. | `R/sloop-fm1/firmware/src/usb.c:17-21`; `R/fm1-base/device/unity.c:76-78`; `R/fm1-base/bsp/chip/usb.c:575-590` | V ↓ |
| **SOF clock** | 1 kHz SOF | Used only for presence/suspend detection. The drift servos run on ring fill (Melodee: the I2S clock is independent of SOF). | — | — | `R/fm1-base/bsp/chip/usb.c:1500-1520`; `R/melodee/firmware/src/usb_audio_stream.c:6-9` | V ↓ |
| **Multichannel capture** | At 44.1k: 7 ch × 24-bit or 11 × 16-bit per endpoint; at 48k: 6 or 10. Only 288 B packets are proven on this SIE. | Melodee: 4 mono stems (24-bit alt, but zero-padded 16-bit content). JIANT: 8 channels. | fm1-base patch (own AS descriptors) | ~256 B ring per 128 frames per channel at 16-bit | `R/melodee/firmware/src/usb_audio_stream.c:11-45`; JIANT README | S ↓ |
| **UAC playback into the engine** | Host 2-ch → ring → resampled to the I2S clock | Everyone mixes after the master. PIGEON records host audio as sampler material (offline). **Real-time synthesis input: nobody** (CYMA-1 VOX planned). | fm1-base patch v2 `plat_usb_audio_read` (voice spec §3.13) | ~86 instr/sample read; +8.4 KB RAM | `R/fm1-base/bsp/chip/usb.c:150-188`; https://github.com/TheShrimpBlimp/pigeon | V ↓ |
| **Endpoints and extra classes** | 5 hardware endpoints (EP0 + 4 IN/OUT pairs). fm1-base map: EP1 MIDI, EP4 audio, EP2/EP3 CDC. MS OS string 0xEE answered by stock. | CDC optional in fm1-base (macOS 13-15 composite bug worked around). **Vendor bulk plus WebUSB (BOS):** RemoteFM1, measured on a unit at 598-830 KB/s up and 630-855 KB/s down, but with USB polled ~120k/s from the main loop (36-47% of loop time). fm1-base SysEx ≈ 128 KB/s raw (2 kHz poll). | fm1-base patch | The endpoint budget is tight: vendor bulk excludes CDC/UAC in RemoteFM1 | `R/RemoteFM1/firmware/src/usb.c:22-49,226-288`; `R/RemoteFM1/PLAN.md:204-218`; `R/fm1-base/bsp/chip/usb.c:271-370` | V ↓ |
| **USB-MIDI multi-cable / MIDI 2.0 (UMP)** | Up to 16 virtual cables; a MIDI 2.0 alt setting on the same bulk endpoints | **Nobody** (one cable everywhere) | fm1-base patch (descriptors + UMP framer) | Host support varies (check macOS 26 CoreMIDI) | `R/fm1-base/bsp/chip/usb.c:351-362`; `R/melodee/BUILDING.md:189-191` | S |
| **USB host** | SDK host primitives (`usb_h_*`, `usb_h_iso_read`), UAC/HID/MSC host classes, no MIDI class | Melodee (input reported on hardware; OUT model-only). **felucca-bsp MPL-2.0 opt-in port.** fm1-base: not started. | Sync felucca-bsp | No VBUS: needs a powered hub. Never boot as host. | `R/felucca-bsp/chip/usb_host.c`; `R/fm1-base/bsp/docs/EXPERIMENTAL.md:28-32` | V ↓ |
| **BLE-MIDI** (peripheral + central) | fm1-base JieLi stack: advertising 100 ms; connection 11.25-26.25 ms then 11.25-15 ms; local MTU 64; 13-bit ms timestamps; central connects to any MIDI-service advertiser (another FM-1 qualifies) | Stock and fm1-base use it. Optimist has a clean-room stack (GPL-3.0, +30 KB flash / +6 KB RAM) and an FM-1↔FM-1 link with no clock (tested in emulation). **Clock/transport sync between FM-1s: nobody.** | fm1-base API (`ble = true`, `ble_central = true`) | **+124 KB flash, +45 KB RAM, +24 KB pool:** does not fit CYMA-1's 21 KB pool without trimming. RF-trim start-up current can corrupt flash reads (fm1-base mutes 1 s). | `R/fm1-base/ble/midi/le_midi.c:8-279`; `R/fm1-base/README.md:92-98`; `R/optimist/docs/BLE-DEVICES-DESIGN.md:108-129` | V |
| **Custom GATT service** (stock AE40 pattern) | A private write-without-response + notify pair beside BLE-MIDI | Stock uses it. **Community: none.** | fm1-base patch (add entries in `le_midi.c` profile data) | ~1 KB on top of the BLE stack | `[proto]/gatt.py` | V |
| **BLE 5 controller features** (`btctrler.a`) | 2M/Coded PHY, data-length extension, extended/periodic advertising, CTE, **BIS/CIS** (1 BIG of ≤ 5 BIS; `sync_get()` packet clocks in 1/12 µs), wl_mic 1→N. **PAST is absent** (feature bits only). | Unused and unproven on this radio. Optimist answers PHY_REQ with 1M only. | fm1-base patch (Route M) | RAM per BIG unknown; FCC conducted power is 1.44 mW, so Coded-PHY range on this antenna is modest | `[proto]/nm_btctrler.a.txt`; `SDK/include_lib/btstack/third_party/broadcast/big.h:7-119` | S ↓ |
| **A2DP sink** | Stock (§3.4) and official fm1_open (`fm1_bluetooth_audio_read`) | **No community firmware** | JieLi classic stack (Route M) | ~165 KB flash / 40 KB RAM [Optimist estimate]; 100-250 ms latency; pairing writes link keys | `R/fm1_open/sdk/include/fm1.h:45-54`; `R/optimist/docs/BT-CLASSIC.md:281-297` | V |
| **A2DP source** | Optimist clean-room BR/EDR + SBC encoder, measured streaming to a WH-1000XM4 | Experimental (Optimist) | Route G only (GPL-3.0) or JieLi `a2dp_source_*` | 150-300 ms latency: listening mode only | `R/optimist/docs/BT-CLASSIC.md:1-8,281-297` | V |
| **Wi-Fi STA + lwIP** | WPA2/open/hidden; lwIP 2.1.3, UDP (6 PCBs), IGMP (2 groups), DNS | fm1-radio (MP3/HE-AAC radio, TLS), COSMOS, the fm1-base WIFI example | fm1-base API (`wifi = true`) | ~300 KB flash, ~100 KB RAM, ~276 KB pool: **impossible next to CYMA-1.** Flash-read failures under Wi-Fi load. **Not in the FCC filing.** | `R/fm1-base/README.md:75-118`; `R/fm1-base/ble/lwip/lwipopts_sfc.h:119-236`; https://github.com/charlesvestal/fm1-radio | V |
| **OSC / RTP-MIDI / Ableton Link** | OSC = UDP. Soft-AP needs a library swap (`wl_wifi_sfc.a`); mDNS source is in the SDK. RTP-MIDI = AppleMIDI over 2 UDP ports. Link: an official ESP32 port exists; the full C++ library will not fit, and a C re-implementation is likely GPL-2.0+-derived. | **Nobody** | Wi-Fi build + app net code | All inherit the Wi-Fi cost and regulatory gap | `[proto]/nm_wl_wifi_sfc.a.txt`; https://github.com/Ableton/link/pull/68 | S (Link P) |
| **Raw 802.11** (ESP-NOW-style) | `wifi_send_data` (≤ 2510 B, 1-72 Mb/s), monitor mode, channel lock, RX callback with RSSI; JieLi demo streams PCM and JPEG between two AC79s | fm1-base uses only the RX callback (diagnostics) | fm1-base patch | Raw-only footprint unmeasured. Sub-ms only for short frames at ≥ 24 Mb/s. Not FCC-covered. | `SDK/include_lib/net/wifi/wifi_connect.h:202-600`; `SDK/apps/common/example/wifi/raw_txrx/raw_test.c:62-97` | S |
| **Shared timebases** | Wi-Fi TBTT (64 µs units); JieLi BT media sync / TWS piconet clock | Nobody | Route M, undocumented APIs | Heavy stacks | `[proto]/nm_btctrler.a.txt` | P |
| **Wi-Fi/BT coexistence (PTA)** | 8 coexistence tables in `cpu.a` | Nobody runs both radios | fm1-base patch | RAM makes it impractical for a synth | `ASM/rf_coexistence_config.h:1-45` | S |
| **Tooling command space** | Stock syscmd 17-48; fm1-base 0x11, 0x30, 0x41-0x43, 0x50-0x54 plus `APP_USB_CMD`. 0x7D sub-prefixes in community use: 46 4C, 4C 47, 4C 57, 4C 34, 56 4C, 4C 43, 53 52, 52 46, 4C 75, 45 43, 4D 4C, 47 52. fm1_open: `F0 7D 'F' 'M' 02`. | **CYMA-1 already uses `F0 7D 43 59` ("CY"), which is free.** No community syscmd ID ≥ 0x60. | Keep "CY"; use syscmd IDs ≥ 0x60 if a binary channel is added | — | `[proto-v]` tooling verdict; `docs/CYMA-1-sounds-spec.md` T3 | V |

---

## 4. Opportunities ranked for CYMA-1

**Scoring.** Each opportunity gets three marks from 1 to 5, and the score is their product (maximum 125):
- **musical value (V)**;
- **uniqueness among FM-1 firmwares (U)**;
- **feasibility under today's budgets and the no-mods rule (F).**

**Current design facts these plans rely on:**
- **Mode kernel:** a complex one-pole per mode, run mode-major over 32-sample sub-blocks. Cost per mode-sample: FREE 16, EXCITED 19, RAMP 18; bowed coupled modes 29-35.
- **Mode budget:** 96 on the device, 80 under the VOICE rule.
- **Block:** `CY_BLOCK` 128.
- **CPU:** 4,650 instr/sample = 100% of a block. Measured scenarios run at 64-83% in fm1-emu.
- **Memory:** pool 21 KB free; flash 148 KB free.

Sources: `docs/CYMA-1-spec.md` §3.5, §3.8, §3.14; `docs/CYMA-1-voice-spec.md` §2 H7-H9, §3.11.

**Already in flight, not ranked:** CYMA-1 VOX, the real-time USB voice input (§2.3 route 1). Several items below make it stronger.

| Rank | Opportunity | V | U | F | Score | How it plugs into CYMA-1 | Talking-plate link | Gate before relying on it |
|---|---|---|---|---|---|---|---|---|
| **1** | **Hardware FFT: "EAR", MODAL MIRROR and live spectra** | 4 | 5 | 4 | **80** | New own-code module `src/cy_hwfft.c`:<br>- **Driver:** register sequence re-expressed from the Apache-2.0 SDK (`JLM/jl_fft.c:296-337,674-699`), descriptor and buffers in `.bss`, bounded poll on CON bit 7, a crash breadcrumb, and the existing software FFT as fallback.<br>- **Main loop:** MODAL MIRROR device analysis (voice spec §5.2); the SCALE CATCHER key-LED idea (§5.3).<br>- **Audio ISR, VOICE on:** a 512-point real FFT per 128-sample hop costs 16.6 µs of accelerator time (0.6% of a block).<br>- **This removes voice spec D10's blocker:** upstream fm1-emu now models the block from FM-1 measurements. | Could replace or augment the EAR's order-10 LPC chain (counted 144 instr/sample) with an FFT envelope on the same 48-point grid: est. 20-40 instr/sample of CPU windowing and bin pooling. The sand on the VOICE page can show the real voice spectrum. | 1. A SILICON-EXACT golden test on the unit (§5.2 step FFT). 2. Input scaled to ≤ 2^15 (outputs wrap at ±2^28). 3. No xruns under ALNK DMA contention. 4. G-TALK re-run if the EAR changes. |
| **2** | **Hardware resonator bank on JL_EQ ("HW PLATE")** | 5 | 5 | 3 | **75** | New module `src/cy_hwbank.c`. Offload FREE and EXCITED modes of ringing or held slots, i.e. modes with no bow coupling and no per-sample glide:<br>- **Run mode 1:** one input buffer per slot (strikes as impulses, noise, voice) feeds up to 15 single-section outputs per run.<br>- **Coefficients:** refreshed per block from CYMA's (c, s, r) as a1 = 2r·cosθ, a2 = −r².<br>- **State** persists in EQ RAM or `FLT_ADR`.<br>- **Mixing:** the CPU mixes the outputs into L/R with gl/gr: ~4-5 instr per mode-sample instead of 16-19.<br>- **Capacity:** one 15-mode run over 128 frames ≈ 272 µs. Chaining runs from IRQ 60 could add an estimated 60-150 modes per voice set while the CPU renders the rest.<br>- **Onset:** offset the input by one sample to keep CYMA's sine onset, since a biquad's b0 ≠ 0. | **RING is literally "one input → N resonators",** the EQ's native mode. TALK's carrier modes could move there and lift the VOICE rule's 80-mode ceiling or TALK's 4-voice limit. | 1. f32-coefficient precision at long T60. Estimate: ~3 cents frequency error at 40 Hz, < 1 cent above 100 Hz. Measure against the host model. 2. Clicks on coefficient updates (glides, morphs). 3. Timing in situ with audio DMA. 4. Regression tests on upstream fm1-emu's `hweq` model. |
| 3 | **Float-MAC kernels** (inline asm in `src/voice.c`) | 3 | 4 | 4 | 48 | FREE: 6 multiplies + 4 adds, which with MAC/MSC becomes 2 multiplies + 4 MACs. Estimate: −3 to −4 instr per mode-sample (~20-25%). The same idea applies to EXCITED, RAMP and the bow junction. | Returns ~300-400 instr/sample at budget 96, enough to lift the VOICE rule's cuts | Whether the MAC is fused must be measured on the unit (§5.3). If fused, the host reference uses `fmaf` to keep determinism and `demo_hash` (spec §3.12). The kernel lint must accept the asm. |
| 4 | **DEEP PLATE on CPU1, the stock way** (voice spec §5.1) | 5 | 2 | 4 | 40 | fm1-base `plat_cpu2_*` with fallback. Copy V16's lessons: an asymmetric split (most slots on core 1), per-job late and overrun counters shown in the CPU meter, a one-block pipeline only if latency allows, and a stock-proven **320 MHz "stability" setting** as an option. | 8 TALK voices, or full mode sets under VOICE | `fm1-emu --flaky-core1` graceful degradation. The CORE1 probe on the user's unit (§5.2). Never write flash while core 1 runs. |
| 5 | **Native 48 kHz output** | 3 | 4 | 3 | 36 | Make `CY_FS` a runtime value: frequencies are already computed at control rate from Hz. fm1-base patch for the ALNK clock (CLK_CON2 bit 8, PLL_CON1 bit 25) and its timing constants (544-tick frames). Use 48 kHz UAC with no resampler. 96 kHz only as an experiment: modes dominate cost, so the budget would halve. | VOX through a Mac aggregate device at 48 kHz needs no 160/147 FIR and no drift guesswork | The DAC must lock at 48,077 Hz (§5.3 step RATE48). Clicks on switching. Tuning tables regenerated. |
| 6 | **Chip senses: VTEMP drift, power state, JL_RAND** | 2 | 5 | 3 | 30 | Main loop, once per second: VTEMP → a tiny global detune or damping drift ("AGE"). LDO5V → "on USB power" raises the DETAIL ceiling. JL_RAND seeds RAIN and strike jitter. | — | Reproduce the PMU sub-mux write from the `cpu.a` IR (`adc_pmu_detect_en`). Read-only probe first (§5.2 step SENSE). |
| 7 | **USB-MIDI 2.0 and multi-cable** | 2 | 5 | 3 | 30 | fm1-base patch (descriptor alt setting 1 + UMP framer). Per-key pressure as per-note 32-bit controllers. Separate cables for performance, clock and tooling SysEx. | — | macOS 26 CoreMIDI UMP check. Alt 0 stays MIDI 1.0. |
| 8 | **Vector unit as a parallel mixer or coupling engine** | 2 | 5 | 3 | 30 | Mix the EQ-bank outputs (int32 path) or apply a mode-coupling matrix for bowed plates. About CPU speed (~11 ns/element), so the gain is parallelism. | — | Q-format design. Measured rounding (half away from zero). |
| 9 | **IFFT "halo" layer** (hundreds of faint upper partials per voice via overlap-add IFFT) | 3 | 5 | 2 | 30 | A shimmer layer above the mode budget for MALLET/RAIN | — | Transient smear, bin quantisation; design work |
| 10 | **Hardware SRC for VOX and exciters** | 2 | 4 | 3 | 24 | Replace `uac_read`'s Catmull-Rom (86 instr/sample) with a 16-bit mono hardware resample (~3 µs per output, off-CPU). Varispeed MIRROR or exciter samples. The ratio 44,117.647/44,100 is exactly 2500/2499, and 44,117.647/48,000 is 125/136, so small integer rate pairs may express it if the block accepts them. | Voice is fine at 16-bit | Ratio-servo behaviour, the integer-rate hypothesis (P), and filter quality (24 taps) |
| 11 | **CORDIC (copex)** | 2 | 5 | 2 | 20 | sin/cos for modulated slot control (+35 instr/slot today) and exp for pitch → frequency | — | Selector sweep with an illegal-instruction guard (§5.3) |
| 12 | **Mode-group stems and "CV over audio"** | 3 | 2 | 3 | 18 | A separate opt-in image with its own UAC IN alt setting (≤ 7 × 24-bit) | Stem the dry voice and the talking plate separately | Isochronous packets > 288 B on this SIE are unproven |
| 13 | **USB host for MPE controllers** | 3 | 2 | 3 | 18 | Sync felucca-bsp's MPL-2.0 `usb_host.c`; per-note pressure → plate excitation | — | Powered hub; runtime menu only; never the boot default |
| 14 | **Hardware SBC exciter store** | 2 | 4 | 2 | 16 | Compressed impulse and exciter banks in flash | — | Register semantics beyond names |
| — | **Conditional on probe results** | | | | | **If DACL (pin 13) is routed:** a 16-bit mono cue or dry-exciter output. **If PB5 or the HUSB pads read the headphone-detect or amp-CTRL net:** automatic speaker voicing (high-pass, mono-safe mode sets, AGC-friendly lows) versus full-range headphones. | — | §5.2 steps PINS and DACL |
| — | **Not now** | | | | | Bluetooth A2DP sink, custom BLE editor or ensemble link, Wi-Fi OSC/RTP-MIDI/Link, raw 802.11: none fits a 21 KB pool next to CYMA-1, and Wi-Fi is outside the FCC filing. | A2DP: "the phone sings to the plate" (not live) | A memory plan first |

**Recommended build order** (each step keeps a CPU fallback and ships behind a boot-time probe that sets feature bits):
1. Update the local `reference/fm1-emu` clone to upstream master. Fix `[exp]/fm1-emu-fft`'s three wrong assumptions:
   - saturation (the block actually wraps at 29 bits);
   - floor rounding (it actually rounds half away from zero);
   - the 180 MHz clock (use the measured cycle tables instead).
2. Write `cy_accel` (own code): a boot probe for FFT and EQ with bounded waits and breadcrumbs, plus feature flags in telemetry.
3. JL_FFT for MIRROR and the VOICE-page spectrum, then the EAR (with a G-TALK re-run).
4. The JL_EQ bank for RING and ringing tails, then TALK carriers.
5. Float-MAC kernels.
6. DEEP PLATE.
7. 48 kHz.

---

## 5. The safe device-probe plan for the hardware session

### 5.1 Ground rules (red lines, from the flash-safety and no-mods rules)

**Approval and recovery:**
- Every install or write needs the user's explicit OK at that moment.
- Follow the gates of `notes/INSTALL.md`:
  - pinned package sha;
  - `--info` shows **FM-1_015**;
  - a verified full backup;
  - flash JEDEC **0x856014**, or never put custom code on that unit;
  - session conditions.
- Keep the unit on V15 between custom images.

**Never send:**
- `F0 22 24 35 7F F7`.
- `F0 22 24 35 7D F7`, except the install guide's one sanctioned backup step, with the user's OK.

**Never write, fuzz or touch:**
- syscmd 33-36 and 48: never fuzz.
- BLE AE41 on a stock unit: never write.
- Flash below 0x4000, BTIF and key_mac: never write.
- eFuse: never touch.

**Pins and rails:**
- Never drive PA2, PB1, PH7 or PH8.
- Never route an output channel onto PA2.
- Never enable MCPWM fixed pads on PA7/PA8.
- Never drive USB D± while a host or charger is attached.
- Never raise rails above V15 levels.
- Keep DBG_CON opt-in.

**Accelerators:** every accelerator access in a probe gets a bounded wait (≤ 2 ms), a stop-on-timeout, and a `.noinit` breadcrumb so that a probe which crashes is skipped on the next boot instead of boot-looping.

**Audio safety:** keep MASTER low and headphones off the ears for any clock or DAC experiment. Record through an audio interface instead.

### 5.2 Ordered plan

**Stage 0: no device (do these first).**

| # | Step | What it decides | Risk |
|---|---|---|---|
| 0.1 | Pull upstream fm1-emu (UPEMU `hwdsp`/`hweq`/`hwsrc` models fitted to FM-1 measurements) into `reference/fm1-emu`. Correct `[exp]/fm1-emu-fft` and the PROBE expectations: 29-bit wrap (not saturation), round half away from zero (not floor), measured cycle tables (not an assumed 180 MHz). | The PROBE results and CYMA-1 accelerator code can be regression-tested before any install | None |
| 0.2 | Extend PROBE with the new steps below (EQ multi-output, PINS, DACL, SENSE, RAND). Rebuild and re-pin its sha. (The old `FM-1_9500` sha a45881b0…bae8 predates these fixes.) | One install answers everything in Stage 2 | None |
| 0.3 | Optional: run the accelerator kernels on a JieLi AC79 DevKit, if one is available (ip2k has one). | Block behaviour with zero FM-1 risk | None |

**Stage 1: stock V15 only, nothing installed.**

| # | Step | What it decides | Risk |
|---|---|---|---|
| 1.1 | Host reads the descriptors passively (`ioreg -p IOUSB -l -w0`, `system_profiler SPUSBDataType`) | 4C4A:C755, endpoints (EP2-OUT 288, EP3-IN 288, EP4 bulk 64), the 5-interface layout | None |
| 1.2 | `tools/fm1_probe.py` identity check (cmd 0x11 only) | FM-1_015 on this unit (an install-guide gate) | None |
| 1.3 | Analog bench: 24-bit UAC tones and sweeps through stock; record J7 into 10 kΩ and 32 Ω loads | DAC family (noise floor ≈ −103 dB for CS4344 at 3.3 V vs ≈ −96 dB for CJC4344; a −9 to −10 dB shelf near 16 kHz would mean de-emphasis, i.e. U9 pin 2 is strapped), full scale (VA), U5 clip point | None (low volume) |
| 1.4 | Speaker path: L-only, R-only, L−R; level ramp; USB-meter current with and without a plug in J7 | Whether the amp sums L+R; AGC onset; whether the jack shuts the amp down or only cuts its input | None |
| 1.5 | Phone BLE explorer (nRF Connect, LightBlue): **read names and properties only.** Do not write AE41 or subscribe to AE42. | The GATT layout (0x1800, AE40 at 0x60, BLE-MIDI at 0x70) | None |
| 1.6 | Classic BT discovery from a phone; play audio; time tap-to-sound with a mic | That the A2DP sink is user-visible, and its real latency | Low: pairing stores a link key in stock's own flash (normal stock behaviour). Ask first. |
| 1.7 | Charging: LED2 colours while charging and when full, charging with the switch off, USB-meter current | Charger class and position | None |
| 1.8 | Unit off: USB-C breakout plus multimeter, CC1/CC2 to GND, VBUS to GND | 5.1 kΩ sink? Rules C-to-C host operation in or out | None |

**Stage 2: one PROBE diagnostic install** (fm1-base app). Prerequisites: all install-guide gates and the user's OK. The probe writes no flash at runtime and keeps fm1-base's USB-MIDI updater and boot guard. Results appear on screen and via `F0 7D 43 50 01 F7` / `03`.

| # | Step (in run order) | What it decides | Risk |
|---|---|---|---|
| 2.1 | **INFO** (reads only): CLK_SYS_CON, PLL0/1, dividers, CHIP_ID (0x10200), SFC divider, ALNK0 CON0-3, idle FFT/EQ/SRC CON, RING_OSC | The clock tree the bootloader left (360 or 320), silicon revision, exact ALNK mode bits | None |
| 2.2 | **CLOCK:** TIMER4-based CPU MHz. Also try TL_CKCNT; expect it not to count at DBG_CON = 0. | The real CPU clock; whether the counter needs DBG_CON | 0.5 ms IRQ hold |
| 2.3 | **RATE:** rendered frames vs TIMER4 over 3 s | 44,117.6 Hz on this unit | None |
| 2.4 | **FFT + GOLD:** C256, C512, R1024 on the SDK's ramp inputs → FNV fingerprints vs JieLi's silicon vectors | "SILICON-EXACT" proves the block, the driver protocol and bit-exactness | A DMA into the probe's own buffers; 2 ms bounded |
| 2.5 | **ACC:** two-tone R1024 at peaks 2^12, 2^16, 2^20, 2^22 vs the CPU float FFT | SNR and where the 29-bit wrap bites. Sets the block exponent. | As 2.4 |
| 2.6 | **SPEED / OVERLAP:** R512, R1024, a 4-descriptor batch, plus a CPU loop alongside | Agreement with UPEMU (R512 = 398 ticks); whether the CPU keeps full speed | As 2.4 |
| 2.7 | **AUDIO:** 400 back-to-back R1024 while rendering | xruns from DMA contention with ALNK | Audible glitches during the test only |
| 2.8 | **REACH:** buffers in `.bss`, pool and stack (opt-in: cache-RAM 0x1F28000) | Which RAM the accelerator DMA reaches | Low (cache-RAM behind a breadcrumb) |
| 2.9 | **VEC:** complex multiply >>15, MAC32 >>31 vs CPU 64-bit | Rounding (expect half away from zero); an erratum-free 32×32 MAC | As 2.4 |
| 2.10 | **EQ-1:** one RBJ section, 32-sample impulse vs CPU TDF-II | Presence, float semantics, latency | DMA into own buffers; 2 ms bounded |
| 2.11 | **EQ-BANK (new):** run mode 1, 15 single-section resonators, Q from 500 to 5000, 4 s impulse responses vs a double-precision host model; coefficient rewrite mid-ring; timing for 1, 4 and 8 chained runs per block while audio renders | **The go/no-go for "HW PLATE"** (§4 rank 2) | As 2.10 |
| 2.12 | **PINS (new, read-only):** PB5 input with pull-up, then pull-down, and the edge time. HS-USB pads: PHY pull-downs and line-state comparators only (no pull-ups, no SIE). Repeat with a plug in and out of J7 and with USB in and out. | Whether pins 39/40/41 are floating, strapped or wired (for example to the jack detect) | None (inputs only) |
| 2.13 | **DACL (new):** internal DAC alone at analog gain 0, −30 dBFS 1 kHz, I2S muted; read VOUTL via the SAR internal channel | Whether pin 13 drives anything (a possible second output) | Low: start at minimum gain |
| 2.14 | **SENSE (new):** VTEMP, VBAT/4, LDO5V/4, VBG, VDC14, SYSVDD once a second while warming up and plugging/unplugging USB | Temperature slope; USB-power detection; that the PMU sub-mux code works | Low (P33 analog-mux writes only) |
| 2.15 | **RAND (new):** stream 1 MB of R64L reads to the host | TRNG or LFSR; whiteness | None |
| 2.16 | **ERRATUM** (opt-in, 60 s) | This unit's fault rate for inlined 32×32→64 multiplies at 360 MHz | Deliberate corruption; a crash and reboot are possible; no flash involved |
| 2.17 | **CORE1** (opt-in, 5 min, separate `cpu2` build) | Whether this unit can run DEEP PLATE | Core 1 retires under fallback; no reboot loop |

### 5.3 Follow-up diagnostic builds (each one is a separate install decision)

| # | Build | Decides | Risk |
|---|---|---|---|
| F1 | **RATE48/96:** CLK_CON2 bit 8 + PLL_CON1 bit 25, LRDIV 256 → 48,077 Hz; then 128 → 96,154 Hz and 64 → 192,308 Hz; also the 192/17 tap at 128 → 88,235 Hz. Record a 1 kHz tone and a sweep at each. | Whether the DAC locks at each ratio; THD+N per rate | Loud noise if the DAC misbehaves: MASTER low, record via interface |
| F2 | **I2S format toggle:** channel mode 1 (I2S) vs 2 (left-justified) at −6 dBFS | Confirms Philips I2S (a 6 dB step and distortion in the wrong mode) | As F1 |
| F3 | **ISA probes:** float-MAC fused test (a = 1+2^-12, b = 1−2^-12, c = −1); copex selectors 0-31 in both encodings with the EMU illegal-instruction trap; packed-SIMD forms vs known operands | Which float-MAC semantics the host reference needs; whether CORDIC exists | A trap is caught by the EMU handler |
| F4 | **SRC:** sweeps and impulses at ratios 0.5-2.0, the integer-ratio test 2499:2500, consecutive calls | Quality, throughput, seamlessness | Low |
| F5 | **USB:** 48k alt (`uac_fir.h`), 24-bit capture alt, a 4/8-channel IN alt, isochronous maxp > 288 B; underrun counters over 10 minutes | The multichannel ceiling on this SIE | Low |
| F6 | **CLOCK320 A/B** (m-bootloader's 320 MHz settings in RAM code), then rerun ERRATUM and CORE1 | Whether both faults are clock-margin effects. Prices a "stability" setting. | Moderate: a watchdog reboot restores 360. Never with flash writes. |
| F7 | **Two-unit BLE-MIDI clock** (the user owns two FM-1s): central plus peripheral, timestamp de-jitter at 11.25 and 7.5 ms intervals | Whether an FM-1↔FM-1 ensemble reaches sub-ms jitter | Low (probe build, not CYMA-1) |
| F8 | **PERF stalls** (opt-in DBG_CON): the same kernel in XIP, `.ram_text` and with cache-RAM | Placement costs for CYMA-1 hot code | DBG_CON may arm faults |
| F9 | **ANC presence:** one read of 0x1F000 behind the breadcrumb | Whether the block exists | A possible bus fault, skipped the next time |

**Not recommended:**
- Wi-Fi tests: outside the FCC filing.
- Raw 802.11 TX and BLE BIS experiments.
- USB-C accessory-mode tests: they need an adapter; ask the user first.
- An S/PDIF dock.

### 5.4 Photo checklist (only if the user chooses to open the case; photos only, no modification)

**Conditions:**
- Battery unplugged, and the unit never powered while open.
- An ESD strap if available.
- Phone macro mode or a 10× loupe, with a ruler in frame.
- Two raking-light directions per part, and at least 20 px per character.
- Move the red antenna wire gently; never desolder it.

| # | Shot | Decides |
|---|---|---|
| 1 | **SoC, four low-angle side views plus one straight-down** | QFN fillet or LQFP gull-wing. Whether **pin 13** (bottom-left corner, under the antenna wire), **pin 39** and **pins 40/41** (top edge, between PB1/pin 42 and PB6/pin 38) have traces. |
| 2 | **U9:** straight-down macro of the marking and pin-1 dot, plus a 45° view of both pin rows | DAC identity (CS4344 / CJC4344 / ES7144); whether pin 2 carries the BCLK trace (internal-SCLK mode and de-emphasis) |
| 3 | **U5:** marking, plus 45° views of the pins to FB4/FB5 and the EN/gain pins | Headphone amp identity, strapping, output ceiling |
| 4 | **SL5316D:** both long sides at 45°, and the matching area on the bottom side | Where CTRL (pin 4) goes: the jack detect, a pull-up, or a GPIO (PB5?) |
| 5 | Markings of U6, U11, Q1, Q3, D57 and the SOT-23 beside J6 | Audio LDO voltage (DAC full scale), 3.3 V buck, USB protection |
| 6 | **U12** marking; values of R33, R35, R25, R22 and R77; LED2 while charging and when full | Charger part; whether any part can source VBUS (USB host without a hub) |
| 7 | **J7 and J3** at a low angle, with and without a TRS plug | Which leaf lifts: the sleeve-detect vs tip-switch mute mechanism |
| 8 | **U2/U3:** 45° along both rows, plus the bottom side beneath them | Chain order; whether QF/QG/QH (bits 13-15) and bits 11/12 go anywhere |
| 9 | Bottom with the battery lifted aside; the J13 trace | Hidden routing; the speaker feed |
| 10 | USB-C area: the two small resistors, the SOT-23, R21 | CC values; D± protection |
| 11 | LCD FPC printing and the module back | Controller, TE pin |
| 12 | Speaker label and diameter against the ruler | Impedance and power for speaker voicing |
| 13 | If the bare PCB comes out: a strong light behind it | A 2-layer board would show every trace, giving a near-complete netlist |

**Optional meter checks** (unit off, battery unplugged, continuity only; not a modification):
- pins 13, 39, 40 and 41 to anything;
- J7's detect contact ↔ SL5316D pin 4;
- U9 pin 2 ↔ SoC pin 35 (BCLK);
- the destinations of U3's QF-QH;
- CC1/CC2 to GND.

---

## 6. Open questions

Grouped by what answers them: photos (P), the probe session (D), host work (H), or the user (U). Deduplicated across all six tracks.

**The board (photos, meter, or PROBE PINS/DACL):**
1. Are **PB5 (pin 39)**, **HUSBDM/HUSBDP (pins 40/41)** and **DACL (pin 13)** routed? If so, to what: the headphone detect, the amp CTRL, a test pad? (P, D)
2. Is the package **QFN48 or LQFP48**? The scale measurement says a 6 mm body and 0.4 mm pitch (QFN); one photo reading says gull-wing leads. (P)
3. **U9:** identity (CS4344 / CJC4344 / ES7144LV); its VA rail (U6?); whether pin 2 is strapped (internal SCLK, which could leave 44.1k de-emphasis on). (P, D 1.3)
4. **U5:** identity, EN and gain straps, true output ceiling. (P, D 1.3)
5. **SL5316D:** where CTRL goes; whether the speaker gets L+R; sleeve-detect or tip-switch mute; is AGC on by default? (P, D 1.4)
6. **U12:** which charger? **USB-C CC values?** Is any part able to source VBUS? This decides whether USB host can ever work without a powered hub. (P, D 1.8)
7. Do 595 bits 13-15 go anywhere, and are bits 11/12 E6's selects? (P, D)
8. Does the B8 really expose only 2 audio-ADC channels (SEL), and which ones? Relevant only to out-of-policy mods. (H: probe ADC registers read-only)

**The accelerators (PROBE stage 2):**
9. **JL_EQ:** behaviour on coefficient rewrite mid-ring; precision at Q 1000-5000 with f32 coefficients; how many 15-mode runs fit per block alongside ALNK DMA and CPU1; can IRQ 60 chain runs from the audio ISR; can its DMA reach cache-RAM?
10. **JL_FFT:** which clock domain; contention with ALNK DMA; batch-field width; FFT/EQ concurrency on the shared bus.
11. Is the **float MAC fused**? This decides the host-reference strategy for CYMA-1 determinism.
12. Which **copex** selector encoding is real, if any; latency and precision.
13. Real semantics of the **packed-SIMD** forms.
14. **JL_SRC:** does the integer-ratio hypothesis (2499:2500, 125:136) hold? Ratio-servo step size? Any 24-bit mode?
15. **JL_RAND:** TRNG or LFSR; refresh rate.
16. **VTEMP** slope. Can the PMU sub-mux write be reproduced from the `cpu.a` IR without side effects?
17. Does `C0_TL_CKCNT` count with DBG_CON set, without arming fault exceptions?
18. Does **JL_ANC** exist on this die?

**Clocks and audio:**
19. Does the DAC lock at **48,077 / 96,154 / 192,308 Hz**? Is the wire format really Philips I2S? (D F1/F2)
20. Do the 360 MHz multiply erratum and the core-1 misreads go away at **320 MHz**, as M-VAVE's V16 choice hints? Does RING_OSC predict which units misread? (D F6, 2.1)

**Stock firmware (host analysis):**
21. Does anything in V16 reach the dormant FFT and convolution code, for example through runtime-built pointers (a Plate reverb)? Watchpoints in upstream fm1-emu would show it. (H)
22. How does V16 assign notes between its 16-voice (CPU1) and 4-voice (CPU0) engines? (H)
23. What does V16's [ENG+760] flag (which selects the C755 identity) mean, and does its USB-Audio/USB-MIDI menu change the PID? (H, D 1.1 on a V16 unit only)
24. What do syscmd 33-36 operate on? (H, never by fuzzing)
25. What is the 7,690-byte block at offset 0x10000 of V16's DX7.bin? (H)

**Protocols and regulatory:**
26. Is BLE covered by any certification for 2ARCP-FM1? The public summary shows only a DSS (BR/EDR) line. Is Wi-Fi covered anywhere (US/EU)? (H, U)
27. What is the largest isochronous max-packet this SIE accepts (above stock's 288 B)? (D F5)
28. Do BIS/CIS and periodic advertising in `btctrler.a` work with this radio, and at what RAM cost? (D, later)
29. What does a raw-802.11-only Wi-Fi bring-up cost, without wpa_supplicant and lwIP? (H)
30. How does macOS 26 CoreMIDI handle a USB-MIDI 2.0 alt setting? (H)
31. Which licence route, if CYMA-1 ever adds radio features? JieLi binaries are Apache-2.0 (Route M). Melodee and Optimist are GPL-3.0 (Route G). A Link re-implementation is likely GPL-2.0+. (U)

**Policy:**
32. Does the user count an **external adapter cable** (a USB-C breakout for MIDI out, an expression pedal or I2C CV) as a hardware modification? CYMA-1-voice-spec §0.4 treated it as out of policy. The FM-1 itself is untouched. (U)

---

## Appendix A. Refuted or corrected claims (do not repeat these)

| Claim (source) | Correction | Evidence |
|---|---|---|
| Pins 37-41 are flash PD5/PD4/PD6/PD1/PD0 (`REVERSE_ENGINEERING.md:115-121`) | PB7, PB6, PB5, HUSBDM, HUSBDP on the AC7911B8 | `[pins]/sch_b8_note.png`; `R/felucca-bsp/boards/fm1/fm1_adc.h:4-5` |
| U12 SOIC-8 could be flash; "stock SOIC-8 flash swap" (`R/felucca-bsp/docs/EXPERIMENTAL.md`) | It is the charger; the flash is in-package | §3.3 |
| "PB7 = pin 16" (hub) | PB7 is pin 37 | §3.2 |
| PH7 carries UART1 TX (`REVERSE_ENGINEERING.md:128`) | PH7 = UART1_RXA; UART1_TXA = PH6 (an LED line) | DS p.6 |
| PH8 is UART1_RXA | PH8 has no UART fixed function; it reaches UART1 RX via input channel 1 | `R/felucca-bsp/chip/fm1_uart.h:24-41` |
| PC6 is a codec reset | PC6 is I2S DAT3 | `R/fm1_open/app/board/fm1_audio.h:7-9` |
| MIDI OUT on DACL | Impossible: DACL is analog-only | DS p.6 |
| Five spare 74HC595 outputs | At most bits 13-15 | `R/fm1_open/app/board/fm1_input.h:47-56` |
| microSD on SPI1 | SPI1-B MISO is PC8 = LCD D/C. Half-duplex reads on PC10 do work (ST7789 RDSCL). | `ASM/spi.h:29-30,211-246` |
| "Stock firmware is single-core" (`R/ChoralRootFM1/docs/DUALCORE.md:445-446`); "V15 CPU1 parked" | V15 renders all voices on CPU1; V16 renders 16 of 20 there | `[fw-v]` CPU1 verdict |
| V15 has a 16-bit output path | V15 = Q23 + 512-segment tanh soft clip, like V16 | `[fw-v]` output verdict |
| Stock uses packed SIMD (47/60 hits) | Every hit is in data regions | `[soc-v]`, `[fw-v]` |
| The community avoids float MAC with `-ffp-contract=off` | JieLi clang 4.0.1 cannot emit it at all; inline asm works | `[fw-v]/fmac*.c` |
| "JL_FFT unused by everyone" (briefing gap 7) | V16 holds dormant FFT/vector code; a third party measured the block; no shipping firmware uses it | §3.1 |
| JL_EQ unused by the community | Melodee has an opt-in, unvalidated path | `R/melodee/BUILDING.md:718-755` |
| JL_SRC: stock only touches it at startup | V13-V15 link a full driver plus IRQ; V16 dropped it | `[soc-v]` SRC verdict |
| The SDK falls back to software FFT when the block is busy | Only under `KISS_FFT`, which is commented out | `JLM/jl_fft.c:16,142-145` |
| Vector unit ~1 element/cycle, floor rounding; FFT saturates | ~11 ns/element; rounds half away from zero; the FFT wraps at 29 bits | UPEMU |
| EQ RAM = 26 rows; plain DF-II | 20 slots in RAM plus spill to FLT_ADR; transposed DF-II | UPEMU `hweq.rs` |
| SRC CON2/CON3 are rate pairs (FM-1-RE, marked [med]) | CON1 = out<<16 \| in; CON2/3 = strides | UPEMU `hwsrc.rs` |
| The top-7-key row PA6 is readable via AMUX2 | PA6 has no analog function; AMUX2 = PA5 | DS p.5 |
| MIC2N = PA5, MIC3P = PH7, AMUX1 = PH8, AMUX3 = PH6 (board track) | PA4, PH6, PH7, PH4 | `[pins]/sdk_objs/adc.ll:10-12` |
| MIC1P = PH8 (`ladc.h` comment; CYMA-1-voice-spec H2) | MIC1P = PH9 (datasheet and the SDK driver table agree) | `[pins]/sdk_objs/adc.ll:10,71` |
| PAST is supported in `btctrler.a` | Only feature bits exist; no code | `[proto]/nm_btctrler.a.txt` |
| The SOF drives the UAC drift servos | Servos run on ring fill; SOF is used for presence/suspend | `R/melodee/firmware/src/usb_audio_stream.c:6-9` |
| 48 kHz UAC is unproven | It has shipped in Felucca 1.1.5+ and SLOOP 2.5+ | `R/sloop-fm1/firmware/src/usb.c:17-21` |
| Vendor bulk / WebUSB is unused | RemoteFM1, measured on a unit | `R/RemoteFM1/PLAN.md:204-218` |
| USB host needs a clean-room or GPL code | felucca-bsp has an MPL-2.0 port, made with permission | `R/felucca-bsp/NOTICE` |
| Nobody feeds host audio into synthesis | PIGEON records it as samples (offline); real time is still unclaimed | PIGEON README |
| The cycle counter is free to read | `C0_TL_CKCNT` does not count at DBG_CON = 0 | `R/fm1-x0x/docs/plans/2026-10-04-dual-core-and-clock.md:101-108` |
| ETM, MCPWM, output channels and JL_RAND are unused by the community | fm1-base uses ETM; Melodee uses MCPWM and the output channels; Optimist uses JL_RAND | §3.1 |
| Stock's HUSB-PHY writes prove HUSB use | They are the generic SDK GPIO driver (pins 150/151) | `[soc-v]` HUSB verdict |
| "Event 0x72 carries `00 59` frames" (FM-1-RE, V13/V14) | In V15, `00 59` frames arrive on ATT handle 0x62 (AE41); 0x72 is BLE-MIDI | `[proto-v]` AE40 verdict |
| "Only parameter 0x40 answers" | The handshake unpacks to syscmd 0x11 | `[fw]` summary |
| FM-1-RE "ADDA codec / DAC trim" is the audio path; 0x12F40 = "DAC DMA ch B" | Mostly RF analog plus linked SDK drivers; 0x12F40 is JL_AUDIO.ADC_CON | `ASM/WL82.h:695-701` |
| Fs measured at 44,145 Hz | +620 ppm is impossible for this crystal; Fs = 24 MHz / 544 exactly | `R/Felucca/firmware/src/audio.c:9` |
| Wi-Fi only exceeds certification at high power | The filing has no Wi-Fi line at all | https://fccid.co/fcc/2ARCP-FM1 |
| The 1.5 kΩ USB pull-up is on D+ only; USB-C UART works on a charger | Pull-ups exist on both lines; a BC1.2 charger shorts D+ to D− | DS p.11 Table 2-4 |
| A second MIDI IN on D− via UART1 | UART1 RX is taken (MIDI IN); use UART0/2 via remap 61 | `[pins-v]` |
| The two wake events are extra inputs | They share the 4 input channels | `SDK/cpu/wl82/port_waked_up.c:30-39` |
| CS4344: 105 dB DR on the FM-1 | 103 dB A-weighted at 3.3 V (105 dB is the 5 V figure) | `[board]/ds/cs4344.txt:196-242` |
| "PC3 is not bonded is unproven" (CYMA-1-voice-spec §2) | The B8 re-bond affects only pins 37-41, so the datasheet's port-C table stands: PC3/PC4 are not bonded (S) | DS p.7 |
| JL_SRC cannot express 44,117.6 Hz (voice spec D10) | The ratio is exactly 2500/2499; whether the block accepts small integers is untested (P) | arithmetic |

## Appendix B. Track working files

| Track | Directory | Key files |
|---|---|---|
| SoC peripherals | `build-test/re/soc-peripheral-atlas/` | Datasheet and schematic text; `sdklib/ll/` (SDK bitcode → IR); `dis/` (V15/V16 full disassembly, `sfrscan.py`, `*.sfr.txt`) |
| SoC verification | `build-test/re/soc-verify/` | `cc/` compile tests (FMA, SIMD, copex) plus re-linked stock images (local only) |
| Pins | `build-test/re/pins/` | `fm1_ac7911b8_pins.csv`; `ds-*.png`; `sch_*.png`; `sdk_objs/{gpio,adc,plnk}.ll` |
| Pins verification | `build-test/re/pins-verify/` | `soc_crop.jpg`; AC7916A page renders |
| Board | `build-test/re/board-teardown/` | `crops/`; `ds/` (CS4344, SL5316, TPA6132A2, manual); `fcc/` |
| Board verification | `build-test/re/board-verify/` | WL83 `ac792n_audio_link.{c,h}`; BR23 `ac6951_iis.h` |
| Stock firmware | `build-test/re/stock-fw-internals/` | `dis/v153.objdump.txt`, `dis/v163.objdump.txt`; `out/*.periph.txt`; `scripts/` (local only: M-VAVE-derived) |
| Firmware verification | `build-test/re/firmware-verify/` | `fmac.c`, `fmac_asm.c` |
| Protocols | `build-test/re/protocols-connectivity/` | `dis/app_V1{5,6}.dis`; `nm_*.txt`; `find_desc.py`, `gatt.py`; sparse `ac79-sdk/` |
| Protocols verification | `build-test/re/protocols-verify/` | (empty: the verifier worked from `[proto]` files and web sources) |
| Experiments | `build-test/re/experiments/` | `host/fft_golden_check.py`, `fft_precision_fit.py`; `fm1-emu-fft/`; `probe-app/`, `probe-app-c1/`; `runs/`; `run_all.sh` |
