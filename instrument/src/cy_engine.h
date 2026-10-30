/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 src/cy_engine.h: M2, FROZEN CONTRACT (verbatim from docs/CYMA-1-spec.md 9.3). */
#pragma once
#include "cy_config.h"
#include "cy_plates.h"
typedef struct {
    uint8_t exciter, surface, max_voices, modes_per_voice;
    float decay_mul, hardness, tension_st, palm, force, grit, swell_s, density, noise, damper, bloom, stretch;
    float vibrato_cents, vib_rate_hz, tune_cents, level_db, surface_root;
    float drive_db, tilt_db, room, size_s, damping, predelay_ms;
} cy_patch_t;
enum { CY_VS_FREE, CY_VS_HELD, CY_VS_RING, CY_VS_BOWED, CY_VS_FROZEN };
typedef struct {
    uint32_t seq;
    float mode_energy[CY_NMODES];
    float voice_level[CY_NVOICES];
    uint8_t voice_press[CY_NVOICES];
    int8_t voice_key[CY_NVOICES];
    uint8_t voice_state[CY_NVOICES];
    uint8_t voice_note[CY_NVOICES];
    float key_level[CY_NKEYS];
    float newest_hz;
    float peak_l, peak_r, gr_db, bow_force, est_instr;
    uint16_t modes_used, mode_budget, nan_resets;
    uint8_t frozen, surface, latched, damping;
} cy_telemetry_t;
typedef struct { uint8_t type, vel; uint16_t u, v; } cy_uievent_t;
enum { CY_UIEV_STRIKE = 1, CY_UIEV_DROP = 2, CY_UIEV_SLIP = 3, CY_UIEV_TOUCH = 4 };
void cy_engine_init(void);
void cy_engine_seed(uint32_t seed);
void cy_engine_set_patch(const cy_patch_t *p);
void cy_engine_note_on(int key, int note, int vel);
void cy_engine_note_on_midi(int key, int note, int vel);   /* (additive, review round) cy_engine_note_on for a note
                                                           * played from MIDI: a SURFACE point takes the MIDI P_eff
                                                           * (v sw + (1 - v sw) P) and channel pressure */
void cy_engine_note_off(int key, int note);
void cy_engine_pressure(int key, int note, int press);
void cy_engine_channel_pressure(int press);
void cy_engine_bend(float semitones);
void cy_engine_freeze(int on);
void cy_engine_latch(int on);
void cy_engine_damp(int on);
void cy_engine_panic(void);
void cy_engine_set_mode_budget(int n);
void cy_engine_set_bow_cap(int n);      /* (v1.1) the device governor's cap on held bowed notes, 1..CY_MAX_BOWED */
void cy_engine_set_tail_cap(int n);     /* (v1.1) the device governor's cap on released notes still ringing, 0..8 */
void cy_engine_tails_off(void);         /* (v1.1) the device governor: released notes ringing now fade over 5 ms */
void cy_engine_restart(int hand);       /* (v1.1.1, additive) everything heard fades out over 8.7 ms with the patch and
                                         * plate it was playing, then the engine and the room start again from cold (as
                                         * cy_engine_init and cy_fx_init leave them; the queue kept) with, as a first
                                         * patch: hand 0, the patch in force then; hand 1, the next one set after
                                         * cy_engine_restart_ready() says so (it stays silent until then). The events
                                         * after it wait for that. Publish no geometry or patch meanwhile */
int cy_engine_restart_ready(void);      /* (v1.1.1) a hand-over restart has faded out: set the geometry, then the patch */
void cy_engine_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void cy_engine_telemetry_get(cy_telemetry_t *out);
int cy_engine_event(cy_uievent_t *e);
const char *cy_engine_exciter_name(int exc);
