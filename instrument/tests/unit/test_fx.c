/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 tests/unit/test_fx.c (M3): the acceptance tests of src/fx.c (spec 3.10-3.11, 8).
 *
 * Build and run from the project root (host only: this file may use libm and double; src/fx.c may not):
 *   cc -O2 -std=c99 -ffp-contract=off -DPLAT_HOST -Iinstrument/src -Ireference/fm1-base/include \
 *      instrument/tests/unit/test_fx.c instrument/src/fx.c -lm -o test_fx && ./test_fx [EVIDENCE_DIR]
 * EVIDENCE_DIR (default instrument/build/evidence/fx) gets the impulse-response WAVs, spectra CSVs and fx_test.log.
 *
 * What it proves, each with numbers in the log:
 *   limiter      hot input (to +36 dBFS, every stage at its worst) never passes -0.3 dBFS in the 24-bit output
 *   aliasing     the 2x ADAA drive against naive 1x, 1x ADAA and 2x-without-ADAA references, on-bin sines
 *   decay        measured T60 (Schroeder, -5..-35 dB) against SIZE: mid-band, broadband, and per third-octave
 *                band (STFT) with DAMPING against the absorbers' design
 *   stability    every corner of the 6 parameters, random jumps, hot noise: finite, bounded, decays to zero
 *   nan          a NaN or Inf on the bus resets the chain, counts once, and the output stays clean
 *   silence      zeros in give exact zeros out, and no fx state ever goes subnormal
 *   latency      the measured dry delay equals cy_fx_latency(), and the engaged drive matches it
 *   smoothing    jumping any parameter makes no click (second-difference test)
 *   + the IR WAVs, reverb level calibration, stereo decorrelation, gain staging, determinism, host timing, and
 *   demo WAVs to listen to. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cy_fx.h"

uint32_t cy_fx_debug_subnormals(void);   /* fx.c, PLAT_HOST only */
float cy_fx_debug_gin(void);

#define FS 44100.0
#define QMAX 8388607.0
static const char *outdir = "instrument/build/evidence/fx";
static FILE *logf_;
static int fails, checks;

static void say(const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    if (logf_) {
        va_start(a, fmt);
        vfprintf(logf_, fmt, a);
        va_end(a);
    }
}
static void check(int ok, const char *fmt, ...)
{
    char buf[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof buf, fmt, a);
    va_end(a);
    checks++;
    if (!ok)
        fails++;
    say("%s %s\n", ok ? "  ok  " : "  FAIL", buf);
}

/* ------------------------------------------------------------------------------------------------ helpers --- */
static uint32_t rng = 0x12345678u;
static double urand(void) { rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5; return (rng >> 8) * (1.0 / 16777216.0); }
static double grand(void) { double u = urand() + 1e-12, v = urand(); return sqrt(-2.0 * log(u)) * cos(6.283185307179586 * v); }

static cy_fx_params_t P(float drive, float tilt, float room, float size, float damp, float pd)
{
    cy_fx_params_t p;
    p.drive_db = drive, p.tilt_db = tilt, p.room = room, p.size_s = size, p.damping = damp, p.predelay_ms = pd;
    return p;
}

/* run n frames of float input through the chain in CY_BLOCK blocks (the input is copied: fx works in place) */
static void run(const float *L, const float *R, size_t n, uint32_t master, int32_t *out)
{
    float l[CY_BLOCK], r[CY_BLOCK];
    size_t i = 0;
    while (i < n) {
        size_t k = n - i > CY_BLOCK ? CY_BLOCK : n - i;
        memcpy(l, L + i, k * sizeof(float));
        memcpy(r, R + i, k * sizeof(float));
        cy_fx_process(l, r, (uint32_t)k, master, out + 2 * i);
        i += k;
    }
}
static void run_silence(size_t n, uint32_t master, int32_t *out)
{
    float l[CY_BLOCK], r[CY_BLOCK];
    size_t i = 0;
    while (i < n) {
        size_t k = n - i > CY_BLOCK ? CY_BLOCK : n - i;
        memset(l, 0, sizeof l), memset(r, 0, sizeof r);
        cy_fx_process(l, r, (uint32_t)k, master, out ? out + 2 * i : (int32_t[2 * CY_BLOCK]){0});
        i += k;
    }
}
static int32_t maxabs_q(const int32_t *q, size_t n2)
{
    int32_t m = 0;
    size_t i;
    for (i = 0; i < n2; i++)
        if (abs(q[i]) > m)
            m = abs(q[i]);
    return m;
}
static double db(double x) { return 20.0 * log10(x > 1e-300 ? x : 1e-300); }

static void wav24(const char *name, const int32_t *q, size_t frames)
{
    char path[1024];
    FILE *f;
    size_t i;
    uint32_t data = (uint32_t)(frames * 6), v;
    unsigned char h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                           0x44, 0xAC, 0, 0, 0x98, 0x09, 0x04, 0, 6, 0, 24, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
    v = data + 36;
    h[4] = v & 255, h[5] = (v >> 8) & 255, h[6] = (v >> 16) & 255, h[7] = v >> 24;
    h[40] = data & 255, h[41] = (data >> 8) & 255, h[42] = (data >> 16) & 255, h[43] = data >> 24;
    snprintf(path, sizeof path, "%s/%s", outdir, name);
    f = fopen(path, "wb");
    if (!f) {
        say("  (could not write %s)\n", path);
        return;
    }
    fwrite(h, 1, 44, f);
    for (i = 0; i < 2 * frames; i++) {
        unsigned char b[3] = {(unsigned char)(q[i] & 255), (unsigned char)((q[i] >> 8) & 255), (unsigned char)((q[i] >> 16) & 255)};
        fwrite(b, 1, 3, f);
    }
    fclose(f);
    say("  wrote %s (%.2f s, 24-bit stereo)\n", path, frames / FS);
}

/* in-place radix-2 complex FFT */
static void fft(double *re, double *im, size_t n)
{
    size_t i, j, k, m;
    for (i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i];
            re[i] = re[j], re[j] = t, t = im[i], im[i] = im[j], im[j] = t;
        }
    }
    for (m = 2; m <= n; m <<= 1) {
        double a = -6.283185307179586 / (double)m, wr = cos(a), wi = sin(a);
        for (k = 0; k < n; k += m) {
            double cr = 1.0, ci = 0.0;
            for (j = 0; j < m / 2; j++) {
                size_t p = k + j, q = p + m / 2;
                double tr = re[q] * cr - im[q] * ci, ti = re[q] * ci + im[q] * cr, t;
                re[q] = re[p] - tr, im[q] = im[p] - ti, re[p] += tr, im[p] += ti;
                t = cr * wr - ci * wi, ci = cr * wi + ci * wr, cr = t;
            }
        }
    }
}

/* T60 from a power envelope: Schroeder backward integral, a least-squares line on the EDC between lo and hi dB */
static double t60_edc(const double *pw, size_t n, double lo, double hi)
{
    double *edc = malloc(n * sizeof(double)), acc = 0.0, sx = 0, sy = 0, sxx = 0, sxy = 0, cnt = 0, slope;
    size_t i;
    for (i = n; i-- > 0;)
        acc += pw[i], edc[i] = acc;
    for (i = 0; i < n; i++) {
        double e = 10.0 * log10(edc[i] / edc[0] + 1e-300);
        if (e <= lo && e >= hi) {
            double t = i / FS;
            sx += t, sy += e, sxx += t * t, sxy += t * e, cnt++;
        }
    }
    free(edc);
    if (cnt < 10)
        return -1.0;
    slope = (cnt * sxy - sx * sy) / (cnt * sxx - sx * sx);   /* dB per second */
    return -60.0 / slope;
}

/* T60 per band from an STFT (Blackman-Harris 4-term, 4096 points, hop 256: sidelobes -92 dB, so a band's EDC is
 * not lifted by the slower decay of its neighbours the way a biquad band-pass's skirts lift it). The band is
 * fc 2^(+-1/6) (third octave); q holds interleaved stereo Q23 from sample `from` on. */
static double t60_band(const int32_t *q, size_t n, size_t from, double fc)
{
    const size_t N = 4096, hop = 256;
    size_t nf = (n - from - N) / hop, f, i, k0 = (size_t)(fc * pow(2.0, -1.0 / 6.0) * N / FS), k1 = (size_t)(fc * pow(2.0, 1.0 / 6.0) * N / FS + 1.0);
    double *re = malloc(N * sizeof(double)), *im = malloc(N * sizeof(double)), *env = calloc(nf, sizeof(double)), *edc = calloc(nf, sizeof(double));
    double acc = 0, sx = 0, sy = 0, sxx = 0, sxy = 0, cnt = 0, win[4096];
    int c;
    for (i = 0; i < N; i++) {
        double x = 6.283185307179586 * i / (N - 1);
        win[i] = 0.35875 - 0.48829 * cos(x) + 0.14128 * cos(2 * x) - 0.01168 * cos(3 * x);
    }
    for (f = 0; f < nf; f++)
        for (c = 0; c < 2; c++) {
            size_t k;
            for (i = 0; i < N; i++)
                re[i] = win[i] * q[2 * (from + f * hop + i) + c] / QMAX, im[i] = 0.0;
            fft(re, im, N);
            for (k = k0; k <= k1 && k < N / 2; k++)
                env[f] += re[k] * re[k] + im[k] * im[k];
        }
    for (f = nf; f-- > 0;)
        acc += env[f], edc[f] = acc;
    for (f = 0; f < nf; f++) {
        double e = 10.0 * log10(edc[f] / edc[0] + 1e-300);
        if (e <= -5.0 && e >= -35.0) {
            double t = f * hop / FS;
            sx += t, sy += e, sxx += t * t, sxy += t * e, cnt++;
        }
    }
    free(re), free(im), free(env), free(edc);
    return cnt < 4 ? -1.0 : -60.0 / ((cnt * sxy - sx * sy) / (cnt * sxx - sx * sx));
}

/* --------------------------------------------------------------------------------------------- 1. limiter --- */
static void test_limiter(void)
{
    const size_t n = (size_t)(4 * FS);
    float *L = malloc(n * sizeof(float)), *R = malloc(n * sizeof(float));
    int32_t *q = malloc(2 * n * sizeof(int32_t));
    const int32_t ceil_q = (int32_t)floor(CY_CEIL * QMAX + 0.5);
    int32_t worst = 0;
    int s;
    cy_fx_meter_t m;
    say("\n[1] limiter: hot input never passes CY_CEIL = %.5f (-0.3 dBFS) = %d in Q23\n", CY_CEIL, ceil_q);
    for (s = 0; s < 8; s++) {
        size_t i;
        const char *what = "";
        cy_fx_init();
        switch (s) {
        case 0: cy_fx_set(&(cy_fx_params_t){0, 0, 0, 2.8f, 0.55f, 12}); what = "100 Hz sine +12 dBFS, dry"; break;
        case 1: cy_fx_set(&(cy_fx_params_t){0, 6, 0.3f, 2.8f, 0.55f, 12}); what = "sweep 20 Hz-20 kHz +6 dBFS, tilt +6"; break;
        case 2: cy_fx_set(&(cy_fx_params_t){24, 6, 1, 12, 1, 0}); what = "noise +20 dBFS rms, drive 24, room 1, 12 s"; break;
        case 3: cy_fx_set(&(cy_fx_params_t){0, 0, 0, 0.3f, 1, 0}); what = "isolated +36 dBFS one-sample spikes"; break;
        case 4: cy_fx_set(&(cy_fx_params_t){24, -6, 1, 0.3f, 0.2f, 60}); what = "+/-16 square bursts, drive 24"; break;
        case 5: cy_fx_set(&(cy_fx_params_t){0, 0, 0.5f, 6, 0.6f, 20}); what = "8 s-like mix: 8 strikes stacked at +18 dB"; break;
        case 6: cy_fx_set(&(cy_fx_params_t){12, 3, 0.7f, 4, 0.5f, 30}); what = "random params every block, noise +12 dBFS"; break;
        default: cy_fx_set(&(cy_fx_params_t){24, 6, 1, 12, 1, 60}); what = "hard-panned L-only +30 dBFS 15 kHz"; break;
        }
        for (i = 0; i < n; i++) {
            double t = i / FS, x = 0, y = 0;
            switch (s) {
            case 0: x = y = 4.0 * sin(6.283185307179586 * 100.0 * t); break;
            case 1: { double f = 20.0 * pow(1000.0, t / 4.0); x = y = 2.0 * sin(6.283185307179586 * 20.0 * 4.0 / log(1000.0) * (pow(1000.0, t / 4.0) - 1.0)); (void)f; } break;
            case 2: x = 10.0 * grand(), y = 10.0 * grand(); break;
            case 3: x = y = (i % 997 == 500) ? ((i / 997) & 1 ? -64.0 : 64.0) : 0.0; break;
            case 4: x = y = ((i / 2205) & 1) ? (((i / 37) & 1) ? 16.0 : -16.0) : 0.0; break;
            case 5: { double e = exp(-fmod(t, 0.5) * 6.0); x = e * 8.0 * 0.125 * (sin(6.283185307179586 * 523.0 * t) + 0.7 * sin(6.283185307179586 * 1417.0 * t));
                      y = e * 8.0 * 0.125 * (sin(6.283185307179586 * 659.0 * t) + 0.5 * sin(6.283185307179586 * 2213.0 * t)); } break;
            case 6: x = 4.0 * grand(), y = 4.0 * grand(); break;
            default: x = 31.6 * sin(6.283185307179586 * 15000.0 * t), y = 0.0; break;
            }
            L[i] = (float)x, R[i] = (float)y;
        }
        if (s == 6) {                    /* random parameter jumps every block */
            size_t b;
            float l[CY_BLOCK], r[CY_BLOCK];
            for (b = 0; b < n / CY_BLOCK; b++) {
                cy_fx_set(&(cy_fx_params_t){(float)(24 * urand()), (float)(12 * urand() - 6), (float)urand(),
                                            (float)(0.3 + 11.7 * urand()), (float)(0.2 + 0.8 * urand()), (float)(60 * urand())});
                memcpy(l, L + b * CY_BLOCK, sizeof l), memcpy(r, R + b * CY_BLOCK, sizeof r);
                cy_fx_process(l, r, CY_BLOCK, 4096u, q + 2 * b * CY_BLOCK);
            }
        } else {
            run(L, R, n, 4096u, q);
        }
        {
            int32_t mx = maxabs_q(q, 2 * (n / CY_BLOCK) * CY_BLOCK);
            cy_fx_meter(&m);
            if (mx > worst)
                worst = mx;
            check(mx <= ceil_q, "%-44s peak %d = %.4f dBFS (GR meter %.1f dB)", what, mx, db(mx / QMAX), m.gr_db);
            if (s == 5)
                wav24("fx_limiter_strikes.wav", q, n);   /* the stacked strikes, limited: see limiter.png */
        }
    }
    check(worst <= ceil_q, "worst over all: %.5f dBFS <= -0.3 dBFS", db(worst / QMAX));
    /* the ceiling with MASTER below full: the limiter is pre-MASTER, so the output scales exactly */
    {
        size_t i;
        int32_t mx;
        cy_fx_init();
        cy_fx_set(&(cy_fx_params_t){0, 0, 0, 2.8f, 0.55f, 0});
        for (i = 0; i < n; i++)
            L[i] = R[i] = (float)(4.0 * sin(6.283185307179586 * 100.0 * i / FS));
        run(L, R, n, 2048u, q);
        mx = maxabs_q(q + n, n);     /* the second half: MASTER's 10 ms glide long done */
        check(fabs(db(mx / QMAX) - (db(CY_CEIL) - 12.0412)) < 0.01, "MASTER 2048 = -12.04 dB after the limiter: peak %.3f dBFS (want %.3f)",
              db(mx / QMAX), db(CY_CEIL) - 12.0412);
    }
    /* GR reads what it did: +12 dB into a -0.3 ceiling is ~ 12.3 dB of reduction */
    free(L), free(R), free(q);
}

/* -------------------------------------------------------------------------------------------- 2. aliasing --- */
static const double HB[8] = {0.053615466649, 0.193436724219, 0.372315971295, 0.546689394877,
                             0.693030266958, 0.806703409456, 0.894013055065, 0.965874302792};
typedef struct { double s[16][2], u1, s1; } ref2x_t;
static double ap4d(double x, double (*s)[2], const double *c)
{
    int k;
    for (k = 0; k < 4; k++) {
        double y = c[k] * (x - s[k][1]) + s[k][0];
        s[k][0] = x, s[k][1] = y, x = y;
    }
    return x;
}
static double nl(double u) { return u / sqrt(1.0 + u * u); }

/* the alias measure of an N-point periodic output: power off the harmonic bins, relative to the fundamental */
static void alias_measure(const double *y, size_t N, size_t k, double *tot_dbc, double *worst_dbc, double *below10k_dbc,
                          double *fund_db, FILE *csv, const char *tag)
{
    double *re = calloc(N, sizeof(double)), *im = calloc(N, sizeof(double)), pf, pa = 0, pa10 = 0, worst = 0;
    unsigned char *harm = calloc(N / 2 + 1, 1);
    size_t i, h;
    for (i = 0; i < N; i++)
        re[i] = y[i];
    fft(re, im, N);
    for (h = 1; h * k <= N / 2; h++)
        harm[h * k] = 1;
    pf = re[k] * re[k] + im[k] * im[k];
    for (i = 2; i < N / 2; i++) {
        double p = re[i] * re[i] + im[i] * im[i];
        if (csv)
            fprintf(csv, "%s,%.3f,%.3f\n", tag, i * FS / N, 10.0 * log10(p / pf + 1e-300));
        if (harm[i])
            continue;
        pa += p;
        if (i * FS / N < 10000.0)
            pa10 += p;
        if (p > worst)
            worst = p;
    }
    *tot_dbc = 10.0 * log10(pa / pf + 1e-300);
    *worst_dbc = 10.0 * log10(worst / pf + 1e-300);
    *below10k_dbc = 10.0 * log10(pa10 / pf + 1e-300);
    *fund_db = 10.0 * log10(pf) - 20.0 * log10(N / 2.0);
    free(re), free(im), free(harm);
}

static void test_aliasing(void)
{
    const size_t N = 65536, warm = 32768;
    static const size_t K[] = {1487, 4459, 7431, 10403, 14861};   /* odd bins: ~1, 3, 5, 7, 10 kHz */
    static const double DRV[] = {12.0, 24.0};
    double *y = malloc(N * sizeof(double));
    float *L = malloc((N + warm) * sizeof(float)), *R = malloc((N + warm) * sizeof(float));
    int32_t *q = malloc(2 * (N + warm) * sizeof(int32_t));
    char path[1024];
    FILE *csv;
    size_t ki, di;
    double sum_imp = 0, cnt = 0;
    snprintf(path, sizeof path, "%s/alias_spectra.csv", outdir);
    csv = fopen(path, "w");
    if (csv)
        fprintf(csv, "tag,hz,dbc\n");
    say("\n[2] drive aliasing: a -6 dBFS sine on an exact bin (N = %zu, rectangular window, steady state); alias = all power\n"
        "    off the harmonic bins, in dB relative to the fundamental (dBc). References computed in double here.\n", N);
    say("    %-6s %-8s | %-21s | %-21s | %-21s | %-21s | %s\n", "drive", "f (Hz)", "naive 1x  tot/worst", "ADAA 1x  tot/worst",
        "2x no-ADAA tot/worst", "fx: 2x ADAA tot/worst", "fx <10k | gain vs naive");
    for (di = 0; di < 2; di++)
        for (ki = 0; ki < sizeof K / sizeof K[0]; ki++) {
            size_t k = K[ki], i;
            double f = k * FS / N, d = pow(10.0, DRV[di] / 20.0), mk = sqrt(1.0 + (0.125 * d) * (0.125 * d)) / d;
            double r[4][4], u1 = 0, s1 = 1;
            ref2x_t st;
            int v;
            char tag[64];
            /* naive 1x */
            for (i = 0; i < N; i++)
                y[i] = mk * nl(d * 0.5 * sin(6.283185307179586 * (double)((i * k) % N) / N));
            snprintf(tag, sizeof tag, "naive_%.0fdB_%.0fHz", DRV[di], f);
            alias_measure(y, N, k, &r[0][0], &r[0][1], &r[0][2], &r[0][3], (di == 1 && ki == 3) ? csv : NULL, tag);
            /* ADAA 1x (two periods: the first warms the state) */
            for (i = 0; i < 2 * N; i++) {
                double u = d * 0.5 * sin(6.283185307179586 * (double)((i * k) % N) / N), s = sqrt(1.0 + u * u);
                double o = (u + u1) / (s + s1);
                u1 = u, s1 = s;
                if (i >= N)
                    y[i - N] = mk * o;
            }
            alias_measure(y, N, k, &r[1][0], &r[1][1], &r[1][2], &r[1][3], NULL, "");
            /* 2x without ADAA: the same halfbands, the plain curve at 2x */
            memset(&st, 0, sizeof st);
            for (i = 0; i < 3 * N; i++) {
                double x = 0.5 * sin(6.283185307179586 * (double)((i * k) % N) / N), ca[4], cb[4], a, b, o;
                for (v = 0; v < 4; v++)
                    ca[v] = (double)(float)HB[2 * v], cb[v] = (double)(float)HB[2 * v + 1];
                a = ap4d(x, &st.s[0], ca), b = ap4d(x, &st.s[4], cb);
                o = 0.5 * mk * (ap4d(nl(d * b), &st.s[8], ca) + ap4d(nl(d * a), &st.s[12], cb));
                if (i >= 2 * N)
                    y[i - 2 * N] = o;
            }
            alias_measure(y, N, k, &r[2][0], &r[2][1], &r[2][2], &r[2][3], NULL, "");
            /* the real thing */
            cy_fx_init();
            cy_fx_set(&(cy_fx_params_t){(float)DRV[di], 0, 0, 2.8f, 0.55f, 0});
            for (i = 0; i < N + warm; i++)
                L[i] = R[i] = (float)(0.5 * sin(6.283185307179586 * (double)((i * k) % N) / N));
            run(L, R, N + warm, 4096u, q);
            for (i = 0; i < N; i++)
                y[i] = q[2 * (warm + i)] / QMAX;
            snprintf(tag, sizeof tag, "fx_%.0fdB_%.0fHz", DRV[di], f);
            alias_measure(y, N, k, &r[3][0], &r[3][1], &r[3][2], &r[3][3], (di == 1 && ki == 3) ? csv : NULL, tag);
            say("    %4.0fdB %8.1f | %8.1f / %8.1f   | %8.1f / %8.1f   | %8.1f / %8.1f   | %8.1f / %8.1f   | %6.1f  | %+6.1f dB\n",
                DRV[di], f, r[0][0], r[0][1], r[1][0], r[1][1], r[2][0], r[2][1], r[3][0], r[3][1], r[3][2], r[3][0] - r[0][0]);
            if (f > 2000.0)
                sum_imp += r[3][0] - r[0][0], cnt++;
            check(r[3][0] < r[0][0] - 10.0 || r[3][0] < -100.0, "drive %.0f dB, %.0f Hz: 2x ADAA %.1f dBc vs naive %.1f dBc (%.1f dB less aliasing)",
                  DRV[di], f, r[3][0], r[0][0], r[0][0] - r[3][0]);
            check(fabs(r[3][3] - r[0][3]) < 1.0, "  same saturation: fundamental %.2f dBFS (naive %.2f)", r[3][3], r[0][3]);
        }
    say("    mean alias reduction vs naive above 2 kHz: %.1f dB\n", -sum_imp / cnt);
    if (csv)
        fclose(csv);
    free(y), free(L), free(R), free(q);
}

/* ----------------------------------------------------------------------------------------------- 3. decay --- */
/* the T60 the absorbers give at f, averaged over the lines (the same formulas as fx.c, in double) */
static double t60_expected(double size, double damp, double f)
{
    static const double M[8] = {1499, 1787, 2053, 2357, 2633, 2939, 3251, 3697};
    double acc = 0;
    int k;
    for (k = 0; k < 8; k++) {
        double g = pow(10.0, -3.0 * M[k] / (size * FS)), gh = pow(10.0, -3.0 * M[k] / (size * damp * FS));
        double p = (g - gh) / (g + gh), w = 6.283185307179586 * f / FS;
        double mag = g * (1.0 - p) / sqrt(1.0 - 2.0 * p * cos(w) + p * p);
        acc += -3.0 * M[k] / (FS * log10(mag));
    }
    return acc / 8.0;
}

static size_t impulse_response(cy_fx_params_t p, double secs, int32_t **qout)
{
    size_t n = (size_t)(secs * FS), i;
    float *L = calloc(n, sizeof(float)), *R = calloc(n, sizeof(float));
    int32_t *q = malloc(2 * n * sizeof(int32_t));
    cy_fx_init();
    cy_fx_set(&p);
    run_silence(CY_BLOCK, 4096u, NULL);      /* the snap block: the targets are in */
    L[0] = R[0] = 0.5f;
    run(L, R, n, 4096u, q);
    (void)i;
    free(L), free(R);
    *qout = q;
    return n;
}

static void test_decay(void)
{
    static const float SZ[] = {0.3f, 0.6f, 1.2f, 2.8f, 6.0f, 12.0f};
    static const double BANDS[] = {250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0};
    size_t s, b;
    say("\n[3] FDN decay: impulse, ROOM 1, PREDELAY 0. Broadband T60 from the Schroeder EDC (-5..-35 dB) of L^2 + R^2\n"
        "    after 1,000 samples; T_mid = the mean of the 500 Hz and 1 kHz third-octave T60s (STFT, see t60_band)\n");
    for (s = 0; s < sizeof SZ / sizeof SZ[0]; s++) {
        int32_t *q;
        double secs = SZ[s] * 2.4 + 1.0, t60, tmid;
        size_t n = impulse_response(P(0, 0, 1, SZ[s], 1.0f, 0), secs, &q), i;
        double *pw = malloc(n * sizeof(double));
        for (i = 0; i < n; i++)
            pw[i] = i < 1000 ? 0.0 : (double)q[2 * i] * q[2 * i] + (double)q[2 * i + 1] * q[2 * i + 1];
        t60 = t60_edc(pw + 1000, n - 1000, -5.0, -35.0);
        tmid = 0.5 * (t60_band(q, n, 1000, 500.0) + t60_band(q, n, 1000, 1000.0));
        check(fabs(tmid / SZ[s] - 1.0) < 0.05 && fabs(t60 / SZ[s] - 1.0) < 0.10,
              "SIZE %5.1f s, DAMPING 100%%: T_mid %6.3f s (%+5.1f%%), broadband %6.3f s (%+5.1f%%)", SZ[s], tmid,
              100.0 * (tmid / SZ[s] - 1.0), t60, 100.0 * (t60 / SZ[s] - 1.0));
        free(pw), free(q);
    }
    say("    per third-octave band with DAMPING (T60 at Nyquist = SIZE x DAMPING), against the absorbers' own T60(f):\n");
    {
        static const float DP[] = {1.0f, 0.55f, 0.2f};
        static const float SZB[] = {2.8f, 2.8f, 6.0f};
        size_t di;
        for (di = 0; di < 3; di++) {
            int32_t *q;
            float size = SZB[di];
            size_t n = impulse_response(P(0, 0, 1, size, DP[di], 0), size * 2.4 + 1.0, &q);
            for (b = 0; b < sizeof BANDS / sizeof BANDS[0]; b++) {
                double t60 = t60_band(q, n, 1000, BANDS[b]), ex = t60_expected(size, DP[di], BANDS[b]);
                check(fabs(t60 / ex - 1.0) < 0.10, "SIZE %.1f s, DAMPING %3.0f%%, %5.0f Hz: T60 %6.3f s, expected %6.3f s (%+5.1f%%)",
                      size, DP[di] * 100.0, BANDS[b], t60, ex, 100.0 * (t60 / ex - 1.0));
            }
            free(q);
        }
    }
}

/* ------------------------------------------------------------------- 4. reverb level, stereo, the IR WAVs --- */
/* the wet energy of a unit impulse (sum of wet^2 per channel) and the L/R correlation of the wet tail */
static void wet_energy(float size, float damp, double *el, double *er, double *rho)
{
    int32_t *q;
    size_t n = impulse_response(P(0, 0, 1, size, damp, 0), size * 2.4 + 1.0, &q), i;
    double lr = 0;
    *el = *er = 0;
    for (i = 1000; i < n; i++) {
        double a = q[2 * i] / QMAX / 0.5 / 1.25, c = q[2 * i + 1] / QMAX / 0.5 / 1.25;   /* per unit impulse, wet only */
        *el += a * a, *er += c * c, lr += a * c;
    }
    *rho = lr / sqrt(*el * *er);
    free(q);
}
static void test_reverb_character(void)
{
    static const float SZ[] = {0.3f, 1.0f, 2.8f, 6.0f, 12.0f};
    double el, er, rho, e1ref;
    size_t s;
    say("\n[4] reverb level and character. E = the wet energy of a unit impulse (sum of wet^2). The level law: at DAMPING\n"
        "    100 %% E grows as SIZE^1/2 (fx.c sets the input gain from the absorbers' mean g^2); DAMPING then only takes\n"
        "    away highs, so darker small rooms are quieter. Calibration: E = 0.45 at the defaults (2.8 s, 55 %%), which is\n"
        "    a steady-state wet/dry of -10.9 dB at the default ROOM 30 %%.\n");
    wet_energy(2.8f, 1.0f, &el, &er, &rho);
    e1ref = 0.5 * (el + er);
    for (s = 0; s < sizeof SZ / sizeof SZ[0]; s++) {
        double want = e1ref * sqrt(SZ[s] / 2.8), e1, e55, rd;
        wet_energy(SZ[s], 1.0f, &el, &er, &rho);
        e1 = 0.5 * (el + er);
        check(fabs(10.0 * log10(e1 / want)) < 1.5, "SIZE %4.1f s, DAMPING 100%%: E %.3f vs the SIZE^1/2 law's %.3f (%+.2f dB)", SZ[s], e1,
              want, 10.0 * log10(e1 / want));
        wet_energy(SZ[s], 0.55f, &el, &er, &rho);
        e55 = 0.5 * (el + er);
        rd = (0.375 * 0.375 * e55) / (0.88 * 0.88);
        say("    SIZE %4.1f s, DAMPING  55%%: E L %.3f R %.3f (input gain %.3f): at ROOM 30%% wet/dry %+5.1f dB; tail L/R rho %+.3f\n",
            SZ[s], el, er, cy_fx_debug_gin(), 10.0 * log10(rd), rho);
        if (SZ[s] == 2.8f)
            check(fabs(10.0 * log10(e55 / 0.45)) < 0.5, "  the calibration: E %.3f at the defaults (want 0.45, %+.2f dB)", e55, 10.0 * log10(e55 / 0.45));
        check(fabs(rho) < 0.1, "  L/R tail decorrelated (|rho| %.3f < 0.1)", fabs(rho));
        check(fabs(10.0 * log10(el / er)) < 1.0, "  L/R balanced (%.2f dB)", 10.0 * log10(el / er));
    }
    {
        int32_t *q;
        size_t n;
        say("    impulse responses (impulse 0.5 = -6 dBFS at t = 0, MASTER full):\n");
        n = impulse_response(P(0, 0, 0.30f, 2.8f, 0.55f, 12), 6.0, &q);
        wav24("fx_ir_default.wav", q, n), free(q);
        n = impulse_response(P(0, 0, 1.0f, 2.8f, 0.55f, 12), 6.0, &q);
        wav24("fx_ir_room100_2s8.wav", q, n), free(q);
        n = impulse_response(P(0, 0, 1.0f, 8.0f, 0.45f, 30), 16.0, &q);
        wav24("fx_ir_cathedral_8s.wav", q, n), free(q);
        n = impulse_response(P(0, 0, 1.0f, 0.8f, 0.55f, 0), 3.0, &q);
        wav24("fx_ir_small_0s8.wav", q, n), free(q);
    }
}

/* ------------------------------------------------------------------------------------------- 5. stability --- */
static void test_stability(void)
{
    static const float DRV[] = {0, 12, 24}, TLT[] = {-6, 0, 6}, RM[] = {0, 0.5f, 1}, SZ[] = {0.3f, 2.8f, 12}, DP[] = {0.2f, 0.55f, 1},
                       PD[] = {0, 30, 60};
    const size_t n = 8192;
    float L[8192], R[8192];
    int32_t q[2 * 8192];
    int a, b, c, d, e, f, bad = 0, combos = 0;
    int32_t worst = 0;
    cy_fx_meter_t m;
    size_t i;
    say("\n[5] stability: all 729 corners of (DRIVE, TILT, ROOM, SIZE, DAMPING, PREDELAY), each 0.19 s of -6 dBFS noise with a\n"
        "    +12 dBFS burst, parameters jumping between corners with no reset; then 4,000 random jumps; then silence\n");
    cy_fx_init();
    for (a = 0; a < 3; a++) for (b = 0; b < 3; b++) for (c = 0; c < 3; c++) for (d = 0; d < 3; d++) for (e = 0; e < 3; e++) for (f = 0; f < 3; f++) {
        cy_fx_set(&(cy_fx_params_t){DRV[a], TLT[b], RM[c], SZ[d], DP[e], PD[f]});
        for (i = 0; i < n; i++) {
            double g = i > 2000 && i < 2400 ? 4.0 : 0.5;
            L[i] = (float)(g * grand()), R[i] = (float)(g * grand());
        }
        run(L, R, n, 4096u, q);
        worst = maxabs_q(q, 2 * n) > worst ? maxabs_q(q, 2 * n) : worst;
        cy_fx_meter(&m);
        bad += !(isfinite(m.peak_l) && isfinite(m.peak_r) && isfinite(m.gr_db));
        combos++;
    }
    cy_fx_meter(&m);
    check(bad == 0 && m.nan_resets == 0, "%d corners: meters finite, no NaN resets (%u), output peak %.3f dBFS", combos, m.nan_resets, db(worst / QMAX));
    for (a = 0; a < 4000; a++) {
        float l[CY_BLOCK], r[CY_BLOCK];
        int32_t o[2 * CY_BLOCK];
        cy_fx_set(&(cy_fx_params_t){(float)(24 * urand()), (float)(12 * urand() - 6), (float)urand(), (float)(0.3 + 11.7 * urand()),
                                    (float)(0.2 + 0.8 * urand()), (float)(60 * urand())});
        for (i = 0; i < CY_BLOCK; i++)
            l[i] = (float)(2.0 * grand()), r[i] = (float)(2.0 * grand());
        cy_fx_process(l, r, CY_BLOCK, (uint32_t)(4096 * urand()), o);
        worst = maxabs_q(o, 2 * CY_BLOCK) > worst ? maxabs_q(o, 2 * CY_BLOCK) : worst;
    }
    cy_fx_meter(&m);
    check(m.nan_resets == 0 && worst <= (int32_t)floor(CY_CEIL * QMAX + 0.5), "4,000 random jumps (MASTER random too): no NaN resets, peak %.3f dBFS", db(worst / QMAX));
    /* the longest, brightest tail must still die: SIZE 12 s, DAMPING 100 %, ROOM 100 %, then 70 s of silence */
    cy_fx_set(&(cy_fx_params_t){24, 6, 1, 12, 1, 60});
    for (i = 0; i < n; i++)
        L[i] = (float)(4.0 * grand()), R[i] = (float)(4.0 * grand());
    run(L, R, n, 4096u, q);
    {
        size_t blocks = (size_t)(70.0 * FS) / CY_BLOCK, k, last_nz = 0;
        int32_t o[2 * CY_BLOCK];
        double e10 = 0, e30 = 0;
        for (k = 0; k < blocks; k++) {
            float l[CY_BLOCK] = {0}, r[CY_BLOCK] = {0};
            cy_fx_process(l, r, CY_BLOCK, 4096u, o);
            for (i = 0; i < 2 * CY_BLOCK; i++) {
                if (o[i])
                    last_nz = k;
                if (k * CY_BLOCK > 5 * FS && k * CY_BLOCK < 6 * FS)
                    e10 += (double)o[i] * o[i];
                if (k * CY_BLOCK > 15 * FS && k * CY_BLOCK < 16 * FS)
                    e30 += (double)o[i] * o[i];
            }
        }
        check(last_nz * CY_BLOCK < 60 * FS, "SIZE 12 s tail after a hot burst: last nonzero output at %.1f s, then exact zeros (decays, no runaway)",
              last_nz * CY_BLOCK / FS);
        say("    (tail energy 5 s -> 15 s fell %.1f dB: %.1f dB per 12 s, SIZE asks 60)\n", 10 * log10(e10 / (e30 + 1e-30)),
            10 * log10(e10 / (e30 + 1e-30)) * 12.0 / 10.0);
    }
}

/* ------------------------------------------------------------------------------------------------- 6. NaN --- */
static void test_nan(void)
{
    float l[CY_BLOCK], r[CY_BLOCK];
    int32_t o[2 * CY_BLOCK];
    cy_fx_meter_t m;
    int k, zero = 1, alive = 0;
    size_t i;
    say("\n[6] NaN / Inf on the bus\n");
    cy_fx_init();
    cy_fx_set(&(cy_fx_params_t){12, 2, 0.6f, 4, 0.5f, 20});
    for (k = 0; k < 50; k++) {
        for (i = 0; i < CY_BLOCK; i++)
            l[i] = r[i] = (float)(0.3 * sin(0.05 * (k * CY_BLOCK + i)));
        cy_fx_process(l, r, CY_BLOCK, 4096u, o);
    }
    for (i = 0; i < CY_BLOCK; i++)
        l[i] = r[i] = 0.2f;
    l[17] = NAN;
    cy_fx_process(l, r, CY_BLOCK, 4096u, o);
    for (i = 0; i < 2 * CY_BLOCK; i++)
        zero &= o[i] == 0;
    cy_fx_meter(&m);
    check(m.nan_resets == 1 && zero, "one NaN: nan_resets %u, that block's output all zero (%s)", m.nan_resets, zero ? "yes" : "no");
    for (i = 0; i < CY_BLOCK; i++)
        l[i] = r[i] = 0.1f;
    r[3] = INFINITY;
    cy_fx_process(l, r, CY_BLOCK, 4096u, o);
    cy_fx_meter(&m);
    check(m.nan_resets == 2, "one +Inf: nan_resets %u", m.nan_resets);
    for (k = 0; k < 100; k++) {
        for (i = 0; i < CY_BLOCK; i++)
            l[i] = r[i] = (float)(0.3 * sin(0.05 * (k * CY_BLOCK + i)));
        cy_fx_process(l, r, CY_BLOCK, 4096u, o);
        for (i = 0; i < 2 * CY_BLOCK; i++)
            alive |= o[i] != 0;
    }
    cy_fx_meter(&m);
    check(alive && m.nan_resets == 2 && isfinite(m.peak_l), "afterwards: plays again, no further resets, meters finite (peak %.3f)", m.peak_l);
}

/* --------------------------------------------------------------------------------------------- 7. silence --- */
static void test_silence(void)
{
    static const cy_fx_params_t PS[] = {{0, 0, 0.3f, 2.8f, 0.55f, 12}, {24, 6, 1, 12, 1, 60}, {12, -6, 0.5f, 0.3f, 0.2f, 0}};
    size_t pi, i;
    say("\n[7] silence in -> silence out, and no subnormal anywhere in the fx state\n");
    for (pi = 0; pi < 3; pi++) {
        size_t n = (size_t)(10 * FS);
        int32_t *q = malloc(2 * n * sizeof(int32_t));
        cy_fx_init();
        cy_fx_set(&PS[pi]);
        run_silence(n, 4096u, q);
        check(maxabs_q(q, 2 * n) == 0 && cy_fx_debug_subnormals() == 0, "params %zu: 10 s of zeros -> %s, %u subnormal state floats", pi,
              maxabs_q(q, 2 * n) == 0 ? "exact zeros" : "NONZERO", cy_fx_debug_subnormals());
        free(q);
    }
    for (pi = 0; pi < 3; pi++) {
        size_t n = (size_t)(1 * FS), ns = (size_t)(40 * FS), last = 0, k;
        float *L = malloc(n * sizeof(float)), *R = malloc(n * sizeof(float));
        int32_t *q = malloc(2 * ns * sizeof(int32_t));
        uint32_t sub = 0;
        cy_fx_init();
        cy_fx_set(&PS[pi]);
        for (i = 0; i < n; i++)
            L[i] = (float)(0.5 * grand()), R[i] = (float)(0.5 * grand());
        run(L, R, n, 4096u, q);
        for (k = 0; k < ns / CY_BLOCK; k++) {
            run_silence(CY_BLOCK, 4096u, q + 2 * k * CY_BLOCK);
            if (k % 64 == 0)
                sub += cy_fx_debug_subnormals();
        }
        for (i = 0; i < 2 * (ns / CY_BLOCK) * CY_BLOCK; i++)
            if (q[i])
                last = i / 2;
        check(last < ns - (size_t)FS && sub == 0, "params %zu: 1 s of noise then 40 s of zeros: exact zeros from %.2f s on; %u subnormals over 216 scans",
              pi, last / FS, sub);
        free(L), free(R), free(q);
    }
}

/* --------------------------------------------------------------------------------------------- 8. latency --- */
static void test_latency(void)
{
    int32_t *q;
    size_t n, i, at = 0;
    int32_t mx = 0;
    say("\n[8] latency\n");
    n = impulse_response(P(0, 0, 0, 2.8f, 0.55f, 0), 0.1, &q);
    for (i = 0; i < n; i++)
        if (abs(q[2 * i]) > mx)
            mx = abs(q[2 * i]), at = i;
    check(at == cy_fx_latency() && abs(mx - (int32_t)(0.5 * QMAX + 0.5)) <= 1, "dry impulse: out at %zu samples (%.3f ms), cy_fx_latency() = %u, level %d (want %d)",
          at, at * 1000.0 / FS, cy_fx_latency(), mx, (int32_t)(0.5 * QMAX + 0.5));
    free(q);
    /* the engaged drive against the bypassed one, by the phase of a 215 Hz sine (both through the same 5 Hz DC
     * blocker, whose phase lead cancels in the difference) */
    {
        const size_t N = 1 << 15;
        float *L = malloc(N * sizeof(float)), *R = malloc(N * sizeof(float));
        int32_t *o = malloc(2 * N * sizeof(int32_t));
        double ph[2], w = 6.283185307179586 * 215.33203125 / FS;   /* bin 160 of 32768 */
        int e;
        for (e = 0; e < 2; e++) {
            double s = 0, c = 0;
            cy_fx_init();
            cy_fx_set(&(cy_fx_params_t){e ? 0.2f : 0.0f, 0, 0, 2.8f, 0.55f, 0});
            for (i = 0; i < N; i++)
                L[i] = R[i] = (float)(0.01 * sin(w * i));
            run(L, R, N, 4096u, o);
            for (i = N / 2; i < N; i++)
                s += o[2 * i] * sin(w * i), c += o[2 * i] * cos(w * i);
            ph[e] = -atan2(c, s) / w;
        }
        say("    215 Hz phase delay: bypassed %.3f, drive engaged (0.2 dB) %.3f samples: the wet path is %.3f samples\n"
            "    against the dry path's %u (halfbands 2.69 + ADAA 0.25 + droop FIR 2 at DC)\n", ph[0], ph[1], ph[1] - ph[0] + 5.0, 5u);
        check(fabs(ph[1] - ph[0]) < 0.25, "  engaged and bypassed latency agree within a quarter sample (%+.3f)", ph[1] - ph[0]);
        free(L), free(R), free(o);
    }
}

/* ---------------------------------------------------------------------------- 9. responses and gain staging --- */
static double sine_gain(cy_fx_params_t p, double f, double amp)
{
    const size_t N = 1 << 15;
    float *L = malloc(N * sizeof(float)), *R = malloc(N * sizeof(float));
    int32_t *o = malloc(2 * N * sizeof(int32_t));
    double s = 0, c = 0, w = 6.283185307179586 * f / FS;
    size_t i;
    cy_fx_init();
    cy_fx_set(&p);
    for (i = 0; i < N; i++)
        L[i] = R[i] = (float)(amp * sin(w * i));
    run(L, R, N, 4096u, o);
    for (i = N / 2; i < N; i++)
        s += o[2 * i] * sin(w * i), c += o[2 * i] * cos(w * i);
    free(L), free(R), free(o);
    return 2.0 * sqrt(s * s + c * c) / (N / 2) / QMAX / amp;
}
static void test_responses(void)
{
    static const double F[] = {100, 1000, 5000, 10000, 15000, 20000};
    size_t i;
    say("\n[9] small-signal responses (ROOM 0) and gain staging\n    f (Hz):               ");
    for (i = 0; i < 6; i++)
        say("%9.0f", F[i]);
    say("\n    bypass (dB):          ");
    for (i = 0; i < 6; i++)
        say("%9.3f", db(sine_gain(P(0, 0, 0, 2.8f, 0.55f, 0), F[i], 0.01)));
    say("\n    drive 0.2 dB (dB):    ");
    for (i = 0; i < 6; i++)
        say("%9.3f", db(sine_gain(P(0.2f, 0, 0, 2.8f, 0.55f, 0), F[i], 0.01)));
    say("   (ADAA-1 is a half-sample average at 2x: -%.2f dB at 20 kHz by design)\n", -db(cos(3.141592653589793 * 20000.0 / (2 * FS))));
    say("    tilt +6 (dB):         ");
    for (i = 0; i < 6; i++)
        say("%9.3f", db(sine_gain(P(0, 6, 0, 2.8f, 0.55f, 0), F[i], 0.01)));
    say("\n    tilt -6 (dB):         ");
    for (i = 0; i < 6; i++)
        say("%9.3f", db(sine_gain(P(0, -6, 0, 2.8f, 0.55f, 0), F[i], 0.01)));
    say("\n");
    check(fabs(db(sine_gain(P(0, 0, 0, 2.8f, 0.55f, 0), 1000, 0.01))) < 0.01, "bypass is unity at 1 kHz");
    check(fabs(db(sine_gain(P(0, 6, 0, 2.8f, 0.55f, 0), 20, 0.01)) + 3.0) < 0.3 && fabs(db(sine_gain(P(0, 6, 0, 2.8f, 0.55f, 0), 20000, 0.01)) - 3.0) < 0.3,
          "TILT +6: -3 dB at 20 Hz, +3 dB at 20 kHz (%.2f, %.2f)", db(sine_gain(P(0, 6, 0, 2.8f, 0.55f, 0), 20, 0.01)),
          db(sine_gain(P(0, 6, 0, 2.8f, 0.55f, 0), 20000, 0.01)));
    {
        double g0 = db(sine_gain(P(0, 0, 0, 2.8f, 0.55f, 0), 440, 0.125)), g24 = db(sine_gain(P(24, 0, 0, 2.8f, 0.55f, 0), 440, 0.125));
        double g9 = db(sine_gain(P(9, 0, 0, 2.8f, 0.55f, 0), 440, 0.125));
        check(fabs(g0) < 0.01 && fabs(g9) < 1.0 && fabs(g24) < 1.5, "a -18 dBFS 440 Hz sine keeps its level through DRIVE: 0 dB %+.2f, 9 dB %+.2f, 24 dB %+.2f dB",
              g0, g9, g24);
    }
}

/* ------------------------------------------------------------------------------------------- 10. smoothing --- */
/* the largest |second difference| in [a, b) of channel 0: a step or a corner shows up as a spike far above a sine's */
static double d2max(const int32_t *q, size_t a, size_t b)
{
    double m = 0;
    size_t i;
    for (i = a + 2; i < b; i++) {
        double d = fabs((double)q[2 * i] - 2.0 * q[2 * (i - 1)] + q[2 * (i - 2)]) / QMAX;
        if (d > m)
            m = d;
    }
    return m;
}
static void test_smoothing(void)
{
    static const struct { const char *what; cy_fx_params_t a, b; uint32_t ma, mb; } J[] = {
        {"DRIVE 0 -> 24 dB (engage)", {0, 0, 0, 2.8f, 0.55f, 0}, {24, 0, 0, 2.8f, 0.55f, 0}, 4096, 4096},
        {"DRIVE 24 -> 0 dB (bypass)", {24, 0, 0, 2.8f, 0.55f, 0}, {0, 0, 0, 2.8f, 0.55f, 0}, 4096, 4096},
        {"DRIVE 0.05 -> 0.15 dB (threshold)", {0.05f, 0, 0, 2.8f, 0.55f, 0}, {0.15f, 0, 0, 2.8f, 0.55f, 0}, 4096, 4096},
        {"TILT -6 -> +6", {0, -6, 0, 2.8f, 0.55f, 0}, {0, 6, 0, 2.8f, 0.55f, 0}, 4096, 4096},
        {"ROOM 0 -> 100 %", {0, 0, 0, 2.8f, 0.55f, 12}, {0, 0, 1, 2.8f, 0.55f, 12}, 4096, 4096},
        {"SIZE 0.3 -> 12 s (ROOM 100)", {0, 0, 1, 0.3f, 0.55f, 12}, {0, 0, 1, 12, 0.55f, 12}, 4096, 4096},
        {"DAMPING 20 -> 100 % (ROOM 100)", {0, 0, 1, 2.8f, 0.2f, 12}, {0, 0, 1, 2.8f, 1, 12}, 4096, 4096},
        {"PREDELAY 0 -> 60 ms (ROOM 100)", {0, 0, 1, 2.8f, 0.55f, 0}, {0, 0, 1, 2.8f, 0.55f, 60}, 4096, 4096},
        {"MASTER 4096 -> 1024", {0, 0, 0.3f, 2.8f, 0.55f, 12}, {0, 0, 0.3f, 2.8f, 0.55f, 12}, 4096, 1024},
        {"MASTER 0 -> 4096", {0, 0, 0.3f, 2.8f, 0.55f, 12}, {0, 0, 0.3f, 2.8f, 0.55f, 12}, 0, 4096},
    };
    const size_t n = (size_t)(3 * FS), t1 = (size_t)(1.5 * FS) / CY_BLOCK * CY_BLOCK;
    float *L = malloc(n * sizeof(float)), *R = malloc(n * sizeof(float));
    int32_t *q = malloc(2 * n * sizeof(int32_t));
    size_t j, i;
    say("\n[10] smoothing: a 220 Hz -12 dBFS sine; each parameter jumps at 1.5 s; max |second difference| in the 0.3 s after\n"
        "     the jump against the larger of the steady states before and after (a click is a spike well above both)\n");
    for (i = 0; i < n; i++)
        L[i] = R[i] = (float)(0.25 * sin(6.283185307179586 * 220.0 * i / FS));
    for (j = 0; j < sizeof J / sizeof J[0]; j++) {
        size_t b;
        double before, after, during;
        cy_fx_init();
        cy_fx_set(&J[j].a);
        for (b = 0; b < n / CY_BLOCK; b++) {
            float l[CY_BLOCK], r[CY_BLOCK];
            if (b * CY_BLOCK == t1)
                cy_fx_set(&J[j].b);
            memcpy(l, L + b * CY_BLOCK, sizeof l), memcpy(r, R + b * CY_BLOCK, sizeof r);
            cy_fx_process(l, r, CY_BLOCK, b * CY_BLOCK < t1 ? J[j].ma : J[j].mb, q + 2 * b * CY_BLOCK);
        }
        before = d2max(q, t1 - (size_t)(0.3 * FS), t1);
        during = d2max(q, t1, t1 + (size_t)(0.3 * FS));
        after = d2max(q, t1 + (size_t)(1.0 * FS), t1 + (size_t)(1.3 * FS));
        check(during <= 1.15 * fmax(before, after) + 2e-5, "%-34s during %.2e vs before %.2e / after %.2e", J[j].what, during, before, after);
    }
    free(L), free(R), free(q);
}

/* ------------------------------------------------------------------------------------ 11. determinism, cost --- */
static uint32_t hash_run(void)
{
    const size_t n = (size_t)(2 * FS);
    float *L = malloc(n * sizeof(float)), *R = malloc(n * sizeof(float));
    int32_t *q = malloc(2 * n * sizeof(int32_t));
    uint32_t h = 0x811C9DC5u, s = 0xBEEFu;
    size_t i;
    for (i = 0; i < n; i++) {
        s ^= s << 13, s ^= s >> 17, s ^= s << 5;
        L[i] = (float)((int32_t)(s >> 8) - 8388608) * (0.4f / 8388608.0f) * (i < n / 4 ? 1.0f : 0.0f);
        R[i] = L[i] * 0.7f;
    }
    cy_fx_init();
    cy_fx_set(&(cy_fx_params_t){6, 1, 0.5f, 3, 0.6f, 15});
    run(L, R, n, 3300u, q);
    for (i = 0; i < 2 * n; i++) {
        uint32_t v = (uint32_t)q[i], k;
        for (k = 0; k < 4; k++)
            h = (h ^ ((v >> (8 * k)) & 255u)) * 0x01000193u;
    }
    free(L), free(R), free(q);
    return h;
}
static void test_determinism_cost(void)
{
    uint32_t h1 = hash_run(), h2 = hash_run();
    say("\n[11] determinism and host cost\n");
    check(h1 == h2, "two runs after cy_fx_init: FNV-1a %08X == %08X", h1, h2);
    {
        const size_t n = (size_t)(20 * FS);
        float *L = malloc(n * sizeof(float)), *R = malloc(n * sizeof(float));
        int32_t *q = malloc(2 * n * sizeof(int32_t));
        size_t i;
        int v;
        for (i = 0; i < n; i++)
            L[i] = (float)(0.2 * grand()), R[i] = (float)(0.2 * grand());
        for (v = 0; v < 3; v++) {
            clock_t c0;
            double ns;
            cy_fx_init();
            cy_fx_set(&(cy_fx_params_t){v == 1 ? 9.0f : 0.0f, 0, 0.3f, 2.8f, 0.55f, 12});
            c0 = clock();
            if (v == 2)
                run_silence(n, 4096u, q);
            else
                run(L, R, n, 4096u, q);
            ns = (double)(clock() - c0) / CLOCKS_PER_SEC * 1e9 / n;
            say("    %-24s %6.1f ns per stereo sample (%.2f%% of real time on this host)\n",
                v == 0 ? "noise, drive off:" : v == 1 ? "noise, drive 9 dB:" : "silence, drive off:", ns, ns * FS / 1e7);
        }
        free(L), free(R), free(q);
    }
}

/* ------------------------------------------------------------------------------------ 12. demos (to hear) --- */
/* a cheap struck-metal voice: 6 inharmonic partials with their own decays (a stand-in for the engine), at -18 dBFS */
static void bells(float *L, float *R, size_t n, double every, double pan)
{
    static const double RATIO[6] = {1.0, 2.32, 4.25, 6.63, 9.38, 12.6}, DEC[6] = {2.2, 1.4, 0.9, 0.6, 0.4, 0.25};
    static const double ROOT[8] = {220.0, 293.66, 261.63, 329.63, 196.0, 392.0, 246.94, 174.61};
    size_t i;
    for (i = 0; i < n; i++) {
        double t = i / FS, xl = 0.0, xr = 0.0;
        int k, note, last = (int)(t / every);
        for (note = 0; note <= last; note++) {   /* every note rings on under the next (none is cut) */
            double tn = t - note * every, x = 0.0;
            for (k = 0; k < 6; k++)
                x += exp(-tn / DEC[k]) * sin(6.283185307179586 * ROOT[note % 8] * RATIO[k] * tn) / (1.0 + k);
            x *= 0.125 / 2.45 * (1.0 - exp(-tn * 3000.0));
            xl += x * (1.0 - pan * (note & 1)), xr += x * (1.0 - pan * !(note & 1));
        }
        L[i] = (float)xl, R[i] = (float)xr;
    }
}
static void test_demos(void)
{
    const size_t n = (size_t)(12 * FS);
    float *L = malloc(n * sizeof(float)), *R = malloc(n * sizeof(float));
    int32_t *q = malloc(2 * n * sizeof(int32_t));
    size_t b;
    say("\n[12] demos to listen to (a synthetic struck-metal voice standing in for the engine, -18 dBFS strikes)\n");
    bells(L, R, n, 1.5, 0.3);
    cy_fx_init();
    cy_fx_set(&(cy_fx_params_t){0, 0, 0.30f, 2.8f, 0.55f, 12});
    run(L, R, n, 4096u, q);
    wav24("fx_demo_defaults.wav", q, n);
    cy_fx_init();
    cy_fx_set(&(cy_fx_params_t){0, 0, 0.55f, 8.0f, 0.45f, 30});
    run(L, R, n, 4096u, q);
    wav24("fx_demo_cathedral.wav", q, n);
    cy_fx_init();                          /* DRIVE swept 0 -> 24 dB over 10 s, a block at a time, ROOM 30 % */
    for (b = 0; b < n / CY_BLOCK; b++) {
        float l[CY_BLOCK], r[CY_BLOCK], dd = (float)(24.0 * fmin(1.0, b * CY_BLOCK / (10.0 * FS)));
        cy_fx_set(&(cy_fx_params_t){dd, 0, 0.30f, 2.8f, 0.55f, 12});
        memcpy(l, L + b * CY_BLOCK, sizeof l), memcpy(r, R + b * CY_BLOCK, sizeof r);
        cy_fx_process(l, r, CY_BLOCK, 4096u, q + 2 * b * CY_BLOCK);
    }
    wav24("fx_demo_drive_sweep.wav", q, (n / CY_BLOCK) * CY_BLOCK);
    free(L), free(R), free(q);
}

int main(int argc, char **argv)
{
    char path[1024];
    if (argc > 1)
        outdir = argv[1];
    snprintf(path, sizeof path, "%s/fx_test.log", outdir);
    logf_ = fopen(path, "w");
    say("CYMA-1 fx acceptance tests (src/fx.c), CY_FS %.1f Hz, CY_BLOCK %u, cy_fx_latency %u\n", (double)CY_FS, CY_BLOCK, cy_fx_latency());
    test_limiter();
    test_aliasing();
    test_decay();
    test_reverb_character();
    test_stability();
    test_nan();
    test_silence();
    test_latency();
    test_responses();
    test_smoothing();
    test_determinism_cost();
    test_demos();
    say("\n%s: %d of %d checks passed\n", fails ? "FAIL" : "PASS", checks - fails, checks);
    if (logf_)
        fclose(logf_);
    return fails ? 1 : 0;
}
