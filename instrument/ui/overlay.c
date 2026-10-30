/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 UI (M5): overlays (spec §6.7, §6.5 footer overlays). Part of the unity build: included by ui/ui.c after
 * gfx2.c, sand.c, plate.c and views.c.
 *
 * In the plate box: the focus pill (a knob was touched: its value large, top-left, where it does not hide the
 * figure the knob is changing), the bottom readout (SHAPE, PRESETS, ALGORITHM; the morph-scrub's pair), the hint
 * line, the tour chips with their chevrons pointing off-screen at the physical control, and the splash's lines (the
 * credit's PRESENT, the logo's tagline). In the footer: messages, the SAVE question, the USB question of the sound
 * transfer (docs/CYMA-1-sounds-spec.md §6.6), the morph scrubber. A question on the footer puts the plate's hint
 * line down; a pitch message (OCTAVE, TRANSPOSE) leaves the footer's name in place and flashes its badge (chrome.c).
 *
 * ui_ov_prepare runs once a frame: it advances the fades (80 ms in, 1.2 s hold, 150 ms out) and lays out the
 * text, and returns a signature of everything visible, so the plate redraws when an overlay changes and not
 * otherwise. The draw functions then only paint, into whichever surface they are given. Nothing overlaps: the hint
 * waits while the tour is up, and a tour chip steps aside for the pills. */
#define UI_SAVE_REPLACE 1                    /* the SAVE dialog draws save_state 3, REPLACE (app.c reads it) */

typedef struct {
    float a;                              /* opacity 0..1 (the fade) */
    uint8_t on, numeric, src, glyph;      /* shown; a number (HERO) or text; focus source (4: the morph pair);
                                           * ALGORITHM's glyph */
    uint16_t hue;
    const felucca_font_t *tf;             /* the text face: MID, or VAL when MID is too wide */
    char label[16], num[24], unit[24];    /* label; the number (or the text) and its unit. The morph pair: A in
                                           * label, "> B" in unit */
    int32_t w, h, y, numw;
} ui_pill_t;
static ui_pill_t ui_fp, ui_bp;            /* focus pill, bottom readout */
static char ui_hint_s[40];
static uint8_t ui_hint_on;
static int8_t ui_hint_mute_p = -1;        /* the hint a footer question put down (-1 none): it stays down after the */
static uint32_t ui_hint_mute_t;           /* question, until a knob is touched again (a new focus_t) */
static uint8_t ui_bp_b;                   /* the morph pair: B is the nearer */
#define UI_BP_Y 190                       /* the bottom readout's top (one line, 24 px) */
#define UI_BP_Y2 172                      /* ... and the morph pair's (two lines, 42 px) */

static const char *ui_hero_chars = " %+-.0123456789:*";
static int ui_is_hero(char c)
{
    const char *p;
    for (p = ui_hero_chars; *p; p++)
        if (*p == c)
            return 1;
    return 0;
}

static int ui_streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static int ui_prefix(const char *s, const char *p)   /* does s start with p? */
{
    while (*p && *s == *p)
        s++, p++;
    return *p == 0;
}

/* an app_say message still up (1.3 s; the app keeps a question up by renewing msg_until every frame) */
static int ui_msg_live(void)
{
    return ui_st->msg[0][0] && (int32_t)(ui_st->msg_until - ui_now) > 0;
}

/* a question from the sound transfer over USB (docs/CYMA-1-sounds-spec.md §6.6): line 1 "USB: SAVE TO U07?",
 * "USB: REPLACE U07?", "USB: DELETE U07?", "USB: SAVE 3 SOUNDS?" or "USB: CHANGE 12 SLOTS?", line 2 always
 * "SAVE=YES HOME=NO", held up by the app until SAVE, HOME or 30 s */
#define UI_USB_ASK "SAVE=YES HOME=NO"
static int ui_usb_question(void)
{
    return ui_msg_live() && ui_prefix(ui_st->msg[0], "USB: ") && ui_streq(ui_st->msg[1], UI_USB_ASK);
}

/* a question on the footer (the SAVE dialog, or the USB one): the plate's hint line steps down for it */
static int ui_asking(void)
{
    return (ui_st->save_state >= 1 && ui_st->save_state <= 3) || ui_usb_question();
}

/* a pitch message (app_say "OCTAVE +1", "TRANSPOSE -3"): 1 octave, 2 transpose, else 0. The footer keeps the
 * preset's number and name and flashes the pitch's badge instead (spec §6.5: the octave and the transpose are the
 * footer's badges; the message over the name would only repeat what the badge says, and hide what is playing) */
static int ui_pitch_flash(void)
{
    if (!ui_msg_live())
        return 0;
    return ui_streq(ui_st->msg[0], "OCTAVE") ? 1 : ui_streq(ui_st->msg[0], "TRANSPOSE") ? 2 : 0;
}

/* the 16 x 16 exciter glyph at (x, y), amber, all of it inside x, y + 0.8 .. 15.2: MALLET (a head on a stick), BOW
 * (a curved stick, its hair and the frog), RAIN (3 drops) */
static void ui_glyph(sf_t *sf, int32_t x, int32_t y, int exc, uint32_t c, uint32_t a)
{
    float fx = (float)x, fy = (float)y;
    if (!sf_hits(sf, x - 1, y - 1, x + 17, y + 17))
        return;
    if (exc == CY_EXC_BOW) {
        int32_t k;
        float px = 0.0f, py = 0.0f;
        for (k = 0; k <= 12; k++) {           /* the stick: an arc bowing up-left, bottom-left to top-right */
            float ang = 3.40f + (4.46f - 3.40f) * (float)k * (1.0f / 12.0f);
            float qx = fx + 19.1f + 18.2f * fm_cosf(ang), qy = fy + 19.1f + 18.2f * fm_sinf(ang);
            if (k)
                sf_capsule(sf, px, py, qx, qy, 0.7f, c, a);
            px = qx, py = qy;
        }
        sf_line(sf, fx + 1.5f, fy + 14.5f, fx + 14.5f, fy + 1.5f, c, (a * 3u) >> 2);   /* the hair, end to end */
        sf_capsule(sf, fx + 1.8f, fy + 12.2f, fx + 3.8f, fy + 14.2f, 1.0f, c, a);       /* the frog */
    } else if (exc == CY_EXC_RAIN) {
        static const float D[3][2] = {{4.0f, 4.0f}, {11.5f, 7.0f}, {6.0f, 12.0f}};
        int32_t k;
        for (k = 0; k < 3; k++) {             /* a drop: a disc with a point on top */
            float cx = fx + D[k][0], cy = fy + D[k][1];
            sf_disc(sf, cx, cy + 1.2f, 2.0f, c, a);
            sf_tri(sf, cx - 1.9f, cy + 0.9f, cx + 1.9f, cy + 0.9f, cx, cy - 3.0f, c, a);
        }
    } else {
        sf_capsule(sf, fx + 3.0f, fy + 14.5f, fx + 10.0f, fy + 6.5f, 0.85f, c, a);   /* the stick */
        sf_disc(sf, fx + 11.5f, fy + 4.5f, 3.6f, c, a);                               /* the head */
    }
}

static void ui_preset_num(char *t, uint32_t p)   /* "07", "U07" */
{
    if (p < CY_NFACTORY)
        sf_utoa(t, p + 1u, 2);
    else {
        t[0] = 'U';
        sf_utoa(t + 1, p - CY_NFACTORY + 1u, 2);
    }
}

/* a morph end's number: the factory preset of that name ("03"), or "U" (a user preset: its name is the pill's) */
static void ui_morph_num(char *t, const char *name)
{
    uint32_t k;
    for (k = 0; k < CY_NFACTORY; k++)
        if (ui_streq(CY_FACTORY[k].name, name)) {
            sf_utoa(t, k + 1u, 2);
            return;
        }
    t[0] = 'U', t[1] = 0;
}

/* a pill's fade toward on: 80 ms in, 150 ms out */
static void ui_fade(float *a, int on)
{
    if (on)
        *a = fm_minf(1.0f, *a + ui_fdt * (1.0f / 0.08f));
    else
        *a = fm_maxf(0.0f, *a - ui_fdt * (1.0f / 0.15f));
    if (ui_shot)                              /* deterministic screenshots: no half-faded frames */
        *a = on ? 1.0f : 0.0f;
}

static int ui_touring(void) { return ui_st->tour_step > CY_TOUR_OFF && ui_st->tour_step < CY_TOUR_DONE; }

/* the chevrons' pulse, 0.5 .. 1 at 1.5 Hz, in 8 steps: a chevron is redrawn when its step changes (~20 times a
 * second), not every frame */
static uint32_t ui_pulse_lv(void)
{
    float p = 0.5f + 0.5f * fm_sinf(6.2831853f * 1.5f * (float)ui_now * 0.001f);   /* 0..1 */
    uint32_t q = (uint32_t)(p * 8.0f);
    return q > 7u ? 7u : q;
}
static float ui_pulse(void) { return 0.5f + (float)ui_pulse_lv() * (0.5f / 7.0f); }

static uint32_t ui_ov_prepare(int *anim)
{
    const cy_ui_state_t *st = ui_st;
    uint32_t h = 0x811C9DC5u, q;
    int recent = st->focus_param >= -1 && ui_now - st->focus_t < 1280u && st->focus_t != 0u;
    int fon = recent && st->focus_src == 0 && st->focus_param >= 0 && st->focus_param < P_NPARAMS;
    int mon = st->morph_active != 0;
    int bon = mon || (recent && st->focus_src >= 1 && st->focus_src <= 3);
    float fa = ui_fp.a, ba = ui_bp.a;
    *anim = 0;
    /* the focus pill */
    ui_fade(&ui_fp.a, fon);
    if (fon) {
        int id = st->focus_param, n = 0, i;
        char buf[32];
        uint8_t pg = CY_PARAMS[id].page;
        ui_fp.on = 1;
        ui_fp.hue = pg < PG_N ? UI_PAGE_HUE[pg] : UI_C_TEAL;
        sf_strcpy(ui_fp.label, CY_PARAMS[id].name, sizeof ui_fp.label);
        cy_param_format(id, st->norm[id], buf, sizeof buf, 1);
        /* M4's rule: a numeric long form starts with one of 0-9 + - . *; its leading run of HERO glyphs is the
         * number, the rest the unit (or a word: "80% PLATE"); anything else is text */
        ui_fp.numeric = (buf[0] >= '0' && buf[0] <= '9') || buf[0] == '+' || buf[0] == '-' || buf[0] == '.' || buf[0] == '*';
        if (ui_fp.numeric) {
            while (buf[n] && ui_is_hero(buf[n]))
                n++;
            for (i = 0; i < n && i < 23; i++)
                ui_fp.num[i] = buf[i];
            while (i > 0 && ui_fp.num[i - 1] == ' ')
                i--;
            ui_fp.num[i] = 0;
            sf_strcpy(ui_fp.unit, buf + n, sizeof ui_fp.unit);
            ui_fp.numw = sf_text_w(&FONT_CY_HERO, ui_fp.num, 0);
            ui_fp.w = ui_fp.numw + (ui_fp.unit[0] ? 3 + sf_text_w(&FONT_CY_VAL, ui_fp.unit, 0) : 0);
            ui_fp.h = 55;                     /* (~7 px over the label's caps and under the numerals' baseline) */
        } else {
            sf_strcpy(ui_fp.num, buf, sizeof ui_fp.num);
            ui_fp.unit[0] = 0;
            ui_fp.tf = &FONT_CY_MID;
            ui_fp.w = sf_text_w(&FONT_CY_MID, buf, 0);
            if (ui_fp.w > 160) {              /* "GLASS > STEEL 40%" in MID is the whole box: VAL instead */
                ui_fp.tf = &FONT_CY_VAL;
                ui_fp.w = sf_text_w(&FONT_CY_VAL, buf, 0);
            }
            ui_fp.h = 38;
        }
        i = sf_text_w(&FONT_CY_LBL, ui_fp.label, 1);
        ui_fp.w = (ui_fp.w > i ? ui_fp.w : i) + 16;
        if (ui_fp.w < 70)
            ui_fp.w = 70;
        if (ui_fp.w > 176)
            ui_fp.w = 176;
    }
    if (ui_fp.a <= 0.0f)
        ui_fp.on = 0;
    /* the bottom readout: SHAPE, ALGORITHM, PRESETS; while SEL+PRESETS scrubs, the pair (both names whole, in two
     * lines: the footer has room for their numbers only) */
    ui_fade(&ui_bp.a, bon);
    if (bon) {
        ui_bp.on = 1;
        ui_bp.unit[0] = ui_bp.label[0] = 0;
        ui_bp.glyph = 0;
        ui_bp.h = 24, ui_bp.y = UI_BP_Y;
        if (mon) {
            int32_t wa, wb;
            ui_bp.src = 4;
            ui_bp_b = st->morph_t >= 0.5f;
            sf_strcpy(ui_bp.label, st->morph_a, sizeof ui_bp.label);
            sf_strcpy(ui_bp.unit, "> ", sizeof ui_bp.unit);     /* '>' draws as U+203A */
            sf_strcat(ui_bp.unit, st->morph_b, sizeof ui_bp.unit);
            ui_bp.num[0] = 0;
            wa = sf_text_w(&FONT_CY_VAL, ui_bp.label, 0), wb = sf_text_w(&FONT_CY_VAL, ui_bp.unit, 0);
            ui_bp.w = (wa > wb ? wa : wb) + 24;
            ui_bp.h = 42, ui_bp.y = UI_BP_Y2;
        } else {
            ui_bp.src = st->focus_src;
            ui_bp.glyph = st->focus_src == 2;
            if (st->focus_src == 1)
                cy_param_format(P_SHAPE, st->norm[P_SHAPE], ui_bp.num, sizeof ui_bp.num, 1);
            else if (st->focus_src == 2)
                sf_strcpy(ui_bp.num, cy_engine_exciter_name(st->exciter), sizeof ui_bp.num);
            else {
                ui_preset_num(ui_bp.num, st->preset);
                sf_strcat(ui_bp.num, " ` ", sizeof ui_bp.num);   /* '`' draws as the middle dot in FONT_CY_VAL */
                sf_strcat(ui_bp.num, st->preset_name, sizeof ui_bp.num);
            }
            ui_bp.w = sf_text_w(&FONT_CY_VAL, ui_bp.num, 0) + (ui_bp.glyph ? 22 : 0) + 24;
        }
        if (ui_bp.w > 180)
            ui_bp.w = 180;
    }
    if (ui_bp.a <= 0.0f)
        ui_bp.on = 0;
    /* the hint line: the first touches of a parameter, when the bottom readout and the tour are not up. A question
     * on the footer (SAVE TO U01?, a USB question) puts it down: the hint is about the knob turned before, and it
     * stays down when the question closes, until a knob is touched again */
    if (ui_asking() && st->hint_param >= 0)
        ui_hint_mute_p = st->hint_param, ui_hint_mute_t = st->focus_t;
    else if (ui_hint_mute_p >= 0 && (st->hint_param != ui_hint_mute_p || (st->focus_src == 0 && st->focus_t != ui_hint_mute_t)))
        ui_hint_mute_p = -1;
    ui_hint_on = st->hint_param >= 0 && st->hint_param < P_NPARAMS && !ui_bp.on && !ui_touring() && CY_PARAMS[st->hint_param].hint
              && !ui_asking() && ui_hint_mute_p < 0;
    if (ui_hint_on)
        sf_strcpy(ui_hint_s, CY_PARAMS[st->hint_param].hint, sizeof ui_hint_s);
    /* what is visible, as a signature (the chevron's pulse is not in it: ui.c redraws only its strip for that) */
    q = (uint32_t)(ui_fp.a * 32.0f) | (uint32_t)(ui_bp.a * 32.0f) << 8 | (uint32_t)ui_hint_on << 16
      | (uint32_t)(ui_touring() ? st->tour_step : 0u) << 17 | (uint32_t)ui_bp.src << 21 | (uint32_t)ui_bp_b << 24;
    h = sf_hash(h, &q, sizeof q);
    if (ui_fp.on)
        h = sf_hash_s(sf_hash_s(sf_hash_s(h, ui_fp.label), ui_fp.num), ui_fp.unit);
    if (ui_bp.on)
        h = sf_hash_s(sf_hash_s(sf_hash_s(h, ui_bp.num), ui_bp.label), ui_bp.unit);
    if (ui_hint_on)
        h = sf_hash_s(h, ui_hint_s);
    if (fa != ui_fp.a || ba != ui_bp.a)
        *anim = 1;                            /* fading */
    return h;
}

/* the tour (spec §6.7): chip text, chip top-left (x < 0: centred in the box), chevron y, chevron kind (0 down,
 * 1 left, 2 double left), hue. The left chevrons sit at the physical encoder's height beside the screen (the panel:
 * SELECT level with screen y ~46, ALGORITHM and PRESETS with ~172; the double chevron: PRESETS is the further left) */
static const struct { const char *t; int16_t x, y, cy; uint8_t kind; uint16_t hue; } UI_TOUR[5] = {
    {"PLAY A KEY", -1, 190, 214, 0, UI_C_AMBER},
    {"LEAN IN: PRESS HARDER", -1, 190, 214, 0, UI_C_MAGENTA},
    {"TURN SELECT: SHAPE", 30, 48, 52, 1, UI_C_TEAL},
    {"TURN ALGORITHM: EXCITER", 30, 164, 172, 1, UI_C_AMBER},
    {"PRESETS: 24 OBJECTS", 30, 164, 172, 2, UI_C_VIOLET},
};

/* where this frame's chip goes: its home, out of the pills' way (the focus pill top-left, the bottom readout) */
static int32_t ui_chip_y(uint32_t s)
{
    int32_t y = UI_TOUR[s].y;
    if (s == CY_TOUR_SELECT - 1u && ui_fp.on)  /* under the focus pill; the chevron still points at SELECT */
        y = 40 + ui_fp.h + 6;
    if (ui_bp.on && y + 17 + 4 > ui_bp.y)      /* above the bottom readout */
        y = ui_bp.y - 17 - 4;
    return y;
}

/* the chevrons that sit in the left rail (steps 3..5): drawn by the ladder */
static void ui_ov_rails(sf_t *sf)
{
    uint32_t s = ui_st->tour_step;
    uint32_t a;
    float cy;
    if (s < CY_TOUR_SELECT || s > CY_TOUR_PRESET)
        return;
    a = sf_cov(ui_pulse(), 32u);
    cy = (float)UI_TOUR[s - 1u].cy + 0.5f;
    sf_tri(sf, 1.0f, cy, 8.0f, cy - 5.5f, 8.0f, cy + 5.5f, UI_TOUR[s - 1u].hue, a);
    if (UI_TOUR[s - 1u].kind == 2)
        sf_tri(sf, 8.0f, cy, 15.0f, cy - 5.5f, 15.0f, cy + 5.5f, UI_TOUR[s - 1u].hue, a);
}

static void ui_ov_plate(sf_t *sf, int splash)
{
    const cy_ui_state_t *st = ui_st;
    if (splash) {                             /* PRESENT under the credit, then the tagline under the logo: each in
                                               * through its figure's hold, out as the figure bursts, on one line (a
                                               * cut: the line shown then, as it was, fades out as the burst starts) */
        uint32_t t = ui_now - st->boot_t, tf = t < ui_spl_spr ? t : ui_spl_spr;
        float a = ui_ramp(tf, UI_SPL_CR_HOLD, UI_SPL_CR_HOLD + 300u) * (1.0f - ui_ramp(tf, UI_SPL_BURST, UI_SPL_LOGO));
        const char *s = UI_SPL_PRESENT;
        int32_t track = 3;                    /* (spaced out: the credit's small line) */
        if (tf >= UI_SPL_LOGO) {
            a = ui_ramp(tf, UI_SPL_LG_HOLD, UI_SPL_LG_HOLD + UI_SPL_TAG_IN);
            s = APP_TAGLINE, track = 1;
        }
        a *= 1.0f - ui_ramp(t, ui_spl_spr, ui_spl_spr + 300u);
        if (a > 0.0f && sf_hits(sf, 28, 160, 212, 160 + FONT_CY_LBL.h)) {
            int32_t w = sf_text_w(&FONT_CY_LBL, s, track);
            sf_text(sf, 120 - w / 2, 160, &FONT_CY_LBL, s, UI_C_INK_DIM, sf_cov(a, 32u), track);
        }
        return;
    }
    if (ui_fp.on && sf_hits(sf, 32, 40, 32 + ui_fp.w, 40 + ui_fp.h)) {
        uint32_t a = sf_cov(ui_fp.a, 32u);
        sf_rrect(sf, 32, 40, ui_fp.w, ui_fp.h, 8.0f, UI_C_VOID, sf_cov(0.7f * ui_fp.a, 32u));
        sf_text(sf, 40, 45, &FONT_CY_LBL, ui_fp.label, ui_fp.hue, a, 1);
        if (ui_fp.numeric) {
            sf_text(sf, 40, 56, &FONT_CY_HERO, ui_fp.num, UI_C_INK, a, 0);
            if (ui_fp.unit[0])                /* the unit on the number's baseline */
                sf_text(sf, 40 + ui_fp.numw + 3, 56 + CY_BASE_HERO - CY_BASE_VAL, &FONT_CY_VAL, ui_fp.unit, UI_C_INK_DIM, a, 0);
        } else {
            int32_t base = 74;                /* the text's baseline: 4 px above the pill's bottom */
            sf_text(sf, 40, base - (ui_fp.tf == &FONT_CY_MID ? CY_BASE_MID : CY_BASE_VAL), ui_fp.tf, ui_fp.num, UI_C_INK, a, 0);
        }
    }
    if (ui_bp.on && sf_hits(sf, 120 - ui_bp.w / 2, ui_bp.y, 120 + ui_bp.w / 2 + 1, ui_bp.y + ui_bp.h)) {
        uint32_t a = sf_cov(ui_bp.a, 32u);
        int32_t x = 120 - ui_bp.w / 2, y = ui_bp.y;
        sf_rrect(sf, x, y, ui_bp.w, ui_bp.h, 12.0f, UI_C_VOID, sf_cov(0.7f * ui_bp.a, 32u));
        x += 12;
        if (ui_bp.src == 4) {                 /* the morph pair: the nearer one bright */
            sf_text(sf, x, y + 4, &FONT_CY_VAL, ui_bp.label, ui_bp_b ? UI_C_INK_DIM : UI_C_INK, a, 0);
            sf_text(sf, x, y + 22, &FONT_CY_VAL, ui_bp.unit, ui_bp_b ? UI_C_INK : UI_C_INK_DIM, a, 0);
        } else {
            if (ui_bp.glyph) {
                ui_glyph(sf, x, y + 4, st->exciter, UI_C_AMBER, a);
                x += 22;
            }
            sf_text(sf, x, y + 17 - CY_BASE_VAL, &FONT_CY_VAL, ui_bp.num, UI_C_INK, a, 0);
        }
    }
    if (ui_hint_on && sf_hits(sf, 28, 204, 212, 217)) {
        int32_t w = sf_text_w(&FONT_CY_LBL, ui_hint_s, 0);
        sf_rect(sf, 28, 204, 184, 13, UI_C_VOID, 22u);
        sf_text(sf, 120 - w / 2, 204, &FONT_CY_LBL, ui_hint_s, UI_C_INK_DIM, 32u, 0);
    }
    if (ui_touring()) {
        uint32_t s = st->tour_step - 1u;
        int32_t w = sf_text_w(&FONT_CY_LBL, UI_TOUR[s].t, 1) + 14, x = UI_TOUR[s].x < 0 ? 120 - w / 2 : UI_TOUR[s].x;
        int32_t y = ui_chip_y(s);
        if (sf_hits(sf, x, y, x + w, y + 17)) {
            sf_rrect(sf, x, y, w, 17, 8.5f, UI_TOUR[s].hue, 30u);
            sf_text(sf, x + 7, y + 2, &FONT_CY_LBL, UI_TOUR[s].t, UI_C_VOID, 32u, 1);
        }
        if (UI_TOUR[s].kind == 0 && !ui_bp.on) {   /* down, at the keybed (it waits while the readout is up) */
            float cy = (float)UI_TOUR[s].cy;
            sf_tri(sf, 113.0f, cy - 4.0f, 127.0f, cy - 4.0f, 120.0f, cy + 4.0f, UI_TOUR[s].hue, sf_cov(ui_pulse(), 32u));
        }
    }
}

/* does this frame show a pulsing chevron on the plate (tour steps 1 and 2)? Its strip is the last one */
static int ui_ov_plate_pulse(void)
{
    return (ui_st->tour_step == CY_TOUR_KEY || ui_st->tour_step == CY_TOUR_LEAN) && !ui_bp.on;
}

/* a footer message's state word by what it means: FREEZE ice, DAMP and failures clip, LATCH ink, OFF and NOTHING
 * CHANGED dim; anything else the page's hue */
static uint16_t ui_msg_col(const char *a, const char *b)
{
    const cy_ui_state_t *st = ui_st;
    if (ui_streq(b, "OFF") || ui_streq(b, "NOTHING CHANGED"))   /* (USB: a question declined or timed out) */
        return UI_C_INK_DIM;
    if (ui_streq(a, "FREEZE"))
        return UI_C_ICE;
    if (ui_streq(a, "DAMP") || ui_streq(a, "PANIC") || ui_streq(a, "SAVE FAILED") || ui_streq(a, "SETTINGS")
        || ui_streq(a, "BANK LOCKED"))
        return UI_C_CLIP;
    if (ui_streq(a, "LATCH"))                 /* (OCTAVE and TRANSPOSE flash their badge instead: ui_pitch_flash) */
        return UI_C_INK;
    return UI_PAGE_HUE[st->page < PG_N ? st->page : 0];
}

/* text from x on the footer's line, clipped at x1 with its last 6 px fading into the footer (a name or a message
 * too long for what is left of the line); returns where it ended */
static int32_t ui_foot_text(sf_t *sf, int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c, int32_t x1)
{
    int32_t i, cx1 = sf->cx1;
    if (x + sf_text_w(f, s, 0) <= x1)
        return sf_text(sf, x, y, f, s, c, 32u, 0);
    sf->cx1 = x1;
    sf_text(sf, x, y, f, s, c, 32u, 0);
    for (i = 0; i < 6; i++)
        sf_rect(sf, x1 - 6 + i, 221, 1, 19, UI_C_SURFACE, (uint32_t)(5 * (i + 1)));
    sf->cx1 = cx1;
    return x1;
}

/* the footer's overlays, right of the exciter glyph: the SAVE question, a USB question, the morph scrubber, a
 * message (not a pitch message: the footer flashes its badge instead). Returns 0 when none is up, 1 when one is
 * (and draws it, if sf), 2 when it is the USB question, which takes the glyph's place too; hashes its content into
 * *h */
static int ui_ov_footer(sf_t *sf, uint32_t *h)
{
    const cy_ui_state_t *st = ui_st;
    char t[40], n[8];
    if (ui_usb_question()) {
        /* "USB: SAVE TO U07?" and "SAVE=YES HOME=NO" on one line: 146 + 6 + 96 px at the longest ("USB: CHANGE 32
         * SLOTS?") would not fit the 214 px right of the glyph, so "USB" stands where the exciter glyph does (the
         * question is the cable's, not the exciter's) and the rest of line 1 follows in the value face, as SAVE's own
         * question does: amber to save into free slots, red when it would replace or delete (as REPLACE is) */
        const char *q = st->msg[0] + 5;
        uint16_t c = ui_prefix(q, "SAVE") ? UI_C_AMBER : UI_C_CLIP;
        /* line 2 as its two words with a 3 px gap, not Terminus's 6 px space: 93 px, so the longest line 1 (CHANGE 32
         * SLOTS?, 114 px from x 24) keeps 7 px clear of it */
        int32_t wy = sf_text_w(&FONT_CY_TERM, "SAVE=YES", 0), w2 = wy + 3 + sf_text_w(&FONT_CY_TERM, "HOME=NO", 0);
        *h = sf_hash_s(sf_hash_s(*h, "USB?"), q);
        if (sf) {
            sf_text(sf, 3, 225, &FONT_CY_TERM, "USB", c, 32u, 0);
            ui_foot_text(sf, 24, 222, &FONT_CY_VAL, q, c, 238 - w2 - 4);
            sf_text(sf, 238 - w2, 225, &FONT_CY_TERM, "SAVE=YES", UI_C_INK_DIM, 32u, 0);
            sf_text(sf, 238 - w2 + wy + 3, 225, &FONT_CY_TERM, "HOME=NO", UI_C_INK_DIM, 32u, 0);
        }
        return 2;
    }
    if (st->save_state >= 1 && st->save_state <= 3) {   /* 3: REPLACE, a slot that holds another preset */
        ui_preset_num(n, st->save_slot);
        sf_strcpy(t, st->save_state == 1 ? "SAVE TO " : st->save_state == 2 ? "SAVE " : "REPLACE ", sizeof t);
        sf_strcat(t, n, sizeof t);
        sf_strcat(t, "?", sizeof t);
        *h = sf_hash_s(*h, t);
        if (st->save_state == 3)
            *h = sf_hash_s(*h, st->msg[1]);
        if (sf) {
            int32_t x = sf_text(sf, 24, 222, &FONT_CY_VAL, t, st->save_state == 3 ? UI_C_CLIP : UI_C_AMBER, 32u, 0);
            if (st->save_state == 3)          /* the preset it replaces: app.c puts its name in msg[1] */
                sf_text(sf, x + 6, 225, &FONT_CY_TERM, st->msg[1], UI_C_INK_DIM, 32u, 0);
            else
                sf_text(sf, 238 - sf_text_w(&FONT_CY_TERM, "SAVE=OK", 0), 225, &FONT_CY_TERM, "SAVE=OK", UI_C_INK_DIM,
                        32u, 0);
        }
        return 1;
    }
    if (st->morph_active) {                   /* 02 --(dot)-- 03 40% (the names are in the readout above) */
        float tt = fm_clampf(st->morph_t, 0.0f, 1.0f);
        int32_t pc = (int32_t)(tt * 100.0f + 0.5f), dx = 46 + (int32_t)(tt * 128.0f + 0.5f);
        *h = sf_hash(sf_hash_s(sf_hash_s(*h, st->morph_a), st->morph_b), &dx, sizeof dx);
        *h = sf_hash(*h, &pc, sizeof pc);
        if (sf) {
            char a[4], b[4];
            ui_morph_num(a, st->morph_a);
            ui_morph_num(b, st->morph_b);
            sf_text(sf, 24, 225, &FONT_CY_TERM, a, tt < 0.5f ? UI_C_INK : UI_C_INK_DIM, 32u, 0);
            sf_rect(sf, 46, 229, 128, 1, UI_C_RULE, 32u);
            sf_rect(sf, 46, 229, dx - 46, 1, UI_C_AMBER, 32u);
            sf_disc(sf, (float)dx, 229.5f, 2.8f, UI_C_AMBER, 32u);
            sf_text(sf, 182, 225, &FONT_CY_TERM, b, tt >= 0.5f ? UI_C_INK : UI_C_INK_DIM, 32u, 0);
            sf_utoa(t, (uint32_t)pc, 1);
            sf_strcat(t, "%", sizeof t);
            sf_text(sf, 238 - sf_text_w(&FONT_CY_TERM, t, 0), 225, &FONT_CY_TERM, t, UI_C_INK, 32u, 0);
        }
        return 1;
    }
    if (ui_msg_live() && !ui_pitch_flash()) {   /* app_say: 1.3 s ("FROM USB" and the sound's name, "SAVED" ...) */
        *h = sf_hash_s(sf_hash_s(*h, st->msg[0]), st->msg[1]);
        if (sf) {                             /* a pair too long for the line (EDITS KEPT and a wide name) fades out */
            int32_t x = ui_foot_text(sf, 24, 222, &FONT_CY_VAL, st->msg[0], UI_C_INK, 238);
            if (st->msg[1][0] && x + 6 < 232)
                ui_foot_text(sf, x + 6, 222, &FONT_CY_VAL, st->msg[1], ui_msg_col(st->msg[0], st->msg[1]), 238);
        }
        return 1;
    }
    return 0;
}
