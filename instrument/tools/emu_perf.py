#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""CYMA-1 device-time gates (tools/run_all.sh --perf): the shipping package, exactly as built (build/cyma-1.fwsc, no
instrumentation, no special build), run in the fm1-emu chip emulator at the FM-1's throughput, and timed from
breakpoints on the firmware's own functions.

  tools/emu_perf.py [run] [--app DIR] [--out DIR] [--emu BIN] [--only play,aether,held,idle] [--jobs N] [--profile]
                    [--render-max-pct 85] [--ui-cpu-ms 8] [--ui-steps 4.4e6] [--latency-p90-ms 15]
  tools/emu_perf.py build-emu [--force]    the emulator with FM1_HZ and FM1_PCHIST (reference/fm1-emu + 2 small
                                           patches, built offline in docker; about 20 s)
  tools/emu_perf.py analyse [--out DIR]    the gates again from the saved runs (OUT/SCENARIO/emu.log), no emulator
  tools/emu_perf.py selftest               the analysis against synthetic breakpoint logs (no docker, no build)

How it measures. fm1-emu executes one instruction a cycle; FM1_HZ=205000000 makes its clock 205 MHz, the FM-1's
measured native throughput (205 instructions/us at 360 MHz: research briefing §1), so emulated time is device time
and the audio DMA, TIMER4 and the governor's CPU meter all run against it. The emulator prints the instruction count
at every breakpoint (FM1_BREAK_PCS). They sit on:
  fm1_alnk0_irq (entry, and its return in isr_alnk0)  each audio block: render time, the half's period
  ui_frame (entry and return)                          each screen frame: wall time, and its CPU time once the audio
                                                       ISRs and LCD DMA waits inside it are taken out
  fm1_lcd_wait (entry and returns), pl_render (entry)  the LCD waits inside a frame; plate redraws (idle must be 0)
  cy_engine_note_on (entry)                            when a key or MIDI note reaches the engine: latency
and fm1-base's fm1b_dbg block is dumped at the end (halves, max_us, late = xruns). Return addresses come from
build/cyma-1.dis (the instruction after each call), so nothing is hard-coded and nothing is added to the image.

Scenarios (each from a fresh boot, so the first-boot tour is part of it, as for a new owner):
  play    preset 01, 4-note MIDI chords every 500 ms with a lean, and a key on the panel between them (10 s)
  aether  preset 21 AETHER (a bowed, morphing drone) chosen with PRESETS, a held 4-note bowed chord, pressure (8 s)
  held    preset 01, one key held from 4.5 s: the screen's cost while a note rings, frames of 5.5-6.5 s (7.5 s)
  idle    the tour taken (a note, a lean, SELECT, ALGORITHM, PRESETS), DAMP, then silence (9 s)
The screen's windows start once the boot splash is over: app_ui_init runs 0.62 s after reset in fm1-emu, and the
splash (ui/ui.c UI_SPL_END) takes 9.15 s since v1.1.2's longer credit hold (6.15 s in v1.1.1, 3.15 s with v1.1's
credit, 1.6 s before), so left alone it ends at 9.77 s. But a key, button or detent cuts it short: its burst into the plate starts at the next
frame and the chrome draws 0.5 s later. play's first panel key (2.25 s) and aether's PRESETS detent (1.7 s) cut it,
so their screens are live from about 2.75 s and 2.2 s, before their windows (from 4 s). held moved whole, by 2 s,
with the credit: its key goes down at 4.5 s (2.5 s), half-way between two of fm1-base's once-a-second usb_start
stalls (a key at 4.0 s waits out one: 36 ms). That key now cuts the splash, so the chrome draws at about 5.0 s and
the sand starts to follow the note there; its window moved by 0.5 s with that, to 5.5-6.5 s (5-6 s: the chrome's
first whole draw and the sand's first jump inside it), 0.5-1.5 s after the plate goes live as it was 0.5-1.5 s
after the strike before.

Gates (the defaults; each a flag so the integrator can ratchet them):
  render   the slowest audio block <= 85 % of the half (2.90 ms), every scenario
  xruns    fm1b_dbg.late == 0 (the DMA never overtook the render), every scenario, shipping governor
  ui_cpu   UI frame CPU, median while notes ring <= 8 ms (spec §6.9 asks 6 ms; reported beside it), every scenario
           that plays
  ui_steps UI frame wall time in instructions, median over 5.5-6.5 s of `held` <= 4.4 M (a regression ceiling: the
           review's 4.0 M over its 3.75 M at the stock 240 MHz, which reads 4.08 M at 205 MHz; the target is 1.6 M,
           6 ms of CPU plus the audio ISR's share)
  latency  input to sound (the next block after the engine got the note, plus the half the DMA then plays), p90
           <= 15 ms, keys and MIDI apart
  idle     plate redraws in the silent idle window == 0
Exit 0 every gate passes, 1 a gate fails, 77 cannot run here (no docker, no emulator, no device build): run_all
shows that as SKIP.

Caveats, stated in the report too: fm1-emu has no cache model (the FM-1's XIP flash cache can only make it slower
than 205/us on a cold path), its key model lands a press in 1.5 ms, and its UART MIDI delivers bytes at once. Still
not a measurement on an FM-1, but it is the real image at the real clock, deterministic run to run.
"""
import argparse
import bisect
import concurrent.futures as cf
import datetime
import hashlib
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
DEV_HZ = 205_000_000          # FM-1 native instructions per second (briefing §1: ~205 instr/us at 360 MHz)
EMU_STOCK_HZ = 240_000_000    # fm1-emu's own clock when FM1_HZ is not honoured
FS_DEV = 44117.647            # FM-1 I2S: 192 MHz / 17 / 256
IMAGE = "rust:1-slim-bookworm"
DBG_MAGIC = 0x44424731        # fm1b_dbg ("DBG1"): magic, halves, max_us, nested, in_audio, late, timer_irqs,
DBG_FIELDS = ["magic", "halves", "max_us", "nested", "in_audio", "late", "timer_irqs", "ui_frames", "last_us",
              "cpu_q12", "boots", "stage", "crash_seen"]
PATCH_VERSION = "1"           # bump when PATCHES change: the emulator is rebuilt

# fm1-emu, patched: FM1_HZ sets the emulated clock; FM1_PCHIST writes the whole pc histogram (both env vars only)
PATCHES = [
    ("crates/fm1-soc/src/periph.rs",
     "            hz: CPU_HZ,\n",
     "            hz: std::env::var(\"FM1_HZ\").ok().and_then(|v| v.parse().ok()).unwrap_or(CPU_HZ),\n"),
    ("crates/fm1-emu/src/main.rs",
     "        println!();\n        println!(\"pc histogram top-8:\");\n",
     "        println!();\n"
     "        if let Ok(path) = std::env::var(\"FM1_PCHIST\") {\n"
     "            use std::io::Write;\n"
     "            if let Ok(f) = std::fs::File::create(&path) {\n"
     "                let mut w = std::io::BufWriter::new(f);\n"
     "                for (pc, n) in pcs.iter() {\n"
     "                    let _ = writeln!(w, \"{pc:08x} {n}\");\n"
     "                }\n"
     "            }\n"
     "        }\n"
     "        println!(\"pc histogram top-8:\");\n"),
]

# ui_steps: the review set "4.0 M now" over a 3.75 M frame measured at fm1-emu's stock 240 MHz; at 205 MHz the same
# frame reads 4.08 M (the audio ISR takes a larger share of the wall time), so the same ceiling here is 4.4 M
DEFAULT_GATES = {"render_max_pct": 85.0, "ui_cpu_ms": 8.0, "ui_cpu_target_ms": 6.0, "ui_steps": 4.4e6,
                 "ui_steps_target": 1.6e6, "latency_p90_ms": 15.0}


# ------------------------------------------------------------------------------------------------ scenarios ---
def midi(t, *b):
    return (t, "midi:" + "".join(f"{x:02x}" for x in b), None)


def scen_play():
    """preset 01 (as shipped), 16 four-note MIDI chords 500 ms apart, leaned on, and a panel key between each"""
    ev, inputs = [], []
    chord = [48, 52, 55, 59]
    keys = [7, 9, 11, 12, 14, 16, 19, 21]          # C4 D4 E4 F4 G4 A4 C5 D5: never a note a chord is holding
    for i in range(16):
        t, sh = 2000 + 500 * i, (i % 3) * 2
        for j, n in enumerate(chord):
            ev.append(midi(t + 5 * j, 0x90, n + sh, 100))
            inputs.append({"t_ms": t + 5 * j, "kind": "midi", "note": n + sh})
            ev.append(midi(t + 450, 0x80, n + sh, 0))
        ev += [midi(t + 150, 0xD0, 80), midi(t + 420, 0xD0, 0)]
        k = keys[i % len(keys)]
        ev.append((t + 250, f"n{k}", t + 400))
        inputs.append({"t_ms": t + 250, "kind": "key", "key": k, "note": 53 + k})
    return {"ms": 10000, "events": ev, "inputs": inputs, "busy": (4.0, 10.0), "snaps": [4300, 9300],
            "about": "preset 01 SINGING BOWL as shipped: 16 four-note MIDI chords 500 ms apart with a lean (channel "
                     "pressure 80), and a panel key between each chord"}


def scen_aether():
    """21 AETHER: BOW + MORPH 60 %, the heaviest factory preset (the review measured its blocks at 123 % of the half)"""
    ev = [(1700, "PRESET:+20", None)]
    inputs = []
    for j, n in enumerate([48, 55, 60, 64]):
        ev.append(midi(2500 + 10 * j, 0x90, n, 100))
        inputs.append({"t_ms": 2500 + 10 * j, "kind": "midi", "note": n})
        ev.append(midi(7900, 0x80, n, 0))
    ev += [midi(2700, 0xD0, 90), midi(5000, 0x90, 67, 100), midi(7900, 0x80, 67, 0)]
    inputs.append({"t_ms": 5000, "kind": "midi", "note": 67})
    return {"ms": 8000, "events": ev, "inputs": inputs, "busy": (4.0, 8.0), "snaps": [3900, 6000],
            "about": "preset 21 AETHER (BOW, MORPH 60 %) chosen with PRESETS: a held, bowed 4-note chord with "
                     "channel pressure 90, a fifth note at 5 s"}


def scen_held():
    """one key held from 4.5 s (the review's measurement of the screen's cost: the median frame 0.5-1.5 s into the
    note; 2.5 s and 3-4 s until the splash's credit made the splash end at 3.77 s; 5-6 s until the key began to cut
    the splash, the chrome at 5.0 s: 0.5-1.5 s after the plate goes live)"""
    return {"ms": 7500, "events": [(4500, "n7", 6500)], "inputs": [{"t_ms": 4500, "kind": "key", "key": 7, "note": 60}],
            "busy": (5.5, 6.5), "ui_steps": True, "snaps": [5500],
            "about": "preset 01, first boot, one key (C4) held from 4.5 s to 6.5 s (it cuts the splash short: the "
                     "chrome at 5.0 s); the frames of 5.5-6.5 s"}


def scen_idle():
    """the tour taken the way a new owner takes it, the plate damped, then silence: nothing should redraw"""
    ev = [midi(1800, 0x90, 60, 100), midi(1850, 0xD0, 127), midi(2000, 0xD0, 0), midi(2050, 0x80, 60, 0),
          (2300, "SELECT:+1", None), (2500, "ALGO:+3", None), (2700, "PRESET:+1", None), (3000, "PRESET:-1", None),
          (3300, "SELECT:-1", None), (3600, "REC", 4600)]
    inputs = [{"t_ms": 1800, "kind": "midi", "note": 60}]
    return {"ms": 9000, "events": ev, "inputs": inputs, "busy": None, "idle": (7.0, 9.0), "snaps": [4000, 8500],
            "about": "first boot: a note, a lean, SELECT, ALGORITHM (a full turn: back to MALLET), PRESETS (there and "
                     "back), then DAMP held 1 s; the idle window is 7-9 s"}


SCENARIOS = {"play": scen_play, "aether": scen_aether, "held": scen_held, "idle": scen_idle}


def keys_arg(events):
    out = []
    for t, what, t2 in sorted(events, key=lambda e: e[0]):
        if what.startswith("midi:") or ":" in what:
            out.append(f"{t}:{what}")
        else:
            out.append(f"{t}-{t2}:{what}" if t2 else f"{t}:{what}")
    return ",".join(out)


# ------------------------------------------------------------------------------------------------- symbols ---
LINE_RE = re.compile(r"^\s*([0-9a-f]{6,8}):\s+((?:[0-9a-f]{2} )+)\s*\t(.*)$")
LABEL_RE = re.compile(r"^([A-Za-z_.$][\w.$]*):\s*$")
CALL_RE = re.compile(r"\bcall\b[^<]*<([A-Za-z_.$][\w.$]*) : ([0-9a-f]+) >")
REF_RE = re.compile(r"<([A-Za-z_.$][\w.$]*) : ([0-9a-f]+) >")


def read_dis(path):
    """functions {name: entry}, function ranges [(start, name)], call sites {callee: [return address]}, and data
    symbols seen as references {name: addr}, from the device build's objdump text"""
    funcs, starts, calls, refs = {}, [], {}, {}
    label = None
    with open(path, errors="replace") as f:
        for line in f:
            m = LABEL_RE.match(line)
            if m:
                label = m.group(1)
                continue
            m = LINE_RE.match(line)
            if not m:
                continue
            a = int(m.group(1), 16)
            if label is not None:
                funcs.setdefault(label, a)
                starts.append((a, label))
                label = None
            text = m.group(3)
            c = CALL_RE.search(text)
            if c:
                calls.setdefault(c.group(1), []).append(a + len(m.group(2).split()))
            for r in REF_RE.finditer(text):
                refs.setdefault(r.group(1), int(r.group(2), 16))
    starts.sort()
    return {"funcs": funcs, "starts": starts, "calls": calls, "refs": refs}


def resolve(dis):
    """the breakpoints: {role: [addresses]}; a missing optional symbol leaves its role empty"""
    f, c = dis["funcs"], dis["calls"]
    need = {"isr": "fm1_alnk0_irq", "ui": "ui_frame", "note": "cy_engine_note_on"}
    miss = [s for s in need.values() if s not in f]
    if miss or "fm1b_dbg" not in dis["refs"]:
        raise SystemExit(f"emu_perf: build/cyma-1.dis has no {', '.join(miss + ([] if 'fm1b_dbg' in dis['refs'] else ['fm1b_dbg']))}")
    b = {"isr_in": [f["fm1_alnk0_irq"]], "isr_out": c.get("fm1_alnk0_irq", []),
         "ui_in": [f["ui_frame"]], "ui_out": c.get("ui_frame", []),
         "note": [f["cy_engine_note_on"]],
         "lcd_in": [f["fm1_lcd_wait"]] if "fm1_lcd_wait" in f else [], "lcd_out": c.get("fm1_lcd_wait", []),
         "plate": [f["pl_render"]] if "pl_render" in f else []}
    if not b["isr_out"] or not b["ui_out"]:
        raise SystemExit("emu_perf: no call site of fm1_alnk0_irq or ui_frame in build/cyma-1.dis")
    return b, dis["refs"]["fm1b_dbg"]


# ------------------------------------------------------------------------------------------------ analysis ---
BREAK_RE = re.compile(r"^break c(\d) pc=0x([0-9a-f]+) \[(\d+)\].*?r0-11=\[([^\]]*)\]")


def parse_log(text):
    hits, dump, on = [], [], False
    for line in text.splitlines():
        if line.startswith("break "):
            m = BREAK_RE.match(line)
            if m:
                regs = [int(x, 16) for x in m.group(4).replace(" ", "").split(",") if x]
                hits.append((int(m.group(2), 16), int(m.group(3)), regs, int(m.group(1))))
            continue
        if line.startswith("dump 0x"):
            on = True
            continue
        if on:
            m = re.match(r"\s+0x[0-9a-f]+:\s+(.*)", line)
            if not m:
                on = False
                continue
            dump += [int(x, 16) for x in m.group(1).split()]
    return hits, dump, ("cpu1 released" in text)


def pairs(hits, ins, outs):
    """[(start_step, end_step)] for each entry followed by its return (not re-entrant)"""
    res, cur = [], None
    for pc, st, _, _ in hits:
        if pc in ins:
            cur = st
        elif pc in outs and cur is not None:
            res.append((cur, st))
            cur = None
    return res


def overlap(iv, others_start, others):
    """steps of interval iv covered by the sorted, disjoint intervals `others` (their starts in others_start)"""
    a, b = iv
    tot = 0
    i = max(0, bisect.bisect_right(others_start, a) - 1)
    while i < len(others) and others[i][0] < b:
        s, e = others[i]
        lo, hi = max(a, s), min(b, e)
        if hi > lo:
            tot += hi - lo
        i += 1
    return tot


def pct(xs, q):
    if not xs:
        return None
    s = sorted(xs)
    k = (len(s) - 1) * q / 100.0
    lo, hi = int(k), min(int(k) + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def analyse(hits, dump, bp, scen, hz=DEV_HZ):
    """every figure of one run, from its breakpoint hits"""
    S = lambda st: st / hz                                   # noqa: E731  steps -> seconds
    isr = pairs(hits, set(bp["isr_in"]), set(bp["isr_out"]))
    ui = pairs(hits, set(bp["ui_in"]), set(bp["ui_out"]))
    lcd = pairs(hits, set(bp["lcd_in"]), set(bp["lcd_out"])) if bp["lcd_in"] else []
    isr_s = [a for a, _ in isr]
    lcd_s = [a for a, _ in lcd]
    r = {"blocks": len(isr), "frames": len(ui)}
    if len(isr) < 20:
        r["error"] = f"only {len(isr)} audio blocks seen: the image did not run"
        return r
    period = statistics.median(b[0] - a[0] for a, b in zip(isr, isr[1:]))
    r["half_steps"] = period
    r["half_us"] = period / hz * 1e6
    r["frames_per_half"] = round(r["half_us"] * 1e-6 * FS_DEV)
    # an app's half is a power of two frames (APP_BLOCK): at the stock 240 MHz clock it would read as 150 or 300
    r["clock_ok"] = (r["frames_per_half"] in (64, 128, 256, 512) and
                     abs(period - r["frames_per_half"] / FS_DEV * hz) < 0.01 * period)
    t_from = 1.5
    blk = [(b - a) for a, b in isr if S(a) >= t_from]
    if blk:
        r["render_max_us"] = max(blk) / hz * 1e6
        r["render_p99_us"] = pct(blk, 99) / hz * 1e6
        r["render_mean_us"] = statistics.mean(blk) / hz * 1e6
        r["render_max_pct"] = 100.0 * max(blk) / period
        r["render_p99_pct"] = 100.0 * pct(blk, 99) / period
        r["render_mean_pct"] = 100.0 * statistics.mean(blk) / period
        r["blocks_over_85"] = sum(1 for x in blk if x > 0.85 * period)
        r["blocks_over_100"] = sum(1 for x in blk if x > period)
    if dump and dump[0] == DBG_MAGIC:
        r["dbg"] = dict(zip(DBG_FIELDS, dump))
        r["xruns"] = r["dbg"]["late"]
    else:
        r["xruns"] = None
    # the screen
    frames = []
    for a, b in ui:
        tot = b - a
        i_isr = overlap((a, b), isr_s, isr)
        w = 0
        if lcd:                                              # LCD waits inside the frame, less any ISR inside them
            j = max(0, bisect.bisect_right(lcd_s, a) - 1)
            while j < len(lcd) and lcd[j][0] < b:
                s, e = max(a, lcd[j][0]), min(b, lcd[j][1])
                if e > s:
                    w += (e - s) - overlap((s, e), isr_s, isr)
                j += 1
        frames.append({"t": S(a), "steps": tot, "isr": i_isr, "lcd": w, "cpu": tot - i_isr - w})
    plate = sorted(st for pc, st, _, _ in hits if pc in bp["plate"])

    def frame_stats(lo, hi):
        fr = [f for f in frames if lo <= f["t"] < hi]
        d = {"window": [lo, hi], "n": len(fr), "fps": len(fr) / (hi - lo),
             "plate_renders": sum(1 for st in plate if lo <= S(st) < hi) if bp["plate"] else None}
        if not fr:
            return d
        for k in ("steps", "cpu", "isr", "lcd"):
            v = [f[k] for f in fr]
            d[k + "_median"] = statistics.median(v)
            d[k + "_p90"] = pct(v, 90)
            d[k + "_max"] = max(v)
        d["cpu_median_ms"] = d["cpu_median"] / hz * 1e3
        d["cpu_p90_ms"] = d["cpu_p90"] / hz * 1e3
        d["wall_median_ms"] = d["steps_median"] / hz * 1e3
        return d
    if scen.get("busy"):
        r["ui_busy"] = frame_stats(*scen["busy"])
    if scen.get("idle"):
        r["ui_idle"] = frame_stats(*scen["idle"])
    # latency: each input to the engine's note-on, then to the next block, plus the half the DMA plays it in
    notes = [(st, regs) for pc, st, regs, _ in hits if pc in bp["note"]]
    used, lat = set(), []
    for inp in scen.get("inputs", []):
        t0 = inp["t_ms"] / 1000.0 * hz
        hit = None
        for i, (st, regs) in enumerate(notes):
            if i in used or st < t0 or len(regs) < 3:
                continue
            if st > t0 + 0.5 * hz:
                break
            key = regs[0] - (1 << 32) if regs[0] >= 1 << 31 else regs[0]
            if regs[1] != inp["note"] or (inp["kind"] == "key" and key != inp["key"]):
                continue
            hit = (i, st)
            break
        if hit is None:
            lat.append({**inp, "missed": True})
            continue
        used.add(hit[0])
        j = bisect.bisect_left(isr_s, hit[1])
        nxt = isr_s[j] if j < len(isr_s) else None
        e = {**inp, "engine_ms": (hit[1] - t0) / hz * 1e3}
        if nxt is not None:
            e["sound_ms"] = (nxt - t0 + period) / hz * 1e3
        lat.append(e)
    r["latency"] = lat
    for kind in ("midi", "key"):
        v = [e["sound_ms"] for e in lat if e["kind"] == kind and "sound_ms" in e]
        miss = sum(1 for e in lat if e["kind"] == kind and e.get("missed"))
        if v or miss:
            r[f"lat_{kind}"] = {"n": len(v), "missed": miss, "p50_ms": pct(v, 50), "p90_ms": pct(v, 90),
                                "max_ms": max(v) if v else None,
                                "engine_p90_ms": pct([e["engine_ms"] for e in lat if e["kind"] == kind and "engine_ms" in e], 90)}
    return r


def wav_rms_db(path, t0, t1):
    try:
        with wave.open(path) as w:
            fs, n, ch = w.getframerate(), w.getnframes(), w.getnchannels()
            a = max(0, int(t0 * fs))
            w.setpos(min(a, n))
            raw = w.readframes(max(0, min(n, int(t1 * fs)) - a))
    except (OSError, wave.Error, EOFError):
        return None
    import array
    s = array.array("h", raw)
    if not s:
        return None
    ms = sum(x * x for x in s[::7]) / len(s[::7])
    return 10.0 * __import__("math").log10(ms / (32768.0 ** 2) + 1e-30)


# ----------------------------------------------------------------------------------------------------- gates ---
def gates(results, g):
    out = []

    def gate(gid, desc, value, limit, unit, ok, note=""):
        out.append({"id": gid, "desc": desc, "value": value, "limit": limit, "unit": unit, "pass": ok, "note": note})
    for name, r in results.items():
        if r.get("error"):
            gate(f"{name}_run", f"{name}: the scenario ran", None, None, "", False, r["error"])
            continue
        if not r.get("clock_ok", False):
            gate(f"{name}_clock", f"{name}: the emulator ran at {DEV_HZ / 1e6:.0f} MHz (FM1_HZ honoured)",
                 r.get("half_steps"), None, "steps/half", False,
                 "the half's period is not 205 MHz's: an unpatched fm1-emu (tools/emu_perf.py build-emu)")
            continue
        if "render_max_pct" in r:
            gate(f"{name}_render", f"{name}: slowest audio block <= {g['render_max_pct']:.0f} % of the half "
                 f"({r['half_us']:.0f} us)", round(r["render_max_pct"], 1), g["render_max_pct"], "%",
                 r["render_max_pct"] <= g["render_max_pct"],
                 f"max {r['render_max_us']:.0f} us, p99 {r['render_p99_pct']:.0f} %, mean {r['render_mean_pct']:.0f} %; "
                 f"{r['blocks_over_85']} blocks over 85 %, {r['blocks_over_100']} over 100 %")
        x = r.get("xruns")
        gate(f"{name}_xruns", f"{name}: no xruns (fm1b_dbg.late), shipping governor", x, 0, "", x == 0,
             (f"fm1-base's max_us {r['dbg']['max_us']} us, cpu_q12 {r['dbg']['cpu_q12']}" if r.get("dbg") else
              "no fm1b_dbg dump"))
        u = r.get("ui_busy")
        if u and not u["n"]:
            gate(f"{name}_ui_cpu", f"{name}: UI frames while notes ring", 0, None, "frames", False,
                 "no ui_frame returned in the window: the main loop stalled")
        elif u:
            ms = u["cpu_median_ms"]
            gate(f"{name}_ui_cpu", f"{name}: UI frame CPU, median while notes ring <= {g['ui_cpu_ms']:.0f} ms",
                 round(ms, 2), g["ui_cpu_ms"], "ms", ms <= g["ui_cpu_ms"],
                 f"spec §6.9 target {g['ui_cpu_target_ms']:.0f} ms; p90 {u['cpu_p90_ms']:.1f} ms; {u['fps']:.1f} fps; "
                 f"wall {u['wall_median_ms']:.1f} ms of which audio ISR {u['isr_median'] / DEV_HZ * 1e3:.1f}, "
                 f"LCD wait {u['lcd_median'] / DEV_HZ * 1e3:.1f}")
        if u and u["n"] and r.get("ui_steps_gate"):
            st = u["steps_median"]
            gate(f"{name}_ui_steps", f"{name}: UI frame wall time, median while a note rings <= "
                 f"{g['ui_steps'] / 1e6:.1f} M instructions", round(st / 1e6, 3), g["ui_steps"] / 1e6, "M",
                 st <= g["ui_steps"], f"target {g['ui_steps_target'] / 1e6:.1f} M; p90 {u['steps_p90'] / 1e6:.2f} M, "
                 f"max {u['steps_max'] / 1e6:.2f} M")
        for kind, what in (("midi", "MIDI"), ("key", "panel key")):
            L = r.get(f"lat_{kind}")
            if not L:
                continue
            ok = L["missed"] == 0 and L["p90_ms"] is not None and L["p90_ms"] <= g["latency_p90_ms"]
            gate(f"{name}_lat_{kind}", f"{name}: {what} to sound, p90 <= {g['latency_p90_ms']:.0f} ms",
                 None if L["p90_ms"] is None else round(L["p90_ms"], 1), g["latency_p90_ms"], "ms", ok,
                 f"{L['n']} notes" + (f", {L['missed']} never reached the engine" if L["missed"] else "") +
                 (f"; p50 {L['p50_ms']:.1f}, max {L['max_ms']:.1f} ms; to the engine p90 {L['engine_p90_ms']:.1f} ms"
                  if L["n"] else ""))
        i = r.get("ui_idle")
        if i is not None and i.get("plate_renders") is not None:
            gate(f"{name}_plate_idle", f"{name}: no plate redraw in silent idle ({i['window'][0]:.0f}-"
                 f"{i['window'][1]:.0f} s)", i["plate_renders"], 0, "redraws", i["plate_renders"] == 0,
                 f"{i['fps']:.1f} frames/s idle" + (f", CPU median {i['cpu_median_ms']:.2f} ms a frame" if i["n"] else ""))
        if r.get("audible_db") is not None and name != "idle":
            gate(f"{name}_audible", f"{name}: the scenario played (output RMS > -60 dBFS while busy)",
                 round(r["audible_db"], 1), -60, "dBFS", r["audible_db"] > -60)
    return out


# ------------------------------------------------------------------------------------------------- emulator ---
def find_root(app):
    d = os.path.abspath(app)
    while d != "/" and not os.path.isdir(os.path.join(d, "reference", "fm1-base")):
        d = os.path.dirname(d)
    return d if d != "/" else None


def emu_dir(root):
    return os.environ.get("FM1_EMU_PERF_DIR") or os.path.join(root, "build-test", "fm1-emu-perf")


def emu_stamp(root):
    ref = os.path.join(root, "reference", "fm1-emu")
    h = hashlib.sha256(PATCH_VERSION.encode())
    for rel, _, _ in PATCHES:
        with open(os.path.join(ref, rel), "rb") as f:
            h.update(f.read())
    return h.hexdigest()[:16]


def docker_ok():
    try:
        return subprocess.run(["docker", "info"], capture_output=True, timeout=30).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def build_emu(root, force=False):
    """reference/fm1-emu copied, patched (FM1_HZ, FM1_PCHIST) and built in docker; returns the binary or None"""
    d = emu_dir(root)
    binp = os.path.join(d, "target", "release", "fm1-emu")
    stamp = os.path.join(d, "stamp")
    want = emu_stamp(root)
    if not force and os.path.exists(binp) and os.path.exists(stamp) and open(stamp).read().strip() == want:
        return binp
    ref = os.path.join(root, "reference", "fm1-emu")
    src = os.path.join(d, "src")
    print(f"emu_perf: building the patched fm1-emu in {d}")
    if os.path.isdir(src):
        shutil.rmtree(src)
    shutil.copytree(ref, src, ignore=shutil.ignore_patterns(".git", "target", "work", "web", "fm1-sid", "probes",
                                                           "docs", "*.wav", "*.bmp"))
    for rel, old, new in PATCHES:
        p = os.path.join(src, rel)
        with open(p) as f:
            s = f.read()
        if s.count(old) != 1:
            print(f"emu_perf: reference/fm1-emu changed: the patch anchor in {rel} is gone (update PATCHES)")
            return None
        with open(p, "w") as f:
            f.write(s.replace(old, new))
    env = ["-e", f"CARGO_TARGET_DIR={os.path.join(d, 'target')}"]
    cargo = ["cargo", "build", "--release", "-p", "fm1-emu"]
    home = os.path.join(root, "build-test", "cargo-home")
    if os.path.isdir(os.path.join(home, "registry")):
        env += ["-e", f"CARGO_HOME={home}"]
        cargo.append("--offline")
    cmd = ["docker", "run", "--rm", "--platform", "linux/arm64", "--user", f"{os.getuid()}:{os.getgid()}"] + env + \
          ["-v", f"{root}:{root}", "-w", src, IMAGE] + cargo
    p = subprocess.run(cmd, capture_output=True, text=True)
    with open(os.path.join(d, "build.log"), "w") as f:
        f.write(p.stdout + p.stderr)
    if p.returncode != 0 or not os.path.exists(binp):
        print("emu_perf: the emulator did not build:\n" + "\n".join((p.stdout + p.stderr).splitlines()[-12:]))
        return None
    with open(stamp, "w") as f:
        f.write(want + "\n")
    return binp


def run_one(name, scen, ctx):
    rd = os.path.join(ctx["out"], name)
    if os.path.isdir(rd):
        shutil.rmtree(rd)
    os.makedirs(os.path.join(rd, "work"))
    fw = os.path.join(rd, "cyma-1.fwsc")
    shutil.copy2(ctx["fwsc"], fw)
    bp = ctx["bp"]
    pcs = sorted({a for v in bp.values() for a in v})
    env = ["-e", f"FM1_HZ={DEV_HZ}", "-e", "FM1_BREAK_MAX=200000000", "-e", "FM1_BREAK_PCS=" + ",".join(f"{a:x}" for a in pcs)]
    if ctx["profile"]:
        env += ["-e", "FM1_PCHIST=pchist.txt"]
    args = ["--fwsc", fw, "--ms", str(scen["ms"]), "--keys", keys_arg(scen["events"]), "--dump", f"{ctx['dbg']:x}",
            str(len(DBG_FIELDS)), "--wav", "out.wav", "--snap", ",".join(str(s) for s in scen["snaps"])]
    cmd = ["docker", "run", "--rm", "--platform", "linux/arm64", "--user", f"{os.getuid()}:{os.getgid()}"] + env + \
          ["-v", f"{ctx['root']}:{ctx['root']}", "-w", rd, IMAGE, ctx["emu"]] + args
    with open(os.path.join(rd, "cmd.txt"), "w") as f:
        f.write(" ".join(f"'{c}'" if " " in c else c for c in cmd) + "\n")
    t0 = datetime.datetime.now()
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=scen["ms"] / 1000 * 90 + 300)
        log, rc = p.stdout + p.stderr, p.returncode
    except subprocess.TimeoutExpired as e:
        log, rc = (e.stdout or "") + "\nTIMEOUT\n", -9
    with open(os.path.join(rd, "emu.log"), "w") as f:
        f.write(log)
    wall = (datetime.datetime.now() - t0).total_seconds()
    print(f"  ran {name:7s} {scen['ms'] / 1000:.0f} s emulated in {wall:.0f} s (exit {rc})", flush=True)
    return name, rc, wall


def snaps_to_png(rd, name, scen, out):
    try:
        from PIL import Image
    except ImportError:
        return []
    made = []
    for t in scen["snaps"]:
        b = os.path.join(rd, "work", f"lcd_{t}.bmp")
        if os.path.exists(b):
            p = os.path.join(out, f"{name}_{t}ms.png")
            Image.open(b).resize((480, 480), Image.NEAREST).save(p)
            made.append(os.path.basename(p))
    return made


def profile(rd, dis, top=14):
    ph = os.path.join(rd, "pchist.txt")
    if not os.path.exists(ph):
        return None
    starts = dis["starts"]
    addrs = [a for a, _ in starts]
    by, tot = {}, 0
    with open(ph) as f:
        for line in f:
            a, n = line.split()
            a, n = int(a, 16), int(n)
            tot += n
            i = bisect.bisect_right(addrs, a) - 1
            nm = starts[i][1] if i >= 0 else "?"
            by[nm] = by.get(nm, 0) + n
    return {"total": tot, "top": sorted(by.items(), key=lambda x: -x[1])[:top]}


# --------------------------------------------------------------------------------------------------- report ---
def fmt(v):
    if v is None:
        return "-"
    if isinstance(v, float):
        return f"{v:.3g}" if abs(v) < 1000 else f"{v:.0f}"
    return str(v)


def write_report(out, res):
    L = []
    w = L.append
    w("CYMA-1 device time, measured in fm1-emu at 205 instructions/us (tools/emu_perf.py)")
    w(f"package {res['package']['file']}  {res['package']['sha256']}  ({res['package']['bytes']} B)")
    w("the shipping image, unmodified; breakpoints on fm1_alnk0_irq, ui_frame, fm1_lcd_wait, pl_render, "
      "cy_engine_note_on. fm1-emu has no cache model: on the FM-1 a cold flash path can only be slower")
    w("")
    for name, r in res["scenarios"].items():
        w(f"== {name}: {res['about'][name]}")
        if r.get("error"):
            w(f"   {r['error']}")
            continue
        w(f"   audio  {r['blocks']} blocks, half {r['half_us']:.0f} us ({r['frames_per_half']} frames)"
          + (f"; render mean {r['render_mean_pct']:.0f} %, p99 {r['render_p99_pct']:.0f} %, max {r['render_max_pct']:.0f} %"
             f" ({r['render_max_us']:.0f} us); xruns {r['xruns']}" if "render_max_pct" in r else ""))
        if r.get("dbg"):
            d = r["dbg"]
            w(f"   fm1b_dbg halves {d['halves']}, max_us {d['max_us']}, late {d['late']}, ui_frames {d['ui_frames']}, "
              f"cpu {d['cpu_q12'] * 100 // 4096} % (smoothed, last)")
        for key, label in (("ui_busy", "screen, notes ringing"), ("ui_idle", "screen, idle")):
            u = r.get(key)
            if u and not u["n"]:
                w(f"   {label} {u['window'][0]:g}-{u['window'][1]:g} s: no frames"
                  + (f"; plate redraws {u['plate_renders']}" if u.get("plate_renders") is not None else ""))
            elif u:
                w(f"   {label} {u['window'][0]:g}-{u['window'][1]:g} s: {u['n']} frames ({u['fps']:.1f}/s); wall "
                  f"median {u['wall_median_ms']:.1f} ms ({u['steps_median'] / 1e6:.2f} M instr), CPU median "
                  f"{u['cpu_median_ms']:.1f} ms, p90 {u['cpu_p90_ms']:.1f} ms; audio ISR {u['isr_median'] / DEV_HZ * 1e3:.1f} ms, "
                  f"LCD wait {u['lcd_median'] / DEV_HZ * 1e3:.1f} ms a frame"
                  + (f"; plate redraws {u['plate_renders']}" if u.get("plate_renders") is not None else ""))
        for kind in ("midi", "key"):
            l_ = r.get(f"lat_{kind}")
            if l_ and l_["n"]:
                w(f"   latency {kind}: {l_['n']} notes, p50 {l_['p50_ms']:.1f} ms, p90 {l_['p90_ms']:.1f} ms, max "
                  f"{l_['max_ms']:.1f} ms (to the engine p90 {l_['engine_p90_ms']:.1f} ms)"
                  + (f", {l_['missed']} missed" if l_["missed"] else ""))
        if r.get("audible_db") is not None:
            w(f"   output RMS while busy {r['audible_db']:.1f} dBFS (the emulator's 16-bit capture)")
        if r.get("profile"):
            p = r["profile"]
            w("   most executed (whole run): " + ", ".join(f"{n} {100 * c / p['total']:.1f} %" for n, c in p["top"][:10]))
        if r.get("snaps"):
            w("   screens: " + ", ".join(r["snaps"]))
        w("")
    w("GATES")
    for g in res["gates"]:
        st = "PASS" if g["pass"] else "FAIL"
        w(f"  {st}  {g['id']:<20} {fmt(g['value']):>8} (limit {fmt(g['limit'])} {g['unit']})  {g['desc']}")
        if g["note"]:
            w(f"          {g['note']}")
    n_ok = sum(1 for g in res["gates"] if g["pass"])
    w("")
    w(f"emu_perf: {n_ok}/{len(res['gates'])} device-time gates pass. fm1-emu at 205 MHz, the real image: not a "
      f"measurement on an FM-1.")
    text = "\n".join(L) + "\n"
    with open(os.path.join(out, "report.txt"), "w") as f:
        f.write(text)
    return text


# ----------------------------------------------------------------------------------------------- commands ---
def load_build(app):
    b = os.path.join(app, "build")
    fw, dis = os.path.join(b, "cyma-1.fwsc"), os.path.join(b, "cyma-1.dis")
    if not (os.path.exists(fw) and os.path.exists(dis)):
        return None, None
    if os.path.getmtime(dis) + 120 < os.path.getmtime(fw):
        print("emu_perf: build/cyma-1.dis is older than the package: rebuild the device package")
        return None, None
    return fw, read_dis(dis)


def finish(out, app, fw, dis, bp, names, scen_defs, gopts, profile_on):
    results = {}
    for name in names:
        rd = os.path.join(out, name)
        lp = os.path.join(rd, "emu.log")
        if not os.path.exists(lp):
            results[name] = {"error": "no emu.log (the scenario did not run)"}
            continue
        with open(lp, errors="replace") as f:
            text = f.read()
        hits, dump, dual = parse_log(text)
        r = analyse(hits, dump, bp, scen_defs[name])
        if dual:
            r["error"] = "the second core started: instruction counts are no longer device time"
        r["ui_steps_gate"] = bool(scen_defs[name].get("ui_steps"))
        busy = scen_defs[name].get("busy")
        if busy:
            r["audible_db"] = wav_rms_db(os.path.join(rd, "out.wav"), busy[0] - 1.0, busy[1] - 1.0)
        r["snaps"] = snaps_to_png(rd, name, scen_defs[name], out)
        if profile_on:
            r["profile"] = profile(rd, dis)
        results[name] = r
    with open(fw, "rb") as f:
        raw = f.read()
    res = {"generated": datetime.datetime.now().isoformat(timespec="seconds"), "app": app,
           "package": {"file": os.path.relpath(fw, app), "sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw)},
           "clock_hz": DEV_HZ, "thresholds": gopts,
           "label": "fm1-emu at 205 instructions/us, the unmodified device image: not measured on an FM-1",
           "about": {n: scen_defs[n]["about"] for n in names}, "scenarios": results}
    res["gates"] = gates(results, gopts)
    res["pass"] = all(g["pass"] for g in res["gates"]) and bool(res["gates"])
    with open(os.path.join(out, "results.json"), "w") as f:
        json.dump(res, f, indent=1, default=str)
    print(write_report(out, res), end="")
    return 0 if res["pass"] else 1


def gate_opts(a):
    g = dict(DEFAULT_GATES)
    g.update({"render_max_pct": a.render_max_pct, "ui_cpu_ms": a.ui_cpu_ms, "ui_steps": a.ui_steps,
              "latency_p90_ms": a.latency_p90_ms})
    return g


def cmd_run(a):
    app = os.path.abspath(a.app)
    root = find_root(app)
    out = os.path.abspath(a.out or os.path.join(app, "build", "emu_perf"))
    names = [n.strip() for n in (a.only or ",".join(SCENARIOS)).split(",") if n.strip()]
    bad = [n for n in names if n not in SCENARIOS]
    if bad:
        print(f"emu_perf: no scenario {', '.join(bad)} (there are {', '.join(SCENARIOS)})")
        return 2
    if not root:
        print("emu_perf: no project root (reference/fm1-base) above the app: docker mounts the project")
        return 77
    case = app + "/"
    if not case.startswith(root + "/"):
        print(f"emu_perf: {app} is outside {root}")
        return 77
    fw, dis = load_build(app)
    if not fw:
        print("emu_perf: no device build (build/cyma-1.fwsc and .dis): tools/build_device.sh APP --no-ble")
        return 77
    if not docker_ok():
        print("emu_perf: no docker engine (colima start): the emulator runs in a linux/arm64 container")
        return 77
    emu = a.emu or os.environ.get("FM1_EMU_PERF") or build_emu(root)
    if not emu or not os.path.exists(emu):
        print("emu_perf: no patched fm1-emu (tools/emu_perf.py build-emu)")
        return 77
    bp, dbg = resolve(dis)
    os.makedirs(out, exist_ok=True)
    defs = {n: SCENARIOS[n]() for n in names}
    ctx = {"out": out, "fwsc": fw, "bp": bp, "dbg": dbg, "root": root, "emu": os.path.abspath(emu),
           "profile": a.profile}
    with open(os.path.join(out, "breakpoints.json"), "w") as f:
        json.dump({k: [f"0x{x:08x}" for x in v] for k, v in bp.items()} | {"fm1b_dbg": f"0x{dbg:08x}"}, f, indent=1)
    print(f"emu_perf: {len(names)} scenarios ({', '.join(names)}) in fm1-emu at {DEV_HZ / 1e6:.0f} MHz, {a.jobs} at a time"
          f" (about 18 s of wall time per emulated second)", flush=True)
    with cf.ThreadPoolExecutor(max_workers=max(1, a.jobs)) as ex:
        list(ex.map(lambda n: run_one(n, defs[n], ctx), names))
    return finish(out, app, fw, dis, bp, names, defs, gate_opts(a), a.profile)


def cmd_analyse(a):
    app = os.path.abspath(a.app)
    out = os.path.abspath(a.out or os.path.join(app, "build", "emu_perf"))
    fw, dis = load_build(app)
    if not fw:
        print("emu_perf: no device build")
        return 77
    bp, _ = resolve(dis)
    names = [n for n in SCENARIOS if os.path.exists(os.path.join(out, n, "emu.log"))]
    if a.only:
        names = [n for n in names if n in a.only.split(",")]
    if not names:
        print(f"emu_perf: no saved runs in {out}")
        return 77
    return finish(out, app, fw, dis, bp, names, {n: SCENARIOS[n]() for n in names}, gate_opts(a), a.profile)


def cmd_build_emu(a):
    root = find_root(os.path.abspath(a.app))
    if not root or not docker_ok():
        print("emu_perf: needs the project root and a docker engine")
        return 77
    b = build_emu(root, force=a.force)
    print(f"emu_perf: {b}" if b else "emu_perf: build failed")
    return 0 if b else 1


def synth_log(blocks, period, cost, frames=(), notes=(), plate=(), lcd=(), late=0, hz=DEV_HZ):
    """a fake fm1-emu log: ISR hits every `period` steps costing cost(i), ui frames [(start, end)], note-ons
    [(step, key, note)], plate hits, lcd waits; addresses as in SELFTEST_BP"""
    ev = []
    for i in range(blocks):
        s = 30_000_000 + i * period
        ev += [(s, 0x100, []), (s + cost(i), 0x104, [])]
    for a, b in frames:
        ev += [(a, 0x200, []), (b, 0x204, [])]
    for a, b in lcd:
        ev += [(a, 0x300, []), (b, 0x304, [])]
    for st, k, n in notes:
        ev.append((st, 0x400, [k & 0xFFFFFFFF, n, 100]))
    for st in plate:
        ev.append((st, 0x500, []))
    ev.sort()
    lines = [f"break c0 pc={pc:#010x} [{st}] sp=0x01c7bfe4 rets=0x0 irq_levels=[] r0-11=[" +
             ", ".join(f"{x:x}" for x in (regs + [0] * 12)[:12]) + "]" for st, pc, regs in ev]
    d = [DBG_MAGIC, blocks, 999, 0, 0, late, 0, len(frames), 0, 0, 1, 5, 0]
    lines += [f"dump 0x01c7c1a0 ({len(d)} words):", "  0x01c7c1a0: " + " ".join(f"{x:08x}" for x in d[:8]),
              "  0x01c7c1c0: " + " ".join(f"{x:08x}" for x in d[8:])]
    return "\n".join(lines) + "\n"


SELFTEST_BP = {"isr_in": [0x100], "isr_out": [0x104], "ui_in": [0x200], "ui_out": [0x204], "note": [0x400],
               "lcd_in": [0x300], "lcd_out": [0x304], "plate": [0x500]}


def cmd_selftest(_a):
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        print(f"  {'ok  ' if cond else 'FAIL'} {name}" + (f"  ({detail})" if detail else ""))
    per = round(128 / FS_DEV * DEV_HZ)                       # 594,766 steps: a 128-frame half at 205 MHz
    # 1. a steady 40 % load with one 90 % block: the peak, the period, the clock check
    t = synth_log(3000, per, lambda i: int(per * (0.9 if i == 2000 else 0.4)))
    h, d, _ = parse_log(t)
    r = analyse(h, d, SELFTEST_BP, {"inputs": []})
    check("period and frames per half", r["frames_per_half"] == 128 and abs(r["half_us"] - 2901.4) < 1, f"{r['half_us']:.1f} us")
    check("clock check passes at 205 MHz", r["clock_ok"])
    check("peak block found", abs(r["render_max_pct"] - 90) < 0.1 and r["blocks_over_85"] == 1, f"{r['render_max_pct']:.2f} %")
    check("mean load", abs(r["render_mean_pct"] - 40) < 0.1, f"{r['render_mean_pct']:.2f} %")
    check("xruns from fm1b_dbg", r["xruns"] == 0)
    g = gates({"s": r}, DEFAULT_GATES)
    check("render gate fails at 90 % > 85 %", any(x["id"] == "s_render" and not x["pass"] for x in g))
    # 2. the stock clock (240 MHz) is caught
    t2 = synth_log(400, round(128 / FS_DEV * EMU_STOCK_HZ), lambda i: 1000)
    h2, d2, _ = parse_log(t2)
    r2 = analyse(h2, d2, SELFTEST_BP, {"inputs": []})
    check("an unpatched emulator (240 MHz) is caught", not r2["clock_ok"])
    # 3. UI frames: ISR and LCD waits taken out of the wall time
    base = 30_000_000
    frames = [(base + 10 * per + 1000, base + 10 * per + 1000 + 3 * per)]   # covers ISRs 11, 12, 13 (each 0.4 per)
    lcd = [(base + 12 * per + 0.5 * per, base + 12 * per + 0.9 * per)]      # 0.4 per of wait, ISR 12 ends at 0.4: none inside
    lcd = [(int(a), int(b)) for a, b in lcd]
    t3 = synth_log(400, per, lambda i: int(per * 0.4), frames=frames, lcd=lcd)
    h3, d3, _ = parse_log(t3)
    r3 = analyse(h3, d3, SELFTEST_BP, {"inputs": [], "busy": (0.0, 1.0)})
    u = r3["ui_busy"]
    want_isr = 3 * int(per * 0.4)
    check("frame wall steps", u["steps_median"] == 3 * per, f"{u['steps_median']}")
    check("audio ISR inside a frame", u["isr_median"] == want_isr, f"{u['isr_median']} vs {want_isr}")
    check("LCD wait inside a frame", abs(u["lcd_median"] - int(0.4 * per)) <= 1, f"{u['lcd_median']}")
    check("frame CPU = wall - ISR - wait", u["cpu_median"] == 3 * per - want_isr - u["lcd_median"])
    # 4. latency: a MIDI note reaching the engine 1 ms after it is sent; a key that never does
    t_in = 1000.0                                                            # ms
    st_note = int((t_in + 1.0) / 1000 * DEV_HZ)
    t4 = synth_log(1000, per, lambda i: 1000, notes=[(st_note, -1, 60)], plate=[int(2.5 * DEV_HZ)])
    h4, d4, _ = parse_log(t4)
    scen = {"inputs": [{"t_ms": t_in, "kind": "midi", "note": 60}, {"t_ms": 1200, "kind": "key", "key": 7, "note": 60}],
            "idle": (2.0, 3.0)}
    r4 = analyse(h4, d4, SELFTEST_BP, scen)
    lm = r4["lat_midi"]
    nxt = 30_000_000 + ((st_note - 30_000_000) // per + 1) * per
    want = (nxt - t_in / 1000 * DEV_HZ + per) / DEV_HZ * 1e3
    check("MIDI latency = next block + a half", abs(lm["p50_ms"] - want) < 1e-6, f"{lm['p50_ms']:.3f} vs {want:.3f} ms")
    check("a key that never reached the engine is missed", r4["lat_key"]["missed"] == 1)
    check("a plate redraw in the idle window is counted", r4["ui_idle"]["plate_renders"] == 1)
    # 5. keys argument and the call-site parser
    k = keys_arg([(2000, "midi:903c64", None), (2250, "n7", 2400), (1700, "PRESET:+20", None), (3600, "REC", 4600)])
    check("keys argument", k == "1700:PRESET:+20,2000:midi:903c64,2250-2400:n7,3600-4600:REC", k)
    m = LINE_RE.match(" 2000498:    80 ea f2 72       \tcall 58852 <fm1_alnk0_irq : 200ea80 >")
    check("a call's return address", m and int(m.group(1), 16) + len(m.group(2).split()) == 0x200049C)
    print("selftest: " + ("all passed" if ok else "FAILED"))
    return 0 if ok else 1


def main():
    app = os.path.dirname(HERE)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", nargs="?", default="run", choices=["run", "analyse", "build-emu", "selftest"])
    ap.add_argument("--app", default=app)
    ap.add_argument("--out")
    ap.add_argument("--emu", help="a fm1-emu binary that honours FM1_HZ (default: built by build-emu)")
    ap.add_argument("--only", help="comma-separated scenarios (play, aether, held, idle)")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--profile", action="store_true", help="FM1_PCHIST: the most executed functions per scenario")
    ap.add_argument("--force", action="store_true", help="build-emu: rebuild even when up to date")
    ap.add_argument("--render-max-pct", type=float, default=DEFAULT_GATES["render_max_pct"])
    ap.add_argument("--ui-cpu-ms", type=float, default=DEFAULT_GATES["ui_cpu_ms"])
    ap.add_argument("--ui-steps", type=float, default=DEFAULT_GATES["ui_steps"])
    ap.add_argument("--latency-p90-ms", type=float, default=DEFAULT_GATES["latency_p90_ms"])
    a = ap.parse_args()
    return {"run": cmd_run, "analyse": cmd_analyse, "build-emu": cmd_build_emu, "selftest": cmd_selftest}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
