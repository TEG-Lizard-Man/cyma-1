/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 src/cy_params.h: M4, FROZEN CONTRACT (verbatim from docs/CYMA-1-spec.md 9.5). */
#pragma once
#include "cy_config.h"
#include "cy_plates.h"
#include "cy_engine.h"
enum {
    P_SHAPE, P_EXCITER, P_KEYMODE,
    P_MATERIAL, P_STIFFNESS, P_CARVE, P_DECAY,
    P_CARVE_TARGET, P_BRIGHT, P_SHIMMER, P_MODES,
    P_HARDNESS, P_TENSION, P_FORCE, P_GRIT, P_DENSITY, P_SPREAD, P_POS_X, P_POS_Y,
    P_NOISE, P_DAMPER, P_BLOOM, P_PALM, P_SWELL,
    P_MICS, P_ROOM, P_SIZE, P_DRIVE,
    P_PREDELAY, P_DAMPING, P_TILT, P_LEVEL,
    P_WANDER, P_MORPH_DEPTH, P_LFO_RATE, P_STRETCH,
    P_LFO_WAVE, P_LFO_POS, P_VIBRATO, P_VIB_RATE,
    P_NPRESET,
    P_VEL_CURVE = P_NPRESET, P_PRESS_SENS, P_TUNE, P_VOICES,
    P_THEME, P_GRAINS, P_MOTION, P_DETAIL,
    P_NPARAMS
};
enum { PG_PLATE, PG_EXCITE, PG_SPACE, PG_MOTION, PG_GLOBAL, PG_N };
enum { CY_CV_LIN, CY_CV_LOG, CY_CV_ENUM, CY_CV_MORPH };
enum { CY_LFO_SINE, CY_LFO_TRI, CY_LFO_SH, CY_LFO_DRIFT };
typedef struct {
    const char *name, *unit;
    float lo, hi, def;
    uint8_t curve, detents, bipolar, page;
    const char *const *labels;
    const char *hint;
} cy_param_def_t;
extern const cy_param_def_t CY_PARAMS[P_NPARAMS];
int cy_page_param(int page, int exciter, int layer, int knob);
float cy_param_value(int id, float norm);
float cy_param_norm(int id, float value);
float cy_param_step(int id, float norm, float detents, uint8_t *sticky);
void cy_param_format(int id, float norm, char *buf, int n, int longform);
const char *cy_page_name(int page);
#define CY_PRESET_NAME 14
typedef struct { char name[CY_PRESET_NAME]; uint16_t v[P_NPRESET]; } cy_preset_t;
#define CY_NFACTORY 24
#define CY_NUSER 32
#define CY_USER_MAGIC 0x31414D43u
typedef struct { uint32_t magic; uint16_t version, rsv; uint32_t used_mask; cy_preset_t p[CY_NUSER]; } cy_user_bank_t;
extern const cy_preset_t CY_FACTORY[CY_NFACTORY];
void cy_preset_load(const cy_preset_t *p, float *norm);
void cy_preset_store(const float *norm, const char *name, cy_preset_t *p);
void cy_preset_lerp(const float *a, const float *b, float t, float *out);
void cy_params_defaults(float *norm);
typedef struct { float lfo, orbit_u, orbit_v, press_max; int8_t octave; } cy_mod_t;
void cy_patch_build(const float *norm, const cy_mod_t *mod, cy_patch_t *patch, cy_geom_in_t *geom);
float cy_lfo_step(float *phase, uint32_t *rng, float *state, int wave, float rate_hz, float dt);
