#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""CYMA-1's browser build, packaged (spec §7.1, M6): dist/web/ as a folder of static files that works from any static
host, from file://, and inside a sandboxed iframe; and dist/artifact/, the same page in the shape a claude.ai Artifact
publishes.

    python3 instrument/tools/package_web.py [--app DIR] [--out DIR] [--no-inline-wasm] [--artifact DIR | --no-artifact]
                                            [--share-base URL] [--no-loudness] [--check]

It takes what `build.py APP --web` made (build/emu/index.html, worklet.js, app.wasm: the page is the app's own,
web/emu/index.html, with {{NAME}} filled in) and writes:

  dist/web/
    index.html   the page, with
                   - the worklet inlined as <script type="text/worklet" id="cy-worklet"> (the page loads it through a
                     Blob URL, so one file carries the audio code and no second request can be blocked);
                   - a base64 copy of app.wasm as <script type="application/octet-stream" id="cy-wasm">, which the
                     page uses only where fetching app.wasm cannot work: file://, or a sandboxed frame, whose opaque
                     origin makes the fetch cross-origin and a plain static host sends no CORS headers
                     (--no-inline-wasm leaves it out);
                   - build/spec/SPEC.json injected at /*CY_SPEC*/null, and the demo list at /*CY_DEMOS*/[];
                   - CYMA-1 Sounds (docs/CYMA-1-sounds-spec.md §7.4): tools/cyma1_params.json (the generated parameter
                     table and the factory sounds) at /*CY_PARAMS*/null, so the page decodes files and codes and lists
                     the factory sounds before the engine is on; and --share-base URL at /*CY_SHARE_BASE*/"", the base
                     the page's share links use (publish the artifact once, learn its URL, republish with it: an
                     artifact keeps its URL). Either placeholder may be left empty on purpose (no table file yet: the
                     page then reads the table from app.wasm after switch-on; no --share-base: links where the page
                     can see its own address, codes everywhere): it is then marked :none, never left raw;
    app.wasm     the firmware in WebAssembly;
    worklet.js   the worklet again, for a host whose content security policy refuses Blob URLs;
    demos/       the host-rendered demos: render_demos.sh's synced MP4 (the screen and the sound in one film) with its
                 PNG poster where it made one; else the MP3 (320 kbps; encoded here from the WAV when render_demos made
                 none) and the sand GIF when build/demos has one of reasonable size. Their loudness is matched here
                 (the media only, never the instrument's gain): each is measured (EBU R128, ffmpeg's ebur128) and one
                 more than 0.5 LU from LOUD_TARGET (-18 LUFS) gets its audio re-encoded with the gain that reaches it,
                 the picture copied untouched. The true peak stays at or under -1 dBTP: a limiter catches up to 4 dB of
                 peaks, and beyond that the demo stays a little quieter. Already-matched media (render_demos may match
                 them itself) are copied as they are. --no-loudness copies them unmeasured;
    SPEC.json    the figures, for whoever wants the numbers without the page.

  dist/artifact/ (with the default --out; --artifact DIR asks for it anywhere, --no-artifact never): the publishable
  folder.
    index.html   the platform wraps a published page in its own doctype, html, head and body, and reads the title
                 from the first 8 KB, so this one has none of those tags: it starts with <title>CYMA-1</title>, then
                 the page's <style> (the Google Fonts stylesheet as its first @import), then the body's content and
                 scripts. No inline wasm (the artifact's origin is real: app.wasm is fetched, once, as the page loads);
                 the worklet stays inline (a Blob URL), with worklet.js beside it as the fallback.
    app.wasm, worklet.js, SPEC.json, demos/   the supporting files, every one fetched by a relative URL.
  The script prints the Artifact tool's `files` map and the capabilities to publish with ({"downloads": true}: the
  page's Capture saves its .zip through the viewer's downloads capability, since a download link is inert there).

--check checks an existing dist/web (and dist/artifact when it exists) without building: among the rest, that the
CY_PARAMS and CY_SHARE_BASE placeholders were filled or deliberately left empty, and that the page's share-link code
(cy2) still carries the defaults frozen for it (CY2_DEF below) and claude.ai's 128-character anchor limit. A package
prints how many factory sounds get a link that fits. Every URL in the page is relative.
Only the standard library is needed (and ffmpeg: the MP3s, the loudness; or lame for the MP3s alone)."""
import argparse
import base64
import hashlib
import math
import struct
import zlib
import json
import os
import re
import shutil
import subprocess
import sys
import wave
from pathlib import Path

APP_DEFAULT = Path(__file__).resolve().parent.parent

# the eight demos of spec §8.4: a title and a line for the Listen cards (a demo not listed here gets its file name)
DEMO_TEXT = {
    "01_singing_bowl_lean": ("Singing bowl, leaning in", "A bronze bowl struck softly, then pressed: it bends sharp and starts to sing."),
    "02_handpan_groove": ("Handpan groove", "A steel shell tuned fundamental, octave and fifth, played as a groove in D minor."),
    "03_circle_to_star_morph": ("Circle into star", "One held note while SELECT melts the plate from a circle through a hexagon into a star."),
    "04_steel_rain_swells": ("Steel rain swells", "Rain on a steel hexagon: lean harder and it pours."),
    "05_tabla_tension_bends": ("Tabla tension bends", "A loaded skin tuned harmonic; pressure bends it up like a bayan heel."),
    "06_frozen_gong_drone": ("Frozen gong drone", "A cathedral gong caught by FREEZE and left to hang."),
    "07_surface_gong_harmonics": ("Surface gong harmonics", "SURFACE mode: the keybed is one gong; light touches on dark keys pull out harmonics."),
    "08_bowed_glass_chladni": ("Bowed glass Chladni", "Ernst Chladni's own demonstration: a glass square bowed until the sand snaps into its figure."),
}
GIF_MAX = 6 * 1024 * 1024
VIDEO_MAX = 15 * 1024 * 1024                # (an artifact's binary file limit, too)

# the claude.ai Artifact contract (what a published page may be and load)
ART_TITLE = "CYMA-1"
ART_TITLE_SCAN = 8192                       # the platform reads the title from the first 8 KB
ART_PAGE_MAX = 16 * 1024 * 1024             # the page, and each text file
ART_BINARY_MAX = 15 * 1024 * 1024           # each binary file
ART_FILES_MAX = 255
ART_HOSTS = ("https://fonts.googleapis.com/", "https://fonts.gstatic.com/", "https://cdnjs.cloudflare.com/",
             "https://cdn.jsdelivr.net/npm/")
ART_CAPABILITIES = {"downloads": True}
TEXT_EXT = {".html", ".js", ".json", ".css", ".txt", ".md", ".svg"}

# CYMA-1 Sounds (docs/CYMA-1-sounds-spec.md §3.6, §7.4): the placeholders and what may fill them
PARAMS_MARK, PARAMS_SET, PARAMS_NONE = "/*CY_PARAMS*/null", "/*CY_PARAMS:set*/", "/*CY_PARAMS:none*/null"
BASE_MARK, BASE_SET, BASE_NONE = '/*CY_SHARE_BASE*/""', "/*CY_SHARE_BASE:set*/", '/*CY_SHARE_BASE:none*/""'
SHARE_BASE_RE = re.compile(r"^https://[A-Za-z0-9./_~-]+$")
PARAM_FIELDS = ("id", "key", "name", "unit", "lo", "hi", "def", "curve", "detents", "int", "def_q16", "labels")
N_PARAMS_V1, N_FACTORY = 40, 24

# CYMA-1 Sounds: the share link's cy2 code (the page's §4a: claude.ai hands a page its top-level #anchor only up to 128
# characters, and a cy1 code is 126-140). A cy2 record stores only the values that differ from CY2_DEF, so CY2_DEF is
# FROZEN FOREVER: param_set 1's def_q16 as of 2026-10-24. This is a second copy on purpose: every package and --check
# compare it with the page's own literal (an edit to either is a problem), and the factory sounds' link lengths are
# printed against SHARE_LINK_MAX. A later parameter (41 onward) is appended to both, never anything else.
CY2_DEF = (8192, 0, 0, 39321, 52428, 0, 32768, 0, 32768, 13107, 65535, 36044, 18724, 39321, 26214, 26214, 19661, 40632,
           26214, 9830, 0, 32768, 19661, 42790, 32768, 19661, 39681, 0, 13107, 28672, 32768, 49151, 6554, 0, 32768, 0,
           65535, 0, 0, 54426)
SHARE_LINK_MAX = 128
CY2_DEF_RE = re.compile(r"const CY2_DEF = Object\.freeze\(\[([0-9,\s]+)\]\)")
LINK_MAX_RE = re.compile(r"const SHARE_LINK_MAX = (\d+);")

# the demos' loudness (the media only): integrated EBU R128 loudness, and the true-peak ceiling
LOUD_TARGET = -18.0                         # LUFS
LOUD_TOL = 0.5                              # LU: closer than this is left alone (idempotent)
LOUD_CEIL = -1.0                            # dBTP
LOUD_LIMIT_MAX = 4.0                        # dB of peaks a limiter may take; past that the gain is lowered instead


def die(msg):
    sys.exit(f"package_web: {msg}")


def cy2_encode(name, q16):
    """the reference cy2 writer, as the page's linkEncode: N (40), L, a 5-byte bitmap (bit i = parameter i differs from
    CY2_DEF, LSB first), the name, the differing values as uint16 LE, zlib's CRC-32 LE; "cy2." + base64url, no padding.
    The name must already be clean (the page's cleanName: the factory names are)"""
    nb = name.encode("ascii")
    n = N_PARAMS_V1
    bm = bytearray((n + 7) // 8)
    vals = []
    for i in range(n):
        if q16[i] != CY2_DEF[i]:
            bm[i >> 3] |= 1 << (i & 7)
            vals.append(q16[i])
    rec = bytes([n, len(nb)]) + bytes(bm) + nb + struct.pack(f"<{len(vals)}H", *vals)
    blob = rec + struct.pack("<I", zlib.crc32(rec) & 0xFFFFFFFF)
    return "cy2." + base64.urlsafe_b64encode(blob).decode("ascii").rstrip("=")


def cy2_report(table):
    """the factory sounds' cy2 link lengths against the 128-character anchor: (line to print, how many do not fit)"""
    if not table:
        return "cy2 links: no parameter table here, nothing to measure", 0
    lens = sorted(len(cy2_encode(f["name"], f["q16"])) for f in table["factory"])
    cy1 = sorted(4 + math.ceil(4 * (8 + len(f["name"]) + 2 * N_PARAMS_V1) / 3) for f in table["factory"])
    over = sum(1 for x in lens if x > SHARE_LINK_MAX)
    moved = [p["key"] for p, d in zip(table["params"], CY2_DEF) if p["def_q16"] != d]
    line = (f"cy2 links: {len(lens) - over} of {len(lens)} factory sounds fit the {SHARE_LINK_MAX}-character anchor "
            f"(cy2 {lens[0]}..{lens[-1]}, median {lens[len(lens) // 2]}; their cy1 codes are {cy1[0]}..{cy1[-1]})")
    if moved:
        line += (f"\n         note: the firmware's defaults moved from cy2's frozen ones ({', '.join(moved)}): cy2 never "
                 f"reads them, so links still decode exactly; they are only longer")
    return line, over


def check_cy2(html, problems, where):
    """the page's frozen cy2 defaults and its link limit are the ones this script freezes too"""
    m = CY2_DEF_RE.search(html)
    if not m:
        problems.append(f"no CY2_DEF table in {where}: the page cannot make or read share links (cy2)")
    elif tuple(int(x) for x in m.group(1).replace("\n", " ").split(",") if x.strip()) != CY2_DEF:
        problems.append(f"the CY2_DEF table in {where} is not the frozen one: cy2 links made before would decode wrong")
    m = LINK_MAX_RE.search(html)
    if not m or int(m.group(1)) != SHARE_LINK_MAX:
        problems.append(f"SHARE_LINK_MAX in {where} is {m.group(1) if m else 'missing'}, not {SHARE_LINK_MAX} (claude.ai's anchor limit)")


def check_line_ends(html, problems, where):
    """no raw U+2028 / U+2029 anywhere in the page: JavaScript reads them as line ends, so one inside a regular
    expression or string literal of the inline script stops the whole script from parsing (a page built during the
    hardening round did exactly that: 'Invalid regular expression: missing /'). Written as escapes they are fine"""
    for cp in (0x2028, 0x2029):
        n = html.count(chr(cp))
        if n:
            at = html.index(chr(cp))
            line = html.count("\n", 0, at) + 1
            problems.append(f"{n} raw U+{cp:04X} in {where} (first on line {line}): the inline script would not parse; "
                            f"write it as an escape")


def script_safe(text):
    """text inside <script>...</script>: no '</script' (nor '<!--') may close or confuse the element"""
    return re.sub(r"</(script)", r"<\\/\1", text, flags=re.I).replace("<!--", "<\\!--")


def wav_seconds(p):
    try:
        with wave.open(str(p), "rb") as w:
            return w.getnframes() / float(w.getframerate())
    except Exception:          # 24-bit WAVs open fine; anything odd just has no duration
        return None


def mp3_encode(src, dst, bitrate):
    """320 kbps CBR MP3: ffmpeg with libmp3lame, else lame itself"""
    if shutil.which("ffmpeg"):
        r = subprocess.run(["ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i", str(src), "-codec:a", "libmp3lame",
                            "-b:a", bitrate, "-ar", "44100", str(dst)], capture_output=True, text=True)
        if r.returncode == 0 and dst.exists():
            return True
        sys.stderr.write(r.stderr)
    if shutil.which("lame"):
        r = subprocess.run(["lame", "--silent", "-b", bitrate.rstrip("k"), "--cbr", str(src), str(dst)], capture_output=True, text=True)
        if r.returncode == 0 and dst.exists():
            return True
        sys.stderr.write(r.stderr)
    return False


def params_table(app):
    """tools/cyma1_params.json (FW's params_json output, §3.6 schema I1), checked; None when the file is not there"""
    p = app / "tools" / "cyma1_params.json"
    if not p.exists():
        return None
    try:
        t = json.loads(p.read_text())
    except json.JSONDecodeError as e:
        die(f"{p} is not JSON: {e}")
    bad = []
    if not isinstance(t, dict) or t.get("param_set") != 1:
        bad.append("param_set is not 1")
    params = t.get("params") if isinstance(t, dict) else None
    if not isinstance(params, list) or len(params) != N_PARAMS_V1:
        bad.append(f"params is not a list of {N_PARAMS_V1}")
    else:
        for i, q in enumerate(params):
            if not isinstance(q, dict) or any(f not in q for f in PARAM_FIELDS) or q.get("id") != i:
                bad.append(f"params[{i}] lacks a field of {PARAM_FIELDS} or its id is not {i}")
                break
            if q["curve"] not in ("LIN", "LOG", "ENUM", "MORPH"):
                bad.append(f"params[{i}].curve is {q['curve']!r}")
    fac = t.get("factory") if isinstance(t, dict) else None
    if not isinstance(fac, list) or len(fac) != N_FACTORY:
        bad.append(f"factory is not a list of {N_FACTORY}")
    else:
        for i, f in enumerate(fac):
            q16 = f.get("q16") if isinstance(f, dict) else None
            if not isinstance(q16, list) or len(q16) != N_PARAMS_V1 or not all(isinstance(v, int) and 0 <= v <= 65535 for v in q16):
                bad.append(f"factory[{i}].q16 is not {N_PARAMS_V1} values 0..65535")
                break
            if not isinstance(f.get("name"), str) or f.get("num") != i + 1:
                bad.append(f"factory[{i}] has no name or its num is not {i + 1}")
                break
    if bad:
        die(f"{p}: not the parameter table the page reads (§3.6): " + "; ".join(bad))
    return t


def loudness(path):
    """(integrated LUFS, true peak dBTP) of a media file's audio, by ffmpeg's ebur128; None when it cannot say"""
    if not shutil.which("ffmpeg"):
        return None
    r = subprocess.run(["ffmpeg", "-nostdin", "-hide_banner", "-i", str(path), "-map", "0:a:0", "-af", "ebur128=peak=true",
                        "-f", "null", "-"], capture_output=True, text=True)
    i = re.findall(r"^\s+I:\s+(-?[\d.]+|-inf) LUFS", r.stderr, re.M)
    tp = re.findall(r"^\s+Peak:\s+(-?[\d.]+|-inf) dBFS", r.stderr, re.M)
    if r.returncode != 0 or not i or not tp or "inf" in i[-1]:
        return None
    return float(i[-1]), float(tp[-1]) if "inf" not in tp[-1] else -120.0


def aac_encoder():
    r = subprocess.run(["ffmpeg", "-hide_banner", "-encoders"], capture_output=True, text=True)
    return "aac_at" if " aac_at " in r.stdout else "aac"


def match_loudness(src, dst, wav=None):
    """dst (a copy of src) at LOUD_TARGET when it is more than LOUD_TOL away: the audio re-encoded with the gain (a
    film's picture copied as it is; an MP3 encoded again from its WAV when there is one), a limiter at the ceiling when
    the gain would push the true peak past it. -> (loudness after, gain dB, limited) or None (left as it was)"""
    m = loudness(dst)
    if m is None:
        return None
    lufs, tp = m
    if abs(lufs - LOUD_TARGET) <= LOUD_TOL:
        return lufs, 0.0, False
    gain = LOUD_TARGET - lufs
    gain = min(gain, LOUD_CEIL - tp + LOUD_LIMIT_MAX)       # (at most LOUD_LIMIT_MAX dB for the limiter to catch)
    limited = tp + gain > LOUD_CEIL - 0.3
    af = f"volume={gain:.2f}dB"
    if limited:                                            # (sample peaks 0.5 dB under the ceiling: AAC's overshoot)
        af += f",alimiter=limit={10 ** ((LOUD_CEIL - 0.5) / 20):.4f}:attack=5:release=60:level=false"
    tmp = dst.with_name(dst.stem + ".loud" + dst.suffix)
    if dst.suffix == ".mp4":
        cmd = ["ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i", str(src), "-map", "0", "-map_metadata", "0",
               "-c:v", "copy", "-af", af, "-c:a", aac_encoder(), "-b:a", "256k", "-movflags", "+faststart", str(tmp)]
    else:
        cmd = ["ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i", str(wav if wav and wav.exists() else src),
               "-map_metadata", "0", "-af", af, "-codec:a", "libmp3lame", "-b:a", "320k", "-ar", "44100", str(tmp)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not tmp.exists():
        sys.stderr.write(r.stderr)
        tmp.unlink(missing_ok=True)
        return None
    after = loudness(tmp)
    if after is None:
        tmp.unlink(missing_ok=True)
        return None
    tmp.replace(dst)
    return after[0], gain, limited


def script_meta(app, stem):
    """the demo script's own "# TITLE:" and "# ABOUT:" lines (tests/demos/NAME.txt, M8's)"""
    meta, p = {}, app / "tests" / "demos" / f"{stem}.txt"
    if p.exists():
        for line in p.read_text(errors="replace").splitlines():
            if line.startswith("# TITLE:"):
                meta.setdefault("title", line[8:].strip())
            elif line.startswith("# ABOUT:"):
                meta.setdefault("about", line[8:].strip())
    return meta


def demos(app, out, bitrate, loud=True):
    """build/demos (tools/render_demos.sh: NAME.wav, NAME.mp4 and its NAME.png poster, NAME.mp3 at 320 kbps with its
    tags, NAME.gif, demos.json) -> out/demos and the page's list. A demo with an MP4 (not older than its WAV) ships the
    film and its poster only: it carries the sound and the screen in sync, so the MP3 and the GIF would be dead weight.
    Otherwise the MP3 render_demos made is used as it is when it is not older than its WAV, or the WAV is encoded
    here, with the GIF beside it. Titles: demos.json, then the script's TITLE / ABOUT lines, then DEMO_TEXT."""
    src = app / "build" / "demos"
    if not src.is_dir():
        print(f"package_web: no {src}: the Listen section will say there are no recordings")
        return []
    listing = {}
    if (src / "demos.json").exists():
        try:
            listing = {d.get("id"): d for d in json.loads((src / "demos.json").read_text()) if isinstance(d, dict)}
        except (json.JSONDecodeError, AttributeError):
            print(f"package_web: {src / 'demos.json'} is not a list of demos: ignored")
    dst = out / "demos"
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True)
    stems = sorted({p.stem for p in src.iterdir() if p.suffix in (".wav", ".mp3", ".mp4")})
    listed = []
    for stem in stems:
        wav, mp3, out_mp3 = src / f"{stem}.wav", src / f"{stem}.mp3", dst / f"{stem}.mp3"
        mp4, png = src / f"{stem}.mp4", src / f"{stem}.png"
        film = (mp4.exists() and mp4.stat().st_size <= VIDEO_MAX and
                (not wav.exists() or mp4.stat().st_mtime >= wav.stat().st_mtime))
        if mp4.exists() and mp4.stat().st_size > VIDEO_MAX:
            print(f"package_web: {mp4.name} is over {VIDEO_MAX >> 20} MB: the MP3 and GIF instead")
        if film:
            shutil.copy2(mp4, dst / mp4.name)
            entry = {"video": f"demos/{mp4.name}"}
            if png.exists():
                shutil.copy2(png, dst / png.name)
                entry["poster"] = f"demos/{png.name}"
        elif mp3.exists() and (not wav.exists() or mp3.stat().st_mtime >= wav.stat().st_mtime):
            shutil.copy2(mp3, out_mp3)
        elif not (wav.exists() and mp3_encode(wav, out_mp3, bitrate)):
            print(f"package_web: {stem}: no MP3 and none could be made (ffmpeg with libmp3lame, or lame): left out")
            continue
        if not film:
            entry = {"file": f"demos/{stem}.mp3"}
        media = dst / (mp4.name if film else f"{stem}.mp3")
        if loud:
            got = match_loudness(mp4 if film else media, media, wav)
            if got is None:
                print(f"package_web: {media.name}: loudness not measured (ffmpeg?): copied as it is")
            else:
                lufs, gain, limited = got
                entry["lufs"] = round(lufs, 1)
                if gain:
                    print(f"package_web: {media.name}: {lufs - 0:.1f} LUFS after {gain:+.1f} dB" + (" (peaks limited)" if limited else ""))
        info = dict(listing.get(stem) or {})
        meta = script_meta(app, stem)
        title, about = DEMO_TEXT.get(stem, (re.sub(r"^\d+_", "", stem).replace("_", " ").capitalize(), ""))
        entry["title"] = info.get("title") if info.get("title") not in (None, "", stem) else meta.get("title", title)
        entry["about"] = info.get("about") or meta.get("about", about)
        secs = info.get("seconds") or (wav_seconds(wav) if wav.exists() else None)
        if secs:
            entry["seconds"] = round(float(secs), 1)
        gif = src / f"{stem}.gif"
        if film:
            pass
        elif gif.exists() and gif.stat().st_size <= GIF_MAX:
            shutil.copy2(gif, dst / gif.name)
            entry["gif"] = f"demos/{gif.name}"
        elif gif.exists():
            print(f"package_web: {gif.name} is over {GIF_MAX >> 20} MB: left out")
        listed.append(entry)
    return listed


def demo_list(html):
    at = html.find("const CY_DEMOS = /*CY_DEMOS*/")
    if at < 0:
        return None
    listed, _ = json.JSONDecoder().raw_decode(html, at + len("const CY_DEMOS = /*CY_DEMOS*/"))
    return listed


def check_refs(html, out, problems, allowed_abs):
    """every src/href points at a file in the folder, or at an allowed absolute URL; the demo list's files exist"""
    for url in re.findall(r'(?:src|href)="([^"]+)"', html):
        if url.startswith(allowed_abs) or url.startswith(("data:", "#")):
            continue
        if re.match(r"^[a-z]+:", url) or url.startswith("/"):
            problems.append(f"absolute URL in the page: {url}")
        elif not (out / url).exists():
            problems.append(f"missing file the page references: {url}")
    listed = demo_list(html)
    if listed is None:
        problems.append("the demo list placeholder is gone")
    else:
        for d in listed:
            for k in ("file", "gif", "video", "poster"):
                if k in d and not (out / d[k]).exists():
                    problems.append(f"missing demo file {d[k]}")


def check_placeholders(html, problems, where):
    """the Sounds placeholders: filled (:set) or deliberately left empty (:none), never left raw"""
    for raw, done in ((PARAMS_MARK, (PARAMS_SET, PARAMS_NONE)), (BASE_MARK, (BASE_SET, BASE_NONE))):
        if raw in html:
            problems.append(f"{raw} left in {where}: the page was not packaged")
        elif not any(d in html for d in done):
            problems.append(f"{raw.split('*/')[0]}*/ is gone from {where}: the page cannot find its value")
    m = re.search(re.escape(BASE_SET) + r'"([^"]*)"', html)
    if m and not SHARE_BASE_RE.match(m.group(1)):
        problems.append(f"the share base in {where} is {m.group(1)!r}: it must match {SHARE_BASE_RE.pattern}")


def check(out):
    """what the page needs is there, and nothing in it points off the folder but the fonts"""
    html = (out / "index.html").read_text()
    problems = []
    for marker in ("<!--CY_WORKLET-->", "<!--CY_WASM-->", "{{NAME}}", "{{ID}}"):
        if marker in html:
            problems.append(f"{marker} left in index.html")
    check_placeholders(html, problems, "index.html")
    check_cy2(html, problems, "index.html")
    check_line_ends(html, problems, "index.html")
    if 'id="cy-worklet"' not in html:
        problems.append("the worklet is not inlined")
    check_refs(html, out, problems, ("https://fonts.googleapis.com", "https://fonts.gstatic.com", "https://github.com/"))
    for f in ("app.wasm", "worklet.js"):
        if not (out / f).exists():
            problems.append(f"missing {f}")
    return problems


def check_artifact(out):
    """the claude.ai Artifact contract: no document tags of its own, the bare title first and within the first 8 KB,
    the style next, nothing loaded from a host the CSP refuses, external links opening a new tab, the sizes and the
    file count within the limits, and everything the page fetches beside it"""
    html = (out / "index.html").read_text()
    problems = []
    tag = re.compile(r"<!doctype\b|<html\b|</html\s*>|<head\b|</head\s*>|<body\b|</body\s*>|<meta\b|<link\b", re.I)
    for m in tag.finditer(html):
        problems.append(f"document tag in the artifact page at byte {m.start()}: {html[m.start():m.start() + 24]!r}")
    first = re.match(r"\s*<title>([^<]*)</title>\s*<style>", html)
    if not first:
        problems.append("the artifact page does not start with <title>...</title> then <style>")
    elif first.group(1) != ART_TITLE:
        problems.append(f"the artifact's title is {first.group(1)!r}: it must be the name alone, {ART_TITLE!r}")
    elif first.end(1) > ART_TITLE_SCAN:
        problems.append("the artifact's <title> is past the first 8 KB")
    if len(re.findall(r"<title>", html)) != 1 or len(re.findall(r"<style>", html)) != 1:
        problems.append("the artifact page needs exactly one <title> and one <style>")
    for marker in ("<!--CY_WORKLET-->", "<!--CY_WASM-->", "{{NAME}}", "{{ID}}", "/*CY_SPEC*/null"):
        if marker in html:
            problems.append(f"{marker} left in the artifact page")
    check_placeholders(html, problems, "the artifact page")
    check_cy2(html, problems, "the artifact page")
    check_line_ends(html, problems, "the artifact page")
    if 'id="cy-worklet"' not in html:
        problems.append("the worklet is not inlined in the artifact page")
    if 'id="cy-wasm"' in html:
        problems.append("the artifact page carries the inline wasm (it is fetched there: app.wasm)")
    # external resources: only the hosts the artifact CSP allows (a stylesheet @import, a font, a script)
    for url in re.findall(r"""@import\s+url\(["']?([^"')]+)|url\(["']?(https?://[^"')]+)""", html):
        u = url[0] or url[1]
        if u.startswith("http") and not u.startswith(ART_HOSTS):
            problems.append(f"the artifact loads from a host its CSP refuses: {u}")
    for attr, url in re.findall(r'<(?:script|img|audio|video|source|iframe|embed|object|link)\b[^>]*?\b(src|href)="(https?:[^"]+)"', html):
        if not url.startswith(ART_HOSTS):
            problems.append(f"the artifact loads from a host its CSP refuses: {url}")
    for js in re.findall(r"""fetch\(\s*["'](https?:[^"']+)""", html):
        problems.append(f"the artifact fetches from another host: {js}")
    # an external link must open a new tab: inside the frame it would replace the instrument
    for a in re.findall(r'<a\b[^>]*\bhref="https?:[^"]*"[^>]*>', html):
        if 'target="_blank"' not in a:
            problems.append(f"external link without target=\"_blank\": {a[:80]}")
    check_refs(html, out, problems, ART_HOSTS + ("https://github.com/",))
    for f in ("app.wasm", "worklet.js"):
        if not (out / f).exists():
            problems.append(f"missing {f} beside the artifact page")
    files = [p for p in out.rglob("*") if p.is_file()]
    if len(files) > ART_FILES_MAX:
        problems.append(f"{len(files)} files: an artifact holds at most {ART_FILES_MAX}")
    for p in files:
        size = p.stat().st_size
        limit = ART_PAGE_MAX if p.suffix.lower() in TEXT_EXT else ART_BINARY_MAX
        if size > limit:
            problems.append(f"{p.relative_to(out)} is {size / 1048576:.1f} MB: over the artifact's {limit >> 20} MB")
    return problems


def artifact_page(html, sha):
    """the packaged page (worklet inlined, SPEC and demos injected, no inline wasm) in the Artifact's shape. Built from
    the parts, never by stripping tags with a pattern (a naive </?head> also eats <header>): the one <style>, the
    one <body>'s content, and the Google Fonts stylesheet the <head> links, as the style's first @import"""
    if html.count("<style>") != 1 or html.count("</style>") != 1:
        die("the page must have exactly one <style> for the artifact shape")
    if html.count("<body>") != 1 or html.count("</body>") != 1:
        die("the page must have exactly one <body> (without attributes) for the artifact shape")
    style = html[html.index("<style>") + len("<style>"):html.index("</style>")]
    body = html[html.index("<body>") + len("<body>"):html.index("</body>")]
    fonts = re.findall(r'<link rel="stylesheet" href="(https://fonts\.googleapis\.com/[^"]+)"', html)
    imports = "".join(f'@import url("{u}");\n' for u in fonts)
    note = (f"<!-- CYMA-1 (MPL-2.0): a playable Chladni plate, firmware for the M-VAVE FM-1 running in the browser. "
            f"app.wasm sha256 {sha[:16]} -->\n")
    return f"<title>{ART_TITLE}</title>\n<style>\n{imports}{style.lstrip(chr(10))}</style>\n{note}{body.strip()}\n"


def link_or_copy(src, dst):
    """a supporting file into the artifact folder: a hard link where the file system allows (the demos are ~15 MB)"""
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.unlink(missing_ok=True)
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def write_artifact(art, html_base, wasm, worklet, spec_path, web_out, sha):
    if art.exists():
        shutil.rmtree(art)
    art.mkdir(parents=True)
    page = artifact_page(html_base.replace("<!--CY_WASM-->", "", 1), sha)
    (art / "index.html").write_text(page)
    (art / "app.wasm").write_bytes(wasm)
    (art / "worklet.js").write_text(worklet)
    if spec_path.exists():
        link_or_copy(spec_path, art / "SPEC.json")
    if (web_out / "demos").is_dir():
        for p in sorted((web_out / "demos").iterdir()):
            if p.is_file():
                link_or_copy(p, art / "demos" / p.name)
    return page


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--app", type=Path, default=APP_DEFAULT, help="the app directory (default: this script's instrument/)")
    ap.add_argument("--out", type=Path, help="where to write (default: APP/dist/web)")
    ap.add_argument("--no-inline-wasm", action="store_true", help="leave the base64 wasm fallback out of index.html")
    ap.add_argument("--artifact", type=Path, help="where to write the claude.ai Artifact folder (default: APP/dist/artifact)")
    ap.add_argument("--no-artifact", action="store_true", help="do not write the Artifact folder")
    ap.add_argument("--bitrate", default="320k")
    ap.add_argument("--share-base", metavar="URL", help="the page's share-link base: the published artifact's URL "
                    "(https://claude.ai/artifact/ID); it must match " + SHARE_BASE_RE.pattern)
    ap.add_argument("--no-loudness", action="store_true", help="copy the demo media without matching their loudness")
    ap.add_argument("--check", action="store_true", help="only check an existing dist/web (and dist/artifact)")
    a = ap.parse_args()
    if a.share_base is not None and not SHARE_BASE_RE.match(a.share_base):
        die(f"--share-base {a.share_base!r}: it must match {SHARE_BASE_RE.pattern} (https, no query, no #)")
    app = a.app.resolve()
    out = (a.out or app / "dist" / "web").resolve()
    # the artifact folder comes with the default package (dist/web), or wherever --artifact says; a package written
    # elsewhere with --out (a test's variant, --no-inline-wasm) leaves it alone
    art = None if a.no_artifact else a.artifact.resolve() if a.artifact else None if a.out else (app / "dist" / "artifact").resolve()
    if a.check:
        problems = check(out)
        if art and (art / "index.html").exists():
            problems += [f"artifact: {p}" for p in check_artifact(art)]
        for p in problems:
            print("package_web: " + p)
        return 1 if problems else 0

    emu = app / "build" / "emu"
    for f in ("index.html", "worklet.js", "app.wasm"):
        if not (emu / f).exists():
            die(f"no {emu / f}: run `python3 reference/fm1-base/tools/build.py {app} --web` first")
    html = (emu / "index.html").read_text()
    if "<!--CY_WORKLET-->" not in html:
        die(f"{emu / 'index.html'} is not CYMA-1's page (no <!--CY_WORKLET--> marker): is web/emu/index.html in the app?")
    if "<!--CY_WASM-->" not in html:
        die(f"{emu / 'index.html'} has no <!--CY_WASM--> marker")
    worklet = (emu / "worklet.js").read_text()
    wasm = (emu / "app.wasm").read_bytes()

    out.mkdir(parents=True, exist_ok=True)
    for old in ("index.html", "app.wasm", "worklet.js", "SPEC.json"):
        (out / old).unlink(missing_ok=True)

    html = html.replace("<!--CY_WORKLET-->", f'<script type="text/worklet" id="cy-worklet">\n{script_safe(worklet)}\n</script>', 1)

    spec_path = app / "build" / "spec" / "SPEC.json"
    if spec_path.exists():
        try:
            spec = json.loads(spec_path.read_text())
        except json.JSONDecodeError as e:
            die(f"{spec_path} is not JSON: {e}")
        if "/*CY_SPEC*/null" not in html:
            die("no /*CY_SPEC*/null placeholder in the page")
        html = html.replace("/*CY_SPEC*/null", "/*CY_SPEC*/" + script_safe(json.dumps(spec, separators=(",", ":"))), 1)
        shutil.copy2(spec_path, out / "SPEC.json")
    else:
        print(f"package_web: no {spec_path} yet (tools/golden.py writes it): the spec card will say the figures come later")

    # CYMA-1 Sounds: the parameter table and the share-link base (§7.4)
    if PARAMS_MARK not in html:
        die(f"no {PARAMS_MARK} placeholder in the page")
    table = params_table(app)
    if table is None:
        print("package_web: no tools/cyma1_params.json (FW's params_json): the page reads the table from app.wasm after switch-on")
        html = html.replace(PARAMS_MARK, PARAMS_NONE, 1)
    else:
        html = html.replace(PARAMS_MARK, PARAMS_SET + script_safe(json.dumps(table, separators=(",", ":"))), 1)
    if BASE_MARK not in html:
        die(f"no {BASE_MARK} placeholder in the page")
    html = html.replace(BASE_MARK, (BASE_SET + json.dumps(a.share_base)) if a.share_base else BASE_NONE, 1)

    listed = demos(app, out, a.bitrate, loud=not a.no_loudness)
    if "/*CY_DEMOS*/[]" not in html:
        die("no /*CY_DEMOS*/[] placeholder in the page")
    html = html.replace("/*CY_DEMOS*/[]", "/*CY_DEMOS*/" + script_safe(json.dumps(listed, separators=(",", ":"))), 1)
    sha = hashlib.sha256(wasm).hexdigest()
    html_base = html                     # (the artifact is made from this: no inline wasm, no head of its own)

    if a.no_inline_wasm:
        html = html.replace("<!--CY_WASM-->", "", 1)
    else:
        b64 = base64.b64encode(wasm).decode("ascii")
        lines = "\n".join(b64[i:i + 4096] for i in range(0, len(b64), 4096))
        html = html.replace("<!--CY_WASM-->", f'<script type="application/octet-stream" id="cy-wasm">\n{lines}\n</script>', 1)
    build_meta = f'<meta name="cyma-build" content="app.wasm sha256 {sha[:16]}">'
    if '<meta charset="utf-8">' in html:
        html = html.replace('<meta charset="utf-8">', f'<meta charset="utf-8">\n{build_meta}', 1)
    else:
        html = html.replace("<head>", f"<head>\n{build_meta}", 1)
    (out / "index.html").write_text(html)
    (out / "app.wasm").write_bytes(wasm)
    (out / "worklet.js").write_text(worklet)

    problems = check(out)
    size = sum(p.stat().st_size for p in out.rglob("*") if p.is_file())
    print(f"web      {out}  (index.html {(out / 'index.html').stat().st_size // 1024} KB, app.wasm {len(wasm) // 1024} KB, "
          f"{len(listed)} demos, SPEC {'yes' if spec_path.exists() else 'not yet'}, params {'yes' if table else 'from app.wasm'}, "
          f"share base {a.share_base or 'none'}, {size / 1048576:.1f} MB in all)")
    print("         " + cy2_report(table)[0])
    louds = [d["lufs"] for d in listed if "lufs" in d]
    if louds:
        print(f"         demo loudness {min(louds):.1f} .. {max(louds):.1f} LUFS (target {LOUD_TARGET:.0f})")

    if art:
        page = write_artifact(art, html_base, wasm, worklet, spec_path, out, sha)
        problems += [f"artifact: {p}" for p in check_artifact(art)]
        files = {str(p.relative_to(art)): str(p) for p in sorted(art.rglob("*")) if p.is_file() and p.relative_to(art).as_posix() != "index.html"}
        asize = sum(p.stat().st_size for p in art.rglob("*") if p.is_file())
        print(f"artifact {art}  (index.html {len(page.encode()) // 1024} KB, {len(files)} supporting files, {asize / 1048576:.1f} MB in all)")
        (art.parent / "artifact.json").write_text(json.dumps({"file_path": str(art / "index.html"), "files": files,
                                                              "capabilities": ART_CAPABILITIES, "icon": "music"}, indent=1) + "\n")
        print(f"         publish: file_path {art / 'index.html'}, capabilities {json.dumps(ART_CAPABILITIES)}, "
              f"files: the map in {art.parent / 'artifact.json'}")

    for p in problems:
        print("package_web: " + p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
