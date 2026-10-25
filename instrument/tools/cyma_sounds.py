#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""CYMA-1 Sounds on the command line: save, restore and share the sounds on an FM-1 running CYMA-1, over USB-MIDI
(docs/CYMA-1-sounds-spec.md §9.2, the frozen interface I8).

    cyma_sounds.py [--port NAME] [--virtual [--wasm PATH] [--virtual-answer yes|no|none]] [--yes] [-q] COMMAND ...
      ports                                  candidate MIDI ports (fm1-base's PORT_RE only: never probes other gear)
      info                                   identity, HELLO, the slot list (read-only)             (alias: list)
      get (edit|U01..U32|all) [-o FILE]     one sound -> NAME.cyma1.json; all -> "FM-1 YYYY-MM-DD HH-MM.cyma1-bank.json"
      audition SOURCE                        PUT_EDIT: plays on the FM-1, nothing saved
      put SOURCE [--slot Uxx|free]           PUT_SLOT + COMMIT (the FM-1 asks); default free
      push-bank BANK [--replace] [--no-backup] [--dry-run]
                                             add to free slots, or --replace all; a safety backup of the FM-1's bank
                                             first (~/Music/CYMA-1/backups/FM-1 YYYY-MM-DD HH-MM-SS.cyma1-bank.json)
      erase Uxx                              ERASE_SLOT + COMMIT
      encode FILE [--base URL] [--link auto|cy1|cy2]
                                             prints the code (cy1.) and, with --base, the link; auto (the default)
                                             adds the short cy2. link when it fits claude.ai's 128-character anchor
      decode SOURCE [-o FILE]                code or link (cy1. or cy2.) -> NAME.cyma1.json
      validate FILE...                       offline check of sound and bank files (§3.3 rules); prints notes
      selftest                               the codec against tests/sounds/vectors.json
      backup [-o FILE]                       alias: get all, into the backups folder by default
      restore BANK [--no-backup] [--dry-run] alias: push-bank BANK --replace
    SOURCE = a .cyma1.json file, a cy1. code, a link (#cy1. or #cy2.), or text holding one

Codes and links (spec §4; cy2. is the page's short link):
  - The code this tool prints is always cy1. (126-140 characters), the spec's share code.
  - cy2. is the short form for links: the same sound, with each value that equals param_set 1's default left out.
    claude.ai passes a link's anchor (the text after #) into the page only when it is at most 128 characters
    (measured 2026-10-24), and most cy1. codes are longer (126-140 characters; only short names fit), so a link
    that should open inside claude.ai uses cy2.
    A sound that changes many settings may still not fit: then share the code or the file.
  - Both decode everywhere this tool reads a code: a bare code, a link, or text holding either.

Safety (spec §5.6, §10):
  - The first frame to any port is fm1-base's standard identity query (HS_QUERY, what M-UPGRADE sends). Only an FM-1
    that answers as CYMA-1 (FM-1_94...) gets anything else, so a stock unit, an update loader or another firmware
    receives exactly that one query.
  - Every frame goes through a send allowlist: HS_QUERY, or F0 7D 43 59 01 ... F7 of at most 306 bytes. This tool
    cannot send the update or bootloader keys, the remote-control frames, or anything in the updater's framing.
  - The FM-1 never writes its user presets from USB without a press of SAVE on the FM-1 itself (a sound only
    auditioned is remembered by the settings autosave, as a knob turn is). --yes skips only this computer's
    question; the FM-1 always asks.
  - --dry-run sends only reads (identity, HELLO, GET_DIR, GET_SLOT) and prints the plan.
  - Files are never overwritten without --force.
  - Text from files and from the FM-1 (credits, notes, bank names, unknown keys, the identity) is shown without
    control characters or Unicode format characters (bidi overrides, zero-width characters), so it cannot reorder or
    hide what the terminal shows; the identity's other bytes are shown as \\xNN.
  - Never use this during a firmware install. On macOS Live can stay open during sound transfers (CoreMIDI shares the
    port); a note from Live that lands inside a frame only costs a retry.

--virtual runs the real firmware (app.wasm) in node as a virtual FM-1 (tools/sx_bridge.mjs), so every command can be
tried, and tested, without hardware. Nothing then touches MIDI at all.

Exit codes (as fm1_backup.py): 0 done; 1 declined, timed out or cancelled; 2 bad arguments, file or code; 3 FM-1 not
found; 4 connection lost or no answer; 5 busy (E_BUSY, E_HOLD or E_ASKING); 6 wrong firmware, protocol or a locked
bank.

Device commands need mido with python-rtmidi (the project's .venv312 has them). The offline commands (encode, decode,
validate, selftest) need only Python 3.9+.
"""
from __future__ import annotations

import argparse
import base64
import datetime
import hashlib
import json
import math
import os
import queue
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unicodedata
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
APP_DIR = HERE.parent
CLI_VERSION = "DEV"                     # made_with.version of files this tool makes ("DEV" unless a release stamps it)
TABLE_PATH = HERE / "cyma1_params.json"
VECTORS_PATH = APP_DIR / "tests" / "sounds" / "vectors.json"
BRIDGE_PATH = HERE / "sx_bridge.mjs"
BACKUP_DIR = Path.home() / "Music" / "CYMA-1" / "backups"

EXIT_OK, EXIT_NO, EXIT_ARGS, EXIT_NOTFOUND, EXIT_LOST, EXIT_BUSY, EXIT_FIRMWARE = 0, 1, 2, 3, 4, 5, 6

# seconds (spec §5.5 host policy, frozen); the tests shorten them
DELAY = {"reply": 0.3, "hello": 1.0, "retries": 3, "commit": 35.0, "poll": 2.0, "hs": 1.0, "open": 0.3,
         "bridge_ready": 20.0, "bridge_quit": 5.0}

FILE_MAX = 1 << 20                      # 1 MiB (spec §3.3 rule 1, §10)
TEXT_MAX = 4096                         # a pasted code or link
BANK_MAX = 256                          # sounds per bank file
PARAMS_KEYS_MAX = 128
AUTHOR_MAX, NOTES_MAX = 32, 280
N_USER = 32


class CymaError(Exception):
    """a failure the user is told about in plain words, with its exit code"""

    def __init__(self, code, msg):
        super().__init__(msg)
        self.code = code


class FormatError(ValueError):
    """a codec refusal: notcode / damaged / newer (codes, records); notours / version / frame (SysEx)"""

    def __init__(self, code):
        super().__init__(code)
        self.code = code


# ================================================================================ the parameter table (spec §3.6)
class Table:
    """tools/cyma1_params.json (generated from instrument/src by the host's params_json; schema I1)"""

    def __init__(self, path=TABLE_PATH):
        try:
            raw = json.loads(Path(path).read_text(encoding="utf-8"))
        except FileNotFoundError:
            raise CymaError(EXIT_ARGS, f"{path} is missing: it is generated with the instrument "
                                       f"(the host build's params_json command)") from None
        except (OSError, ValueError) as e:
            raise CymaError(EXIT_ARGS, f"{path} is not readable: {e}") from None
        self.path = Path(path)
        self.params = raw["params"]
        self.factory = raw.get("factory", [])
        self.param_set = raw.get("param_set", 1)
        if [p["id"] for p in self.params] != list(range(len(self.params))) or len(self.params) < 1:
            raise CymaError(EXIT_ARGS, f"{path}: the parameter ids are not 0..N-1")
        self.n = len(self.params)
        self.def_q16 = [p["def_q16"] for p in self.params]
        self.keys = {p["key"]: p for p in self.params}
        self.aliases = {}
        for p in self.params:
            a = norm_key(p["name"])
            if a != p["key"]:
                self.aliases[a] = p["key"]


_TABLE = None


def table():
    global _TABLE
    if _TABLE is None:
        _TABLE = Table()
    return _TABLE


# ======================================================================================== names (spec §3.4)
NAME_MAX = 14
NAME_SET = frozenset("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -.'&+/!?#():")


def clean_name(s):
    """the one name rule (JS and Python do the NFKD step; the device's C drops bytes >= 0x80 instead)"""
    if isinstance(s, (bytes, bytearray)):
        s = bytes(s).decode("latin-1")
    s = unicodedata.normalize("NFKD", str(s))
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


def file_stem(name):
    """a filename from a cleaned name: / \\ : ? * " < > | become -"""
    return re.sub(r'[/\\:?*"<>|]', "-", name)


def slot_name(i):
    return "U%02d" % (i + 1)


def parse_slot(s):
    """'U07' / 'u7' / '7' -> 6; CymaError otherwise"""
    m = re.fullmatch(r"[Uu]?0*(\d{1,2})", str(s).strip())
    if not m or not 1 <= int(m[1]) <= N_USER:
        raise CymaError(EXIT_ARGS, f"{s!r} is not a slot: U01..U32")
    return int(m[1]) - 1


# ============================================================================== the parameter curves (spec §3.2)
def grid(p):
    if p["curve"] == "ENUM":
        return max(p["detents"] - 1, 1)
    if p["curve"] == "MORPH":
        return p["detents"] * int(p["hi"] - p["lo"] + 0.5)
    return p["detents"] or 1


Q16_NUDGE = 1e-6                        # exact halves round up whatever the last ulp of log2 says


def clamp01(n):
    return 0.0 if not n > 0.0 else 1.0 if n > 1.0 else n


def q16(n):
    """norm -> stored value: round half up, a half within 1e-6 counted as a half"""
    return int(math.floor(clamp01(n) * 65535.0 + 0.5 + Q16_NUDGE))


def q16_risky(n):
    y = clamp01(n) * 65535.0 + 0.5 + Q16_NUDGE
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
    lo, hi, d = p["lo"], p["hi"], grid(p)
    if p["curve"] == "LOG":
        if not v > lo:
            return 0.0
        n = math.log2(v / lo) / math.log2(hi / lo)
    else:
        n = (v - lo) / (hi - lo)
    n = clamp01(n)
    if p["curve"] == "ENUM" or p["int"]:
        n = math.floor(n * d + 0.5) / d
    return n


def num_out(x):
    """a JSON number: an integer when whole (JS and Python then print the same text)"""
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
        return int(math.floor(v + 0.5))
    for d in range(0, 7):
        k = 10 ** d
        s = math.floor(abs(v) * k + 0.5) / k
        s = -s if v < 0 else s
        n2 = norm_of(p, s)
        if q16(n2) == q and (p["curve"] != "LOG" or not q16_risky(n2)):
            return num_out(s)
    return num_out(v)


def norm_key(k):
    return re.sub(r"_+", "_", re.sub(r"[^a-z0-9]", "_", str(k).lower())).strip("_")


def label_key(s):
    return re.sub(r"[^A-Z0-9]", "", str(s).upper())


def from_json_value(p, v):
    """-> (q16, note): note None, 'clamped' or 'default' (spec §3.3 reader rule 3)"""
    t = table()
    if p["curve"] in ("ENUM", "MORPH") and isinstance(v, str) and p["labels"]:
        labs = [label_key(s) for s in p["labels"]]
        if label_key(v) in labs:
            k = labs.index(label_key(v))
            return (q16(k / grid(p)) if p["curve"] == "ENUM" else q16(norm_of(p, p["lo"] + k))), None
        return t.def_q16[p["id"]], "default"
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        return t.def_q16[p["id"]], "default"
    if isinstance(v, int) and abs(v) > 1e300:   # (JSON.parse reads such a literal as Infinity)
        return t.def_q16[p["id"]], "default"
    if not math.isfinite(v):
        return t.def_q16[p["id"]], "default"
    if p["curve"] == "ENUM":                    # an index, clamped
        k = min(max(int(math.floor(v + 0.5)), 0), grid(p))
        return q16(k / grid(p)), (None if k == v else "clamped")
    c = min(max(float(v), p["lo"]), p["hi"])
    return q16(norm_of(p, c)), (None if c == v else "clamped")


# ===================================================================================== sounds and notes
class Sound:
    """a sound: its cleaned name and its stored values (q16, param_set order); plus what the file said about it"""

    def __init__(self, name, q, author=None, slot=None, notes=None, text=None, created=None):
        self.name = clean_name(name)
        self.q = list(q)
        self.author = author
        self.slot = slot                   # 0..31 or None
        self.notes = notes or Notes()      # what reading it changed
        self.text = text                   # a file's free-text notes (shown, never stored)
        self.created = created

    def same(self, other):
        return other is not None and self.name == other.name and self.q == other.q

    def __repr__(self):
        return f"Sound({self.name!r})"


class Notes:
    """what reading a sound changed, for the import note"""

    def __init__(self):
        self.skipped = []                  # unknown keys (as written)
        self.missing = []                  # parameter keys not in the file
        self.clamped = []                  # parameter keys
        self.defaulted = []                # parameter keys (wrong type, NaN, unknown label)
        self.extra = 0                     # a record's values beyond this table (a newer CYMA-1)
        self.older = 0                     # a record's missing values (an older CYMA-1)
        self.other = []                    # anything else, as sentences

    def __bool__(self):
        return bool(self.skipped or self.missing or self.clamped or self.defaulted or self.extra or self.older
                    or self.other)

    def messages(self):
        t = table()
        disp = lambda keys: ", ".join(t.keys[k]["name"] if k in t.keys else shown_key(k).upper() for k in keys)
        out = []
        if self.skipped:
            n = len(self.skipped)
            out.append(f"Made with a newer CYMA-1: {n} of its settings {'are' if n != 1 else 'is'}n't known here and "
                       f"{'were' if n != 1 else 'was'} left out ({disp(self.skipped[:12])}"
                       f"{', ...' if n > 12 else ''}).")
        if self.extra:
            out.append(f"Made with a newer CYMA-1: {self.extra} of its settings "
                       f"{'are' if self.extra != 1 else 'is'}n't known here and "
                       f"{'were' if self.extra != 1 else 'was'} left out.")
        if self.older or self.missing:
            n = self.older or len(self.missing)
            names = f" ({disp(self.missing[:12])}{', ...' if len(self.missing) > 12 else ''})" if self.missing else ""
            out.append(f"Made with an older CYMA-1: {n} setting{'s' if n != 1 else ''} it doesn't have use "
                       f"{'their defaults' if n != 1 else 'its default'}{names}.")
        if self.clamped:
            n = len(self.clamped)
            out.append(f"{n} setting{'s were' if n != 1 else ' was'} out of range and "
                       f"{'were' if n != 1 else 'was'} brought inside it ({disp(self.clamped)}).")
        if self.defaulted:
            n = len(self.defaulted)
            out.append(f"{n} setting{'s' if n != 1 else ''} had a value CYMA-1 can't read and "
                       f"use{'s' if n == 1 else ''} {'its default' if n == 1 else 'their defaults'} "
                       f"({disp(self.defaulted)}).")
        out.extend(self.other)
        return out


FORBIDDEN_KEYS = ("__proto__", "constructor", "prototype")


def sound_params_from_json(params):
    """a file's params object -> (q16[N], Notes) (spec §3.3 rule 3, 4)"""
    t = table()
    q = list(t.def_q16)
    notes = Notes()
    exact = {k: v for k, v in params.items() if k in t.keys}
    seen = set()
    for k, v in params.items():
        if k in t.keys:
            continue
        nk = norm_key(k)
        nk = t.aliases.get(nk, nk)
        if k not in FORBIDDEN_KEYS and nk in t.keys and nk not in exact and nk not in seen:
            exact[nk] = v                       # an alias ("POS X", "LFO>POS"); an exact key beats it
            seen.add(nk)
        elif k in FORBIDDEN_KEYS or nk not in t.keys:
            notes.skipped.append(k)
    for key, v in exact.items():
        p = t.keys[key]
        q[p["id"]], note = from_json_value(p, v)
        if note == "clamped":
            notes.clamped.append(key)
        elif note == "default":
            notes.defaulted.append(key)
    order = {p["key"]: p["id"] for p in t.params}
    notes.clamped.sort(key=order.get)
    notes.defaulted.sort(key=order.get)
    notes.missing = [p["key"] for p in t.params if p["key"] not in exact]
    return q, notes


# ------------------------------------------------------------------ free text from files (spec §3.3 rule 6, §10)
# One rule for every piece of free text a file carries and this tool shows or writes (a credit, notes, a bank's name, an
# unknown key): the page's cleanText (web/emu/index.html), step for step, so it can neither drive the terminal nor
# reorder or hide what it shows:
#   1. line breaks (CR LF, CR, LF, U+2028, U+2029) become LF in multi-line text (notes), else a space;
#   2. tab, VT, FF and every other space (Zs) become a plain space;
#   3. removed: every other control character (Cc: C0, DEL, C1 including NEL), every Unicode format character (Cf:
#      the bidi embeddings, overrides and isolates U+202A-202E, U+2066-2069, the marks U+200E/200F/061C, zero-width
#      U+200B-200D, U+2060-2064, U+FEFF, U+00AD, the tags), lone surrogates (Cs), the other default-ignorable code
#      points (variation selectors, the Hangul fillers, U+034F, U+180B-180F) and U+2800, the blank that looks like
#      nothing;
#   4. a run of more than 4 combining marks is cut to 4 (stacked marks can't spill over the text around them);
#   5. runs of spaces become one, a space next to a line break goes, at most one empty line in a row; trim;
#   6. cut to the limit, counted in characters (code points), then trim again.
_DEFAULT_IGNORABLE = ((0x00AD, 0x00AD), (0x034F, 0x034F), (0x061C, 0x061C), (0x115F, 0x1160), (0x17B4, 0x17B5),
                      (0x180B, 0x180F), (0x200B, 0x200F), (0x202A, 0x202E), (0x2060, 0x206F), (0x3164, 0x3164),
                      (0xFE00, 0xFE0F), (0xFEFF, 0xFEFF), (0xFFA0, 0xFFA0), (0xFFF0, 0xFFF8), (0x1BCA0, 0x1BCA3),
                      (0x1D173, 0x1D17A), (0xE0000, 0xE0FFF))       # Unicode's Default_Ignorable_Code_Point
_BREAKS_RE = re.compile("\r\n?|[\n\u2028\u2029]")


def _invisible(c):
    o = ord(c)
    if o == 0x2800 or unicodedata.category(c) in ("Cc", "Cf", "Cs"):
        return True
    return o >= 0xAD and any(lo <= o <= hi for lo, hi in _DEFAULT_IGNORABLE)


def clean_text(s, limit, multiline=False):
    """free text from a file -> safe to show (the rule above); None when it is not a string or cleans to nothing"""
    if not isinstance(s, str):
        return None
    s = _BREAKS_RE.sub("\n" if multiline else " ", s)
    out, marks = [], 0
    for c in s:
        if c == "\n":
            out.append(c)
            marks = 0
            continue
        if c in "\t\x0b\x0c" or unicodedata.category(c) == "Zs":
            c = " "
        elif _invisible(c):
            continue
        if unicodedata.category(c)[0] == "M":
            marks += 1
            if marks > 4:
                continue
        else:
            marks = 0
        out.append(c)
    s = re.sub(" {2,}", " ", "".join(out))
    s = re.sub("\n{3,}", "\n\n", re.sub(" ?\n ?", "\n", s)).strip(" \n")
    s = s[:limit].strip(" \n")
    return s or None


def strip_control(s):
    """(kept for callers outside this file) one line of free text, no limit"""
    return clean_text(s, len(s)) or ""


def clean_author(a):
    return clean_text(a, AUTHOR_MAX)


def clean_notes(t):
    return clean_text(t, NOTES_MAX, multiline=True)


def shown_bytes_text(s):
    """text that came from the FM-1 (its identity, HELLO's version): printable ASCII as it is, every other character
    as \\xNN (or \\uNNNN), so a hostile or broken unit can't put control or format characters on the terminal"""
    return "".join(c if " " <= c <= "~" and c != "\\" else
                   ("\\\\" if c == "\\" else "\\x%02x" % ord(c) if ord(c) < 0x100 else "\\u%04x" % ord(c))
                   for c in str(s))


def shown_key(k, limit=40):
    """a file's key (an unknown parameter), as the notes show it"""
    return clean_text(str(k), limit) or "?"


def iso_now():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def sound_to_json(snd, where="cli", version=CLI_VERSION, author=None, created=None):
    """the sound file (spec §3.3): the fields in this order, all params in param_set order"""
    t = table()
    d = {"format": "cyma1-sound", "version": 1, "name": snd.name}
    author = clean_author(author if author is not None else snd.author)
    if author:
        d["author"] = author
    d["created"] = created or iso_now()
    d["made_with"] = {"app": "CYMA-1", "version": version, "where": where}
    d["param_set"] = 1
    d["params"] = {p["key"]: json_value(p, snd.q[p["id"]]) for p in t.params}
    return d


def bank_to_json(sounds, label, where="cli", version=CLI_VERSION, created=None):
    """the bank file (spec §3.5): sounds with their slot when they have one"""
    t = table()
    out = []
    for s in sounds:
        e = {}
        if s.slot is not None:
            e["slot"] = s.slot + 1
        e["name"] = s.name
        if clean_author(s.author):
            e["author"] = clean_author(s.author)
        e["params"] = {p["key"]: json_value(p, s.q[p["id"]]) for p in t.params}
        out.append(e)
    return {"format": "cyma1-bank", "version": 1, "name": label, "created": created or iso_now(),
            "made_with": {"app": "CYMA-1", "version": version, "where": where}, "param_set": 1, "sounds": out}


def dump_json(d):
    return json.dumps(d, indent=2, ensure_ascii=False) + "\n"


MSG_NOT_CYMA = "That file isn't a CYMA-1 sound or bank."
MSG_NEWER_FILE = "This file was made with a newer CYMA-1. Open the newest CYMA-1 page to load it."
MSG_TOO_BIG = "That file is too big to be a CYMA-1 bank (over 1 MB)."


def _refuse_constant(c):
    raise ValueError(f"{c} is not JSON")


def parse_json_text(text):
    """JSON.parse's rules: no NaN or Infinity literals; a nesting-depth error counts as not ours"""
    try:
        return json.loads(text, parse_constant=_refuse_constant)
    except RecursionError:
        raise CymaError(EXIT_ARGS, MSG_NOT_CYMA) from None


def check_version(d):
    v = d.get("version")
    if isinstance(v, bool) or not isinstance(v, int):
        raise CymaError(EXIT_ARGS, MSG_NOT_CYMA)
    if v <= 1:
        return
    c = d.get("compat")
    if isinstance(c, int) and not isinstance(c, bool) and c <= 1:
        return
    raise CymaError(EXIT_ARGS, MSG_NEWER_FILE)


def sound_from_obj(o):
    """one sound object (a sound file, or an entry of a bank) -> Sound; CymaError when it is not one"""
    if not isinstance(o, dict):
        raise CymaError(EXIT_ARGS, MSG_NOT_CYMA)
    params = o.get("params")
    if not isinstance(params, dict) or len(params) > PARAMS_KEYS_MAX:
        raise CymaError(EXIT_ARGS, MSG_NOT_CYMA)
    q, notes = sound_params_from_json(params)
    name = o.get("name")
    snd = Sound(name if isinstance(name, str) else "", q, author=clean_author(o.get("author")), notes=notes)
    txt = o.get("notes")
    snd.text = clean_notes(txt)
    snd.created = clean_text(o.get("created"), 40)
    return snd


def read_json_document(d):
    """a parsed document -> ('sound', [Sound]) or ('bank', [Sound], label); CymaError otherwise"""
    if not isinstance(d, dict):
        raise CymaError(EXIT_ARGS, MSG_NOT_CYMA)
    fmt = d.get("format")
    if fmt == "cyma1-sound":
        check_version(d)
        return "sound", [sound_from_obj(d)], None
    if fmt == "cyma1-bank":
        check_version(d)
        lst = d.get("sounds")
        if not isinstance(lst, list) or len(lst) > BANK_MAX:
            raise CymaError(EXIT_ARGS, MSG_NOT_CYMA)
        out, taken, bank_notes = [], set(), []
        for i, o in enumerate(lst):
            try:
                s = sound_from_obj(o)
            except CymaError:
                bank_notes.append(f"Sound {i + 1} in the bank isn't readable and was left out.")
                continue
            sl = o.get("slot")
            if sl is not None:
                if isinstance(sl, int) and not isinstance(sl, bool) and 1 <= sl <= N_USER and sl - 1 not in taken:
                    s.slot = sl - 1
                    taken.add(sl - 1)
                else:
                    s.notes.other.append(f"Its slot ({clean_text(str(sl), 12) or '?'}) is not U01-U32 or is used "
                                         f"twice in the bank, "
                                         f"so it goes to the next free one.")
            out.append(s)
        label = d.get("name")
        label = clean_text(label, 80)
        return "bank", out, (label, bank_notes)
    raise CymaError(EXIT_ARGS, MSG_NOT_CYMA)


def read_file_bytes(path):
    """the file's bytes, refusing over 1 MiB before reading (spec §10)"""
    p = Path(path)
    try:
        st = p.stat()
    except FileNotFoundError:
        raise CymaError(EXIT_ARGS, f"{path}: no such file") from None
    except OSError as e:
        raise CymaError(EXIT_ARGS, f"{path}: Couldn't read that file ({e.strerror}).") from None
    if p.is_dir():
        raise CymaError(EXIT_ARGS, f"{path}: is a folder, not a file")
    if st.st_size > FILE_MAX:
        raise CymaError(EXIT_ARGS, MSG_TOO_BIG)
    try:
        with open(p, "rb") as f:
            b = f.read(FILE_MAX + 1)
    except OSError as e:
        raise CymaError(EXIT_ARGS, f"Couldn't read that file ({e.strerror}).") from None
    if len(b) > FILE_MAX:
        raise CymaError(EXIT_ARGS, MSG_TOO_BIG)
    return b


class Loaded:
    """what a file, a code or a link held: kind 'sound' (one), 'bank' or 'codes'"""

    def __init__(self, kind, sounds, origin, label=None, notes=None):
        self.kind, self.sounds, self.origin, self.label, self.notes = kind, sounds, origin, label, notes or []


CODE_FIND_RE = re.compile(r"cy1\.[A-Za-z0-9_-]+|cy2\.[A-Za-z0-9_-]{10,}")   # the page's CODES_RE


def read_document_bytes(b, origin):
    """the import's parse order (spec §8.2.7): a sound, a bank, text holding cy1. or cy2. codes; else an error"""
    try:
        text = b.decode("utf-8-sig")
    except UnicodeDecodeError:
        raise CymaError(EXIT_ARGS, MSG_NOT_CYMA) from None
    d, js = None, False
    try:
        d = parse_json_text(text)
        js = True
    except ValueError:
        pass
    if js and isinstance(d, dict) and "format" in d:
        kind, sounds, extra = read_json_document(d)
        if kind == "bank":
            label, bank_notes = extra
            return Loaded("bank", sounds, origin, label=label, notes=bank_notes)
        return Loaded("sound", sounds, origin)
    if js and not isinstance(d, str):
        raise CymaError(EXIT_ARGS, MSG_NOT_CYMA)
    codes = codes_in_text(text)
    if not codes:
        raise CymaError(EXIT_ARGS, MSG_NOT_CYMA)
    if len(codes) > BANK_MAX:
        raise CymaError(EXIT_ARGS, MSG_NOT_CYMA)
    if len(codes) == 1:
        return Loaded("sound", [code_decode_sound(codes[0])], origin)
    sounds, notes, first = [], [], None
    for k, c in enumerate(codes):               # a list of codes: a damaged one is left out, with a note
        try:
            snd = code_decode_sound(c)
        except CymaError as e:
            first = first or e
            notes.append(f"Code {k + 1} ({c[:16]}...) was left out: {e}")
            continue
        if not any(snd.same(o) for o in sounds):    # (a message with the code and its short link: one sound)
            sounds.append(snd)
    if not sounds:
        raise first
    if len(sounds) == 1:
        return Loaded("sound", sounds, origin, notes=notes)
    return Loaded("codes", sounds, origin, notes=notes)


def codes_in_text(text):
    """the cy1. and cy2. codes in a text: one (which may be wrapped across lines, as chat apps do), or several (one per
    line, each whole)"""
    found = CODE_FIND_RE.findall(drop_format_chars(text))
    if not found and len(text) <= TEXT_MAX and CODE_AT_RE.search("".join(drop_format_chars(text).split())):
        return [text]                           # (one code a chat app broke over lines, before its 10th character)
    if not found:
        return []
    if len(found) == 1:
        return [text] if len(text) <= TEXT_MAX else found
    return found


def looks_like_code(s):
    s = drop_format_chars(s)
    return "cy1." in s or "cy2." in s or CODE_LATER_RE.search(s) is not None


def load_source(src):
    """SOURCE: a .cyma1.json file (or any CYMA-1 file), a cy1. or cy2. code, a link, or text holding one"""
    p = Path(src)
    try:
        is_file = p.is_file()
    except OSError:
        is_file = False
    if is_file:
        return read_document_bytes(read_file_bytes(p), p.name)
    if looks_like_code(src):
        return Loaded("sound", [code_decode_sound(src)], "code")
    raise CymaError(EXIT_ARGS, f"{clean_text(src, 80) or repr(src)}: no such file, and not a cy1. code or link")


# ===================================================================== the sound record (spec §3.7)
LAYOUT, PARAM_SET = 0x01, 0x01
REC_NAME_MAX, REC_N_MAX = 32, 64


def rec_encode(name, q):
    nb = clean_name(name).encode("ascii")
    if not 1 <= len(q) <= REC_N_MAX or not all(isinstance(v, int) and 0 <= v <= 0xFFFF for v in q):
        raise ValueError("a record holds 1..64 values of 0..65535")
    return bytes([LAYOUT, PARAM_SET, len(q), len(nb)]) + nb + struct.pack("<%dH" % len(q), *q)


def rec_decode(b):
    """-> (name, q16[N], extra, missing). FormatError: 'newer' (layout or param_set), 'damaged' (length)"""
    t = table()
    if len(b) < 4:
        raise FormatError("damaged")
    if b[0] != LAYOUT or b[1] != PARAM_SET:
        raise FormatError("newer")
    n, ln = b[2], b[3]
    if not 1 <= n <= REC_N_MAX or ln > REC_NAME_MAX or len(b) != 4 + ln + 2 * n:
        raise FormatError("damaged")
    name = clean_name(bytes(c for c in b[4:4 + ln] if c < 0x80))
    v = struct.unpack_from("<%dH" % n, b, 4 + ln)
    q = list(t.def_q16)
    q[:min(n, t.n)] = v[:t.n]
    return name, q, max(0, n - t.n), max(0, t.n - n)


def rec_sound(b):
    name, q, extra, missing = rec_decode(b)
    s = Sound(name, q)
    s.notes.extra, s.notes.older = extra, missing
    return s


# ================================================================================ the share code (spec §4)
CODE_PREFIX = "cy1."
TOKEN_RE = re.compile(r"[A-Za-z0-9_-]*")
MSG_CODE = {"notcode": "That isn't a CYMA-1 code. Codes start with cy1.",
            "damaged": "That sound code is damaged or cut off, so nothing was loaded. Ask for the link or the code "
                       "again.",
            "newer": "This sound needs a newer CYMA-1. Nothing was loaded."}


def crc32(b):
    return zlib.crc32(bytes(b)) & 0xFFFFFFFF


def code_encode(name, q):
    rec = rec_encode(name, q)
    blob = rec + struct.pack("<I", crc32(rec))
    return CODE_PREFIX + base64.urlsafe_b64encode(blob).decode("ascii").rstrip("=")


def drop_format_chars(s):
    """remove Unicode format characters (Cf): a chat app may break a long code with a zero-width space or a soft
    hyphen, and none of them is ever part of a code"""
    return "".join(c for c in s if not ("\u00ad" <= c and unicodedata.category(c) == "Cf"))


def code_token(text):
    """the spec §4 decoder's steps 1-5, for both prefixes -> ('1' or '2', token). Whitespace (chat apps wrap long
    lines) and format characters are removed first. The first code counts: a "cy1.", or a "cy2." with at least 10
    token characters after it (the shortest cy2. there can be; fewer is not taken for one, so "cy2.AQEo" stays notcode
    as v1 says). Neither, but a "cy3." to "cy9." with a token: newer"""
    if len(text) > TEXT_MAX:
        raise FormatError("notcode")
    s = "".join(drop_format_chars(text).split())
    m = CODE_AT_RE.search(s)
    if not m:
        raise FormatError("newer" if CODE_LATER_RE.search(s) else "notcode")
    two = m.group(0) == CODE2_PREFIX
    tok = TOKEN_RE.match(s, m.end()).group(0)
    if (len(tok) < 12 and not two) or len(tok) > 240 or len(tok) % 4 == 1:
        raise FormatError("damaged")
    return ("2" if two else "1"), tok


def unb64_crc(tok, least=9, canonical=False):
    """steps 6-7: base64url, at least `least` bytes (and, for cy2., only the canonical text: the unused bits of the
    last character zero), the CRC-32 (little-endian, last) -> the bytes before it"""
    try:
        blob = base64.urlsafe_b64decode(tok + "=" * (-len(tok) % 4))
    except (ValueError, TypeError):
        raise FormatError("damaged") from None
    if len(blob) < least or (canonical and base64.urlsafe_b64encode(blob).decode("ascii").rstrip("=") != tok):
        raise FormatError("damaged")
    rec, crc = blob[:-4], blob[-4:]
    if struct.unpack("<I", crc)[0] != crc32(rec):
        raise FormatError("damaged")
    return rec


def code_decode(text):
    """a bare code (cy1. or cy2.), '#cy1.X' / '#cy2.X', a whole link, or text holding one -> (name, q16, extra,
    missing). FormatError: notcode / damaged / newer (spec §4 decoder, with cy2. beside cy1.)"""
    kind, tok = code_token(text)
    if kind == "1":
        return rec_decode(unb64_crc(tok, 9))
    return rec2_decode(unb64_crc(tok, 7, canonical=True))


# ====================================================================== the short link code: cy2. (beside spec §4)
# claude.ai passes a link's anchor (the text after #) into the page only when it is at most 128 characters (measured
# in the real frame on 2026-10-24: anchors of 64, 100 and 128 arrived, 145 never did), and every cy1. code is 126-140.
# cy2. (the page's "§4a", web/emu/index.html linkEncode / rec2Decode: this is the same format, byte for byte) carries
# the same sound in fewer bytes by keeping only the values that differ from CY2_BASE:
#
#   code2  = "cy2." + base64url_nopad(body || CRC32_LE(body))       (CRC-32 and base64url exactly as cy1.'s)
#   body   = [N][L][bitmap: B = ceil(N/8) bytes][name: L bytes ASCII][values: one u16 LE per set bit, in order]
#   bitmap = bit i (byte i >> 3, bit i & 7, LSB first) set = parameter i is stored; bits at N and above are 0
#
# - The encoder writes N = 40, the cleaned name (§3.4), and sets bit i exactly when q[i] != CY2_BASE[i].
# - The decoder: steps 1-7 as cy1.'s (a cy2. token needs at least 10 characters, must be the canonical base64url text
#   of its bytes, and a blob at least 7 bytes); then N 1..64,
#   L <= 32, at least 2 + B + L bytes, no bit at or above N, and exactly 2 + B + L + 2 x popcount bytes, else damaged.
#   A parameter below 40 that is not stored, and every one at N and above (an older writer's: "missing"), is
#   CY2_BASE's; a stored value past the 40 this tool knows is counted ("extra") and left out, as cy1.'s are. There
#   is no layout byte: a different layout would be a new prefix, and "cy3." to "cy9." already read as newer.
# - Size: 4 + ceil(4 (2 + B + L + 2k + 4) / 3) characters for k stored values. The anchor "cy2." + token fits 128
#   exactly when the blob is at most 93 bytes: k <= 34 with a 14-letter name. Every factory sound fits (50-74).
# - CY2_BASE is FROZEN with cy2.: param_set 1's def_q16 as of 2026-10-24 (tools/cyma1_params.json), whatever
#   params.c's defaults become, so a future change of a default can never change what an old link plays. The
#   selftest says if the table and it ever part.
CODE2_PREFIX = "cy2."
CY2_BASE = (8192, 0, 0, 39321, 52428, 0, 32768, 0, 32768, 13107, 65535, 36044, 18724, 39321, 26214, 26214, 19661,
            40632, 26214, 9830, 0, 32768, 19661, 42790, 32768, 19661, 39681, 0, 13107, 28672, 32768, 49151, 6554, 0,
            32768, 0, 65535, 0, 0, 54426)
ANCHOR_MAX = 128                        # claude.ai's limit on a link's anchor (the text after #), measured
CODE_AT_RE = re.compile(r"cy1\.|cy2\.(?=[A-Za-z0-9_-]{10})")
CODE_LATER_RE = re.compile(r"cy[3-9]\.[A-Za-z0-9_-]{10}")


def rec2_encode(name, q):
    nb = clean_name(name).encode("ascii")
    n = len(q)
    if not 1 <= n <= REC_N_MAX or not all(isinstance(v, int) and 0 <= v <= 0xFFFF for v in q):
        raise ValueError("a record holds 1..64 values of 0..65535")
    bm, vals = bytearray((n + 7) // 8), []
    for i, v in enumerate(q):
        if i >= len(CY2_BASE) or v != CY2_BASE[i]:
            bm[i >> 3] |= 1 << (i & 7)
            vals.append(v)
    return bytes([n, len(nb)]) + bytes(bm) + nb + struct.pack("<%dH" % len(vals), *vals)


def rec2_decode(b):
    """a cy2. body -> (name, q16[], extra, missing). FormatError: 'damaged'"""
    t = table()
    if len(b) < 2:
        raise FormatError("damaged")
    n, ln = b[0], b[1]
    nb = (n + 7) // 8
    if not 1 <= n <= REC_N_MAX or ln > REC_NAME_MAX or len(b) < 2 + nb + ln:
        raise FormatError("damaged")
    mask = int.from_bytes(b[2:2 + nb], "little")
    k = bin(mask).count("1")
    if mask >> n or len(b) != 2 + nb + ln + 2 * k:
        raise FormatError("damaged")
    name = clean_name(bytes(c for c in b[2 + nb:2 + nb + ln] if c < 0x80))
    stored = struct.unpack_from("<%dH" % k, b, 2 + nb + ln)
    q = list(CY2_BASE[:t.n]) + list(t.def_q16[len(CY2_BASE):])
    extra, j = 0, 0
    for i in range(n):
        if mask >> i & 1:
            if i < t.n:
                q[i] = stored[j]
            else:
                extra += 1
            j += 1
    return name, q, extra, max(0, t.n - n)


def code2_encode(name, q):
    body = rec2_encode(name, q)
    blob = body + struct.pack("<I", crc32(body))
    return CODE2_PREFIX + base64.urlsafe_b64encode(blob).decode("ascii").rstrip("=")


def short_code(name, q):
    """the cy2. code when it fits claude.ai's anchor (at most 128 characters), else None"""
    c = code2_encode(name, q)
    return c if len(c) <= ANCHOR_MAX else None


def code_decode_sound(text):
    try:
        name, q, extra, missing = code_decode(text)
    except FormatError as e:
        raise CymaError(EXIT_ARGS, MSG_CODE.get(e.code, MSG_CODE["damaged"])) from None
    s = Sound(name, q)
    s.notes.extra, s.notes.older = extra, missing
    return s


SHARE_BASE_RE = re.compile(r"https?://[A-Za-z0-9./_~:-]+")


def share_link(base, code):
    base = base.split("#", 1)[0]
    if not SHARE_BASE_RE.fullmatch(base) or "?" in base:
        raise CymaError(EXIT_ARGS, f"{base!r} is not a page address (https://..., no ? or #)")
    return base + "#" + code


# ================================================================================ SysEx framing (spec §5)
SX_PREFIX = bytes([0x7D, 0x43, 0x59])       # MIDI non-commercial ID, "CY"
SX_VER = 0x01
SX_MAX_PAYLOAD = 256
SX_MAX_FRAME = 306
HELLO, GET_EDIT, PUT_EDIT, GET_SLOT, PUT_SLOT, ERASE_SLOT, GET_DIR, COMMIT, ABORT, NAK = (
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x7F)
CMD_NAME = {HELLO: "HELLO", GET_EDIT: "GET_EDIT", PUT_EDIT: "PUT_EDIT", GET_SLOT: "GET_SLOT", PUT_SLOT: "PUT_SLOT",
            ERASE_SLOT: "ERASE_SLOT", GET_DIR: "GET_DIR", COMMIT: "COMMIT", ABORT: "ABORT", NAK: "NAK"}
OK, PENDING, E_FRAME, E_VERSION, E_CMD, E_ARG, E_PARAM_SET = 0x00, 0x01, 0x10, 0x11, 0x12, 0x13, 0x14
E_BUSY, E_HOLD, E_NOTHING, E_ASKING, E_LOCKED = 0x31, 0x32, 0x33, 0x34, 0x35
E_DECLINED, E_TIMEOUT, E_FLASH = 0x40, 0x41, 0x42
ST_NAME = {OK: "OK", PENDING: "PENDING", E_FRAME: "E_FRAME", E_VERSION: "E_VERSION", E_CMD: "E_CMD", E_ARG: "E_ARG",
           E_PARAM_SET: "E_PARAM_SET", E_BUSY: "E_BUSY", E_HOLD: "E_HOLD", E_NOTHING: "E_NOTHING",
           E_ASKING: "E_ASKING", E_LOCKED: "E_LOCKED", E_DECLINED: "E_DECLINED", E_TIMEOUT: "E_TIMEOUT",
           E_FLASH: "E_FLASH"}
HOST_KIND_CLI = 2


def pack7(b):
    """fm1-base's 8-to-7, LSB first (bsp/chip/ota.c ota_pack7)"""
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
    """strict (spec §5.2): every byte < 0x80, no spare byte, the spare high bits zero"""
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
    if not (0 <= cmd <= 0x7F and 0 <= seq <= 0x7F and len(payload) <= SX_MAX_PAYLOAD):
        raise ValueError("a frame's cmd and seq are 7-bit and its payload at most 256 bytes")
    hdr = bytes([SX_VER, cmd, seq])
    return b"\xF0" + SX_PREFIX + hdr + pack7(bytes(payload) + struct.pack("<I", crc32(hdr + bytes(payload)))) + b"\xF7"


def sx_parse(frame):
    """F0..F7 -> (ver, cmd, seq, payload). FormatError: notours / version / frame"""
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


# ------------------------------------------------------------- the identity gate (spec §5.6 P7, D12)
HS_QUERY = bytes([0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7])   # M-UPGRADE's identity query
PORT_RE = re.compile(r"fm-?1|felucca|ota|composite|sinco|usb-midi", re.I)       # fm1-base's: never probe other gear
CYMA_ID_RE = re.compile(r"FM-1_94(00|\d{6})")


def allowed(frame):
    """the send allowlist (spec §5.6 P7): HS_QUERY, or one of our frames: F0 7D 43 59 01 ... F7, <= 306 B, 7-bit"""
    frame = bytes(frame)
    if frame == HS_QUERY:
        return True
    return (13 <= len(frame) <= SX_MAX_FRAME and frame[:5] == b"\xF0\x7D\x43\x59\x01" and frame[-1] == 0xF7
            and all(x < 0x80 for x in frame[1:-1]))


class NotAllowed(RuntimeError):
    pass


def check_allowed(frame):
    if not allowed(frame):
        raise NotAllowed("refused to send a frame outside the allowlist: " + bytes(frame[:8]).hex(" ") + " ...")


def unpack7_loose(s):
    """the updater's own unpack (ota.c ota_unpack7): for the identity reply only"""
    out, acc, nb = bytearray(), 0, 0
    for b in s:
        acc |= (b & 0x7F) << nb
        nb += 7
        while nb >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            nb -= 8
    return bytes(out)


def parse_identity(pkt):
    """fm1-base's handshake reply (ota.c ota_reply_identity: 00 59 11, 27 bytes of identity) -> text, or None.

    The identity rule (one rule, in this tool and the page): the text is the 27 bytes with the trailing NULs removed,
    and nothing else is removed. fm1-base's ota_reply_identity always fills the bytes after the identity with zeros,
    so a real CYMA-1 never has anything after its first NUL; a reply that does (FM-1_9400 NUL junk) keeps the NUL in its
    text, fails the FM-1_94... match, and is refused: the gate fails closed. This is also what fm1-base's own installer
    reads (tools/fm1_install.py parse_identity)."""
    if len(pkt) < 2 or pkt[0] != 0xF0 or pkt[-1] != 0xF7:
        return None
    d = unpack7_loose(pkt[1:-1])
    if len(d) != 34 or d[:3] != b"\x00\x59\x11":
        return None
    return d[6:33].rstrip(b"\0").decode("latin-1")


def is_cyma_identity(ident):
    return isinstance(ident, str) and CYMA_ID_RE.fullmatch(ident) is not None


def identity_frame(text):
    """the reply an FM-1 sends to HS_QUERY (for the virtual FM-1 and the tests); text may be bytes (any 27)"""
    body = (bytes(text) if isinstance(text, (bytes, bytearray)) else text.encode("latin-1"))[:27].ljust(27, b"\0")
    msg = bytes([0x00, 0x59, 0x11, 27, 0, 0]) + body + bytes([~sum(body) & 0xFF])
    return b"\xF0" + pack7(msg) + b"\xF7"


def identity_refusal(ident):
    """why an FM-1 that answered the identity query gets nothing else (the identity shown with shown_bytes_text)"""
    shown = shown_bytes_text(ident)
    if ident.lower().startswith("ota-"):
        return "The FM-1 is in update mode. Nothing was sent."
    if re.fullmatch(r"FM-1_0\d\d", ident) and ident != "FM-1_099":
        return f"This FM-1 runs M-VAVE's own firmware ({shown}). Install CYMA-1 to send sounds."
    return f"This FM-1 runs other firmware ({shown}), not CYMA-1."


# ===================================================================================== backends (spec §9.3, I8)
class Link:
    """one connection to an FM-1: whole F0..F7 frames both ways. send() checks the allowlist"""
    name = "?"

    def send(self, frame: bytes) -> bool:
        raise NotImplementedError

    def read(self, timeout: float):
        raise NotImplementedError

    def close(self) -> None:
        pass

    @property
    def lost(self):
        return False

    def drain(self):
        while self.read(0) is not None:
            pass


class TraceLink(Link):
    """--trace: every frame sent and received, as hex on stderr (a MIDI monitor's view)"""

    def __init__(self, link, err):
        self.link, self.err, self.name = link, err, link.name

    def send(self, frame):
        check_allowed(frame)
        print("-> " + bytes(frame).hex(" ").upper(), file=self.err, flush=True)
        return self.link.send(frame)

    def read(self, timeout):
        f = self.link.read(timeout)
        if f is not None:
            print("<- " + bytes(f).hex(" ").upper(), file=self.err, flush=True)
        return f

    @property
    def lost(self):
        return self.link.lost

    def close(self):
        self.link.close()


class Backend:
    virtual = False

    def input_names(self) -> list:
        raise NotImplementedError

    def output_names(self) -> list:
        raise NotImplementedError

    def open(self, in_name: str, out_name: str) -> Link:
        raise NotImplementedError


class MidoLink(Link):
    """a MIDI in/out pair through mido + python-rtmidi, with a SysEx queue (the note path's messages are ignored)"""

    def __init__(self, backend, in_name, out_name):
        self.backend, self.name, self.mido = backend, in_name, backend.mido
        self.q = queue.Queue()
        self.sent = []
        self.inp = self.mido.open_input(in_name, callback=self._rx)
        try:
            self.out = self.mido.open_output(out_name)
        except Exception:
            self.inp.close()
            raise
        time.sleep(DELAY["open"])               # CoreMIDI may drop a message sent just after opening

    def _rx(self, msg):
        if msg.type == "sysex":
            self.q.put(bytes([0xF0, *msg.data, 0xF7]))

    @property
    def lost(self):
        try:
            return self.name not in self.backend.input_names()
        except Exception:
            return True

    def send(self, frame):
        check_allowed(frame)
        try:
            self.out.send(self.mido.Message("sysex", data=bytes(frame[1:-1])))
            return True
        except Exception:
            return False

    def read(self, timeout):
        try:
            if timeout <= 0:
                return self.q.get_nowait()
            return self.q.get(timeout=timeout)
        except queue.Empty:
            return None

    def close(self):
        for p in (getattr(self, "inp", None), getattr(self, "out", None)):
            try:
                if p is not None:
                    p.close()
            except Exception:
                pass


class MidoBackend(Backend):
    def __init__(self):
        try:
            import mido
            mido.set_backend("mido.backends.rtmidi")
        except Exception as e:
            venv = next((p / ".venv312" / "bin" / "python" for p in APP_DIR.parents
                         if (p / ".venv312" / "bin" / "python").is_file()), None)
            hint = f", or run this with {venv}" if venv else ""
            raise CymaError(EXIT_ARGS, f"The FM-1 commands need mido and python-rtmidi ({e}): python3 -m pip install "
                                       f"mido python-rtmidi{hint}. (--virtual, encode, decode, validate and selftest "
                                       f"work without them.)") from None
        self.mido = mido

    def input_names(self):
        return list(dict.fromkeys(self.mido.get_input_names()))

    def output_names(self):
        return list(dict.fromkeys(self.mido.get_output_names()))

    def open(self, in_name, out_name):
        return MidoLink(self, in_name, out_name)


VIRTUAL_PORT = "CYMA-1 FM-1 (virtual)"


def default_wasm():
    for p in (APP_DIR / "build" / "emu" / "app.wasm", APP_DIR / "dist" / "web" / "app.wasm"):
        if p.is_file():
            return p
    return APP_DIR / "build" / "emu" / "app.wasm"


class BridgeLink(Link):
    """the virtual FM-1: node tools/sx_bridge.mjs running app.wasm, one hex frame per line both ways (spec §9.4)"""

    def __init__(self, cmd, name=VIRTUAL_PORT):
        self.name = name
        self.q = queue.Queue()
        self.state = None
        self.ready = threading.Event()
        self.eof = threading.Event()
        self.err_tail = []
        try:
            self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                         bufsize=0)
        except OSError as e:
            raise CymaError(EXIT_NOTFOUND, f"Couldn't start the virtual FM-1 ({cmd[0]}: {e.strerror}). It needs "
                                           f"node on the PATH (or CYMA1_NODE).") from None
        self.threads = [threading.Thread(target=self._reader, daemon=True),
                        threading.Thread(target=self._stderr, daemon=True)]
        for t in self.threads:
            t.start()
        end = time.monotonic() + DELAY["bridge_ready"]
        while not self.ready.wait(0.05):
            if self.eof.is_set() or time.monotonic() > end:
                self.close()
                tail = "; ".join(self.err_tail[-3:])
                raise CymaError(EXIT_NOTFOUND, "The virtual FM-1 did not start" + (f": {tail}" if tail else "."))

    def _reader(self):
        for line in iter(self.proc.stdout.readline, b""):
            s = line.decode("ascii", "replace").strip()
            if not s:
                continue
            if s == "#ready":
                self.ready.set()
            elif s.startswith("#state "):
                try:
                    self.state = int(s[7:])
                except ValueError:
                    pass
            elif not s.startswith("#"):
                try:
                    self.q.put(bytes.fromhex(s))
                except ValueError:
                    pass
        self.eof.set()

    def _stderr(self):
        for line in iter(self.proc.stderr.readline, b""):
            self.err_tail = (self.err_tail + [line.decode("utf-8", "replace").strip()])[-20:]

    @property
    def lost(self):
        return self.eof.is_set() and self.q.empty()

    def _write(self, text):
        try:
            self.proc.stdin.write(text.encode("ascii"))
            self.proc.stdin.flush()
            return True
        except (OSError, ValueError):
            return False

    def send(self, frame):
        check_allowed(frame)
        if self.eof.is_set():
            return False
        return self._write(bytes(frame).hex().upper() + "\n")

    def read(self, timeout):
        try:
            if timeout <= 0:
                return self.q.get_nowait()
            end = time.monotonic() + timeout
            while True:                         # (wake up on EOF instead of sleeping the whole timeout)
                left = end - time.monotonic()
                if left <= 0:
                    return None
                try:
                    return self.q.get(timeout=min(left, 0.05))
                except queue.Empty:
                    if self.eof.is_set():
                        return None
        except queue.Empty:
            return None

    def close(self):
        if self.proc.poll() is None:
            self._write("!quit\n")
            try:
                self.proc.stdin.close()
            except OSError:
                pass
            try:
                self.proc.wait(DELAY["bridge_quit"])
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        for t in self.threads:
            t.join(1.0)
        for f in (self.proc.stdin, self.proc.stdout, self.proc.stderr):
            try:
                f.close()
            except (OSError, ValueError):
                pass


class BridgeBackend(Backend):
    """--virtual: one virtual port, backed by sx_bridge.mjs and the firmware's own app.wasm"""
    virtual = True

    def __init__(self, wasm=None, answer="yes", identity=None, store=None, answer_ms=None, die_after=None,
                 identity_hex=None):
        self.wasm = Path(wasm) if wasm else default_wasm()
        if not self.wasm.is_file():
            raise CymaError(EXIT_NOTFOUND, f"The virtual FM-1 needs app.wasm ({self.wasm} is missing): build it with "
                                           f"tools/run_all.sh --quick, or pass --wasm PATH.")
        node = os.environ.get("CYMA1_NODE") or shutil.which("node")
        if not node:
            raise CymaError(EXIT_NOTFOUND, "The virtual FM-1 needs node (https://nodejs.org) on the PATH.")
        self.cmd = [node, str(BRIDGE_PATH), "--wasm", str(self.wasm), "--answer", answer]
        if identity:
            self.cmd += ["--identity", identity]
        if store:
            self.cmd += ["--store", str(store)]
        if answer_ms is not None:
            self.cmd += ["--answer-ms", str(int(answer_ms))]
        if die_after is not None:
            self.cmd += ["--die-after", str(int(die_after))]
        if identity_hex is not None:
            if not re.fullmatch(r"(?:[0-9A-Fa-f]{2}){0,27}", identity_hex):
                raise CymaError(EXIT_ARGS, "--virtual-identity-hex: up to 27 bytes as hex")
            self.cmd += ["--identity-hex", identity_hex]

    def input_names(self):
        return [VIRTUAL_PORT]

    def output_names(self):
        return [VIRTUAL_PORT]

    def open(self, in_name, out_name):
        if in_name != VIRTUAL_PORT:
            raise OSError("no such port")
        return BridgeLink(self.cmd)


# ======================================================================= the session (spec §5.3-§5.5, CySx)
class Lost(CymaError):
    def __init__(self, msg="The FM-1 stopped answering, so nothing on it was changed. Check the cable and try again."):
        super().__init__(EXIT_LOST, msg)


class DeviceError(CymaError):
    """a reply's status that ends the command"""

    def __init__(self, status, cmd=None):
        self.status = status
        code, msg = STATUS_EXIT.get(status, (EXIT_FIRMWARE, None))
        if msg is None:
            msg = f"The FM-1 refused {CMD_NAME.get(cmd, 'the request')} ({ST_NAME.get(status, hex(status))})."
        super().__init__(code, msg)


STATUS_EXIT = {
    E_BUSY: (EXIT_BUSY, "The FM-1 is busy (a SAVE question or COMPARE is open). Finish it, then try again."),
    E_HOLD: (EXIT_BUSY, "The FM-1 is sending a backup. Try again when it's done."),
    E_ASKING: (EXIT_BUSY, "The FM-1 is already asking a question on its screen. Answer it, then try again."),
    E_LOCKED: (EXIT_FIRMWARE, "The FM-1's sounds were saved by a newer CYMA-1, so this tool won't change them."),
    E_PARAM_SET: (EXIT_FIRMWARE, "The CYMA-1 on this FM-1 is older than this tool's sound format. Update it."),
    E_VERSION: (EXIT_FIRMWARE, "The CYMA-1 on this FM-1 speaks another sound protocol. Update CYMA-1 and these tools."),
    E_DECLINED: (EXIT_NO, "You pressed HOME on the FM-1, so nothing on it was changed."),
    E_TIMEOUT: (EXIT_NO, "Nobody answered on the FM-1 within 30 s, so nothing on it was changed."),
    E_FLASH: (EXIT_NO, "The FM-1 couldn't write its memory (SAVE FAILED). The sounds stay in its RAM until it is "
                       "switched off: try again."),
    E_NOTHING: (EXIT_NO, "The FM-1 dropped the sounds before they were saved (another program connected to it, or "
                         "it waited over 60 s), so nothing was changed. Try again."),
}


class Hello:
    def __init__(self, body):
        if len(body) < 10:
            raise FormatError("frame")
        (self.proto, self.param_set, self.n_params, self.n_user, self.name_max) = body[0:5]
        self.max_payload = struct.unpack_from("<H", body, 5)[0]
        self.flags, self.state = body[7], body[8]
        al = body[9]
        self.app = body[10:10 + al].decode("latin-1")
        vl = body[10 + al] if len(body) > 10 + al else 0
        # (the FM-1's own text: shown, and written into files' made_with.version, as printable ASCII only)
        self.version = shown_bytes_text(body[11 + al:11 + al + vl].decode("latin-1"))[:40] or "?"

    @property
    def emulator(self):
        return bool(self.flags & 2)

    @property
    def locked(self):
        return bool(self.flags & 4)


class Dir:
    def __init__(self, body):
        if len(body) < 19:
            raise FormatError("frame")
        self.used, self.staged, self.erased, self.bank_crc = struct.unpack_from("<IIII", body)
        self.state, self.last_result, self.last_commit_seq = body[16], body[17], body[18]

    def used_slots(self):
        return [i for i in range(N_USER) if self.used >> i & 1]


class Session:
    """the host side of the protocol: SEQ, retries with the same SEQ, the duplicate-safe COMMIT wait"""

    def __init__(self, link, ui=None):
        self.link = link
        self.ui = ui or Ui()
        self.seq = random.randrange(128)       # (a fresh start can never meet the device's duplicate cache)
        self.sent = {}                          # command name -> requests sent (not counting retries)
        self.finals = {}                        # COMMIT seq -> (status, used_mask)
        self.staging = False                    # PUT_SLOT / ERASE_SLOT sent and not committed or aborted
        self.mine = (0, 0)                      # what this session staged: (staged_mask, erase_mask)
        self.commit_seq = None                  # the COMMIT that asked on the FM-1
        self.hello = None

    def _count(self, name):
        self.sent[name] = self.sent.get(name, 0) + 1

    def _take(self, f):
        """one received frame -> (cmd, seq, payload) if it is ours and whole; records COMMIT results"""
        try:
            _, cmd, seq, payload = sx_parse(f)
        except FormatError:
            return None
        if cmd == (COMMIT | 0x40) and payload and payload[0] not in (PENDING,) and payload[0] < 0x80:
            used = struct.unpack_from("<I", payload, 1)[0] if len(payload) >= 5 else None
            if payload[0] != OK or used is not None:
                self.finals[seq] = (payload[0], used)
        return cmd, seq, payload

    def request(self, cmd, payload=b"", wait=None):
        """send one request, retry 3x with the same SEQ; -> (status, body after the status byte)"""
        seq = self.seq
        self.seq = (self.seq + 1) & 0x7F
        frame = sx_frame(cmd, seq, payload)
        wait = wait if wait is not None else (DELAY["hello"] if cmd == HELLO else DELAY["reply"])
        self._count(CMD_NAME.get(cmd, hex(cmd)))
        for _ in range(1 + DELAY["retries"]):
            if not self.link.send(frame):
                raise Lost()
            end = time.monotonic() + wait
            while True:
                left = end - time.monotonic()
                if left <= 0:
                    break
                f = self.link.read(left)
                if f is None:
                    if self.link.lost:
                        raise Lost()
                    continue
                r = self._take(f)
                if r is None or r[1] != seq:
                    continue
                rcmd, _, body = r
                if rcmd == NAK:
                    st = body[0] if body else E_FRAME
                    if st == E_FRAME:
                        break                   # damaged on the way: send it again (same SEQ)
                    raise DeviceError(st, cmd)
                if rcmd != (cmd | 0x40) or not body:
                    continue
                return body[0], body[1:]
        if self.link.lost:
            raise Lost()
        raise NoAnswer(cmd)

    def ok(self, cmd, payload=b""):
        st, body = self.request(cmd, payload)
        if st != OK:
            raise DeviceError(st, cmd)
        return body

    # ---- the commands
    def do_hello(self):
        st, body = self.request(HELLO, bytes([1, HOST_KIND_CLI]))
        if st != OK:
            raise DeviceError(st, HELLO)
        try:
            self.hello = Hello(body)
        except (FormatError, IndexError, struct.error):
            raise CymaError(EXIT_FIRMWARE, "The FM-1 answered HELLO in a way this tool doesn't understand.") from None
        self.staging = False
        return self.hello

    def get_dir(self):
        try:
            return Dir(self.ok(GET_DIR))
        except (FormatError, struct.error):
            raise CymaError(EXIT_FIRMWARE, "The FM-1 sent a slot list this tool doesn't understand.") from None

    def get_slot(self, i):
        body = self.ok(GET_SLOT, bytes([i]))
        if len(body) < 2 or body[0] != i:
            raise CymaError(EXIT_FIRMWARE, f"The FM-1 answered GET_SLOT {slot_name(i)} for another slot.")
        if not body[1]:
            return None
        try:
            s = rec_sound(body[2:])
        except FormatError as e:
            raise CymaError(EXIT_FIRMWARE, f"The FM-1 sent {slot_name(i)} in a format this tool can't read "
                                           f"({e.code}).") from None
        s.slot = i
        return s

    def get_edit(self):
        body = self.ok(GET_EDIT)
        if len(body) < 2:
            raise CymaError(EXIT_FIRMWARE, "The FM-1 answered GET_EDIT in a way this tool doesn't understand.")
        try:
            s = rec_sound(body[2:])
        except FormatError as e:
            raise CymaError(EXIT_FIRMWARE, f"The FM-1 sent its sound in a format this tool can't read "
                                           f"({e.code}).") from None
        return body[0], body[1], s

    def put_edit(self, snd, glide=True):
        self.ok(PUT_EDIT, bytes([1 if glide else 0]) + rec_encode(snd.name, snd.q))

    def put_slot(self, i, snd):
        self.staging = True
        body = self.ok(PUT_SLOT, bytes([i]) + rec_encode(snd.name, snd.q))
        self.mine = (self.mine[0] | 1 << i, self.mine[1] & ~(1 << i))
        self._staged(body, i, "")

    def erase_slot(self, i):
        self.staging = True
        body = self.ok(ERASE_SLOT, bytes([i]))
        self.mine = (self.mine[0] & ~(1 << i), self.mine[1] | 1 << i)
        self._staged(body, i, "the delete of ")

    def _staged(self, body, i, what):
        """the reply's masks must be exactly what this session staged (else another host is staging too)"""
        if len(body) < 9 or body[0] != i:
            raise CymaError(EXIT_FIRMWARE, f"The FM-1 did not stage {what}{slot_name(i)}.")
        if struct.unpack_from("<II", body, 1) != self.mine:
            raise CymaError(EXIT_BUSY, "Another program is sending sounds to the FM-1 at the same time (the CYMA-1 "
                                       "page?), so nothing was changed. Close it, then try again.")

    def abort(self):
        """drop the staging (and close an open question as declined); best effort"""
        try:
            saved, DELAY["retries"] = DELAY["retries"], 1
            try:
                self.request(ABORT)
            finally:
                DELAY["retries"] = saved
        except CymaError:
            pass
        self.staging = False

    def commit(self, on_pending=None):
        """COMMIT, then wait for the player's answer on the FM-1 -> (status, used_mask)"""
        seq = self.commit_seq = self.seq
        st, body = self.request(COMMIT, bytes([0]))
        if st == PENDING:
            if on_pending:
                on_pending()
            return self.wait_commit(seq)
        if st == OK or st in (E_DECLINED, E_TIMEOUT, E_FLASH):  # (a retried COMMIT answered from the cache: final)
            self.staging = False
            used = struct.unpack_from("<I", body, 0)[0] if len(body) >= 4 else None
            return st, used
        raise DeviceError(st, COMMIT)                           # (the staging stays: the caller aborts it)

    def wait_commit(self, seq):
        """the 0x48 result, or GET_DIR every 2 s (state != 2 and last_commit_seq == seq: final), up to 35 s"""
        end = time.monotonic() + DELAY["commit"]
        next_poll = time.monotonic() + DELAY["poll"]
        while True:
            if seq in self.finals:
                self.staging = False
                return self.finals[seq]
            now = time.monotonic()
            if now >= end:
                raise Lost(CANT_TELL)
            if now >= next_poll:
                try:
                    d = self.get_dir()
                except Lost:
                    raise Lost(CANT_TELL) from None
                if seq in self.finals:
                    continue
                if d.state != 2 and d.last_commit_seq == seq:
                    self.staging = False
                    return d.last_result, d.used
                next_poll = time.monotonic() + DELAY["poll"]
                continue
            f = self.link.read(min(next_poll, end) - now)
            if f is not None:
                self._take(f)
            elif self.link.lost:
                raise Lost(CANT_TELL)

    def summary(self):
        order = ["HELLO", "GET_EDIT", "GET_DIR", "GET_SLOT", "PUT_EDIT", "PUT_SLOT", "ERASE_SLOT", "COMMIT", "ABORT"]
        parts = ["identity"]
        for k in order:
            n = self.sent.get(k, 0)
            if n:
                parts.append(k if n == 1 else f"{k} x{n}")
        return ", ".join(parts)


CANT_TELL = ("The FM-1 stopped answering while it asked, so this tool can't tell whether anything was saved. Check "
             "the FM-1 (and the cable), then run: cyma_sounds.py info")


class NoAnswer(Lost):
    def __init__(self, cmd):
        super().__init__()
        self.cmd = cmd


# ================================================================================== talking to the user
class Ui:
    def __init__(self, quiet=False, yes=False, out=None, err=None, trace=False):
        self.quiet, self.yes, self.trace = quiet, yes, trace
        self.out = out or sys.stdout
        self.err = err or sys.stderr

    @staticmethod
    def _print(msg, f, **kw):
        """print; on a terminal that can't show a character (LANG=C, a cp1252 console), show it as \\uNNNN instead
        of failing"""
        try:
            print(msg, file=f, flush=True, **kw)
        except UnicodeEncodeError:
            enc = getattr(f, "encoding", None) or "ascii"
            print(str(msg).encode(enc, "backslashreplace").decode(enc, "replace"), file=f, flush=True, **kw)

    def say(self, msg=""):
        if not self.quiet:
            self._print(msg, self.out)

    def result(self, msg):
        self._print(msg, self.out)

    def warn(self, msg):
        self._print(msg, self.err)

    def confirm(self, question):
        """this computer's question (--yes answers it); the FM-1's own question is separate and always asked"""
        if self.yes:
            print(f"{question} [y/N] y (--yes)", file=self.out, flush=True)
            return True
        print(f"{question} [y/N] ", end="", file=self.out, flush=True)
        try:
            a = input()
        except (EOFError, KeyboardInterrupt):
            print(file=self.out)
            return False
        if not sys.stdin.isatty():
            print(a.strip(), file=self.out, flush=True)    # (an answer from a pipe is not echoed by a terminal)
        return a.strip().lower() in ("y", "yes")


def arrow():
    enc = (getattr(sys.stdout, "encoding", "") or "").lower()
    return "\u203a" if "utf" in enc else ">"


def describe(snd):
    """EXCITER · SHAPE · MATERIAL, from the values (a shape between keyframes reads HEX›SQUARE)"""
    t = table()
    dot = " \u00b7 " if arrow() != ">" else " - "
    parts = []
    for key in ("exciter", "shape", "material"):
        p = t.keys.get(key)
        if not p:
            continue
        q = snd.q[p["id"]]
        if p["curve"] == "ENUM":
            parts.append(json_value(p, q))
        elif p["curve"] == "MORPH" and p["labels"]:
            v = p["lo"] + q / 65535.0 * (p["hi"] - p["lo"]) - p["lo"]
            k = int(math.floor(v + 0.5))
            if abs(v - k) < 0.02:
                parts.append(p["labels"][min(max(k, 0), len(p["labels"]) - 1)])
            else:
                a = min(max(int(math.floor(v)), 0), len(p["labels"]) - 1)
                parts.append(p["labels"][a] + arrow() + p["labels"][min(a + 1, len(p["labels"]) - 1)])
    return dot.join(parts)


def preset_label(idx):
    t = table()
    if idx < 24:
        f = next((f for f in t.factory if f.get("num") == idx + 1), None)
        return "%02d %s" % (idx + 1, f["name"] if f else "")
    return slot_name(idx - 24)


def write_new_file(path, text, force):
    """write a file, never over an existing one without --force (atomic: a temp file, then a rename)"""
    path = Path(path)
    if path.exists() and not force:
        raise CymaError(EXIT_ARGS, f"{path} already exists: use --force to replace it, or -o another name.")
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".cyma1-", suffix=".tmp", dir=str(path.parent))
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        if not force and path.exists():
            raise CymaError(EXIT_ARGS, f"{path} already exists: use --force to replace it, or -o another name.")
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
    return path


def out_path(opt, default_name):
    """-o FILE (or a folder to put the default name in); else the default name here"""
    if not opt:
        return Path(default_name)
    p = Path(opt).expanduser()
    if p.is_dir() or str(opt).endswith(("/", os.sep)):
        return p / default_name
    return p


def unique_path(folder, stem, ext):
    p = Path(folder) / f"{stem}{ext}"
    k = 2
    while p.exists():
        p = Path(folder) / f"{stem} {k}{ext}"
        k += 1
    return p


# ======================================================================================= the connection
def candidates(names, port):
    if port:
        return [n for n in names if port.lower() in n.lower()]
    return [n for n in names if PORT_RE.search(n)]


def pair_output(name, outs):
    if name in outs:
        return name
    base = re.sub(r"\s+\d+$", "", name)         # Windows numbers each port
    same = [o for o in outs if re.sub(r"\s+\d+$", "", o) == base]
    if len(same) == 1:
        return same[0]
    return outs[0] if len(outs) == 1 else None


def handshake(link, tries=2):
    """HS_QUERY, the identity gate's only frame -> identity text or None"""
    link.drain()
    for _ in range(tries):
        if not link.send(HS_QUERY):
            return None
        end = time.monotonic() + DELAY["hs"]
        while True:
            left = end - time.monotonic()
            if left <= 0:
                break
            p = link.read(left)
            if p is None:
                if link.lost:
                    return None
                continue
            ident = parse_identity(p)
            if ident:
                return ident
    return None


class Device:
    def __init__(self, link, ident, port, session):
        self.link, self.ident, self.port, self.s = link, ident, port, session
        self.dir = None


NOT_FOUND = "No FM-1 found. Plug it in with USB, switch it on, and try again."


def connect(backend, port, ui):
    """the identity gate (spec §5.6 P7), then HELLO: -> Device. Only an FM-1 answering as CYMA-1 gets more than
    HS_QUERY"""
    names = backend.input_names()
    cands = candidates(names, port)
    if not cands:
        raise CymaError(EXIT_NOTFOUND, NOT_FOUND if not port else
                        f"No MIDI port matches {port!r}. Run: cyma_sounds.py ports")
    outs = backend.output_names()
    refused = []
    for name in cands:
        out = pair_output(name, outs)
        if not out:
            continue
        try:
            link = backend.open(name, out)
        except CymaError:
            raise
        except Exception:
            continue
        if ui.trace:
            link = TraceLink(link, ui.err)
        ident = handshake(link)
        if ident and is_cyma_identity(ident):
            return hello(link, ident, name, ui)
        if ident:
            refused.append(ident)
        link.close()
    if refused:
        raise CymaError(EXIT_FIRMWARE, identity_refusal(refused[0]))
    raise CymaError(EXIT_NOTFOUND, NOT_FOUND)


def hello(link, ident, name, ui):
    s = Session(link, ui)
    try:
        h = s.do_hello()
    except NoAnswer:
        link.close()
        raise CymaError(EXIT_FIRMWARE, "This CYMA-1 is too old for sound transfer. Update it.") from None
    except CymaError:
        link.close()
        raise
    if h.app != "CYMA-1":
        link.close()
        raise CymaError(EXIT_FIRMWARE, f"This FM-1 runs other firmware ({shown_bytes_text(ident)}, "
                                       f"{shown_bytes_text(h.app)}), not CYMA-1.")
    if h.proto != 1:
        link.close()
        raise CymaError(EXIT_FIRMWARE, f"This CYMA-1 speaks sound protocol {h.proto}; these tools speak 1. Update "
                                       f"the tools.")
    return Device(link, ident, name, s)


def headline(dev):
    h = dev.s.hello
    used = bin(dev.dir.used).count("1") if dev.dir else 0
    where = ", virtual" if h.emulator else ""
    return (f'FM-1 on "{dev.port}": {h.app} {h.version} ({dev.ident}{where}), protocol {h.proto}, '
            f"{used} of {N_USER} slots used")


def read_bank(dev, ui=None):
    """GET_DIR and every used slot -> list of 32 (Sound or None)"""
    dev.dir = dev.s.get_dir()
    bank = [None] * N_USER
    for i in dev.dir.used_slots():
        bank[i] = dev.s.get_slot(i)
    return bank


def require_writable(dev):
    if dev.s.hello.locked:
        raise DeviceError(E_LOCKED)


# ========================================================================================== commands
def commit_line1(puts, erases, bank_before):
    """what the FM-1 asks (spec §6.6)"""
    if len(puts) == 1 and not erases:
        i = puts[0]
        return f"USB: {'REPLACE' if bank_before[i] else 'SAVE TO'} {slot_name(i)}?"
    if len(erases) == 1 and not puts:
        return f"USB: DELETE {slot_name(erases[0])}?"
    if puts and not erases:
        return f"USB: SAVE {len(puts)} SOUNDS?"
    return f"USB: CHANGE {len(set(puts) | set(erases))} SLOTS?"


def do_commit(dev, ui, puts, erases, bank_before, what):
    """COMMIT the staging and wait for the player on the FM-1; -> used_mask. CymaError on anything but yes"""
    line1 = commit_line1(puts, erases, bank_before)
    keep = "keep it" if len(puts) + len(erases) == 1 else "keep them"

    def pending():
        ui.say(f"Sent. On the FM-1: {line1} Press SAVE to {keep}, HOME to cancel (30 s).")

    st, used = dev.s.commit(pending)
    if st != OK:
        raise DeviceError(st, COMMIT)
    return used


def guard_staging(dev, ui, fn):
    """run fn; on Ctrl-C or an error after staging began, ABORT so nothing stays staged on the FM-1"""
    try:
        return fn()
    except KeyboardInterrupt:
        asked = dev.s.commit_seq
        if dev.s.staging:
            dev.s.abort()
        if asked is not None:                   # (SAVE may have been pressed just before the cancel arrived)
            try:
                d = dev.s.get_dir()
            except CymaError:
                raise CymaError(EXIT_NO, "Cancelled. " + CANT_TELL) from None
            if d.last_commit_seq == asked and d.last_result == OK:
                raise CymaError(EXIT_NO, "Cancelled, but the FM-1 had already saved before the cancel arrived.") \
                    from None
        raise CymaError(EXIT_NO, "Cancelled: nothing on the FM-1 was changed.") from None
    except CymaError as e:
        if dev.s.staging and not isinstance(e, Lost):
            dev.s.abort()
        raise


def cmd_ports(args, ui, backend):
    names = backend.input_names()
    cands = candidates(names, args.port)
    others = [n for n in names if n not in cands]
    if cands:
        ui.result("MIDI ports that could be an FM-1 (nothing is sent until a command needs it):")
        for n in cands:
            ui.result("  " + n)
    if others and not backend.virtual:
        ui.say("Other MIDI ports (never touched): " + ", ".join(others))
    if not cands:
        raise CymaError(EXIT_NOTFOUND, NOT_FOUND if not args.port else f"No MIDI port matches {args.port!r}.")
    return EXIT_OK


def cmd_info(args, ui, backend):
    dev = connect(backend, args.port, ui)
    try:
        dev.dir = dev.s.get_dir()
        ui.result(headline(dev))
        if dev.s.hello.locked:
            raise DeviceError(E_LOCKED)
        preset, flags, cur = dev.s.get_edit()
        state = [w for b, w in ((1, "edited"), (2, "from USB"), (4, "COMPARE held")) if flags & b]
        ui.result(f"Playing: {preset_label(preset)}{(' (' + ', '.join(state) + ')') if state else ''}"
                  f"{': ' + cur.name if preset >= 24 or flags & 3 else ''}")
        used = dev.dir.used_slots()
        if not used:
            ui.result("No sounds in U01-U32 yet.")
        for i in used:
            s = dev.s.get_slot(i)
            if s:
                ui.result(f"  {slot_name(i)}  {s.name:<14}  {describe(s)}")
        if dev.dir.state:
            ui.say("(The FM-1 is " + ("asking a question on its screen." if dev.dir.state == 2 else
                                      "holding staged changes from another session; they drop after 60 s.") + ")")
        return EXIT_OK
    finally:
        dev.link.close()


def cmd_get(args, ui, backend):
    what = args.what.lower()
    if what not in ("edit", "all"):
        slot = parse_slot(args.what)
    dev = connect(backend, args.port, ui)
    try:
        h = dev.s.hello
        if what == "all":
            if h.locked:
                raise DeviceError(E_LOCKED)
            bank = read_bank(dev)
            ui.say(headline(dev))
            sounds = [s for s in bank if s]
            if getattr(args, "backup_mode", False) and not args.output:
                folder = Path(os.environ.get("CYMA1_BACKUP_DIR") or BACKUP_DIR).expanduser()
                path = unique_path(folder, "FM-1 " + datetime.datetime.now().strftime("%Y-%m-%d %H-%M-%S"),
                                   ".cyma1-bank.json")
            else:
                path = out_path(args.output, "FM-1 " + datetime.datetime.now().strftime("%Y-%m-%d %H-%M")
                                + ".cyma1-bank.json")
            label = path.name[:-len(".cyma1-bank.json")] if path.name.endswith(".cyma1-bank.json") else path.stem
            write_new_file(path, dump_json(bank_to_json(sounds, label, where="fm1", version=h.version)), args.force)
            ui.result(f"Saved the FM-1's {len(sounds)} sound{'s' if len(sounds) != 1 else ''} to {path}.")
            return EXIT_OK
        if what == "edit":
            preset, flags, snd = dev.s.get_edit()
            src = preset_label(preset) + (", edited" if flags & 1 else "")
        else:
            if h.locked:
                raise DeviceError(E_LOCKED)
            snd = dev.s.get_slot(slot)
            if snd is None:
                raise CymaError(EXIT_ARGS, f"{slot_name(slot)} on the FM-1 is empty.")
            src = slot_name(slot)
        path = out_path(args.output, file_stem(snd.name) + ".cyma1.json")
        write_new_file(path, dump_json(sound_to_json(snd, where="fm1", version=h.version)), args.force)
        ui.result(f"Saved {snd.name} ({src}) to {path}.")
        return EXIT_OK
    finally:
        dev.link.close()


def one_sound(src):
    ld = load_source(src)
    if ld.kind != "sound":
        raise CymaError(EXIT_ARGS, f"{src}: holds {len(ld.sounds)} sounds; this takes one sound (a .cyma1.json "
                                   f"file, a code or a link). For a bank use push-bank.")
    return ld.sounds[0], ld


def indent_lines(text, pad):
    """multi-line text (a file's notes, already cleaned) with its later lines indented under the first"""
    return text.replace("\n", "\n" + pad)


def print_notes(ui, snd, prefix="  "):
    for m in snd.notes.messages():
        ui.say(prefix + m)
    if snd.author:
        ui.say(prefix + f"Credit: {snd.author} (a label in the file, not verified)")
    if snd.text:
        ui.say(prefix + "Notes: " + indent_lines(snd.text, prefix + "       "))


def cmd_audition(args, ui, backend):
    snd, _ = one_sound(args.source)
    print_notes(ui, snd)
    dev = connect(backend, args.port, ui)
    try:
        ui.say(headline(dev))
        dev.s.put_edit(snd, glide=True)
        ui.result(f"Playing {snd.name} on the FM-1. Press SAVE there to keep it, or SEL+SAVE to go back.")
        return EXIT_OK
    finally:
        dev.link.close()


def cmd_put(args, ui, backend):
    snd, _ = one_sound(args.source)
    print_notes(ui, snd)
    want = (args.slot or "free").lower()
    slot = None if want == "free" else parse_slot(want)
    dev = connect(backend, args.port, ui)
    try:
        require_writable(dev)
        dev.dir = dev.s.get_dir()
        ui.say(headline(dev))
        before = [None] * N_USER
        if slot is None:
            free = [i for i in range(N_USER) if not dev.dir.used >> i & 1]
            if not free:
                raise CymaError(EXIT_ARGS, "All 32 slots on the FM-1 are used. Pick one to replace with --slot Uxx, "
                                           "or erase one first.")
            slot = free[0]
        elif dev.dir.used >> slot & 1:
            old = before[slot] = dev.s.get_slot(slot)       # (None if it was emptied meanwhile)
            if old is not None and old.same(snd):
                ui.result(f"{slot_name(slot)} on the FM-1 already holds this {snd.name}: nothing to send.")
                return EXIT_OK
            if old is not None and args.dry_run:
                ui.result(f"Would replace {slot_name(slot)} {old.name} with {snd.name}.")
            elif old is not None and not ui.confirm(f"{slot_name(slot)} holds {old.name}. Replace it with {snd.name}?"):
                raise CymaError(EXIT_NO, "Nothing sent.")
        if args.dry_run:
            if not before[slot]:
                ui.result(f"Would save {snd.name} to {slot_name(slot)} (empty).")
            ui.result(f"Dry run: nothing written. Sent: {dev.s.summary()}.")
            return EXIT_OK

        def run():
            dev.s.put_slot(slot, snd)
            recheck(dev, ui)
            do_commit(dev, ui, [slot], [], before, snd.name)
        guard_staging(dev, ui, run)
        ui.result(f"Saved {snd.name} to {slot_name(slot)}.")
        return EXIT_OK
    finally:
        dev.link.close()


def recheck(dev, ui):
    """just before COMMIT: the FM-1's slots must still be as planned (a sound saved on it meanwhile stops the
    commit)"""
    d = dev.s.get_dir()
    if (d.staged, d.erased) != dev.s.mine:
        dev.s.abort()
        raise CymaError(EXIT_BUSY, "Another program is sending sounds to the FM-1 at the same time (the CYMA-1 "
                                   "page?), or the FM-1 dropped them, so nothing was changed. Try again.")
    if d.used != dev.dir.used:
        dev.s.abort()
        raise CymaError(EXIT_NO, "The FM-1's sounds changed while this was sending (was a sound saved on it?), so "
                                 "nothing was changed. Run the command again.")


def plan_add(sounds, bank):
    """add: free slots in file order, skipping exact duplicates of what the FM-1 holds -> (placement, dups, left)"""
    place, dups, left = {}, [], []
    free = [i for i in range(N_USER) if bank[i] is None]
    have = [s for s in bank if s]
    for s in sounds:
        if any(s.same(h) for h in have) or any(s.same(p) for p in place.values()):
            dups.append(s)
            continue
        if free:
            place[free.pop(0)] = s
        else:
            left.append(s)
    return place, dups, left


def plan_replace(sounds):
    """replace: each sound at its unique slot when it has one, the rest in order; the first 32 -> (placement, cut)"""
    use, cut = sounds[:N_USER], sounds[N_USER:]
    place = {}
    rest = []
    for s in use:
        if s.slot is not None and s.slot not in place:
            place[s.slot] = s
        else:
            rest.append(s)
    free = [i for i in range(N_USER) if i not in place]
    for s in rest:
        place[free.pop(0)] = s
    return place, cut


def cmd_push_bank(args, ui, backend):
    ld = load_source(args.bank)
    sounds = ld.sounds
    for m in ld.notes:
        ui.say("  " + m)
    for s in sounds:
        if s.notes:
            ui.say(f"  {s.name}: " + " ".join(s.notes.messages()))
    if not sounds:
        raise CymaError(EXIT_ARGS, f"{args.bank}: holds no sounds.")
    dev = connect(backend, args.port, ui)
    try:
        require_writable(dev)
        bank = read_bank(dev)
        ui.say(headline(dev))
        have = sum(1 for s in bank if s)
        if args.replace:
            place, cut = plan_replace(sounds)
            if cut:
                ui.say(f"This bank has {len(sounds)} sounds; the FM-1 holds 32. The first 32 will be used.")
            puts = sorted(i for i in place if not place[i].same(bank[i]))
            erases = sorted(i for i in range(N_USER) if bank[i] is not None and i not in place)
            ui.result(f"{'Would replace' if args.dry_run else 'Replacing'} all 32 slots (the FM-1 has {have} "
                      f"sound{'s' if have != 1 else ''}, the bank {min(len(sounds), N_USER)}):")
            empty_run = []
            for i in range(N_USER):
                old = bank[i].name if bank[i] else "(empty)"
                if i in place:
                    flush_empty(ui, empty_run)
                    new = place[i].name + ("  (unchanged)" if i not in puts else "")
                    ui.result(f"  {slot_name(i)}  {old}  ->  {new}")
                elif bank[i]:
                    flush_empty(ui, empty_run)
                    ui.result(f"  {slot_name(i)}  {old}  ->  (deleted)")
                else:
                    empty_run.append(i)
            flush_empty(ui, empty_run)
        else:
            place, dups, left = plan_add(sounds, bank)
            puts, erases = sorted(place), []
            for s in dups:
                ui.say(f"  {s.name} is already on the FM-1: skipped.")
            if left:
                nfree = len(place)
                ui.result(f"Only {nfree} slot{'s are' if nfree != 1 else ' is'} free on the FM-1; the bank has "
                          f"{nfree + len(left)} new sounds.")
                if not place:
                    raise CymaError(EXIT_ARGS, "Erase some sounds on the FM-1 first, or use --replace.")
                if not args.dry_run and not ui.confirm(f"Add the first {nfree}?"):
                    raise CymaError(EXIT_NO, "Nothing sent.")
            if not place:
                ui.result("Nothing to add: the FM-1 already has every sound in this bank.")
                return EXIT_OK
            n = len(place)
            ui.result(f"{'Would add' if args.dry_run else 'Adding'} {n} sound{'s' if n != 1 else ''}:")
            for i in puts:
                ui.result(f"  {slot_name(i)}  (empty)  ->  {place[i].name}")
        if not puts and not erases:
            ui.result("Nothing to change: the FM-1 already holds exactly these sounds.")
            return EXIT_OK
        backup = None
        if not args.no_backup:
            folder = Path(args.backup_dir).expanduser() if args.backup_dir else BACKUP_DIR
            backup = unique_path(folder, "FM-1 " + datetime.datetime.now().strftime("%Y-%m-%d %H-%M-%S"),
                                 ".cyma1-bank.json")
            ui.result(f"Safety backup first: {tilde(backup)}")
        if args.dry_run:
            ui.result(f"Dry run: nothing written. Sent: {dev.s.summary()}.")
            return EXIT_OK
        if args.replace:
            q = f"Replace all 32 slots on the FM-1 with these {len(place)} sound{'s' if len(place) != 1 else ''}?"
            if erases or any(bank[i] for i in puts):
                q += (f" The FM-1's {have} sound{'s' if have != 1 else ''} will be gone"
                      + ((" (it is" if have == 1 else " (they are") + " in the safety backup)." if backup
                         else (" unless you get it first." if have == 1 else " unless you get them first.")))
            if not ui.confirm(q):
                raise CymaError(EXIT_NO, "Nothing sent.")
        if backup:
            label = backup.name[:-len(".cyma1-bank.json")]
            write_new_file(backup, dump_json(bank_to_json([s for s in bank if s], label, where="fm1",
                                                          version=dev.s.hello.version)), False)
            ui.say(f"Saved the safety backup ({have} sound{'s' if have != 1 else ''}).")

        def run():
            for i in puts:
                dev.s.put_slot(i, place[i])
            for i in erases:
                dev.s.erase_slot(i)
            recheck(dev, ui)
            return do_commit(dev, ui, puts, erases, bank, "")
        guard_staging(dev, ui, run)
        verify(dev, place, erases, puts)
        n = len(place) if args.replace else len(puts)
        ui.result(f"Saved {n} sound{'s' if n != 1 else ''} on the FM-1. On the FM-1, turn PRESETS past 24 to find "
                  f"them.")
        return EXIT_OK
    finally:
        dev.link.close()


def flush_empty(ui, run):
    if not run:
        return
    a, b = run[0], run[-1]
    r = slot_name(a) if a == b else f"{slot_name(a)}-{slot_name(b)}"
    ui.result(f"  {r}  (empty)  stay{'s' if a == b else ''} empty")
    run.clear()


def tilde(p):
    try:
        return "~/" + str(Path(p).resolve().relative_to(Path.home().resolve()))
    except ValueError:
        return str(p)


def verify(dev, place, erases, puts):
    """after the FM-1 said yes: GET_DIR and GET_SLOT of each changed slot must read back what was sent"""
    d = dev.s.get_dir()
    for i in puts:
        got = dev.s.get_slot(i)
        if not place[i].same(got):
            raise CymaError(EXIT_NO, f"The FM-1 said it saved, but {slot_name(i)} reads back differently. Run "
                                     f"cyma_sounds.py info, and try again.")
    for i in erases:
        if d.used >> i & 1:
            raise CymaError(EXIT_NO, f"The FM-1 said it saved, but {slot_name(i)} still holds a sound.")


def cmd_erase(args, ui, backend):
    slot = parse_slot(args.slot)
    dev = connect(backend, args.port, ui)
    try:
        require_writable(dev)
        dev.dir = dev.s.get_dir()
        ui.say(headline(dev))
        if not dev.dir.used >> slot & 1:
            ui.result(f"{slot_name(slot)} on the FM-1 is already empty.")
            return EXIT_OK
        old = dev.s.get_slot(slot)
        before = [None] * N_USER
        before[slot] = old
        if args.dry_run:
            ui.result(f"Would delete {slot_name(slot)} {old.name if old else ''}.")
            ui.result(f"Dry run: nothing written. Sent: {dev.s.summary()}.")
            return EXIT_OK
        if not ui.confirm(f"Delete {slot_name(slot)} {old.name if old else ''} from the FM-1?"):
            raise CymaError(EXIT_NO, "Nothing sent.")

        def run():
            dev.s.erase_slot(slot)
            recheck(dev, ui)
            do_commit(dev, ui, [], [slot], before, old.name if old else "")
        guard_staging(dev, ui, run)
        ui.result(f"Deleted {old.name if old else 'the sound'} from {slot_name(slot)} on the FM-1.")
        return EXIT_OK
    finally:
        dev.link.close()


def cmd_encode(args, ui, backend=None):
    """the code is always cy1.; with --base, the links: auto = the cy1. link and, when it fits claude.ai's anchor,
    the short cy2. link; cy1 = the cy1. link only; cy2 = the short link only (exit 2 when a sound doesn't fit)"""
    ld = load_source(args.file)
    many = len(ld.sounds) > 1
    mode = getattr(args, "link", None) or "auto"
    if mode != "auto" and not args.base:
        raise CymaError(EXIT_ARGS, "--link needs --base URL (the page's address).")
    if args.base:
        share_link(args.base, "")                       # (a bad base is refused before anything is printed)
    rows = []
    for s in ld.sounds:
        code = code_encode(s.name, s.q)
        short = short_code(s.name, s.q)
        if mode == "cy2" and not short:
            raise CymaError(EXIT_ARGS, f"{s.name} changes too many settings for a short link (its cy2. code is "
                                       f"{len(code2_encode(s.name, s.q))} characters; claude.ai passes at most "
                                       f"{ANCHOR_MAX} after the #). Share the code or the file instead.")
        rows.append((s, code, short))
    for s, code, short in rows:
        link = share_link(args.base, code) if args.base and mode != "cy2" else None
        slink = share_link(args.base, short) if args.base and short and mode != "cy1" else None
        if many:
            cols = [s.name, code]
            if args.base and mode == "auto":
                cols += [link, slink or "-"]
            elif args.base:
                cols += [link or slink]
            ui.result("\t".join(cols))
            continue
        for m in s.notes.messages():
            ui.say(m)
        ui.result(code)
        if link:
            ui.result(link)
        if slink and link:
            ui.say(f"Short link ({len(short)} characters after the #: it opens inside claude.ai, which passes at most "
                   f"{ANCHOR_MAX}):")
        if slink:
            ui.result(slink)
        if args.base and mode == "auto" and not short:
            ui.say(f"(No short link: {s.name} changes too many settings, so even its cy2. form is "
                   f"{len(code2_encode(s.name, s.q))} characters, and claude.ai passes at most {ANCHOR_MAX} after the "
                   f"#. Inside claude.ai, share the code or the file.)")
    if many and args.base and mode == "auto":
        ui.say(f"(Columns: name, code, link, short link for claude.ai or - when the sound doesn't fit "
               f"{ANCHOR_MAX} characters.)")
    return EXIT_OK


def cmd_decode(args, ui, backend=None):
    ld = load_source(args.source)
    if len(ld.sounds) > 1 and args.output and not Path(args.output).is_dir():
        raise CymaError(EXIT_ARGS, f"{args.source} holds {len(ld.sounds)} sounds: give -o a folder.")
    for s in ld.sounds:
        print_notes(ui, s, "")
        path = out_path(args.output, file_stem(s.name) + ".cyma1.json")
        write_new_file(path, dump_json(sound_to_json(s, where="cli")), args.force)
        ui.result(f"Saved {s.name} ({describe(s)}) to {path}.")
    return EXIT_OK


def cmd_validate(args, ui, backend=None):
    bad = 0
    for f in args.files:
        try:
            ld = read_document_bytes(read_file_bytes(f), Path(f).name)
        except CymaError as e:
            ui.result(f"{f}: {e}")
            bad += 1
            continue
        notes = list(ld.notes)
        for s in ld.sounds:
            for m in s.notes.messages():
                notes.append((f"{s.name}: " if len(ld.sounds) > 1 else "") + m)
        what = {"sound": f"sound {ld.sounds[0].name}" if ld.sounds else "sound",
                "bank": f"bank of {len(ld.sounds)} sound{'s' if len(ld.sounds) != 1 else ''}"
                        + (f" ({ld.label})" if ld.label else ""),
                "codes": f"{len(ld.sounds)} codes"}[ld.kind]
        strict_bad = args.strict and notes
        ui.result(f"{f}: {'ok' if not notes else ('not clean' if strict_bad else 'ok, with notes')}: {what}")
        for m in notes:
            ui.result("  - " + m)
        for s in ld.sounds:
            who = f" ({s.name})" if len(ld.sounds) > 1 else ""
            if s.author:
                ui.result(f"  credit{who}: {s.author} (a label in the file, not verified)")
            if s.text:
                ui.result(f"  notes{who}: " + indent_lines(s.text, " " * (9 + len(who))))
        if strict_bad:
            bad += 1
    return EXIT_ARGS if bad else EXIT_OK


# ============================================================================================ selftest
def cmd_selftest(args, ui, backend=None):
    path = Path(args.vectors) if args.vectors else VECTORS_PATH
    try:
        vec = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        raise CymaError(EXIT_ARGS, f"Couldn't read the vectors ({path}): {e}") from None
    t = table()
    fails, n = [], 0

    def check(ok, what):
        nonlocal n
        n += 1
        if not ok:
            fails.append(what)

    check("%08X" % crc32(bytes.fromhex(vec["crc32_check"]["input_hex"])) == vec["crc32_check"]["crc32"], "crc32")
    for s, c in vec["names"]:
        check(clean_name(s) == c, f"name {s!r} -> {clean_name(s)!r}, want {c!r}")
    tf = {f["num"]: f for f in t.factory}
    for f in vec["factory"]:
        q = f["q16"]
        check(tf.get(f["num"], {}).get("q16") == q and tf.get(f["num"], {}).get("name") == f["name"],
              f"factory {f['num']}: tools/cyma1_params.json drifted from the vectors")
        check(rec_encode(f["name"], q).hex() == f["record_hex"], f"record {f['name']}")
        check(code_encode(f["name"], q) == f["code"], f"code {f['name']}")
        mine = {p["key"]: json_value(p, q[p["id"]]) for p in t.params}
        check(json.dumps(mine) == json.dumps(f["params"]), f"writer {f['name']}")
        qj, notes = sound_params_from_json(f["params"])
        check(qj == q and not notes, f"reader {f['name']}")
        check(code_decode(f["code"]) == (f["name"], q, 0, 0), f"decode {f['name']}")
        snd = Sound(f["name"], q)
        back = read_document_bytes(dump_json(sound_to_json(snd)).encode(), "x")
        check(back.kind == "sound" and back.sounds[0].same(snd) and not back.sounds[0].notes, f"file {f['name']}")
    bowl = vec["factory"][0]
    check(code_decode("Listen: https://claude.ai/artifact/x#" + bowl["code"][:60] + "\n" + bowl["code"][60:] + ").")
          == (bowl["name"], bowl["q16"], 0, 0), "decode inside text")
    for kind, lst in vec["bad_codes"].items():
        for c in lst:                           # ("cy2.AQEo" is still notcode: a cy2. needs 10 token characters)
            try:
                code_decode(c)
                check(False, f"bad code accepted: {c[:24]}")
            except FormatError as e:
                check(e.code == kind, f"bad code {c[:24]}: {e.code}, want {kind}")
    selftest_cy2(vec, check)
    selftest_text_and_identity(check)
    r38 = rec_decode(bytes.fromhex(vec["records_tolerated"]["n38_hex"]))
    r42 = rec_decode(bytes.fromhex(vec["records_tolerated"]["n42_hex"]))
    check(r38[1][:38] == bowl["q16"][:38] and r38[1][38:] == t.def_q16[38:] and r38[3] == 2, "record N=38")
    check(r42[1] == bowl["q16"] and r42[2] == 2, "record N=42")
    edge = vec["json_import_edge"]
    qe, ne = sound_params_from_json(edge["params"])
    check(qe == edge["q16"] and ne.skipped == edge["skipped"] and len(ne.missing) == edge["missing_count"],
          "json import edge")
    for k, hx in vec["frames"].items():
        fr = bytes.fromhex(hx)
        try:
            _, cmd, seq, payload = sx_parse(fr)
            check(sx_frame(cmd, seq, payload) == fr, f"frame {k}")
            check(allowed(fr), f"frame {k} refused by the allowlist")
        except FormatError as e:
            check(False, f"frame {k}: {e.code}")
    for k, hx in vec["not_ours"].items():
        fr = b"\xF0" + bytes.fromhex(hx) + b"\xF7"
        check(allowed(fr) == (k == "HS_QUERY"), f"allowlist on {k}")
    check(not allowed(bytes([0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7])) and
          not allowed(bytes([0xF0, 0x22, 0x24, 0x35, 0x7D, 0xF7])), "allowlist on the update keys")
    check(not allowed(sx_frame(PUT_SLOT, 1, bytes(SX_MAX_PAYLOAD)) + b"\x00"), "allowlist on length")
    check(parse_identity(identity_frame("FM-1_9400")) == "FM-1_9400", "identity frame")
    if not args.quick:
        dg = vec.get("writer_digest") or {}
        for p in t.params:
            dom = range(65536) if p["curve"] != "ENUM" and not p["int"] else \
                [q16(k / grid(p)) for k in range(grid(p) + 1)]
            vals = [json_value(p, q) for q in dom]
            text = "\n".join(fixed6(v) for v in vals)
            check(hashlib.sha256(text.encode()).hexdigest() == dg.get(p["key"]), f"writer digest {p['key']}")
            bad = [q for q, v in zip(dom, vals) if from_json_value(p, json.loads(json.dumps(v)))[0] != q]
            check(not bad, f"json round trip {p['key']}: {len(bad)} values")
    if fails:
        for f in fails[:40]:
            ui.result("FAIL " + f)
        ui.result(f"selftest: {len(fails)} of {n} checks failed")
        return EXIT_NO
    ui.result(f"selftest: {n} checks ok ({path}{', quick' if args.quick else ', with the exhaustive writer check'})")
    return EXIT_OK


def selftest_cy2(vec, check):
    """cy2.: every factory sound both ways and inside text; the frozen base; what fits; the decoder's refusals;
    truncations and substitutions never accepted"""
    t = table()
    check(list(CY2_BASE) == t.def_q16[:len(CY2_BASE)], "CY2_BASE is no longer param_set 1's def_q16 (cy2. keeps its "
                                                         "frozen base; check that the table change is intended)")
    lens = []
    for f in vec["factory"]:
        q = f["q16"]
        c2 = code2_encode(f["name"], q)
        lens.append(len(c2))
        check(code_decode(c2) == (f["name"], q, 0, 0), f"cy2 decode {f['name']}")
        check(short_code(f["name"], q) == c2 and len(c2) <= ANCHOR_MAX, f"cy2 {f['name']} fits the anchor")
        body = rec2_encode(f["name"], q)
        k = sum(1 for i, v in enumerate(q) if v != CY2_BASE[i])
        check(len(body) == 2 + 5 + len(f["name"]) + 2 * k, f"cy2 body length {f['name']}")
        check(code_decode("#" + c2) == code_decode("https://claude.ai/artifact/x#" + c2), f"cy2 link {f['name']}")
    bowl = vec["factory"][0]
    c2 = code2_encode(bowl["name"], bowl["q16"])
    check(len(c2) == 67, f"cy2 SINGING BOWL is 67 characters (the page's figure), not {len(c2)}")
    want = (bowl["name"], bowl["q16"], 0, 0)
    check(code_decode("Listen: https://claude.ai/artifact/x#" + c2[:30] + "\n  " + c2[30:] + ").") == want,
          "cy2 inside wrapped text")
    check(code_decode(c2[:7] + "\n" + c2[7:]) == want, "cy2 wrapped before its 10th character")
    check(read_document_bytes(("see cy2." + c2[4:9] + "\n" + c2[9:] + "\n").encode(), "x").sounds[0].same(
        Sound(*want[:2])), "a file holding one cy2. wrapped before its 10th character")
    check(code_decode(c2[:20] + "\u200b" + c2[20:40] + "\xad" + c2[40:]) == want, "cy2 with zero-width breaks")
    both = read_document_bytes(f"code {bowl['code']}\nlink https://claude.ai/artifact/x#{c2}\n".encode(), "x")
    check(both.kind == "sound" and len(both.sounds) == 1 and both.sounds[0].name == bowl["name"],
          "a message with the code and its short link is one sound")
    two = read_document_bytes(f"{c2}\n{vec['factory'][1]['code']}\n".encode(), "x")
    check(two.kind == "codes" and [s.name for s in two.sounds] == [bowl["name"], vec["factory"][1]["name"]],
          "a list of a cy2. and a cy1. code")
    # the extremes: all defaults (k = 0), every value changed with a 14-letter name (does not fit), N = 1 and N = 64
    d0 = code2_encode("", list(CY2_BASE))
    check(code_decode(d0) == ("UNTITLED", list(CY2_BASE), 0, 0) and len(d0) == 4 + 26, "cy2 all defaults")
    qx = [(v + 1) & 0xFFFF for v in CY2_BASE]
    cx = code2_encode("ABCDEFGHIJKLMN", qx)
    check(code_decode(cx) == ("ABCDEFGHIJKLMN", qx, 0, 0) and short_code("ABCDEFGHIJKLMN", qx) is None
          and len(cx) == 4 + 140, "cy2 every value changed: decodes, does not fit")
    fit = list(CY2_BASE)
    for i in range(34):
        fit[i] = (fit[i] + 1) & 0xFFFF
    check(short_code("ABCDEFGHIJKLMN", fit) is not None and len(code2_encode("ABCDEFGHIJKLMN", fit)) == 128,
          "cy2: 34 changed values and 14 letters is exactly 128")
    fit[34] = (fit[34] + 1) & 0xFFFF
    check(short_code("ABCDEFGHIJKLMN", fit) is None, "cy2: 35 changed values and 14 letters does not fit")
    r1 = code_decode(code2_encode("ONE", [123]))
    check(r1[1][0] == 123 and r1[1][1:] == list(CY2_BASE[1:]) and r1[2:] == (0, t.n - 1), "cy2 N=1 (older)")
    r64 = code_decode(code2_encode("SIXTYFOUR", list(bowl["q16"]) + [7] * 24))
    check(r64[1] == bowl["q16"] and r64[2:] == (24, 0), "cy2 N=64 (newer)")

    def blob_code(body):
        return CODE2_PREFIX + base64.urlsafe_b64encode(body + struct.pack("<I", crc32(body))).decode().rstrip("=")

    def kind(code):
        try:
            code_decode(code)
            return "ok"
        except FormatError as e:
            return e.code
    good = rec2_encode(bowl["name"], bowl["q16"])
    check(kind(blob_code(good)) == "ok", "cy2 rebuilt from its body")
    check(kind(blob_code(good[:-1])) == "damaged", "cy2 short by a byte -> damaged")
    check(kind(blob_code(good + b"\0")) == "damaged", "cy2 a byte over -> damaged")
    stray = bytes([36]) + good[1:6] + bytes([good[6] | 0x80]) + good[7:] + b"\0\0"   # bit 39 with N = 36
    check(kind(blob_code(stray)) == "damaged", "cy2 a bitmap bit at or above N -> damaged")
    check(kind(blob_code(good[:1] + bytes([33]) + good[2:])) == "damaged", "cy2 L 33 -> damaged")
    check(kind(blob_code(bytes([0]) + good[1:])) == "damaged", "cy2 N 0 -> damaged")
    check(kind(blob_code(bytes([65]) + good[1:])) == "damaged", "cy2 N 65 -> damaged")
    check(kind("cy2.AQEo") == "notcode", "cy2 with fewer than 10 token characters is not a code")
    check(kind("cy3.AQEoDFNJTkdJ") == "newer" and kind("see #cy9.AAAAAAAAAAAA") == "newer", "cy3. to cy9. -> newer")
    check(kind("cy3.AQEo") == "notcode", "cy3. with a short token is not a code")
    a64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
    if (len(c2) - 4) % 4:                                # (the last character has unused low bits: set one)
        odd = c2[:-1] + a64[a64.index(c2[-1]) | 1]
        check(odd != c2 and kind(odd) == "damaged", "cy2 only in its canonical text (the unused bits zero)")
    wrong = tried = 0
    for i in range(1, len(c2)):                          # every truncation
        tried += 1
        wrong += kind(c2[:i]) == "ok"
    rnd = random.Random(5)
    alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
    for i in range(4, len(c2)):                          # 5 substitutions at every character of the token
        for ch in rnd.sample([x for x in alpha if x != c2[i]], 5):
            tried += 1
            m = c2[:i] + ch + c2[i + 1:]
            if kind(m) == "ok" and code_decode(m) != want:
                wrong += 1
    check(wrong == 0 and tried > 300, f"cy2 robustness: {wrong} of {tried} truncations or substitutions wrongly "
                                      f"accepted")
    check(min(lens) == 50 and max(lens) == 74, f"cy2 factory lengths {min(lens)}-{max(lens)}, the page says 50-74")


def selftest_text_and_identity(check):
    """the free-text rule (author, notes, labels, keys) and the identity rule"""
    check(clean_author("\u202eKRAM\u202c \u200b\u2066x\u2069") == "KRAM x", "author: bidi and zero-width removed")
    check(clean_author("Mark\u0007\u001b[2J") == "Mark[2J", "author: controls removed")
    check(clean_author("\ufeff\xadA\u200dB\U000e0041\ufe0f\u3164\u2800") == "AB",
          "author: BOM, soft hyphen, ZWJ, tag, variation selector, Hangul filler, blank removed")
    check(clean_author("\ud800Mark") == "Mark", "author: a lone surrogate removed")
    check(clean_author("A\xa0\u2003\tB") == "A B", "author: other spaces become one plain space")
    check(clean_author("e" + "\u0301" * 30) == "e" + "\u0301" * 4, "author: at most 4 combining marks in a row")
    check(clean_author("x" * 40) == "x" * 32 and clean_author("  \u200b ") is None, "author: limit, empty")
    check(clean_author("\U0001f3b9" * 40) == "\U0001f3b9" * 32, "author: the limit counts characters")
    check(clean_notes("line one \r\n line two\u2028three\tfour\u202e\n\n\n\nfive") ==
          "line one\nline two\nthree four\n\nfive", "notes: line breaks kept, at most one empty line")
    check(len(clean_notes("n" * 1000)) == NOTES_MAX, "notes: limit")
    check(clean_text("BANK\u2029NAME", 80) == "BANK NAME", "a label is one line")
    check(shown_key("flutter\u202e\u001b[31m") == "flutter[31m", "unknown key shown clean")
    check(shown_bytes_text("FM-1_9400\x00junk\x1b\xff\\") == "FM-1_9400\\x00junk\\x1b\\xff\\\\",
          "identity shown as printable ASCII and \\xNN")
    for raw, want in ((b"FM-1_9400", "FM-1_9400"), (b"FM-1_94001234", "FM-1_94001234"),
                      (b"FM-1_9400\0junk", "FM-1_9400\x00junk"), (b"\0\0FM-1_9400", "\x00\x00FM-1_9400"),
                      (b"FM-1_9400" + b"\0" * 18, "FM-1_9400"), (b"", "")):
        got = parse_identity(identity_frame(raw))
        check(got == want, f"identity {raw!r} -> {got!r}, want {want!r}")
    check(is_cyma_identity("FM-1_9400") and is_cyma_identity("FM-1_94123456"), "identity: CYMA-1")
    for bad in ("FM-1_9400\x00junk", "FM-1_9400\x00", "FM-1_9400 ", "fm-1_9400", "FM-1_940012", "FM-1_015", "",
                "FM-1_94\xff", "XFM-1_9400", "\x00FM-1_9400"):
        check(not is_cyma_identity(bad), f"identity {bad!r} refused")


def fixed6(x):
    if isinstance(x, str):
        return x
    s = "%.6f" % x
    s = s.rstrip("0").rstrip(".") if "." in s else s
    return "0" if s == "-0" else s


# ================================================================================================ main
def make_backend(args):
    if args.virtual:
        store = getattr(args, "virtual_store", None)
        return BridgeBackend(wasm=args.wasm, answer=args.virtual_answer, identity=args.virtual_identity,
                             store=store, answer_ms=args.virtual_answer_ms, die_after=args.virtual_die_after,
                             identity_hex=args.virtual_identity_hex)
    return MidoBackend()


def build_parser():
    common = argparse.ArgumentParser(add_help=False)
    g = common.add_argument_group("connection (before or after the command)")
    g.add_argument("--port", metavar="NAME", default=argparse.SUPPRESS,
                   help="use the MIDI port whose name contains NAME (default: fm1-base's FM-1 port names)")
    g.add_argument("--virtual", action="store_true", default=argparse.SUPPRESS,
                   help="a virtual FM-1: the firmware's app.wasm in node (tools/sx_bridge.mjs); no MIDI at all")
    g.add_argument("--wasm", metavar="PATH", default=argparse.SUPPRESS,
                   help="--virtual: the app.wasm to run (default build/emu/app.wasm)")
    g.add_argument("--virtual-answer", choices=("yes", "no", "none"), default=argparse.SUPPRESS,
                   help="--virtual: how the virtual FM-1's player answers its question (default yes)")
    g.add_argument("--virtual-store", metavar="FILE", default=argparse.SUPPRESS,
                   help="--virtual: keep the virtual FM-1's memory in FILE between runs (default: forget it)")
    g.add_argument("--virtual-identity", "--identity", dest="virtual_identity", metavar="ID",
                   default=argparse.SUPPRESS, help="--virtual: the identity it answers (default FM-1_9400)")
    g.add_argument("--virtual-answer-ms", type=int, metavar="MS", default=argparse.SUPPRESS, help=argparse.SUPPRESS)
    g.add_argument("--virtual-die-after", type=int, metavar="N", default=argparse.SUPPRESS,
                   help=argparse.SUPPRESS)       # (a test hook: the bridge dies at its Nth frame)
    g.add_argument("--virtual-identity-hex", metavar="HEX", default=argparse.SUPPRESS,
                   help=argparse.SUPPRESS)       # (a test hook: the identity reply's 27 bytes, as hex)
    g.add_argument("--yes", "-y", action="store_true", default=argparse.SUPPRESS,
                   help="answer this computer's questions yes (the FM-1 still asks)")
    g.add_argument("-q", "--quiet", action="store_true", default=argparse.SUPPRESS,
                   help="print only results and errors")
    g.add_argument("--force", action="store_true", default=argparse.SUPPRESS,
                   help="replace files that already exist")
    g.add_argument("--trace", action="store_true", default=argparse.SUPPRESS,
                   help="print every frame sent (->) and received (<-) on stderr, as a MIDI monitor would")

    ap = argparse.ArgumentParser(prog="cyma_sounds.py", parents=[common],
                                 description="Save, restore and share CYMA-1 sounds: files, codes and links, and an "
                                             "FM-1 running CYMA-1 over USB.",
                                 epilog="SOURCE is a .cyma1.json file, a cy1. code, or a link (#cy1. or #cy2.). "
                                        "The FM-1 always asks "
                                        "on its own screen before it saves anything (press SAVE there). Exit codes: "
                                        "0 done, 1 declined/timed out/cancelled, 2 bad arguments/file/code, 3 FM-1 "
                                        "not found, 4 connection lost, 5 busy, 6 wrong firmware/protocol/locked bank.")
    sub = ap.add_subparsers(dest="command", metavar="COMMAND")
    sub.required = True

    def cmd(name, help_, aliases=()):
        return sub.add_parser(name, parents=[common], help=help_, aliases=list(aliases))

    cmd("ports", "candidate MIDI ports (fm1-base's FM-1 names only: never probes other gear)")
    cmd("info", "identity, HELLO, the slot list (read-only)", ["list"])
    p = cmd("get", "one sound -> NAME.cyma1.json; all -> a bank file")
    p.add_argument("what", metavar="edit|U01..U32|all")
    p.add_argument("-o", "--output", metavar="FILE")
    p = cmd("backup", "the FM-1's sounds into a bank file (get all; default in ~/Music/CYMA-1/backups)")
    p.add_argument("-o", "--output", metavar="FILE")
    p = cmd("audition", "play a sound on the FM-1 (PUT_EDIT): nothing saved")
    p.add_argument("source", metavar="SOURCE")
    p = cmd("put", "save a sound into a slot (the FM-1 asks)")
    p.add_argument("source", metavar="SOURCE")
    p.add_argument("--slot", metavar="Uxx|free", default="free")
    p.add_argument("--dry-run", action="store_true", help="read only, print the plan")
    for name, h in (("push-bank", "add a bank's sounds to free slots, or --replace all (the FM-1 asks)"),
                    ("restore", "replace the FM-1's sounds with a bank's (push-bank --replace)")):
        p = cmd(name, h)
        p.add_argument("bank", metavar="BANK")
        if name == "push-bank":
            p.add_argument("--replace", action="store_true", help="replace all 32 slots")
        p.add_argument("--no-backup", action="store_true", help="skip the safety backup of the FM-1's bank")
        p.add_argument("--backup-dir", metavar="DIR", default=os.environ.get("CYMA1_BACKUP_DIR"),
                       help="where the safety backup goes (default ~/Music/CYMA-1/backups)")
        p.add_argument("--dry-run", action="store_true", help="read only, print the plan")
    p = cmd("erase", "delete a slot (the FM-1 asks)")
    p.add_argument("slot", metavar="Uxx")
    p.add_argument("--dry-run", action="store_true", help="read only, print the plan")
    p = cmd("encode", "print a sound's code (and link)")
    p.add_argument("file", metavar="FILE")
    p.add_argument("--base", metavar="URL", help="the page's address, for a link")
    p.add_argument("--link", choices=("auto", "cy1", "cy2"), default="auto",
                   help="with --base: auto (the default) prints the cy1. link and the short cy2. link when it fits "
                        "claude.ai's 128-character anchor; cy1 or cy2 prints just that one")
    p = cmd("decode", "a code or link (cy1. or cy2.) -> NAME.cyma1.json")
    p.add_argument("source", metavar="SOURCE")
    p.add_argument("-o", "--output", metavar="FILE")
    p = cmd("validate", "check sound and bank files offline (spec §3.3); prints notes")
    p.add_argument("files", nargs="+", metavar="FILE")
    p.add_argument("--strict", action="store_true", help="notes count as failures (a curated library)")
    p = cmd("selftest", "the codec against tests/sounds/vectors.json")
    p.add_argument("--vectors", metavar="PATH")
    p.add_argument("--quick", action="store_true", help="skip the exhaustive writer check (~10 s)")
    return ap


DEFAULTS = {"port": None, "virtual": False, "wasm": None, "virtual_answer": "yes", "virtual_store": None,
            "virtual_identity": None, "virtual_answer_ms": None, "virtual_die_after": None,
            "virtual_identity_hex": None, "yes": False,
            "quiet": False, "force": False, "trace": False}

COMMANDS = {"ports": cmd_ports, "info": cmd_info, "list": cmd_info, "get": cmd_get, "audition": cmd_audition,
            "put": cmd_put, "push-bank": cmd_push_bank, "erase": cmd_erase, "encode": cmd_encode,
            "decode": cmd_decode, "validate": cmd_validate, "selftest": cmd_selftest}
OFFLINE = ("encode", "decode", "validate", "selftest")


def main(argv=None, out=None, err=None):
    ap = build_parser()
    args = ap.parse_args(argv)
    for k, v in DEFAULTS.items():
        if not hasattr(args, k):
            setattr(args, k, v)
    ui = Ui(quiet=args.quiet, yes=args.yes, out=out, err=err, trace=args.trace)
    command = args.command
    if command == "backup":
        command, args.what, args.backup_mode = "get", "all", True
    elif command == "restore":
        command, args.replace = "push-bank", True
    try:
        backend = None if command in OFFLINE else make_backend(args)
        return COMMANDS[command](args, ui, backend)
    except CymaError as e:
        ui.warn(str(e))
        return e.code
    except NotAllowed as e:                     # (a bug: never reached by the commands above)
        ui.warn(f"cyma_sounds: {e}")
        return EXIT_FIRMWARE
    except KeyboardInterrupt:
        ui.warn("Cancelled.")
        return EXIT_NO
    except BrokenPipeError:
        return EXIT_NO


if __name__ == "__main__":
    sys.exit(main())
