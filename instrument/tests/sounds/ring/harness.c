/* SPDX-License-Identifier: MPL-2.0 */
/* Adversarial routing harness for CYMA-1 Sounds (docs/CYMA-1-sounds-spec.md §5.6).
 *
 * One translation unit, built like the host simulator: fm1-base's host/plat_host.c, which #includes CYMA-1's app.c
 * (and so app_sounds.c with the real app_sysex), then fm1-base's OWN routing code, extracted verbatim by extract.py:
 *   usb.c       sysex_byte / midi_in_event (the ISR's SysEx assembly, the 6-byte update and UBOOT keys), the single
 *               receive slot (ota_frame_get / ota_frame_done) and the TX ring (ota_wire_send)
 *   ota.c       whole: ota_take / ota_service (the identity handshake), pack7
 *   main_fm1.c  remote_frame, usb_cmd, log_frame's accept test, and the main loop's dispatch block itself
 *   backup.c    bu_start / bu_stop / bu_frame and the backup state; unity.c the backup glue and ann_wire_send
 * The dispatch block is compiled twice: as CYMA-1 builds it (APP_SYSEX: app_sysex last in the chain) and as the
 * same platform without the hook (the baseline: a firmware without CYMA-1's handler). Every name the block calls goes
 * through a recorder (gen/dispatch_pre.h), so each received frame's consumer is known.
 *
 * Host -> device bytes go in as USB-MIDI event packets (CIN 4..7 SysEx, channel messages, realtime), 16 per 0.5 ms
 * ISR slot (one EP1 transfer), interleaved between sources as CoreMIDI may; the main loop runs the dispatch at a
 * chosen rate. Device -> host packets are drained from the TX ring 16 per slot and reassembled. On the device
 * app_sounds.c's replies go out through unity.c's ann_wire_send; here PLAT_HOST prints them ("sysex_out HEX"), and
 * the harness routes each one through the extracted ann_wire_send (dev_ann_wire_send), so the gating is the device's.
 *
 * In the tree since the hardening round (spec revision 2026-10-24b R4): the release verifier's harness, extended by
 * the fw group with the send gate's rows (T3c: floods at every host read rate, judged against the same platform
 * without app_sysex; T3d: a normal session; T9: GET_DIR's cached CRC after 20,000 random bank changes). It is the only
 * test of app_sx_send's ring gate: the host simulator and the wasm have no USB send ring. run_all.sh's sounds_ring step
 * runs extract.py into build/run_all/sounds_ring/gen, builds this at -O2 like the host simulator, and runs --quick
 * (about 25 s; the full run, about 100 s, is ./harness with no argument). PASS / FAIL rows are judged; NOTE rows are
 * the measured residual on very slow hosts, printed and not judged. Exit 1 on any failed check.
 *
 * Never touches USB or MIDI: everything is in memory. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int hs_printf(const char *fmt, ...);
/* harden-fw: CYMA-1's send gate (app_sounds.c app_sx_send) reads fm1-base's USB SysEx TX ring. On the device usb.c's
 * so_w / so_r are in scope there (unity.c #includes usb.c ahead of app.c); here the same two objects are declared
 * ahead (C tentative definitions: gen/fm1_extract.c's identical declarations name the same objects) and named for the
 * gate exactly as the device build names them. A build of the pre-fix app_sounds.c ignores the three names. */
static volatile uint32_t so_w, so_r;
#define APP_SX_RING_N 128u
#define APP_SX_RING_W so_w
#define APP_SX_RING_R so_r
#define printf hs_printf
#define main plat_host_main
#include "plat_host.c"
#undef main
#undef printf

/* ------------------------------------------------- the platform, as unity.c sets it up --- */
#define FELUCCA_OTA 1
#define SXQ 128u
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#ifndef FELUCCA_ID
#define FELUCCA_ID "FM-1_9400"
#endif
static struct {
    uint8_t config;
    uint8_t sysex[16];
    uint8_t sx_len, sx_on;
    volatile uint8_t suspended, uboot_req, ota_req;
} usb;
static uint32_t hs_chan_msgs;
static void midi_enqueue(uint32_t pkt, uint32_t n) { (void)pkt, (void)n, hs_chan_msgs++; }
static uint32_t ota_now_ms(void);
static void ota_idle(void);
static void hs_log_frame_taken(void);
static int restore_rx(uint32_t len, uint32_t crc, int *begun_out);
static uint8_t flash_ok = 1;
static struct { int stage; } fm1b_dbg;
static uint32_t hs_ota_flash_ops, hs_ota_commits;
static int ota_erase(uint32_t off) { (void)off, hs_ota_flash_ops++; return -8; }
static int ota_prog(uint32_t off, const void *p, uint32_t n) { (void)off, (void)p, (void)n, hs_ota_flash_ops++; return -8; }
static int ota_fread(uint32_t off, void *p, uint32_t n) { (void)off; memset(p, 0xFF, n); return 0; }
static void ota_show(uint32_t step, int32_t code) { (void)step, (void)code; }
static void ota_commit(const uint8_t *parm) { (void)parm, hs_ota_commits++; }
static int hs_ann_frame(const uint8_t *p, uint32_t n, uint32_t now);   /* the recorders (dispatch_pre.h) */
static int hs_log_frame(const uint8_t *p, uint32_t n);
static int hs_remote_frame(const uint8_t *p, uint32_t n);
static int hs_app_sysex(const uint8_t *p, uint32_t n);
static uint32_t hs_ota_service(void);
static void hs_usb_cmd(uint32_t d);
static void hs_app_panic(void);
static int hs_ota_session(void);
static void hs_enter_uboot(const char *why);
static void hs_bu_step(void);
#include "gen/fm1_extract.c"
#ifdef APP_SX_QUEUED_MAX                       /* harden-fw's app_sounds.c (the send gate, GET_DIR's cached CRC) */
#define HS_FIXED 1
static uint32_t hs_held(void) { return app_sx.held; }
#else                                          /* the pre-fix app_sounds.c: every reply straight to ann_wire_send */
#define HS_FIXED 0
static uint32_t hs_held(void) { return 0; }
#endif
static uint32_t hs_held0;                      /* hs_held() at reset_platform */
static int hs_sink_quiet;                      /* T7: our printed replies are dropped unparsed (the cost of app_sysex
                                                * itself, without the harness's own hex parsing and ring pushes) */

/* ------------------------------------------------------------------- helpers --- */
static int fails, checks;
static FILE *hs_log;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 11); }
static uint32_t rndn(uint32_t n) { return n ? rnd() % n : 0; }

static uint32_t crc32_ref(uint32_t c, const uint8_t *p, uint32_t n)   /* bitwise zlib CRC-32, independent of cy_crc32 */
{
    c = ~c;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = c & 1u ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}
static uint32_t fnv(const uint8_t *p, uint32_t n)
{
    uint32_t h = 2166136261u;
    while (n--)
        h = (h ^ *p++) * 16777619u;
    return h;
}

/* ------------------------------------------------------ frames, host side --- */
typedef struct { uint8_t b[1100]; uint32_t n; } fr_t;

static void fr_ota(fr_t *f, uint8_t cmd, const uint8_t *body, uint32_t n)   /* F0 pack7(00 59 cmd len24 0 body ~sum) F7 */
{
    uint8_t m[700];
    uint32_t s = 0, i;
    m[0] = 0, m[1] = 0x59, m[2] = cmd, m[3] = (uint8_t)n, m[4] = (uint8_t)(n >> 8), m[5] = 0;
    for (i = 0; i < n; i++)
        m[6 + i] = body[i], s += body[i];
    m[6 + n] = (uint8_t)~s;
    f->b[0] = 0xF0;
    f->n = 1 + ota_pack7(m, 7 + n, f->b + 1);
    f->b[f->n++] = 0xF7;
}
static void fr_raw(fr_t *f, const uint8_t *b, uint32_t n) { memcpy(f->b, b, n), f->n = n; }

/* ours: F0 7D 43 59 VER CMD SEQ pack7(payload ‖ CRC32_LE(VER CMD SEQ payload)) F7, built with fm1-base's pack7 and an
 * independent CRC */
static void fr_ours(fr_t *f, uint8_t ver, uint8_t cmd, uint8_t seq, const uint8_t *pl, uint32_t n)
{
    uint8_t m[700], h[3] = {ver, cmd, seq};
    uint32_t c;
    memcpy(m, pl, n);
    c = crc32_ref(crc32_ref(0, h, 3), pl, n);
    m[n] = (uint8_t)c, m[n + 1] = (uint8_t)(c >> 8), m[n + 2] = (uint8_t)(c >> 16), m[n + 3] = (uint8_t)(c >> 24);
    f->b[0] = 0xF0, f->b[1] = 0x7D, f->b[2] = 0x43, f->b[3] = 0x59, f->b[4] = ver, f->b[5] = cmd, f->b[6] = seq;
    f->n = 7 + ota_pack7(m, n + 4, f->b + 7);
    f->b[f->n++] = 0xF7;
}
static uint32_t rec_make(uint8_t *r, int valid)   /* a sound record (§3.7); valid 0: any lengths */
{
    uint32_t N = valid ? 40 : 1 + rndn(70), L = valid ? rndn(15) : rndn(40), i, n;
    r[0] = valid || rndn(8) ? 1 : (uint8_t)rnd(), r[1] = valid || rndn(8) ? 1 : (uint8_t)rnd();
    r[2] = (uint8_t)N, r[3] = (uint8_t)L;
    for (i = 0; i < L; i++)
        r[4 + i] = (uint8_t)(valid ? "ABCDEFGH IJ-K"[rndn(13)] : rnd());
    for (i = 0; i < 2 * N; i++)
        r[4 + L + i] = (uint8_t)rnd();
    n = 4 + L + 2 * N;
    if (!valid && rndn(4) == 0)
        n = rndn(n + 3);                       /* a wrong length */
    return n;
}
static uint8_t seqn;
static void fr_ours_random(fr_t *f, int valid_cmds_only)
{
    uint8_t pl[300];
    uint32_t n = 0;
    uint8_t cmd = (uint8_t)(1 + rndn(9));
    if (!valid_cmds_only && rndn(10) == 0)
        cmd = (uint8_t)rndn(128);
    switch (cmd) {
    case 1: pl[0] = 1, pl[1] = (uint8_t)rndn(4), n = 2; break;
    case 3: pl[0] = (uint8_t)rndn(4), n = 1 + rec_make(pl + 1, rndn(3) != 0); break;
    case 4: pl[0] = (uint8_t)rndn(36), n = 1; break;
    case 5: pl[0] = (uint8_t)rndn(36), n = 1 + rec_make(pl + 1, rndn(3) != 0); break;
    case 6: pl[0] = (uint8_t)rndn(36), n = 1; break;
    case 8: pl[0] = 0, n = 1; break;
    default: n = rndn(4) == 0 ? rndn(256) : 0; for (uint32_t i = 0; i < n; i++) pl[i] = (uint8_t)rnd(); break;
    }
    if (n > 256)
        n = 256;
    fr_ours(f, rndn(30) ? 1 : (uint8_t)rndn(128), cmd, seqn = (uint8_t)((seqn + 1) & 127), pl, n);
}
static void fr_mutate(fr_t *f)   /* damage one of ours, keeping it 7-bit inside unless asked */
{
    switch (rndn(7)) {
    case 0: if (f->n > 9) f->b[7 + rndn(f->n - 8)] ^= (uint8_t)(1 + rndn(127)); break;       /* a byte (CRC/pack7) */
    case 1: if (f->n > 9) { uint32_t k = 7 + rndn(f->n - 8); memmove(f->b + k, f->b + k + 1, f->n - k - 1); f->n--; } break;
    case 2: if (f->n < 1000) { uint32_t k = 7 + rndn(f->n - 7); memmove(f->b + k + 1, f->b + k, f->n - k); f->b[k] = (uint8_t)rndn(128); f->n++; } break;
    case 3: f->n = 4 + rndn(f->n - 4); break;                                                  /* truncated: no F7 */
    case 4: f->b[f->n - 2] |= 0x40; break;                                                     /* last byte's spare bits */
    case 5: while (f->n < 700) f->b[f->n - 1] = (uint8_t)rndn(128), f->b[f->n++] = 0xF7; break;   /* past 640 */
    default: f->b[4] = (uint8_t)rndn(128); break;                                               /* version */
    }
}

/* the platform's frames: each with its expected consumer */
enum { K_HSQ, K_OTAKEY, K_UBOOTKEY, K_BACKUP, K_RESTORE, K_MIRROR, K_APPCMD, K_REMOTE_BTN, K_REMOTE_KEYS, K_REMOTE_ENC,
       K_REMOTE_MASTER, K_REMOTE_OP9, K_LOG, K_ANN_HELLO, K_ANN_BYE, K_ANN_WHERE, K_WIFI, K_EDITOR, K_NEAR1, K_NEAR2,
       K_NEAR3, K_SHORT1, K_SHORT2, K_N };
static const char *const KNAME[K_N] = {"HS_QUERY", "OTA key F0 22 24 35 7F F7", "UBOOT key F0 22 24 35 7D F7",
    "backup 0x42", "restore 0x41", "mirror MR_REQ 0x53", "APP_USB_CMD-style 0x60", "remote 7D 4C 43 01 (buttons)",
    "remote 02 (keys)", "remote 03 (encoder)", "remote 04 (MASTER)", "remote op 9", "log 7D 4C 47", "screen reader HELLO",
    "screen reader BYE", "screen reader WHERE", "wifi 7D 4C 57", "Felucca editor 7D 46 4C", "near miss 7D 43 58",
    "near miss 7D 42 59", "near miss FD 43 59 (bit 7)", "F0 7D 43 F7", "F0 7D F7"};
static void fr_platform(fr_t *f, int k)
{
    static const uint8_t HSQ[] = {0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7};
    uint8_t b[16];
    switch (k) {
    case K_HSQ: fr_raw(f, HSQ, sizeof HSQ); break;
    case K_OTAKEY: { const uint8_t x[] = {0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7}; fr_raw(f, x, 6); break; }
    case K_UBOOTKEY: { const uint8_t x[] = {0xF0, 0x22, 0x24, 0x35, 0x7D, 0xF7}; fr_raw(f, x, 6); break; }
    case K_BACKUP: bu_wr32(b, BU_MAGIC); fr_ota(f, BU_REQ, b, 4); break;
    case K_RESTORE: bu_wr32(b, 64), bu_wr32(b + 4, 0x12345678u); fr_ota(f, RS_REQ, b, 8); break;
    case K_MIRROR: b[0] = 1; fr_ota(f, 0x53, b, 1); break;
    case K_APPCMD: b[0] = 1, b[1] = 2, b[2] = 3, b[3] = 4, b[4] = 5; fr_ota(f, 0x60, b, 5); break;
    case K_REMOTE_BTN: { const uint8_t x[] = {0xF0, 0x7D, 0x4C, 0x43, 1, 0, 0, 0xF7}; fr_raw(f, x, 8); break; }
    case K_REMOTE_KEYS: { const uint8_t x[] = {0xF0, 0x7D, 0x4C, 0x43, 2, 0, 0, 0, 0, 0xF7}; fr_raw(f, x, 10); break; }
    case K_REMOTE_ENC: { const uint8_t x[] = {0xF0, 0x7D, 0x4C, 0x43, 3, 9, 64, 0xF7}; fr_raw(f, x, 8); break; }
    case K_REMOTE_MASTER: { const uint8_t x[] = {0xF0, 0x7D, 0x4C, 0x43, 4, 0x7F, 0x7F, 0xF7}; fr_raw(f, x, 8); break; }
    case K_REMOTE_OP9: { const uint8_t x[] = {0xF0, 0x7D, 0x4C, 0x43, 9, 0xF7}; fr_raw(f, x, 6); break; }
    case K_LOG: { const uint8_t x[] = {0xF0, 0x7D, 0x4C, 0x47, 0xF7}; fr_raw(f, x, 5); break; }
    case K_ANN_HELLO: { const uint8_t x[] = {0xF0, 0x7D, 0x53, 0x52, 1, 1, 0xF7}; fr_raw(f, x, 7); break; }
    case K_ANN_BYE: { const uint8_t x[] = {0xF0, 0x7D, 0x53, 0x52, 2, 0xF7}; fr_raw(f, x, 6); break; }
    case K_ANN_WHERE: { const uint8_t x[] = {0xF0, 0x7D, 0x53, 0x52, 3, 0xF7}; fr_raw(f, x, 6); break; }
    case K_WIFI: { const uint8_t x[] = {0xF0, 0x7D, 0x4C, 0x57, 4, 0xF7}; fr_raw(f, x, 6); break; }
    case K_EDITOR: { const uint8_t x[] = {0xF0, 0x7D, 0x46, 0x4C, 1, 0xF7}; fr_raw(f, x, 6); break; }
    case K_NEAR1: { const uint8_t x[] = {0xF0, 0x7D, 0x43, 0x58, 1, 1, 1, 0x10, 0x20, 0xF7}; fr_raw(f, x, 10); break; }
    case K_NEAR2: { const uint8_t x[] = {0xF0, 0x7D, 0x42, 0x59, 1, 1, 1, 0x10, 0x20, 0xF7}; fr_raw(f, x, 10); break; }
    case K_NEAR3: { const uint8_t x[] = {0xF0, 0x7C, 0x43, 0x59, 1, 1, 1, 0x10, 0x20, 0xF7}; fr_raw(f, x, 10); break; }
    case K_SHORT1: { const uint8_t x[] = {0xF0, 0x7D, 0x43, 0xF7}; fr_raw(f, x, 4); break; }
    default: { const uint8_t x[] = {0xF0, 0x7D, 0xF7}; fr_raw(f, x, 3); break; }
    }
}

/* ---------------------------------------------- USB-MIDI packets, both ways --- */
#define PQ (1u << 22)
static uint32_t *pq;                           /* host -> device event packets, in order */
static uint32_t pq_w, pq_r;
static uint32_t pkt_of(uint32_t cin, const uint8_t *b, uint32_t k)
{
    return cin | (uint32_t)b[0] << 8 | (k > 1 ? (uint32_t)b[1] << 16 : 0u) | (k > 2 ? (uint32_t)b[2] << 24 : 0u);
}
/* a SysEx (F0 .. F7, or cut short) as CoreMIDI sends it: CIN 4 for three bytes that go on, 5/6/7 for the end */
static uint32_t sx_packets(const uint8_t *b, uint32_t n, uint32_t *out)
{
    uint32_t i = 0, o = 0;
    while (i < n) {
        uint32_t k = n - i >= 3 ? 3 : n - i;
        int ends = b[i + k - 1] == 0xF7;
        out[o++] = pkt_of(ends ? 4 + k : (k == 3 ? 4 : 4 + k), b + i, k);
        i += k;
    }
    return o;
}
static void pq_push(uint32_t p) { if (pq_w - pq_r < PQ) pq[pq_w++ % PQ] = p; }
static void send_frame(const fr_t *f)
{
    uint32_t t[400], k = sx_packets(f->b, f->n, t);
    for (uint32_t i = 0; i < k; i++)
        pq_push(t[i]);
}
static void send_note(void) { uint8_t b[3] = {0x90, 60, 100}; pq_push(pkt_of(9, b, 3)); }
static void send_clock(void) { uint8_t b[1] = {0xF8}; pq_push(pkt_of(15, b, 1)); }

/* device -> host: the TX ring drained, packets reassembled into frames */
typedef struct { uint32_t frames, bad, ident, ours, ours_bad, remote_ack, log, bu_data, bu_end, other; uint8_t ident_ok; } txs_t;
static txs_t tx;
static uint8_t tx_ours_last[320];              /* harden-fw: the last frame of ours the host received (T3d) */
static uint32_t tx_ours_last_n;
static uint8_t txf[2048];
static uint32_t txn;
static int tx_in;
static void tx_frame(const uint8_t *f, uint32_t n)
{
    uint8_t d[700];
    uint32_t i, m;
    tx.frames++;
    for (i = 1; i + 1 < n; i++)
        if (f[i] & 0x80) { tx.bad++; return; }
    if (n >= 4 && f[1] == 0x7D && f[2] == 0x43 && f[3] == 0x59) {
        tx.ours++;
        if (n > 306 || n < 13) tx.ours_bad++;
        if (n <= sizeof tx_ours_last) memcpy(tx_ours_last, f, n), tx_ours_last_n = n;
        return;
    }
    if (n >= 5 && f[1] == 0x7D && f[2] == 0x4C && f[3] == 0x43) { tx.remote_ack++; return; }
    if (n >= 4 && f[1] == 0x7D && f[2] == 0x4C && f[3] == 0x47) { tx.log++; return; }
    m = ota_unpack7(f + 1, n - 2, d, sizeof d);
    if (m >= 7 && d[0] == 0 && d[1] == 0x59) {
        if (d[2] == 0x11 && m == 34) {
            tx.ident++;
            tx.ident_ok = !memcmp(d + 6, FELUCCA_ID, sizeof FELUCCA_ID - 1);
        } else if (d[2] == BU_REQ) tx.bu_data++;
        else if (d[2] == BU_END) tx.bu_end++;
        else tx.other++;
        return;
    }
    tx.other++;
}
static void tx_byte(uint8_t b)
{
    if (b == 0xF0) {
        if (tx_in) tx.bad++;                   /* a frame begun inside another: interleaved or cut */
        tx_in = 1, txn = 0;
    }
    if (!tx_in) { tx.bad++; return; }
    if (txn < sizeof txf) txf[txn++] = b;
    if (b == 0xF7) { tx_frame(txf, txn); tx_in = 0; }
}
static void tx_drain(uint32_t max)
{
    while (so_r != so_w && max--) {
        uint32_t p = sx_out_q[so_r % SXQ], cin = p & 15u, k = cin == 4 || cin == 7 ? 3 : cin == 6 ? 2 : cin == 5 ? 1 : 0;
        so_r++;
        for (uint32_t i = 0; i < k; i++)
            tx_byte((uint8_t)(p >> (8 * (i + 1))));
    }
}

/* the ISR's 0.5 ms: one EP1 OUT transfer (16 events) in, 16 packets out */
static uint32_t hs_isr_slots, tx_per_slot = 16, tx_every = 1;   /* the host's IN polling: n packets every k slots */
static void isr_slot(void)
{
    uint32_t k = 0;
    hs_isr_slots++;
    while (pq_r != pq_w && k++ < 16)
        midi_in_event(pq[pq_r++ % PQ]);
    if (hs_isr_slots % tx_every == 0)
        tx_drain(tx_per_slot);
}

/* ------------------------------------------------ recorders (dispatch_pre.h) --- */
enum { C_ANN = 1, C_LOG, C_REMOTE, C_APP_TOOK, C_APP_DECLINED, C_OTA, C_USBCMD, C_OTAREQ, C_UBOOT, C_PANIC };
typedef struct { uint32_t h; uint16_t n; uint8_t cons, cmd, ours; int32_t d; } ev_t;
#define NEV (1u << 21)
static ev_t *evs;
static uint32_t nev;
static void ev(uint8_t cons, const uint8_t *p, uint32_t n, int32_t d, uint8_t cmd)
{
    if (nev < NEV) {
        evs[nev].h = p ? fnv(p, n) : 0, evs[nev].n = (uint16_t)n, evs[nev].cons = cons, evs[nev].cmd = cmd, evs[nev].d = d;
        evs[nev].ours = p && n >= 3 && p[0] == 0x7D && p[1] == 0x43 && p[2] == 0x59;
        nev++;
    }
}
static uint32_t hs_app_calls, hs_app_took, hs_log_taken, hs_ota_reqs, hs_uboots, hs_panics, hs_busy_waits, hs_restores;
static uint32_t hs_bu_starts, hs_bu_stalls, hs_sent_ours, hs_dropped_ours;
static int hs_ann_frame(const uint8_t *p, uint32_t n, uint32_t now)
{
    int r = ann_frame(p, n, now);
    if (r) ev(C_ANN, p, n, 0, 0);
    return r;
}
static void hs_log_frame_taken(void) { hs_log_taken++; }
static int hs_log_frame(const uint8_t *p, uint32_t n)
{
    int r = log_frame(p, n);
    if (r) ev(C_LOG, p, n, 0, 0);
    return r;
}
static int hs_remote_frame(const uint8_t *p, uint32_t n)
{
    int r = remote_frame(p, n);
    if (r) ev(C_REMOTE, p, n, 0, 0);
    return r;
}
static int hs_app_sysex(const uint8_t *p, uint32_t n)
{
    int r;
    hs_app_calls++;
    r = app_sysex(p, n);
#if HS_MUTANT == 1                             /* a broken handler: takes the updater's 00 59 frames too */
    if (!r && n >= 1 && p[0] == 0x00) r = 1;
#elif HS_MUTANT == 2                           /* a broken handler: takes every 7D frame (as Dinghy routes them) */
    if (!r && n >= 1 && p[0] == 0x7D) r = 1;
#endif
    hs_app_took += r != 0;
    ev(r ? C_APP_TOOK : C_APP_DECLINED, p, n, 0, 0);
    return r;
}
static uint32_t hs_ota_service(void)
{
    uint8_t cp[640];
    uint32_t n = 0, d;
    int had = sx_ready != 0;
    if (had) n = sx_frame_len, memcpy(cp, sx_frame, n);
    d = ota_service();
    if (had) ev(C_OTA, cp, n, (int32_t)d, d >= 3 ? ota_dec[2] : 0);
    return d;
}
static void hs_usb_cmd(uint32_t d)
{
    if (d >= 7u) ev(C_USBCMD, 0, 0, (int32_t)d, ota_dec[2]);
    usb_cmd(d);
}
static void hs_app_panic(void) { hs_panics++; ev(C_PANIC, 0, 0, 0, 0); app_panic(); }
static int hs_ota_session(void) { hs_ota_reqs++; ev(C_OTAREQ, 0, 0, 0, 0); return -1; }
static void hs_enter_uboot(const char *why) { (void)why; hs_uboots++; usb.uboot_req = 0; ev(C_UBOOT, 0, 0, 0, 0); }
#define HS_BU_IMG 1024u
static void hs_bu_step(void)   /* bu_step's shape (room, stall, one frame a pass, the end), its body modelled */
{
    uint8_t b[4 + BU_CHUNK];
    int r;
    if (!bu.on)
        return;
    r = bu_room();
    if (r <= 0) {
        if (r < 0 || ota_now_ms() - bu.last > BU_STALL_MS)
            bu_stop(), hs_bu_stalls++;
        return;
    }
    memset(b, 0x55, sizeof b);
    if (bu.off < HS_BU_IMG) {
        bu_wr32(b, bu.off);
        bu_frame(BU_REQ, b, 4u + BU_CHUNK);
        bu.off += BU_CHUNK;
        bu.last = ota_now_ms();
        return;
    }
    bu_wr32(b, bu.off), bu_wr32(b + 4, 0), b[8] = 0, b[9] = bu.flags;
    bu_frame(BU_END, b, 10);
    bu_stop();
}
static int hs_restore_mode;                    /* 0: the player says no; 1: restored (rs_reload) */
static int restore_rx(uint32_t len, uint32_t crc, int *begun_out)
{
    (void)len, (void)crc;
    hs_restores++;
    if (app_backup_playing()) { *begun_out = 0; return RS_E_PLAYING; }
    app_panic();                               /* rs_begin */
    *begun_out = 1;
    if (hs_restore_mode) { app_panic(); app_backup_restored(); return 0; }   /* rs_reload */
    return RS_E_NO;
}
static uint32_t ota_now_ms(void) { return now_ms; }
static uint32_t hs_idle_guard;
static uint32_t hs_idle_half;
static void ota_idle(void)                     /* a busy-wait in ota_wire_send: the ISR goes on meanwhile */
{
    hs_busy_waits++;
    if (++hs_idle_half % 2u == 0u)             /* harden-fw: and so does the 1 kHz timer (two slots a millisecond), so
                                                * ota_wire_send's own 200 ms limit is reached against a host that
                                                * never reads, as on the device */
        now_ms++;
    if (++hs_idle_guard > 10000000u) { fprintf(stderr, "FAIL ota_idle: stuck\n"); exit(3); }
    isr_slot();
}

/* app_sounds.c's replies (PLAT_HOST prints them) through the device's ann_wire_send */
static char hp_line[4096];
static uint32_t hp_n;
static void hp_flush_line(void)
{
    static const char P[] = "sysex_out ";
    hp_line[hp_n] = 0;
    if (hs_sink_quiet && !strncmp(hp_line, P, sizeof P - 1)) {
        hp_n = 0;
        return;
    }
    if (!strncmp(hp_line, P, sizeof P - 1)) {
        uint8_t f[700];
        uint32_t n = 0;
        for (const char *h = hp_line + sizeof P - 1; h[0] && h[1] && n < sizeof f; h += 2) {
            unsigned v;
            if (sscanf(h, "%2x", &v) != 1) break;
            f[n++] = (uint8_t)v;
        }
        if (dev_ann_wire_send(f, n) == 0) hs_sent_ours++; else hs_dropped_ours++;
    } else if (hs_log) {
        fputs(hp_line, hs_log), fputc('\n', hs_log);
    }
    hp_n = 0;
}
static int hs_printf(const char *fmt, ...)
{
    char b[8192];
    va_list ap;
    int k;
    va_start(ap, fmt);
    k = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    for (int i = 0; i < k && i < (int)sizeof b - 1; i++) {
        if (b[i] == '\n') hp_flush_line();
        else if (hp_n < sizeof hp_line - 1) hp_line[hp_n++] = b[i];
    }
    return k;
}

/* --------------------------------------------------------- the device loop --- */
static int with_app = 1;                       /* 1: CYMA-1's dispatch (app_sysex last); 0: the same without it */
static uint32_t phys_btn;
static void dispatch(void) { if (with_app) fm1_dispatch_with(); else fm1_dispatch_without(); }
/* one millisecond: two ISR slots, the main loop's dispatch `per_ms` times (0: once every `every` ms), the app */
static uint32_t tick;                          /* the main loop's phase (reset per run: both runs alike) */
static void dev_ms(int per_ms, int every)
{
    for (int s = 0; s < 2; s++) {
        isr_slot();
        if (per_ms > 0)
            for (int k = 0; k < (per_ms + 1) / 2; k++) dispatch();
    }
    if (per_ms <= 0 && (++tick % (uint32_t)(every > 0 ? every : 1)) == 0)
        dispatch();
    held_btn = phys_btn | remote.btn;          /* plat_fm1.c plat_buttons: the panel's own | remote.btn */
    run_ms(1);
}
static void dev_run(uint32_t ms, int per_ms, int every) { while (ms--) dev_ms(per_ms, every); }
static void reset_platform(void)
{
    memset(&usb, 0, sizeof usb);
    usb.config = 1;
    sx_pos = sx_frame_len = 0, sx_ready = sx_collect = sx_busy = 0;
    so_w = so_r = 0, pq_w = pq_r = 0;
    memset(&tx, 0, sizeof tx), tx_in = 0, txn = 0;
    if (bu.on) bu_stop();
    memset(&bu, 0, sizeof bu);
    remote.btn = remote.keys = 0, remote.master = -1;
    memset((void *)remote.enc, 0, sizeof remote.enc);
    nev = 0;
    hs_app_calls = hs_app_took = hs_log_taken = hs_ota_reqs = hs_uboots = hs_panics = hs_busy_waits = 0;
    hs_restores = hs_bu_starts = hs_bu_stalls = hs_sent_ours = hs_dropped_ours = 0;
    hs_held0 = hs_held();
    hs_ota_flash_ops = hs_ota_commits = 0, hs_idle_guard = 0;
    phys_btn = 0, held_btn = 0;
    tick = 0;
}
static void press(int b, int ms) { phys_btn |= 1u << b; dev_run(40, 2, 0); phys_btn &= ~(1u << b); dev_run((uint32_t)ms, 2, 0); }

/* drive our handler into a state: 0 idle (HELLO), 1 staged (PUT_SLOT), 2 asking (PUT_SLOT + COMMIT) */
static void our_state(int st)
{
    fr_t f;
    uint8_t pl[120];
    pl[0] = 1, pl[1] = 3;
    fr_ours(&f, 1, 1, seqn = (uint8_t)((seqn + 1) & 127), pl, 2);
    send_frame(&f);
    dev_run(20, 2, 0);
    if (st >= 1) {
        pl[0] = (uint8_t)rndn(32);
        uint32_t n = 1 + rec_make(pl + 1, 1);
        fr_ours(&f, 1, 5, seqn = (uint8_t)((seqn + 1) & 127), pl, n);
        send_frame(&f);
        dev_run(20, 2, 0);
    }
    if (st >= 2) {
        pl[0] = 0;
        fr_ours(&f, 1, 8, seqn = (uint8_t)((seqn + 1) & 127), pl, 1);
        send_frame(&f);
        dev_run(20, 2, 0);
    }
}

/* =================================================================== T1: the routing matrix === */
/* every platform frame, in each state of ours and after each kind of frame of ours, reaches the platform's handler
 * (and does its job), and app_sysex never claims it */
static int expect_consumer(int k)
{
    switch (k) {
    case K_HSQ: case K_BACKUP: case K_RESTORE: case K_MIRROR: case K_APPCMD: case K_WIFI: case K_EDITOR:
    case K_NEAR1: case K_NEAR2: case K_NEAR3: case K_SHORT1: case K_SHORT2: return C_OTA;
    case K_OTAKEY: return C_OTAREQ;
    case K_UBOOTKEY: return C_UBOOT;
    case K_LOG: return C_LOG;
    case K_ANN_HELLO: case K_ANN_BYE: case K_ANN_WHERE: return C_ANN;
    default: return C_REMOTE;
    }
}
static const char *const PRE[] = {"nothing", "a valid frame of ours (same transfer)", "a damaged frame of ours",
    "a truncated frame of ours (no F7)", "an oversized frame of ours (> 640 B)", "a frame of ours cut by a note-on",
    "realtime clocks inside the platform frame", "ours with CMD >= 0x40 (an echo)", "ours with VER 2"};
#define NPRE 9
/* one cell: our handler in state st, a prelude frame `pre`, then platform frame k; gap 1: the main loop gets a pass
 * between them (the prelude in its own ISR slot), gap 0: both in one EP1 transfer. Returns 1 when k reached its
 * consumer and did its job; *claimed: app_sysex took it */
static int t1_cell(int st, int pre, int k, int gap, int app, int *claimed)
{
    fr_t f, g;
    int want = expect_consumer(k), got_cons = 0, effect = 0, have_pre = 1;
    uint32_t e0, ident0, bu0, ack0;
    reset_platform();
    with_app = app;
    if (st < 3) our_state(st);
    else { bu_wr32(g.b, BU_MAGIC); fr_ota(&f, BU_REQ, g.b, 4); send_frame(&f); dev_run(2, 2, 0); }   /* 3: a backup streaming */
    if (app) CHECK(st == 3 || app_sx.state == (st == 2 ? 2 : st), "state %d not reached (%d)", st, app_sx.state);
    e0 = nev, ident0 = tx.ident, bu0 = tx.bu_end, ack0 = tx.remote_ack;
    fr_platform(&f, k);
    switch (pre) {
    case 1: fr_ours_random(&g, 1); send_frame(&g); break;
    case 2: fr_ours_random(&g, 1); fr_mutate(&g); g.b[g.n - 1] = 0xF7; send_frame(&g); break;
    case 3: fr_ours_random(&g, 1); g.n = 4 + rndn(g.n - 5); send_frame(&g); break;
    case 4: fr_ours_random(&g, 1); while (g.n < 700) g.b[g.n - 1] = 0x11, g.b[g.n++] = 0xF7; send_frame(&g); break;
    case 5: fr_ours_random(&g, 1); { uint32_t t[400], m = sx_packets(g.b, g.n, t); for (uint32_t i = 0; i < m; i++) { if (i == m / 2) send_note(); pq_push(t[i]); } } break;
    case 7: { uint8_t pl[4] = {0}; fr_ours(&g, 1, (uint8_t)(0x40 + rndn(64)), 1, pl, 1); send_frame(&g); } break;
    case 8: { uint8_t pl[4] = {0}; fr_ours(&g, 2, 7, 1, pl, 0); send_frame(&g); } break;
    default: have_pre = 0; break;
    }
    if (have_pre && gap) dev_run(3, 2, 0);    /* the prelude delivered and dispatched first */
    if (pre == 6) { uint32_t t[400], m = sx_packets(f.b, f.n, t); for (uint32_t i = 0; i < m; i++) { pq_push(t[i]); send_clock(); } }
    else send_frame(&f);
    dev_run(60, 2, 0);                         /* a fast main loop: a dispatch after every ISR slot */
    *claimed = 0;
    for (uint32_t i = e0; i < nev; i++) {
        if (evs[i].h == fnv(f.b + 1, f.n - 2) && evs[i].n == f.n - 2) {
            if (evs[i].cons == C_APP_TOOK) *claimed = 1;
            if (evs[i].cons == want) got_cons = 1;
        }
        if ((want == C_OTAREQ || want == C_UBOOT) && evs[i].cons == want) got_cons = 1;
    }
    switch (k) {                               /* the handler's job, seen from outside */
    case K_HSQ: effect = tx.ident > ident0 && tx.ident_ok; break;
    case K_OTAKEY: effect = hs_ota_reqs == 1 && hs_panics >= 1 && (!app || (app_sx.state != 2 && app_sx.state != 1)); break;
    case K_UBOOTKEY: effect = hs_uboots == 1; break;
    case K_BACKUP: effect = tx.bu_end > bu0; break;
    case K_RESTORE: effect = hs_restores >= 1; break;
    case K_MIRROR: case K_APPCMD: { effect = 0; for (uint32_t i = e0; i < nev; i++) if (evs[i].cons == C_USBCMD && evs[i].cmd == (k == K_MIRROR ? 0x53 : 0x60)) effect = 1; break; }
    case K_REMOTE_BTN: case K_REMOTE_KEYS: case K_REMOTE_ENC: case K_REMOTE_MASTER: case K_REMOTE_OP9: effect = tx.remote_ack > ack0 || k == K_REMOTE_OP9; break;
    case K_LOG: effect = hs_log_taken >= 1; break;
    case K_ANN_HELLO: effect = ann.on == 1; break;
    case K_ANN_BYE: effect = ann.on == 0; break;
    default: effect = 1; break;                /* (dropped by ota_take: nothing to see but the consumer) */
    }
    CHECK(tx.bad == 0, "TX stream malformed in cell st %d pre %d k %s (%u)", st, pre, KNAME[k], tx.bad);
    CHECK(hs_ota_commits == 0 && hs_ota_flash_ops == 0, "update flash touched");
    if (bu.on) bu_stop();
    remote.btn = 0;
    return got_cons && effect;
}
static void t1_routing_matrix(void)
{
    int bad = 0, cells = 0, same_with = 0, same_without = 0, same_diff = 0, same_cells = 0;
    fprintf(stderr, "T1 routing matrix: %d platform frames x 4 states of ours (idle, staged, asking, a backup streaming) "
            "x %d preludes\n", K_N, NPRE);
    for (int st = 0; st < 4; st++)
        for (int pre = 0; pre < NPRE; pre++)
            for (int k = 0; k < K_N; k++) {
                int claimed, claimed0, ok, ok_with, ok_without;
                uint64_t keep = rng;
                ok = t1_cell(st, pre, k, 1, 1, &claimed);
                cells++;
                if (claimed || !ok) {
                    bad++;
                    fprintf(stderr, "  FAIL cell: state %d, before it %s, %s: claimed by app_sysex %d, delivered+effect %d\n",
                            st, PRE[pre], KNAME[k], claimed, ok);
                }
                CHECK(!claimed && ok, "routing cell st %d pre %d k %s", st, pre, KNAME[k]);
                /* the same with no gap (one EP1 transfer), with and without app_sysex: the platform's single slot
                 * drops a frame that lands while another is pending; the drop must not depend on our handler */
                if (pre == 0 || pre == 6) continue;
                rng = keep;
                ok_with = t1_cell(st, pre, k, 0, 1, &claimed);
                rng = keep;
                ok_without = t1_cell(st, pre, k, 0, 0, &claimed0);
                same_cells++;
                same_with += ok_with, same_without += ok_without;
                if (k == K_OTAKEY || k == K_UBOOTKEY)   /* the keys are read in the ISR whatever the slot holds */
                    CHECK(ok_with && ok_without, "T1: %s behind %s in one transfer not served (%d/%d)", KNAME[k], PRE[pre], ok_with, ok_without);
                if (ok_with != ok_without || claimed) {
                    same_diff++;
                    fprintf(stderr, "  FAIL same-transfer cell differs: state %d, %s, %s: with %d, without %d, claimed %d\n",
                            st, PRE[pre], KNAME[k], ok_with, ok_without, claimed);
                }
            }
    CHECK(same_diff == 0, "T1 same-transfer cells differ: %d", same_diff);
    fprintf(stderr, "T1 %s: %d cells delivered with a main-loop pass between (%d failed); %d same-transfer cells: "
            "platform frame served in %d with app_sysex, %d without it (%d differ; the rest dropped by usb.c's single "
            "slot, as on any fm1-base firmware, and retried by the host)\n",
            bad || same_diff ? "FAIL" : "PASS", cells, bad, same_cells, same_with, same_without, same_diff);
}

/* ================================================== T2/T3: with vs without app_sysex, same traffic === */
typedef struct { int nmsg, per_ms, every, ours_pct, damage_pct, note_pct; } sched_t;
static uint64_t sched_seed;
static void gen_traffic(const sched_t *s, uint32_t *n_platform, uint32_t *n_keys)
{
    /* two sources (ours and the platform's), their packets interleaved as a host may merge them */
#define GT_MAX (1u << 20)
    static uint32_t A[GT_MAX], B[GT_MAX];
    uint32_t na = 0, nb = 0, ia = 0, ib = 0;
    *n_platform = *n_keys = 0;
    for (int m = 0; m < s->nmsg; m++) {
        fr_t f;
        uint32_t t[400], k;
        if ((int)rndn(100) < s->ours_pct) {
            fr_ours_random(&f, 0);
            if ((int)rndn(100) < s->damage_pct) fr_mutate(&f);
            k = sx_packets(f.b, f.n, t);
            for (uint32_t i = 0; i < k && na < GT_MAX; i++) A[na++] = t[i];
        } else {
            int kk = (int)rndn(K_N);
            if (kk == K_UBOOTKEY) kk = K_HSQ;      /* (the UBOOT key resets the chip: tested on its own in T1) */
            if (kk == K_REMOTE_BTN) kk = K_REMOTE_MASTER;   /* (no button presses here: a SAVE would be a press) */
            fr_platform(&f, kk);
            if (kk == K_OTAKEY) (*n_keys)++;
            (*n_platform)++;
            k = sx_packets(f.b, f.n, t);
            for (uint32_t i = 0; i < k && nb < GT_MAX; i++) B[nb++] = t[i];
        }
        if ((int)rndn(100) < s->note_pct && na < GT_MAX) { uint8_t nn[3] = {0x90, 60, 100}; A[na++] = pkt_of(9, nn, 3); }
    }
    /* merge: a source's SysEx packets stay in order; a message from the other source may land between them only at
     * message boundaries on a well-behaved host (CoreMIDI sends a SysEx whole), but notes may land anywhere */
    CHECK(na < GT_MAX && nb < GT_MAX, "gen_traffic: buffer full (%u/%u)", na, nb);
    while (ia < na || ib < nb) {
        int take_a = ib >= nb || (ia < na && rndn(2));
        uint32_t *S = take_a ? A : B, *i = take_a ? &ia : &ib, n = take_a ? na : nb;
        do pq_push(S[*i]), (*i)++;              /* one whole message: up to its end packet (CIN 5/6/7) or a note */
        while (*i < n && !((S[*i - 1] & 15u) >= 5u && (S[*i - 1] & 15u) <= 7u) && (S[*i - 1] & 15u) == 4u);
    }
}
typedef struct { uint32_t nev, ota_reqs, panics, ident, bu_end, bu_data, remote_ack, log, restores, app_took,
                 ours_reached_ota_accepted, tx_bad, busy, n_platform_cons, sent, dropped, slots; uint32_t h; } run_t;
static void run_once(const sched_t *s, int app, run_t *r)
{
    uint32_t np, nk;
    rng = sched_seed;
    reset_platform();
    with_app = app;
    hs_restore_mode = 0;
    gen_traffic(s, &np, &nk);
    while (pq_r != pq_w) dev_ms(s->per_ms, s->every);
    dev_run(200, s->per_ms, s->every);
    memset(r, 0, sizeof *r);
    r->h = 2166136261u;
    for (uint32_t i = 0; i < nev; i++) {          /* the platform's consumption, in order (ours excluded) */
        const ev_t *e = &evs[i];
        if (e->cons == C_APP_TOOK || e->cons == C_APP_DECLINED) continue;
        if (e->ours) {                            /* a frame of ours reaching the platform's own consumers */
            if (e->cons != C_OTA || e->d != 0) r->ours_reached_ota_accepted++;
            continue;                             /* (baseline only: CYMA-1's build takes them in app_sysex) */
        }
        r->h = (r->h ^ e->cons) * 16777619u;
        r->h = (r->h ^ e->h) * 16777619u;
        r->h = (r->h ^ (uint32_t)e->d) * 16777619u;
        r->h = (r->h ^ e->cmd) * 16777619u;
        r->n_platform_cons++;
    }
    r->nev = nev, r->ota_reqs = hs_ota_reqs, r->panics = hs_panics, r->ident = tx.ident, r->bu_end = tx.bu_end;
    r->bu_data = tx.bu_data, r->remote_ack = tx.remote_ack, r->log = hs_log_taken, r->restores = hs_restores;
    r->app_took = hs_app_took, r->tx_bad = tx.bad, r->busy = hs_busy_waits, r->sent = hs_sent_ours;
    r->dropped = hs_dropped_ours, r->slots = hs_isr_slots;
    CHECK(hs_ota_commits == 0 && hs_ota_flash_ops == 0, "update flash touched");
    (void)np, (void)nk;
}
/* our frames' hashes, to tell them apart in the baseline's ota_service events */
static int is_ours_ev(const ev_t *e) { (void)e; return 0; }
static void t2_differential(int rounds, const char *label, sched_t base, int vary)
{
    int diff = 0;
    uint64_t pc_with = 0, pc_without = 0, busy_with = 0, busy_without = 0, took = 0, sent = 0, dropped = 0, bu_with = 0, bu_without = 0;
    for (int r = 0; r < rounds; r++) {
        sched_t s = base;
        run_t a, b;
        sched_seed = 0x1234567ull + (uint64_t)r * 0x9E3779B97F4A7C15ull;
        rng = sched_seed ^ 0xABCDEF;
        if (vary) {
            s.nmsg = 20 + (int)rndn(200);
            s.per_ms = (int)rndn(5) - 1;           /* -1/0: once every `every` ms; 1..3 a dispatch every slot or more */
            s.every = 1 + (int)rndn(20);
            s.ours_pct = (int)rndn(100);
            s.damage_pct = (int)rndn(60);
            s.note_pct = (int)rndn(30);
        }
        run_once(&s, 1, &a);
        run_once(&s, 0, &b);
        pc_with += a.n_platform_cons, pc_without += b.n_platform_cons;
        busy_with += a.busy, busy_without += b.busy, took += a.app_took, sent += a.sent, dropped += a.dropped;
        bu_with += a.bu_end, bu_without += b.bu_end;
        if (a.h != b.h || a.ota_reqs != b.ota_reqs || a.ident != b.ident || a.remote_ack != b.remote_ack ||
            a.log != b.log || a.restores != b.restores || a.bu_end != b.bu_end) {
            diff++;
            if (diff <= 5)
                fprintf(stderr, "  %s round %d DIFFERS: platform events %u/%u, ota_req %u/%u, ident %u/%u, acks %u/%u, "
                        "log %u/%u, restores %u/%u, backup ends %u/%u (with/without; per_ms %d every %d ours %d%%)\n",
                        label, r, a.n_platform_cons, b.n_platform_cons, a.ota_reqs, b.ota_reqs, a.ident, b.ident,
                        a.remote_ack, b.remote_ack, a.log, b.log, a.restores, b.restores, a.bu_end, b.bu_end,
                        s.per_ms, s.every, s.ours_pct);
        }
        CHECK(a.tx_bad == 0 && b.tx_bad == 0, "%s round %d: TX malformed (%u/%u)", label, r, a.tx_bad, b.tx_bad);
        CHECK(a.ours_reached_ota_accepted == 0 && b.ours_reached_ota_accepted == 0,
              "%s round %d: a frame of ours taken by a platform handler (%u/%u)", label, r, a.ours_reached_ota_accepted,
              b.ours_reached_ota_accepted);
    }
    CHECK(diff == 0, "%s: %d of %d rounds differ", label, diff, rounds);
    fprintf(stderr, "%s %s: %d rounds; platform consumptions %llu with app_sysex / %llu without (identical traces in "
            "%d rounds); app_sysex took %llu frames; replies sent %llu, dropped (ring full or backup) %llu; "
            "platform busy-waits %llu/%llu; backup ends %llu/%llu\n",
            label, diff ? "FAIL" : "PASS", rounds, (unsigned long long)pc_with, (unsigned long long)pc_without,
            rounds - diff, (unsigned long long)took, (unsigned long long)sent, (unsigned long long)dropped,
            (unsigned long long)busy_with, (unsigned long long)busy_without, (unsigned long long)bu_with,
            (unsigned long long)bu_without);
    (void)is_ours_ev;
}

/* ======================================= T3c: TX-ring pressure: our big replies vs the platform's own sends === */
/* a host that floods our requests without waiting, at the full EP1 rate, with the platform's frames mixed in, while
 * reading the device slowly (or not at all). Run twice on the same traffic: with app_sysex in the dispatch and
 * without it (a firmware without CYMA-1's handler). The platform's blocking ota_wire_send (identity replies, remote
 * acks) must never busy-wait because of us: with a host that reads at any rate, its busy-wait slots with app_sysex
 * must not exceed those without, and it must serve and answer exactly as many frames.
 *   ours: 0 GET_EDIT / GET_SLOT (128 B replies, 43 packets), fresh SEQs; 1 the same request again and again (the
 *         duplicate cache's resend); 2 a mix of GET_DIR, HELLO and damaged frames (NAKs)
 *   kinds: 0 the verifier's mix (HS_QUERY, remote MASTER / encoder, log, screen-reader HELLO); 1 HS_QUERY only
 *   (harden-fw: verdicts added, drain 0 = a host that never reads, the variants above) */
typedef struct { uint32_t drain, every; int stop_and_wait, ours_pct, ours, kinds; } t3c_t;
static uint64_t t3c_total_busy[2], t3c_runs_bad;
static void t3c_ring_pressure(int rounds, t3c_t c)
{
    uint64_t busy[2] = {0, 0}, served[2] = {0, 0}, ident[2] = {0, 0}, acks[2] = {0, 0}, bad[2] = {0, 0}, dropped = 0,
             sent = 0, answered_saw = 0, answered_sent = 0;
    uint32_t maxrun[2] = {0, 0};
    int never = c.drain == 0, ok, strict;
    for (int r = 0; r < rounds; r++)
        for (int app = 1; app >= 0; app--) {
            uint32_t b0, bmax = 0, saw = 0, got = 0;
            rng = 0xC0FFEEull + (uint64_t)r + (uint64_t)c.ours * 104729u + (uint64_t)c.kinds * 1299709u + (uint64_t)(c.ours_pct != 90) * 15485863u;   /* (the verifier's seeds for its five) */
            reset_platform();
            with_app = app;
            tx_per_slot = c.drain, tx_every = c.every;
            for (int m = 0; m < 3000; m++) {
                fr_t f;
                if ((int)rndn(100) < c.ours_pct) {
                    uint8_t pl[4] = {(uint8_t)rndn(32), 0, 0, 0};
                    if (c.ours == 1) fr_ours(&f, 1, 2, 5, pl, 0);                              /* GET_EDIT, SEQ 5, again */
                    else if (c.ours == 2) {
                        uint32_t k = rndn(4);
                        if (k == 0) fr_ours(&f, 1, 7, (uint8_t)(m & 127), pl, 0);              /* GET_DIR */
                        else if (k == 1) { pl[0] = 1, pl[1] = 3; fr_ours(&f, 1, 1, (uint8_t)(m & 127), pl, 2); }   /* HELLO */
                        else if (k == 2) { fr_ours(&f, 1, 7, (uint8_t)(m & 127), pl, 0); f.b[8] ^= 0x21; }   /* a NAK */
                        else fr_ours(&f, 2, 7, (uint8_t)(m & 127), pl, 0);                     /* VER 2: a NAK */
                    } else if (rndn(2)) fr_ours(&f, 1, 2, (uint8_t)(m & 127), pl, 0);          /* GET_EDIT */
                    else fr_ours(&f, 1, 4, (uint8_t)(m & 127), pl, 1);                         /* GET_SLOT (used or not) */
                    saw++;
                } else if (c.kinds == 1) {
                    fr_platform(&f, K_HSQ);
                } else {
                    static const int K[] = {K_HSQ, K_REMOTE_MASTER, K_REMOTE_ENC, K_LOG, K_ANN_HELLO};
                    fr_platform(&f, K[rndn(5)]);
                }
                send_frame(&f);
            }
            if (c.stop_and_wait) {                      /* as our tools do: one request, then its reply (300 ms) */
                pq_w = pq_r = 0;
                for (int m = 0; m < 300; m++) {
                    fr_t f;
                    uint32_t g0 = tx.ours, t = 0;
                    uint8_t pl[2] = {(uint8_t)rndn(32), 0};
                    uint8_t cc = (uint8_t)(rndn(2) ? 2 : 4);
                    fr_ours(&f, 1, cc, (uint8_t)(m & 127), pl, cc == 4 ? 1u : 0u);
                    send_frame(&f);
                    while (tx.ours == g0 && t++ < (app ? 300u : 5u)) { b0 = hs_busy_waits; dev_ms(3, 1); if (hs_busy_waits - b0 > bmax) bmax = hs_busy_waits - b0; }
                    if (app) got += tx.ours != g0;
                    if (rndn(3) == 0) {                 /* another app's frame, at its own time */
                        static const int K[] = {K_HSQ, K_REMOTE_MASTER, K_LOG};
                        fr_platform(&f, K[rndn(3)]);
                        send_frame(&f);
                        dev_run(2 + rndn(5), 3, 1);
                    }
                }
                if (app) answered_sent += 300, answered_saw += got;
            }
            while (pq_r != pq_w) {
                b0 = hs_busy_waits;
                dev_ms(3, 1);
                if (hs_busy_waits - b0 > bmax) bmax = hs_busy_waits - b0;
            }
            dev_run(100, 3, 1);
            if (getenv("HS_DEBUG")) fprintf(stderr, "dbg app %d nev %u slots %u pq %u/%u tx frames %u ours %u ident %u app_calls %u\n", app, nev, hs_isr_slots, pq_r, pq_w, tx.frames, tx.ours, tx.ident, hs_app_calls);
            busy[app] += hs_busy_waits, ident[app] += tx.ident, acks[app] += tx.remote_ack, bad[app] += tx.bad;
            if (bmax > maxrun[app]) maxrun[app] = bmax;
            for (uint32_t i = 0; i < nev; i++) served[app] += !evs[i].ours && evs[i].cons != C_APP_DECLINED && evs[i].cons != C_APP_TOOK;
            if (app) dropped += hs_dropped_ours + (hs_held() - hs_held0), sent += hs_sent_ours;
            (void)saw;
        }
    tx_per_slot = 16, tx_every = 1;
    t3c_total_busy[1] += busy[1], t3c_total_busy[0] += busy[0];
    /* the claim, and so the verdict: when the host reads at least 2 packets a millisecond and the platform's own replies
     * alone never fill the ring (busy-waits without app_sysex 0), ours must not make it wait either. Slower hosts, and
     * floods the platform cannot keep up with by itself, are measured and printed (NOTE), not judged */
    strict = !never && c.drain * 2u >= 2u * c.every && busy[0] == 0;
    if (!strict) {
        ok = bad[0] == 0 && bad[1] == 0;
    } else {
        ok = bad[0] == 0 && bad[1] == 0 && busy[1] <= busy[0] && ident[1] == ident[0] && acks[1] == acks[0] &&
             served[1] == served[0];
        CHECK(busy[1] <= busy[0], "T3c drain %u/%u ours %d kinds %d: the platform busy-waited %llu ISR slots with app_sysex, %llu without",
              c.drain, c.every, c.ours, c.kinds, (unsigned long long)busy[1], (unsigned long long)busy[0]);
        CHECK(ident[1] == ident[0] && acks[1] == acks[0] && served[1] == served[0],
              "T3c drain %u/%u ours %d kinds %d: platform answers differ (identity %llu/%llu, acks %llu/%llu, served %llu/%llu)",
              c.drain, c.every, c.ours, c.kinds, (unsigned long long)ident[1], (unsigned long long)ident[0],
              (unsigned long long)acks[1], (unsigned long long)acks[0], (unsigned long long)served[1], (unsigned long long)served[0]);
    }
    CHECK(bad[0] == 0 && bad[1] == 0, "T3c: TX malformed (%llu/%llu)", (unsigned long long)bad[1], (unsigned long long)bad[0]);
    if (c.stop_and_wait)
        CHECK(answered_saw == answered_sent, "T3c: stop-and-wait requests answered %llu of %llu", (unsigned long long)answered_saw, (unsigned long long)answered_sent);
    if (!ok) t3c_runs_bad++;
    fprintf(stderr, "T3c (host reads %u packets every %u x 0.5 ms; %d%% ours: %s; platform: %s%s) %s: %d rounds of 3,000 frames at the full EP1 "
            "rate, a fast main loop: platform frames served %llu with app_sysex / %llu without; identity replies %llu/%llu; "
            "remote acks %llu/%llu; our replies sent %llu, dropped %llu; the platform's ota_wire_send busy-waited "
            "%llu/%llu ISR slots in all (longest within one millisecond: %u/%u slots of 0.5 ms); TX malformed %llu/%llu",
            c.drain, c.every, c.ours_pct, c.ours == 1 ? "one request repeated (the duplicate cache)" : c.ours == 2 ? "GET_DIR, HELLO, NAKs" : "GET_EDIT/GET_SLOT",
            c.kinds ? "HS_QUERY only" : "identity, remote, log, reader", c.stop_and_wait ? "; then 300 requests stop-and-wait as our tools send them" : "",
            !strict ? (ok ? "NOTE" : "FAIL") : ok ? "PASS" : "FAIL", rounds, (unsigned long long)served[1], (unsigned long long)served[0],
            (unsigned long long)ident[1], (unsigned long long)ident[0], (unsigned long long)acks[1], (unsigned long long)acks[0],
            (unsigned long long)sent, (unsigned long long)dropped, (unsigned long long)busy[1], (unsigned long long)busy[0], maxrun[1], maxrun[0],
            (unsigned long long)bad[1], (unsigned long long)bad[0]);
    if (c.stop_and_wait)
        fprintf(stderr, "; stop-and-wait requests answered %llu of %llu", (unsigned long long)answered_saw, (unsigned long long)answered_sent);
    if (!strict)
        fprintf(stderr, " [%s]", never ? "nobody reads at all" : busy[0] ? "the platform alone already waits" : "the host reads under 2 packets a millisecond");
    fprintf(stderr, "\n");
}

/* ============================== T3d: normal request / reply still works (a session as our tools run it) === */
/* stop-and-wait with the frozen host policy (§5.5: 300 ms, 1,000 ms for HELLO, 3 retries with the same SEQ): the
 * identity query, HELLO, GET_DIR, the 32 GET_SLOTs of a bank pull, GET_EDIT, PUT_EDIT, three PUT_SLOTs and a COMMIT
 * answered on the panel (SAVE: 0x48 OK), GET_DIR again (final), then the two cases where one request makes two frames
 * (HELLO and ABORT while the question is open: the 0x48 E_DECLINED and the request's own reply), at three host read
 * rates. Every request must be answered, with the right CMD, SEQ and status. */
static uint32_t sess_tries, sess_reqs, sess_fail, sess_retried, sess_wrong;
static uint8_t sess_seq;
static int sess_status(uint8_t *cmd, uint8_t *seq)     /* the last frame of ours the host got: its status byte */
{
    uint8_t d[300];
    int32_t m;
    if (tx_ours_last_n < 9) return -1;
    m = cy_unpack7(tx_ours_last + 7, tx_ours_last_n - 8, d, sizeof d);
    *cmd = tx_ours_last[5], *seq = tx_ours_last[6];
    return m >= 5 ? d[0] : -1;
}
static int sess_wait_ours(uint32_t g0, uint32_t ms)    /* a new frame of ours within ms */
{
    for (uint32_t t = 0; t < ms && tx.ours == g0; t++) dev_ms(2, 1);
    return tx.ours != g0;
}
/* one request, stop-and-wait; want_cmd: the reply's CMD (cmd | 0x40); returns its status, -1 no answer */
static int sess_req(uint8_t cmd, const uint8_t *pl, uint32_t n, int expect_extra_first)
{
    fr_t f;
    uint8_t rc, rs;
    int st = -1;
    sess_seq = (uint8_t)((sess_seq + 1) & 127);
    fr_ours(&f, 1, cmd, sess_seq, pl, n);
    sess_reqs++;
    for (int tr = 0; tr < 4; tr++) {
        uint32_t g0 = tx.ours;
        send_frame(&f);
        sess_tries++;
        if (tr) sess_retried++;
        while (sess_wait_ours(g0, cmd == 1 ? 1000u : 300u)) {   /* frames of ours: the reply (or a 0x48 first) */
            st = sess_status(&rc, &rs);
            if (rc == (cmd | 0x40) && rs == sess_seq && !(expect_extra_first && rc == 0x48)) return st;
            g0 = tx.ours;
        }
    }
    sess_fail++;
    return -1;
}
static void t3d_session(void)
{
    static const uint32_t RATES[3][2] = {{16, 1}, {4, 2}, {1, 2}};
    for (int app_fixed_only = 0; app_fixed_only < 1; app_fixed_only++)
    for (int ri = 0; ri < 3; ri++) {
        uint32_t i0, held0, ext_retries;
        uint8_t pl[120];
        int st, ok = 1;
        reset_platform();
        with_app = 1;
        tx_per_slot = RATES[ri][0], tx_every = RATES[ri][1];
        sess_tries = sess_reqs = sess_fail = sess_retried = sess_wrong = 0;
        tx_ours_last_n = 0;
        held0 = hs_held();
        dev_run(50, 2, 0);
        if (app.save_state) press(B_HOME, 50);
        {   /* the identity gate first */
            fr_t f;
            fr_platform(&f, K_HSQ);
            i0 = tx.ident;
            send_frame(&f);
            for (int t = 0; t < 300 && tx.ident == i0; t++) dev_ms(2, 1);
            if (tx.ident == i0) ok = 0, sess_fail++;
        }
        pl[0] = 1, pl[1] = 3;
        if ((st = sess_req(1, pl, 2, 0)) != 0) ok = 0, sess_wrong++;                 /* HELLO */
        if ((st = sess_req(7, pl, 0, 0)) != 0) ok = 0, sess_wrong++;                 /* GET_DIR */
        for (int s2 = 0; s2 < 32; s2++) { pl[0] = (uint8_t)s2; if (sess_req(4, pl, 1, 0) != 0) ok = 0, sess_wrong++; }   /* GET_SLOT x32 */
        if (sess_req(2, pl, 0, 0) != 0) ok = 0, sess_wrong++;                        /* GET_EDIT */
        pl[0] = 1; { uint32_t n = 1 + rec_make(pl + 1, 1); if (sess_req(3, pl, n, 0) != 0) ok = 0, sess_wrong++; }   /* PUT_EDIT */
        for (int k = 0; k < 3; k++) { pl[0] = (uint8_t)(3 + 4 * k); uint32_t n = 1 + rec_make(pl + 1, 1); if (sess_req(5, pl, n, 0) != 0) ok = 0, sess_wrong++; }
        pl[0] = 0;
        if (sess_req(8, pl, 1, 0) != 1) ok = 0, sess_wrong++;                        /* COMMIT: PENDING */
        {   /* SAVE on the panel: the 0x48 OK arrives unsolicited */
            uint32_t g0 = tx.ours;
            uint8_t rc, rs;
            phys_btn |= 1u << B_SAVE; dev_run(40, 2, 0); phys_btn = 0;
            for (int t = 0; t < 400 && !(tx.ours != g0 && sess_status(&rc, &rs) == 0 && rc == 0x48); t++) dev_ms(2, 1);
            if (!(tx.ours != g0 && sess_status(&rc, &rs) == 0 && rc == 0x48)) ok = 0, sess_wrong++;
        }
        if (sess_req(7, pl, 0, 0) != 0) ok = 0, sess_wrong++;                        /* GET_DIR: final */
        ext_retries = sess_retried;
        /* the two-frame cases: HELLO, then ABORT, while a question is open */
        for (int k = 0; k < 2; k++) {
            uint32_t n;
            pl[0] = 9; n = 1 + rec_make(pl + 1, 1);
            if (sess_req(5, pl, n, 0) != 0) ok = 0, sess_wrong++;
            pl[0] = 0;
            if (sess_req(8, pl, 1, 0) != 1) ok = 0, sess_wrong++;
            if (k == 0) { pl[0] = 1, pl[1] = 3; if (sess_req(1, pl, 2, 1) != 0) ok = 0, sess_wrong++; }
            else if (sess_req(9, pl, 0, 1) != 0) ok = 0, sess_wrong++;
        }
        if (sess_fail) ok = 0;
        CHECK(ok && sess_fail == 0, "T3d (host reads %u per %u slots): %u requests, %u failed, %u wrong", RATES[ri][0], RATES[ri][1], sess_reqs, sess_fail, sess_wrong);
        CHECK(ext_retries == 0, "T3d (host reads %u per %u slots): %u retries before the two-frame cases", RATES[ri][0], RATES[ri][1], ext_retries);
        fprintf(stderr, "T3d session (host reads %u packets every %u x 0.5 ms) %s: %u requests (identity, HELLO, GET_DIR, 32 GET_SLOT, "
                "GET_EDIT, PUT_EDIT, 3 PUT_SLOT, COMMIT + SAVE on the panel -> 0x48 OK, GET_DIR; then HELLO and ABORT "
                "during a question): all answered with the right CMD/SEQ/status %s; retries %u before the two-frame cases, "
                "%u in them (the 0x48 E_DECLINED goes first; the request's own reply follows on the host's retry with "
                "the same SEQ); replies held back by the gate %u\n",
                RATES[ri][0], RATES[ri][1], ok ? "PASS" : "FAIL", sess_reqs, ok ? "yes" : "NO", ext_retries,
                sess_retried - ext_retries, hs_held() - held0);
    }
    tx_per_slot = 16, tx_every = 1;
}

/* ============================================ T4: our frames never trigger keys, never pass ota_take === */
static void t4_ours_never_platform(long iters)
{
    long keys = 0, accepted = 0, published = 0;
    for (long i = 0; i < iters; i++) {
        fr_t f;
        uint32_t d;
        fr_ours_random(&f, 0);
        if (rndn(3) == 0) fr_mutate(&f);
        memset(&usb, 0, sizeof usb), usb.config = 1, sx_ready = 0, sx_collect = 0;
        for (uint32_t k = 0; k < f.n; k++) sysex_byte(f.b[k]);
        if (usb.ota_req || usb.uboot_req) keys++;
        if (sx_ready) {
            published++;
            d = ota_take();                         /* the update framing's decoder: a firmware without our handler */
            if (d) accepted++;
        }
        sx_ready = 0;
    }
    CHECK(keys == 0 && accepted == 0, "T4: keys %ld, ota_take accepted %ld", keys, accepted);
    fprintf(stderr, "T4 %s: %ld frames of ours (a third damaged) through usb.c sysex_byte and ota.c ota_take: "
            "%ld published, %ld update/UBOOT keys, %ld accepted by ota_take\n", keys || accepted ? "FAIL" : "PASS",
            iters, published, keys, accepted);
}

/* ======================================== T5: no OBJ_USER write without a SAVE press (and what does write) === */
static void t5_no_flash_without_save(long iters)
{
    uint32_t u0, w0, set0, commits = 0, asks = 0, puts_edit = 0;
    fr_t f;
    reset_platform();
    with_app = 1;
    u0 = app_snd_writes, w0 = store_writes;
    for (long i = 0; i < iters; i++) {
        uint32_t r = rndn(1000);
        if (r < 700) {
            fr_ours_random(&f, 0);
            if (f.b[5] == 8) commits++;
            if (f.b[5] == 3) puts_edit++;
            if (rndn(10) == 0) fr_mutate(&f);
            send_frame(&f);
            dev_run(1 + rndn(30), (int)rndn(3), 1);
        } else if (r < 760) {
            static const int NOT_SAVE[] = {B_HOME, B_ENV, B_FX, B_LFO, B_GLO, B_SEL, B_EDIT, B_ARP, B_SEQ, B_PLAY};
            press(NOT_SAVE[rndn(sizeof NOT_SAVE / sizeof NOT_SAVE[0])], 20);
        } else if (r < 770) {                       /* SEL + SAVE (revert / "no") */
            phys_btn |= 1u << B_SEL; dev_run(10, 2, 0);
            phys_btn |= 1u << B_SAVE; dev_run(40, 2, 0);
            phys_btn = 0; dev_run(20, 2, 0);
        } else if (r < 780) {
            fr_platform(&f, K_BACKUP), send_frame(&f), dev_run(50, 2, 0);
        } else if (r < 785) {
            hs_restore_mode = (int)rndn(2);
            fr_platform(&f, K_RESTORE), send_frame(&f), dev_run(20, 2, 0);
        } else if (r < 790) {
            fr_platform(&f, K_OTAKEY), send_frame(&f), dev_run(20, 2, 0);
        } else if (r < 800) {
            dev_run(31000 + rndn(35000), 0, 4);     /* past the question's 30 s, the staging's 60 s */
        } else {
            dev_run(1 + rndn(200), 1, 0);
        }
        if (app_sx.state == 2) asks++;
        CHECK(app_snd_writes == u0, "T5: OBJ_USER written with no SAVE press (iteration %ld)", i);
        if (app_snd_writes != u0) break;
    }
    set0 = store_writes - w0 - (app_snd_writes - u0);
    fprintf(stderr, "T5a %s: %ld steps (%u COMMITs, %u PUT_EDITs, %u passes with the question open; backups, restores, "
            "update keys, timeouts, every button but SAVE): OBJ_USER writes %u; other store writes (settings autosave) %u\n",
            app_snd_writes == u0 ? "PASS" : "FAIL", iters, commits, puts_edit, asks, app_snd_writes - u0, set0);

    /* b: a SAVE press while asking writes exactly once */
    {
        uint32_t ok = 0, tries = 0;
        for (int i = 0; i < 40; i++) {
            uint32_t w = app_snd_writes;
            reset_platform();
            dev_run(100, 2, 0);
            if (app.save_state) press(B_HOME, 50);
            our_state(2);
            if (app_sx.state != 2) continue;
            tries++;
            CHECK(app_snd_writes == w, "T5b: written before the press");
            press(B_SAVE, 300);
            if (app_snd_writes == w + 1 && app_sx.last_result == 0) ok++;
        }
        CHECK(ok == tries && tries > 30, "T5b: %u of %u", ok, tries);
        fprintf(stderr, "T5b %s: a SAVE press during the question wrote exactly once in %u of %u\n",
                ok == tries ? "PASS" : "FAIL", ok, tries);
    }
    /* c: remote control's SAVE (7D 4C 43 01, fm1-base's own frames) answers the question too: known, outside our tools */
    {
        uint32_t w;
        reset_platform();
        our_state(2);
        w = app_snd_writes;
        if (app_sx.state == 2) {
            uint8_t on[] = {0xF0, 0x7D, 0x4C, 0x43, 1, (uint8_t)((1u << B_SAVE) & 0x7F), (uint8_t)(((1u << B_SAVE) >> 7) & 0x7F), 0xF7};
            uint8_t off[] = {0xF0, 0x7D, 0x4C, 0x43, 1, 0, 0, 0xF7};
            fr_raw(&f, on, sizeof on), send_frame(&f), dev_run(60, 2, 0);
            fr_raw(&f, off, sizeof off), send_frame(&f), dev_run(300, 2, 0);
        }
        fprintf(stderr, "T5c NOTE: fm1-base's remote-control SAVE (F0 7D 4C 43 01 ..) during the question: OBJ_USER writes "
                "+%u (no hand on the panel; the platform's remote control, not our frames; our tools' allowlist cannot send it)\n",
                app_snd_writes - w);
    }
    /* d: PUT_EDIT alone, then quiet: does anything reach the flash? (with a control run that sends nothing) */
    for (int with_put = 0; with_put < 2; with_put++) {
        uint32_t w1, s1, h0[OBJ_NOBJ], h1[OBJ_NOBJ], wrote = 0;
        uint8_t pl[120];
        reset_platform();
        dev_run(70000, 0, 4);                       /* settle: any pending autosave done */
        our_state(0);                               /* HELLO only (no state change) */
        dev_run(70000, 0, 4);
        w1 = store_writes, s1 = app_snd_writes;
        for (int o = 0; o < OBJ_NOBJ; o++) h0[o] = store_len[o] > 0 ? fnv(store[o], (uint32_t)store_len[o]) : 0;
        if (with_put) {
            pl[0] = 1;
            uint32_t n = 1 + rec_make(pl + 1, 1);
            fr_ours(&f, 1, 3, seqn = (uint8_t)((seqn + 1) & 127), pl, n);
            send_frame(&f);
        }
        dev_run(5 * 60 * 1000, 0, 4);
        for (int o = 0; o < OBJ_NOBJ; o++) { h1[o] = store_len[o] > 0 ? fnv(store[o], (uint32_t)store_len[o]) : 0; if (h1[o] != h0[o]) wrote |= 1u << o; }
        fprintf(stderr, "T5d NOTE (%s): then 5 min untouched: store writes +%u (OBJ_USER +%u); objects whose bytes changed: mask 0x%x "
                "(OBJ_SETTINGS = %d, OBJ_USER = %d)\n", with_put ? "one PUT_EDIT, the audition the spec calls RAM only" : "control: nothing sent",
                store_writes - w1, app_snd_writes - s1, wrote, OBJ_SETTINGS, OBJ_USER);
    }
}

/* ======================================================= T6: raw fuzz of app_sysex (ASan/UBSan build) === */
static void t6_raw_fuzz(long iters)
{
    static uint8_t b[1024];
    long took = 0;
    uint32_t u0 = app_snd_writes;
    reset_platform();
    with_app = 1;
    for (long i = 0; i < iters; i++) {
        uint32_t n = rndn(5) == 0 ? rndn(8) : rndn(700);
        for (uint32_t k = 0; k < n; k++) b[k] = (uint8_t)(rndn(4) ? rnd() & 0x7F : rnd());
        if (n >= 3 && rndn(3)) b[0] = 0x7D, b[1] = 0x43, b[2] = 0x59;
        if (n >= 4 && rndn(2)) b[3] = 1;
        uint8_t *heap = malloc(n ? n : 1);        /* exact-size heap copy: ASan sees any read past n */
        memcpy(heap, b, n);
        took += app_sysex(heap, n);
        free(heap);
        if ((i & 1023) == 0) run_ms(1 + rndn(40));
    }
    CHECK(app_snd_writes == u0, "T6: OBJ_USER writes from raw fuzz");
    fprintf(stderr, "T6 %s: %ld raw buffers (0..700 B, 8-bit bytes too) straight into app_sysex: %ld claimed, no "
            "sanitizer report, OBJ_USER writes %u\n", app_snd_writes == u0 ? "PASS" : "FAIL", iters, took, app_snd_writes - u0);
}

/* ============================== T8: the page's sound operations with hostile bytes (a decoded code or file) === */
/* everything a share code or a file can put in front of the firmware arrives as a cy_preset_t (94 B) or a whole
 * cy_user_bank_t (3,020 B) through web.c's box: random bytes there (any name bytes, unterminated; any u16 values; any
 * used mask), through app_snd_audition / put / bank / save, then the engine renders: no sanitizer report, every sample
 * finite, every name in the bank still cleaned */
static void t8_snd_ops_fuzz(long iters)
{
    static int32_t blk[2 * APP_BLOCK];
    long nonfinite = 0, badname = 0, ops = 0;
    reset_platform();
    for (long i = 0; i < iters; i++) {
        cy_preset_t p;
        uint8_t *b = (uint8_t *)&p;
        for (uint32_t k = 0; k < sizeof p; k++) b[k] = (uint8_t)rnd();
        switch (rndn(6)) {
        case 0: case 1: app_snd_audition(&p, (int)rndn(4), (int)rndn(2)); break;
        case 2: app_snd_put((int)rndn(40) - 4, rndn(5) ? &p : 0); break;
        case 3: { char nm[20]; for (int k = 0; k < 20; k++) nm[k] = (char)rnd(); app_snd_save((int)rndn(40) - 4, nm); break; }
        case 4: { static cy_user_bank_t bk; uint8_t *q = (uint8_t *)&bk; for (uint32_t k = 0; k < sizeof bk; k++) q[k] = (uint8_t)rnd();
                  if (rndn(4)) bk.magic = CY_USER_MAGIC, bk.version = APP_BANK_VERSION; app_snd_bank(&bk); break; }
        default: app_snd_load((int)rndn(70) - 5, (int)rndn(2)); break;
        }
        ops++;
        if ((i & 15) == 0) {
            run_ms(1 + rndn(20));
            app_render(blk, APP_BLOCK, 4096);
            for (int k = 0; k < 2 * APP_BLOCK; k++) if (blk[k] > (1 << 24) || blk[k] < -(1 << 24)) nonfinite++;
            for (int s2 = 0; s2 < CY_NUSER; s2++) if (app_bank.used_mask >> s2 & 1u)
                for (int c = 0; c < CY_PRESET_NAME; c++) { char ch = app_bank.p[s2].name[c];
                    if (ch && !((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || strchr(" -.'&+/!?#():", ch))) badname++; }
        }
    }
    CHECK(nonfinite == 0 && badname == 0, "T8: out-of-range samples %ld, uncleaned name bytes %ld", nonfinite, badname);
    fprintf(stderr, "T8 %s: %ld page operations with random preset / bank bytes (audition, put, save, bank, load), the engine "
            "rendering between: %ld samples out of range, %ld uncleaned name bytes in the bank\n",
            nonfinite || badname ? "FAIL" : "PASS", ops, nonfinite, badname);
}

/* ================================ T9: GET_DIR's bank CRC (cached by harden-fw) is always the bank's own === */
/* every way the bank changes (the page's put, bank and save; SAVE on the panel; a USB staging answered yes or no on
 * the panel; a restore over USB; a reboot; a store that reads back as another format), in random order, each
 * followed by GET_DIR: the reply's bank_crc must equal an independent CRC-32 of the 3,020 B bank in RAM */
static int t9_get_dir_crc(uint32_t *out)
{
    fr_t f;
    uint8_t d[64];
    int32_t m;
    fr_ours(&f, 1, 7, seqn = (uint8_t)((seqn + 1) & 127), 0, 0);
    app_sysex(f.b + 1, f.n - 2);
    tx_drain(1000);
    if (app_sx.tx_n < 9 || app_sx_tx[5] != 0x47) return -1;
    m = cy_unpack7(app_sx_tx + 7, app_sx.tx_n - 8u, d, sizeof d);
    if (m != 24) return -1;
    *out = d[13] | (uint32_t)d[14] << 8 | (uint32_t)d[15] << 16 | (uint32_t)d[16] << 24;
    return 0;
}
static void t9_crc_cache(long iters)
{
    long checked = 0, wrong = 0, hits = 0, ops[10] = {0};
    fr_t f;
    uint8_t pl[120];
    reset_platform();
    with_app = 1;
    hs_restore_mode = 1;
    for (long i = 0; i < iters; i++) {
        uint32_t got, want, k = rndn(10);
        ops[k]++;
        switch (k) {
        case 0: { cy_preset_t p; uint8_t *b = (uint8_t *)&p; for (uint32_t j = 0; j < sizeof p; j++) b[j] = (uint8_t)rnd();
                  app_snd_put((int)rndn(32), rndn(4) ? &p : 0); break; }
        case 1: { static cy_user_bank_t bk; uint8_t *q = (uint8_t *)&bk; for (uint32_t j = 0; j < sizeof bk; j++) q[j] = (uint8_t)rnd();
                  bk.magic = CY_USER_MAGIC, bk.version = APP_BANK_VERSION; app_snd_bank(&bk); break; }
        case 2: { char nm[15] = "T9 SOUND"; nm[8] = (char)('A' + rndn(26)); app_snd_save((int)rndn(32), nm); break; }
        case 3: app_set_param((int)rndn(P_NPRESET), (float)rndn(1000) / 1000.0f); press(B_SAVE, 30); press(B_SAVE, 300); break;   /* panel SAVE */
        case 4: case 5: {                       /* USB: stage 1-3 slots, COMMIT, then SAVE (yes) or HOME (no) on the panel */
            int nput = 1 + (int)rndn(3);
            for (int j = 0; j < nput; j++) {
                pl[0] = (uint8_t)rndn(32);
                uint32_t n = 1 + rec_make(pl + 1, 1);
                if (rndn(4) == 0) { fr_ours(&f, 1, 6, seqn = (uint8_t)((seqn + 1) & 127), pl, 1); }
                else fr_ours(&f, 1, 5, seqn = (uint8_t)((seqn + 1) & 127), pl, n);
                app_sysex(f.b + 1, f.n - 2); tx_drain(1000);
            }
            pl[0] = 0;
            fr_ours(&f, 1, 8, seqn = (uint8_t)((seqn + 1) & 127), pl, 1);
            app_sysex(f.b + 1, f.n - 2); tx_drain(1000);
            if (app_sx.state == 2) press(k == 4 ? B_SAVE : B_HOME, 300);
            break; }
        case 6: fr_platform(&f, K_RESTORE); send_frame(&f); dev_run(30, 2, 0); break;   /* restore: app_backup_restored */
        case 7: boot(); dev_run(20, 2, 0); break;                                         /* reboot: app_init */
        case 8: if (rndn(8) == 0) {             /* the store reads back as another format: the bank locked, zeroed */
                    store[OBJ_USER][0] ^= 0x5A; boot(); dev_run(20, 2, 0); store[OBJ_USER][0] ^= 0x5A; boot(); dev_run(20, 2, 0);
                } break;
        default: dev_run(1 + rndn(50), 2, 0); break;                                      /* nothing: the cache hits */
        }
#if HS_FIXED
        hits += app.bank_crc_ok != 0;
#endif
        if (t9_get_dir_crc(&got) != 0) { wrong++; continue; }
        want = crc32_ref(0, (const uint8_t *)&app_bank, sizeof app_bank);
        checked++;
        if (got != want) { wrong++; if (wrong < 5) fprintf(stderr, "  T9 iteration %ld after op %u: GET_DIR bank_crc %08X, the bank's %08X\n", i, k, got, want); }
    }
    hs_restore_mode = 0;
#if HS_FIXED
    CHECK(app_sx.crc_stale == 0, "T9: the host's own check saw %u stale CRCs", app_sx.crc_stale);
#endif
    CHECK(wrong == 0 && checked == iters, "T9: %ld of %ld GET_DIR CRCs wrong", wrong, checked);
    fprintf(stderr, "T9 %s: %ld GET_DIRs, each after one of: page put %ld, page bank %ld, page save %ld, panel SAVE %ld, "
            "USB commit answered yes %ld / no %ld, USB restore %ld, reboot %ld, store unreadable %ld, nothing %ld: "
            "bank_crc equal to an independent CRC-32 of the bank in %ld, wrong in %ld; served from the cache %ld\n",
            wrong == 0 && checked == iters ? "PASS" : "FAIL", iters, ops[0], ops[1], ops[2], ops[3], ops[4], ops[5], ops[6],
            ops[7], ops[8], ops[9], checked - wrong, wrong, hits);
}

/* ============================================================== T7: cost of app_sysex per frame === */
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void t7_cost(void)
{
    fr_t f;
    const char *names[] = {"HELLO", "GET_EDIT", "PUT_EDIT", "GET_SLOT", "PUT_SLOT", "ERASE", "GET_DIR", "COMMIT", "ABORT"};
    reset_platform();
    hs_sink_quiet = 1;                         /* harden-fw: app_sysex's own cost (its printed reply dropped unparsed) */
#if HS_FIXED
    app_sx_crc_verify = 0;                     /* (the device's path: no host-only check) */
#endif
    for (int c = 1; c <= 9; c++) {
        uint8_t pl[120];
        uint32_t n = 0;
        if (c == 1) pl[0] = 1, pl[1] = 3, n = 2;
        if (c == 3) pl[0] = 1, n = 1 + rec_make(pl + 1, 1);
        if (c == 4 || c == 6) pl[0] = 3, n = 1;
        if (c == 5) pl[0] = 3, n = 1 + rec_make(pl + 1, 1);
        if (c == 8) pl[0] = 0, n = 1;
        double t0 = now_s();
        int reps = 20000;
        for (int r = 0; r < reps; r++) {
            fr_ours(&f, 1, (uint8_t)c, (uint8_t)(r & 127), pl, n);
            app_sysex(f.b + 1, f.n - 2);
            if (c == 8) app_sx_abort(0x40);
        }
        double t1 = now_s();
        double t2 = now_s();
        for (int r = 0; r < reps; r++) fr_ours(&f, 1, (uint8_t)c, (uint8_t)(r & 127), pl, n);
        double t3 = now_s();
        fprintf(stderr, "T7 cost: %-8s %.2f us per frame on this Mac (frame building excluded)\n", names[c - 1],
                ((t1 - t0) - (t3 - t2)) / reps * 1e6);
    }
    {   /* harden-fw: GET_DIR in detail */
        int reps = 20000;
        double t0, t1, tb;
        for (int v = 0; v < (HS_FIXED ? 3 : 1); v++) {
#if HS_FIXED
            app_sx_crc_verify = v == 2;        /* 0, 1: the device's path (no check); 2: with the host-only check */
#endif
            t0 = now_s();
            for (int r = 0; r < reps; r++) {
#if HS_FIXED
                if (v == 1) app.bank_crc_ok = 0;   /* a miss every time: the bank changed before each GET_DIR */
#endif
                fr_ours(&f, 1, 7, (uint8_t)(r & 127), 0, 0);
                app_sysex(f.b + 1, f.n - 2);
            }
            t1 = now_s();
            tb = now_s();
            for (int r = 0; r < reps; r++) fr_ours(&f, 1, 7, (uint8_t)(r & 127), 0, 0);
            tb = now_s() - tb;
            fprintf(stderr, "T7 cost: GET_DIR %s %.2f us per frame on this Mac\n",
                    !HS_FIXED ? "(pre-fix: the CRC of the bank every time)" : v == 0 ? "(the cached CRC: the device's path)"
                    : v == 1 ? "(the CRC computed: the first after a bank change)" : "(cached, with the host-only check)",
                    ((t1 - t0) - tb) / reps * 1e6);
        }
#if HS_FIXED
        app_sx_crc_verify = 1;
#endif
        t0 = now_s();
        volatile uint32_t sink = 0;
        for (int r = 0; r < reps; r++) sink += cy_crc32(0, (const uint8_t *)&app_bank, sizeof app_bank);
        fprintf(stderr, "T7 cost: cy_crc32 over the 3,020 B bank alone %.2f us on this Mac\n", (now_s() - t0) / reps * 1e6);
        (void)sink;
    }
    hs_sink_quiet = 0;
    {   /* the baseline: ota_take on a frame of ours (what a firmware without the hook does with it) */
        fr_ours(&f, 1, 7, 1, 0, 0);
        double t0 = now_s();
        for (int r = 0; r < 200000; r++) { memcpy(sx_frame, f.b + 1, f.n - 2); sx_frame_len = f.n - 2; sx_ready = 1; ota_take(); }
        fprintf(stderr, "T7 cost: ota_take on the same frame %.3f us\n", (now_s() - t0) / 200000 * 1e6);
    }
}

int main(int argc, char **argv)
{
    long t4n = 1000000, t5n = 20000, t6n = 2000000;
    int t2n = 1500, t3n = 40;
    const char *only = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--quick")) t4n = 100000, t5n = 3000, t6n = 200000, t2n = 200, t3n = 6;
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) hs_log = fopen(argv[++i], "w");
    }
    pq = calloc(PQ, sizeof *pq);
    evs = calloc(NEV, sizeof *evs);
    plat_host_dir = ".";
    midi_log = fopen("/dev/null", "w");
    said_print = 0;
    boot();
    run_ms(2000);
    reset_platform();
#define RUN(name, call) if (!only || strstr(only, name)) { call; }
    RUN("T1", t1_routing_matrix());
    {
        sched_t s = {0};
        RUN("T2", t2_differential(t2n, "T2 differential (random traffic, random main-loop pace)", s, 1));
        sched_t fl = {.nmsg = 6000, .per_ms = 0, .every = 8, .ours_pct = 97, .damage_pct = 30, .note_pct = 5};
        RUN("T3", t2_differential(t3n, "T3 flood (6,000 messages, 97% ours, a dispatch every 8 ms)", fl, 0));
        sched_t fl2 = {.nmsg = 6000, .per_ms = 3, .every = 1, .ours_pct = 97, .damage_pct = 0, .note_pct = 0};
        RUN("T3", t2_differential(t3n, "T3 flood (6,000 messages, 97% valid ours, a fast main loop)", fl2, 0));
    }
    /* the verifier's five, then harden-fw's harsher ones */
    RUN("T3c", t3c_ring_pressure(t3n, (t3c_t){16, 1, 0, 90, 0, 0}));
    RUN("T3c", t3c_ring_pressure(t3n, (t3c_t){16, 2, 0, 90, 0, 0}));
    RUN("T3c", t3c_ring_pressure(t3n, (t3c_t){8, 2, 0, 90, 0, 0}));
    RUN("T3c", t3c_ring_pressure(t3n, (t3c_t){4, 2, 0, 90, 0, 0}));
    RUN("T3c", t3c_ring_pressure(t3n, (t3c_t){4, 2, 1, 90, 0, 0}));
    RUN("T3c", t3c_ring_pressure(t3n, (t3c_t){2, 2, 0, 90, 0, 0}));
    RUN("T3c", t3c_ring_pressure(t3n, (t3c_t){3, 2, 0, 90, 0, 0}));
    RUN("T3c", t3c_ring_pressure(t3n, (t3c_t){16, 1, 0, 50, 0, 1}));
    RUN("T3c", t3c_ring_pressure(t3n, (t3c_t){16, 2, 0, 50, 0, 1}));
    RUN("T3c", t3c_ring_pressure(t3n, (t3c_t){8, 2, 0, 50, 0, 1}));
    RUN("T3c", t3c_ring_pressure(t3n, (t3c_t){4, 2, 0, 90, 1, 0}));
    RUN("T3c", t3c_ring_pressure(t3n, (t3c_t){4, 2, 0, 90, 2, 0}));
    RUN("T3c", t3c_ring_pressure(t3n, (t3c_t){2, 2, 0, 90, 2, 0}));
    RUN("T3c", t3c_ring_pressure(t3n, (t3c_t){1, 2, 0, 90, 0, 0}));
    RUN("T3c", t3c_ring_pressure(t3n, (t3c_t){1, 4, 0, 90, 0, 0}));
    RUN("T3c", t3c_ring_pressure(t3n, (t3c_t){1, 8, 0, 90, 0, 0}));
    RUN("T3c", t3c_ring_pressure(t3n, (t3c_t){4, 2, 0, 50, 0, 1}));
    RUN("T3c", t3c_ring_pressure(t3n, (t3c_t){1, 4, 0, 50, 0, 1}));
    RUN("T3c", t3c_ring_pressure(t3n, (t3c_t){1, 4, 0, 90, 1, 0}));
    RUN("T3c", t3c_ring_pressure(t3n, (t3c_t){1, 4, 0, 90, 2, 0}));
    RUN("T3c", t3c_ring_pressure(t3n, (t3c_t){1, 4, 1, 90, 0, 1}));
    RUN("T3c", t3c_ring_pressure(t3n, (t3c_t){0, 1, 0, 90, 0, 0}));
    RUN("T3c", t3c_ring_pressure(t3n, (t3c_t){0, 1, 0, 50, 0, 1}));
    RUN("T3d", t3d_session());
    RUN("T9", t9_crc_cache(t5n));
    RUN("T4", t4_ours_never_platform(t4n));
    RUN("T5", t5_no_flash_without_save(t5n));
    RUN("T6", t6_raw_fuzz(t6n));
    RUN("T8", t8_snd_ops_fuzz(t5n * 10));
    RUN("T7", t7_cost());
    fprintf(stderr, "harness: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
