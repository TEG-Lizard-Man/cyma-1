/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 UI stub (M5): the real UI (instrument/ui/) on a fake engine and a fake object builder, so the screen can
 * be developed, timed and screenshotted on its own, before and apart from the engine. Part of the unity build
 * (host/plat_host.c includes it). Host only.
 *
 * The object builder does the steps of spec §3.2 that the screen reads, from the plate tables (M1's
 * src/plates_data.c, linked as an o2 unit): the shape morph of the fields and the SDF, the cover, the ratios with
 * STIFFNESS, the strike vector, the mics, the keybed points, the rain points. No CARVE, no tracking, no material law.
 *
 * The engine is a bank of decaying mode energies: a strike adds (vel^1.4 phi_k(p) H_k)^2 per mode, with the Hertz
 * contact spectrum H_k of spec §3.6 (hardness from the live parameter), each mode decays with its own T60 (higher
 * modes faster), "bow" holds one degenerate group at a level with the rest faint, "rain" drops small strikes near
 * POS. It posts the same UI events (strike, drop, touch) and fills the same telemetry the real engine does.
 *
 * The panel works as the instrument's: page buttons, SEL for layer B, EDIT for the view, K1..K4 on the page's
 * parameters, SELECT the shape, ALGORITHM the exciter, PRESETS the factory presets (each with its readout and focus
 * timing), keys strike (TUNED at POS, SURFACE at the key's point). Host commands set anything directly:
 *
 *   param NAME VALUE      a parameter in display units (NAME with _ for spaces)
 *   ui FIELD VALUE        page layer exciter keymode view theme reduced frozen latched damping edited octave preset
 *                         grains cpu press tour sprinkle sweep save slot hint (a parameter name, or -1)
 *   focus NAME            a knob touch on parameter NAME (the focus pill)
 *   readout shape|algo|preset   the bottom readout
 *   msg A B               a footer message (1.3 s)
 *   morph T A B | morph off     the morph scrubber
 *   strike U V VEL        a strike at (U, V) % of the plate
 *   bow K DB | bow off    hold mode K's group at DB (dB of energy), the rest 30 dB down
 *   rain on|off           drops near POS
 *   energy K DB           set one mode's energy
 *   decay S | silence     the fake T60 (s); every energy to zero
 *   hold K | unhold K     a key held without striking (SURFACE dots); keypress K P sets its pressure 0..127
 *   perf | perfreset      ui_frame time: median, p99, max (us) over the frames since perfreset, wall (ui_frame_us)
 *   blitmark              remember the blit count (expect blits_since: lcd_blit calls since then)
 *                         and the thread's CPU time
 *   shotmode 0|1          deterministic particles
 * expects: ui_us, ui_max, view, page, moved (grains that moved last frame), loud (loudness x 100), nodal (per-mille of
 * grains on the strongest group's nodal lines), blits (lcd_blit calls so far), blits_since, grains (in play), hop (the grains' mean step
 * last frame, 1/100 px)
 * more commands: sanddump (the sand's groups and weights), sharp X (the sand's contrast exponent) */
#include "fastmath.h"
#include "src/cy_config.h"
#include "src/cy_plates.h"
#include "src/cy_engine.h"
#include "src/cy_params.h"
#include "ui/ui.h"
#include "ui/ui.c"

/* ------------------------------------------------------------------------------------------ object builder --- */
static cy_object_t app_obj;
static cy_fieldcache_t app_fc;
static cy_geom_in_t app_gin;
static uint8_t app_gin_ok;
static const char *const APP_SHN[CY_NSHAPES] = {"DRUM", "CIRCLE", "HEX", "SQUARE", "TRIANGLE", "STAR", "GUITAR", "STADIUM", "BAR"};
static const char *const APP_SHS[CY_NSHAPES] = {"DRM", "CIR", "HEX", "SQR", "TRI", "STR", "GTR", "STD", "BAR"};

const char *cy_geom_shape_name(int s) { return APP_SHN[s < 0 ? 0 : s >= CY_NSHAPES ? CY_NSHAPES - 1 : s]; }
const char *cy_geom_shape_short(int s) { return APP_SHS[s < 0 ? 0 : s >= CY_NSHAPES ? CY_NSHAPES - 1 : s]; }
const cy_object_t *cy_geom_object(void) { return &app_obj; }
const cy_fieldcache_t *cy_geom_fields(void) { return &app_fc; }

float cy_geom_field_at(const cy_fieldcache_t *f, int mode, float u, float v)
{
    float gx = fm_clampf(u * (float)CY_GRID - 0.5f, 0.0f, (float)CY_GRID - 1.001f);
    float gy = fm_clampf(v * (float)CY_GRID - 0.5f, 0.0f, (float)CY_GRID - 1.001f), fx, fy;
    int i = (int)gx, j = (int)gy;
    if (i > CY_GRID - 2) i = CY_GRID - 2;
    if (j > CY_GRID - 2) j = CY_GRID - 2;
    fx = gx - (float)i, fy = gy - (float)j;
    return ((f->field[mode][j][i] * (1.0f - fx) + f->field[mode][j][i + 1] * fx) * (1.0f - fy) +
            (f->field[mode][j + 1][i] * (1.0f - fx) + f->field[mode][j + 1][i + 1] * fx) * fy) * (1.0f / 127.0f);
}

float cy_geom_sdf_at(const cy_fieldcache_t *f, float u, float v)
{
    float gx = fm_clampf(u * (float)CY_SDF - 0.5f, 0.0f, (float)CY_SDF - 1.001f);
    float gy = fm_clampf(v * (float)CY_SDF - 0.5f, 0.0f, (float)CY_SDF - 1.001f), fx, fy;
    int i = (int)gx, j = (int)gy;
    if (i > CY_SDF - 2) i = CY_SDF - 2;
    if (j > CY_SDF - 2) j = CY_SDF - 2;
    fx = gx - (float)i, fy = gy - (float)j;
    return ((f->sdf[j][i] * (1.0f - fx) + f->sdf[j][i + 1] * fx) * (1.0f - fy) +
            (f->sdf[j + 1][i] * (1.0f - fx) + f->sdf[j + 1][i + 1] * fx) * fy) * (1.0f / 128.0f);
}

static void app_pull_in(float *u, float *v)   /* spec §3.2: step 2 % toward the centre while sdf >= -0.03 */
{
    int k;
    for (k = 0; k < 50 && cy_geom_sdf_at(&app_fc, *u, *v) >= -0.03f; k++)
        *u += (0.5f - *u) * 0.02f, *v += (0.5f - *v) * 0.02f;
}

void cy_geom_update(const cy_geom_in_t *in)
{
    int fields = !app_gin_ok || in->shape != app_gin.shape, obj = fields || in->stiffness != app_gin.stiffness
              || in->pos_u != app_gin.pos_u || in->pos_v != app_gin.pos_v || in->mic_spread != app_gin.mic_spread
              || in->rain_spread != app_gin.rain_spread;
    float sh = fm_clampf(in->shape, 0.0f, 8.0f), t, lam[CY_NMODES], mn = 1e30f, s, s1, eps;
    int seg = (int)sh, k, i, j;
    const cy_plate_t *A, *B;
    if (seg > 7)
        seg = 7;
    t = sh - (float)seg;
    A = &CY_PLATES[seg], B = &CY_PLATES[seg + 1];
    app_gin = *in;
    app_gin_ok = 1;
    if (fields) {
        for (k = 0; k < CY_NMODES; k++)
            for (j = 0; j < CY_GRID; j++)
                for (i = 0; i < CY_GRID; i++) {
                    float f = (float)A->field[k][j][i] * (1.0f - t) + (float)B->field[k][j][i] * t;
                    app_fc.field[k][j][i] = (int8_t)(f + (f < 0.0f ? -0.5f : 0.5f));
                }
        for (j = 0; j < CY_SDF; j++)
            for (i = 0; i < CY_SDF; i++) {
                float f = (float)A->sdf[j][i] * (1.0f - t) + (float)B->sdf[j][i] * t;
                app_fc.sdf[j][i] = (int8_t)(f + (f < 0.0f ? -0.5f : 0.5f));
            }
        for (j = 0; j < CY_COVER; j++)        /* cover = clamp(0.5 - d_px / 2) 255 at each 2 px cell */
            for (i = 0; i < CY_COVER; i++) {
                float d = cy_geom_sdf_at(&app_fc, ((float)i + 0.5f) / CY_COVER, ((float)j + 0.5f) / CY_COVER) * 184.0f;
                app_fc.cover[j][i] = (uint8_t)(fm_clampf(0.5f - 0.5f * d, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
        app_fc.seq++;
    }
    if (!obj)
        return;
    for (k = 0; k < CY_NMODES; k++) {         /* ratios: lambda morph, then STIFFNESS (spec §3.2 steps 2, 3) */
        lam[k] = fm_exp2f(A->log2_lambda[k] * (1.0f - t) + B->log2_lambda[k] * t);
        mn = fm_minf(mn, lam[k]);
    }
    s = fm_clampf(in->stiffness, 0.0f, 1.0f);
    s1 = fm_minf(s, 0.8f) / 0.8f;
    eps = s1 * s1 / (1.0f - s1 * s1 + 0.02f);
    for (k = 0; k < CY_NMODES; k++) {
        float L = lam[k] / (mn > 0.0f ? mn : 1.0f), r = fm_sqrtf((L + eps * L * L) / (1.0f + eps));
        if (s > 0.8f)
            r = fm_powf(r, 1.0f + 1.25f * (s - 0.8f));
        app_obj.ratio[k] = r;
        app_obj.log2_ratio[k] = fm_log2f(r);
        app_obj.degen[k] = (t < 0.5f ? A : B)->degen[k];
        app_obj.damp_div[k] = 1.0f;
        app_obj.order[k] = (uint8_t)k;
    }
    for (i = 1; i < CY_NMODES; i++)           /* order: by ratio (insertion sort) */
        for (j = i; j > 0 && app_obj.ratio[app_obj.order[j]] < app_obj.ratio[app_obj.order[j - 1]]; j--) {
            uint8_t x = app_obj.order[j];
            app_obj.order[j] = app_obj.order[j - 1], app_obj.order[j - 1] = x;
        }
    app_obj.pos_uv[0] = fm_clampf(in->pos_u, 0.0f, 1.0f);
    app_obj.pos_uv[1] = fm_clampf(in->pos_v, 0.0f, 1.0f);
    for (k = 0; k < CY_NMODES; k++)
        app_obj.strike[k] = cy_geom_field_at(&app_fc, k, app_obj.pos_uv[0], app_obj.pos_uv[1]);
    for (i = 0; i < 2; i++) {                 /* the mics */
        float u = 0.5f + (i ? 0.33f : -0.33f) * in->mic_spread, v = 0.62f;
        app_pull_in(&u, &v);
        app_obj.mic_uv[i][0] = u, app_obj.mic_uv[i][1] = v;
    }
    {                                         /* the keybed's points (spec §3.2 step 11), panned by POS */
        static const uint8_t W[16] = PLAT_WHITE_KEYS, Bk[11] = PLAT_BLACK_KEYS, AJ[11] = {0, 1, 2, 4, 5, 7, 8, 9, 11, 12, 14};
        for (i = 0; i < 27; i++) {
            float u, v;
            int w = -1, b = -1;
            for (j = 0; j < 16; j++)
                if (W[j] == i) w = j;
            for (j = 0; j < 11; j++)
                if (Bk[j] == i) b = j;
            if (w >= 0)
                u = 0.10f + 0.80f * (float)w / 15.0f, v = 0.66f;
            else
                u = 0.10f + 0.80f * ((float)AJ[b] + 0.5f) / 15.0f, v = 0.36f;
            u += (app_obj.pos_uv[0] - 0.62f) * 0.5f, v += (app_obj.pos_uv[1] - 0.40f) * 0.5f;
            app_pull_in(&u, &v);
            app_obj.key_uv[i][0] = u, app_obj.key_uv[i][1] = v;
            for (k = 0; k < CY_NMODES; k++)
                app_obj.surface[i][k] = cy_geom_field_at(&app_fc, k, u, v);
        }
    }
    {                                         /* rain points: seeded at every build, so rebuilds are identical */
        uint32_t r = 0xA11CEu;
        for (i = 0; i < CY_RAIN_PTS; i++) {
            float rr = 0.45f * in->rain_spread * fm_sqrtf(cy_randf(&r)), a = 6.2831853f * cy_randf(&r);
            float u = app_obj.pos_uv[0] + rr * fm_cosf(a), v = app_obj.pos_uv[1] + rr * fm_sinf(a);
            app_pull_in(&u, &v);
            app_obj.rain_uv[i][0] = u, app_obj.rain_uv[i][1] = v;
        }
    }
    app_obj.clamped = (float)A->clamped * (1.0f - t) + (float)B->clamped * t;
    app_obj.out_norm = 0.12f;
    app_obj.seq++;
}

void cy_geom_init(void)
{
    app_gin_ok = 0;
}

/* --------------------------------------------------------------------------------------------- fake engine --- */
static float app_e[CY_NMODES];
static float app_t60 = 2.5f;
static int8_t app_bow = -1;
static float app_bow_db = -14.0f;
static uint8_t app_rain;
static cy_telemetry_t app_tel;
static cy_uievent_t app_evq[64];
static uint32_t app_ev_w, app_ev_r, app_rng = 0x2F6B9C13u;
static struct { int8_t key; uint8_t state, press; float lvl; uint32_t age; } app_v[CY_NVOICES];
static uint32_t app_age;

const char *cy_engine_exciter_name(int exc)
{
    static const char *const N[CY_NEXC] = {"MALLET", "BOW", "RAIN"};
    return N[exc < 0 ? 0 : exc >= CY_NEXC ? CY_NEXC - 1 : exc];
}
void cy_engine_telemetry_get(cy_telemetry_t *t) { *t = app_tel; }
int cy_engine_event(cy_uievent_t *e)
{
    if (app_ev_r == app_ev_w)
        return 0;
    *e = app_evq[app_ev_r++ % 64u];
    return 1;
}
static void app_post(int type, float u, float v, int vel)
{
    cy_uievent_t *e;
    if (app_ev_w - app_ev_r >= 64u)
        return;
    e = &app_evq[app_ev_w++ % 64u];
    e->type = (uint8_t)type, e->vel = (uint8_t)vel;
    e->u = (uint16_t)(fm_clampf(u, 0.0f, 1.0f) * 65535.0f), e->v = (uint16_t)(fm_clampf(v, 0.0f, 1.0f) * 65535.0f);
}

static float app_norm[P_NPARAMS];
#define APP_V(id) cy_param_value((id), app_norm[(id)])

static float app_hertz(float f, float tau)    /* |cos(pi f tau) / (1 - 4 f^2 tau^2)| (spec §3.6) */
{
    float x = 2.0f * f * tau, d = 1.0f - x * x;
    return fm_fabsf(d) < 1e-3f ? 0.785398f : fm_fabsf(fm_cosf(3.14159265f * f * tau) / d);
}

static void app_strike(float u, float v, int vel, int key, int event)
{
    float hard = APP_V(P_HARDNESS) * 0.01f, tau = 0.008f * fm_powf(0.25f / 8.0f, hard), amp;
    int k, slot = 0;
    uint32_t oldest = 0xFFFFFFFFu;
    tau = fm_maxf(tau * fm_powf((float)vel / 64.0f, -0.2f), 6.0f / 44100.0f);
    amp = 0.1f * fm_powf((float)vel / 127.0f, 1.4f);
    for (k = 0; k < CY_NMODES; k++) {
        float s = cy_geom_field_at(&app_fc, k, u, v), a = amp * s * app_hertz(261.63f * app_obj.ratio[k], tau);
        app_e[k] += a * a;
    }
    for (k = 0; k < CY_NVOICES; k++) {        /* a voice: a free one, else the oldest */
        if (app_v[k].state == CY_VS_FREE) {
            slot = k;
            break;
        }
        if (app_v[k].age < oldest)
            oldest = app_v[k].age, slot = k;
    }
    app_v[slot].key = (int8_t)key, app_v[slot].state = key >= 0 ? CY_VS_HELD : CY_VS_RING;
    app_v[slot].lvl = 1.0f, app_v[slot].age = ++app_age, app_v[slot].press = 0;
    app_tel.newest_hz = 261.63f * fm_exp2f((float)((key >= 0 ? key + 53 : 60) - 60) / 12.0f);
    if (event)
        app_post(CY_UIEV_STRIKE, u, v, vel);
}

/* ---------------------------------------------------------------------------------------------- the stub app --- */
static cy_ui_state_t app_st;
static uint32_t app_last, app_boot_t, app_keys, app_btn;
static uint8_t app_page, app_view, app_frozen, app_latched, app_damping;
static int8_t app_octave;
static uint16_t app_preset;
static uint8_t app_press[CY_NKEYS];
static uint32_t app_hold;                     /* keys held by the 'hold' command */
static uint8_t app_layer_ovr, app_damp_ovr;   /* 'ui layer' / 'ui damping': 1 + the value, over the buttons */
static uint16_t app_gx[SD_MAX], app_gy[SD_MAX];   /* the grains a frame ago: their mean step (expect hop) */
static float app_hop;
static uint32_t app_us[4096], app_cpu[4096], app_nus;   /* ui_frame: wall time (ui_frame_us), thread CPU time */
static uint32_t app_blitmark;                 /* the blit count at the last "blitmark" (expect blits_since) */
static uint8_t app_sticky[P_NPARAMS];

static void app_load_preset(int i)
{
    int k;
    i = (i % CY_NFACTORY + CY_NFACTORY) % CY_NFACTORY;
    cy_preset_load(&CY_FACTORY[i], app_norm);
    app_preset = (uint16_t)i;
    for (k = 0; k < CY_PRESET_NAME && CY_FACTORY[i].name[k]; k++)
        app_st.preset_name[k] = CY_FACTORY[i].name[k];
    app_st.preset_name[k] = 0;
}

void app_init(void)
{
    cy_params_defaults(app_norm);
    cy_geom_init();
    app_load_preset(0);
    app_st.focus_param = -1;
    app_st.hint_param = -1;
}
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t i;
    (void)master_q12;
    for (i = 0; i < 2u * n; i++)
        lr[i] = 0;
}
void app_panic(void) {}
void app_ui_init(void)
{
    app_boot_t = app_last = plat_ms();
    ui_init();
}
void app_splash(void) { ui_splash_static(); }
void app_redraw(void) { ui_redraw(); }
void app_say(const char *a, const char *b)
{
    uint32_t i;
    for (i = 0; i < 23u && a && a[i]; i++)
        app_st.msg[0][i] = a[i];
    app_st.msg[0][i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++)
        app_st.msg[1][i] = b[i];
    app_st.msg[1][i] = 0;
    app_st.msg_until = plat_ms() + 1300u;
}

static void app_input(void)
{
    static const uint8_t PB[PG_N] = {B_HOME, B_ENV, B_FX, B_LFO, B_GLO};
    uint32_t btn = plat_buttons(), down = btn & ~app_btn, keys = plat_keys(), k;
    int32_t e;
    for (k = 0; k < PG_N; k++)
        if (down >> PB[k] & 1u)
            app_page = (uint8_t)k;
    if (down >> B_EDIT & 1u)
        app_view = (uint8_t)((app_view + 1u) % CY_NVIEW);
    if (down >> B_PLAY & 1u)
        app_frozen ^= 1u;
    if (down >> B_ARP & 1u)
        app_latched ^= 1u;
    if (down >> B_SEQ & 1u)
        app_norm[P_KEYMODE] = app_norm[P_KEYMODE] > 0.5f ? 0.0f : 1.0f;
    if (down >> B_OCTUP & 1u && app_octave < 3)
        app_octave++;
    if (down >> B_OCTDN & 1u && app_octave > -3)
        app_octave--;
    app_damping = app_damp_ovr ? (uint8_t)(app_damp_ovr - 1u) : (uint8_t)(btn >> B_REC & 1u);
    app_btn = btn;
    if ((e = plat_enc(EN_SELECT)) != 0) {
        app_norm[P_SHAPE] = cy_param_step(P_SHAPE, app_norm[P_SHAPE], (float)e, &app_sticky[P_SHAPE]);
        app_st.focus_param = P_SHAPE, app_st.focus_src = 1, app_st.focus_t = plat_ms();
    }
    if ((e = plat_enc(EN_ALGO)) != 0) {
        int x = (((int)APP_V(P_EXCITER) + (int)e) % CY_NEXC + CY_NEXC) % CY_NEXC;
        app_norm[P_EXCITER] = (float)x / (float)(CY_NEXC - 1);
        app_st.focus_param = P_EXCITER, app_st.focus_src = 2, app_st.focus_t = plat_ms();
    }
    if ((e = plat_enc(EN_PRESET)) != 0) {
        app_load_preset(app_preset + e);
        app_st.focus_param = -1, app_st.focus_src = 3, app_st.focus_t = plat_ms();
    }
    for (k = 0; k < 4u; k++) {
        int id;
        if ((e = plat_enc((int)(EN_K1 + k))) == 0)
            continue;
        id = cy_page_param(app_page, (int)APP_V(P_EXCITER), app_st.layer, (int)k);
        if (id < 0)
            continue;
        app_norm[id] = cy_param_step(id, app_norm[id], (float)e, &app_sticky[id]);
        app_st.focus_param = (int8_t)id, app_st.focus_src = 0, app_st.focus_t = plat_ms();
    }
    for (k = 0; k < CY_NKEYS; k++) {          /* keys strike: at POS (TUNED) or at the key's point (SURFACE) */
        uint32_t bit = 1u << k;
        if ((keys & bit) && !(app_keys & bit)) {
            if (APP_V(P_KEYMODE) > 0.5f)
                app_strike(app_obj.key_uv[k][0], app_obj.key_uv[k][1], 100, (int)k, 1);
            else if ((int)APP_V(P_EXCITER) == CY_EXC_MALLET)
                app_strike(app_obj.pos_uv[0], app_obj.pos_uv[1], 100, (int)k, 1);
            else if ((int)APP_V(P_EXCITER) == CY_EXC_BOW)
                app_bow = (int8_t)(4 + (k * 7u) % 20u), app_bow_db = -14.0f;
            else
                app_rain = 1;
        } else if (!(keys & bit) && (app_keys & bit)) {
            uint32_t v;
            for (v = 0; v < CY_NVOICES; v++)
                if (app_v[v].key == (int)k && app_v[v].state == CY_VS_HELD)
                    app_v[v].state = CY_VS_RING;
            if ((int)APP_V(P_EXCITER) == CY_EXC_BOW && !(keys & ~bit))
                app_bow = -1;
            if ((int)APP_V(P_EXCITER) == CY_EXC_RAIN && !(keys & ~bit))
                app_rain = 0;
        }
    }
    app_keys = keys;
}
void app_input_only(void) { app_input(); }

static void app_engine(float dt)
{
    float tot = 0.0f, mx = 0.0f;
    uint32_t k, active = 0;
    for (k = 0; k < CY_NMODES; k++) {         /* decay: each mode its own T60, higher ones faster */
        float t60 = app_t60 / (1.0f + 0.12f * (app_obj.ratio[k] - 1.0f));
        if (!app_frozen)
            app_e[k] *= fm_expf(-dt * 13.8155f / fm_maxf(t60, 0.05f) * (app_damping ? 20.0f : 1.0f));
    }
    if (app_bow >= 0) {                       /* bowed: one group held at the level, the rest 30 dB down */
        float eb = fm_exp2f(app_bow_db * 0.33219281f), s2 = 0.0f, a = 1.0f - fm_expf(-dt * 4.0f);
        uint8_t g = app_obj.degen[app_bow];
        for (k = 0; k < CY_NMODES; k++)
            if (app_obj.degen[k] == g)
                s2 += app_obj.strike[k] * app_obj.strike[k];
        for (k = 0; k < CY_NMODES; k++) {
            float tgt = app_obj.degen[k] == g ? (s2 > 1e-6f ? eb * app_obj.strike[k] * app_obj.strike[k] / s2 : eb) : eb * 0.001f;
            app_e[k] += (tgt - app_e[k]) * a;
        }
        app_tel.bow_force = 0.6f;
    } else {
        app_tel.bow_force = 0.0f;
    }
    if (app_rain && cy_randf(&app_rng) < 0.6f) {   /* rain: small strikes near POS */
        uint32_t i = cy_rand(&app_rng) % CY_RAIN_PTS;
        int vel = 30 + (int)(cy_rand(&app_rng) % 50u);
        app_strike(app_obj.rain_uv[i][0], app_obj.rain_uv[i][1], vel, -1, 0);
        app_post(CY_UIEV_DROP, app_obj.rain_uv[i][0], app_obj.rain_uv[i][1], vel);
    }
    for (k = 0; k < CY_NMODES; k++) {
        if (app_e[k] < 1e-14f)
            app_e[k] = 0.0f;
        app_tel.mode_energy[k] = app_e[k];
        tot += app_e[k];
        active += app_e[k] > 1e-9f;
    }
    for (k = 0; k < CY_NVOICES; k++) {
        if (app_v[k].state == CY_VS_FREE)
            continue;
        if (!app_frozen && app_v[k].state != CY_VS_HELD)
            app_v[k].lvl *= fm_expf(-dt * 13.8155f / app_t60);
        if (app_v[k].lvl < 1e-3f)
            app_v[k].state = CY_VS_FREE, app_v[k].key = -1;
        app_tel.voice_state[k] = app_frozen && app_v[k].state != CY_VS_FREE ? CY_VS_FROZEN : app_v[k].state;
        app_tel.voice_key[k] = app_v[k].key;
        app_tel.voice_press[k] = app_v[k].key >= 0 ? app_press[app_v[k].key] : 0;
        app_tel.voice_level[k] = app_v[k].lvl;
    }
    for (k = 0; k < CY_NVOICES; k++)
        if (app_v[k].state == CY_VS_FREE)
            app_tel.voice_state[k] = CY_VS_FREE, app_tel.voice_press[k] = 0;
    for (k = 0; k < CY_NKEYS; k++) {          /* SURFACE: sqrt(sum e_k surface[j][k]^2) / max */
        uint32_t m;
        float s = 0.0f;
        for (m = 0; m < CY_NMODES; m++)
            s += app_e[m] * app_obj.surface[k][m] * app_obj.surface[k][m];
        app_tel.key_level[k] = fm_sqrtf(s);
        mx = fm_maxf(mx, app_tel.key_level[k]);
    }
    for (k = 0; k < CY_NKEYS; k++)
        app_tel.key_level[k] = mx > 0.0f ? app_tel.key_level[k] / mx : 0.0f;
    app_tel.peak_l = fm_minf(1.0f, 1.4f * fm_sqrtf(tot));
    app_tel.peak_r = fm_minf(1.0f, 1.3f * fm_sqrtf(tot));
    app_tel.gr_db = app_tel.peak_l > 0.9f ? 2.0f : 0.0f;
    app_tel.est_instr = 300.0f + 15.0f * (float)active;
    app_tel.modes_used = (uint16_t)active;
    app_tel.mode_budget = CY_BUDGET_DEFAULT;
    app_tel.frozen = app_frozen, app_tel.latched = app_latched, app_tel.damping = app_damping;
    app_tel.surface = (uint8_t)(APP_V(P_KEYMODE) > 0.5f);
    app_tel.seq += 2u;
}

void app_frame(void)
{
    uint32_t now = plat_ms(), k;
    float dt = (float)(now - app_last) * 0.001f, pm = 0.0f;
    cy_geom_in_t g;
    app_last = now;
    app_input();
    app_engine(dt);
    g.shape = APP_V(P_SHAPE), g.stiffness = APP_V(P_STIFFNESS) * 0.01f, g.carve = APP_V(P_CARVE) * 0.01f;
    g.carve_target = (uint8_t)APP_V(P_CARVE_TARGET), g.surface = (uint8_t)(APP_V(P_KEYMODE) > 0.5f), g.rsv[0] = g.rsv[1] = 0;
    g.shimmer_cents = APP_V(P_SHIMMER), g.material = APP_V(P_MATERIAL), g.bright = APP_V(P_BRIGHT) * 0.01f;
    g.pos_u = APP_V(P_POS_X) * 0.01f, g.pos_v = APP_V(P_POS_Y) * 0.01f, g.mic_spread = APP_V(P_MICS) * 0.01f;
    g.rain_spread = APP_V(P_SPREAD) * 0.01f, g.wander = APP_V(P_WANDER) * 0.01f;
    cy_geom_update(&g);
    app_st.now = now;
    app_st.norm = app_norm;
    app_st.page = app_page;
    app_st.layer = app_layer_ovr ? (uint8_t)(app_layer_ovr - 1u) : (uint8_t)(app_btn >> B_SEL & 1u);
    app_st.sel_held = app_st.layer;
    app_st.exciter = (uint8_t)APP_V(P_EXCITER);
    app_st.keymode = (uint8_t)(APP_V(P_KEYMODE) > 0.5f);
    app_st.view = app_view;
    app_st.theme = (uint8_t)APP_V(P_THEME);
    app_st.reduced_motion = (uint8_t)APP_V(P_MOTION);
    app_st.frozen = app_frozen, app_st.latched = app_latched, app_st.damping = app_damping;
    app_st.octave = app_octave;
    app_st.preset = app_preset;
    app_st.grains = (uint16_t)APP_V(P_GRAINS);
    app_st.keys_held = app_keys | app_hold;
    for (k = 0; k < CY_NKEYS; k++) {
        app_st.key_press[k] = (app_st.keys_held >> k & 1u) ? app_press[k] : 0;
        if (app_st.key_press[k] > pm)
            pm = app_st.key_press[k];
    }
    app_st.press_max = pm * (1.0f / 127.0f);
    app_st.boot_t = app_boot_t;
    {                                         /* (CPU time too: on a busy machine the wall time has the OS in it) */
        struct timespec c0, c1;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c0);
        ui_frame(&app_st);
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c1);
        app_cpu[app_nus % 4096u] = (uint32_t)((c1.tv_sec - c0.tv_sec) * 1000000L + (c1.tv_nsec - c0.tv_nsec) / 1000L);
        app_us[app_nus++ % 4096u] = ui_frame_us();
    }
    {                                         /* the grains' mean step this frame, px */
        float sum = 0.0f;
        for (k = 0; k < sd_n; k++) {
            float dx = ((float)sd_g[k].x - (float)app_gx[k]) * (1.0f / 256.0f), dy = ((float)sd_g[k].y - (float)app_gy[k]) * (1.0f / 256.0f);
            sum += fm_sqrtf(dx * dx + dy * dy);
            app_gx[k] = sd_g[k].x, app_gy[k] = sd_g[k].y;
        }
        app_hop = sd_n ? sum / (float)sd_n : 0.0f;
    }
}

static int app_param_id(const char *a)
{
    char name[32];
    int i;
    for (i = 0; a[i] && i < 31; i++)
        name[i] = a[i] == '_' ? ' ' : a[i];
    name[i] = 0;
    for (i = 0; i < P_NPARAMS; i++)
        if (!strcmp(CY_PARAMS[i].name, name))
            return i;
    return -1;
}

static int app_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* per-mille of the grains on the dominant group's nodal lines (|psi| < 0.1): ~100 for an even sprinkle, high for
 * a crisp figure. The sand's own strongest shape (sd_psi[0]), bilinear at each grain */
static int app_nodal(void)
{
    uint32_t i, n = 0;
    if (!sd_nk || !sd_n)
        return 0;
    for (i = 0; i < sd_n; i++) {
        float gx = fm_clampf((float)sd_g[i].x / 256.0f * (24.0f / 184.0f) - 0.5f, 0.0f, 22.999f);
        float gy = fm_clampf((float)sd_g[i].y / 256.0f * (24.0f / 184.0f) - 0.5f, 0.0f, 22.999f), fx, fy, v;
        int x = (int)gx, y = (int)gy;
        const float *P = sd_psi[0];
        fx = gx - (float)x, fy = gy - (float)y;
        v = (P[y * 24 + x] * (1.0f - fx) + P[y * 24 + x + 1] * fx) * (1.0f - fy) + (P[(y + 1) * 24 + x] * (1.0f - fx) + P[(y + 1) * 24 + x + 1] * fx) * fy;
        n += fm_fabsf(v) < 0.1f;
    }
    return (int)(n * 1000u / sd_n);
}

int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "ui_us")) *got = (int)ui_frame_us();
    else if (!strcmp(what, "ui_max")) *got = (int)ui_frame_us_max();
    else if (!strcmp(what, "view")) *got = app_view;
    else if (!strcmp(what, "page")) *got = app_page;
    else if (!strcmp(what, "moved")) *got = sd_moved;
    else if (!strcmp(what, "loud")) *got = (int)(sd_loud * 100.0f);
    else if (!strcmp(what, "nodal")) *got = app_nodal();
    else if (!strcmp(what, "blits")) *got = (int)blits;   /* plat_host's count of lcd_blit calls: 0 fps = no change */
    else if (!strcmp(what, "blits_since")) *got = (int)(blits - app_blitmark);   /* since the last "blitmark" */
    else if (!strcmp(what, "grains")) *got = sd_n;
    else if (!strcmp(what, "hop")) *got = (int)(app_hop * 100.0f);
    else
        return 0;
    return 1;
}

int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d)
{
    uint32_t now = plat_ms();
    (void)d;
    if (!strcmp(cmd, "param")) {
        int id = app_param_id(a);
        if (id < 0) {
            printf("FAIL param %s: no such parameter\n", a);
            exit(2);
        }
        app_norm[id] = cy_param_norm(id, (float)atof(b));
    } else if (!strcmp(cmd, "ui")) {
        int v = atoi(b);
        if (!strcmp(a, "page")) app_page = (uint8_t)v;
        else if (!strcmp(a, "layer")) app_layer_ovr = (uint8_t)(1 + (v != 0));
        else if (!strcmp(a, "exciter")) app_norm[P_EXCITER] = (float)v / (float)(CY_NEXC - 1);
        else if (!strcmp(a, "keymode")) app_norm[P_KEYMODE] = v ? 1.0f : 0.0f;
        else if (!strcmp(a, "view")) app_view = (uint8_t)v;
        else if (!strcmp(a, "theme")) app_norm[P_THEME] = v ? 1.0f : 0.0f;
        else if (!strcmp(a, "reduced")) app_norm[P_MOTION] = v ? 1.0f : 0.0f;
        else if (!strcmp(a, "frozen")) app_frozen = (uint8_t)v;
        else if (!strcmp(a, "latched")) app_latched = (uint8_t)v;
        else if (!strcmp(a, "damping")) app_damp_ovr = (uint8_t)(1 + (v != 0));
        else if (!strcmp(a, "edited")) app_st.edited = (uint8_t)v;
        else if (!strcmp(a, "octave")) app_octave = (int8_t)v;
        else if (!strcmp(a, "preset")) app_load_preset(v - 1);
        else if (!strcmp(a, "grains")) app_norm[P_GRAINS] = cy_param_norm(P_GRAINS, (float)v);
        else if (!strcmp(a, "cpu")) app_st.cpu_pct = (uint8_t)v;
        else if (!strcmp(a, "tour")) app_st.tour_step = (uint8_t)v;
        else if (!strcmp(a, "sprinkle")) app_st.sprinkle_t = now ? now : 1u;
        else if (!strcmp(a, "sweep")) app_st.sweep_t = now ? now : 1u;
        else if (!strcmp(a, "save")) app_st.save_state = (uint8_t)v, app_st.save_slot = (uint16_t)atoi(c);
        else if (!strcmp(a, "hint")) app_st.hint_param = (int8_t)(app_param_id(b) >= 0 ? app_param_id(b) : v);
        else if (!strcmp(a, "press")) { uint32_t k; for (k = 0; k < CY_NKEYS; k++) app_press[k] = (uint8_t)v; }
        else {
            printf("FAIL ui %s: no such field\n", a);
            exit(2);
        }
    } else if (!strcmp(cmd, "focus")) {
        int id = app_param_id(a);
        if (id < 0) {
            printf("FAIL focus %s: no such parameter\n", a);
            exit(2);
        }
        app_st.focus_param = (int8_t)id, app_st.focus_src = 0, app_st.focus_t = now ? now : 1u;
    } else if (!strcmp(cmd, "readout")) {
        app_st.focus_src = (uint8_t)(!strcmp(a, "shape") ? 1 : !strcmp(a, "algo") ? 2 : 3);
        app_st.focus_param = (int8_t)(app_st.focus_src == 1 ? P_SHAPE : app_st.focus_src == 2 ? P_EXCITER : -1);
        app_st.focus_t = now ? now : 1u;
    } else if (!strcmp(cmd, "msg")) {
        char x[24], y[24];
        int i;
        for (i = 0; a[i] && i < 23; i++) x[i] = a[i] == '_' ? ' ' : a[i];
        x[i] = 0;
        for (i = 0; b[i] && i < 23; i++) y[i] = b[i] == '_' ? ' ' : b[i];
        y[i] = 0;
        app_say(x, y);
    } else if (!strcmp(cmd, "morph")) {
        if (!strcmp(a, "off"))
            app_st.morph_active = 0;
        else {
            int i;
            app_st.morph_active = 1, app_st.morph_t = (float)atof(a);
            for (i = 0; b[i] && i < 15; i++) app_st.morph_a[i] = b[i] == '_' ? ' ' : b[i];
            app_st.morph_a[i] = 0;
            for (i = 0; c[i] && i < 15; i++) app_st.morph_b[i] = c[i] == '_' ? ' ' : c[i];
            app_st.morph_b[i] = 0;
        }
    } else if (!strcmp(cmd, "strike")) {
        app_strike((float)atof(a) * 0.01f, (float)atof(b) * 0.01f, c[0] ? atoi(c) : 100, -1, 1);
    } else if (!strcmp(cmd, "bow")) {
        app_bow = !strcmp(a, "off") ? -1 : (int8_t)atoi(a);
        if (b[0])
            app_bow_db = (float)atof(b);
    } else if (!strcmp(cmd, "rain")) {
        app_rain = !strcmp(a, "on");
    } else if (!strcmp(cmd, "energy")) {
        app_e[atoi(a) % CY_NMODES] = fm_exp2f((float)atof(b) * 0.33219281f);
    } else if (!strcmp(cmd, "decay")) {
        app_t60 = fm_maxf(0.05f, (float)atof(a));
    } else if (!strcmp(cmd, "silence")) {
        uint32_t k;
        for (k = 0; k < CY_NMODES; k++)
            app_e[k] = 0.0f;
        app_bow = -1, app_rain = 0;
    } else if (!strcmp(cmd, "hold") || !strcmp(cmd, "unhold")) {
        uint32_t bit = 1u << (atoi(a) % CY_NKEYS);
        app_hold = cmd[0] == 'h' ? app_hold | bit : app_hold & ~bit;
    } else if (!strcmp(cmd, "keypress")) {
        app_press[atoi(a) % CY_NKEYS] = (uint8_t)atoi(b);
    } else if (!strcmp(cmd, "perfreset")) {
        app_nus = 0;
    } else if (!strcmp(cmd, "blitmark")) {
        app_blitmark = (uint32_t)blits;
    } else if (!strcmp(cmd, "perf")) {
        static uint32_t t[4096];
        uint32_t n = app_nus < 4096u ? app_nus : 4096u;
        memcpy(t, app_us, n * sizeof t[0]);
        qsort(t, n, sizeof t[0], app_cmp);
        printf("uiperf frames %u med %u p99 %u max %u us", n, n ? t[n / 2u] : 0u, n ? t[n * 99u / 100u] : 0u, n ? t[n - 1u] : 0u);
        memcpy(t, app_cpu, n * sizeof t[0]);
        qsort(t, n, sizeof t[0], app_cmp);
        printf(" | cpu med %u p99 %u max %u us\n", n ? t[n / 2u] : 0u, n ? t[n * 99u / 100u] : 0u, n ? t[n - 1u] : 0u);
    } else if (!strcmp(cmd, "sanddump")) {    /* the sand's field: its groups and weights */
        uint32_t k, g;
        printf("sand loud %.2f nk %u:", (double)sd_loud, sd_nk);
        for (k = 0; k < sd_nk; k++)
            printf(" w%u=%.4f", k, (double)sd_w[k]);
        printf("\n");
        for (g = 0; g < sd_gn && g < 12u; g++) {
            float e = 0.0f;
            for (k = sd_gs[g]; k < sd_gs[g + 1]; k++)
                e += app_e[sd_gk[k]];
            printf("  group %u modes", g);
            for (k = sd_gs[g]; k < sd_gs[g + 1]; k++)
                printf(" %u", sd_gk[k]);
            printf(" ratio %.3f e %.2e  e*r^4 %.2e\n", (double)sd_gr[g], (double)e, (double)(e * sd_gr[g] * sd_gr[g] * sd_gr[g] * sd_gr[g]));
        }
    } else if (!strcmp(cmd, "sharp")) {
        sd_sharp = (float)atof(a);
    } else if (!strcmp(cmd, "shotmode")) {
        ui_shot_mode(atoi(a));
    } else {
        return 0;
    }
    return 1;
}
