/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 src/cy_plates.h: M1, FROZEN CONTRACT (verbatim from docs/CYMA-1-spec.md 9.2). */
#pragma once
#include "cy_config.h"
typedef struct {
    char name[12];
    uint8_t clamped;                            /* 1 Dirichlet (DRUM) */
    uint8_t rsv[3];
    float log2_lambda[CY_NMODES];               /* tracked order; >= 0 */
    float monopole[CY_NMODES];
    uint8_t degen[CY_NMODES];
    int8_t sdf[CY_SDF][CY_SDF];                 /* plate units * 128, < 0 inside */
    int8_t field[CY_NMODES][CY_GRID][CY_GRID];  /* [mode][row v][col u], max |phi| 127 */
} cy_plate_t;
extern const cy_plate_t CY_PLATES[CY_NSHAPES];
typedef struct {
    float shape, stiffness, carve;
    uint8_t carve_target, surface, rsv[2];
    float shimmer_cents, material, bright;
    float pos_u, pos_v, mic_spread, rain_spread, wander;
} cy_geom_in_t;
typedef struct {
    uint32_t seq;
    float ratio[CY_NMODES], log2_ratio[CY_NMODES], damp_div[CY_NMODES];
    float t0;
    float strike[CY_NMODES], mic_l[CY_NMODES], mic_r[CY_NMODES];
    float out_norm;
    uint8_t order[CY_NMODES], degen[CY_NMODES];
    float surface[CY_NKEYS][CY_NMODES];
    float rain[CY_RAIN_PTS][CY_NMODES];
    float wander[CY_WANDER_PTS][CY_NMODES];
    float key_uv[CY_NKEYS][2], rain_uv[CY_RAIN_PTS][2], wander_uv[CY_WANDER_PTS][2];
    float mic_uv[2][2], pos_uv[2];
    float clamped;
} cy_object_t;
typedef struct {
    uint32_t seq;
    int8_t field[CY_NMODES][CY_GRID][CY_GRID];
    int8_t sdf[CY_SDF][CY_SDF];
    uint8_t cover[CY_COVER][CY_COVER];
} cy_fieldcache_t;
void cy_geom_init(void);
void cy_geom_update(const cy_geom_in_t *in);
const cy_object_t *cy_geom_object(void);
const cy_fieldcache_t *cy_geom_fields(void);
float cy_geom_field_at(const cy_fieldcache_t *f, int mode, float u, float v);   /* bilinear, -1..1 */
float cy_geom_sdf_at(const cy_fieldcache_t *f, float u, float v);               /* plate units */
const char *cy_geom_shape_name(int shape);    /* "DRUM".."BAR" */
const char *cy_geom_shape_short(int shape);   /* "DRM" "CIR" "HEX" "SQR" "TRI" "STR" "GTR" "STD" "BAR" */
