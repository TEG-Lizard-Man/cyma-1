/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 object builder (M1, spec §3.2): the shape morph, the published object (what the audio side plays) and the
 * field cache (what the UI draws). Main loop only: the audio interrupt reads cy_geom_object() once per render and
 * otherwise only SURFACE rain's field reads, guarded by cy_geom_fields_stable() (below).
 *
 * The data flow, per cy_geom_update():
 *   CY_PLATES (offline scipy eigenmodes, tracked so mode k is "the same mode" in every shape)
 *     -> morph seg/t: field, SDF (int8 lerps), cover (AA mask, <= 30 Hz while moving), Lambda, monopole, clamped
 *     -> ratios: STIFFNESS (membrane -> plate -> bell), CARVE (pull toward a target set), SHIMMER (split twins)
 *     -> damping: the material law; order; strike / mic / key / rain / wander gains sampled from the field cache
 *     -> publish into the back object, seq++, swap.
 * Only what an input change touches is rebuilt (a dirty mask), so an LFO on POS costs a few thousand MACs and an
 * unchanged frame costs a compare.
 *
 * Conventions (shared with the UI): the plate square is [0,1]^2, u right, v down. field[k][i][j] sits at the cell
 * centre ((j + .5)/24, (i + .5)/24); sdf[i][j] at ((j + .5)/48, (i + .5)/48), plate units x 128, < 0 inside;
 * cover[i][j] at ((j + .5)/92, (i + .5)/92), 255 inside. The samplers clamp to the outermost centres.
 *
 * Where this goes past the letter of §3.2 (each where the spec is silent or its rule fails a shape):
 *   - the strike point is pulled inside like the mics (a DRUM struck off the plate is silent); pos_uv is that point;
 *   - "pull inside" lands on the -.03 contour instead of up to a step past it (no 2 %-of-radius jumps while the
 *     shape morphs), and a point the 50 radial steps cannot bring in (6:1 BAR, STAR valleys) walks the SDF gradient;
 *   - geom's own geometry reads the two keyframes' SDFs lerped in float, not the int8 cache (no 1/128 ticks);
 *   - the 16 loudness probes sit on the spec's .2 .. .8 grid scaled to the shape's centroid and spread (the BAR has
 *     no probe inside the unit-square grid), with soft inside weights so out_norm is continuous;
 *   - mid-morph, CARVE and SHIMMER are evaluated with both keyframes' twin groups and lerped (no jump at t = .5);
 *     the published degen[] is the nearer keyframe's. */
#include "cy_plates.h"
#include "fastmath.h"
#include "plat.h"                       /* plat_ms: the cover's 30 Hz ceiling; PLAT_WHITE_KEYS / PLAT_BLACK_KEYS */

#define CY_G_PLANE (CY_GRID * CY_GRID)
#define CY_G_NF (CY_NMODES * CY_G_PLANE)          /* 18,432 field bytes */
#define CY_G_NS (CY_SDF * CY_SDF)                 /* 2,304 SDF bytes */
#define CY_G_COVER_MS 33u                         /* <= 30 Hz cover rebuilds while the shape moves */

enum { CY_G_DSHAPE = 1, CY_G_DRATIO = 2, CY_G_DDAMP = 4, CY_G_DPOS = 8, CY_G_DMIC = 16, CY_G_DRAIN = 32,
       CY_G_DWANDER = 64, CY_G_DALL = 127 };

static cy_object_t cy_g_obj[2] CY_POOL;           /* published / being built (spec §3.15: 2 x 11.1 KB) */
static cy_fieldcache_t cy_g_fc CY_POOL;           /* 29.2 KB: one copy, the UI reads it on the same thread */
static const cy_object_t *volatile cy_g_front = &cy_g_obj[0];
/* The one exception to "the audio interrupt never calls in here": SURFACE rain reads the field cache (engine.c
 * cy_rain_surface), which the main loop lerps in place. cy_g_fseq is odd while it does; cy_g_fpub is its value when
 * the published object was built from it. The render reads the cache only when the two are equal and even. */
static volatile uint32_t cy_g_fseq, cy_g_fpub;
#define CY_G_BARRIER() __asm__ volatile("" ::: "memory")

static struct {
    cy_geom_in_t in;                    /* the (sanitised) inputs the published object was built from */
    uint8_t valid, cover_dirty, seg, rsv;
    float t;
    float lam[CY_NMODES];               /* Lambda_k after the morph, min 1 */
    float mono[CY_NMODES];
    float pre[CY_NMODES];               /* ratio after STIFFNESS: CARVE's input */
    float sig_l[CY_NMODES], sig_r[CY_NMODES];   /* far-field pickup signs (seed 0xC0FFEE) */
    float key0[CY_NKEYS][2];            /* the keybed on the plate before POS pans it */
    float pu, pv;                       /* the strike point, pulled inside */
    float cu, cv, su, sv;               /* the inside's centroid and spread: the loudness probes' frame */
    uint32_t cover_ms;
} cy_g;

static uint8_t cy_g_ci[CY_COVER];       /* cover cell -> SDF column / row and its weight (separable, fixed) */
static float cy_g_cf[CY_COVER];

static const char *const CY_G_SHORT[CY_NSHAPES] = {"DRM", "CIR", "HEX", "SQR", "TRI", "STR", "GTR", "STD", "BAR"};

/* spec §3.2.6, as log2 so the law lerps with exp2/log2 only: (log2 T0 [s at 261.63 Hz], log2 beta, gamma) */
static const float CY_G_MAT[CY_NMAT][3] = {
    {-2.473931188f, 1.584962501f, 1.6f},    /* RUBBER  0.18 s, 3.0 */
    {-0.321928095f, 0.584962501f, 1.5f},    /* WOOD    0.80 s, 1.5 */
    {1.137503524f, -1.514573173f, 1.1f},    /* GLASS   2.2 s, 0.35 */
    {2.321928095f, -3.058893689f, 0.9f},    /* STEEL   5.0 s, 0.12 */
    {3.321928095f, -4.058893689f, 0.75f},   /* BRONZE 10.0 s, 0.06 */
    {4.807354922f, -6.058893689f, 0.5f},    /* AETHER 28.0 s, 0.015 */
};
/* CARVE targets as log2 (spec §3.2.4): BELL {1, 2, 2.4, 3, 4, 5.333, 6, 8, 10.667, 12, 16}, BAR {1, 4, 10, 20, 32} */
static const float CY_G_BELL[11] = {0.0f, 1.0f, 1.263034406f, 1.584962501f, 2.0f, 2.415037499f, 2.584962501f, 3.0f,
                                    3.415037499f, 3.584962501f, 4.0f};
static const float CY_G_BAR[5] = {0.0f, 2.0f, 3.321928095f, 4.321928095f, 5.0f};

/* ------------------------------------------------------------------------------------------------ helpers --- */
static float cy_g_clampf(float x, float lo, float hi) { return x >= lo ? (x <= hi ? x : hi) : lo; }   /* NaN -> lo */

static void cy_g_zero(void *p, uint32_t n)       /* n a multiple of 4: every target is a word-aligned struct */
{
    uint32_t *w = (uint32_t *)p, i;
    for (i = 0; i < n >> 2; i++)
        w[i] = 0u;
}
static void cy_g_copy(void *d, const void *s, uint32_t n)
{
    uint32_t *a = (uint32_t *)d, i;
    const uint32_t *b = (const uint32_t *)s;
    for (i = 0; i < n >> 2; i++)
        a[i] = b[i];
}

float cy_geom_sdf_at(const cy_fieldcache_t *f, float u, float v)
{
    float x = cy_g_clampf(u * (float)CY_SDF - 0.5f, 0.0f, (float)(CY_SDF - 1));
    float y = cy_g_clampf(v * (float)CY_SDF - 0.5f, 0.0f, (float)(CY_SDF - 1));
    int j = (int)x, i = (int)y;
    const int8_t *p;
    float a, b;
    if (j > CY_SDF - 2) j = CY_SDF - 2;
    if (i > CY_SDF - 2) i = CY_SDF - 2;
    x -= (float)j, y -= (float)i;
    p = &f->sdf[i][j];
    a = (float)p[0] + x * (float)(p[1] - p[0]);
    b = (float)p[CY_SDF] + x * (float)(p[CY_SDF + 1] - p[CY_SDF]);
    return (a + y * (b - a)) * (1.0f / 128.0f);
}

float cy_geom_field_at(const cy_fieldcache_t *f, int mode, float u, float v)
{
    float x = cy_g_clampf(u * (float)CY_GRID - 0.5f, 0.0f, (float)(CY_GRID - 1));
    float y = cy_g_clampf(v * (float)CY_GRID - 0.5f, 0.0f, (float)(CY_GRID - 1));
    int j = (int)x, i = (int)y;
    const int8_t *p;
    float a, b;
    if (j > CY_GRID - 2) j = CY_GRID - 2;
    if (i > CY_GRID - 2) i = CY_GRID - 2;
    mode = mode < 0 ? 0 : mode >= CY_NMODES ? CY_NMODES - 1 : mode;
    x -= (float)j, y -= (float)i;
    p = &f->field[mode][i][j];
    a = (float)p[0] + x * (float)(p[1] - p[0]);
    b = (float)p[CY_GRID] + x * (float)(p[CY_GRID + 1] - p[CY_GRID]);
    return (a + y * (b - a)) * (1.0f / 127.0f);
}

/* one point's bilinear weights, reused for all 32 modes (a point costs 4 loads and 4 MACs per mode) */
typedef struct { int o; float w0, w1, w2, w3; } cy_g_tap_t;
static void cy_g_tap(cy_g_tap_t *t, float u, float v)
{
    float x = cy_g_clampf(u * (float)CY_GRID - 0.5f, 0.0f, (float)(CY_GRID - 1));
    float y = cy_g_clampf(v * (float)CY_GRID - 0.5f, 0.0f, (float)(CY_GRID - 1));
    int j = (int)x, i = (int)y;
    if (j > CY_GRID - 2) j = CY_GRID - 2;
    if (i > CY_GRID - 2) i = CY_GRID - 2;
    x -= (float)j, y -= (float)i;
    t->o = i * CY_GRID + j;
    t->w0 = (1.0f - x) * (1.0f - y), t->w1 = x * (1.0f - y), t->w2 = (1.0f - x) * y, t->w3 = x * y;
}
static void cy_g_sample(const cy_g_tap_t *t, float *out)       /* out[k] = phi_k / 127, k = 0..31 */
{
    const int8_t *p = &cy_g_fc.field[0][0][0] + t->o;
    int k;
    for (k = 0; k < CY_NMODES; k++, p += CY_G_PLANE)
        out[k] = (t->w0 * (float)p[0] + t->w1 * (float)p[1] + t->w2 * (float)p[CY_GRID]
                  + t->w3 * (float)p[CY_GRID + 1]) * (1.0f / 127.0f);
}

/* "pull inside" (spec §3.2.9): while the SDF says outside or within .03 of the edge, step 2 % of the way toward the
 * plate centre, at most 50 steps (64 % of the way in). That covers the default keybed on every shape, but not the
 * 6:1 BAR with POS at an extreme, nor a wide rain scatter in a STAR valley: a point still on or over the edge then
 * walks down the SDF's gradient to .03 inside (Newton on a distance, |grad| ~ 1), so nothing is ever published off
 * the plate (a DRUM strike there would be silent, a dot drawn there would float in the void). */
/* geom's own distance: the two keyframes' SDFs sampled and lerped in float, so the points and the loudness probes
 * move continuously with SHAPE (the published int8 lerp ticks in 1/128 steps; a probe weight read from it would
 * jump by a quarter at a tick) */
static float cy_g_sdf(float u, float v)
{
    float x = cy_g_clampf(u * (float)CY_SDF - 0.5f, 0.0f, (float)(CY_SDF - 1));
    float y = cy_g_clampf(v * (float)CY_SDF - 0.5f, 0.0f, (float)(CY_SDF - 1));
    int j = (int)x, i = (int)y, o;
    const int8_t *a = &CY_PLATES[cy_g.seg].sdf[0][0], *b = &CY_PLATES[cy_g.seg + 1].sdf[0][0];
    float w0, w1, w2, w3, da, db;
    if (j > CY_SDF - 2) j = CY_SDF - 2;
    if (i > CY_SDF - 2) i = CY_SDF - 2;
    x -= (float)j, y -= (float)i;
    o = i * CY_SDF + j;
    w0 = (1.0f - x) * (1.0f - y), w1 = x * (1.0f - y), w2 = (1.0f - x) * y, w3 = x * y;
    da = w0 * (float)a[o] + w1 * (float)a[o + 1] + w2 * (float)a[o + CY_SDF] + w3 * (float)a[o + CY_SDF + 1];
    if (cy_g.t <= 0.0f)
        return da * (1.0f / 128.0f);
    db = w0 * (float)b[o] + w1 * (float)b[o + 1] + w2 * (float)b[o + CY_SDF] + w3 * (float)b[o + CY_SDF + 1];
    return (da + cy_g.t * (db - da)) * (1.0f / 128.0f);
}

static void cy_g_inside(float *u, float *v)
{
    const float h = 1.0f / (float)CY_SDF;
    float d = cy_g_sdf(*u, *v), pu = *u, pv = *v, pd = d;
    int n;
    for (n = 0; n < 50 && d >= -0.03f; n++) {
        pu = *u, pv = *v, pd = d;
        *u += 0.02f * (0.5f - *u), *v += 0.02f * (0.5f - *v);
        d = cy_g_sdf(*u, *v);
    }
    if (n > 0 && d < -0.03f && pd > d) {                 /* land on the -.03 contour, not up to a step past it: */
        float f = (pd + 0.03f) / (pd - d);              /* the stepping's integer count would make points jump */
        *u = pu + f * (*u - pu), *v = pv + f * (*v - pv);   /* by 2 % of their radius as the shape morphs */
        return;
    }
    for (n = 0; n < 8 && d >= -0.01f; n++) {
        float gx = cy_g_sdf(*u + h, *v) - cy_g_sdf(*u - h, *v);
        float gy = cy_g_sdf(*u, *v + h) - cy_g_sdf(*u, *v - h);
        float g2 = gx * gx + gy * gy, k;
        if (g2 < 4e-4f * h * h)                          /* |grad| < .01: no direction to walk */
            break;
        k = (d + 0.03f) * 2.0f * h / g2;                 /* grad = g / 2h: step (d + .03) / |grad| down it */
        *u = cy_g_clampf(*u - k * gx, 0.0f, 1.0f);
        *v = cy_g_clampf(*v - k * gy, 0.0f, 1.0f);
        d = cy_g_sdf(*u, *v);
    }
}

/* ------------------------------------------------------------------------------------------------- shape --- */
/* the int8 tables lerp in 16.16 fixed point: exact to the rounding (w = t 65536), no float per byte, and the
 * +128 bias keeps the shift on a non-negative value (portable rounding, half up) */
static void cy_g_lerp8(int8_t *d, const int8_t *a, const int8_t *b, uint32_t n, int32_t w)
{
    uint32_t i;
    int32_t wa = 65536 - w;
    if (w <= 0) {
        for (i = 0; i < n; i++) d[i] = a[i];
        return;
    }
    if (w >= 65536) {
        for (i = 0; i < n; i++) d[i] = b[i];
        return;
    }
    for (i = 0; i < n; i++) {
        int32_t x = (int32_t)a[i] * wa + (int32_t)b[i] * w + (128 << 16) + 32768;
        d[i] = (int8_t)((int32_t)((uint32_t)x >> 16) - 128);
    }
}

/* cover[i][j] = clamp(.5 - d_px / 2, 0, 1) 255, d_px = SDF x 184 (the plate box): a half-resolution AA mask */
static void cy_g_cover(void)
{
    int i, j;
    for (i = 0; i < CY_COVER; i++) {
        const int8_t *r0 = cy_g_fc.sdf[cy_g_ci[i]], *r1 = r0 + CY_SDF;
        float fy = cy_g_cf[i];
        for (j = 0; j < CY_COVER; j++) {
            int c = cy_g_ci[j];
            float fx = cy_g_cf[j];
            float a = (float)r0[c] + fx * (float)(r0[c + 1] - r0[c]);
            float b = (float)r1[c] + fx * (float)(r1[c + 1] - r1[c]);
            float d = (a + fy * (b - a)) * (92.0f / 128.0f);   /* d_px / 2 = SDF x 184 / 2 */
            cy_g_fc.cover[i][j] = (uint8_t)(cy_g_clampf(0.5f - d, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
    cy_g.cover_ms = plat_ms();
    cy_g.cover_dirty = 0;
}

static void cy_g_shape(cy_object_t *o, float shape, uint32_t now)
{
    int seg = shape >= 7.0f ? 7 : (int)shape, k, i, j;
    float t = shape - (float)seg, mn = 1e30f, sw = 0.0f, su = 0.0f, sv = 0.0f, suu = 0.0f, svv = 0.0f;
    const cy_plate_t *a = &CY_PLATES[seg], *b = &CY_PLATES[seg + 1];
    int32_t w = (int32_t)(t * 65536.0f + 0.5f);
    cy_g.seg = (uint8_t)seg, cy_g.t = t;
    cy_g_fseq = cy_g_fseq + 1u;                                /* odd: the cache is being rewritten */
    CY_G_BARRIER();
    cy_g_lerp8(&cy_g_fc.field[0][0][0], &a->field[0][0][0], &b->field[0][0][0], CY_G_NF, w);
    cy_g_lerp8(&cy_g_fc.sdf[0][0], &a->sdf[0][0], &b->sdf[0][0], CY_G_NS, w);
    CY_G_BARRIER();
    cy_g_fseq = cy_g_fseq + 1u;
    cy_g.cover_dirty = 1;
    if ((uint32_t)(now - cy_g.cover_ms) >= CY_G_COVER_MS)
        cy_g_cover();
    cy_g_fc.seq++;
    for (k = 0; k < CY_NMODES; k++) {                          /* Lambda: a geometric lerp, then min 1 */
        cy_g.lam[k] = fm_exp2f(a->log2_lambda[k] + t * (b->log2_lambda[k] - a->log2_lambda[k]));
        cy_g.mono[k] = a->monopole[k] + t * (b->monopole[k] - a->monopole[k]);
        if (cy_g.lam[k] < mn)
            mn = cy_g.lam[k];
    }
    mn = 1.0f / mn;
    for (k = 0; k < CY_NMODES; k++)
        cy_g.lam[k] *= mn;
    for (k = 0; k < CY_NMODES; k++)                             /* the nearer keyframe's twins */
        o->degen[k] = (t < 0.5f ? a : b)->degen[k];
    o->clamped = (float)a->clamped + t * (float)((int)b->clamped - (int)a->clamped);
    /* the inside's centroid and spread from the SDF (a soft inside weight, so it moves continuously with t): the
     * loudness probes sit at the spec's 0.2 .. 0.8 grid of a unit-square plate, scaled to the shape's own extent,
     * so a thin BAR gets the same probe density as a CIRCLE */
    for (i = 0; i < CY_SDF; i++)
        for (j = 0; j < CY_SDF; j++) {
            float sa = (float)a->sdf[i][j], sd = sa + t * ((float)b->sdf[i][j] - sa);
            float wt = cy_g_clampf(0.5f - sd * (48.0f / 128.0f), 0.0f, 1.0f);
            float u = ((float)j + 0.5f) * (1.0f / CY_SDF), v = ((float)i + 0.5f) * (1.0f / CY_SDF);
            sw += wt, su += wt * u, sv += wt * v, suu += wt * u * u, svv += wt * v * v;
        }
    if (sw < 1.0f) {
        cy_g.cu = cy_g.cv = 0.5f, cy_g.su = cy_g.sv = 0.05f;
    } else {
        sw = 1.0f / sw;
        cy_g.cu = su * sw, cy_g.cv = sv * sw;
        cy_g.su = fm_sqrtf(fm_maxf(suu * sw - cy_g.cu * cy_g.cu, 1e-6f));
        cy_g.sv = fm_sqrtf(fm_maxf(svv * sw - cy_g.cv * cy_g.cv, 1e-6f));
    }
}

/* ------------------------------------------------------------------------------------------------ ratios --- */
/* CARVE then SHIMMER with one keyframe's twin groups, into log2 ratios (spec §3.2.4-5). Mid-morph both keyframes'
 * groupings are evaluated and their results lerped by t, so neither changes in a jump at t = .5 (what remains is
 * inherent: a rank swap or HARMONIC's rounding, which the engine's object glide smooths). */
static void cy_g_carve(const float *r, const uint8_t *dg, int target, float carve, float shimmer, float *lr)
{
    float gsum[CY_NMODES], glog[CY_NMODES];
    uint8_t gcnt[CY_NMODES], gidx[CY_NMODES], list[CY_NMODES], seen[CY_NMODES], has[CY_NMODES];
    int k, n = 0, a, b;
    for (k = 0; k < CY_NMODES; k++)
        gsum[k] = 0.0f, gcnt[k] = 0, seen[k] = 0, has[k] = 0;
    for (k = 0; k < CY_NMODES; k++) {
        int g = dg[k] & (CY_NMODES - 1);
        gsum[g] += r[k];
        gcnt[g]++;
    }
    for (k = 0; k < CY_NMODES; k++)
        if (gcnt[k]) {
            gsum[k] /= (float)gcnt[k];                  /* the group's ratio: its members' mean */
            list[n++] = (uint8_t)k;
        }
    for (a = 1; a < n; a++)                             /* rank groups by ratio (insertion: n <= 32, stable) */
        for (b = a; b > 0 && gsum[list[b]] < gsum[list[b - 1]]; b--) {
            uint8_t x = list[b];
            list[b] = list[b - 1], list[b - 1] = x;
        }
    for (a = 0; a < n; a++) {
        int g = list[a];
        has[g] = 1;
        if (target == CY_CARVE_HARMONIC) {
            float h = (float)(int)(gsum[g] + 0.5f);
            glog[g] = a == 0 ? 0.0f : fm_log2f(h < 2.0f ? 2.0f : h);
        } else if (target == CY_CARVE_BELL && a < 11) {
            glog[g] = CY_G_BELL[a];
        } else if (target == CY_CARVE_BAR && a < 5) {
            glog[g] = CY_G_BAR[a];
        } else {
            has[g] = 0;                                 /* beyond the list: unchanged */
        }
    }
    for (k = 0; k < CY_NMODES; k++) {
        int g = dg[k] & (CY_NMODES - 1), m = gcnt[g];
        float l = fm_log2f(r[k]);
        if (has[g])
            l += carve * (glog[g] - l);
        gidx[k] = seen[g]++;                             /* members in tracked order: stable through a morph */
        if (m > 1)
            l += ((float)gidx[k] - 0.5f * (float)(m - 1)) * shimmer / (float)(m - 1) * (1.0f / 1200.0f);
        lr[k] = l;
    }
}

static void cy_g_ratios(cy_object_t *o, const cy_geom_in_t *q)
{
    float s = q->stiffness, s1 = fm_minf(s, 0.8f) * 1.25f, eps = s1 * s1 / (1.0f - s1 * s1 + 0.02f), mn = 1e30f;
    float p = 1.0f + 1.25f * (s - 0.8f);
    int k, a, b;
    for (k = 0; k < CY_NMODES; k++) {                   /* STIFFNESS: s 0 membrane (sqrt), .8 plate (~lambda) */
        float L = cy_g.lam[k], r = fm_sqrtf((L + eps * L * L) / (1.0f + eps));
        if (s > 0.8f)
            r = fm_powf(r, p);                          /* BELL+: stretched */
        cy_g.pre[k] = r;
        if (r < mn)
            mn = r;
    }
    mn = 1.0f / mn;                                     /* (the lowest is 1 analytically; this makes it exact) */
    for (k = 0; k < CY_NMODES; k++)
        cy_g.pre[k] *= mn;
    if (q->carve <= 0.0f && q->shimmer_cents <= 0.0f) {
        for (k = 0; k < CY_NMODES; k++)
            o->ratio[k] = cy_g.pre[k], o->log2_ratio[k] = fm_log2f(cy_g.pre[k]);
    } else {
        float la[CY_NMODES], lb[CY_NMODES], t = cy_g.t;
        cy_g_carve(cy_g.pre, CY_PLATES[cy_g.seg].degen, q->carve_target, q->carve, q->shimmer_cents, la);
        if (t > 0.0f)
            cy_g_carve(cy_g.pre, CY_PLATES[cy_g.seg + 1].degen, q->carve_target, q->carve, q->shimmer_cents, lb);
        for (k = 0; k < CY_NMODES; k++) {
            float l = t > 0.0f ? la[k] + t * (lb[k] - la[k]) : la[k];
            o->log2_ratio[k] = l, o->ratio[k] = fm_exp2f(l);
        }
    }
    for (k = 0; k < CY_NMODES; k++)                     /* order: ascending ratio, ties by index */
        o->order[k] = (uint8_t)k;
    for (a = 1; a < CY_NMODES; a++)
        for (b = a; b > 0 && o->ratio[o->order[b]] < o->ratio[o->order[b - 1]]; b--) {
            uint8_t x = o->order[b];
            o->order[b] = o->order[b - 1], o->order[b - 1] = x;
        }
}

/* the material law (spec §3.2.6): (log T0, log beta, gamma) lerped between the six anchors; damp_div_k = 1 +
 * beta_eff (ratio^gamma - 1), so the lowest mode rings for exactly T0 and the rest shorter by the law */
static void cy_g_damp(cy_object_t *o, const cy_geom_in_t *q)
{
    float m = q->material, f, lt, lb, g, beff;
    int i = m >= 4.0f ? 4 : (int)m, k;
    f = m - (float)i;
    lt = CY_G_MAT[i][0] + f * (CY_G_MAT[i + 1][0] - CY_G_MAT[i][0]);
    lb = CY_G_MAT[i][1] + f * (CY_G_MAT[i + 1][1] - CY_G_MAT[i][1]);
    g = CY_G_MAT[i][2] + f * (CY_G_MAT[i + 1][2] - CY_G_MAT[i][2]);
    beff = fm_exp2f(lb - 2.0f * q->bright);             /* beta 4^-bright */
    o->t0 = fm_exp2f(lt);
    for (k = 0; k < CY_NMODES; k++)
        o->damp_div[k] = fm_maxf(1.0f + beff * (fm_exp2f(g * o->log2_ratio[k]) - 1.0f), 0.1f);
}

/* --------------------------------------------------------------------------------------------- the points --- */
static void cy_g_pos(cy_object_t *o, const cy_geom_in_t *q)
{
    cy_g_tap_t tp;
    float du, dv;
    int i;
    cy_g.pu = q->pos_u, cy_g.pv = q->pos_v;
    cy_g_inside(&cy_g.pu, &cy_g.pv);                    /* a strike off the plate would be silent on DRUM */
    o->pos_uv[0] = cy_g.pu, o->pos_uv[1] = cy_g.pv;
    cy_g_tap(&tp, cy_g.pu, cy_g.pv);
    cy_g_sample(&tp, o->strike);
    du = (q->pos_u - 0.62f) * 0.5f, dv = (q->pos_v - 0.40f) * 0.5f;   /* POS pans the keybed over the plate */
    for (i = 0; i < CY_NKEYS; i++) {
        float u = cy_g.key0[i][0] + du, v = cy_g.key0[i][1] + dv;
        cy_g_inside(&u, &v);
        o->key_uv[i][0] = u, o->key_uv[i][1] = v;
        cy_g_tap(&tp, u, v);
        cy_g_sample(&tp, o->surface[i]);
    }
}

/* points scattered around the strike point: r = R sqrt(u1) (uniform over the disc), a = 2 pi u2, re-seeded on every
 * build so a rebuild is bit-identical */
static void cy_g_scatter(float (*uv)[2], float (*tab)[CY_NMODES], int n, float radius, uint32_t seed)
{
    cy_g_tap_t tp;
    int i;
    for (i = 0; i < n; i++) {
        float r = radius * fm_sqrtf(cy_randf(&seed)), a = 6.28318531f * cy_randf(&seed);
        float u = cy_g.pu + r * fm_cosf(a), v = cy_g.pv + r * fm_sinf(a);
        cy_g_inside(&u, &v);
        uv[i][0] = u, uv[i][1] = v;
        cy_g_tap(&tp, u, v);
        cy_g_sample(&tp, tab[i]);
    }
}

/* the two mics (spec §3.2.9) and the loudness normaliser (§3.2.10) */
static void cy_g_mics(cy_object_t *o, const cy_geom_in_t *q)
{
    static const float PROBE[4] = {-1.03923048f, -0.34641016f, 0.34641016f, 1.03923048f};   /* +-.1, +-.3 of a box */
    float pl[CY_NMODES], pr[CY_NMODES], ph[CY_NMODES], g2[CY_NMODES], e = 0.0f, ws = 0.0f;
    cy_g_tap_t tp;
    int k, a, b;
    o->mic_uv[0][0] = 0.5f - 0.33f * q->mic_spread, o->mic_uv[0][1] = 0.62f;
    o->mic_uv[1][0] = 0.5f + 0.33f * q->mic_spread, o->mic_uv[1][1] = 0.62f;
    cy_g_inside(&o->mic_uv[0][0], &o->mic_uv[0][1]);
    cy_g_inside(&o->mic_uv[1][0], &o->mic_uv[1][1]);
    cy_g_tap(&tp, o->mic_uv[0][0], o->mic_uv[0][1]);
    cy_g_sample(&tp, pl);
    cy_g_tap(&tp, o->mic_uv[1][0], o->mic_uv[1][1]);
    cy_g_sample(&tp, pr);
    for (k = 0; k < CY_NMODES; k++) {                   /* near field + a far-field pickup + the monopole */
        /* Below ratio 2 the far field is one shared sign, opening to the independent L/R signs by ratio 4
         * (integration fix, M7). Tracked mode 1 is the dipole that is the fundamental of 20 of the 24 factory
         * presets; it is antisymmetric about u = .5, so the mirrored mics hear it in opposite phase, and its two
         * random signs happen to be opposite too: mic_l = -mic_r exactly, a fundamental that vanished in mono
         * (MARIMBA and KALIMBA sounded at their 2.7-3.9x partial). Bass mono-compatible, overtones still wide. */
        float w = cy_g_clampf(o->log2_ratio[k] - 1.0f, 0.0f, 1.0f);
        float sr = cy_g.sig_l[k] + w * (cy_g.sig_r[k] - cy_g.sig_l[k]);
        o->mic_l[k] = 0.75f * pl[k] + 0.35f * cy_g.sig_l[k] + 0.6f * cy_g.mono[k];
        o->mic_r[k] = 0.75f * pr[k] + 0.35f * sr + 0.6f * cy_g.mono[k];
        g2[k] = 0.5f * (o->mic_l[k] * o->mic_l[k] + o->mic_r[k] * o->mic_r[k]);
    }
    for (a = 0; a < 4; a++)                             /* 16 probes, soft inside weights (continuous in t) */
        for (b = 0; b < 4; b++) {
            float u = cy_g.cu + PROBE[b] * cy_g.su, v = cy_g.cv + PROBE[a] * cy_g.sv, s = 0.0f;
            float w = cy_g_clampf(-cy_g_sdf(u, v) * (1.0f / 0.03f), 0.0f, 1.0f);
            if (w <= 0.0f)
                continue;
            cy_g_tap(&tp, u, v);
            cy_g_sample(&tp, ph);
            for (k = 0; k < CY_NMODES; k++)
                s += ph[k] * ph[k] * g2[k];
            e += w * s, ws += w;
        }
    if (ws < 1e-3f) {                                   /* (no probe inside: the centroid, pulled in) */
        float u = cy_g.cu, v = cy_g.cv;
        cy_g_inside(&u, &v);
        cy_g_tap(&tp, u, v);
        cy_g_sample(&tp, ph);
        for (k = 0; k < CY_NMODES; k++)
            e += ph[k] * ph[k] * g2[k];
        ws = 1.0f;
    }
    o->out_norm = 0.12f / fm_sqrtf(e / ws + 1e-6f);
}

/* ------------------------------------------------------------------------------------------------ the API --- */
static void cy_g_sanitise(const cy_geom_in_t *in, cy_geom_in_t *q)
{
    cy_g_zero(q, sizeof *q);
    q->shape = cy_g_clampf(in->shape, 0.0f, (float)(CY_NSHAPES - 1));
    q->stiffness = cy_g_clampf(in->stiffness, 0.0f, 1.0f);
    q->carve = cy_g_clampf(in->carve, 0.0f, 1.0f);
    q->carve_target = in->carve_target < CY_NCARVE ? in->carve_target : 0;
    q->surface = in->surface ? 1 : 0;
    q->shimmer_cents = cy_g_clampf(in->shimmer_cents, 0.0f, 100.0f);
    q->material = cy_g_clampf(in->material, 0.0f, (float)(CY_NMAT - 1));
    q->bright = cy_g_clampf(in->bright, -1.0f, 1.0f);
    q->pos_u = cy_g_clampf(in->pos_u, 0.0f, 1.0f);
    q->pos_v = cy_g_clampf(in->pos_v, 0.0f, 1.0f);
    q->mic_spread = cy_g_clampf(in->mic_spread, 0.0f, 1.0f);
    q->rain_spread = cy_g_clampf(in->rain_spread, 0.0f, 1.0f);
    q->wander = cy_g_clampf(in->wander, 0.0f, 1.0f);
}

void cy_geom_update(const cy_geom_in_t *in)
{
    cy_geom_in_t q;
    const cy_geom_in_t *p = &cy_g.in;
    const cy_object_t *front = cy_g_front;
    cy_object_t *o = front == &cy_g_obj[0] ? &cy_g_obj[1] : &cy_g_obj[0];
    uint32_t now = plat_ms(), d = 0;
    cy_g_sanitise(in, &q);
    if (!cy_g.valid) {
        d = CY_G_DALL;
    } else {
        if (q.shape != p->shape) d |= CY_G_DALL;        /* a new field touches every gain */
        if (q.stiffness != p->stiffness || q.carve != p->carve || q.carve_target != p->carve_target
            || q.shimmer_cents != p->shimmer_cents)
            d |= CY_G_DRATIO | CY_G_DDAMP | CY_G_DMIC;   /* (the far-field share follows the ratio) */
        if (q.material != p->material || q.bright != p->bright) d |= CY_G_DDAMP;
        if (q.pos_u != p->pos_u || q.pos_v != p->pos_v) d |= CY_G_DPOS | CY_G_DRAIN | CY_G_DWANDER;
        if (q.mic_spread != p->mic_spread) d |= CY_G_DMIC;
        if (q.rain_spread != p->rain_spread) d |= CY_G_DRAIN;
        if (q.wander != p->wander) d |= CY_G_DWANDER;
        if (q.surface != p->surface) cy_g.in.surface = q.surface;   /* (the key table is always kept current) */
    }
    if (!d) {                                            /* unchanged: only a stale cover may need its turn */
        if (cy_g.cover_dirty && (uint32_t)(now - cy_g.cover_ms) >= CY_G_COVER_MS) {
            cy_g_cover();
            cy_g_fc.seq++;
        }
        return;
    }
    if (cy_g.valid)
        cy_g_copy(o, front, sizeof *o);                  /* the back starts as the published object */
    if (d & CY_G_DSHAPE)
        cy_g_shape(o, q.shape, now);
    if (d & CY_G_DRATIO)
        cy_g_ratios(o, &q);
    if (d & CY_G_DDAMP)
        cy_g_damp(o, &q);
    if (d & CY_G_DPOS)
        cy_g_pos(o, &q);
    if (d & CY_G_DMIC)
        cy_g_mics(o, &q);
    if (d & CY_G_DRAIN)
        cy_g_scatter(o->rain_uv, o->rain, CY_RAIN_PTS, 0.45f * q.rain_spread, 0xA11CEu);
    if (d & CY_G_DWANDER)
        cy_g_scatter(o->wander_uv, o->wander, CY_WANDER_PTS, 0.30f * q.wander, 0xBEEFu);
    cy_g.in = q;
    cy_g.valid = 1;
    o->seq = front->seq + 1u;
    CY_G_BARRIER();                                      /* (every store that built o lands before the pointer does:
                                                          * the compiler may otherwise sink them past a volatile store) */
    cy_g_front = o;                                      /* publish: the next render reads the new object */
    CY_G_BARRIER();
    cy_g_fpub = cy_g_fseq;                               /* (after the object: a render in between sees a mismatch) */
}

int cy_geom_fields_stable(void);                         /* (declared for the engine in cy_engine_int.h) */
int cy_geom_fields_stable(void)
{
    uint32_t s = cy_g_fseq;
    return !(s & 1u) && s == cy_g_fpub;
}

void cy_geom_init(void)
{
    static const uint8_t WK[16] = PLAT_WHITE_KEYS, BK[11] = PLAT_BLACK_KEYS;
    static const uint8_t AFTER[11] = {0, 1, 2, 4, 5, 7, 8, 9, 11, 12, 14};   /* the white key each black follows */
    cy_geom_in_t def;
    uint32_t s = 0xC0FFEEu;
    int i, k, same = 1;
    cy_g_zero(cy_g_obj, sizeof cy_g_obj);
    cy_g_zero(&cy_g_fc, sizeof cy_g_fc);
    cy_g_zero(&cy_g, sizeof cy_g);
    cy_g_front = &cy_g_obj[0];
    for (i = 0; i < CY_COVER; i++) {                    /* cover cell centre -> SDF cell coordinates */
        float x = cy_g_clampf(((float)i + 0.5f) * ((float)CY_SDF / (float)CY_COVER) - 0.5f, 0.0f,
                              (float)(CY_SDF - 1));
        int c = (int)x;
        if (c > CY_SDF - 2) c = CY_SDF - 2;
        cy_g_ci[i] = (uint8_t)c, cy_g_cf[i] = x - (float)c;
    }
    for (k = 0; k < CY_NMODES; k++) {                   /* fixed far-field signs, L and R drawn apart */
        cy_g.sig_l[k] = (cy_rand(&s) >> 16 & 1u) ? 1.0f : -1.0f;
        cy_g.sig_r[k] = (cy_rand(&s) >> 16 & 1u) ? 1.0f : -1.0f;
        same &= cy_g.sig_l[k] == cy_g.sig_r[k];
    }
    if (same)
        cy_g.sig_r[0] = -cy_g.sig_r[0];
    for (i = 0; i < 16; i++)                             /* the panel's keybed (web/emu index.html), on the plate */
        cy_g.key0[WK[i]][0] = 0.10f + 0.80f * (float)i / 15.0f, cy_g.key0[WK[i]][1] = 0.66f;
    for (i = 0; i < 11; i++)
        cy_g.key0[BK[i]][0] = 0.10f + 0.80f * ((float)AFTER[i] + 0.5f) / 15.0f, cy_g.key0[BK[i]][1] = 0.36f;
    cy_g.cover_ms = plat_ms() - CY_G_COVER_MS;           /* the first build draws its cover at once */
    cy_g_zero(&def, sizeof def);                         /* the parameter defaults (spec §4.1): a valid object now */
    def.shape = 1.0f, def.stiffness = 0.8f, def.shimmer_cents = 3.0f, def.material = 3.0f;
    def.pos_u = 0.62f, def.pos_v = 0.40f, def.mic_spread = 0.5f, def.rain_spread = 0.3f, def.wander = 0.1f;
    cy_geom_update(&def);
}

const cy_object_t *cy_geom_object(void) { return cy_g_front; }
const cy_fieldcache_t *cy_geom_fields(void) { return &cy_g_fc; }
const char *cy_geom_shape_name(int shape) { return CY_PLATES[shape < 0 ? 0 : shape >= CY_NSHAPES ? CY_NSHAPES - 1 : shape].name; }
const char *cy_geom_shape_short(int shape) { return CY_G_SHORT[shape < 0 ? 0 : shape >= CY_NSHAPES ? CY_NSHAPES - 1 : shape]; }
