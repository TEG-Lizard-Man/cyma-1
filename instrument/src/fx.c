/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 master fx (M3, spec 3.10-3.11). The engine's float bus in, 24-bit integers out:
 *
 *   bus -> scan -> DRIVE (2x halfband, ADAA-1) -> TILT -> REVERB (8-line FDN, dry/wet) -> DC blocker
 *       -> LIMITER (64-sample look-ahead, CY_CEIL) -> MASTER -> one quantisation to Q23
 *
 * All float32 at the full rate; the only integer step is the last. Why each piece is the way it is:
 *  - DRIVE runs its saturator at 2x between two polyphase allpass halfbands, and the saturator itself is
 *    antiderivative anti-aliased: 2x keeps the low harmonics of bright modes from folding at all, ADAA-1 knocks down
 *    the high ones that still would (test_fx.c: 24-71 dB less aliasing than the naive curve). At small signal
 *    ADAA-1 at 2x is a two-tap mean (-2.4 dB at 20 kHz), so a 5-tap linear-phase FIR after the decimator flattens it
 *    to +-0.16 dB. Without the saturator the up -> down chain is exactly the allpass A(z) B(z); the whole wet path is
 *    4.94 samples at DC (chain 2.69, ADAA 1/4, FIR 2), so the dry path is delayed FX_DRY_D = 5: the 5 ms engage
 *    crossfade does not comb, and the latency does not change with the drive. tools/design_halfband.py makes and
 *    checks every one of these numbers.
 *  - TILT comes before the reverb: the room hears what you hear.
 *  - REVERB: 8 prime lines and Hadamard feedback, which is lossless, so every bit of decay comes from the per-line
 *    Jot absorbers and the T60 is what SIZE says (DAMPING scales the T60 at Nyquist). Lines 1 and 5 drift +-8
 *    samples against metallic ringing. The input gain follows the network's energy: the wet energy of an impulse is
 *    sum(g^2) / (1 - g^2) over passes, so the gain is set from the absorbers' mean g^2 to make that energy grow as
 *    SIZE^1/2: early reflections land at about the same level at any SIZE, a 12 s hall does not swamp the plate and
 *    a 0.3 s room is still heard (levels in test_fx.c [4]).
 *  - DC blocker before the limiter, so DC never costs headroom.
 *  - LIMITER: sample peak, stereo linked. The gain needed by each sample is held as a sliding minimum over the
 *    look-ahead (a monotonic deque), released over 80 ms, then box-averaged over the look-ahead: the average reaches
 *    the needed gain exactly as the peak leaves the delay, so the output never passes the ceiling (proof at
 *    cy_fx_out). The box sum is an exact Q24 integer, so it never drifts.
 *  - MASTER after the limiter, in float: the limiter never depends on the volume knob, and all 24 bits remain at
 *    any MASTER setting.
 *
 * Threads: cy_fx_set runs in the main loop and only writes the six targets (a read torn across them is harmless).
 * Everything else belongs to the render. cy_fx_process works in place on l/r, the engine's scratch bus.
 *
 * Denormals: a 1e-18 floor is added to every bus sample (and, as spec 3.10.3 says, to every FDN line input), so
 * each recursive state settles on a tiny normal value instead of decaying through the subnormal range. The DC
 * blocker removes the floor before the output, and the block code flushes its decaying state and the meters.
 * Silence in gives exact zeros out (test_fx.c also scans every state for subnormals).
 *
 * Every stage runs in passes over the block with its state in registers (the device's 16 registers are shared with
 * the FPU; a one-loop-per-sample FDN spilled 8 lines' state every sample and cost half again as much).
 *
 * Cost per stereo sample, counted from the device build's loops (its dual issue not credited): scan 29, DRIVE about
 * 340 engaged (16 bypassed), TILT 20, predelay and diffusers 80, the FDN's lines, mixing and writes about 310,
 * DC + limiter + MASTER + Q23 about 150: about 600 with the drive off and 940 with it on (spec 3.14 guessed 400 and
 * 590). .ram_text 3.8 KB. Pool 102,136 B: lines 81,376, predelay 10,624, diffusers 3,608, block scratch 5,120,
 * limiter 1,408. */
#include "cy_fx.h"
#include "fastmath.h"

#define CY_FX_INL static inline __attribute__((always_inline))

#define FX_NL 8
#define FX_MARGIN 16                     /* each line holds m + 16: room for the +-8 drift and the Hermite taps */
#define FX_LINE_BUF (20216 + FX_NL * FX_MARGIN)
#define FX_PD_LEN 2656                   /* 60 ms is 2,647 samples at the device's 44,117.6 Hz, + the lerp tap */
#define FX_AP_BUF (139 + 379 + 107 + 277)
#define FX_AP_G 0.62f
#define FX_LA 64                         /* the look-ahead N (power of two: the index is masked) */
#define FX_WIN (FX_LA + 1)               /* the minimum's window: the delay N needs N + 1 (see cy_fx_out) */
#define FX_DQ 128                        /* deque ring >= FX_WIN, a power of two */
#define FX_DRY_D 5                       /* the drive's dry path: round(4.94), see the top */
#define FX_TINY 1e-18f
#define FX_BUS_MAX 64.0f                 /* +36 dBFS: anything louder on the bus is a bug upstream; keeps u^2 finite */
#define FX_ISQRT8 0.353553391f
#define FX_LOG2_10 3.321928095f
#define FX_GIN_K 0.87f                   /* reverb level: wet energy 0.45 at SIZE 2.8 s, DAMPING 55 % (test_fx.c [4]) */
#define FX_PD_VMAX 0.1f                  /* predelay glide: at most 0.1 sample per sample (1.8 semitones of Doppler) */
#define FX_CEIL_T (CY_CEIL * 0.999999f)  /* the limiter aims a hair under the ceiling: float rounding stays below */

static const uint16_t CY_FX_M[FX_NL] = {1499, 1787, 2053, 2357, 2633, 2939, 3251, 3697};   /* primes, 20,216 */
static const uint16_t CY_FX_APL[4] = {139, 379, 107, 277};      /* diffusers: L 139 -> 379, R 107 -> 277 */
static const float CY_FX_HB[8] = {                              /* tools/design_halfband.py --check */
    0.053615466649f, 0.193436724219f, 0.372315971295f, 0.546689394877f,
    0.693030266958f, 0.806703409456f, 0.894013055065f, 0.965874302792f,
};
static const float CY_FX_DROOP[3] = {1.114654862f, -0.077683244f, 0.020355813f};   /* c0 c1 c2: ditto */
static const cy_fx_params_t CY_FX_DEF = {0.0f, 0.0f, 0.30f, 2.8f, 0.55f, 12.0f};   /* the spec 4.1 defaults */

/* the pool: zeroed by cy_fx_reset, never assumed */
static float cy_fx_line[FX_LINE_BUF] CY_POOL;
static float cy_fx_pd[FX_PD_LEN] CY_POOL;
static float cy_fx_apb[FX_AP_BUF] CY_POOL;
static float cy_fx_la[2][FX_LA] CY_POOL;                        /* the limiter's audio delay */
static int32_t cy_fx_box[FX_LA] CY_POOL;                        /* released gains, Q24 */
static float cy_fx_dqv[FX_DQ] CY_POOL;                          /* the deque: gains ascending front to back */
static uint8_t cy_fx_dqt[FX_DQ] CY_POOL;                        /* ... and when each was pushed (mod 256) */
#define FX_ROW (FX_NL + 2)                                      /* a scratch row per sample: 8 lines, el, er */
static float cy_fx_scr[FX_ROW * CY_BLOCK] CY_POOL;              /* a block's scratch (the drive borrows it too) */
#ifdef PLAT_HOST                         /* host tests only: the float output just before the one quantisation */
#define FX_TAP_N 4096u
static float cy_fx_tapv[2u * FX_TAP_N];
static uint32_t cy_fx_tapn;
#endif

typedef struct {
    float up[2][5], dn[2][5];            /* allpass chains A and B, up and down: {chain x[n-1], each section's y[n-1]} */
    float u1, s1;                        /* ADAA: u[n-1] and S(u[n-1]) */
    float fir[4];                        /* the droop FIR's last 4 inputs, newest first */
    float dry[8];                        /* the dry path's delay ring (mask 7) */
} cy_fx_drv_t;

typedef struct {                         /* one block's per-sample ramps: a start value and a step */
    float gl, dgl, gh, dgh, dry, ddry, wet, dwet, gin, dgin, pd, dpd;
    float b[FX_NL], db[FX_NL], p[FX_NL], dp[FX_NL];
    float md[2], dmd[2];                 /* the drifting lines' delay offsets from m (samples, -8..8) */
} cy_fx_ramp_t;

static struct {
    cy_fx_params_t tgt;                  /* cy_fx_set's targets */
    /* smoothed controls (per block) and where the last block's ramps ended */
    float drive_db, tilt_db, room, lsize, damping, pds, pd;
    float d, mk, gl, gh, dry, wet, gin, b[FX_NL], p[FX_NL];
    float csize, cdamp, cgin;            /* the (size, damping) b[] and p[] were made for, and its input gain */
    uint8_t snap;                        /* the next block jumps straight to the targets (after a reset) */
    /* drive */
    cy_fx_drv_t dv[2];
    float xf;                            /* wet share of the engage crossfade */
    uint8_t run, warm, dw;               /* the 2x path is running; its first (silent) block; dry ring index */
    /* tilt */
    float lp[2];
    /* reverb */
    uint32_t pw, apw[4], lw[FX_NL];      /* write indices: predelay, diffusers, lines */
    float z[FX_NL];                      /* absorber states */
    float ma[2], mb[2], mt[2], md[2];    /* drift of lines 1 and 5: from, to, eased phase 0..1, the offset now */
    uint32_t rng[2];                     /* ... and their own xorshifts (each line draws in its own order) */
    /* DC blocker */
    float dcx[2], dcy[2];
    /* limiter */
    float grel;
    int32_t sum;                         /* sum of cy_fx_box, exact */
    uint32_t dqh, dqt, li;
    uint8_t now;
    /* master and meters */
    float gm;                            /* < 0: take the first block's MASTER at once */
    float peak[2], gr;
    uint32_t nan;
    /* constants (cy_fx_init: they follow CY_FS) */
    float ta, dcr, krel, km, mdt[2], xfs;
    float *line[FX_NL], *ap[4];
} cy_fx;

/* ------------------------------------------------------------------ hot helpers (inlined: CY_HOT calls nothing) --- */

/* fastmath.h's helpers, forced inline: the device's clang 4.0.1 leaves even fm_clampf out of line at -O2, and a
 * CY_HOT function that calls one fails the link (a .ram_text -> flash jump does not reach) */
CY_FX_INL float cy_fx_abs(float x) { fm_bits_t b; b.f = x; b.u &= 0x7FFFFFFFu; return b.f; }
CY_FX_INL float cy_fx_min(float a, float b) { return a < b ? a : b; }
CY_FX_INL float cy_fx_max(float a, float b) { return a > b ? a : b; }
CY_FX_INL float cy_fx_clamp(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
CY_FX_INL float cy_fx_sqrt1(float x)     /* fm_sqrtf for x >= 1 (rel 2e-7): the rsqrt seed and three Newton steps */
{
    fm_bits_t b;
    float y;
    b.f = x;
    b.u = 0x5F375A86u - (b.u >> 1);
    y = b.f;
    y = y * (1.5f - 0.5f * x * y * y);
    y = y * (1.5f - 0.5f * x * y * y);
    y = y * (1.5f - 0.5f * x * y * y);
    return x * y;
}

/* four first-order allpass sections in series, (a + z^-1) / (1 + a z^-1) each as y = a (x - y[n-1]) + x[n-1], over a
 * block with the states in registers. Section k's previous input is section k-1's previous output, so a chain keeps
 * 5 states, not 8: st = {the chain's x[n-1], each section's y[n-1]}. in may be out. */
CY_FX_INL void cy_fx_ap4(const float *in, float *out, uint32_t n, float *st, float c0, float c1, float c2, float c3)
{
    float xp = st[0], y0 = st[1], y1 = st[2], y2 = st[3], y3 = st[4];
    uint32_t i;
    for (i = 0; i < n; i++) {
        float x = in[i], t0, t1, t2;
        t0 = c0 * (x - y0) + xp, xp = x;
        t1 = c1 * (t0 - y1) + y0, y0 = t0;
        t2 = c2 * (t1 - y2) + y1, y1 = t1;
        y3 = c3 * (t2 - y3) + y2, y2 = t2;
        out[i] = y3;
    }
    st[0] = xp, st[1] = y0, st[2] = y1, st[3] = y2, st[4] = y3;
}

/* a Schroeder allpass diffuser (z^-N - g) / (1 - g z^-N) on a ring of exactly N */
CY_FX_INL float cy_fx_dap(float x, float *buf, uint32_t *idx, uint32_t len)
{
    uint32_t k = *idx;
    float d = buf[k], w = x + FX_AP_G * d;
    buf[k] = w;
    *idx = k + 1u == len ? 0u : k + 1u;
    return d - FX_AP_G * w;
}

/* a 4-point Hermite read of a line at a fractional delay m + off (off in -8..8; m + off - 1 >= 1 and
 * m + off + 2 < len: the margin). The delay comes in as the integer m plus a small float offset, never as one float
 * near 3000: there a float's step is 2^-12 sample, so a per-sample ramp of the delay rounds to a jittering
 * fraction, and that jitter is broadband phase noise (it put a white floor about 90 dB under a held tone across the
 * whole band, the 0.45 fs gate included; integration fix, M7). Offsets near 8 resolve to 1e-6 sample. */
CY_FX_INL float cy_fx_tap(const float *buf, uint32_t w, uint32_t len, int32_t m, float off)
{
    int32_t kf = (int32_t)(off + 8.0f) - 8, k0 = m + kf, n = (int32_t)len;   /* floor(off) for off >= -8 */
    float f = off - (float)kf, xm, x0, x1, x2, c1, c2, c3;
    int32_t i0 = (int32_t)w + n - k0, im, i1, i2;   /* delay k0 sits at (w - k0) mod len */
    if (i0 >= n)
        i0 -= n;
    im = i0 + 1 >= n ? i0 + 1 - n : i0 + 1;         /* delay k0 - 1 (newer) */
    i1 = i0 > 0 ? i0 - 1 : n - 1;                   /* delay k0 + 1 (older) */
    i2 = i1 > 0 ? i1 - 1 : n - 1;
    xm = buf[im], x0 = buf[i0], x1 = buf[i1], x2 = buf[i2];
    c1 = 0.5f * (x1 - xm);
    c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
    return ((c3 * f + c2) * f + c1) * f + x0;
}

/* the next drift target, -8..8 samples (the xorshift of cy_config.h; signed casts: no soft-float helper) */
CY_FX_INL float cy_fx_drift(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13, x ^= x >> 17, x ^= x << 5, *s = x;
    return (float)(int32_t)(x >> 8) * (16.0f / 16777216.0f) - 8.0f;
}

/* -------------------------------------------------------------------------------------- the stages (CY_HOT) --- */

/* the bus: a NaN or Inf anywhere sends the caller to reset; magnitudes clamp; the denormal floor goes on */
CY_HOT static uint32_t cy_fx_scan(float *l, float *r, uint32_t n)
{
    uint32_t i, bad = 0;
    for (i = 0; i < n; i++) {
        fm_bits_t a, b;
        a.f = l[i], b.f = r[i];
        bad |= (uint32_t)((a.u & 0x7F800000u) == 0x7F800000u) | (uint32_t)((b.u & 0x7F800000u) == 0x7F800000u);
        l[i] = cy_fx_clamp(a.f, -FX_BUS_MAX, FX_BUS_MAX) + FX_TINY;
        r[i] = cy_fx_clamp(b.f, -FX_BUS_MAX, FX_BUS_MAX) + FX_TINY;
    }
    return bad;
}

/* DRIVE bypassed: only the dry path's delay, so the latency does not change with the drive */
CY_HOT static void cy_fx_drive_dry(float *l, float *r, uint32_t n)
{
    float *dl = cy_fx.dv[0].dry, *dr = cy_fx.dv[1].dry;
    uint32_t i, w = cy_fx.dw;
    for (i = 0; i < n; i++, w++) {
        float xl = l[i], xr = r[i];
        l[i] = dl[(w - FX_DRY_D) & 7u], dl[w & 7u] = xl;
        r[i] = dr[(w - FX_DRY_D) & 7u], dr[w & 7u] = xr;
    }
    cy_fx.dw = (uint8_t)(w & 7u);
}

/* DRIVE engaged or fading, a channel at a time and a stage at a time over the block (the states stay in registers):
 * up 2x (path A's output, then path B's, per input sample), u = d x through ADAA-1 of x / sqrt(1 + x^2), down (path
 * A takes the later sample of each pair), the droop FIR, the makeup, then the crossfade against the delayed dry
 * signal. ADAA-1 is (F(u) - F(u1)) / (u - u1) with F = S = sqrt(1 + u^2), rewritten as (u + u1) / (S(u) + S(u1)):
 * exact, no division by u - u1, and S >= 1 keeps it well conditioned. */
CY_HOT static void cy_fx_drive_run(float *l, float *r, uint32_t n, float d0, float dd, float mk0, float dmk,
                                   float xf0, float dxf)
{
    const float a0 = CY_FX_HB[0], a1 = CY_FX_HB[1], a2 = CY_FX_HB[2], a3 = CY_FX_HB[3];
    const float a4 = CY_FX_HB[4], a5 = CY_FX_HB[5], a6 = CY_FX_HB[6], a7 = CY_FX_HB[7];
    const float f0 = CY_FX_DROOP[0], f1 = CY_FX_DROOP[1], f2 = CY_FX_DROOP[2];
    float *ua = cy_fx_scr, *ub = cy_fx_scr + CY_BLOCK, xf = xf0;
    uint32_t c, i, w;
    for (c = 0; c < 2u; c++) {
        cy_fx_drv_t *v = &cy_fx.dv[c];
        float *io = c ? r : l, *dr = v->dry, d = d0, mk = mk0, u1 = v->u1, s1 = v->s1;
        float q0 = v->fir[0], q1 = v->fir[1], q2 = v->fir[2], q3 = v->fir[3];
        cy_fx_ap4(io, ua, n, v->up[0], a0, a2, a4, a6);
        cy_fx_ap4(io, ub, n, v->up[1], a1, a3, a5, a7);
        for (i = 0; i < n; i++) {
            float u, s;
            d += dd;
            u = d * ua[i], s = cy_fx_sqrt1(1.0f + u * u), ua[i] = (u + u1) / (s + s1), u1 = u, s1 = s;
            u = d * ub[i], s = cy_fx_sqrt1(1.0f + u * u), ub[i] = (u + u1) / (s + s1), u1 = u, s1 = s;
        }
        cy_fx_ap4(ub, ub, n, v->dn[0], a0, a2, a4, a6);
        cy_fx_ap4(ua, ua, n, v->dn[1], a1, a3, a5, a7);
        xf = xf0;
        for (i = 0, w = cy_fx.dw; i < n; i++, w++) {
            float h = 0.5f * (ub[i] + ua[i]), wet, dry;
            mk += dmk;
            xf = cy_fx_clamp(xf + dxf, 0.0f, 1.0f);
            wet = mk * (f2 * (h + q3) + f1 * (q0 + q2) + f0 * q1);
            q3 = q2, q2 = q1, q1 = q0, q0 = h;
            dry = dr[(w - FX_DRY_D) & 7u], dr[w & 7u] = io[i];
            io[i] = dry + xf * (wet - dry);
        }
        v->u1 = u1, v->s1 = s1, v->fir[0] = q0, v->fir[1] = q1, v->fir[2] = q2, v->fir[3] = q3;
    }
    cy_fx.dw = (uint8_t)((cy_fx.dw + n) & 7u);
    cy_fx.xf = xf;
}

/* TILT, then the REVERB (mono predelay, two diffusers per side, the FDN, the dry/wet mix), in passes over the block.
 * Every line is at least 1,489 samples long, longer than a block, so a block's reads all come from before it and
 * its writes can wait: read each line through its absorber (a line at a time, its states in registers), mix all 8
 * per sample, write each line. The scratch holds a row of 10 per sample (8 lines, el, er), so the mixing pass walks
 * one pointer. The absorbers carry the Hadamard's 1/sqrt 8 (b is scaled; z is then z / sqrt 8), and el, er carry
 * the denormal floor: the mixing pass has no constants left to hold. */
CY_HOT static void cy_fx_space(float *l, float *r, uint32_t n, cy_fx_ramp_t *rp)
{
    float *zb = cy_fx_scr;
    uint32_t i, k;
    {   /* TILT: y = lp gL + (x - lp) gH around a 650 Hz one-pole (gL = gH = 1 passes x bit-exact) */
        float gl = rp->gl, gh = rp->gh, dgl = rp->dgl, dgh = rp->dgh, lpl = cy_fx.lp[0], lpr = cy_fx.lp[1], ta = cy_fx.ta;
        for (i = 0; i < n; i++) {
            float xl = l[i], xr = r[i];
            gl += dgl, gh += dgh;
            lpl += ta * (xl - lpl), lpr += ta * (xr - lpr);
            l[i] = gh * xl + (gl - gh) * lpl;
            r[i] = gh * xr + (gl - gh) * lpr;
        }
        cy_fx.lp[0] = lpl, cy_fx.lp[1] = lpr;
    }
    {   /* PREDELAY (mono), written first so 0 ms reads this very sample; a linear tap only while it glides. Its
         * output waits in the rows' slot 0 (the lines fill slots 0..7 only later) */
        float pd = rp->pd, dpd = rp->dpd, *pdb = cy_fx_pd, *row = zb;
        uint32_t pw = cy_fx.pw;
        for (i = 0; i < n; i++, row += FX_ROW) {
            int32_t kd, ia, ib;
            float f;
            pd += dpd;
            pdb[pw] = 0.5f * (l[i] + r[i]);
            kd = (int32_t)pd, f = pd - (float)kd;
            ia = (int32_t)pw - kd;
            if (ia < 0)
                ia += FX_PD_LEN;
            ib = ia > 0 ? ia - 1 : FX_PD_LEN - 1;
            row[0] = pdb[ia] + f * (pdb[ib] - pdb[ia]);
            pw = pw + 1u == FX_PD_LEN ? 0u : pw + 1u;
        }
        cy_fx.pw = pw;
    }
    for (k = 0; k < 2u; k++) {   /* the DIFFUSERS, a side at a time: left (139, 379) -> el feeds the even lines,
                                  * right (107, 277) -> er the odd ones; with the input gain and the denormal floor */
        float gin = rp->gin, dgin = rp->dgin, *ba = cy_fx.ap[2u * k], *bb = cy_fx.ap[2u * k + 1u], *row = zb;
        uint32_t pa = cy_fx.apw[2u * k], pb = cy_fx.apw[2u * k + 1u];
        uint32_t la = CY_FX_APL[2u * k], lb = CY_FX_APL[2u * k + 1u];
        for (i = 0; i < n; i++, row += FX_ROW) {
            gin += dgin;
            row[8u + k] = gin * cy_fx_dap(cy_fx_dap(row[0], ba, &pa, la), bb, &pb, lb) + FX_TINY;
        }
        cy_fx.apw[2u * k] = pa, cy_fx.apw[2u * k + 1u] = pb;
    }
    for (k = 0; k < FX_NL; k++) {   /* the lines' outputs through the ABSORBERS (DC gain g, Nyquist gain g_hi) */
        const float *buf = cy_fx.line[k];
        float *zo = zb + k, b = rp->b[k], p = rp->p[k], db = rp->db[k], dp = rp->dp[k], z = cy_fx.z[k];
        uint32_t len = (uint32_t)CY_FX_M[k] + FX_MARGIN, w = cy_fx.lw[k];
        if (k == 1u || k == 5u) {   /* drifting +-8 samples (the block code eases it): a Hermite read */
            float d = rp->md[k == 5u], dd = rp->dmd[k == 5u];   /* the drift offset, -8..8 */
            int32_t m = (int32_t)CY_FX_M[k];
            /* review-perf: the same taps and arithmetic as cy_fx_tap (bit-exact), but the integer delay is held while
             * floor(d) stays put (it moves about once in 8 blocks) and the 4 taps walk a pointer: the wrap tests and
             * the int <-> float conversions of the index leave the per-sample path. Near the ring's end (the window
             * would straddle it) and when floor(d) moves, a sample takes cy_fx_tap as before. */
            for (i = 0; i < n;) {
                int32_t kfi = (int32_t)(d + dd + 8.0f), kf = kfi - 8, k0 = m + kf, nn = (int32_t)len, i0, run;
                float kff = (float)kf;
                i0 = (int32_t)w + nn - k0;
                if (i0 >= nn)
                    i0 -= nn;
                run = nn - 2 - i0;                   /* samples before the window [i0 - 2, i0 + 1] reaches the end */
                if (i0 < 2 || run <= 0) {            /* (straddling the start or the end: the general tap) */
                    d += dd, b += db, p += dp;
                    z = b * cy_fx_tap(buf, w, len, m, d) + p * z;
                    *zo = z;
                    zo += FX_ROW;
                    w = w + 1u == len ? 0u : w + 1u;
                    i++;
                    continue;
                }
                if (run > (int32_t)(n - i))
                    run = (int32_t)(n - i);
                {
                    const float *x = buf + i0;
                    int32_t j;
                    for (j = 0; j < run; j++, x++, zo += FX_ROW) {
                        float dn = d + dd, f, xm, x0, x1, x2, c1, c2, c3;
                        if ((int32_t)(dn + 8.0f) != kfi)
                            break;                   /* floor(d) moved: re-base (this sample is not consumed) */
                        d = dn, b += db, p += dp;
                        f = d - kff;
                        xm = x[1], x0 = x[0], x1 = x[-1], x2 = x[-2];
                        c1 = 0.5f * (x1 - xm);
                        c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
                        c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
                        z = b * (((c3 * f + c2) * f + c1) * f + x0) + p * z;
                        *zo = z;
                    }
                    i += (uint32_t)j;
                    w += (uint32_t)j;
                    if (w >= len)
                        w -= len;
                }
            }
        } else {                    /* delay m sits at w + 16 (the ring is m + 16): one or two straight runs */
            uint32_t ri = w + FX_MARGIN, e;
            if (ri >= len)
                ri -= len;
            for (i = 0; i < n;) {
                const float *src = buf + ri;
                e = i + (len - ri) < n ? i + (len - ri) : n;
                for (; i < e; i++, zo += FX_ROW)
                    b += db, p += dp, z = b * *src++ + p * z, *zo = z;
                ri = 0;
            }
        }
        cy_fx.z[k] = z;
    }
    {   /* the FEEDBACK and the injection, and the outputs. The feedback is the 8-point fast Walsh-Hadamard (/ sqrt 8,
         * in z already): orthogonal, lossless. The outputs come out of its first stage and wait in slots 8 and 9 for
         * the mix. L: spec 3.10.3's signs (+ + - + - - + -). R (+ - + - - - + +): orthogonal to L, and of the 35
         * orthogonal sign sets the one that keeps the tail's L/R correlation lowest at every SIZE (|rho| <= 0.03;
         * the Hadamard rows leave up to 0.5 in small rooms, where the lines' very unequal gains break the
         * orthogonality). */
        float *row = zb;
        for (i = 0; i < n; i++, row += FX_ROW) {
            float s0 = row[0] + row[1], s1 = row[0] - row[1], s2 = row[2] + row[3], s3 = row[2] - row[3];
            float s4 = row[4] + row[5], s5 = row[4] - row[5], s6 = row[6] + row[7], s7 = row[6] - row[7];
            float e = row[8], o = row[9], h0, h1, h2, h3, h4, h5, h6, h7;
            row[8] = (s0 - s3) - (s4 - s7), row[9] = (s1 + s3) - (s4 - s6);
            h0 = s0 + s2, h2 = s0 - s2, h1 = s1 + s3, h3 = s1 - s3;
            h4 = s4 + s6, h6 = s4 - s6, h5 = s5 + s7, h7 = s5 - s7;
            row[0] = (h0 + h4) + e, row[4] = (h0 - h4) + e, row[2] = (h2 + h6) + e, row[6] = (h2 - h6) + e;
            row[1] = (h1 + h5) + o, row[5] = (h1 - h5) + o, row[3] = (h3 + h7) + o, row[7] = (h3 - h7) + o;
        }
    }
    {   /* the dry/wet mix */
        float dry = rp->dry, wet = rp->wet, ddry = rp->ddry, dwet = rp->dwet, *row = zb;
        for (i = 0; i < n; i++, row += FX_ROW) {
            dry += ddry, wet += dwet;
            l[i] = dry * l[i] + wet * row[8];
            r[i] = dry * r[i] + wet * row[9];
        }
    }
    for (k = 0; k < FX_NL; k++) {   /* write the lines: one or two straight runs */
        float *buf = cy_fx.line[k];
        const float *zo = zb + k;
        uint32_t len = (uint32_t)CY_FX_M[k] + FX_MARGIN, w = cy_fx.lw[k], e;
        for (i = 0; i < n;) {
            float *dst = buf + w;
            e = i + (len - w) < n ? i + (len - w) : n;
            w += e - i;
            for (; i < e; i++, zo += FX_ROW)
                *dst++ = *zo;
            if (w == len)
                w = 0;
        }
        cy_fx.lw[k] = w;
    }
}

/* DC blocker, LIMITER, MASTER, Q23, in three passes over the block (the gains wait in the scratch). Why the limiter
 * cannot overshoot: the sample leaving the delay at n is x[n-N]; its gain is the mean of grel[n-N+1..n], and each
 * of those grel[k] <= min(greq[k-N..k]), a window that holds n-N. So every term, and the mean, is <= greq[n-N], and
 * |x[n-N]| greq[n-N] <= the ceiling. Returns nonzero if a non-finite value appeared inside the chain (the caller
 * resets). */
CY_HOT static uint32_t cy_fx_out(float *l, float *r, uint32_t n, int32_t *lr, float gmt, float *pk_out,
                                 float *gmin_out)
{
    float *g = cy_fx_scr;
    uint32_t i, bad = 0;
    {   /* the DC blocker (5 Hz), in place, and the gain each sample needs */
        float xl1 = cy_fx.dcx[0], xr1 = cy_fx.dcx[1], yl1 = cy_fx.dcy[0], yr1 = cy_fx.dcy[1], dcr = cy_fx.dcr;
        for (i = 0; i < n; i++) {
            float xl = l[i], xr = r[i], pk;
            yl1 = xl - xl1 + dcr * yl1, xl1 = xl, l[i] = yl1;
            yr1 = xr - xr1 + dcr * yr1, xr1 = xr, r[i] = yr1;
            pk = cy_fx_max(cy_fx_abs(yl1), cy_fx_abs(yr1));
            bad |= (uint32_t)!(pk < 1e30f);
            g[i] = pk > FX_CEIL_T ? FX_CEIL_T / pk : 1.0f;
        }
        cy_fx.dcx[0] = xl1, cy_fx.dcx[1] = xr1, cy_fx.dcy[0] = yl1, cy_fx.dcy[1] = yr1;
        if (bad)                 /* (never, with a finite bus) keep a NaN out of the delay: it would reach the
                                  * quantiser 64 samples on, inside this very block. The caller resets after. */
            for (i = 0; i < n; i++)
                l[i] = r[i] = 0.0f;
    }
    {   /* the sliding minimum over N + 1 (a monotonic deque: pop the back while not smaller, expire the front), the
         * 80 ms release (never above the minimum), the box mean over N as an exact sum of Q24 gains rounded down */
        float grel = cy_fx.grel, krel = cy_fx.krel, *dqv = cy_fx_dqv;
        int32_t sum = cy_fx.sum, *box = cy_fx_box;
        uint32_t dqh = cy_fx.dqh, dqt = cy_fx.dqt, li = cy_fx.li;
        uint8_t now = cy_fx.now, *dqm = cy_fx_dqt;
        for (i = 0; i < n; i++) {
            float greq = g[i];
            int32_t qg;
            while (dqt != dqh && dqv[(dqt - 1u) & (FX_DQ - 1)] >= greq)
                dqt--;
            dqv[dqt & (FX_DQ - 1)] = greq, dqm[dqt & (FX_DQ - 1)] = now, dqt++;
            if ((uint8_t)(now - dqm[dqh & (FX_DQ - 1)]) >= FX_WIN)
                dqh++;
            now++;
            grel += (1.0f - grel) * krel;
            grel = cy_fx_min(grel, dqv[dqh & (FX_DQ - 1)]);
            qg = (int32_t)(grel * 16777216.0f);
            sum += qg - box[li], box[li] = qg, li = (li + 1u) & (FX_LA - 1);
            g[i] = (float)sum * (1.0f / 1073741824.0f);
        }
        cy_fx.grel = grel, cy_fx.sum = sum, cy_fx.dqh = dqh, cy_fx.dqt = dqt, cy_fx.now = now;
    }
    {   /* the look-ahead delay and the gain, the meters (post-limiter, pre-MASTER), MASTER ((m / 4096)^2, 10 ms,
         * after the limiter), the one quantisation (round half away from zero; no dither: 24 bits, the DAC
         * self-dithers) */
        float gm = cy_fx.gm, km = cy_fx.km, pkl = 0.0f, pkr = 0.0f, gmn = 1.0f, *lal = cy_fx_la[0], *lar = cy_fx_la[1];
        uint32_t li = cy_fx.li;
        for (i = 0; i < n; i++) {
            float ol = lal[li] * g[i], orr = lar[li] * g[i];
            int32_t ql, qr;
            lal[li] = l[i], lar[li] = r[i], li = (li + 1u) & (FX_LA - 1);
            /* belt and braces: float rounding in g is under 1e-7 and FX_CEIL_T sits 1e-6 under, so this never bites.
             * It is also spec 3.10.7's clamp to +-8388607, made stricter: MASTER's gm never exceeds 1 */
            ol = cy_fx_clamp(ol, -CY_CEIL, CY_CEIL), orr = cy_fx_clamp(orr, -CY_CEIL, CY_CEIL);
            pkl = cy_fx_max(pkl, cy_fx_abs(ol)), pkr = cy_fx_max(pkr, cy_fx_abs(orr)), gmn = cy_fx_min(gmn, g[i]);
            gm += (gmt - gm) * km;
            ol *= gm, orr *= gm;
#ifdef PLAT_HOST
            if (cy_fx_tapn < FX_TAP_N)
                cy_fx_tapv[2u * cy_fx_tapn] = ol, cy_fx_tapv[2u * cy_fx_tapn + 1u] = orr, cy_fx_tapn++;
#endif
            ql = (int32_t)(ol * CY_OUT_SCALE + (ol >= 0.0f ? 0.5f : -0.5f));
            qr = (int32_t)(orr * CY_OUT_SCALE + (orr >= 0.0f ? 0.5f : -0.5f));
            lr[2u * i] = ql, lr[2u * i + 1u] = qr;
        }
        cy_fx.li = li, cy_fx.gm = gm;
        pk_out[0] = pkl, pk_out[1] = pkr, *gmin_out = gmn;
    }
    return bad;
}

/* XIP flash (0x02xxxxxx) -> RAM (0x01C0xxxx) is beyond the 23-bit call range, so the block code calls the CY_HOT
 * stages through pointers the compiler cannot fold back into direct calls (fm1-base's FL_FAR, bsp/chip/fm1_flash.h) */
static uint32_t (*volatile cy_fx_scan_far)(float *, float *, uint32_t) = cy_fx_scan;
static void (*volatile cy_fx_dry_far)(float *, float *, uint32_t) = cy_fx_drive_dry;
static void (*volatile cy_fx_run_far)(float *, float *, uint32_t, float, float, float, float, float, float) = cy_fx_drive_run;
static void (*volatile cy_fx_space_far)(float *, float *, uint32_t, cy_fx_ramp_t *) = cy_fx_space;
static uint32_t (*volatile cy_fx_out_far)(float *, float *, uint32_t, int32_t *, float, float *, float *) = cy_fx_out;

/* ------------------------------------------------------------------------------------- control (per block) --- */

/* the absorber of a line of m samples: one-pole with DC gain g (T60 = t_lo) and Nyquist gain g_hi (T60 = t_hi),
 * times 1/sqrt 8; returns g */
static float cy_fx_absorber(float m, float t_lo, float t_hi, float *b, float *p)
{
    float g = fm_exp2f(-3.0f * FX_LOG2_10 * m / (t_lo * CY_FS));
    float gh = fm_exp2f(-3.0f * FX_LOG2_10 * m / (t_hi * CY_FS));
    float pp = (g - gh) / (g + gh);
    *p = pp, *b = g * (1.0f - pp) * FX_ISQRT8;   /* the Hadamard's 1/sqrt 8 rides here (see cy_fx_space) */
    return g;
}

static float cy_fx_toward(float s, float t, float k, float eps)   /* one-pole step, snapping when close */
{
    s += (t - s) * k;
    return fm_fabsf(t - s) < eps ? t : s;
}

static void cy_fx_chunk(float *l, float *r, uint32_t n, uint32_t master_q12, int32_t *lr)
{
    cy_fx_params_t t = cy_fx.tgt;
    cy_fx_ramp_t rp;
    float fn = (float)(int32_t)n, inv = 1.0f / fn, nd, nmk, ngl, ngh, ndry, nwet, size, pdt, pd0, m;
    float k20, k80, pk[2], gmin, dec;
    uint32_t k, on;
    if (cy_fx_scan_far(l, r, n)) {           /* a NaN or Inf from upstream: reset, count, and play this block silent */
        cy_fx_reset();
        cy_fx.nan++;
        for (k = 0; k < n; k++)
            l[k] = r[k] = FX_TINY;
    }
    /* smoothing: one-pole per block on the controls (20 ms, SIZE and DAMPING 80 ms), ramped per sample below */
    k20 = 1.0f - fm_expf(-fn / (0.020f * CY_FS));
    k80 = 1.0f - fm_expf(-fn / (0.080f * CY_FS));
    pdt = fm_floorf(t.predelay_ms * (0.001f * CY_FS) + 0.5f);   /* whole samples: a settled tap does not filter */
    pdt = fm_clampf(pdt, 0.0f, (float)(FX_PD_LEN - 2));
    pd0 = cy_fx.pd;
    if (cy_fx.snap) {
        cy_fx.drive_db = t.drive_db, cy_fx.tilt_db = t.tilt_db, cy_fx.room = t.room;
        cy_fx.lsize = fm_log2f(t.size_s), cy_fx.damping = t.damping, cy_fx.pds = cy_fx.pd = pd0 = pdt;
    } else {
        cy_fx.drive_db = cy_fx_toward(cy_fx.drive_db, t.drive_db, k20, 1e-4f);
        cy_fx.tilt_db = cy_fx_toward(cy_fx.tilt_db, t.tilt_db, k20, 1e-4f);
        cy_fx.room = cy_fx_toward(cy_fx.room, t.room, k20, 1e-6f);
        cy_fx.lsize = cy_fx_toward(cy_fx.lsize, fm_log2f(t.size_s), k80, 1e-6f);
        cy_fx.damping = cy_fx_toward(cy_fx.damping, t.damping, k80, 1e-6f);
        /* the predelay glides: a slew-limited one-pole (at most FX_PD_VMAX sample per sample of Doppler on the
         * reverb's input, so a big jump takes up to 0.6 s), then a second one-pole to round its corners, and it
         * lands on a whole sample (no interpolation filtering once settled) */
        m = fm_clampf((pdt - cy_fx.pds) * k20, -FX_PD_VMAX * fn, FX_PD_VMAX * fn);
        cy_fx.pds = fm_fabsf(pdt - cy_fx.pds - m) < 1e-3f ? pdt : cy_fx.pds + m;
        cy_fx.pd = cy_fx_toward(cy_fx.pd, cy_fx.pds, k20, 1e-3f);
    }
    /* the new end values */
    nd = fm_db2lin(cy_fx.drive_db);
    nmk = fm_sqrtf(1.0f + 0.015625f * nd * nd) / nd;          /* keeps a -18 dBFS signal at its level */
    ngl = fm_db2lin(-0.5f * cy_fx.tilt_db), ngh = fm_db2lin(0.5f * cy_fx.tilt_db);   /* 10^(-+tilt/40) */
    ndry = 1.0f - 0.4f * cy_fx.room, nwet = 1.25f * cy_fx.room;
    size = fm_exp2f(cy_fx.lsize);
    if (cy_fx.snap || size != cy_fx.csize || cy_fx.damping != cy_fx.cdamp) {
        /* new absorbers, and the input gain from their mean g^2: wet energy ~ g2 / (1 - g2), aimed at
         * FX_GIN_K^2 (size / 2.8)^1/2 */
        float g2 = 0.0f;
        for (k = 0; k < FX_NL; k++) {
            float g = cy_fx_absorber((float)CY_FX_M[k], size, size * cy_fx.damping, &rp.b[k], &rp.p[k]);
            g2 += 0.125f * g * g;
        }
        cy_fx.cgin = FX_GIN_K * fm_sqrtf(fm_sqrtf(size * (1.0f / 2.8f)) * (1.0f - g2) / g2);
        cy_fx.csize = size, cy_fx.cdamp = cy_fx.damping;
    } else {
        for (k = 0; k < FX_NL; k++)
            rp.b[k] = cy_fx.b[k], rp.p[k] = cy_fx.p[k];
    }
    if (cy_fx.snap) {                    /* no ramps: the start values are the end values */
        cy_fx.d = nd, cy_fx.mk = nmk, cy_fx.gl = ngl, cy_fx.gh = ngh, cy_fx.dry = ndry, cy_fx.wet = nwet;
        cy_fx.gin = cy_fx.cgin;
        for (k = 0; k < FX_NL; k++)
            cy_fx.b[k] = rp.b[k], cy_fx.p[k] = rp.p[k];
    }
    /* ramps from the last block's end values to the new ones (b, p: rp holds the new ones until here) */
    for (k = 0; k < FX_NL; k++) {
        float bn = rp.b[k], pn = rp.p[k];
        rp.b[k] = cy_fx.b[k], rp.db[k] = (bn - cy_fx.b[k]) * inv, cy_fx.b[k] = bn;
        rp.p[k] = cy_fx.p[k], rp.dp[k] = (pn - cy_fx.p[k]) * inv, cy_fx.p[k] = pn;
    }
    rp.gl = cy_fx.gl, rp.dgl = (ngl - cy_fx.gl) * inv, cy_fx.gl = ngl;
    rp.gh = cy_fx.gh, rp.dgh = (ngh - cy_fx.gh) * inv, cy_fx.gh = ngh;
    rp.dry = cy_fx.dry, rp.ddry = (ndry - cy_fx.dry) * inv, cy_fx.dry = ndry;
    rp.wet = cy_fx.wet, rp.dwet = (nwet - cy_fx.wet) * inv, cy_fx.wet = nwet;
    rp.gin = cy_fx.gin, rp.dgin = (cy_fx.cgin - cy_fx.gin) * inv, cy_fx.gin = cy_fx.cgin;
    rp.pd = pd0, rp.dpd = (cy_fx.pd - pd0) * inv;
    /* the drift of lines 1 and 5: new random targets every 0.37 s and 0.53 s, cubic-eased between them, here per
     * block and ramped per sample (the curve is straight to 1e-3 sample over a block) */
    for (k = 0; k < 2u; k++) {
        float tt = cy_fx.mt[k] + fn * cy_fx.mdt[k], dn;
        if (tt >= 1.0f)
            tt -= 1.0f, cy_fx.ma[k] = cy_fx.mb[k], cy_fx.mb[k] = cy_fx_drift(&cy_fx.rng[k]);
        cy_fx.mt[k] = tt;
        dn = cy_fx.ma[k] + (cy_fx.mb[k] - cy_fx.ma[k]) * tt * tt * (3.0f - 2.0f * tt);   /* offset from m */
        rp.md[k] = cy_fx.md[k], rp.dmd[k] = (dn - cy_fx.md[k]) * inv, cy_fx.md[k] = dn;
    }

    /* DRIVE: engaged at >= 0.1 dB; a fresh engage starts its states at the input's DC steady state, runs one
     * silent block, then fades in over 5 ms (and out the same way) */
    on = cy_fx.drive_db >= 0.1f;
    if (cy_fx.snap) {
        cy_fx.run = (uint8_t)on, cy_fx.warm = 0, cy_fx.xf = on ? 1.0f : 0.0f;
    } else if (on && !cy_fx.run) {
        uint32_t c, j;
        for (c = 0; c < 2u; c++) {
            cy_fx_drv_t *v = &cy_fx.dv[c];
            float x0 = c ? r[0] : l[0], u = cy_fx.d * x0, s = fm_sqrtf(1.0f + u * u);
            for (j = 0; j < 5u; j++)
                v->up[0][j] = v->up[1][j] = x0, v->dn[0][j] = v->dn[1][j] = u / s;
            for (j = 0; j < 4u; j++)
                v->fir[j] = u / s;
            v->u1 = u, v->s1 = s;
        }
        cy_fx.run = 1, cy_fx.warm = 1, cy_fx.xf = 0.0f;
    }
    if (cy_fx.run) {
        float dxf = cy_fx.warm ? 0.0f : on ? cy_fx.xfs : -cy_fx.xfs;
        cy_fx_run_far(l, r, n, cy_fx.d, (nd - cy_fx.d) * inv, cy_fx.mk, (nmk - cy_fx.mk) * inv, cy_fx.xf, dxf);
        cy_fx.warm = 0;
        if (!on && cy_fx.xf <= 0.0f)
            cy_fx.run = 0;
    } else {
        cy_fx_dry_far(l, r, n);
    }
    cy_fx.d = nd, cy_fx.mk = nmk;

    cy_fx_space_far(l, r, n, &rp);

    /* MASTER: the raw pot position 0..4096 (APP_GAIN(m) = m), squared */
    m = (float)(int32_t)(master_q12 > 4096u ? 4096u : master_q12) * (1.0f / 4096.0f);
    m *= m;
    if (cy_fx.gm < 0.0f)
        cy_fx.gm = m;
    if (cy_fx_out_far(l, r, n, lr, m, pk, &gmin)) {   /* non-finite inside the chain (it cannot be; still) */
        cy_fx_reset();
        cy_fx.nan++;
        return;
    }
    /* meters: post-limiter, pre-MASTER peaks with a 300 ms decay; GR in dB (>= 0) the same way */
    dec = fm_expf(-fn / (0.300f * CY_FS));
    cy_fx.peak[0] = fm_maxf(pk[0], cy_fx.peak[0] * dec);
    cy_fx.peak[1] = fm_maxf(pk[1], cy_fx.peak[1] * dec);
    cy_fx.gr = fm_maxf(-6.020599913f * fm_log2f(gmin), cy_fx.gr * dec);
    /* flush what decays toward zero instead of onto the floor */
    for (k = 0; k < 2u; k++) {
        cy_fx.dcy[k] = fm_flush(cy_fx.dcy[k]);
        cy_fx.peak[k] = cy_fx.peak[k] < 1e-9f ? 0.0f : cy_fx.peak[k];
    }
    cy_fx.gr = cy_fx.gr < 1e-6f ? 0.0f : cy_fx.gr;
    if (fm_fabsf(cy_fx.gm - m) < 1e-7f)
        cy_fx.gm = m;
    cy_fx.snap = 0;
}

/* ------------------------------------------------------------------------------------------------- the API --- */

void cy_fx_init(void)
{
    uint32_t k, off = 0;
    for (k = 0; k < FX_NL; k++)
        cy_fx.line[k] = cy_fx_line + off, off += (uint32_t)CY_FX_M[k] + FX_MARGIN;
    for (k = 0, off = 0; k < 4u; k++)
        cy_fx.ap[k] = cy_fx_apb + off, off += CY_FX_APL[k];
    cy_fx.ta = 1.0f - fm_expf(-FM_TWO_PI * 650.0f / CY_FS);
    cy_fx.dcr = 1.0f - FM_TWO_PI * 5.0f / CY_FS;
    cy_fx.krel = 1.0f - fm_expf(-1.0f / (0.080f * CY_FS));
    cy_fx.km = 1.0f - fm_expf(-1.0f / (0.010f * CY_FS));
    cy_fx.mdt[0] = 1.0f / (0.37f * CY_FS), cy_fx.mdt[1] = 1.0f / (0.53f * CY_FS);
    cy_fx.xfs = 1.0f / (0.005f * CY_FS);
    cy_fx.tgt = CY_FX_DEF;
    cy_fx.nan = 0;
    cy_fx_reset();
}

void cy_fx_set(const cy_fx_params_t *p)
{
    cy_fx_params_t q;
#define FX_SANE(f, lo, hi) q.f = p->f != p->f ? CY_FX_DEF.f : fm_clampf(p->f, lo, hi)   /* NaN: the default */
    FX_SANE(drive_db, 0.0f, 24.0f);
    FX_SANE(tilt_db, -6.0f, 6.0f);
    FX_SANE(room, 0.0f, 1.0f);
    FX_SANE(size_s, 0.3f, 12.0f);
    FX_SANE(damping, 0.2f, 1.0f);
    FX_SANE(predelay_ms, 0.0f, 60.0f);
#undef FX_SANE
    cy_fx.tgt = q;
}

void cy_fx_process(float *l, float *r, uint32_t n, uint32_t master_q12, int32_t *lr)
{
    while (n) {
        uint32_t k = n > CY_BLOCK ? CY_BLOCK : n;
        cy_fx_chunk(l, r, k, master_q12, lr);
        l += k, r += k, lr += 2u * k, n -= k;
    }
}

/* n bytes (a multiple of 4) to zero: the device has no libc headers, and this is init-time only */
static void cy_fx_zero(void *p, uint32_t n)
{
    uint32_t *w = (uint32_t *)p, i;
    for (i = 0; i < n / 4u; i++)
        w[i] = 0;
}

/* every audio state back to silence; the targets and the NaN counter stay, the next block snaps to the targets */
void cy_fx_reset(void)
{
    uint32_t k;
    cy_fx_zero(cy_fx_line, sizeof cy_fx_line);
    cy_fx_zero(cy_fx_pd, sizeof cy_fx_pd);
    cy_fx_zero(cy_fx_apb, sizeof cy_fx_apb);
    cy_fx_zero(cy_fx_la, sizeof cy_fx_la);
    cy_fx_zero(cy_fx_dqv, sizeof cy_fx_dqv);
    cy_fx_zero(cy_fx_dqt, sizeof cy_fx_dqt);
    cy_fx_zero(cy_fx.dv, sizeof cy_fx.dv);
    for (k = 0; k < FX_LA; k++)
        cy_fx_box[k] = 16777216;         /* gain 1.0 in Q24 */
    cy_fx.sum = 1073741824;
    cy_fx.grel = 1.0f, cy_fx.dqh = cy_fx.dqt = cy_fx.li = 0, cy_fx.now = 0;
    for (k = 0; k < FX_NL; k++)
        cy_fx.z[k] = 0.0f, cy_fx.lw[k] = 0;
    for (k = 0; k < 4u; k++)
        cy_fx.apw[k] = 0;
    cy_fx.pw = 0, cy_fx.dw = 0, cy_fx.run = cy_fx.warm = 0, cy_fx.xf = 0.0f;
    cy_fx.lp[0] = cy_fx.lp[1] = cy_fx.dcx[0] = cy_fx.dcx[1] = cy_fx.dcy[0] = cy_fx.dcy[1] = 0.0f;
    cy_fx.rng[0] = 0x5EED0F3Du, cy_fx.rng[1] = 0x2545F491u;   /* fixed seeds: cy_fx_init makes runs repeat */
    for (k = 0; k < 2u; k++)
        cy_fx.ma[k] = 0.0f, cy_fx.mb[k] = cy_fx_drift(&cy_fx.rng[k]), cy_fx.mt[k] = 0.0f,
        cy_fx.md[k] = 0.0f;
    cy_fx.gm = -1.0f;
    cy_fx.peak[0] = cy_fx.peak[1] = cy_fx.gr = 0.0f;
    cy_fx.snap = 1;
}

void cy_fx_meter(cy_fx_meter_t *m)
{
    m->peak_l = cy_fx.peak[0];
    m->peak_r = cy_fx.peak[1];
    m->gr_db = cy_fx.gr;
    m->nan_resets = cy_fx.nan;
}

/* samples from bus to lr: the look-ahead plus the drive's (always present) dry delay */
uint32_t cy_fx_latency(void) { return FX_LA + FX_DRY_D; }

#ifdef PLAT_HOST
/* tests/unit/test_fx.c only (not in cy_fx.h): how many floats of the fx state are subnormal right now */
static uint32_t cy_fx_count_sub(const float *p, uint32_t n)
{
    uint32_t i, c = 0;
    for (i = 0; i < n; i++) {
        fm_bits_t b;
        b.f = p[i];
        c += (uint32_t)((b.u & 0x7F800000u) == 0u && (b.u & 0x007FFFFFu) != 0u);
    }
    return c;
}
uint32_t cy_fx_debug_subnormals(void)
{
    return cy_fx_count_sub(cy_fx_line, FX_LINE_BUF) + cy_fx_count_sub(cy_fx_pd, FX_PD_LEN)
           + cy_fx_count_sub(cy_fx_apb, FX_AP_BUF) + cy_fx_count_sub(&cy_fx_la[0][0], 2u * FX_LA)
           + cy_fx_count_sub(cy_fx_dqv, FX_DQ) + cy_fx_count_sub(cy_fx_scr, FX_ROW * CY_BLOCK) + cy_fx_count_sub((const float *)cy_fx.dv, sizeof cy_fx.dv / 4u)
           + cy_fx_count_sub(cy_fx.z, FX_NL) + cy_fx_count_sub(cy_fx.lp, 2u) + cy_fx_count_sub(cy_fx.dcx, 2u)
           + cy_fx_count_sub(cy_fx.dcy, 2u) + cy_fx_count_sub(&cy_fx.grel, 1u) + cy_fx_count_sub(&cy_fx.gm, 1u)
           + cy_fx_count_sub(cy_fx.peak, 2u);
}
/* the FDN input gain now (the test's level calibration) */
float cy_fx_debug_gin(void) { return cy_fx.gin; }
/* the float output (post-MASTER, pre-quantisation) of every sample since the last call, interleaved L R; up to 4096
 * frames are kept. The app's host `wavf` capture drains it after each render (integration, M7): spec 8.3.2's
 * ultrasonic gate measures this, because the undithered 24-bit container's own floor is what a quiet int24 render
 * shows above 0.45 fs (requantising the render band-limited to 18 kHz reproduces its figure). */
uint32_t cy_fx_debug_tap(const float **lr)
{
    uint32_t n = cy_fx_tapn;
    *lr = cy_fx_tapv;
    cy_fx_tapn = 0;
    return n;
}
#endif
