/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 UI (M5): what the views draw on top of the plate, and the MESH view. Part of the unity build: included by
 * ui/ui.c after gfx2.c and sand.c.
 *
 * Plate marks (SAND and FIELD, spec §6.6 step 4), each from telemetry or geometry, nothing decorative: the strike
 * reticle at the object's POS, ripples from the engine's UI events (strike, rain drop, bow slip, SURFACE touch),
 * the two mics, the bow's contact bar with sparkles in proportion to bow force, and in SURFACE the keybed's 27
 * strike points with pressure rings. They are drawn per strip in screen coordinates (gfx2.c clips), so the
 * per-frame random sparkles are re-seeded per strip from the frame's clock: every strip draws the same dots.
 *
 * In the PHOSPHOR theme the marks take the phosphor's tones (one green CRT, no amber or violet on it).
 *
 * MESH (§6.6): the 24 x 24 field grid as a wireframe lifted by the slow-motion displacement, turning slowly, scaled
 * so the shape fills the box whatever its size. Points are projected once a frame; each strip then draws every
 * segment that crosses it, back to front. A grid line that leaves the plate stops where the SDF crosses zero, so the
 * mesh meets the rim (the SDF's zero contour, marching squares, rebuilt when the shape changes, at height 0). */

#define VW_NRIP 16
enum { VW_R_STRIKE, VW_R_DROP, VW_R_TOUCH, VW_R_SLIP };
static const uint16_t VW_RIP_MS[4] = {350, 250, 300, 160};
typedef struct { float x, y; uint32_t t0; uint8_t type, vel, live, rsv; } vw_rip_t;
static vw_rip_t vw_rip[VW_NRIP];
static uint32_t vw_rip_w;

/* a mark at plate (u, v) 0..1 */
static void vw_ripple(float u, float v, int type, int vel)
{
    vw_rip_t *r;
    if (ui_st->reduced_motion && type != VW_R_TOUCH)   /* reduced motion: no ripples (a touch flash still shows) */
        return;
    r = &vw_rip[vw_rip_w++ % VW_NRIP];
    r->x = 28.0f + u * 184.0f, r->y = 36.0f + v * 184.0f;
    r->t0 = ui_now, r->type = (uint8_t)type, r->vel = (uint8_t)vel, r->live = 1;
}

static int vw_ripples_live(void)
{
    uint32_t i;
    int n = 0;
    for (i = 0; i < VW_NRIP; i++)
        if (vw_rip[i].live) {
            if (ui_now - vw_rip[i].t0 >= VW_RIP_MS[vw_rip[i].type])
                vw_rip[i].live = 0;
            else
                n++;
        }
    return n;
}

static inline float vw_px(float u) { return 28.0f + u * 184.0f; }
static inline float vw_py(float v) { return 36.0f + v * 184.0f; }

static void vw_marks(sf_t *sf)
{
    const cy_ui_state_t *st = ui_st;
    uint32_t i, k;
    int surf = st->keymode != 0, phos = st->theme == 1;
    uint16_t c_ret = phos ? UI_C_PHOS_HI : UI_C_AMBER, c_mic = phos ? UI_C_PHOS_LO : UI_C_VIOLET;
    uint16_t c_bow = phos ? UI_C_ICE : UI_C_MAGENTA;
    /* ripples, oldest first */
    for (i = 0; i < VW_NRIP; i++) {
        const vw_rip_t *r = &vw_rip[(vw_rip_w + i) % VW_NRIP];
        float t;
        if (!r->live || ui_now - r->t0 >= VW_RIP_MS[r->type])
            continue;
        t = (float)(ui_now - r->t0) / (float)VW_RIP_MS[r->type];
        switch (r->type) {
        case VW_R_STRIKE: {                  /* 0 -> 40 px over 350 ms, alpha 1 - t (louder: brighter) */
            float a = (1.0f - t) * (0.45f + 0.55f * (float)r->vel * (1.0f / 127.0f));
            sf_ring(sf, r->x, r->y, 40.0f * t, 1.0f, c_ret, sf_cov(a, 32u));
            break;
        }
        case VW_R_DROP:                      /* sand_hi at 40 %, up to 12 px over 250 ms */
            sf_ring(sf, r->x, r->y, 1.0f + 11.0f * t, 1.0f, ui_sand_hi565, sf_cov(0.4f * (1.0f - t), 32u));
            break;
        case VW_R_TOUCH:                     /* the touched point flashes white */
            sf_disc(sf, r->x, r->y, 3.5f - 1.5f * t, UI_C_WHITE, sf_cov(1.0f - t, 32u));
            break;
        default:                             /* a slip of the bow: a small magenta flick */
            sf_ring(sf, r->x, r->y, 2.0f + 5.0f * t, 1.0f, c_bow, sf_cov(0.7f * (1.0f - t), 32u));
            break;
        }
    }
    /* the strike reticle: an amber ring r 5 with 4 ticks of 3 px (TUNED: in SURFACE the keys are the strike points) */
    if (!surf) {
        float x = vw_px(ui_obj->pos_uv[0]), y = vw_py(ui_obj->pos_uv[1]);
        uint32_t ra = phos ? 28u : 30u, ta = 28u;
        sf_ring(sf, x, y, 5.0f, 1.0f, c_ret, ra);
        sf_line(sf, x - 9.5f, y, x - 6.5f, y, c_ret, ta);
        sf_line(sf, x + 6.5f, y, x + 9.5f, y, c_ret, ta);
        sf_line(sf, x, y - 9.5f, x, y - 6.5f, c_ret, ta);
        sf_line(sf, x, y + 6.5f, x, y + 9.5f, c_ret, ta);
    }
    /* the mics: violet hollow rings, swelling a pixel with the output level */
    for (k = 0; k < 2u; k++) {
        float pk = k ? ui_tel.peak_r : ui_tel.peak_l, lv = 0.0f;
        if (pk > 1e-6f)
            lv = fm_clampf((6.0206f * fm_log2f(pk) + 60.0f) * (1.0f / 60.0f), 0.0f, 1.0f);   /* 20 log10 */
        sf_ring(sf, vw_px(ui_obj->mic_uv[k][0]), vw_py(ui_obj->mic_uv[k][1]), 3.5f + lv, 1.0f, c_mic, 28u);
    }
    /* SURFACE: the 27 key points; held keys ink; pressure a magenta ring 3 + 6 P */
    if (surf)
        for (k = 0; k < CY_NKEYS; k++) {
            float x = vw_px(ui_obj->key_uv[k][0]), y = vw_py(ui_obj->key_uv[k][1]), P = (float)st->key_press[k] * (1.0f / 127.0f);
            int held = (int)(st->keys_held >> k & 1u);
            sf_disc(sf, x, y, held ? 1.6f : 1.0f, held ? UI_C_INK : UI_C_INK_DIM, 32u);
            if (held && P > 0.02f)
                sf_ring(sf, x, y, 3.0f + 6.0f * P, 1.0f, c_bow, 28u);
        }
    /* the bow: a magenta 14 x 2 bar along the plate's edge direction at the bow point, sparkles ~ bow force */
    if (st->exciter == CY_EXC_BOW && ui_tel.bow_force > 0.01f) {
        float bu = ui_obj->pos_uv[0], bv = ui_obj->pos_uv[1], gx = 0.0f, gy = 0.0f, tx, ty, il, x, y;
        uint32_t n, rng = vw_seed;
        if (surf) {                          /* the held key pressed hardest */
            int best = -1, bp = -1;
            for (k = 0; k < CY_NKEYS; k++)
                if ((st->keys_held >> k & 1u) && (int)st->key_press[k] > bp)
                    bp = st->key_press[k], best = (int)k;
            if (best >= 0)
                bu = ui_obj->key_uv[best][0], bv = ui_obj->key_uv[best][1];
        }
        x = vw_px(bu), y = vw_py(bv);
        (void)pl_sdf(bu * 184.0f, bv * 184.0f, &gx, &gy);
        il = gx * gx + gy * gy;
        if (il > 1e-6f) {
            il = fm_rsqrtf(il);
            tx = -gy * il, ty = gx * il;     /* the tangent: across the SDF's gradient */
        } else {
            tx = 1.0f, ty = 0.0f;
        }
        sf_capsule(sf, x - 6.0f * tx, y - 6.0f * ty, x + 6.0f * tx, y + 6.0f * ty, 0.75f, c_bow, 32u);
        n = (uint32_t)(fm_clampf(ui_tel.bow_force, 0.0f, 1.0f) * 12.0f + 0.5f);
        for (k = 0; k < n; k++) {
            float r = 3.0f + 8.0f * cy_randf(&rng), a = 6.2831853f * cy_randf(&rng);
            sf_px(sf, (int32_t)(x + r * fm_cosf(a)), (int32_t)(y + r * fm_sinf(a)), c_bow, 12u + (cy_rand(&rng) & 15u));
        }
    }
}

/* ---- MESH ---- */
#define VW_NSEG 640                       /* the rim contour's segments (a star's is ~260) */
static int16_t vw_mp[CY_GRID * CY_GRID][2] CY_POOL;   /* projected grid points, 1/16 px */
static uint16_t vw_seg[VW_NSEG][4] CY_POOL;           /* rim contour, plate u, v x 65535 */
static float vw_sd[CY_GRID * CY_GRID] CY_POOL;      /* the SDF (px) at each grid point: in or out, and where a line
                                                     * leaving the plate crosses the rim */
static uint16_t vw_nseg;
static uint32_t vw_seg_seq = 0xFFFFFFFFu;
static float vw_s = 150.0f;               /* the projection's scale: the shape's widest radius to 84 px */
static float vw_az = 0.5236f, vw_ca = 0.866f, vw_sa = 0.5f;   /* azimuth (30 degrees to start) */
static uint32_t vw_heard;                 /* ms the plate was last heard: the camera stops a second after */

/* the rim: marching squares over the 48 x 48 SDF, edges where it changes sign; the grid points' SDF; the scale
 * that fits the shape's widest radius to 84 px (the box's half width less a margin), 150..210 */
static void vw_outline(void)
{
    int32_t i, j;
    float r2 = 0.0f;
    if (ui_fc->seq == vw_seg_seq)
        return;
    vw_seg_seq = ui_fc->seq;
    vw_nseg = 0;
    for (j = 0; j < CY_GRID; j++)
        for (i = 0; i < CY_GRID; i++)
            vw_sd[j * CY_GRID + i] = pl_sdf(((float)i + 0.5f) * (184.0f / CY_GRID), ((float)j + 0.5f) * (184.0f / CY_GRID), 0, 0);
    for (j = 0; j < CY_SDF - 1; j++)
        for (i = 0; i < CY_SDF - 1; i++) {
            float s[4], pu[4], pv[4];
            int c, n = 0, e;
            s[0] = ui_fc->sdf[j][i], s[1] = ui_fc->sdf[j][i + 1], s[2] = ui_fc->sdf[j + 1][i + 1], s[3] = ui_fc->sdf[j + 1][i];
            c = (s[0] < 0.0f) | (s[1] < 0.0f) << 1 | (s[2] < 0.0f) << 2 | (s[3] < 0.0f) << 3;
            if (c == 0 || c == 15)
                continue;
            for (e = 0; e < 4; e++) {            /* the crossing on each edge of the cell, around it */
                int a = e, b = (e + 1) & 3;
                float t;
                static const int8_t CX[4] = {0, 1, 1, 0}, CY[4] = {0, 0, 1, 1};
                if ((s[a] < 0.0f) == (s[b] < 0.0f))
                    continue;
                t = s[a] / (s[a] - s[b]);
                pu[n] = ((float)(i + CX[a]) + t * (float)(CX[b] - CX[a]) + 0.5f) * (1.0f / CY_SDF);
                pv[n] = ((float)(j + CY[a]) + t * (float)(CY[b] - CY[a]) + 0.5f) * (1.0f / CY_SDF);
                n++;
            }
            for (e = 0; e < n; e++) {
                float du = pu[e] - 0.5f, dv = pv[e] - 0.5f;
                r2 = fm_maxf(r2, du * du + dv * dv);
            }
            for (e = 0; e + 1 < n && vw_nseg < VW_NSEG; e += 2) {   /* (a saddle has 4 crossings: two segments) */
                vw_seg[vw_nseg][0] = (uint16_t)(fm_clampf(pu[e], 0.0f, 1.0f) * 65535.0f);
                vw_seg[vw_nseg][1] = (uint16_t)(fm_clampf(pv[e], 0.0f, 1.0f) * 65535.0f);
                vw_seg[vw_nseg][2] = (uint16_t)(fm_clampf(pu[e + 1], 0.0f, 1.0f) * 65535.0f);
                vw_seg[vw_nseg][3] = (uint16_t)(fm_clampf(pv[e + 1], 0.0f, 1.0f) * 65535.0f);
                vw_nseg++;
            }
        }
    vw_s = r2 > 1e-4f ? fm_clampf(84.0f * fm_rsqrtf(r2), 150.0f, 210.0f) : 150.0f;
}

/* plate (u - .5, v - .5) and height h to the screen: X = 120 + s x', Y = 128 + s y' sin 35 - 28 h */
static inline void vw_project(float u, float v, float h, float *X, float *Y)
{
    float x = u - 0.5f, y = v - 0.5f, xr = x * vw_ca - y * vw_sa, yr = x * vw_sa + y * vw_ca;
    *X = 120.0f + vw_s * xr;
    *Y = 128.0f + vw_s * 0.573576f * yr - 28.0f * h;
}

static void vw_mesh_prepare(const float *hgrid)
{
    uint32_t i, j;
    int moving = !ui_st->reduced_motion && !ui_shot && ui_now - vw_heard < 1000u;
    if (sd_loud > 0.0f)
        vw_heard = ui_now;
    if (moving) {                             /* 8 degrees a second */
        vw_az += 0.13963f * ui_dt;
        if (vw_az > 6.2831853f)
            vw_az -= 6.2831853f;
    }
    vw_ca = fm_cosf(vw_az), vw_sa = fm_sinf(vw_az);
    vw_outline();                             /* (first: the scale) */
    for (j = 0; j < CY_GRID; j++)
        for (i = 0; i < CY_GRID; i++) {
            float X, Y;
            vw_project(((float)i + 0.5f) * (1.0f / CY_GRID), ((float)j + 0.5f) * (1.0f / CY_GRID),
                       fm_clampf(hgrid[j * CY_GRID + i], -1.0f, 1.0f), &X, &Y);
            vw_mp[j * CY_GRID + i][0] = (int16_t)(X * 16.0f);
            vw_mp[j * CY_GRID + i][1] = (int16_t)(Y * 16.0f);
        }
}

static int vw_mesh_moving(void) { return !ui_st->reduced_motion && !ui_shot && ui_now - vw_heard < 1000u; }

static inline int vw_in(uint32_t c) { return vw_sd[c] < 0.0f; }   /* the grid point is on the plate */

/* the grid segment a-b, both on the plate, or a on it and b off it: then it stops where the SDF crosses zero */
static void vw_seg_draw(sf_t *sf, uint32_t a, uint32_t b, const float *hgrid)
{
    float x0 = (float)vw_mp[a][0] * (1.0f / 16.0f), y0 = (float)vw_mp[a][1] * (1.0f / 16.0f);
    float x1 = (float)vw_mp[b][0] * (1.0f / 16.0f), y1 = (float)vw_mp[b][1] * (1.0f / 16.0f);
    float h = 0.5f * (hgrid[a] + hgrid[b]);
    int32_t q;
    if (!vw_in(b)) {                          /* the crossing, t = sdf_a / (sdf_a - sdf_b): the projection is linear */
        float t = vw_sd[a] / (vw_sd[a] - vw_sd[b]);
        x1 = x0 + (x1 - x0) * t, y1 = y0 + (y1 - y0) * t;
        h = hgrid[a] + (hgrid[b] - hgrid[a]) * 0.5f * t;
    }
    if (fm_maxf(y0, y1) + 1.0f < (float)sf->cy0 || fm_minf(y0, y1) - 1.0f > (float)sf->cy1)
        return;
    q = (int32_t)(fm_clampf(h, -1.0f, 1.0f) * 32.0f + 32.5f);
    sf_line(sf, x0, y0, x1, y1, vw_mcol[q], 32u);
}

/* one strip of the MESH view (the strip is already void) */
static void vw_mesh_strip(sf_t *sf, const float *hgrid)
{
    int32_t ii, jj, i, j, di = vw_sa >= 0.0f ? 1 : -1, dj = vw_ca >= 0.0f ? 1 : -1;
    uint32_t k;
    /* the rim at h = 0 first: the grid passes over it */
    for (k = 0; k < vw_nseg; k++) {
        float x0, y0, x1, y1;
        vw_project((float)vw_seg[k][0] * (1.0f / 65535.0f), (float)vw_seg[k][1] * (1.0f / 65535.0f), 0.0f, &x0, &y0);
        vw_project((float)vw_seg[k][2] * (1.0f / 65535.0f), (float)vw_seg[k][3] * (1.0f / 65535.0f), 0.0f, &x1, &y1);
        if (fm_maxf(y0, y1) + 1.0f < (float)sf->cy0 || fm_minf(y0, y1) - 1.0f > (float)sf->cy1)
            continue;
        sf_line(sf, x0, y0, x1, y1, ui_rim565, 26u);
    }
    /* back to front: y' = u sin + v cos grows with i when sin > 0 and with j when cos > 0. A segment with one end on
     * the plate is drawn from that end to the rim */
    for (jj = 0; jj < CY_GRID; jj++) {
        j = dj > 0 ? jj : CY_GRID - 1 - jj;
        for (ii = 0; ii < CY_GRID; ii++) {
            uint32_t c = (uint32_t)(j * CY_GRID), cb;
            int in;
            i = di > 0 ? ii : CY_GRID - 1 - ii;
            c += (uint32_t)i;
            in = vw_in(c);
            if (i - di >= 0 && i - di < CY_GRID) {
                cb = c - (uint32_t)di;
                if (in)
                    vw_seg_draw(sf, vw_in(cb) ? cb : c, vw_in(cb) ? c : cb, hgrid);
                else if (vw_in(cb))
                    vw_seg_draw(sf, cb, c, hgrid);
            }
            if (j - dj >= 0 && j - dj < CY_GRID) {
                cb = c - (uint32_t)(dj * CY_GRID);
                if (in)
                    vw_seg_draw(sf, vw_in(cb) ? cb : c, vw_in(cb) ? c : cb, hgrid);
                else if (vw_in(cb))
                    vw_seg_draw(sf, cb, c, hgrid);
            }
        }
    }
    /* the time-scale tag: "x1/440" = the newest note slowed to 0.6 Hz, right-aligned inside the box */
    if (ui_tel.newest_hz > 1.0f && sf_hits(sf, 160, 206, 212, 218)) {
        char t[16];
        float r = ui_tel.newest_hz * (1.0f / 0.6f);
        uint32_t v = (uint32_t)(r + 0.5f), m = 1;
        while (v >= 100u * m)                 /* two significant figures */
            m *= 10u;
        v = (v + m / 2u) / m * m;
        sf_strcpy(t, "x1/", sizeof t);
        sf_utoa(t + 3, v, 1);
        sf_text(sf, 208 - sf_text_w(&FONT_CY_TERM, t, 0), 206, &FONT_CY_TERM, t, UI_C_INK_DIM, 32u, 0);
    }
}
