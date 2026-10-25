/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 sound transfer codec (docs/CYMA-1-sounds-spec.md §3.7, §4, §5.2): no libc, no division, main loop only */
#pragma once
#include "cy_params.h"
#define CY_XFER_LAYOUT 1u
#define CY_XFER_PARAM_SET 1u
#define CY_XFER_NAME_MAX 32u                                    /* a record's name bytes, as read */
#define CY_XFER_N_MAX 64u                                       /* a record's values, as read */
#define CY_XFER_REC_V1 (4u + CY_PRESET_NAME + 2u * P_NPRESET)   /* 98: the most v1 writes */
enum { CY_REC_OK = 0, CY_REC_E_LENGTH = -1, CY_REC_E_NEWER = -2 };
uint32_t cy_crc32(uint32_t crc, const uint8_t *p, uint32_t n);  /* zlib's: cy_crc32(0, p, n); chains; 16-entry table */
uint32_t cy_pack7(const uint8_t *in, uint32_t n, uint8_t *out); /* fm1-base's LSB-first 8-to-7: returns ceil(8n/7) */
int32_t  cy_unpack7(const uint8_t *in, uint32_t m, uint8_t *out, uint32_t max);   /* strict (§5.2); -1 malformed */
uint32_t cy_rec_put(const cy_preset_t *p, uint8_t *out);        /* 4 + L + 80, L = strnlen(name, 14) */
int      cy_rec_get(const uint8_t *in, uint32_t n, cy_preset_t *p);   /* CY_REC_*: N < 40 -> defaults; N > 40 ignored;
                                                                       * the name cleaned (cy_name_clean) */
void     cy_name_clean(const uint8_t *in, uint32_t n, char out[CY_PRESET_NAME]);   /* §3.4; zero-padded; "UNTITLED" */
#ifdef PLAT_HOST
uint32_t cy_params_json(char *buf, uint32_t max);               /* §3.6 schema; length, 0 = no room (16 KiB is ample) */
#endif
