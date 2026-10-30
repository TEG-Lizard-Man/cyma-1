/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 src/cy_fx.h: M3, FROZEN CONTRACT (verbatim from docs/CYMA-1-spec.md 9.4). */
#pragma once
#include "cy_config.h"
typedef struct { float drive_db, tilt_db, room, size_s, damping, predelay_ms; } cy_fx_params_t;
typedef struct { float peak_l, peak_r, gr_db; uint32_t nan_resets; } cy_fx_meter_t;
void cy_fx_init(void);
void cy_fx_set(const cy_fx_params_t *p);
void cy_fx_process(float *l, float *r, uint32_t n, uint32_t master_q12, int32_t *lr);
void cy_fx_reset(void);
void cy_fx_clear(void);   /* (v1.1.1, additive) cy_fx_reset, but the reverb's two drifting lines go on as they were */
void cy_fx_snap(void);    /* (v1.1.1, additive) the next block takes the targets at once; the audio state untouched */
void cy_fx_meter(cy_fx_meter_t *m);
uint32_t cy_fx_latency(void);
