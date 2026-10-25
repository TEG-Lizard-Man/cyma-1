/* SPDX-License-Identifier: MPL-2.0 */
/* web.c: CYMA-1's page exports, compiled into app.wasm by $B/web/emu/emu_web.c (APP_WEB) */
/* Spec §7.5 (M6). Not a unit of its own: emu_web.c includes it at its very end (app.h APP_WEB), after the whole
 * simulator and app.c, with __attribute__ restored, so app.c's accessors and the src/ headers are in scope. These
 * sit beside fm1-base's own web_* exports (the screen, the store, MIDI, web_render), which the page and worklet.js
 * keep using unchanged; what CYMA-1 adds is what the generic ones cannot say:
 *   web_cy_leds    the 41 LEDs at their four levels (fm1-base's web_lit_* / web_dim_* are two bits, and lose the
 *                  mid level the SURFACE vibration map and the OCT/EDIT LEDs need), and PLAY's green apart;
 *   web_cy_press   the pressure each key is played with, as the firmware reads it (the page's magenta halos show
 *                  what the instrument heard, not what the mouse asked for);
 *   web_cy_info    a line of JSON for the caption under the panel and the screen reader's "say" text; its seq
 *                  changes only when the text does, so the worklet sends it to the page only then;
 *   web_cy_demo_hash  spec §3.12's determinism check, the same fixed performance the host's demo_hash renders;
 *   web_cy_fs      the rate the device renders at (CY_FS, 44100 in this host build): the worklet resamples when the
 *                  browser runs its AudioContext at another rate, and writes WAVs at this one;
 *   web_cy_reduce_motion  the viewer's prefers-reduced-motion on a first boot (no saved settings): the firmware's
 *                  own MOTION global (GLOBAL page) goes to REDUCE through app_set_param, as a knob would set it, so
 *                  it is saved with the other settings and the player can turn it back. (app.c's statics are in
 *                  scope: plat_host.c includes app.c, and emu_web.c includes this file after it.)
 * CYMA-1 Sounds (docs/CYMA-1-sounds-spec.md §7, version 3): the page's sound operations, through the same accessors
 * as the USB SysEx handler (app_sounds.c), and that handler itself for the loopback tests and the virtual FM-1:
 *   web_cy_box     3,072 B both ways: a cy_preset_t (94 B: name[14], then 40 u16 little-endian) or a cy_user_bank_t
 *                  (3,020 B: magic "CMA1", version 1, rsv, used_mask, then 32 presets), the store's own layouts;
 *   web_cy_state   the state word: 0-7 the loaded preset (0..23 factory, 24..55 user), 8 edited, 9 COMPARE held,
 *                  10 browsing, 11 the SAVE dialog, 12 gliding, 13 auditioned, 14 bank locked, 15 a USB question,
 *                  16 a SAVE write pending;
 *   web_cy_cur / _audition / _load / _save / _put / _clear / _bank   0 ok, -1 a bad argument or an empty slot, -2 the
 *                  store failed (kept in RAM), -3 busy (COMPARE held, a USB question open), -4 the bank is locked
 *                  (cur returns the loaded preset);
 *   web_cy_factory the 24 factory sounds (24 x 94 B); web_cy_params the parameter table JSON (tools/cyma1_params.json
 *                  is this, committed); web_cy_app_version APP_VERSION;
 *   web_cy_sx_in / web_cy_sx   a SysEx frame (the bytes between F0 and F7, up to 640) to app_sysex: 1 = taken; its
 *                  replies (and a COMMIT's later result) as whole F0..F7 frames from web_cy_sx_out, web_cy_sx_out_len
 *                  bytes, until web_cy_sx_out_clear. */
__attribute__((used, visibility("default"))) const uint8_t *web_cy_leds(void) { return cy_led_levels(); }      /* 42 B */
__attribute__((used, visibility("default"))) const uint8_t *web_cy_press(void) { return cy_key_pressure(); }   /* 27 B */
static uint32_t web_cy_seq_;
__attribute__((used, visibility("default"))) const char *web_cy_info(void) { return cy_info_json(&web_cy_seq_); }
__attribute__((used, visibility("default"))) uint32_t web_cy_info_seq(void) { (void)cy_info_json(&web_cy_seq_); return web_cy_seq_; }
__attribute__((used, visibility("default"))) uint32_t web_cy_demo_hash(int id) { return cy_demo_hash(id); }
__attribute__((used, visibility("default"))) uint32_t web_cy_version(void) { return 3u; }   /* 3: + Sounds */
__attribute__((used, visibility("default"))) uint32_t web_cy_fs(void) { return (uint32_t)(CY_FS + 0.5f); }   /* Hz */
__attribute__((used, visibility("default"))) void web_cy_reduce_motion(int on)
{
    app_set_param(P_MOTION, app_enum_norm(P_MOTION, on ? 1 : 0));
}

/* ---- CYMA-1 Sounds (§7.1, frozen I4b) */
static union { cy_preset_t p; cy_user_bank_t b; uint8_t u8[3072]; } web_cy_box_;
_Static_assert(sizeof(cy_preset_t) == 94 && sizeof(cy_user_bank_t) == 3020, "the page's byte layouts");
static uint8_t web_cy_sx_in_[640];
__attribute__((used, visibility("default"))) uint8_t *web_cy_box(void) { return web_cy_box_.u8; }
__attribute__((used, visibility("default"))) uint32_t web_cy_state(void) { return app_snd_state(); }
__attribute__((used, visibility("default"))) int web_cy_cur(void) { return app_snd_cur(&web_cy_box_.p); }
__attribute__((used, visibility("default"))) int web_cy_audition(int flags)
{
    return app_snd_audition(&web_cy_box_.p, flags & 3, 0);   /* (b0 glide, b1 plain edits; never "FROM USB") */
}
__attribute__((used, visibility("default"))) int web_cy_load(int idx, int glide) { return app_snd_load(idx, glide); }
__attribute__((used, visibility("default"))) int web_cy_save(int slot)
{
    char nm[CY_PRESET_NAME];                   /* (only the box's name is read: the sound saved is the one playing) */
    int i;
    for (i = 0; i < CY_PRESET_NAME; i++)
        nm[i] = web_cy_box_.p.name[i];
    return app_snd_save(slot, nm);
}
__attribute__((used, visibility("default"))) int web_cy_put(int slot) { return app_snd_put(slot, &web_cy_box_.p); }
__attribute__((used, visibility("default"))) int web_cy_clear(int slot) { return app_snd_put(slot, 0); }
__attribute__((used, visibility("default"))) int web_cy_bank(void) { return app_snd_bank(&web_cy_box_.b); }
__attribute__((used, visibility("default"))) const uint8_t *web_cy_factory(void) { return (const uint8_t *)CY_FACTORY; }
__attribute__((used, visibility("default"))) const char *web_cy_params(void)
{
    static char buf[16384];                    /* (made once: CY_PARAMS and CY_FACTORY are const) */
    if (!buf[0])
        cy_params_json(buf, sizeof buf);
    return buf;
}
__attribute__((used, visibility("default"))) const char *web_cy_app_version(void) { return APP_VERSION; }
__attribute__((used, visibility("default"))) uint8_t *web_cy_sx_in(void) { return web_cy_sx_in_; }
__attribute__((used, visibility("default"))) int web_cy_sx(uint32_t n)
{
    return n <= sizeof web_cy_sx_in_ ? app_sysex(web_cy_sx_in_, n) : 0;   /* (longer: the USB layer drops it) */
}
__attribute__((used, visibility("default"))) const uint8_t *web_cy_sx_out(void) { return app_sx_out; }
__attribute__((used, visibility("default"))) uint32_t web_cy_sx_out_len(void) { return app_sx_out_n; }
__attribute__((used, visibility("default"))) void web_cy_sx_out_clear(void) { app_sx_out_n = 0; }
