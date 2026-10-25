/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 ui/ui.h: M5, FROZEN CONTRACT (verbatim from docs/CYMA-1-spec.md 9.6). */
#pragma once
#include <stdint.h>
#include "src/cy_params.h"
enum { CY_VIEW_SAND, CY_VIEW_FIELD, CY_VIEW_MESH, CY_NVIEW };
enum { CY_TOUR_OFF, CY_TOUR_KEY, CY_TOUR_LEAN, CY_TOUR_SELECT, CY_TOUR_ALGO, CY_TOUR_PRESET, CY_TOUR_DONE };
typedef struct {
    uint32_t now;
    const float *norm;                   /* P_NPARAMS live values */
    uint8_t page, layer, exciter, keymode, view, theme, reduced_motion;
    uint8_t frozen, latched, damping, edited, sel_held;
    int8_t octave, transpose;
    uint16_t preset;                     /* 0..23 factory, 24..55 user */
    char preset_name[16];
    int8_t focus_param;  uint32_t focus_t;  uint8_t focus_src;   /* 0 knob 1 SELECT 2 ALGO 3 PRESETS */
    uint8_t save_state;  uint16_t save_slot;
    uint8_t morph_active; float morph_t; char morph_a[16], morph_b[16];
    char msg[2][24]; uint32_t msg_until;
    int8_t hint_param;
    uint8_t tour_step;
    uint16_t grains;
    uint32_t sprinkle_t, sweep_t;
    uint8_t cpu_pct;
    uint32_t keys_held; uint8_t key_press[CY_NKEYS];
    float lfo, orbit_u, orbit_v, press_max;
    uint32_t boot_t;                     /* ms of app_ui_init: splash timing */
} cy_ui_state_t;
void ui_init(void);
void ui_frame(const cy_ui_state_t *st);
void ui_redraw(void);
void ui_splash_static(void);
uint32_t ui_frame_us(void);
uint32_t ui_frame_us_max(void);
void ui_shot_mode(int on);
