#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""CYMA-1 Sounds T7 (docs/CYMA-1-sounds-spec.md §9, §10, §11.1): tools/cyma_sounds.py, tested from the outside.

    python3 tools/test_cyma_sounds.py [-v] [-k PATTERN] [--quick]

Offline: selftest; every factory code decodes to its vector file and encodes back to the same code; a wrapped link in
chat text decodes; the bad codes and a corpus of bad files (too big, not JSON, nested 100,000 deep, newer, a bank of
257, binary, prototype pollution, unknown and out-of-range values) through validate and decode give exit 2 for what is
refused and exit 0 with notes for what is only odd, and never a traceback; files are never overwritten without
--force; file names follow §3.3. The short link code (spec revision 2026-10-24b), against vectors.json "cy2", the
same vectors the page is held to: every factory link code decodes, encode --base prints exactly the short link, the
edges (128 fits, 131 does not: --link cy2 exits 2), the refusals, N = 38 / 42, format characters inside a code; and a
file's credit, notes and keys lose their Unicode format characters on the terminal and in what is written.

The virtual FM-1 (--virtual: tools/sx_bridge.mjs running build/emu/app.wasm, the firmware's own code): info; put
--slot free answered yes, and the slot reads back equal; answered no -> exit 1, nothing written; audition writes
nothing; get all; erase; push-bank --replace writes the safety backup before its first write; --dry-run sends reads
only; a stock FM-1 (--identity FM-1_015) and other firmware -> exit 6 with only the identity query sent; the bridge
killed mid-push -> exit 4 and the bank unchanged; a locked bank -> exit 6, unchanged. Every frame any command sent is
checked against the send allowlist (§5.6 P7).

Safety: every device command here runs with --virtual (the helper refuses anything else), so nothing is sent to a MIDI
port; HOME and CYMA1_BACKUP_DIR point into a temporary folder, so nothing is written under ~/Music.
Exit 0 all passed, 1 a failure, 77 no tools/cyma_sounds.py yet. --quick skips the exhaustive selftest.
"""
import base64
import json
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
APP = HERE.parent
CLI = HERE / "cyma_sounds.py"
BRIDGE = HERE / "sx_bridge.mjs"
WASM = Path(os.environ.get("CYMA1_TEST_WASM") or APP / "build" / "emu" / "app.wasm")
VEC = json.loads((APP / "tests" / "sounds" / "vectors.json").read_text(encoding="utf-8"))
TABLE = json.loads((HERE / "cyma1_params.json").read_text(encoding="utf-8")) if (HERE / "cyma1_params.json").exists() else None
QUICK = "--quick" in sys.argv
if QUICK:
    sys.argv.remove("--quick")
OFFLINE_CMDS = {"encode", "decode", "validate", "selftest"}
HS_QUERY = bytes.fromhex("F0003245000000407FF7")
WRITE_CMDS = {0x03, 0x05, 0x06, 0x08}               # PUT_EDIT, PUT_SLOT, ERASE_SLOT, COMMIT


# ------------------------------------------------------------------------- a small codec, for the fixtures
def crc32(b):
    return zlib.crc32(bytes(b)) & 0xFFFFFFFF


def rec_encode(name, q):
    nb = name.encode("ascii")
    return bytes([1, 1, len(q), len(nb)]) + nb + struct.pack("<%dH" % len(q), *q)


def code_of(name, q):
    r = rec_encode(name, q)
    return "cy1." + base64.urlsafe_b64encode(r + struct.pack("<I", crc32(r))).decode().rstrip("=")


def sound_doc(name, params, **extra):
    d = {"format": "cyma1-sound", "version": 1, "name": name, "created": "2026-10-24T18:20:00Z",
         "made_with": {"app": "CYMA-1", "version": "DEV", "where": "web"}, "param_set": 1, "params": params}
    d.update(extra)
    return d


def bank_bytes(entries, version=1, magic=0x31414D43):
    """a cy_user_bank_t: entries {slot: (name, q16[40])}"""
    b = bytearray(3020)
    used = 0
    struct.pack_into("<IHHI", b, 0, magic, version, 0, 0)
    for s, (name, q) in entries.items():
        o = 12 + 94 * s
        b[o:o + len(name)] = name.encode("ascii")
        struct.pack_into("<40H", b, o + 14, *q)
        used |= 1 << s
    struct.pack_into("<I", b, 8, used)
    return bytes(b)


def store_file(path, bank):
    Path(path).write_text(json.dumps({"2": base64.b64encode(bank).decode()}), encoding="utf-8")


def store_bank(path):
    """the OBJ_USER bytes the virtual FM-1 saved (the page's localStorage format)"""
    raw = json.loads(Path(path).read_text(encoding="utf-8"))
    if "cyma-1-store" in raw:
        raw = raw["cyma-1-store"]
    return base64.b64decode(raw["2"]) if "2" in raw else b""


def bank_used(b):
    return struct.unpack_from("<I", b, 8)[0] if len(b) == 3020 else 0


def bank_slot(b, s):
    o = 12 + 94 * s
    return b[o:o + 14].rstrip(b"\0").decode("ascii", "replace"), list(struct.unpack_from("<40H", b, o + 14))


def wasm_has_sounds():
    try:
        return b"web_cy_box" in WASM.read_bytes()
    except OSError:
        return False


F = VEC["factory"]


# ------------------------------------------------------------------------------------------------ the runner
class Base(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="cyma_sounds_t7_"))
        cls.env = dict(os.environ)
        cls.env.update({"HOME": str(cls.tmp / "home"), "CYMA1_BACKUP_DIR": str(cls.tmp / "backups"),
                        "PYTHONDONTWRITEBYTECODE": "1"})
        (cls.tmp / "home").mkdir()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def cli(self, *args, timeout=180, cwd=None):
        """run cyma_sounds.py; device commands only ever with --virtual (never a MIDI port)"""
        args = [str(a) for a in args]
        cmd = next((a for a in args if not a.startswith("-") and a not in ("yes", "no", "none")), "")
        if cmd not in OFFLINE_CMDS:
            assert "--virtual" in args, f"refusing to run a device command without --virtual: {args}"
        p = subprocess.run([sys.executable, str(CLI)] + args, capture_output=True, text=True, timeout=timeout,
                           env=self.env, cwd=str(cwd or self.tmp))
        out = p.stdout + p.stderr
        self.assertNotIn("Traceback", out, f"a traceback from {args}:\n{out[-2000:]}")
        return p.returncode, p.stdout, p.stderr

    def write(self, name, data):
        p = self.tmp / name
        p.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(data, (dict, list)):
            data = json.dumps(data)
        if isinstance(data, str):
            data = data.encode("utf-8")
        p.write_bytes(data)
        return p


@unittest.skipUnless(CLI.exists(), "no tools/cyma_sounds.py yet")
class Offline(Base):
    def test_selftest(self):
        rc, out, err = self.cli("selftest", "--quick")
        self.assertEqual(rc, 0, out + err)
        if not QUICK:
            rc, out, err = self.cli("selftest", timeout=600)
            self.assertEqual(rc, 0, out + err)

    def test_decode_encode_every_factory_code(self):
        d = self.tmp / "dec"
        d.mkdir(exist_ok=True)
        for f in F:
            rc, out, err = self.cli("decode", f["code"], "-o", d, "--force")
            self.assertEqual(rc, 0, f"{f['name']}: {out}{err}")
            p = d / (re.sub(r'[/\\:?*"<>|]', "-", f["name"]) + ".cyma1.json")
            self.assertTrue(p.exists(), f"{p.name} not written ({out})")
            doc = json.loads(p.read_text(encoding="utf-8"))
            self.assertEqual(doc["format"], "cyma1-sound")
            self.assertEqual(doc["version"], 1)
            self.assertEqual(doc["name"], f["name"])
            self.assertEqual(doc["param_set"], 1)
            self.assertEqual(doc["made_with"]["where"], "cli")
            self.assertEqual(doc["params"], f["params"], f"{f['name']}: the file's values are not the vectors'")
            self.assertEqual(list(doc["params"]), [p_["key"] for p_ in TABLE["params"]] if TABLE else list(f["params"]),
                             "params not in param_set order")
            self.assertNotIn("author", doc, "a code carries no author")
            rc, out, err = self.cli("encode", p)
            self.assertEqual(rc, 0, out + err)
            self.assertIn(f["code"], out, f"{f['name']}: encode does not give the vector's code back")

    def test_link_in_chat_text(self):
        code = F[0]["code"]
        text = "Listen to this: https://claude.ai/artifact/abc#" + code[:50] + "\n  " + code[50:] + ")."
        rc, out, err = self.cli("decode", text, "-o", self.tmp / "link.cyma1.json", "--force")
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(json.loads((self.tmp / "link.cyma1.json").read_text())["name"], "SINGING BOWL")

    def test_encode_with_base_gives_link(self):
        p = self.write("bowl.cyma1.json", sound_doc("SINGING BOWL", F[0]["params"]))
        rc, out, err = self.cli("encode", p, "--base", "https://claude.ai/artifact/abc")
        self.assertEqual(rc, 0, out + err)
        self.assertIn("https://claude.ai/artifact/abc#" + F[0]["code"], out)

    def test_bad_codes(self):
        for kind, lst in VEC["bad_codes"].items():
            for c in lst:
                if not c:
                    continue                            # (an empty argument: argparse's, not the decoder's)
                rc, out, err = self.cli("decode", c, "-o", self.tmp / "never.cyma1.json")
                self.assertEqual(rc, 2, f"{kind} {c[:30]}: exit {rc}\n{out}{err}")
        self.assertFalse((self.tmp / "never.cyma1.json").exists())
        for junk in ("x" * 5000, "cy1." + "A" * 4000, "☃ cy1.!!!!", "cy1.AQEo\u0001\u0002", "#cy1.", "cy1"):
            rc, out, err = self.cli("decode", junk)
            self.assertEqual(rc, 2, f"{junk[:20]!r}: exit {rc}")

    def corpus(self):
        good = sound_doc("GOOD ONE", F[0]["params"])
        deep = "[" * 100000 + "]" * 100000
        big = dict(good, padding="x" * (1024 * 1024))
        bank257 = {"format": "cyma1-bank", "version": 1, "name": "TOO MANY", "param_set": 1,
                   "sounds": [{"name": "S%d" % i, "params": F[i % 24]["params"]} for i in range(257)]}
        bank2 = {"format": "cyma1-bank", "version": 1, "name": "TWO", "param_set": 1,
                 "sounds": [{"slot": 1, "name": "DUSK BOWL", "params": F[0]["params"]},
                            {"slot": 5, "name": "Singing bowl", "author": "Mark", "params": F[1]["params"]}]}
        pollution = sound_doc("POLLUTED", {"__proto__": {"admin": True}, "constructor": {"prototype": 1},
                                           "prototype": 3, "shape": 2})
        pollution["__proto__"] = {"polluted": True}
        rnd = random.Random(7)
        return {
            # refused: exit 2
            "empty.json": (b"", 2),
            "not_json.json": (b"hello, this is not JSON", 2),
            "array.json": ([1, 2, 3], 2),
            "null.json": (b"null", 2),
            "string.json": (b'"cyma1-sound"', 2),
            "deep.json": (deep, 2),
            "binary.json": (bytes(rnd.randrange(256) for _ in range(4096)), 2),
            "format_x.json": (dict(good, format="cyma1-something"), 2),
            "format_v2.json": (dict(good, version=2), 2),
            "version_str.json": (dict(good, version="1"), 2),
            "too_big.json": (big, 2),
            "five_mb.json": (b" " * (5 * 1024 * 1024) + json.dumps(good).encode(), 2),
            "bank_257.json": (bank257, 2),
            "no_params.json": (b'{"format": "cyma1-sound", "version": 1, "name": "X", "params": 7}', 2),
            # accepted: exit 0, perhaps with notes
            "good.cyma1.json": (good, 0),
            "bank.cyma1-bank.json": (bank2, 0),
            "compat.json": (dict(good, version=5, compat=1), 0),
            "pollution.json": (pollution, 0),
            "unknown_params.json": (sound_doc("UNKNOWN", dict(F[0]["params"], flutter=3, grain="fine")), 0),
            "odd_values.json": (sound_doc("odd", {"decay": 99, "exciter": "SCRAPE", "modes": "many",
                                                  "lfo_wave": "s&h", "POS X": 58, "level": -1e9}), 0),
            "aliases.json": (sound_doc("ALIASES", {"POS X": 58, "LFO>POS": 20, "MORPH": 25, "TARGET": "BELL",
                                                   "KEYS": "SURFACE"}), 0),
            "long_text.json": (sound_doc("x" * 40, F[0]["params"], author="\u0007" + "A" * 100, notes="n" * 1000), 0),
            "renamed.cyma1.txt": (good, 0),                   # readers decide by format, never by extension
        }

    def test_validate_corpus(self):
        files = {}
        for name, (data, want) in self.corpus().items():
            files[name] = (self.write("corpus/" + name, data), want)
        for name, (p, want) in files.items():
            rc, out, err = self.cli("validate", p)
            self.assertEqual(rc, want, f"validate {name}: exit {rc}, want {want}\n{(out + err)[-600:]}")
        rc, out, err = self.cli("validate", files["unknown_params.json"][0])
        self.assertIn("FLUTTER", (out + err).upper(), "an unknown key is listed in the notes")
        rc, out, err = self.cli("validate", "--strict", files["unknown_params.json"][0])
        self.assertNotEqual(rc, 0, "--strict: notes are failures")
        rc, out, err = self.cli("validate", files["good.cyma1.json"][0], files["bank.cyma1-bank.json"][0])
        self.assertEqual(rc, 0, out + err)
        rc, out, err = self.cli("validate", files["good.cyma1.json"][0], files["format_v2.json"][0])
        self.assertEqual(rc, 2, "one bad file of two fails the run")

    def test_decode_corpus(self):
        out_dir = self.tmp / "decoded"
        out_dir.mkdir(exist_ok=True)
        for name, (data, want) in self.corpus().items():
            if name.startswith("bank"):
                continue                                     # (a bank decodes to a folder of sounds: below)
            p = self.write("corpus2/" + name, data)
            rc, out, err = self.cli("decode", p, "-o", out_dir, "--force")
            self.assertEqual(rc, want, f"decode {name}: exit {rc}, want {want}\n{(out + err)[-600:]}")
        got = json.loads((out_dir / "POLLUTED.cyma1.json").read_text())
        self.assertNotIn("__proto__", got)
        self.assertEqual(got["params"]["shape"], 2)
        odd = json.loads((out_dir / "ODD.cyma1.json").read_text())["params"]
        self.assertEqual(odd["decay"], 10)
        self.assertEqual(odd["exciter"], "MALLET")             # the default
        self.assertEqual(odd["lfo_wave"], "S&H")
        self.assertEqual(odd["pos_x"], 58)
        self.assertEqual(odd["level"], -18)
        al = json.loads((out_dir / "ALIASES.cyma1.json").read_text())["params"]
        self.assertEqual((al["pos_x"], al["lfo_pos"], al["morph_depth"], al["carve_target"], al["keymode"]),
                         (58, 20, 25, "BELL", "SURFACE"))
        lt = json.loads((out_dir / "XXXXXXXXXXXXXX.cyma1.json").read_text())
        self.assertEqual(lt["name"], "XXXXXXXXXXXXXX")
        if "author" in lt:
            self.assertLessEqual(len(lt["author"]), 32)
            self.assertFalse(any(ord(c) < 32 for c in lt["author"]))

    def test_never_overwrites(self):
        target = self.write("exists.cyma1.json", "keep me")
        rc, out, err = self.cli("decode", F[1]["code"], "-o", target)
        self.assertEqual(rc, 2, out + err)
        self.assertEqual(target.read_text(), "keep me")
        rc, out, err = self.cli("decode", F[1]["code"], "-o", target, "--force")
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(json.loads(target.read_text())["name"], "HANDPAN")

    def test_file_names(self):
        d = self.tmp / "names"
        d.mkdir(exist_ok=True)
        rc, out, err = self.cli("decode", code_of("A/B:C?(1)", F[2]["q16"]), "-o", d)
        self.assertEqual(rc, 0, out + err)
        self.assertTrue((d / "A-B-C-(1).cyma1.json").exists(), sorted(x.name for x in d.iterdir()))


def cli_knows_cy2():
    try:
        return "cy2." in CLI.read_text(encoding="utf-8")
    except OSError:
        return False


CF_RE = re.compile("[­؀-؅؜۝܏᠎​-‏‪-‮⁠-⁤⁦-⁯"
                   "﻿￹-￻]")              # (Unicode's format characters, Cf, the ones a text can carry)


@unittest.skipUnless(CLI.exists() and "cy2" in VEC and cli_knows_cy2(), "no cy2 vectors, or a CLI without cy2. yet")
class ShortLinks(Base):
    """the short link code cy2. and the 128-character anchor (spec revision 2026-10-24b), against vectors.json "cy2":
    the same vectors the page is held to in T5 (tools/sounds_chrome_test.mjs), so the two cannot drift apart"""
    BASE = "https://claude.ai/artifact/CLw6XpfjKrVVbpJJEY4tX7"

    def test_every_factory_link_code_decodes(self):
        d = self.tmp / "cy2dec"
        d.mkdir(exist_ok=True)
        for f, e in zip(F, VEC["cy2"]["factory"]):
            for src in (e["code"], self.BASE + "#" + e["code"]):
                rc, out, err = self.cli("decode", src, "-o", d / "x.cyma1.json", "--force")
                self.assertEqual(rc, 0, f"{f['name']}: {out}{err}")
                doc = json.loads((d / "x.cyma1.json").read_text(encoding="utf-8"))
                self.assertEqual((doc["name"], doc["params"]), (f["name"], f["params"]), f"{f['name']} from {src[:40]}")

    def test_encode_offers_the_short_link_exactly(self):
        d = self.tmp / "cy2enc"
        d.mkdir(exist_ok=True)
        for f, e in zip(F, VEC["cy2"]["factory"]):
            p = self.write(f"cy2enc/{f['num']}.cyma1.json", sound_doc(f["name"], f["params"]))
            rc, out, err = self.cli("encode", p, "--base", self.BASE)
            self.assertEqual(rc, 0, out + err)
            lines = out.split()
            self.assertIn(f["code"], lines, f"{f['name']}: the code is cy1., as before")
            self.assertIn(self.BASE + "#" + e["code"], lines, f"{f['name']}: no short link {e['code'][:20]}…")
            self.assertTrue(e["anchor_fits"] and len(e["code"]) <= VEC["cy2"]["anchor_max"])

    def test_edges_fit_and_do_not(self):
        ed = VEC["cy2"]["edges"]
        for key in ("longest_that_fits", "shortest_that_does_not", "every_value_changed"):
            e = ed[key]
            p = self.tmp / f"edge_{key}.cyma1.json"
            rc, out, err = self.cli("decode", e["code"], "-o", p, "--force")
            self.assertEqual(rc, 0, out + err)
            rc, out, err = self.cli("encode", p, "--base", self.BASE)
            self.assertEqual(rc, 0, out + err)
            short = self.BASE + "#" + e["code"]
            if e["anchor_fits"]:
                self.assertIn(short, out.split(), f"{key} ({e['length']} characters) fits: its short link is offered")
            else:
                self.assertNotIn(short, out, f"{key} ({e['length']} characters) does not fit: no short link")
                rc2, out2, err2 = self.cli("encode", p, "--base", self.BASE, "--link", "cy2")
                self.assertEqual(rc2, 2, f"--link cy2 for a sound that does not fit: exit {rc2}\n{out2}{err2}")

    def test_bad_link_codes_are_refused(self):
        for kind, lst in VEC["cy2"]["bad"].items():
            for c in lst:
                rc, out, err = self.cli("decode", c, "-o", self.tmp / "never2.cyma1.json")
                self.assertEqual(rc, 2, f"{kind} {c[:30]}: exit {rc}\n{out}{err}")
        self.assertFalse((self.tmp / "never2.cyma1.json").exists())

    def test_older_and_newer_writers(self):
        for key, t in VEC["cy2"]["tolerated"].items():
            p = self.tmp / f"tol_{key}.cyma1.json"
            rc, out, err = self.cli("decode", t["code"], "-o", p, "--force")
            self.assertEqual(rc, 0, f"{key}: {out}{err}")
            doc = json.loads(p.read_text(encoding="utf-8"))
            self.assertEqual(t["q16"], F[0]["q16"], "(the vector is SINGING BOWL's values, as N 38 / 42 read them)")
            self.assertEqual((doc["name"], doc["params"]), ("TEST", F[0]["params"]), key)

    def test_format_characters_inside_a_code(self):
        fc = VEC["cy2"]["format_chars"]
        rc, out, err = self.cli("decode", fc["text"], "-o", self.tmp / "zw.cyma1.json", "--force")
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(json.loads((self.tmp / "zw.cyma1.json").read_text())["name"], fc["name"])


@unittest.skipUnless(CLI.exists(), "no tools/cyma_sounds.py yet")
class FormatCharacters(Base):
    """a file's free text (credit, notes, a bank's name, an unknown key) loses its Unicode format characters (bidi
    overrides and isolates, zero-width characters, the BOM) before the tool shows it or writes it (revision 2026-10-24b)"""
    CF = "‮gnp.exe⁦ Mark​‍﻿⁩؜"

    def test_author_notes_and_keys(self):
        doc = sound_doc("CF TEXT", dict(F[3]["params"], **{"fl‮utter": 3}), author=self.CF,
                        notes="line one line‮ two​")
        p = self.write("cf.cyma1.json", doc)
        rc, out, err = self.cli("validate", p)
        self.assertEqual(rc, 0, out + err)
        self.assertFalse(CF_RE.search(out + err), f"format characters on the terminal: {[hex(ord(c)) for c in CF_RE.findall(out + err)]}")
        o = self.tmp / "cf_out"
        o.mkdir(exist_ok=True)
        rc, out, err = self.cli("decode", p, "-o", o, "--force")
        self.assertEqual(rc, 0, out + err)
        self.assertFalse(CF_RE.search(out + err), "format characters on the terminal (decode)")
        got = json.loads((o / "CF TEXT.cyma1.json").read_text(encoding="utf-8"))
        self.assertIn("Mark", got.get("author", ""))
        self.assertFalse(CF_RE.search(json.dumps(got, ensure_ascii=False)), f"format characters written: {got.get('author')!r}")
        bank = {"format": "cyma1-bank", "version": 1, "name": "BANK‮KNAB", "param_set": 1,
                "sounds": [{"name": "ONE", "author": self.CF, "params": F[0]["params"]}]}
        bp = self.write("cf.cyma1-bank.json", bank)
        rc, out, err = self.cli("validate", bp)
        self.assertEqual(rc, 0, out + err)
        self.assertFalse(CF_RE.search(out + err), "format characters on the terminal (bank)")


@unittest.skipUnless(CLI.exists() and BRIDGE.exists() and shutil.which("node") and wasm_has_sounds(),
                     "the virtual FM-1 needs tools/sx_bridge.mjs, node and an app.wasm with the Sounds exports")
class Virtual(Base):
    def v(self, *args, answer="yes", store=None, identity=None, die_after=None, identity_hex=None, timeout=240):
        a = list(args) + ["--virtual", "--wasm", WASM, "--virtual-answer", answer, "--trace", "--yes"]
        if store:
            a += ["--virtual-store", store]
        if identity:
            a += ["--identity", identity]
        if die_after is not None:
            a += ["--virtual-die-after", die_after]
        if identity_hex is not None:
            a += ["--virtual-identity-hex", identity_hex]
        rc, out, err = self.cli(*a, timeout=timeout)
        sent = [bytes.fromhex(l[3:].replace(" ", "")) for l in err.splitlines() if l.startswith("-> ")]
        for f in sent:                                   # §5.6 P7: the send allowlist, on every frame
            ok = f == HS_QUERY or (f[:5] == bytes([0xF0, 0x7D, 0x43, 0x59, 0x01]) and f[-1] == 0xF7 and
                                   len(f) <= 306 and all(x < 0x80 for x in f[1:-1]))
            self.assertTrue(ok, f"a frame outside the allowlist: {f.hex()}")
        self.assertTrue(not sent or sent[0] == HS_QUERY, "the first frame is not the identity query")
        return rc, out, err, sent

    def fresh_store(self, name, entries=None):
        p = self.tmp / name
        if entries is None:
            p.unlink(missing_ok=True)
        else:
            store_file(p, bank_bytes(entries))
        return p

    def test_info(self):
        st = self.fresh_store("info.json", {0: ("DUSK BOWL", F[0]["q16"]), 4: ("RAIN ROOM", F[7]["q16"])})
        rc, out, err, sent = self.v("info", store=st)
        self.assertEqual(rc, 0, out + err)
        self.assertIn("CYMA-1", out + err)
        self.assertIn("DUSK BOWL", out)
        self.assertIn("RAIN ROOM", out)
        self.assertFalse([f for f in sent if f != HS_QUERY and f[5] in WRITE_CMDS], "info sent a write")

    def test_put_free_then_reads_back(self):
        st = self.fresh_store("put.json", {0: ("TAKEN", F[3]["q16"])})
        rc, out, err, sent = self.v("put", F[0]["code"], "--slot", "free", store=st)
        self.assertEqual(rc, 0, out + err)
        b = store_bank(st)
        self.assertEqual(bank_used(b), 0b11)
        self.assertEqual(bank_slot(b, 1), ("SINGING BOWL", F[0]["q16"]))
        self.assertEqual(bank_slot(b, 0), ("TAKEN", F[3]["q16"]), "the used slot was not touched")
        o = self.tmp / "u02.cyma1.json"
        rc, out, err, _ = self.v("get", "U02", "-o", o, "--force", store=st)
        self.assertEqual(rc, 0, out + err)
        doc = json.loads(o.read_text())
        self.assertEqual((doc["name"], doc["params"]), ("SINGING BOWL", F[0]["params"]))
        self.assertEqual(doc["made_with"]["where"], "fm1")

    def test_put_declined_writes_nothing(self):
        st = self.fresh_store("declined.json", {0: ("KEEP", F[5]["q16"])})
        before = store_bank(st)
        rc, out, err, sent = self.v("put", F[1]["code"], "--slot", "U07", store=st, answer="no")
        self.assertEqual(rc, 1, out + err)
        self.assertEqual(store_bank(st), before)

    def test_audition_writes_nothing(self):
        st = self.fresh_store("aud.json", {2: ("THREE", F[2]["q16"])})
        before = store_bank(st)
        rc, out, err, sent = self.v("audition", F[6]["code"], store=st)
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(store_bank(st), before)
        self.assertTrue(any(f != HS_QUERY and f[5] == 0x03 for f in sent), "no PUT_EDIT sent")
        self.assertFalse(any(f != HS_QUERY and f[5] in (0x05, 0x06, 0x08) for f in sent), "audition staged a write")

    def test_get_all_and_erase(self):
        st = self.fresh_store("all.json", {0: ("ONE", F[0]["q16"]), 6: ("SEVEN", F[6]["q16"])})
        o = self.tmp / "all.cyma1-bank.json"
        rc, out, err, _ = self.v("get", "all", "-o", o, "--force", store=st)
        self.assertEqual(rc, 0, out + err)
        doc = json.loads(o.read_text())
        self.assertEqual(doc["format"], "cyma1-bank")
        self.assertEqual([(s.get("slot"), s["name"]) for s in doc["sounds"]], [(1, "ONE"), (7, "SEVEN")])
        self.assertEqual(doc["sounds"][1]["params"], F[6]["params"])
        rc, out, err, _ = self.v("erase", "U07", store=st)
        self.assertEqual(rc, 0, out + err)
        b = store_bank(st)
        self.assertEqual(bank_used(b), 1)

    def test_push_bank_replace_backs_up_first(self):
        st = self.fresh_store("push.json", {0: ("OLD ONE", F[10]["q16"]), 1: ("OLD TWO", F[11]["q16"])})
        bank = {"format": "cyma1-bank", "version": 1, "name": "NEW", "param_set": 1,
                "sounds": [{"slot": 1, "name": "NEW ONE", "params": F[0]["params"]},
                           {"name": "NEW TWO", "params": F[1]["params"]},
                           {"slot": 9, "name": "NEW NINE", "params": F[2]["params"]}]}
        bp = self.write("push.cyma1-bank.json", bank)
        bdir = self.tmp / "backups"
        shutil.rmtree(bdir, ignore_errors=True)
        rc, out, err, sent = self.v("push-bank", bp, "--replace", store=st)
        self.assertEqual(rc, 0, out + err)
        backups = sorted(bdir.glob("*.cyma1-bank.json"))
        self.assertEqual(len(backups), 1, f"one safety backup, in CYMA1_BACKUP_DIR: {backups}")
        bdoc = json.loads(backups[0].read_text())
        self.assertEqual([s["name"] for s in bdoc["sounds"]], ["OLD ONE", "OLD TWO"])
        self.assertEqual(bdoc["sounds"][0]["params"], F[10]["params"])
        cmds = [f[5] for f in sent if f != HS_QUERY]
        first_write = next(i for i, c in enumerate(cmds) if c in WRITE_CMDS)
        self.assertGreaterEqual(cmds[:first_write].count(0x04), 2, "the backup's GET_SLOTs come before any write")
        self.assertLessEqual(backups[0].stat().st_mtime, Path(st).stat().st_mtime)
        b = store_bank(st)
        names = {s: bank_slot(b, s)[0] for s in range(32) if bank_used(b) >> s & 1}
        self.assertEqual(names[0], "NEW ONE")
        self.assertEqual(names[8], "NEW NINE")
        self.assertIn("NEW TWO", names.values())
        self.assertEqual(len(names), 3, f"--replace: the old sounds are gone: {names}")

    def test_dry_run_reads_only(self):
        st = self.fresh_store("dry.json", {0: ("STAYS", F[4]["q16"])})
        before = store_bank(st)
        bp = self.write("dry.cyma1-bank.json", {"format": "cyma1-bank", "version": 1, "name": "D", "param_set": 1,
                                                 "sounds": [{"name": "WOULD", "params": F[3]["params"]}]})
        rc, out, err, sent = self.v("push-bank", bp, "--replace", "--dry-run", store=st)
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(store_bank(st), before)
        self.assertFalse([f.hex() for f in sent if f != HS_QUERY and f[5] in WRITE_CMDS], "a dry run sent a write")
        self.assertFalse(list((self.tmp / "backups").glob("*dry*")), "")

    def test_identity_nul_then_bytes_is_refused(self):
        # the identity rule (spec revision 2026-10-24b R5): the 27 bytes with the trailing NULs removed, nothing else;
        # bytes after an interior NUL stay in the text, so "FM-1_9400" NUL "junk" is not CYMA-1 (the page reads it so too)
        st = self.fresh_store("ident0.json", {0: ("UNTOUCHED", F[0]["q16"])})
        before = store_bank(st)
        rc, out, err, sent = self.v("put", F[1]["code"], store=st, identity_hex=b"FM-1_9400\0junk".hex())
        self.assertEqual(rc, 6, out + err)
        self.assertEqual(sent, [HS_QUERY])
        self.assertIn("FM-1_9400\\x00junk", err)
        self.assertEqual(store_bank(st), before)

    def test_identity_gate(self):
        for ident in ("FM-1_015", "FM-1_906", "ota-loader"):
            st = self.fresh_store("ident.json", {0: ("UNTOUCHED", F[0]["q16"])})
            before = store_bank(st)
            rc, out, err, sent = self.v("put", F[1]["code"], store=st, identity=ident)
            self.assertEqual(rc, 6, f"{ident}: exit {rc}\n{out}{err}")
            self.assertEqual(sent, [HS_QUERY], f"{ident}: frames beyond the identity query: {[f.hex() for f in sent]}")
            self.assertEqual(store_bank(st), before)

    def test_bridge_dies_mid_push(self):
        st = self.fresh_store("die.json", {0: ("SURVIVOR", F[8]["q16"]), 3: ("ALSO", F[9]["q16"])})
        before = store_bank(st)
        bp = self.write("die.cyma1-bank.json", {"format": "cyma1-bank", "version": 1, "name": "D", "param_set": 1,
                                                 "sounds": [{"name": "N%d" % i, "params": F[i]["params"]} for i in range(6)]})
        rc, out, err, sent = self.v("push-bank", bp, "--replace", "--no-backup", store=st, die_after=6)
        self.assertEqual(rc, 4, out + err)
        self.assertEqual(store_bank(st), before, "the bank changed although the cable went mid-push")

    def test_locked_bank(self):
        st = self.tmp / "locked.json"
        foreign = bank_bytes({0: ("NEWER FORMAT", F[0]["q16"])}, version=2)
        store_file(st, foreign)
        for args in (("info",), ("put", F[1]["code"])):
            rc, out, err, sent = self.v(*args, store=st)
            self.assertEqual(rc, 6, f"{args[0]}: exit {rc}\n{out}{err}")
            self.assertEqual(store_bank(st), foreign)


def main():
    if not CLI.exists():
        print("test_cyma_sounds: no tools/cyma_sounds.py yet: SKIP")
        return 77
    prog = unittest.main(exit=False, verbosity=2 if "-v" in sys.argv else 1)
    r = prog.result
    print(f"test_cyma_sounds: {r.testsRun} tests, {len(r.failures)} failures, {len(r.errors)} errors, "
          f"{len(r.skipped)} skipped")
    return 0 if r.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
