/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 shared constants. FROZEN CONTRACT. */
#pragma once
#include <stdint.h>
#ifdef PLAT_HOST
#define CY_FS 44100.0f
#else
#define CY_FS 44117.647f               /* FM-1 I2S: 192 MHz / 17 / 256 */
#endif
#define CY_BLOCK 128u                  /* == APP_BLOCK */
#define CY_CR 32u                      /* control sub-block */
#define CY_NSHAPES 9
#define CY_NMODES 32
#define CY_GRID 24
#define CY_SDF 48
#define CY_COVER 92
#define CY_NVOICES 8
#define CY_NSLOTS (CY_NVOICES + 1)     /* unused since the review round: the engine has 8 voices, a 4-slot steal-fade
                                        * pool (CY_NFADE, src/cy_engine_int.h) and the SURFACE slot */
#define CY_NKEYS 27
#define CY_RAIN_PTS 32
#define CY_WANDER_PTS 16
#define CY_MAX_BOWED 4
#define CY_BUDGET_MIN 48
#define CY_BUDGET_MAX 128
#ifdef PLAT_HOST
#define CY_BUDGET_DEFAULT 128
#else
#define CY_BUDGET_DEFAULT 96
#endif
#define CY_OUT_SCALE 8388607.0f
#define CY_CEIL 0.96605f               /* -0.3 dBFS */
enum { CY_SHAPE_DRUM, CY_SHAPE_CIRCLE, CY_SHAPE_HEX, CY_SHAPE_SQUARE, CY_SHAPE_TRIANGLE,
       CY_SHAPE_STAR, CY_SHAPE_GUITAR, CY_SHAPE_STADIUM, CY_SHAPE_BAR };
enum { CY_EXC_MALLET, CY_EXC_BOW, CY_EXC_RAIN, CY_NEXC };
enum { CY_CARVE_HARMONIC, CY_CARVE_BELL, CY_CARVE_BAR, CY_NCARVE };
enum { CY_MAT_RUBBER, CY_MAT_WOOD, CY_MAT_GLASS, CY_MAT_STEEL, CY_MAT_BRONZE, CY_MAT_AETHER, CY_NMAT };
#if defined(PLAT_HOST) || defined(CY_NO_RAMTEXT)
#define CY_HOT
#define CY_POOL
#else
#define CY_HOT __attribute__((section(".ram_text"), noinline, used))
#define CY_POOL __attribute__((section(".pool")))
#endif
static inline uint32_t cy_rand(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }
static inline float cy_randf(uint32_t *s) { return (float)(cy_rand(s) >> 8) * (1.0f / 16777216.0f); }   /* [0,1) */
