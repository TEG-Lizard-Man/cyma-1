/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 src/cy_xfer.c: the sound transfer codec (docs/CYMA-1-sounds-spec.md §3.4 names, §3.6 the parameter table,
 * §3.7 the binary sound record, §4 the share code's CRC, §5.2 the SysEx framing). The device half and the host half
 * of one codec: the page (JS) and the CLI (Python) implement the same bytes, and all three reproduce the frozen
 * vectors (build-test/presets-design/vectors.json, copied to tests/sounds/vectors.json).
 *
 * The device's rules: no libc, no division (the chip's divide is a slow library call), no table bigger than the
 * CRC's 16 entries, main loop only, and the decoders never read past the length they are given. Everything a host
 * sends is untrusted: every length is checked before a byte is read, every u16 value is a valid sound (cy_preset_load
 * snaps to the detents), and a name is cleaned to the FM-1's character set whatever arrived.
 *
 * cy_params_json (PLAT_HOST only: the host simulator's `params_json`, the page's web_cy_params) prints the §3.6
 * table from CY_PARAMS and CY_FACTORY themselves, so the page and the CLI never hand-copy a range, a default or a
 * label. Its text is byte-identical to the design's prototype (build-test/presets-design/sounds_dump.c). */
#include "cy_xfer.h"
#ifdef PLAT_HOST
#include <stdarg.h>
#include <stdio.h>
#endif

/* ------------------------------------------------------------------ CRC-32 --- */
/* zlib's CRC-32 (IEEE 802.3, reflected, init and xorout 0xFFFFFFFF), a nibble at a time: 64 B of table */
static const uint32_t CY_XF_CRC[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

uint32_t cy_crc32(uint32_t crc, const uint8_t *p, uint32_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ CY_XF_CRC[crc & 15u];
        crc = (crc >> 4) ^ CY_XF_CRC[crc & 15u];
    }
    return ~crc;
}

/* ------------------------------------------------------------------- pack7 --- */
/* fm1-base's 8-to-7 (bsp/chip/ota.c ota_pack7): the bytes as one LSB-first bit stream, cut into 7-bit groups */
uint32_t cy_pack7(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)*in++ << nb;
        nb += 8u;
        while (nb >= 7u) {
            out[o++] = (uint8_t)(acc & 0x7Fu);
            acc >>= 7;
            nb -= 7u;
        }
    }
    if (nb)
        out[o++] = (uint8_t)(acc & 0x7Fu);
    return o;
}

/* strict (§5.2): m packed bytes hold n = floor(7m / 8); refused (-1) if a byte has its top bit, if a byte is spare
 * (m != ceil(8n / 7)), if the last byte's unused high bits are not zero, or if n would pass max. Without a divide:
 * 7m >> 3 is the floor, and m == ceil(8n / 7) is 7(m - 1) < 8n (8n <= 7m holds by construction) */
int32_t cy_unpack7(const uint8_t *in, uint32_t m, uint8_t *out, uint32_t max)
{
    uint32_t n, acc = 0, nb = 0, o = 0, i;
    if (m > 0x01000000u)                       /* (no frame is this long: and 7m stays inside 32 bits) */
        return -1;
    n = (7u * m) >> 3;
    if (7u * m >= 8u * n + 7u || n > max)
        return -1;
    for (i = 0; i < m; i++) {
        if (in[i] & 0x80u)
            return -1;
        acc |= (uint32_t)in[i] << nb;
        nb += 7u;
        if (nb >= 8u) {
            out[o++] = (uint8_t)acc;
            acc >>= 8;
            nb -= 8u;
        }
    }
    return acc ? -1 : (int32_t)o;
}

/* ------------------------------------------------------------------- names --- */
/* §3.4, the device's half: ASCII a-z folded to A-Z; '_', tab, CR and LF are spaces; everything outside
 * A-Z 0-9 space - . ' & + / ! ? # ( ) : is dropped (bytes >= 0x80 too: the host halves fold accents first, NFKD);
 * runs of spaces become one, none leads or trails; at most 14; nothing left is UNTITLED. A space that would be the
 * 14th character ends the name, since cutting at 14 and trimming the end would drop it anyway. Zero-padded, as
 * cy_preset_store pads, so two cleanings of one name are the same bytes; a 14-letter name has no terminator */
void cy_name_clean(const uint8_t *in, uint32_t n, char out[CY_PRESET_NAME])
{
    static const char OK[] = " -.'&+/!?#():";
    uint32_t i, o = 0, k;
    int sp = 0;
    for (i = 0; i < CY_PRESET_NAME; i++)
        out[i] = 0;
    for (i = 0; in && i < n && o < CY_PRESET_NAME; i++) {
        uint8_t c = in[i];
        int keep = 0;
        if (c >= 'a' && c <= 'z')
            c = (uint8_t)(c - 32u);
        if (c == '_' || c == '\t' || c == '\r' || c == '\n')
            c = ' ';
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            keep = 1;
        for (k = 0; OK[k] && !keep; k++)
            keep = c == (uint8_t)OK[k];
        if (!keep)
            continue;
        if (c == ' ') {
            sp = o > 0;                        /* (leading spaces dropped; a run is one space, written lazily) */
            continue;
        }
        if (sp && o >= CY_PRESET_NAME - 1u)
            break;                             /* (the space would be the 14th character: the name ends here) */
        if (sp)
            out[o++] = ' ';
        sp = 0;
        out[o++] = (char)c;
    }
    if (!o) {
        static const char U[] = "UNTITLED";
        for (i = 0; U[i]; i++)
            out[i] = U[i];
    }
}

/* ------------------------------------------------------------------ record --- */
/* §3.7: layout, param_set, N, L, the name's L bytes, then N values as u16 little-endian; v1 writes N = 40 */
uint32_t cy_rec_put(const cy_preset_t *p, uint8_t *out)
{
    uint32_t L = 0, i;
    while (L < CY_PRESET_NAME && p->name[L])
        L++;
    out[0] = (uint8_t)CY_XFER_LAYOUT;
    out[1] = (uint8_t)CY_XFER_PARAM_SET;
    out[2] = (uint8_t)P_NPRESET;
    out[3] = (uint8_t)L;
    for (i = 0; i < L; i++)
        out[4u + i] = (uint8_t)p->name[i];
    for (i = 0; i < P_NPRESET; i++) {
        out[4u + L + 2u * i] = (uint8_t)p->v[i];
        out[5u + L + 2u * i] = (uint8_t)(p->v[i] >> 8);
    }
    return 4u + L + 2u * P_NPRESET;
}

/* the exact length or nothing: a layout or param_set other than 1 is NEWER (the device answers E_PARAM_SET), any
 * other inconsistency LENGTH (E_ARG). N < 40: the parameters a sound from an older CYMA-1 lacks take their defaults
 * (cy_params_defaults quantised as cy_preset_store quantises: the table's def_q16); N > 40: a newer CYMA-1's extra
 * values are ignored. p is written only on success */
int cy_rec_get(const uint8_t *in, uint32_t n, cy_preset_t *p)
{
    uint32_t N, L, i;
    if (!in || !p || n < 4u)
        return CY_REC_E_LENGTH;
    if (in[0] != CY_XFER_LAYOUT || in[1] != CY_XFER_PARAM_SET)
        return CY_REC_E_NEWER;
    N = in[2];
    L = in[3];
    if (N < 1u || N > CY_XFER_N_MAX || L > CY_XFER_NAME_MAX || n != 4u + L + 2u * N)
        return CY_REC_E_LENGTH;
    cy_name_clean(in + 4, L, p->name);
    if (N < P_NPRESET) {
        float d[P_NPARAMS];
        cy_params_defaults(d);
        for (i = N; i < P_NPRESET; i++) {
            float x = d[i] >= 0.0f ? (d[i] <= 1.0f ? d[i] : 1.0f) : 0.0f;   /* (and NaN -> 0, as cy_preset_store) */
            p->v[i] = (uint16_t)(x * 65535.0f + 0.5f);
        }
    }
    for (i = 0; i < N && i < P_NPRESET; i++)
        p->v[i] = (uint16_t)(in[4u + L + 2u * i] | (uint32_t)in[5u + L + 2u * i] << 8);
    return CY_REC_OK;
}

/* ------------------------------------------------- the parameter table (host) --- */
#ifdef PLAT_HOST
/* the file key: the P_* identifier in lower case, without P_ (§3.2). Stable across display-name changes; append-only
 * with param_set 1 (§3.1) */
static const char *const CY_XF_KEY[] = {
    "shape", "exciter", "keymode", "material", "stiffness", "carve", "decay", "carve_target", "bright", "shimmer",
    "modes", "hardness", "tension", "force", "grit", "density", "spread", "pos_x", "pos_y", "noise", "damper",
    "bloom", "palm", "swell", "mics", "room", "size", "drive", "predelay", "damping", "tilt", "level", "wander",
    "morph_depth", "lfo_rate", "stretch", "lfo_wave", "lfo_pos", "vibrato", "vib_rate",
};
typedef char cy_xf_assert_keys[sizeof CY_XF_KEY / sizeof CY_XF_KEY[0] == P_NPRESET ? 1 : -1];
static const char *const CY_XF_CURVE[4] = {"LIN", "LOG", "ENUM", "MORPH"};

typedef struct { char *buf; uint32_t max, n; int full; } cy_xf_out_t;
static void cy_xf_printf(cy_xf_out_t *o, const char *fmt, ...)
{
    va_list ap;
    int k;
    if (o->full)
        return;
    va_start(ap, fmt);
    k = vsnprintf(o->buf + o->n, o->max - o->n, fmt, ap);
    va_end(ap);
    if (k < 0 || (uint32_t)k >= o->max - o->n)
        o->full = 1;
    else
        o->n += (uint32_t)k;
}
static void cy_xf_str(cy_xf_out_t *o, const char *s, uint32_t max)   /* a JSON string: quotes, \ and controls escaped */
{
    uint32_t i;
    cy_xf_printf(o, "\"");
    for (i = 0; s && s[i] && i < max; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c == '"' || c == '\\')
            cy_xf_printf(o, "\\%c", c);
        else if (c < 0x20u)
            cy_xf_printf(o, "\\u%04x", c);
        else
            cy_xf_printf(o, "%c", c);
    }
    cy_xf_printf(o, "\"");
}

/* §3.6 (frozen I1): param_set, the 40 parameters (lo / hi / def as %.7g: params.c's decimal literals), the 24 factory
 * sounds as stored. One entry a line, so a diff of the committed file reads as the change it is */
uint32_t cy_params_json(char *buf, uint32_t max)
{
    cy_xf_out_t o;
    float n[P_NPARAMS];
    int i, k, p;
    if (!buf || max < 2u)
        return 0;
    o.buf = buf, o.max = max, o.n = 0, o.full = 0;
    buf[0] = 0;
    cy_params_defaults(n);
    cy_xf_printf(&o, "{\"param_set\":%u,\"params\":[\n", (unsigned)CY_XFER_PARAM_SET);
    for (i = 0; i < P_NPRESET; i++) {
        const cy_param_def_t *d = &CY_PARAMS[i];
        int nlab = d->curve == CY_CV_ENUM ? d->detents : d->curve == CY_CV_MORPH ? (int)(d->hi - d->lo + 0.5f) + 1 : 0;
        cy_xf_printf(&o, " {\"id\":%d,\"key\":", i);
        cy_xf_str(&o, CY_XF_KEY[i], 64u);
        cy_xf_printf(&o, ",\"name\":");
        cy_xf_str(&o, d->name, 64u);
        cy_xf_printf(&o, ",\"unit\":");
        cy_xf_str(&o, d->unit, 64u);
        cy_xf_printf(&o, ",\"lo\":%.7g,\"hi\":%.7g,\"def\":%.7g,\"curve\":\"%s\",\"detents\":%d,"
                         "\"int\":%s,\"def_q16\":%u",
                     (double)d->lo, (double)d->hi, (double)d->def, CY_XF_CURVE[d->curve & 3u], d->detents,
                     i == P_MODES ? "true" : "false", (unsigned)(n[i] * 65535.0f + 0.5f));
        cy_xf_printf(&o, ",\"labels\":");
        if (nlab && d->labels) {
            cy_xf_printf(&o, "[");
            for (k = 0; k < nlab; k++) {
                if (k)
                    cy_xf_printf(&o, ",");
                cy_xf_str(&o, d->labels[k], 64u);
            }
            cy_xf_printf(&o, "]");
        } else {
            cy_xf_printf(&o, "null");
        }
        cy_xf_printf(&o, "}%s\n", i + 1 < P_NPRESET ? "," : "");
    }
    cy_xf_printf(&o, "],\"factory\":[\n");
    for (p = 0; p < CY_NFACTORY; p++) {
        cy_xf_printf(&o, " {\"num\":%d,\"name\":", p + 1);
        cy_xf_str(&o, CY_FACTORY[p].name, CY_PRESET_NAME);   /* (a 14-letter name has no terminator) */
        cy_xf_printf(&o, ",\"q16\":[");
        for (i = 0; i < P_NPRESET; i++)
            cy_xf_printf(&o, "%s%u", i ? "," : "", (unsigned)CY_FACTORY[p].v[i]);
        cy_xf_printf(&o, "]}%s\n", p + 1 < CY_NFACTORY ? "," : "");
    }
    cy_xf_printf(&o, "]}\n");
    if (o.full) {
        buf[0] = 0;
        return 0;
    }
    return o.n;
}
#endif
