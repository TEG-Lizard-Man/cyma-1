/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 Sounds: save, restore, share (docs/CYMA-1-sounds-spec.md §5, §6.3, §7). A unity part: app.c includes it at
 * its very end and it is never compiled on its own, so app.c's statics (the values, the bank, the SAVE path) are in
 * scope. Statics are prefixed app_snd_ (the shared accessors) and app_sx_ (the SysEx handler).
 *
 * Two doors to one sound model:
 *   the page (web.c, PLAT_HOST: the host simulator and the wasm): synchronous writes the user clicked for, through
 *     app_snd_save / put / bank, and the audition, the current sound and the loads;
 *   USB SysEx from a computer (app_sysex, fm1-base's APP_SYSEX hook; also compiled into the wasm for the loopback
 *     tests and the virtual FM-1): PUT_EDIT only plays a sound (RAM, as turning PRESETS does; SEL+SAVE reverts it);
 *     slot changes are staged in RAM, and COMMIT asks on the FM-1's screen. Nothing from USB reaches the user bank
 *     (OBJ_USER) without a SAVE press on the device (an audition is an edit, which the settings autosave stores as it
 *     does a knob turn), and then through SAVE's own confirmed write (ctl_save_service: the fade around the ~50 ms
 *     flash stall, held while a USB backup streams).
 *
 * Safety (§5.6). app_sysex claims a frame only when it starts 7D 43 59 ("CY" under the non-commercial ID): the
 * updater's frames are pack7(00 59 ...) and start 00, fm1-base's own 7D channels have other second and third bytes,
 * and the two 6-byte keys never become frames. It never writes the flash, never blocks, never crowds the platform's own
 * replies out of the USB send ring (app_sx_send: one frame of ours in the ring at a time, and only into a nearly idle
 * ring), and decodes every frame strictly (cy_unpack7, the CRC) before acting on it. Frames with CMD >= 0x40 (replies:
 * ours echoed back, a loop) are claimed and ignored, never answered. */

/* ------------------------------------------------------------- protocol --- */
#define APP_SX_VER 0x01u
#define APP_SX_PROTO 1u
#define APP_SX_MAX_PAYLOAD 256u
enum {
    APP_SX_HELLO = 0x01, APP_SX_GET_EDIT, APP_SX_PUT_EDIT, APP_SX_GET_SLOT, APP_SX_PUT_SLOT, APP_SX_ERASE_SLOT,
    APP_SX_GET_DIR, APP_SX_COMMIT, APP_SX_ABORT, APP_SX_NAK = 0x7F
};
enum {
    APP_SX_OK = 0x00, APP_SX_PENDING = 0x01, APP_SX_E_FRAME = 0x10, APP_SX_E_VERSION = 0x11, APP_SX_E_CMD = 0x12,
    APP_SX_E_ARG = 0x13, APP_SX_E_PARAM_SET = 0x14, APP_SX_E_BUSY = 0x31, APP_SX_E_HOLD = 0x32,
    APP_SX_E_NOTHING = 0x33, APP_SX_E_ASKING = 0x34, APP_SX_E_LOCKED = 0x35, APP_SX_E_DECLINED = 0x40,
    APP_SX_E_TIMEOUT = 0x41, APP_SX_E_FLASH = 0x42
};
/* the states (§5.5): IDLE, STAGED (slot changes held in RAM), ASKING (the question on the screen), and WRITING (said
 * yes, SAVE's write queued: reported to a host as 2, so it waits for the result) */
enum { APP_SX_IDLE, APP_SX_STAGED, APP_SX_ASKING, APP_SX_WRITING };
#define APP_SX_ASK_MS 30000u                   /* the question closes itself (E_TIMEOUT) */
#define APP_SX_STAGE_MS 60000u                 /* staging with no frame from the host this long: dropped */
/* The USB send ring (the device). fm1-base's bsp/chip/usb.c queues SysEx for the host in a ring of SXQ USB-MIDI event
 * packets (sx_out_q: so_w moved on by the main loop's ota_wire_send, so_r by the USB interrupt as the host reads).
 * unity.c makes SXQ 128 and #includes usb.c ahead of app.c, so the names are in scope here, and unity.c's own
 * ann_wire_send reads them exactly so (SXQ - (so_w - so_r) free): a documented coupling to fm1-base internals, read
 * only, never written (spec §14 R6). The platform's own replies go through the blocking ota_wire_send, which busy-waits
 * up to 200 ms for room with the main loop stalled: the identity reply and the update handshake (ota_send_msg, up to
 * 50 B = 17 packets), the remote-control acks (2), the log readout (16 a line). So a frame of ours goes only while the
 * ring is nearly idle (APP_SX_QUEUED_MAX packets waiting at most) and none of ours is still in it; otherwise it is
 * dropped, the host retries with the same SEQ, and the duplicate cache answers. A build-test harness may supply the
 * three names itself */
#if !defined(PLAT_HOST) && !defined(APP_SX_RING_N)
#define APP_SX_RING_N SXQ
#define APP_SX_RING_W so_w
#define APP_SX_RING_R so_r
#endif
#define APP_SX_QUEUED_MAX 21u                  /* so our largest reply (GET_EDIT, GET_SLOT: 128 B, 43 packets) never takes
                                                * the ring past 64: half of it stays the platform's (three of its largest
                                                * frames and a margin), and a smaller reply leaves more */
#ifdef APP_SX_RING_N                           /* (that arithmetic holds for fm1-base's 128-packet ring: a smaller one
                                                * stops the build rather than leave the platform less room) */
typedef char app_sx_assert_ring[APP_SX_RING_N >= APP_SX_QUEUED_MAX + 43u + 64u ? 1 : -1];
#endif

static struct {
    uint8_t state, last_cmd, last_seq, commit_seq, last_result, last_commit_seq;
    uint16_t tx_n;                             /* the cached reply's length (0: none) */
    uint32_t staged, erased, last_crc, heard_t, ask_t;
    uint32_t changed;                          /* WRITING: the slots the yes changed (the follow, the words) */
    uint8_t puts, erases;                      /* the question's counts, kept for the words after the write */
    char q[24];                                /* the question's first line, for the footer */
#ifdef APP_SX_RING_N
    uint16_t ring_end;                         /* the send ring's write index after our last frame, mod 2^16 (in the
                                                * struct's tail padding: app_sx stays 60 B) */
#endif
#ifdef PLAT_HOST
    int16_t status;                            /* the host's sx_status: the last reply's status byte, -1 none */
    uint8_t cmd;                               /* sx_cmd */
    int8_t taken;                              /* sx_taken: the last host "sysex" frame's app_sysex, -1 none */
    uint32_t replies;                          /* sx_replies */
    uint32_t held;                             /* frames app_sx_send kept back for the platform's room (a harness) */
    uint32_t crc_stale;                        /* sx_crc_stale: GET_DIRs whose cached CRC was not the bank's */
#endif
} app_sx;
static cy_user_bank_t app_sx_stage PLAT_POOL;  /* 3,020 B: PUT_SLOT's records until COMMIT (zeroed by app_sx_init) */
static uint8_t app_sx_rx[264], app_sx_tx[312]; /* the decoded request (payload + CRC); the last reply (the dup cache) */
#ifdef PLAT_HOST
static uint32_t app_snd_writes;                /* OBJ_USER writes since boot (the host's user_writes) */
static int app_snd_wrc;                        /* the last one's result */
static int app_sx_crc_verify = 1;              /* GET_DIR checks its cached CRC against the bank (a harness's timing
                                                * turns it off: the device has no check) */
#endif
#ifdef PLAT_WEB
static uint8_t app_sx_out[2048];               /* whole F0..F7 reply frames, back to back (web.c drains them) */
static uint32_t app_sx_out_n;
#endif

/* --------------------------------------------------------------- helpers --- */
static uint32_t app_sx_rd32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void app_sx_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}
static uint32_t app_snd_nlen(const char *s)    /* strnlen(s, 14): a preset's name has no terminator at 14 */
{
    uint32_t n = 0;
    while (n < CY_PRESET_NAME && s[n])
        n++;
    return n;
}
static void app_snd_zero(void *d, uint32_t n)
{
    uint8_t *p = (uint8_t *)d;
    while (n--)
        *p++ = 0;
}

/* the one OBJ_USER write (SAVE's, a USB commit's, the page's): counted for the host's user_writes */
static int app_snd_store(void)
{
    int rc = plat_store_save(OBJ_USER, &app_bank, sizeof app_bank);
#ifdef PLAT_HOST
    app_snd_writes++;
    app_snd_wrc = rc;
#endif
    return rc;
}

/* the bank in the format this build writes (as SAVE makes it): a bank that was never written starts empty */
static void app_snd_bank_ready(void)
{
    app.bank_crc_ok = 0;                       /* (every caller goes on to change the bank: GET_DIR's CRC again) */
    if (app_bank.magic != CY_USER_MAGIC) {
        app_snd_zero(&app_bank, sizeof app_bank);
        app_bank.magic = CY_USER_MAGIC;
    }
    app_bank.version = APP_BANK_VERSION;
}

/* ===================================================== the shared accessors (§6.3) === */
/* the one way to end gestures before an external change: a morph-scrub ends, a browse still settling lands, the SAVE
 * dialog closes */
static void app_snd_quiet(void)
{
    ctl_morph_end();
    if (app.browsing)
        ctl_browse_land(0);
    ctl_save_cancel();
}

/* the sound playing, as SAVE would store it: the targets (a glide lands where it is going; COMPARE's edits) and the
 * name; returns the loaded preset (0..23 factory, 24..55 user) */
static int app_snd_cur(cy_preset_t *out)
{
    cy_preset_store(app_target, app.name, out);
    return app.preset;
}

/* a sound plays now, in RAM, as edits of the preset that is loaded (stored values and app.preset unchanged): SAVE
 * lights, COMPARE plays what was there, SEL+SAVE reverts. flags b0 glides 300 ms (the enums at once), b1 makes them
 * plain edits (the name kept, not marked auditioned: the page's undo of an arrival). -3 while COMPARE is held or a
 * USB question is open */
static int app_snd_audition(const cy_preset_t *p, int flags, int from_usb)
{
    float v[P_NPARAMS];
    char nm[CY_PRESET_NAME];
    int i;
    if (app.compare || app_sx_asking())        /* (USB's PUT_EDIT answers E_ASKING before it gets here) */
        return -3;
    app_snd_quiet();
    for (i = 0; i < P_NPARAMS; i++)            /* (the globals ride along: params.c gets whole arrays) */
        v[i] = app_norm[i];
    cy_preset_load(p, v);
    app_targets_give(v, flags & 1);
    if (!(flags & 2)) {
        cy_name_clean((const uint8_t *)p->name, app_snd_nlen(p->name), nm);
        for (i = 0; i < CY_PRESET_NAME; i++)
            app.name[i] = nm[i];
        app.name[CY_PRESET_NAME] = 0;
        app.auditioned = 1;
    }
    if (from_usb)
        app_say("FROM USB", app.name);
    return 0;
}

/* a user slot changed under the sound playing (the page's put or bank, a USB commit): if that slot is loaded and not
 * edited, it is reloaded (gliding); if it is edited, its stored values (and, unless the sound is an audition, its
 * name) follow and the edits stay. A deleted loaded slot keeps playing: SAVE can make it again */
static void app_snd_follow(int slot)
{
    int idx = CY_NFACTORY + slot, i;
    char nm[16];
    if (slot < 0 || slot >= CY_NUSER || app.preset != idx || app.morphing || !app_user_used(slot))
        return;
    if (!app_is_edited() && !app.compare) {
        app_preset_load(idx, 1);
        return;
    }
    app_preset_values(idx, app_stored, app.auditioned ? nm : app.name);
    if (app.compare)                           /* (COMPARE plays the stored sound: the new one) */
        for (i = 0; i < P_NPRESET; i++)
            app_norm[i] = app_from[i] = app_stored[i];
}

#ifdef PLAT_HOST
/* the state word (§7.2): 0-7 the loaded preset, 8 edited, 9 COMPARE, 10 browsing, 11 the SAVE dialog, 12 gliding,
 * 13 auditioned, 14 bank locked, 15 a USB question open, 16 a SAVE write pending */
static uint32_t app_snd_state(void)
{
    return (uint32_t)(app.preset & 0xFFu) | (uint32_t)(app_is_edited() != 0) << 8 | (uint32_t)(app.compare != 0) << 9 |
           (uint32_t)(app.browsing != 0) << 10 | (uint32_t)(app.save_state != 0) << 11 |
           (uint32_t)(app.gliding != 0) << 12 | (uint32_t)(app.auditioned != 0) << 13 |
           (uint32_t)(app.bank_locked != 0) << 14 | (uint32_t)(app_sx_asking() != 0) << 15 |
           (uint32_t)(app.save_pending != 0) << 16;
}

/* a confirmed SAVE still waiting for its fade is written now (as app_backup_flush does), before the bank moves on */
static void app_snd_flush(void)
{
    if (app.save_pending) {
        app.save_pending = 0;
        ctl_save_write(app.save_pend_slot, plat_ms());
        app_mute = 0;
    }
}

/* load preset idx (0..55) as PRESETS does, gliding when asked: another preset lands as a browse lands (the edits of
 * the one it leaves are kept, and that preset's own kept edits come back); the loaded preset again is its stored
 * sound (the page's Revert). -1 out of range or an empty slot */
static int app_snd_load(int idx, int glide)
{
    if (idx < 0 || idx >= APP_NPRE || (idx >= CY_NFACTORY && !app_user_used(idx - CY_NFACTORY)))
        return -1;
    app_snd_quiet();
    if (app.compare)
        ctl_compare(0);
    if (idx == app.preset) {
        app_preset_load(idx, glide != 0);
    } else {
        app.browse = (uint16_t)idx;
        app.browsing = 1;
        ctl_browse_land(glide != 0);
    }
    return 0;
}

/* the sound playing saved into user slot 0..31 under name (up to 14 bytes, cleaned): SAVE's own write half
 * (ctl_save_to: SAVED Uxx, the slot loaded, the fade around the write). 0, -1 bad slot, -2 the store failed (kept in
 * RAM, playable), -3 COMPARE held or a USB question open, -4 the bank is another format's */
static int app_snd_save(int slot, const char *name)
{
    char nm[CY_PRESET_NAME];
    uint32_t w;
    int i;
    if (slot < 0 || slot >= CY_NUSER || !name)
        return -1;
    if (app.bank_locked)
        return -4;
    if (app.compare || app_sx_asking() || app.save_hold)
        return -3;
    app_snd_flush();
    app_snd_quiet();
    cy_name_clean((const uint8_t *)name, app_snd_nlen(name), nm);
    for (i = 0; i < CY_PRESET_NAME; i++)
        app.name[i] = nm[i];
    app.name[CY_PRESET_NAME] = 0;
    w = app_snd_writes;
    ctl_save_to(slot, plat_ms());
    return app_snd_writes != w && app_snd_wrc != 0 ? -2 : 0;
}

/* user slot 0..31 becomes p (a rename, an undone delete, a factory copy), or empty (p == 0); one write, then the
 * sound playing follows. 0, -1, -2, -3 (a USB question open), -4 */
static int app_snd_put(int slot, const cy_preset_t *p)
{
    cy_preset_t n;
    int i, same;
    if (slot < 0 || slot >= CY_NUSER)
        return -1;
    if (app.bank_locked)
        return -4;
    if (app_sx_asking() || app.save_hold)
        return -3;
    app_snd_flush();
    app_snd_bank_ready();
    if (p) {
        n = *p;
        cy_name_clean((const uint8_t *)p->name, app_snd_nlen(p->name), n.name);
    } else {
        app_snd_zero(&n, sizeof n);
    }
    same = p && app_user_used(slot);           /* (a rename keeps the slot's sound: its kept edits still apply) */
    for (i = 0; same && i < P_NPRESET; i++)
        same = app_bank.p[slot].v[i] == n.v[i];
    if (!same && app.mem_ok && app.mem_preset == CY_NFACTORY + slot)
        app.mem_ok = 0;
    app_bank.p[slot] = n;
    if (p)
        app_bank.used_mask |= 1u << slot;
    else
        app_bank.used_mask &= ~(1u << slot);
    i = app_snd_store();
    app_snd_follow(slot);
    return i ? -2 : 0;
}

/* all 32 slots from b (an import, Replace, an undo): magic CMA1 and version 1 (else -1); the unused slots zeroed and
 * every name cleaned; one write, then the sound playing follows */
static int app_snd_bank(const cy_user_bank_t *b)
{
    int s, rc;
    if (!b || b->magic != CY_USER_MAGIC || b->version != APP_BANK_VERSION)
        return -1;
    if (app.bank_locked)
        return -4;
    if (app_sx_asking() || app.save_hold)
        return -3;
    app_snd_flush();
    if (app.mem_ok && app.mem_preset >= CY_NFACTORY) {   /* (kept edits of a slot whose sound changed: dropped) */
        int k = app.mem_preset - CY_NFACTORY, i, same = app_user_used(k) && (b->used_mask >> k & 1u);
        for (i = 0; same && i < P_NPRESET; i++)
            same = app_bank.p[k].v[i] == b->p[k].v[i];
        if (!same)
            app.mem_ok = 0;
    }
    app_bank = *b;
    app.bank_crc_ok = 0;
    for (s = 0; s < CY_NUSER; s++) {
        if (app_bank.used_mask >> s & 1u) {
            char nm[CY_PRESET_NAME];
            int i;
            cy_name_clean((const uint8_t *)app_bank.p[s].name, app_snd_nlen(app_bank.p[s].name), nm);
            for (i = 0; i < CY_PRESET_NAME; i++)
                app_bank.p[s].name[i] = nm[i];
        } else {
            app_snd_zero(&app_bank.p[s], sizeof app_bank.p[s]);
        }
    }
    rc = app_snd_store();
    if (app.preset >= CY_NFACTORY)
        app_snd_follow(app.preset - CY_NFACTORY);
    return rc ? -2 : 0;
}
#endif

/* ========================================================== the SysEx handler (§5) === */
static void app_sx_send(const uint8_t *f, uint32_t n)
{
#ifdef APP_SX_RING_N
    uint32_t r = APP_SX_RING_R, w = APP_SX_RING_W;   /* (r can only move on meanwhile: w - r never under-counts) */
    if (w - r > APP_SX_QUEUED_MAX ||           /* the ring not nearly idle (the host reads slowly, or not at all) */
        (uint16_t)(app_sx.ring_end + ~r) < w - r) {   /* (end - r - 1) ours still waits in it: one at a time */
#ifdef PLAT_HOST
        app_sx.held++;
#endif
        return;                                /* dropped: the host retries with the same SEQ, the cache answers */
    }
#endif
#ifndef PLAT_HOST
    (void)ann_wire_send(f, n);                 /* never waits: USB down, a backup streaming: dropped as well */
#elif defined(PLAT_WEB)
    uint32_t i;
    if (app_sx_out_n + n <= sizeof app_sx_out) {
        for (i = 0; i < n; i++)
            app_sx_out[app_sx_out_n + i] = f[i];
        app_sx_out_n += n;
    }
#else
    uint32_t i;
    printf("sysex_out ");
    for (i = 0; i < n; i++)
        printf("%02X", f[i]);
    printf("\n");
#endif
#ifdef APP_SX_RING_N
    app_sx.ring_end = (uint16_t)APP_SX_RING_W;   /* (not sent after all: then whatever waits goes first, too) */
#endif
#ifdef PLAT_HOST
    if (n >= 10u) {                            /* (what the host's expects read: the reply's CMD and status byte,
                                                * the payload's first: pack7's first 7 bits, the next byte's low bit) */
        app_sx.cmd = f[5];
        app_sx.status = (int16_t)(f[7] | (f[8] & 1u) << 7);
        app_sx.replies++;
    }
#endif
}

/* F0 7D 43 59 VER CMD SEQ pack7(payload ‖ CRC32_LE(VER CMD SEQ payload)) F7 into f (312 B is the largest); pl needs
 * 4 bytes of room after its n for the CRC */
static uint32_t app_sx_build(uint8_t *f, uint8_t cmd, uint8_t seq, uint8_t *pl, uint32_t n)
{
    uint32_t crc;
    f[0] = 0xF0u, f[1] = 0x7Du, f[2] = 0x43u, f[3] = 0x59u, f[4] = APP_SX_VER, f[5] = cmd, f[6] = seq;
    crc = cy_crc32(cy_crc32(0, f + 4, 3), pl, n);
    app_sx_wr32(pl + n, crc);
    n = 7u + cy_pack7(pl, n + 4u, f + 7);
    f[n++] = 0xF7u;
    return n;
}

/* a reply to the request in hand: kept as the duplicate cache, then sent */
static void app_sx_reply(uint8_t cmd, uint8_t seq, uint8_t *pl, uint32_t n)
{
    app_sx.tx_n = (uint16_t)app_sx_build(app_sx_tx, (uint8_t)(cmd | 0x40u), seq, pl, n);
    app_sx_send(app_sx_tx, app_sx.tx_n);
}
static void app_sx_status(uint8_t cmd, uint8_t seq, uint8_t st)
{
    uint8_t r[8];
    r[0] = st;
    app_sx_reply(cmd, seq, r, 1);
}

/* a frame that cannot be read (packing, length, CRC) or another version: answered, never cached */
static void app_sx_nak(uint8_t st, uint8_t cmd, uint8_t seq)
{
    uint8_t r[8], f[24];
    r[0] = st, r[1] = cmd, r[2] = APP_SX_PROTO;
    app_sx_send(f, app_sx_build(f, APP_SX_NAK, seq & 0x7Fu, r, 3));
}

static void app_sx_drop(void)                  /* the staging gone (a write already queued finishes) */
{
    app_sx.staged = app_sx.erased = 0;
    if (app_sx.state != APP_SX_WRITING)
        app_sx.state = APP_SX_IDLE;
}

static void app_sx_unsay(void)                 /* the question off the footer, if it is still there */
{
    uint32_t i;
    for (i = 0; app_sx.q[i] && app.msg[0][i] == app_sx.q[i]; i++)
        ;
    if (!app_sx.q[i] && !app.msg[0][i])
        app.msg_until = plat_ms();
}

/* a COMMIT's final result: recorded for GET_DIR, and sent unsolicited as CMD 0x48 with the COMMIT's SEQ. If the
 * duplicate cache still holds that COMMIT (its PENDING), the result replaces it: a retried COMMIT gets the result,
 * never a second question */
static void app_sx_final(uint8_t st)
{
    uint8_t r[12], f[32];
    app_sx.last_result = st;
    app_sx.last_commit_seq = app_sx.commit_seq;
    r[0] = st;
    app_sx_wr32(r + 1, app_bank.used_mask);
    if (app_sx.tx_n && app_sx.last_cmd == APP_SX_COMMIT && app_sx.last_seq == app_sx.commit_seq) {
        app_sx.tx_n = (uint16_t)app_sx_build(app_sx_tx, APP_SX_COMMIT | 0x40u, app_sx.commit_seq, r, 5);
        app_sx_send(app_sx_tx, app_sx.tx_n);
    } else {
        app_sx_send(f, app_sx_build(f, APP_SX_COMMIT | 0x40u, app_sx.commit_seq, r, 5));
    }
}

/* the question closed without a write: no, a timeout, or HELLO / ABORT from the host meanwhile */
static void app_sx_decline(uint8_t st)
{
    app_sx_final(st);
    app_sx.state = APP_SX_IDLE;
    app_sx_drop();
    app_say("USB", "NOTHING CHANGED");
}

/* ---- the hooks app.c calls (§6.4) */
static void app_sx_init(void)
{
    app_snd_zero(&app_sx, sizeof app_sx);
    app_snd_zero(&app_sx_stage, sizeof app_sx_stage);   /* (the pool is not cleared at boot) */
    app_sx.last_commit_seq = 0xFFu;            /* (no SEQ: a host never mistakes a first COMMIT for a finished one) */
#ifdef PLAT_HOST
    app_sx.status = -1;
    app_sx.taken = -1;
    app_snd_writes = 0;
    app_snd_wrc = 0;
#endif
}

static int app_sx_asking(void) { return app_sx.state == APP_SX_ASKING; }

/* the player's answer (ctl_buttons, ctl_encoders): yes puts the staging into the bank in RAM and queues SAVE's own
 * write (faded if anything sounds, held while a backup streams); app_sx_written reports it */
static void app_sx_answer(int yes)
{
    uint32_t s;
    if (app_sx.state != APP_SX_ASKING)
        return;
    if (!yes) {
        app_sx_decline(APP_SX_E_DECLINED);
        return;
    }
    app_snd_bank_ready();
    for (s = 0; s < CY_NUSER; s++) {
        uint32_t bit = 1u << s;
        if (app_sx.staged & bit) {
            app_bank.p[s] = app_sx_stage.p[s];
            app_bank.used_mask |= bit;
        } else if (app_sx.erased & bit) {
            app_snd_zero(&app_bank.p[s], sizeof app_bank.p[s]);
            app_bank.used_mask &= ~bit;
        }
    }
    app_sx.changed = app_sx.staged | app_sx.erased;
    if (app.mem_ok && app.mem_preset >= CY_NFACTORY && (app_sx.changed >> (app.mem_preset - CY_NFACTORY) & 1u))
        app.mem_ok = 0;                        /* (the edits it kept were of a sound now replaced: as SAVE does) */
    app_sx.staged = app_sx.erased = 0;
    app_sx.state = APP_SX_WRITING;
    app.save_pending = 1;
    app.save_pend_slot = APP_SAVE_USB;
    ctl_save_service(plat_ms());
}

/* SAVE's write of a USB commit is done (ctl_save_write, slot APP_SAVE_USB): the words, the host told (0x48: OK, or
 * E_FLASH with the new bank still in RAM and playable until power-off, as SAVE keeps it), the sound playing follows */
static void app_sx_written(int rc)
{
    char t[24];
    uint32_t ch = app_sx.changed;
    int s;
    if (rc) {
        app_say("SAVE FAILED", "USB");
    } else if (app_sx.puts + app_sx.erases == 1u) {
        for (s = 0; s < CY_NUSER - 1 && !(ch >> s & 1u); s++)
            ;
        app_preset_num(t, CY_NFACTORY + s, sizeof t);
        app_say(app_sx.puts ? "SAVED" : "DELETED", t);
    } else {
        t[0] = 0;
        app_cat_int(t, app_sx.erases ? app_popcount(ch) : app_sx.puts, sizeof t);
        app_cat(t, app_sx.erases ? " SLOTS" : " SOUNDS", sizeof t);
        app_say("SAVED", t);
    }
    if (app_sx.state == APP_SX_WRITING) {
        app_sx_final(rc ? APP_SX_E_FLASH : APP_SX_OK);
        app_sx.state = APP_SX_IDLE;
    }
    app_sx.changed = 0;
    if (app.preset >= CY_NFACTORY && (ch >> (app.preset - CY_NFACTORY) & 1u))
        app_snd_follow(app.preset - CY_NFACTORY);
}

/* the staging dropped, nothing sent (app_panic: an update or a restore starts; app_backup_restored): an open
 * question counts as `result` (declined) for GET_DIR. A yes whose write is still queued finishes */
static void app_sx_abort(int result)
{
    if (app_sx.state == APP_SX_ASKING || (app_sx.state == APP_SX_WRITING && !app.save_pending)) {
        app_sx.last_result = (uint8_t)result;
        app_sx.last_commit_seq = app_sx.commit_seq;
        if (app_sx.state == APP_SX_ASKING)
            app_sx_unsay();
        app_sx.state = APP_SX_IDLE;
    }
    app_sx_drop();
}

/* every frame (app_frame): the question stays on the footer (not said again) and closes itself after 30 s; staging
 * the host left alone for 60 s is dropped */
static void app_sx_tick(uint32_t now)
{
    if (app_sx.state == APP_SX_ASKING) {
        uint32_t i;
        if (now - app_sx.ask_t >= APP_SX_ASK_MS) {
            app_sx_decline(APP_SX_E_TIMEOUT);
            return;
        }
        for (i = 0; app_sx.q[i] && app.msg[0][i] == app_sx.q[i]; i++)
            ;
        if (!app_sx.q[i] && !app.msg[0][i]) {  /* (ours: kept up) */
            app.msg_until = now + 200u;
        } else if ((int32_t)(now - app.msg_until) >= 0) {   /* (another message had its time: ours again, quietly) */
            app_cpy(app.msg[0], app_sx.q, sizeof app.msg[0]);
            app_cpy(app.msg[1], "SAVE=YES HOME=NO", sizeof app.msg[1]);
            app.msg_until = now + 200u;
        }
    } else if (app_sx.state == APP_SX_STAGED && now - app_sx.heard_t >= APP_SX_STAGE_MS) {
        app_sx_drop();
    }
}

/* ---- the commands (§5.3) */
static uint8_t app_sx_state_byte(void)         /* what a host is told: WRITING is still the question's (2) */
{
    return app_sx.state == APP_SX_WRITING ? (uint8_t)APP_SX_ASKING : app_sx.state;
}

static void app_sx_hello(uint8_t seq)
{
    uint8_t r[96];
    uint32_t n = 0, i;
    static const char V[] = APP_VERSION, A[] = APP_NAME;
    if (app_sx.state == APP_SX_ASKING)         /* (HELLO resets: an open question counts as declined) */
        app_sx_decline(APP_SX_E_DECLINED);
    app_sx_drop();
    r[n++] = APP_SX_OK;
    r[n++] = APP_SX_PROTO;
    r[n++] = (uint8_t)CY_XFER_PARAM_SET;
    r[n++] = (uint8_t)P_NPRESET;
    r[n++] = (uint8_t)CY_NUSER;
    r[n++] = (uint8_t)CY_PRESET_NAME;
    r[n++] = (uint8_t)(APP_SX_MAX_PAYLOAD & 0xFFu);
    r[n++] = (uint8_t)(APP_SX_MAX_PAYLOAD >> 8);
#ifdef PLAT_HOST
    r[n++] = (uint8_t)(0x01u | 0x02u | (app.bank_locked ? 0x04u : 0u));   /* b1: an emulator (host or wasm) */
#else
    r[n++] = (uint8_t)(0x01u | (app.bank_locked ? 0x04u : 0u));           /* b0: writes need the device's SAVE */
#endif
    r[n++] = app_sx_state_byte();
    r[n++] = (uint8_t)(sizeof A - 1u);
    for (i = 0; i + 1u < sizeof A; i++)
        r[n++] = (uint8_t)A[i];
    {
        uint32_t vl = sizeof V - 1u > 48u ? 48u : (uint32_t)(sizeof V - 1u);
        r[n++] = (uint8_t)vl;
        for (i = 0; i < vl; i++)
            r[n++] = (uint8_t)V[i];
    }
    app_sx_reply(APP_SX_HELLO, seq, r, n);
}

/* the commands that stage: E_LOCKED for another format's bank, E_ASKING during the question, E_BUSY while a yes is
 * being written */
static uint8_t app_sx_stage_refusal(void)
{
    if (app.bank_locked)
        return APP_SX_E_LOCKED;
    if (app_sx.state == APP_SX_ASKING)
        return APP_SX_E_ASKING;
    if (app_sx.state == APP_SX_WRITING)
        return APP_SX_E_BUSY;
    return APP_SX_OK;
}
static void app_sx_staged_reply(uint8_t cmd, uint8_t seq, uint8_t slot)
{
    uint8_t r[16];
    r[0] = APP_SX_OK;
    r[1] = slot;
    app_sx_wr32(r + 2, app_sx.staged);
    app_sx_wr32(r + 6, app_sx.erased);
    app_sx_reply(cmd, seq, r, 10);
}

static void app_sx_commit(uint8_t seq, uint32_t now)
{
    uint32_t all = app_sx.staged | app_sx.erased, s;
    uint8_t st = APP_SX_OK;
    char t[8];
    if (app_sx.state == APP_SX_ASKING)
        st = APP_SX_E_ASKING;
    else if (app.bank_locked)
        st = APP_SX_E_LOCKED;
    else if (app.save_hold)
        st = APP_SX_E_HOLD;
    else if (app_sx.state == APP_SX_WRITING || app.save_state || app.save_pending || app.compare || app.morphing ||
             app.browsing)
        st = APP_SX_E_BUSY;
    else if (!all)
        st = APP_SX_E_NOTHING;
    if (st != APP_SX_OK) {
        app_sx_status(APP_SX_COMMIT, seq, st);
        return;
    }
    app_sx.puts = (uint8_t)app_popcount(app_sx.staged);
    app_sx.erases = (uint8_t)app_popcount(app_sx.erased);
    for (s = 0; s < CY_NUSER - 1u && !(all >> s & 1u); s++)
        ;
    app_preset_num(t, CY_NFACTORY + (int)s, sizeof t);
    app_cpy(app_sx.q, "USB: ", sizeof app_sx.q);
    if (app_sx.puts == 1u && !app_sx.erases) {         /* USB: SAVE TO U07? / USB: REPLACE U07? */
        app_cat(app_sx.q, app_user_used((int)s) ? "REPLACE " : "SAVE TO ", sizeof app_sx.q);
        app_cat(app_sx.q, t, sizeof app_sx.q);
    } else if (!app_sx.puts && app_sx.erases == 1u) {  /* USB: DELETE U07? */
        app_cat(app_sx.q, "DELETE ", sizeof app_sx.q);
        app_cat(app_sx.q, t, sizeof app_sx.q);
    } else if (!app_sx.erases) {                       /* USB: SAVE 3 SOUNDS? */
        app_cat(app_sx.q, "SAVE ", sizeof app_sx.q);
        app_cat_int(app_sx.q, app_sx.puts, sizeof app_sx.q);
        app_cat(app_sx.q, " SOUNDS", sizeof app_sx.q);
    } else {                                           /* USB: CHANGE 12 SLOTS? */
        app_cat(app_sx.q, "CHANGE ", sizeof app_sx.q);
        app_cat_int(app_sx.q, app_popcount(all), sizeof app_sx.q);
        app_cat(app_sx.q, " SLOTS", sizeof app_sx.q);
    }
    app_cat(app_sx.q, "?", sizeof app_sx.q);
    app_sx.state = APP_SX_ASKING;
    app_sx.ask_t = now;
    app_sx.commit_seq = seq;
    app_sx.last_commit_seq = 0xFFu;            /* (not final until the answer: GET_DIR polls see it so) */
    app_say(app_sx.q, "SAVE=YES HOME=NO");
    app_sx_status(APP_SX_COMMIT, seq, APP_SX_PENDING);
}

/* one request, decoded and checked (pl: its payload, n bytes); every path answers exactly once */
static void app_sx_do(uint8_t cmd, uint8_t seq, const uint8_t *pl, uint32_t n, uint32_t now)
{
    uint8_t r[8 + CY_XFER_REC_V1 + 8];
    cy_preset_t p;
    int rc;
    switch (cmd) {
    case APP_SX_HELLO:                         /* host_proto, host_kind: informational */
        if (n != 2u)
            break;
        app_sx_hello(seq);
        return;
    case APP_SX_GET_EDIT:
        if (n != 0u)
            break;
        r[0] = APP_SX_OK;
        r[1] = (uint8_t)app_snd_cur(&p);
        r[2] = (uint8_t)((app_is_edited() ? 1u : 0u) | (app.auditioned ? 2u : 0u) | (app.compare ? 4u : 0u));
        app_sx_reply(cmd, seq, r, 3u + cy_rec_put(&p, r + 3));
        return;
    case APP_SX_PUT_EDIT:                      /* flags (b0 glide, b1 plain edits), record: RAM only */
        if (app_sx.state == APP_SX_ASKING) {
            app_sx_status(cmd, seq, APP_SX_E_ASKING);
            return;
        }
        if (n < 1u)
            break;
        rc = cy_rec_get(pl + 1, n - 1u, &p);
        if (rc != CY_REC_OK) {
            app_sx_status(cmd, seq, rc == CY_REC_E_NEWER ? APP_SX_E_PARAM_SET : APP_SX_E_ARG);
            return;
        }
        app_sx_status(cmd, seq, app_snd_audition(&p, pl[0] & 3, 1) ? APP_SX_E_BUSY : APP_SX_OK);
        return;
    case APP_SX_GET_SLOT:                      /* slot: the committed bank */
        if (n != 1u || pl[0] >= CY_NUSER)
            break;
        if (app.bank_locked) {
            app_sx_status(cmd, seq, APP_SX_E_LOCKED);
            return;
        }
        r[0] = APP_SX_OK;
        r[1] = pl[0];
        r[2] = (uint8_t)app_user_used(pl[0]);
        app_sx_reply(cmd, seq, r, 3u + (r[2] ? cy_rec_put(&app_bank.p[pl[0]], r + 3) : 0u));
        return;
    case APP_SX_PUT_SLOT:                      /* slot, record: staged */
        if (n < 1u || pl[0] >= CY_NUSER)
            break;
        if ((r[0] = app_sx_stage_refusal()) != APP_SX_OK) {
            app_sx_status(cmd, seq, r[0]);
            return;
        }
        rc = cy_rec_get(pl + 1, n - 1u, &p);
        if (rc != CY_REC_OK) {
            app_sx_status(cmd, seq, rc == CY_REC_E_NEWER ? APP_SX_E_PARAM_SET : APP_SX_E_ARG);
            return;
        }
        app_sx_stage.p[pl[0]] = p;
        app_sx.staged |= 1u << pl[0];
        app_sx.erased &= ~(1u << pl[0]);
        app_sx.state = APP_SX_STAGED;
        app_sx_staged_reply(cmd, seq, pl[0]);
        return;
    case APP_SX_ERASE_SLOT:                    /* slot: staged */
        if (n != 1u || pl[0] >= CY_NUSER)
            break;
        if ((r[0] = app_sx_stage_refusal()) != APP_SX_OK) {
            app_sx_status(cmd, seq, r[0]);
            return;
        }
        app_sx.erased |= 1u << pl[0];
        app_sx.staged &= ~(1u << pl[0]);
        app_sx.state = APP_SX_STAGED;
        app_sx_staged_reply(cmd, seq, pl[0]);
        return;
    case APP_SX_GET_DIR:
        if (n != 0u)
            break;
        r[0] = APP_SX_OK;
        app_sx_wr32(r + 1, app_bank.used_mask);
        app_sx_wr32(r + 5, app_sx.staged);
        app_sx_wr32(r + 9, app_sx.erased);
        if (!app.bank_crc_ok) {                /* the 3,020 B bank's CRC (some 33,000 instructions on the device, the
                                                * USB receive slot held meanwhile), kept until the bank changes */
            app.bank_crc = cy_crc32(0, (const uint8_t *)&app_bank, sizeof app_bank);
            app.bank_crc_ok = 1;
        }
#ifdef PLAT_HOST
        else if (app_sx_crc_verify && app.bank_crc != cy_crc32(0, (const uint8_t *)&app_bank, sizeof app_bank)) {
            app_sx.crc_stale++;                /* (a change to app_bank that did not clear app.bank_crc_ok) */
            printf("FAIL GET_DIR: the cached bank CRC is stale\n");
        }
#endif
        app_sx_wr32(r + 13, app.bank_crc);
        r[17] = app_sx_state_byte();
        r[18] = app_sx.last_result;
        r[19] = app_sx.last_commit_seq;
        app_sx_reply(cmd, seq, r, 20);
        return;
    case APP_SX_COMMIT:                        /* flags (reserved, 0) */
        if (n != 1u)
            break;
        app_sx_commit(seq, now);
        return;
    case APP_SX_ABORT:
        if (n != 0u)
            break;
        if (app_sx.state == APP_SX_ASKING)
            app_sx_decline(APP_SX_E_DECLINED);
        app_sx_drop();
        app_sx_status(cmd, seq, APP_SX_OK);
        return;
    default:                                   /* 0x00, 0x0A..0x3F */
        if (cmd == 0x3Fu) {                    /* its reply lands on 0x7F, the NAK: so it carries a NAK's whole
                                                * payload (status, offending CMD, proto), which still starts E_CMD */
            r[0] = APP_SX_E_CMD, r[1] = cmd, r[2] = APP_SX_PROTO;
            app_sx_reply(cmd, seq, r, 3);
            return;
        }
        app_sx_status(cmd, seq, APP_SX_E_CMD);
        return;
    }
    app_sx_status(cmd, seq, APP_SX_E_ARG);     /* (a fixed-size command of the wrong size, a slot past U32) */
}

/* the bytes after 7D 43 59: VER CMD SEQ, then the packed payload and CRC. Decoded into app_sx_rx at once (the USB
 * layer's buffer is released when app_sysex returns) */
static void app_sx_frame(const uint8_t *f, uint32_t n)
{
    uint32_t now = plat_ms(), crc;
    uint8_t cmd, seq;
    int32_t d;
    if (n < 3u)                                /* (no header: nothing to answer) */
        return;
    cmd = f[1];
    seq = f[2];
    if (cmd >= 0x40u)                          /* a reply (ours echoed back, a loop): claimed, never answered */
        return;
    if (f[0] != APP_SX_VER) {
        app_sx_nak(APP_SX_E_VERSION, cmd, seq);
        return;
    }
    d = cy_unpack7(f + 3, n - 3u, app_sx_rx, sizeof app_sx_rx - 4u);
    if (seq >= 0x80u || d < 4 || d > (int32_t)(APP_SX_MAX_PAYLOAD + 4u)) {
        app_sx_nak(APP_SX_E_FRAME, cmd, seq);
        return;
    }
    crc = cy_crc32(cy_crc32(0, f, 3), app_sx_rx, (uint32_t)d - 4u);
    if (crc != app_sx_rd32(app_sx_rx + d - 4)) {
        app_sx_nak(APP_SX_E_FRAME, cmd, seq);
        return;
    }
    app_sx.heard_t = now;
    if (cmd != APP_SX_HELLO && app_sx.tx_n && cmd == app_sx.last_cmd && seq == app_sx.last_seq &&
        crc == app_sx.last_crc) {              /* a retry: the cached reply again, not executed twice (HELLO always
                                                * runs: it is the reset, and idempotent) */
        app_sx_send(app_sx_tx, app_sx.tx_n);
        return;
    }
    app_sx.last_cmd = cmd;
    app_sx.last_seq = seq;
    app_sx.last_crc = crc;
    app_sx_do(cmd, seq, app_sx_rx, (uint32_t)d - 4u, now);
}

/* APP_SYSEX (app.h): a USB SysEx frame the platform's own handlers did not take (p: the bytes between F0 and F7),
 * from the main loop; 1 = ours. Strict prefix: everything else (the updater's 00 59 frames above all) goes on to
 * ota_service untouched */
int app_sysex(const uint8_t *p, uint32_t n)
{
    if (n < 3u || p[0] != 0x7Du || p[1] != 0x43u || p[2] != 0x59u)
        return 0;                              /* not ours: the updater's 00 59 frames go on to ota_service */
    app_sx_frame(p + 3, n - 3u);               /* copies and decodes into app_sx_rx at once, acts, replies */
    return 1;
}

/* ===================================================== the host simulator (§6.7) === */
#ifdef PLAT_HOST
static int app_sx_expect(const char *what)
{
    if (!strcmp(what, "sx_state")) return app_sx_state_byte();
    if (!strcmp(what, "sx_status")) return app_sx.status;
    if (!strcmp(what, "sx_cmd")) return app_sx.cmd;
    if (!strcmp(what, "sx_replies")) return (int)app_sx.replies;
    if (!strcmp(what, "sx_result")) return app_sx.last_result;
    if (!strcmp(what, "sx_staged")) return (int)(app_sx.staged | app_sx.erased);
    if (!strcmp(what, "sx_taken")) return app_sx.taken;
    if (!strcmp(what, "sx_crc_stale")) return (int)app_sx.crc_stale;
    if (!strcmp(what, "user_writes")) return (int)app_snd_writes;
    return 0;
}

/* one frame as hex (F0 .. F7; whitespace ignored) to app_sysex, as the USB layer hands it over */
static void app_sx_host_frame(const char *h, const char *where)
{
    static uint8_t b[1024];
    uint32_t n = 0;
    int hi = -1;
    for (; *h; h++) {
        int v = *h >= '0' && *h <= '9' ? *h - '0' : *h >= 'a' && *h <= 'f' ? *h - 'a' + 10
              : *h >= 'A' && *h <= 'F' ? *h - 'A' + 10 : -1;
        if (v < 0) {
            if (*h == ' ' || *h == '\t' || *h == '\r' || *h == '\n')
                continue;
            printf("FAIL %s: not hex at '%c'\n", where, *h);
            exit(2);
        }
        if (hi < 0) {
            hi = v;
        } else {
            if (n >= sizeof b) {
                printf("FAIL %s: over %u bytes\n", where, (unsigned)sizeof b);
                exit(2);
            }
            b[n++] = (uint8_t)(hi << 4 | v);
            hi = -1;
        }
    }
    if (hi >= 0 || n < 2u || b[0] != 0xF0u || b[n - 1u] != 0xF7u) {
        printf("FAIL %s: want one whole frame, F0 .. F7, as hex\n", where);
        exit(2);
    }
    app_sx.taken = (int8_t)app_sysex(b + 1, n - 2u);
    if (!app_sx.taken)
        printf("  sysex: not ours (%u B): the platform's\n", (unsigned)n);
}

/* a command's PATH: the rest of its script line (a path may hold spaces: the project's does), "out:NAME" being NAME
 * in the run's directory */
static void app_sx_path(char *out, uint32_t max, const char *cmd)
{
    const char *a = plat_host_line;
    uint32_t n;
    while (*a == ' ' || *a == '\t')
        a++;
    a += strlen(cmd);
    while (*a == ' ' || *a == '\t')
        a++;
    if (!strncmp(a, "out:", 4))
        snprintf(out, max, "%s/%s", plat_host_dir, a + 4);
    else
        snprintf(out, max, "%s", a);
    for (n = (uint32_t)strlen(out); n && (out[n - 1u] == '\n' || out[n - 1u] == '\r' || out[n - 1u] == ' '); n--)
        out[n - 1u] = 0;
}

static void app_sx_host_cmd(const char *cmd, const char *a)
{
    (void)a;                                   /* (the line's own words: a path may hold spaces) */
    char path[600];
    if (!strcmp(cmd, "sysex")) {               /* sysex HEX...: the rest of the line (its words concatenated) */
        const char *h = plat_host_line;
        while (*h == ' ' || *h == '\t')
            h++;
        h += strlen("sysex");
        app_sx_host_frame(h, "sysex");
    } else if (!strcmp(cmd, "sysexf")) {       /* sysexf PATH: a frame a line (blank lines and # comments skipped) */
        static char line[4096];
        FILE *f;
        app_sx_path(path, sizeof path, cmd);
        if (!(f = fopen(path, "r"))) {
            printf("FAIL sysexf %s: cannot open\n", path);
            exit(2);
        }
        while (fgets(line, sizeof line, f)) {
            const char *s = line;
            while (*s == ' ' || *s == '\t')
                s++;
            if (*s && *s != '#' && *s != '\n' && *s != '\r')
                app_sx_host_frame(s, "sysexf");
        }
        fclose(f);
    } else if (!strcmp(cmd, "params_json")) {  /* params_json PATH: the §3.6 table (tools/cyma1_params.json) */
        static char buf[16384];
        uint32_t n = cy_params_json(buf, sizeof buf);
        FILE *f;
        app_sx_path(path, sizeof path, cmd);
        if (!n || !(f = fopen(path, "wb"))) {
            printf("FAIL params_json %s: %s\n", path, n ? "cannot write" : "no room");
            exit(2);
        }
        fwrite(buf, 1, n, f);
        fclose(f);
        printf("params_json: %u B -> %s\n", (unsigned)n, path);
    }
}
#endif
