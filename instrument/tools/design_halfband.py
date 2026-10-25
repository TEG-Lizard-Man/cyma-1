#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""CYMA-1 tools/design_halfband.py (M3): the 2x drive's polyphase allpass halfband, designed and measured.

The DRIVE stage (spec 3.10.1) runs its nonlinearity at 2x through a two-path polyphase IIR halfband: the classic
elliptic-halfband-as-two-allpass-chains design (Valenzuela & Constantinides 1983; the closed form below is the one
Laurent de Soras' HIIR library uses, rewritten here from the maths, no code copied). Each first-order section is
(a + z^-1) / (1 + a z^-1) at the LOW rate; even-indexed coefficients form path A, odd-indexed path B, and

    H(z) = 0.5 * (A(z^2) + z^-1 B(z^2))

is the halfband lowpass at the HIGH rate. The upsampler emits A(x) then B(x) for each input sample (that is 2 H(z) on
the zero-stuffed input); the downsampler of a pair (v0 earlier, v1 later) is 0.5 * (A(v1) + B(v0)), i.e. the
odd-phase output of H(z). (Spec 3.10.1 writes this as A(x_even) + B(x_odd), counting the pair's samples from 1.)

Parameterisation. An elliptic halfband has its transition band centred on fs/4 and is fixed by (order, transition
width). With fs2 = the 2x rate:
    tbw = (f_stop - f_pass) / fs2,  f_pass = fs2/4 - tbw*fs2/2,  f_stop = fs2/4 + tbw*fs2/2.
The spec's 8 coefficients are this design with tbw = 2.05 / 88.2 (half of (24.1 - 20) kHz / 88.2 kHz): flat (ripple
~1e-8 dB) to 21.025 kHz and -85.0 dB from 23.075 kHz at fs2 = 88.2 kHz, so they meet "flat to 20 kHz, about -85 dB
at >= 24.1 kHz" with room on both sides. `--check` proves the coefficients reproduce to 1e-9 and the response holds.

It also designs the drive's other two constants: the 5-tap linear-phase FIR after the decimator that undoes ADAA-1's
small-signal droop (cos(w/4): -2.4 dB at 20 kHz, flattened to +-0.16 dB), and the dry path's delay (round of the wet
path's 4.94 samples at DC) that keeps the engage crossfade from combing. src/fx.c carries all three.

    python3 tools/design_halfband.py                 # print the coefficients (C), the measured response, the delay
    python3 tools/design_halfband.py --check         # exit 1 unless it matches spec 3.10.1 and the response holds
    python3 tools/design_halfband.py --plot out.png  # magnitude / group delay plot (needs matplotlib)
    python3 tools/design_halfband.py --ncoef 8 --tbw 0.0465   # any other design

Needs only numpy (matplotlib for --plot).
"""
import argparse
import math
import sys

import numpy as np

# spec 3.10.1, verbatim (and src/fx.c's CY_HB)
SPEC = [0.053615466649, 0.193436724219, 0.372315971295, 0.546689394877,
        0.693030266958, 0.806703409456, 0.894013055065, 0.965874302792]
SPEC_TBW = 2.05 / 88.2


def transition_params(tbw):
    """the elliptic modulus k (as k^2 of tan) and nome q for a transition width tbw (relative to fs2)"""
    k = math.tan((1.0 - 2.0 * tbw) * math.pi / 4.0) ** 2
    kk = (1.0 - k * k) ** 0.25
    e = 0.5 * (1.0 - kk) / (1.0 + kk)
    e4 = e ** 4
    q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)))   # the nome's series in e (converges fast: e << 1)
    return k, q


def _theta_num(q, order, c):
    s, i, sign = 0.0, 0, 1.0
    while True:
        t = q ** (i * (i + 1)) * math.sin((2 * i + 1) * c * math.pi / order) * sign
        s += t
        sign, i = -sign, i + 1
        if abs(t) < 1e-100:
            return s


def _theta_den(q, order, c):
    s, i, sign = 0.0, 1, -1.0
    while True:
        t = q ** (i * i) * math.cos(2 * i * c * math.pi / order) * sign
        s += t
        sign, i = -sign, i + 1
        if abs(t) < 1e-100:
            return s


def design(ncoef, tbw):
    """the ncoef allpass coefficients (ascending) of the order 2*ncoef+1 elliptic halfband"""
    k, q = transition_params(tbw)
    order = 2 * ncoef + 1
    out = []
    for c in range(1, ncoef + 1):
        ww = _theta_num(q, order, c) * q ** 0.25 / (_theta_den(q, order, c) + 0.5)   # Jacobi theta quotients
        w2 = ww * ww
        x = math.sqrt((1.0 - w2 * k) * (1.0 - w2 / k)) / (1.0 + w2)
        out.append((1.0 - x) / (1.0 + x))
    return out


def predicted_atten_db(ncoef, tbw):
    """the stopband attenuation the closed form predicts"""
    _, q = transition_params(tbw)
    a = 4.0 * q ** ((2 * ncoef + 1) * 0.5)
    return -10.0 * math.log10(a / (1.0 + a))


def response(coefs, npts=200001):
    """H(e^jw) of 0.5 (A(z^2) + z^-1 B(z^2)) on w in [0, pi] (high rate)"""
    w = np.linspace(0.0, np.pi, npts)
    z = np.exp(1j * w)
    a = np.ones_like(z)
    b = np.ones_like(z)
    for i, c in enumerate(coefs):
        sec = (c + z ** -2) / (1.0 + c * z ** -2)
        if i % 2 == 0:
            a *= sec
        else:
            b *= sec
    return w, 0.5 * (a + b / z)


def simulate_chain(coefs, n=4096):
    """the float64 time-domain up -> down chain exactly as fx.c runs it (no nonlinearity): its impulse response at
    the base rate. Proves the polyphase wiring (which sample feeds which path) is the lowpass one."""
    ca, cb = coefs[0::2], coefs[1::2]

    def chain(cs):
        st = [[0.0, 0.0] for _ in cs]   # per section: x[n-1], y[n-1]

        def run(x):
            for s, a in zip(st, cs):
                y = a * (x - s[1]) + s[0]
                s[0], s[1] = x, y
                x = y
            return x
        return run
    ua, ub, da, db = chain(ca), chain(cb), chain(ca), chain(cb)
    h = np.zeros(n)
    for m in range(n):
        x = 1.0 if m == 0 else 0.0
        v0, v1 = ua(x), ub(x)                 # upsampler: path A first, then path B
        h[m] = 0.5 * (da(v1) + db(v0))        # downsampler: A takes the later sample of the pair
    return h


def design_droop(fs, f_hi=20000.0):
    """the 5-tap linear-phase FIR (c2 c1 c0 c1 c2, DC gain 1) after the decimator that undoes ADAA-1's droop.
    At small signal ADAA-1 at 2x is the two-tap mean of the 2x signal: |H| = cos(w2 / 2) = cos(w / 4) at the base
    rate (-2.4 dB at 20 kHz). Minimax in dB of |FIR| cos(w/4) over 0..f_hi, by a zooming grid over (c1, c2) (c0 is
    fixed by the DC gain): deterministic, numpy only."""
    w = 2.0 * np.pi * np.linspace(0.0, f_hi, 1601) / fs
    droop = np.cos(w / 4.0)

    def err(c1, c2):
        c0 = 1.0 - 2.0 * c1 - 2.0 * c2
        h = c0 + 2.0 * c1 * np.cos(w) + 2.0 * c2 * np.cos(2.0 * w)
        with np.errstate(divide="ignore"):
            return float(np.max(np.abs(20.0 * np.log10(np.abs(h * droop)))))
    best = (err(0.0, 0.0), 0.0, 0.0)
    span = 0.2
    for _ in range(40):
        _, b1, b2 = best
        for c1 in np.linspace(b1 - span, b1 + span, 21):
            for c2 in np.linspace(b2 - span, b2 + span, 21):
                e = err(c1, c2)
                if e < best[0]:
                    best = (e, c1, c2)
        span *= 0.6
    e, c1, c2 = best
    return [1.0 - 2.0 * c1 - 2.0 * c2, c1, c2], e


def report(coefs, fs2, f_pass, f_stop):
    w, h = response(coefs)
    f = w / np.pi * fs2 / 2.0
    mag = 20.0 * np.log10(np.abs(h) + 1e-300)
    pb = f <= f_pass
    sb = f >= f_stop
    ripple = float(max(abs(mag[pb].min()), abs(mag[pb].max())))
    stop = float(mag[sb].max())
    gd = -np.gradient(np.unwrap(np.angle(h)), w)       # high-rate samples
    hb = simulate_chain(coefs)
    fr = np.fft.rfft(hb, 1 << 16)
    fb = np.linspace(0.0, fs2 / 4.0, fr.size)
    gdc = -np.gradient(np.unwrap(np.angle(fr)), 2.0 * np.pi * np.arange(fr.size) / (1 << 16))
    out = {"ripple_db": ripple, "stop_db": stop, "gd_hi_dc": float(gd[1]),
           "chain_gd_dc": float(gdc[1]), "chain_dc_gain": float(abs(fr[0])),
           "chain_gd_10k": float(gdc[np.argmin(abs(fb - 10000.0))]),
           "chain_gd_20k": float(gdc[np.argmin(abs(fb - 20000.0))]),
           "chain_20k_db": float(20.0 * np.log10(abs(fr[np.argmin(abs(fb - 20000.0))])))}
    return out, (f, mag, gd)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ncoef", type=int, default=8)
    ap.add_argument("--tbw", type=float, default=SPEC_TBW, help="transition width / fs2 (default: the spec's)")
    ap.add_argument("--fs", type=float, default=44100.0, help="the base rate (the 2x rate is twice this)")
    ap.add_argument("--check", action="store_true", help="assert spec 3.10.1's coefficients and response")
    ap.add_argument("--plot", help="write a PNG of the magnitude and group delay")
    a = ap.parse_args()

    fs2 = 2.0 * a.fs
    coefs = design(a.ncoef, a.tbw)
    f_pass = fs2 / 4.0 - a.tbw * fs2 / 2.0
    f_stop = fs2 / 4.0 + a.tbw * fs2 / 2.0
    r, (f, mag, gd) = report(coefs, fs2, f_pass, f_stop)
    # the spec's own figures: flat to 20 kHz, the stopband from 24.1 kHz
    r20, _ = report(coefs, fs2, 20000.0, 24100.0)

    print(f"/* {a.ncoef} coefficients, tbw {a.tbw:.9f} (design band {f_pass:.0f}..{f_stop:.0f} Hz at fs2 "
          f"{fs2:.0f}); tools/design_halfband.py */")
    print("static const float CY_HB[%d] = {" % a.ncoef)
    for i in range(0, a.ncoef, 4):
        print("    " + ", ".join("%.12ff" % c for c in coefs[i:i + 4]) + ",")
    print("};")
    print(f"design band: ripple {r['ripple_db']:.2e} dB to {f_pass:.0f} Hz, stopband {r['stop_db']:.2f} dB from "
          f"{f_stop:.0f} Hz (closed form predicts -{predicted_atten_db(a.ncoef, a.tbw):.2f} dB)")
    print(f"spec band:   ripple {r20['ripple_db']:.2e} dB to 20000 Hz, stopband {r20['stop_db']:.2f} dB from 24100 Hz")
    print(f"halfband group delay at DC: {r['gd_hi_dc']:.4f} high-rate samples")
    print(f"up->down chain (base rate): DC gain {r['chain_dc_gain']:.9f}, group delay {r['chain_gd_dc']:.4f} samples "
          f"at DC, {r['chain_gd_10k']:.3f} at 10 kHz, {r['chain_gd_20k']:.3f} at 20 kHz; "
          f"|H| at 20 kHz {r['chain_20k_db']:.2e} dB")
    droop, derr = design_droop(a.fs)
    wet_gd = r["chain_gd_dc"] + 0.25 + 2.0
    print("/* ADAA-1 droop compensation, 5-tap linear phase after the decimator: c0 c1 c2 (taps c2 c1 c0 c1 c2) */")
    print("static const float CY_FX_DROOP[3] = {%.9ff, %.9ff, %.9ff};" % tuple(droop))
    print(f"droop: ADAA-1 alone is {20.0 * math.log10(math.cos(math.pi * 20000.0 / fs2)):.2f} dB at 20 kHz; with"
          f" the FIR within +-{derr:.3f} dB from 0 to 20 kHz")
    print(f"drive wet path at DC: chain {r['chain_gd_dc']:.3f} + ADAA 0.25 + FIR 2 = {wet_gd:.3f} samples; fx.c delays"
          f" the dry path round() = {int(round(wet_gd))} so the 5 ms engage crossfade does not comb")

    ok = True
    if a.check:
        err = max(abs(x - y) for x, y in zip(coefs, SPEC)) if a.ncoef == len(SPEC) else 1.0
        tests = [("coefficients reproduce spec 3.10.1 (max err %.1e)" % err, err < 1e-9),
                 ("flat to 20 kHz (ripple %.2e dB < 0.001)" % r20["ripple_db"], r20["ripple_db"] < 1e-3),
                 ("stopband from 24.1 kHz %.2f dB <= -84.5" % r20["stop_db"], r20["stop_db"] <= -84.5),
                 ("chain DC gain 1 (%.9f)" % r["chain_dc_gain"], abs(r["chain_dc_gain"] - 1.0) < 1e-6),
                 ("chain passes 20 kHz (%.2e dB)" % r["chain_20k_db"], abs(r["chain_20k_db"]) < 1e-3),
                 ("droop FIR flat to 20 kHz within +-%.3f dB (< 0.2)" % derr, derr < 0.2)]
        for name, good in tests:
            print(("ok   " if good else "FAIL ") + name)
            ok = ok and good

    if a.plot:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(2, 1, figsize=(8, 6), sharex=True)
        ax[0].plot(f / 1000.0, mag, lw=1)
        ax[0].axvline(20.0, color="g", ls=":", lw=1)
        ax[0].axvline(24.1, color="r", ls=":", lw=1)
        ax[0].axhline(-85.0, color="r", ls=":", lw=1)
        ax[0].set_ylim(-130, 5)
        ax[0].set_ylabel("|H| dB")
        ax[0].set_title(f"CYMA-1 drive halfband: {a.ncoef} allpass coefs, fs2 = {fs2 / 1000:.1f} kHz")
        ax[0].grid(alpha=0.3)
        ax[1].plot(f / 1000.0, gd, lw=1)
        ax[1].set_ylim(0, 40)
        ax[1].set_ylabel("group delay (2x samples)")
        ax[1].set_xlabel("kHz")
        ax[1].grid(alpha=0.3)
        fig.tight_layout()
        fig.savefig(a.plot, dpi=110)
        print("wrote", a.plot)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
