/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 app glue (M7; spec §5, §8.1, §9.7): every control, the presets and the user bank, the preset glide and the
 * morph-scrub, the LFO, the mode budget and its governor, the four-level LEDs, MIDI in and out, the UI state, speech,
 * the settings and their autosave, the host simulator's commands and checks, and the determinism hash.
 *
 * It is the unity part: device/unity.c and host/plat_host.c include it after the platform, so gfx.c, announce.c and
 * lib/touch.c are in scope, and it includes ui/ui.c. The DSP lives in src/ (separate -O2 units, fm1.toml o2) and is
 * reached only through the frozen headers. Statics are prefixed app_, ctl_ or led_ (spec §2.4.4).
 *
 * Who runs what. The platform calls app_render from the audio interrupt (the engine and the host's tap, nothing
 * else), app_frame about 60 times a second and app_input_only in between. Every control is read in app_input, from
 * both, so a key or a detent is acted on within a millisecond of arriving. Everything that runs on a clock (the
 * PRESETS settle, the glide, SEL+SEQ's hold, the tour's timeout, the governor, autosave) runs in app_frame, in §9.7's
 * order.
 *
 * The values. app_norm[] holds the live values, 0..1, for the 40 preset parameters and the 8 globals. A preset
 * parameter also has a target (where the live value settles: the player's edits) and a stored value (the preset as
 * loaded or saved). "Edited" means the target differs from the stored value at the preset's own 16-bit resolution,
 * so turning a knob away and back is not an edit. Knobs edit the target, so a detent always counts from the value
 * the knob means; the glide ramps live from where it was to the target (an edit mid-glide just moves the target);
 * COMPARE swaps live to the stored values and back; the morph-scrub writes live and target together.
 *
 * What the UI is told (cy_ui_state_t, ui/ui.h): focus_src 0 a knob (focus_param its id), 1 SELECT (P_SHAPE), 2
 * ALGORITHM (P_EXCITER), 3 PRESETS (focus_param -1; preset / preset_name are the browsed one while it settles);
 * save_state 1 "SAVE TO Uxx?" (a free slot), 2 "SAVE Uxx?" (this user preset again), 3 "REPLACE Uxx?" (a slot that
 * holds another preset: only when the UI draws it, UI_SAVE_REPLACE; otherwise the footer message says it),
 * save_slot in preset's numbering (24..55); hint_param >= 0 while a hint line is due; tour_step 1..5 the chips,
 * CY_TOUR_DONE for 1.5 s after the last, then OFF; sprinkle_t / sweep_t change at each SPRINKLE / SWEEP; timestamps
 * are never 0 (0 is "never"). The device's frame pacing (§6.9) is ui_frame's own, from cpu_pct.
 *
 * Fixes after the M8 review (fix-app): the governor sheds on a near-miss block and on voices once modes no longer
 * help; a note poll between the plate's strips (UI_POLL); REPLACE for an occupied slot; LFO-driven geometry published
 * at most 20 Hz; REVERT undoable; a browse keeps the edits it left; CC120 / 121 / 122, a separate pedal latch, channel
 * pressure only while MIDI notes are held; SAVE fades the output around its flash stall; the edit buffer survives a
 * power-off (settings v2); the tour in any order. Each is marked "fix-app" where it lives. */
#include "fastmath.h"
#include "src/cy_config.h"
#include "src/cy_plates.h"
#include "src/cy_engine.h"
#include "src/cy_fx.h"                 /* (cy_demo_hash re-inits fx, spec §3.12; the NaN meter for "expect nan") */
#include "src/cy_params.h"
#include "src/cy_xfer.h"               /* (CYMA-1 Sounds: the record, the CRC, pack7, the name rule; app_sounds.c) */
#include "ui/ui.h"
/* CYMA-1 Sounds (docs/CYMA-1-sounds-spec.md §6.4): app_sounds.c, included at the end, holds the SysEx handler and the
 * page's sound accessors; these are the hooks the rest of app.c calls into it */
static int app_sx_asking(void);                /* a USB COMMIT's question is on the screen */
static void app_sx_answer(int yes);            /* the player's answer to it */
static void app_sx_tick(uint32_t now);
static void app_sx_abort(int result);          /* the staging dropped (an update, a restore) */
static void app_sx_written(int rc);            /* SAVE's write of a USB commit is done */
static void app_sx_init(void);
static int app_snd_store(void);                /* the one OBJ_USER write (counted for the host's user_writes) */
#ifdef PLAT_HOST
static int app_sx_expect(const char *what);
static void app_sx_host_cmd(const char *cmd, const char *a);
#endif
/* fix-app: the UI calls UI_POLL() between the plate's strips and before the chrome (when it has the hook), so a key
 * or a MIDI note reaches the engine within a strip's time, not after the whole frame (up to 64 ms on the device) */
static void app_poll_notes(void);
#define UI_POLL() app_poll_notes()
#include "ui/ui.c"

#define APP_NGLOB (P_NPARAMS - P_NPRESET)
#define APP_NPRE (CY_NFACTORY + CY_NUSER)      /* 56: factory 01..24, then user U01..U32 */
#define APP_SET_MAGIC 0x31544553u              /* "SET1": another magic or version loads the defaults */
#define APP_SET_VERSION 2u                     /* 2: + the edit buffer (fix-app); a v1 record is migrated */
#define APP_SET_V1_SIZE 76u                    /* (v1's layout: the fields before edit_preset) */
#define APP_BANK_VERSION 1u                    /* cy_user_bank_t.version this build reads and writes */
#define APP_GLIDE_MS 300u                      /* a preset load's physics-continuous glide (§5.1) */
#define APP_SETTLE_MS 120u                     /* PRESETS: the load waits this long after the last detent */
#define APP_SAY_MS 1300u                       /* a footer message (§6.5) */
#define APP_TOUCH_MS 1500u                     /* a knob left this long: its next turn is a new touch (hints) */
#define APP_HINT_MS 2400u                      /* a hint line stays up this long after the last detent */
#define APP_SWEEP_MS 1000u                     /* SEL+SEQ held this long: SWEEP instead of SPRINKLE */
#define APP_BLINK_MS 600u                      /* SEQ's blink after a sprinkle or a sweep */
#define APP_TALK_MS 350u                       /* a value is said once its knob has rested this long */
#define APP_TOUR_LEAN_MS 6000u                 /* tour step 2 stops waiting for a lean */
#define APP_TOUR_END_MS 1500u                  /* CY_TOUR_DONE shows this long, then the tour is off */
#define APP_QUIET_MS 3000u                     /* autosave: every voice free this long ... */
#define APP_SAVE_GAP_MS 30000u                 /* ... and this long since the last save (a flash write stalls ~50 ms) */
#define APP_SAVE_GAP_PRESET_MS 120000u         /* (only the preset number changed: browsing never wears the flash) */
#define APP_RING_LV 0.00316f                   /* -50 dB: a released key still ringing keeps a glow (§5.5) */
#define APP_SILENT_LV 0.0001f                  /* -80 dBFS on the meters: nothing sounds (autosave may write) */
#define APP_UNDO_MS 5000u                      /* REVERT: SEL+SAVE again within this long, nothing edited since: undo */
#define APP_SAVE_IDLE_MS 15000u                /* the SAVE dialog left untouched this long: cancelled */
#define APP_MUTE_MS 80u                        /* SAVE: the output fades (fx's 10 ms MASTER smoothing) before the write */
#define APP_TOUR_SKIP_MS 8000u                 /* a tour chip with other controls in use this long: skipped */
#define APP_CHORD_BUDGET 64                    /* the chord guard's budget: three onsets together fit under it */
#define APP_SAVE_USB 0xFFu                     /* save_pend_slot: SAVE's write of a USB commit (app_sounds.c) */

/* the settings record (OBJ_SETTINGS, §5.7): the globals, where the player was, the tour and the hints seen; v2 adds
 * the edit buffer (fix-app): the edits of the boot preset (edit_live 1), or the ones a browse left behind (0) */
typedef struct {
    uint32_t magic;
    uint16_t version, preset;                  /* preset: 0..23 factory, 24..55 user */
    uint16_t glob[APP_NGLOB];                  /* P_VEL_CURVE .. P_DETAIL, norm x 65535 */
    int8_t octave;
    uint8_t view, tour_done, tour_boots;       /* (tour_boots was v1's reserved byte, always 0 there) */
    uint8_t hint_count[P_NPARAMS];             /* touches seen per parameter: the hint line shows the first three */
    uint16_t edit_preset;                      /* v2: whose edits; 0xFFFF none */
    uint8_t edit_live, rsv2;
    uint16_t edit[P_NPRESET];                  /* v2: the targets, norm x 65535 */
} app_settings_t;
static app_settings_t app_set;
static cy_user_bank_t app_bank;                /* OBJ_USER: 3,020 B, written whole on each SAVE */
/* the store's limits, checked by the compiler (fix-app): the bank is stored at sector offset 256 and must leave the
 * loader's record bytes 0xF00..0xF07 erased; no object may pass PLAT_STORE_MAX; the settings' layout is fixed (v1 is
 * its first 76 bytes, which the migration relies on). A layout change bumps its version, so an old record is never
 * read as a new one */
typedef char app_assert_bank_fits[sizeof(cy_user_bank_t) <= 0xF00u - 256u ? 1 : -1];
typedef char app_assert_bank_store[sizeof(cy_user_bank_t) <= PLAT_STORE_MAX ? 1 : -1];
typedef char app_assert_settings[sizeof(app_settings_t) == 160u ? 1 : -1];
typedef char app_assert_settings_v1[__builtin_offsetof(app_settings_t, edit_preset) == APP_SET_V1_SIZE ? 1 : -1];

/* the values (the header comment): preset parameters in the first P_NPRESET of each; every array handed to
 * params.c is P_NPARAMS long, so no reading of it can run past the end */
static float app_norm[P_NPARAMS];              /* live */
static float app_target[P_NPARAMS];            /* where live settles: the edits */
static float app_stored[P_NPARAMS];            /* the preset as loaded or saved */
static float app_from[P_NPARAMS];              /* the glide's start */
static float app_ma[P_NPARAMS], app_mb[P_NPARAMS];   /* the morph-scrub's pair */
static uint8_t ctl_sticky[P_NPARAMS];          /* cy_param_step's dwell state, per parameter */

static float app_undo[P_NPRESET];              /* REVERT's undo: the targets it threw away (fix-app) */
static float app_mem[P_NPRESET];               /* a browse's edit memory: the edited preset it left (fix-app) */
static float app_ms[P_NPRESET];                /* the morph-scrub's start: the sound when it began (edits and all) */

static cy_patch_t app_patch;
static cy_geom_in_t app_geom;
static cy_geom_in_t app_geom_pub;              /* what cy_geom_update was last given (the LFO's rate limit) */
static cy_mod_t app_mod;
static cy_telemetry_t app_tel;
static cy_ui_state_t app_ui;
static volatile uint32_t app_overloads;        /* app_overload, from the audio interrupt: the frame acts on it */
static volatile uint32_t app_blk_pk;           /* the heaviest render since the frame looked, per mille of its block */
static volatile uint8_t app_mute;              /* the render's MASTER at 0 (fx fades it in 10 ms): SAVE, an update */

static struct {
    /* where the player is */
    uint8_t page, prev_page, layer, view, keymode;
    int8_t octave, transpose;
    uint8_t frozen, latched, damping, compare, edited;
    uint8_t latch_btn, latch_pedal;            /* ARP's LATCH and the sustain pedal's: the engine has either */
    uint8_t local_off;                         /* CC122 0: the keys send MIDI but do not play (a DAW echoes them) */
    uint16_t preset;                           /* loaded: 0..23 factory, 24..55 user */
    char name[16];
    uint8_t auditioned;                        /* the targets and name are a sound from outside (a link, a file, USB's
                                                * PUT_EDIT), playing as edits of preset: SAVE offers a free slot */
    /* PRESETS: the name at once, the load APP_SETTLE_MS after the last detent */
    uint8_t browsing;
    uint16_t browse;
    uint32_t browse_t;
    /* the glide of a preset load */
    uint8_t gliding;
    uint32_t glide_t;
    /* the SAVE dialog: 1 "SAVE TO Uxx?" (a free slot), 2 "SAVE Uxx?" (this user preset again), 3 "REPLACE Uxx?" */
    uint8_t save_state;
    uint16_t save_slot;                        /* 0..31: U01..U32 */
    uint32_t save_t;                           /* the dialog's last touch (it closes itself after APP_SAVE_IDLE_MS) */
    uint8_t save_pending, bank_locked;         /* a confirmed SAVE waiting for the fade; the bank is another format's */
    uint8_t save_hold;                         /* a USB backup streams: nothing is written meanwhile */
    uint8_t bank_crc_ok;                       /* CYMA-1 Sounds: bank_crc is app_bank's (GET_DIR, app_sounds.c);
                                                * whatever changes app_bank clears it */
    uint16_t save_pend_slot;
    uint32_t save_pend_t;
    /* REVERT's undo and the browse's edit memory (fix-app) */
    uint8_t undo_ok, mem_ok;
    uint16_t undo_preset, mem_preset;
    uint32_t undo_t;
    uint32_t edit_sig;                         /* the edits' fingerprint: a change dirties the settings (edit buffer) */
    /* SEL+PRESETS morph-scrub */
    uint8_t morphing;
    int8_t morph_dir;
    uint16_t morph_ia, morph_ib, morph_is;     /* the pair, and where the scrub began */
    uint8_t morph_hops;                        /* pairs moved on from the start: the way back */
    float morph_t;
    char morph_a[16], morph_b[16], morph_s[16];
    /* SEL+SEQ (tap or hold), the OCT pair (PANIC undoes the first one's step) */
    uint8_t seq_combo, seq_swept;
    uint32_t seq_t, sprinkle_t, sweep_t, sprinkles, sweeps;
    int8_t oct_was, trans_was;
    /* the keys and MIDI */
    uint32_t keys, btn;                        /* held at the last input */
    uint8_t vel[NKEYS], press[NKEYS];
    int8_t note[NKEYS];                        /* the note each held key sounds; -1 none (an octave change mid-note
                                                * still releases the note it started) */
    int8_t midi_key[128];                      /* a MIDI note held: -1 in TUNED, its point 0..26 in SURFACE; -2 not */
    uint8_t midi_press[NKEYS], midi_at;        /* MIDI pressure on SURFACE points; the last pressure from MIDI */
    uint8_t midi_held;                         /* MIDI notes held (channel pressure reaches the engine only then) */
    uint8_t midi_cp, cp_sent;                  /* the last channel pressure / CC1 in; what the engine was given */
    uint8_t press_out[NKEYS];                  /* cy_key_pressure: the keys' and MIDI's */
    uint8_t out_n[128];                        /* MIDI out: how many keys sound each note number (one on, one off) */
    uint8_t at_out[NKEYS];                     /* MIDI out: the poly aftertouch last sent per key */
    uint32_t at_t[NKEYS];
    float bend;
    /* the screen's transient state */
    int8_t focus_param, hint_param, touch_param;
    uint8_t focus_src;
    uint32_t focus_t, hint_until, touch_t;
    char msg[2][24];
    uint32_t msg_until;
    /* speech: the newest line for the page's live region (cy_info_json), and a knob's value waiting to settle */
    char speech[64], later[64];
    uint32_t say_n, later_t;
    uint8_t later_set;
    int8_t later_page;                         /* the page the waiting line names (SEL joins it: "EXCITE, LAYER B") */
    uint8_t tour, tour_mask, tour_other;       /* the chip; the steps done (bit = step); other controls used since */
    uint32_t tour_t;
    uint32_t boot_t, last_frame;
    /* modulation */
    float lfo_phase, lfo_state[4], orbit;      /* (four floats of state: params.c's LFO may keep more than two) */
    uint32_t lfo_rng;
    uint32_t enc_t[NE];                        /* the last detent of each encoder: acceleration */
    int8_t enc_dir[NE];                        /* and its direction: a reversal is never accelerated */
    /* the geometry's publishing (the LFO's rate limit) */
    float geo_shape, geo_u, geo_v;             /* the unmodulated SHAPE / POS last published */
    uint32_t geo_t, geo_pubs;
    uint8_t geo_ok;
    /* the mode budget and its governor */
    uint16_t budget;
    uint8_t gov_voices;                        /* the polyphony the governor allows (8 = VOICES alone) */
    uint32_t gov_t, gov_low_t, ovl_seen, ovl_t;
    uint32_t pk_hold, pk_hold_t;               /* the heaviest block lately (per mille), held 8 s */
    uint32_t chord_t;                          /* the chord guard: note-ons in the last 10 ms */
    uint8_t chord_n;
    /* settings autosave */
    uint8_t set_dirty;                         /* bit 0 anything, bit 1 more than the preset number */
    uint8_t set_fails;
    uint32_t set_saved_t, quiet_t, set_saves;
    uint32_t bank_crc;                         /* CYMA-1 Sounds: GET_DIR's CRC of app_bank, while bank_crc_ok */
#ifdef PLAT_WEB
    uint8_t ting;                              /* the boot's soft C4 (§6.7): 1 struck, 2 released */
#endif
} app;
static uint8_t led_lv[42];                     /* §5.5: keys 0..26, buttons 27..40 (B_*), [41] PLAY green; 0..3 */
static uint8_t led_surf[NKEYS];                /* the SURFACE map, with its hysteresis */
static uint8_t led_sent[42], led_ok;           /* what the panel was last given */
static float led_vpk[CY_NVOICES];              /* each voice's peak since its note began (MIDI notes' ring) */
static uint8_t led_vnote[CY_NVOICES];

static const uint8_t ctl_page_btn[PG_N] = {B_HOME, B_ENV, B_FX, B_LFO, B_GLO};
static const char *const app_view_name[CY_NVIEW] = {"SAND", "FIELD", "MESH"};

/* ------------------------------------------------------------------ text --- */
/* (no libc on the device: by hand, always terminated, never past max) */
static void app_cpy(char *d, const char *s, uint32_t max)
{
    uint32_t n = 0;
    while (s && s[n] && n < max - 1u) {
        d[n] = s[n];
        n++;
    }
    d[n] = 0;
}
static void app_cat(char *d, const char *s, uint32_t max)
{
    uint32_t n = 0;
    while (d[n] && n < max - 1u)
        n++;
    while (s && *s && n < max - 1u)
        d[n++] = *s++;
    d[n] = 0;
}
static void app_cat_int(char *d, int32_t v, uint32_t max)
{
    char t[12], o[13];
    int n = 0, k = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        o[k++] = '-';
    while (n)
        o[k++] = t[--n];
    o[k] = 0;
    app_cat(d, o, max);
}
static void app_cat_signed(char *d, int32_t v, uint32_t max)   /* "+1", "0", "-3" */
{
    if (v > 0)
        app_cat(d, "+", max);
    app_cat_int(d, v, max);
}
static void app_preset_num(char *d, int idx, uint32_t max)     /* "07", "U07" */
{
    int n = idx < CY_NFACTORY ? idx + 1 : idx - CY_NFACTORY + 1;
    d[0] = 0;
    if (idx >= CY_NFACTORY)
        app_cat(d, "U", max);
    if (n < 10)
        app_cat(d, "0", max);
    app_cat_int(d, n, max);
}

/* ---------------------------------------------------------------- values --- */
static float app_v(int id) { return cy_param_value(id, app_norm[id]); }
static int app_is_enum(int id) { return CY_PARAMS[id].curve == CY_CV_ENUM; }
/* an ENUM's index, round(n (N - 1)) (§4), from the norm: params.c may show it in display units (VOICES 4/6/8,
 * DETAIL 64/96/128) */
static int app_enum(int id)
{
    int n = CY_PARAMS[id].detents;
    return n > 1 ? (int)(fm_clampf(app_norm[id], 0.0f, 1.0f) * (float)(n - 1) + 0.5f) : 0;
}
static float app_enum_norm(int id, int i)
{
    int n = CY_PARAMS[id].detents;
    return n > 1 ? fm_clampf((float)i / (float)(n - 1), 0.0f, 1.0f) : 0.0f;
}
static int app_exciter(void) { return app_enum(P_EXCITER); }
static int app_keymode(void) { return app_enum(P_KEYMODE); }
static uint32_t app_q16(float n) { return (uint32_t)(fm_clampf(n, 0.0f, 1.0f) * 65535.0f + 0.5f); }
static int app_detail_modes(void)
{
    static const uint8_t D[3] = {64, 96, 128};
    int d = app_enum(P_DETAIL);
    return D[d < 0 ? 0 : d > 2 ? 2 : d];
}

/* a parameter's detent grid across norm 0..1, as cy_param_step walks it (§4): ENUM N - 1 steps, MORPH detents a
 * segment, LIN and LOG detents */
static int app_grid(int id)
{
    const cy_param_def_t *d = &CY_PARAMS[id];
    if (d->curve == CY_CV_ENUM)
        return d->detents > 1 ? d->detents - 1 : 1;
    if (d->curve == CY_CV_MORPH)
        return d->detents * (int)(d->hi - d->lo + 0.5f);
    return d->detents ? d->detents : 1;
}
/* a stored preset value is 16 bits, and 1/8 (SHAPE CIRCLE) has no 16-bit form: a value that stores as the same 16
 * bits as a grid point IS that point. Without this a keyframe would sit 1/65535 off its grid, its dwell would be
 * skipped going up and cost a silent detent going down. Below the preset's own resolution, so inaudible, and
 * "edited" is unchanged (the same 16 bits) */
static float app_ongrid(int id, float n)
{
    float D = (float)app_grid(id), g = fm_floorf(n * D + 0.5f) / D;
    return app_q16(g) == app_q16(n) ? g : n;
}

static void app_budget_set(int n)
{
    n = n < CY_BUDGET_MIN ? CY_BUDGET_MIN : n > CY_BUDGET_MAX ? CY_BUDGET_MAX : n;
    if (n != app.budget) {
        app.budget = (uint16_t)n;
        cy_engine_set_mode_budget(n);
    }
}

static void app_dirty(void) { app.set_dirty = 3; }
static void app_dirty_preset(void) { app.set_dirty |= 1u; }   /* (the preset number alone: the longer gap) */

/* a global changed: what it drives outside the patch (VOICES and TUNE reach the engine through cy_patch_build) */
static void app_global_changed(int id)
{
    app_dirty();
    if (id == P_VEL_CURVE)
        touch_curve((int)fm_floorf(app_v(P_VEL_CURVE) + 0.5f));
    else if (id == P_PRESS_SENS)
        touch_sens((int)fm_floorf(app_v(P_PRESS_SENS) + 0.5f));
    else if (id == P_DETAIL)
        app_budget_set(app_detail_modes());   /* the ceiling the governor climbs to; the player asked for it now */
}

/* one parameter set by the player (a knob, SELECT, ALGORITHM, SEQ, the host's "param"). Edits are of the target,
 * the value the knob means: during a preset's glide the parameter glides on to its new target (no jump; an enum
 * switches at once), and the next detent counts from there, not from wherever the ramp happened to be */
static void app_set_param(int id, float n)
{
    n = fm_clampf(n, 0.0f, 1.0f);
    if (id < 0 || id >= P_NPARAMS)
        return;
    if (id >= P_NPRESET) {
        if (app_norm[id] != n) {
            app_norm[id] = n;
            app_global_changed(id);
        }
        return;
    }
    app_target[id] = n;
    if (!app.gliding || app_is_enum(id))
        app_norm[id] = app_from[id] = n;
}
static float app_edit_value(int id) { return id < P_NPRESET ? app_target[id] : app_norm[id]; }

static int app_is_edited(void)
{
    int i;
    for (i = 0; i < P_NPRESET; i++)
        if (app_q16(app_target[i]) != app_q16(app_stored[i]))
            return 1;
    return 0;
}

/* ----------------------------------------------------------------- notes --- */
static void ctl_midi_note(uint32_t st, int32_t note, uint32_t vel)   /* USB-MIDI out, channel 1 */
{
    plat_midi_send((st >> 4) | st << 8 | (uint32_t)note << 16 | vel << 24, TO_USB);
}
/* a key's note on / off out (fix-app): two keys can sound one note number after an octave change; the DAW hears one
 * note-on (the first) and one note-off (the last), never a note-off that ends a note another key still holds */
static void ctl_out_on(int32_t note, uint32_t vel)
{
    note &= 127;
    if (app.out_n[note]++ == 0u)
        ctl_midi_note(0x90u, note, vel);
}
static void ctl_out_off(int32_t note)
{
    note &= 127;
    if (app.out_n[note] && --app.out_n[note] == 0u)
        ctl_midi_note(0x80u, note, 0);
}

/* channel pressure / CC1 reach the engine only while a MIDI note is held (fix-app): a mod wheel or a DAW's CC1 left
 * up never bends or palms the panel's own keys, and the last MIDI note's release puts it back to 0 */
static void ctl_cp_apply(void)
{
    uint8_t v = app.midi_held ? app.midi_cp : 0u;
    if (v != app.cp_sent) {
        app.cp_sent = v;
        cy_engine_channel_pressure((int)v);
    }
}

/* every note let go: the keys' (with their note-offs out) and MIDI's; nothing physically held sounds again until
 * it is pressed again (a mode change, PANIC) */
static void ctl_release_all(void)
{
    int k;
    for (k = 0; k < NKEYS; k++) {
        if (app.note[k] >= 0) {
            if (!app.local_off)
                cy_engine_note_off(k, app.note[k]);
            ctl_out_off(app.note[k]);
        }
        app.note[k] = -1;
        app.press[k] = 0;
        app.midi_press[k] = 0;
    }
    for (k = 0; k < 128; k++)
        if (app.midi_key[k] != -2) {
            cy_engine_note_off(app.midi_key[k], k);
            app.midi_key[k] = -2;
        }
    app.midi_held = 0;
    ctl_cp_apply();
}

/* KEYS changed (SEQ, a preset, COMPARE, the morph): what was held is let go in the old mode's terms, before the
 * patch tells the engine the new one. TUNED slots still sounding ring out (§3.7). KEYS or the EXCITER changed: the
 * engine hears the new patch now, not at the next frame, so a key struck a moment after SEQ or ALGORITHM is played in
 * the mode the screen already shows (fix-app) */
static void app_patch_now(void);
static uint8_t app_exc_seen;
static void ctl_keymode_watch(void)
{
    int km = app_keymode(), ex = app_exciter();
    if (km != app.keymode) {
        ctl_release_all();
        app.keymode = (uint8_t)km;
        app_exc_seen = (uint8_t)ex;
        app_patch_now();
    } else if (ex != app_exc_seen) {
        app_exc_seen = (uint8_t)ex;
        app_patch_now();
    }
}

static int32_t app_key_note(uint32_t k)   /* §5.4: F3 + key, the octave, and (TUNED only) the transpose */
{
    return clamp(53 + (int32_t)k + 12 * app.octave + (app.keymode ? 0 : app.transpose), 0, 127);
}

/* --------------------------------------------------------------- presets --- */
static int app_user_used(int i)
{
    return i >= 0 && i < CY_NUSER && app_bank.magic == CY_USER_MAGIC && (app_bank.used_mask >> i & 1u);
}

/* preset idx's values (P_NPRESET of them into out) and name (16 B); 0 = no such preset (an empty user slot) */
static int app_preset_values(int idx, float *out, char *name)
{
    const cy_preset_t *p;
    float t[P_NPARAMS];
    int i;
    if (idx >= 0 && idx < CY_NFACTORY)
        p = &CY_FACTORY[idx];
    else if (app_user_used(idx - CY_NFACTORY))
        p = &app_bank.p[idx - CY_NFACTORY];
    else
        return 0;
    for (i = 0; i < P_NPARAMS; i++)            /* (the globals ride along untouched, whatever params.c writes) */
        t[i] = app_norm[i];
    cy_preset_load(p, t);
    for (i = 0; i < P_NPRESET; i++)
        out[i] = app_ongrid(i, fm_clampf(t[i], 0.0f, 1.0f));
    if (name) {
        for (i = 0; i < CY_PRESET_NAME && p->name[i]; i++)   /* (a 14-letter name has no terminator) */
            name[i] = p->name[i];
        name[i] = 0;
    }
    return 1;
}

static int app_preset_next(int cur, int dir)   /* 01..24, U01..U32, the empty user slots skipped, wrapping */
{
    int i;
    for (i = 0; i < APP_NPRE; i++) {
        cur = (cur + (dir < 0 ? APP_NPRE - 1 : 1)) % APP_NPRE;
        if (cur < CY_NFACTORY || app_user_used(cur - CY_NFACTORY))
            return cur;
    }
    return cur;
}

static void app_glide_finish(void)              /* the glide's end, now */
{
    int i;
    if (!app.gliding)
        return;
    for (i = 0; i < P_NPRESET; i++)
        app_norm[i] = app_from[i] = app_target[i];
    app.gliding = 0;
}

/* load preset idx: stored and target at once; live jumps (glide 0: the host's "preset", boot) or ramps over
 * APP_GLIDE_MS with the enums switched at the start (glide 1), so ringing notes glide through (§5.1) */
static int app_preset_load(int idx, int glide)
{
    float v[P_NPARAMS];
    char nm[16];
    int i;
    if (!app_preset_values(idx, v, nm))
        return 0;
    for (i = 0; i < P_NPRESET; i++) {
        app_stored[i] = app_target[i] = v[i];
        if (glide && !app_is_enum(i)) {
            app_from[i] = app_norm[i];
        } else {
            app_norm[i] = app_from[i] = v[i];
        }
    }
    app.gliding = (uint8_t)(glide != 0);
    app.glide_t = plat_ms();
    app_cpy(app.name, nm, sizeof app.name);
    app.preset = (uint16_t)idx;
    app.auditioned = 0;                        /* (the preset's own sound and name now) */
    app.undo_ok = 0;                           /* (REVERT's undo belongs to the sound it reverted) */
    if (app_set.preset != app.preset) {
        app_set.preset = app.preset;
        app_dirty_preset();
    }
    ctl_keymode_watch();
    return 1;
}

/* the targets now, as they are kept (16 bits a value: the preset's own resolution) */
static void app_targets_take(float *d)
{
    int i;
    for (i = 0; i < P_NPRESET; i++)
        d[i] = app_target[i];
}
/* targets given back (REVERT's undo, the edit memory, the edit buffer at boot): they glide there as a load does,
 * enums at once, and stay edits of the preset that is loaded */
static void app_targets_give(const float *s, int glide)
{
    int i;
    for (i = 0; i < P_NPRESET; i++) {
        app_target[i] = app_ongrid(i, fm_clampf(s[i], 0.0f, 1.0f));
        if (glide && !app_is_enum(i))
            app_from[i] = app_norm[i];
        else
            app_norm[i] = app_from[i] = app_target[i];
    }
    app.gliding = (uint8_t)(glide != 0);
    app.glide_t = plat_ms();
    ctl_keymode_watch();
}

static void app_glide_step(uint32_t now)
{
    float t;
    int i;
    if (!app.gliding)
        return;
    t = (float)(now - app.glide_t) * (1.0f / (float)APP_GLIDE_MS);
    if (t >= 1.0f) {
        app_glide_finish();
        return;
    }
    for (i = 0; i < P_NPRESET; i++)
        if (!app_is_enum(i))
            app_norm[i] = app_from[i] + (app_target[i] - app_from[i]) * t;
}

/* ---------------------------------------------------------------- speech --- */
/* the screen's text as a reader hears it: '>' between words is "to" (HEX > SQUARE), '*' is "x" (DECAY *1.6), and a
 * unit right after a number is a word ("+2.0st" is "+2.0 SEMITONES": a reader's voice says "st" as "street") */
static int app_is_digit(char c) { return c >= '0' && c <= '9'; }
static int app_is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static void app_speech_text(char *d, uint32_t max, const char *a, const char *b)
{
    static const char *const UNIT[][2] = {
        {"st", "SEMITONES"}, {"c", "CENTS"}, {"dB", "DECIBELS"}, {"ms", "MILLISECONDS"}, {"Hz", "HERTZ"},
        {"kHz", "KILOHERTZ"}, {"s", "SECONDS"},
    };
    char t[ANN_MAX + 1];                       /* (a and b copied first: d may be a) */
    uint32_t i, n = 0, u;
    t[0] = 0;
    app_cat(t, a, sizeof t);
    if (b && b[0]) {
        app_cat(t, " ", sizeof t);
        app_cat(t, b, sizeof t);
    }
    for (i = 0; t[i] && n < max - 1u; i++) {
        if (t[i] == '>' && i > 0 && t[i - 1] == ' ' && t[i + 1] == ' ') {
            const char *to = "TO";
            while (*to && n < max - 1u)
                d[n++] = *to++;
            continue;
        }
        if (app_is_alpha(t[i]) && i > 0 && app_is_digit(t[i - 1])) {   /* a unit glued to its number */
            for (u = 0; u < sizeof UNIT / sizeof UNIT[0]; u++) {
                uint32_t k = 0;
                while (UNIT[u][0][k] && t[i + k] == UNIT[u][0][k])
                    k++;
                if (!UNIT[u][0][k] && !app_is_alpha(t[i + k])) {
                    const char *w = UNIT[u][1];
                    if (n < max - 1u)
                        d[n++] = ' ';
                    while (*w && n < max - 1u)
                        d[n++] = *w++;
                    i += k - 1u;
                    break;
                }
            }
            if (u < sizeof UNIT / sizeof UNIT[0])
                continue;
        }
        d[n++] = t[i] == '*' ? 'x' : t[i];
    }
    d[n] = 0;
}

static void app_speech_set(const char *s)     /* the page's live region: a new line */
{
    app_cpy(app.speech, s, sizeof app.speech);
    app.say_n++;
}

/* a message: in the footer for 1.3 s (msg[0] ink, msg[1] the page's hue), to a screen reader now, and to the page */
void app_say(const char *a, const char *b)
{
    char t[64];
    app_cpy(app.msg[0], a, sizeof app.msg[0]);
    app_cpy(app.msg[1], b, sizeof app.msg[1]);
    app.msg_until = plat_ms() + APP_SAY_MS;
    app_speech_text(t, sizeof t, a, b);
    app.later_set = 0;                         /* (newer than a value still waiting) */
    app_speech_set(t);
    if (ann_on())                              /* announce.c, fm1-reader */
        ann_say(t, 0, ANN_NOW);
}

/* a value the screen already shows (a knob's focus pill, SELECT's readout): no footer message; the reader and the
 * page hear it once the turning rests, so a fast turn is one sentence, not forty */
static void app_tell(const char *a, const char *b)
{
    char t[64];
    uint32_t now = plat_ms();
    app_speech_text(t, sizeof t, a, b);
    app_cpy(app.later, t, sizeof app.later);
    app.later_t = now;
    app.later_set = 1;
    app.later_page = -1;
    if (ann_on())
        ann_later(t, ANN_NOW, APP_TALK_MS, now);
}

/* a reader that has just started listening (or asks) hears the whole screen */
static void app_speak(uint32_t now)
{
    char t[ANN_MAX + 1], v[32];
    int k, exc = app_exciter();
    if (app.later_set && now - app.later_t >= APP_TALK_MS) {
        app.later_set = 0;
        app_speech_set(app.later);
    }
    if (ann_on() && ann_where()) {
        t[0] = 0;
        app_cat(t, APP_NAME ". ", sizeof t);
        app_preset_num(v, app.preset, sizeof v);
        app_cat(t, v, sizeof t);
        app_cat(t, " ", sizeof t);
        app_cat(t, app.name, sizeof t);
        app_cat(t, app.edited ? ", EDITED. " : ". ", sizeof t);
        app_cat(t, cy_page_name(app.page), sizeof t);
        app_cat(t, app.layer ? ", LAYER B: " : ", LAYER A: ", sizeof t);
        for (k = 0; k < 4; k++) {
            int id = cy_page_param(app.page, exc, app.layer, k);
            if (id < 0)
                continue;
            app_cat(t, CY_PARAMS[id].name, sizeof t);
            app_cat(t, " ", sizeof t);
            cy_param_format(id, app_norm[id], v, sizeof v, 0);
            app_cat(t, v, sizeof t);
            app_cat(t, k < 3 ? ", " : ". ", sizeof t);
        }
        app_cat(t, cy_engine_exciter_name(exc), sizeof t);
        app_cat(t, ". ", sizeof t);
        cy_param_format(P_SHAPE, app_norm[P_SHAPE], v, sizeof v, 1);
        app_cat(t, v, sizeof t);
        app_cat(t, app.keymode ? ". SURFACE" : ". TUNED", sizeof t);
        app_cat(t, ". OCTAVE ", sizeof t);
        app_cat_signed(t, app.octave, sizeof t);
        if (app.transpose) {
            app_cat(t, ". TRANSPOSE ", sizeof t);
            app_cat_signed(t, app.transpose, sizeof t);
        }
        if (app.frozen)
            app_cat(t, ". FROZEN", sizeof t);
        if (app.latched)
            app_cat(t, ". LATCHED", sizeof t);
        if (app.damping)
            app_cat(t, ". DAMP", sizeof t);
        if (app.local_off)
            app_cat(t, ". LOCAL OFF", sizeof t);
        app_speech_text(t, sizeof t, t, 0);
        ann_say(t, 0, ANN_NOW);
    }
    ann_tick(now);
}

/* ------------------------------------------------------------------ tour --- */
/* §6.7: five chips, each pointing at a control. In any order (fix-app): using a chip's control marks its step done
 * whenever it happens, and the chip shown is the first step not yet done, so a player who explores freely is never
 * stuck behind one chip. A chip whose control goes unused while other controls are (three of them, or eight seconds
 * with at least one) is skipped. The tour counts as seen (tour_done) at its end, after any three steps, or at the
 * second boot. SEL+GLO replays it from the start */
static const char *const app_tour_text[CY_TOUR_DONE] = {
    "", "PLAY A KEY", "LEAN IN: PRESS HARDER", "TURN SELECT: SHAPE", "TURN ALGORITHM: EXCITER",
    "PRESETS: 24 OBJECTS",
};
static int app_popcount(uint32_t v)
{
    int n = 0;
    for (; v; v &= v - 1u)
        n++;
    return n;
}
static void ctl_tour_seen(void)
{
    if (!app_set.tour_done) {
        app_set.tour_done = 1;
        app_dirty();
    }
}
static void ctl_tour_show(uint32_t now)       /* the first step not done; said when it changes */
{
    int s;
    if (app.tour == CY_TOUR_OFF || app.tour == CY_TOUR_DONE)
        return;
    for (s = CY_TOUR_KEY; s < CY_TOUR_DONE && (app.tour_mask >> s & 1u); s++)
        ;
    if (s == app.tour)
        return;
    app.tour = (uint8_t)s;
    app.tour_t = now ? now : 1u;
    app.tour_other = 0;
    if (s == CY_TOUR_DONE)
        ctl_tour_seen();
    else
        app_tell("TOUR", app_tour_text[s]);
}
/* a control was used: step (CY_TOUR_KEY .. CY_TOUR_PRESET) is its chip's, 0 any other control */
static void ctl_tour_step(int step)
{
    if (app.tour == CY_TOUR_OFF || app.tour == CY_TOUR_DONE)
        return;
    if (step > CY_TOUR_OFF && step < CY_TOUR_DONE)
        app.tour_mask |= (uint8_t)(1u << step);
    if (step != app.tour && step != CY_TOUR_KEY && step != CY_TOUR_LEAN && app.tour_other < 255u)
        app.tour_other++;                      /* (another control, or another chip's: playing is never "other") */
    if (app_popcount(app.tour_mask & 0x3Eu) >= 3)
        ctl_tour_seen();
    ctl_tour_show(plat_ms());
}
static void ctl_tour_start(uint32_t now, int step)   /* the tour from this step (host "tour N": the steps before done) */
{
    app.tour_mask = (uint8_t)((1u << step) - 2u);   /* (bits 1 .. step - 1) */
    app.tour = (uint8_t)step;
    app.tour_t = now ? now : 1u;
    app.tour_other = 0;
}
static void ctl_tour_tick(uint32_t now)
{
    if (app.tour == CY_TOUR_OFF)
        return;
    if (app.tour != CY_TOUR_DONE) {
        if (!(app.tour_mask >> CY_TOUR_LEAN & 1u) && (app_mod.press_max * 127.0f > 40.0f || app.midi_at > 40u))
            ctl_tour_step(CY_TOUR_LEAN);       /* (a lean, whenever it comes) */
        if (app.tour == CY_TOUR_LEAN && now - app.tour_t >= APP_TOUR_LEAN_MS)
            ctl_tour_step(CY_TOUR_LEAN);
        else if (app.tour < CY_TOUR_DONE &&
                 (app.tour_other >= 3u || (app.tour_other >= 1u && now - app.tour_t >= APP_TOUR_SKIP_MS)))
            ctl_tour_step(app.tour);           /* (its control unused while others are: skipped) */
    }
    if (app.tour == CY_TOUR_DONE && now - app.tour_t >= APP_TOUR_END_MS)
        app.tour = CY_TOUR_OFF;
}

/* ------------------------------------------------------------------ keys --- */
static void app_chord_guard(uint32_t now);
static void ctl_key_down(uint32_t k)
{
    uint32_t v = touch_vel(k);
    int32_t note = app_key_note(k);
    v = v < 1u ? 1u : v > 127u ? 127u : v;
    app.vel[k] = (uint8_t)v;
    app.press[k] = 0;
    app.at_out[k] = 0;
    app.note[k] = (int8_t)note;
    if (!app.local_off) {                      /* (LOCAL OFF: MIDI out and the LEDs only; the DAW plays it back) */
        app_chord_guard(plat_ms());
        cy_engine_note_on((int)k, note, (int)v);   /* SURFACE: the engine strikes (vel >= 24) or touches point k */
    }
    ctl_out_on(note, v);
    ctl_tour_step(CY_TOUR_KEY);
}

static void ctl_key_up(uint32_t k)
{
    if (app.note[k] >= 0) {
        if (!app.local_off)
            cy_engine_note_off((int)k, app.note[k]);
        ctl_out_off(app.note[k]);
    }
    app.note[k] = -1;
    app.press[k] = 0;
}

/* a key's pressure as poly aftertouch out (fix-app): thinned to a change of 2 or more, or the last value after 8 ms,
 * and never into a full USB queue, so the note-offs that follow always have room */
static void ctl_key_at(uint32_t k, uint32_t now)
{
    uint8_t p = app.press[k], o = app.at_out[k];
    int d = (int)p - (int)o;
    if (p == o)
        return;
    if ((d < 2 && d > -2 && p != 0u && now - app.at_t[k] < 8u) || plat_midi_out_room(TO_USB) < 8u)
        return;
    app.at_out[k] = p;
    app.at_t[k] = now;
    ctl_midi_note(0xA0u, app.note[k], p);
}

static void ctl_keys(void)
{
    uint32_t keys, k;
    touch_poll();                              /* the sensed keys' presses, velocities and pressure */
    keys = touch_keys();
    ctl_keymode_watch();
    for (k = 0; k < NKEYS; k++) {
        uint32_t bit = 1u << k;
        if ((keys & bit) && !(app.keys & bit)) {
            ctl_key_down(k);
        } else if (!(keys & bit) && (app.keys & bit)) {
            ctl_key_up(k);
        } else if ((keys & bit) && app.note[k] >= 0) {   /* pressure (keys 0..19): the engine and poly AT out */
            uint8_t p = touch_pressure(k);
            if (p != app.press[k]) {
                app.press[k] = p;
                if (!app.local_off)
                    cy_engine_pressure((int)k, app.note[k], p);
            }
            ctl_key_at(k, plat_ms());          /* (also catches up a value the thinning held back) */
        }
    }
    app.keys = keys;
}

/* --------------------------------------------------------------- buttons --- */
static void ctl_latch_apply(void)              /* the engine's LATCH: ARP's or the pedal's */
{
    uint8_t on = (uint8_t)(app.latch_btn || app.latch_pedal);
    if (on != app.latched) {
        app.latched = on;
        cy_engine_latch(on);
    }
}

/* PANIC (OCT- + OCT+) and MIDI's All Sound Off: every note let go, FREEZE / LATCH / DAMP off, the bend and the
 * channel pressure back to 0, then the engine's 3 ms fade. The panel's PANIC also puts LOCAL back on */
static void ctl_panic_all(int panel, const char *a, const char *b)
{
    ctl_release_all();
    app.frozen = app.damping = 0;
    app.latch_btn = app.latch_pedal = app.latched = 0;
    app.bend = 0.0f;
    app.midi_cp = app.midi_at = 0;
    if (panel)
        app.local_off = 0;
    cy_engine_freeze(0);
    cy_engine_latch(0);
    cy_engine_damp(0);
    cy_engine_bend(0.0f);
    app.cp_sent = 0;
    cy_engine_channel_pressure(0);
    cy_engine_panic();
    app_say(a, b);
}
static void ctl_panic(void) { ctl_panic_all(1, "PANIC", "ALL NOTES OFF"); }

/* ---- the SAVE dialog (fix-app): a slot that holds another preset says REPLACE and the preset's name, so one turn too
 * far is seen before it is confirmed; a full bank starts on U32 as a REPLACE, never silently on U01; the dialog
 * closes itself when left alone; the write fades the output around the flash's stall instead of cutting it */
static void ctl_save_cancel(void) { app.save_state = 0; }

static uint8_t ctl_save_state_of(int slot)    /* 2 this user preset again, 3 another preset's slot, 1 a free one */
{
    if (app.preset >= CY_NFACTORY && !app.auditioned && slot == app.preset - CY_NFACTORY)
        return 2u;                             /* (an auditioned sound is not "this preset again": a REPLACE) */
    return app_user_used(slot) ? 3u : 1u;
}

static void ctl_save_slot_say(void)
{
    char t[8], a[24], nm[16];
    float v[P_NPARAMS];
    app_preset_num(t, CY_NFACTORY + app.save_slot, sizeof t);
    if (app.save_state == 3u) {
        a[0] = 0;
        app_cat(a, "REPLACE ", sizeof a);
        app_cat(a, t, sizeof a);
        if (!app_preset_values(CY_NFACTORY + app.save_slot, v, nm))
            nm[0] = 0;
        app_tell(a, nm);
    } else {
        app_tell(app.save_state == 2u ? "SAVE" : "SAVE TO", t);
    }
}

/* the slot's occupant, for the footer's REPLACE line */
static void app_save_occupant(char *nm)
{
    float v[P_NPARAMS];
    if (!app_preset_values(CY_NFACTORY + app.save_slot, v, nm))
        nm[0] = 0;
}

static int app_sounding(void)                  /* anything to hear: a voice, or the room's tail above -80 dBFS */
{
    int i;
    if (app_tel.peak_l >= APP_SILENT_LV || app_tel.peak_r >= APP_SILENT_LV)
        return 1;
    for (i = 0; i < CY_NVOICES; i++)
        if (app_tel.voice_state[i] != CY_VS_FREE)
            return 1;
    return 0;
}

static void app_settings_save(uint32_t now, int quiet);
static void ctl_save_write(int i, uint32_t now)   /* the bank to the flash (the output already faded, if it sounded) */
{
    char t[8];
    if (i == (int)APP_SAVE_USB) {              /* a USB commit's (app_sounds.c): its own words, and the host told */
        app_sx_written(app_snd_store());
        return;
    }
    app_preset_num(t, CY_NFACTORY + i, sizeof t);
    if (app_snd_store() != 0) {
        app_say("SAVE FAILED", t);             /* (kept in RAM: still playable until power-off) */
    } else {
        app_say("SAVED", t);
        app_settings_save(now, 1);             /* (and the saved slot is the boot preset at once, in the same quiet) */
    }
}

/* SAVE: the first press asks (a factory preset or a sound from outside: the first free user slot; a user preset:
 * itself), the second writes */
static void ctl_browse_land(int glide);
static void ctl_save_service(uint32_t now);
static void ctl_save_to(int i, uint32_t now);
static void ctl_save_press(void)
{
    uint32_t now = plat_ms();
    int i;
    if (app.compare || app.save_pending)
        return;
    if (app.bank_locked) {                     /* (another format's bank: never written over, fix-app) */
        app_say("BANK LOCKED", "NEWER FORMAT");
        return;
    }
    if (!app.save_state) {
        if (app.browsing)                      /* (a browse still settling: the preset whose name is showing) */
            ctl_browse_land(1);
        if (app.preset >= CY_NFACTORY && !app.auditioned) {   /* (an audition never lands on the slot it came
                                                                * from: a double SAVE press cannot replace it) */
            app.save_slot = (uint16_t)(app.preset - CY_NFACTORY);
        } else {
            app.save_slot = CY_NUSER - 1;      /* (a full bank: U32, as a REPLACE) */
            for (i = 0; i < CY_NUSER; i++)
                if (!app_user_used(i)) {
                    app.save_slot = (uint16_t)i;
                    break;
                }
        }
        app.save_state = ctl_save_state_of(app.save_slot);
        app.save_t = now;
        ctl_save_slot_say();
        return;
    }
    ctl_save_to(app.save_slot, now);
}

/* SAVE's write half (the second press; the page's Save, app_sounds.c): the targets (a glide in progress lands where
 * it is going) and the name into user slot i, the slot loaded, then the flash write, faded if anything sounds */
static void ctl_save_to(int i, uint32_t now)
{
    if (app_bank.magic != CY_USER_MAGIC) {
        uint8_t *p = (uint8_t *)&app_bank;
        uint32_t j;
        for (j = 0; j < sizeof app_bank; j++)
            p[j] = 0;
        app_bank.magic = CY_USER_MAGIC;
    }
    app_bank.version = APP_BANK_VERSION;
    cy_preset_store(app_target, app.name, &app_bank.p[i]);
    app_bank.used_mask |= 1u << i;
    app.bank_crc_ok = 0;                       /* (CYMA-1 Sounds: GET_DIR's CRC is computed again) */
    app.save_state = 0;
    if (app.mem_ok && app.mem_preset == CY_NFACTORY + i)   /* (the edits it kept were of what is now replaced) */
        app.mem_ok = 0;
    app_glide_finish();
    app_preset_load(CY_NFACTORY + i, 0);       /* stored = what was written, to the bit */
    app.save_pending = 1;                      /* the flash write: now if nothing sounds, else after the fade */
    app.save_pend_slot = (uint16_t)i;
    ctl_save_service(now);
}

/* a confirmed SAVE's flash write. The erase stalls the audio ~50 ms with the interrupts off (the DMA plays zeros):
 * into a ringing plate that was a cut to digital silence and back, two clicks. So when anything sounds, MASTER
 * fades first (APP_MUTE_MS), then the write, then it fades back in. Held while a USB backup streams (fix-app) */
static void ctl_save_service(uint32_t now)
{
    if (!app.save_pending || app.save_hold)
        return;
    if (app.save_pending == 1u && app_sounding()) {
        char t[8];
        if (app.save_pend_slot == APP_SAVE_USB)   /* (a USB commit's: SAVING USB) */
            app_cpy(t, "USB", sizeof t);
        else
            app_preset_num(t, CY_NFACTORY + app.save_pend_slot, sizeof t);
        app_mute = 1;
        app.save_pend_t = now;
        app.save_pending = 2;
        app_say("SAVING", t);
        return;
    }
    if (app.save_pending == 2u && now - app.save_pend_t < APP_MUTE_MS)
        return;
    app.save_pending = 0;
    ctl_save_write(app.save_pend_slot, now);
    app_mute = 0;
}

/* REVERT (SEL+SAVE): the stored preset again, gliding. Undoable (fix-app): the edits it threw away come back with
 * SEL+SAVE once more within 5 s, so the natural SAVE tap after a layer-B edit costs nothing */
static void ctl_morph_end(void);
static void ctl_revert(uint32_t now)
{
    int keep = 0;
    ctl_morph_end();
    ctl_save_cancel();
    app.browsing = 0;                          /* (and a browse still settling is called off) */
    if (app.compare)
        return;
    if (!app_is_edited() && app.undo_ok && app.undo_preset == app.preset && now - app.undo_t < APP_UNDO_MS) {
        app.undo_ok = 0;
        app_targets_give(app_undo, 1);
        app_say("UNDO", "EDITS BACK");
        return;
    }
    if (app_is_edited()) {
        app_targets_take(app_undo);
        keep = 1;
    }
    app_preset_load(app.preset, 1);
    if (keep) {
        app.undo_ok = 1;
        app.undo_preset = app.preset;
        app.undo_t = now;
        app_say("REVERTED", "SEL+SAVE: UNDO");
    } else {
        app_say("REVERT", app.name);
    }
}

/* COMPARE (SEL+HOME, while HOME is held): the stored preset plays; letting go brings the edits back */
static void ctl_compare(int on)
{
    int i;
    if (on == app.compare)
        return;
    app_glide_finish();
    app.compare = (uint8_t)on;
    for (i = 0; i < P_NPRESET; i++)
        app_norm[i] = on ? app_stored[i] : app_target[i];
    app_say(on ? "COMPARE" : "EDITED", on ? "STORED" : "");
    ctl_keymode_watch();
}

/* a browse lands (the settle, SAVE, the morph): the preset it shows is loaded. The edits of the preset it leaves are
 * kept (one deep), and landing on that preset again brings them back, still edits (fix-app): a brushed PRESETS
 * encoder never costs an hour's tweak. REVERT still gives the stored sound */
static void ctl_browse_land(int glide)
{
    float keep[P_NPRESET];
    int from = app.preset, to = app.browse, had = 0, i;
    app.browsing = 0;
    if (app.compare)
        ctl_compare(0);
    if (to == from && app_is_edited())         /* (out and back before it settled: nothing to load) */
        return;
    if (app.mem_ok && app.mem_preset == to) {
        for (i = 0; i < P_NPRESET; i++)
            keep[i] = app_mem[i];
        had = 1;
        app.mem_ok = 0;
    }
    if (to != from && app_is_edited()) {
        app_targets_take(app_mem);
        app.mem_ok = 1;
        app.mem_preset = (uint16_t)from;
    }
    if (!app_preset_load(to, glide))
        return;
    if (had) {
        app_targets_give(keep, glide);
        app_say("EDITS KEPT", app.name);
    }
}

/* the morph-scrub's end (SEL let go): the preset is whichever of the pair is nearer; edited unless exactly on one */
static void ctl_morph_end(void)
{
    float v[P_NPARAMS];
    char nm[16];
    int idx, i;
    if (!app.morphing)
        return;
    app.morphing = 0;
    idx = app.morph_t < 0.5f ? app.morph_ia : app.morph_ib;
    if (!app_preset_values(idx, v, nm))
        return;
    for (i = 0; i < P_NPRESET; i++)
        app_stored[i] = v[i];
    app_cpy(app.name, nm, sizeof app.name);
    app.preset = (uint16_t)idx;
    app.undo_ok = 0;
    if (app_set.preset != app.preset) {
        app_set.preset = app.preset;
        app_dirty_preset();
    }
}

static void ctl_page(int pg)
{
    if (pg == PG_GLOBAL && app.page == PG_GLOBAL) {   /* GLO again: back where you were */
        pg = app.prev_page;
    } else if (app.page != PG_GLOBAL) {
        app.prev_page = app.page;
    }
    app.page = (uint8_t)pg;
    app_tell(cy_page_name(pg), app.layer ? "LAYER B" : "");
    app.later_page = (int8_t)pg;
}

static void ctl_buttons(void)
{
    uint32_t btn = plat_buttons(), down = btn & ~app.btn, up = app.btn & ~btn, now = plat_ms();
    int sel = (int)(btn >> B_SEL & 1u), k;
    char t[8];
    if ((btn ^ app.btn) >> B_SEL & 1u) {       /* (a page just named: one sentence, "EXCITE LAYER B") */
        if (app.later_set && app.later_page >= 0 && now - app.later_t < APP_TALK_MS) {
            int pg = app.later_page;
            app_tell(cy_page_name(pg), sel ? "LAYER B" : "LAYER A");
            app.later_page = (int8_t)pg;
        } else {
            app_tell(sel ? "LAYER B" : "LAYER A", "");
        }
    }
    app.layer = (uint8_t)sel;
    if ((up >> B_SEL & 1u) && app.morphing)
        ctl_morph_end();
    if (app_sx_asking()) {                     /* a USB question (app_sounds.c): SAVE says yes; a page button, SEL+SAVE
                                                * or PANIC says no; those presses do nothing else (PANIC still panics) */
        uint32_t pages = 1u << B_HOME | 1u << B_ENV | 1u << B_FX | 1u << B_LFO | 1u << B_GLO, ans = 1u << B_SAVE | pages;
        if ((down >> B_SAVE & 1u) && !sel) {
            app_sx_answer(1);
            down &= ~ans;
        } else if (down & ans) {
            app_sx_answer(0);
            down &= ~ans;
        } else if (((down >> B_OCTDN | down >> B_OCTUP) & 1u) && (btn >> B_OCTDN & 1u) && (btn >> B_OCTUP & 1u)) {
            app_sx_answer(0);
        }
    }
    if (down & ~(1u << B_SEL))                 /* (any other button: the tour's "other control") */
        ctl_tour_step(0);
    for (k = 0; k < PG_N; k++) {               /* HOME ENV FX LFO GLO */
        uint32_t b = ctl_page_btn[k];
        if (!(down >> b & 1u))
            continue;
        if (app.save_state) {                  /* any page button cancels the SAVE dialog */
            ctl_save_cancel();
            app_tell("SAVE", "CANCELLED");
        } else if (sel && b == B_HOME) {
            ctl_compare(1);
        } else if (sel && b == B_GLO) {        /* replay the tour */
            ctl_tour_start(now, CY_TOUR_KEY);
            app_say("TOUR", "PLAY A KEY");
        } else {
            ctl_page(k);
        }
    }
    if ((up >> B_HOME & 1u) && app.compare)
        ctl_compare(0);
    /* OCT- / OCT+: the octave (SEL: the transpose); both together PANIC, undoing the first one's step */
    if ((down >> B_OCTDN & 1u) || (down >> B_OCTUP & 1u)) {
        int both = (btn >> B_OCTDN & 1u) && (btn >> B_OCTUP & 1u);
        int d = (down >> B_OCTUP & 1u) ? 1 : -1;
        if (both) {
            if (((down >> B_OCTDN) & (down >> B_OCTUP) & 1u) == 0u) {   /* (one was held: its step undone) */
                app.octave = app.oct_was;
                app.transpose = app.trans_was;
            }
            ctl_panic();
        } else if (sel) {
            app.oct_was = app.octave;
            app.trans_was = app.transpose;
            app.transpose = (int8_t)clamp(app.transpose + d, -12, 12);
            t[0] = 0;
            app_cat_signed(t, app.transpose, sizeof t);
            app_say("TRANSPOSE", t);           /* (SURFACE too: its root moves with it, fix-app) */
        } else {
            app.oct_was = app.octave;
            app.trans_was = app.transpose;
            app.octave = (int8_t)clamp(app.octave + d, -3, 3);
            if (app.octave != app.oct_was)
                app_dirty();
            t[0] = 0;
            app_cat_signed(t, app.octave, sizeof t);
            app_say("OCTAVE", t);
        }
    }
    if (down >> B_EDIT & 1u) {                 /* the view: SAND, FIELD, MESH */
        app.view = (uint8_t)((app.view + 1u) % CY_NVIEW);
        app_dirty();
        app_say(app_view_name[app.view], "");
    }
    if (down >> B_SEQ & 1u) {
        if (sel) {                             /* SEL+SEQ: tap SPRINKLE, hold SWEEP (decided at release or 1 s) */
            app.seq_combo = 1;
            app.seq_swept = 0;
            app.seq_t = now;
        } else if (!app.compare) {             /* KEYS: SURFACE <-> TUNED, an edit of the preset */
            app_set_param(P_KEYMODE, app_enum_norm(P_KEYMODE, !app_keymode()));
            ctl_keymode_watch();
            app_say("KEYS", app.keymode ? "SURFACE" : "TUNED");
        }
    }
    if ((up >> B_SEQ & 1u) && app.seq_combo) {
        if (!app.seq_swept) {
            app.sprinkle_t = now ? now : 1u;
            app.sprinkles++;
            app_say("SPRINKLE", "");
        }
        app.seq_combo = 0;
    }
    if (down >> B_PLAY & 1u) {
        app.frozen ^= 1u;
        cy_engine_freeze(app.frozen);
        app_say("FREEZE", app.frozen ? "ON" : "OFF");
    }
    if (down >> B_ARP & 1u) {                  /* LATCH: ARP's own (the pedal's is separate, fix-app) */
        app.latch_btn ^= 1u;
        ctl_latch_apply();
        app_say("LATCH", app.latch_btn ? "ON" : app.latch_pedal ? "OFF (PEDAL)" : "OFF");
    }
    if ((down | up) >> B_REC & 1u) {           /* DAMP: a palm on the plate while held */
        app.damping = (uint8_t)(btn >> B_REC & 1u);
        cy_engine_damp(app.damping);
        if (app.damping)
            app_say("DAMP", "");
    }
    if (down >> B_SAVE & 1u) {
        if (sel)
            ctl_revert(now);
        else
            ctl_save_press();
    }
    app.btn = btn;
    ctl_keymode_watch();
}

/* -------------------------------------------------------------- encoders --- */
/* detents times the acceleration (§5.1): by the time since the encoder's last detent; the host's "spin" is exact.
 * fix-app: a reversal is never accelerated (the one detent back that corrects an overshoot is one detent), and a
 * batch of detents read together (the device reads them after a long frame) is timed per detent, so the feel does
 * not change with the screen's load */
static int32_t ctl_accel(int role, int32_t e, int32_t mid, int32_t fast)
{
    uint32_t now = plat_ms(), dt = now - app.enc_t[role], n = (uint32_t)(e < 0 ? -e : e);
    int8_t dir = (int8_t)(e < 0 ? -1 : 1), was = app.enc_dir[role];
    app.enc_t[role] = now;
    app.enc_dir[role] = dir;
#ifdef PLAT_HOST
    if (plat_host_exact)
        return e;
#endif
    if (was != dir)
        return e;
    dt /= n ? n : 1u;
    return dt < 15u ? e * fast : dt < 40u ? e * mid : e;
}

/* n single detents of parameter id (each through params.c, so a dwell or a sticky zero counts detents as the hand
 * makes them) */
static float ctl_steps(int id, float v, int32_t n)
{
    int32_t s = n < 0 ? -1 : 1, i;
    for (i = 0; i < (n < 0 ? -n : n) && i < 512; i++)
        v = cy_param_step(id, v, (float)s, &ctl_sticky[id]);
    return v;
}

static uint32_t app_stamp(void) { uint32_t t = plat_ms(); return t ? t : 1u; }   /* (0 means "never" to the UI) */
static void ctl_focus(int id, int src)
{
    app.focus_param = (int8_t)id;
    app.focus_src = (uint8_t)src;
    app.focus_t = app_stamp();
}

/* a knob's detents: the hint line for its first three touches. The acceleration scales with the parameter's grid
 * (fix-app): x3 / x8 on a 100-step value, x2 / x3 on a 40..79-step one, none on a coarse one (VEL CURVE's 10 steps,
 * MODES' 24): a full sweep never takes fewer than about 12 detents, and a coarse value is always exact */
static void ctl_knob(int id, int32_t e, int role)
{
    uint32_t now = plat_ms();
    char t[32];
    int g = app_grid(id);
    if (app.compare && id < P_NPRESET)         /* (COMPARE plays the stored preset: nothing to edit) */
        return;
    if (!app_is_enum(id))                      /* (an enum moves one value a detent) */
        e = ctl_accel(role, e, g >= 80 ? 3 : g >= 40 ? 2 : 1, g >= 80 ? 8 : g >= 40 ? 3 : 1);
    app_set_param(id, ctl_steps(id, app_edit_value(id), e));
    ctl_focus(id, 0);
    ctl_tour_step(0);
    if (app.touch_param != id || now - app.touch_t >= APP_TOUCH_MS) {   /* a new touch */
        if (app_set.hint_count[id] < 3u) {
            app_set.hint_count[id]++;
            app_dirty();
            app.hint_param = (int8_t)id;
        } else {
            app.hint_param = -1;
        }
    }
    app.touch_param = (int8_t)id;
    app.touch_t = now;
    if (app.hint_param == id)
        app.hint_until = now + APP_HINT_MS;
    cy_param_format(id, app_edit_value(id), t, sizeof t, 1);
    app_tell(CY_PARAMS[id].name, t);
}

/* SEL+PRESETS: A = the sound now, B = the neighbour the turn points to; 1/16 a detent; at 1.0 on to the next pair.
 * Symmetric (fix-app): turning back below 0 returns to the previous pair (7/8 of the way, and so on back to the sound
 * it began with), and turning the other way at the start flips B to the other neighbour, so an overshoot by a detent
 * never strands the scrub */
static void ctl_morph_pair_b(int idx)          /* B = preset idx */
{
    app.morph_ib = (uint16_t)idx;
    app_preset_values(idx, app_mb, app.morph_b);
}
static void ctl_morph_detent(int s)
{
    float v[P_NPARAMS];
    int i;
    if (!app.morphing) {
        app_glide_finish();
        for (i = 0; i < P_NPRESET; i++)
            app_ms[i] = app_ma[i] = app_target[i];
        app.morph_is = app.morph_ia = app.preset;
        app.morph_dir = (int8_t)s;
        app.morph_hops = 0;
        if (!app_preset_values(app_preset_next(app.preset, s), app_mb, app.morph_b))
            return;
        app.morph_ib = (uint16_t)app_preset_next(app.preset, s);
        app_cpy(app.morph_a, app.name, sizeof app.morph_a);
        app_cpy(app.morph_s, app.name, sizeof app.morph_s);
        app.morph_t = 0.0f;
        app.morphing = 1;
        app.undo_ok = 0;
    }
    if (s == app.morph_dir) {
        app.morph_t += 1.0f / 16.0f;
        if (app.morph_t >= 1.0f) {             /* on to the next pair: B is the new A */
            for (i = 0; i < P_NPRESET; i++)
                app_ma[i] = app_mb[i];
            app_cpy(app.morph_a, app.morph_b, sizeof app.morph_a);
            app.morph_ia = app.morph_ib;
            ctl_morph_pair_b(app_preset_next(app.morph_ia, app.morph_dir));
            app.morph_t = 0.0f;
            if (app.morph_hops < 255u)
                app.morph_hops++;
        }
    } else {
        app.morph_t -= 1.0f / 16.0f;
        if (app.morph_t < 0.0f && app.morph_hops) {   /* back to the previous pair: A is the new B */
            app.morph_hops--;
            for (i = 0; i < P_NPRESET; i++)
                app_mb[i] = app_ma[i];
            app_cpy(app.morph_b, app.morph_a, sizeof app.morph_b);
            app.morph_ib = app.morph_ia;
            if (!app.morph_hops) {             /* (the first pair's A is the sound the scrub began with) */
                for (i = 0; i < P_NPRESET; i++)
                    app_ma[i] = app_ms[i];
                app_cpy(app.morph_a, app.morph_s, sizeof app.morph_a);
                app.morph_ia = app.morph_is;
            } else {
                app.morph_ia = (uint16_t)app_preset_next(app.morph_ia, -app.morph_dir);
                app_preset_values(app.morph_ia, app_ma, app.morph_a);
            }
            app.morph_t = 1.0f - 1.0f / 16.0f;
        } else if (app.morph_t < 0.0f) {       /* past the start: B is the neighbour the other way */
            app.morph_dir = (int8_t)-app.morph_dir;
            ctl_morph_pair_b(app_preset_next(app.morph_is, app.morph_dir));
            app.morph_t = 1.0f / 16.0f;
        }
    }
    for (i = 0; i < P_NPARAMS; i++)            /* (params.c gets whole arrays) */
        v[i] = app_norm[i];
    cy_preset_lerp(app_ma, app_mb, app.morph_t, v);
    for (i = 0; i < P_NPRESET; i++)
        app_norm[i] = app_target[i] = app_from[i] = fm_clampf(v[i], 0.0f, 1.0f);
}

static void ctl_encoders(void)
{
    int32_t e;
    int k, sel = (int)(plat_buttons() >> B_SEL & 1u);
    char t[32];
    if ((e = plat_enc(EN_SELECT)) != 0 && !app.compare) {   /* SHAPE, on every page */
        e = ctl_accel(EN_SELECT, e, 2, 4);
        app_set_param(P_SHAPE, ctl_steps(P_SHAPE, app_target[P_SHAPE], e));
        ctl_focus(P_SHAPE, 1);
        cy_param_format(P_SHAPE, app_target[P_SHAPE], t, sizeof t, 1);
        app_tell("SHAPE", t);
        ctl_tour_step(CY_TOUR_SELECT);
    }
    if ((e = plat_enc(EN_ALGO)) != 0 && !app.compare) {     /* EXCITER: MALLET -> BOW -> RAIN, wrapping */
        int x = ((app_exciter() + (int)(e % CY_NEXC)) % CY_NEXC + CY_NEXC) % CY_NEXC;
        app_set_param(P_EXCITER, app_enum_norm(P_EXCITER, x));
        ctl_keymode_watch();                   /* (the engine hears the new exciter before any next key) */
        ctl_focus(P_EXCITER, 2);
        app_tell("EXCITER", cy_engine_exciter_name(x));
        ctl_tour_step(CY_TOUR_ALGO);
    }
    if ((e = plat_enc(EN_PRESET)) != 0 && app_sx_asking()) {
        app_sx_answer(0);                      /* a USB question (app_sounds.c): a turn says no, and is swallowed */
    } else if (e != 0) {
        int s = e < 0 ? -1 : 1, n = (int)(e < 0 ? -e : e), i;
        ctl_tour_step(CY_TOUR_PRESET);
        if (app.save_state) {                  /* the SAVE dialog: the slot, all 32 */
            app.save_slot = (uint16_t)(((int)app.save_slot + (int)(e % CY_NUSER) + CY_NUSER) % CY_NUSER);
            app.save_state = ctl_save_state_of(app.save_slot);
            app.save_t = plat_ms();
            ctl_save_slot_say();
        } else if (app.compare) {              /* (COMPARE plays the stored preset: nothing to browse) */
        } else if (sel) {                      /* the morph-scrub */
            if (app.browsing)                  /* (a browse still settling lands first) */
                ctl_browse_land(0);
            for (i = 0; i < n && i < 64; i++)
                ctl_morph_detent(s);
            ctl_keymode_watch();
            t[0] = 0;
            app_cat_int(t, (int32_t)(app.morph_t * 100.0f + 0.5f), sizeof t);
            app_cat(t, "%", sizeof t);
            app_tell("MORPH", t);
        } else {                               /* browse: the name now, the load once the turning rests */
            int p = app.browsing ? app.browse : app.preset;
            char nm[16];
            float v[P_NPARAMS];
            for (i = 0; i < n && i < APP_NPRE; i++)
                p = app_preset_next(p, s);
            app.browsing = 1;
            app.browse = (uint16_t)p;
            app.browse_t = plat_ms();
            ctl_focus(-1, 3);
            app_preset_num(t, p, sizeof t);
            app_preset_values(p, v, nm);
            app_tell(t, nm);
        }
    }
    for (k = 0; k < 4; k++) {                  /* K1..K4: the page's four, layer A or (SEL held) B */
        int id;
        if ((e = plat_enc(EN_K1 + k)) == 0)
            continue;
        id = cy_page_param(app.page, app_exciter(), app.layer, k);
        if (id >= 0 && id < P_NPARAMS)
            ctl_knob(id, e, EN_K1 + k);
    }
    ctl_keymode_watch();
}

static void app_midi_in(int poll);
/* the controls and MIDI in, from app_frame and from every main-loop pass between frames: a key, a detent or a
 * MIDI note is acted on within a millisecond, whatever the frame rate (the device draws at 20..60 fps) */
static void app_input(void)
{
    ctl_keys();
    ctl_buttons();
    ctl_encoders();
    app_midi_in(0);
}
void app_input_only(void) { app_input(); }

/* the note path alone, from inside ui_frame (UI_POLL, between the plate's strips; fix-app): the keys and MIDI in,
 * which only queue engine events and change the app's own state (the UI's copy is next frame's). Buttons and
 * encoders wait for the frame; so does a Program Change (app_midi_in: it would publish geometry mid-frame) */
static void app_poll_notes(void)
{
    ctl_keys();
    app_midi_in(1);
}

/* ------------------------------------------------------------------- MIDI --- */
/* MIDI in, polled in the main loop (§5.4): any channel. TUNED notes have no key (-1); SURFACE notes are the points
 * the keys would be (key = note - 53 - 12 octave), the rest ignored. Where a note went is remembered, so its
 * note-off and pressure find it whatever changed since.
 * fix-app: CC120 All Sound Off is a PANIC (latched and frozen voices too); CC121 resets the controllers (bend,
 * pressure, mod wheel, pedal: RP-015); CC122 Local Control (0: the keys send MIDI but do not play, for a DAW that
 * echoes them back); CC64 is the pedal's own latch, beside ARP's; Program Change 0..55 loads presets 01..24, U01..U32
 * as a browse does; channel pressure and CC1 reach the engine only while MIDI notes are held */
static void app_midi_notes_off(void)          /* CC123 (and the start of CC120): MIDI's notes let go */
{
    int n;
    for (n = 0; n < 128; n++)
        if (app.midi_key[n] != -2) {
            if (app.midi_key[n] >= 0)
                app.midi_press[app.midi_key[n]] = 0;
            cy_engine_note_off(app.midi_key[n], n);
            app.midi_key[n] = -2;
        }
    app.midi_held = 0;
    ctl_cp_apply();
}

static void app_midi_program(int pc)
{
    char t[8];
    if (pc < 0 || pc >= APP_NPRE || (pc >= CY_NFACTORY && !app_user_used(pc - CY_NFACTORY)))
        return;
    ctl_morph_end();
    ctl_save_cancel();
    app.browse = (uint16_t)pc;
    app.browsing = 1;
    ctl_browse_land(1);
    app_preset_num(t, app.preset, sizeof t);
    app_say(t, app.name);
}

static void app_midi_one(uint32_t pkt)
{
    uint32_t st = (pkt >> 8) & 0xF0u, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
    if (st == 0x90u && d2) {
        int key = -1;
        if (app.keymode) {
            key = (int)d1 - (53 + 12 * app.octave);
            if (key < 0 || key >= NKEYS)
                return;
        }
        if (app.midi_key[d1] != -2)            /* (a repeat without its note-off: the old one let go first) */
            cy_engine_note_off(app.midi_key[d1], (int)d1);
        else if (app.midi_held < 255u)
            app.midi_held++;
        app.midi_key[d1] = (int8_t)key;
        if (key >= 0)
            app.midi_press[key] = 0;
        ctl_cp_apply();
        app_chord_guard(plat_ms());
        if (key >= 0)                          /* a SURFACE point from MIDI: the MIDI P_eff and channel pressure */
            cy_engine_note_on_midi(key, (int)d1, (int)d2);
        else
            cy_engine_note_on(key, (int)d1, (int)d2);
        ctl_tour_step(CY_TOUR_KEY);
    } else if (st == 0x80u || st == 0x90u) {
        if (app.midi_key[d1] != -2) {
            if (app.midi_key[d1] >= 0)
                app.midi_press[app.midi_key[d1]] = 0;
            cy_engine_note_off(app.midi_key[d1], (int)d1);
            app.midi_key[d1] = -2;
            if (app.midi_held)
                app.midi_held--;
            ctl_cp_apply();
        }
    } else if (st == 0xA0u) {                  /* poly aftertouch: that note's pressure */
        if (app.midi_key[d1] != -2) {
            if (app.midi_key[d1] >= 0)
                app.midi_press[app.midi_key[d1]] = (uint8_t)d2;
            cy_engine_pressure(app.midi_key[d1], (int)d1, (int)d2);
            app.midi_at = (uint8_t)d2;
        }
    } else if (st == 0xD0u || (st == 0xB0u && d1 == 1u)) {   /* channel pressure, CC1: every held slot */
        app.midi_cp = (uint8_t)(st == 0xD0u ? d1 : d2);
        app.midi_at = app.midi_cp;
        ctl_cp_apply();
    } else if (st == 0xB0u && d1 == 64u) {     /* the sustain pedal: LATCH, its own */
        uint8_t on = (uint8_t)(d2 >= 64u);
        if (on != app.latch_pedal) {
            app.latch_pedal = on;
            ctl_latch_apply();
            app_tell("LATCH", on ? "ON (PEDAL)" : app.latch_btn ? "ON (ARP)" : "OFF");
        }
    } else if (st == 0xB0u && d1 == 120u) {    /* All Sound Off: silence, now */
        ctl_panic_all(0, "ALL SOUND OFF", "");
    } else if (st == 0xB0u && d1 == 121u) {    /* Reset All Controllers */
        int k;
        app.bend = 0.0f;
        cy_engine_bend(0.0f);
        app.midi_cp = app.midi_at = 0;
        ctl_cp_apply();
        for (k = 0; k < 128; k++)              /* (each held MIDI note's own pressure too) */
            if (app.midi_key[k] != -2) {
                if (app.midi_key[k] >= 0)
                    app.midi_press[app.midi_key[k]] = 0;
                cy_engine_pressure(app.midi_key[k], k, 0);
            }
        if (app.latch_pedal) {
            app.latch_pedal = 0;
            ctl_latch_apply();
        }
    } else if (st == 0xB0u && d1 == 122u) {    /* Local Control */
        uint8_t off = (uint8_t)(d2 < 64u);
        if (off != app.local_off) {
            int k;
            for (k = 0; k < NKEYS; k++)        /* (the keys' notes let go in the old terms) */
                if (app.note[k] >= 0) {
                    if (!app.local_off)
                        cy_engine_note_off(k, app.note[k]);
                    ctl_out_off(app.note[k]);
                    app.note[k] = -1;
                    app.press[k] = 0;
                }
            app.local_off = off;
            app_say("LOCAL", off ? "OFF" : "ON");
        }
    } else if (st == 0xB0u && d1 == 123u) {    /* All Notes Off (the pedal still holds what it latched) */
        app_midi_notes_off();
    } else if (st == 0xC0u) {                  /* Program Change */
        app_midi_program((int)d1);
    } else if (st == 0xE0u) {                  /* pitch bend, +-2 semitones */
        app.bend = (float)((int32_t)(d1 | d2 << 7) - 8192) * (2.0f / 8192.0f);
        cy_engine_bend(app.bend);
    }
}

static uint32_t app_midi_defer;                /* a message the UI's poll left for the frame */
static uint8_t app_midi_deferred;
static void app_midi_in(int poll)
{
    uint32_t pkt;
    int port;
    if (app_midi_deferred) {
        if (poll)                              /* (in order: nothing overtakes it) */
            return;
        app_midi_deferred = 0;
        app_midi_one(app_midi_defer);
    }
    while (plat_midi_in(&pkt, &port)) {
        if (poll && ((pkt >> 8) & 0xF0u) == 0xC0u) {
            app_midi_defer = pkt;
            app_midi_deferred = 1;
            return;
        }
        app_midi_one(pkt);
    }
}

/* ------------------------------------------------------------ the clocks --- */
static void ctl_tick(uint32_t now)
{
    if (app.browsing && now - app.browse_t >= APP_SETTLE_MS)   /* the browse lands, gliding */
        ctl_browse_land(1);
    if (app.save_state && now - app.save_t >= APP_SAVE_IDLE_MS) {   /* the SAVE dialog, forgotten: closed */
        ctl_save_cancel();
        app_say("SAVE", "CANCELLED");
    }
    ctl_save_service(now);                     /* (a confirmed SAVE: faded out, it writes, then fades back in) */
    if (app.seq_combo && !app.seq_swept && now - app.seq_t >= APP_SWEEP_MS) {   /* SEL+SEQ held: SWEEP */
        app.seq_swept = 1;
        app.sweep_t = now ? now : 1u;
        app.sweeps++;
        app_say("SWEEP", "");
    }
    if (app.hint_param >= 0 && (int32_t)(now - app.hint_until) >= 0)
        app.hint_param = -1;
    ctl_tour_tick(now);
}

static void app_lfo(float dt)
{
    float rate = app_v(P_LFO_RATE), pmax = 0.0f;
    uint32_t i;
    for (i = 0; i < NKEYS; i++)                /* cy_key_pressure: the keys' and MIDI's */
        app.press_out[i] = app.press[i] > app.midi_press[i] ? app.press[i] : app.midi_press[i];
    app_mod.lfo = cy_lfo_step(&app.lfo_phase, &app.lfo_rng, app.lfo_state, app_enum(P_LFO_WAVE), rate, dt);
    app.orbit += rate * dt;                    /* LFO>POS orbits at the LFO's rate (§3.9): turns, our own count */
    app.orbit -= fm_floorf(app.orbit);
    app_mod.orbit_u = fm_cosf(6.2831853f * app.orbit);
    app_mod.orbit_v = fm_sinf(6.2831853f * app.orbit);
    for (i = 0; i < NKEYS; i++)                /* the hardest held press: the keys', MIDI's, the engine's */
        if ((float)app.press_out[i] > pmax)
            pmax = (float)app.press_out[i];
    for (i = 0; i < CY_NVOICES; i++)
        if (app_tel.voice_state[i] != CY_VS_FREE && (float)app_tel.voice_press[i] > pmax)
            pmax = (float)app_tel.voice_press[i];
    app_mod.press_max = pmax * (1.0f / 127.0f);
    app_mod.octave = app.octave;
}

/* the mode budget's governor (§3.8; the device's, or the host's under test). Slow: every 500 ms, over 80 % takes 8
 * modes; under 62 % for 2 s gives 8 back, up to DETAIL. Fast: an overloaded block (app_overload) takes 32 at once.
 * fix-app: it also acts before the xrun. app_render times every block, and a near miss (a block over 80 % of its
 * time) takes 16 at once, since the next chord's onset (contact and noise run every mode through the excited
 * kernel: ~+50 % of a block for four notes) is what overflows. A give-back waits until the heaviest block of the last
 * 8 s left room for an onset (under 60 %), so it does not climb straight back into the next xrun. A shed cuts from
 * where the budget bites (the modes in use + 8: a bowed patch uses fewer than CY_BUDGET_MIN, so it goes straight to the
 * floor). At CY_BUDGET_MIN, where modes no longer help, it sheds voices instead, VOICES down to 4 (new notes then
 * steal), and it gives voices back before modes */
#ifdef PLAT_HOST
static uint8_t app_gov_on, app_gov_cpu;        /* the host's "governor 1" and "cpu N": the device's logic, testable */
static uint16_t app_gov_pk;                    /* the host's "peak N": a block's load per mille, as app_render's */
#endif
static int app_voices_set(void) { return (int)(app_v(P_VOICES) + 0.5f); }   /* VOICES: 4 / 6 / 8 */
static void app_gov_shed(int modes, uint32_t now)
{
    int used = (int)app_tel.modes_used, b = (int)app.budget;
    if (used + 8 < b)                          /* (a budget above what plays sheds nothing: cut from where it bites) */
        b = used + 8;
    if (app.budget > CY_BUDGET_MIN)            /* modes first, down to the floor (measured: cutting voices first let */
        app_budget_set(b - modes);             /*  a bowed onset's modes grow, 34 xruns against 12; clamped at MIN) */
    else if (app.gov_voices > 4u)
        app.gov_voices = (uint8_t)(app.gov_voices - 2u);
    app.gov_low_t = now;
}
static void app_gov_give(uint32_t now)
{
    if (app.gov_voices < CY_NVOICES && app.gov_voices < app_voices_set()) {
        app.gov_voices = (uint8_t)(app.gov_voices + 2u);
    } else if (app.budget < app_detail_modes()) {
        int n = app.budget + 8;
        app_budget_set(n > app_detail_modes() ? app_detail_modes() : n);
    }
    app.gov_low_t = now;                       /* (2 s more of calm before the next step) */
}
static void app_governor(uint32_t now)
{
    uint32_t cpu, pk, ovl = app_overloads;
#ifdef PLAT_HOST
    if (!app_gov_on)
        return;
    cpu = app_gov_cpu;
    pk = app_gov_pk;
    app_gov_pk = 0;
#else
    cpu = plat_cpu_pct();
    pk = app_blk_pk;                           /* (read and cleared between two renders: one block's peak may go to */
    app_blk_pk = 0;                            /*  the next frame, never lost for long) */
#endif
    if (pk >= app.pk_hold || now - app.pk_hold_t >= 8000u) {
        app.pk_hold = pk;
        app.pk_hold_t = now;
    }
    if (ovl != app.ovl_seen) {
        app.ovl_seen = ovl;
        if (now - app.ovl_t >= 100u) {         /* (one long burst is one step, not a slide to the floor) */
            app.ovl_t = now;
            app_gov_shed(32, now);
        }
    } else if (pk >= 800u && now - app.ovl_t >= 100u) {   /* a near miss: shed before the next onset overflows */
        app.ovl_t = now;
        app_gov_shed(16, now);
    }
    if (now - app.gov_t < 500u)
        return;
    app.gov_t = now;
    if (cpu > 80u)
        app_gov_shed(8, now);
    else if (cpu >= 62u || app.pk_hold >= 600u)
        app.gov_low_t = now;
    else if (now - app.gov_low_t >= 2000u)
        app_gov_give(now);
}

/* the chord guard (fix-app): a third note-on within 10 ms is a chord whose onsets land in the same blocks, and each
 * onset runs every mode of its slot through the excited kernel (contact and noise): four at budget 96 measured 102 %
 * of a block, the first chord's xrun, before the governor could see anything. So, before the third note reaches the
 * engine, the budget drops to APP_CHORD_BUDGET (the engine reads it at the start of the render that takes the notes)
 * and the give-back waits as after a near miss. A melody or a dyad keeps DETAIL's budget. A DAW's chords arrive
 * together; the keys' and the UI's poll spread a played chord over a few blocks, which helps too */
static void app_chord_guard(uint32_t now)
{
#ifdef PLAT_HOST
    if (!app_gov_on)                           /* (the host and the browser: no governor, unless a test asks) */
        return;
#endif
    if (now - app.chord_t > 10u) {
        app.chord_t = now;
        app.chord_n = 0;
    }
    if (++app.chord_n >= 3u && app.budget > APP_CHORD_BUDGET) {
        app_budget_set(APP_CHORD_BUDGET);
        app.gov_low_t = now;
        if (app.pk_hold < 700u)
            app.pk_hold = 700u, app.pk_hold_t = now;
    }
}

/* ------------------------------------------------------- patch, geometry --- */
/* the patch from the live values, with what the app adds: SEL+OCT's transpose moves SURFACE's root too (it is the
 * TUNED keys' transpose otherwise, fix-app), and the governor's polyphony */
static void app_patch_build(void)
{
    cy_patch_build(app_norm, &app_mod, &app_patch, &app_geom);
    if (app_patch.surface)
        app_patch.surface_root += (float)app.transpose;
    if (app_patch.max_voices > app.gov_voices)
        app_patch.max_voices = app.gov_voices;
}

/* the geometry's input to cy_geom_update, every frame (a stale field cover takes its turn there). A knob, a glide or
 * a preset moves SHAPE and POS at once. The LFO alone (MORPH, LFO>POS) publishes in steps of 0.02 SHAPE / 0.005 POS
 * (a partial moves a few cents at most, and the engine glides each step over 40 ms), at most every 50 ms while its
 * rate is under 1 Hz, and any smaller change lands within 250 ms. Each publish costs ~1.5 M instructions on the device
 * (the geometry's rebuild and field cover, the UI's band map and rim cache: ~7 ms) and re-glides every live mode;
 * AETHER republished at every frame (fix-app) */
static void app_geom_update(uint32_t now)
{
    cy_geom_in_t *g = &app_geom;
    float sb = app_v(P_SHAPE), ub = app_v(P_POS_X), vb = app_v(P_POS_Y);
    if (app.geo_ok && sb == app.geo_shape && ub == app.geo_u && vb == app.geo_v) {   /* the LFO alone moved them */
        float ds = fm_fabsf(g->shape - app_geom_pub.shape);
        float dp = fm_maxf(fm_fabsf(g->pos_u - app_geom_pub.pos_u), fm_fabsf(g->pos_v - app_geom_pub.pos_v));
        uint32_t gap = app_v(P_LFO_RATE) < 1.0f ? 50u : 0u, dt = now - app.geo_t;
        if ((ds != 0.0f || dp != 0.0f) && (dt < gap || (ds < 0.02f && dp < 0.005f && dt < 250u))) {
            g->shape = app_geom_pub.shape;
            g->pos_u = app_geom_pub.pos_u;
            g->pos_v = app_geom_pub.pos_v;
        }
    }
    app.geo_shape = sb, app.geo_u = ub, app.geo_v = vb, app.geo_ok = 1;
    if (g->shape != app_geom_pub.shape || g->pos_u != app_geom_pub.pos_u || g->pos_v != app_geom_pub.pos_v) {
        app.geo_t = now;
        app.geo_pubs++;
    }
    app_geom_pub = *g;
    cy_geom_update(g);
}

static void app_patch_now(void)                /* the engine and the geometry hear the patch now (KEYS, EXCITER) */
{
    app_patch_build();
    app_geom_update(plat_ms());
    cy_engine_set_patch(&app_patch);
}

/* ------------------------------------------------------------------ LEDs --- */
static void led_update(uint32_t now)
{
    uint32_t k, lit_b = 0, lit_k = 0, mid_b = 0, mid_k = 0, dim_b = 0, dim_k = 0;
    uint8_t ring[NKEYS], held[NKEYS];
    for (k = 0; k < NKEYS; k++) {              /* (held means sounding a note: a key held through PANIC is not) */
        ring[k] = 0;
        held[k] = (uint8_t)((app.keys >> k & 1u) && app.note[k] >= 0 ? (app.press[k] > 64u ? 3u : 2u) : 0u);
    }
    if (!app.keymode) {                        /* TUNED: "ringing above -50 dB" is against the note's own peak, so a
                                                * soft note glows as long as a loud one */
        for (k = 0; k < NKEYS; k++)            /* the keys' own notes: the engine's level of each key's newest slot */
            ring[k] = (uint8_t)(app_tel.key_level[k] > APP_RING_LV);
        for (k = 0; k < CY_NVOICES; k++) {     /* MIDI's notes (key -1): where they fall on the keybed, if they do */
            int key, st = app_tel.voice_state[k];
            float lv = app_tel.voice_level[k];
            if (st == CY_VS_FREE || led_vnote[k] != app_tel.voice_note[k])
                led_vpk[k] = 0.0f;
            led_vnote[k] = app_tel.voice_note[k];
            if (st == CY_VS_FREE)
                continue;
            led_vpk[k] = lv > led_vpk[k] ? lv : led_vpk[k];
            if (app_tel.voice_key[k] >= 0)
                continue;
            key = (int)app_tel.voice_note[k] - 53 - 12 * app.octave - app.transpose;
            if (key < 0 || key >= NKEYS)
                continue;
            if ((st == CY_VS_HELD || st == CY_VS_BOWED) && app.midi_key[app_tel.voice_note[k] & 127u] != -2 &&
                held[key] < 2u)
                held[key] = app_tel.voice_press[k] > 64u ? 3u : 2u;
            if (lv > APP_RING_LV * led_vpk[k])
                ring[key] = 1;
        }
    }
    for (k = 0; k < NKEYS; k++) {
        uint8_t l;
        if (app.keymode) {                     /* SURFACE: the live vibration map, with hysteresis; nodes go dark */
            static const float UP[3] = {0.12f, 0.35f, 0.65f}, DN[3] = {0.08f, 0.28f, 0.55f};
            float lv = app_tel.key_level[k];
            l = led_surf[k];
            while (l < 3u && lv >= UP[l])
                l++;
            while (l > 0u && lv < DN[l - 1u])
                l--;
            led_surf[k] = l;
        } else {
            l = held[k] ? held[k] : ring[k];
        }
        led_lv[k] = l;
    }
    for (k = 0; k < NB; k++)
        led_lv[27 + k] = 1;                    /* every unlit button glows: the panel reads in the dark */
    for (k = 0; k < PG_N; k++)
        led_lv[27 + ctl_page_btn[k]] = k == app.page ? 3u : 1u;
    led_lv[27 + B_SEL] = app.layer ? 3u : 1u;
    led_lv[27 + B_EDIT] = (uint8_t)(1u + app.view);
    {
        uint32_t t = app.sprinkle_t > app.sweep_t ? app.sprinkle_t : app.sweep_t;
        if (t && now - t < APP_BLINK_MS)       /* a sprinkle or a sweep: 2 / 3 at 4 Hz */
            led_lv[27 + B_SEQ] = ((now - t) / 125u) & 1u ? 2u : 3u;
        else
            led_lv[27 + B_SEQ] = app.keymode ? 3u : 1u;
    }
    led_lv[27 + B_PLAY] = app.frozen ? 0u : 1u;
    led_lv[41] = app.frozen ? 3u : 0u;         /* FREEZE: PLAY's green LED */
    led_lv[27 + B_REC] = app.damping ? 3u : 1u;
    led_lv[27 + B_ARP] = app.latched ? 3u : 1u;   /* (either latch: ARP's or the pedal's) */
    if (app.save_state || app_sx_asking())
        led_lv[27 + B_SAVE] = (now / 250u) & 1u ? 1u : 3u;   /* the SAVE dialog, a USB question: blinking at 2 Hz */
    else
        led_lv[27 + B_SAVE] = app.edited ? 3u : 1u;
    led_lv[27 + B_OCTDN] = app.octave <= -2 ? 3u : app.octave == -1 ? 2u : 1u;
    led_lv[27 + B_OCTUP] = app.octave >= 2 ? 3u : app.octave == 1 ? 2u : 1u;
    for (k = 0; k < 42u; k++)
        if (led_lv[k] != led_sent[k])
            break;
    if (k == 42u && led_ok)
        return;
    for (k = 0; k < 41u; k++) {
        uint32_t bit = k < 27u ? 1u << k : 1u << (k - 27u), l = led_lv[k];
        if (k < 27u)
            lit_k |= l == 3u ? bit : 0u, mid_k |= l == 2u ? bit : 0u, dim_k |= l == 1u ? bit : 0u;
        else
            lit_b |= l == 3u ? bit : 0u, mid_b |= l == 2u ? bit : 0u, dim_b |= l == 1u ? bit : 0u;
    }
    if (led_lv[41])
        lit_b |= LED_PLAY_GREEN;
    plat_lights(lit_b, lit_k, dim_b, dim_k, 1);
    plat_lights_mid(mid_b, mid_k);
    for (k = 0; k < 42u; k++)
        led_sent[k] = led_lv[k];
    led_ok = 1;
}

/* -------------------------------------------------------------- settings --- */
static void app_settings_defaults(void)
{
    uint8_t *p = (uint8_t *)&app_set;
    uint32_t i;
    for (i = 0; i < sizeof app_set; i++)
        p[i] = 0;
    app_set.magic = APP_SET_MAGIC;
    app_set.version = APP_SET_VERSION;
    app_set.edit_preset = 0xFFFFu;
    for (i = 0; i < APP_NGLOB; i++)
        app_set.glob[i] = (uint16_t)app_q16(app_norm[P_NPRESET + i]);
}

/* the edit buffer (settings v2, fix-app): the loaded preset's edits, or else the ones a browse kept; or none */
static void app_edit_fill(void)
{
    int i, live = app_is_edited();
    const float *s = live ? app_target : app.mem_ok ? app_mem : 0;
    app_set.edit_preset = s ? (uint16_t)(live ? app.preset : app.mem_preset) : 0xFFFFu;
    app_set.edit_live = (uint8_t)(live != 0);
    app_set.rsv2 = 0;
    for (i = 0; i < P_NPRESET; i++)
        app_set.edit[i] = (uint16_t)(s ? app_q16(s[i]) : 0u);
}
static uint32_t app_edit_sig(void)             /* its fingerprint: a change dirties the settings */
{
    uint32_t h = 0x811C9DC5u;
    int i, live = app_is_edited();
    const float *s = live ? app_target : app.mem_ok ? app_mem : 0;
    if (!s)
        return h;
    h = (h ^ (uint32_t)(live ? app.preset : (1u << 16) | app.mem_preset)) * 0x01000193u;
    for (i = 0; i < P_NPRESET; i++)
        h = (h ^ app_q16(s[i])) * 0x01000193u;
    return h;
}

static void app_settings_save(uint32_t now, int quiet)
{
    uint32_t i;
    for (i = 0; i < APP_NGLOB; i++)
        app_set.glob[i] = (uint16_t)app_q16(app_norm[P_NPRESET + i]);
    app_set.octave = app.octave;
    app_set.view = app.view;
    app_edit_fill();
    app.set_saved_t = now;                     /* (a failure waits before it tries again: see app_autosave) */
    if (plat_store_save(OBJ_SETTINGS, &app_set, sizeof app_set) == 0) {
        app.set_dirty = 0;
        app.set_saves++;
        app.set_fails = 0;
    } else {
        if (!app.set_fails && !quiet)          /* (said once: a flash that cannot be written is not news twice) */
            app_say("SETTINGS", "NOT SAVED");
        if (app.set_fails < 255u)
            app.set_fails++;
    }
}

/* §5.7: only when something changed, every voice has been free for 3 s, and the last save is 30 s old (2 min when
 * only the preset number changed: browsing never wears the flash): the flash write's ~50 ms stall never lands in a
 * performance. A write that fails waits twice as long each time (fix-app), up to 32 times */
static void app_autosave(uint32_t now)
{
    int quiet = !app.keys && app_tel.peak_l < APP_SILENT_LV && app_tel.peak_r < APP_SILENT_LV, i;
    uint32_t gap = (app.set_dirty & 2u) ? APP_SAVE_GAP_MS : APP_SAVE_GAP_PRESET_MS;
    for (i = 0; i < CY_NVOICES && quiet; i++)
        quiet = app_tel.voice_state[i] == CY_VS_FREE;
    for (i = 0; i < 128 && quiet; i++)
        quiet = app.midi_key[i] == -2;
    if (!quiet) {
        app.quiet_t = now;
        return;
    }
    gap <<= app.set_fails < 5u ? app.set_fails : 5u;
    if (app.set_dirty && !app.save_hold && now - app.quiet_t >= APP_QUIET_MS && now - app.set_saved_t >= gap)
        app_settings_save(now, 0);
}

/* what the flash holds, into the app (boot, and a restore from USB): the settings (v1 migrated; anything else, the
 * defaults), the user bank (another format's is locked, never overwritten: fix-app), the preset and its edits */
static void app_load_stored(uint32_t now)
{
    uint32_t i;
    int n, sz, dirty = 0;
    uint8_t *p = (uint8_t *)&app_set;
    for (i = 0; i < sizeof app_set; i++)
        p[i] = 0;
    n = plat_store_load(OBJ_SETTINGS, &app_set, sizeof app_set);
    if (n == (int)APP_SET_V1_SIZE && app_set.magic == APP_SET_MAGIC && app_set.version == 1u) {   /* v1 */
        app_set.version = APP_SET_VERSION;
        app_set.tour_boots = 0;
        app_set.edit_preset = 0xFFFFu;
        n = (int)sizeof app_set;
        dirty = 1;
    }
    if (n != (int)sizeof app_set || app_set.magic != APP_SET_MAGIC || app_set.version != APP_SET_VERSION ||
        app_set.octave < -3 || app_set.octave > 3 || app_set.view >= CY_NVIEW || app_set.preset >= APP_NPRE ||
        (app_set.edit_preset >= APP_NPRE && app_set.edit_preset != 0xFFFFu) || app_set.edit_live > 1u)
        app_settings_defaults();
    for (i = 0; i < APP_NGLOB; i++)
        app_norm[P_NPRESET + i] = (float)app_set.glob[i] * (1.0f / 65535.0f);
    app.bank_locked = 0;
    sz = plat_store_size(OBJ_USER);
    if (sz < 0 || plat_store_load(OBJ_USER, &app_bank, sizeof app_bank) != (int)sizeof app_bank ||
        sz != (int)sizeof app_bank || app_bank.magic != CY_USER_MAGIC || app_bank.version != APP_BANK_VERSION) {
        uint8_t *b = (uint8_t *)&app_bank;
        for (i = 0; i < sizeof app_bank; i++)
            b[i] = 0;
        app.bank_locked = (uint8_t)(sz > 0);   /* (none, or both copies bad: nothing to keep; else not ours) */
    }
    app.bank_crc_ok = 0;                       /* (CYMA-1 Sounds: GET_DIR's CRC is computed again) */
    app.octave = app_set.octave;
    app.view = app_set.view;
    if (!app_set.tour_done && app_set.tour_boots) {   /* the second boot: the tour has been seen */
        app_set.tour_done = 1;
        dirty = 1;
    } else if (!app_set.tour_done) {
        app_set.tour_boots = 1;
        dirty = 1;
    }
    app.tour = CY_TOUR_OFF;
    if (!app_set.tour_done)                    /* the first boot shows the way */
        ctl_tour_start(now, CY_TOUR_KEY);
    app_budget_set(app_detail_modes());
    app.mem_ok = 0;
    if (!app_preset_load(app_set.preset, 0))   /* (a user slot since emptied: the first) */
        app_preset_load(0, 0);
    if (app_set.edit_preset == app.preset && app_set.edit_live) {   /* the edits it was left with */
        float v[P_NPRESET];
        for (i = 0; i < P_NPRESET; i++)
            v[i] = (float)app_set.edit[i] * (1.0f / 65535.0f);
        app_targets_give(v, 0);
    } else if (app_set.edit_preset != 0xFFFFu && !app_set.edit_live && app_set.edit_preset != app.preset &&
               (app_set.edit_preset < CY_NFACTORY || app_user_used(app_set.edit_preset - CY_NFACTORY))) {
        for (i = 0; i < P_NPRESET; i++)        /* (a browse's kept edits: kept still) */
            app_mem[i] = app_ongrid((int)i, (float)app_set.edit[i] * (1.0f / 65535.0f));
        app.mem_ok = 1;
        app.mem_preset = app_set.edit_preset;
    }
    app.keymode = (uint8_t)app_keymode();
    app_exc_seen = (uint8_t)app_exciter();
    app.set_dirty = (uint8_t)(dirty ? 3u : 0u);
    app.set_saved_t = app.quiet_t = now;
    app.edit_sig = app_edit_sig();
}

void app_init(void)
{
    uint32_t i, now = plat_ms();
    uint8_t *p = (uint8_t *)&app;
    for (i = 0; i < sizeof app; i++)           /* (a host "reboot" runs this again: everything from the flash) */
        p[i] = 0;
    for (i = 0; i < sizeof led_sent; i++)
        led_lv[i] = led_sent[i] = 0;
    for (i = 0; i < NKEYS; i++)
        led_surf[i] = 0;
    for (i = 0; i < CY_NVOICES; i++)
        led_vpk[i] = 0.0f, led_vnote[i] = 0;
    for (i = 0; i < P_NPARAMS; i++)
        ctl_sticky[i] = 0;
    led_ok = 0;
    app_overloads = 0;
    app_blk_pk = 0;
    app_mute = 0;
    app_midi_deferred = 0;
    app_sx_init();                             /* (CYMA-1 Sounds: nothing staged, no question) */
    cy_geom_init();
    cy_fx_init();
    cy_engine_init();
    cy_engine_seed(0x9E3779B9u);
    cy_params_defaults(app_norm);
    app.page = app.prev_page = PG_PLATE;
    app.lfo_rng = 0x6C8E9CF5u;                 /* the modulation's RNG: apart from the engine's and the UI's */
    app.focus_param = app.hint_param = app.touch_param = -1;
    app.later_page = -1;
    app.gov_voices = CY_NVOICES;
    for (i = 0; i < NKEYS; i++)
        app.note[i] = -1;
    for (i = 0; i < 128u; i++)
        app.midi_key[i] = -2;
    app_load_stored(now);                      /* (app.budget is 0 before it: the engine always hears it) */
    app_patch_build();
    app_geom_update(now);
    cy_engine_set_patch(&app_patch);
}

/* ---------------------------------------------------------------- render --- */
#ifdef PLAT_HOST
static void app_host_tap(const int32_t *lr, uint32_t n, uint32_t us);
#endif
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t t0 = plat_ticks24();
    cy_engine_render(lr, n, app_mute ? 0u : master_q12);   /* (muted: fx's MASTER smoothing fades it in ~10 ms) */
#ifdef PLAT_HOST
    app_host_tap(lr, n, (plat_ticks24() - t0) / PLAT_TICKS_PER_US);
#else
    {   /* this block's load, per mille of its time (544 ticks a sample at 44,117.647 Hz): the governor's near miss */
        uint32_t pm = (plat_ticks24() - t0) * 125u / (n * 68u);
        if (pm > app_blk_pk)
            app_blk_pk = pm;
    }
#endif
}

/* silence now, before an update or a restore rewrites the flash: the voices fade (the engine's PANIC), and so does
 * the room's tail (MASTER muted, fix-app), which otherwise stuttered through every erase. app_ui_init lets it go */
void app_panic(void)
{
    app_mute = 1;
    cy_engine_panic();
    app_sx_abort(0x40);                        /* (a USB question or staging: dropped, as declined) */
}
void app_overload(void) { app_overloads++; }   /* the audio interrupt: the frame's governor sheds */

/* backup and restore over USB (fm1.toml backup = true, fix-app): the settings and the user bank, all or nothing */
int app_backup_playing(void) { return app_sounding() || app.keys || app.midi_held; }
void app_backup_flush(void)                    /* a backup starts (stopped): what is not saved yet, saved */
{
    uint32_t now = plat_ms();
    if (app.save_pending) {
        app.save_pending = 0;
        ctl_save_write(app.save_pend_slot, now);
        app_mute = 0;
    }
    if (app.set_dirty)
        app_settings_save(now, 1);
}
void app_backup_hold(int on)                   /* a backup streams: nothing written meanwhile */
{
    app.save_hold = (uint8_t)(on != 0);
}
void app_backup_restored(void)                 /* everything from the flash again (the DSP keeps running) */
{
    uint32_t now = plat_ms();
    ctl_release_all();
    app.compare = app.gliding = app.morphing = app.browsing = app.save_state = app.save_pending = 0;
    app.undo_ok = 0;
    app_sx_abort(0x40);                        /* (a USB question or staging: dropped, as declined) */
    app_load_stored(now);
    touch_curve((int)fm_floorf(app_v(P_VEL_CURVE) + 0.5f));
    touch_sens((int)fm_floorf(app_v(P_PRESS_SENS) + 0.5f));
    app_patch_now();
    app_mute = 0;
}

/* -------------------------------------------------------------------- UI --- */
/* SAVE + HOME held at power-on (fix-app): the settings back to their defaults (preset 01, the globals, the octave,
 * the view, the edit buffer), in RAM now and in the flash at the next autosave; the user bank is kept. A way out of
 * a stored state a future bug could trip on. (REC is the chip's update mode and OCT- + OCT+ the calibration: both
 * the platform's) */
static void app_settings_reset(uint32_t now)
{
    float d[P_NPARAMS];
    int i;
    cy_params_defaults(d);
    for (i = P_NPRESET; i < P_NPARAMS; i++)
        app_norm[i] = d[i];
    app_settings_defaults();
    app.octave = app.transpose = 0;
    app.view = 0;
    app.mem_ok = 0;
    app_budget_set(app_detail_modes());
    app_preset_load(0, 0);
    ctl_tour_start(now, CY_TOUR_KEY);
    app_dirty();
    app_patch_now();
    app_say("SETTINGS RESET", "");
}

void app_ui_init(void)
{
    uint32_t now = plat_ms(), b;
    int first = !app.boot_t;
    touch_on(1);                               /* velocity and pressure: the keys' readings on */
    b = plat_buttons();
    if (first && (b >> B_SAVE & 1u) && (b >> B_HOME & 1u)) {
        app_settings_reset(now);
        app.btn = b;                           /* (the held pair is not a SAVE and a page press) */
    }
    touch_curve((int)fm_floorf(app_v(P_VEL_CURVE) + 0.5f));
    touch_sens((int)fm_floorf(app_v(P_PRESS_SENS) + 0.5f));
    if (first)                                 /* (again after an update that did not commit: no second splash) */
        app.boot_t = now ? now : 1u;
    app.last_frame = now;
    app_mute = 0;
    ui_init();
}
void app_splash(void) { ui_splash_static(); }  /* §6.7: the UI's static first screen (the void); its splash runs in
                                                * ui_frame's first 1.6 s, not blocking */
void app_redraw(void) { ui_redraw(); }

static void app_fill_ui(uint32_t now)
{
    cy_ui_state_t *s = &app_ui;
    uint32_t i, held = 0;
    for (i = 0; i < NKEYS; i++)                /* keys sounding a note (one held through PANIC is not) */
        if ((app.keys >> i & 1u) && app.note[i] >= 0)
            held |= 1u << i;
    for (i = 0; i < 128u; i++)                 /* SURFACE points held from MIDI show as held */
        if (app.midi_key[i] >= 0)
            held |= 1u << app.midi_key[i];
    s->now = now;
    s->norm = app_norm;
    s->page = app.page;
    s->layer = app.layer;
    s->exciter = (uint8_t)app_exciter();
    s->keymode = app.keymode;
    s->view = app.view;
    s->theme = (uint8_t)app_enum(P_THEME);
    s->reduced_motion = (uint8_t)app_enum(P_MOTION);
    s->frozen = app.frozen;
    s->latched = app.latched;
    s->damping = app.damping;
    s->edited = (uint8_t)(app.edited && !app.browsing && !app.compare);
    s->sel_held = app.layer;
    s->octave = app.octave;
    s->transpose = app.transpose;
    if (app.browsing) {                        /* the name appears at once (§5.1) */
        float v[P_NPARAMS];
        s->preset = app.browse;
        app_preset_values(app.browse, v, s->preset_name);
    } else {
        s->preset = app.preset;
        app_cpy(s->preset_name, app.name, sizeof s->preset_name);
    }
    s->focus_param = app.focus_param;
    s->focus_t = app.focus_t;
    s->focus_src = app.focus_src;
    s->save_state = app.save_state;
    s->save_slot = (uint16_t)(CY_NFACTORY + app.save_slot);   /* (as preset is: 24..55 the user slots) */
    s->morph_active = app.morphing;
    s->morph_t = app.morph_t;
    app_cpy(s->morph_a, app.morph_a, sizeof s->morph_a);
    app_cpy(s->morph_b, app.morph_b, sizeof s->morph_b);
    app_cpy(s->msg[0], app.msg[0], sizeof s->msg[0]);
    app_cpy(s->msg[1], app.msg[1], sizeof s->msg[1]);
    s->msg_until = app.msg_until;
    if (app.save_state == 3u) {                /* REPLACE: the occupant's name in msg[1] */
#ifdef UI_SAVE_REPLACE
        app_cpy(s->msg[0], "REPLACE", sizeof s->msg[0]);   /* (the UI draws "REPLACE Uxx?" and msg[1]) */
#else
        char t[8];                             /* (a UI that draws only 1 and 2: the footer message says it) */
        s->save_state = 0;
        app_preset_num(t, (int)s->save_slot, sizeof t);
        app_cpy(s->msg[0], "REPLACE ", sizeof s->msg[0]);
        app_cat(s->msg[0], t, sizeof s->msg[0]);
        app_cat(s->msg[0], "?", sizeof s->msg[0]);
        s->msg_until = now + 1000u;
#endif
        app_save_occupant(s->msg[1]);
    }
    s->hint_param = app.hint_param;
    s->tour_step = app.tour;
    s->grains = (uint16_t)(app_v(P_GRAINS) + 0.5f);
#ifndef PLAT_HOST
    s->cpu_pct = (uint8_t)(plat_cpu_pct() > 255u ? 255u : plat_cpu_pct());   /* (the UI paces the device's frames by
                                                                            * it, §6.9: 30, 60 or 20 fps) */
#else
    {                                          /* host and browser: the engine's model, of ~3,500 per sample */
        float pct = app_tel.est_instr * (100.0f / 3500.0f);
        s->cpu_pct = (uint8_t)(pct < 0.0f ? 0.0f : pct > 255.0f ? 255.0f : pct + 0.5f);
    }
#endif
    s->sprinkle_t = app.sprinkle_t;
    s->sweep_t = app.sweep_t;
    s->keys_held = held;
    for (i = 0; i < NKEYS; i++)
        s->key_press[i] = app.press_out[i];
    s->lfo = app_mod.lfo;
    s->orbit_u = app_mod.orbit_u;
    s->orbit_v = app_mod.orbit_v;
    s->press_max = app_mod.press_max;
    s->boot_t = app.boot_t;
}

#ifdef PLAT_WEB
/* the browser's boot ends with a soft C4, a "ting" (§6.7) */
static void app_ting(uint32_t now)
{
    int key = app.keymode ? 7 : -1;
    if (app.ting == 0 && now - app.boot_t >= 1500u) {
        cy_engine_note_on(key, 60, 40);
        app.ting = 1;
    } else if (app.ting == 1 && now - app.boot_t >= 1650u) {
        cy_engine_note_off(key, 60);
        app.ting = 2;
    }
}
#endif

#ifdef PLAT_HOST
static void app_host_frame(void);
#endif
/* the platform calls this every 16 ms (60 Hz). Everything here is cheap except drawing, and ui_frame paces its own
 * drawing on the device (§6.9: 30 fps, 60 when the render has room, 20 when it has none) */
void app_frame(void)
{
    uint32_t now = plat_ms(), sig;
    float dt;
    dt = (float)(now - app.last_frame) * 0.001f;
    dt = dt > 0.1f ? 0.1f : dt;                /* (a stall, a flash write: the LFO does not leap) */
    app.last_frame = now;
    app_input();                                                      /* 1 input, 2 MIDI in */
    ctl_tick(now);                                                    /*   (the clocks: settle, hold, tour, SAVE) */
    app_lfo(dt);                                                      /* 3 LFO */
    app_glide_step(now);                                              /* 4 glide */
    ctl_keymode_watch();
    app_patch_build();                                                /* 5 */
    app_geom_update(now);                                             /* 6 (the LFO's steps rate-limited) */
    cy_engine_set_patch(&app_patch);                                  /* 7 */
    cy_engine_telemetry_get(&app_tel);
    app_governor(now);                                                /* 8 governor */
    app.edited = (uint8_t)app_is_edited();
    if ((sig = app_edit_sig()) != app.edit_sig) {                     /*   (the edit buffer: autosaved) */
        app.edit_sig = sig;
        app_dirty();
    }
    led_update(now);                                                  /* 9 LEDs */
    app_fill_ui(now);                                                 /* 10 */
    ui_frame(&app_ui);                                                /* 11 (UI_POLL: keys and MIDI between strips) */
    app_speak(now);                                                   /* 12 speech */
    app_autosave(now);                                                /* 13 settings */
    app_sx_tick(now);                                                 /* 14 a USB question's footer and timeout */
#ifdef PLAT_WEB
    app_ting(now);
#endif
#ifdef PLAT_HOST
    app_host_frame();
#endif
}

/* ---------------------------------------------------- the page's accessors --- */
const uint8_t *cy_led_levels(void) { return led_lv; }
const uint8_t *cy_key_pressure(void) { return app.press_out; }

static void app_json_str(char *d, const char *s, uint32_t max)   /* a JSON string body: quotes and controls out */
{
    char t[2] = {0, 0};
    for (; s && *s; s++)
        if (*s != '"' && *s != '\\' && (uint8_t)*s >= 32u && (uint8_t)*s < 127u) {
            t[0] = *s;
            app_cat(d, t, max);
        }
}

/* <= 300 B of JSON for the page's caption and live region (§7.5); *seq changes when the text does */
const char *cy_info_json(uint32_t *seq)
{
    static char buf[320], prev[320], say[49];
    static uint32_t info_seq;
    uint32_t i, voices = 0;
    const uint32_t M = sizeof buf;
    int sh = (int)(app_v(P_SHAPE) + 0.5f);
    for (i = 0; i < CY_NVOICES; i++)
        voices += app_tel.voice_state[i] != CY_VS_FREE;
    app_cpy(say, app.speech, sizeof say);
    buf[0] = 0;
    app_cat(buf, "{\"preset\":\"", M), app_json_str(buf, app.name, M);
    app_cat(buf, "\",\"num\":", M), app_cat_int(buf, app.preset + 1, M);
    app_cat(buf, ",\"page\":\"", M), app_cat(buf, cy_page_name(app.page), M);
    app_cat(buf, "\",\"layer\":", M), app_cat_int(buf, app.layer, M);
    app_cat(buf, ",\"exciter\":\"", M), app_cat(buf, cy_engine_exciter_name(app_exciter()), M);
    app_cat(buf, "\",\"shape\":\"", M), app_cat(buf, cy_geom_shape_name(sh < 0 ? 0 : sh > 8 ? 8 : sh), M);
    app_cat(buf, "\",\"keys\":\"", M), app_cat(buf, app.keymode ? "SURFACE" : "TUNED", M);
    app_cat(buf, "\",\"view\":\"", M), app_cat(buf, app_view_name[app.view % CY_NVIEW], M);
    app_cat(buf, "\",\"octave\":", M), app_cat_int(buf, app.octave, M);
    app_cat(buf, ",\"frozen\":", M), app_cat_int(buf, app.frozen, M);
    app_cat(buf, ",\"latched\":", M), app_cat_int(buf, app.latched, M);
    app_cat(buf, ",\"voices\":", M), app_cat_int(buf, (int32_t)voices, M);
    app_cat(buf, ",\"est\":", M), app_cat_int(buf, (int32_t)(app_tel.est_instr + 0.5f), M);
    app_cat(buf, ",\"say\":\"", M), app_json_str(buf, say, M);
    app_cat(buf, "\",\"say_n\":", M), app_cat_int(buf, (int32_t)app.say_n, M);
    app_cat(buf, "}", M);
    for (i = 0; i < M && buf[i] == prev[i] && buf[i]; i++)
        ;
    if (buf[i] != prev[i]) {
        for (i = 0; i < M; i++)
            prev[i] = buf[i];
        info_seq++;
    }
    if (seq)
        *seq = info_seq;
    return buf;
}

/* a fixed 4 s performance, FNV-1a over the int32 output: host == host, host == wasm (§3.12). Main loop only, with
 * no render meanwhile (the host and the browser are one thread; the device never calls it, so it is not in the
 * device image at all: fix-app, 1.1 KB of RAM). It plays preset id (1..24) from a cold engine: strikes, a chord, a
 * pressure ramp and releases, all on block boundaries */
#if defined(PLAT_HOST) || defined(PLAT_WEB)
uint32_t cy_demo_hash(int id)
{
    static int32_t blk[2 * CY_BLOCK];
    enum { APP_DH_ON, APP_DH_OFF, APP_DH_PRESS };
    static const struct { uint16_t blk; uint8_t type; int8_t key; uint8_t v; } EV[] = {
        {0, APP_DH_ON, 7, 100}, {86, APP_DH_ON, 11, 90}, {172, APP_DH_ON, 14, 110}, {200, APP_DH_PRESS, 14, 30},
        {240, APP_DH_PRESS, 14, 70}, {280, APP_DH_PRESS, 14, 110}, {345, APP_DH_OFF, 7, 0}, {430, APP_DH_ON, 19, 70},
        {520, APP_DH_PRESS, 14, 20}, {690, APP_DH_OFF, 11, 0}, {690, APP_DH_OFF, 14, 0}, {860, APP_DH_OFF, 19, 0},
        {1000, APP_DH_ON, 0, 127}, {1100, APP_DH_OFF, 0, 0},
    };
    float norm[P_NPARAMS];
    cy_patch_t patch;
    cy_geom_in_t geom;
    cy_mod_t mod = {0.0f, 1.0f, 0.0f, 0.0f, 0};
    uint32_t h = 0x811C9DC5u, b, i, e = 0;
    cy_params_defaults(norm);
    cy_preset_load(&CY_FACTORY[((id > 0 ? id : 1) - 1) % CY_NFACTORY], norm);
    cy_patch_build(norm, &mod, &patch, &geom);
    cy_geom_update(&geom);
    cy_fx_init();
    cy_engine_init();
    cy_engine_seed(0x9E3779B9u);
    cy_engine_set_mode_budget(CY_BUDGET_DEFAULT);
    cy_engine_set_patch(&patch);
    for (b = 0; b < 1378u; b++) {              /* 4 s at 44.1 kHz in 128-frame blocks */
        for (; e < sizeof EV / sizeof EV[0] && EV[e].blk == b; e++) {
            int note = 53 + EV[e].key;
            if (EV[e].type == APP_DH_ON)
                cy_engine_note_on(EV[e].key, note, EV[e].v);
            else if (EV[e].type == APP_DH_OFF)
                cy_engine_note_off(EV[e].key, note);
            else
                cy_engine_pressure(EV[e].key, note, EV[e].v);
        }
        cy_engine_render(blk, CY_BLOCK, 4096u);
        for (i = 0; i < 2u * CY_BLOCK; i++) {
            uint32_t v = (uint32_t)blk[i], k;
            for (k = 0; k < 4u; k++)
                h = (h ^ ((v >> (8u * k)) & 0xFFu)) * 0x01000193u;
        }
    }
    cy_fx_init();                              /* live again, as the player left it */
    cy_engine_init();
    cy_engine_seed(0x9E3779B9u);
    cy_engine_set_mode_budget(app.budget);
    app_patch_build();
    app_geom_update(plat_ms());                /* (always given whole: the geometry holds the demo's until then) */
    cy_engine_set_patch(&app_patch);
    cy_engine_freeze(app.frozen);
    cy_engine_latch(app.latched);
    cy_engine_damp(app.damping);
    cy_engine_bend(app.bend);
    app.cp_sent = 0;                           /* (the engine was re-inited: its channel pressure is 0 again) */
    ctl_cp_apply();
    return h;
}
#endif

/* ------------------------------------------------------- the host's tests --- */
#ifdef PLAT_HOST
static FILE *app_wav, *app_wavf;
static uint32_t app_wav_frames, app_wav_atexit, app_wavf_frames, app_wavf_atexit;
uint32_t cy_fx_debug_tap(const float **lr);    /* fx.c, PLAT_HOST only: the float output before the quantiser */
static int32_t app_peakq, app_peakq_max;
static uint32_t app_render_us[1024], app_ui_us[1024], app_nr, app_nu;
static uint32_t app_modes_max, app_voices_max, app_nan_max;
static float app_est_max;

static void app_wav_head(FILE *f, uint32_t frames)
{
    uint32_t bytes = frames * 6u, v;
    uint16_t s;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f), v = 36u + bytes, fwrite(&v, 4, 1, f), fwrite("WAVEfmt ", 1, 8, f);
    v = 16, fwrite(&v, 4, 1, f), s = 1, fwrite(&s, 2, 1, f), s = 2, fwrite(&s, 2, 1, f);
    v = 44100, fwrite(&v, 4, 1, f), v = 44100u * 6u, fwrite(&v, 4, 1, f), s = 6, fwrite(&s, 2, 1, f);
    s = 24, fwrite(&s, 2, 1, f), fwrite("data", 1, 4, f), fwrite(&bytes, 4, 1, f);
    fseek(f, 0, SEEK_END);
}
/* the float twin of the capture (host `wavf`, integration): 32-bit IEEE float, the fx output just before the one
 * quantisation; golden.py's ultrasonic gate reads it (an undithered int24 render of a quiet note shows only the
 * container's own floor above 0.45 fs) */
static void app_wavf_head(FILE *f, uint32_t frames)
{
    uint32_t bytes = frames * 8u, v;
    uint16_t s;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f), v = 36u + bytes, fwrite(&v, 4, 1, f), fwrite("WAVEfmt ", 1, 8, f);
    v = 16, fwrite(&v, 4, 1, f), s = 3, fwrite(&s, 2, 1, f), s = 2, fwrite(&s, 2, 1, f);
    v = 44100, fwrite(&v, 4, 1, f), v = 44100u * 8u, fwrite(&v, 4, 1, f), s = 8, fwrite(&s, 2, 1, f);
    s = 32, fwrite(&s, 2, 1, f), fwrite("data", 1, 4, f), fwrite(&bytes, 4, 1, f);
    fseek(f, 0, SEEK_END);
}
static void app_wav_stop(void)
{
    if (!app_wav)
        return;
    app_wav_head(app_wav, app_wav_frames);
    fclose(app_wav);
    app_wav = 0;
}
static void app_wavf_stop(void)
{
    if (!app_wavf)
        return;
    app_wavf_head(app_wavf, app_wavf_frames);
    fclose(app_wavf);
    app_wavf = 0;
}

static void app_host_tap(const int32_t *lr, uint32_t n, uint32_t us)   /* the exact 24-bit output, and its peak */
{
    uint32_t i;
    for (i = 0; i < 2u * n; i++) {
        int32_t v = lr[i], a = v < 0 ? -v : v;
        if (a > app_peakq)
            app_peakq = a;
        if (a > app_peakq_max)
            app_peakq_max = a;
        if (app_wav) {
            uint8_t t[3];
            v = clamp(v, -8388608, 8388607);
            t[0] = (uint8_t)v, t[1] = (uint8_t)(v >> 8), t[2] = (uint8_t)(v >> 16);
            fwrite(t, 1, 3, app_wav);
        }
    }
    if (app_wav)
        app_wav_frames += n;
    {   /* always drained, so a capture starts with this render's samples */
        const float *f;
        uint32_t m = cy_fx_debug_tap(&f);
        if (app_wavf && m)
            fwrite(f, 4, 2u * m, app_wavf), app_wavf_frames += m;
    }
    app_render_us[app_nr++ % 1024u] = us;
}

static uint32_t app_nan(void)
{
    cy_fx_meter_t m;
    cy_fx_meter(&m);
    return app_tel.nan_resets + m.nan_resets;
}

static void app_host_frame(void)
{
    uint32_t i, v = 0;
    for (i = 0; i < CY_NVOICES; i++)
        v += app_tel.voice_state[i] != CY_VS_FREE;
    if (v > app_voices_max)
        app_voices_max = v;
    if (app_tel.modes_used > app_modes_max)
        app_modes_max = app_tel.modes_used;
    if (app_nan() > app_nan_max)
        app_nan_max = app_nan();
    if (app_tel.est_instr > app_est_max)
        app_est_max = app_tel.est_instr;
    app_ui_us[app_nu++ % 1024u] = ui_frame_us();
}

static int app_cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}
static void app_stats(const uint32_t *ring, uint32_t count, uint32_t *med, uint32_t *p99, uint32_t *mx)
{
    static uint32_t t[1024];
    uint32_t n = count < 1024u ? count : 1024u;
    *med = *p99 = *mx = 0;
    if (!n)
        return;
    memcpy(t, ring, n * sizeof t[0]);
    qsort(t, n, sizeof t[0], app_cmp_u32);
    *med = t[n / 2u], *p99 = t[n * 99u / 100u], *mx = t[n - 1u];
}

static int app_btn_index(const char *s)
{
    static const char *const N[NB] = {"FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ",
                                      "PLAY", "REC", "OCT-", "OCT+"};
    int i;
    if (!strcmp(s, "PLAYG"))                   /* PLAY's green LED */
        return 14;
    for (i = 0; i < NB; i++)
        if (!strcmp(s, N[i]))
            return i;
    return -1;
}

static int app_param_named(const char *s)      /* "LFO_RATE" is LFO RATE; -1 none */
{
    char name[64];
    int i;
    for (i = 0; s[i] && i < 63; i++)
        name[i] = s[i] == '_' ? ' ' : s[i];
    name[i] = 0;
    for (i = 0; i < P_NPARAMS; i++)
        if (!strcmp(CY_PARAMS[i].name, name))
            return i;
    return -1;
}

/* spec §8.1, plus what the controls test reads: p_NAME (the value in display units x 100), n_NAME (norm x 1000),
 * hints_NAME, userN (U0N saved), midikeyN (where MIDI note N went: -1 TUNED, a point, -2 not held) */
int app_expect(const char *what, int *got)
{
    int i, v = 0, id;
    for (i = 0; i < CY_NVOICES; i++)
        v += app_tel.voice_state[i] != CY_VS_FREE;
    if (!strcmp(what, "voices")) *got = v;
    else if (!strcmp(what, "modes")) *got = app_tel.modes_used;
    else if (!strcmp(what, "exciter")) *got = app_exciter();
    else if (!strcmp(what, "page")) *got = app.page;
    else if (!strcmp(what, "layer")) *got = app.layer;
    else if (!strcmp(what, "view")) *got = app.view;
    else if (!strcmp(what, "preset")) *got = (app.browsing ? app.browse : app.preset) + 1;   /* as shown */
    else if (!strcmp(what, "loaded")) *got = app.preset + 1;
    else if (!strcmp(what, "keymode")) *got = app.keymode;
    else if (!strcmp(what, "frozen")) *got = app.frozen;
    else if (!strcmp(what, "latched")) *got = app.latched;
    else if (!strcmp(what, "damping")) *got = app.damping;
    else if (!strcmp(what, "octave")) *got = app.octave;
    else if (!strcmp(what, "transpose")) *got = app.transpose;
    else if (!strcmp(what, "edited")) *got = app_is_edited();
    else if (!strcmp(what, "nan")) *got = (int)app_nan();
    else if (!strcmp(what, "budget")) *got = app_tel.mode_budget;
    else if (!strcmp(what, "tour")) *got = app.tour;
    else if (!strcmp(what, "tour_done")) *got = app_set.tour_done;
    else if (!strcmp(what, "peakq")) *got = app_peakq;
    else if (!strcmp(what, "compare")) *got = app.compare;
    else if (!strcmp(what, "glide")) *got = app.gliding;
    else if (!strcmp(what, "browsing")) *got = app.browsing;
    else if (!strcmp(what, "save_state")) *got = app.save_state;
    else if (!strcmp(what, "save_slot")) *got = app.save_slot;
    else if (!strcmp(what, "morph")) *got = app.morphing;
    else if (!strcmp(what, "morph_t16")) *got = (int)(app.morph_t * 16.0f + 0.5f);
    else if (!strcmp(what, "morph_a")) *got = app.morph_ia + 1;
    else if (!strcmp(what, "morph_b")) *got = app.morph_ib + 1;
    else if (!strcmp(what, "focus")) *got = app.focus_param;
    else if (!strcmp(what, "focus_src")) *got = app.focus_src;
    else if (!strcmp(what, "hint")) *got = app.hint_param;
    else if (!strcmp(what, "sprinkles")) *got = (int)app.sprinkles;
    else if (!strcmp(what, "sweeps")) *got = (int)app.sweeps;
    else if (!strcmp(what, "setdirty")) *got = app.set_dirty;
    else if (!strcmp(what, "setsaves")) *got = (int)app.set_saves;
    else if (!strcmp(what, "say_n")) *got = (int)app.say_n;
    else if (!strcmp(what, "master")) *got = (int)APP_GAIN(plat_master());   /* MASTER as the render gets it */
    else if (!strcmp(what, "bend100")) *got = (int)(app.bend * 100.0f + (app.bend < 0.0f ? -0.5f : 0.5f));
    else if (!strcmp(what, "gov_voices")) *got = app.gov_voices;              /* (fix-app's) */
    else if (!strcmp(what, "max_voices")) *got = app_patch.max_voices;
    else if (!strcmp(what, "geom_pubs")) *got = (int)app.geo_pubs;
    else if (!strcmp(what, "local_off")) *got = app.local_off;
    else if (!strcmp(what, "latch_btn")) *got = app.latch_btn;
    else if (!strcmp(what, "latch_pedal")) *got = app.latch_pedal;
    else if (!strcmp(what, "bank_locked")) *got = app.bank_locked;
    else if (!strcmp(what, "undo")) *got = app.undo_ok;
    else if (!strcmp(what, "mem")) *got = app.mem_ok ? app.mem_preset + 1 : 0;
    else if (!strcmp(what, "mute")) *got = app_mute;
    else if (!strcmp(what, "save_pending")) *got = app.save_pending;
    else if (!strcmp(what, "cp")) *got = app.cp_sent;
    else if (!strcmp(what, "midi_held")) *got = app.midi_held;
    else if (!strcmp(what, "tour_mask")) *got = app.tour_mask;
    else if (!strcmp(what, "surface_root")) *got = (int)(app_patch.surface_root + 0.5f);
    else if (!strcmp(what, "set_version")) *got = app_set.version;
    else if (!strcmp(what, "set_fails")) *got = app.set_fails;
    else if (!strcmp(what, "auditioned")) *got = app.auditioned;             /* (CYMA-1 Sounds, §6.7) */
    else if (!strcmp(what, "user_mask")) *got = (int)app_bank.used_mask;
    else if (!strcmp(what, "user_writes") || !strcmp(what, "sx_state") || !strcmp(what, "sx_status") ||
             !strcmp(what, "sx_cmd") || !strcmp(what, "sx_replies") || !strcmp(what, "sx_result") ||
             !strcmp(what, "sx_staged") || !strcmp(what, "sx_taken") || !strcmp(what, "sx_crc_stale"))
        *got = app_sx_expect(what);
    else if (!strcmp(what, "notes")) {         /* keys sounding a note */
        *got = 0;
        for (i = 0; i < NKEYS; i++)
            *got += app.note[i] >= 0;
    } else if (!strcmp(what, "midi_notes")) {
        *got = 0;
        for (i = 0; i < 128; i++)
            *got += app.midi_key[i] != -2;
    } else if (!strncmp(what, "pressk", 6) && what[6]) *got = app.press_out[atoi(what + 6) % NKEYS];
    else if (!strncmp(what, "midikey", 7)) *got = app.midi_key[atoi(what + 7) & 127];
    else if (!strncmp(what, "user", 4) && what[4] >= '0' && what[4] <= '9') *got = app_user_used(atoi(what + 4) - 1);
    else if (!strncmp(what, "ledk", 4) && what[4]) *got = led_lv[atoi(what + 4) % NKEYS];
    else if (!strncmp(what, "ledb_", 5) && app_btn_index(what + 5) >= 0) *got = led_lv[27 + app_btn_index(what + 5)];
    else if (!strncmp(what, "p_", 2) && (id = app_param_named(what + 2)) >= 0) {
        float x = app_v(id) * 100.0f;
        *got = (int)(x + (x < 0.0f ? -0.5f : 0.5f));
    } else if (!strncmp(what, "n_", 2) && (id = app_param_named(what + 2)) >= 0)
        *got = (int)(app_norm[id] * 1000.0f + 0.5f);
    else if (!strncmp(what, "hints_", 6) && (id = app_param_named(what + 6)) >= 0)
        *got = app_set.hint_count[id];
    else
        return 0;
    return 1;
}

/* spec §8.1 (+ governor / cpu / overload, the governor under test); 1 = handled (the platform never sees it) */
int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d)
{
    (void)c, (void)d;
    if (!strcmp(cmd, "wav24")) {
        char path[600];
        app_wav_stop();
        if (!strncmp(a, "out:", 4))
            snprintf(path, sizeof path, "%s/%s", plat_host_dir, a + 4);
        else
            snprintf(path, sizeof path, "%s", a);
        app_wav = fopen(path, "wb");
        app_wav_frames = 0;
        if (!app_wav) {
            printf("FAIL wav24 %s: cannot open\n", path);
            exit(2);
        }
        app_wav_head(app_wav, 0);
        if (!app_wav_atexit++)
            atexit(app_wav_stop);              /* (a script that ends recording still gets a whole file) */
    } else if (!strcmp(cmd, "wav24stop")) {
        app_wav_stop();
    } else if (!strcmp(cmd, "wavf")) {         /* (integration) the float twin of wav24: see app_wavf_head */
        char path[600];
        app_wavf_stop();
        if (!strncmp(a, "out:", 4))
            snprintf(path, sizeof path, "%s/%s", plat_host_dir, a + 4);
        else
            snprintf(path, sizeof path, "%s", a);
        app_wavf = fopen(path, "wb");
        app_wavf_frames = 0;
        if (!app_wavf) {
            printf("FAIL wavf %s: cannot open\n", path);
            exit(2);
        }
        app_wavf_head(app_wavf, 0);
        if (!app_wavf_atexit++)
            atexit(app_wavf_stop);
    } else if (!strcmp(cmd, "wavfstop")) {
        app_wavf_stop();
    } else if (!strcmp(cmd, "preset")) {       /* 1..56, at once */
        float v[P_NPARAMS];
        int n = atoi(a);
        if (n < 1 || n > APP_NPRE || !app_preset_values(n - 1, v, 0)) {
            printf("FAIL preset %s: 1..%d (an empty user slot has none)\n", a, APP_NPRE);
            exit(2);
        }
        ctl_morph_end();
        ctl_compare(0);
        app.browsing = 0;
        app.mem_ok = 0;                        /* (a test's direct load: no edits kept from before) */
        app_preset_load(n - 1, 0);
    } else if (!strcmp(cmd, "param")) {        /* NAME VALUE, display units (an enum: its index) */
        int i = app_param_named(a);
        if (i < 0) {
            printf("FAIL param %s: no such parameter\n", a);
            exit(2);
        }
        app_set_param(i, cy_param_norm(i, (float)atof(b)));
        ctl_keymode_watch();
    } else if (!strcmp(cmd, "seed")) {
        cy_engine_seed((uint32_t)strtoul(a, 0, 0));
    } else if (!strcmp(cmd, "budget")) {
        app_budget_set(atoi(a));
    } else if (!strcmp(cmd, "perf")) {
        uint32_t um, up, ux, rm, rp, rx;
        app_stats(app_ui_us, app_nu, &um, &up, &ux);
        app_stats(app_render_us, app_nr, &rm, &rp, &rx);
        printf("perf %u %u %u %u %u %u\n", um, up, ux, rm, rx, (uint32_t)app_est_max);
    } else if (!strcmp(cmd, "metrics")) {
        printf("{\"peak_q23_max\":%d,\"nan\":%u,\"modes_max\":%u,\"est_max\":%u,\"voices_max\":%u}\n",
               app_peakq_max, app_nan_max, app_modes_max, (uint32_t)app_est_max, app_voices_max);
    } else if (!strcmp(cmd, "demo_hash")) {
        int n = atoi(a);
        printf("demo_hash %d 0x%08X\n", n, cy_demo_hash(n));
    } else if (!strcmp(cmd, "shotmode")) {
        ui_shot_mode(atoi(a));
    } else if (!strcmp(cmd, "tour")) {         /* N: that step now, the ones before it done (0 off) */
        int st = clamp(atoi(a), CY_TOUR_OFF, CY_TOUR_DONE);
        if (st == CY_TOUR_OFF)
            app.tour = CY_TOUR_OFF;
        else
            ctl_tour_start(plat_ms(), st);
    } else if (!strcmp(cmd, "press_all")) {    /* P: 0..127 on every held sensed key (touch's 0..100 % lean) */
        uint32_t k, p = (uint32_t)clamp(atoi(a), 0, 127) * 100u / 127u;
        for (k = 0; k < NKEYS; k++)
            if ((plat_keys() >> k & 1u) && touch_sensed(k))
                app_host_key(k, app.vel[k] ? app.vel[k] : 100u, p);
    } else if (!strcmp(cmd, "governor")) {     /* 1: the device's governor runs on the host, fed by "cpu N" */
        app_gov_on = (uint8_t)(atoi(a) != 0);
        app.gov_t = app.gov_low_t = plat_ms();
    } else if (!strcmp(cmd, "cpu")) {
        app_gov_cpu = (uint8_t)clamp(atoi(a), 0, 255);
    } else if (!strcmp(cmd, "geompubs")) {     /* N: the geometry's SHAPE / POS publish count, to measure from */
        app.geo_pubs = (uint32_t)atoi(a);
    } else if (!strcmp(cmd, "peak")) {         /* N: a block's load per mille, as app_render measures it on the device */
        app_gov_pk = (uint16_t)clamp(atoi(a), 0, 4000);
    } else if (!strcmp(cmd, "setsaves")) {     /* N: the autosave count, to measure from */
        app.set_saves = (uint32_t)atoi(a);
    } else if (!strcmp(cmd, "overload")) {     /* an overloaded block, as the device reports one */
        app_overload();
    } else if (!strcmp(cmd, "sysex") || !strcmp(cmd, "sysexf") || !strcmp(cmd, "params_json")) {
        app_sx_host_cmd(cmd, a);               /* (CYMA-1 Sounds, §6.7: a USB SysEx frame in; the parameter table) */
    } else if (!strcmp(cmd, "info")) {         /* the page's caption JSON (cy_info_json), and the LED levels */
        uint32_t i, s;
        const char *j = cy_info_json(&s);
        printf("info %s (%u B, seq %u)\nlevels ", j, (unsigned)strlen(j), s);
        for (i = 0; i < 42u; i++)
            printf("%u%s", led_lv[i], i == 26u || i == 40u ? " " : "");
        printf("\n");
    } else {
        if (!strcmp(cmd, "peakreset"))         /* the platform's too: it restarts its own peak */
            app_peakq = 0;
        return 0;
    }
    return 1;
}
#endif

/* ------------------------------------------------- CYMA-1 Sounds (§6.3) --- */
#include "app_sounds.c"                        /* the SysEx handler and the page's sound accessors (a unity part) */
