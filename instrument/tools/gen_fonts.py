#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""CYMA-1's faces (spec §6.3) into one C header, in fm1-base's felucca_font_t format (tools/gen_font.py):

    gen_fonts.py OUT.h          (fm1.toml: gen = ["tools/gen_fonts.py {gen}/cy_fonts.h"])

  FONT_CY_LBL   Barlow Semi Condensed SemiBold 11 px, 32..95     caps labels (the UI adds +1 px tracking)
  FONT_CY_VAL   Barlow Semi Condensed Medium   15 px, 32..126    values, preset names
  FONT_CY_MID   Barlow Semi Condensed SemiBold 23 px, 32..95     enum readouts
  FONT_CY_HERO  Barlow Semi Condensed SemiBold 40 px, 32..58     " %+-.0123456789:*" only; the rest empty (bw 0)
  FONT_CY_TERM  Terminus ter-u12n.bdf          12 px, 32..126    dense data
  CY_LOGO       Barlow Semi Condensed Bold, a 1-bit 120 x 40 mask of "CYMA-1" (rows of 15 bytes, MSB first)
  CY_CREDIT     Barlow Semi Condensed Bold, a 1-bit 144 x 48 mask of the boot's credit, "TEAM EG &" over
                "LIZARD_MAN" (rows of 18 bytes, MSB first): the splash's first beat
  Each mask also gets CY_<NAME>_N, its lit pixels (the splash spreads the grains over them)

4-bit alpha, tabular digits (OpenType tnum), and substitutions drawn in place of ASCII, so the C strings stay ASCII:
'>' is U+203A (›) and '*' is U+00D7 (×) in every TTF face, and in FONT_CY_VAL '`' is U+00B7 (·, the preset readout's
"07 · TABLA SKIN"). The TTF path is gen_font.render's, plus those substitutions and an empty-glyph mask; the BDF path
is gen_font.render itself.

Each face also gets CY_BASE_<NAME>: its baseline's row inside the glyph box (asc - top for a TTF face, the font
bounding box's ascent for the BDF one), so the UI can set two faces on one baseline (a FONT_CY_HERO number with its
FONT_CY_VAL unit) without measuring glyphs at run time.

Pillow needs libraqm for `features`. Without it (Pillow raises "... not supported without libraqm") we warn and
render without tnum: digits are then proportional, and the layout still works. On macOS set
DYLD_LIBRARY_PATH=/opt/homebrew/lib so Pillow finds Homebrew's libraqm.

fm1-base is found at $FM1_BASE, else at reference/fm1-base in an ancestor directory, else at the project's path
(a private copy of instrument/ in the system's temp folder has no ancestor with it).
"""
import os
import sys
from pathlib import Path

PROJECT_BASE = Path(__file__).resolve().parents[2] / "reference" / "fm1-base"   # (the project root: instrument/'s parent)


def find_base():
    cands = [Path(os.environ["FM1_BASE"])] if os.environ.get("FM1_BASE") else []
    here = Path(__file__).resolve()
    cands += [p / "reference" / "fm1-base" for p in here.parents]
    cands.append(PROJECT_BASE)
    for c in cands:
        if (c / "tools" / "gen_font.py").exists():
            return c
    raise SystemExit("gen_fonts: fm1-base not found (set FM1_BASE)")


BASE = find_base()
sys.path.insert(0, str(BASE / "tools"))
import gen_font  # noqa: E402  (fm1-base: render(), font_path(), PAD)
from PIL import Image, ImageDraw, ImageFont  # noqa: E402

SUBST = {ord(">"): "›", ord("*"): "×"}
SUBST_FACE = {"CY_VAL": {ord("`"): "·"}}          # more, per face
HERO_SET = set(" %+-.0123456789:*")
# (name, face, px, first, last, keep): keep(ch) False -> an empty glyph (advance kept, no bitmap)
FACES = [
    ("CY_LBL", "BarlowSemiCondensed-SemiBold.ttf", 11, 32, 95, None),
    ("CY_VAL", "BarlowSemiCondensed-Medium.ttf", 15, 32, 126, None),
    ("CY_MID", "BarlowSemiCondensed-SemiBold.ttf", 23, 32, 95, None),
    ("CY_HERO", "BarlowSemiCondensed-SemiBold.ttf", 40, 32, 58, lambda ch: ch in HERO_SET),
    ("CY_TERM", "ter-u12n.bdf", 12, 32, 126, None),
]
RAQM_WARNED = []


def with_features(fn):
    """call fn(["tnum"]); without libraqm, warn once and call fn(None)"""
    if RAQM_WARNED:
        return fn(None)
    try:
        return fn(["tnum"])
    except (KeyError, OSError, ValueError) as e:
        if "libraqm" not in str(e):
            raise
        RAQM_WARNED.append(1)
        print("gen_fonts: warning: Pillow has no libraqm; rendering without tnum (proportional digits). "
              "Set DYLD_LIBRARY_PATH=/opt/homebrew/lib for tabular figures.", file=sys.stderr)
        return fn(None)


def render_ttf(face, px, first, last, subst):
    """gen_font.render's TTF path (same metrics and crop), with the substitutions; returns (h, base, [(adv, bw,
    alpha[])]), base = the baseline's row in the glyph box"""
    font = ImageFont.truetype(str(gen_font.font_path(face)), px)
    pad = gen_font.PAD

    def go(feats):
        asc, desc = font.getmetrics()
        top = max(0, font.getbbox("A8|(", features=feats)[1] - 1)
        h = asc + desc - top
        glyphs = []
        for c in range(first, last + 1):
            ch = subst.get(c, chr(c))
            adv = int(round(font.getlength(ch, features=feats)))
            bw = adv + 2 * pad
            img = Image.new("L", (bw, asc + desc), 0)
            ImageDraw.Draw(img).text((pad, 0), ch, font=font, fill=255, features=feats)
            img = img.crop((0, top, bw, top + h))
            glyphs.append((adv, bw, [min(15, (v + 8) // 17) for v in img.tobytes()]))
        return h, asc - top, glyphs
    return with_features(go)


def bdf_base(face):
    """a BDF face's baseline row in gen_font's cell: the bounding box's height plus its (negative) y offset"""
    for line in gen_font.font_path(face).read_text(errors="replace").splitlines():
        if line.startswith("FONTBOUNDINGBOX"):
            _, w, h, x, y = line.split()[:5]
            return int(h) + int(y)
    raise SystemExit(f"gen_fonts: {face} has no FONTBOUNDINGBOX")


def emit_font(lines, name, h, glyphs, first, last, keep):
    data, offs, bws = [], [], []
    for i, (adv, bw, g) in enumerate(glyphs):
        if keep and not keep(chr(first + i)):
            bw = 0                                   # empty: cv_text draws nothing, the pen still advances
        offs.append(len(data))
        bws.append(bw)
        for y in range(h if bw else 0):
            row = g[y * bw:(y + 1) * bw] + [0]
            for x in range(0, bw, 2):
                data.append((row[x] << 4) | row[x + 1])
    assert len(data) < 65536, f"{name}: {len(data)} B (gen_font's 64 KB limit)"
    lines.append(f"static const uint8_t FONT_{name}_DATA[{max(1, len(data))}] = {{")
    for i in range(0, len(data), 24):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 24]) + ",")
    if not data:
        lines.append("    0,")
    lines.append("};")
    lines.append(f"static const uint16_t FONT_{name}_OFF[{len(offs)}] = {{" + ", ".join(map(str, offs)) + "};")
    lines.append(f"static const uint8_t FONT_{name}_ADV[{len(glyphs)}] = {{" + ", ".join(str(a) for a, _, _ in glyphs) + "};")
    lines.append(f"static const uint8_t FONT_{name}_BW[{len(bws)}] = {{" + ", ".join(map(str, bws)) + "};")
    lines.append(f"static const felucca_font_t FONT_{name} = {{ {h}, {gen_font.PAD}, {first}, {last}, "
                 f"FONT_{name}_ADV, FONT_{name}_BW, FONT_{name}_OFF, FONT_{name}_DATA }};")
    lines.append("")
    return len(data)


def logo(lines, w=120, h=40):
    """"CYMA-1" in Barlow Semi Condensed Bold, as large as fits w x h, centred; 1 bit a pixel"""
    path = str(gen_font.font_path("BarlowSemiCondensed-Bold.ttf"))
    for px in range(60, 10, -1):
        font = ImageFont.truetype(path, px)
        x0, y0, x1, y1 = font.getbbox("CYMA-1")
        if x1 - x0 <= w - 2 and y1 - y0 <= h - 2:
            break
    img = Image.new("L", (w, h), 0)
    ImageDraw.Draw(img).text(((w - (x1 - x0)) // 2 - x0, (h - (y1 - y0)) // 2 - y0), "CYMA-1", font=font, fill=255)
    return emit_mask(lines, "CY_LOGO", img, px)


def emit_mask(lines, name, img, px):
    """a 1-bit mask (alpha >= 128 is lit), rows of (w + 7) / 8 bytes, MSB first; NAME_W, NAME_H and NAME_N (lit)"""
    w, h = img.size
    px_ = img.tobytes()
    bpr = (w + 7) // 8
    bits = bytearray(bpr * h)
    lit = 0
    for y in range(h):
        for x in range(w):
            if px_[y * w + x] >= 128:
                bits[y * bpr + x // 8] |= 0x80 >> (x & 7)
                lit += 1
    lines.append(f"#define {name}_W {w}")
    lines.append(f"#define {name}_H {h}")
    lines.append(f"#define {name}_N {lit}u   /* lit pixels */")
    lines.append(f"static const uint8_t {name}[{len(bits)}] = {{  /* Barlow SC Bold {px} px */")
    for i in range(0, len(bits), 24):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in bits[i:i + 24]) + ",")
    lines.append("};")
    return len(bits)


CREDIT = ("TEAM EG &", "LIZARD_MAN")   # the boot's credit: a real ampersand and a real underscore


def credit(lines, w=144, h=48, lead=7):
    """the credit's lines in Barlow Semi Condensed Bold, one size, as large as fits w x h with `lead` px between a
    line's baseline and the next one's cap height, each line centred; 1 bit a pixel. Barlow's underscore sits on the
    baseline, inside the cap height, so the block is (lines x cap height) + the leads"""
    path = str(gen_font.font_path("BarlowSemiCondensed-Bold.ttf"))
    for px in range(40, 10, -1):
        font = ImageFont.truetype(path, px)
        _, ct, _, cb = font.getbbox("H")
        boxes = [font.getbbox(s) for s in CREDIT]
        block = max(b[3] for b in boxes[-1:]) - ct + (cb - ct + lead) * (len(CREDIT) - 1)
        if max(b[2] - b[0] for b in boxes) <= w - 2 and block <= h - 2:
            break
    img = Image.new("L", (w, h), 0)
    y = (h - block) // 2 - ct
    for s, (x0, _, x1, _) in zip(CREDIT, boxes):
        ImageDraw.Draw(img).text(((w - (x1 - x0)) // 2 - x0, y), s, font=font, fill=255)
        y += cb - ct + lead
    return emit_mask(lines, "CY_CREDIT", img, px)


def main(out):
    lines = ["/* generated by instrument/tools/gen_fonts.py (CYMA-1, spec 6.3): felucca_font_t faces, gfx.c's format.",
             " * '>' draws as U+203A and '*' as U+00D7. Include after gfx.c. */", "#pragma once", "#include <stdint.h>", ""]
    total = 0
    for name, face, px, first, last, keep in FACES:
        if face.endswith(".bdf"):
            h, glyphs = gen_font.render(face, px, 1, True, first, last)
            base = bdf_base(face)
        else:
            h, base, glyphs = render_ttf(face, px, first, last, {**SUBST, **SUBST_FACE.get(name, {})})
            if name == "CY_HERO":                # digits and signs have no descenders: drop those rows (flash)
                h2 = min(h, base + 2)
                glyphs = [(a, bw, g[:h2 * bw]) for a, bw, g in glyphs]
                h = h2
        n = emit_font(lines, name, h, glyphs, first, last, keep)
        lines.insert(len(lines) - 1, f"#define CY_BASE_{name[3:]} {base}   /* FONT_{name}'s baseline row (h {h}) */")
        total += n
        print(f"font {name}: {face} {px}px h {h} base {base}, digit adv {glyphs[ord('0') - first][0]}, {n} B")
    total += logo(lines)
    total += credit(lines)
    print(f"cy fonts: {total} B" + (" (no tnum: no libraqm)" if RAQM_WARNED else ""))
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    Path(out).write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    main(sys.argv[1])
