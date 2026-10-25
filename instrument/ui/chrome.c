/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 UI (M5): the chrome around the plate (spec §6.4, §6.5). Part of the unity build: included by ui/ui.c after
 * gfx2.c, sand.c, plate.c, views.c and overlay.c.
 *
 *   knob strip  y 0..35   four 60 x 36 cells, K1..K4 left to right, as the encoders sit right of the screen
 *   ladder      x 0..27   every mode at its frequency ratio (log scale): the partials you hear, CARVE's targets
 *   meters      x 212..239  L / R, gain reduction, pressure and the leaned-on voices; LOAD on the GLOBAL page (and
 *                           anywhere once it passes 70 %)
 *   footer      y 220..239  exciter glyph, preset number and name, state and pitch badges, voices; messages, SAVE,
 *                           morph over it
 *
 * Each part draws into gfx.c's canvas (cv_px, 240 x 36 = 8,640 pixels: every one of these fits) and goes out in one
 * blit. lcd_sync() first, every time: the canvas may still be on its way to the panel from the last part. A part
 * redraws only when a signature of what it draws changes (the cells, the footer; the rails, checked at 30 Hz, by
 * what reaches the screen: a bar's quarter pixel, a meter's pixel), so a fading note stops redrawing them when its
 * last partial drops under the ladder's -60 dB floor. */

typedef struct {
    int8_t id;                            /* the parameter on this knob (-1 none) */
    uint8_t page, layer;
    float bar, vel;                       /* the value bar's end, px into the 48 px track, tweened */
    uint32_t flip_t;                      /* when the page or layer last changed: the label slides in */
    uint32_t sig;
    int8_t fid;                           /* the value's short form, as last formatted (for fid at fnorm) */
    float fnorm;
    char fval[24];
} ui_cell_t;
static ui_cell_t ui_cell[4];
static uint32_t ui_foot_sig, ui_rail_sig;
static float ui_pk_db[2] = {-90.0f, -90.0f};   /* peak hold (dB) and when it was set */
static uint32_t ui_pk_t[2];
static int8_t ui_lad_q[CY_NMODES];        /* the ladder as drawn: each mode's bar, 1/4 px of 20 (-1 none) */
static uint32_t ui_lad_loud[2];           /* ... and the 8 loudest (drawn 2 px thick), a bit each */

/* the value bar's target, px of 48, for parameter id's live value */
static float ui_bar_of(int id, float n)
{
    (void)id;
    return fm_clampf(n, 0.0f, 1.0f) * 48.0f;
}

/* the live-modulated position of the bar (px), or -1: TENSION / FORCE / STRETCH follow pressure, DENSITY the
 * pressure's rate boost, POS the LFO orbit (spec §6.4) */
static float ui_live_px(int id)
{
    const cy_ui_state_t *st = ui_st;
    float n = st->norm[id], P = fm_clampf(st->press_max, 0.0f, 1.0f);
    int held = st->keys_held != 0u && P > 0.01f;
    switch (id) {
    case P_TENSION:
        return st->exciter == CY_EXC_MALLET && held ? ui_bar_of(id, n * P) : -1.0f;
    case P_FORCE:
        return st->exciter == CY_EXC_BOW && held ? ui_bar_of(id, n * P) : -1.0f;
    case P_STRETCH:
        return held ? ui_bar_of(id, n * P) : -1.0f;
    case P_DENSITY:                           /* rate ~ density^2 (0.3 + 0.7 P): the equivalent density */
        return st->exciter == CY_EXC_RAIN && held ? ui_bar_of(id, n * fm_sqrtf(0.3f + 0.7f * P)) : -1.0f;
    case P_POS_X:
        return st->keymode ? -1.0f : ui_bar_of(id, ui_obj->pos_uv[0]);
    case P_POS_Y:
        return st->keymode ? -1.0f : ui_bar_of(id, ui_obj->pos_uv[1]);
    default:
        return -1.0f;
    }
}

/* the critically damped tween (70 ms to settle): x follows target, exactly, whatever the frame time */
static void ui_tween(float *x, float *v, float target, float dt)
{
    const float w = 66.0f;                    /* settles (to 1 %) in ~4.6 / w = 70 ms */
    float x0 = *x - target, e = fm_expf(-w * dt), t1 = (*v + w * x0) * dt;
    *x = target + (x0 + t1) * e;
    *v = (*v - w * t1) * e;
    if (fm_fabsf(*x - target) < 0.02f && fm_fabsf(*v) < 0.5f)
        *x = target, *v = 0.0f;
}

/* a knob cell's label: the parameter's name, or a short form where the name with the labels' +1 px tracking would
 * run into the cell's rule (the focus pill and the hints keep the full name). Every label then keeps the tracking */
static const char *ui_cell_label(int id)
{
    switch (id) {
    case P_STIFFNESS: return "STIFF";
    case P_HARDNESS: return "HARD";
    case P_PREDELAY: return "PRE-DLY";
    case P_VEL_CURVE: return "VEL CRV";
    case P_PRESS_SENS: return "PRS SENS";
    default: return CY_PARAMS[id].name;
    }
}

/* a label's opacity over the cell by its page's hue, so every page's labels read alike (~5.4:1 or better on void:
 * magenta and violet are darker hues than teal and amber, silver lighter) */
static float ui_label_alpha(uint32_t page)
{
    return page == PG_SPACE || page == PG_MOTION ? 0.88f : page == PG_GLOBAL ? 0.70f : 0.75f;
}

static void ui_cell_draw(int k, const char *val, int touched, float slide, float live)
{
    const cy_ui_state_t *st = ui_st;
    ui_cell_t *c = &ui_cell[k];
    uint32_t pg = st->page < PG_N ? st->page : 0u;
    int32_t x0 = 60 * k, tw, ty;
    uint16_t hue = UI_PAGE_HUE[pg];
    uint32_t la;
    float e, b0, b1;
    sf_t sf;
    lcd_sync();
    sf_init(&sf, cv_px, x0, 0, 60, 36);
    sf_rect(&sf, x0, 0, 60, 36, touched ? UI_C_LIFT : UI_C_VOID, 32u);
    if (k < 3)
        sf_rect(&sf, x0 + 59, 6, 1, 24, UI_C_INK_FAINT, 32u);
    if (touched)
        sf_rect(&sf, x0 + 2, 34, 56, 2, hue, 32u);
    if (st->layer)                            /* layer B: a 4 px hue notch in the top-right corner */
        sf_tri(&sf, (float)(x0 + 54), 0.0f, (float)(x0 + 58), 0.0f, (float)(x0 + 58), 4.0f, hue, 32u);
    if (c->id >= 0) {
        const char *lbl = ui_cell_label(c->id);
        int32_t tr = 1;
        /* the label slides up 6 px over 120 ms after a flip; by hue over the cell, 100 % while touched */
        e = 1.0f - (1.0f - slide) * (1.0f - slide);
        la = sf_cov(e * (touched ? 1.0f : ui_label_alpha(pg)), 32u);
        tw = sf_text_w(&FONT_CY_LBL, lbl, 1);
        if (tw > 52)                          /* (no label is: a safety net for a renamed parameter) */
            tr = 0;
        ty = 4 + (int32_t)(6.0f * (1.0f - e) + 0.5f);
        sf_text(&sf, x0 + 6, ty, &FONT_CY_LBL, lbl, hue, la, tr);
        /* the value, short form: VAL, shifted left if it must be, LBL if it still does not fit */
        tw = sf_text_w(&FONT_CY_VAL, val, 0);
        if (tw <= 52)
            sf_text(&sf, x0 + 6, 15, &FONT_CY_VAL, val, UI_C_INK, 32u, 0);
        else if (tw <= 56)
            sf_text(&sf, x0 + 58 - tw, 15, &FONT_CY_VAL, val, UI_C_INK, 32u, 0);
        else
            sf_text(&sf, x0 + 6, 15 + CY_BASE_VAL - CY_BASE_LBL, &FONT_CY_LBL, val, UI_C_INK, 32u, 0);
        /* the bar: rule track, hue fill (bipolar from the centre), an ink cap at the end */
        sf_rect(&sf, x0 + 6, 31, 48, 2, UI_C_RULE, 32u);
        b0 = CY_PARAMS[c->id].bipolar ? 24.0f : 0.0f;
        b1 = c->bar;
        if (b1 < b0) {
            float t = b0;
            b0 = b1, b1 = t;
        }
        if (b1 - b0 > 0.0f) {
            int32_t i0 = (int32_t)b0;
            float f0 = 1.0f - (b0 - (float)i0);
            sf_rect(&sf, x0 + 6 + i0, 31, 1, 2, hue, sf_cov(f0, 32u));
            sf_hbar(&sf, x0 + 7 + i0, b1 - (float)i0 - 1.0f, 32.0f, 2.0f, hue, 32u);
        }
        {
            int32_t cx = x0 + 6 + (int32_t)(c->bar + 0.5f) - (c->bar >= 47.5f ? 1 : 0);
            sf_rect(&sf, cx, 31, 1, 2, UI_C_INK, 32u);
        }
        if (live >= 0.0f)                     /* live modulation: magenta 1 x 4 at y 27..30 */
            sf_rect(&sf, x0 + 6 + (int32_t)(live + 0.5f) - (live >= 47.5f ? 1 : 0), 27, 1, 4, UI_C_MAGENTA, 32u);
    }
    lcd_blit((uint32_t)x0, 0, 60, 36, cv_px);
}

/* the four knob cells: each redraws when what it shows changes (or while it animates). A value is formatted again
 * only when it changed (the formatter is the cells' largest cost on a frame that draws nothing) */
static void ui_knobs(int force)
{
    const cy_ui_state_t *st = ui_st;
    int k;
    for (k = 0; k < 4; k++) {
        ui_cell_t *c = &ui_cell[k];
        int id = cy_page_param(st->page, st->exciter, st->layer, k), touched;
        const char *val = "";
        float live = -1.0f, slide, tgt;
        uint32_t h = 0x811C9DC5u, q[6];
        if (id >= P_NPARAMS)
            id = -1;
        if (id != c->id || st->layer != c->layer || st->page != c->page) {
            if (id != c->id && id >= 0)
                c->bar = ui_bar_of(id, st->norm[id]), c->vel = 0.0f;   /* a new parameter: no tween across */
            c->id = (int8_t)id, c->layer = st->layer, c->page = st->page;
            c->flip_t = ui_now;
        }
        touched = 0;
        if (id >= 0) {
            if (id != c->fid || st->norm[id] != c->fnorm) {
                cy_param_format(id, st->norm[id], c->fval, sizeof c->fval, 0);
                c->fid = (int8_t)id, c->fnorm = st->norm[id];
            }
            val = c->fval;
            tgt = ui_bar_of(id, st->norm[id]);
            ui_tween(&c->bar, &c->vel, tgt, ui_shot ? 1.0f : ui_fdt);
            live = ui_live_px(id);
            if (live >= 0.0f && fm_fabsf(live - c->bar) <= 1.0f)
                live = -1.0f;                 /* within a pixel of the base: no tick */
            touched = st->focus_src == 0 && st->focus_param == id && st->focus_t && ui_now - st->focus_t < 1200u;
        }
        slide = ui_shot ? 1.0f : fm_clampf((float)(ui_now - c->flip_t) * (1.0f / 120.0f), 0.0f, 1.0f);
        q[0] = (uint32_t)(id + 1) | (uint32_t)st->layer << 8 | (uint32_t)st->page << 9 | (uint32_t)touched << 12;
        q[1] = (uint32_t)(c->bar * 8.0f);
        q[2] = (uint32_t)(int32_t)(live * 2.0f + 2.0f);
        q[3] = (uint32_t)(slide * 32.0f);
        q[4] = q[5] = 0;
        h = sf_hash_s(sf_hash(h, q, sizeof q), val);
        if (force || h != c->sig) {
            c->sig = h;
            ui_cell_draw(k, val, touched, slide, live);
        }
    }
}

/* ---- the partial ladder (x 0..27): y(r) = 216 - 29 log2 r ---- */
static const float UI_BELL_T[11] = {1.0f, 2.0f, 2.4f, 3.0f, 4.0f, 5.333f, 6.0f, 8.0f, 10.667f, 12.0f, 16.0f};
static const float UI_BAR_T[5] = {1.0f, 4.0f, 10.0f, 20.0f, 32.0f};

/* the ladder as it will be drawn, from the telemetry: each mode's bar in quarter pixels of its 20 (-60..0 dB), and
 * the 8 loudest */
static void ui_ladder_model(void)
{
    float db[CY_NMODES];
    uint32_t k, j;
    for (k = 0; k < CY_NMODES; k++) {
        float e = ui_tel.mode_energy[k];
        db[k] = e > 1e-12f ? 3.0103f * fm_log2f(e) : -120.0f;
        ui_lad_q[k] = (int8_t)(db[k] > -60.0f ? (int32_t)((fm_minf(db[k], 0.0f) + 60.0f) * (80.0f / 60.0f)) : -1);
    }
    ui_lad_loud[0] = ui_lad_loud[1] = 0;
    for (j = 0; j < 8u; j++) {                /* the 8 loudest are drawn 2 px thick */
        int best = -1;
        for (k = 0; k < CY_NMODES; k++)
            if (!(ui_lad_loud[k >> 5] >> (k & 31u) & 1u) && db[k] > -60.0f && (best < 0 || db[k] > db[best]))
                best = (int)k;
        if (best < 0)
            break;
        ui_lad_loud[best >> 5] |= 1u << (best & 31);
    }
}

static void ui_ladder(void)
{
    const cy_ui_state_t *st = ui_st;
    float carve = st->norm[P_CARVE];
    uint32_t k;
    sf_t sf;
    lcd_sync();
    sf_init(&sf, cv_px, 0, 36, 28, 184);
    sf_rect(&sf, 0, 36, 28, 184, UI_C_VOID, 32u);
    for (k = 1; k <= 16u; k++)                /* integer harmonics: faint dots */
        sf_px(&sf, 2, (int32_t)(216.0f - 29.0f * fm_log2f((float)k) + 0.5f), UI_C_INK_FAINT, 32u);
    if (carve > 0.004f) {                     /* CARVE's targets: amber ticks, firmer with more carve */
        int tg = (int)cy_param_value(P_CARVE_TARGET, st->norm[P_CARVE_TARGET]);
        uint32_t a = sf_cov(0.35f + 0.65f * carve, 32u), n = tg == CY_CARVE_BELL ? 11u : tg == CY_CARVE_BAR ? 5u : 16u;
        for (k = 0; k < n; k++) {
            float r = tg == CY_CARVE_BELL ? UI_BELL_T[k] : tg == CY_CARVE_BAR ? UI_BAR_T[k] : (float)(k + 1u);
            sf_rect(&sf, 25, (int32_t)(216.0f - 29.0f * fm_log2f(r) + 0.5f), 3, 1, UI_C_AMBER, a);
        }
    }
    for (k = 0; k < CY_NMODES; k++) {
        float lv = (float)ui_lad_q[k] * (1.0f / 80.0f), r = ui_obj->ratio[k], y;
        ui_rgb_t c;
        if (ui_lad_q[k] <= 0 || r <= 0.0f)
            continue;
        y = 216.5f - 29.0f * fm_log2f(r);
        if (y < 36.0f)
            continue;
        c = pl_mix(UI_TEAL8, ui_sand_hi, lv);
        sf_hbar(&sf, 5, lv * 20.0f, y, (ui_lad_loud[k >> 5] >> (k & 31u) & 1u) ? 2.0f : 1.0f, pl_565(c), 32u);
    }
    ui_ov_rails(&sf);
    lcd_blit(0, 36, 28, 184, cv_px);
}

static float ui_db20(float lin) { return lin > 1e-6f ? 6.0206f * fm_log2f(lin) : -120.0f; }

/* a meter level (dBFS) as the pixels it lights above the bottom, 0..157 (-60..0 dBFS) */
static int32_t ui_meter_px(float db) { return (int32_t)(fm_clampf((db + 60.0f) * (1.0f / 60.0f), 0.0f, 1.0f) * 157.0f + 0.5f); }

/* LOAD, the figure, and whether it is shown: on the GLOBAL page (with VOICES and DETAIL, what it is about), or
 * anywhere once it passes 70 % (a warning, clip-coloured from 90 %). Device: the measured audio load; host and web:
 * the engine's model estimate against 3,500, shown on the GLOBAL page only (integration, review round: there it is a
 * model of the FM-1's load, not this computer's, so it never warns; the browser's own CPU sits in the page's pill) */
static int32_t ui_load(int *shown)
{
    const cy_ui_state_t *st = ui_st;
    int32_t load;
#ifdef PLAT_HOST
    load = (int32_t)(ui_tel.est_instr * (100.0f / 3500.0f) + 0.5f);
#else
    load = st->cpu_pct;
#endif
    load = load < 0 ? 0 : load > 999 ? 999 : load;
#ifdef PLAT_HOST
    *shown = st->page == PG_GLOBAL;
#else
    *shown = st->page == PG_GLOBAL || load >= 70;
#endif
    return load;
}

/* a voice's pressure tick (its y), or -1: only for a voice held or bowed and leaned on */
static int32_t ui_voice_tick(uint32_t k)
{
    uint8_t s = ui_tel.voice_state[k];
    if ((s != CY_VS_HELD && s != CY_VS_BOWED) || ui_tel.voice_press[k] <= 2u)
        return -1;
    return 195 - (int32_t)((float)ui_tel.voice_press[k] * (154.0f / 127.0f));
}

/* ---- the meters (x 212..239) ---- */
static void ui_meters(void)
{
    const cy_ui_state_t *st = ui_st;
    uint32_t ch, k;
    char t[8];
    int32_t load, n, y1 = 40, y12 = 40;
    int shown;
    sf_t sf;
    while (y1 < 197 && -60.0f + 60.0f * (float)(197 - y1) * (1.0f / 157.0f) >= -1.0f)   /* (the zones' first rows) */
        y1++;
    y12 = y1;
    while (y12 < 197 && -60.0f + 60.0f * (float)(197 - y12) * (1.0f / 157.0f) >= -12.0f)
        y12++;
    lcd_sync();
    sf_init(&sf, cv_px, 212, 36, 28, 184);
    sf_rect(&sf, 212, 36, 28, 184, UI_C_VOID, 32u);
    for (ch = 0; ch < 2u; ch++) {            /* L x 215..217, R x 219..221, y 40..196, (dBFS + 60) / 60 */
        float db = ui_db20(ch ? ui_tel.peak_r : ui_tel.peak_l);
        int32_t x = ch ? 219 : 215, top = 197 - ui_meter_px(db), y;
        if (db > ui_pk_db[ch] || ui_now - ui_pk_t[ch] > 1000u)
            ui_pk_db[ch] = db, ui_pk_t[ch] = ui_now;
        sf_rect(&sf, x, 40, 3, 157, UI_C_RULE, 22u);
        /* ink below -12 dBFS, sand_lo to -1, clip above: three runs (rows y < y1 are clip, y < y12 sand_lo) */
        y = top;
        if (y < y1)
            sf_rect(&sf, x, y, 3, y1 - y, UI_C_CLIP, 32u), y = y1;
        if (y < y12)
            sf_rect(&sf, x, y, 3, y12 - y, ui_sand_lo565, 32u), y = y12;
        if (y <= 196)
            sf_rect(&sf, x, y, 3, 197 - y, UI_C_INK, 32u);
        if (ui_pk_db[ch] > -60.0f)
            sf_rect(&sf, x, 197 - ui_meter_px(ui_pk_db[ch]), 3, 1, ui_pk_db[ch] >= -1.0f ? UI_C_CLIP : UI_C_INK, 32u);
    }
    if (ui_tel.gr_db != 0.0f) {              /* gain reduction: a clip line down from the top, 4 px a dB */
        n = (int32_t)(fm_fabsf(ui_tel.gr_db) * 4.0f + 0.5f);
        sf_rect(&sf, 223, 40, 1, n > 157 ? 157 : n, UI_C_CLIP, 32u);
    }
    /* pressure: a track like the meters', the held maximum in magenta; a 2 px ink tick per voice leaned on */
    sf_rect(&sf, 230, 40, 4, 157, UI_C_RULE, 22u);
    n = (int32_t)(fm_clampf(st->press_max, 0.0f, 1.0f) * 155.0f + 0.5f);
    if (n > 0)
        sf_rect(&sf, 230, 197 - n, 4, n, UI_C_MAGENTA, 26u);
    for (k = 0; k < CY_NVOICES; k++) {
        int32_t y = ui_voice_tick(k);
        if (y >= 0)
            sf_rect(&sf, 235, y, 2, 1, UI_C_INK, 32u);
    }
    load = ui_load(&shown);
    if (shown) {
        sf_utoa(t, (uint32_t)load, 1);
        sf_strcat(t, "%", sizeof t);
        sf_text(&sf, 214, 203, &FONT_CY_TERM, t, load >= 90 ? UI_C_CLIP : UI_C_INK_DIM, 32u, 0);
    }
    lcd_blit(212, 36, 28, 184, cv_px);
}

/* the rails' signature: what they would draw now, as it reaches the screen (so a fading note's inaudible tail does
 * not keep redrawing them) */
static uint32_t ui_rails_hash(void)
{
    const cy_ui_state_t *st = ui_st;
    uint32_t h = 0x811C9DC5u, k, q[8];
    int shown;
    int32_t load = ui_load(&shown), tick[CY_NVOICES];
    ui_ladder_model();
    h = sf_hash(h, ui_lad_q, sizeof ui_lad_q);
    h = sf_hash(h, ui_lad_loud, sizeof ui_lad_loud);
    h = sf_hash(h, ui_obj->ratio, sizeof ui_obj->ratio);
    q[0] = (uint32_t)ui_meter_px(ui_db20(ui_tel.peak_l)) | (uint32_t)ui_meter_px(ui_db20(ui_tel.peak_r)) << 8;
    q[1] = (uint32_t)(int32_t)(fm_fabsf(ui_tel.gr_db) * 4.0f + 0.5f);
    q[2] = (uint32_t)(int32_t)(fm_clampf(st->press_max, 0.0f, 1.0f) * 155.0f + 0.5f);
    q[3] = (uint32_t)st->tour_step | (st->tour_step >= CY_TOUR_SELECT && st->tour_step <= CY_TOUR_PRESET ? ui_pulse_lv() << 8 : 0u);
    q[4] = (uint32_t)(st->norm[P_CARVE] * 64.0f) ^ (uint32_t)(st->norm[P_CARVE_TARGET] * 8.0f) << 8;
    q[5] = shown ? (uint32_t)load + 1u : 0u;  /* (LOAD only while it is shown) */
    q[6] = (uint32_t)(ui_pk_db[0] > -60.0f ? ui_meter_px(ui_pk_db[0]) : -1) ^ (uint32_t)(ui_pk_db[1] > -60.0f ? ui_meter_px(ui_pk_db[1]) : -1) << 8;
    q[7] = 0;
    h = sf_hash(h, q, sizeof q);
    for (k = 0; k < CY_NVOICES; k++)
        tick[k] = ui_voice_tick(k);
    h = sf_hash(h, tick, sizeof tick);
    return sf_hash(h, &ui_sand_hi, sizeof ui_sand_hi);
}

/* ---- the footer (y 220..239) ---- */
static uint16_t ui_voice_col(uint32_t k)
{
    uint8_t s = ui_tel.voice_state[k];
    if (s == CY_VS_FROZEN)
        return UI_C_ICE;
    if (s == CY_VS_FREE)
        return UI_C_RULE;
    if ((s == CY_VS_HELD || s == CY_VS_BOWED) && ui_tel.voice_press[k] > 8u)
        return UI_C_MAGENTA;
    return s == CY_VS_RING ? UI_C_INK_DIM : UI_C_INK;
}

/* "+1", "-2" (the octave), "T+5", "T-3" (the transpose: TUNED only; SURFACE plays the gong's own points); with a
 * prefix, zero is "OCT 0", "T 0" (a flashed badge only: a plain badge at zero is not shown) */
static void ui_signed(char *t, const char *pre, int v)
{
    uint32_t n = sf_strlen(pre);
    sf_strcpy(t, pre, 8);
    t[n] = v < 0 ? '-' : v == 0 && n ? ' ' : '+';
    sf_utoa(t + n + 1, (uint32_t)(v < 0 ? -v : v), 1);
}

/* the footer: the exciter glyph; the preset's number and its name, always in the value face (a name that would run
 * into the badges fades out at their edge instead of changing face); the badges, right-aligned before the voices:
 * the states (FRZ ice, LAT, DMP clip) and the pitch (octave, transpose), all at once; the voice dots or SURF.
 * OCT and SEL+OCT keep all of it in view: for the 1.3 s of their message the pitch's badge flashes as an ink chip
 * reading OCT+1 (OCT 0 on the way back, T+5 for the transpose), where the message would have covered the name */
static void ui_footer(int force)
{
    const cy_ui_state_t *st = ui_st;
    uint32_t h = 0x811C9DC5u, k, q[3];
    const char *bt[5];
    uint16_t bc[5];
    uint8_t bf[5];
    char num[8], oct[8], trn[8];
    int nb = 0, ov, tr = st->keymode ? 0 : st->transpose, fl = ui_pitch_flash();
    sf_t sf;
    if (st->frozen)
        bt[nb] = "FRZ", bf[nb] = 0, bc[nb++] = UI_C_ICE;
    if (st->latched)
        bt[nb] = "LAT", bf[nb] = 0, bc[nb++] = UI_C_INK;
    if (st->damping)
        bt[nb] = "DMP", bf[nb] = 0, bc[nb++] = UI_C_CLIP;
    if (st->octave || fl == 1) {
        ui_signed(oct, fl == 1 ? "OCT" : "", st->octave);
        bt[nb] = oct, bf[nb] = fl == 1, bc[nb++] = UI_C_INK;
    }
    if (fl == 2)                              /* (SURFACE too: its root moves with the transpose) */
        tr = st->transpose;
    if (tr || fl == 2) {
        ui_signed(trn, "T", tr);
        bt[nb] = trn, bf[nb] = fl == 2, bc[nb++] = UI_C_INK;
    }
    ov = ui_ov_footer(0, &h);
    q[0] = (uint32_t)st->exciter | (uint32_t)st->keymode << 4 | (uint32_t)st->edited << 5 | (uint32_t)st->preset << 8;
    q[1] = (uint32_t)(st->frozen | st->latched << 1 | st->damping << 2) | (uint32_t)(st->octave + 8) << 4 | (uint32_t)ov << 12
         | (uint32_t)(tr + 16) << 16 | (uint32_t)fl << 24;
    q[2] = 0;
    for (k = 0; k < CY_NVOICES; k++)
        q[2] = q[2] * 5u + ui_voice_col(k) % 251u;
    h = sf_hash_s(sf_hash(h, q, sizeof q), st->preset_name);
    if (!force && h == ui_foot_sig)
        return;
    ui_foot_sig = h;
    lcd_sync();
    sf_init(&sf, cv_px, 0, 220, 240, 20);
    sf_rect(&sf, 0, 220, 240, 20, UI_C_SURFACE, 32u);
    sf_rect(&sf, 0, 220, 240, 1, UI_C_RULE, 32u);
    if (ov != 2)                              /* (the USB question puts "USB" there) */
        ui_glyph(&sf, 4, 222, st->exciter, UI_C_AMBER, 32u);
    if (!ui_ov_footer(&sf, &h)) {
        int32_t x, nx, bx = 200, lim, dotw = st->edited ? 8 : 0, i;
        ui_preset_num(num, st->preset);
        sf_text(&sf, 24, 225, &FONT_CY_TERM, num, UI_C_INK_DIM, 32u, 0);
        nx = 24 + sf_text_w(&FONT_CY_TERM, num, 0) + 8;
        for (i = nb - 1; i >= 0; i--) {       /* the badges, right to left, 5 px apart (a flashed one 3 px more) */
            int32_t bw = sf_text_w(&FONT_CY_TERM, bt[i], 0);
            if (bf[i]) {                      /* flashed: ink chip, the footer's colour in it */
                bx -= bw + 3;
                sf_rrect(&sf, bx - 3, 223, bw + 6, 14, 4.0f, bc[i], 32u);
                sf_text(&sf, bx, 225, &FONT_CY_TERM, bt[i], UI_C_SURFACE, 32u, 0);
                bx -= 3 + 5;
            } else {
                bx -= bw;
                sf_text(&sf, bx, 225, &FONT_CY_TERM, bt[i], bc[i], 32u, 0);
                bx -= 5;
            }
        }
        lim = (nb ? bx - 1 : 200) - dotw;     /* where the name must end: too long beside the badges, it is clipped,
                                               * its last 6 px fading out */
        x = ui_foot_text(&sf, nx, 222, &FONT_CY_VAL, st->preset_name, UI_C_INK, lim);
        if (st->edited)                       /* edited: a 4 px amber dot after the name */
            sf_disc(&sf, (float)x + 6.0f, 231.0f, 2.0f, UI_C_AMBER, 32u);
        if (st->keymode) {                    /* SURFACE: a magenta outlined pill instead of the voices */
            sf_rrect(&sf, 203, 224, 36, 12, 6.0f, UI_C_MAGENTA, 32u);
            sf_rrect(&sf, 204, 225, 34, 10, 5.0f, UI_C_SURFACE, 32u);
            sf_text(&sf, 221 - sf_text_w(&FONT_CY_TERM, "SURF", 0) / 2, 224, &FONT_CY_TERM, "SURF", UI_C_MAGENTA, 32u, 0);
        } else {
            for (k = 0; k < CY_NVOICES; k++)  /* 3 x 3 dots, an even 4 px pitch */
                sf_rect(&sf, 205 + 4 * (int32_t)k, 229, 3, 3, ui_voice_col(k), 32u);
        }
    }
    lcd_blit(0, 220, 240, 20, cv_px);
}
