/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 UI (M5): the sand. Part of the unity build: included by ui/ui.c (after gfx2.c), itself included by app.c.
 *
 * Physics (spec §6.6, reference tools/proto/sand2.py). What throws sand off a plate is its vertical ACCELERATION:
 * a mode's amplitude times its frequency squared, so its power goes as e rho^4 (e its energy, rho its ratio). Each
 * frame the engine's mode energies become
 *     E(p) = sum_k w_k psi_k(p)^2,
 * over the strongest K = 4 degenerate groups (modes sharing a frequency), psi_k being a group's STRUCK combination of
 * its fields (strike-weighted, so a circle's twin cos/sin modes struck at one point make one diameter through that
 * point, not a blur), normalised to max |psi| = 1. E is normalised by a running max (2 s release) and scaled by the
 * loudness, so a fading note calms the sand while the figure holds. A grain where sqrt(E) exceeds 0.05 is airborne:
 * it hops at random in proportion to how far over it is and drifts down the slope of E, so it leaves the antinodes
 * and collects on the nodal lines, where E is zero. Unlifted grains do not hop; settled sand stays when the sound
 * stops, as on a real plate.
 *
 * Where this departs from summing E on the 24 x 24 grid, and why (each was measured against per-mille-on-nodal-line
 * counts over 9 shapes and by eye; tests/ui_stub reproduces it):
 *   - each grain interpolates psi and squares it: interpolating psi^2 itself fills in the zeros between grid points,
 *     so nodal lines come out as broken squares;
 *   - group weights are (e rho^4)^2, normalised (sd_sharp): a struck plate rings in many modes at once and their
 *     plain sum has no zeros anywhere, so no grain ever rests; squared, the strongest group draws the figure and the
 *     others perturb it, and as the mix changes (high modes die first, CARVE slides) the figure changes with it;
 *   - rho^4 rolls off above ratio 12: a tracked mode can sit at ratio 37 (DRUM's fundamental on the free CIRCLE)
 *     where rho^4 = 2e6 lets a -60 dB whisper outweigh the note, and the grid cannot draw it anyway;
 *   - while the plate sounds, an unlifted grain in the outer half of the quiet band creeps toward E = 0 (a gentle
 *     Newton step), so the sand forms ONE line on the node rather than two at the edges of the band.
 *
 * Storage. A grain is 6 bytes: x, y in 1/256 px of the 184 x 184 plate box, a flag byte and a timer byte (the
 * sprinkle's fall). Rendering is per 40-row strip: the grains are count-sorted into 8-row bands, splatted into a u8
 * density strip with bilinear weights (2 halo rows each side, so the glow is continuous across strip edges), then
 * a 3 x 3 box glow and a 64-entry tone map composite them over the plate surface already in the strip buffer. Each
 * density row keeps a mask of the 8-px tiles its splats touched, and the glow and the blend visit only those tiles
 * and their neighbours: sand on nodal lines covers a small part of each row, rings and crosses at both ends. */

#define SD_MAX 2000                      /* the GRAINS ceiling (P_GRAINS 600..2000) */
#define SD_PW 184                        /* the plate box, px */
#define SD_HALO 2
#define SD_ROWS (40 + 2 * SD_HALO)       /* a strip's density rows */
#define SD_NB 24                         /* 8-row bands (184 / 8 = 23, one spare for grains that fall in from above) */
#define SD_G 24                          /* CY_GRID */
#define SD_G2PX (184.0f / 24.0f)         /* grid cell, px */
#define SD_AIR 1u                        /* lifted this frame: drawn offset (-1, -1) over a shadow */
#define SD_FALL 2u                       /* sprinkled: falling in, t counts down in 4 ms steps */
#define SD_FALL_T 100u                   /* 400 ms */
#define SD_FALL_PX 12.0f

typedef struct { uint16_t x, y; uint8_t f, t; } sd_grain_t;

static sd_grain_t sd_g[SD_MAX] CY_POOL;
static uint16_t sd_idx[SD_MAX] CY_POOL;           /* grain numbers, by band */
static union { uint8_t b[SD_PW * SD_ROWS]; uint32_t w[SD_PW * SD_ROWS / 4]; } sd_du CY_POOL;
#define sd_dens sd_du.b                           /* one strip's density, 64 = one whole grain on a pixel (w: 4 at a time) */
#define SD_K 4                           /* the strongest groups the sand feels */
static float sd_sharp = 2.0f;            /* contrast: a group's weight goes as (e rho^4)^sd_sharp (1 = the plain sum) */
static float sd_psi[SD_K][SD_G * SD_G] CY_POOL;   /* their struck shapes, normalised to max |psi| = 1 */
static float sd_w[SD_K];                 /* and their weights in E = sum w psi^2 (normalised: max E = loudness) */
static uint8_t sd_nk;
static uint16_t sd_band[SD_NB + 1];
static uint32_t sd_rng = 0x51A2D00Du;   /* the UI's particle RNG: apart from the engine's (spec §3.12) */
static uint16_t sd_n;                    /* grains in play */
static uint16_t sd_moved;                /* grains that moved this frame (0: settled) */
static uint16_t sd_nfall;                /* grains still falling in (a sprinkle) */
static uint16_t sd_nair;                 /* grains drawn in the air (lifted or falling): they cast shadows */
static uint16_t sd_rimfade[256];         /* the sand's alpha over the rim by the cell's SDF (1/4 px) + 128, x 256
                                          * (plate.c builds it with the band map) */
static uint32_t sd_jolt_t;               /* when the shape's last change jolted the sand (ms) */
static uint8_t sd_rebin = 1;             /* grains were placed outside sd_step: the bands need sorting again */
static float sd_emax, sd_loud;           /* E's running max; the loudness 0..1 */
static float sd_fade = 1.0f;             /* the sweep's fade */
static uint32_t sd_sweep_at;             /* the sweep started (ms); 0 none */
static uint8_t sd_sweeping;
/* degenerate groups, rebuilt when the object changes: the modes of group i are sd_gk[sd_gs[i] .. sd_gs[i+1]) */
static uint8_t sd_gk[CY_NMODES], sd_gs[CY_NMODES + 1], sd_gn;
static float sd_gr[CY_NMODES];           /* a group's mean ratio */
static uint32_t sd_gseq = 0xFFFFFFFFu;

static inline float sd_rand01(void) { return cy_randf(&sd_rng); }
static inline float sd_tri(void) { return sd_rand01() + sd_rand01() - 1.0f; }   /* triangular on (-1, 1) */

/* cover at a grain position (1/256 px): the half-resolution 92 x 92 map, 255 inside */
static inline uint32_t sd_cover(uint32_t x8, uint32_t y8)
{
    uint32_t cx = x8 >> 9, cy = y8 >> 9;
    return (cx < CY_COVER && cy < CY_COVER) ? ui_fc->cover[cy][cx] : 0u;
}

/* a random position inside the plate (cover >= 128); the plate's centre if 32 tries all miss (a sliver of a shape) */
static void sd_place(sd_grain_t *g)
{
    int k;
    for (k = 0; k < 32; k++) {
        uint32_t x = (uint32_t)(sd_rand01() * (float)(SD_PW * 256 - 1)), y = (uint32_t)(sd_rand01() * (float)(SD_PW * 256 - 1));
        if (sd_cover(x, y) >= 128u) {
            g->x = (uint16_t)x, g->y = (uint16_t)y;
            return;
        }
    }
    g->x = g->y = (uint16_t)(SD_PW * 128);
}

static void sd_init(void)
{
    uint32_t i;
    sd_rng = 0x51A2D00Du;
    for (i = 0; i < SD_MAX; i++) {
        sd_g[i].f = sd_g[i].t = 0;
        sd_place(&sd_g[i]);
    }
    for (i = 0; i < SD_PW * SD_ROWS; i++)
        sd_dens[i] = 0;
    for (i = 0; i < SD_K * SD_G * SD_G; i++)
        sd_psi[0][i] = 0.0f;
    sd_nk = 0;
    sd_nfall = 0;
    sd_emax = sd_loud = 0.0f;
    sd_fade = 1.0f;
    sd_sweeping = 0;
    sd_gseq = 0xFFFFFFFFu;
    sd_rebin = 1;
}

/* the grains that change position because the shape did: those now outside re-spawn inside; every grain gets one
 * jolt (a third of the full hop), so the sand visibly flows into the new geometry. A shape that keeps moving (SELECT
 * turning, the LFO melting it) jolts the sand at most every 100 ms: the figure stays legible while it flows */
static void sd_shape_changed(void)
{
    uint32_t i;
    sd_rebin = 1;
    int jolt = ui_now - sd_jolt_t >= 100u || ui_shot;
    if (jolt)
        sd_jolt_t = ui_now;
    for (i = 0; i < sd_n; i++) {             /* (the grains out of play are placed afresh when they come back) */
        sd_grain_t *g = &sd_g[i];
        int32_t x, y;
        if (sd_cover(g->x, g->y) < 128u) {
            sd_place(g);
            continue;
        }
        if (!jolt)
            continue;
        x = (int32_t)g->x + (int32_t)(sd_tri() * (5.5f / 3.0f) * 256.0f);
        y = (int32_t)g->y + (int32_t)(sd_tri() * (5.5f / 3.0f) * 256.0f);
        if (x >= 0 && y >= 0 && x < SD_PW * 256 && y < SD_PW * 256 && sd_cover((uint32_t)x, (uint32_t)y) >= 128u)
            g->x = (uint16_t)x, g->y = (uint16_t)y;
    }
}

/* SPRINKLE: a quarter of the grains re-spawn at random inside, falling in from 12 px up over 400 ms */
static void sd_sprinkle(uint32_t frac4)
{
    uint32_t i, ph = cy_rand(&sd_rng) & 3u;
    sd_rebin = 1;
    for (i = 0; i < sd_n; i++)
        if (frac4 >= 4u || ((i + ph) & 3u) == 0u) {
            sd_place(&sd_g[i]);
            sd_g[i].f = SD_FALL;
            sd_g[i].t = (uint8_t)(SD_FALL_T - (cy_rand(&sd_rng) & 31u));   /* (staggered: a shower, not a slab) */
            sd_nfall++;
        }
}

/* SWEEP: everything fades over 600 ms, then the whole plate is sprinkled afresh */
static void sd_sweep(void)
{
    sd_sweep_at = ui_now ? ui_now : 1u;
    sd_sweeping = 1;
}

static void sd_groups(void)
{
    uint32_t k, n = 0, i;
    uint8_t seen[CY_NMODES];
    if (ui_obj->seq == sd_gseq)
        return;
    sd_gseq = ui_obj->seq;
    for (k = 0; k < CY_NMODES; k++)
        seen[k] = 0;
    sd_gn = 0;
    for (k = 0; k < CY_NMODES; k++) {        /* each id once, members in mode order */
        float r = 0.0f;
        uint32_t m = 0;
        if (seen[k])
            continue;
        sd_gs[sd_gn] = (uint8_t)n;
        for (i = k; i < CY_NMODES; i++)
            if (!seen[i] && ui_obj->degen[i] == ui_obj->degen[k]) {
                seen[i] = 1;
                sd_gk[n++] = (uint8_t)i;
                r += ui_obj->ratio[i];
                m++;
            }
        sd_gr[sd_gn++] = r / (float)m;
    }
    sd_gs[sd_gn] = (uint8_t)n;
}

/* E = sum w_k psi_k^2 from the telemetry's mode energies and the strike vector (see the top of this file): the K
 * strongest groups' struck shapes into sd_psi, their weights (scaled so that max E = the loudness) into sd_w */
static void sd_field(const float *strike)
{
    float tot = 0.0f, mx = 0.0f, wg[CY_NMODES], wmax = 0.0f, nrm;
    uint8_t top[SD_K];
    uint32_t g, c, k, n = 0;
    sd_groups();
    for (c = 0; c < CY_NMODES; c++)
        tot += ui_tel.mode_energy[c];
    sd_loud = tot > 1e-12f ? fm_clampf((3.0103f * fm_log2f(tot) + 70.0f) * (1.0f / 50.0f), 0.0f, 1.0f) : 0.0f;
    if (sd_loud <= 0.0f) {                   /* silent: no field, the max decays (the idle frame's whole cost) */
        sd_nk = 0;
        sd_emax *= fm_expf(-ui_dt * 0.5f);
        return;
    }
    for (g = 0; g < sd_gn; g++) {            /* each group's acceleration power, e_g rho_g^4, rolled off above 12 */
        float eg = 0.0f, r2 = sd_gr[g] * sd_gr[g], q = r2 * (1.0f / 144.0f);
        for (k = sd_gs[g]; k < sd_gs[g + 1]; k++)
            eg += ui_tel.mode_energy[sd_gk[k]];
        wg[g] = eg * r2 * r2 / (1.0f + q * q);
        wmax = fm_maxf(wmax, wg[g]);
    }
    if (sd_loud > 0.0f && wmax > 1e-20f)
        for (k = 0; k < SD_K; k++) {         /* the K strongest */
            int best = -1;
            for (g = 0; g < sd_gn; g++) {
                uint32_t j, used = 0;
                for (j = 0; j < n; j++)
                    used |= top[j] == g;
                if (!used && wg[g] > wmax * 1e-4f && (best < 0 || wg[g] > wg[best]))
                    best = (int)g;
            }
            if (best < 0)
                break;
            top[n++] = (uint8_t)best;
        }
    sd_nk = 0;
    for (k = 0; k < n; k++) {
        uint32_t a = sd_gs[top[k]], b = sd_gs[top[k] + 1], j;
        float s2 = 0.0f, m = 0.0f, *psi = sd_psi[sd_nk], im;
        for (j = a; j < b; j++)
            s2 += strike[sd_gk[j]] * strike[sd_gk[j]];
        for (c = 0; c < SD_G * SD_G; c++)
            psi[c] = 0.0f;
        for (j = a; j < b; j++) {            /* the struck combination; with no strike weight (a node), the
                                              * energy-weighted one, so a group never vanishes from the picture */
            uint32_t mk = sd_gk[j];
            const int8_t *f = &ui_fc->field[mk][0][0];
            float wk = s2 > 1e-4f ? strike[mk] : fm_sqrtf(ui_tel.mode_energy[mk]);
            if (wk == 0.0f)
                continue;
            for (c = 0; c < SD_G * SD_G; c++)
                psi[c] += wk * (float)f[c];
        }
        for (c = 0; c < SD_G * SD_G; c++)
            m = fm_maxf(m, fm_fabsf(psi[c]));
        if (m <= 0.0f)
            continue;
        im = 1.0f / m;
        for (c = 0; c < SD_G * SD_G; c++)
            psi[c] *= im;
        sd_w[sd_nk++] = wg[top[k]] / wmax;   /* 0..1, then the contrast */
    }
    for (k = 0; k < sd_nk; k++)
        sd_w[k] = sd_w[k] > 0.0f ? fm_powf(sd_w[k], sd_sharp) : 0.0f;
    for (c = 0; c < SD_G * SD_G; c++) {      /* max E on the grid */
        float e = 0.0f;
        for (k = 0; k < sd_nk; k++)
            e += sd_w[k] * sd_psi[k][c] * sd_psi[k][c];
        mx = fm_maxf(mx, e);
    }
    sd_emax = fm_maxf(mx, sd_emax * fm_expf(-ui_dt * 0.5f));   /* 2 s release */
    nrm = sd_emax > 1e-20f ? sd_loud / sd_emax : 0.0f;
    for (k = 0; k < sd_nk; k++)
        sd_w[k] *= nrm;
}

/* one step of every grain, dt_n = dt x 60 (frame-rate independent) */
static void sd_step(void)
{
    uint32_t i, moved = 0, nfall = 0;
    const float s = 1.0f / 256.0f, k = 1.0f / SD_G2PX, hop = 5.5f * ui_dtn, slide = 1.1f * ui_dtn;
    uint32_t fall = (uint32_t)(ui_dt * 250.0f + 0.5f);   /* 4 ms steps */
    if (!fall)
        fall = 1;
    if (!sd_nk && !sd_nfall) {               /* silent and nothing falling: nothing moves */
        sd_moved = 0;
        return;
    }
    for (i = 0; i < sd_n; i++) {
        sd_grain_t *g = &sd_g[i];
        float px, py, gx, gy, fx, fy, e = 0.0f, eu = 0.0f, ev = 0.0f, a, lift, nx, ny;
        int32_t i0, j0, c, xi, yi;
        uint32_t q;
        if (g->f & SD_FALL) {
            g->t = (uint8_t)(g->t > fall ? g->t - fall : 0u);
            if (!g->t)
                g->f &= (uint8_t)~SD_FALL;
            moved++, nfall++;
            continue;
        }
        if (!sd_nk) {
            g->f &= (uint8_t)~SD_AIR;
            continue;
        }
        px = (float)g->x * s, py = (float)g->y * s;
        gx = fm_clampf(px * k - 0.5f, 0.0f, (float)SD_G - 1.001f);
        gy = fm_clampf(py * k - 0.5f, 0.0f, (float)SD_G - 1.001f);
        i0 = (int32_t)gx, j0 = (int32_t)gy;
        if (i0 > SD_G - 2) i0 = SD_G - 2;
        if (j0 > SD_G - 2) j0 = SD_G - 2;
        fx = gx - (float)i0, fy = gy - (float)j0;
        c = j0 * SD_G + i0;
        for (q = 0; q < sd_nk; q++) {        /* psi bilinear, and its derivatives (per grid cell) */
            const float *P = sd_psi[q];
            float p00 = P[c], p10 = P[c + 1], p01 = P[c + SD_G], p11 = P[c + SD_G + 1];
            float top = p00 + (p10 - p00) * fx, bot = p01 + (p11 - p01) * fx, v = top + (bot - top) * fy;
            float du = (p10 - p00) + ((p11 - p01) - (p10 - p00)) * fy, dv = bot - top, w2 = 2.0f * sd_w[q] * v;
            e += sd_w[q] * v * v;
            eu += w2 * du;
            ev += w2 * dv;
        }
        a = e > 0.0f ? fm_sqrtf(e) : 0.0f;
        lift = a - 0.05f;
        if (lift <= 0.0f) {
            /* below the throw: no hop. While the plate sounds, a grain in the outer half of the quiet band still
             * creeps down the slope (a Newton step toward E = 0, an eighth of the way, at most 0.2 px), so the sand
             * gathers on the nodal line instead of piling up at both edges of the band, and the line keeps the
             * grainy width of the band's core; silence stops it dead */
            float gu = eu * (1.0f / SD_G2PX), gv = ev * (1.0f / SD_G2PX), g2 = gu * gu + gv * gv, st;
            g->f &= (uint8_t)~SD_AIR;
            if (a > 0.025f && g2 > 1e-12f) {
                st = fm_minf(0.25f * ui_dtn * e / g2, 0.2f * fm_rsqrtf(g2));
                nx = px - st * gu, ny = py - st * gv;
                xi = (int32_t)(nx * 256.0f), yi = (int32_t)(ny * 256.0f);
                if ((xi != (int32_t)g->x || yi != (int32_t)g->y) && xi >= 0 && yi >= 0 && xi < SD_PW * 256 && yi < SD_PW * 256
                    && sd_cover((uint32_t)xi, (uint32_t)yi) >= 128u)
                    g->x = (uint16_t)xi, g->y = (uint16_t)yi;
            }
            continue;
        }
        g->f = (uint8_t)(lift > 0.15f ? g->f | SD_AIR : g->f & ~SD_AIR);
        /* the slope as the prototype measured it: E over two cells (2 x the derivative per cell) */
        nx = px + hop * lift * sd_tri() - slide * 2.0f * eu;
        ny = py + hop * lift * sd_tri() - slide * 2.0f * ev;
        xi = (int32_t)(nx * 256.0f), yi = (int32_t)(ny * 256.0f);
        if (xi < 0 || yi < 0 || xi >= SD_PW * 256 || yi >= SD_PW * 256 || sd_cover((uint32_t)xi, (uint32_t)yi) < 128u)
            continue;                        /* off the plate: the move is refused */
        g->x = (uint16_t)xi, g->y = (uint16_t)yi;
        moved++;
    }
    sd_moved = (uint16_t)(moved > 65535u ? 65535u : moved);
    sd_nfall = (uint16_t)nfall;
}

/* the sweep's clock: the fade, then the fresh sprinkle */
static void sd_sweep_tick(void)
{
    uint32_t t;
    if (!sd_sweeping)
        return;
    t = ui_now - sd_sweep_at;
    if (t < 600u) {
        sd_fade = 1.0f - (float)t * (1.0f / 600.0f);
    } else {
        sd_sweeping = 0;
        sd_fade = 1.0f;
        sd_sprinkle(4u);
    }
}

/* ---- the splash (spec §6.7): grains spring onto a mask, hold, then burst. Two masks: the boot's credit (CY_CREDIT,
 * "TEAM EG &" over "LIZARD_MAN"), then the CY_LOGO; the last burst is into an even sprinkle on the plate ---- */
typedef struct { const uint8_t *bits; int32_t w, h, x, y; uint32_t n; } sd_mask_t;   /* x, y: its top-left (box px) */
static const sd_mask_t SD_CREDIT = {CY_CREDIT, CY_CREDIT_W, CY_CREDIT_H, (SD_PW - CY_CREDIT_W) / 2, 60, CY_CREDIT_N};
static const sd_mask_t SD_LOGO = {CY_LOGO, CY_LOGO_W, CY_LOGO_H, 32, 72, CY_LOGO_N};   /* at screen (60, 108) */
enum { SD_SPL_CREDIT, SD_SPL_LOGO, SD_SPL_SCATTER, SD_SPL_SPRINKLE };
static uint32_t sd_hash(uint32_t i)
{
    i ^= i >> 16, i *= 0x7FEB352Du, i ^= i >> 15, i *= 0x846CA68Bu, i ^= i >> 16;
    return i;
}
static int sd_mask_px(const sd_mask_t *m, int32_t x, int32_t y)
{
    return m->bits[y * ((m->w + 7) / 8) + x / 8] >> (7 - (x & 7)) & 1;
}

/* every grain toward its target in `to`: SD_SPL_CREDIT / SD_SPL_LOGO its share of the mask's lit pixels (the
 * (k * lit / n)-th, k the grain's slot); SD_SPL_SCATTER a fixed random point in a disc over the box (the credit's
 * burst); SD_SPL_SPRINKLE a fixed random point inside the plate. rate: 1/s of an exponential approach (9: tau
 * 110 ms), 0 = exactly there (a hold, or reduced motion's cut). The logo's slots are the grains in order, as they
 * always were; the credit's are a stride through them (k = i x 1237 mod n), so the grains it borrows above GRAINS
 * (ui_splash_grains) come from all over it, not from the end of its last line */
#define SD_SPL_STRIDE 1237u                   /* prime: coprime to every n (600..2000), so k -> i is a permutation */
static uint32_t sd_spl_inv_n, sd_spl_inv = 1u;   /* the stride's inverse mod n, for that n */
static void sd_splash_step(int to, float rate)
{
    const sd_mask_t *m = to == SD_SPL_CREDIT ? &SD_CREDIT : to == SD_SPL_LOGO ? &SD_LOGO : 0;
    uint32_t k, lit = 0, want, n = sd_n ? sd_n : 1u, inv = 1u;
    int32_t cur = -1;                        /* the mask cursor (linear), lit = lit pixels in [0, cur] */
    float kk = rate > 0.0f ? 1.0f - fm_expf(-ui_dt * rate) : 1.0f;
    if (to == SD_SPL_CREDIT) {               /* slot k is grain k x inv mod n (once per n: a search of < n steps) */
        if (sd_spl_inv_n != n) {
            uint32_t a = SD_SPL_STRIDE % n;
            sd_spl_inv_n = n;
            for (sd_spl_inv = 1u; sd_spl_inv < n && a * sd_spl_inv % n != 1u; sd_spl_inv++)
                ;
            if (sd_spl_inv >= n)             /* (not coprime: the grains in order) */
                sd_spl_inv = 1u;
        }
        inv = sd_spl_inv;
    }
    for (k = 0; k < sd_n; k++) {
        uint32_t i = inv == 1u ? k : k * inv % n;
        sd_grain_t *g = &sd_g[i];
        uint32_t h = sd_hash(i * 2654435761u + 17u);
        float tx, ty, nx, ny, d;
#ifdef UI_POLL
        if ((k & 511u) == 511u)              /* (the app's note path every 512 grains: see ui_frame's splash) */
            UI_POLL();
#endif
        if (to == SD_SPL_SPRINKLE) {         /* a fixed target per grain: rehash until it lands inside (32 tries:
                                              * with 8, a BAR's 1 in 6 that missed every time piled up in the centre) */
            uint32_t j, x = SD_PW * 128, y = SD_PW * 128;
            for (j = 0; j < 32u; j++, h = sd_hash(h)) {
                uint32_t xx = (h & 0xFFFFu) * (SD_PW * 256 - 1) >> 16, yy = (h >> 16) * (SD_PW * 256 - 1) >> 16;
                if (sd_cover(xx, yy) >= 128u) {
                    x = xx, y = yy;
                    break;
                }
            }
            tx = (float)x, ty = (float)y;
        } else if (to == SD_SPL_SCATTER) {   /* an even dusting of a disc (r 90 px) over the box: rehash until in */
            uint32_t j, x = SD_PW * 128, y = SD_PW * 128;
            for (j = 0, h = sd_hash(h ^ 0x5CA77E2Bu); j < 8u; j++, h = sd_hash(h)) {
                int32_t dx = (int32_t)((h & 0xFFFFu) * (SD_PW * 256 - 1) >> 16) - SD_PW * 128;
                int32_t dy = (int32_t)((h >> 16) * (SD_PW * 256 - 1) >> 16) - SD_PW * 128;
                if ((uint32_t)(dx * dx) + (uint32_t)(dy * dy) < 90u * 90u * 65536u) {
                    x = (uint32_t)(dx + SD_PW * 128), y = (uint32_t)(dy + SD_PW * 128);
                    break;
                }
            }
            tx = (float)x, ty = (float)y;
        } else {                             /* the (k * lit / n)-th lit pixel, found by walking the mask once */
            int32_t lx, ly;
            want = k * m->n / n;             /* (<= 2000 x 6912: no overflow) */
            while (lit <= want && cur < m->w * m->h - 1) {
                cur++;
                lit += (uint32_t)sd_mask_px(m, cur % m->w, cur / m->w);
            }
            lx = cur % m->w, ly = cur / m->w;
            tx = ((float)(lx + m->x) + (float)(h & 255u) * (1.0f / 256.0f)) * 256.0f;
            ty = ((float)(ly + m->y) + (float)(h >> 8 & 255u) * (1.0f / 256.0f)) * 256.0f;
        }
        nx = (float)g->x + (tx - (float)g->x) * kk;
        ny = (float)g->y + (ty - (float)g->y) * kk;
        if (kk >= 1.0f)
            nx = tx, ny = ty;
        d = fm_fabsf(tx - nx) + fm_fabsf(ty - ny);
        g->x = (uint16_t)fm_clampf(nx, 0.0f, (float)(SD_PW * 256 - 1));
        g->y = (uint16_t)fm_clampf(ny, 0.0f, (float)(SD_PW * 256 - 1));
        g->f = (uint8_t)(d > 512.0f ? SD_AIR : 0u);   /* in flight (> 2 px from home): drawn over its shadow */
        g->t = 0;
    }
    sd_moved = (uint16_t)sd_n;
}

/* scatter every grain over the whole box (the splash starts from an even dusting of the screen) */
static void sd_scatter_box(void)
{
    uint32_t i;
    sd_rebin = 1;
    for (i = 0; i < SD_MAX; i++) {
        sd_g[i].x = (uint16_t)(sd_rand01() * (float)(SD_PW * 256 - 1));
        sd_g[i].y = (uint16_t)(sd_rand01() * (float)(SD_PW * 256 - 1));
        sd_g[i].f = sd_g[i].t = 0;
    }
}

/* ---- drawing ---- */
/* where a grain is drawn, in 1/256 px: lifted grains up-left of their shadow, falling ones above where they land */
static inline void sd_draw_pos(const sd_grain_t *g, int32_t *x, int32_t *y)
{
    *x = (int32_t)g->x;
    *y = (int32_t)g->y;
    if (g->f & SD_FALL)
        *y -= (int32_t)((float)g->t * (SD_FALL_PX * 256.0f / (float)SD_FALL_T));
    if (g->f & (SD_AIR | SD_FALL)) {
        *x -= 256;
        *y -= 256;
    }
}

/* count-sort the grains into 8-row bands by where they are drawn */
static void sd_bin(void)
{
    uint32_t i, cnt[SD_NB], air = 0;
    for (i = 0; i < SD_NB; i++)
        cnt[i] = 0;
    for (i = 0; i < sd_n; i++) {
        int32_t x, y, b;
        air += (sd_g[i].f & (SD_AIR | SD_FALL)) != 0;
        sd_draw_pos(&sd_g[i], &x, &y);
        b = y < 0 ? 0 : (y >> 11) + 1;       /* band 0: above the plate (falling in) */
        cnt[b < SD_NB ? b : SD_NB - 1]++;
    }
    sd_nair = (uint16_t)air;
    sd_rebin = 0;
    sd_band[0] = 0;
    for (i = 0; i < SD_NB; i++)
        sd_band[i + 1] = (uint16_t)(sd_band[i] + cnt[i]);
    for (i = 0; i < SD_NB; i++)
        cnt[i] = sd_band[i];
    for (i = 0; i < sd_n; i++) {
        int32_t x, y, b;
        sd_draw_pos(&sd_g[i], &x, &y);
        b = y < 0 ? 0 : (y >> 11) + 1;
        sd_idx[cnt[b < SD_NB ? b : SD_NB - 1]++] = (uint16_t)i;
    }
}

/* the bands that hold grains drawn on plate rows [y0, y1) */
static void sd_bands(int32_t y0, int32_t y1, uint32_t *a, uint32_t *b)
{
    int32_t ba = y0 < 0 ? 0 : (y0 >> 3) + 1, bb = y1 < 0 ? 0 : (y1 >> 3) + 1;
    if (y0 < 8)
        ba = 0;
    if (bb > SD_NB - 1)
        bb = SD_NB - 1;
    *a = sd_band[ba];
    *b = sd_band[bb + 1];
}

/* shadows of the lifted and falling grains on plate rows [ys, ye): 50 % toward void, 1 px down-right of where the
 * grain really is. sf is the strip's surface (screen coordinates) */
static void sd_shadows(sf_t *sf, int32_t ys, int32_t ye)
{
    uint32_t a, b, k;
    sd_bands(ys - 2 - (int32_t)SD_FALL_PX - 2, ye + 1, &a, &b);   /* (a falling grain is drawn up to 13 px above its shadow) */
    for (k = a; k < b; k++) {
        const sd_grain_t *g = &sd_g[sd_idx[k]];
        int32_t x, y;
        if (!(g->f & (SD_AIR | SD_FALL)))
            continue;
        x = (int32_t)(g->x >> 8) + 1, y = (int32_t)(g->y >> 8) + 1;
        if (y >= ys && y < ye)
            sf_px(sf, 28 + x, 36 + y, UI_C_VOID, 16u);
    }
}

/* the tone map: 64 steps of v = 0.8 dens + 0.4 glow (grains), alpha = 1 - e^-v, colour sand_lo .. sand_hi by alpha;
 * the composite stops at step SD_IDX_MAX (alpha 0.96), so a pile keeps a trace of the plate and its grain */
static uint8_t sd_lut_a[64];
static uint16_t sd_lut565[64];           /* the colour by step, RGB565 (unswapped): the blend is gfx2.c's packed one */

static void sd_lut(const ui_rgb_t *lo, const ui_rgb_t *hi, int freeze, float fade)
{
    uint32_t i;
    for (i = 0; i < 64u; i++) {
        float v = (float)i * (1.0f / 16.0f), a = 1.0f - fm_expf(-v), r, g, b;
        r = (float)lo->r + ((float)hi->r - (float)lo->r) * a;
        g = (float)lo->g + ((float)hi->g - (float)lo->g) * a;
        b = (float)lo->b + ((float)hi->b - (float)lo->b) * a;
        if (freeze) {                        /* FREEZE: the sand 45 % toward a frosted white (ice itself on a warm
                                              * sand reads as lime; the rim keeps the ice) */
            r += ((float)UI_FROST8.r - r) * 0.45f;
            g += ((float)UI_FROST8.g - g) * 0.45f;
            b += ((float)UI_FROST8.b - b) * 0.45f;
        }
        sd_lut_a[i] = (uint8_t)(i ? fm_clampf(a * fade * 255.0f + 0.5f, 0.0f, 255.0f) : 0.0f);
        sd_lut565[i] = (uint16_t)((uint32_t)fm_minf((r + 4.0f) * 0.125f, 31.0f) << 11 | (uint32_t)fm_minf((g + 2.0f) * 0.25f, 63.0f) << 5
                                  | (uint32_t)fm_minf((b + 4.0f) * 0.125f, 31.0f));
    }
}

/* splat, glow and composite the sand of plate rows [ys, ye) over the strip buffer buf (184 wide, row 0 = ys). band /
 * far_in: the plate's band map, so sand fades across the rim.
 *
 * A pixel's tone step is (205 d + 6 G) >> 10, d its density and G the 3 x 3 sum around it: so a pixel without a
 * grain of its own shows only where G >= 171, three grains' worth close by. Each density row keeps, per 8-px tile,
 * which tiles its splats touched and how much weight landed in each. An output tile whose 3 x 3-tile neighbourhood
 * holds less than 171 is "light": only its pixels with a grain can show, and each of those gathers its 3 x 3 sum
 * directly. Only "heavy" tiles (piles, dense lines) run the row-sum glow over every pixel. Both give the same pixels;
 * scattered, hopping sand (every grain apart) costs a third of what one glow pass over the whole strip did. The
 * touched tiles are cleared at the end, so the density strip starts each strip at zero without a full memset */
#define SD_NT 23                         /* 8-px tiles across the box */
#define SD_TALL ((1u << SD_NT) - 1u)
#define SD_IDX_MAX 52u
#define SD_GLOW_MIN 171u                 /* the least G that shows on its own: 6 G >= 1024 */
typedef union { uint16_t h[SD_PW]; uint32_t w[SD_PW / 2]; } sd_hrow_t;   /* (w: two pixels at a time) */
static const sd_hrow_t sd_zero;
static inline uint32_t sd_dil(uint32_t m) { return (m | m << 1 | m >> 1) & SD_TALL; }

/* one sand pixel over the surface: d its density, G its 3 x 3 sum (plate row py, column x) */
static inline void sd_px(uint16_t *out, uint32_t x, uint32_t py, uint32_t d, uint32_t G, const int8_t *brow, int32_t far_in)
{
    uint32_t idx = (d * 205u + G * 6u) >> 10, al, n;
    int32_t bd;
    if (!idx)
        return;
    if (idx > SD_IDX_MAX)
        idx = SD_IDX_MAX;
    al = sd_lut_a[idx];
    bd = brow[x >> 1];
    if (bd > far_in)                         /* over the rim: fainter, so the rim stays a line */
        al = (al * sd_rimfade[bd + 128]) >> 8;
    /* +-6 % of alpha by a fixed per-pixel hash: a dense pile keeps its grain instead of a flat blob */
    n = ((x * 0x9E3779B1u) ^ (py * 0x85EBCA77u)) >> 28;
    al += (al * n) >> 7;
    al = (al - (al >> 4) + 4u) >> 3;         /* to gfx2.c's 0..32 */
    if (!al)
        return;
    out[x] = al >= 32u ? swap16(sd_lut565[idx]) : sf_mix(out[x], sd_lut565[idx], al);
}

static void sd_strip(uint16_t *buf, int32_t ys, int32_t ye, const int8_t *band, int32_t far_in)
{
    uint32_t a, b, k, tm[SD_ROWS], hv[SD_ROWS], wm[3] = {0, 0, 0};
    uint16_t wt[SD_ROWS][SD_NT];             /* the splat weight per row and tile (a bound on its density sum) */
    int32_t y, base = ys - SD_HALO;
    sd_hrow_t hs[3];
    const sd_hrow_t *hr[SD_ROWS];
    memset(wt, 0, sizeof wt);
    for (k = 0; k < (uint32_t)SD_ROWS; k++)
        tm[k] = hv[k] = 0, hr[k] = &sd_zero;
    sd_bands(base - 1, ye + SD_HALO, &a, &b);
    for (k = a; k < b; k++) {
        int32_t gx, gy, i, j, fx, fy, r;
        uint32_t tb;
        sd_draw_pos(&sd_g[sd_idx[k]], &gx, &gy);
        gx -= 128, gy -= 128;                /* to pixel-centre coordinates */
        i = gx >> 8, j = (gy >> 8) - base, fx = gx & 255, fy = gy & 255;
        if (j < -1 || j >= SD_ROWS || i < -1 || i >= SD_PW)
            continue;
        tb = (i >= 0 ? 1u << (i >> 3) : 0u) | (i + 1 < SD_PW ? 1u << ((i + 1) >> 3) : 0u);
        for (r = 0; r < 2; r++) {
            int32_t jj = j + r, wy = r ? fy : 256 - fy, c;
            uint8_t *row;
            if (jj < 0 || jj >= SD_ROWS)
                continue;
            row = &sd_dens[jj * SD_PW];
            for (c = 0; c < 2; c++) {
                int32_t ii = i + c, w = ((c ? fx : 256 - fx) * wy) >> 10, v;
                if (ii < 0 || ii >= SD_PW || !w)
                    continue;
                v = row[ii] + w;
                row[ii] = (uint8_t)(v > 255 ? 255 : v);
                wt[jj][ii >> 3] = (uint16_t)(wt[jj][ii >> 3] + w);
            }
            tm[jj] |= tb;
        }
    }
    /* the heavy tiles of each output row: a 3 x 3-tile neighbourhood with weight enough for a glow to show */
    {
        uint16_t w3[SD_ROWS][SD_NT];         /* each row's 3-tile sums */
        int32_t r, t;
        for (r = 0; r < SD_ROWS; r++)
            if (tm[r])
                for (t = 0; t < SD_NT; t++)
                    w3[r][t] = (uint16_t)(wt[r][t] + (t ? wt[r][t - 1] : 0) + (t + 1 < SD_NT ? wt[r][t + 1] : 0));
        for (r = 1; r + 1 < SD_ROWS; r++) {
            uint32_t m = 0;
            if (!(tm[r - 1] | tm[r] | tm[r + 1]))
                continue;
            for (t = 0; t < SD_NT; t++) {
                uint32_t s3 = (tm[r - 1] ? w3[r - 1][t] : 0u) + (tm[r] ? w3[r][t] : 0u) + (tm[r + 1] ? w3[r + 1][t] : 0u);
                if (s3 >= SD_GLOW_MIN)
                    m |= 1u << t;
            }
            hv[r] = m;
        }
    }
    /* the rows' horizontal 3-sums, only over the tiles a heavy tile next to them needs; three buffers, rolled. A
     * buffer is zero outside the tiles it was last written over, so the blend may read any of them */
    memset(hs, 0, sizeof hs);
    for (y = ys - 1; y <= ye; y++) {
        int32_t r = y - base;
        if (r >= 0 && r < SD_ROWS && tm[r]) {
            uint32_t need = (r > 0 ? hv[r - 1] : 0u) | hv[r] | (r + 1 < SD_ROWS ? hv[r + 1] : 0u);
            uint32_t m = (uint32_t)r % 3u, want = sd_dil(tm[r]) & need, stale = wm[m] & ~want, t;
            const uint8_t *d = &sd_dens[r * SD_PW];
            uint16_t *h = hs[m].h;
            for (t = 0; stale; t++, stale >>= 1)
                if (stale & 1u)
                    hs[m].w[4u * t] = hs[m].w[4u * t + 1u] = hs[m].w[4u * t + 2u] = hs[m].w[4u * t + 3u] = 0;
            for (t = 0; t < SD_NT; t++)
                if (want >> t & 1u) {
                    int32_t x = 8 * (int32_t)t, x1 = x + 8;
                    if (!x)                   /* (the box's edges: one neighbour) */
                        h[0] = (uint16_t)(d[0] + d[1]), x = 1;
                    if (x1 == SD_PW)
                        h[SD_PW - 1] = (uint16_t)(d[SD_PW - 2] + d[SD_PW - 1]), x1 = SD_PW - 1;
                    {                         /* a rolling window: one load a pixel */
                        uint32_t a0 = d[x - 1], a1 = d[x];
                        for (; x < x1; x++) {
                            uint32_t a2 = d[x + 1];
                            h[x] = (uint16_t)(a0 + a1 + a2);
                            a0 = a1, a1 = a2;
                        }
                    }
                }
            wm[m] = want;
            hr[r] = want ? &hs[m] : &sd_zero;
        }
        if (y - 1 < ys)                       /* (the first two passes only fill the window) */
            continue;
        {   /* output row y - 1 */
            int32_t ro = r - 1;
            uint32_t py = (uint32_t)(y - 1), t, x;
            uint16_t *out = &buf[(y - 1 - ys) * SD_PW];
            const uint8_t *d = &sd_dens[ro * SD_PW];
            const sd_hrow_t *hm = hr[ro - 1], *h0 = hr[ro], *hp = hr[ro + 1];
            const int8_t *brow = &band[(py >> 1) * CY_COVER];
            uint32_t mh = hv[ro], ml = tm[ro] & ~hv[ro];
            for (t = 0; mh; t++, mh >>= 1) {  /* heavy tiles: every pixel, by the row sums, two at a time */
                uint32_t x2;
                if (!(mh & 1u))
                    continue;
                for (x2 = 4u * t; x2 < 4u * t + 4u; x2++) {
                    uint32_t g2 = hm->w[x2] + h0->w[x2] + hp->w[x2];   /* (lanes <= 3 x 765: no carry between them) */
                    if (!g2 || (!(d[2u * x2] | d[2u * x2 + 1u]) && (g2 & 0xFFFFu) < SD_GLOW_MIN && (g2 >> 16) < SD_GLOW_MIN))
                        continue;                 /* (no grain, and a glow too faint to show: nothing to draw) */
                    for (x = 2u * x2; x < 2u * x2 + 2u; x++)
                        sd_px(out, x, py, d[x], (uint32_t)hm->h[x] + h0->h[x] + hp->h[x], brow, far_in);
                }
            }
            for (t = 0; ml; t++, ml >>= 1) {  /* light tiles: only the pixels with a grain, each gathering its 3 x 3 */
                const uint32_t *dw;
                if (!(ml & 1u))
                    continue;
                dw = &sd_du.w[((uint32_t)ro * SD_PW + 8u * t) >> 2];
                if (!(dw[0] | dw[1]))
                    continue;
                for (x = 8u * t; x < 8u * t + 8u; x++) {
                    const uint8_t *dm = d - SD_PW, *dp = d + SD_PW;
                    uint32_t G;
                    if (!d[x])
                        continue;
                    G = (uint32_t)dm[x] + d[x] + dp[x];
                    if (x)
                        G += (uint32_t)dm[x - 1] + d[x - 1] + dp[x - 1];
                    if (x + 1 < SD_PW)
                        G += (uint32_t)dm[x + 1] + d[x + 1] + dp[x + 1];
                    sd_px(out, x, py, d[x], G, brow, far_in);
                }
            }
        }
    }
    for (k = 0; k < (uint32_t)SD_ROWS; k++) {   /* the density strip back to zero: only the touched tiles */
        uint32_t m = tm[k], t;
        uint32_t *row = &sd_du.w[k * (SD_PW / 4u)];
        for (t = 0; m; t++, m >>= 1)
            if (m & 1u)
                row[2u * t] = row[2u * t + 1u] = 0;
    }
}
