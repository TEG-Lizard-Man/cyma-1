/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 UI (M5): the plate box, 184 x 184 at screen (28, 36). Part of the unity build: included by ui/ui.c after
 * gfx2.c and sand.c.
 *
 * No framebuffer: the box is drawn in 5 strips (40/40/40/40/24 rows) into two ping-pong 184 x 40 buffers. Each strip
 * drawn goes into the other buffer from the last one and out with lcd_blit, which on the device starts a DMA and
 * returns; drawing the next strip overlaps that transfer, and the next lcd_blit waits for it. 29 KB instead of 66 KB.
 * A frame may redraw only some strips (a mask): a tour chevron pulsing in the last strip redraws that strip alone.
 *
 * Each strip: the surface (a material colour, darker or brighter with the slow-motion displacement, with a key
 * light from the top-left), the rim (anti-aliased from the SDF, lit on the edges that face the light, a soft void
 * shadow outside, 2 px for the clamped DRUM), then the view's content (sand / field / mesh), the marks (views.c)
 * and the overlays (overlay.c).
 *
 * Speed (the device's frame budget is 6 ms, spec §6.9). The SAND surface is smooth, so it is computed at half
 * resolution (92 x 92) and each value lands on a 2 x 2 block. A band map, the SDF at each half-res cell rebuilt only
 * when the shape changes, sorts the cells three ways:
 *   - far inside: one colour per cell, its tint index (from the displacement, in fixed point) and its key light
 *     picking the cell's four dithered pixels from a palette (pl_pal: every tint x light level, dithered once with
 *     the Bayer matrix's top-left 2 x 2, built when the colours change); a few integer operations a cell;
 *   - far outside: void;
 *   - near the edge: each pixel from the rim cache, built with the band map: its distance to the edge and its light.
 * The splash's fade-in is folded into the palette and the rim weights (rebuilt each splash frame, a few hundred
 * entries), so it takes the same fast path. FIELD needs the nodal lines sharp, so it runs at full resolution, but with
 * the grid interpolated along each row once (3 multiplies a pixel). */

#define PL_W 184
#define PL_STRIP 40
#define PL_HR 92                          /* half resolution = CY_COVER */

static uint16_t pl_buf[2][PL_W * PL_STRIP] CY_POOL;
static int8_t pl_band[PL_HR * PL_HR] CY_POOL;   /* the SDF at each half-res cell centre, 1/4 px, clamped */
static float pl_D[CY_GRID * CY_GRID] CY_POOL;   /* the slow-motion displacement, normalised -1..1 */
static float pl_theta[CY_NMODES];         /* the slow-motion phases */
static float pl_dmax;                     /* |D|'s running max */
static uint32_t pl_band_seq = 0xFFFFFFFFu;
static float pl_rimw = 1.0f;              /* rim width: 2 px when clamped (DRUM) */
static float pl_sa = 1.0f;                /* the plate's opacity (the splash fades it in) */
static float pl_area = 1.0f;              /* the plate's area against the CIRCLE's: the grains */
static int16_t pl_tint[65][3];            /* SAND surface for k = -1..1 (k = 0.5 D) */
static int16_t pl_fcol[65][3];            /* FIELD: field_cold .. plate .. field_warm */
static ui_rgb_t pl_rimc;                  /* the rim (frozen: 40 % toward ice) */
static uint8_t pl_hx_i0[PL_HR], pl_x_i0[PL_W];
static float pl_hx_f[PL_HR], pl_x_f[PL_W];
static uint8_t pl_hx_q[PL_HR];            /* pl_hx_f in 1/256 (the tint index's fixed-point interpolation) */
static uint8_t pl_klc16[PL_HR];           /* the key light's column term at each half-res column centre, 1/16 level */
static uint8_t pl_sx_i0[PL_HR];           /* the SDF's column at each half-res cell centre */
static float pl_sx_f[PL_HR];
static uint8_t pl_c0[CY_SDF];             /* the cells whose SDF column is c: [pl_c0[c], pl_c0[c + 1]) */
static uint32_t pl_csig = 0xFFFFFFFFu;    /* the colour inputs last built for */
static uint8_t pl_shadows = 1;            /* airborne grains' shadows can be seen on this plate (>= 1.12:1) */
static uint32_t pl_fade_seen = 0xFFFFFFFFu;
/* the rim cache: each pixel of the half-res cells near the edge, in raster order, built when the shape (or the
 * colours) change: its distance to the edge (1/32 px, low byte) and how much it faces the light (0..16, high byte);
 * or, for a pixel the surface does not reach (the rim, the shadow outside: ~3/4 of them), the finished pixel itself,
 * flagged in pl_edk. A frame then draws the rim with a few table lookups and integer multiplies a pixel, most of it
 * with a load, instead of a bilinear SDF sample and a square root. The worst shape (STAR) has ~4,500 such pixels;
 * past the cap, the exact path */
#define PL_EDGE_MAX 6144
static uint16_t pl_ed[PL_EDGE_MAX] CY_POOL;
static uint8_t pl_edk[PL_EDGE_MAX / 8];
static uint8_t pl_cst;                     /* the cache holds finished pixels (built with the colours of that time) */
static uint16_t pl_erow[PL_HR];            /* the cache's first entry for each half-res row */
static uint8_t pl_ecut = PL_HR;            /* rows from here on are not cached (the cap) */
static uint16_t pl_ea_s[256], pl_ea_r[256], pl_ea_v[256];   /* by distance: surface, rim, shadowed void, x 256 */
static int16_t pl_rimlit0[17][3];          /* the rim's colour by how much it faces the light */
static int16_t pl_rimlit[17][3];           /* the same through the splash's fade */
static int8_t pl_far_in = -9;              /* band map thresholds (1/4 px): far inside at or below, far outside at */
#define PL_FAR_OUT 13                      /* or above (rim w + AA + a cell's half-diagonal; shadow 2.5 px + that) */
/* the far-inside palette. A sounding plate's tint index stays within 32 +- 16 (k = 0.5 D, |D| <= the loudness
 * <= 1), and the key light within whole levels 6..15 over the box ((x - 40)^2 + (y - 40)^2 <= 2 x 143^2) */
#define PL_Q0 16
#define PL_NQ 33
#define PL_KL0 6
#define PL_NKL 10
static uint16_t pl_pal[PL_NQ * PL_NKL][4] CY_POOL;   /* a cell's pixels: top-left, top-right, bottom-left, -right */
static int16_t pl_qb[64];                  /* a tint index's palette row, less PL_KL0: + the light level = the entry */
static uint8_t pl_qc[64];                  /* a tint index, clamped, from PL_Q0 */
static int16_t pl_tsa[PL_NQ][3];           /* the tint by index (from PL_Q0) through the splash's fade */
static int32_t pl_kls = 256;               /* the key light's weight through the fade, 1/256 */
static uint32_t pl_d_csig = 0xFFFFFFFFu;   /* what pl_derive last built for: colours, fade, rim width */
static int32_t pl_d_sa = -1;
static float pl_d_rimw = -1.0f;
#define PL_AREA_REF 5627.0f                /* the CIRCLE's cover in half-res cells (r ~0.46): GRAINS is its count */

/* the SDF (plate units x 128) at plate pixel position (x, y), in pixels, < 0 inside; its gradient (per SDF cell)
 * if asked */
static float pl_sdf(float x, float y, float *gx, float *gy)
{
    float sx = fm_clampf(x * ((float)CY_SDF / 184.0f) - 0.5f, 0.0f, (float)CY_SDF - 1.001f);
    float sy = fm_clampf(y * ((float)CY_SDF / 184.0f) - 0.5f, 0.0f, (float)CY_SDF - 1.001f);
    int32_t i = (int32_t)sx, j = (int32_t)sy;
    float fx, fy, s00, s10, s01, s11;
    if (i > CY_SDF - 2) i = CY_SDF - 2;
    if (j > CY_SDF - 2) j = CY_SDF - 2;
    fx = sx - (float)i, fy = sy - (float)j;
    s00 = ui_fc->sdf[j][i], s10 = ui_fc->sdf[j][i + 1], s01 = ui_fc->sdf[j + 1][i], s11 = ui_fc->sdf[j + 1][i + 1];
    if (gx) {
        *gx = (s10 - s00) * (1.0f - fy) + (s11 - s01) * fy;
        *gy = (s01 - s00) * (1.0f - fx) + (s11 - s10) * fx;
    }
    return ((s00 * (1.0f - fx) + s10 * fx) * (1.0f - fy) + (s01 * (1.0f - fx) + s11 * fx) * fy) * (184.0f / 128.0f);
}

/* an SDF row or column coordinate: the grid index and fraction at plate position p (px) */
static void pl_sdf_coord(float p, uint8_t *i0, float *f)
{
    float s = fm_clampf(p * ((float)CY_SDF / 184.0f) - 0.5f, 0.0f, (float)CY_SDF - 1.001f);
    int32_t i = (int32_t)s;
    if (i > CY_SDF - 2)
        i = CY_SDF - 2;
    *i0 = (uint8_t)i;
    *f = s - (float)i;
}

static void pl_tables(void)
{
    uint32_t i;
    for (i = 0; i < 64u; i++) {
        uint32_t q = i < PL_Q0 ? 0u : i >= PL_Q0 + PL_NQ ? PL_NQ - 1u : i - PL_Q0;
        pl_qc[i] = (uint8_t)q;
        pl_qb[i] = (int16_t)(q * PL_NKL - PL_KL0);
    }
    for (i = 0; i < PL_HR; i++) {
        float c = (float)(2u * i + 1u), g = fm_clampf(c * ((float)CY_GRID / 184.0f) - 0.5f, 0.0f, (float)CY_GRID - 1.001f);
        float kl = (c - 40.0f) * (c - 40.0f) * (15.3f / 67712.0f);   /* the key light's column term */
        pl_hx_i0[i] = (uint8_t)(g > (float)(CY_GRID - 2) ? CY_GRID - 2 : (int32_t)g);
        pl_hx_f[i] = g - (float)pl_hx_i0[i];
        pl_hx_q[i] = (uint8_t)fm_minf(pl_hx_f[i] * 256.0f + 0.5f, 255.0f);
        pl_klc16[i] = (uint8_t)(kl * 16.0f + 0.5f);
        pl_sdf_coord(c, &pl_sx_i0[i], &pl_sx_f[i]);
    }
    for (i = 0; i < CY_SDF; i++) {            /* (pl_sx_i0 never decreases) */
        uint32_t h = 0;
        while (h < PL_HR && pl_sx_i0[h] < i)
            h++;
        pl_c0[i] = (uint8_t)h;
    }
    for (i = 0; i < PL_W; i++) {
        float g = fm_clampf(((float)i + 0.5f) * ((float)CY_GRID / 184.0f) - 0.5f, 0.0f, (float)CY_GRID - 1.001f);
        pl_x_i0[i] = (uint8_t)(g > (float)(CY_GRID - 2) ? CY_GRID - 2 : (int32_t)g);
        pl_x_f[i] = g - (float)pl_x_i0[i];
    }
}

static inline ui_rgb_t pl_hex(uint32_t h)
{
    ui_rgb_t c;
    c.r = (uint8_t)(h >> 16), c.g = (uint8_t)(h >> 8), c.b = (uint8_t)h;
    return c;
}
static inline ui_rgb_t pl_mix(ui_rgb_t a, ui_rgb_t b, float t)
{
    ui_rgb_t c;
    c.r = (uint8_t)((float)a.r + ((float)b.r - (float)a.r) * t + 0.5f);
    c.g = (uint8_t)((float)a.g + ((float)b.g - (float)a.g) * t + 0.5f);
    c.b = (uint8_t)((float)a.b + ((float)b.b - (float)a.b) * t + 0.5f);
    return c;
}
static inline uint16_t pl_565(ui_rgb_t c) { return (uint16_t)((c.r >> 3) << 11 | (c.g >> 2) << 5 | (c.b >> 3)); }

/* WCAG relative luminance of an RGB888 colour */
static float pl_lum(float r, float g, float b)
{
    float c[3], y;
    uint32_t i;
    c[0] = r, c[1] = g, c[2] = b;
    for (i = 0; i < 3u; i++) {
        float v = fm_clampf(c[i], 0.0f, 255.0f) * (1.0f / 255.0f);
        c[i] = v <= 0.04045f ? v * (1.0f / 12.92f) : fm_powf((v + 0.055f) * (1.0f / 1.055f), 2.4f);
    }
    y = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
    return y;
}

/* the MATERIAL tint (spec §6.2): plate, plate_hi, sand_lo, sand_hi interpolated in RGB888 by MATERIAL (PHOSPHOR:
 * its own row); every LUT that depends on them. The sweep's fade only re-tones the sand */
static void pl_colors(void)
{
    const cy_ui_state_t *st = ui_st;
    float m = fm_clampf(cy_param_value(P_MATERIAL, st->norm[P_MATERIAL]), 0.0f, (float)(UI_NMAT - 1));
    int phos = st->theme == 1;
    uint32_t sig = (uint32_t)(m * 64.0f) | (uint32_t)phos << 12 | (uint32_t)(st->frozen != 0) << 13, i;
    uint32_t fade = (uint32_t)(sd_fade * 64.0f);
    int32_t a = (int32_t)m;
    float t = m - (float)a;
    ui_rgb_t c[4], hi, lo, m0, md, cold;
    if (sig == pl_csig) {
        if (fade != pl_fade_seen) {           /* the sweep: the sand fades, nothing else changes */
            pl_fade_seen = fade;
            hi = ui_sand_hi, lo = ui_sand_lo;
            sd_lut(&lo, &hi, st->frozen != 0, sd_fade);
            ui_plate_dirty = 1;
        }
        return;
    }
    pl_csig = sig;
    pl_fade_seen = fade;
    if (a >= UI_NMAT - 1)
        a = UI_NMAT - 2, t = 1.0f;
    for (i = 0; i < 4u; i++)
        c[i] = phos ? pl_hex(UI_MAT[UI_NMAT][i]) : pl_mix(pl_hex(UI_MAT[a][i]), pl_hex(UI_MAT[a + 1][i]), t);
    if (st->frozen && !phos) {                /* FREEZE: the whole plate a shade colder */
        c[0] = pl_mix(c[0], pl_hex(UI_FROST_PLATE), 0.10f);
        c[1] = pl_mix(c[1], pl_hex(UI_FROST_PLATE), 0.10f);
    }
    ui_plate = c[0], ui_plate_hi = c[1], ui_sand_lo = c[2], ui_sand_hi = c[3];
    {   /* a shadow (50 % toward void) against the plate at its mean key light: on the darkest materials (BRONZE,
         * AETHER, PHOSPHOR) it cannot be seen, and the shadow pass is skipped */
        float pr = (float)ui_plate.r + 11.0f, pg = (float)ui_plate.g + 11.0f, pb = (float)ui_plate.b + 11.0f;
        float lp = pl_lum(pr, pg, pb), ls = pl_lum(0.5f * (pr + UI_VOID8.r), 0.5f * (pg + UI_VOID8.g), 0.5f * (pb + UI_VOID8.b));
        pl_shadows = (lp + 0.05f) >= 1.12f * (ls + 0.05f);
    }
    pl_rimc = phos ? pl_hex(UI_PHOS_RIM) : UI_RIM8;
    if (st->frozen)
        pl_rimc = pl_mix(pl_rimc, UI_ICE8, 0.4f);
    ui_rim565 = pl_565(pl_rimc);
    for (i = 0; i <= 16u; i++) {
        ui_rgb_t q = pl_mix(pl_rimc, UI_INK8, 0.4f * (float)i * (1.0f / 16.0f));   /* lit: up to 40 % brighter */
        pl_rimlit0[i][0] = q.r, pl_rimlit0[i][1] = q.g, pl_rimlit0[i][2] = q.b;
    }
    ui_sand_hi565 = pl_565(ui_sand_hi);
    ui_sand_lo565 = pl_565(ui_sand_lo);
    /* MESH: the grid at rest a light steel blue (7:1 on void), troughs a sea blue (5.5:1), crests sand_hi; FIELD: the
     * cold lobe a blue as bright as the warm one is (4.3:1 against the plate), so both halves of a mode read */
    m0 = phos ? pl_rimc : UI_MESH08;
    md = phos ? ui_sand_lo : UI_MESHLO8;
    cold = phos ? UI_VOID8 : UI_FCOLD8;
    for (i = 0; i <= 64u; i++) {
        float k = (float)i * (1.0f / 32.0f) - 1.0f;
        ui_rgb_t s = k >= 0.0f ? pl_mix(ui_plate, ui_plate_hi, k) : pl_mix(ui_plate, UI_VOID8, -0.6f * k);
        ui_rgb_t f = k >= 0.0f ? pl_mix(ui_plate, phos ? ui_sand_lo : UI_WARM8, k) : pl_mix(ui_plate, cold, -k);
        ui_rgb_t mc = k >= 0.0f ? pl_mix(m0, ui_sand_hi, k) : pl_mix(m0, md, -k);
        pl_tint[i][0] = s.r, pl_tint[i][1] = s.g, pl_tint[i][2] = s.b;
        pl_fcol[i][0] = f.r, pl_fcol[i][1] = f.g, pl_fcol[i][2] = f.b;
        vw_mcol[i] = pl_565(mc);
    }
    hi = ui_sand_hi, lo = ui_sand_lo;
    sd_lut(&lo, &hi, st->frozen != 0, sd_fade);
    ui_plate_dirty = 1;
}

/* everything the surface's fast paths read that depends on the colours, the rim width and the splash's fade: the
 * far-inside palette, the faded tint, the rim's lit colours and the edge weights by distance. A few hundred entries,
 * so rebuilding it every splash frame costs far less than the per-pixel fade it replaces */
static int pl_derive(void)
{
    int32_t sa = (int32_t)(fm_clampf(pl_sa, 0.0f, 1.0f) * 256.0f + 0.5f), q, k, c;
    uint32_t i;
    const int32_t V[3] = {UI_VOID8.r, UI_VOID8.g, UI_VOID8.b};
    if (pl_csig == pl_d_csig && sa == pl_d_sa && pl_rimw == pl_d_rimw)
        return 0;
    pl_d_csig = pl_csig, pl_d_sa = sa, pl_d_rimw = pl_rimw;
    pl_kls = sa;
    for (q = 0; q < PL_NQ; q++)
        for (c = 0; c < 3; c++)
            pl_tsa[q][c] = (int16_t)(V[c] + (((int32_t)pl_tint[q + PL_Q0][c] - V[c]) * sa >> 8));
    for (q = 0; q < PL_NQ; q++)
        for (k = 0; k < PL_NKL; k++) {
            int32_t ka = (k + PL_KL0) * sa >> 8;
            uint32_t r = (uint32_t)(pl_tsa[q][0] + ka), g = (uint32_t)(pl_tsa[q][1] + ka), b = (uint32_t)(pl_tsa[q][2] + ka);
            uint16_t *p = pl_pal[q * PL_NKL + k];
            p[0] = ui_dith(r, g, b, UI_BAYER[0][0]);
            p[1] = ui_dith(r, g, b, UI_BAYER[0][1]);
            p[2] = ui_dith(r, g, b, UI_BAYER[1][0]);
            p[3] = ui_dith(r, g, b, UI_BAYER[1][1]);
        }
    for (i = 0; i <= 16u; i++)
        for (c = 0; c < 3; c++)
            pl_rimlit[i][c] = (int16_t)(V[c] + (((int32_t)pl_rimlit0[i][c] - V[c]) * sa >> 8));
    for (i = 0; i < 256u; i++) {              /* the weights by distance d (px, < 0 inside) */
        float d = ((float)i - 128.0f) * (1.0f / 32.0f), ain = fm_clampf(0.5f - d, 0.0f, 1.0f);
        float as = fm_clampf(0.5f - (d + pl_rimw), 0.0f, 1.0f), sh = d > 0.0f ? fm_clampf(1.0f - d * 0.4f, 0.0f, 1.0f) * 0.55f : 0.55f;
        pl_ea_s[i] = (uint16_t)(as * 256.0f + 0.5f);
        pl_ea_r[i] = (uint16_t)((ain - as) * 256.0f + 0.5f);
        /* the shadowed void: void x (1 - sh), faded in with the plate (void itself where the plate is not yet) */
        pl_ea_v[i] = (uint16_t)((1.0f - ain) * (1.0f - (float)sa * (1.0f / 256.0f) * sh) * 256.0f + 0.5f);
    }
    ui_plate_dirty = 1;
    return 1;
}

/* the band map, the rim cache, the plate's area and the sand's fade across the rim, when the shape (the field
 * cache) changes. The SDF is interpolated along each row once; the cells between two SDF columns that are both far
 * inside (or both far outside) are only marked so, a run at a time; a cell near the edge takes its gradient and
 * light once, and its four pixels' distances from the plane through its centre (the SDF is bilinear: over half a
 * pixel, a plane). A shape morph rebuilds this every frame it moves: ~0.2 M instructions on the device */
static uint8_t pl_band_last;               /* the last physics tick rebuilt the band map */
static int pl_band_update(int force)
{
    uint32_t hx, hy, c, n = 0, area = 0, i;
    const float k4 = 4.0f * (184.0f / 128.0f), ik4 = 1.0f / k4;   /* SDF units to 1/4 px, and back */
    float fi, fo = (float)PL_FAR_OUT;
    int cst;
    if (ui_fc->seq == pl_band_seq) {
        pl_band_last = 0;
        return 0;
    }
    if (pl_band_last && !force) {             /* a shape that keeps moving (the LFO melting it): every other tick; a
                                               * rim one tick behind cannot be seen, and the last shape is built */
        pl_band_last = 0;
        return 0;
    }
    pl_band_last = 1;
    pl_band_seq = ui_fc->seq;
    pl_rimw = ui_obj->clamped > 0.5f ? 2.0f : 1.0f;
    pl_far_in = (int8_t)(pl_rimw > 1.5f ? -13 : -9);
    fi = (float)pl_far_in;
    pl_ecut = PL_HR;
    pl_derive();                              /* (the weights and the rim's colours: the finished pixels need them) */
    cst = pl_kls == 256;                      /* (the splash's fade changes them every frame: none finished then) */
    pl_cst = (uint8_t)cst;
    for (hy = 0; hy < PL_HR; hy++) {
        float R[CY_SDF], G[CY_SDF], fy;
        uint8_t j;
        int8_t *bnd = &pl_band[hy * PL_HR];
        pl_erow[hy] = (uint16_t)n;
        if (!(hy & 1u))                       /* the area, for the grain count: every other cell is plenty */
            for (hx = 0; hx < PL_HR; hx += 2)
                area += ui_fc->cover[hy][hx];
        pl_sdf_coord((float)(2u * hy + 1u), &j, &fy);
        for (i = 0; i < CY_SDF; i++) {        /* the row through the cells' centres (1/4 px), and its y difference */
            float s0 = (float)ui_fc->sdf[j][i], s1 = (float)ui_fc->sdf[j + 1][i];
            R[i] = (s0 + (s1 - s0) * fy) * k4;
            G[i] = s1 - s0;
        }
        for (c = 0; c + 1 < CY_SDF; c++) {   /* the cells over SDF columns c .. c + 1, left to right */
            uint32_t h0 = pl_c0[c], h1 = pl_c0[c + 1];
            float r0 = R[c], r1 = R[c + 1];
            if (h0 >= h1)
                continue;
            if ((r0 <= fi && r1 <= fi) || (r0 >= fo && r1 >= fo)) {   /* far inside or outside, the whole run */
                int8_t v = r0 <= fi ? -127 : 127;
                for (hx = h0; hx < h1; hx++)
                    bnd[hx] = v;
                continue;
            }
            for (hx = h0; hx < h1; hx++) {
                float f = pl_sx_f[hx], d = r0 + (r1 - r0) * f, gx, gy, il, lit = 0.0f;
                int32_t dc, ex, ey, k;
                uint8_t l;
                int8_t b = (int8_t)fm_clampf(d + (d < 0.0f ? -0.5f : 0.5f), -127.0f, 127.0f);
                bnd[hx] = b;
                if (b <= pl_far_in || b >= PL_FAR_OUT || pl_ecut < PL_HR)
                    continue;
                if (n + 4u > PL_EDGE_MAX) {   /* full: this row and the rest take the exact path */
                    pl_ecut = (uint8_t)hy;
                    continue;
                }
                gx = (r1 - r0) * ik4, gy = G[c] + (G[c + 1] - G[c]) * f;   /* SDF units per SDF cell */
                il = gx * gx + gy * gy;
                if (il > 1e-6f)                /* the outward normal against the light from the top-left */
                    lit = fm_clampf(-(gx + gy) * fm_rsqrtf(il) * 0.70710678f, 0.0f, 1.0f);
                l = (uint8_t)(lit * 16.0f + 0.5f);
                /* the pixels' distances (1/32 px): the centre's, +- half a pixel along the gradient (an SDF cell
                 * is 184 / 48 px, an SDF unit 184 / 128 px: the distance moves g x 48 / 128 px a px) */
                dc = (int32_t)(d * 8.0f + (d < 0.0f ? -0.5f : 0.5f));
                ex = (int32_t)(gx * 6.0f + (gx < 0.0f ? -0.5f : 0.5f));
                ey = (int32_t)(gy * 6.0f + (gy < 0.0f ? -0.5f : 0.5f));
                for (k = 0; k < 4; k++, n++) { /* (x, y) (x+1, y) (x, y+1) (x+1, y+1) */
                    int32_t dp = dc + (k & 1 ? ex : -ex) + (k & 2 ? ey : -ey);
                    uint32_t di = (uint32_t)((dp < -127 ? -127 : dp > 127 ? 127 : dp) + 128);
                    if (cst && !pl_ea_s[di]) {   /* the surface does not reach it: the pixel, finished */
                        const int16_t *rc = pl_rimlit[l];
                        uint32_t wr = pl_ea_r[di], wv = pl_ea_v[di];
                        pl_ed[n] = ui_dith(((uint32_t)rc[0] * wr + UI_VOID8.r * wv) >> 8, ((uint32_t)rc[1] * wr + UI_VOID8.g * wv) >> 8,
                                           ((uint32_t)rc[2] * wr + UI_VOID8.b * wv) >> 8,
                                           UI_BAYER[(2u * hy + ((uint32_t)k >> 1)) & 3u][(2u * hx + ((uint32_t)k & 1u)) & 3u]);
                        pl_edk[n >> 3] = (uint8_t)(pl_edk[n >> 3] | 1u << (n & 7u));
                    } else {
                        pl_ed[n] = (uint16_t)((di - 128u) & 255u) | (uint16_t)(l << 8);
                        pl_edk[n >> 3] = (uint8_t)(pl_edk[n >> 3] & ~(1u << (n & 7u)));
                    }
                }
            }
        }
    }
    pl_area = (float)area * (4.0f / 255.0f) * (1.0f / PL_AREA_REF);
    for (i = 0; i < 256u; i++) {              /* the sand over the rim: down to 35 %, so the rim stays a clean line */
        float d = ((float)i - 128.0f) * 0.25f, as = fm_clampf(0.5f - (d + pl_rimw), 0.0f, 1.0f);
        sd_rimfade[i] = (uint16_t)((0.35f + 0.65f * as) * 256.0f + 0.5f);
    }
    return 1;
}

/* the slow-motion displacement (spec §6.6): D = sum a_k cos(theta_k) phi_k, a_k = sqrt(e_k) (signed by the strike,
 * so twin modes add coherently), theta_k turning at 0.6 Hz x ratio_k (about 1/400 of real speed), modes above
 * 12 Hz of slow motion faded out. Normalised by a running max with a 2 s release, times the loudness. Silent and
 * already flat: nothing to do but let the max decay */
static uint8_t pl_flat;                   /* D is all zero */
static void pl_disp(const float *strike)
{
    float a[CY_NMODES], mx = 0.0f, sc;
    uint32_t k, c, n = 0;
    uint8_t act[CY_NMODES];
    int run = !ui_st->reduced_motion;
    for (k = 0; k < CY_NMODES; k++) {
        float f = 0.6f * ui_obj->ratio[k], w = fm_clampf((12.0f - f) * 0.5f, 0.0f, 1.0f), e = ui_tel.mode_energy[k];
        if (run) {
            pl_theta[k] += 6.2831853f * f * ui_dt;
            if (pl_theta[k] > 6.2831853f)
                pl_theta[k] -= 6.2831853f * fm_floorf(pl_theta[k] * (1.0f / 6.2831853f));
        }
        a[k] = (e > 0.0f && w > 0.0f) ? (strike[k] < 0.0f ? -1.0f : 1.0f) * w * fm_sqrtf(e) * fm_cosf(pl_theta[k]) * (1.0f / 127.0f) : 0.0f;
        if (a[k] != 0.0f)
            act[n++] = (uint8_t)k;
    }
    if (!n || sd_loud <= 0.0f) {              /* nothing to show: D is zero (once), the max decays */
        if (!pl_flat)
            for (c = 0; c < CY_GRID * CY_GRID; c++)
                pl_D[c] = 0.0f;
        pl_flat = 1;
        pl_dmax *= fm_expf(-ui_dt * 0.5f);
        return;
    }
    pl_flat = 0;
    for (c = 0; c < CY_GRID * CY_GRID; c++)
        pl_D[c] = 0.0f;
    for (k = 0; k < n; k++) {
        const int8_t *f = &ui_fc->field[act[k]][0][0];
        float ak = a[act[k]];
        for (c = 0; c < CY_GRID * CY_GRID; c++)
            pl_D[c] += ak * (float)f[c];
    }
    for (c = 0; c < CY_GRID * CY_GRID; c++)
        mx = fm_maxf(mx, fm_fabsf(pl_D[c]));
    pl_dmax = fm_maxf(mx, pl_dmax * fm_expf(-ui_dt * 0.5f));
    sc = pl_dmax > 1e-12f ? sd_loud / pl_dmax : 0.0f;
    for (c = 0; c < CY_GRID * CY_GRID; c++)
        pl_D[c] *= sc;
}

/* one pixel near the rim, exactly (rows past the rim cache's cap; FIELD's uncached rows): the surface colour
 * (sr, sg, sb) inside, the lit rim, the void (with a soft shadow) outside, by the distance to the edge (px, plate
 * coordinates x, y). The same arithmetic as the cache's, fade included: (sr, sg, sb) is already faded */
static inline uint16_t pl_edge(int32_t x, int32_t y, int32_t sr, int32_t sg, int32_t sb, uint32_t t)
{
    float gx, gy, d = pl_sdf((float)x + 0.5f, (float)y + 0.5f, &gx, &gy), sa = (float)pl_kls * (1.0f / 256.0f);
    float ain = fm_clampf(0.5f - d, 0.0f, 1.0f), as = fm_clampf(0.5f - (d + pl_rimw), 0.0f, 1.0f), ar = ain - as;
    float sh = d > 0.0f ? fm_clampf(1.0f - d * 0.4f, 0.0f, 1.0f) * 0.55f : 0.55f, wv = (1.0f - ain) * (1.0f - sa * sh);
    float il = gx * gx + gy * gy, lit = 0.0f, rr, rg, rb, r, g, b;
    if (il > 1e-6f)                           /* the outward normal against the light from the top-left */
        lit = fm_clampf(-(gx + gy) * fm_rsqrtf(il) * 0.70710678f, 0.0f, 1.0f) * 0.4f;
    rr = (float)UI_VOID8.r + ((float)pl_rimc.r + ((float)UI_INK8.r - (float)pl_rimc.r) * lit - (float)UI_VOID8.r) * sa;
    rg = (float)UI_VOID8.g + ((float)pl_rimc.g + ((float)UI_INK8.g - (float)pl_rimc.g) * lit - (float)UI_VOID8.g) * sa;
    rb = (float)UI_VOID8.b + ((float)pl_rimc.b + ((float)UI_INK8.b - (float)pl_rimc.b) * lit - (float)UI_VOID8.b) * sa;
    r = (float)sr * as + rr * ar + (float)UI_VOID8.r * wv;
    g = (float)sg * as + rg * ar + (float)UI_VOID8.g * wv;
    b = (float)sb * as + rb * ar + (float)UI_VOID8.b * wv;
    return ui_dith((uint32_t)(r + 0.5f), (uint32_t)(g + 0.5f), (uint32_t)(b + 0.5f), t);
}

/* a cached rim pixel: finished, or the surface colour weighted in, the lit rim, the shadowed void (spec §6.6 step 3) */
static inline uint16_t pl_edge_c(uint32_t e, int32_t sr, int32_t sg, int32_t sb, uint32_t t)
{
    uint32_t v = pl_ed[e], di, ws, wr, wv;
    const int16_t *rc;
    if (pl_edk[e >> 3] >> (e & 7u) & 1u)
        return (uint16_t)v;
    di = (uint32_t)((int8_t)(v & 255u) + 128), ws = pl_ea_s[di], wr = pl_ea_r[di], wv = pl_ea_v[di];
    rc = pl_rimlit[v >> 8];
    uint32_t r = ((uint32_t)sr * ws + (uint32_t)rc[0] * wr + UI_VOID8.r * wv) >> 8;
    uint32_t g = ((uint32_t)sg * ws + (uint32_t)rc[1] * wr + UI_VOID8.g * wv) >> 8;
    uint32_t b = ((uint32_t)sb * ws + (uint32_t)rc[2] * wr + UI_VOID8.b * wv) >> 8;
    return ui_dith(r, g, b, t);
}

/* the SAND surface of plate rows [ys, ye) (even, a whole strip) into buf; vis scales the displacement tint */
static void pl_surface_sand(uint16_t *buf, int32_t ys, int32_t ye, float vis)
{
    const uint16_t vsw = swap16(UI_C_VOID);
    const float kq = 16.0f * 256.0f * vis;    /* D to the tint index, x 256: q = 32 + 0.5 D x 32 */
    const int32_t kls = pl_kls;
    int32_t hy;
    for (hy = ys >> 1; hy < ye >> 1; hy++) {
        float gy = fm_clampf((float)(2 * hy + 1) * ((float)CY_GRID / 184.0f) - 0.5f, 0.0f, (float)CY_GRID - 1.001f), fy;
        float yc = (float)(2 * hy + 1) - 40.0f;
        int32_t Rq[CY_GRID], j0 = (int32_t)gy, i, hx, cached = hy < pl_ecut;
        /* the key light from the top-left: lum + 0.06 (1 - ((x-40)^2 + (y-40)^2) / (2 184^2)), the row's part */
        int32_t klr = (int32_t)((15.3f - yc * yc * (15.3f / 67712.0f)) * 16.0f);
        uint32_t e = pl_erow[hy];
        uint16_t *r0 = &buf[(2 * hy - ys) * PL_W], *r1 = r0 + PL_W;
        const uint8_t *b0 = UI_BAYER[(2 * hy) & 3], *b1 = UI_BAYER[(2 * hy + 1) & 3];
        const int8_t *band = &pl_band[hy * PL_HR];
        if (j0 > CY_GRID - 2)
            j0 = CY_GRID - 2;
        fy = gy - (float)j0;
        for (i = 0; i < CY_GRID; i++)         /* the displacement along this row, as the tint index x 256 */
            Rq[i] = (int32_t)((pl_D[j0 * CY_GRID + i] * (1.0f - fy) + pl_D[(j0 + 1) * CY_GRID + i] * fy) * kq) + 8320;
        for (hx = 0; hx < PL_HR; hx++) {
            int32_t x = 2 * hx, bd = band[hx], q, kl;
            if (bd >= PL_FAR_OUT) {           /* far outside */
                r0[x] = r0[x + 1] = r1[x] = r1[x + 1] = vsw;
                continue;
            }
            i = pl_hx_i0[hx];
            q = ((Rq[i] + (((Rq[i + 1] - Rq[i]) * (int32_t)pl_hx_q[hx]) >> 8)) >> 8) & 63;   /* (16..48 by design) */
            kl = (klr - (int32_t)pl_klc16[hx]) >> 4;                                        /* (6..15 by design) */
            if (bd <= pl_far_in) {            /* far inside: the cell's four pixels from the palette */
                const uint16_t *p;
                int32_t e2 = pl_qb[q] + kl;
                if ((uint32_t)e2 >= PL_NQ * PL_NKL)
                    e2 = e2 < 0 ? 0 : PL_NQ * PL_NKL - 1;
                p = pl_pal[e2];
                r0[x] = p[0], r0[x + 1] = p[1], r1[x] = p[2], r1[x + 1] = p[3];
            } else {                          /* near the edge: the surface colour, then the rim per pixel */
                int32_t qc = pl_qc[q], ka = (kl * kls) >> 8, r = pl_tsa[qc][0] + ka, g = pl_tsa[qc][1] + ka, b = pl_tsa[qc][2] + ka;
                if (cached) {                 /* from the rim cache */
                    r0[x] = pl_edge_c(e, r, g, b, b0[x & 3]);
                    r0[x + 1] = pl_edge_c(e + 1u, r, g, b, b0[(x + 1) & 3]);
                    r1[x] = pl_edge_c(e + 2u, r, g, b, b1[x & 3]);
                    r1[x + 1] = pl_edge_c(e + 3u, r, g, b, b1[(x + 1) & 3]);
                    e += 4u;
                } else {                      /* past the cache's cap: exactly */
                    r0[x] = pl_edge(x, 2 * hy, r, g, b, b0[x & 3]);
                    r0[x + 1] = pl_edge(x + 1, 2 * hy, r, g, b, b0[(x + 1) & 3]);
                    r1[x] = pl_edge(x, 2 * hy + 1, r, g, b, b1[x & 3]);
                    r1[x + 1] = pl_edge(x + 1, 2 * hy + 1, r, g, b, b1[(x + 1) & 3]);
                }
            }
        }
    }
}

/* the FIELD view of plate rows [ys, ye): the displacement through the diverging map, nodal lines in ink at 60 %
 * where it changes sign (anti-aliased: alpha by the distance to the zero, |D| / |grad D|), no sand. vis is the
 * loudness: a quiet note still shows its mode shape at 40 % of the map (D is normalised to |D| <= vis, so the gain
 * (0.4 + 0.6 vis) / vis), the nodal lines' slope gate is relative to the level (so they hold as a note fades), and
 * the lines fade with the sound; a silent (flat) field draws none */
static void pl_surface_field(uint16_t *buf, int32_t ys, int32_t ye, float vis)
{
    const uint16_t vsw = swap16(UI_C_VOID);
    const float g2p = (float)CY_GRID / 184.0f, gain = (0.4f + 0.6f * vis) / fm_maxf(vis, 1e-3f);
    const float gate = 2e-3f * fm_maxf(vis, 0.02f);
    float lines = fm_clampf(vis * 4.0f, 0.0f, 1.0f) * 0.6f;
    int32_t y;
    for (y = ys; y < ye; y++) {
        float R[CY_GRID], Ry[CY_GRID], gy = fm_clampf(((float)y + 0.5f) * g2p - 0.5f, 0.0f, (float)CY_GRID - 1.001f), fy;
        int32_t j0 = (int32_t)gy, i, x, cached = (y >> 1) < pl_ecut;
        uint32_t e = pl_erow[y >> 1] + 2u * (uint32_t)(y & 1);   /* the cache: 4 entries a cell, 2 a cell row */
        uint16_t *out = &buf[(y - ys) * PL_W];
        const uint8_t *bay = UI_BAYER[y & 3];
        const int8_t *band = &pl_band[(y >> 1) * PL_HR];
        if (j0 > CY_GRID - 2)
            j0 = CY_GRID - 2;
        fy = gy - (float)j0;
        for (i = 0; i < CY_GRID; i++) {
            float a = pl_D[j0 * CY_GRID + i], b = pl_D[(j0 + 1) * CY_GRID + i];
            R[i] = a + (b - a) * fy;
            Ry[i] = (b - a) * g2p;
        }
        for (x = 0; x < PL_W; x++) {
            int32_t bd = band[x >> 1], q, r, g, b;
            float f, D, dx, dy, dist, al;
            if (bd >= PL_FAR_OUT) {
                out[x] = vsw;
                continue;
            }
            i = pl_x_i0[x], f = pl_x_f[x];
            D = R[i] + (R[i + 1] - R[i]) * f;
            dx = (R[i + 1] - R[i]) * g2p;
            dy = Ry[i] + (Ry[i + 1] - Ry[i]) * f;
            q = (int32_t)(fm_clampf(D * gain, -1.0f, 1.0f) * 32.0f + 32.5f);
            r = pl_fcol[q][0], g = pl_fcol[q][1], b = pl_fcol[q][2];
            dist = fm_fabsf(dx) + fm_fabsf(dy);   /* the slope, per px */
            al = dist > gate ? (1.0f - fm_fabsf(D) / dist) * lines : 0.0f;   /* a flat field has no lines */
            if (al > 0.0f) {
                r += (int32_t)(((float)UI_INK8.r - (float)r) * al);
                g += (int32_t)(((float)UI_INK8.g - (float)g) * al);
                b += (int32_t)(((float)UI_INK8.b - (float)b) * al);
            }
            if (bd <= pl_far_in) {
                out[x] = ui_dith((uint32_t)r, (uint32_t)g, (uint32_t)b, bay[x & 3]);
            } else if (cached) {
                out[x] = pl_edge_c(e + (uint32_t)(x & 1), r, g, b, bay[x & 3]);
                if (x & 1)
                    e += 4u;                  /* (both of the cell's pixels on this row done: the next cell) */
            } else {
                out[x] = pl_edge(x, y, r, g, b, bay[x & 3]);
            }
        }
    }
}

/* the box, strip by strip: the strips in mask (bit s = strip s, rows 40 s ..) */
#define PL_ALL 0x1Fu
static void pl_render(int splash, uint32_t mask)
{
    int32_t s;
    uint32_t nb = 0;
    const cy_ui_state_t *st = ui_st;
    int view = splash ? CY_VIEW_SAND : st->view;
    float vis = st->reduced_motion ? 0.0f : 1.0f;
    if (pl_derive() && (pl_kls == 256 || pl_cst)) {   /* new colours: the rim cache's finished pixels with them */
        pl_band_seq = 0xFFFFFFFFu;
        pl_band_update(1);
    }
    if (view == CY_VIEW_MESH)
        vw_mesh_prepare(pl_D);
    vw_seed = sd_hash(ui_now * 2654435761u + 7u);
    lcd_sync();                               /* the last frame's last strip may still be on its way out */
    for (s = 0; s < 5; s++) {
        int32_t ys = s * PL_STRIP, ye = ys + PL_STRIP > PL_W ? PL_W : ys + PL_STRIP, rows = ye - ys;
        uint16_t *buf;
        sf_t sf;
        if (!(mask >> s & 1u))
            continue;
        buf = pl_buf[nb++ & 1u];              /* (the other buffer from the last strip's: that one may be in flight) */
        sf_init(&sf, buf, 28, 36 + ys, PL_W, rows);
        if (view == CY_VIEW_FIELD) {
            pl_surface_field(buf, ys, ye, sd_loud);
        } else if (view == CY_VIEW_MESH) {
            sf_rect(&sf, 28, 36 + ys, PL_W, rows, UI_C_VOID, 32u);
            vw_mesh_strip(&sf, pl_D);
        } else {
            pl_surface_sand(buf, ys, ye, vis);
            if (sd_nair && pl_shadows)        /* (only grains in the air cast one, and only where it shows) */
                sd_shadows(&sf, ys, ye);
            /* (the splash's credit and logo stand on no plate: until the plate is whole, no sand is over a rim to
             * keep clean, so none is faded there, whatever the shape) */
            sd_strip(buf, ys, ye, pl_band, splash ? 127 : pl_far_in);
        }
        if (!splash && view != CY_VIEW_MESH)
            vw_marks(&sf);
        ui_ov_plate(&sf, splash);
        lcd_blit(28, (uint32_t)(36 + ys), PL_W, (uint32_t)rows, buf);
#ifdef UI_POLL
        UI_POLL();                            /* the app's note path between strips: a key waits a strip, not a frame */
#endif
    }
}
