/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 unit test for M2 (src/engine.c, voice.c, bow.c), host only (double and libm are fine here; the engine
 * itself uses neither):
 *
 *   cc -O2 -std=c99 -ffp-contract=off -DPLAT_HOST -Iinstrument/src -Ireference/fm1-base/include \
 *      instrument/tests/unit/test_engine.c instrument/src/engine.c instrument/src/voice.c instrument/src/bow.c \
 *      -lm -o test_engine
 *   ./test_engine [EVIDENCE_DIR]           exit 0 = every check passed; EVIDENCE_DIR gets WAVs and a CSV or two
 *   ./test_engine --hash                   prints only the determinism hash (run it under node after emcc too)
 *
 * By default the test supplies its own geometry (an analytic disc whose ratio set it chooses: inharmonic plate,
 * Bessel membrane, harmonic) and a capturing fx stub, so it measures the engine's float bus exactly. Integration
 * variants: -DCY_TEST_REAL_GEOM links src/geom.c + src/plates_data.c, -DCY_TEST_REAL_FX links src/fx.c (the bus is
 * then read before fx from cy_e.bus_* only when fx does not write it in place; checks that need the pre-fx bus are
 * skipped).
 *
 * The acceptance list (task + spec §3.3-3.9, 3.12-3.14): no NaN or denormal over long renders and exact zeros after;
 * sine-phase onsets (the kernel against its closed form) and click-free key-up; tuning and ratio accuracy; the mode
 * budget; stealing fades in 5 ms without a click; the bow sweep across force x pressure x pitch; RAIN density
 * statistics (Poisson rate and inter-arrival CV); telemetry consistency; deterministic hashes; the CPU model. Plus:
 * freeze bounded, SURFACE touch harmonics and the key-LED map, nothing above 0.45 fs, morph glide without clicks,
 * BLOOM's pitch fall, latch / damp / panic, MIDI pressure. */
#define _POSIX_C_SOURCE 200112L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cy_engine_int.h"

static int n_check, n_fail, quiet;
#define CHECK(c, ...) do { n_check++; if (!(c)) { n_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)
#define SAY(...) do { if (!quiet) printf(__VA_ARGS__); } while (0)
#define T_PI 3.14159265358979323846
#define FS 44100.0

static const char *ev_dir;

/* ------------------------------------------------------------------ capture --- */
#define CAP_MAX (44100 * 100)
static float *capL, *capR;
static size_t capN;
static int32_t blk[2 * CY_BLOCK];
static void cap_reset(void) { capN = 0; }

#ifndef CY_TEST_REAL_FX
void cy_fx_init(void) {}
void cy_fx_set(const cy_fx_params_t *p) { (void)p; }
void cy_fx_process(float *l, float *r, uint32_t n, uint32_t m, int32_t *lr)
{
    uint32_t i;
    (void)m;
    for (i = 0; i < n; i++) {
        float a = l[i] * 8388607.0f, b = r[i] * 8388607.0f;
        lr[2 * i] = (int32_t)(a > 8388607.0f ? 8388607.0f : a < -8388607.0f ? -8388607.0f : a);
        lr[2 * i + 1] = (int32_t)(b > 8388607.0f ? 8388607.0f : b < -8388607.0f ? -8388607.0f : b);
    }
}
void cy_fx_reset(void) {}
void cy_fx_clear(void) {}                /* (v1.1.1: cy_engine_restart's) */
void cy_fx_snap(void) {}
void cy_fx_meter(cy_fx_meter_t *m) { memset(m, 0, sizeof *m); }
uint32_t cy_fx_latency(void) { return 0; }
#endif

/* ---------------------------------------------------------------- mock geom --- */
uint32_t plat_ms(void) { static uint32_t t = 1000; return t += 16; }   /* (real geom reads a clock) */
enum { SET_PLATE, SET_BESSEL, SET_HARM };
static const double BESSEL[CY_NMODES] = {1.0, 1.5933, 2.1355, 2.2954, 2.6531, 2.9173, 3.1554, 3.5001, 3.5985,
    3.6475, 4.0589, 4.0773, 4.2307, 4.6105, 4.8315, 4.9696, 5.1308, 5.4124, 5.5732, 5.6566, 5.8131, 6.1527, 6.1595,
    6.2058, 6.3984, 6.5288, 6.7065, 6.7472, 6.9605, 7.0106, 7.1922, 7.2341};
#ifndef CY_TEST_REAL_GEOM
static cy_object_t mobj;
static cy_fieldcache_t mfc;
static int mm[CY_NMODES], mn[CY_NMODES], msin[CY_NMODES];
const cy_object_t *cy_geom_object(void) { return &mobj; }
const cy_fieldcache_t *cy_geom_fields(void) { return &mfc; }
static double mphi(int k, double u, double v)          /* an analytic disc mode, |phi| <= 1 */
{
    double du = u - 0.5, dv = v - 0.5, r = sqrt(du * du + dv * dv) / 0.46, th = atan2(dv, du);
    double a = msin[k] ? sin(mm[k] * th) : cos(mm[k] * th);
    if (r > 1.0)
        r = 1.0;
    return a * cos(T_PI * (mn[k] + 0.5 * mm[k] + 0.15) * r);
}
float cy_geom_field_at(const cy_fieldcache_t *f, int mode, float u, float v) { (void)f; return (float)mphi(mode, u, v); }
static int mfc_busy;                                   /* a test can mark the cache "being rewritten" (geom.c's seqlock) */
int cy_geom_fields_stable(void) { return !mfc_busy; }
static void pull_in(double *u, double *v)
{
    int i;
    for (i = 0; i < 50; i++) {
        double du = *u - 0.5, dv = *v - 0.5;
        if (sqrt(du * du + dv * dv) < 0.43)
            break;
        *u += 0.02 * (0.5 - *u), *v += 0.02 * (0.5 - *v);
    }
}

static int set_now = SET_PLATE;
static double mpos_u = 0.62, mpos_v = 0.40, mwander = 0.0, mmorph = 0.0;
/* build the object: ratios from the set (morph blends toward the harmonic set in log2), the STEEL material law,
 * the mics, out_norm, SURFACE key points, rain and wander points, exactly as spec §3.2 lays them out */
static void mock_build(void)
{
    static int inited;
    double ratio[CY_NMODES], sig_l[CY_NMODES], sig_r[CY_NMODES], eref = 0.0;
    uint32_t s = 0xC0FFEEu;
    int k, i, j, np = 0;
    if (!inited) {                       /* mode shapes: (m, n) by Chladni's law (m + 2n)^2, cos/sin pairs */
        int c = 0, m, n;
        double key[64];
        int km[64], kn[64], ks[64];
        for (m = 0; m < 8; m++)
            for (n = 0; n < 6; n++) {
                if ((m == 0 && n == 0) || (m == 1 && n == 0))
                    continue;
                for (i = 0; i < (m ? 2 : 1) && c < 64; i++)
                    key[c] = pow(m + 2.0 * n + 0.07 * m, 2.0) + 1e-6 * c, km[c] = m, kn[c] = n, ks[c] = i, c++;
            }
        for (k = 0; k < CY_NMODES; k++) {    /* the 32 lowest */
            int b = -1;
            for (i = 0; i < c; i++)
                if (key[i] >= 0.0 && (b < 0 || key[i] < key[b]))
                    b = i;
            mm[k] = km[b], mn[k] = kn[b], msin[k] = ks[b];
            ratio[k] = key[b];
            key[b] = -1.0;
        }
        inited = 1;
        for (k = 0; k < CY_NMODES; k++)
            for (i = 0; i < CY_GRID; i++)
                for (j = 0; j < CY_GRID; j++)
                    mfc.field[k][i][j] = (int8_t)lrint(127.0 * mphi(k, (j + 0.5) / CY_GRID, (i + 0.5) / CY_GRID));
    }
    for (k = 0; k < CY_NMODES; k++) {
        double r0 = pow(mm[k] + 2.0 * mn[k] + 0.07 * mm[k], 2.0) / pow(2.14, 2.0), rt;
        if (set_now == SET_BESSEL)
            r0 = BESSEL[k];
        else if (set_now == SET_HARM)
            r0 = k + 1.0;
        if (msin[k] && set_now == SET_PLATE)
            r0 *= pow(2.0, 3.0 / 1200.0);    /* SHIMMER 3 c on the degenerate partner */
        rt = k + 1.0;
        ratio[k] = pow(2.0, (1.0 - mmorph) * log2(r0) + mmorph * log2(rt));
    }
    {   /* normalise to the lowest, then order */
        double mn0 = 1e9;
        for (k = 0; k < CY_NMODES; k++)
            mn0 = fmin(mn0, ratio[k]);
        for (k = 0; k < CY_NMODES; k++)
            ratio[k] /= mn0;
    }
    for (k = 0; k < CY_NMODES; k++) {
        int c = 0;
        mobj.ratio[k] = (float)ratio[k];
        mobj.log2_ratio[k] = (float)log2(ratio[k]);
        mobj.damp_div[k] = (float)(1.0 + 0.12 * (pow(ratio[k], 0.9) - 1.0));   /* STEEL */
        for (i = 0; i < CY_NMODES; i++)
            c += ratio[i] < ratio[k] || (ratio[i] == ratio[k] && i < k);
        mobj.order[c] = (uint8_t)k;
        mobj.degen[k] = (uint8_t)k;
        sig_l[k] = (cy_rand(&s) & 1u) ? 1.0 : -1.0;
        sig_r[k] = (cy_rand(&s) & 1u) ? 1.0 : -1.0;
        if (sig_r[k] == sig_l[k] && (k & 1))
            sig_r[k] = -sig_r[k];
    }
    mobj.t0 = 5.0f;
    {
        double ml[2] = {0.5 - 0.33 * 0.5, 0.62}, mr[2] = {0.5 + 0.33 * 0.5, 0.62};
        for (k = 0; k < CY_NMODES; k++) {
            mobj.mic_l[k] = (float)(0.75 * mphi(k, ml[0], ml[1]) + 0.35 * sig_l[k]);
            mobj.mic_r[k] = (float)(0.75 * mphi(k, mr[0], mr[1]) + 0.35 * sig_r[k]);
        }
        mobj.mic_uv[0][0] = (float)ml[0], mobj.mic_uv[0][1] = (float)ml[1];
        mobj.mic_uv[1][0] = (float)mr[0], mobj.mic_uv[1][1] = (float)mr[1];
    }
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            double u = 0.2 + 0.2 * j, v = 0.2 + 0.2 * i, a = 0.0;
            if (hypot(u - 0.5, v - 0.5) > 0.44)
                continue;
            for (k = 0; k < CY_NMODES; k++)
                a += mphi(k, u, v) * mphi(k, u, v) * (mobj.mic_l[k] * mobj.mic_l[k] + mobj.mic_r[k] * mobj.mic_r[k]) * 0.5;
            eref += a, np++;
        }
    eref /= np;
    mobj.out_norm = (float)(0.12 / sqrt(eref + 1e-6));
    for (k = 0; k < CY_NMODES; k++)
        mobj.strike[k] = (float)mphi(k, mpos_u, mpos_v);
    mobj.pos_uv[0] = (float)mpos_u, mobj.pos_uv[1] = (float)mpos_v;
    {   /* SURFACE: the keybed, panned by POS (spec §3.2 step 11) */
        static const int AJ[11] = {0, 1, 2, 4, 5, 7, 8, 9, 11, 12, 14};
        static const int WHITE[16] = {0, 2, 4, 5, 7, 9, 11, 12, 14, 16, 17, 19, 21, 23, 24, 26};
        static const int BLACK[11] = {1, 3, 6, 8, 10, 13, 15, 18, 20, 22, 25};
        for (i = 0; i < 16; i++) {
            double u = 0.10 + 0.80 * i / 15.0 + (mpos_u - 0.62) * 0.5, v = 0.66 + (mpos_v - 0.40) * 0.5;
            pull_in(&u, &v);
            mobj.key_uv[WHITE[i]][0] = (float)u, mobj.key_uv[WHITE[i]][1] = (float)v;
        }
        for (i = 0; i < 11; i++) {
            double u = 0.10 + 0.80 * (AJ[i] + 0.5) / 15.0 + (mpos_u - 0.62) * 0.5, v = 0.36 + (mpos_v - 0.40) * 0.5;
            pull_in(&u, &v);
            mobj.key_uv[BLACK[i]][0] = (float)u, mobj.key_uv[BLACK[i]][1] = (float)v;
        }
        for (i = 0; i < CY_NKEYS; i++)
            for (k = 0; k < CY_NMODES; k++)
                mobj.surface[i][k] = (float)mphi(k, mobj.key_uv[i][0], mobj.key_uv[i][1]);
    }
    s = 0xA11CEu;
    for (i = 0; i < CY_RAIN_PTS; i++) {
        double r = 0.45 * 0.3 * sqrt(cy_randf(&s)), a = 2.0 * T_PI * cy_randf(&s);
        double u = mpos_u + r * cos(a), v = mpos_v + r * sin(a);
        pull_in(&u, &v);
        mobj.rain_uv[i][0] = (float)u, mobj.rain_uv[i][1] = (float)v;
        for (k = 0; k < CY_NMODES; k++)
            mobj.rain[i][k] = (float)mphi(k, u, v);
    }
    s = 0xBEEFu;
    for (i = 0; i < CY_WANDER_PTS; i++) {
        double r = 0.30 * mwander * sqrt(cy_randf(&s)), a = 2.0 * T_PI * cy_randf(&s);
        double u = mpos_u + r * cos(a), v = mpos_v + r * sin(a);
        pull_in(&u, &v);
        mobj.wander_uv[i][0] = (float)u, mobj.wander_uv[i][1] = (float)v;
        for (k = 0; k < CY_NMODES; k++)
            mobj.wander[i][k] = (float)mphi(k, u, v);
    }
    mobj.seq++;
}
static void geom_set(int set, double pu, double pv)
{
    set_now = set, mpos_u = pu, mpos_v = pv, mmorph = 0.0;
    mock_build();
}
#else
static void geom_set(int set, double pu, double pv)   /* real geom: set -> a shape and stiffness */
{
    cy_geom_in_t g;
    static int inited;
    memset(&g, 0, sizeof g);
    if (!inited)
        cy_geom_init(), inited = 1;
    g.shape = set == 1 ? 0.0f : set == 2 ? 8.0f : 1.0f;
    g.stiffness = set == 1 ? 0.0f : 0.8f;
    g.material = 3.0f;
    g.shimmer_cents = 3.0f;
    g.pos_u = (float)pu, g.pos_v = (float)pv;
    g.mic_spread = 0.5f, g.rain_spread = 0.3f;
    cy_geom_update(&g);
}
#endif

/* -------------------------------------------------------------------- patch --- */
static cy_patch_t P;
static void patch_default(void)
{
    memset(&P, 0, sizeof P);
    P.exciter = CY_EXC_MALLET, P.surface = 0, P.max_voices = 8, P.modes_per_voice = 32;
    P.decay_mul = 1.0f, P.hardness = 0.55f, P.tension_st = 2.0f, P.palm = 0.3f, P.force = 0.6f, P.grit = 0.4f;
    P.swell_s = 0.25f, P.density = 0.4f, P.noise = 0.15f, P.damper = 0.0f, P.bloom = 0.0f, P.stretch = 0.0f;
    P.vibrato_cents = 0.0f, P.vib_rate_hz = 5.0f, P.tune_cents = 0.0f, P.level_db = 0.0f, P.surface_root = 48.0f;
    P.drive_db = 0.0f, P.tilt_db = 0.0f, P.room = 0.3f, P.size_s = 2.8f, P.damping = 0.55f, P.predelay_ms = 12.0f;
}

/* ------------------------------------------------------------------- render --- */
static uint32_t last_seq, seq_bad, budget_bad, modes_max, nyq_bad;
static float est_max;
static cy_telemetry_t T;
static uint32_t drops, strikes, slips, touches, drop_t[20000], ndrop_t;
static uint32_t nsamp;
static int subnormal_seen;

static int is_sub(float x) { return x != 0.0f && fabsf(x) < 1.17549435e-38f; }
static void scan_slots(void)
{
    int i, k;
    for (k = 0; k < CY_NKEYS; k++)                /* (the SURFACE keys' smoothed pressure: fix-dsp) */
        if (is_sub(cy_e.sk_P[k]))
            subnormal_seen++;
    for (i = 0; i < CY_NALL; i++) {
        const cy_slot_t *sl = &cy_e.sl[i];
        for (k = 0; k < CY_NMODES; k++)           /* (the bow point's field glide too: fix-dsp) */
            if (is_sub(sl->zr[k]) || is_sub(sl->zi[k]) || is_sub(sl->gl[k]) || is_sub(sl->gr[k]) || is_sub(sl->bphi[k]))
                subnormal_seen++;
        if (sl->state != CY_VS_FREE)
            for (k = 0; k < CY_NMODES; k++)
                if ((sl->act >> k & 1u) && sl->w[k] > (float)(2.0 * T_PI * 0.45) + 1e-4f)
                    nyq_bad++;
    }
}
static void render1(void)
{
    uint32_t i;
    cy_uievent_t ev;
    cy_engine_render(blk, CY_BLOCK, 4096u);
    for (i = 0; i < CY_BLOCK && capN < CAP_MAX; i++, capN++) {
        capL[capN] = cy_e.bus_l[i];
        capR[capN] = cy_e.bus_r[i];
        if (is_sub(cy_e.bus_l[i]) || is_sub(cy_e.bus_r[i]))
            subnormal_seen++;
    }
    nsamp += CY_BLOCK;
    cy_engine_telemetry_get(&T);
    if ((T.seq & 1u) || T.seq != last_seq + 2u)
        seq_bad++;
    last_seq = T.seq;
    if (T.modes_used > T.mode_budget)
        budget_bad++;
    if (T.modes_used > modes_max)
        modes_max = T.modes_used;
    if (T.est_instr > est_max)
        est_max = T.est_instr;
    while (cy_engine_event(&ev)) {
        if (ev.type == CY_UIEV_DROP) {
            drops++;
            if (ndrop_t < 20000u)
                drop_t[ndrop_t++] = nsamp;
        }
        strikes += ev.type == CY_UIEV_STRIKE;
        slips += ev.type == CY_UIEV_SLIP;
        touches += ev.type == CY_UIEV_TOUCH;
    }
    scan_slots();
}
static void run_ms(double ms)
{
    int nb = (int)ceil(ms * FS / 1000.0 / CY_BLOCK), i;
    for (i = 0; i < nb; i++)
        render1();
}
static void eng_reset(void)
{
    cy_engine_init();
    cy_engine_set_patch(&P);
    cap_reset();
    cy_engine_telemetry_get(&T);
    last_seq = T.seq;
    drops = strikes = slips = touches = 0;
    ndrop_t = 0;
    nsamp = 0;
}

/* ----------------------------------------------------------------- analysis --- */
static double rms(const float *x, size_t a, size_t b)
{
    double s = 0.0;
    size_t i;
    for (i = a; i < b; i++)
        s += (double)x[i] * x[i];
    return b > a ? sqrt(s / (double)(b - a)) : 0.0;
}
static double peak(const float *x, size_t a, size_t b)
{
    double p = 0.0;
    size_t i;
    for (i = a; i < b; i++)
        p = fmax(p, fabs(x[i]));
    return p;
}
static double db(double a) { return 20.0 * log10(a + 1e-30); }
static int32_t peak_q32(const int32_t *x, size_t n)     /* (v1.1.1) the largest |q| of an int32 output */
{
    int32_t p = 0;
    size_t i;
    for (i = 0; i < n; i++)
        p = x[i] > p ? x[i] : -x[i] > p ? -x[i] : p;
    return p;
}
/* Hann-windowed DFT magnitude at f over x[a..b) */
static double dft_mag(const float *x, size_t a, size_t b, double f)
{
    double re = 0.0, im = 0.0, w = 2.0 * T_PI * f / FS;
    size_t i, n = b - a;
    for (i = 0; i < n; i++) {
        double h = 0.5 - 0.5 * cos(2.0 * T_PI * i / (n - 1)), v = x[a + i] * h;
        re += v * cos(w * i), im -= v * sin(w * i);
    }
    return sqrt(re * re + im * im);
}
/* the spectral peak's frequency near f0 (+-rel), golden-section on the windowed DFT */
static double peak_freq(const float *x, size_t a, size_t b, double f0, double rel)
{
    double lo0 = f0 * (1.0 - rel), step = 2.0 * f0 * rel / 64.0, lo, hi, g = 0.6180339887, c, d;
    int i, best = 0;
    double bm = -1.0;
    for (i = 0; i <= 64; i++) {          /* coarse scan first, then golden section around the best bin */
        double m = dft_mag(x, a, b, lo0 + step * i);
        if (m > bm)
            bm = m, best = i;
    }
    lo = lo0 + step * (best - 1), hi = lo0 + step * (best + 1);
    c = hi - g * (hi - lo), d = lo + g * (hi - lo);
    for (i = 0; i < 40; i++) {
        if (dft_mag(x, a, b, c) > dft_mag(x, a, b, d))
            hi = d;
        else
            lo = c;
        c = hi - g * (hi - lo), d = lo + g * (hi - lo);
    }
    return 0.5 * (lo + hi);
}
/* spec §8.3 click metric: HP(8 kHz) envelope in 1 ms frames vs its running median over the previous 40 ms; returns
 * the largest excess in dB over frames [a, b). Frames more than 50 dB under the window's program level are ignored
 * and the median is floored there: the bus here has no reverb to smooth it, and the 8 kHz+ residue of a decaying
 * chord jitters +-5..7 dB from frame to frame by beating alone (measured: 65-80 dB under the program, with no event
 * visible in the envelope), masked and far below anything audible; a real click rises far above it (the hard-cut
 * self-check in t_steal proves the detector still sees one, with margin) */
static double click_db(const float *x, size_t n, size_t a, size_t b)
{
    static double env[44100 * 100 / 44 + 8];
    double b0, b1, b2, a1, a2, x1 = 0, x2 = 0, y1 = 0, y2 = 0, worst = -200.0, floor_db;
    double w0 = 2.0 * T_PI * 8000.0 / FS, al = sin(w0) / (2.0 * 0.70710678), cw = cos(w0), a0 = 1.0 + al;
    size_t i, nf = n / 44, f;
    b0 = (1.0 + cw) / 2.0 / a0, b1 = -(1.0 + cw) / a0, b2 = (1.0 + cw) / 2.0 / a0, a1 = -2.0 * cw / a0, a2 = (1.0 - al) / a0;
    for (f = 0; f < nf; f++) {
        double s = 0.0;
        for (i = f * 44; i < f * 44 + 44; i++) {
            double y = b0 * x[i] + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1, x1 = x[i], y2 = y1, y1 = y;
            s += y * y;
        }
        env[f] = 10.0 * log10(s / 44.0 + 1e-30);
    }
    {   /* the floor: 50 dB under the window's broadband level */
        double s = 0.0;
        for (i = a; i < b && i < n; i++)
            s += (double)x[i] * x[i];
        floor_db = 10.0 * log10(s / (double)(b - a) + 1e-30) - 50.0;
    }
    for (f = a / 44; f < b / 44 && f < nf; f++) {
        double t[40], m;
        int k, c = 0, j;
        if (f < 40 || env[f] < floor_db)
            continue;
        for (k = 1; k <= 40; k++)
            t[c++] = env[f - k];
        for (k = 1; k < c; k++)
            for (j = k; j > 0 && t[j] < t[j - 1]; j--) {
                double q = t[j];
                t[j] = t[j - 1], t[j - 1] = q;
            }
        m = 0.5 * (t[19] + t[20]);
        if (m < floor_db)
            m = floor_db;
        worst = fmax(worst, env[f] - m);
    }
    return worst;
}
/* the HP(8 kHz) envelope's loudest 1 ms frame over [a, b), dBFS: for comparing a render with an event against the
 * same render without it (a click can only add HF energy; a relative excess can rise just because the event made
 * the background quieter, as a steal does by removing a voice) */
static double hp_peak_db(const float *x, size_t n, size_t a, size_t b)
{
    double b0, b1, b2, a1, a2, x1 = 0, x2 = 0, y1 = 0, y2 = 0, pk = -300.0, s = 0.0;
    double w0 = 2.0 * T_PI * 8000.0 / FS, al = sin(w0) / (2.0 * 0.70710678), cw = cos(w0), a0 = 1.0 + al;
    size_t i;
    b0 = (1.0 + cw) / 2.0 / a0, b1 = -(1.0 + cw) / a0, b2 = (1.0 + cw) / 2.0 / a0, a1 = -2.0 * cw / a0, a2 = (1.0 - al) / a0;
    for (i = 0; i < b && i < n; i++) {
        double y = b0 * x[i] + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1, x1 = x[i], y2 = y1, y1 = y;
        if (i >= a) {
            s += y * y;
            if ((i - a) % 44u == 43u) {
                pk = fmax(pk, 10.0 * log10(s / 44.0 + 1e-30));
                s = 0.0;
            }
        }
    }
    return pk;
}
/* golden.py's spec §8.3.2 measure: energy above 0.45 fs over all of it, a Welch PSD (Blackman-Harris, 8192, half
 * overlap) of one channel; a radix-2 FFT in double (host test code) */
static void fft(double *re, double *im, int n)
{
    int i, j, k, m;
    for (i = 1, j = 0; i < n; i++) {
        int b = n >> 1;
        for (; j & b; b >>= 1)
            j ^= b;
        j ^= b;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (m = 2; m <= n; m <<= 1)
        for (i = 0; i < n; i += m)
            for (k = 0; k < m / 2; k++) {
                double a = -2.0 * T_PI * k / m, wr = cos(a), wi = sin(a);
                double xr = re[i + k + m / 2] * wr - im[i + k + m / 2] * wi, xi = re[i + k + m / 2] * wi + im[i + k + m / 2] * wr;
                re[i + k + m / 2] = re[i + k] - xr, im[i + k + m / 2] = im[i + k] - xi;
                re[i + k] += xr, im[i + k] += xi;
            }
}
static double ultra_db(const float *x, size_t n)
{
    static double re[8192], im[8192], w[8192];
    double tot = 0.0, hi = 0.0;
    size_t s;
    int i;
    for (i = 0; i < 8192; i++) {
        double t = 2.0 * T_PI * i / 8192.0;
        w[i] = 0.35875 - 0.48829 * cos(t) + 0.14128 * cos(2.0 * t) - 0.01168 * cos(3.0 * t);
    }
    for (s = 0; s + 8192 <= n; s += 4096) {
        for (i = 0; i < 8192; i++)
            re[i] = x[s + i] * w[i], im[i] = 0.0;
        fft(re, im, 8192);
        for (i = 0; i <= 4096; i++) {
            double p = re[i] * re[i] + im[i] * im[i];
            tot += p;
            if (i * FS / 8192.0 >= 0.45 * FS)
                hi += p;
        }
    }
    return tot > 0.0 ? 10.0 * log10(hi / tot + 1e-300) : -300.0;
}
static void wav24(const char *name, const float *L, const float *R, size_t a, size_t b, double gain)
{
    char path[1024];
    FILE *f;
    uint32_t n = (uint32_t)(b - a), v;
    uint16_t s;
    size_t i;
    if (!ev_dir)
        return;
    snprintf(path, sizeof path, "%s/%s", ev_dir, name);
    if (!(f = fopen(path, "wb")))
        return;
    fwrite("RIFF", 1, 4, f), v = 36u + n * 6u, fwrite(&v, 4, 1, f), fwrite("WAVEfmt ", 1, 8, f);
    v = 16, fwrite(&v, 4, 1, f), s = 1, fwrite(&s, 2, 1, f), s = 2, fwrite(&s, 2, 1, f);
    v = 44100, fwrite(&v, 4, 1, f), v = 44100u * 6u, fwrite(&v, 4, 1, f), s = 6, fwrite(&s, 2, 1, f);
    s = 24, fwrite(&s, 2, 1, f), fwrite("data", 1, 4, f), v = n * 6u, fwrite(&v, 4, 1, f);
    for (i = a; i < b; i++) {
        int c;
        for (c = 0; c < 2; c++) {
            double x = (c ? R[i] : L[i]) * gain * 8388607.0;
            int32_t q = (int32_t)lrint(fmax(-8388607.0, fmin(8388607.0, x)));
            uint8_t t[3] = {(uint8_t)q, (uint8_t)(q >> 8), (uint8_t)(q >> 16)};
            fwrite(t, 1, 3, f);
        }
    }
    fclose(f);
}
static int live_voices(void)
{
    int i, v = 0;
    for (i = 0; i < CY_NVOICES; i++)
        v += T.voice_state[i] != CY_VS_FREE;
    return v;
}

/* ==================================================================== tests === */
static void t_sine_onset(void)
{
    int k, i, n0, nn;
    double worst = 0.0, pk = 0.0, y[CY_BLOCK], xin[CY_BLOCK], tau, h[4] = {0, 0, 0, 0};
    SAY("strike onset: the contact pulse through the kernel, against its closed form\n");
    patch_default();
    P.noise = 0.0f;
    geom_set(SET_PLATE, 0.62, 0.40);
    eng_reset();
    run_ms(20);
    n0 = (int)capN;
    CHECK(peak(capL, 0, capN) == 0.0, "silence before the note is not exact zero");
    cy_engine_note_on(7, 60, 100);
    render1();
    /* the contact force the engine plays (engine.c cy_pulse): a unit-area half-sine of N = round(tau fs) samples,
     * scaled by (vel/127)^1.4, through the band-limiter (1 + z^-1)^4 / 16; then each mode is z' = (c + i s) z + b x */
    tau = 0.008 * pow(2.0, -5.0 * P.hardness) * pow(100.0 / 64.0, -0.2);
    nn = (int)(tau * FS + 0.5);
    for (i = 0; i < CY_BLOCK; i++) {
        double v = i < nn ? pow(100.0 / 127.0, 1.4) * sin(T_PI / (2.0 * nn)) * sin(T_PI * (i + 0.5) / nn) : 0.0;
        xin[i] = 0.0625 * (v + h[3]) + 0.25 * (h[0] + h[2]) + 0.375 * h[1];
        h[3] = h[2], h[2] = h[1], h[1] = h[0], h[0] = v;
    }
    memset(y, 0, sizeof y);
    {
        const cy_slot_t *sl = &cy_e.sl[0];
        for (k = 0; k < CY_NMODES; k++) {
            double zr = 0.0, zi = 0.0, c = sl->c[k], sn = sl->s[k], g = sl->gl[k], b = sl->phi[k];
            if (!(sl->act >> k & 1u))
                continue;
            for (i = 0; i < CY_BLOCK; i++) {
                double t = c * zr - sn * zi + b * xin[i];
                zi = sn * zr + c * zi, zr = t;
                y[i] += g * zi;
            }
        }
    }
    for (i = 0; i < CY_BLOCK; i++) {
        worst = fmax(worst, fabs(y[i] - capL[n0 + i]));
        pk = fmax(pk, fabs(y[i]));
    }
    CHECK(nn > 4 && worst <= 1e-4 * pk + 1e-9, "onset differs from the closed form by %.3g (peak %.3g, N %d)", worst,
          pk, nn);
    CHECK(fabs(capL[n0]) <= 1e-3 * pk && fabs(capL[n0 + 1]) <= 1e-2 * pk, "onset not smooth: %.3g %.3g (peak %.3g)",
          capL[n0], capL[n0 + 1], pk);
    SAY("  first block matches the contact pulse's closed form to %.2g of peak (N = %d samples); y[0..2] = %.2g %.2g "
        "%.2g of the block peak: a t^3 onset\n", worst / pk, nn, capL[n0] / pk, capL[n0 + 1] / pk, capL[n0 + 2] / pk);
}

static void t_tuning_ratios(void)
{
    double f0 = 440.0 * pow(2.0, (48 - 69) / 12.0), worst = 0.0;
    int k;
    SAY("tuning and ratio accuracy (Bessel set, C3)\n");
    patch_default();
    P.decay_mul = 10.0f, P.noise = 0.0f, P.hardness = 0.6f;
    geom_set(SET_BESSEL, 0.62, 0.40);
    eng_reset();
    cy_engine_note_on(-1, 48, 110);
    run_ms(3000);
    for (k = 0; k < 8; k++) {                    /* the engine's accuracy against the object it was handed */
        const cy_object_t *o = cy_geom_object();
        double fe = f0 * o->ratio[o->order[k]], fm = peak_freq(capL, 2205, 2205 + 88200, fe, 0.004), gap = 1.0;
        double err = fabs(fm / fe - 1.0) * 100.0, eb = fabs(fm / (f0 * BESSEL[k]) - 1.0) * 100.0;
        int j;
        for (j = 0; j < CY_NMODES; j++)      /* a SHIMMER twin within 1 % cannot be resolved in 2 s: report only */
            if (j != o->order[k])
                gap = fmin(gap, fabs(o->ratio[j] / o->ratio[o->order[k]] - 1.0));
        if (gap > 0.01)
            worst = fmax(worst, err);
        SAY("  mode %d: expected %.3f Hz, measured %.3f Hz (%.4f%%%s; %.2f%% from Bessel)\n", k, fe, fm, err,
            gap > 0.01 ? "" : ", twin", eb);
    }
    CHECK(worst < 0.02, "ratio / tuning error %.4f%% > 0.02%%", worst);
    wav24("bessel_c3.wav", capL, capR, 0, capN, 4.0);
}

static void t_keyup_click(void)
{
    size_t a;
    double c_db, dbefore = 0, dafter = 0;
    size_t i;
    SAY("key-up with full damper: no step\n");
    patch_default();
    P.noise = 0.0f, P.hardness = 0.1f, P.damper = 1.0f;
    geom_set(SET_PLATE, 0.62, 0.40);
    eng_reset();
    cy_engine_note_on(5, 46, 100);
    run_ms(400);
    a = capN;
    cy_engine_note_off(5, 46);
    run_ms(400);
    for (i = a - 2205; i < a; i++)       /* the largest step of the 50 ms before (several periods) */
        dbefore = fmax(dbefore, fabs(capL[i] - capL[i - 1]));
    for (i = a; i < a + 441; i++)
        dafter = fmax(dafter, fabs(capL[i] - capL[i - 1]));
    c_db = click_db(capL, capN, a - 441, a + 4410);
    CHECK(dafter <= 1.1 * dbefore, "max |dy| after key-up %.3g > before %.3g", dafter, dbefore);
    CHECK(c_db < 6.0, "key-up HP(8k) spike %.1f dB", c_db);
    CHECK(rms(capL, capN - 4410, capN) < 0.01 * rms(capL, a - 4410, a), "damper 100%% did not stop the note");
    SAY("  max|dy| before %.3g after %.3g; HP spike %.1f dB; tail %.1f dB under the held level\n", dbefore, dafter,
        c_db, db(rms(capL, capN - 4410, capN) / rms(capL, a - 4410, a)));
}

static void t_budget(void)
{
    static const int notes[8] = {48, 52, 55, 59, 62, 65, 69, 72};
    int i, k, minm = 99, maxm = 0;
    double c_db;
    size_t a;
    SAY("mode budget\n");
    patch_default();
    P.noise = 0.0f, P.hardness = 0.2f, P.decay_mul = 4.0f;
    geom_set(SET_PLATE, 0.62, 0.40);
    eng_reset();
    cy_engine_set_mode_budget(64);
    for (i = 0; i < 8; i++) {
        cy_engine_note_on(i, notes[i], 100);
        run_ms(30);
    }
    run_ms(100);
    for (i = 0; i < CY_NVOICES; i++) {
        int c = (int)cy_popcount(cy_e.sl[i].act);
        minm = c < minm ? c : minm, maxm = c > maxm ? c : maxm;
    }
    CHECK(T.modes_used <= 64, "budget 64: %u modes used", T.modes_used);
    CHECK(live_voices() == 8, "budget squeezed out voices: %d live", live_voices());
    CHECK(minm >= 6, "a voice kept only %d modes at budget 64 x 8 voices", minm);
    SAY("  budget 64, 8 voices: %u modes used, per voice %d..%d\n", T.modes_used, minm, maxm);
    a = capN;
    cy_engine_set_mode_budget(48);   /* a governor drop mid-chord: the highest modes fade out over one render */
    run_ms(200);
    c_db = click_db(capL, capN, a - 2205, a + 4410);
    CHECK(T.modes_used <= 48, "budget 48: %u used", T.modes_used);
    {   /* the same chord with no drop: the drop may add nothing to the chord's own HF jitter */
        double c_ref;
        uint32_t used48 = T.modes_used;
        eng_reset();
        cy_engine_set_mode_budget(64);
        for (i = 0; i < 8; i++) {
            cy_engine_note_on(i, notes[i], 100);
            run_ms(30);
        }
        run_ms(100);
        run_ms(200);
        c_ref = click_db(capL, capN, a - 2205, a + 4410);
        CHECK(c_db < 6.0 || c_db <= c_ref + 1.0, "budget drop adds a transient: HP spike %.1f dB vs %.1f dB without",
              c_db, c_ref);
        SAY("  drop to 48: %u used, HP spike %.1f dB (the chord without a drop: %.1f dB)\n", used48, c_db, c_ref);
    }
    eng_reset();
    cy_engine_set_mode_budget(48);
    cy_engine_set_mode_budget(128);
    for (i = 0; i < 8; i++)
        cy_engine_note_off(i, notes[i]);
    run_ms(100);
    for (k = 0; k < 2; k++)
        cy_engine_note_on(10 + k, 60 + 7 * k, 100);
    run_ms(50);
    CHECK(budget_bad == 0, "modes_used exceeded the budget in %u renders", budget_bad);
}

static void t_steal(void)
{
    static const int notes[5] = {48, 55, 60, 64, 67};
    int i, pass;
    size_t a = 0;
    double c_db = 0, c_ref = 0, c_cut = 0, p_db = 0, p_ref = 0, p_cut = 0;
    SAY("voice stealing\n");
    for (pass = 0; pass < 3; pass++) {           /* 0: steal; 1: the same notes with room (no steal); 2: a hard cut */
        patch_default();
        P.noise = 0.0f, P.hardness = 0.3f, P.decay_mul = 3.0f, P.max_voices = pass == 1 ? 8 : 4;
        geom_set(SET_PLATE, 0.62, 0.40);
        eng_reset();
        for (i = 0; i < 4; i++) {
            cy_engine_note_on(i, notes[i], 90);
            run_ms(120);
        }
        a = capN;
        cy_engine_note_on(4, notes[4], 90);
        render1();
        if (pass == 0) {
            int fb = 0;
            CHECK(cy_e.sl[CY_SFADE].fading, "the 5th note did not steal into the fade slot");
            CHECK(live_voices() == 4, "%d voices live with max 4", live_voices());
            while (cy_e.sl[CY_SFADE].fading && fb < 20)
                render1(), fb++;
            CHECK(fb >= 1 && fb <= 2, "the steal fade lasted %d more renders (5 ms = ~1.7)", fb);
        }
        if (pass == 2) {
            cy_e.sl[CY_SFADE].fading = 0;
            cy_slot_clear(&cy_e.sl[CY_SFADE]);
        }
        run_ms(200);
        if (pass == 0) {
            c_db = click_db(capL, capN, a - 2205, a + 4410);
            p_db = hp_peak_db(capL, capN, a - 441, a + 4410);
            wav24("steal.wav", capL, capR, 0, capN, 4.0);
        } else if (pass == 1) {
            c_ref = click_db(capL, capN, a - 2205, a + 4410);
            p_ref = hp_peak_db(capL, capN, a - 441, a + 4410);
        } else {
            c_cut = click_db(capL, capN, a - 2205, a + 4410);
            p_cut = hp_peak_db(capL, capN, a - 441, a + 4410);
        }
    }
    /* the new note's own onset is in the window: the steal may add no HF energy over the same onset without one */
    CHECK(p_db <= p_ref + 1.0, "steal adds HF: HP peak %.1f dBFS vs %.1f dBFS with no steal", p_db, p_ref);
    CHECK(c_db < 6.0 || c_db <= c_ref + 1.0, "steal clicks: HP spike %.1f dB (%.1f dB with no steal)", c_db, c_ref);
    {   /* the detectors work: a one-sample click of 10 % of the window's peak spliced into the steal render */
        static float tmp[44100];
        size_t j;
        double pk = peak(capL, a - 2205, a + 4410), p2, c2;
        eng_reset();                             /* (re-render pass 0 into the capture) */
        patch_default();
        P.noise = 0.0f, P.hardness = 0.3f, P.decay_mul = 3.0f, P.max_voices = 4;
        cy_engine_set_patch(&P);
        for (i = 0; i < 4; i++) {
            cy_engine_note_on(i, notes[i], 90);
            run_ms(120);
        }
        a = capN;
        cy_engine_note_on(4, notes[4], 90);
        run_ms(200);
        for (j = 0; j < capN && j < 44100; j++)
            tmp[j] = capL[j];
        tmp[a + 300] += (float)(0.1 * pk);
        p2 = hp_peak_db(tmp, capN, a - 441, a + 4410);
        c2 = click_db(tmp, capN, a - 2205, a + 4410);
        CHECK(p2 > p_db + 6.0 && c2 > 6.0, "the detectors miss a 10 %% click: HF %.1f vs %.1f dBFS, spike %.1f dB",
              p2, p_db, c2);
        SAY("  detector self-check: a 10 %% one-sample click reads %.1f dBFS HF (steal %.1f) and a %.1f dB spike\n", p2,
            p_db, c2);
    }
    /* and under a new note with no strike (a bow swelling in), against the same bow onset with no steal */
    {
        double c_bow = 0.0, c_bow0 = 0.0, q0 = 0.0, q1 = 0.0;
        for (pass = 0; pass < 2; pass++) {
            patch_default();
            P.noise = 0.0f, P.hardness = 0.3f, P.decay_mul = 3.0f, P.max_voices = pass ? 8 : 4;
            geom_set(SET_PLATE, 0.62, 0.40);
            eng_reset();
            for (i = 0; i < 4; i++) {
                cy_engine_note_on(i, notes[i], 90);
                run_ms(120);
            }
            P.exciter = CY_EXC_BOW;
            cy_engine_set_patch(&P);
            a = capN;
            cy_engine_note_on(4, notes[4], 90);
            run_ms(200);
            *(pass ? &c_bow0 : &c_bow) = click_db(capL, capN, a - 2205, a + 4410);
            *(pass ? &q0 : &q1) = hp_peak_db(capL, capN, a - 441, a + 4410);
        }
        /* (the relative spike is reported only: the bow's own slip catch ~30 ms in reads higher over a background a
         * steal has made quieter, which is no click; the HF energy says whether the steal added any) */
        CHECK(q1 <= q0 + 1.0, "steal under a bow onset adds HF: %.1f dBFS vs %.1f dBFS with no steal", q1, q0);
        SAY("  5th of 4 voices: HP spike %.1f dB, HF peak %.1f dBFS (without a steal %.1f dB, %.1f dBFS; a hard cut "
            "%.1f dB, %.1f dBFS); under a bow onset %.1f dB, %.1f dBFS (without a steal %.1f dB, %.1f dBFS)\n", c_db,
            p_db, c_ref, p_ref, c_cut, p_cut, c_bow, q1, c_bow0, q0);
    }
}

#ifdef CY_TEST_REAL_GEOM
#define BOW_U 0.12                       /* the bowed presets' edge (09 BOWED GLASS PAD: POS 12, 50) */
#define BOW_V 0.50
#define BOW_FLOOR -30.0
#else
#define BOW_U 0.62                       /* the default POS: the mock disc's edge excites all its modes alike */
#define BOW_V 0.40
#define BOW_FLOOR -55.0
#endif
static void t_bow_sweep(void)
{
    static const int notes[4] = {53, 60, 72, 79};
    static const float forces[5] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    static const int press[4] = {0, 32, 64, 127};
    int ni, fi, pi, gi;
    double worst_hi = -200.0, worst_lo = 200.0;
    uint32_t bis = 0, samples = 0, gov = 0;
    FILE *csv = 0;
    char path[1024];
    SAY("bow sweep: force x pressure x pitch (and grit 0 / 1)\n");
    if (ev_dir) {
        snprintf(path, sizeof path, "%s/bow_sweep.csv", ev_dir);
        csv = fopen(path, "w");
        if (csv)
            fprintf(csv, "grit,note,force,press,rms_db,peak_db,bisect_pct\n");
    }
    for (gi = 0; gi < 3; gi++)
        for (ni = 0; ni < 4; ni++)
            for (fi = 0; fi < 5; fi++)
                for (pi = 0; pi < 4; pi++) {
                    float grit = gi == 0 ? 0.4f : gi == 1 ? 0.0f : 1.0f;
                    double r, p;
                    uint32_t b0;
                    if (gi && (fi % 2 || pi % 2 == 0))
                        continue;            /* grit extremes on a sub-grid */
                    patch_default();
                    P.exciter = CY_EXC_BOW, P.force = forces[fi], P.grit = grit, P.decay_mul = 2.0f;
                    geom_set(SET_PLATE, BOW_U, BOW_V);
                    eng_reset();
                    cy_engine_note_on(7, notes[ni], 100);
                    cy_engine_pressure(7, notes[ni], press[pi]);
                    b0 = cy_e.sl[0].bisect;
                    run_ms(2000);
                    r = rms(capL, 44100, 88200), p = peak(capL, 0, capN);
                    bis += cy_e.sl[0].bisect - b0, samples += 88200;
                    {
                        int k;
                        int bad = 0;
                        for (k = 0; k < (int)capN; k++)
                            bad |= !isfinite(capL[k]);
                        CHECK(!bad && T.nan_resets == 0, "bow NaN at note %d force %.2f press %d", notes[ni],
                              forces[fi], press[pi]);
                    }
                    if (cy_e.sl[0].E > 4.0f * cy_e.sl[0].Etgt)
                        gov++;
                    CHECK(db(r) <= -3.0, "bow RMS %.1f dBFS > -3 (note %d force %.2f press %d grit %.1f)", db(r),
                          notes[ni], forces[fi], press[pi], grit);
                    worst_hi = fmax(worst_hi, db(r));
                    if (forces[fi] >= 0.5f && press[pi] >= 64) {
                        /* the spec's floor (§8.2 bow_sweep) is end to end, on the real plates; the mock disc couples
                         * the bow differently, so there it only has to speak */
                        CHECK(db(r) >= BOW_FLOOR, "bow RMS %.1f dBFS < %.0f (note %d force %.2f press %d grit %.1f)",
                              db(r), BOW_FLOOR, notes[ni], forces[fi], press[pi], grit);
                        worst_lo = fmin(worst_lo, db(r));
                    }
                    if (csv)
                        fprintf(csv, "%.1f,%d,%.2f,%d,%.2f,%.2f,%.3f\n", grit, notes[ni], forces[fi], press[pi], db(r),
                                db(p), 100.0 * (cy_e.sl[0].bisect - b0) / 88200.0);
                    if (gi == 0 && fi == 3 && pi == 3 && ev_dir) {
                        char nm[64];
                        snprintf(nm, sizeof nm, "bow_n%d.wav", notes[ni]);
                        wav24(nm, capL, capR, 0, capN, 2.0);
                    }
                }
    if (csv)
        fclose(csv);
    SAY("  RMS range over the grid: loudest %.1f dBFS; quietest (force>=.5, P>=64) %.1f dBFS; bisection on %.2f%% of "
        "samples; governor active at the end of %u runs\n", worst_hi, worst_lo, 100.0 * bis / samples, gov);
}

static void t_bow_release_and_latch(void)
{
    size_t a;
    double c_db, held, after;
    SAY("bow lift, latch and exciter switch\n");
    patch_default();
    P.exciter = CY_EXC_BOW, P.force = 0.75f;
    geom_set(SET_PLATE, 0.12, 0.50);
    eng_reset();
    cy_engine_note_on(7, 60, 100);
    cy_engine_pressure(7, 60, 100);
    run_ms(1500);
    a = capN;
    held = rms(capL, a - 22050, a);
    cy_engine_note_off(7, 60);
    run_ms(1500);
    c_db = click_db(capL, capN, a - 2205, a + 8820);
    CHECK(c_db < 6.0, "bow lift clicked: %.1f dB", c_db);
    CHECK(cy_e.sl[0].bowing == 0, "bow did not lift after key-up");
    SAY("  lift: HP spike %.1f dB\n", c_db);
    /* latch: the bow keeps going at its last pressure after key-up */
    eng_reset();
    cy_engine_latch(1);
    cy_engine_note_on(7, 60, 100);
    cy_engine_pressure(7, 60, 100);
    run_ms(1500);
    cy_engine_note_off(7, 60);
    run_ms(1500);
    after = rms(capL, capN - 22050, capN);
    CHECK(cy_e.sl[0].bowing && T.latched, "latched bow stopped");
    CHECK(after > 0.3 * held, "latched bow lost level: %.1f dB vs %.1f dB held", db(after), db(held));
    cy_engine_latch(0);
    run_ms(1500);
    CHECK(!cy_e.sl[0].bowing, "latch off did not release the bow");
    /* ALGORITHM turned while bowing: it stops bowing and rings out */
    eng_reset();
    cy_engine_note_on(7, 60, 100);
    cy_engine_pressure(7, 60, 100);
    run_ms(800);
    P.exciter = CY_EXC_MALLET;
    cy_engine_set_patch(&P);
    a = capN;
    run_ms(200);                                 /* the bow lifts through its 15 ms gate */
    CHECK(!cy_e.sl[0].bowing && T.voice_state[0] == CY_VS_RING, "exciter change: still bowing (state %d)",
          T.voice_state[0]);
    c_db = click_db(capL, capN, a - 2205, a + 8820);
    CHECK(c_db < 6.0, "exciter change under a bow clicked: %.1f dB", c_db);
    SAY("  latch held %.1f dB vs %.1f dB; exciter switch rings out\n", db(after), db(held));
}

static void t_rain_stats(void)
{
    static const float dens[3] = {0.0f, 0.4f, 1.0f};
    static const int press[3] = {0, 64, 127};
    int i;
    SAY("RAIN density statistics\n");
    for (i = 0; i < 3; i++) {
        double secs = 20.0, rate, exp_n, cv, m = 0, v = 0, peff;
        uint32_t k, n0, t0;
        patch_default();
        P.exciter = CY_EXC_RAIN, P.density = dens[i], P.swell_s = 0.005f;
        geom_set(SET_PLATE, 0.62, 0.40);
        eng_reset();
        cy_engine_note_on(7, 60, 100);
        cy_engine_pressure(7, 60, press[i]);
        run_ms(500);
        n0 = drops, t0 = ndrop_t;
        run_ms(secs * 1000.0);
        peff = fmin(1.0, 0.4 * 100.0 / 127.0 + press[i] / 127.0);
        rate = 2.0 + 120.0 * dens[i] * dens[i] * (0.3 + 0.7 * peff);
        exp_n = rate * secs;
        for (k = t0 + 1; k < ndrop_t; k++)
            m += (double)(drop_t[k] - drop_t[k - 1]);
        m /= (double)(ndrop_t - t0 - 1);
        for (k = t0 + 1; k < ndrop_t; k++)
            v += pow((double)(drop_t[k] - drop_t[k - 1]) - m, 2.0);
        cv = sqrt(v / (double)(ndrop_t - t0 - 2)) / m;
        CHECK(fabs((drops - n0) - exp_n) <= 4.0 * sqrt(exp_n) + 2.0, "density %.1f: %u drops in %.0f s, expected %.1f",
              dens[i], drops - n0, secs, exp_n);
        CHECK(fabs(cv - 1.0) < 0.2, "density %.1f: inter-arrival CV %.2f (Poisson: 1)", dens[i], cv);
        SAY("  density %.1f press %d: %u drops / %.0f s (expected %.1f, rate %.1f Hz), inter-arrival CV %.2f\n",
            dens[i], press[i], drops - n0, secs, exp_n, rate, cv);
        if (i == 2)
            wav24("rain_dense.wav", capL, capR, 0, capN < 44100 * 6 ? capN : 44100 * 6, 4.0);
        cy_engine_note_off(7, 60);
        n0 = drops;
        run_ms(1000);
        CHECK(drops == n0, "drops continued after key-up");
    }
}

/* (integration, M7) an excited sub-block must not apply a stale gain-ramp step: dgl/dgr keep the last ramp's
 * value, and the EXCITED kernel once used them whether or not the gains were ramping, so every drop after a LEVEL
 * move drifted the gains with nothing to pull them back (demo 04's rain grew a mode to +130 dBFS on the bus). Every
 * live mode's gain must stay at a * mic_k, a = out_norm * level * trim, through dense rain after level changes. */
static void t_gain_drift(void)
{
    int i, j, k, bad = 0;
    double worst = 0.0;
    float lv[4] = {-6.0f, 3.0f, -2.0f, 0.0f};
    SAY("gains through excitation after a ramp (stale step)\n");
    {   /* the kernel itself: an EXCITED mode whose gains are not ramping keeps them, whatever dgl/dgr hold */
        static cy_slot_t ks;
        static float kx[CY_CR], kL[CY_CR], kR[CY_CR];
        memset(&ks, 0, sizeof ks);
        ks.act = ks.run = ks.exc = 1u, ks.ramp = 0u, ks.glm = 0u;
        ks.c[0] = 0.99f, ks.s[0] = 0.1f, ks.b[0] = 0.5f, ks.gl[0] = 0.25f, ks.gr[0] = -0.125f;
        ks.dgl[0] = 0.75f, ks.dgr[0] = -0.5f;    /* a stale step, as a finished kill fade leaves it */
        for (i = 0; i < (int)CY_CR; i++)
            kx[i] = i == 3 ? 1.0f : 0.0f;
        cy_modes_block(&ks, kL, kR, kx, CY_CR);
        CHECK(ks.gl[0] == 0.25f && ks.gr[0] == -0.125f, "EXCITED kernel applied a stale gain step: gl %.4g gr %.4g",
              ks.gl[0], ks.gr[0]);
        ks.ramp = 1u;                            /* and while they do ramp, it does apply it */
        cy_modes_block(&ks, kL, kR, kx, CY_CR);
        CHECK(fabsf(ks.gl[0] - (0.25f + 0.75f * (float)CY_CR)) < 1e-3f, "EXCITED kernel ignored its gain ramp: gl %.4g",
              ks.gl[0]);
    }
    patch_default();
    P.exciter = CY_EXC_RAIN, P.density = 1.0f, P.swell_s = 0.005f;
    geom_set(SET_PLATE, 0.62, 0.40);
    eng_reset();
    cy_engine_set_mode_budget(CY_BUDGET_MIN);   /* six rain voices on 48 modes: the budget kills and re-wakes modes
                                                 * all the time, and a kill's fade leaves a big step behind */
    for (i = 0; i < 6; i++)
        cy_engine_note_on(4 + 3 * i, 57 + 3 * i, 100), cy_engine_pressure(4 + 3 * i, 57 + 3 * i, 127);
    run_ms(300);
    for (j = 0; j < 4; j++) {
        P.level_db = lv[j];
        cy_engine_set_patch(&P);
        for (i = 0; i < 120; i++) {              /* ~350 ms a step: the level glides, then holds while it rains */
            render1();
            for (k = 0; k < CY_NVOICES; k++) {
                const cy_slot_t *sl = &cy_e.sl[k];
                float a = cy_e.onorm * cy_e.lev * CY_OUT_TRIM * sl->trim;
                int m;
                if (sl->state == CY_VS_FREE || sl->fading || sl->ramp)
                    continue;
                for (m = 0; m < CY_NMODES; m++) {
                    double want = fabs((double)(a * cy_e.obj->mic_l[m])), got = fabs((double)sl->gl[m]);
                    if (!(sl->act >> m & 1u) || (sl->kill >> m & 1u) || sl->w[m] > CY_EDGE_A)
                        continue;
                    if (got > 1.02 * want + 1e-6)
                        bad++;
                    if (want > 1e-6 && got / want > worst)
                        worst = got / want;
                }
            }
        }
    }
    CHECK(bad == 0, "%d mode-renders with a gain past its target (worst %.3gx): a stale ramp step drifted it", bad,
          worst);
    CHECK(peak(capL, 0, capN) < 1.0, "rain after LEVEL moves peaked at %.2f on the bus", peak(capL, 0, capN));
    SAY("  worst gain / target %.4f over 4 LEVEL moves under dense rain, 6 voices on a 48-mode budget\n", worst);
    for (i = 0; i < 6; i++)
        cy_engine_note_off(4 + 3 * i, 57 + 3 * i);
    cy_engine_set_mode_budget(CY_BUDGET_DEFAULT);
}

static void t_freeze(void)
{
    double e0, e1, emax = 0;
    int i;
    SAY("freeze\n");
    patch_default();
    P.decay_mul = 1.0f;
    geom_set(SET_PLATE, 0.62, 0.40);
    eng_reset();
    cy_engine_note_on(0, 48, 100), cy_engine_note_on(4, 52, 100), cy_engine_note_on(7, 55, 100);
    run_ms(300);
    cy_engine_freeze(1);
    run_ms(50);
    e0 = cy_e.sl[0].E + cy_e.sl[1].E + cy_e.sl[2].E;
    for (i = 0; i < 20; i++) {
        run_ms(1000);
        emax = fmax(emax, cy_e.sl[0].E + cy_e.sl[1].E + cy_e.sl[2].E);
    }
    e1 = cy_e.sl[0].E + cy_e.sl[1].E + cy_e.sl[2].E;
    CHECK(T.frozen == 1 && T.voice_state[0] == CY_VS_FROZEN, "not frozen (state %d)", T.voice_state[0]);
    CHECK(emax <= 2.0 * e0 + 1e-12, "frozen energy grew to %.2fx", emax / e0);
    CHECK(e1 >= 0.5 * e0, "frozen energy fell to %.2fx in 20 s", e1 / e0);
    cy_engine_note_off(0, 48), cy_engine_note_off(4, 52), cy_engine_note_off(7, 55);
    run_ms(100);
    CHECK(T.voice_state[0] == CY_VS_FROZEN, "a key-up unfroze a frozen slot");
    cy_engine_freeze(0);
    run_ms(3000);
    CHECK(cy_e.sl[0].E < 0.1 * e1, "unfrozen slots did not decay");
    SAY("  20 s frozen: energy %.3fx..%.3fx of the start; released and decayed\n", e1 / e0, emax / e0);
}

static void t_surface(void)
{
    int k, dark = 0, lit = 0, j, seen = 0, bd = 0, bl = 0;
    double c_ant, c_node;
    SAY("SURFACE: key map and touch harmonics\n");
    patch_default();
    P.surface = 1, P.decay_mul = 3.0f;
    geom_set(SET_PLATE, 0.62, 0.40);
    eng_reset();
    /* the M8 scenario: strikes, then light touches (vel 10, press 30); the touched keys and every key on the
     * surviving modes' nodal lines go dark while the rest stay lit */
    cy_engine_note_on(12, 65, 110);
    run_ms(150);
    cy_engine_note_on(3, 56, 10);
    cy_engine_pressure(3, 56, 30);
    cy_engine_note_on(21, 74, 10);
    cy_engine_pressure(21, 74, 30);
    for (j = 0; j < 80; j++) {
        run_ms(25);
        dark = lit = 0;
        for (k = 0; k < CY_NKEYS; k++)
            dark += T.key_level[k] < 0.08f, lit += T.key_level[k] > 0.35f;
        if (dark >= 3 && lit >= 3 && !seen)
            seen = 1, bd = dark, bl = lit;
    }
    CHECK(seen, "no moment with >= 3 dark and >= 3 lit keys (last: dark %d lit %d)", dark, lit);
    CHECK(T.surface == 1, "telemetry surface flag");
    for (k = 0; k < CY_NKEYS; k++)
        CHECK(T.key_level[k] >= 0.0f && T.key_level[k] <= 1.0f, "key_level[%d] = %f", k, T.key_level[k]);
    SAY("  strike on key 12, touches on 3 and 21: %d dark and %d lit keys at once\n", bd, bl);
    cy_engine_note_off(3, 56), cy_engine_note_off(21, 74), cy_engine_note_off(12, 65);
    /* a light touch: modes with an antinode under the finger die, modes with a node there ring on */
    {
        int tk = 3, kn = -1, ka = -1;
        double en0[CY_NMODES], en1[CY_NMODES];
        eng_reset();
        cy_engine_note_on(20, 73, 110);
        run_ms(100);
        for (k = 0; k < CY_NMODES; k++)
            en0[k] = cy_e.sl[CY_SSURF].e[k];
        cy_engine_note_on(tk, 56, 10);           /* vel < 24: a touch */
        cy_engine_pressure(tk, 56, 30);
        run_ms(400);
        for (k = 0; k < CY_NMODES; k++)
            en1[k] = cy_e.sl[CY_SSURF].e[k];
        for (k = 0; k < CY_NMODES; k++) {
            float s = fabsf(cy_geom_object()->surface[tk][k]);
            if (en0[k] <= 0.0)
                continue;
            if (s < 0.08f && (kn < 0 || en0[k] > en0[kn]))
                kn = k;
            if (s > 0.6f && (ka < 0 || en0[k] > en0[ka]))
                ka = k;
        }
        CHECK(touches >= 1, "no TOUCH event");
        if (kn >= 0 && ka >= 0) {
            c_node = 10.0 * log10(en1[kn] / en0[kn]);
            c_ant = 10.0 * log10(en1[ka] / en0[ka] + 1e-30);
            CHECK(c_ant < c_node - 20.0, "touch: antinode mode fell %.1f dB, nodal mode %.1f dB", c_ant, c_node);
            SAY("  touch on key %d: nodal mode %d %.1f dB, antinode mode %d %.1f dB in 400 ms\n", tk, kn, c_node, ka,
                c_ant);
        }
        cy_engine_note_off(tk, 56), cy_engine_note_off(20, 73);
        run_ms(200);
    }
    wav24("surface.wav", capL, capR, 0, capN, 4.0);
}

static void t_nyquist(void)
{
    SAY("nothing above 0.45 fs\n");
    patch_default();
    P.stretch = 1.0f, P.decay_mul = 2.0f, P.hardness = 1.0f, P.noise = 1.0f;
    geom_set(SET_PLATE, 0.62, 0.40);
    eng_reset();
    nyq_bad = 0;
    cy_engine_note_on(7, 103, 127);              /* G5 + 2 octaves */
    cy_engine_pressure(7, 103, 127);
    cy_engine_bend(2.0f);
    run_ms(500);
    cy_engine_bend(0.0f);
    cy_engine_note_on(8, 96, 127);
    run_ms(500);
    CHECK(nyq_bad == 0, "%u mode-renders above 0.45 fs", nyq_bad);
    wav24("ultra_g7.wav", capL, capR, 0, capN, 4.0);
    SAY("  stretch + bend at G7: no active mode above 0.45 fs (spectrum in ultra_g7.wav)\n");
}

static void t_ultrasonic(void)
{
    static const struct { const char *name; int exc; float hard, noise, bloom, decay, vib, stretch; int press; } C[] = {
        {"hard mallet, NOISE 100", CY_EXC_MALLET, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0},
        {"soft mallet", CY_EXC_MALLET, 0.0f, 0.15f, 0.0f, 1.0f, 0.0f, 0.0f, 0},
        {"kick: BLOOM +85, DECAY x0.45", CY_EXC_MALLET, 0.7f, 0.15f, 0.85f, 0.45f, 0.0f, 0.0f, 0},
        {"gong: BLOOM -35, DECAY x3", CY_EXC_MALLET, 0.2f, 0.15f, -0.35f, 3.0f, 0.0f, 0.0f, 0},
        {"vibrato 50 c + STRETCH leaned", CY_EXC_MALLET, 0.55f, 0.15f, 0.0f, 1.0f, 50.0f, 1.0f, 127},
        {"rain, DENSITY 100, leaned", CY_EXC_RAIN, 0.55f, 0.15f, 0.0f, 1.0f, 0.0f, 0.0f, 127},
        {"bow, FORCE 75, leaned, vibrato", CY_EXC_BOW, 0.55f, 0.15f, 0.0f, 2.0f, 20.0f, 0.0f, 110},
    };
    int i;
    double worst = -300.0;
    SAY("nothing above 0.45 fs, by golden.py's measure (spec 8.3.2: <= -100 dB), G7 vel 127 on the bus\n");
    for (i = 0; i < (int)(sizeof C / sizeof C[0]); i++) {
        double u;
        patch_default();
        P.exciter = (uint8_t)C[i].exc, P.hardness = C[i].hard, P.noise = C[i].noise, P.bloom = C[i].bloom;
        P.decay_mul = C[i].decay, P.vibrato_cents = C[i].vib, P.stretch = C[i].stretch;
        P.density = 1.0f, P.force = 0.75f;
        geom_set(SET_PLATE, 0.62, 0.40);
        eng_reset();
        run_ms(200);
        cy_engine_note_on(19, 103, 127);         /* G5 + OCT+2 (key 19 senses pressure) */
        if (C[i].press)
            cy_engine_pressure(19, 103, C[i].press);
        if (i == 4)
            cy_engine_bend(2.0f);
        run_ms(1500);
        cy_engine_note_off(19, 103);
        cy_engine_bend(0.0f);
        run_ms(1500);
        u = ultra_db(capL, capN);
        worst = fmax(worst, u);
        CHECK(u <= -100.0, "%s: %.1f dB above 0.45 fs", C[i].name, u);
        SAY("  %-32s %7.1f dB\n", C[i].name, u);
    }
    SAY("  worst %.1f dB\n", worst);
}

static void t_morph_glide(void)
{
#ifndef CY_TEST_REAL_GEOM
    size_t a;
    double c_db, c_ref, p_m, p_ref;
    int i, pass, bow;
    SAY("object glide while sounding (SELECT spin: plate -> harmonic over 8 s at 60 Hz)\n");
    for (bow = 0; bow < 2; bow++)
        for (pass = 0; pass < 2; pass++) {   /* pass 0 morphs, pass 1 holds the same note unmorphed */
            patch_default();
            if (bow)
                P.exciter = CY_EXC_BOW, P.force = 0.6f;
            else
                P.decay_mul = 6.0f, P.hardness = 0.3f;
            geom_set(SET_PLATE, bow ? 0.12 : 0.62, bow ? 0.50 : 0.40);
            eng_reset();
            cy_engine_note_on(7, 57, 100);
            if (bow)
                cy_engine_pressure(7, 57, 90);
            run_ms(800);
            a = capN;
            for (i = 0; i <= 480; i++) {
                mmorph = pass ? 0.0 : i / 480.0;
                mock_build();
                run_ms(1000.0 / 60.0);
            }
            CHECK(T.nan_resets == 0, "NaN during morph");
            if (!pass) {
                c_db = click_db(capL, capN, a, capN);
                p_m = hp_peak_db(capL, capN, a, capN);
                wav24(bow ? "morph_bow.wav" : "morph_mallet.wav", capL, capR, 0, capN, 2.0);
            } else {
                c_ref = click_db(capL, capN, a, capN);
                p_ref = hp_peak_db(capL, capN, a, capN);
                if (!bow) {              /* a ringing struck note: deterministic, so the spec's no-click test applies */
                    CHECK(c_db < 6.0 || c_db <= c_ref + 1.0, "morph clicked under a ringing note: %.1f dB (%.1f held)",
                          c_db, c_ref);
                } else {                 /* a bowed note's slips are chaotic: the morph may add no HF energy */
                    CHECK(p_m <= p_ref + 3.0, "morph under a bow adds HF: %.1f dBFS (%.1f held)", p_m, p_ref);
                }
                SAY("  %s A3: worst HP spike %.1f dB, HF peak %.1f dBFS (held unmorphed: %.1f dB, %.1f dBFS)\n",
                    bow ? "bowed " : "struck", c_db, p_m, c_ref, p_ref);
            }
        }
    mmorph = 0.0;
    mock_build();
#endif
}

static void t_bloom(void)
{
    double fa, fb, f0 = 440.0 * pow(2.0, (36 - 69) / 12.0);
    int sgn;
    SAY("BLOOM pitch movement\n");
    for (sgn = 0; sgn < 2; sgn++) {
        patch_default();
        P.bloom = sgn ? -0.35f : 0.85f, P.decay_mul = 0.8f, P.noise = 0.0f;
        geom_set(SET_BESSEL, 0.5, 0.5);
        eng_reset();
        cy_engine_note_on(-1, 36, 120);
        run_ms(2500);
        fa = peak_freq(capL, 0, 4096, f0 * (1.0 + 0.06 * P.bloom), 0.08);
        fb = peak_freq(capL, 88200, 88200 + 16384, f0, 0.08);
        SAY("  bloom %+.2f: early %.2f Hz, late %.2f Hz (%.2f%%)\n", P.bloom, fa, fb, 100.0 * (fa / fb - 1.0));
        if (!sgn)
            CHECK(fa > fb * 1.02, "positive bloom: no pitch drop (%.2f -> %.2f)", fa, fb);
        else
            CHECK(fa < fb * 0.995, "negative bloom: no pitch rise (%.2f -> %.2f)", fa, fb);
    }
}

static void t_damp_panic_midi(void)
{
    size_t a;
    double l0, l1, c_db;
    SAY("DAMP, PANIC, MIDI pressure\n");
    patch_default();
    P.decay_mul = 4.0f, P.noise = 0.0f, P.hardness = 0.1f;
    geom_set(SET_PLATE, 0.62, 0.40);
    eng_reset();
    cy_engine_note_on(-1, 60, 100);
    run_ms(300);
    a = capN;
    l0 = rms(capL, a - 2205, a);
    cy_engine_damp(1);
    run_ms(160);
    l1 = rms(capL, capN - 2205, capN);
    CHECK(db(l1 / l0) < -60.0 * 0.11 / 0.08, "DAMP: only %.1f dB in 160 ms", db(l1 / l0));
    CHECK(T.damping == 1, "telemetry damping flag");
    cy_engine_damp(0);
    SAY("  DAMP: %.1f dB in 160 ms (T60 <= 80 ms)\n", db(l1 / l0));
    /* MIDI: key -1, poly pressure by note drives the slot's pressure (and so TENSION); a MIDI note never takes the
     * sensing keys' P_eff formula (fix-dsp: aftertouch only adds, see t_fixes) */
    eng_reset();
    cy_engine_note_on(-1, 64, 100);
    run_ms(50);
    CHECK(!cy_e.sl[0].has_at, "MIDI note on the sensing keys' P_eff formula");
    cy_engine_pressure(-1, 64, 127);
    run_ms(100);
    CHECK(!cy_e.sl[0].has_at && cy_e.sl[0].P > 0.9f, "MIDI poly AT not applied (P %.2f)", cy_e.sl[0].P);
    cy_engine_note_off(-1, 64);
    run_ms(20);
    CHECK(!cy_e.sl[0].down, "MIDI note-off by note failed");
    cy_engine_channel_pressure(0);
    /* PANIC: silent within a render and a half, without a click */
    eng_reset();
    cy_engine_note_on(1, 50, 120), cy_engine_note_on(2, 57, 120);
    cy_engine_freeze(1), cy_engine_latch(1);
    run_ms(300);
    a = capN;
    cy_engine_panic();
    run_ms(300);
    c_db = click_db(capL, capN, a - 2205, a + 4410);
    CHECK(peak(capL, a + 2 * CY_BLOCK, capN) == 0.0, "panic left sound (%.3g)", peak(capL, a + 2 * CY_BLOCK, capN));
    CHECK(T.frozen == 0 && T.latched == 0 && live_voices() == 0, "panic left state");
    SAY("  PANIC: exact silence after 2 renders, HP spike %.1f dB\n", c_db);
    (void)c_db;
}

/* (v1.1.1) cy_engine_restart, the boot logo's two hand-overs. What is heard fades out over CY_RESTART_FADE renders
 * as the old sound times a raised cosine (a patch set meanwhile is not heard in it); a hand-over (1) then waits silent,
 * its events held, for the next patch and takes it as a first one; and from there a note renders bit for bit as on a
 * fresh engine. A restart (0) takes the patch in force at once. The output read is lr, after the stub fx */
static void rs_run(int nb, int32_t *dst)
{
    int i, j;
    for (i = 0; i < nb; i++) {
        render1();
        for (j = 0; j < (int)CY_BLOCK; j++)
            dst[i * (int)CY_BLOCK + j] = blk[2 * j];
    }
}
static void t_restart(void)
{
    static int32_t u[16 * CY_BLOCK], f[16 * CY_BLOCK], ref[200 * CY_BLOCK], aft[200 * CY_BLOCK];
    cy_patch_t P2;
    double worst = 0.0, pk = 0.0;
    int i, diff = 0, held, ready;
    SAY("restart (v1.1.1: the boot logo's hand-overs)\n");
    patch_default();
    P.decay_mul = 4.0f, P.noise = 0.0f, P.hardness = 0.7f;
    geom_set(SET_PLATE, 0.62, 0.40);
    eng_reset();                                         /* the untouched: a loud note, 16 renders from 300 ms */
    cy_engine_note_on(-1, 60, 120);
    run_ms(300);
    rs_run(16, u);
    eng_reset();                                         /* the same, a hand-over there, another patch set at once */
    cy_engine_note_on(-1, 60, 120);
    run_ms(300);
    cy_engine_restart(1);
    P2 = P;
    P2.decay_mul = 0.2f, P2.level_db = 6.0f, P2.tension_st = -12.0f;
    cy_engine_set_patch(&P2);
    cy_engine_note_on(-1, 64, 100);                      /* (held behind the restart) */
    rs_run(16, f);
    for (i = 0; i < 16 * (int)CY_BLOCK; i++) {
        int j = i + 1;
        double g = j >= 3 * (int)CY_BLOCK ? 0.0 : 0.5 + 0.5 * cos(T_PI * j / (3.0 * CY_BLOCK));
        worst = fmax(worst, fabs((double)f[i] - (double)u[i] * g));
        pk = fmax(pk, fabs((double)u[i]));
    }
    held = live_voices();
    ready = cy_engine_restart_ready();
    CHECK(worst <= 2.0, "restart fade: %.0f LSB off the old sound x the raised cosine", worst);
    CHECK(ready && held == 0, "hand-over: not waiting silent after the fade (ready %d, voices %d)", ready, held);
    cy_engine_set_patch(&P);                             /* the hand-over's patch: the note goes with it */
    rs_run(200, aft);
    CHECK(!cy_engine_restart_ready() && cy_e.decay == P.decay_mul, "hand-over: the patch not taken as a first one");
    eng_reset();                                         /* the reference: a fresh engine, the note at render 0 */
    cy_engine_note_on(-1, 64, 100);
    rs_run(200, ref);
    for (i = 0; i < 200 * (int)CY_BLOCK; i++)
        diff += aft[i] != ref[i];
    CHECK(diff == 0 && peak_q32(ref, 200 * CY_BLOCK) > 0, "after a hand-over, %d samples differ from a fresh engine's", diff);
    SAY("  fade: within %.0f LSB of the old sound x the raised cosine (peak %.0f); then silent, held; the note after "
        "the hand-over: %d samples off a fresh engine's\n", worst, pk, diff);
    eng_reset();                                         /* restart (0): the patch in force, at once */
    cy_engine_note_on(-1, 60, 120);
    run_ms(300);
    cy_engine_restart(0);
    cy_engine_note_on(-1, 64, 100);
    rs_run(3, f);
    rs_run(200, aft);
    for (i = 0, diff = 0; i < 200 * (int)CY_BLOCK; i++)
        diff += aft[i] != ref[i];
    CHECK(diff == 0 && !cy_engine_restart_ready(), "restart (0): %d samples off a fresh engine's", diff);
}

/* fix-dsp regressions (the review's verified findings): each case failed before its fix */
static int fading_slots(void)
{
    uint32_t i;
    int n = 0;
    for (i = CY_SFADE; i < CY_SFADE + CY_NFADE; i++)
        n += cy_e.sl[i].fading != 0;
    return n;
}
static int slot_of(int note)
{
    int i;
    for (i = 0; i < CY_NVOICES; i++)
        if (cy_e.sl[i].state != CY_VS_FREE && !cy_e.sl[i].fading && cy_e.sl[i].note == note)
            return i;
    return -1;
}
static void t_fixes(void)
{
    static const int ch[8] = {48, 52, 55, 59, 62, 65, 69, 72};
    int i, k, s;
    size_t a;
    SAY("fix-dsp regressions\n");
    {   /* (1) a re-strike after budget contention gets every mode, each with a pole radius (before: the modes the
         * budget allowed later kept r = 0, so c = s = 0: silent, culled; a re-strike sounded its first 8 partials) */
        int n0, n1, nz = 0;
        patch_default();
        P.decay_mul = 4.0f, P.noise = 0.0f;
        geom_set(SET_PLATE, 0.62, 0.40);
        eng_reset();
        cy_engine_set_mode_budget(48);
        for (i = 0; i < 6; i++)
            cy_engine_note_on(i, ch[i], 110);
        run_ms(50);
        s = slot_of(ch[0]);
        n0 = s >= 0 ? (int)cy_popcount(cy_e.sl[s].act) : -1;
        P.damper = 1.0f;                     /* the others go, fast (the key-up damper) */
        cy_engine_set_patch(&P);
        for (i = 1; i < 6; i++)
            cy_engine_note_off(i, ch[i]);
        run_ms(2500);
        cy_engine_note_on(0, ch[0], 110);    /* key 0 is still down: a re-strike of the ringing note */
        run_ms(60);
        s = slot_of(ch[0]);
        n1 = s >= 0 ? (int)cy_popcount(cy_e.sl[s].act) : -1;
        if (s >= 0)
            for (k = 0; k < CY_N; k++)
                nz += (cy_e.sl[s].act >> k & 1u) && (cy_e.sl[s].r[k] <= 0.0f || cy_e.sl[s].e[k] <= 0.0f);
        CHECK(n0 > 0 && n0 <= 10, "contended first strike ran %d modes (budget 48 over 6 voices)", n0);
        CHECK(n1 >= 30 && nz == 0, "re-strike after contention: %d modes, %d of them dead (r or energy 0)", n1, nz);
        SAY("  re-strike after contention: %d -> %d modes, %d dead\n", n0, n1, nz);
    }
    {   /* (2) a 4-note chord into 8 ringing voices, in one render: four steals, no cut (before: 3 hard cuts) */
        uint32_t c0;
        int nf;
        double c_db, c_ref;
        int pass;
        for (pass = 0; pass < 2; pass++) {   /* pass 1: the same chord into a pool with room (no steal) */
            patch_default();
            P.decay_mul = 6.0f, P.noise = 0.0f, P.hardness = 0.3f, P.max_voices = pass ? 8 : 8;
            geom_set(SET_PLATE, 0.62, 0.40);
            eng_reset();
            for (i = 0; i < (pass ? 4 : 8); i++) {
                cy_engine_note_on(i, 40 + 3 * i, 90);
                run_ms(40);
            }
            run_ms(300);
            c0 = cy_e.steal_cuts;
            a = capN;
            for (i = 0; i < 4; i++)
                cy_engine_note_on(20 + i, 72 + 2 * i, 90);
            render1();
            nf = fading_slots();
            run_ms(300);
            if (!pass) {
                c_db = hp_peak_db(capL, capN, a - 441, a + 2205);
                CHECK(cy_e.steal_cuts == c0 && nf == 4, "4 steals in one render: %u cut, %d fading", cy_e.steal_cuts - c0, nf);
            } else {
                c_ref = hp_peak_db(capL, capN, a - 441, a + 2205);
                CHECK(c_db <= c_ref + 6.0, "chord into a full pool: 8 kHz peak %.1f dBFS (%.1f with room)", c_db, c_ref);
                SAY("  4-note chord into 8 ringing voices: 0 cuts, 8 kHz peak %.1f dBFS (%.1f with room)\n", c_db, c_ref);
            }
        }
    }
    {   /* (3) MIDI: aftertouch only adds (before: the first light touch switched to 0.4 v_n sw + P, -5 dB) */
        float p_none, p_light, p_full;
        patch_default();
        P.exciter = CY_EXC_BOW;
        geom_set(SET_PLATE, 0.12, 0.50);
        eng_reset();
        cy_engine_note_on(-1, 60, 100);
        run_ms(400);
        p_none = cy_e.sl[0].Peff;
        cy_engine_channel_pressure(8);
        run_ms(5);
        p_light = cy_e.sl[0].Peff;
        cy_engine_channel_pressure(127);
        run_ms(300);
        p_full = cy_e.sl[0].Peff;
        cy_engine_channel_pressure(0);
        CHECK(p_light >= p_none && p_full > 0.97f, "MIDI P_eff not monotonic: %.3f none, %.3f AT 8, %.3f AT 127",
              p_none, p_light, p_full);
        SAY("  MIDI bow P_eff: %.3f, AT 8 %.3f, AT 127 %.3f\n", p_none, p_light, p_full);
        /* (and channel pressure reaches MIDI notes only: a key-played note keeps its own pressure) */
        eng_reset();
        cy_engine_note_on(5, 60, 100);
        cy_engine_channel_pressure(127);
        run_ms(100);
        CHECK(cy_e.sl[0].P == 0.0f, "channel pressure reached a key-played note (P %.2f)", cy_e.sl[0].P);
        cy_engine_channel_pressure(0);
    }
    {   /* (4) SURFACE from MIDI: no sensing-key step on points 0..19, and channel pressure arrives */
        float pm, pk;
        patch_default();
        P.surface = 1, P.exciter = CY_EXC_BOW;
        geom_set(SET_PLATE, 0.62, 0.40);
        eng_reset();
        cy_engine_note_on_midi(18, 71, 100);
        run_ms(400);
        pm = cy_e.sl[CY_SSURF].Peff;
        cy_engine_channel_pressure(100);
        run_ms(200);
        CHECK(cy_e.sk_P[18] > 0.6f && !cy_e.sk_at[18], "SURFACE MIDI point: sk_P %.2f, sensing %d", cy_e.sk_P[18],
              cy_e.sk_at[18]);
        cy_engine_channel_pressure(0);
        eng_reset();
        cy_engine_note_on(18, 71, 100);      /* the same point from the keybed: gentle until leaned on */
        cy_engine_channel_pressure(100);
        run_ms(400);
        pk = cy_e.sl[CY_SSURF].Peff;
        CHECK(pm > 2.0f * pk && cy_e.sk_P[18] == 0.0f, "SURFACE P_eff: MIDI %.3f, keybed %.3f (sk_P %.2f)", pm, pk,
              cy_e.sk_P[18]);
        cy_engine_channel_pressure(0);
        SAY("  SURFACE point 18 P_eff: MIDI %.3f, keybed %.3f\n", pm, pk);
    }
    {   /* (5) SURFACE: OCT while the plate rings leaves the old plate ringing out at its own pitch (before: every
         * partial jumped an octave at one sub-block edge) */
        double p_oct = 0.0, p_ref = 0.0;
        float f_old = 0.0f, f_fade = 0.0f;
        int pass;
        for (pass = 0; pass < 2; pass++) {
            patch_default();
            P.surface = 1, P.decay_mul = 4.0f, P.noise = 0.0f;
            geom_set(SET_PLATE, 0.62, 0.40);
            eng_reset();
            cy_engine_note_on(9, 62, 110);
            run_ms(400);
            f_old = cy_e.sl[CY_SSURF].fnote;
            if (!pass) {
                P.surface_root = 60.0f;
                cy_engine_set_patch(&P);
            }
            a = capN;
            cy_engine_note_on(4, 57, 30);
            render1();
            if (!pass)
                for (i = CY_SFADE; i < (int)(CY_SFADE + CY_NFADE); i++)
                    if (cy_e.sl[i].fading)
                        f_fade = cy_e.sl[i].fnote;
            run_ms(200);
            if (!pass)
                p_oct = hp_peak_db(capL, capN, a - 441, a + 4410);
            else
                p_ref = hp_peak_db(capL, capN, a - 441, a + 4410);
        }
        CHECK(fabs(f_fade - f_old) < 1e-3f, "OCT: the old plate did not ring out at its pitch (%.2f vs %.2f Hz)", f_fade,
              f_old);
        CHECK(p_oct <= p_ref + 6.0, "OCT while ringing: 8 kHz peak %.1f dBFS (%.1f without)", p_oct, p_ref);
        SAY("  SURFACE OCT while ringing: 8 kHz peak %.1f dBFS (%.1f without the OCT)\n", p_oct, p_ref);
    }
    {   /* (6) LATCH holds a SURFACE bow (before: the LED said LATCH, the point stopped at once) */
        double l_held, l_rel;
        patch_default();
        P.surface = 1, P.exciter = CY_EXC_BOW, P.decay_mul = 0.1f;
        geom_set(SET_PLATE, 0.62, 0.40);
        eng_reset();
        cy_engine_note_on(7, 60, 100);
        cy_engine_pressure(7, 60, 100);
        run_ms(500);
        cy_engine_latch(1);
        cy_engine_note_off(7, 60);
        run_ms(1500);
        l_held = rms(capL, capN - 4410, capN);
        CHECK(cy_e.sk_lhold[7] && cy_e.sl[CY_SSURF].bowing && db(l_held) > -60.0, "SURFACE latch: %.1f dBFS, held %d",
              db(l_held), cy_e.sk_lhold[7]);
        cy_engine_latch(0);
        run_ms(1500);
        l_rel = rms(capL, capN - 4410, capN);
        CHECK(!cy_e.sk_down[7] && db(l_rel) < db(l_held) - 40.0, "SURFACE latch off: %.1f dBFS", db(l_rel));
        SAY("  SURFACE bow latched: %.1f dBFS held, %.1f after LATCH off\n", db(l_held), db(l_rel));
    }
    {   /* (7) DAMP chokes a frozen slot (spec §3.4: every slot while held) */
        double l0, l1;
        patch_default();
        P.decay_mul = 4.0f;
        geom_set(SET_PLATE, 0.62, 0.40);
        eng_reset();
        cy_engine_note_on(0, 50, 110), cy_engine_note_on(1, 57, 110);
        run_ms(300);
        cy_engine_freeze(1);
        run_ms(200);
        l0 = rms(capL, capN - 882, capN);
        cy_engine_damp(1);
        run_ms(300);
        l1 = rms(capL, capN - 882, capN);
        cy_engine_damp(0), cy_engine_freeze(0);
        CHECK(db(l1 / l0) < -40.0, "DAMP on a frozen chord: only %.1f dB in 300 ms", db(l1 / l0));
        SAY("  DAMP on a frozen chord: %.1f dB in 300 ms\n", db(l1 / l0));
    }
    {   /* (8) no subnormals: a bow at FORCE 0 for 40 s (its coupled modes are never culled), a SURFACE bow moved onto
         * a point where modes have an exact-zero field, a SURFACE key's pressure let go */
        patch_default();
        P.exciter = CY_EXC_BOW, P.force = 0.6f;
        geom_set(SET_PLATE, 0.62, 0.40);
        eng_reset();
        subnormal_seen = 0;
        cy_engine_note_on(3, 60, 100);
        cy_engine_pressure(3, 60, 80);
        run_ms(500);
        P.force = 0.0f;
        cy_engine_set_patch(&P);
        run_ms(40000);
        cy_engine_note_off(3, 60);
        patch_default();
        P.surface = 1, P.exciter = CY_EXC_BOW;
        eng_reset();
        cy_engine_note_on(3, 56, 100), cy_engine_pressure(3, 56, 100);
        run_ms(500);
        cy_engine_note_on(13, 66, 100), cy_engine_pressure(13, 66, 127), cy_engine_note_off(3, 56);
        run_ms(3000);
        cy_engine_pressure(13, 66, 0);
        run_ms(3000);
        CHECK(subnormal_seen == 0, "%d subnormal values (bow FORCE 0, field glide, SURFACE pressure)", subnormal_seen);
        SAY("  subnormals with FORCE 0 for 40 s, a field glide to zeros, pressure let go: %d\n", subnormal_seen);
    }
    {   /* (9) a note in the same render as PANIC sounds (before: the panicked slots took the whole budget) */
        int nm;
        patch_default();
        P.decay_mul = 4.0f;
        geom_set(SET_PLATE, 0.62, 0.40);
        eng_reset();
        for (i = 0; i < 8; i++)
            cy_engine_note_on(i, ch[i], 100);
        run_ms(300);
        cy_engine_panic();
        cy_engine_note_on(0, 60, 100);
        render1();
        run_ms(60);
        s = slot_of(60);
        nm = s >= 0 ? (int)cy_popcount(cy_e.sl[s].act) : 0;
        CHECK(nm >= 16, "a note with PANIC in the same render: %d modes", nm);
        SAY("  note in the PANIC render: %d modes\n", nm);
    }
    {   /* (10) the bow's coupled-mode share follows the budget: at the floor, 4 bows run 6 coupled modes each */
        int mx = 0;
        patch_default();
        P.exciter = CY_EXC_BOW, P.grit = 1.0f, P.force = 1.0f;
        geom_set(SET_PLATE, 0.12, 0.50);
        eng_reset();
        cy_engine_set_mode_budget(48);
        for (i = 0; i < 4; i++)
            cy_engine_note_on(i, ch[i], 110), cy_engine_pressure(i, ch[i], 120);
        run_ms(500);
        for (i = 0; i < CY_NVOICES; i++)
            if (cy_e.sl[i].bowing && cy_e.sl[i].bnc > mx)
                mx = cy_e.sl[i].bnc;
        CHECK(mx >= 1 && mx <= 6 && T.nan_resets == 0, "4 bows at budget 48: %d coupled modes in a slot", mx);
        SAY("  4 bows at budget 48: <= %d coupled modes each\n", mx);
    }
#ifndef CY_TEST_REAL_GEOM
    {   /* (11) SURFACE rain while the field cache is being rewritten: the drops still land, from the key points */
        uint32_t d0;
        patch_default();
        P.surface = 1, P.exciter = CY_EXC_RAIN, P.density = 0.8f;
        geom_set(SET_PLATE, 0.62, 0.40);
        eng_reset();
        mfc_busy = 1;
        cy_engine_note_on(9, 62, 100);
        d0 = drops;
        run_ms(500);
        mfc_busy = 0;
        CHECK(drops > d0 + 5 && T.nan_resets == 0 && rms(capL, capN - 4410, capN) > 0.0,
              "SURFACE rain with the cache busy: %u drops", drops - d0);
    }
#endif
}

static void t_level(void)
{
    double pk;
    SAY("gain staging\n");
    patch_default();
    geom_set(SET_PLATE, 0.62, 0.40);
    eng_reset();
    cy_engine_note_on(7, 60, 100);
    run_ms(1000);
    pk = peak(capL, 0, capN);
    pk = fmax(pk, peak(capR, 0, capN));
#ifdef CY_TEST_REAL_GEOM
    CHECK(db(pk) > -26.0 && db(pk) < -10.0, "vel-100 C4 strike peaks at %.1f dBFS (target about -18)", db(pk));
#endif
    SAY("  vel-100 C4 strike: bus peak %.1f dBFS (spec target about -18; the out_norm is geom's, so this is checked "
        "only against the real plates)\n", db(pk));
    wav24("strike_c4.wav", capL, capR, 0, capN, 1.0);
}

static uint32_t perf_hash(uint32_t seed)
{
    uint32_t h = 0x811C9DC5u;
    size_t i;
    int b;
    patch_default();
    geom_set(SET_PLATE, 0.62, 0.40);
    eng_reset();
    cy_engine_seed(seed);
    for (b = 0; b < 1400; b++) {                 /* ~4 s: every exciter, SURFACE, bend, pressure, freeze */
        if (b == 0) cy_engine_note_on(7, 60, 100);
        if (b == 60) cy_engine_note_on(11, 64, 90), cy_engine_pressure(11, 64, 70);
        if (b == 120) { P.exciter = CY_EXC_BOW; cy_engine_set_patch(&P); cy_engine_note_on(3, 56, 100); }
        if (b == 130) cy_engine_pressure(3, 56, 110);
        if (b == 300) { P.exciter = CY_EXC_RAIN; cy_engine_set_patch(&P); cy_engine_note_on(14, 67, 110); }
        if (b == 310) cy_engine_pressure(14, 67, 90);
        if (b == 500) cy_engine_note_off(3, 56), cy_engine_bend(0.7f);
        if (b == 600) cy_engine_freeze(1);
        if (b == 700) cy_engine_freeze(0), cy_engine_note_off(14, 67);
        if (b == 800) { P.exciter = CY_EXC_MALLET; P.surface = 1; cy_engine_set_patch(&P); cy_engine_note_on(9, 62, 100); }
        if (b == 900) cy_engine_note_on(2, 55, 12), cy_engine_pressure(2, 55, 40);
        if (b == 1000) { P.exciter = CY_EXC_RAIN; cy_engine_set_patch(&P); cy_engine_note_on(20, 73, 100); }
        if (b == 1200) cy_engine_note_off(20, 73), cy_engine_note_off(2, 55), cy_engine_note_off(9, 62);
        render1();
    }
    for (i = 0; i < capN; i++) {
        uint32_t v[2], k;
        memcpy(&v[0], &capL[i], 4), memcpy(&v[1], &capR[i], 4);
        for (k = 0; k < 8; k++)
            h = (h ^ ((v[k >> 2] >> (8u * (k & 3u))) & 0xFFu)) * 0x01000193u;
    }
    return h;
}

static void t_determinism(void)
{
    uint32_t h1, h2, h3;
    SAY("determinism\n");
    h1 = perf_hash(0x9E3779B9u);
    h2 = perf_hash(0x9E3779B9u);
    h3 = perf_hash(12345u);
    CHECK(h1 == h2, "same seed, different hash: %08X vs %08X", h1, h2);
    CHECK(h1 != h3, "a different seed gave the same hash");
    CHECK(T.nan_resets == 0, "NaN in the performance");
    SAY("  performance hash 0x%08X twice; seed 12345 gives 0x%08X\n", h1, h3);
}

static void t_long_clean(void)
{
    int s, k;
    size_t a;
    SAY("long render: no NaN, no denormal, exact zeros after\n");
    patch_default();
    P.decay_mul = 2.0f;
    geom_set(SET_PLATE, 0.62, 0.40);
    eng_reset();
    subnormal_seen = 0;
    nyq_bad = 0;
    for (s = 0; s < 60; s++) {                   /* a minute of mixed playing */
        int note = 48 + (s * 7) % 30, key = s % 27;
        P.exciter = (uint8_t)(s / 4 % 3);
        P.surface = (uint8_t)(s / 12 % 2);
        P.bloom = (s % 5) ? 0.0f : 0.5f;
        P.vibrato_cents = (s % 7) ? 0.0f : 20.0f;
        cy_engine_set_patch(&P);
        cy_engine_note_on(key, note, 40 + (s * 37) % 88);
        cy_engine_pressure(key, note, (s * 53) % 128);
        run_ms(600);
        cy_engine_note_off(key, note);
        run_ms(400);
    }
    for (k = 0; k < (int)capN; k++)
        if (!isfinite(capL[k]) || !isfinite(capR[k]))
            break;
    CHECK(k == (int)capN, "non-finite sample at %d", k);
    P.surface = 0;
    cy_engine_set_patch(&P);
    run_ms(60000);                               /* a minute of silence */
    a = capN - 5 * 44100;
    CHECK(peak(capL, a, capN) == 0.0 && peak(capR, a, capN) == 0.0, "the last 5 s are not exact zeros");
    CHECK(subnormal_seen == 0, "%d subnormal values seen", subnormal_seen);
    CHECK(T.nan_resets == 0, "%u NaN resets", T.nan_resets);
    CHECK(live_voices() == 0 && T.modes_used == 0, "voices still live after a minute of silence");
    CHECK(seq_bad == 0, "telemetry seq not even/+2 in %u renders", seq_bad);
    SAY("  60 s played + 60 s idle: finite, no subnormals, exact zeros, seq always even\n");
}

static void t_telemetry(void)
{
    int i, ok = 1;
    uint32_t sum = 0;
    SAY("telemetry consistency\n");
    patch_default();
    geom_set(SET_PLATE, 0.62, 0.40);
    eng_reset();
    cy_engine_note_on(3, 56, 100);
    cy_engine_note_on(9, 62, 80);
    cy_engine_note_on(-1, 70, 64);
    run_ms(100);
    CHECK(live_voices() == 3, "voices %d, 3 played", live_voices());
    for (i = 0; i < CY_NVOICES; i++) {
        if (T.voice_state[i] == CY_VS_FREE)
            continue;
        ok &= (T.voice_key[i] == 3 && T.voice_note[i] == 56) || (T.voice_key[i] == 9 && T.voice_note[i] == 62) ||
              (T.voice_key[i] == -1 && T.voice_note[i] == 70);
        ok &= T.voice_state[i] == CY_VS_HELD;
    }
    CHECK(ok, "voice key / note / state do not match what was played");
    for (i = 0; i < CY_NALL; i++)
        sum += cy_popcount(cy_e.sl[i].act);
    CHECK(sum == T.modes_used, "modes_used %u != active %u", T.modes_used, sum);
    CHECK(T.key_level[3] > 0.0f && T.key_level[9] > 0.0f && T.key_level[0] == 0.0f, "TUNED key levels");
    CHECK(T.est_instr > 400.0f && T.est_instr < 3500.0f, "est_instr %.0f", T.est_instr);
    CHECK(T.newest_hz > 400.0f && T.newest_hz < 480.0f, "newest_hz %.1f (A4 played last)", T.newest_hz);
    {
        double s = 0;
        for (i = 0; i < CY_NMODES; i++) {
            CHECK(T.mode_energy[i] >= 0.0f && isfinite(T.mode_energy[i]), "mode_energy[%d]", i);
            s += T.mode_energy[i];
        }
        CHECK(s > 1e-6 && s < 1.0, "mode energy sum %.3g", s);
    }
    CHECK(strikes == 3, "STRIKE events %u, 3 played", strikes);
    cy_engine_note_off(3, 56);
    run_ms(50);
    for (i = 0; i < CY_NVOICES; i++)
        if (T.voice_key[i] == 3)
            CHECK(T.voice_state[i] == CY_VS_RING, "released voice state %d", T.voice_state[i]);
    cy_engine_latch(1), cy_engine_freeze(1), cy_engine_damp(1);
    run_ms(10);
    CHECK(T.latched && T.frozen && T.damping, "flags not mirrored");
    cy_engine_set_mode_budget(77);
    run_ms(10);
    CHECK(T.mode_budget == 77, "budget %u", T.mode_budget);
    CHECK(strcmp(cy_engine_exciter_name(CY_EXC_BOW), CY_BOW_MODEL ? "BOW" : "BREATH") == 0, "exciter name");
    SAY("  voices, keys, notes, states, modes, levels, flags, budget, events all consistent\n");
}

static void t_cpu(void)
{
    static const int ch[8] = {48, 52, 55, 59, 62, 65, 69, 72};
    int i;
    float typ, worst, dev, bow4;
    double ns;
    struct timespec t0, t1;
    SAY("CPU model (spec 3.14; algorithmic estimate, host build, not measured on the FM-1)\n");
    patch_default();
    geom_set(SET_PLATE, 0.62, 0.40);
    /* typical: 4 voices left ringing, ~70 active modes (MODES 18) */
    P.modes_per_voice = 18;
    eng_reset();
    for (i = 0; i < 4; i++)
        cy_engine_note_on(i, ch[i], 100), cy_engine_note_off(i, ch[i]);
    run_ms(1500);
    typ = T.est_instr;
    SAY("  typical: %u modes ringing in 4 voices -> est %.0f instr/sample (spec about 1,700)\n", T.modes_used, typ);
    P.modes_per_voice = 32;
    /* worst, no bow: 8 fresh fortissimo voices at budget 128 */
    eng_reset();
    est_max = 0;
    for (i = 0; i < 8; i++)
        cy_engine_note_on(i, ch[i], 127);
    run_ms(200);
    worst = est_max;
    SAY("  worst, no bow, budget 128: est %.0f (spec about 3,400)\n", worst);
    CHECK(worst <= 3500.0f, "worst no-bow estimate %.0f > 3,500", worst);
    eng_reset();
    est_max = 0;
    cy_engine_set_mode_budget(96);
    for (i = 0; i < 8; i++)
        cy_engine_note_on(i, ch[i], 127);
    run_ms(200);
    dev = est_max;
    SAY("  device default (budget 96): est %.0f (spec <= 2,900)\n", dev);
    CHECK(dev <= 2900.0f, "device-config estimate %.0f > 2,900", dev);
    /* four bows */
    P.exciter = CY_EXC_BOW;
    eng_reset();
    est_max = 0;
    for (i = 0; i < 4; i++)
        cy_engine_note_on(i, ch[i], 100), cy_engine_pressure(i, ch[i], 100);
    run_ms(500);
    bow4 = est_max;
    SAY("  4 bowed voices, budget 128: est %.0f\n", bow4);
    /* host time per sample of the 8-voice worst case (reference only) */
    P.exciter = CY_EXC_MALLET;
    eng_reset();
    for (i = 0; i < 8; i++)
        cy_engine_note_on(i, ch[i], 127);
    render1();
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (i = 0; i < 344; i++)
        cy_engine_render(blk, CY_BLOCK, 4096u);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ns = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (344.0 * CY_BLOCK);
    SAY("  host: %.1f ns per output sample for 8 fresh voices (%.1f%% of real time on this machine)\n", ns,
        ns / (1e9 / FS) * 100.0);
    CHECK(typ > 800.0f && typ < 2600.0f, "typical estimate %.0f far from the spec's ~1,700", typ);
    if (ev_dir) {
        char path[1024];
        FILE *f;
        snprintf(path, sizeof path, "%s/cpu_model.txt", ev_dir);
        if ((f = fopen(path, "w"))) {
            fprintf(f, "label: algorithmic model estimate, host build, not measured on the FM-1\n");
            fprintf(f, "typical_4_ringing %.0f\nworst_no_bow_budget128 %.0f\nworst_no_bow_budget96 %.0f\n", typ, worst, dev);
            fprintf(f, "four_bows_budget128 %.0f\nhost_ns_per_sample_8voices %.1f\n", bow4, ns);
            fclose(f);
        }
    }
}

int main(int argc, char **argv)
{
    capL = (float *)malloc(CAP_MAX * sizeof(float));
    capR = (float *)malloc(CAP_MAX * sizeof(float));
    if (argc > 1 && !strcmp(argv[1], "--hash")) {
        quiet = 1;
        printf("engine_hash 0x%08X\n", perf_hash(0x9E3779B9u));
        return 0;
    }
    if (argc > 1)
        ev_dir = argv[1];
    printf("CYMA-1 engine unit test (CY_FS %.1f, budget %d, bow model %d)\n", (double)CY_FS, CY_BUDGET_DEFAULT,
           CY_BOW_MODEL);
    t_sine_onset();
    t_tuning_ratios();
    t_keyup_click();
    t_level();
    t_budget();
    t_steal();
    t_bow_sweep();
    t_bow_release_and_latch();
    t_rain_stats();
    t_gain_drift();
    t_freeze();
    t_surface();
    t_nyquist();
    t_ultrasonic();
    t_morph_glide();
    t_bloom();
    t_damp_panic_midi();
    t_restart();
    t_fixes();
    t_telemetry();
    t_determinism();
    t_long_clean();
    t_cpu();
    printf("%d checks, %d failed\n", n_check, n_fail);
    return n_fail ? 1 : 0;
}
