#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""CYMA-1 spec sheet (spec §8.3.9): build/spec/SPEC.json for the page's spec card, and docs/SPEC.md for people, from
the last golden run (tools/golden.py -> build/golden/results.json).

  tools/spec.py [--app DIR] [--results JSON] [--json OUT] [--md OUT]

SPEC.json carries the schema of spec §8.3, in its order, plus "demo_hash_host" (after determinism_host_eq_wasm): the
host build's demo_hash 1, which the page's determinism check compares the browser's against. A figure the golden run
could not measure (a module still a stub, a gate that failed to produce a number) is null, never a guess; the page
shows a dash for it. The constants (budgets, sample rates, voices and partials) are read from src/cy_config.h, so they
cannot drift from the code. Every figure is algorithmic, from the host build: none is measured on an FM-1, and both
files say so.

docs/SPEC.md also gets, when they exist:
  - the device package's identity, size and SHA-256, from build/SHA256SUMS (run_all.sh --device writes it), checked
    against the file itself: never a hand-copied hash;
  - the device-time figures from build/emu_perf/results.json (run_all.sh --perf: the package in fm1-emu at 205
    instructions/us), only when they were measured on that same package.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LABEL = "algorithmic, host build, not device-measured"
HEADLINE = "Algorithmic figures from the host build. Not yet measured on the FM-1."


def config(app):
    """CY_FS, CY_BUDGET_DEFAULT (host first, then device), CY_NVOICES, CY_NMODES from cy_config.h"""
    c = {"fs_host": 44100.0, "fs_device": 44117.647, "budget_host": 128, "budget_device": 96, "voices": 8, "modes": 32}
    try:
        with open(os.path.join(app, "src", "cy_config.h")) as f:
            t = f.read()
    except OSError:
        return c
    fs = re.findall(r"#define\s+CY_FS\s+([0-9.]+)f", t)
    bd = re.findall(r"#define\s+CY_BUDGET_DEFAULT\s+(\d+)", t)
    if len(fs) >= 2:
        c["fs_host"], c["fs_device"] = float(fs[0]), float(fs[1])
    if len(bd) >= 2:
        c["budget_host"], c["budget_device"] = int(bd[0]), int(bd[1])
    m = re.search(r"#define\s+CY_NVOICES\s+(\d+)", t)
    c["voices"] = int(m.group(1)) if m else c["voices"]
    m = re.search(r"#define\s+CY_NMODES\s+(\d+)", t)
    c["modes"] = int(m.group(1)) if m else c["modes"]
    return c


def num(v):
    return int(v) if isinstance(v, float) and v.is_integer() and abs(v) > 100 else v


def build_spec(app, res):
    c = config(app)
    s = (res or {}).get("spec", {})
    fsd = c["fs_device"]
    return {
        "engine": f"modal, {c['voices']} voices x up to {c['modes']} partials",
        "budget_host": c["budget_host"],
        "budget_device": c["budget_device"],
        "fs_host": int(c["fs_host"]) if float(c["fs_host"]).is_integer() else c["fs_host"],
        "fs_device": round(fsd, 3),
        "ultrasonic_db_max": s.get("ultrasonic_db_max"),
        "drive_alias_db": s.get("drive_alias_db"),
        "peak_dbfs_max": s.get("peak_dbfs_max"),
        "silence_exact_zero": s.get("silence_exact_zero"),
        "ratio_err_bessel_pct": s.get("ratio_err_bessel_pct"),
        "ratio_err_euler_pct": s.get("ratio_err_euler_pct"),
        "fdn_t60_err_pct": s.get("fdn_t60_err_pct"),
        "determinism_host_eq_wasm": s.get("determinism_host_eq_wasm"),
        "demo_hash_host": s.get("demo_hash_host"),
        "est_instr_max_host": num(s.get("est_instr_max_host")),
        "est_instr_max_device_cfg": num(s.get("est_instr_max_device_cfg")),
        "ui_frame_us_median": num(s.get("ui_frame_us_median")),
        "label": LABEL,
    }


ROWS = [  # key, what it is, the target the spec sets, the gates behind it
    ("engine", "Synthesis", "", []),
    ("budget_host", "Mode budget, host and browser (partials per render)", "", []),
    ("budget_device", "Mode budget, device default (governed by CPU load)", "", []),
    ("fs_host", "Sample rate, host and browser (Hz)", "", []),
    ("fs_device", "Sample rate, FM-1 I2S (Hz): 192 MHz / 17 / 256; tuning is built for it", "", []),
    ("ultrasonic_db_max", "Energy above 0.45 fs, worst preset at G5 + 2 octaves, drive off (dB re total; float output before the 24-bit quantiser)", "<= -100",
     ["ultrasonic"]),
    ("drive_alias_db", "The same with DRIVE (2x oversampled ADAA) on the drive presets (dB)", "<= -80",
     ["ultrasonic_drive"]),
    ("peak_dbfs_max", "Highest sample peak in any render (dBFS)", "<= -0.30", ["peak"]),
    ("silence_exact_zero", "60 s after playing, the output is exact digital zero", "true", ["silence_zero"]),
    ("ratio_err_bessel_pct", "Clamped circle (DRUM) as a membrane: worst mode-ratio error against Bessel (%)", "<= 3",
     ["ratio_bessel", "ratio_table"]),
    ("ratio_err_euler_pct", "BAR at plate stiffness: worst bending-mode error against Euler free-free (%)", "<= 1",
     ["ratio_euler"]),
    ("fdn_t60_err_pct", "Reverb (8-line FDN) decay time error at 2 s and 8 s (%)", "<= 10", ["fdn_t60"]),
    ("determinism_host_eq_wasm", "The browser build renders bit-identically to the host build", "true",
     ["determinism_host", "determinism_wasm"]),
    ("demo_hash_host", "demo_hash 1 of the host build (the page's determinism check compares the browser's to it)", "",
     ["determinism_host"]),
    ("est_instr_max_host", "Engine's own cost model, worst case, host config (instructions a sample)", "<= 3500",
     ["est_host"]),
    ("est_instr_max_device_cfg", "The same at the device's default budget (instructions a sample)", "<= 2900",
     ["est_device"]),
    ("ui_frame_us_median", "UI frame time, native host, median (us)", "<= 350", ["ui_median"]),
    ("label", "Provenance", "", []),
]


def show(v):
    if v is None:
        return "not measured"
    if v is True:
        return "yes"
    if v is False:
        return "no"
    return str(v)


def package_identity(raw):
    """the identity an FM-1 package carries ("FM-1_9400"): one marker byte after each of its first 20 blocks of 0x30
    (fm1-base's fm1_install.py product_of), or None"""
    if len(raw) < 20 * 0x30:
        return None
    return "".join(chr((raw[i * 0x30 + 0x2F] - i - 1) & 0xFF) for i in range(20) if raw[i * 0x30 + 0x2F] != 0x7D) or None


def package_info(app):
    """build/SHA256SUMS against the files themselves: [{file, bytes, sha256, listed, ok, identity}], or None"""
    b = os.path.join(app, "build")
    sums = os.path.join(b, "SHA256SUMS")
    if not os.path.exists(sums):
        return None
    rows = []
    with open(sums) as f:
        for line in f:
            m = re.match(r"^([0-9a-f]{64})\s+\*?(\S.*)$", line.strip())
            if not m:
                continue
            name = m.group(2).strip()
            p = os.path.join(b, name)
            r = {"file": name, "listed": m.group(1), "sha256": None, "bytes": None, "ok": False, "identity": None}
            if os.path.exists(p):
                with open(p, "rb") as g:
                    raw = g.read()
                r.update(sha256=hashlib.sha256(raw).hexdigest(), bytes=len(raw))
                r["ok"] = r["sha256"] == r["listed"]
                if name.endswith(".fwsc"):
                    r["identity"] = package_identity(raw)
            rows.append(r)
    return {"rows": rows, "when": datetime.datetime.fromtimestamp(os.path.getmtime(sums)).isoformat(timespec="seconds")}


def emu_perf_info(app, pkg):
    """build/emu_perf/results.json when it measured the package build/SHA256SUMS lists (else it is stale: None, why)"""
    p = os.path.join(app, "build", "emu_perf", "results.json")
    try:
        with open(p) as f:
            r = json.load(f)
    except (OSError, ValueError):
        return None, None
    fw = next((x for x in (pkg or {}).get("rows", []) if x["file"].endswith(".fwsc")), None)
    measured = (r.get("package") or {}).get("sha256")
    if fw is None or not fw["ok"] or measured != fw["sha256"]:
        return None, (f"build/emu_perf/results.json ({r.get('generated', '?')}) measured another package "
                      f"(sha256 {str(measured)[:12]}...): run `tools/run_all.sh --perf` again")
    return r, None


def md_device(w, app):
    pkg = package_info(app)
    perf, stale = emu_perf_info(app, pkg)
    if pkg:
        w("## Device package (built, never flashed)")
        w("")
        w(f"From `build/SHA256SUMS` ({pkg['when']}), each hash checked against the file itself. The build is "
          "reproducible: the same tree gives the same bytes in any folder. Nothing authenticates a `.fwsc`, so compare "
          "this hash with the file before any install (README, *Before you flash anything*).")
        w("")
        w("| File | Bytes | Identity | SHA-256 | Check |")
        w("|---|---|---|---|---|")
        for r in pkg["rows"]:
            chk = "matches the file" if r["ok"] else ("**the file is missing**" if r["sha256"] is None else
                                                      "**STALE: the file has changed since**")
            w(f"| `build/{r['file']}` | {r['bytes'] if r['bytes'] is not None else '-'} | {r['identity'] or ''} | "
              f"`{r['listed']}` | {chk} |")
        w("")
    if perf:
        w("## Device time, measured in the fm1-emu chip emulator")
        w("")
        w(f"**{perf.get('label', '')}.** `tools/emu_perf.py` (`run_all.sh --perf`, {perf.get('generated', '?')}) runs the "
          "package above, unmodified, in fm1-emu with its clock set to 205 MHz: the FM-1's measured 205 native "
          "instructions/us. Breakpoints on the firmware's own functions time each audio block, each screen frame and "
          "each note. fm1-emu has no cache model: on the FM-1, a cold flash path can only be slower.")
        w("")
        w("| Scenario | Slowest block (% of the 2.9 ms half) | Xruns | UI frame CPU, median (ms) | UI frame wall, median | "
          "Frames/s | Note to sound p90 (MIDI / key) | Idle plate redraws |")
        w("|---|---|---|---|---|---|---|---|")
        for name, r in perf.get("scenarios", {}).items():
            if r.get("error"):
                w(f"| {name} | {r['error']} | | | | | | |")
                continue
            u = r.get("ui_busy") or {}
            i = r.get("ui_idle") or {}
            lm, lk = r.get("lat_midi") or {}, r.get("lat_key") or {}
            f1 = lambda v, d=1: "-" if v is None else f"{v:.{d}f}"   # noqa: E731
            wall = (f"{f1(u.get('wall_median_ms'))} ms ({f1(u['steps_median'] / 1e6, 2)} M instr)"
                    if u.get("steps_median") is not None else "-")
            w(f"| {name} | {f1(r.get('render_max_pct'), 0)} | {f1(r.get('xruns'), 0) if r.get('xruns') is not None else '-'} | "
              f"{f1(u.get('cpu_median_ms'))} | {wall} | "
              f"{f1(u.get('fps') if u else i.get('fps'))} | {f1(lm.get('p90_ms'))} / {f1(lk.get('p90_ms'))} ms | "
              f"{i.get('plate_renders') if i.get('plate_renders') is not None else '-'} |")
        w("")
        gs = perf.get("gates", [])
        w(f"{sum(1 for g in gs if g['pass'])} of {len(gs)} device-time gates pass.")
        w("")
        w("| | Gate | Value | Limit | Note |")
        w("|---|---|---|---|---|")
        for g in gs:
            v = g["value"]
            note = (g.get("note") or "").replace("|", "/")
            w(f"| {'pass' if g['pass'] else '**FAIL**'} | {g['desc']} | {'' if v is None else v} | "
              f"{'' if g['limit'] is None else g['limit']} {g['unit']} | {note} |")
        w("")
        w("What each scenario plays: " + "; ".join(f"**{k}**: {v}" for k, v in perf.get("about", {}).items()) + ".")
        w("")
    elif stale:
        w("## Device time, measured in the fm1-emu chip emulator")
        w("")
        w(f"Not shown: {stale}")
        w("")


def build_md(spec, res, app=None):
    gates = {g["id"]: g for g in (res or {}).get("gates", [])}
    scen = (res or {}).get("scenarios", {})
    when = (res or {}).get("generated", "never")
    np_ = sum(1 for g in gates.values() if g["pass"])
    nf = sum(1 for g in gates.values() if g["pass"] is False and not g["advisory"])
    sp = sum(1 for k, r in scen.items() if r.get("pass") and not k.startswith(("ratio_", "demo_hash")))
    sn = sum(1 for k in scen if not k.startswith(("ratio_", "demo_hash")))
    L = []
    w = L.append
    w("# CYMA-1 spec sheet")
    w("")
    w(f"**{HEADLINE}**")
    w("")
    w("Every number below comes from running the firmware's own C code in the host simulator (and, for the")
    w("determinism row, the browser build in node). None of it has been measured on an M-VAVE FM-1: the device")
    w("package is built but has never been installed. The CPU figures are the engine's cost model, not a profile.")
    w("")
    if res:
        w(f"Generated by `tools/spec.py` from the golden run of {when}: {sp} of {sn} scenario scripts and "
          f"{np_} of {len(gates)} gates pass" + (f", {nf} hard gates fail." if nf else "."))
        src = res.get("app", "")
        rel = src.split("M-VAVE Synth Experiment/")[-1] if "M-VAVE Synth Experiment/" in src else src
        w(f"The run built and tested `{rel}`" + (" (a snapshot of the tree taken while the modules were still landing; "
          "the integrator's `tools/run_all.sh` on `instrument/` regenerates this page)." if rel != "instrument" else "."))
    else:
        w("No golden run found yet: run `instrument/tools/run_all.sh` (or `tools/golden.py`), then `tools/spec.py`.")
    w("")
    w("| Figure | Value | Target | Gate |")
    w("|---|---|---|---|")
    for key, what, target, gids in ROWS:
        st = []
        for gid in gids:
            g = gates.get(gid)
            if g is None:
                st.append("not run")
            else:
                st.append("pass" if g["pass"] else ("skipped" if g["pass"] is None else "FAIL"))
        stat = " / ".join(dict.fromkeys(st)) if st else ""
        w(f"| {what} | {show(spec.get(key))} | {target} | {stat} |")
    w("")
    if gates:
        w("## Every gate")
        w("")
        w("Spec sections refer to `docs/CYMA-1-spec.md`. A WARN is advisory (it guides preset voicing, it does not "
          "fail the build).")
        w("")
        w("| | Section | Gate | Value | Limit | Note |")
        w("|---|---|---|---|---|---|")
        for g in gates.values():
            st = "skip" if g["pass"] is None else ("pass" if g["pass"] else ("WARN" if g["advisory"] else "**FAIL**"))
            v = g["value"]
            v = f"{v:.3g}" if isinstance(v, float) else ("" if v is None else str(v))
            lim = g["limit"] if g["limit"] is not None else ""
            note = (g.get("note") or "").replace("|", "/")
            if len(note) > 160:
                note = note[:157] + "..."
            w(f"| {st} | {g['section']} | {g['desc']} | {v} | {lim} {g['unit']} | {note} |")
        w("")
    if scen:
        w("## Scenario scripts (`tests/*.txt`)")
        w("")
        w("| | Script | Expects passed | Failed | Simulated |")
        w("|---|---|---|---|---|")
        for k, r in scen.items():
            if k.startswith(("ratio_", "demo_hash")):
                continue
            w(f"| {'pass' if r.get('pass') else '**FAIL**'} | {k} | {r.get('oks', 0)} | {len(r.get('fails') or [])} | "
              f"{(r.get('sim_ms') or 0) / 1000:.0f} s |")
        w("")
    if app:
        md_device(w, app)
    w("## How these are made")
    w("")
    w("- `tools/golden.py` runs every scenario through `build/host/cyma-1_host`, then measures the renders with numpy")
    w("  and scipy: FFT peak ratios against Bessel and Euler, a Welch spectrum above 0.45 fs, sample peaks, a")
    w("  high-passed click detector, a Schroeder-curve T60 of `src/fx.c` driven by an impulse, and `demo_hash` on two")
    w("  host runs and in wasm (`tools/wasm_smoke.mjs`).")
    w("- `tools/spec.py` writes `build/spec/SPEC.json` (the page's spec card) and this file.")
    w("- `tools/run_all.sh` does all of it, from a clean build. With `--device` it also builds the package and writes")
    w("  `build/SHA256SUMS`; with `--perf`, it times that package in fm1-emu (`tools/emu_perf.py`).")
    w("")
    return "\n".join(L) + "\n"


def main():
    app = os.path.dirname(HERE)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--app", default=app)
    ap.add_argument("--results")
    ap.add_argument("--json")
    ap.add_argument("--md")
    a = ap.parse_args()
    app = os.path.abspath(a.app)
    rp = a.results or os.path.join(app, "build", "golden", "results.json")
    res = None
    try:
        with open(rp) as f:
            res = json.load(f)
    except (OSError, ValueError):
        print(f"spec: no golden results at {rp}: every measured figure is null")
    spec = build_spec(app, res)
    jp = a.json or os.path.join(app, "build", "spec", "SPEC.json")
    mp = a.md or os.path.join(app, "docs", "SPEC.md")
    os.makedirs(os.path.dirname(jp), exist_ok=True)
    os.makedirs(os.path.dirname(mp), exist_ok=True)
    with open(jp, "w") as f:
        json.dump(spec, f, separators=(",", ":"))
        f.write("\n")
    with open(mp, "w") as f:
        f.write(build_md(spec, res, app))
    nulls = [k for k, v in spec.items() if v is None]
    print(f"spec: {jp}" + (f" ({len(nulls)} figures not measured: {', '.join(nulls)})" if nulls else ""))
    print(f"spec: {mp}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
