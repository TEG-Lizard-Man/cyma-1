#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""CYMA-1 Sounds: the frozen test vectors (docs/CYMA-1-sounds-spec.md §12.2 I2/I3), their C header, and a frame
helper for the scenario scripts.

This is the reference codec of the spec (build-test/presets-design/cyma_sounds_ref.py), carried into the tree
unchanged in substance, so the vectors can be re-derived and checked from the app's own parameter table at any time.
tests/sounds/vectors.json is the frozen copy of the design's vectors: this tool never rewrites it unless told to.

    gen_vectors.py [check] [--quick] [--app DIR] [--params FILE]
        re-derive every vector from tools/cyma1_params.json (FW generates it from src/: §3.6) and compare them, key by
        key, with tests/sounds/vectors.json; run the reference's own checks (the exhaustive JSON round trip and writer
        digests unless --quick, the share code's robustness, the 100,000-frame update-safety fuzz); check that
        vectors.h is the header of vectors.json, and that every frame in tests/sounds_*.txt is well formed (a line
        after a "# corrupt" comment is meant to be bad and is skipped); the short link code cy2 (spec revision
        2026-10-24b: its vectors, its robustness, its frozen defaults); and that docs/SOUNDS.md and the README quote
        the vectors exactly and no longer use the retired flash wording. Exit 0 all ok, 1 a failure, 77 no table yet
    gen_vectors.py header [--app DIR]          write tests/sounds/vectors.h from vectors.json (and the table)
    gen_vectors.py write --out FILE [--force]  write the re-derived vectors to FILE (never the frozen file unless
                                               --force names it)
    gen_vectors.py frame CMD SEQ [PAYLOAD_HEX] one SysEx frame F0..F7 as hex (CMD and SEQ in hex)
    gen_vectors.py rec (factory:N | NAME=q16,q16,...)   a sound record as hex
    gen_vectors.py parse HEX                    a frame decoded (header, payload, CRC), for reading a log
    gen_vectors.py scripts                      write tests/sounds_sysex.txt and tests/sounds_safety.txt (T2, T3;
                                               the safety script ends with the hardening round's 240-frame flood)
    gen_vectors.py scen [--host BIN] [--out DIR]   run those two through the host simulator, as golden.py runs its
                                               scenarios (#@log notes honoured); exit 77 when there is no app_sysex yet

Python 3.8+, stdlib only.
"""
import argparse
import base64
import hashlib
import json
import math
import os
import random
import re
import struct
import sys
import unicodedata
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
APP_DEFAULT = os.path.dirname(os.path.dirname(HERE))
N_V1 = 40

PARAMS = FACTORY = DEF_Q16 = KEYS = ALIASES = None


def load_table(path):
    """the §3.6 table (I1): the parameters and the 24 factory sounds"""
    global PARAMS, FACTORY, DEF_Q16, KEYS, ALIASES
    with open(path) as f:
        table = json.load(f)
    PARAMS, FACTORY = table["params"], table["factory"]
    if len(PARAMS) != N_V1 or [p["id"] for p in PARAMS] != list(range(N_V1)):
        raise SystemExit(f"{path}: not 40 parameters in order (param_set 1 is append-only)")
    DEF_Q16 = [p["def_q16"] for p in PARAMS]
    KEYS = {p["key"]: p for p in PARAMS}
    ALIASES = {}
    for p in PARAMS:
        a = norm_key(p["name"])
        if a != p["key"]:
            ALIASES[a] = p["key"]
    return table


# ============================================================================================ names (spec §3.4)
NAME_MAX = 14
NAME_SET = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -.'&+/!?#():")


def clean_name(s):
    """The one name rule (JS and Python do the NFKD step; C drops bytes >= 0x80 instead)."""
    if isinstance(s, (bytes, bytearray)):
        s = bytes(s).decode("latin-1")
    s = unicodedata.normalize("NFKD", s)
    s = "".join(c for c in s if not 0x300 <= ord(c) <= 0x36F)
    out = []
    for c in s:
        if "a" <= c <= "z":
            c = chr(ord(c) - 32)
        elif c in "_\t\r\n":
            c = " "
        if c in NAME_SET:
            out.append(c)
    s = re.sub(" +", " ", "".join(out)).strip()
    s = s[:NAME_MAX].rstrip()
    return s or "UNTITLED"


def clean_name_c(raw):
    """what the device's cy_name_clean makes of raw bytes: the bytes >= 0x80 dropped, then the same rule"""
    return clean_name(bytes(c for c in raw if c < 0x80))


# ============================================================================== the parameter curves (spec §3.2)
def grid(p):
    if p["curve"] == "ENUM":
        return max(p["detents"] - 1, 1)
    if p["curve"] == "MORPH":
        return p["detents"] * int(p["hi"] - p["lo"] + 0.5)
    return p["detents"] or 1


Q16_NUDGE = 1e-6


def q16(n):
    n = 0.0 if not n > 0.0 else 1.0 if n > 1.0 else n
    return int(math.floor(n * 65535.0 + 0.5 + Q16_NUDGE))


def q16_risky(n):
    n = 0.0 if not n > 0.0 else 1.0 if n > 1.0 else n
    y = n * 65535.0 + 0.5 + Q16_NUDGE
    return abs(y - round(y)) < 1e-9


def value_of(p, n):
    lo, hi = p["lo"], p["hi"]
    if p["curve"] == "LOG":
        return lo * math.pow(hi / lo, n)
    v = lo + n * (hi - lo)
    if p["int"]:
        step = (hi - lo) / grid(p)
        v = lo + math.floor((v - lo) / step + 0.5) * step
    return v


def norm_of(p, v):
    lo, hi, D = p["lo"], p["hi"], grid(p)
    if p["curve"] == "LOG":
        if not v > lo:
            return 0.0
        n = math.log2(v / lo) / math.log2(hi / lo)
    else:
        n = (v - lo) / (hi - lo)
    n = 0.0 if not n > 0.0 else 1.0 if n > 1.0 else n
    if p["curve"] == "ENUM" or p["int"]:
        n = math.floor(n * D + 0.5) / D
    return n


def num_out(x):
    if x == math.floor(x) and abs(x) < 1e15:
        return int(x)
    return x


def json_value(p, q):
    """the file value of a stored q16 (spec §3.2 writer rule)"""
    n = q / 65535.0
    if p["curve"] == "ENUM":
        return p["labels"][int(math.floor(n * grid(p) + 0.5))]
    v = value_of(p, n)
    if p["int"]:
        return int(round(v))
    for d in range(0, 7):
        k = 10 ** d
        s = math.floor(abs(v) * k + 0.5) / k
        s = -s if v < 0 else s
        n2 = norm_of(p, s)
        if q16(n2) == q and (p["curve"] != "LOG" or not q16_risky(n2)):
            return num_out(s)
    return num_out(v)


def fixed6(x):
    if isinstance(x, str):
        return x
    t = "%.6f" % x
    t = t.rstrip("0").rstrip(".") if "." in t else t
    return "0" if t == "-0" else t


def writer_domain(p):
    return range(65536) if p["curve"] != "ENUM" and not p["int"] else [q16(k / grid(p)) for k in range(grid(p) + 1)]


def writer_digest(p):
    return hashlib.sha256("\n".join(fixed6(json_value(p, q)) for q in writer_domain(p)).encode()).hexdigest()


def norm_key(k):
    return re.sub(r"_+", "_", re.sub(r"[^a-z0-9]", "_", str(k).lower())).strip("_")


def label_key(s):
    return re.sub(r"[^A-Z0-9]", "", str(s).upper())


def from_json_value(p, v):
    """-> (q16, note) where note is None, 'clamped' or 'default' (spec §3.3 reader rule 3)"""
    if p["curve"] in ("ENUM", "MORPH") and isinstance(v, str) and p["labels"]:
        labs = [label_key(s) for s in p["labels"]]
        if label_key(v) in labs:
            k = labs.index(label_key(v))
            return (q16(k / grid(p)) if p["curve"] == "ENUM" else q16(norm_of(p, p["lo"] + k))), None
        return DEF_Q16[p["id"]], "default"
    if isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v):
        return DEF_Q16[p["id"]], "default"
    if p["curve"] == "ENUM":
        k = min(max(int(round(v)), 0), grid(p))
        return q16(k / grid(p)), (None if k == v else "clamped")
    c = min(max(float(v), p["lo"]), p["hi"])
    return q16(norm_of(p, c)), (None if c == v else "clamped")


def sound_params_from_json(params):
    q = list(DEF_Q16)
    seen, skipped, notes = set(), [], []
    exact = {k: v for k, v in params.items() if k in KEYS}
    for k, v in params.items():
        if k in KEYS:
            continue
        nk = norm_key(k)
        nk = ALIASES.get(nk, nk)
        if nk in KEYS and nk not in exact and nk not in seen:
            exact[nk] = v
            seen.add(nk)
        elif nk not in KEYS:
            skipped.append(k)
    for key, v in exact.items():
        p = KEYS[key]
        q[p["id"]], note = from_json_value(p, v)
        if note:
            notes.append((key, note))
    missing = [p["key"] for p in PARAMS if p["key"] not in exact]
    return q, skipped, missing, notes


def sound_json(name, q, author=None, created=None, where="web"):
    d = {"format": "cyma1-sound", "version": 1, "name": clean_name(name)}
    if author:
        d["author"] = author
    d["created"] = created or "2026-10-24T18:20:00Z"
    d["made_with"] = {"app": "CYMA-1", "version": "DEV", "where": where}
    d["param_set"] = 1
    d["params"] = {p["key"]: json_value(p, q[p["id"]]) for p in PARAMS}
    return d


# ======================================================================== the sound record (spec §3.7)
LAYOUT, PARAM_SET = 0x01, 0x01
REC_NAME_MAX, REC_N_MAX = 32, 64


def rec_encode(name, q):
    nb = clean_name(name).encode("ascii")
    assert 1 <= len(q) <= REC_N_MAX and all(0 <= v <= 0xFFFF for v in q)
    return bytes([LAYOUT, PARAM_SET, len(q), len(nb)]) + nb + struct.pack("<%dH" % len(q), *q)


class FormatError(ValueError):
    def __init__(self, code):
        super().__init__(code)
        self.code = code


def rec_decode(b):
    if len(b) < 4:
        raise FormatError("damaged")
    if b[0] != LAYOUT or b[1] != PARAM_SET:
        raise FormatError("newer")
    n, L = b[2], b[3]
    if not 1 <= n <= REC_N_MAX or L > REC_NAME_MAX or len(b) != 4 + L + 2 * n:
        raise FormatError("damaged")
    name = clean_name(bytes(c for c in b[4:4 + L] if c < 0x80))
    v = struct.unpack_from("<%dH" % n, b, 4 + L)
    q = list(DEF_Q16)
    q[:min(n, N_V1)] = v[:N_V1]
    return name, q, max(0, n - N_V1), max(0, N_V1 - n)


# =================================================================================== share code (spec §4)
CODE_PREFIX = "cy1."
TOKEN_RE = re.compile(r"[A-Za-z0-9_-]*")


def crc32(b):
    return zlib.crc32(bytes(b)) & 0xFFFFFFFF


def code_encode(name, q):
    rec = rec_encode(name, q)
    blob = rec + struct.pack("<I", crc32(rec))
    return CODE_PREFIX + base64.urlsafe_b64encode(blob).decode().rstrip("=")


def code_decode(text):
    if len(text) > 4096:
        raise FormatError("notcode")
    s = "".join(text.split())
    i = s.find(CODE_PREFIX)
    if i < 0:
        raise FormatError("notcode")
    tok = TOKEN_RE.match(s, i + 4).group(0)
    if not 12 <= len(tok) <= 240 or len(tok) % 4 == 1:
        raise FormatError("damaged")
    blob = base64.urlsafe_b64decode(tok + "=" * (-len(tok) % 4))
    rec, crc = blob[:-4], blob[-4:]
    if len(blob) < 9 or struct.unpack("<I", crc)[0] != crc32(rec):
        raise FormatError("damaged")
    return rec_decode(rec)


# ===================================================== the short link code cy2 (spec revision 2026-10-24b, §4a)
# claude.ai hands the page a top-level #anchor only when it is at most 128 characters (measured 2026-10-24 in the real
# frame: 64, 100 and 128 arrived; 145 never did; 129-144 were not tried), and a cy1 code is 126-140. cy2 keeps only the
# values that differ from CY2_DEF:
#   body  = N (1..64; v1 writes 40) | L (0..32; v1 writes <= 14, cleaned) | bitmap, ceil(N/8) bytes, bit i (byte i>>3,
#           bit i&7) set = parameter i stored, bits at N and above 0 | name, L bytes | one u16 LE per set bit, in order
#   code2 = "cy2." + base64url_nopad(body || CRC32_LE(body))
# CY2_DEF is FROZEN with cy2: param_set 1's def_q16 as of 2026-10-24. It never changes, whatever params.c's defaults
# become (a clear bit means "this value"). Written here from that date's table, never read from the current one.
CODE2_PREFIX = "cy2."
ANCHOR_MAX = 128                                     # the longest #anchor claude.ai passes in (the text after '#')
CY2_DEF = (8192, 0, 0, 39321, 52428, 0, 32768, 0, 32768, 13107, 65535, 36044, 18724, 39321, 26214, 26214, 19661, 40632,
           26214, 9830, 0, 32768, 19661, 42790, 32768, 19661, 39681, 0, 13107, 28672, 32768, 49151, 6554, 0, 32768, 0,
           65535, 0, 0, 54426)
CODE_AT_RE = re.compile(r"cy1\.|cy2\.(?=[A-Za-z0-9_-]{10})")   # the first code: cy1., or cy2. with >= 10 characters
CODE_LATER_RE = re.compile(r"cy[3-9]\.[A-Za-z0-9_-]{10}")       # a later code version: "newer"


def b64url(b):
    return base64.urlsafe_b64encode(bytes(b)).decode().rstrip("=")


def rec2_encode(name, q, n=None):
    """the cy2 body: N, L, the bitmap of the values that differ from CY2_DEF, the name, those values"""
    nb = clean_name(name).encode("ascii")
    n = len(q) if n is None else n
    assert 1 <= n <= REC_N_MAX and len(q) == n and all(0 <= v <= 0xFFFF for v in q)
    bm, vals = bytearray((n + 7) // 8), []
    for i, v in enumerate(q):
        if i >= len(CY2_DEF) or v != CY2_DEF[i]:
            bm[i >> 3] |= 1 << (i & 7)
            vals.append(v)
    return bytes([n, len(nb)]) + bytes(bm) + nb + struct.pack("<%dH" % len(vals), *vals)


def code2_encode(name, q):
    body = rec2_encode(name, q)
    return CODE2_PREFIX + b64url(body + struct.pack("<I", crc32(body)))


def code2_len(name, q):
    """4 + ceil(4 (2 + B + L + 2k + 4) / 3), k the values that differ from CY2_DEF"""
    k = sum(1 for i, v in enumerate(q) if i >= len(CY2_DEF) or v != CY2_DEF[i])
    return 4 + -(-4 * (2 + (len(q) + 7) // 8 + len(clean_name(name)) + 2 * k + 4) // 3)


def rec2_decode(b):
    if len(b) < 2:
        raise FormatError("damaged")
    n, L = b[0], b[1]
    B = (n + 7) // 8
    if not 1 <= n <= REC_N_MAX or L > REC_NAME_MAX or len(b) < 2 + B + L:
        raise FormatError("damaged")
    bits = [i for i in range(8 * B) if b[2 + (i >> 3)] >> (i & 7) & 1]
    if any(i >= n for i in bits) or len(b) != 2 + B + L + 2 * len(bits):
        raise FormatError("damaged")
    name = clean_name(bytes(c for c in b[2 + B:2 + B + L] if c < 0x80))
    vals = struct.unpack_from("<%dH" % len(bits), b, 2 + B + L)
    q, extra = list(CY2_DEF), 0
    for i, v in zip(bits, vals):
        if i < N_V1:
            q[i] = v
        else:
            extra += 1
    return name, q, extra, max(0, N_V1 - n)


def strip_for_code(text):
    """what a decoder removes before it looks for a code: whitespace (chat apps wrap long lines) and, as of revision
    2026-10-24b, Unicode format characters (Cf: zero-width spaces and joiners, the soft hyphen, the BOM, bidi marks),
    which some chat apps put into long words and no code contains"""
    return "".join(c for c in text if not c.isspace() and unicodedata.category(c) != "Cf")


def any_decode(text):
    """spec §4's decoder with cy2 beside cy1 (revision 2026-10-24b): a bare code, '#cy..', a whole link, or text
    holding one -> (name, q16, extra, missing, kind). FormatError: notcode / damaged / newer"""
    if len(text) > 4096:
        raise FormatError("notcode")
    s = strip_for_code(text)
    m = CODE_AT_RE.search(s)
    if not m:
        raise FormatError("newer" if CODE_LATER_RE.search(s) else "notcode")
    two = m.group(0) == CODE2_PREFIX
    tok = TOKEN_RE.match(s, m.start() + 4).group(0)
    if (len(tok) < 12 and not two) or len(tok) > 240 or len(tok) % 4 == 1:
        raise FormatError("damaged")
    blob = base64.urlsafe_b64decode(tok + "=" * (-len(tok) % 4))
    if len(blob) < (7 if two else 9) or (two and b64url(blob) != tok):   # cy2: only the canonical text
        raise FormatError("damaged")
    rec, crc = blob[:-4], blob[-4:]
    if struct.unpack("<I", crc)[0] != crc32(rec):
        raise FormatError("damaged")
    return (rec2_decode(rec) if two else rec_decode(rec)) + ("cy2" if two else "cy1",)


def cy2_custom(name, body):
    """a cy2 code around any body (for the refusal vectors): the CRC made right, so only the body is wrong"""
    return CODE2_PREFIX + b64url(body + struct.pack("<I", crc32(body)))


def build_cy2():
    """the cy2 vectors (revision 2026-10-24b): every factory sound's link code, its length and whether it fits the
    128-character anchor; the edges; what an older and a newer writer's N gives; the refusals"""
    fac = []
    for f in FACTORY:
        c2 = code2_encode(f["name"], f["q16"])
        k = sum(1 for i, v in enumerate(f["q16"]) if v != CY2_DEF[i])
        fac.append({"num": f["num"], "name": f["name"], "code": c2, "length": len(c2), "stored": k,
                    "anchor_fits": len(c2) <= ANCHOR_MAX, "cy1_length": len(code_encode(f["name"], f["q16"])),
                    "cy1_anchor_fits": len(code_encode(f["name"], f["q16"])) <= ANCHOR_MAX})
    bowl = FACTORY[0]["q16"]

    def moved(p):
        """a value other than CY2_DEF's that a sound file carries exactly and that stays away from the default (so an
        edge keeps its length through a file and through the engine, which may move a value by one q16 step): the
        next grid point for a label or a whole number, else a quarter of the range away"""
        d = CY2_DEF[p["id"]]
        if p["curve"] == "ENUM" or p["int"]:
            dom = list(writer_domain(p))
            return next(v for v in dom[dom.index(d) + 1:] + dom if v != d) if d in dom else dom[0]
        return d + 0x4000 if d < 0xC000 else d - 0x4000

    changed = [moved(p) for p in PARAMS]
    at = lambda k: [changed[i] if i < k else CY2_DEF[i] for i in range(N_V1)]   # the first k values changed
    assert all(from_json_value(p, json.loads(json.dumps(json_value(p, changed[p["id"]]))))[0] == changed[p["id"]] !=
               CY2_DEF[p["id"]] for p in PARAMS), "an edge value a file cannot carry"
    edges = {
        "all_defaults": {"name": "", "q16": list(CY2_DEF)},
        "every_value_changed": {"name": "CATHEDRAL GONG", "q16": changed},
        "longest_that_fits": {"name": "CATHEDRAL GONG", "q16": at(34)},
        "shortest_that_does_not": {"name": "CATHEDRAL GONG", "q16": at(35)},
    }
    for e in edges.values():
        e["code"] = code2_encode(e["name"], e["q16"])
        e["length"] = len(e["code"])
        e["anchor_fits"] = e["length"] <= ANCHOR_MAX
        e["decoded_name"] = clean_name(e["name"])
    body40 = rec2_encode("TEST", bowl)
    b38 = rec2_encode("TEST", bowl[:38], n=38)
    b42 = rec2_encode("TEST", bowl + [1, 2], n=42)
    good = code2_encode(FACTORY[0]["name"], bowl)
    tok = good[4:]
    A64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
    stray = (8 * len(base64.urlsafe_b64decode(tok + "=" * (-len(tok) % 4)))) % 6   # data bits in the last character
    noncanon = tok[:-1] + A64[A64.index(tok[-1]) | 1] if stray in (2, 4) else None   # the same bytes, a stray bit set
    assert noncanon and noncanon != tok and base64.urlsafe_b64decode(noncanon + "=" * (-len(noncanon) % 4)) == \
        base64.urlsafe_b64decode(tok + "=" * (-len(tok) % 4))
    q39 = list(bowl)
    q39[39] = 1                                           # stored: bit 39 set
    n39 = bytearray(rec2_encode("TEST", q39))
    n39[0] = 39                                           # N 39 with bit 39 set: a stored value at N
    bad = {
        "notcode": ["cy2.AQEo", "cy2.", "cy2", "cy2.AAAAAAAA!"],
        "damaged": [good[:30], good[:-1], good[:-2] + ("A" if good[-2] != "A" else "B") + good[-1],
                    CODE2_PREFIX + noncanon,                                                    # not canonical
                    cy2_custom("x", bytes([0, 4]) + b"TEST"),                                   # N 0
                    cy2_custom("x", bytes([65, 4]) + bytes(9) + b"TEST"),                       # N 65
                    cy2_custom("x", bytes([40, 33]) + bytes(5) + b"A" * 33),                    # L 33
                    cy2_custom("x", bytes(n39)),                                                # a set bit at N
                    cy2_custom("x", body40 + b"\x00\x00"),                                      # 2 bytes too many
                    cy2_custom("x", body40[:-1]),                                               # 1 byte too few
                    CODE2_PREFIX + "A" * 241],
        "newer": ["cy3.AAAAAAAAAAAAAAAA", "Listen: https://x/#cy4.AAAAAAAAAAAAAAAA"],
    }
    return {
        "spec": "docs/CYMA-1-sounds-spec.md, revision 2026-10-24b (the short link code, §4a)",
        "anchor_max": ANCHOR_MAX,
        "def_q16": list(CY2_DEF),
        "factory": fac,
        "edges": edges,
        "tolerated": {"n38": {"code": cy2_custom("x", b38), "q16": bowl[:38] + list(CY2_DEF[38:]), "missing": 2, "extra": 0},
                      "n42": {"code": cy2_custom("x", b42), "q16": list(bowl), "missing": 0, "extra": 2}},
        "format_chars": {"text": good[:20] + "​" + good[20:40] + "­" + good[40:60] + "﻿" + good[60:],
                         "name": FACTORY[0]["name"]},
        "bad": bad,
    }


# ================================================================================ SysEx framing (spec §5)
SX_PREFIX = bytes([0x7D, 0x43, 0x59])
SX_VER = 0x01
SX_MAX_PAYLOAD = 256
CMD = dict(HELLO=0x01, GET_EDIT=0x02, PUT_EDIT=0x03, GET_SLOT=0x04, PUT_SLOT=0x05, ERASE_SLOT=0x06, GET_DIR=0x07,
           COMMIT=0x08, ABORT=0x09, NAK=0x7F)
ST = dict(OK=0x00, PENDING=0x01, E_FRAME=0x10, E_VERSION=0x11, E_CMD=0x12, E_ARG=0x13, E_PARAM_SET=0x14,
          E_BUSY=0x31, E_HOLD=0x32, E_NOTHING=0x33, E_ASKING=0x34, E_LOCKED=0x35,
          E_DECLINED=0x40, E_TIMEOUT=0x41, E_FLASH=0x42)


def pack7(b):
    out, acc, nb = bytearray(), 0, 0
    for x in b:
        acc |= x << nb
        nb += 8
        while nb >= 7:
            out.append(acc & 0x7F)
            acc >>= 7
            nb -= 7
    if nb:
        out.append(acc & 0x7F)
    return bytes(out)


def unpack7(s):
    m = len(s)
    n = (7 * m) // 8
    if any(x & 0x80 for x in s) or m != -(-8 * n // 7):
        raise FormatError("frame")
    acc, nb, out = 0, 0, bytearray()
    for x in s:
        acc |= x << nb
        nb += 7
        if nb >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            nb -= 8
    if acc:
        raise FormatError("frame")
    return bytes(out)


def sx_frame(cmd, seq, payload=b""):
    assert 0 <= cmd <= 0x7F and 0 <= seq <= 0x7F and len(payload) <= SX_MAX_PAYLOAD
    hdr = bytes([SX_VER, cmd, seq])
    return b"\xF0" + SX_PREFIX + hdr + pack7(payload + struct.pack("<I", crc32(hdr + payload))) + b"\xF7"


def sx_parse(frame):
    if len(frame) < 13 or frame[0] != 0xF0 or frame[-1] != 0xF7 or frame[1:4] != SX_PREFIX:
        raise FormatError("notours")
    ver, cmd, seq = frame[4], frame[5], frame[6]
    if ver != SX_VER:
        raise FormatError("version")
    body = unpack7(frame[7:-1])
    if len(body) < 4 or len(body) - 4 > SX_MAX_PAYLOAD:
        raise FormatError("frame")
    payload, crc = body[:-4], struct.unpack("<I", body[-4:])[0]
    if crc != crc32(bytes([ver, cmd, seq]) + payload):
        raise FormatError("frame")
    return ver, cmd, seq, payload


def app_sysex_claims(p):
    return len(p) >= 3 and p[0] == 0x7D and p[1] == 0x43 and p[2] == 0x59


def ota_take_accepts(p):
    """fm1-base bsp/chip/ota.c ota_take on the bytes between F0 and F7: does it decode as the update's framing?"""
    out, acc, nb = bytearray(), 0, 0
    for x in p:
        acc |= (x & 0x7F) << nb
        nb += 7
        if nb >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            nb -= 8
    d = len(out)
    if d < 7 or d > 640 or out[0] != 0 or out[1] != 0x59 or int.from_bytes(out[3:6], "little") + 7 != d:
        return False
    return (~sum(out[6:d - 1])) & 0xFF == out[d - 1]


def usb_key(frame):
    if len(frame) != 6:
        return None
    if frame == bytes([0xF0, 0x22, 0x24, 0x35, 0x7D, 0xF7]):
        return "UBOOT"
    if frame[1:5] == bytes([0x22, 0x24, 0x35, 0x7F]):
        return "OTA"
    return None


HS_QUERY = bytes([0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7])


def hello_reply(state=0, flags=0x01, version=b"DEV"):
    app = b"CYMA-1"
    return (bytes([ST["OK"], 1, PARAM_SET, N_V1, 32, NAME_MAX]) + struct.pack("<H", SX_MAX_PAYLOAD) +
            bytes([flags, state, len(app)]) + app + bytes([len(version)]) + version)


def hx(b):
    return " ".join("%02X" % x for x in b)


# ======================================================================================== the vectors (I2, I3)
NAMES = ["glass rain", "Über Pad", "<script>", "A*B>C", "  lo-fi   keys_2 ", "A very long sound name",
         "   ", "Café: ¿qué?", "BOWED GLASS", "CATHEDRAL GONG", "tab\there", "50% wet",
         "rock'n'roll (live)", "☃ snow"]


def example_frames(rec):
    ex = {}
    ex["HELLO"] = sx_frame(CMD["HELLO"], 0x01, bytes([1, 2]))
    ex["HELLO reply"] = sx_frame(CMD["HELLO"] | 0x40, 0x01, hello_reply())
    ex["GET_DIR"] = sx_frame(CMD["GET_DIR"], 0x02)
    ex["PUT_EDIT"] = sx_frame(CMD["PUT_EDIT"], 0x03, bytes([0x01]) + rec)
    ex["PUT_EDIT reply"] = sx_frame(CMD["PUT_EDIT"] | 0x40, 0x03, bytes([ST["OK"]]))
    ex["PUT_SLOT U05"] = sx_frame(CMD["PUT_SLOT"], 0x04, bytes([4]) + rec)
    ex["PUT_SLOT reply"] = sx_frame(CMD["PUT_SLOT"] | 0x40, 0x04,
                                    bytes([ST["OK"], 4]) + struct.pack("<II", 1 << 4, 0))
    ex["GET_SLOT U05"] = sx_frame(CMD["GET_SLOT"], 0x05, bytes([4]))
    ex["GET_SLOT reply"] = sx_frame(CMD["GET_SLOT"] | 0x40, 0x05, bytes([ST["OK"], 4, 1]) + rec)
    ex["COMMIT"] = sx_frame(CMD["COMMIT"], 0x06, bytes([0]))
    ex["COMMIT reply (PENDING)"] = sx_frame(CMD["COMMIT"] | 0x40, 0x06, bytes([ST["PENDING"]]))
    ex["COMMIT result (OK)"] = sx_frame(CMD["COMMIT"] | 0x40, 0x06, bytes([ST["OK"]]) + struct.pack("<I", 1 << 4))
    ex["NAK (bad CRC)"] = sx_frame(CMD["NAK"], 0x07, bytes([ST["E_FRAME"], 0x05, 1]))
    return ex


NOT_OURS = {"HS_QUERY": HS_QUERY[1:-1], "UBOOT key": bytes([0x22, 0x24, 0x35, 0x7D]),
            "OTA key": bytes([0x22, 0x24, 0x35, 0x7F]), "screen reader": bytes([0x7D, 0x53, 0x52, 1]),
            "log": bytes([0x7D, 0x4C, 0x47, 1]), "remote": bytes([0x7D, 0x4C, 0x43, 1]),
            "wifi": bytes([0x7D, 0x4C, 0x57, 1]), "backup 0x41": pack7(bytes([0, 0x59, 0x41, 0, 0, 0, 0xFF])),
            "LOOM-8": bytes([0x7D, 0x46, 0x4C, 1]), "empty": b"", "7D only": b"\x7D", "7D 43": b"\x7D\x43"}


def build_vectors(quick=False):
    """exactly what cyma_sounds_ref.py writes (its step 7), from the loaded table"""
    bowl = FACTORY[0]
    Q = bowl["q16"]
    rec = rec_encode(bowl["name"], Q)
    code = code_encode(bowl["name"], Q)
    ex = example_frames(rec)
    bad_codes = {"notcode": ["hello", "cy2.AQEo", ""],
                 "damaged": [code[:40], code[:-1], code[:-6] + ("A" if code[-6] != "A" else "B") + code[-5:],
                             "cy1." + "A" * 300],
                 "newer": [CODE_PREFIX + base64.urlsafe_b64encode(b + struct.pack("<I", crc32(b))).decode().rstrip("=")
                           for b in [bytes([2]) + rec[1:], bytes([1, 2]) + rec[2:]]]}
    r38 = bytes([1, 1, 38, 4]) + b"TEST" + struct.pack("<38H", *Q[:38])
    r42 = bytes([1, 1, 42, 4]) + b"TEST" + struct.pack("<42H", *(Q + [1, 2]))
    j_edge = {"POS X": 58, "lfo_wave": "s&h", "decay": 99, "exciter": "SCRAPE", "modes": "many", "flutter": 3,
              "MORPH": 25, "pos_x": 12}
    qe, ske, mie, _ = sound_params_from_json(j_edge)
    return {
        "spec": "docs/CYMA-1-sounds-spec.md v1",
        "param_set": 1,
        "factory": [{"num": f["num"], "name": f["name"], "q16": f["q16"],
                     "record_hex": rec_encode(f["name"], f["q16"]).hex(),
                     "code": code_encode(f["name"], f["q16"]),
                     "params": {p["key"]: json_value(p, f["q16"][p["id"]]) for p in PARAMS}} for f in FACTORY],
        "names": [[s, clean_name(s)] for s in NAMES],
        "bad_codes": bad_codes,
        "records_tolerated": {"n38_hex": r38.hex(), "n42_hex": r42.hex()},
        "json_import_edge": {"params": j_edge, "q16": qe, "skipped": ske, "missing_count": len(mie)},
        "frames": {k: f.hex() for k, f in ex.items()},
        "not_ours": {k: p.hex() for k, p in NOT_OURS.items()},
        "writer_digest": ({p["key"]: writer_digest(p) for p in PARAMS} if not quick else None),
        "crc32_check": {"input_hex": b"123456789".hex(), "crc32": "%08X" % crc32(b"123456789")},
        "cy2": build_cy2(),
    }


def reference_checks(quick, log):
    """the reference's own checks (its steps 1-6): -> list of failures"""
    fails = []
    bowl = FACTORY[0]
    Q = bowl["q16"]
    if not quick:
        for p in PARAMS:
            bad = [q for q in writer_domain(p) if from_json_value(p, json.loads(json.dumps(json_value(p, q))))[0] != q]
            if bad:
                fails.append(f"json round trip {p['key']}: {len(bad)} values, first {bad[:3]}")
        log("json round trip, exhaustive over 40 parameters: " + ("FAIL" if fails else "ok"))
    code = code_encode(bowl["name"], Q)
    got = code_decode("Listen: https://claude.ai/artifact/x#" + code[:60] + "\n" + code[60:] + ").")
    if got != ("SINGING BOWL", Q, 0, 0):
        fails.append("the worked example does not decode from a wrapped link")
    qj, sk, mi, notes = sound_params_from_json(sound_json(bowl["name"], Q)["params"])
    if qj != Q or sk or mi or notes:
        fails.append("the worked example's sound file does not read back")
    body, wrong = code[4:], 0
    for k in range(1, len(body)):
        try:
            code_decode(CODE_PREFIX + body[:k])
            wrong += 1
        except FormatError:
            pass
    for k in range(len(body)):
        for c in "A_9z-":
            if c == body[k]:
                continue
            try:
                if code_decode(CODE_PREFIX + body[:k] + c + body[k + 1:])[1] != Q:
                    wrong += 1
            except FormatError:
                pass
    log("share code robustness: %d truncations and substitutions accepted wrongly" % wrong)
    if wrong:
        fails.append("code robustness")
    sizes = [len(code_encode(f["name"], f["q16"])) for f in FACTORY]
    if min(sizes) < 126 or max(sizes) > 140 or len(code) != 138:
        fails.append(f"code lengths {min(sizes)}..{max(sizes)}, SINGING BOWL {len(code)} (spec: 126..140, 138)")
    ex = example_frames(rec_encode(bowl["name"], Q))
    largest = sx_frame(CMD["PUT_SLOT"], 0x08, bytes(SX_MAX_PAYLOAD))
    if len(largest) != 306:
        fails.append(f"largest frame {len(largest)} B, not 306")
    rnd = random.Random(1)
    ours = list(ex.values()) + [largest]
    for _ in range(100000):
        ours.append(sx_frame(rnd.randrange(128), rnd.randrange(128),
                             bytes(rnd.randrange(256) for _ in range(rnd.randrange(0, 257)))))
    bad = 0
    for f in ours:
        p = f[1:-1]
        if len(f) < 13 or usb_key(f) is not None or any(x >= 0x80 for x in p) or ota_take_accepts(p) \
                or not app_sysex_claims(p):
            bad += 1
    for k, p in NOT_OURS.items():
        if app_sysex_claims(p):
            fails.append(f"the claim rule takes {k}")
    if not (ota_take_accepts(NOT_OURS["HS_QUERY"]) and ota_take_accepts(NOT_OURS["backup 0x41"])):
        fails.append("the update framing model refuses HS_QUERY or backup 0x41")
    log("update safety: %d of %d frames of ours broke P1/P3/P4 (a 6-byte key, a byte >= 0x80, the update framing)"
        % (bad, len(ours)))
    if bad:
        fails.append("update safety fuzz")
    fails += cy2_checks(log)
    return fails


def cy2_checks(log):
    """the short link code (revision 2026-10-24b): the reference against its own vectors and the spec's claims"""
    fails = []
    v = build_cy2()
    if list(CY2_DEF) != DEF_Q16[:len(CY2_DEF)]:
        fails.append("tools/cyma1_params.json's def_q16 is no longer cy2's frozen CY2_DEF: cy2 keeps its table (never "
                     "change CY2_DEF); decide what the new defaults mean for cy1's missing values, and say so in the spec")
    for f, e in zip(FACTORY, v["factory"]):
        for text in (e["code"], "#" + e["code"], "Listen: https://claude.ai/artifact/x#" + e["code"][:30] + "\n  " +
                     e["code"][30:] + ")."):
            if any_decode(text) != (f["name"], f["q16"], 0, 0, "cy2"):
                fails.append(f"cy2 {f['name']} does not decode from {text[:40]!r}")
        if e["length"] != code2_len(f["name"], f["q16"]):
            fails.append(f"cy2 {f['name']}: {e['length']} characters, the formula says {code2_len(f['name'], f['q16'])}")
        if any_decode(f_code := code_encode(f["name"], f["q16"])) != (f["name"], f["q16"], 0, 0, "cy1"):
            fails.append(f"cy1 {f['name']} no longer decodes beside cy2 ({f_code[:20]})")
    lens = [e["length"] for e in v["factory"]]
    fit = sum(e["anchor_fits"] for e in v["factory"])
    log("cy2: the 24 factory link codes are %d..%d characters (SINGING BOWL %d); %d of 24 fit the %d-character anchor"
        " (cy1: %d..%d, %d fit)" % (min(lens), max(lens), lens[0], fit, ANCHOR_MAX,
                                    min(e["cy1_length"] for e in v["factory"]), max(e["cy1_length"] for e in v["factory"]),
                                    sum(e["cy1_anchor_fits"] for e in v["factory"])))
    if fit != 24:
        fails.append(f"cy2: only {fit} of the 24 factory sounds fit a link")
    ed = v["edges"]
    want = {"all_defaults": (30, True), "every_value_changed": (144, False), "longest_that_fits": (128, True),
            "shortest_that_does_not": (131, False)}
    for k, (n, fits) in want.items():
        e = ed[k]
        if (e["length"], e["anchor_fits"]) != (n, fits) or any_decode(e["code"])[:2] != (e["decoded_name"], e["q16"]):
            fails.append(f"cy2 edge {k}: {e['length']} characters, fits {e['anchor_fits']} (want {n}, {fits}), or it "
                         f"does not read back")
    for k, t in v["tolerated"].items():
        got = any_decode(t["code"])
        if got[1:4] != (t["q16"], t["extra"], t["missing"]):
            fails.append(f"cy2 tolerated {k}: extra {got[2]}, missing {got[3]}")
    if any_decode(v["format_chars"]["text"])[:2] != (v["format_chars"]["name"], FACTORY[0]["q16"]):
        fails.append("cy2 with zero-width characters, a soft hyphen and a BOM inside does not decode")
    for kind, lst in v["bad"].items():
        for c in lst:
            try:
                any_decode(c)
                fails.append(f"cy2 bad code accepted: {c[:30]}")
            except FormatError as e:
                if e.code != kind:
                    fails.append(f"cy2 bad code {c[:30]}: {e.code}, not {kind}")
    body, Q, wrong = v["factory"][0]["code"][4:], FACTORY[0]["q16"], 0
    for k in range(1, len(body)):
        try:
            any_decode(CODE2_PREFIX + body[:k])
            wrong += 1
        except FormatError:
            pass
    for k in range(len(body)):
        for c in "A_9z-":
            if c != body[k]:
                try:
                    if any_decode(CODE2_PREFIX + body[:k] + c + body[k + 1:])[1] != Q:
                        wrong += 1
                except FormatError:
                    pass
    log("cy2 robustness: %d truncations and substitutions of SINGING BOWL's link code accepted wrongly" % wrong)
    if wrong:
        fails.append("cy2 robustness")
    return fails


# ============================================================================================= vectors.h
def c_str(s):
    """a C string literal of raw bytes (octal escapes for non-ASCII: never greedy past three digits)"""
    b = s.encode("utf-8") if isinstance(s, str) else bytes(s)
    o = []
    for c in b:
        if c == 0x22:
            o.append('\\"')
        elif c == 0x5C:
            o.append("\\\\")
        elif c == 0x09:
            o.append("\\t")
        elif 0x20 <= c < 0x7F:
            o.append(chr(c))
        else:
            o.append("\\%03o" % c)
    return '"' + "".join(o) + '"'


def c_bytes(b, per=24, indent="      "):
    rows = []
    for i in range(0, len(b), per):
        rows.append(indent + ", ".join("0x%02X" % x for x in b[i:i + per]))
    return ",\n".join(rows) if rows else indent + "0"


def make_header(vec):
    L = []
    w = L.append
    w("/* SPDX-License-Identifier: MPL-2.0 */")
    w("/* CYMA-1 Sounds: the frozen test vectors (docs/CYMA-1-sounds-spec.md §12.2 I2/I3) for the C codec.")
    w(" * GENERATED by tests/sounds/gen_vectors.py header from tests/sounds/vectors.json and tools/cyma1_params.json:")
    w(" * do not edit (gen_vectors.py check fails when this file and vectors.json disagree). */")
    w("#pragma once")
    w("#include <stdint.h>")
    w("")
    w("#define CYV_NFACTORY %d" % len(vec["factory"]))
    w("#define CYV_NPARAMS %d" % N_V1)
    w("#define CYV_NFRAMES %d" % len(vec["frames"]))
    crc_in = bytes.fromhex(vec["crc32_check"]["input_hex"])
    w("static const char CYV_CRC_INPUT[] = %s;          /* zlib's check value input */" % c_str(crc_in))
    w("#define CYV_CRC_CHECK 0x%sU" % vec["crc32_check"]["crc32"])
    bowl = bytes.fromhex(vec["factory"][0]["record_hex"])
    w("#define CYV_BOWL_CRC 0x%08XU                       /* the CRC-32 of SINGING BOWL's record (spec §4) */" % crc32(bowl))
    w("")
    w("/* the defaults as stored (def_q16, §3.6): what a record's missing values become */")
    w("static const uint16_t CYV_DEF_Q16[CYV_NPARAMS] = {%s};" % ", ".join(str(x) for x in DEF_Q16))
    w("")
    w("typedef struct {")
    w("    int num;")
    w("    const char *name;")
    w("    uint16_t q16[CYV_NPARAMS];")
    w("    uint8_t rec_len;")
    w("    uint8_t rec[98];")
    w("    const char *code;")
    w("} cyv_factory_t;")
    w("static const cyv_factory_t CYV_FACTORY[CYV_NFACTORY] = {")
    for f in vec["factory"]:
        r = bytes.fromhex(f["record_hex"])
        w("  {%d, %s," % (f["num"], c_str(f["name"])))
        w("   {%s}," % ", ".join(str(x) for x in f["q16"]))
        w("   %d, {" % len(r))
        w(c_bytes(r, 24, "      ") + "},")
        w("   %s}," % c_str(f["code"]))
    w("};")
    w("")
    w("typedef struct {")
    w("    const char *in;")
    w("    const char *out;")
    w("} cyv_name_t;")
    w("/* vectors.json \"names\" whose input is plain ASCII: the C rule gives the same result as JS and Python */")
    w("static const cyv_name_t CYV_NAMES_ASCII[] = {")
    na = 0
    for s, c in vec["names"]:
        if all(ord(ch) < 0x80 for ch in s):
            w("  {%s, %s}," % (c_str(s), c_str(c)))
            na += 1
    w("};")
    w("#define CYV_NNAMES_ASCII %d" % na)
    w("/* the others, as UTF-8 bytes, with what the device makes of them: bytes >= 0x80 dropped (it has no NFKD), then")
    w(" * the same rule (spec §3.4 step 1). Derived here; vectors.json holds the JS/Python results */")
    w("static const cyv_name_t CYV_NAMES_C[] = {")
    nc = 0
    for s, _ in vec["names"]:
        if not all(ord(ch) < 0x80 for ch in s):
            w("  {%s, %s}," % (c_str(s), c_str(clean_name_c(s.encode("utf-8")))))
            nc += 1
    w("};")
    w("#define CYV_NNAMES_C %d" % nc)
    w("")
    w("typedef struct {")
    w("    const char *what;")
    w("    uint16_t len;")
    w("    uint8_t b[160];")
    w("} cyv_frame_t;")
    w("/* vectors.json \"frames\": whole F0..F7 frames */")
    w("static const cyv_frame_t CYV_FRAMES[CYV_NFRAMES] = {")
    for k, h in vec["frames"].items():
        b = bytes.fromhex(h)
        w("  {%s, %d, {" % (c_str(k), len(b)))
        w(c_bytes(b, 24, "      ") + "}},")
    w("};")
    w("")
    for k, nm in (("n38_hex", "CYV_REC_N38"), ("n42_hex", "CYV_REC_N42")):
        b = bytes.fromhex(vec["records_tolerated"][k])
        w("static const uint8_t %s[%d] = {" % (nm, len(b)))
        w(c_bytes(b, 24, "    ") + "};")
    w("")
    w("/* vectors.json \"not_ours\": the bytes between F0 and F7 of frames app_sysex must leave to the platform */")
    w("typedef struct {")
    w("    const char *what;")
    w("    uint8_t len;")
    w("    uint8_t b[16];")
    w("} cyv_not_ours_t;")
    w("static const cyv_not_ours_t CYV_NOT_OURS[] = {")
    for k, h in vec["not_ours"].items():
        b = bytes.fromhex(h)
        w("  {%s, %d, {%s}}," % (c_str(k), len(b), ", ".join("0x%02X" % x for x in b) or "0"))
    w("};")
    w("#define CYV_NNOT_OURS %d" % len(vec["not_ours"]))
    return "\n".join(L) + "\n"


# ======================================================================== the scenario scripts' frames
def script_frames(path):
    """(line number, hex, meant_bad) of every 'sysex' frame in a host script"""
    out, corrupt = [], False
    with open(path) as f:
        for i, raw in enumerate(f, 1):
            line = raw.strip()
            if line.startswith("#"):
                if re.match(r"#\s*corrupt\b", line, re.I):
                    corrupt = True
                continue
            tok = line.split()
            if tok and tok[0] == "sysex":
                out.append((i, "".join(tok[1:]), corrupt))
                corrupt = False
            elif tok:
                corrupt = False
    return out


def check_scripts(app, log):
    fails = []
    import glob
    paths = sorted(glob.glob(os.path.join(app, "tests", "sounds_*.txt")))
    for p in paths:
        frames = script_frames(p)
        nb = 0
        for ln, h, bad in frames:
            try:
                b = bytes.fromhex(h)
            except ValueError:
                fails.append(f"{os.path.basename(p)}:{ln}: not hex")
                continue
            if len(h) > 444:
                fails.append(f"{os.path.basename(p)}:{ln}: {len(b)} B is over a script line's 222 B (use sysexf)")
            if bad:
                nb += 1
                continue
            if not b or b[0] != 0xF0 or b[-1] != 0xF7:
                fails.append(f"{os.path.basename(p)}:{ln}: not F0..F7")
                continue
            if b[1:4] == SX_PREFIX:
                try:
                    sx_parse(b)
                except FormatError as e:
                    fails.append(f"{os.path.basename(p)}:{ln}: ours, but {e.code} (mark a deliberate one '# corrupt')")
        log(f"{os.path.basename(p)}: {len(frames)} frames, {nb} deliberately bad, the rest well formed")
    return fails


# ====================================================================== the scenario scripts (T2, T3: §11.1)
# tests/sounds_sysex.txt and tests/sounds_safety.txt are written from here ("gen_vectors.py scripts"), so every frame,
# CRC and expected reply in them comes from this codec, never from a hand. "check" fails when they are stale.
U32 = lambda x: struct.pack("<I", x & 0xFFFFFFFF)
STATUS = {v: k for k, v in ST.items()}
HOST_FLAGS = 0x03                                    # HELLO flags in the host and wasm builds: confirm (b0), emulator (b1)
CY_USER_MAGIC = 0x31414D43                           # "CMA1"


class BankModel:
    """the device's cy_user_bank_t in RAM, as the scenario changes it: for GET_DIR's bank_crc and GET_SLOT replies"""

    def __init__(self):
        self.magic, self.version, self.used = 0, 0, 0
        self.p = [None] * 32                         # (name, q16[40]) or None (all-zero bytes)

    def put(self, slot, name, q):
        self.magic, self.version = CY_USER_MAGIC, 1
        self.p[slot] = (clean_name(name), list(q))
        self.used |= 1 << slot

    def erase(self, slot):
        self.magic, self.version = CY_USER_MAGIC, 1
        self.p[slot] = None
        self.used &= ~(1 << slot)

    def bytes(self):
        b = struct.pack("<IHHI", self.magic, self.version, 0, self.used)
        for e in self.p:
            if e is None:
                b += bytes(94)
            else:
                b += e[0].encode("ascii").ljust(14, b"\0") + struct.pack("<40H", *e[1])
        assert len(b) == 3020
        return b

    def crc(self):
        return crc32(self.bytes())

    def copy(self):
        m = BankModel()
        m.magic, m.version, m.used, m.p = self.magic, self.version, self.used, list(self.p)
        return m


class Script:
    def __init__(self, path, header):
        self.path, self.L = path, list(header)
        self.seq, self.replies = 0, 0

    def c(self, text=""):
        for t in text.split("\n"):
            self.L.append(("# " + t).rstrip())

    def raw(self, *lines):
        for ln in lines:
            if ln == "reboot":           # (sx_crc_stale counts per boot: every GET_DIR of this boot served a fresh CRC)
                self.exp("sx_crc_stale", 0)
            self.L.append(ln)

    def exp(self, what, val):
        self.L.append(f"expect {what} {val}")

    def next_seq(self):
        self.seq = (self.seq + 1) & 0x7F
        return self.seq

    def frame(self, cmd, payload=b"", seq=None):
        return sx_frame(cmd, self.next_seq() if seq is None else seq, payload)

    def send(self, f, reply=None, status=None, counts=True, quiet=False, note=None, taken=None):
        """one frame in. reply: the exact reply payload (checked in the log), status: its status byte (sx_status, sx_cmd
        checked); counts: the frame gets exactly one reply now (sx_replies); quiet: no reply at all (claimed and ignored,
        or not ours: no sysex_out line in the log, sx_replies unchanged)"""
        cmd, seq = (f[5], f[6]) if len(f) > 7 else (0, 0)
        rcmd = 0x7F if status in (ST["E_FRAME"], ST["E_VERSION"]) else (cmd | 0x40) & 0x7F
        if status in (ST["E_FRAME"], ST["E_VERSION"]):     # a deliberately bad frame: the reference refuses it alike
            try:
                sx_parse(f)
                raise AssertionError("a 'bad' frame the reference accepts: " + f.hex())
            except FormatError as e:
                short = e.code == "notours" and f[1:4] == SX_PREFIX and len(f) < 13   # (the host parser's length rule)
                assert e.code == ("frame" if status == ST["E_FRAME"] else "version") or short, (e.code, f.hex())
        if note:
            self.c(note)
        if reply is not None:
            self.L.append(r"#@log (?i)sysex_out\s*" + sx_frame(rcmd, seq, reply).hex().upper() + r"\b")
        elif quiet:
            self.L.append(r"#@nolog (?i)sysex_out")
        self.L.append("sysex " + f.hex().upper())
        if taken is not None:
            self.exp("sx_taken", taken)                # app_sysex's own answer: 1 ours, 0 left to the platform
        if quiet:
            if self.replies is not None:
                self.exp("sx_replies", self.replies)
            return
        if counts and self.replies is not None:
            self.replies += 1
            self.exp("sx_replies", self.replies)
        if status is not None:
            self.exp("sx_status", status)
            self.exp("sx_cmd", rcmd)

    def got_async(self, n=1):
        """an unsolicited 0x48 (the COMMIT's final result) went out"""
        if self.replies is not None:
            self.replies += n
            self.exp("sx_replies", self.replies)

    def unknown_count(self, why):
        self.c(f"(from here sx_replies is not counted: {why})")
        self.replies = None

    def text(self):
        return "\n".join(self.L) + "\n"


def _q(n):
    return FACTORY[n - 1]["q16"]


def _rec(name, q):
    return rec_encode(name, q)


def _dir_reply(bank, staged=0, erased=0, state=0, last_result=0, last_seq=0):
    return bytes([0]) + U32(bank.used) + U32(staged) + U32(erased) + U32(bank.crc()) + bytes([state, last_result, last_seq])


SCRIPT_HEAD = ["# SPDX-License-Identifier: MPL-2.0",
               "# GENERATED by tests/sounds/gen_vectors.py scripts (every frame, CRC and reply from the spec's reference",
               "# codec): edit the generator, not this file. gen_vectors.py check fails when the two disagree."]


def script_sysex():
    s = Script("sounds_sysex.txt", SCRIPT_HEAD + [
        "# CYMA-1 Sounds T2 (docs/CYMA-1-sounds-spec.md §5, §6.3-6.7, §11.1): the USB SysEx protocol through app_sysex in",
        "# the host simulator. PUT_EDIT only plays (RAM; SAVE then goes to the first free slot even from a user slot);",
        "# slot changes are staged and written once, only after SAVE is pressed on the panel during the question; HOME, a",
        "# page button, SEL+SAVE, PRESETS, PANIC, ABORT and HELLO decline; a timeout declines; a failed flash keeps the",
        "# bank in RAM. Every reply is checked: its status (sx_status) and count (sx_replies), and where it is",
        "# deterministic, its exact bytes in the log (#@log), including GET_DIR's CRC of the whole bank in RAM.",
        "#   run by tools/run_all.sh (step sounds_scen): gen_vectors.py scen --host build/host/cyma-1_host"])
    bank = BankModel()
    E = ST
    s.raw("master 4096", "wait 1700", "tour 0")
    s.exp("preset", 1)
    s.exp("page", 0)
    for w, v in (("sx_state", 0), ("sx_replies", 0), ("sx_status", -1), ("user_writes", 0), ("user_mask", 0),
                 ("auditioned", 0), ("sx_staged", 0)):
        s.exp(w, v)

    s.c("---- HELLO: the reply names the protocol, the table, the limits; flags: SAVE confirms (b0), emulator (b1)")
    s.send(s.frame(CMD["HELLO"], bytes([1, 3])), reply=hello_reply(state=0, flags=HOST_FLAGS), status=0)
    s.send(s.frame(CMD["GET_DIR"]), status=0)
    s.exp("sx_state", 0)

    s.c("---- refusals that change nothing: an unknown command, wrong lengths, slot 32, a newer record, nothing staged")
    s.send(s.frame(0x0A), reply=bytes([E["E_CMD"]]), status=E["E_CMD"])
    s.send(s.frame(0x3E, b"\x01\x02"), reply=bytes([E["E_CMD"]]), status=E["E_CMD"],
           note="(0x3E, the last unknown request tested: 0x3F's reply code 0x7F would be the NAK's)")
    s.send(s.frame(CMD["GET_SLOT"], bytes([32])), reply=bytes([E["E_ARG"]]), status=E["E_ARG"])
    s.send(s.frame(CMD["GET_SLOT"], bytes([0, 0])), reply=bytes([E["E_ARG"]]), status=E["E_ARG"],
           note="GET_SLOT is one byte: two is E_ARG")
    s.send(s.frame(CMD["GET_DIR"], bytes([0])), reply=bytes([E["E_ARG"]]), status=E["E_ARG"])
    rec5 = _rec("MARIMBA", _q(4))
    s.send(s.frame(CMD["PUT_SLOT"], bytes([4]) + rec5[:-1]), reply=bytes([E["E_ARG"]]), status=E["E_ARG"],
           note="a record one byte short")
    s.send(s.frame(CMD["PUT_SLOT"], bytes([32]) + rec5), reply=bytes([E["E_ARG"]]), status=E["E_ARG"])
    s.send(s.frame(CMD["PUT_SLOT"], bytes([4, 1, 1, 0, 0])), reply=bytes([E["E_ARG"]]), status=E["E_ARG"],
           note="a record with N = 0")
    s.send(s.frame(CMD["PUT_SLOT"], bytes([4]) + bytes([2]) + rec5[1:]), reply=bytes([E["E_PARAM_SET"]]),
           status=E["E_PARAM_SET"], note="layout 2: a newer record")
    s.send(s.frame(CMD["PUT_EDIT"], bytes([0]) + bytes([1, 2]) + rec5[2:]), reply=bytes([E["E_PARAM_SET"]]),
           status=E["E_PARAM_SET"], note="param_set 2")
    s.send(s.frame(CMD["ERASE_SLOT"], bytes([40])), reply=bytes([E["E_ARG"]]), status=E["E_ARG"])
    s.send(s.frame(CMD["COMMIT"], bytes([0])), reply=bytes([E["E_NOTHING"]]), status=E["E_NOTHING"])
    s.send(s.frame(CMD["GET_SLOT"], bytes([0])), reply=bytes([0, 0, 0]), status=0, note="U01 is empty")
    s.exp("sx_state", 0)
    s.exp("sx_staged", 0)
    s.exp("user_writes", 0)

    s.c("---- PUT_EDIT (glide): it plays, nothing is written; the sound counts as edited and auditioned")
    q2 = _q(2)
    s.send(s.frame(CMD["PUT_EDIT"], bytes([1]) + _rec("From host", q2)), reply=bytes([0]), status=0)
    for w, v in (("auditioned", 1), ("edited", 1), ("loaded", 1), ("user_writes", 0), ("sx_state", 0)):
        s.exp(w, v)
    s.raw("wait 400")
    s.exp("glide", 0)
    s.exp("edited", 1)
    s.c("GET_EDIT: preset 0 (01 loaded), flags edited + auditioned, the record exactly what SAVE would store")
    s.send(s.frame(CMD["GET_EDIT"]), reply=bytes([0, 0, 3]) + _rec("FROM HOST", q2), status=0)

    s.c("---- SAVE SAVE: an auditioned sound goes to the first free slot (U01)")
    s.raw("tap SAVE", "wait 200")
    s.exp("save_state", 1)
    s.exp("save_slot", 0)
    s.raw("tap SAVE", "wait 300")
    bank.put(0, "FROM HOST", q2)
    for w, v in (("loaded", 25), ("auditioned", 0), ("edited", 0), ("user_writes", 1), ("user_mask", bank.used),
                 ("user1", 1)):
        s.exp(w, v)
    s.send(s.frame(CMD["GET_SLOT"], bytes([0])), reply=bytes([0, 0, 1]) + _rec("FROM HOST", q2), status=0)

    s.c("---- from a loaded user slot (U01): a PUT_EDIT, then SAVE SAVE goes to U02, never silently over U01")
    q3 = _q(3)
    s.send(s.frame(CMD["PUT_EDIT"], bytes([0]) + _rec("second take", q3)), reply=bytes([0]), status=0)
    s.exp("auditioned", 1)
    s.exp("loaded", 25)
    s.exp("edited", 1)
    s.raw("wait 100", "tap SAVE", "wait 200")
    s.exp("save_state", 1)
    s.exp("save_slot", 1)
    s.raw("tap SAVE", "wait 300")
    bank.put(1, "SECOND TAKE", q3)
    for w, v in (("loaded", 26), ("auditioned", 0), ("user_writes", 2), ("user_mask", bank.used)):
        s.exp(w, v)

    s.c("---- PUT_EDIT as plain edits (flags b1): the name stays, not auditioned; SEL+SAVE reverts it")
    q4 = _q(4)
    s.send(s.frame(CMD["PUT_EDIT"], bytes([2]) + _rec("IGNORED NAME", q4)), reply=bytes([0]), status=0)
    s.exp("auditioned", 0)
    s.exp("edited", 1)
    s.send(s.frame(CMD["GET_EDIT"]), reply=bytes([0, 25, 1]) + _rec("SECOND TAKE", q4), status=0)
    s.raw("press SEL", "tap SAVE", "release SEL", "wait 400")
    s.exp("edited", 0)
    s.exp("user_writes", 2)

    s.c("---- COMPARE held (SEL+HOME): PUT_EDIT is E_BUSY")
    s.raw("press SEL", "press HOME", "wait 100")
    s.exp("compare", 1)
    s.send(s.frame(CMD["PUT_EDIT"], bytes([0]) + _rec("BUSY", q4)), reply=bytes([E["E_BUSY"]]), status=E["E_BUSY"])
    s.raw("release HOME", "release SEL", "wait 100")
    s.exp("compare", 0)
    s.exp("auditioned", 0)

    s.c("---- three PUT_SLOTs: staged in RAM only (the names cleaned on the way in)")
    trio = [(4, "MARIMBA", _q(4)), (5, "VIBES", _q(5)), (6, "lower case 07", _q(6))]
    staged = 0
    for slot, name, q in trio:
        staged |= 1 << slot
        s.send(s.frame(CMD["PUT_SLOT"], bytes([slot]) + _rec(name, q)),
               reply=bytes([0, slot]) + U32(staged) + U32(0), status=0)
        s.exp("sx_state", 1)
    s.exp("sx_staged", staged)
    s.exp("user_writes", 2)
    s.send(s.frame(CMD["GET_SLOT"], bytes([4])), reply=bytes([0, 4, 0]), status=0,
           note="GET_SLOT reads the committed bank: U05 is still empty")

    s.c("---- COMMIT: the question (USB: SAVE 3 SOUNDS?), then HOME: declined, nothing written")
    fc = s.frame(CMD["COMMIT"], bytes([0]))
    s.send(fc, reply=bytes([E["PENDING"]]), status=E["PENDING"])
    s.exp("sx_state", 2)
    s.send(fc, reply=bytes([E["PENDING"]]), status=E["PENDING"],
           note="the same COMMIT again (a host retry, same SEQ): the cached PENDING, not executed again (not E_ASKING)")
    s.exp("sx_state", 2)
    s.send(s.frame(CMD["COMMIT"], bytes([0])), reply=bytes([E["E_ASKING"]]), status=E["E_ASKING"],
           note="a new COMMIT while the question is open")
    s.send(s.frame(CMD["PUT_SLOT"], bytes([7]) + _rec("LATE", q4)), reply=bytes([E["E_ASKING"]]), status=E["E_ASKING"])
    s.send(s.frame(CMD["ERASE_SLOT"], bytes([0])), reply=bytes([E["E_ASKING"]]), status=E["E_ASKING"])
    s.send(s.frame(CMD["PUT_EDIT"], bytes([0]) + _rec("LATE", q4)), reply=bytes([E["E_ASKING"]]), status=E["E_ASKING"])
    s.send(s.frame(CMD["GET_SLOT"], bytes([4])), reply=bytes([0, 4, 0]), status=0, note="reads still work")
    s.send(s.frame(CMD["GET_DIR"]), status=0)
    s.exp("sx_state", 2)
    s.exp("sx_staged", staged)
    s.raw("tap HOME", "wait 100")
    s.got_async()
    for w, v in (("sx_result", E["E_DECLINED"]), ("sx_status", E["E_DECLINED"]), ("sx_cmd", 0x48), ("sx_state", 0),
                 ("sx_staged", 0), ("user_writes", 2), ("user_mask", bank.used), ("page", 0), ("loaded", 26)):
        s.exp(w, v)

    s.c("---- again, with a note ringing; SAVE: written once (faded around the stall), the mask as staged")
    for slot, name, q in trio:
        s.send(s.frame(CMD["PUT_SLOT"], bytes([slot]) + _rec(name, q)), status=0)
    s.exp("sx_staged", staged)
    s.raw("key 7 down 100", "wait 50")
    fc = s.frame(CMD["COMMIT"], bytes([0]))
    commit_seq = fc[6]
    s.send(fc, reply=bytes([E["PENDING"]]), status=E["PENDING"])
    s.raw("tap SAVE", "wait 300")
    for slot, name, q in trio:
        bank.put(slot, name, q)
    s.got_async()
    for w, v in (("sx_result", 0), ("sx_status", 0), ("sx_cmd", 0x48), ("sx_state", 0), ("sx_staged", 0),
                 ("user_writes", 3), ("user_mask", bank.used), ("user5", 1), ("user7", 1), ("save_pending", 0),
                 ("loaded", 26), ("edited", 0)):
        s.exp(w, v)
    s.raw("key 7 up", "wait 100")
    s.send(s.frame(CMD["GET_DIR"]), reply=_dir_reply(bank, last_result=0, last_seq=commit_seq), status=0,
           note="GET_DIR: the used mask, nothing staged, the CRC-32 of the 3,020 B bank, idle, the last COMMIT's result")
    for slot, name, q in trio:
        s.send(s.frame(CMD["GET_SLOT"], bytes([slot])), reply=bytes([0, slot, 1]) + _rec(name, q), status=0)

    s.c("---- a replace of the loaded slot (U06, unedited): it follows, reloaded (USB: REPLACE U06?)")
    s.raw("preset 30", "wait 400")
    s.exp("loaded", 30)
    s.exp("edited", 0)
    q7 = _q(7)
    s.send(s.frame(CMD["PUT_SLOT"], bytes([5]) + _rec("NEW SIX", q7)), status=0)
    fc = s.frame(CMD["COMMIT"], bytes([0]))
    s.send(fc, status=E["PENDING"])
    s.raw("tap SAVE", "wait 500")
    bank.put(5, "NEW SIX", q7)
    s.got_async()
    for w, v in (("sx_result", 0), ("user_writes", 4), ("user_mask", bank.used), ("loaded", 30), ("edited", 0)):
        s.exp(w, v)
    s.send(s.frame(CMD["GET_EDIT"]), reply=bytes([0, 29, 0]) + _rec("NEW SIX", q7), status=0)
    s.c("edited, then replaced: the stored sound and name follow, the edits stay; SEL+SAVE gives the new sound")
    s.raw("param DECAY 5", "wait 100")
    s.exp("edited", 1)
    q8 = _q(8)
    s.send(s.frame(CMD["PUT_SLOT"], bytes([5]) + _rec("NEWER SIX", q8)), status=0)
    s.send(s.frame(CMD["COMMIT"], bytes([0])), status=E["PENDING"])
    s.raw("tap SAVE", "wait 300")
    bank.put(5, "NEWER SIX", q8)
    s.got_async()
    for w, v in (("sx_result", 0), ("user_writes", 5), ("loaded", 30), ("edited", 1)):
        s.exp(w, v)
    s.raw("press SEL", "tap SAVE", "release SEL", "wait 500")
    s.exp("edited", 0)
    s.send(s.frame(CMD["GET_EDIT"]), reply=bytes([0, 29, 0]) + _rec("NEWER SIX", q8), status=0)

    s.c("---- an erase and a put at once (USB: CHANGE 2 SLOTS?); then the loaded slot erased: it keeps playing")
    q9 = _q(9)
    s.send(s.frame(CMD["ERASE_SLOT"], bytes([4])), reply=bytes([0, 4]) + U32(0) + U32(1 << 4), status=0)
    s.send(s.frame(CMD["PUT_SLOT"], bytes([2]) + _rec("BOWED GLASS", q9)), reply=bytes([0, 2]) + U32(1 << 2) + U32(1 << 4),
           status=0)
    s.send(s.frame(CMD["ERASE_SLOT"], bytes([2])), reply=bytes([0, 2]) + U32(0) + U32((1 << 4) | (1 << 2)), status=0,
           note="an erase clears that slot's staged bit, and a put its erase bit")
    s.send(s.frame(CMD["PUT_SLOT"], bytes([2]) + _rec("BOWED GLASS", q9)), reply=bytes([0, 2]) + U32(1 << 2) + U32(1 << 4),
           status=0)
    s.exp("sx_staged", (1 << 4) | (1 << 2))
    s.send(s.frame(CMD["COMMIT"], bytes([0])), status=E["PENDING"])
    s.raw("tap SAVE", "wait 300")
    bank.erase(4)
    bank.put(2, "BOWED GLASS", q9)
    s.got_async()
    for w, v in (("sx_result", 0), ("user_writes", 6), ("user_mask", bank.used), ("user5", 0), ("user3", 1)):
        s.exp(w, v)
    s.send(s.frame(CMD["GET_SLOT"], bytes([4])), reply=bytes([0, 4, 0]), status=0)
    s.send(s.frame(CMD["ERASE_SLOT"], bytes([5])), status=0)
    fc = s.frame(CMD["COMMIT"], bytes([0]))
    s.send(fc, status=E["PENDING"])
    s.raw("tap SAVE", "wait 300")
    bank.erase(5)
    s.got_async()
    for w, v in (("sx_result", 0), ("user_writes", 7), ("user_mask", bank.used), ("loaded", 30)):
        s.exp(w, v)
    s.send(s.frame(CMD["GET_DIR"]), reply=_dir_reply(bank, last_result=0, last_seq=fc[6]), status=0)

    s.c("---- nobody answers: E_TIMEOUT after 30 s, nothing written")
    q10 = _q(10)
    s.send(s.frame(CMD["PUT_SLOT"], bytes([7]) + _rec("NOBODY", q10)), status=0)
    s.send(s.frame(CMD["COMMIT"], bytes([0])), status=E["PENDING"])
    s.raw("wait 29000")
    s.exp("sx_state", 2)
    s.raw("wait 2000")
    s.got_async()
    for w, v in (("sx_result", E["E_TIMEOUT"]), ("sx_status", E["E_TIMEOUT"]), ("sx_state", 0), ("sx_staged", 0),
                 ("user_writes", 7), ("user_mask", bank.used)):
        s.exp(w, v)

    s.c("---- staging left alone: dropped after 60 s of silence from the host, with no frame sent")
    s.send(s.frame(CMD["PUT_SLOT"], bytes([7]) + _rec("FORGOTTEN", q10)), status=0)
    s.raw("wait 55000")
    s.exp("sx_state", 1)
    s.raw("wait 6000")
    s.exp("sx_state", 0)
    s.exp("sx_staged", 0)
    s.exp("sx_replies", s.replies)

    s.c("---- COMMIT while the panel's SAVE dialog is open: E_BUSY; HOME closes the dialog, the staging stays")
    s.send(s.frame(CMD["PUT_SLOT"], bytes([7]) + _rec("PATIENT", q10)), status=0)
    s.raw("tap SAVE", "wait 100")
    s.exp("save_state", ">0")
    s.send(s.frame(CMD["COMMIT"], bytes([0])), reply=bytes([E["E_BUSY"]]), status=E["E_BUSY"])
    s.raw("tap HOME", "wait 100")
    s.exp("save_state", 0)
    s.exp("sx_state", 1)

    s.c("---- the other ways to say no, each declining (0x40) with nothing written and the press doing nothing else")
    for what, lines, after in (
            ("a page button (FX): the page does not change", ["tap FX", "wait 100"], [("page", 0)]),
            ("SEL+SAVE: declined, not a REVERT", ["press SEL", "tap SAVE", "release SEL", "wait 100"],
             [("edited", 0), ("loaded", 30)]),
            ("PRESETS: the turn is swallowed", ["turn PRESET 1", "wait 700"], [("loaded", 30), ("browsing", 0)]),
            ("PANIC (OCT- and OCT+): declined, and PANIC still happens", ["press OCT-", "press OCT+", "wait 60",
                                                                         "release OCT+", "release OCT-", "wait 100"],
             [("octave", 0)])):
        s.c(what)
        s.send(s.frame(CMD["PUT_SLOT"], bytes([7]) + _rec("PATIENT", q10)), status=0)
        s.send(s.frame(CMD["COMMIT"], bytes([0])), status=E["PENDING"])
        s.exp("sx_state", 2)
        s.raw(*lines)
        s.got_async()
        for w, v in [("sx_result", E["E_DECLINED"]), ("sx_status", E["E_DECLINED"]), ("sx_state", 0),
                     ("user_writes", 7), ("user_mask", bank.used)] + after:
            s.exp(w, v)

    s.c("---- ABORT during the question: declined, staging dropped")
    s.send(s.frame(CMD["PUT_SLOT"], bytes([7]) + _rec("PATIENT", q10)), status=0)
    s.send(s.frame(CMD["COMMIT"], bytes([0])), status=E["PENDING"])
    s.send(s.frame(CMD["ABORT"]), counts=False)
    s.unknown_count("whether ABORT's reply or the 0x48 goes out first, and whether HELLO sends a 0x48, are not fixed")
    for w, v in (("sx_result", E["E_DECLINED"]), ("sx_state", 0), ("sx_staged", 0), ("user_writes", 7),
                 ("user_mask", bank.used)):
        s.exp(w, v)
    s.c("---- HELLO during the question: it resets, the question counts as declined")
    s.send(s.frame(CMD["PUT_SLOT"], bytes([7]) + _rec("PATIENT", q10)), status=0)
    s.send(s.frame(CMD["COMMIT"], bytes([0])), status=E["PENDING"])
    s.send(s.frame(CMD["HELLO"], bytes([1, 3])), counts=False)
    for w, v in (("sx_result", E["E_DECLINED"]), ("sx_state", 0), ("sx_staged", 0), ("user_writes", 7),
                 ("user_mask", bank.used)):
        s.exp(w, v)
    s.send(s.frame(CMD["GET_DIR"]), status=0)

    s.c("---- the flash fails: E_FLASH; the new bank stays in RAM (GET_SLOT shows it) until power-off")
    flash_bank = bank.copy()
    s.raw("flash_fail 1")
    s.send(s.frame(CMD["PUT_SLOT"], bytes([9]) + _rec("RAM ONLY", q10)), status=0)
    s.send(s.frame(CMD["COMMIT"], bytes([0])), status=E["PENDING"])
    s.raw("tap SAVE", "wait 300")
    bank.put(9, "RAM ONLY", q10)
    for w, v in (("sx_result", E["E_FLASH"]), ("sx_status", E["E_FLASH"]), ("sx_state", 0), ("user_mask", bank.used),
                 ("user10", 1)):
        s.exp(w, v)
    s.send(s.frame(CMD["GET_SLOT"], bytes([9])), reply=bytes([0, 9, 1]) + _rec("RAM ONLY", q10), status=0)
    s.raw("flash_fail 0", "wait 100")

    s.c("---- reboot: the bank as last written to the flash (U10 was RAM only); the slots' bytes unchanged")
    s.raw("reboot", "wait 1700", "tour 0")
    s.exp("user_mask", flash_bank.used)
    s.exp("user10", 0)
    s.exp("sx_state", 0)
    s.send(s.frame(CMD["HELLO"], bytes([1, 3])), reply=hello_reply(state=0, flags=HOST_FLAGS), status=0, counts=False)
    for slot in (0, 1, 2, 6):
        nm, q = flash_bank.p[slot]
        s.send(s.frame(CMD["GET_SLOT"], bytes([slot])), reply=bytes([0, slot, 1]) + _rec(nm, q), status=0, counts=False)
    s.send(s.frame(CMD["GET_SLOT"], bytes([9])), reply=bytes([0, 9, 0]), status=0, counts=False)
    s.exp("sx_crc_stale", 0)        # (the host build re-checks GET_DIR's cached bank CRC on every GET_DIR: never stale)
    s.exp("nan", 0)
    return s


def script_safety():
    s = Script("sounds_safety.txt", SCRIPT_HEAD + [
        "# CYMA-1 Sounds T3 (docs/CYMA-1-sounds-spec.md §5.6 P2-P4, P6; §11.1): our SysEx handler never takes, answers or",
        "# is upset by the platform's frames, and the platform's own USB jobs (backup, restore) still work while our",
        "# frames are being handled. Every platform frame below is declined by app_sysex (sx_taken 0: on the device it goes",
        "# on to fm1-base: ann/log/remote/wifi, then ota_service and usb_cmd), gets no reply and changes nothing (the",
        "# wasm test, tools/sounds_smoke.mjs, checks the same through web_cy_sx's return). Damaged frames of ours get",
        "# a NAK and change nothing; reply-range commands are claimed and never answered (no echo loops).",
        "#   run by tools/run_all.sh (step sounds_scen): gen_vectors.py scen --host build/host/cyma-1_host"])
    E = ST
    s.raw("master 4096", "wait 1700", "tour 0")
    s.exp("sx_replies", 0)
    s.c("U01 saved from the panel first, so the backup below holds a bank")
    s.raw("param DECAY 3", "wait 100", "tap SAVE", "wait 200", "tap SAVE", "wait 300")
    s.exp("user_mask", 1)
    s.exp("user_writes", 1)
    s.send(s.frame(CMD["HELLO"], bytes([1, 3])), reply=hello_reply(state=0, flags=HOST_FLAGS), status=0, taken=1)
    rec = _rec("SAFE", _q(1))
    s.send(s.frame(CMD["PUT_SLOT"], bytes([1]) + rec), reply=bytes([0, 1]) + U32(2) + U32(0), status=0)
    s.exp("sx_state", 1)
    s.exp("sx_staged", 2)

    def upd(cmd, body=b""):
        """a frame in the updater's framing: F0 pack7(00 59 cmd len24 body ~sum(body)) F7 (bsp/chip/ota.c)"""
        m = bytes([0, 0x59, cmd]) + len(body).to_bytes(3, "little") + body + bytes([(~sum(body)) & 0xFF])
        f = b"\xF0" + pack7(m) + b"\xF7"
        assert ota_take_accepts(f[1:-1]) and not app_sysex_claims(f[1:-1])
        return f

    platform = [
        ("the M-UPGRADE identity handshake HS_QUERY (cmd 0x11)", HS_QUERY),
        ("a backup request (BU_REQ 0x42, the magic)", upd(0x42, struct.pack("<I", 0x314D5943))),
        ("a restore request (RS_REQ 0x41: length, CRC)", upd(0x41, struct.pack("<II", 3100, 0x12345678))),
        ("a backup's end (BU_END 0x43)", upd(0x43)),
        ("the vectors' backup 0x41 frame", b"\xF0" + NOT_OURS["backup 0x41"] + b"\xF7"),
        ("a mirror request (MR_REQ 0x50, on)", upd(0x50, bytes([1]))),
        ("the mirror's remote (MR_REMOTE 0x54)", upd(0x54, bytes([1, 0, 0, 0, 0, 0]))),
        ("an update read reply (cmd 0x30)", upd(0x30, bytes(range(16)))),
        ("an app command in the update's framing (APP_USB_CMD, cmd 0x60)", upd(0x60, b"CYMA")),
        ("the UBOOT key's body (the ISR takes the real one)", bytes.fromhex("F02224357DF7")),
        ("the OTA (upgrade) key's body (the ISR takes the real one)", bytes.fromhex("F02224357FF7")),
        ("a screen reader's HELLO (7D 53 52)", bytes([0xF0, 0x7D, 0x53, 0x52, 0x01, 0x01, 0xF7])),
        ("the crash log's request (7D 4C 47)", bytes([0xF0, 0x7D, 0x4C, 0x47, 0x01, 0xF7])),
        ("the remote's button frame (7D 4C 43 01): no button is pressed", bytes([0xF0, 0x7D, 0x4C, 0x43, 0x01, 0x7F, 0x01, 0xF7])),
        ("a WiFi frame (7D 4C 57)", bytes([0xF0, 0x7D, 0x4C, 0x57, 0x01, 0xF7])),
        ("LOOM-8's (7D 46 4C)", bytes([0xF0, 0x7D, 0x46, 0x4C, 0x01, 0xF7])),
        ("7D 43 (two bytes of our prefix)", bytes([0xF0, 0x7D, 0x43, 0xF7])),
        ("7D 43 58 (one off)", bytes([0xF0, 0x7D, 0x43, 0x58, 0x01, 0x01, 0x01, 0xF7])),
        ("7C 43 59 (one off)", bytes([0xF0, 0x7C, 0x43, 0x59, 0x01, 0x01, 0x01, 0xF7])),
        ("7D alone", bytes([0xF0, 0x7D, 0xF7])),
        ("an empty frame", bytes([0xF0, 0xF7])),
    ]
    s.c("---- the platform's frames while our staging is open: no reply, no change")
    for what, f in platform:
        s.send(f, quiet=True, note=what, taken=0)
        s.exp("sx_state", 1)
        s.exp("sx_staged", 2)
    s.exp("page", 0)
    s.exp("user_writes", 1)

    s.c("---- damaged frames of ours: a NAK (E_FRAME) with the offending CMD, and nothing changes")
    good = s.frame(CMD["PUT_SLOT"], bytes([2]) + rec)
    seq = good[6]
    bad_crc = bytearray(good)
    bad_crc[-3] ^= 0x01
    s.c("corrupt: the CRC")
    s.send(bytes(bad_crc), taken=1, reply=bytes([E["E_FRAME"], CMD["PUT_SLOT"], 1]), status=E["E_FRAME"])
    s.exp("sx_staged", 2)
    hi = bytearray(good)
    hi[20] |= 0x80
    s.c("corrupt: a byte >= 0x80 inside (USB never delivers one: the handler must still refuse it)")
    s.send(bytes(hi), reply=bytes([E["E_FRAME"], CMD["PUT_SLOT"], 1]), status=E["E_FRAME"])
    spare = bytearray(good)
    spare[-2] |= 0x40
    s.c("corrupt: a spare bit set in the last packed byte")
    s.send(bytes(spare), reply=bytes([E["E_FRAME"], CMD["PUT_SLOT"], 1]), status=E["E_FRAME"])
    s.c("corrupt: a packed length no payload has (one byte more than the frame)")
    k = (1 - len(good[7:-1])) % 8 or 8                  # zero bytes to a packed length m = 1 mod 8: no n packs to it
    longer = good[:-1] + bytes(k) + good[-1:]
    s.send(longer, reply=bytes([E["E_FRAME"], CMD["PUT_SLOT"], 1]), status=E["E_FRAME"])
    s.c("corrupt: cut short (the CRC gone)")
    s.send(good[:-6] + good[-1:], reply=bytes([E["E_FRAME"], CMD["PUT_SLOT"], 1]), status=E["E_FRAME"])
    s.c("corrupt: header only, no body at all")
    s.send(good[:7] + good[-1:], reply=bytes([E["E_FRAME"], CMD["PUT_SLOT"], 1]), status=E["E_FRAME"])
    s.exp("sx_state", 1)
    s.exp("sx_staged", 2)
    s.c("corrupt (VER 2): a NAK E_VERSION, nothing changes")
    v2 = bytearray(good)
    v2[4] = 2
    s.send(bytes(v2), reply=bytes([E["E_VERSION"], CMD["PUT_SLOT"], 1]), status=E["E_VERSION"])
    s.exp("sx_staged", 2)
    s.c("---- frames in the reply range (CMD >= 0x40) are claimed and never answered: no echo loops")
    for cmd, pl in ((0x41, hello_reply()), (0x45, bytes([0, 1]) + U32(2) + U32(0)), (0x48, bytes([0]) + U32(1)),
                    (0x7F, bytes([E["E_FRAME"], 5, 1])), (0x40, b""), (0x7E, b"\x01")):
        s.send(sx_frame(cmd, 0x33, pl), quiet=True, taken=1)
        s.exp("sx_staged", 2)
    s.c("and the good frame still works after all that")
    s.send(good, reply=bytes([0, 2]) + U32(6) + U32(0), status=0)
    s.exp("sx_staged", 6)

    s.c("---- the platform's backup over USB while our staging is open: it works, and the staging survives it")
    s.raw("backup out:sounds_safety_bank.bin")
    s.exp("sx_state", 1)
    s.exp("sx_staged", 6)
    s.exp("user_writes", 1)

    s.c("---- a question open, the identity handshake arrives (an installer polling): not ours, the question stays")
    fc = s.frame(CMD["COMMIT"], bytes([0]))
    s.send(fc, reply=bytes([E["PENDING"]]), status=E["PENDING"])
    s.exp("sx_state", 2)
    s.send(HS_QUERY, quiet=True, taken=0)
    s.exp("sx_state", 2)
    s.c("---- then a restore over USB starts: app_panic and app_backup_restored close our question (declined) and drop")
    s.c("the staging; the restore itself completes (restore_rc 0) and nothing of ours was written")
    s.raw("restore out:sounds_safety_bank.bin", "wait 200")
    for w, v in (("restore_rc", 0), ("sx_state", 0), ("sx_staged", 0), ("sx_result", E["E_DECLINED"]),
                 ("user_mask", 1), ("user_writes", 1)):
        s.exp(w, v)
    s.unknown_count("whether an abort by the platform sends the 0x48 is not fixed by the spec")
    s.c("---- our protocol still answers afterwards")
    s.send(s.frame(CMD["HELLO"], bytes([1, 3])), reply=hello_reply(state=0, flags=HOST_FLAGS), status=0)
    s.send(s.frame(CMD["GET_DIR"]), status=0)
    s.c("---- frames too short for a header: claimed (our prefix), nothing changes, no crash")
    for h in ("F07D4359F7", "F07D435901F7", "F07D43590105F7"):
        s.c("corrupt: %d bytes between F0 and F7" % (len(h) // 2 - 2))
        s.raw("sysex " + h)
        s.exp("sx_taken", 1)
    s.exp("sx_state", 0)
    s.exp("user_mask", 1)
    s.exp("user_writes", 1)
    s.exp("nan", 0)
    script_flood(s, platform)
    return s


FLOOD_N = 240                                        # the flood's frames (one in ten the platform's)


def script_flood(s, platform):
    """the hardening round (spec revision 2026-10-24b): a host that floods our requests without waiting for replies,
    with the platform's frames mixed in. The host simulator has no USB send ring (on the device app_sx_send now keeps
    half of fm1-base's ring free for the platform's own replies and has one frame of ours in it at a time; that is
    measured with fm1-base's own usb.c, outside this script). What the simulator does show: under the flood every
    platform frame is still declined at once (sx_taken 0) and gets nothing from us, every frame of ours gets exactly
    its reply, byte for byte, and GET_DIR's bank CRC (cached by the hardening round's firmware until the bank changes)
    is the CRC of the bank as it is, across a USB commit, a panel SAVE, a USB restore and a reboot. Every frame below
    goes in back to back: no main-loop pass between them unless a "wait" says so"""
    E = ST
    rnd = random.Random(20261024)
    s.c("")
    s.c("==== a flood (the hardening round, spec revision 2026-10-24b): %d frames back to back, no main-loop pass" % FLOOD_N)
    s.c("between them, one in ten the platform's. Each platform frame: declined at once (sx_taken 0), no reply from us,")
    s.c("nothing changes. Each frame of ours: exactly its reply, byte for byte. GET_DIR's CRC is always the bank's as it")
    s.c("is now (the firmware may cache it: across a USB commit, a panel SAVE, a restore and a reboot it must follow).")
    s.c("(The USB send ring is the device's, not the simulator's: the reserve app_sx_send keeps for the platform's own")
    s.c("replies is measured against fm1-base's usb.c, see the spec's revision 2026-10-24b.)")
    s.raw("reboot", "wait 1700", "tour 0")
    s.replies = 0
    s.exp("sx_replies", 0)
    s.exp("sx_state", 0)
    s.exp("user_mask", 1)
    s.c("GET_DIR once with the bank as the flash has it (U01 from the panel), so a cached CRC exists before it changes")
    s.send(s.frame(CMD["GET_DIR"]), status=0)
    s.c("U01 erased over USB (USB: DELETE U01?), SAVE: the bank is empty, and known to the model from here")
    s.send(s.frame(CMD["ERASE_SLOT"], bytes([0])), reply=bytes([0, 0]) + U32(0) + U32(1), status=0)
    fc = s.frame(CMD["COMMIT"], bytes([0]))
    s.send(fc, reply=bytes([E["PENDING"]]), status=E["PENDING"])
    s.raw("tap SAVE", "wait 300")
    s.got_async()
    bank = BankModel()
    bank.magic, bank.version = CY_USER_MAGIC, 1
    last_seq, writes = fc[6], 1
    s.exp("sx_result", 0)
    s.exp("user_mask", 0)
    s.exp("user_writes", writes)
    s.send(s.frame(CMD["GET_DIR"]), reply=_dir_reply(bank, last_result=0, last_seq=last_seq), status=0,
           note="the CRC after the USB commit: the empty bank's, not the cached one of the bank before")
    s.c("three sounds over USB (U01, U04, U32), SAVE")
    trio = [(0, "FLOOD A", _q(10)), (3, "FLOOD B", _q(11)), (31, "FLOOD C", _q(12))]
    staged = 0
    for slot, name, q in trio:
        staged |= 1 << slot
        s.send(s.frame(CMD["PUT_SLOT"], bytes([slot]) + _rec(name, q)), reply=bytes([0, slot]) + U32(staged) + U32(0),
               status=0)
    fc = s.frame(CMD["COMMIT"], bytes([0]))
    s.send(fc, reply=bytes([E["PENDING"]]), status=E["PENDING"])
    s.raw("tap SAVE", "wait 300")
    s.got_async()
    for slot, name, q in trio:
        bank.put(slot, name, q)
    last_seq, writes = fc[6], writes + 1
    s.exp("user_mask", bank.used)
    s.exp("user_writes", writes)
    s.c("the platform's USB backup of this bank, for the restore after the flood")
    s.raw("backup out:sounds_flood_bank.bin")
    trio_bank = bank.copy()
    s.c("factory 01 loaded, then a sound played over USB (PUT_EDIT): GET_EDIT's reply is known too")
    s.raw("preset 1", "wait 400")
    q_edit = _q(13)
    s.send(s.frame(CMD["PUT_EDIT"], bytes([0]) + _rec("FLOOD EDIT", q_edit)), reply=bytes([0]), status=0)
    s.raw("wait 400")
    edit = [0, 3]                                     # GET_EDIT: preset index, flags (edited + auditioned)

    def dir_now():
        return _dir_reply(bank, last_result=0, last_seq=last_seq)

    def one_of_ours(i):
        """a request of ours, its exact reply payload (None: claimed and never answered), or a damaged frame (NAK)"""
        k = rnd.randrange(100)
        if k < 30:
            slot = rnd.choice([0, 3, 31, rnd.randrange(32)])
            e = bank.p[slot]
            return s.frame(CMD["GET_SLOT"], bytes([slot])), (bytes([0, slot, 1]) + _rec(*e) if e else bytes([0, slot, 0]))
        if k < 50:
            return s.frame(CMD["GET_DIR"]), dir_now()
        if k < 70:
            return s.frame(CMD["GET_EDIT"]), bytes([0, edit[0], edit[1]]) + _rec("FLOOD EDIT", q_edit)
        if k < 78:
            return s.frame(CMD["HELLO"], bytes([1, 3])), hello_reply(state=0, flags=HOST_FLAGS)
        if k < 90:                                    # damaged: a bit of the CRC flipped -> a NAK, never cached
            f = bytearray(s.frame(CMD["GET_SLOT"], bytes([rnd.randrange(32)])))
            f[-3] ^= 1 << rnd.randrange(7)
            return bytes(f), "nak"
        if k < 95:                                    # a reply's CMD (an echo): claimed, never answered
            return sx_frame(0x40 | rnd.choice([1, 2, 4, 7]), rnd.randrange(128), bytes([0])), None
        return s.frame(CMD["GET_SLOT"], bytes([40 + rnd.randrange(80)])), bytes([E["E_ARG"]])   # past U32: E_ARG

    prev = None
    n_platform = n_ours = 0
    for i in range(FLOOD_N):
        if i == FLOOD_N // 2:
            s.c("---- mid-flood: U08 staged and committed (SAVE pressed: the only main-loop pass in the flood); the very")
            s.c("next GET_DIR has the new bank's CRC and the COMMIT's result")
            rec8 = _rec("FLOOD MID", _q(14))
            s.send(s.frame(CMD["PUT_SLOT"], bytes([7]) + rec8), reply=bytes([0, 7]) + U32(1 << 7) + U32(0), status=0)
            s.send(s.frame(CMD["GET_DIR"]), reply=_dir_reply(bank, staged=1 << 7, state=1, last_result=0,
                                                             last_seq=last_seq), status=0)
            fc = s.frame(CMD["COMMIT"], bytes([0]))
            s.send(fc, reply=bytes([E["PENDING"]]), status=E["PENDING"])
            s.raw("tap SAVE", "wait 300")
            s.got_async()
            bank.put(7, "FLOOD MID", _q(14))
            last_seq, writes = fc[6], writes + 1
            s.send(s.frame(CMD["GET_DIR"]), reply=dir_now(), status=0)
            s.exp("user_writes", writes)
        if i == (3 * FLOOD_N) // 4:
            s.c("---- and a SAVE SAVE on the panel: the auditioned sound goes to the first free slot (U02); GET_DIR follows")
            s.raw("tap SAVE", "wait 200", "tap SAVE", "wait 300")
            free = bank.p.index(None)
            bank.put(free, "FLOOD EDIT", q_edit)
            edit[0], edit[1] = 24 + free, 0
            writes += 1
            s.exp("user_writes", writes)
            s.exp("user_mask", bank.used)
            s.send(s.frame(CMD["GET_DIR"]), reply=dir_now(), status=0)
        if i % 10 == 9:
            what, f = platform[(i // 10) % len(platform)]
            s.send(f, quiet=True, taken=0, note="platform: " + what)
            n_platform += 1
            continue
        f, rep = one_of_ours(i)
        n_ours += 1
        if rep == "nak":
            s.send(f, reply=bytes([E["E_FRAME"], f[5], 1]), status=E["E_FRAME"], taken=1, note="corrupt: one CRC bit")
        elif rep is None:
            s.send(f, quiet=True, taken=1)
        else:
            s.send(f, reply=rep, status=rep[0], taken=1)
            prev = (f, rep)
        if prev and rnd.randrange(8) == 0 and rep not in (None, "nak") and f[5] != CMD["HELLO"]:
            s.send(prev[0], reply=prev[1], status=prev[1][0], taken=1, note="the same request again (a host's retry): "
                   "the same reply")
            n_ours += 1
    s.c("---- after the flood: nothing staged, the bank as the model says, every write a SAVE press")
    s.send(s.frame(CMD["GET_DIR"]), reply=dir_now(), status=0)
    s.exp("user_writes", writes)
    s.exp("sx_staged", 0)
    s.c("---- the platform restores the backup taken before the flood (U01, U04, U32): GET_DIR's CRC follows the bank")
    s.c("the restore put back (nothing was open on our side, so nothing of ours is sent)")
    s.raw("restore out:sounds_flood_bank.bin", "wait 200")
    bank = trio_bank
    s.exp("restore_rc", 0)
    s.exp("user_mask", bank.used)
    s.exp("user_writes", writes)
    s.send(s.frame(CMD["GET_DIR"]), reply=dir_now(), status=0)
    for slot in range(32):
        e = bank.p[slot]
        if e:
            s.send(s.frame(CMD["GET_SLOT"], bytes([slot])), reply=bytes([0, slot, 1]) + _rec(*e), status=0)
    s.raw("reboot", "wait 1700", "tour 0")
    s.replies = 0
    s.exp("user_mask", bank.used)
    s.exp("sx_state", 0)
    s.send(s.frame(CMD["GET_DIR"]), reply=_dir_reply(bank, last_result=0, last_seq=0xFF), status=0,
           note="after a reboot: the bank as written, its CRC computed again (no COMMIT since the boot: SEQ 0xFF)")
    for w, v in (("sx_staged", 0), ("user_writes", 0), ("sx_crc_stale", 0), ("nan", 0)):
        s.exp(w, v)
    s.c(f"(the flood: {n_ours} frames of ours, {n_platform} of the platform's)")


def all_scripts():
    return [script_sysex(), script_safety()]


def run_scenarios(app, host, out):
    """the two scripts through the host simulator, as tools/golden.py runs its scenarios (its #@ notes honoured)"""
    sys.path.insert(0, os.path.join(app, "tools"))
    import golden                                         # noqa: E402  (the app's own harness)

    class Ctx:
        pass
    ctx = Ctx()
    ctx.out, ctx.host = out, host
    os.makedirs(out, exist_ok=True)
    bad = 0
    for name in ("sounds_sysex", "sounds_safety"):
        path = os.path.join(app, "tests", name + ".txt")
        r = golden.run_script(ctx, name, path)
        notes_bad = [d for d in r["directives"] if not d["pass"]]
        print(f"  {'ok  ' if r['pass'] else 'FAIL'} {name}: rc {r['rc']}, {r['oks']} expects ok, "
              f"{len(r['fails'])} failed, {len(r['directives']) - len(notes_bad)}/{len(r['directives'])} log notes; "
              f"{r['sim_ms'] / 1000:.0f} s simulated (log {r['log']})")
        for f in r["fails"][:25]:
            print("       " + f)
        for d in notes_bad[:25]:
            print(f"       note {d['note'][:110]}  ({d['why']})")
        bad += not r["pass"]
    return bad


# ============================================================================================== commands
def table_path(args):
    return args.params or os.path.join(args.app, "tools", "cyma1_params.json")


def cmd_check(args):
    tp = table_path(args)
    if not os.path.exists(tp):
        print(f"gen_vectors: no {tp} yet (FW generates it with the host command params_json): SKIP")
        return 77
    load_table(tp)
    vpath = os.path.join(args.app, "tests", "sounds", "vectors.json")
    with open(vpath, encoding="utf-8") as f:
        frozen = json.load(f)
    fails = []
    log = print
    mine = build_vectors(args.quick)
    for k in sorted(set(frozen) | set(mine)):
        if k == "writer_digest" and args.quick:
            continue
        if frozen.get(k) != mine.get(k):
            sub = ""
            if isinstance(frozen.get(k), list) and isinstance(mine.get(k), list):
                diff = [i for i, (a, b) in enumerate(zip(frozen[k], mine[k])) if a != b]
                sub = f" (first differing entry {diff[0] if diff else 'length'})"
            elif isinstance(frozen.get(k), dict) and isinstance(mine.get(k), dict):
                diff = [x for x in sorted(set(frozen[k]) | set(mine[k])) if frozen[k].get(x) != mine[k].get(x)]
                sub = f" ({', '.join(map(str, diff[:5]))})"
            fails.append(f"vectors.json \"{k}\" differs from what {os.path.basename(tp)} gives{sub}")
    log("vectors.json: re-derived from %s: %s" % (os.path.relpath(tp, args.app),
                                                  "identical" if not fails else "DIFFERENT"))
    fails += reference_checks(args.quick, log)
    hpath = os.path.join(args.app, "tests", "sounds", "vectors.h")
    want_h = make_header(frozen)
    try:
        with open(hpath, encoding="utf-8") as f:
            have_h = f.read()
    except OSError:
        have_h = None
    if have_h != want_h:
        fails.append("tests/sounds/vectors.h is not the header of vectors.json (gen_vectors.py header)")
    else:
        log("vectors.h: the header of vectors.json")
    for sc in all_scripts():
        try:
            with open(os.path.join(args.app, "tests", sc.path), encoding="utf-8") as f:
                have = f.read()
        except OSError:
            have = None
        if have != sc.text():
            fails.append(f"tests/{sc.path} is not what the generator writes (gen_vectors.py scripts)")
    fails += check_scripts(args.app, log)
    fails += check_docs(args.app, frozen, log)
    if fails:
        print("FAIL\n  " + "\n  ".join(fails))
        return 1
    print("gen_vectors: all checks ok")
    return 0


RETIRED_WORDS = [                                    # revision 2026-10-24b, R3: the flash rule as it really is
    "Nothing is written without a SAVE press",
    "nothing from USB is written without",
    "Nothing from USB is written to the FM-1's memory until",
    "No flash write from USB without",
]


def check_docs(app, vec, log):
    """the user's guide and the README quote the vectors exactly, and no longer use the retired wording"""
    fails, seen = [], 0
    for rel in ("docs/SOUNDS.md", "README.md"):
        try:
            with open(os.path.join(app, rel), encoding="utf-8") as f:
                t = f.read()
        except OSError:
            continue
        seen += 1
        flat = " ".join(t.split())
        for w in RETIRED_WORDS:
            if w.lower() in flat.lower():
                fails.append(f"{rel} still says \"{w}…\" (the user bank is never written without SAVE; an audition is "
                             f"remembered by the settings autosave: spec revision 2026-10-24b, R3)")
        if rel == "docs/SOUNDS.md":
            for what, code in (("SINGING BOWL's code", vec["factory"][0]["code"]),
                               ("SINGING BOWL's link code", (vec.get("cy2") or {}).get("factory", [{}])[0].get("code"))):
                if code and code not in t:
                    fails.append(f"{rel} does not quote {what} as vectors.json has it")
            for c in re.findall(r"\bcy[12]\.[A-Za-z0-9_-]{10,}", t):
                try:
                    any_decode(c)
                except FormatError as e:
                    fails.append(f"{rel} quotes a code that does not decode ({e.code}): {c[:24]}…")
    if seen:
        log(f"docs: {seen} files quote the vectors and use the revised wording" if not fails else "docs: DIFFERENT")
    return fails


def cmd_header(args):
    tp = table_path(args)
    load_table(tp)
    with open(os.path.join(args.app, "tests", "sounds", "vectors.json"), encoding="utf-8") as f:
        vec = json.load(f)
    out = os.path.join(args.app, "tests", "sounds", "vectors.h")
    with open(out, "w", encoding="utf-8") as f:
        f.write(make_header(vec))
    print(f"wrote {out}")
    return 0


def cmd_write(args):
    load_table(table_path(args))
    frozen = os.path.abspath(os.path.join(args.app, "tests", "sounds", "vectors.json"))
    if os.path.abspath(args.out) == frozen and not args.force:
        print("gen_vectors: vectors.json is frozen (§12.2 I2): pass --force to overwrite it")
        return 2
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(json.dumps(build_vectors(args.quick), indent=1) + "\n")
    print(f"wrote {args.out}")
    return 0


def cmd_scripts(args):
    load_table(table_path(args))
    for sc in all_scripts():
        p = os.path.join(args.app, "tests", sc.path)
        with open(p, "w", encoding="utf-8") as f:
            f.write(sc.text())
        print(f"wrote {p} ({len(sc.L)} lines)")
    return 0


def cmd_scen(args):
    host = args.host or os.path.join(args.app, "build", "host", "cyma-1_host")
    if not os.access(host, os.X_OK):
        print(f"gen_vectors scen: no host simulator {host}: SKIP")
        return 77
    with open(os.path.join(args.app, "app.c"), encoding="utf-8") as f:
        body = f.read()
    if "app_sysex" not in body and not os.path.exists(os.path.join(args.app, "app_sounds.c")):
        print("gen_vectors scen: this app.c has no app_sysex yet (FW's app_sounds.c): SKIP")
        return 77
    out = args.out or os.path.join(args.app, "build", "sounds_scen")
    bad = run_scenarios(args.app, host, out)
    print(f"sounds scenarios: {2 - bad}/2 pass")
    return 1 if bad else 0


def cmd_frame(args):
    pl = bytes.fromhex(args.payload) if args.payload else b""
    print(sx_frame(int(args.fcmd, 16), int(args.seq, 16), pl).hex().upper())
    return 0


def cmd_rec(args):
    if args.what.startswith("factory:"):
        load_table(table_path(args))
        f = FACTORY[int(args.what[8:]) - 1]
        print(rec_encode(f["name"], f["q16"]).hex().upper())
    else:
        name, _, vals = args.what.partition("=")
        print(rec_encode(name, [int(x) for x in vals.split(",")]).hex().upper())
    return 0


def cmd_parse(args):
    b = bytes.fromhex(args.hex)
    try:
        ver, cmd, seq, pl = sx_parse(b)
    except FormatError as e:
        print(f"not a good frame of ours: {e.code}")
        return 1
    names = {v: k for k, v in CMD.items()}
    print(f"VER {ver} CMD {cmd:02X} ({names.get(cmd & 0x3F if cmd != 0x7F else cmd, '?')}"
          f"{' reply' if cmd & 0x40 and cmd != 0x7F else ''}) SEQ {seq} payload ({len(pl)} B) {hx(pl)}")
    return 0


def main(argv=None):
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--app", default=APP_DEFAULT, help="the app directory (default: this file's tree)")
    common.add_argument("--params", help="the parameter table (default APP/tools/cyma1_params.json)")
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", metavar="COMMAND")
    c = sub.add_parser("check", parents=[common])
    c.add_argument("--quick", action="store_true")
    sub.add_parser("header", parents=[common])
    w = sub.add_parser("write", parents=[common])
    w.add_argument("--out", required=True)
    w.add_argument("--force", action="store_true")
    w.add_argument("--quick", action="store_true")
    f = sub.add_parser("frame", parents=[common])
    f.add_argument("fcmd", metavar="CMD")
    f.add_argument("seq")
    f.add_argument("payload", nargs="?")
    r = sub.add_parser("rec", parents=[common])
    r.add_argument("what")
    p = sub.add_parser("parse", parents=[common])
    p.add_argument("hex")
    sub.add_parser("scripts", parents=[common])
    sc = sub.add_parser("scen", parents=[common])
    sc.add_argument("--host")
    sc.add_argument("--out")
    argv = list(sys.argv[1:] if argv is None else argv)
    if not any(a in sub.choices for a in argv) and not any(a in ("-h", "--help") for a in argv):
        argv = ["check"] + argv
    args = ap.parse_args(argv)
    args.app = os.path.abspath(args.app)
    return {"check": cmd_check, "header": cmd_header, "write": cmd_write, "frame": cmd_frame, "rec": cmd_rec,
            "parse": cmd_parse, "scripts": cmd_scripts, "scen": cmd_scen}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
