#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""CYMA-1 contact sheet (spec §8.2 screens): every PNG of a directory on one page, in name order, each labelled.

  tools/contact_sheet.py [DIR] [--out PNG] [--cols 6] [--scale 1] [--title TEXT]

DIR defaults to APP/build/golden/screens (tests/screens.txt's shots, named sNN_what.png; the sNN_ prefix orders them
and is dropped from the label). The sheet goes to DIR/../contact_sheet.png unless --out says otherwise. The tiles
keep the screen's pixels exact (nearest-neighbour when scaled): a review of the UI, not a thumbnail gallery.
Labels use Barlow Semi Condensed from fm1-base's fonts when it is there, else Pillow's default.
"""
import argparse
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def find_font(size, app):
    d = os.path.abspath(app)
    while True:
        c = os.path.join(d, "reference", "fm1-base", "assets", "fonts", "BarlowSemiCondensed-SemiBold.ttf")
        if os.path.exists(c):
            break
        if os.path.dirname(d) == d:
            c = None
            break
        d = os.path.dirname(d)
    if os.environ.get("FM1_BASE"):
        c2 = os.path.join(os.environ["FM1_BASE"], "assets", "fonts", "BarlowSemiCondensed-SemiBold.ttf")
        c = c2 if os.path.exists(c2) else c
    from PIL import ImageFont
    try:
        return ImageFont.truetype(c or os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "reference",
                                  "fm1-base", "assets", "fonts", "BarlowSemiCondensed-SemiBold.ttf"), size)
    except OSError:
        return ImageFont.load_default()


def label_of(path):
    n = os.path.splitext(os.path.basename(path))[0]
    n = re.sub(r"^s\d+_", "", n)
    return n.replace("_", " ")


def main():
    app = os.path.dirname(HERE)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir", nargs="?", default=os.path.join(app, "build", "golden", "screens"))
    ap.add_argument("--out")
    ap.add_argument("--cols", type=int, default=6)
    ap.add_argument("--scale", type=int, default=1)
    ap.add_argument("--title", default="CYMA-1 screens (host simulator, shotmode 1)")
    a = ap.parse_args()
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print("contact_sheet: needs Pillow")
        return 2
    files = sorted(glob.glob(os.path.join(a.dir, "*.png")))
    if not files:
        print(f"contact_sheet: no PNGs in {a.dir}")
        return 1
    out = a.out or os.path.join(os.path.dirname(os.path.abspath(a.dir)), "contact_sheet.png")
    tile = 240 * a.scale
    pad, lab, head = 12, 22, 44
    cols = max(1, min(a.cols, len(files)))
    rows = (len(files) + cols - 1) // cols
    W = pad + cols * (tile + pad)
    H = head + rows * (tile + lab + pad) + pad
    bg, ink, dim = (11, 12, 16), (239, 239, 247), (140, 146, 165)
    sheet = Image.new("RGB", (W, H), bg)
    d = ImageDraw.Draw(sheet)
    ft, fl = find_font(22, app), find_font(14, app)
    d.text((pad, 10), a.title, fill=ink, font=ft)
    d.text((W - pad - 360, 16), f"{len(files)} screens · algorithmic, host build", fill=dim, font=fl)
    blank = 0
    for i, p in enumerate(files):
        r, c = divmod(i, cols)
        x, y = pad + c * (tile + pad), head + r * (tile + lab + pad)
        try:
            im = Image.open(p).convert("RGB")
        except OSError:
            d.text((x, y), "unreadable", fill=dim, font=fl)
            continue
        if im.size != (240, 240):
            im = im.resize((240, 240))
        if len(im.getcolors(maxcolors=4) or [0] * 5) <= 1:
            blank += 1
        if a.scale != 1:
            im = im.resize((tile, tile), Image.NEAREST)
        sheet.paste(im, (x, y))
        d.rectangle((x - 1, y - 1, x + tile, y + tile), outline=(33, 40, 57))
        d.text((x, y + tile + 4), label_of(p), fill=dim, font=fl)
    sheet.save(out)
    print(f"contact_sheet: {len(files)} screens -> {out}" + (f" ({blank} of them a single flat colour)" if blank else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
