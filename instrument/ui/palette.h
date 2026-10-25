/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 palette (M5, spec §6.2): the exact RGB565 tokens, their RGB888 for the 8-bit blends the plate does, the
 * MATERIAL tint anchors and the 4x4 Bayer matrix that turns RGB888 into RGB565 without banding. Included once, by
 * ui/ui.c (unity build). Canvas buffers hold the panel's byte order: swap16 a token before it goes into one. */
#pragma once

/* RGB565, unswapped (ink on void 17.4:1; labels ink_dim 6.4:1) */
#define UI_C_VOID 0x0842u
#define UI_C_SURFACE 0x10A4u
#define UI_C_LIFT 0x10E5u
#define UI_C_RULE 0x2147u
#define UI_C_RIM 0x3AAFu
#define UI_C_INK 0xEF7Eu
#define UI_C_INK_DIM 0x8C94u
#define UI_C_INK_FAINT 0x4A8Cu
#define UI_C_TEAL 0x5E98u                /* PLATE */
#define UI_C_AMBER 0xF52Bu               /* EXCITE, and the exciter everywhere */
#define UI_C_VIOLET 0xA47Fu              /* SPACE, and the mics */
#define UI_C_MAGENTA 0xFAF4u             /* MOTION, and pressure / live modulation everywhere */
#define UI_C_SILVER 0xCE7Bu              /* GLOBAL */
#define UI_C_CLIP 0xFA47u
#define UI_C_ICE 0x9FF7u                 /* FREEZE */
#define UI_C_FIELD_COLD 0x19CBu
#define UI_C_FIELD_WARM 0xF52Bu
#define UI_C_GLOW 0xCC27u
#define UI_C_WHITE 0xFFFFu               /* a TOUCH flash only */

/* the same tokens in RGB888, for the plate's 8-bit arithmetic (then dithered back to the exact 565) */
typedef struct { uint8_t r, g, b; } ui_rgb_t;
#define UI_RGB(h) {(uint8_t)((h) >> 16), (uint8_t)((h) >> 8), (uint8_t)(h)}
static const ui_rgb_t UI_VOID8 = UI_RGB(0x080810), UI_RIM8 = UI_RGB(0x39557B), UI_INK8 = UI_RGB(0xEFEFF7),
                      UI_ICE8 = UI_RGB(0x9CFFBD), UI_TEAL8 = UI_RGB(0x5AD3C6), UI_COLD8 = UI_RGB(0x18385A),
                      UI_WARM8 = UI_RGB(0xF7A65A), UI_DEEP8 = UI_RGB(0x185D73),
                      UI_FCOLD8 = UI_RGB(0x4F86C6),   /* FIELD's cold lobe: as bright as warm is (4.3:1 on the plate) */
                      UI_MESH08 = UI_RGB(0x7E9CC0),   /* MESH at rest, h = 0 (7.0:1 on void) */
                      UI_MESHLO8 = UI_RGB(0x3D8FB0),  /* MESH troughs, h = -1 (5.5:1) */
                      UI_FROST8 = UI_RGB(0xDFFFF0);   /* FREEZE: the sand 45 % toward this frosted white */
#define UI_FROST_PLATE 0x102830u         /* FREEZE: the plate 10 % toward this (RGB888) */
/* PHOSPHOR's marks (RGB565): the reticle and ripples sand_hi, the mics sand_lo, the bow ice */
#define UI_C_PHOS_HI 0xCFFAu             /* #CEFFD6 */
#define UI_C_PHOS_LO 0x3E2Bu             /* #39C75A */

/* MATERIAL tint anchors (RGB888), interpolated by the MATERIAL value 0..5 and dithered to 565; the last row is the
 * PHOSPHOR theme, which ignores the material. Columns: plate, plate_hi, sand_lo, sand_hi */
#define UI_NMAT 6
static const uint32_t UI_MAT[UI_NMAT + 1][4] = {
    {0x181818, 0x393439, 0xB58A5A, 0xF7E3CE},   /* RUBBER */
    {0x211408, 0x5A3821, 0xD6AA6B, 0xFFF3D6},   /* WOOD */
    {0x082029, 0x10516B, 0xA5D7CE, 0xEFFFFF},   /* GLASS */
    {0x101821, 0x18385A, 0xCEAA6B, 0xFFF3DE},   /* STEEL */
    {0x181008, 0x5A3410, 0xDEA642, 0xFFEFC6},   /* BRONZE */
    {0x100C21, 0x31205A, 0xB5A6EF, 0xF7F3FF},   /* AETHER */
    {0x000000, 0x002008, 0x39C75A, 0xCEFFD6},   /* PHOSPHOR */
};
#define UI_PHOS_RIM 0x1E6B34u            /* the rim in PHOSPHOR (RGB888): the tokens' blue rim reads wrong on green */

/* Bayer 4x4, t = B[y & 3][x & 3]: r5 = min(31, (r8 + (t >> 1)) >> 3), g6 = min(63, (g8 + (t >> 2)) >> 2), the same
 * for b5 as for r5 */
static const uint8_t UI_BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

/* an RGB888 colour (each 0..255, already clamped) at Bayer threshold t, as the panel's byte-swapped RGB565 */
static inline uint16_t ui_dith(uint32_t r, uint32_t g, uint32_t b, uint32_t t)
{
    uint32_t r5 = (r + (t >> 1)) >> 3, g6 = (g + (t >> 2)) >> 2, b5 = (b + (t >> 1)) >> 3, v;
    r5 = r5 > 31u ? 31u : r5;
    g6 = g6 > 63u ? 63u : g6;
    b5 = b5 > 31u ? 31u : b5;
    v = r5 << 11 | g6 << 5 | b5;
    return (uint16_t)((v >> 8) | ((v & 0xFFu) << 8));
}

/* the page hues (PLATE teal, EXCITE amber, SPACE violet, MOTION magenta, GLOBAL silver) */
static const uint16_t UI_PAGE_HUE[PG_N] = {UI_C_TEAL, UI_C_AMBER, UI_C_VIOLET, UI_C_MAGENTA, UI_C_SILVER};
