/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 UI (M5): the one screen, always live (spec §6). Part of the unity build: app.c includes this after gfx.c,
 * so cv_*, lcd_fill, lcd_blit, lcd_sync, swap16, RGB and felucca_font_t are in scope. Statics are prefixed ui_,
 * sf_, pl_, sd_ or vw_ (spec §2.4.4).
 *
 *   y   0..35   knob strip   (chrome.c)
 *   y  36..219  ladder (x 0..27) | plate box 184 x 184 (x 28..211: plate.c, sand.c, views.c) | meters (x 212..239)
 *   y 220..239  footer       (chrome.c; its overlays and the plate's in overlay.c)
 *
 * What it reads: the app's cy_ui_state_t each frame, the engine's telemetry and UI events (cy_engine_telemetry_get,
 * cy_engine_event: this is the events' one consumer), and the published object and field cache (cy_geom_object,
 * cy_geom_fields). Everything on the plate comes from those: nothing moves unless it carries information.
 *
 * When it draws (§6.9): the plate box on every frame something in it moves (sand, sound, a ripple, a fading pill)
 * or changes, the rails at 30 Hz while what they show changes, the knob cells and the footer when what they show
 * changes. Silent, settled and with nothing up, it draws nothing at all (0 fps). On the device the plate runs at
 * 30 fps; 60 when the audio load stays under 60 % for 2 s and a frame is cheap enough (under 7 ms, and 60 of them
 * fit beside the audio within 80 % of the CPU); 20 when 30 would not fit, or over 90 % audio (then also at most
 * 1,200 grains). A frame's cost is measured (an average of the frames that drew the whole plate), so the rate
 * follows what the screen is doing: the main loop always keeps time for the controls. The host and the browser run
 * at 60. The sand's physics is frame-rate independent, so every rate shows the same figure. Under 60 fps the main
 * loop ticks twice or more a frame: the sand's physics (the field, the displacement, the step, the binning) runs on
 * the tick before the one that draws, so no single tick holds the main loop for a whole frame's work (the controls
 * are read between ticks).
 *
 * Memory: no framebuffer. The pool holds the two 184 x 40 strip buffers, the sand and its grids, the band map and
 * the mesh points (~80 KB); the knob strip, rails and footer reuse gfx.c's 240 x 36 canvas. */
#include "cy_fonts.h"
#include "palette.h"

/* ---- shared by every ui/ file ---- */
static const cy_ui_state_t *ui_st;        /* this frame's state, from the app */
static cy_telemetry_t ui_tel;             /* this frame's copy of the engine's telemetry */
static const cy_object_t *ui_obj;         /* the published object (geom.c) */
static const cy_fieldcache_t *ui_fc;      /* its fields, SDF and cover */
static uint32_t ui_now;                   /* ms */
static float ui_dt = 1.0f / 60.0f, ui_dtn = 1.0f;   /* seconds since the last physics step, and x 60 */
static float ui_fdt = 1.0f / 60.0f;       /* seconds since the last ui_frame (fades, tweens: every call) */
static uint8_t ui_shot;                   /* shotmode: deterministic particles, fixed steps, no half-fades */
static uint8_t ui_plate_dirty = 1, ui_full = 1;
static ui_rgb_t ui_plate, ui_plate_hi, ui_sand_lo, ui_sand_hi;   /* the MATERIAL tint, this frame */
static uint16_t ui_rim565 = UI_C_RIM, ui_sand_hi565 = UI_C_INK, ui_sand_lo565 = UI_C_GLOW;
static uint16_t vw_mcol[65];              /* MESH colours by height (plate.c builds them, views.c draws them) */
static float ui_strike[CY_NMODES];        /* the strike vector the sand's coherent projection uses */
static int8_t ui_skey = -1;               /* SURFACE: the key struck last */
static uint32_t vw_seed;                  /* this frame's seed for the marks' sparkles (the same in every strip) */

#include "gfx2.c"
static void vw_marks(sf_t *sf);
static void vw_mesh_prepare(const float *hgrid);
static void vw_mesh_strip(sf_t *sf, const float *hgrid);
static void ui_ov_plate(sf_t *sf, int splash);
#include "sand.c"
#include "plate.c"
#include "views.c"
#include "overlay.c"
#include "chrome.c"

static uint32_t ui_us, ui_us_max;
static uint32_t ui_step_t, ui_draw_t, ui_chrome_t, ui_rail_t, ui_cpu_lo_t;
static uint32_t ui_pre_us;                /* the last early physics tick's time (us): part of the next frame's cost */
static uint8_t ui_pre;                    /* the sand was stepped on the tick before this frame's (see ui_frame) */
static uint32_t ui_psig, ui_fc_seen, ui_spr_seen, ui_swp_seen, ui_pulse_seen;
static uint32_t ui_cost = 10000u;         /* a whole-plate frame's cost (us, wall clock), averaged: the pacing's */
static uint8_t ui_started, ui_splash_done, ui_fps = 30, ui_keymode_seen;

void ui_init(void)
{
    uint32_t i;
    ui_obj = cy_geom_object();
    ui_fc = cy_geom_fields();
    pl_tables();
    for (i = 0; i < 2u * PL_W * PL_STRIP; i++)   /* the pool is zeroed by its owner, never assumed */
        pl_buf[0][i] = 0;
    for (i = 0; i < PL_HR * PL_HR; i++)
        pl_band[i] = 127;
    for (i = 0; i < CY_GRID * CY_GRID; i++) {
        pl_D[i] = 0.0f;
        vw_mp[i][0] = vw_mp[i][1] = 0;
    }
    for (i = 0; i < CY_NMODES; i++)
        pl_theta[i] = 0.0f, ui_strike[i] = 0.0f;
    for (i = 0; i < VW_NRIP; i++)
        vw_rip[i].live = 0;
    for (i = 0; i < 4u; i++)
        ui_cell[i].id = ui_cell[i].fid = -2, ui_cell[i].sig = 0, ui_cell[i].bar = ui_cell[i].vel = 0.0f;
    sd_init();
    pl_dmax = 0.0f;
    pl_csig = pl_band_seq = vw_seg_seq = 0xFFFFFFFFu;
    vw_nseg = 0;
    ui_fp.a = ui_bp.a = 0.0f, ui_fp.on = ui_bp.on = 0;
    ui_hint_mute_p = -1, ui_hint_mute_t = 0;
    ui_skey = -1;
    ui_started = ui_splash_done = 0;
    ui_full = ui_plate_dirty = 1;
    ui_us = ui_us_max = 0;
    ui_cost = 10000u;
    pl_d_csig = 0xFFFFFFFFu, pl_d_sa = -1;
    pl_flat = 0;
}

void ui_redraw(void) { ui_full = 1; }

void ui_shot_mode(int on)
{
    ui_shot = (uint8_t)(on != 0);
    if (ui_shot && ui_fc) {                   /* a fresh, reproducible sprinkle and camera */
        uint32_t i;
        sd_rng = 0x51A2D00Du;
        for (i = 0; i < SD_MAX; i++) {
            sd_g[i].f = sd_g[i].t = 0;
            sd_place(&sd_g[i]);
        }
        sd_rebin = 1;
        vw_az = 0.5236f;
        ui_plate_dirty = 1;
    }
}

uint32_t ui_frame_us(void) { return ui_us; }
uint32_t ui_frame_us_max(void) { return ui_us_max; }

/* the splash screen the platform shows while the app starts: only void (spec §6.7); the animated one is ui_frame's */
void ui_splash_static(void)
{
    lcd_fill(0, 0, 240, 240, UI_C_VOID);
    lcd_sync();
}

/* the key point nearest plate (u, v) */
static int ui_near_key(float u, float v)
{
    int k, best = 0;
    float bd = 1e9f;
    for (k = 0; k < CY_NKEYS; k++) {
        float du = ui_obj->key_uv[k][0] - u, dv = ui_obj->key_uv[k][1] - v, d = du * du + dv * dv;
        if (d < bd)
            bd = d, best = k;
    }
    return best;
}

/* the engine's UI events: ripples, flashes, and in SURFACE which key point was struck (the sand's strike vector) */
static void ui_events(int show)
{
    cy_uievent_t e;
    int n = 0;
    while (n++ < 64 && cy_engine_event(&e)) {
        float u = (float)e.u * (1.0f / 65535.0f), v = (float)e.v * (1.0f / 65535.0f);
        if (!show)
            continue;
        switch (e.type) {
        case CY_UIEV_STRIKE:
            vw_ripple(u, v, VW_R_STRIKE, e.vel);
            if (ui_st->keymode)
                ui_skey = (int8_t)ui_near_key(u, v);
            break;
        case CY_UIEV_DROP:
            vw_ripple(u, v, VW_R_DROP, e.vel);
            break;
        case CY_UIEV_SLIP:
            vw_ripple(u, v, VW_R_SLIP, e.vel);
            break;
        case CY_UIEV_TOUCH: {
            int k = ui_near_key(u, v);
            vw_ripple(ui_obj->key_uv[k][0], ui_obj->key_uv[k][1], VW_R_TOUCH, e.vel);
            break;
        }
        default:
            break;
        }
    }
}

static void ui_strike_vec(void)
{
    uint32_t k;
    const float *s = ui_obj->strike;
    if (ui_st->keymode != ui_keymode_seen)
        ui_keymode_seen = ui_st->keymode, ui_skey = -1;
    if (ui_st->keymode && ui_skey >= 0)
        s = ui_obj->surface[ui_skey];
    for (k = 0; k < CY_NMODES; k++)
        ui_strike[k] = s[k];
}

/* the plate's signature: anything discrete it shows (motion is "anim" instead) */
static uint32_t ui_plate_hash(uint32_t ov)
{
    const cy_ui_state_t *st = ui_st;
    uint32_t h = 0x811C9DC5u, q[6];
    q[0] = (uint32_t)st->view | (uint32_t)st->theme << 2 | (uint32_t)st->frozen << 4 | (uint32_t)st->keymode << 5
         | (uint32_t)st->reduced_motion << 6 | (uint32_t)st->exciter << 7;
    q[1] = ui_fc->seq;
    q[2] = ui_obj->seq;
    q[3] = st->keys_held;
    q[4] = ov;
    q[5] = pl_csig;
    h = sf_hash(h, q, sizeof q);
    return sf_hash(h, st->key_press, sizeof st->key_press);
}

void ui_frame(const cy_ui_state_t *st)
{
    uint32_t t0 = plat_ticks24(), since, n, el, period;
    int splash, due, pre, phys, used_pre = 0, anim, ova = 0, whole = 0, rebuilt = 0;
    ui_st = st;
    ui_now = st->now;
    ui_obj = cy_geom_object();
    ui_fc = cy_geom_fields();
    cy_engine_telemetry_get(&ui_tel);
    if (!ui_started) {
        ui_started = 1;
        ui_step_t = ui_draw_t = ui_chrome_t = ui_rail_t = st->now;
        ui_pre = 0;
        ui_spr_seen = st->sprinkle_t, ui_swp_seen = st->sweep_t;
        ui_fc_seen = ui_fc->seq;
        ui_keymode_seen = st->keymode;
        ui_splash_done = st->now - st->boot_t >= 1600u;
        if (!ui_splash_done) {                /* the host never ran app_splash: the screen starts void here */
            lcd_fill(0, 0, 240, 240, UI_C_VOID);
            sd_n = (uint16_t)(st->grains < 600u ? 600u : st->grains > SD_MAX ? SD_MAX : st->grains);
            sd_scatter_box();
        }
    }
    since = st->now - st->boot_t;
    splash = !ui_splash_done && since < 1600u;
    if (!splash && !ui_splash_done) {         /* 1600 ms: the chrome appears, the plate is whole */
        ui_splash_done = 1;
        ui_full = 1;
    }
    ui_events(!splash);
    el = st->now - ui_chrome_t;               /* the frame's own time, for the fades and tweens */
    ui_chrome_t = st->now;
    ui_fdt = ui_shot ? 1.0f / 60.0f : fm_clampf((float)el * 0.001f, 0.001f, 0.1f);
#if !defined(PLAT_HOST) || defined(UI_TEST_PACING)
    /* the device's rate (§6.9): 30 fps; 60 after 2 s under 60 % audio load, when a frame is cheap enough; 20 when 30
     * frames would not fit, or over 90 %. The whole stays under ~80 %: the audio (its share of a frame's wall time
     * is already in cpu_pct) plus fps x a frame's main-loop time */
    {
        uint32_t cpu = st->cpu_pct > 100u ? 100u : st->cpu_pct, net = ui_cost * (100u - cpu) / 100u;
        uint32_t room = cpu < 80u ? (80u - cpu) * 10000u : 0u;   /* us a second the screen may take */
        if (st->cpu_pct > 90u) {
            ui_fps = 20, ui_cpu_lo_t = 0;
        } else if (st->cpu_pct < 60u && net < 7000u && 60u * net <= room) {
            if (!ui_cpu_lo_t)
                ui_cpu_lo_t = st->now ? st->now : 1u;
            ui_fps = st->now - ui_cpu_lo_t >= 2000u ? 60 : 30;
        } else {
            ui_fps = 30, ui_cpu_lo_t = 0;
        }
        if (ui_fps == 30 && 30u * net > room)
            ui_fps = 20;
    }
#else
    ui_fps = 60;
#endif
    if (st->reduced_motion && ui_fps > 20)    /* reduced motion: the sand at 20 Hz (§6.8) */
        ui_fps = 20;
    /* due: this tick draws; pre: it does not, the next one will (ticks are >= 16 ms apart): the physics now */
    period = 1000u / ui_fps;
    due = splash || ui_full || ui_fps >= 60 || st->now - ui_draw_t + 4u >= period;
    pre = !due && !ui_pre && st->now - ui_draw_t + 4u + 16u >= period;
    phys = pre || (due && !ui_pre) || splash;
    if (due) {
        ui_draw_t = st->now;
        used_pre = ui_pre && !splash;
        ui_pre = 0;
    }
    if (pre)
        ui_pre = 1;
    if (phys) {
        el = st->now - ui_step_t;
        ui_step_t = st->now;
        ui_dt = ui_shot ? 1.0f / 60.0f : fm_clampf((float)el * 0.001f, 0.001f, 0.1f);
        ui_dtn = ui_dt * 60.0f;
        pl_colors();
        rebuilt = pl_band_update(ui_full || splash);
        n = st->grains < 600u ? 600u : st->grains > SD_MAX ? SD_MAX : st->grains;
#if !defined(PLAT_HOST) || defined(UI_TEST_PACING)
        if (st->cpu_pct > 90u && n > 1200u)   /* overloaded: at most 1,200 grains */
            n = 1200u;
#endif
        if (!splash) {                        /* GRAINS is for the reference circle: a smaller plate gets fewer, so
                                               * every shape shows the same density (at least 600) */
            uint32_t na = (uint32_t)((float)n * fm_minf(pl_area, 1.0f) + 0.5f);
            n = na < 600u ? (n < 600u ? n : 600u) : na;
        }
        if (n > sd_n && !splash) {            /* more grains: the new ones fall in */
            uint32_t i;
            for (i = sd_n; i < n; i++) {
                sd_place(&sd_g[i]);
                sd_g[i].f = SD_FALL, sd_g[i].t = (uint8_t)(SD_FALL_T - (i & 31u));
                sd_nfall++;
            }
        }
        if (sd_n != n)
            sd_rebin = 1;
        sd_n = (uint16_t)n;
        if (!splash) {
            if (st->sprinkle_t != ui_spr_seen)
                ui_spr_seen = st->sprinkle_t, sd_sprinkle(1u);
            if (st->sweep_t != ui_swp_seen)
                ui_swp_seen = st->sweep_t, sd_sweep();
            sd_sweep_tick();
            pl_colors();                      /* (the sweep's fade, this frame's) */
        }
        if (rebuilt && ui_fc->seq != ui_fc_seen) {   /* a new shape (as the band map has it): the sand flows into it */
            ui_fc_seen = ui_fc->seq;
            if (!splash)
                sd_shape_changed();
        }
        ui_strike_vec();
        sd_field(ui_strike);
        pl_disp(ui_strike);
#ifdef UI_POLL
        UI_POLL();                            /* (the app's note path: see pl_render) */
#endif
    }
    if (splash) {                             /* spec §6.7: grains spring onto the logo, hold, then burst */
        sd_splash_step(since < 700u ? 0 : since < 1100u ? 1 : 2);
        pl_sa = since < 1100u ? 0.0f : (float)(since - 1100u) * (1.0f / 500.0f);
        sd_bin();
        pl_render(1, PL_ALL);
        whole = 1;
    } else {
        uint32_t ps, ov, pulse, mask;
        pl_sa = 1.0f;
        if (phys) {
            sd_step();
            if (sd_moved || sd_rebin)         /* (the positions the next drawing uses, whatever the view) */
                sd_bin();
#ifdef UI_POLL
            UI_POLL();                        /* (after the grain physics, the app's note path: see pl_render) */
#endif
        }
        ov = ui_ov_prepare(&ova);
        ps = ui_plate_hash(ov);
        anim = sd_moved || sd_loud > 0.0f || vw_ripples_live() || ova || sd_sweeping
            || (st->view == CY_VIEW_MESH && vw_mesh_moving());
        pulse = ui_ov_plate_pulse() ? 1u + ui_pulse_lv() : 0u;
        /* the whole box when anything in it moved or changed; only its last strip for a tour chevron's pulse */
        mask = (ui_full || ui_plate_dirty || anim || ps != ui_psig) ? PL_ALL : pulse != ui_pulse_seen ? 1u << 4 : 0u;
        if (due && mask) {
            pl_render(0, mask);
            ui_psig = ps;
            ui_pulse_seen = pulse;
            ui_plate_dirty = 0;
            whole = mask == PL_ALL;
        }
        /* the chrome: knob cells and footer on change, the rails checked at 30 Hz */
#ifdef UI_POLL
        UI_POLL();
#endif
        ui_knobs(ui_full);
#ifdef UI_POLL
        UI_POLL();
#endif
        if (ui_full || st->now - ui_rail_t >= 33u) {
            uint32_t rh = ui_rails_hash();
            if (ui_full || rh != ui_rail_sig
                || (ui_pk_db[0] > -60.0f && st->now - ui_pk_t[0] > 1000u) || (ui_pk_db[1] > -60.0f && st->now - ui_pk_t[1] > 1000u)) {
                ui_rail_sig = rh;
                ui_ladder();
                ui_meters();
                ui_rail_t = st->now;
            }
        }
#ifdef UI_POLL
        UI_POLL();
#endif
        ui_footer(ui_full);
        ui_full = 0;
    }
    ui_us = (plat_ticks24() - t0) / PLAT_TICKS_PER_US;
    if (ui_us > ui_us_max)
        ui_us_max = ui_us;
    if (pre)
        ui_pre_us = ui_us;
    if (whole && !splash) {                   /* the pacing's frame cost (its physics tick too): a quarter of the way
                                               * each whole frame */
        uint32_t c = ui_us + (used_pre ? ui_pre_us : 0u);
        ui_cost = (uint32_t)((int32_t)ui_cost + ((int32_t)(c > 60000u ? 60000u : c) - (int32_t)ui_cost) / 4);
    }
}
