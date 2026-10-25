/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 unit test for M1 (src/geom.c + src/plates_data.c), host only:
 *
 *   cc -O2 -std=c99 -ffp-contract=off -DPLAT_HOST -Iinstrument/src -Ireference/fm1-base/include \
 *      instrument/tests/unit/test_geom.c instrument/src/geom.c instrument/src/plates_data.c -lm -o test_geom
 *   ./test_geom [DUMP_DIR]
 *
 * Checks the spec §3.2 contract: morph continuity, field_at / sdf_at bilinear at cell centres, SDF and cover signs,
 * the published seq and double buffer, finite and bounded surface / rain / wander tables with every point on the
 * plate, the physics (Bessel at stiffness 0, Euler at 0.8, CARVE / SHIMMER / material law), the cover's 30 Hz
 * ceiling and settle rebuild, determinism, NaN inputs, and the §3.2 time budgets. Exit 0 = all passed.
 * With DUMP_DIR it also writes morph.bin (the field cache at shape 0, .125, .25 .. 8) and ratios.csv for the
 * evidence renders. plat_ms is this file's simulated clock (geom.c reads it for the cover's rate limit). */
#define _POSIX_C_SOURCE 200112L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cy_plates.h"

static uint32_t t_ms = 1000u;
uint32_t plat_ms(void) { return t_ms; }

static int n_check, n_fail;
#define CHECK(c, ...) do { n_check++; if (!(c)) { n_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

#define T_PI 3.14159265358979323846
static const double BESSEL[6] = {1.0, 1.5933, 2.1355, 2.2954, 2.6531, 2.9173};
static const double EULER[5] = {1.0, 2.7565, 5.4039, 8.9330, 13.3443};

static cy_geom_in_t defaults(void)
{
    cy_geom_in_t g;
    memset(&g, 0, sizeof g);
    g.shape = 1.0f, g.stiffness = 0.8f, g.shimmer_cents = 0.0f, g.material = 3.0f;
    g.pos_u = 0.62f, g.pos_v = 0.40f, g.mic_spread = 0.5f, g.rain_spread = 0.3f, g.wander = 0.1f;
    return g;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

static int finite_all(const float *p, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (!isfinite(p[i]))
            return 0;
    return 1;
}

/* the analytic shapes of spec §3.1, for the SDF sign checks (margin applied by the caller) */
static double sd_shape(int s, double u, double v)
{
    double du = u - 0.5, dv = v - 0.5, r = hypot(du, dv), d = -1e9, th;
    int k;
    switch (s) {
    case 0: case 1: return r - 0.46;
    case 2:
        for (k = 0; k < 6; k++) {
            double a = T_PI / 6 + k * T_PI / 3;
            d = fmax(d, du * cos(a) + dv * sin(a) - 0.47 * cos(T_PI / 6));
        }
        return d;
    case 3: return fmax(fabs(du), fabs(dv)) - 0.41;
    case 4:
        for (k = 0; k < 3; k++) {
            double a = T_PI / 2 + k * 2 * T_PI / 3;
            d = fmax(d, du * cos(a) + (v - 0.54) * sin(a) - 0.25);
        }
        return d;
    case 5:
        th = atan2(dv, du);
        return fmin(r - (0.31 + 0.15 * cos(5 * (th + T_PI / 2))), r - 0.20);
    case 6: {
        double lo = hypot(du, v - 0.66) - 0.25, up = hypot(du, v - 0.34) - 0.19;
        double w = fmax(fabs(du) - 0.15, fabs(dv) - 0.16);
        return fmin(fmin(lo, up), w);      /* (the smooth union only adds material near the joins) */
    }
    case 7: return hypot(u - fmin(fmax(u, 0.26), 0.74), dv) - 0.22;
    default: return fmax(fabs(du) - 0.45, fabs(dv) - 0.075);
    }
}

/* distinct ratios (merging within 0.5 %), ascending, of the published object */
static int distinct(const cy_object_t *o, double *out, int max)
{
    int n = 0, k;
    for (k = 0; k < CY_NMODES && n < max; k++) {
        double r = o->ratio[o->order[k]];
        if (n == 0 || r / out[n - 1] > 1.005)
            out[n++] = r;
    }
    return n;
}

static void test_init_and_seq(void)
{
    cy_geom_in_t g = defaults();
    const cy_object_t *a, *b;
    static cy_object_t snap;
    uint32_t s0;
    printf("seq and double buffer\n");
    cy_geom_init();
    a = cy_geom_object();
    CHECK(a->seq >= 1u, "init publishes an object (seq %u)", (unsigned)a->seq);
    CHECK(finite_all(a->ratio, CY_NMODES) && a->out_norm > 0.0f, "init object finite");
    cy_geom_update(&g);
    a = cy_geom_object();
    s0 = a->seq;
    cy_geom_update(&g);
    CHECK(cy_geom_object()->seq == s0 && cy_geom_object() == a, "unchanged input: no publish");
    memcpy(&snap, a, sizeof snap);
    g.pos_u = 0.70f;
    cy_geom_update(&g);
    b = cy_geom_object();
    CHECK(b->seq == s0 + 1u, "a change publishes seq + 1 (%u -> %u)", (unsigned)s0, (unsigned)b->seq);
    CHECK(b != a, "the publish swaps buffers");
    CHECK(memcmp(&snap, a, sizeof snap) == 0, "the previously published object is untouched by the update");
    g.shape = 2.5f;
    cy_geom_update(&g);
    CHECK(cy_geom_object()->seq == s0 + 2u && cy_geom_object() == a, "seq + 2, back on the first buffer");
    g.surface = 1;
    cy_geom_update(&g);
    CHECK(cy_geom_object()->seq == s0 + 2u, "SURFACE alone changes nothing geometric (keys always kept)");
}

static void test_bilinear(void)
{
    static cy_fieldcache_t fc;
    int m, i, j, bad = 0, bad2 = 0, n;
    double maxe = 0.0;
    printf("field_at / sdf_at bilinear\n");
    for (m = 0; m < CY_NMODES; m++)
        for (i = 0; i < CY_GRID; i++)
            for (j = 0; j < CY_GRID; j++)
                fc.field[m][i][j] = (int8_t)(-40 + m + 3 * j - 2 * i + (m & 1 ? i : 0));   /* linear, inside int8 */
    for (i = 0; i < CY_SDF; i++)
        for (j = 0; j < CY_SDF; j++)
            fc.sdf[i][j] = (int8_t)(-90 + 2 * j + i);
    for (m = 0; m < CY_NMODES; m++)                                    /* exact at the cell centres */
        for (i = 0; i < CY_GRID; i++)
            for (j = 0; j < CY_GRID; j++) {
                float f = cy_geom_field_at(&fc, m, ((float)j + 0.5f) / CY_GRID, ((float)i + 0.5f) / CY_GRID);
                bad += fabsf(f * 127.0f - (float)fc.field[m][i][j]) > 1e-4f;
            }
    CHECK(bad == 0, "field_at returns the stored value at every cell centre (%d off)", bad);
    for (n = 0; n < 20000; n++) {                                      /* a linear table is reproduced exactly */
        double u = 0.5 / CY_GRID + (23.0 / CY_GRID) * ((n * 7919) % 10007) / 10006.0;
        double v = 0.5 / CY_GRID + (23.0 / CY_GRID) * ((n * 104729) % 10009) / 10008.0;
        double x = u * CY_GRID - 0.5, y = v * CY_GRID - 0.5;
        int mm = n % CY_NMODES;
        double want = (-40 + mm + 3 * x - 2 * y + (mm & 1 ? y : 0)) / 127.0;
        double e = fabs(cy_geom_field_at(&fc, mm, (float)u, (float)v) - want);
        if (e > maxe) maxe = e;
        x = u * 2 * CY_GRID - 0.5, y = v * 2 * CY_GRID - 0.5;          /* (the same u, v on the 48 grid) */
        want = (-90 + 2 * x + y) / 128.0;
        bad2 += fabs(cy_geom_sdf_at(&fc, (float)u, (float)v) - want) > 2e-5;
    }
    CHECK(maxe < 2e-5, "field_at is bilinear between centres (max err %.2e)", maxe);
    CHECK(bad2 == 0, "sdf_at is bilinear between centres (%d off)", bad2);
    CHECK(fabsf(cy_geom_field_at(&fc, 3, -0.5f, -0.5f) - (float)fc.field[3][0][0] / 127.0f) < 1e-6f
          && fabsf(cy_geom_field_at(&fc, 3, 1.5f, 1.5f) - (float)fc.field[3][23][23] / 127.0f) < 1e-6f
          && fabsf(cy_geom_field_at(&fc, 99, 0.01f, 0.99f) - (float)fc.field[31][23][0] / 127.0f) < 1e-6f,
          "field_at clamps to the outer centres and the mode range");
    CHECK(fabsf(cy_geom_sdf_at(&fc, 0.0f, 0.0f) - (float)fc.sdf[0][0] / 128.0f) < 1e-6f
          && fabsf(cy_geom_sdf_at(&fc, 1.0f, 1.0f) - (float)fc.sdf[47][47] / 128.0f) < 1e-6f
          && cy_geom_sdf_at(&fc, NAN, NAN) == (float)fc.sdf[0][0] / 128.0f, "sdf_at clamps (NaN to the corner)");
}

static void test_shapes(void)
{
    cy_geom_in_t g = defaults();
    int s, n, bad, k, i;
    printf("SDF sign, cover, tables per keyframe\n");
    for (s = 0; s < CY_NSHAPES; s++) {
        const cy_object_t *o;
        const cy_fieldcache_t *fc;
        int cov_bad = 0, inside_bad = 0;
        g.shape = (float)s;
        t_ms += 200u;
        cy_geom_update(&g);
        o = cy_geom_object();
        fc = cy_geom_fields();
        bad = 0;
        for (n = 0; n < 4000; n++) {                       /* points clearly in or out of the analytic shape */
            double u = ((n * 7919) % 4001) / 4000.0, v = ((n * 6271) % 3989) / 3988.0, d = sd_shape(s, u, v);
            float e = cy_geom_sdf_at(fc, (float)u, (float)v);
            if (fabs(d) < 0.02) continue;
            bad += (d < 0) != (e < 0.0f);
            if (d < 0 && s != 5 && s != 6 && fabs(e - d) > 0.02) bad++;   /* the distance too where the analytic
                                                       form is exact (inside; not STAR's radial form, not GUITAR's joins) */
        }
        CHECK(bad == 0, "%s: SDF sign and distance match the analytic shape (%d off)", cy_geom_shape_name(s), bad);
        for (i = 0; i < CY_COVER; i++)
            for (k = 0; k < CY_COVER; k++) {
                float d = cy_geom_sdf_at(fc, ((float)k + 0.5f) / CY_COVER, ((float)i + 0.5f) / CY_COVER) * 92.0f;
                int want = (int)(fminf(fmaxf(0.5f - d, 0.0f), 1.0f) * 255.0f + 0.5f);
                cov_bad += abs((int)fc->cover[i][k] - want) > 1;
            }
        CHECK(cov_bad == 0 && fc->cover[CY_COVER / 2][CY_COVER / 2] == 255 && fc->cover[0][0] == 0,
              "%s: cover = clamp(.5 - d_px/2) 255 everywhere (%d off), 255 at the centre, 0 in the corner",
              cy_geom_shape_name(s), cov_bad);
        CHECK(finite_all(&o->ratio[0], CY_NMODES * 3) && finite_all(o->strike, CY_NMODES * 3)
              && finite_all(&o->surface[0][0], CY_NKEYS * CY_NMODES) && finite_all(&o->rain[0][0], CY_RAIN_PTS * CY_NMODES)
              && finite_all(&o->wander[0][0], CY_WANDER_PTS * CY_NMODES) && isfinite(o->out_norm) && isfinite(o->t0),
              "%s: every table finite", cy_geom_shape_name(s));
        bad = 0;
        for (k = 0; k < CY_NMODES; k++) {
            for (i = 0; i < CY_NKEYS; i++) bad += fabsf(o->surface[i][k]) > 1.0001f;
            for (i = 0; i < CY_RAIN_PTS; i++) bad += fabsf(o->rain[i][k]) > 1.0001f;
            for (i = 0; i < CY_WANDER_PTS; i++) bad += fabsf(o->wander[i][k]) > 1.0001f;
            bad += fabsf(o->strike[k]) > 1.0001f;
        }
        CHECK(bad == 0, "%s: |strike|, |surface|, |rain|, |wander| <= 1", cy_geom_shape_name(s));
        for (i = 0; i < CY_NKEYS; i++) inside_bad += cy_geom_sdf_at(fc, o->key_uv[i][0], o->key_uv[i][1]) >= 0.0f;
        for (i = 0; i < CY_RAIN_PTS; i++) inside_bad += cy_geom_sdf_at(fc, o->rain_uv[i][0], o->rain_uv[i][1]) >= 0.0f;
        for (i = 0; i < CY_WANDER_PTS; i++) inside_bad += cy_geom_sdf_at(fc, o->wander_uv[i][0], o->wander_uv[i][1]) >= 0.0f;
        for (i = 0; i < 2; i++) inside_bad += cy_geom_sdf_at(fc, o->mic_uv[i][0], o->mic_uv[i][1]) >= 0.0f;
        inside_bad += cy_geom_sdf_at(fc, o->pos_uv[0], o->pos_uv[1]) >= 0.0f;
        CHECK(inside_bad == 0, "%s: keys, rain, wander, mics and the strike all on the plate (%d off)",
              cy_geom_shape_name(s), inside_bad);
        CHECK(fabsf(o->clamped - (s == 0 ? 1.0f : 0.0f)) < 1e-6f, "%s: clamped %g", cy_geom_shape_name(s),
              (double)o->clamped);
        CHECK(o->out_norm > 0.02f && o->out_norm < 5.0f, "%s: out_norm %.4f in range", cy_geom_shape_name(s),
              (double)o->out_norm);
        for (k = 0; k < CY_NMODES; k++)
            CHECK(o->degen[k] == CY_PLATES[s].degen[k], "%s: degen published", cy_geom_shape_name(s));
    }
    {   /* extreme POS on the thin BAR: the spec's 50 radial steps are not enough there; the fallback must be */
        const cy_object_t *o;
        int out = 0;
        g = defaults();
        g.shape = 8.0f, g.pos_u = 1.0f, g.pos_v = 1.0f, g.rain_spread = 1.0f, g.wander = 1.0f;
        cy_geom_update(&g);
        o = cy_geom_object();
        for (i = 0; i < CY_NKEYS; i++) out += cy_geom_sdf_at(cy_geom_fields(), o->key_uv[i][0], o->key_uv[i][1]) >= 0.0f;
        for (i = 0; i < CY_RAIN_PTS; i++) out += cy_geom_sdf_at(cy_geom_fields(), o->rain_uv[i][0], o->rain_uv[i][1]) >= 0.0f;
        CHECK(out == 0, "BAR with POS (1,1), spread 1: every key and drop still on the plate (%d off)", out);
        g.shape = 5.0f, g.pos_u = 0.0f, g.pos_v = 0.0f;
        cy_geom_update(&g);
        o = cy_geom_object();
        for (out = 0, i = 0; i < CY_RAIN_PTS; i++)
            out += cy_geom_sdf_at(cy_geom_fields(), o->rain_uv[i][0], o->rain_uv[i][1]) >= 0.0f;
        CHECK(out == 0, "STAR with POS (0,0), spread 1: every drop on the plate (%d off)", out);
    }
}

static void test_continuity(void)
{
    cy_geom_in_t g = defaults();
    static float prev_lr[CY_NMODES], prev_on;
    static int8_t prev_f[CY_NMODES][CY_GRID][CY_GRID];
    float max_lr = 0.0f, max_on = 0.0f, max_lr_sh = 0.0f;
    int n, k, max_f = 0, pass, edge_bad = 0;
    printf("morph continuity\n");
    for (pass = 0; pass < 2; pass++) {                  /* pass 1: with SHIMMER (twin groups blended across t) */
        g = defaults();
        g.shimmer_cents = pass ? 15.0f : 0.0f;
        for (n = 0; n <= 8000; n++) {
            const cy_object_t *o;
            g.shape = (float)n * 0.001f;
            t_ms += 16u;
            cy_geom_update(&g);
            o = cy_geom_object();
            if (n > 0) {
                for (k = 0; k < CY_NMODES; k++) {
                    float d = fabsf(o->log2_ratio[k] - prev_lr[k]);
                    if (pass) { if (d > max_lr_sh) max_lr_sh = d; }
                    else if (d > max_lr) max_lr = d;
                }
                if (!pass) {
                    float r = fabsf(o->out_norm / prev_on - 1.0f);
                    const cy_fieldcache_t *fc = cy_geom_fields();
                    int i, j;
                    if (r > max_on) max_on = r;
                    for (k = 0; k < CY_NMODES; k++)
                        for (i = 0; i < CY_GRID; i++)
                            for (j = 0; j < CY_GRID; j++) {
                                int d = abs(fc->field[k][i][j] - prev_f[k][i][j]);
                                if (d > max_f) max_f = d;
                            }
                }
            }
            memcpy(prev_lr, o->log2_ratio, sizeof prev_lr);
            prev_on = o->out_norm;
            memcpy(prev_f, cy_geom_fields()->field, sizeof prev_f);
        }
    }
    CHECK(max_lr < 0.02f, "log2 ratio moves <= 0.02 oct per 0.001 of SHAPE (max %.4f)", (double)max_lr);
    CHECK(max_lr_sh < 0.02f, "with SHIMMER 15c too (max %.4f)", (double)max_lr_sh);
    CHECK(max_on < 0.01f, "out_norm moves < 1 %% per 0.001 of SHAPE (max %.2f %%)", 100.0 * max_on);
    CHECK(max_f <= 2, "field cache moves <= 2 LSB per 0.001 of SHAPE (max %d)", max_f);
    for (k = 1; k < CY_NSHAPES - 1; k++) {               /* the keyframe joins: just below == at */
        float lr[CY_NMODES], f1, f2;
        const cy_object_t *o;
        int m;
        g = defaults();
        g.shape = (float)k - 1e-4f;
        cy_geom_update(&g);
        o = cy_geom_object();
        memcpy(lr, o->log2_ratio, sizeof lr);
        f1 = cy_geom_field_at(cy_geom_fields(), 3, 0.31f, 0.47f);
        g.shape = (float)k;
        cy_geom_update(&g);
        o = cy_geom_object();
        f2 = cy_geom_field_at(cy_geom_fields(), 3, 0.31f, 0.47f);
        for (m = 0; m < CY_NMODES; m++)
            edge_bad += fabsf(lr[m] - o->log2_ratio[m]) > 2e-3f;
        edge_bad += fabsf(f1 - f2) > 2.0f / 127.0f;
    }
    CHECK(edge_bad == 0, "continuous across every keyframe join (%d off)", edge_bad);
}

static void test_physics(void)
{
    cy_geom_in_t g = defaults();
    const cy_object_t *o;
    double d[CY_NMODES], err = 0.0;
    int n, k, s, bad;
    printf("physics\n");
    g.shape = 0.0f, g.stiffness = 0.0f;                  /* DRUM membrane = Bessel (golden test 8.3.1) */
    cy_geom_update(&g);
    o = cy_geom_object();
    n = distinct(o, d, 6);
    for (k = 0; k < 6 && k < n; k++) err = fmax(err, fabs(d[k] / BESSEL[k] - 1.0));
    CHECK(n == 6 && err < 0.03, "DRUM at stiffness 0 within 3 %% of Bessel (max %.2f %%)", 100.0 * err);
    g.shape = 8.0f, g.stiffness = 0.8f;                  /* BAR plate = Euler within 1 % */
    cy_geom_update(&g);
    o = cy_geom_object();
    n = distinct(o, d, 5);
    for (err = 0.0, k = 0; k < 5 && k < n; k++) err = fmax(err, fabs(d[k] / EULER[k] - 1.0));
    CHECK(n == 5 && err < 0.01, "BAR at stiffness 0.8 within 1 %% of Euler (max %.2f %%): %.4f %.4f %.4f %.4f %.4f",
          100.0 * err, d[0], d[1], d[2], d[3], d[4]);
    for (s = 0; s < CY_NSHAPES; s++) {                   /* stiffness laws against the stored Lambda */
        double e0 = 0.0, e8 = 0.0, e1 = 0.0;
        float r8[CY_NMODES];
        g = defaults();
        g.shape = (float)s;
        g.stiffness = 0.0f;
        cy_geom_update(&g);
        o = cy_geom_object();
        for (k = 0; k < CY_NMODES; k++)
            e0 = fmax(e0, fabs(o->ratio[k] / sqrt(exp2(CY_PLATES[s].log2_lambda[k])) - 1.0));
        g.stiffness = 0.8f;
        cy_geom_update(&g);
        o = cy_geom_object();
        for (k = 0; k < CY_NMODES; k++) {
            r8[k] = o->ratio[k];
            e8 = fmax(e8, fabs(o->ratio[k] / exp2(CY_PLATES[s].log2_lambda[k]) - 1.0));
        }
        g.stiffness = 1.0f;
        cy_geom_update(&g);
        o = cy_geom_object();
        for (k = 0; k < CY_NMODES; k++)
            e1 = fmax(e1, fabs(o->ratio[k] / pow(r8[k], 1.25) - 1.0));
        CHECK(e0 < 1e-5 && e8 < 0.01 && e1 < 1e-4, "%s: stiffness 0 = sqrt(L) (%.1e), 0.8 = L within 1 %% (%.2f %%), "
              "1 = plate^1.25 (%.1e)", cy_geom_shape_name(s), e0, 100.0 * e8, e1);
        CHECK(o->ratio[o->order[0]] == 1.0f, "%s: the lowest ratio is exactly 1", cy_geom_shape_name(s));
        for (bad = 0, k = 1; k < CY_NMODES; k++) bad += o->ratio[o->order[k]] < o->ratio[o->order[k - 1]];
        CHECK(bad == 0, "%s: order ascends", cy_geom_shape_name(s));
    }
    {   /* CARVE 100 %: every group lands on its target by rank (spec 3.2.4), groups past a list stay put */
        static const float BELL[11] = {1, 2, 2.4f, 3, 4, 5.333333f, 6, 8, 10.666667f, 12, 16}, BARL[5] = {1, 4, 10, 20, 32};
        float pre[CY_NMODES], gm[CY_NMODES];
        int cnt[CY_NMODES], rank[CY_NMODES], t, sh;
        for (sh = 3; sh <= 8; sh += 5) {                 /* SQUARE (many exact twins) and BAR */
            const uint8_t *dg = CY_PLATES[sh].degen;
            g = defaults();
            g.shape = (float)sh;
            cy_geom_update(&g);
            memcpy(pre, cy_geom_object()->ratio, sizeof pre);
            for (k = 0; k < CY_NMODES; k++) gm[k] = 0.0f, cnt[k] = 0;
            for (k = 0; k < CY_NMODES; k++) gm[dg[k]] += pre[k], cnt[dg[k]]++;
            for (k = 0; k < CY_NMODES; k++) if (cnt[k]) gm[k] /= (float)cnt[k];
            for (k = 0; k < CY_NMODES; k++) {            /* a group's rank: groups with a lower mean ratio */
                int j;
                rank[k] = 0;
                if (!cnt[k]) continue;
                for (j = 0; j < CY_NMODES; j++)
                    rank[k] += cnt[j] && (gm[j] < gm[k] || (gm[j] == gm[k] && j < k));
            }
            for (t = 0; t < CY_NCARVE; t++) {
                g.carve = 1.0f, g.carve_target = (uint8_t)t;
                cy_geom_update(&g);
                o = cy_geom_object();
                for (bad = 0, k = 0; k < CY_NMODES; k++) {
                    int rk = rank[dg[k]];
                    float want = pre[k];
                    if (t == CY_CARVE_HARMONIC) want = rk == 0 ? 1.0f : fmaxf(2.0f, roundf(gm[dg[k]]));
                    else if (t == CY_CARVE_BELL && rk < 11) want = BELL[rk];
                    else if (t == CY_CARVE_BAR && rk < 5) want = BARL[rk];
                    bad += fabsf(o->ratio[k] / want - 1.0f) > 2e-5f;
                }
                CHECK(bad == 0, "%s CARVE %s 100 %%: every group on its target by rank, the rest unchanged (%d off)",
                      cy_geom_shape_name(sh), t == 0 ? "HARMONIC" : t == 1 ? "BELL" : "BAR", bad);
                g.carve = 0.5f;                          /* half way: the geometric mean of ratio and target */
                cy_geom_update(&g);
                o = cy_geom_object();
                for (bad = 0, k = 0; k < CY_NMODES; k++) {
                    int rk = rank[dg[k]];
                    float want = pre[k];
                    if (t == CY_CARVE_HARMONIC) want = rk == 0 ? 1.0f : fmaxf(2.0f, roundf(gm[dg[k]]));
                    else if (t == CY_CARVE_BELL && rk < 11) want = BELL[rk];
                    else if (t == CY_CARVE_BAR && rk < 5) want = BARL[rk];
                    bad += fabsf(o->ratio[k] / sqrtf(pre[k] * want) - 1.0f) > 2e-5f;
                }
                CHECK(bad == 0, "%s CARVE %s 50 %%: log-halfway to the target (%d off)", cy_geom_shape_name(sh),
                      t == 0 ? "HARMONIC" : t == 1 ? "BELL" : "BAR", bad);
            }
        }
    }
    {   /* SHIMMER: a twin pair splits by exactly the setting, centred */
        float base[CY_NMODES];
        int pairs = 0;
        g = defaults();
        g.shape = 3.0f;
        cy_geom_update(&g);
        memcpy(base, cy_geom_object()->log2_ratio, sizeof base);
        g.shimmer_cents = 10.0f;
        cy_geom_update(&g);
        o = cy_geom_object();
        for (bad = 0, k = 0; k < CY_NMODES; k++) {
            int j, m = 0, idx = 0;
            for (j = 0; j < CY_NMODES; j++) {
                if (CY_PLATES[3].degen[j] == CY_PLATES[3].degen[k]) {
                    if (j < k) idx++;
                    m++;
                }
            }
            if (m == 2) {
                float want = (idx ? 5.0f : -5.0f) / 1200.0f;
                pairs++;
                bad += fabsf(o->log2_ratio[k] - base[k] - want) > 2e-5f;
            } else if (m == 1) {
                bad += fabsf(o->log2_ratio[k] - base[k]) > 2e-5f;
            }
        }
        CHECK(bad == 0 && pairs >= 8, "SHIMMER 10c: each of %d twins moves -5c / +5c, singles stay", pairs / 2);
    }
    {   /* the material law */
        static const float T0[6] = {0.18f, 0.80f, 2.2f, 5.0f, 10.0f, 28.0f}, BETA[6] = {3.0f, 1.5f, 0.35f, 0.12f, 0.06f, 0.015f};
        static const float GAM[6] = {1.6f, 1.5f, 1.1f, 0.9f, 0.75f, 0.5f};
        int m;
        g = defaults();
        for (bad = 0, m = 0; m < 6; m++) {
            g.material = (float)m, g.bright = (m & 1) ? 0.5f : -0.25f;
            cy_geom_update(&g);
            o = cy_geom_object();
            bad += fabsf(o->t0 / T0[m] - 1.0f) > 1e-4f;
            bad += fabsf(o->damp_div[o->order[0]] - 1.0f) > 1e-5f;
            for (k = 0; k < CY_NMODES; k++) {
                double want = 1.0 + BETA[m] * pow(4.0, -g.bright) * (pow(o->ratio[k], GAM[m]) - 1.0);
                bad += fabs(o->damp_div[k] / fmax(want, 0.1) - 1.0) > 2e-4;
            }
        }
        CHECK(bad == 0, "material anchors: t0 exact, damp_div = 1 + beta 4^-bright (ratio^gamma - 1), lowest 1");
        g.material = 2.5f, g.bright = 0.0f;
        cy_geom_update(&g);
        CHECK(fabsf(cy_geom_object()->t0 - sqrtf(2.2f * 5.0f)) < 1e-3f, "material 2.5: t0 the geometric mean (%.4f)",
              (double)cy_geom_object()->t0);
    }
    {   /* the points */
        float st[CY_NMODES];
        int same = 0;
        g = defaults();
        g.rain_spread = 0.0f, g.wander = 0.0f, g.mic_spread = 0.0f;
        cy_geom_update(&g);
        o = cy_geom_object();
        memcpy(st, o->strike, sizeof st);
        for (bad = 0, k = 0; k < CY_NMODES; k++) {
            bad += fabsf(o->rain[5][k] - st[k]) > 1e-6f || fabsf(o->wander[7][k] - st[k]) > 1e-6f;
            bad += fabsf(o->strike[k] - cy_geom_field_at(cy_geom_fields(), k, 0.62f, 0.40f)) > 1e-5f;
            same += o->mic_l[k] == o->mic_r[k];
        }
        CHECK(bad == 0, "strike = field at POS; rain / wander at spread 0 = the strike");
        CHECK(same < CY_NMODES / 2 + 8, "mics at one point still differ by their far-field signs (%d of 32 equal)", same);
        CHECK(fabsf(o->key_uv[12][0] - (0.10f + 0.80f * 7.0f / 15.0f)) < 1e-6f && fabsf(o->key_uv[12][1] - 0.66f) < 1e-6f
              && fabsf(o->key_uv[13][0] - (0.10f + 0.80f * 7.5f / 15.0f)) < 1e-6f && fabsf(o->key_uv[13][1] - 0.36f) < 1e-6f,
              "keybed: key 12 (C4, white 7) and key 13 (C#4, after white 7) where the panel puts them");
        g.pos_u = 0.72f;
        cy_geom_update(&g);
        o = cy_geom_object();
        CHECK(fabsf(o->key_uv[12][0] - (0.10f + 0.80f * 7.0f / 15.0f + 0.05f)) < 1e-6f, "POS pans the keybed by half");
        for (bad = 0, k = 0; k < CY_NMODES; k++)
            bad += fabsf(o->surface[12][k] - cy_geom_field_at(cy_geom_fields(), k, o->key_uv[12][0], o->key_uv[12][1])) > 1e-5f;
        CHECK(bad == 0, "surface[key][m] = field at the key");
    }
}

static void test_cover_rate(void)
{
    static uint8_t prev[CY_COVER][CY_COVER];
    cy_geom_in_t g = defaults();
    int n, rebuilds = 0, i, k, bad = 0;
    printf("cover rate limit\n");
    g.shape = 2.0f;
    t_ms += 1000u;
    cy_geom_update(&g);
    memcpy(prev, cy_geom_fields()->cover, sizeof prev);
    for (n = 1; n <= 100; n++) {                          /* moving every 5 ms for 500 ms */
        g.shape = 2.0f + 0.01f * (float)n;
        t_ms += 5u;
        cy_geom_update(&g);
        if (memcmp(prev, cy_geom_fields()->cover, sizeof prev)) {
            rebuilds++;
            memcpy(prev, cy_geom_fields()->cover, sizeof prev);
        }
    }
    CHECK(rebuilds >= 10 && rebuilds <= 16, "moving 500 ms at 200 Hz: %d cover rebuilds (<= 30 Hz)", rebuilds);
    for (n = 0; n < 12; n++) {                            /* then still: the settled cover lands within 100 ms */
        t_ms += 10u;
        cy_geom_update(&g);
    }
    for (i = 0; i < CY_COVER; i++)
        for (k = 0; k < CY_COVER; k++) {
            float d = cy_geom_sdf_at(cy_geom_fields(), ((float)k + 0.5f) / CY_COVER, ((float)i + 0.5f) / CY_COVER) * 92.0f;
            int want = (int)(fminf(fmaxf(0.5f - d, 0.0f), 1.0f) * 255.0f + 0.5f);
            bad += abs((int)cy_geom_fields()->cover[i][k] - want) > 1;
        }
    CHECK(bad == 0, "after settling, the cover matches the current SDF (%d cells off)", bad);
}

static uint32_t hash_obj(void)
{
    const uint8_t *p = (const uint8_t *)cy_geom_object();
    uint32_t h = 0x811C9DC5u, i;
    for (i = 4; i < sizeof(cy_object_t); i++)            /* (not seq) */
        h = (h ^ p[i]) * 0x01000193u;
    p = (const uint8_t *)cy_geom_fields();
    for (i = 4; i < sizeof(cy_fieldcache_t); i++)
        h = (h ^ p[i]) * 0x01000193u;
    return h;
}

static void test_determinism_and_nan(void)
{
    cy_geom_in_t g = defaults();
    uint32_t h[2];
    int r, n;
    printf("determinism, NaN\n");
    for (r = 0; r < 2; r++) {
        uint32_t s = 12345u;
        t_ms = 5000u;
        cy_geom_init();
        for (n = 0; n < 300; n++) {
            g.shape = 8.0f * cy_randf(&s), g.stiffness = cy_randf(&s), g.carve = cy_randf(&s);
            g.carve_target = (uint8_t)(cy_rand(&s) % 3u), g.shimmer_cents = 15.0f * cy_randf(&s);
            g.material = 5.0f * cy_randf(&s), g.bright = 2.0f * cy_randf(&s) - 1.0f;
            g.pos_u = cy_randf(&s), g.pos_v = cy_randf(&s), g.mic_spread = cy_randf(&s);
            g.rain_spread = cy_randf(&s), g.wander = cy_randf(&s);
            t_ms += 40u;
            cy_geom_update(&g);
        }
        h[r] = hash_obj();
    }
    CHECK(h[0] == h[1], "two identical runs build identical objects and caches (%08x %08x)", (unsigned)h[0], (unsigned)h[1]);
    g.shape = NAN, g.stiffness = NAN, g.carve = NAN, g.shimmer_cents = INFINITY, g.material = -INFINITY;
    g.bright = NAN, g.pos_u = NAN, g.pos_v = NAN, g.mic_spread = NAN, g.rain_spread = NAN, g.wander = NAN;
    g.carve_target = 200;
    cy_geom_update(&g);
    {
        const cy_object_t *o = cy_geom_object();
        int ok = finite_all(&o->ratio[0], CY_NMODES * 3) && finite_all(o->strike, CY_NMODES * 3)
                 && finite_all(&o->surface[0][0], CY_NKEYS * CY_NMODES + (CY_RAIN_PTS + CY_WANDER_PTS) * CY_NMODES)
                 && isfinite(o->out_norm) && isfinite(o->t0);
        CHECK(ok, "NaN / Inf / out-of-range inputs give a finite object");
    }
}

static void test_budget(void)
{
    cy_geom_in_t g = defaults();
    double t0, full, pos, same;
    int n, N = 4000;
    printf("budgets (native)\n");
    cy_geom_update(&g);
    t0 = now_s();
    for (n = 0; n < N; n++) {                             /* a moving shape, cover rebuilt every time (worst case) */
        g.shape = (n & 1) ? 2.3f : 2.7f;
        t_ms += 40u;
        cy_geom_update(&g);
    }
    full = (now_s() - t0) / N;
    t0 = now_s();
    for (n = 0; n < N; n++) {
        g.pos_u = (n & 1) ? 0.55f : 0.65f;
        cy_geom_update(&g);
    }
    pos = (now_s() - t0) / N;
    t0 = now_s();
    for (n = 0; n < N * 10; n++)
        cy_geom_update(&g);
    same = (now_s() - t0) / (N * 10);
    printf("  full rebuild %.1f us, pos-only %.2f us, unchanged %.3f us\n", full * 1e6, pos * 1e6, same * 1e6);
    CHECK(full < 300e-6, "full rebuild <= 0.3 ms (%.1f us)", full * 1e6);
    CHECK(pos < 50e-6, "pos-only rebuild <= 0.05 ms (%.2f us)", pos * 1e6);
    CHECK(same < 5e-6, "unchanged call <= 5 us (%.3f us)", same * 1e6);
}

static void dump(const char *dir)
{
    char path[1024];
    FILE *f, *c;
    cy_geom_in_t g = defaults();
    int n, k;
    snprintf(path, sizeof path, "%s/morph.bin", dir);
    f = fopen(path, "wb");
    snprintf(path, sizeof path, "%s/ratios.csv", dir);
    c = fopen(path, "w");
    if (!f || !c) {
        printf("  (dump: cannot write to %s)\n", dir);
        return;
    }
    fprintf(c, "shape,out_norm,t0,clamped");
    for (k = 0; k < CY_NMODES; k++) fprintf(c, ",r%d", k);
    fprintf(c, "\n");
    for (n = 0; n <= 64; n++) {                           /* shape 0 .. 8 in eighths; the cover settled each time */
        const cy_object_t *o;
        g.shape = (float)n * 0.125f;
        t_ms += 200u;
        cy_geom_update(&g);
        t_ms += 200u;
        cy_geom_update(&g);
        o = cy_geom_object();
        fwrite(cy_geom_fields(), sizeof(cy_fieldcache_t), 1, f);
        fwrite(o, sizeof(cy_object_t), 1, f);
        fprintf(c, "%.3f,%.6f,%.4f,%.3f", (double)g.shape, (double)o->out_norm, (double)o->t0, (double)o->clamped);
        for (k = 0; k < CY_NMODES; k++) fprintf(c, ",%.6f", (double)o->ratio[k]);
        fprintf(c, "\n");
    }
    fclose(f);
    fclose(c);
    printf("  dumped %s/morph.bin (65 x (%u + %u) B) and ratios.csv\n", dir, (unsigned)sizeof(cy_fieldcache_t),
           (unsigned)sizeof(cy_object_t));
}

int main(int argc, char **argv)
{
    printf("test_geom: sizeof cy_plate_t %u (x9 = %u), cy_object_t %u, cy_fieldcache_t %u\n",
           (unsigned)sizeof(cy_plate_t), 9u * (unsigned)sizeof(cy_plate_t), (unsigned)sizeof(cy_object_t),
           (unsigned)sizeof(cy_fieldcache_t));
    CHECK(sizeof(cy_plate_t) == 21040u, "cy_plate_t is the spec's 21,040 B");
    CHECK(strcmp(cy_geom_shape_name(0), "DRUM") == 0 && strcmp(cy_geom_shape_name(8), "BAR") == 0
          && strcmp(cy_geom_shape_short(6), "GTR") == 0 && strcmp(cy_geom_shape_name(99), "BAR") == 0,
          "shape names");
    test_init_and_seq();
    test_bilinear();
    test_shapes();
    test_continuity();
    test_physics();
    test_cover_rate();
    test_determinism_and_nan();
    test_budget();
    if (argc > 1)
        dump(argv[1]);
    printf("test_geom: %d checks, %d failed\n", n_check, n_fail);
    return n_fail ? 1 : 0;
}
