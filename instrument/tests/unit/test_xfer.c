/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 Sounds T1 (docs/CYMA-1-sounds-spec.md §11.1): the device codec src/cy_xfer.c against the frozen vectors.
 *
 *   cc -O2 -std=c99 -ffp-contract=off -DPLAT_HOST -Wall -Wextra -Iinstrument/src -Ireference/fm1-base/include \
 *      instrument/tests/unit/test_xfer.c instrument/src/cy_xfer.c instrument/src/params.c instrument/src/presets.c -lm
 *   ./a.out [--fuzz N] [--seed S]
 *
 * (run_all.sh also builds it with -fsanitize=address,undefined and runs the fuzz under them: step xfer_asan.)
 * Checks: cy_crc32's check value and chaining; cy_rec_put of all 24 factory sounds byte for byte; cy_rec_get round
 * trips, N = 38 / 42 tolerance, every truncation and extension refused, layout / param_set 2 -> NEWER, the N and L
 * limits; pack7 / unpack7 on the 13 vector frames, rebuilt byte for byte; the strict-unpack refusals; the ASCII name
 * table and what C makes of the non-ASCII ones; cy_params_json against tools/cyma1_params.json (§3.6). Fuzz (default
 * 10^6 each): random buffers through cy_unpack7 (accepted only if it is exactly a pack7, never past max), random
 * records through cy_rec_get (the reference's verdict, names always in the set, values as sent or the defaults),
 * random bytes through cy_name_clean against an independent model, and corrupted frames (CRC-32 refuses every one).
 * Exit 0 = every check passed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cy_xfer.h"
#include "../sounds/vectors.h"

/* ----------------------------------------------------------- harness --- */
static int t_fail;
static long t_checks;
#define CHECK(c, ...)                                                                                              \
    do {                                                                                                           \
        t_checks++;                                                                                                \
        if (!(c)) {                                                                                                \
            if (t_fail < 60) {                                                                                     \
                printf("  FAIL %s:%d: ", __func__, __LINE__);                                                      \
                printf(__VA_ARGS__);                                                                               \
                printf("\n");                                                                                      \
            }                                                                                                      \
            t_fail++;                                                                                              \
        }                                                                                                          \
    } while (0)

static uint32_t t_rng = 0x2545F491u;
static uint32_t rnd(void)                      /* xorshift32 */
{
    t_rng ^= t_rng << 13;
    t_rng ^= t_rng >> 17;
    t_rng ^= t_rng << 5;
    return t_rng;
}
static uint32_t rndn(uint32_t n) { return n ? rnd() % n : 0; }

static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t ceil87(uint32_t n) { return (8u * n + 6u) / 7u; }

/* the device's name set (spec §3.4) */
static int in_set(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr(" -.'&+/!?#():", c) != NULL);
}

/* a stored name is well formed: 1..14 characters of the set, zero-padded, no leading, trailing or double space */
static int name_ok(const char nm[CY_PRESET_NAME])
{
    int i, len = 0;
    while (len < CY_PRESET_NAME && nm[len])
        len++;
    if (!len || nm[0] == ' ' || nm[len - 1] == ' ')
        return 0;
    for (i = 0; i < len; i++)
        if (!in_set((unsigned char)nm[i]) || (i && nm[i] == ' ' && nm[i - 1] == ' '))
            return 0;
    for (i = len; i < CY_PRESET_NAME; i++)
        if (nm[i])
            return 0;
    return 1;
}

/* an independent model of the C rule: bytes >= 0x80 dropped, then §3.4 steps 2-7, in passes rather than a stream */
static void name_model(const uint8_t *in, uint32_t n, char out[CY_PRESET_NAME])
{
    char a[512], b[512];
    uint32_t i, na = 0, nb = 0;
    for (i = 0; i < n && na < sizeof a - 1; i++) {
        int c = in[i];
        if (c >= 0x80)
            continue;
        if (c >= 'a' && c <= 'z')
            c -= 32;
        if (c == '_' || c == '\t' || c == '\r' || c == '\n')
            c = ' ';
        if (in_set(c))
            a[na++] = (char)c;
    }
    for (i = 0; i < na; i++)                   /* collapse runs of spaces */
        if (!(a[i] == ' ' && nb && b[nb - 1] == ' '))
            b[nb++] = a[i];
    i = 0;
    while (i < nb && b[i] == ' ')              /* trim */
        i++;
    while (nb > i && b[nb - 1] == ' ')
        nb--;
    if (nb - i > CY_PRESET_NAME)               /* cut to 14, then trim the end */
        nb = i + CY_PRESET_NAME;
    while (nb > i && b[nb - 1] == ' ')
        nb--;
    memset(out, 0, CY_PRESET_NAME);
    if (nb == i)
        memcpy(out, "UNTITLED", 8);
    else
        memcpy(out, b + i, nb - i);
}

static void name14(char dst[CY_PRESET_NAME + 1], const char src[CY_PRESET_NAME])
{
    memcpy(dst, src, CY_PRESET_NAME);
    dst[CY_PRESET_NAME] = 0;
}

static void defaults_q16(uint16_t v[P_NPRESET])  /* what the device's own store makes of the defaults */
{
    float d[P_NPARAMS];
    cy_preset_t p;
    cy_params_defaults(d);
    cy_preset_store(d, "", &p);
    memcpy(v, p.v, sizeof p.v);
}

/* ------------------------------------------------------------- layouts --- */
static void test_layouts(void)
{
    uint16_t dq[P_NPRESET];
    int i;
    CHECK(sizeof(cy_preset_t) == 94, "cy_preset_t is %u B (the page's box layout: 94)", (unsigned)sizeof(cy_preset_t));
    CHECK(sizeof(cy_user_bank_t) == 3020, "cy_user_bank_t is %u B (frozen: 3,020)", (unsigned)sizeof(cy_user_bank_t));
    CHECK(CY_XFER_REC_V1 == 98u, "CY_XFER_REC_V1 = %u", (unsigned)CY_XFER_REC_V1);
    CHECK(P_NPRESET == CYV_NPARAMS, "param_set 1 has %d values, the vectors %d", P_NPRESET, CYV_NPARAMS);
    CHECK(CY_NFACTORY == CYV_NFACTORY, "%d factory sounds, the vectors %d", CY_NFACTORY, CYV_NFACTORY);
    CHECK(CY_XFER_LAYOUT == 1u && CY_XFER_PARAM_SET == 1u && CY_XFER_NAME_MAX == 32u && CY_XFER_N_MAX == 64u,
          "the record constants");
    defaults_q16(dq);
    for (i = 0; i < P_NPRESET; i++)
        CHECK(dq[i] == CYV_DEF_Q16[i], "default %d: the store makes %u, the table's def_q16 is %u", i, dq[i], CYV_DEF_Q16[i]);
}

/* -------------------------------------------------------------- CRC-32 --- */
static void test_crc(void)
{
    const uint8_t *s = (const uint8_t *)CYV_CRC_INPUT;
    uint32_t n = (uint32_t)strlen(CYV_CRC_INPUT), k, c;
    uint8_t buf[300];
    CHECK(cy_crc32(0, s, n) == CYV_CRC_CHECK, "crc32(\"123456789\") = %08X, want %08X", cy_crc32(0, s, n), CYV_CRC_CHECK);
    for (k = 0; k <= n; k++)                   /* it chains */
        CHECK(cy_crc32(cy_crc32(0, s, k), s + k, n - k) == CYV_CRC_CHECK, "chained at %u", k);
    CHECK(cy_crc32(0, s, 0) == 0u, "the CRC of nothing is 0");
    CHECK(cy_crc32(0, CYV_FACTORY[0].rec, CYV_FACTORY[0].rec_len) == CYV_BOWL_CRC, "SINGING BOWL's record CRC %08X",
          cy_crc32(0, CYV_FACTORY[0].rec, CYV_FACTORY[0].rec_len));
    for (k = 0; k < sizeof buf; k++)           /* a table-free bitwise CRC agrees on every length */
        buf[k] = (uint8_t)rnd();
    for (n = 0; n <= sizeof buf; n += 7) {
        uint32_t r = 0xFFFFFFFFu, i, b;
        for (i = 0; i < n; i++) {
            r ^= buf[i];
            for (b = 0; b < 8; b++)
                r = (r >> 1) ^ (0xEDB88320u & (0u - (r & 1u)));
        }
        c = ~r;
        CHECK(cy_crc32(0, buf, n) == c, "crc32 of %u random bytes", n);
    }
}

/* -------------------------------------------------------------- record --- */
static void test_record(void)
{
    uint8_t rec[200], r2[200];
    cy_preset_t p, q;
    char a[CY_PRESET_NAME + 1], b[CY_PRESET_NAME + 1];
    uint16_t dq[P_NPRESET];
    int f, i, rc;
    uint32_t n, k;
    defaults_q16(dq);
    for (f = 0; f < CY_NFACTORY; f++) {
        const cyv_factory_t *v = &CYV_FACTORY[f];
        memset(rec, 0xA5, sizeof rec);
        n = cy_rec_put(&CY_FACTORY[f], rec);
        CHECK(n == v->rec_len && !memcmp(rec, v->rec, n), "cy_rec_put %02d %s: %u B, want %u B and the vector's bytes",
              v->num, v->name, n, v->rec_len);
        CHECK(rec[n] == 0xA5, "cy_rec_put %02d wrote past its %u B", v->num, n);
        for (i = 0; i < P_NPRESET; i++)
            CHECK(CY_FACTORY[f].v[i] == v->q16[i], "CY_FACTORY %02d value %d is %u, the vectors %u", v->num, i,
                  CY_FACTORY[f].v[i], v->q16[i]);
        name14(a, CY_FACTORY[f].name);
        CHECK(!strcmp(a, v->name), "CY_FACTORY %02d is %s, the vectors %s", v->num, a, v->name);
        /* back: the same preset, the same bytes again */
        memset(&p, 0x5A, sizeof p);
        rc = cy_rec_get(v->rec, v->rec_len, &p);
        CHECK(rc == CY_REC_OK, "cy_rec_get %02d: rc %d", v->num, rc);
        CHECK(!memcmp(&p, &CY_FACTORY[f], sizeof p), "cy_rec_get %02d is not CY_FACTORY[%d] to the byte", v->num, f);
        CHECK(cy_rec_put(&p, r2) == v->rec_len && !memcmp(r2, v->rec, v->rec_len), "put(get(%02d)) differs", v->num);
        /* every truncation, and one byte more, refused */
        for (k = 0; k < v->rec_len; k++) {
            rc = cy_rec_get(v->rec, k, &q);
            CHECK(rc != CY_REC_OK && (k < 4 || rc == CY_REC_E_LENGTH), "%02d cut to %u B: rc %d (want E_LENGTH)", v->num, k, rc);
        }
        memcpy(rec, v->rec, v->rec_len);
        rec[v->rec_len] = 0;
        CHECK(cy_rec_get(rec, v->rec_len + 1u, &q) == CY_REC_E_LENGTH, "%02d with a byte more accepted", v->num);
    }
    /* N = 38: the missing two take the defaults; N = 42: the extra two ignored */
    memset(&p, 0x5A, sizeof p);
    rc = cy_rec_get(CYV_REC_N38, sizeof CYV_REC_N38, &p);
    CHECK(rc == CY_REC_OK, "N = 38 refused (rc %d)", rc);
    name14(a, p.name);
    CHECK(!strcmp(a, "TEST"), "N = 38 name %s", a);
    for (i = 0; i < P_NPRESET; i++)
        CHECK(p.v[i] == (i < 38 ? CYV_FACTORY[0].q16[i] : CYV_DEF_Q16[i]), "N = 38 value %d = %u", i, p.v[i]);
    memset(&p, 0x5A, sizeof p);
    rc = cy_rec_get(CYV_REC_N42, sizeof CYV_REC_N42, &p);
    CHECK(rc == CY_REC_OK, "N = 42 refused (rc %d)", rc);
    for (i = 0; i < P_NPRESET; i++)
        CHECK(p.v[i] == CYV_FACTORY[0].q16[i], "N = 42 value %d = %u", i, p.v[i]);
    /* layout or param_set 2: newer */
    memcpy(rec, CYV_FACTORY[0].rec, CYV_FACTORY[0].rec_len);
    rec[0] = 2;
    CHECK(cy_rec_get(rec, CYV_FACTORY[0].rec_len, &q) == CY_REC_E_NEWER, "layout 2 is not NEWER");
    rec[0] = 1, rec[1] = 2;
    CHECK(cy_rec_get(rec, CYV_FACTORY[0].rec_len, &q) == CY_REC_E_NEWER, "param_set 2 is not NEWER");
    rec[0] = 0, rec[1] = 1;
    CHECK(cy_rec_get(rec, CYV_FACTORY[0].rec_len, &q) == CY_REC_E_NEWER, "layout 0 is not NEWER");
    /* the N and L limits, each with its exact length */
    {
        static const struct { uint8_t N, L; int ok; } lim[] = {{0, 0, 0}, {1, 0, 1}, {40, 0, 1}, {64, 32, 1},
                                                              {65, 0, 0}, {40, 33, 0}, {255, 0, 0}, {40, 255, 0}};
        for (k = 0; k < sizeof lim / sizeof lim[0]; k++) {
            uint32_t len = 4u + lim[k].L + 2u * lim[k].N, j;
            static uint8_t big[4 + 255 + 2 * 255 + 8];
            big[0] = 1, big[1] = 1, big[2] = lim[k].N, big[3] = lim[k].L;
            for (j = 4; j < len; j++)
                big[j] = (uint8_t)(j < 4u + lim[k].L ? 'A' + j % 26 : j);
            rc = cy_rec_get(big, len, &q);
            CHECK(lim[k].ok ? rc == CY_REC_OK : rc == CY_REC_E_LENGTH, "N %u L %u (%u B): rc %d", lim[k].N, lim[k].L, len, rc);
            if (lim[k].ok && rc == CY_REC_OK) {
                CHECK(name_ok(q.name), "N %u L %u: name not well formed", lim[k].N, lim[k].L);
                for (i = 0; i < P_NPRESET; i++) {
                    uint16_t want = i < lim[k].N ? (uint16_t)(big[4 + lim[k].L + 2 * i] | big[5 + lim[k].L + 2 * i] << 8) : dq[i];
                    CHECK(q.v[i] == want, "N %u L %u value %d = %u, want %u", lim[k].N, lim[k].L, i, q.v[i], want);
                }
            }
        }
    }
    /* the name is cleaned on the way in: lower case, a bad byte, 22 letters, none */
    {
        static const struct { const char *in, *out; } nm[] = {{"glass rain", "GLASS RAIN"}, {"A*B>C\x80\xff", "ABC"},
                                                               {"A very long sound name", "A VERY LONG SO"}, {"", "UNTITLED"},
                                                               {"\x01\x02\x7f", "UNTITLED"}};
        for (k = 0; k < sizeof nm / sizeof nm[0]; k++) {
            uint32_t L = (uint32_t)strlen(nm[k].in);
            rec[0] = 1, rec[1] = 1, rec[2] = P_NPRESET, rec[3] = (uint8_t)L;
            memcpy(rec + 4, nm[k].in, L);
            memcpy(rec + 4 + L, CYV_FACTORY[0].rec + 16, 2u * P_NPRESET);
            rc = cy_rec_get(rec, 4u + L + 2u * P_NPRESET, &q);
            name14(a, q.name);
            CHECK(rc == CY_REC_OK && !strcmp(a, nm[k].out) && name_ok(q.name), "record name \"%s\" -> \"%s\" (rc %d), want %s",
                  nm[k].in, a, rc, nm[k].out);
        }
    }
    /* a 14-letter name has no terminator: L = 14, 98 B; a nameless preset: L = 0, 84 B */
    memset(&p, 0, sizeof p);
    memcpy(p.name, "ABCDEFGHIJKLMN", 14);
    for (i = 0; i < P_NPRESET; i++)
        p.v[i] = (uint16_t)(i * 1000 + 7);
    n = cy_rec_put(&p, rec);
    CHECK(n == 98u && rec[3] == 14 && !memcmp(rec + 4, "ABCDEFGHIJKLMN", 14), "14-letter name: %u B, L %u", n, rec[3]);
    CHECK(cy_rec_get(rec, n, &q) == CY_REC_OK && !memcmp(&q, &p, sizeof p), "14-letter name round trip");
    memset(p.name, 0, sizeof p.name);
    n = cy_rec_put(&p, rec);
    CHECK(n == 84u && rec[3] == 0, "no name: %u B, L %u", n, rec[3]);
    CHECK(cy_rec_get(rec, n, &q) == CY_REC_OK, "no name refused");
    name14(b, q.name);
    CHECK(!strcmp(b, "UNTITLED"), "no name reads back as %s", b);
}

/* --------------------------------------------------------------- pack7 --- */
static void test_pack7(void)
{
    uint8_t in[400], pk[480], out[420];
    uint32_t n, m, i;
    int32_t d;
    for (n = 0; n <= 300; n++) {
        for (i = 0; i < n; i++)
            in[i] = (uint8_t)rnd();
        memset(pk, 0xEE, sizeof pk);
        m = cy_pack7(in, n, pk);
        CHECK(m == ceil87(n), "pack7 of %u B gave %u B, want %u", n, m, ceil87(n));
        CHECK(pk[m] == 0xEE, "pack7 of %u B wrote past %u", n, m);
        for (i = 0; i < m; i++)
            CHECK(pk[i] < 0x80u, "pack7 byte %u of %u is %02X", i, n, pk[i]);
        if (n < 4 || n > 260)                  /* (§5.2's frame limits may live in cy_unpack7 or in its caller) */
            continue;
        memset(out, 0xEE, sizeof out);
        d = cy_unpack7(pk, m, out, 260);
        CHECK(d == (int32_t)n && !memcmp(out, in, n), "unpack7(pack7(%u B)) = %d", n, (int)d);
        CHECK(out[n] == 0xEE, "unpack7 of %u B wrote past it", n);
        CHECK(cy_unpack7(pk, m, out, n - 1u) == -1, "unpack7 of %u B into %u accepted", n, n - 1u);
        CHECK(cy_unpack7(pk, m, out, n) == (int32_t)n, "unpack7 of %u B into exactly %u refused", n, n);
    }
    /* the strict refusals (§5.2) */
    for (i = 0; i < 40; i++)
        in[i] = (uint8_t)(i * 37 + 11);
    m = cy_pack7(in, 40, pk);                  /* 46 B, 40 * 8 = 320 bits: 2 spare bits in the last byte */
    CHECK(m == 46u, "pack7(40) = %u", m);
    CHECK(cy_unpack7(pk, m, out, 260) == 40, "the good one refused");
    for (i = 0; i < m; i++) {                  /* a byte with its top bit set */
        uint8_t keep = pk[i];
        pk[i] |= 0x80u;
        CHECK(cy_unpack7(pk, m, out, 260) == -1, "a byte >= 0x80 at %u accepted", i);
        pk[i] = keep;
    }
    pk[m - 1] |= 0x40u;                        /* a spare high bit of the last byte */
    CHECK(cy_unpack7(pk, m, out, 260) == -1, "a set spare bit accepted");
    pk[m - 1] &= 0x3Fu;
    for (i = 0; i < 42; i++)                   /* 42 B pack to 48 with no spare bit: a 49th byte is a spare byte */
        in[i] = (uint8_t)(i * 53 + 5);
    m = cy_pack7(in, 42, pk);
    CHECK(m == 48u && cy_unpack7(pk, m, out, 260) == 42, "pack7(42) = %u B, or refused", m);
    pk[m] = 0;
    CHECK(cy_unpack7(pk, m + 1u, out, 260) == -1, "a spare byte accepted (%u B: m != ceil(8n/7))", m + 1u);
    for (m = 0; m <= 320; m++) {               /* every m that is not some ceil(8n/7) is refused */
        uint32_t nn = 7u * m / 8u;
        memset(pk, 0, m);
        d = cy_unpack7(pk, m, out, 400);
        if (m != ceil87(nn))
            CHECK(d == -1, "%u packed bytes (not a ceil(8n/7)) accepted as %d", m, (int)d);
        else if (nn >= 4 && nn <= 260)
            CHECK(d == (int32_t)nn, "%u zero bytes: %d, want %u", m, (int)d, nn);
    }
}

/* -------------------------------------------------------------- frames --- */
static uint32_t make_frame(uint8_t cmd, uint8_t seq, const uint8_t *pl, uint32_t n, uint8_t *f)
{
    uint8_t body[300];
    uint32_t crc;
    f[0] = 0xF0, f[1] = 0x7D, f[2] = 0x43, f[3] = 0x59, f[4] = 1, f[5] = cmd, f[6] = seq;
    crc = cy_crc32(cy_crc32(0, f + 4, 3), pl, n);
    memcpy(body, pl, n);
    body[n] = (uint8_t)crc, body[n + 1] = (uint8_t)(crc >> 8), body[n + 2] = (uint8_t)(crc >> 16), body[n + 3] = (uint8_t)(crc >> 24);
    n = 7u + cy_pack7(body, n + 4u, f + 7);
    f[n++] = 0xF7;
    return n;
}

static const cyv_frame_t *frame_named(const char *what)
{
    int i;
    for (i = 0; i < CYV_NFRAMES; i++)
        if (!strcmp(CYV_FRAMES[i].what, what))
            return &CYV_FRAMES[i];
    return NULL;
}

static void test_frames(void)
{
    uint8_t body[300], pk[400], f[400], rec[120], pl[140];
    int i;
    int32_t d;
    uint32_t n, m, rn;
    for (i = 0; i < CYV_NFRAMES; i++) {
        const cyv_frame_t *v = &CYV_FRAMES[i];
        CHECK(v->len >= 13 && v->b[0] == 0xF0 && v->b[v->len - 1] == 0xF7 && v->b[1] == 0x7D && v->b[2] == 0x43 &&
              v->b[3] == 0x59 && v->b[4] == 1, "%s: the header", v->what);
        d = cy_unpack7(v->b + 7, v->len - 8u, body, 260);
        CHECK(d >= 4, "%s: unpack7 refused it (%d)", v->what, (int)d);
        if (d < 4)
            continue;
        n = (uint32_t)d - 4u;
        CHECK(cy_crc32(cy_crc32(0, v->b + 4, 3), body, n) == le32(body + n), "%s: the CRC", v->what);
        m = cy_pack7(body, (uint32_t)d, pk);
        CHECK(m == v->len - 8u && !memcmp(pk, v->b + 7, m), "%s: pack7 does not give the frame back", v->what);
        CHECK(make_frame(v->b[5], v->b[6], body, n, f) == v->len && !memcmp(f, v->b, v->len), "%s: rebuilt differently", v->what);
    }
    /* the frames that carry SINGING BOWL, built from cy_rec_put */
    rn = cy_rec_put(&CY_FACTORY[0], rec);
    {
        static const struct { const char *what; uint8_t cmd, seq, pre[3], npre; } bf[] = {
            {"PUT_SLOT U05", 0x05, 0x04, {4}, 1}, {"PUT_EDIT", 0x03, 0x03, {1}, 1}, {"GET_SLOT reply", 0x44, 0x05, {0, 4, 1}, 3}};
        unsigned k;
        for (k = 0; k < sizeof bf / sizeof bf[0]; k++) {
            const cyv_frame_t *v = frame_named(bf[k].what);
            CHECK(v != NULL, "no vector frame %s", bf[k].what);
            if (!v)
                continue;
            memcpy(pl, bf[k].pre, bf[k].npre);
            memcpy(pl + bf[k].npre, rec, rn);
            n = make_frame(bf[k].cmd, bf[k].seq, pl, bf[k].npre + rn, f);
            CHECK(n == v->len && !memcmp(f, v->b, n), "%s from cy_rec_put: %u B, want the vector's %u B", bf[k].what, n, v->len);
        }
    }
    /* the largest v1 frame: 256 B of payload is 306 B whole (§5.2) */
    memset(pl, 0, sizeof pl);
    {
        static uint8_t big[256];
        uint8_t ff[320];
        memset(big, 0, sizeof big);
        n = make_frame(0x05, 0x08, big, 256, ff);
        CHECK(n == 306u, "the largest frame is %u B, not 306", n);
        CHECK(cy_unpack7(ff + 7, n - 8u, body, 260) == 260, "the largest frame's body refused");
        n = make_frame(0x01, 0x00, big, 0, ff);
        CHECK(n == 13u, "the shortest frame is %u B, not 13", n);
    }
}

/* --------------------------------------------------------------- names --- */
static void test_names(void)
{
    char out[CY_PRESET_NAME], s[CY_PRESET_NAME + 1], m[CY_PRESET_NAME];
    int i;
    for (i = 0; i < CYV_NNAMES_ASCII; i++) {
        memset(out, 0x5A, sizeof out);
        cy_name_clean((const uint8_t *)CYV_NAMES_ASCII[i].in, (uint32_t)strlen(CYV_NAMES_ASCII[i].in), out);
        name14(s, out);
        CHECK(!strcmp(s, CYV_NAMES_ASCII[i].out) && name_ok(out), "name \"%s\" -> \"%s\", want \"%s\"",
              CYV_NAMES_ASCII[i].in, s, CYV_NAMES_ASCII[i].out);
        name_model((const uint8_t *)CYV_NAMES_ASCII[i].in, (uint32_t)strlen(CYV_NAMES_ASCII[i].in), m);
        CHECK(!memcmp(m, out, CY_PRESET_NAME), "the model disagrees on \"%s\"", CYV_NAMES_ASCII[i].in);
    }
    for (i = 0; i < CYV_NNAMES_C; i++) {
        memset(out, 0x5A, sizeof out);
        cy_name_clean((const uint8_t *)CYV_NAMES_C[i].in, (uint32_t)strlen(CYV_NAMES_C[i].in), out);
        name14(s, out);
        CHECK(!strcmp(s, CYV_NAMES_C[i].out) && name_ok(out), "UTF-8 name %d -> \"%s\", want \"%s\" (bytes >= 0x80 dropped)",
              i, s, CYV_NAMES_C[i].out);
    }
    cy_name_clean((const uint8_t *)"", 0, out);
    name14(s, out);
    CHECK(!strcmp(s, "UNTITLED"), "the empty name -> \"%s\"", s);
    cy_name_clean((const uint8_t *)"ABCDEFGHIJKLM X", 15, out);   /* the space would be the 14th: it ends there */
    name14(s, out);
    CHECK(!strcmp(s, "ABCDEFGHIJKLM"), "a 14th-character space -> \"%s\"", s);
    cy_name_clean((const uint8_t *)"ab\0cd", 5, out);              /* a NUL inside is just a byte outside the set */
    name14(s, out);
    CHECK(!strcmp(s, "ABCD"), "a NUL inside -> \"%s\"", s);
}

/* --------------------------------------------------------- params json --- */
static int same_but_space(const char *a, const char *b)
{
    for (;;) {
        while (*a == ' ' || *a == '\n' || *a == '\r' || *a == '\t')
            a++;
        while (*b == ' ' || *b == '\n' || *b == '\r' || *b == '\t')
            b++;
        if (*a != *b)
            return 0;
        if (!*a)
            return 1;
        a++, b++;
    }
}

static void test_params_json(void)
{
    static char buf[16384 + 16], file[65536];
    static const char *const where[] = {"instrument/tools/cyma1_params.json", "tools/cyma1_params.json",
                                        "../tools/cyma1_params.json"};
    uint32_t n, small;
    unsigned k;
    FILE *f = NULL;
    memset(buf, 0x7E, sizeof buf);
    n = cy_params_json(buf, 16384);
    CHECK(n > 0 && n < 16384u, "cy_params_json in 16 KiB: %u", n);
    if (!n)
        return;
    CHECK(buf[n] == 0 && buf[16384] == 0x7E, "cy_params_json: not terminated at %u, or wrote past 16 KiB", n);
    CHECK(!strncmp(buf, "{\"param_set\":1,", 15), "cy_params_json starts \"%.20s\"", buf);
    CHECK(strstr(buf, "\"factory\"") && strstr(buf, "\"SINGING BOWL\"") && strstr(buf, "\"vib_rate\""),
          "cy_params_json lacks the factory list or a key");
    for (small = 0; small < n; small += n / 7u + 1u) {   /* too small: 0, and never a byte past max */
        memset(buf, 0x7E, sizeof buf);
        CHECK(cy_params_json(buf, small) == 0, "cy_params_json in %u B (needs %u) returned non-zero", small, n);
        CHECK(buf[small] == 0x7E, "cy_params_json in %u B wrote past it", small);
    }
    n = cy_params_json(buf, 16384);
    for (k = 0; k < sizeof where / sizeof where[0] && !f; k++)
        f = fopen(where[k], "rb");
    if (!f) {
        printf("  (no tools/cyma1_params.json found from here: the drift check is sounds_smoke's)\n");
        return;
    }
    file[fread(file, 1, sizeof file - 1, f)] = 0;
    fclose(f);
    CHECK(same_but_space(buf, file), "tools/cyma1_params.json is not cy_params_json()'s output: regenerate it "
                                     "(host command params_json)");
}

/* ---------------------------------------------------------------- fuzz --- */
static void fuzz(long iters)
{
    static uint8_t in[400], out[400], pk[480], body[300], fr[400];
    uint16_t dq[P_NPRESET];
    long it, unpack_ok = 0, rec_ok = 0, crc_caught = 0;
    struct { uint8_t pre[16]; cy_preset_t p; uint8_t post[16]; } g;
    defaults_q16(dq);
    for (it = 0; it < iters; it++) {
        uint32_t m, n, i, max, N, L;
        int32_t d;
        int rc, want;
        /* 1. cy_unpack7: random bytes, mostly 7-bit; accepted only as an exact pack7, never past max */
        m = rndn(321);
        for (i = 0; i < m; i++)
            in[i] = (uint8_t)(rndn(64) ? rnd() & 0x7Fu : rnd());
        if (m && rndn(2))
            in[m - 1] &= 0x01u;                /* (often a clean last byte, so the deep path runs) */
        max = rndn(4) ? 260u : rndn(300);
        memset(out, 0xC3, sizeof out);
        d = cy_unpack7(in, m, out, max);
        CHECK(d >= -1 && d <= (int32_t)max, "unpack7: %d from %u B into %u", (int)d, m, max);
        if (d >= 0) {
            unpack_ok++;
            CHECK((uint32_t)d == 7u * m / 8u, "unpack7: %d B from %u", (int)d, m);
            n = cy_pack7(out, (uint32_t)d, pk);
            CHECK(n == m && !memcmp(pk, in, m), "unpack7 accepted %u B that pack7 does not give back", m);
        }
        CHECK(out[max] == 0xC3 || (d >= 0 && (uint32_t)d > max), "unpack7 wrote past max %u", max);
        for (i = d < 0 ? 0 : (uint32_t)d; i < max && i < sizeof out; i++)
            if (out[i] != 0xC3) {
                CHECK(d < 0 && i < 7u * m / 8u, "unpack7 wrote byte %u beyond its %d", i, (int)d);
                break;
            }
        /* 2. cy_rec_get: random records, often nearly right */
        n = rndn(170);
        for (i = 0; i < n; i++)
            in[i] = (uint8_t)rnd();
        if (n >= 4 && rndn(4)) {
            in[0] = rndn(8) ? 1 : (uint8_t)rndn(3);
            in[1] = rndn(8) ? 1 : (uint8_t)rndn(3);
            N = rndn(3) ? (rndn(2) ? 40u : rndn(66)) : rnd() & 0xFFu;
            L = rndn(4) ? rndn(15) : rndn(40);
            in[2] = (uint8_t)N, in[3] = (uint8_t)L;
            if (rndn(3) && 4u + L + 2u * N <= 170u)
                n = 4u + L + 2u * N;
            for (i = 4; i < n && i < 4u + L; i++)
                in[i] = (uint8_t)(rndn(3) ? 0x20 + rndn(0x60) : rnd());
        }
        memset(&g, 0x3C, sizeof g);
        rc = cy_rec_get(in, n, &g.p);
        if (n < 4)
            want = CY_REC_E_LENGTH;
        else if (in[0] != 1 || in[1] != 1)
            want = CY_REC_E_NEWER;
        else if (in[2] < 1 || in[2] > 64 || in[3] > 32 || n != 4u + in[3] + 2u * in[2])
            want = CY_REC_E_LENGTH;
        else
            want = CY_REC_OK;
        CHECK(n < 4 ? rc != CY_REC_OK : rc == want, "rec_get of %u B (%02X %02X N %u L %u): rc %d, want %d", n, in[0],
              n > 1 ? in[1] : 0, n > 2 ? in[2] : 0, n > 3 ? in[3] : 0, rc, want);
        for (i = 0; i < 16; i++)
            CHECK(g.pre[i] == 0x3C && g.post[i] == 0x3C, "rec_get wrote outside the preset");
        if (rc == CY_REC_OK) {
            char mm[CY_PRESET_NAME];
            rec_ok++;
            N = in[2], L = in[3];
            CHECK(name_ok(g.p.name), "rec_get: a name outside the set");
            name_model(in + 4, L, mm);
            CHECK(!memcmp(mm, g.p.name, CY_PRESET_NAME), "rec_get: the name is not the rule's");
            for (i = 0; i < P_NPRESET; i++) {
                uint16_t w = i < N ? (uint16_t)(in[4 + L + 2 * i] | in[5 + L + 2 * i] << 8) : dq[i];
                CHECK(g.p.v[i] == w, "rec_get: value %u is %u, want %u", i, g.p.v[i], w);
            }
        }
        /* 3. cy_name_clean: any bytes, against the model */
        {
            char a[CY_PRESET_NAME], b[CY_PRESET_NAME + 8];
            n = rndn(48);
            for (i = 0; i < n; i++)
                in[i] = (uint8_t)(rndn(2) ? 0x20 + rndn(0x60) : rnd());
            memset(b, 0x3C, sizeof b);
            cy_name_clean(in, n, b);
            name_model(in, n, a);
            CHECK(!memcmp(a, b, CY_PRESET_NAME) && name_ok(b), "name_clean differs from the rule (%u B)", n);
            for (i = CY_PRESET_NAME; i < sizeof b; i++)
                CHECK((uint8_t)b[i] == 0x3C, "name_clean wrote past 14");
        }
        /* 4. a frame with one packed byte changed (7-bit kept): CRC-32 refuses it */
        if (!(it & 7)) {
            uint32_t fl, pos;
            uint8_t was;
            n = rndn(257);
            for (i = 0; i < n; i++)
                body[i] = (uint8_t)rnd();
            fl = make_frame((uint8_t)rndn(64), (uint8_t)rndn(128), body, n, fr);
            pos = 7u + rndn(fl - 8u);
            was = fr[pos];
            fr[pos] = (uint8_t)((was + 1u + rndn(127)) & 0x7Fu);
            d = cy_unpack7(fr + 7, fl - 8u, out, 260);
            if (d < 4 || cy_crc32(cy_crc32(0, fr + 4, 3), out, (uint32_t)d - 4u) != le32(out + d - 4))
                crc_caught++;
            else
                CHECK(0, "a frame with byte %u changed %02X -> %02X passed unpack and CRC", pos, was, fr[pos]);
        }
    }
    printf("  fuzz: %ld rounds; unpack7 accepted %ld (each exactly a pack7), rec_get accepted %ld (each as the rule "
           "says), %ld corrupted frames all refused\n", iters, unpack_ok, rec_ok, crc_caught);
}

int main(int argc, char **argv)
{
    long iters = 1000000;
    int i;
    for (i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--fuzz"))
            iters = atol(argv[++i]);
        else if (!strcmp(argv[i], "--seed"))
            t_rng = (uint32_t)strtoul(argv[++i], 0, 0) | 1u;
    }
    printf("test_xfer: CYMA-1 Sounds T1 (cy_xfer against tests/sounds/vectors.json)\n");
    test_layouts();
    test_crc();
    test_record();
    test_pack7();
    test_frames();
    test_names();
    test_params_json();
    fuzz(iters);
    printf("test_xfer: %ld checks, %d failed: %s\n", t_checks, t_fail, t_fail ? "FAIL" : "PASS");
    return t_fail ? 1 : 0;
}
