#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""CYMA-1 plate data test (M1): checks the COMMITTED src/plates_data.c, parsed straight from the C, against the spec
(section 3.1 and the golden ratios of 8.3), and that it is current and reproducible.

    cd instrument && python3 tests/plates/test_plates.py           # all checks, incl. a full regeneration (~3 s)
    cd instrument && python3 tests/plates/test_plates.py --fast    # skip the regeneration
    (pytest also collects the test_* functions)

What it checks:
  layout      9 shapes in spec order, names, clamped flags, 21,040 B each (189,360 B <= 200 KB)
  lambda      log2(lambda/lambda_min) finite, >= 0, min exactly 0; DRUM membrane ratios within 3 % of Bessel; BAR's
              lowest six = the Euler free-free ratios; degen ids = chains of |dl|/l < 2e-3 numbered by ascending lambda
  monopole    ~0 on free plates (orthogonal to the rigid mode), J0-like on DRUM's fundamental
  sdf         int8, < 0 inside, sign matches the analytic shape, |grad| ~ 1 (a distance, not just a mask)
  fields      max |phi| = 127 per mode; DRUM's are 0 outside (clamped), free plates' are extended; the lowest modes
              are near-orthogonal on the 24 x 24 grid; tracked partners on the smooth edges correlate positively
  freshness   the header's generator SHA-1 is tools/gen_plates.py's; a regeneration reproduces the file byte for byte
              (apart from the date line)"""
import hashlib
import os
import re
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.abspath(os.path.join(HERE, "..", ".."))
DATA = os.path.join(APP, "src", "plates_data.c")
GEN = os.path.join(APP, "tools", "gen_plates.py")
sys.path.insert(0, os.path.join(APP, "tools"))

NAMES = ["DRUM", "CIRCLE", "HEX", "SQUARE", "TRIANGLE", "STAR", "GUITAR", "STADIUM", "BAR"]
NM, G, S = 32, 24, 48
BESSEL = [1.0, 1.5933, 2.1355, 2.2954, 2.6531, 2.9173]
EULER = [1.0, 2.7565, 5.4039, 8.9330, 13.3443, 18.6379]
_cache = {}


def parse(path=DATA):
    """the C initialiser, token by token: [{name, clamped, log2_lambda, monopole, degen, sdf, field}] and the header"""
    if path in _cache:
        return _cache[path]
    src = open(path).read()
    header = src[:src.index("#include")]
    body = src[src.index("CY_PLATES[CY_NSHAPES]"):]
    body = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)
    toks = re.findall(r'"[^"]*"|[-+]?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?f?', body.split("=", 1)[1])
    plates, i = [], 0
    for _ in range(9):
        p = {"name": toks[i].strip('"')}
        i += 1
        p["clamped"] = int(toks[i]); i += 1
        p["rsv"] = [int(x) for x in toks[i:i + 3]]; i += 3
        p["log2_lambda"] = np.array([float(x.rstrip("f")) for x in toks[i:i + NM]]); i += NM
        p["monopole"] = np.array([float(x.rstrip("f")) for x in toks[i:i + NM]]); i += NM
        p["degen"] = np.array([int(x) for x in toks[i:i + NM]]); i += NM
        p["sdf"] = np.array([int(x) for x in toks[i:i + S * S]]).reshape(S, S); i += S * S
        p["field"] = np.array([int(x) for x in toks[i:i + NM * G * G]]).reshape(NM, G, G); i += NM * G * G
        plates.append(p)
    assert i == len(toks), f"{len(toks) - i} stray tokens after the 9th shape"
    _cache[path] = (plates, header)
    return plates, header


def distinct(vals, tol=5e-3):
    out = []
    for v in sorted(vals):
        if not out or v / out[-1] > 1 + tol:
            out.append(v)
    return out


def test_layout():
    plates, header = parse()
    assert [p["name"] for p in plates] == NAMES
    assert [p["clamped"] for p in plates] == [1] + [0] * 8
    assert all(p["rsv"] == [0, 0, 0] for p in plates)
    size = 12 + 1 + 3 + 4 * NM + 4 * NM + NM + S * S + NM * G * G
    assert size == 21040 and 9 * size <= 200 * 1024
    assert "generator sha1" in header and "date" in header


def test_lambda():
    plates, _ = parse()
    for p in plates:
        l2 = p["log2_lambda"]
        assert np.isfinite(l2).all() and (l2 >= 0).all() and l2.min() == 0.0, p["name"]
    drum = np.sqrt(distinct(2.0 ** plates[0]["log2_lambda"]))[:6]
    err = np.abs(drum / BESSEL - 1).max()
    assert err < 0.03, f"DRUM vs Bessel {err:.4f}"
    bar = distinct(2.0 ** plates[8]["log2_lambda"], 1e-4)[:6]
    err = np.abs(np.array(bar) / EULER - 1).max()
    assert err < 1e-5, f"BAR vs Euler {err:.2e}: {bar}"


def test_degen():
    plates, _ = parse()
    for p in plates:
        lam, dg = 2.0 ** p["log2_lambda"], p["degen"]
        o = np.argsort(lam, kind="stable")
        want, gid = np.zeros(NM, int), 0
        for a, b in zip(o[:-1], o[1:]):
            want[b] = gid if abs(lam[b] - lam[a]) / lam[a] < 2e-3 else gid + 1
            gid = want[b]
        assert (dg == want).all(), f"{p['name']}: degen {dg.tolist()} != {want.tolist()}"


def test_monopole():
    plates, _ = parse()
    for p in plates[1:]:
        assert np.abs(p["monopole"]).max() < 0.02, p["name"]
    d = plates[0]
    m0 = d["monopole"][int(np.argmin(d["log2_lambda"]))]
    assert 0.38 < abs(m0) < 0.48, f"DRUM fundamental monopole {m0} (J0 dome: 2 J1(j01)/j01 = 0.432)"
    assert np.abs(d["monopole"]).max() <= 1.0


def test_sdf():
    import gen_plates as gp
    plates, _ = parse()
    c = (np.arange(S) + 0.5) / S
    u, v = np.meshgrid(c, c)
    for p, shp in zip(plates, gp.SHAPES):
        s = p["sdf"] / 128.0
        assert p["sdf"].min() >= -127 and p["sdf"].max() <= 127
        assert s[S // 2, S // 2] < 0 and s[0, 0] > 0 and s[-1, -1] > 0, p["name"]
        a = shp[2](u, v)
        far = np.abs(a) > 0.02
        bad = int(((a < 0) != (s < 0))[far].sum())
        assert bad == 0, f"{p['name']}: {bad} SDF cells disagree with the analytic shape"
        gy, gx = np.gradient(s, 1.0 / S)
        g = np.hypot(gx, gy)[2:-2, 2:-2]
        med = float(np.median(g))
        assert 0.9 < med < 1.1, f"{p['name']}: median |grad sdf| {med:.3f} (a distance has 1)"


def test_fields():
    plates, _ = parse()
    for p in plates:
        f = p["field"].astype(float)
        assert (np.abs(f).max(axis=(1, 2)) == 127).all(), p["name"]
        inside = p["sdf"].reshape(G, 2, G, 2).mean(axis=(1, 3)) < -1.5      # 24-grid cells clearly inside
        lam = p["log2_lambda"]
        low = np.argsort(lam, kind="stable")[:12]
        F = f[low][:, inside]
        F = F / np.linalg.norm(F, axis=1, keepdims=True)
        with np.errstate(all="ignore"):                                   # (numpy 1.26 + Accelerate: spurious)
            C = np.abs(F @ F.T) - np.eye(len(low))
        assert C.max() < 0.25, f"{p['name']}: the lowest 12 modes overlap {C.max():.2f} on the stored grid"
    d = plates[0]
    outside = d["sdf"].reshape(G, 2, G, 2).mean(axis=(1, 3)) > 3.0
    assert np.abs(d["field"][:, outside]).max() <= 1, "DRUM (clamped) fields must be 0 outside"
    c = plates[1]
    outside = c["sdf"].reshape(G, 2, G, 2).mean(axis=(1, 3)) > 3.0
    assert np.abs(c["field"][:, outside]).max() > 30, "free fields must be extended outside, not zeroed"


def test_tracking_partners():
    """mode k in shape i and shape i+1 is the same mode deformed. On the smooth edges (DRUM>CIRCLE>HEX>SQUARE) the
    partners must look alike: positive correlation (the signs were aligned step by step) and strong for the lowest
    modes. Between dissimilar shapes a keyframe-to-keyframe sign is not meaningful (the figures change; the chain of
    in-between solves aligned them, and re-signing against the direct partner was measured not to improve the
    mid-morph lerp), so there only the pairing's strength is reported."""
    plates, _ = parse()
    for a, b in zip(plates[:-1], plates[1:]):
        both = (a["sdf"].reshape(G, 2, G, 2).mean(axis=(1, 3)) < 0) & (b["sdf"].reshape(G, 2, G, 2).mean(axis=(1, 3)) < 0)
        A = a["field"][:, both].astype(float)
        B = b["field"][:, both].astype(float)
        cor = (A * B).sum(1) / (np.linalg.norm(A, axis=1) * np.linalg.norm(B, axis=1) + 1e-9)
        low = np.argsort(np.minimum(a["log2_lambda"], b["log2_lambda"]), kind="stable")[:12]
        if a["name"] in ("DRUM", "CIRCLE", "HEX"):
            # (DRUM's fundamental has no free-edge partner: its counterpart, the rigid mode, is dropped, so it pairs
            # with a leftover at ~0 correlation; no partner may be clearly opposite)
            chk = [k for k in low if not (a["clamped"] and not b["clamped"] and a["log2_lambda"][k] == 0.0)]
            assert (cor > -0.3).all() and (cor[chk] > 0).all(), f"{a['name']}>{b['name']}: partners anti-correlate"
            assert np.median(cor[low]) > 0.8, f"{a['name']}>{b['name']}: low partners {np.median(cor[low]):.2f}"
        print(f"        {a['name']:>8}>{b['name']:<8} partner corr: lowest 12 median {np.median(cor[low]):+.2f}, "
              f"positive {np.mean(cor > 0):.0%}")


def test_fresh():
    _, header = parse()
    sha = re.search(r"generator sha1 ([0-9a-f]{40})", header).group(1)
    with open(GEN, "rb") as f:
        cur = hashlib.sha1(f.read()).hexdigest()
    assert sha == cur, ("src/plates_data.c is stale: regenerate with "
                        "python3 tools/gen_plates.py --out src/plates_data.c --sheet build/plates_sheet.png --test")


def test_reproducible():
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "plates_data.c")
        r = subprocess.run([sys.executable, GEN, "--out", out], capture_output=True, text=True, cwd=APP)
        assert r.returncode == 0, r.stdout + r.stderr
        strip = lambda s: re.sub(r"(date|numpy|scipy) \S+", r"\1 X", s)
        a, b = strip(open(DATA).read()), strip(open(out).read())
        if a != b:
            la, lb = a.splitlines(), b.splitlines()
            diff = sum(x != y for x, y in zip(la, lb)) + abs(len(la) - len(lb))
            raise AssertionError(f"a regeneration differs from the committed file in {diff} lines")


def main():
    fast = "--fast" in sys.argv
    tests = [test_layout, test_lambda, test_degen, test_monopole, test_sdf, test_fields, test_tracking_partners,
             test_fresh] + ([] if fast else [test_reproducible])
    fails = 0
    for t in tests:
        try:
            t()
            print(f"  ok    {t.__name__}")
        except AssertionError as e:
            fails += 1
            print(f"  FAIL  {t.__name__}: {e}")
    print(f"test_plates: {len(tests) - fails}/{len(tests)} passed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
