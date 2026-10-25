/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 M4 acceptance: the parameter table, curves, steps, formats, page map, presets, lerp, patch and LFO.
 *
 *   cc -O2 -std=c99 -ffp-contract=off -DPLAT_HOST -Wall -Wextra -Iinstrument/src -Ireference/fm1-base/include \
 *      [-I<app>/build/gen] instrument/tests/unit/test_params.c instrument/src/params.c instrument/src/presets.c -lm
 *   ./a.out [--dump FILE]
 *
 * With build/gen on the include path (any built copy of the app has it) the pixel widths are measured with the real
 * CY fonts, exactly as gfx.c's text_w and cv_text place glyphs; without it those checks are skipped and said so.
 * --dump writes the format table, the decoded presets and the hints, for the evidence folder and for review.
 * Exit 0 = every check passed. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cy_params.h"

/* ----------------------------------------------------------- harness --- */
static int t_fail, t_checks;
#define CHECK(c, ...)                                                                                              \
    do {                                                                                                           \
        t_checks++;                                                                                                \
        if (!(c)) {                                                                                                \
            if (t_fail < 60) {                                                                                     \
                printf("  FAIL %s:%d: ", __func__, __LINE__);                                                      \
                printf(__VA_ARGS__);                                                                               \
                printf("\n");                                                                                      \
            }                                                                                                      \
            t_fail++;                                                                                              \
        }                                                                                                          \
    } while (0)

static uint32_t t_rng = 0x12345678u;
static float frand(void) { return cy_randf(&t_rng); }

static int grid_of(int id)
{
    const cy_param_def_t *d = &CY_PARAMS[id];
    if (d->curve == CY_CV_ENUM)
        return d->detents - 1;
    if (d->curve == CY_CV_MORPH)
        return d->detents * (int)(d->hi - d->lo + 0.5f);
    return d->detents;
}
static int is_int(int id) { return id == P_MODES || id == P_VEL_CURVE || id == P_PRESS_SENS || id == P_GRAINS; }
static int is_discrete(int id) { return CY_PARAMS[id].curve == CY_CV_ENUM || is_int(id); }
static float range_of(int id) { return CY_PARAMS[id].hi - CY_PARAMS[id].lo; }

/* ------------------------------------------------------------- fonts --- */
#if defined(__has_include)
#if __has_include("cy_fonts.h")
#define T_FONTS 1
#endif
#endif
#ifdef T_FONTS
typedef struct {
    uint8_t h, pad, first, last;
    const uint8_t *adv, *bw;
    const uint16_t *off;
    const uint8_t *data;
} felucca_font_t;
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-const-variable"
#endif
#include "cy_fonts.h"
static uint32_t f_glyph(const felucca_font_t *f, uint32_t ch)   /* gfx.c glyph() */
{
    if (ch >= 'a' && ch <= 'z' && f->last < 'a')
        ch -= 32u;
    if (ch < f->first || ch > f->last || (ch >= 127u && ch < 160u))
        ch = '?';
    return ch - f->first;
}
/* the right edge of the ink of s drawn at x = 0 (cv_text: bitmap starts pad px left of the pen), tracking extra */
static int ink_w(const felucca_font_t *f, const char *s, int track)
{
    int pen = 0, right = 0;
    for (; *s; s++) {
        uint32_t g = f_glyph(f, (uint8_t)*s), w = f->bw[g], bpr = (w + 1u) / 2u, x, y;
        const uint8_t *gd = f->data + f->off[g];
        int r = -1;
        for (y = 0; y < f->h; y++)
            for (x = 0; x < w; x++) {
                uint32_t a = gd[y * bpr + x / 2u];
                a = (x & 1u) ? (a & 15u) : (a >> 4);
                if (a && (int)x > r)
                    r = (int)x;
            }
        if (r >= 0 && pen - f->pad + r + 1 > right)
            right = pen - f->pad + r + 1;
        pen += f->adv[g] + track;
    }
    return right;
}
#endif

/* UI budgets (spec §6.4, §6.7): see the format contract at the top of params.c */
#define W_CELL 52         /* knob cell value: x0+6 .. x0+57 in FONT_CY_VAL, x0+58 clear of the separator */
#define W_LONG 168        /* long form in FONT_CY_VAL inside the 184 px plate box, 8 px margins */
#define W_HINT 176        /* hint line, FONT_CY_LBL untracked, 4 px margins */
static const char HERO_SET[] = " %+-.0123456789:*";
static int in_hero(char c) { return c && strchr(HERO_SET, c) != NULL; }

/* ------------------------------------------------------- the table --- */
static void test_table(void)
{
    int id, i;
    for (id = 0; id < P_NPARAMS; id++) {
        const cy_param_def_t *d = &CY_PARAMS[id];
        CHECK(d->name && d->name[0] && strlen(d->name) <= 10, "name of %d", id);
        for (i = 0; d->name && d->name[i]; i++)
            CHECK((d->name[i] >= 'A' && d->name[i] <= 'Z') || d->name[i] == ' ' || d->name[i] == '>',
                  "%s: name char", d->name);
        CHECK(d->unit != NULL && strlen(d->unit) <= 2, "%s: unit", d->name);
        CHECK(d->lo < d->hi, "%s: lo < hi", d->name);
        CHECK(d->def >= d->lo && d->def <= d->hi, "%s: def in range", d->name);
        CHECK(d->curve <= CY_CV_MORPH, "%s: curve", d->name);
        CHECK(d->detents > 0, "%s: detents", d->name);
        CHECK(d->curve != CY_CV_LOG || d->lo > 0.0f, "%s: LOG lo > 0", d->name);
        CHECK(!d->bipolar || (d->lo < 0.0f && d->hi > 0.0f && fabsf(d->lo + d->hi) < 1e-6f), "%s: bipolar symmetric",
              d->name);
        if (d->curve == CY_CV_ENUM || d->curve == CY_CV_MORPH) {
            int n = d->curve == CY_CV_ENUM ? d->detents : (int)(d->hi - d->lo + 0.5f) + 1;
            CHECK(d->labels != NULL, "%s: labels", d->name);
            for (i = 0; d->labels && i < n; i++)
                CHECK(d->labels[i] && d->labels[i][0] && strlen(d->labels[i]) <= 10, "%s: label %d", d->name, i);
        } else {
            CHECK(d->labels == NULL, "%s: no labels on a number", d->name);
        }
        CHECK(id < P_NPRESET ? (id <= P_KEYMODE ? d->page == PG_N : d->page < PG_GLOBAL) : d->page == PG_GLOBAL,
              "%s: page %d", d->name, d->page);
        CHECK(d->curve != CY_CV_LIN || !is_int(id) || fabsf(fmodf(range_of(id), (float)d->detents)) < 1e-6f,
              "%s: int step", d->name);
    }
    /* spec §4.1/4.2 spot checks */
    CHECK(CY_PARAMS[P_SHAPE].curve == CY_CV_MORPH && CY_PARAMS[P_SHAPE].detents == 16, "SHAPE MORPH 16/seg");
    CHECK(CY_PARAMS[P_DECAY].curve == CY_CV_LOG && CY_PARAMS[P_DECAY].lo == 0.1f, "DECAY LOG");
    CHECK(CY_PARAMS[P_DAMPING].lo == 20.0f && CY_PARAMS[P_DAMPING].detents == 80, "DAMPING 20..100/80");
    CHECK(CY_PARAMS[P_VIB_RATE].detents == 60 && CY_PARAMS[P_VIB_RATE].def == 5.0f, "VIB RATE");
    CHECK(cy_param_value(P_VOICES, cy_param_norm(P_VOICES, CY_PARAMS[P_VOICES].def)) == 8.0f, "VOICES def 8");
#ifdef PLAT_HOST
    CHECK(CY_PARAMS[P_GRAINS].def == 2000.0f && CY_PARAMS[P_DETAIL].def == 128.0f, "host defaults 2000 / 128");
#endif
    CHECK(sizeof(cy_preset_t) == 94u, "cy_preset_t is %u B (spec 94)", (unsigned)sizeof(cy_preset_t));
    CHECK(sizeof(cy_user_bank_t) == 3020u, "cy_user_bank_t is %u B (spec 3020)", (unsigned)sizeof(cy_user_bank_t));
}

/* ---------------------------------------------------- norm <-> value --- */
static void test_roundtrip(void)
{
    int id, i;
    float worst = 0.0f;
    for (id = 0; id < P_NPARAMS; id++) {
        const cy_param_def_t *d = &CY_PARAMS[id];
        int D = grid_of(id);
        float prev = -1e30f;
        for (i = 0; i <= 4000; i++) {
            float n = (float)i / 4000.0f, v = cy_param_value(id, n), n2 = cy_param_norm(id, v), e;
            CHECK(v >= d->lo && v <= d->hi, "%s: value %g out of range at %g", d->name, v, n);
            CHECK(v >= prev, "%s: not monotonic at %g", d->name, n);
            prev = v;
            if (is_discrete(id)) {                  /* norm -> its grid point, and the same value back */
                float g = floorf(n * (float)D + 0.5f) / (float)D;
                CHECK(fabsf(n2 - g) < 1e-6f, "%s: discrete norm %g -> %g (grid %g)", d->name, n, n2, g);
                CHECK(cy_param_value(id, n2) == v, "%s: discrete value", d->name);
            } else {
                e = fabsf(n2 - n);
                if (d->curve == CY_CV_MORPH && fabsf(v - floorf(v + 0.5f)) < 1e-6f)
                    e = fabsf(n2 - n) < 2e-5f ? 0.0f : e;   /* (the keyframe snap is <= 1e-4 / range) */
                if (e > worst)
                    worst = e;
                CHECK(e < 2e-5f, "%s: round trip %g -> %g -> %g", d->name, n, v, n2);
            }
        }
        for (i = 0; i <= D; i++) {                  /* every grid value survives value -> norm -> value */
            float n = (float)i / (float)D, v = cy_param_value(id, n), v2 = cy_param_value(id, cy_param_norm(id, v));
            CHECK(fabsf(v2 - v) <= 1e-4f * fmaxf(1.0f, fabsf(v)), "%s: grid value %g -> %g", d->name, v, v2);
        }
        CHECK(cy_param_value(id, -0.5f) == d->lo && cy_param_value(id, 1.5f) == cy_param_value(id, 1.0f),
              "%s: clamps norm", d->name);
        CHECK(cy_param_value(id, NAN) == d->lo, "%s: NaN norm reads lo", d->name);
        CHECK(cy_param_norm(id, d->lo - 1000.0f) == 0.0f && cy_param_norm(id, d->hi + 1000.0f) == 1.0f,
              "%s: clamps value", d->name);
        CHECK(cy_param_norm(id, NAN) == 0.0f, "%s: NaN value", d->name);
        CHECK(fabsf(cy_param_value(id, 1.0f) - d->hi) <= 1e-5f * fabsf(d->hi) + 1e-6f, "%s: n=1 is hi", d->name);
        CHECK(cy_param_value(id, 0.0f) == d->lo, "%s: n=0 is lo", d->name);
    }
    CHECK(cy_param_value(-1, 0.5f) == 0.0f && cy_param_value(P_NPARAMS, 0.5f) == 0.0f, "bad id value");
    CHECK(cy_param_norm(-1, 0.5f) == 0.0f, "bad id norm");
    /* anchors from the spec */
    CHECK(fabsf(cy_param_value(P_DECAY, 0.5f) - 1.0f) < 1e-5f, "DECAY mid = x1");
    CHECK(cy_param_value(P_VOICES, 0.5f) == 6.0f && cy_param_value(P_DETAIL, 0.5f) == 96.0f, "enum values");
    CHECK(cy_param_value(P_MODES, 0.51f) == 20.0f, "MODES integer: %g", cy_param_value(P_MODES, 0.51f));
    CHECK(cy_param_value(P_GRAINS, 0.45f) == 1200.0f, "GRAINS steps of 100: %g", cy_param_value(P_GRAINS, 0.45f));
    CHECK(cy_param_value(P_SHAPE, 1.0f / 8.0f + 5e-6f) == 1.0f, "SHAPE parks on the keyframe");
    printf("  round trip: worst continuous |n' - n| = %.2e\n", (double)worst);
}

/* -------------------------------------------------------------- pages --- */
static void test_pages(void)
{
    int seen[P_NPARAMS] = {0}, pg, x, l, k, id;
    for (pg = 0; pg < PG_N; pg++)
        for (x = 0; x < CY_NEXC; x++)
            for (l = 0; l < 2; l++)
                for (k = 0; k < 4; k++) {
                    int j;
                    id = cy_page_param(pg, x, l, k);
                    CHECK(id >= P_MATERIAL && id < P_NPARAMS, "page %d exc %d layer %d knob %d -> %d", pg, x, l, k, id);
                    if (id < 0 || id >= P_NPARAMS)
                        continue;
                    seen[id] = 1;
                    CHECK(CY_PARAMS[id].page == pg, "%s on page %d but its hue is page %d", CY_PARAMS[id].name, pg,
                          CY_PARAMS[id].page);
                    for (j = 0; j < k; j++)
                        CHECK(cy_page_param(pg, x, l, j) != id, "duplicate %s", CY_PARAMS[id].name);
                    if (l)
                        for (j = 0; j < 4; j++)
                            CHECK(cy_page_param(pg, x, 0, j) != id, "%s on both layers", CY_PARAMS[id].name);
                }
    for (id = P_MATERIAL; id < P_NPARAMS; id++)
        CHECK(seen[id], "%s reachable from a page", CY_PARAMS[id].name);
    CHECK(!seen[P_SHAPE] && !seen[P_EXCITER] && !seen[P_KEYMODE], "SHAPE/EXCITER/KEYS are not knob params");
    /* spec §5.2 */
    CHECK(cy_page_param(PG_EXCITE, CY_EXC_MALLET, 0, 0) == P_HARDNESS, "MALLET A1");
    CHECK(cy_page_param(PG_EXCITE, CY_EXC_BOW, 0, 0) == P_FORCE, "BOW A1");
    CHECK(cy_page_param(PG_EXCITE, CY_EXC_RAIN, 0, 0) == P_DENSITY, "RAIN A1");
    CHECK(cy_page_param(PG_EXCITE, CY_EXC_MALLET, 0, 3) == P_TENSION, "MALLET A4");
    CHECK(cy_page_param(PG_EXCITE, CY_EXC_BOW, 0, 3) == P_GRIT, "BOW A4");
    CHECK(cy_page_param(PG_EXCITE, CY_EXC_RAIN, 0, 3) == P_SPREAD, "RAIN A4");
    CHECK(cy_page_param(PG_EXCITE, CY_EXC_MALLET, 1, 3) == P_PALM, "MALLET B4");
    CHECK(cy_page_param(PG_EXCITE, CY_EXC_BOW, 1, 3) == P_SWELL && cy_page_param(PG_EXCITE, CY_EXC_RAIN, 1, 3) == P_SWELL,
          "BOW/RAIN B4");
    CHECK(cy_page_param(PG_EXCITE, CY_EXC_RAIN, 0, 1) == P_POS_X && cy_page_param(PG_EXCITE, CY_EXC_BOW, 1, 2) == P_BLOOM,
          "shared EXCITE cells");
    CHECK(cy_page_param(PG_PLATE, 0, 1, 0) == P_CARVE_TARGET && cy_page_param(PG_SPACE, 0, 1, 3) == P_LEVEL &&
              cy_page_param(PG_MOTION, 0, 1, 1) == P_LFO_POS && cy_page_param(PG_GLOBAL, 0, 1, 3) == P_DETAIL,
          "layer B cells");
    CHECK(cy_page_param(-1, 0, 0, 0) == -1 && cy_page_param(PG_N, 0, 0, 0) == -1 && cy_page_param(0, 0, 0, 4) == -1 &&
              cy_page_param(0, 0, 0, -1) == -1, "invalid page / knob -> -1");
    CHECK(cy_page_param(PG_EXCITE, 7, 0, 0) == P_HARDNESS && cy_page_param(PG_PLATE, 0, 5, 0) == P_CARVE_TARGET,
          "out-of-range exciter -> MALLET, any layer != 0 -> B");
    CHECK(!strcmp(cy_page_name(PG_PLATE), "PLATE") && !strcmp(cy_page_name(PG_EXCITE), "EXCITE") &&
              !strcmp(cy_page_name(PG_SPACE), "SPACE") && !strcmp(cy_page_name(PG_MOTION), "MOTION") &&
              !strcmp(cy_page_name(PG_GLOBAL), "GLOBAL") && cy_page_name(99) && cy_page_name(-3),
          "page names");
}

/* -------------------------------------------------------------- steps --- */
static int stops_of(int id)    /* how many dwell detents a full sweep 0 -> 1 meets */
{
    const cy_param_def_t *d = &CY_PARAMS[id];
    if (d->curve == CY_CV_MORPH)
        return 2 * ((int)(d->hi - d->lo + 0.5f) - 1);
    if (d->curve != CY_CV_ENUM && d->bipolar && d->detents >= 20)
        return 3;
    return 0;
}
static void test_steps(void)
{
    int id, i;
    for (id = 0; id < P_NPARAMS; id++) {
        const cy_param_def_t *d = &CY_PARAMS[id];
        int D = grid_of(id), want = D + stops_of(id), n_up = 0, n_dn = 0, holds = 0, lim = 4 * want + 20;
        uint8_t st = 0;
        float n = 0.0f, prev = 0.0f;
        while (n < 1.0f && n_up < lim) {
            n = cy_param_step(id, n, 1.0f, &st);
            n_up++;
            CHECK(n >= prev, "%s: step up went down", d->name);
            if (n == prev)
                holds++;
            else
                CHECK(fabsf((n - prev) * (float)D - 1.0f) < 1e-3f, "%s: step %g != 1/%d", d->name, n - prev, D);
            prev = n;
        }
        CHECK(n_up == want, "%s: 0 -> 1 in %d detents, want %d", d->name, n_up, want);
        CHECK(holds == stops_of(id), "%s: %d dwell detents, want %d", d->name, holds, stops_of(id));
        CHECK(cy_param_step(id, 1.0f, 1.0f, &st) == 1.0f, "%s: clamps at 1", d->name);
        st = 0;
        while (n > 0.0f && n_dn < lim) {
            n = cy_param_step(id, n, -1.0f, &st);
            n_dn++;
        }
        CHECK(n_dn == want, "%s: 1 -> 0 in %d detents, want %d", d->name, n_dn, want);
        CHECK(cy_param_step(id, 0.0f, -3.0f, &st) == 0.0f, "%s: clamps at 0", d->name);
        /* an accelerated detent (x3, x8) is the same as that many single detents, dwell included */
        for (i = 0; i < 200; i++) {
            float n0 = frand(), a = n0, b = n0, mul = (i & 1) ? 8.0f : 3.0f, dir = (i & 2) ? -1.0f : 1.0f;
            uint8_t sa = 0, sb = 0;
            int j;
            if (i & 4)                               /* half the time start on the grid */
                n0 = a = b = floorf(n0 * (float)D + 0.5f) / (float)D;
            a = cy_param_step(id, a, dir * mul, &sa);
            for (j = 0; j < (int)mul; j++)
                b = cy_param_step(id, b, dir, &sb);
            CHECK(a == b && sa == sb, "%s: step x%g from %g: %g vs %g", d->name, (double)mul, n0, a, b);
        }
        CHECK(cy_param_step(id, 0.37f, 0.0f, &st) == 0.37f && cy_param_step(id, 0.37f, NAN, &st) == 0.37f,
              "%s: no-op steps", d->name);
    }
    {   /* MORPH dwell: SHAPE 0 -> 16 detents to CIRCLE, then 2 detents held, then on to 1 + 1/16 */
        uint8_t st = 0;
        float n = 0.0f;
        for (i = 0; i < 16; i++)
            n = cy_param_step(P_SHAPE, n, 1.0f, &st);
        CHECK(cy_param_value(P_SHAPE, n) == 1.0f, "SHAPE reaches CIRCLE in 16 detents (%g)", cy_param_value(P_SHAPE, n));
        n = cy_param_step(P_SHAPE, n, 1.0f, &st);
        n = cy_param_step(P_SHAPE, n, 1.0f, &st);
        CHECK(cy_param_value(P_SHAPE, n) == 1.0f, "SHAPE dwells 2 detents at CIRCLE");
        n = cy_param_step(P_SHAPE, n, 1.0f, &st);
        CHECK(fabsf(cy_param_value(P_SHAPE, n) - 1.0625f) < 1e-5f, "SHAPE leaves CIRCLE on the 3rd");
        /* turning back out of a dwell leaves at once */
        st = 0;
        n = 15.0f / 128.0f;
        n = cy_param_step(P_SHAPE, n, 1.0f, &st);      /* arrive at 1 */
        n = cy_param_step(P_SHAPE, n, -1.0f, &st);     /* back */
        CHECK(fabsf(cy_param_value(P_SHAPE, n) - 0.9375f) < 1e-5f, "reverse leaves a dwell at once");
        /* starting on a keyframe (a preset) moves at once: the dwell is for passing through */
        st = 0;
        n = cy_param_step(P_SHAPE, 2.0f / 8.0f, 1.0f, &st);
        CHECK(fabsf(cy_param_value(P_SHAPE, n) - 2.0625f) < 1e-5f, "a loaded keyframe moves at once");
        /* a stale sticky byte (the value changed since) is ignored */
        st = CY_PARAMS[P_SHAPE].detents ? (uint8_t)(0x80u | 2u) : 0u;
        n = cy_param_step(P_SHAPE, 0.3f, 1.0f, &st);
        CHECK(n > 0.3f, "stale sticky ignored");
    }
    {   /* bipolar zero: BRIGHT +4 -> +2 -> 0 (a detent is 2 of its 200), then 3 held, then -2 */
        uint8_t st = 0;
        float n = cy_param_norm(P_BRIGHT, 4.0f);
        n = cy_param_step(P_BRIGHT, n, -1.0f, &st);
        n = cy_param_step(P_BRIGHT, n, -1.0f, &st);
        CHECK(n == 0.5f && cy_param_value(P_BRIGHT, n) == 0.0f, "BRIGHT lands on 0");
        for (i = 0; i < 3; i++)
            n = cy_param_step(P_BRIGHT, n, -1.0f, &st);
        CHECK(n == 0.5f, "BRIGHT holds 0 for 3 detents");
        n = cy_param_step(P_BRIGHT, n, -1.0f, &st);
        CHECK(fabsf(cy_param_value(P_BRIGHT, n) + 2.0f) < 1e-4f, "BRIGHT leaves 0 on the 4th (%g)",
              cy_param_value(P_BRIGHT, n));
        st = 0;                                         /* VEL CURVE: whole steps, no dwell */
        n = cy_param_step(P_VEL_CURVE, cy_param_norm(P_VEL_CURVE, 1.0f), -1.0f, &st);
        n = cy_param_step(P_VEL_CURVE, n, -1.0f, &st);
        CHECK(cy_param_value(P_VEL_CURVE, n) == -1.0f, "VEL CURVE crosses 0 without a dwell");
    }
    {   /* an off-grid value lands on the grid: DECAY *1.6 */
        uint8_t st = 0;
        float n0 = cy_param_norm(P_DECAY, 1.6f), up = cy_param_step(P_DECAY, n0, 1.0f, &st),
              dn = cy_param_step(P_DECAY, n0, -1.0f, &st);
        CHECK(fabsf(up - 0.61f) < 1e-6f && fabsf(dn - 0.60f) < 1e-6f, "off-grid DECAY %g -> %g / %g", n0, up, dn);
        CHECK(fabsf(cy_param_step(P_POS_X, 0.5f, 0.5f, &st) - 0.505f) < 1e-6f, "a fractional detent");
    }
    {   /* ENUM: one value per detent, clamped (the app wraps ALGORITHM itself) */
        uint8_t st = 0;
        float n = cy_param_step(P_LFO_WAVE, 0.0f, 1.0f, &st);
        CHECK(cy_param_value(P_LFO_WAVE, n) == 1.0f, "LFO WAVE +1");
        CHECK(cy_param_value(P_LFO_WAVE, cy_param_step(P_LFO_WAVE, n, 2.0f, &st)) == 3.0f, "LFO WAVE +2");
        CHECK(cy_param_step(P_VOICES, 1.0f, 1.0f, &st) == 1.0f, "VOICES clamps at 8");
        CHECK(cy_param_value(P_VOICES, cy_param_step(P_VOICES, 1.0f, -1.0f, &st)) == 6.0f, "VOICES 8 -> 6");
        CHECK(cy_param_value(P_EXCITER, cy_param_step(P_EXCITER, 0.0f, 0.4f, &st)) == 1.0f, "a partial enum detent moves 1");
    }
    CHECK(cy_param_step(P_NPARAMS, 0.4f, 1.0f, 0) == 0.4f, "bad id step");
    CHECK(cy_param_step(P_ROOM, 0.4f, 1.0f, 0) > 0.4f, "NULL sticky is fine");
}

/* ------------------------------------------------------------- formats --- */
static void fmt(int id, float v, char *s, char *l)   /* by display value */
{
    float n = cy_param_norm(id, v);
    cy_param_format(id, n, s, 24, 0);
    cy_param_format(id, n, l, 24, 1);
}
#define EXPECT_FMT(id, v, ws, wl)                                                                                  \
    do {                                                                                                           \
        char s_[24], l_[24];                                                                                       \
        fmt(id, v, s_, l_);                                                                                        \
        CHECK(!strcmp(s_, ws), "%s %g short \"%s\", want \"%s\"", CY_PARAMS[id].name, (double)(v), s_, ws);        \
        CHECK(!strcmp(l_, wl), "%s %g long \"%s\", want \"%s\"", CY_PARAMS[id].name, (double)(v), l_, wl);         \
    } while (0)

static int neg_zero(const char *s)     /* "-0", "-0.00dB": a sign on a value that rounds to zero */
{
    if (*s++ != '-')
        return 0;
    for (; (*s >= '0' && *s <= '9') || *s == '.'; s++)
        if (*s >= '1' && *s <= '9')
            return 0;
    return 1;
}
#ifdef T_FONTS
static int worst_cell, worst_long;
static char worst_cell_s[24], worst_long_s[24];
#endif
static void check_strings(int id, float n)
{
    char s[24], l[24];
    int i, upper = 1, numeric;
    const cy_param_def_t *d = &CY_PARAMS[id];
    memset(s, 0x7E, sizeof s);
    memset(l, 0x7E, sizeof l);
    cy_param_format(id, n, s, sizeof s, 0);
    cy_param_format(id, n, l, sizeof l, 1);
    CHECK(s[0] && strlen(s) <= 7, "%s at %g: short \"%s\" is %d glyphs", d->name, n, s, (int)strlen(s));
    CHECK(l[0] && strlen(l) <= 23, "%s at %g: long \"%s\" too long", d->name, n, l);
    CHECK(!neg_zero(s) && !neg_zero(l), "%s: negative zero \"%s\" \"%s\"", d->name, s, l);
    for (i = 0; s[i]; i++)
        CHECK(s[i] >= 32 && s[i] <= 126, "%s: short char %d", d->name, s[i]);
    numeric = strchr("0123456789+-.*", l[0]) != NULL;
    if (numeric) {                                    /* HERO number, then a unit or word in VAL */
        for (i = 0; in_hero(l[i]); i++)
            ;
        CHECK(i > 0 && (l[i] == 0 || (l[i] >= 'A' && l[i] <= 'Z') || (l[i] >= 'a' && l[i] <= 'z')),
              "%s: long \"%s\" does not split as number + unit", d->name, l);
    } else {
        for (i = 0; l[i]; i++)
            if (l[i] >= 'a' && l[i] <= 'z')
                upper = 0;
        CHECK(upper, "%s: text long form \"%s\" must be upper case (FONT_CY_MID)", d->name, l);
    }
#ifdef T_FONTS
    if (d->page < PG_N) {                             /* a knob cell shows it */
        int w = ink_w(&FONT_CY_VAL, s, 0);
        CHECK(w <= W_CELL, "%s: short \"%s\" is %d px > %d", d->name, s, w, W_CELL);
        if (w > worst_cell) {
            worst_cell = w;
            strcpy(worst_cell_s, s);
        }
    }
    {
        int w = ink_w(&FONT_CY_VAL, l, 0);
        CHECK(w <= W_LONG, "%s: long \"%s\" is %d px > %d", d->name, l, w, W_LONG);
        if (w > worst_long) {
            worst_long = w;
            strcpy(worst_long_s, l);
        }
    }
#endif
}

static void test_format(void)
{
    int id, i;
    for (id = 0; id < P_NPARAMS; id++) {
        int D = grid_of(id);
        for (i = 0; i <= D; i++)
            check_strings(id, (float)i / (float)D);
        for (i = 0; i < 300; i++)
            check_strings(id, frand());
        {                                             /* a tiny buffer is never overrun, always terminated */
            int n;
            for (n = 1; n <= 9; n++) {
                char b[16];
                memset(b, 0x55, sizeof b);
                cy_param_format(id, 0.73f, b, n, 1);
                CHECK(memchr(b, 0, (size_t)n) != NULL && (unsigned char)b[n] == 0x55u, "%s: buffer %d", CY_PARAMS[id].name,
                      n);
            }
        }
    }
    cy_param_format(P_ROOM, 0.5f, NULL, 10, 0);       /* (no crash) */
    {
        char b[4] = "xyz";
        cy_param_format(P_NPARAMS, 0.5f, b, 4, 0);
        CHECK(b[0] == 0, "bad id formats empty");
    }
    /* spec §4.1's examples, and the rest of the rules */
    EXPECT_FMT(P_SHAPE, 2.4f, "HEX>SQR", "HEX > SQUARE 40%");
    EXPECT_FMT(P_SHAPE, 2.0f, "HEX", "HEX");
    EXPECT_FMT(P_SHAPE, 4.0f, "TRI", "TRIANGLE");
    EXPECT_FMT(P_SHAPE, 0.4f, "DRM>CIR", "DRUM > CIRCLE 40%");
    EXPECT_FMT(P_SHAPE, 8.0f, "BAR", "BAR");
    EXPECT_FMT(P_MATERIAL, 3.0f, "STEEL", "STEEL");
    EXPECT_FMT(P_MATERIAL, 2.4f, "GLS>STL", "GLASS > STEEL 40%");
    EXPECT_FMT(P_MATERIAL, 0.5f, "RUB>WD", "RUBBER > WOOD 50%");
    EXPECT_FMT(P_EXCITER, 1.0f, "BOW", "BOW");
    EXPECT_FMT(P_KEYMODE, 1.0f, "SURF", "SURFACE");
    EXPECT_FMT(P_STIFFNESS, 10.0f, "SKIN", "10% SKIN");
    EXPECT_FMT(P_STIFFNESS, 80.0f, "PLATE", "80% PLATE");
    EXPECT_FMT(P_STIFFNESS, 97.0f, "BELL+", "97% BELL+");
    EXPECT_FMT(P_STIFFNESS, 62.0f, "62%", "62%");
    EXPECT_FMT(P_CARVE, 40.0f, "40%", "40%");
    EXPECT_FMT(P_DECAY, 1.6f, "*1.6", "*1.60");
    EXPECT_FMT(P_DECAY, 0.45f, "*0.45", "*0.45");
    EXPECT_FMT(P_DECAY, 10.0f, "*10", "*10.0");
    EXPECT_FMT(P_CARVE_TARGET, 0.0f, "HARM", "HARMONIC");
    EXPECT_FMT(P_CARVE_TARGET, 1.0f, "BELL", "BELL");
    EXPECT_FMT(P_BRIGHT, 20.0f, "+20", "+20");
    EXPECT_FMT(P_BRIGHT, -35.0f, "-35", "-35");
    EXPECT_FMT(P_BRIGHT, 0.0f, "0", "0");
    EXPECT_FMT(P_SHIMMER, 6.0f, "6.0c", "6.0c");
    EXPECT_FMT(P_MODES, 32.0f, "32", "32");
    EXPECT_FMT(P_TENSION, 2.0f, "+2.0st", "+2.0st");
    EXPECT_FMT(P_TENSION, 0.0f, "0.0st", "0.0st");
    EXPECT_FMT(P_SWELL, 0.25f, "0.25s", "0.25s");
    EXPECT_FMT(P_SWELL, 0.005f, "0.005s", "0.005s");
    EXPECT_FMT(P_SIZE, 2.8f, "2.8s", "2.80s");
    EXPECT_FMT(P_SIZE, 12.0f, "12.0s", "12.0s");
    EXPECT_FMT(P_DRIVE, 9.0f, "9.0dB", "9.0dB");
    EXPECT_FMT(P_DRIVE, 0.0f, "OFF", "OFF");
    EXPECT_FMT(P_PREDELAY, 12.0f, "12ms", "12ms");
    EXPECT_FMT(P_TILT, 1.0f, "+1.0dB", "+1.00dB");
    EXPECT_FMT(P_TILT, -0.25f, "-0.3dB", "-0.25dB");
    EXPECT_FMT(P_LEVEL, -18.0f, "-18.0dB", "-18.0dB");
    EXPECT_FMT(P_LEVEL, 0.0f, "0.0dB", "0.0dB");
    EXPECT_FMT(P_LFO_RATE, 0.2f, "0.20Hz", "0.20Hz");
    EXPECT_FMT(P_LFO_RATE, 0.01f, "0.010Hz", "0.010Hz");
    EXPECT_FMT(P_LFO_WAVE, 2.0f, "S&H", "S&H");
    EXPECT_FMT(P_VIB_RATE, 5.0f, "5.0Hz", "5.00Hz");
    EXPECT_FMT(P_VIBRATO, 12.0f, "12c", "12c");
    EXPECT_FMT(P_VEL_CURVE, -3.0f, "-3", "-3");
    EXPECT_FMT(P_TUNE, 12.0f, "+12c", "+12c");
    EXPECT_FMT(P_VOICES, 6.0f, "6", "6");
    EXPECT_FMT(P_THEME, 1.0f, "PHOSPH", "PHOSPHOR");
    EXPECT_FMT(P_THEME, 0.0f, "CYMA", "CYMATIC");
    EXPECT_FMT(P_GRAINS, 1200.0f, "1200", "1200");
    EXPECT_FMT(P_MOTION, 1.0f, "REDUCE", "REDUCED");
    EXPECT_FMT(P_DETAIL, 96.0f, "96", "96");
    EXPECT_FMT(P_POS_X, 62.0f, "62%", "62%");
    EXPECT_FMT(P_DAMPING, 55.0f, "55%", "55%");
#ifdef T_FONTS
    printf("  formats: widest knob value \"%s\" %d px (budget %d), widest long form \"%s\" %d px (budget %d)\n",
           worst_cell_s, worst_cell, W_CELL, worst_long_s, worst_long, W_LONG);
#else
    printf("  formats: cy_fonts.h not on the include path: pixel widths SKIPPED (glyph counts checked)\n");
#endif
}

/* -------------------------------------------------------------- hints --- */
static void test_hints(void)
{
    int id, i;
#ifdef T_FONTS
    int widest = 0, widest_id = 0;
#endif
    for (id = 0; id < P_NPARAMS; id++) {
        const char *h = CY_PARAMS[id].hint;
        CHECK(h && h[0] && strlen(h) <= 34, "%s: hint \"%s\" is %d chars", CY_PARAMS[id].name, h ? h : "", h ? (int)strlen(h) : 0);
        for (i = 0; h && h[i]; i++)
            CHECK(h[i] >= 32 && h[i] <= 95, "%s: hint char '%c' outside FONT_CY_LBL (32..95)", CY_PARAMS[id].name, h[i]);
#ifdef T_FONTS
        if (h) {
            int w = ink_w(&FONT_CY_LBL, h, 0);
            CHECK(w <= W_HINT, "%s: hint is %d px > %d", CY_PARAMS[id].name, w, W_HINT);
            if (w > widest)
                widest = w, widest_id = id;
        }
#endif
    }
    CHECK(!strcmp(CY_PARAMS[P_MATERIAL].hint, "RUBBER > BRONZE: HOW LONG IT RINGS"), "spec hint MATERIAL");
    CHECK(!strcmp(CY_PARAMS[P_CARVE].hint, "TUNE THE OVERTONES TO THE TARGET"), "spec hint CARVE");
    CHECK(!strcmp(CY_PARAMS[P_POS_X].hint, "WHERE YOU STRIKE: NODES GO QUIET"), "spec hint POS X");
    CHECK(!strcmp(CY_PARAMS[P_STRETCH].hint, "LEAN IN: THE METAL TIGHTENS"), "spec hint STRETCH");
#ifdef T_FONTS
    printf("  hints: widest \"%s\" %d px untracked (budget %d), %d px with +1 tracking\n", CY_PARAMS[widest_id].hint,
           widest, W_HINT, ink_w(&FONT_CY_LBL, CY_PARAMS[widest_id].hint, 1));
#endif
}

/* ------------------------------------------------------------ presets --- */
typedef struct { int8_t id; float v; } kv_t;
typedef struct { const char *name; kv_t kv[18]; } spec_t;
/* spec §4.4 transcribed independently of presets.c (display units; -1 ends a row). Deliberate deviations, each
 * measured and explained in presets.c: FELT PLATE HARDNESS 40 (spec 10); LEVEL per preset (§4.4 lets M4 match it);
 * and from the integration voicing (M7): MARIMBA POS (20,50) (spec (48,50)), VIBES POS (16,50) (spec default),
 * GLASS LOTUS POS (66,44) (spec (55,35)), GAMELAN STAR POS (68,40) (spec (50,30)) and TEMPLE BELL HARDNESS 22
 * (spec 15); from the review (fix-app): AETHER MORPH 25 (spec 60), so its LFO morphs CIRCLE..HEX as §4.4 says. */
static const spec_t SPEC44[CY_NFACTORY] = {
    {"SINGING BOWL", {{P_SHAPE, 1}, {P_EXCITER, 0}, {P_MATERIAL, 4.0f}, {P_STIFFNESS, 80}, {P_DECAY, 1.6f}, {P_SHIMMER, 6},
                      {P_HARDNESS, 30}, {P_POS_X, 58}, {P_POS_Y, 44}, {P_TENSION, 0.8f}, {P_PALM, 10}, {P_MICS, 60},
                      {P_ROOM, 35}, {P_SIZE, 4.0f}, {P_LEVEL, 3.5f}, {-1, 0}}},
    {"HANDPAN", {{P_SHAPE, 0}, {P_EXCITER, 0}, {P_MATERIAL, 3.0f}, {P_STIFFNESS, 55}, {P_CARVE, 85}, {P_CARVE_TARGET, 0},
                 {P_DECAY, 1.2f}, {P_HARDNESS, 45}, {P_NOISE, 30}, {P_BLOOM, 10}, {P_POS_X, 55}, {P_POS_Y, 50},
                 {P_ROOM, 28}, {P_SIZE, 2.6f}, {P_LEVEL, -4}, {-1, 0}}},
    {"GLASS LOTUS", {{P_SHAPE, 5}, {P_EXCITER, 0}, {P_MATERIAL, 2.0f}, {P_STIFFNESS, 85}, {P_CARVE, 30}, {P_CARVE_TARGET, 1},
                     {P_DECAY, 1.4f}, {P_SHIMMER, 4}, {P_HARDNESS, 85}, {P_POS_X, 66}, {P_POS_Y, 44}, {P_MICS, 85},
                     {P_ROOM, 45}, {P_SIZE, 5.0f}, {P_PREDELAY, 20}, {P_LEVEL, 2}, {-1, 0}}},
    {"MARIMBA", {{P_SHAPE, 8}, {P_EXCITER, 0}, {P_MATERIAL, 1.0f}, {P_STIFFNESS, 80}, {P_CARVE, 90}, {P_CARVE_TARGET, 2},
                 {P_DECAY, 0.9f}, {P_HARDNESS, 50}, {P_POS_X, 20}, {P_POS_Y, 50}, {P_ROOM, 20}, {P_SIZE, 1.6f},
                 {P_TILT, 1}, {P_LEVEL, 6}, {-1, 0}}},
    {"VIBES", {{P_SHAPE, 8}, {P_EXCITER, 0}, {P_MATERIAL, 2.6f}, {P_STIFFNESS, 80}, {P_CARVE, 90}, {P_CARVE_TARGET, 2},
               {P_DECAY, 2.5f}, {P_HARDNESS, 60}, {P_DAMPER, 85}, {P_SHIMMER, 2}, {P_ROOM, 30}, {P_SIZE, 2.2f},
               {P_POS_X, 16}, {P_POS_Y, 50}, {P_LEVEL, 4}, {-1, 0}}},
    {"TABLA SKIN", {{P_SHAPE, 0}, {P_EXCITER, 0}, {P_MATERIAL, 0.6f}, {P_STIFFNESS, 0}, {P_CARVE, 70}, {P_CARVE_TARGET, 0},
                    {P_DECAY, 0.7f}, {P_HARDNESS, 65}, {P_TENSION, 5.0f}, {P_PALM, 50}, {P_BLOOM, 25}, {P_POS_X, 62},
                    {P_POS_Y, 46}, {P_ROOM, 15}, {P_SIZE, 1.2f}, {-1, 0}}},
    {"CATHEDRAL GONG", {{P_SHAPE, 1}, {P_EXCITER, 0}, {P_MATERIAL, 4.3f}, {P_STIFFNESS, 95}, {P_DECAY, 3.0f},
                        {P_HARDNESS, 20}, {P_BLOOM, -35}, {P_SHIMMER, 8}, {P_POS_X, 60}, {P_POS_Y, 42}, {P_ROOM, 50},
                        {P_SIZE, 8.0f}, {P_PREDELAY, 30}, {P_DAMPING, 45}, {P_LEVEL, 6}, {-1, 0}}},
    {"STEEL RAIN", {{P_SHAPE, 2}, {P_EXCITER, 2}, {P_MATERIAL, 3.0f}, {P_STIFFNESS, 80}, {P_DENSITY, 45}, {P_SPREAD, 60},
                    {P_SWELL, 0.4f}, {P_MICS, 90}, {P_ROOM, 45}, {P_SIZE, 4.0f}, {P_LEVEL, -2}, {-1, 0}}},
    {"BOWED GLASS", {{P_SHAPE, 3}, {P_EXCITER, 1}, {P_MATERIAL, 2.0f}, {P_STIFFNESS, 80}, {P_CARVE, 20}, {P_CARVE_TARGET, 0},
                     {P_DECAY, 2.0f}, {P_FORCE, 60}, {P_GRIT, 30}, {P_SWELL, 0.6f}, {P_POS_X, 12}, {P_POS_Y, 50},
                     {P_ROOM, 55}, {P_SIZE, 6.0f}, {P_LEVEL, 4.5f}, {-1, 0}}},
    {"GAMELAN STAR", {{P_SHAPE, 5}, {P_EXCITER, 0}, {P_MATERIAL, 4.0f}, {P_STIFFNESS, 90}, {P_CARVE, 50},
                      {P_CARVE_TARGET, 2}, {P_SHIMMER, 12}, {P_DECAY, 1.8f}, {P_HARDNESS, 60}, {P_POS_X, 68},
                      {P_POS_Y, 40}, {P_ROOM, 35}, {P_SIZE, 3.0f}, {-1, 0}}},
    {"PRISM BELLS", {{P_SHAPE, 4}, {P_EXCITER, 0}, {P_MATERIAL, 2.6f}, {P_STIFFNESS, 100}, {P_CARVE, 80},
                     {P_CARVE_TARGET, 1}, {P_DECAY, 2.0f}, {P_HARDNESS, 75}, {P_SHIMMER, 3}, {P_MICS, 70}, {P_ROOM, 40},
                     {P_SIZE, 4.0f}, {P_LEVEL, -0.5f}, {-1, 0}}},
    {"GUITAR BODY", {{P_SHAPE, 6}, {P_EXCITER, 0}, {P_MATERIAL, 1.0f}, {P_STIFFNESS, 55}, {P_DECAY, 0.6f},
                     {P_HARDNESS, 90}, {P_NOISE, 35}, {P_POS_X, 50}, {P_POS_Y, 72}, {P_ROOM, 15}, {P_SIZE, 1.2f}, {-1, 0}}},
    {"CHAOS STADIUM", {{P_SHAPE, 7}, {P_EXCITER, 2}, {P_MATERIAL, 3.0f}, {P_DECAY, 1.3f}, {P_DENSITY, 25}, {P_SPREAD, 90},
                       {P_WANDER, 50}, {P_LFO_POS, 30}, {P_LFO_RATE, 0.12f}, {P_ROOM, 50}, {P_SIZE, 7.0f}, {P_LEVEL, -2.5f}, {-1, 0}}},
    {"FELT PLATE", {{P_SHAPE, 3}, {P_EXCITER, 0}, {P_MATERIAL, 0.8f}, {P_STIFFNESS, 70}, {P_DECAY, 0.5f}, {P_HARDNESS, 40},
                    {P_DAMPER, 60}, {P_TILT, -2}, {P_ROOM, 25}, {P_SIZE, 1.8f}, {P_LEVEL, 6}, {-1, 0}}},
    {"WATER GONG", {{P_SHAPE, 1}, {P_EXCITER, 0}, {P_MATERIAL, 4.0f}, {P_STIFFNESS, 90}, {P_DECAY, 2.5f}, {P_HARDNESS, 40},
                    {P_BLOOM, 60}, {P_LFO_POS, 30}, {P_LFO_RATE, 0.15f}, {P_ROOM, 55}, {P_SIZE, 9.0f}, {-1, 0}}},
    {"MEMBRANE KICK", {{P_SHAPE, 0}, {P_EXCITER, 0}, {P_MATERIAL, 0.4f}, {P_STIFFNESS, 0}, {P_DECAY, 0.45f},
                       {P_HARDNESS, 70}, {P_BLOOM, 85}, {P_POS_X, 50}, {P_POS_Y, 50}, {P_DRIVE, 9}, {P_ROOM, 8},
                       {P_SIZE, 0.8f}, {P_LEVEL, 2}, {-1, 0}}},
    {"LOOSE TOM", {{P_SHAPE, 0.4f}, {P_EXCITER, 0}, {P_MATERIAL, 0.7f}, {P_STIFFNESS, 15}, {P_DECAY, 0.6f}, {P_HARDNESS, 60},
                   {P_BLOOM, 40}, {P_DRIVE, 4}, {P_ROOM, 18}, {P_SIZE, 1.4f}, {-1, 0}}},
    {"THUNDER SHEET", {{P_SHAPE, 3.4f}, {P_EXCITER, 2}, {P_MATERIAL, 3.0f}, {P_STIFFNESS, 75}, {P_DECAY, 1.5f},
                       {P_DENSITY, 80}, {P_SPREAD, 100}, {P_BLOOM, 20}, {P_NOISE, 50}, {P_DRIVE, 6}, {P_ROOM, 60},
                       {P_SIZE, 10}, {P_LEVEL, -6.5f}, {-1, 0}}},
    {"ICE", {{P_SHAPE, 4}, {P_EXCITER, 0}, {P_MATERIAL, 2.0f}, {P_STIFFNESS, 100}, {P_DECAY, 0.7f}, {P_HARDNESS, 100},
             {P_BRIGHT, 50}, {P_SHIMMER, 0}, {P_MICS, 100}, {P_ROOM, 50}, {P_SIZE, 3.0f}, {-1, 0}}},
    {"KALIMBA TINE", {{P_SHAPE, 8}, {P_EXCITER, 0}, {P_MATERIAL, 3.0f}, {P_DECAY, 1.2f}, {P_HARDNESS, 70}, {P_NOISE, 20},
                      {P_POS_X, 15}, {P_POS_Y, 50}, {P_ROOM, 20}, {P_SIZE, 1.5f}, {P_LEVEL, 2.5f}, {-1, 0}}},
    {"AETHER", {{P_SHAPE, 1.5f}, {P_EXCITER, 1}, {P_MATERIAL, 5.0f}, {P_DECAY, 4.0f}, {P_FORCE, 35}, {P_SWELL, 1.5f},
                {P_MORPH_DEPTH, 25}, {P_LFO_RATE, 0.07f}, {P_LFO_WAVE, CY_LFO_SINE}, {P_ROOM, 70}, {P_SIZE, 12},
                {P_PREDELAY, 45}, {P_LEVEL, -2}, {-1, 0}}},
    {"SURFACE GONG", {{P_KEYMODE, 1}, {P_SHAPE, 1}, {P_EXCITER, 0}, {P_MATERIAL, 4.0f}, {P_STIFFNESS, 90}, {P_DECAY, 3.0f},
                      {P_HARDNESS, 50}, {P_ROOM, 50}, {P_SIZE, 6.0f}, {P_LEVEL, -3.5f}, {-1, 0}}},
    {"BOWED SQUARE", {{P_SHAPE, 3}, {P_EXCITER, 1}, {P_MATERIAL, 3.0f}, {P_DECAY, 1.5f}, {P_FORCE, 75}, {P_GRIT, 60},
                      {P_POS_X, 6}, {P_POS_Y, 50}, {P_ROOM, 40}, {P_SIZE, 3.5f}, {P_LEVEL, 5.5f}, {-1, 0}}},
    {"TEMPLE BELL", {{P_SHAPE, 1}, {P_EXCITER, 0}, {P_MATERIAL, 4.5f}, {P_STIFFNESS, 100}, {P_CARVE, 60},
                     {P_CARVE_TARGET, 1}, {P_DECAY, 4.0f}, {P_HARDNESS, 22}, {P_BLOOM, -10}, {P_SHIMMER, 12}, {P_ROOM, 50},
                     {P_SIZE, 10}, {P_LEVEL, 6}, {-1, 0}}},
};

static int val_close(int id, float got, float want)
{
    const cy_param_def_t *d = &CY_PARAMS[id];
    if (d->curve == CY_CV_LOG)
        return fabsf(got / want - 1.0f) < 2e-4f;
    return fabsf(got - want) <= 2e-4f * (d->hi - d->lo) + 1e-6f;
}

static void preset_name(const cy_preset_t *p, char *out)   /* (a 14-letter name has no terminator) */
{
    int i;
    for (i = 0; i < CY_PRESET_NAME && p->name[i]; i++)
        out[i] = p->name[i];
    out[i] = 0;
}

static void test_presets(void)
{
    float norm[CY_NFACTORY][P_NPARAMS], def[P_NPARAMS];
    int p, q, id, k, min_diff = 99, min_a = 0, min_b = 0;
    char name[16];
    cy_params_defaults(def);
    for (id = 0; id < P_NPARAMS; id++)
        CHECK(val_close(id, cy_param_value(id, def[id]), CY_PARAMS[id].def), "%s: default %g reads %g",
              CY_PARAMS[id].name, CY_PARAMS[id].def, cy_param_value(id, def[id]));
    for (p = 0; p < CY_NFACTORY; p++) {
        const cy_preset_t *pr = &CY_FACTORY[p];
        int listed[P_NPARAMS] = {0}, i;
        memcpy(norm[p], def, sizeof def);
        for (id = P_NPRESET; id < P_NPARAMS; id++)
            norm[p][id] = -7.0f;                       /* load must leave the globals alone */
        cy_preset_load(pr, norm[p]);
        for (id = P_NPRESET; id < P_NPARAMS; id++)
            CHECK(norm[p][id] == -7.0f, "preset load touched global %s", CY_PARAMS[id].name);
        preset_name(pr, name);
        CHECK(name[0] && !strcmp(name, SPEC44[p].name), "preset %02d is \"%s\", want \"%s\"", p + 1, name, SPEC44[p].name);
        for (i = 0; name[i]; i++)
            CHECK((name[i] >= 'A' && name[i] <= 'Z') || name[i] == ' ', "preset %02d name char", p + 1);
        for (q = 0; q < p; q++)
            CHECK(memcmp(CY_FACTORY[q].name, pr->name, CY_PRESET_NAME), "duplicate name %s", name);
        for (id = 0; id < P_NPRESET; id++) {
            float v = cy_param_value(id, norm[p][id]);
            CHECK(norm[p][id] >= 0.0f && norm[p][id] <= 1.0f && v >= CY_PARAMS[id].lo && v <= CY_PARAMS[id].hi,
                  "%s: %s out of range", name, CY_PARAMS[id].name);
        }
        for (k = 0; SPEC44[p].kv[k].id >= 0; k++) {    /* every §4.4 setting */
            int pid = SPEC44[p].kv[k].id;
            float v = cy_param_value(pid, norm[p][pid]);
            listed[pid] = 1;
            CHECK(val_close(pid, v, SPEC44[p].kv[k].v), "%s: %s = %g, spec %g", name, CY_PARAMS[pid].name, v,
                  SPEC44[p].kv[k].v);
        }
        for (id = 0; id < P_NPRESET; id++)             /* ... and the defaults for the rest */
            if (!listed[id])
                CHECK(fabsf(norm[p][id] - def[id]) <= 1.01f / 65535.0f, "%s: unlisted %s = %g, default %g", name,
                      CY_PARAMS[id].name, cy_param_value(id, norm[p][id]), CY_PARAMS[id].def);
        /* exact keyframes and grid points survive the 16-bit store */
        CHECK(cy_param_value(P_SHAPE, norm[p][P_SHAPE]) == floorf(cy_param_value(P_SHAPE, norm[p][P_SHAPE]) + 0.5f) ||
                  p == 16 || p == 17 || p == 20, "%s: SHAPE off its keyframe", name);
    }
    CHECK(cy_param_value(P_SHAPE, norm[0][P_SHAPE]) == 1.0f && norm[0][P_SHAPE] == 0.125f, "01 parks exactly on CIRCLE");
    CHECK(norm[0][P_STIFFNESS] == 0.8f, "STIFFNESS 80 loads as exactly 0.8");
    CHECK(norm[7][P_EXCITER] == 1.0f && norm[8][P_EXCITER] == 0.5f, "enums load exactly");
    /* the presets differ meaningfully: every pair differs by >= 0.05 of the range in >= 3 parameters */
    for (p = 0; p < CY_NFACTORY; p++)
        for (q = p + 1; q < CY_NFACTORY; q++) {
            int n = 0;
            for (id = 0; id < P_NPRESET; id++)
                if (fabsf(norm[p][id] - norm[q][id]) >= 0.05f)
                    n++;
            CHECK(n >= 3, "presets %02d and %02d differ in only %d parameters", p + 1, q + 1, n);
            if (n < min_diff)
                min_diff = n, min_a = p, min_b = q;
        }
    {   /* and they spread over the space: every exciter, shape keyframe group and target is used */
        int exc[CY_NEXC] = {0}, tgt[CY_NCARVE] = {0}, shp[CY_NSHAPES] = {0}, surf = 0;
        for (p = 0; p < CY_NFACTORY; p++) {
            exc[(int)cy_param_value(P_EXCITER, norm[p][P_EXCITER])]++;
            shp[(int)(cy_param_value(P_SHAPE, norm[p][P_SHAPE]) + 0.5f)]++;
            if (cy_param_value(P_CARVE, norm[p][P_CARVE]) > 0.0f)
                tgt[(int)cy_param_value(P_CARVE_TARGET, norm[p][P_CARVE_TARGET])]++;
            surf += (int)cy_param_value(P_KEYMODE, norm[p][P_KEYMODE]);
        }
        for (k = 0; k < CY_NEXC; k++)
            CHECK(exc[k] >= 3, "exciter %d used by %d presets", k, exc[k]);
        for (k = 0; k < CY_NCARVE; k++)
            CHECK(tgt[k] >= 2, "carve target %d used by %d presets", k, tgt[k]);
        for (k = 0; k < CY_NSHAPES; k++)
            CHECK(shp[k] >= 1, "shape %d used by no preset", k);
        CHECK(surf == 1, "one SURFACE preset");
        printf("  presets: exciters M/B/R = %d/%d/%d, closest pair %02d/%02d differ in %d params\n", exc[0], exc[1],
               exc[2], min_a + 1, min_b + 1, min_diff);
    }
}

static void test_store(void)
{
    cy_preset_t p, p2;
    float a[P_NPARAMS], b[P_NPARAMS], c[P_NPARAMS];
    int i, id;
    for (i = 0; i < CY_NFACTORY; i++) {                /* load(store(load(x))) == load(x), bit for bit */
        char nm[16];
        cy_preset_load(&CY_FACTORY[i], a);
        preset_name(&CY_FACTORY[i], nm);
        cy_preset_store(a, nm, &p);
        cy_preset_load(&p, b);
        CHECK(!memcmp(a, b, sizeof(float) * P_NPRESET), "factory %02d not idempotent", i + 1);
        for (id = 0; id < P_NPRESET; id++)              /* (a value snapped to its grid may store 1 step over) */
            CHECK(abs((int)p.v[id] - (int)CY_FACTORY[i].v[id]) <= 1, "factory %02d %s re-stores as %u, was %u", i + 1,
                  CY_PARAMS[id].name, p.v[id], CY_FACTORY[i].v[id]);
        CHECK(!memcmp(p.name, CY_FACTORY[i].name, CY_PRESET_NAME), "factory %02d name round trip", i + 1);
    }
    for (i = 0; i < 2000; i++) {                        /* any norms survive within 1.5 16-bit steps (rounding */
        for (id = 0; id < P_NPRESET; id++)              /* 0.5, then the grid snap <= 1), and a loaded value */
            a[id] = frand();                            /* survives store and load exactly */
        if (i & 1)
            for (id = 0; id < P_NPRESET; id++)          /* half the time near a grid point, where the snap acts */
                a[id] = fminf(1.0f, fmaxf(0.0f, floorf(a[id] * (float)grid_of(id) + 0.5f) / (float)grid_of(id) +
                                                     (frand() - 0.5f) * 3.0f / 65535.0f));
        cy_preset_store(a, "U07", &p);
        cy_preset_load(&p, b);
        for (id = 0; id < P_NPRESET; id++)
            CHECK(fabsf(b[id] - a[id]) <= 1.5001f / 65535.0f, "%s store %g -> %g", CY_PARAMS[id].name, a[id], b[id]);
        cy_preset_store(b, "U07", &p2);
        cy_preset_load(&p2, c);
        CHECK(!memcmp(b, c, sizeof(float) * P_NPRESET), "load(store(b)) != b");
        for (id = 0; id < P_NPRESET; id++)
            CHECK(abs((int)p.v[id] - (int)p2.v[id]) <= 1, "re-store moved %s by more than one step", CY_PARAMS[id].name);
    }
    a[P_ROOM] = 1.7f;
    a[P_DECAY] = -0.2f;
    a[P_SIZE] = NAN;
    cy_preset_store(a, "CATHEDRAL GONG", &p);
    CHECK(p.v[P_ROOM] == 65535u && p.v[P_DECAY] == 0u && p.v[P_SIZE] == 0u, "store clamps (and NaN -> 0)");
    CHECK(!memcmp(p.name, "CATHEDRAL GONG", 14), "14-letter name kept whole");
    cy_preset_store(a, "A NAME FAR TOO LONG", &p);
    CHECK(!memcmp(p.name, "A NAME FAR TOO", 14), "long name cut at 14");
    cy_preset_store(a, "U1", &p);
    CHECK(!memcmp(p.name, "U1\0\0\0\0\0\0\0\0\0\0\0\0", 14), "short name zero-padded");
    cy_preset_store(a, NULL, &p);
    CHECK(p.name[0] == 0 && p.name[13] == 0, "NULL name");
    cy_preset_load(NULL, a);                            /* (no crash) */
    cy_preset_store(a, "X", NULL);
}

/* --------------------------------------------------------------- lerp --- */
static float lip(int id)       /* max |d value / d norm| */
{
    const cy_param_def_t *d = &CY_PARAMS[id];
    if (d->curve == CY_CV_LOG)
        return d->hi * logf(d->hi / d->lo);
    return d->hi - d->lo;
}
static void test_lerp(void)
{
    float a[P_NPARAMS], b[P_NPARAMS], o[P_NPARAMS], prev[P_NPARAMS];
    int pa, pb, id, i, pair;
    const int N = 1000;
    for (pair = 0; pair < 60; pair++) {
        int flips[P_NPARAMS] = {0};
        pa = pair % CY_NFACTORY;
        pb = (pair * 7 + 5) % CY_NFACTORY;
        if (pa == pb)
            pb = (pb + 1) % CY_NFACTORY;
        cy_preset_load(&CY_FACTORY[pa], a);
        cy_preset_load(&CY_FACTORY[pb], b);
        if (pair >= 48)                                  /* and some random corners */
            for (id = 0; id < P_NPRESET; id++)
                a[id] = frand(), b[id] = frand();
        cy_preset_lerp(a, b, 0.0f, o);
        CHECK(!memcmp(o, a, sizeof(float) * P_NPRESET), "lerp t=0 is A");
        cy_preset_lerp(a, b, 1.0f, o);
        CHECK(!memcmp(o, b, sizeof(float) * P_NPRESET), "lerp t=1 is B");
        cy_preset_lerp(a, b, -3.0f, o);
        CHECK(!memcmp(o, a, sizeof(float) * P_NPRESET), "lerp clamps t");
        cy_preset_lerp(a, b, 0.0f, prev);
        for (i = 1; i <= N; i++) {
            float t = (float)i / (float)N;
            cy_preset_lerp(a, b, t, o);
            for (id = 0; id < P_NPRESET; id++) {
                float dv = fabsf(cy_param_value(id, o[id]) - cy_param_value(id, prev[id]));
                if (a[id] == b[id]) {
                    CHECK(o[id] == a[id], "%s: shared value moved", CY_PARAMS[id].name);
                } else if (CY_PARAMS[id].curve == CY_CV_ENUM) {
                    if (o[id] != prev[id]) {
                        flips[id]++;
                        CHECK(fabsf(t - 0.5f) < 1.5f / (float)N, "%s switched at t=%g", CY_PARAMS[id].name, t);
                    }
                } else if (is_int(id)) {
                    CHECK(dv <= range_of(id) / (float)grid_of(id) + 1e-4f, "%s: integer jump %g", CY_PARAMS[id].name, dv);
                } else {                                 /* continuous: Lipschitz in t (keyframe snap 1e-4 aside) */
                    float bound = lip(id) * fabsf(b[id] - a[id]) / (float)N * 1.02f + 1e-5f * range_of(id) +
                                  (CY_PARAMS[id].curve == CY_CV_MORPH ? 1.1e-4f : 0.0f);
                    CHECK(dv <= bound, "%s: lerp jump %g > %g at t=%g", CY_PARAMS[id].name, dv, bound, t);
                }
                CHECK(o[id] >= 0.0f && o[id] <= 1.0f, "lerp out of range");
            }
            memcpy(prev, o, sizeof o);
        }
        for (id = 0; id < P_NPRESET; id++)
            if (CY_PARAMS[id].curve == CY_CV_ENUM && a[id] != b[id])
                CHECK(flips[id] == 1, "%s switched %d times", CY_PARAMS[id].name, flips[id]);
    }
}

/* -------------------------------------------------------------- patch --- */
static int finite_all(const float *f, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (!(f[i] == f[i]) || fabsf(f[i]) > 1e9f)
            return 0;
    return 1;
}
#define IN(x, lo, hi) ((x) >= (lo) && (x) <= (hi))
static void check_patch(const cy_patch_t *p, const cy_geom_in_t *g, const char *what)
{
    CHECK(p->exciter < CY_NEXC && p->surface <= 1 && (p->max_voices == 4 || p->max_voices == 6 || p->max_voices == 8) &&
              IN(p->modes_per_voice, 8, 32), "%s: patch ints", what);
    CHECK(finite_all(&p->decay_mul, (int)((&p->predelay_ms - &p->decay_mul) + 1)), "%s: patch finite", what);
    CHECK(IN(p->decay_mul, 0.1f, 10.0f) && IN(p->hardness, 0, 1) && IN(p->tension_st, 0, 7) && IN(p->palm, 0, 1) &&
              IN(p->force, 0, 1) && IN(p->grit, 0, 1) && IN(p->swell_s, 0.005f, 2.0f) && IN(p->density, 0, 1) &&
              IN(p->noise, 0, 1) && IN(p->damper, 0, 1) && IN(p->bloom, -1, 1) && IN(p->stretch, 0, 1),
          "%s: exciter ranges", what);
    CHECK(IN(p->vibrato_cents, 0, 50) && IN(p->vib_rate_hz, 0.5f, 8.0f) && IN(p->tune_cents, -50, 50) &&
              IN(p->level_db, -18, 6) && IN(p->surface_root, 12, 84), "%s: pitch / level ranges", what);
    CHECK(IN(p->drive_db, 0, 24) && IN(p->tilt_db, -6, 6) && IN(p->room, 0, 1) && IN(p->size_s, 0.3f, 12.0f) &&
              IN(p->damping, 0.2f, 1.0f) && IN(p->predelay_ms, 0, 60), "%s: fx ranges", what);
    CHECK(IN(g->shape, 0, 8) && IN(g->stiffness, 0, 1) && IN(g->carve, 0, 1) && g->carve_target < CY_NCARVE &&
              g->surface <= 1 && !g->rsv[0] && !g->rsv[1] && IN(g->shimmer_cents, 0, 15) && IN(g->material, 0, 5) &&
              IN(g->bright, -1, 1) && IN(g->pos_u, 0, 1) && IN(g->pos_v, 0, 1) && IN(g->mic_spread, 0, 1) &&
              IN(g->rain_spread, 0, 1) && IN(g->wander, 0, 1), "%s: geometry ranges", what);
    CHECK(finite_all(&g->shape, 3) && finite_all(&g->shimmer_cents, 8), "%s: geometry finite", what);
}

static void test_patch(void)
{
    float n[P_NPARAMS];
    cy_patch_t p;
    cy_geom_in_t g;
    cy_mod_t m;
    int i, k, id, oct;
    for (i = 0; i < CY_NFACTORY; i++)
        for (k = 0; k < 16; k++)
            for (oct = -3; oct <= 3; oct++) {
                cy_params_defaults(n);
                cy_preset_load(&CY_FACTORY[i], n);
                m.lfo = (float)(k % 5) * 0.5f - 1.0f;
                m.orbit_u = cosf(0.3927f * (float)k);
                m.orbit_v = sinf(0.3927f * (float)k);
                m.press_max = 0.5f;
                m.octave = (int8_t)oct;
                cy_patch_build(n, &m, &p, &g);
                check_patch(&p, &g, CY_FACTORY[i].name);
                CHECK(p.surface_root == 48.0f + 12.0f * (float)oct, "surface_root");
            }
    for (i = 0; i < 3000; i++) {                         /* fuzz: norms past both ends, NaN, wild modulation */
        for (id = 0; id < P_NPARAMS; id++)
            n[id] = frand() * 1.6f - 0.3f;
        if (i % 7 == 0)
            n[(int)(frand() * P_NPARAMS)] = NAN;
        m.lfo = frand() * 4.0f - 2.0f;
        m.orbit_u = frand() * 4.0f - 2.0f;
        m.orbit_v = i % 11 ? frand() * 4.0f - 2.0f : NAN;
        m.press_max = frand();
        m.octave = (int8_t)((int)(frand() * 13.0f) - 6);
        cy_patch_build(n, &m, &p, &g);
        check_patch(&p, &g, "fuzz");
    }
    cy_patch_build(n, NULL, &p, &g);
    check_patch(&p, &g, "no mod");
    /* the mapping of spec §9.5, exactly */
    for (i = 0; i < 400; i++) {
        float V[P_NPARAMS], pos, sh;
        for (id = 0; id < P_NPARAMS; id++)
            n[id] = frand(), V[id] = cy_param_value(id, n[id]);
        m.lfo = frand() * 2.0f - 1.0f;
        m.orbit_u = frand() * 2.0f - 1.0f;
        m.orbit_v = frand() * 2.0f - 1.0f;
        m.octave = (int8_t)((int)(frand() * 7.0f) - 3);
        cy_patch_build(n, &m, &p, &g);
        sh = fminf(fmaxf(V[P_SHAPE] + 2.0f * V[P_MORPH_DEPTH] / 100.0f * m.lfo, 0.0f), 8.0f);
        CHECK(fabsf(g.shape - sh) < 1e-5f, "shape = SHAPE + 2 MORPH lfo");
        pos = fminf(fmaxf(V[P_POS_X] / 100.0f + 0.25f * V[P_LFO_POS] / 100.0f * m.orbit_u, 0.0f), 1.0f);
        CHECK(fabsf(g.pos_u - pos) < 1e-6f, "pos_u");
        pos = fminf(fmaxf(V[P_POS_Y] / 100.0f + 0.25f * V[P_LFO_POS] / 100.0f * m.orbit_v, 0.0f), 1.0f);
        CHECK(fabsf(g.pos_v - pos) < 1e-6f, "pos_v");
        CHECK(g.stiffness == V[P_STIFFNESS] / 100.0f && g.carve == V[P_CARVE] / 100.0f && g.material == V[P_MATERIAL] &&
                  g.bright == V[P_BRIGHT] / 100.0f && g.shimmer_cents == V[P_SHIMMER] &&
                  g.mic_spread == V[P_MICS] / 100.0f && g.rain_spread == V[P_SPREAD] / 100.0f &&
                  g.wander == V[P_WANDER] / 100.0f && g.carve_target == (uint8_t)V[P_CARVE_TARGET] &&
                  g.surface == (uint8_t)V[P_KEYMODE], "geometry mapping");
        CHECK(p.exciter == (uint8_t)V[P_EXCITER] && p.max_voices == (uint8_t)V[P_VOICES] &&
                  p.modes_per_voice == (uint8_t)V[P_MODES] && p.decay_mul == V[P_DECAY] && p.tension_st == V[P_TENSION] &&
                  p.swell_s == V[P_SWELL] && p.bloom == V[P_BLOOM] / 100.0f && p.tune_cents == V[P_TUNE] &&
                  p.level_db == V[P_LEVEL] && p.drive_db == V[P_DRIVE] && p.tilt_db == V[P_TILT] &&
                  p.size_s == V[P_SIZE] && p.damping == V[P_DAMPING] / 100.0f && p.predelay_ms == V[P_PREDELAY] &&
                  p.vibrato_cents == V[P_VIBRATO] && p.vib_rate_hz == V[P_VIB_RATE] && p.room == V[P_ROOM] / 100.0f &&
                  p.hardness == V[P_HARDNESS] / 100.0f && p.palm == V[P_PALM] / 100.0f && p.force == V[P_FORCE] / 100.0f &&
                  p.grit == V[P_GRIT] / 100.0f && p.density == V[P_DENSITY] / 100.0f && p.noise == V[P_NOISE] / 100.0f &&
                  p.damper == V[P_DAMPER] / 100.0f && p.stretch == V[P_STRETCH] / 100.0f && p.surface == g.surface &&
                  p.surface_root == 48.0f + 12.0f * (float)m.octave, "patch mapping");
    }
    cy_params_defaults(n);                               /* defaults: 8 voices, 32 modes, damping 0.55 */
    m.lfo = m.orbit_u = m.orbit_v = m.press_max = 0.0f;
    m.octave = 0;
    cy_patch_build(n, &m, &p, &g);
    CHECK(p.max_voices == 8 && p.modes_per_voice == 32 && fabsf(p.damping - 0.55f) < 1e-6f && g.shape == 1.0f &&
              fabsf(g.pos_u - 0.62f) < 1e-6f && p.surface_root == 48.0f, "default patch");
}

/* ---------------------------------------------------------------- LFO --- */
static void test_lfo(void)
{
    float ph, st[2], v, prev, dt = 1.0f / 60.0f;
    uint32_t rng;
    int i, w, zc;
    for (w = 0; w < 4; w++) {
        float maxd = 0.0f, lo = 9.0f, hi = -9.0f, rate = 1.0f;
        int changes = 0, wraps = 0;
        ph = 0.0f, st[0] = st[1] = 0.0f, rng = 0x6C8E9CF5u, prev = 0.0f, zc = 0;
        for (i = 0; i < 600; i++) {
            float ph0 = ph;
            v = cy_lfo_step(&ph, &rng, st, w, rate, dt);
            if (ph < ph0)
                wraps++;
            CHECK(v == v && v >= -1.0f && v <= 1.0f, "LFO %d out of range %g", w, v);
            if (i) {
                if (fabsf(v - prev) > maxd)
                    maxd = fabsf(v - prev);
                if (v != prev)
                    changes++;
                if ((prev < 0.0f) != (v < 0.0f))
                    zc++;
            }
            lo = fminf(lo, v), hi = fmaxf(hi, v);
            prev = v;
        }
        CHECK(wraps == 9 || wraps == 10, "LFO %d: %d wraps in 10 s at 1 Hz", w, wraps);
        if (w == CY_LFO_SINE || w == CY_LFO_TRI) {
            CHECK(hi > 0.99f && lo < -0.99f, "LFO %d swing %g..%g", w, lo, hi);
            CHECK(zc >= 19 && zc <= 21, "LFO %d: %d zero crossings in 10 s at 1 Hz", w, zc);
            CHECK(maxd <= (w == CY_LFO_SINE ? 6.2831853f : 4.0f) * rate * dt * 1.01f + 1e-4f, "LFO %d continuity %g", w,
                  maxd);                                 /* peak slope: 2 pi for the sine, 4 for the triangle */
        }
        if (w == CY_LFO_SH)
            CHECK(changes <= wraps && changes >= wraps - 1, "S&H changes %d times for %d wraps", changes, wraps);
        if (w == CY_LFO_DRIFT)                           /* smoothstep slope <= 1.5, step <= 2 */
            CHECK(maxd <= 2.0f * 1.5f * rate * dt + 1e-4f && changes > 500, "DRIFT continuity %g / %d", maxd, changes);
    }
    ph = 0.0f, st[0] = st[1] = 0.0f, rng = 1u;
    CHECK(cy_lfo_step(&ph, &rng, st, CY_LFO_SINE, 0.2f, dt) > 0.0f && fabsf(cy_lfo_step(&ph, &rng, st, CY_LFO_TRI, 0.2f, 0.0f) -
          4.0f * ph) < 1e-6f, "SINE and TRI start at 0, rising");
    {   /* determinism, and a fresh DRIFT moves in its first cycle */
        float a[300], b[300], s1[2] = {0, 0}, s2[2] = {0, 0}, p1 = 0.0f, p2 = 0.0f;
        uint32_t r1 = 0xBEEFu, r2 = 0xBEEFu;
        for (i = 0; i < 300; i++) {
            a[i] = cy_lfo_step(&p1, &r1, s1, CY_LFO_DRIFT, 0.2f, dt);
            b[i] = cy_lfo_step(&p2, &r2, s2, CY_LFO_DRIFT, 0.2f, dt);
        }
        CHECK(!memcmp(a, b, sizeof a), "LFO deterministic");
        CHECK(fabsf(a[299] - a[0]) > 1e-3f, "fresh DRIFT moves at once");
    }
    ph = 0.25f, rng = 0u, st[0] = st[1] = 0.0f;          /* dt 0 / NaN / negative: no motion; rng 0 recovers */
    v = cy_lfo_step(&ph, &rng, st, CY_LFO_SINE, 1.0f, 0.0f);
    CHECK(ph == 0.25f && fabsf(v - 1.0f) < 1e-5f, "dt 0");
    cy_lfo_step(&ph, &rng, st, CY_LFO_SINE, 1.0f, NAN);
    cy_lfo_step(&ph, &rng, st, CY_LFO_SINE, 1.0f, -5.0f);
    CHECK(ph == 0.25f && rng != 0u, "bad dt, rng 0");
    cy_lfo_step(&ph, &rng, st, CY_LFO_SINE, 4.0f, 100.0f);
    CHECK(ph >= 0.0f && ph < 1.0f, "huge dt keeps phase in [0, 1)");
    ph = NAN;
    v = cy_lfo_step(&ph, &rng, NULL, CY_LFO_SH, 1.0f, dt);
    CHECK(ph >= 0.0f && ph < 1.0f && v == v, "NaN phase / NULL state");
}

/* --------------------------------------------------------------- dump --- */
static void dump(const char *path)
{
    FILE *f = fopen(path, "w");
    int id, i, p;
    char s[24], l[24];
    if (!f) {
        printf("  dump: cannot write %s\n", path);
        return;
    }
    fprintf(f, "# CYMA-1 params: every parameter at 0, 1/4, 1/2, 3/4, 1 of its norm and at its default\n");
    fprintf(f, "# id\tname\tunit\tlo\thi\tdef\tcurve\tdetents\tbipolar\tpage\t| short / long ...\n");
    for (id = 0; id < P_NPARAMS; id++) {
        const cy_param_def_t *d = &CY_PARAMS[id];
        float pts[6] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f, cy_param_norm(id, d->def)};
        fprintf(f, "%d\t%s\t%s\t%g\t%g\t%g\t%s\t%d\t%d\t%s\t|", id, d->name, d->unit, d->lo, d->hi, d->def,
                d->curve == CY_CV_LIN ? "LIN" : d->curve == CY_CV_LOG ? "LOG" : d->curve == CY_CV_ENUM ? "ENUM" : "MORPH",
                d->detents, d->bipolar, d->page < PG_N ? cy_page_name(d->page) : "-");
        for (i = 0; i < 6; i++) {
            cy_param_format(id, pts[i], s, sizeof s, 0);
            cy_param_format(id, pts[i], l, sizeof l, 1);
            fprintf(f, "\t%s / %s", s, l);
        }
        fprintf(f, "\n");
    }
    fprintf(f, "\n# hints (%s)\n", "<= 34 chars, FONT_CY_LBL 32..95");
    for (id = 0; id < P_NPARAMS; id++)
#ifdef T_FONTS
        fprintf(f, "%-10s\t%2d ch %3d px\t%s\n", CY_PARAMS[id].name, (int)strlen(CY_PARAMS[id].hint),
                ink_w(&FONT_CY_LBL, CY_PARAMS[id].hint, 0), CY_PARAMS[id].hint);
#else
        fprintf(f, "%-10s\t%2d ch\t%s\n", CY_PARAMS[id].name, (int)strlen(CY_PARAMS[id].hint), CY_PARAMS[id].hint);
#endif
    fprintf(f, "\n# pages (MALLET / BOW / RAIN where they differ)\n");
    for (p = 0; p < PG_N; p++)
        for (i = 0; i < 2; i++) {
            int k, x;
            fprintf(f, "%-6s %c:", cy_page_name(p), 'A' + i);
            for (k = 0; k < 4; k++) {
                int a = cy_page_param(p, 0, i, k);
                fprintf(f, "  %s", CY_PARAMS[a].name);
                for (x = 1; x < CY_NEXC; x++)
                    if (cy_page_param(p, x, i, k) != a)
                        fprintf(f, "/%s", CY_PARAMS[cy_page_param(p, x, i, k)].name);
            }
            fprintf(f, "\n");
        }
    fprintf(f, "\n# factory presets, decoded (short formats; '.' = the default)\n");
    for (p = 0; p < CY_NFACTORY; p++) {
        float n[P_NPARAMS], d[P_NPARAMS];
        char nm[16];
        cy_params_defaults(d);
        cy_params_defaults(n);
        cy_preset_load(&CY_FACTORY[p], n);
        preset_name(&CY_FACTORY[p], nm);
        fprintf(f, "%02d %-14s", p + 1, nm);
        for (id = 0; id < P_NPRESET; id++) {
            if (fabsf(n[id] - d[id]) <= 1.01f / 65535.0f)
                continue;
            cy_param_format(id, n[id], s, sizeof s, 1);
            fprintf(f, "  %s %s", CY_PARAMS[id].name, s);
        }
        fprintf(f, "\n");
    }
    fprintf(f, "\n# norm matrix (rows: presets 01..24; cols: P_SHAPE..P_VIB_RATE; 16-bit)\n");
    for (p = 0; p < CY_NFACTORY; p++) {
        for (id = 0; id < P_NPRESET; id++)
            fprintf(f, "%s%u", id ? "," : "", CY_FACTORY[p].v[id]);
        fprintf(f, "\n");
    }
    fclose(f);
    printf("  dump: wrote %s\n", path);
}

/* fix-app: a factory preset's LFO morph stays in the shapes its voicing names. AETHER (§4.4: "slowly morphs between
 * circle and hexagon") at both LFO extremes reaches exactly CIRCLE (1) and HEX (2), never DRUM or SQUARE; and every
 * preset's morph span is 4 x MORPH_DEPTH shape units, as cy_patch_build's 2 x MORPH x lfo promises */
static void test_morph_span(void)
{
    float n[P_NPARAMS], lo, hi;
    cy_patch_t p;
    cy_geom_in_t g;
    cy_mod_t m = {0.0f, 0.0f, 0.0f, 0.0f, 0};
    int i;
    for (i = 0; i < CY_NFACTORY; i++) {
        float sh, md;
        cy_params_defaults(n);
        cy_preset_load(&CY_FACTORY[i], n);
        sh = cy_param_value(P_SHAPE, n[P_SHAPE]);
        md = cy_param_value(P_MORPH_DEPTH, n[P_MORPH_DEPTH]) / 100.0f;
        m.lfo = -1.0f;
        cy_patch_build(n, &m, &p, &g);
        lo = g.shape;
        m.lfo = 1.0f;
        cy_patch_build(n, &m, &p, &g);
        hi = g.shape;
        CHECK(fabsf(lo - fmaxf(sh - 2.0f * md, 0.0f)) < 1e-4f && fabsf(hi - fminf(sh + 2.0f * md, 8.0f)) < 1e-4f,
              "%s: LFO morph %.3f..%.3f, want SHAPE %.2f +- 2 x %.2f", CY_FACTORY[i].name, lo, hi, sh, md);
        if (!strncmp(CY_FACTORY[i].name, "AETHER", 6))
            CHECK(fabsf(lo - 1.0f) < 1e-4f && fabsf(hi - 2.0f) < 1e-4f, "AETHER morphs %.3f..%.3f, want CIRCLE..HEX",
                  lo, hi);
    }
}

int main(int argc, char **argv)
{
    printf("test_params: CYMA-1 M4 (params, presets, patch, LFO)\n");
    test_table();
    test_roundtrip();
    test_pages();
    test_steps();
    test_format();
    test_hints();
    test_presets();
    test_store();
    test_lerp();
    test_patch();
    test_morph_span();
    test_lfo();
    if (argc > 2 && !strcmp(argv[1], "--dump"))
        dump(argv[2]);
    printf("test_params: %d checks, %d failed: %s\n", t_checks, t_fail, t_fail ? "FAIL" : "PASS");
    return t_fail ? 1 : 0;
}
