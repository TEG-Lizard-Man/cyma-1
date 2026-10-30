/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 engine (M2): the public API (cy_engine.h), the main-loop -> render hand-over, voice allocation and stealing,
 * the mode budget, the exciters' scheduling (MALLET strike and contact noise, BOW, RAIN), SURFACE mode, modulation,
 * telemetry and UI events, the CPU model, and the float bus into fx.c. Spec §3.3-3.9, 3.12-3.14.
 *
 * Slots 0..7 are TUNED voices, 8..11 the steal-fade pool (a stolen voice fades there over 5 ms while the new note
 * starts at once in the freed slot), 12 is the SURFACE slot (one object, the keybed's 27 strike points on it).
 * A render: pick up the patch and the object, apply the queued events (a strike queues the modes its contact will
 * drive), share out the mode budget (only then do those modes start, so a mode the budget will not run is never
 * started), then four 32-sample sub-blocks of control + kernels per live slot, then fx, then telemetry.
 *
 * Where this departs from the spec's letter, and why (each measured; the comments at each site carry the detail):
 *   - strikes and rain drops are played as their Hertz contact force through the modes, band-limited, not as
 *     impulses (an impulse's onset alone put energy above 0.45 fs; spec §8.3.2);
 *   - the bow couples every mode its point can reach, injects at the half-sample phase, and runs at beta 0.008 with a
 *     contact low-pass and bow-speed jitter (bow.c: the spec's constants gave a DC lock-up or chaotic chatter);
 *   - the budget is water-filled, not greedy (a greedy pass silenced half of an 8-note chord);
 *   - modes glide their coefficients when their frequency moves, and fade toward the band edge (voice.c);
 *   - SURFACE: the LED map is energy, a light touch is a firmer damper than a pressed strike (cy_surface_ctl);
 *   - a +6 dB output trim brings a vel-100 strike to §3.11's -18 dBFS (cy_engine_int.h CY_OUT_TRIM);
 *   - four steal-fade slots, not one: a chord into a full pool steals several voices in one render (cy_steal);
 *   - MIDI pressure only adds (P_eff = v_n sw + (1 - v_n sw) P: voice.c), and channel pressure reaches MIDI-played
 *     notes only, not the FM-1's own keys (the render's pressure pickup);
 *   - SURFACE: an OCT change lets the ringing plate ring out at its own pitch (cy_surf_on), LATCH holds released
 *     points (cy_surf_off), and MIDI-played points take the MIDI P_eff and channel pressure (cy_surface_ctl);
 *   - DAMP chokes frozen slots too (spec §3.4's "every slot"; voice.c);
 *   - a bowed slot asks the budget only for the modes its bow reaches, and the bowed slots share half the budget's
 *     modes as coupled modes, so the governor's budget reaches the bow (CY_BOW_SHARE, bow.c). */
#include "cy_engine_int.h"

cy_eng_t cy_e CY_POOL;
cy_q_t cy_q CY_POOL;
static cy_telemetry_t cy_tel CY_POOL;    /* seqlock'd: cy_tel.seq is odd while the render writes it */
#define CY_TEL_SEQ (*(volatile uint32_t *)&cy_tel.seq)
static cy_fx_params_t cy_fxp;
static const char *const CY_EXC_NAME[CY_NEXC] = {"MALLET", CY_BOW_MODEL ? "BOW" : "BREATH", "RAIN"};

#define CY_BARRIER() __asm__ volatile("" ::: "memory")
#define CY_SMOOTH 0.07f                  /* patch smoothing per sub-block: ~10 ms */

/* ============================================================ main loop side === */
static void cy_push(uint8_t type, int key, int note, int vel, uint32_t arg)
{
    uint32_t w = cy_q.ev_w;
    cy_ev_t *ev;
    if (w - cy_q.ev_r >= CY_EVQ) {       /* full: only if nobody renders (never on the device: a render each 2.9 ms) */
        cy_q.dropped++;
        return;
    }
    ev = &cy_q.ev[w & (CY_EVQ - 1u)];
    ev->type = type;
    ev->key = (int8_t)key;
    ev->note = (uint8_t)note;
    ev->vel = (uint8_t)vel;
    ev->arg = arg;
    CY_BARRIER();                        /* the entry is written before the render can see it */
    cy_q.ev_w = w + 1u;
}

void cy_engine_init(void)
{
    uint32_t i;
    __builtin_memset(&cy_e, 0, sizeof cy_e);
    __builtin_memset(&cy_q, 0, sizeof cy_q);
    {   /* (seq survives: a reader mid-copy must see it move) */
        uint32_t sq = CY_TEL_SEQ;
        __builtin_memset(&cy_tel, 0, sizeof cy_tel);
        CY_TEL_SEQ = (sq + 2u) & ~1u;
    }
    for (i = 0; i < CY_NALL; i++)
        cy_slot_clear(&cy_e.sl[i]);
    for (i = 0; i < CY_NVOICES; i++)
        cy_tel.voice_key[i] = -1;
    cy_e.rng = 0x9E3779B9u;
    cy_e.budget = CY_BUDGET_DEFAULT;
    cy_e.bow_key = -1;
    cy_e.obj = cy_geom_object();
    cy_q.budget = CY_BUDGET_DEFAULT;
    cy_q.bow_cap = CY_MAX_BOWED;
    cy_e.bow_cap = CY_MAX_BOWED;
    cy_q.tail_cap = CY_NVOICES;
    cy_e.tail_cap = CY_NVOICES;
    cy_tel.mode_budget = CY_BUDGET_DEFAULT;
}
void cy_engine_seed(uint32_t seed) { cy_push(CY_EV_SEED, 0, 0, 0, seed ? seed : 0x9E3779B9u); }

void cy_engine_set_patch(const cy_patch_t *p)
{
    uint32_t w = cy_q.pfront ^ 1u;       /* the buffer the render is not reading */
    cy_q.pbuf[w] = *p;
    CY_BARRIER();
    cy_q.pfront = w;
    cy_q.pseq++;
}

static void cy_note_on_ev(int key, int note, int vel, uint32_t midi)
{
    if (note < 0 || note > 127)
        return;
    if (vel <= 0) {
        cy_engine_note_off(key, note);
        return;
    }
    if (key >= 0 && key < CY_NKEYS)
        cy_q.press_key[key] = 0;         /* a new press starts from no lean (the app sends pressure on change only) */
    cy_q.press_note[note] = 0;
    cy_push(CY_EV_ON, key >= 0 && key < CY_NKEYS ? key : -1, note, vel > 127 ? 127 : vel, midi);
}
void cy_engine_note_on(int key, int note, int vel) { cy_note_on_ev(key, note, vel, 0u); }
void cy_engine_note_on_midi(int key, int note, int vel) { cy_note_on_ev(key, note, vel, 1u); }
void cy_engine_note_off(int key, int note)
{
    if (note < 0 || note > 127)
        return;
    if (key >= 0 && key < CY_NKEYS)
        cy_q.press_key[key] = 0;
    cy_q.press_note[note] = 0;
    cy_push(CY_EV_OFF, key >= 0 && key < CY_NKEYS ? key : -1, note, 0, 0);
}
void cy_engine_pressure(int key, int note, int press)
{
    uint8_t v = (uint8_t)(press < 0 ? 0 : press > 127 ? 127 : press);
    if (key >= 0 && key < CY_NKEYS)
        cy_q.press_key[key] = v;
    else if (note >= 0 && note < 128)
        cy_q.press_note[note] = v;
}
void cy_engine_channel_pressure(int press) { cy_q.chan = (uint8_t)(press < 0 ? 0 : press > 127 ? 127 : press); }
void cy_engine_bend(float semitones) { cy_q.bend = fm_clampf(semitones, -24.0f, 24.0f); }
void cy_engine_freeze(int on) { cy_push(CY_EV_FREEZE, 0, 0, 0, on != 0); }
void cy_engine_latch(int on) { cy_push(CY_EV_LATCH, 0, 0, 0, on != 0); }
void cy_engine_damp(int on) { cy_push(CY_EV_DAMP, 0, 0, 0, on != 0); }
void cy_engine_panic(void) { cy_push(CY_EV_PANIC, 0, 0, 0, 0); }
void cy_engine_set_mode_budget(int n) { cy_q.budget = n < CY_BUDGET_MIN ? CY_BUDGET_MIN : n > CY_BUDGET_MAX ? CY_BUDGET_MAX : n; }
void cy_engine_set_bow_cap(int n) { cy_q.bow_cap = n < 1 ? 1 : n > CY_MAX_BOWED ? CY_MAX_BOWED : n; }
void cy_engine_set_tail_cap(int n) { cy_q.tail_cap = n < 0 ? 0 : n > (int)CY_NVOICES ? (int)CY_NVOICES : n; }
void cy_engine_tails_off(void) { cy_push(CY_EV_TAILS, 0, 0, 0, 0); }
void cy_engine_restart(int hand)
{
    if (cy_q.ev_w - cy_q.ev_r >= CY_EVQ)   /* (a full queue drops it, as cy_push would: nobody renders) */
        return;
    cy_q.rs_push++;                      /* (first: from here the render takes no new patch until it has applied it) */
    CY_BARRIER();
    cy_push(CY_EV_RESTART, 0, 0, 0, hand != 0);
}
int cy_engine_restart_ready(void) { return cy_q.rs_ready; }

void cy_engine_telemetry_get(cy_telemetry_t *out)
{
    uint32_t s0, s1, tries = 0;
    do {
        s0 = CY_TEL_SEQ;
        CY_BARRIER();
        *out = cy_tel;
        CY_BARRIER();
        s1 = CY_TEL_SEQ;
    } while ((s0 != s1 || (s0 & 1u)) && ++tries < 16u);
}

int cy_engine_event(cy_uievent_t *ev)
{
    uint32_t r = cy_e.ui_r;
    if (r == cy_e.ui_w)
        return 0;
    CY_BARRIER();
    *ev = cy_e.ui[r & (CY_UIQ - 1u)];
    CY_BARRIER();
    cy_e.ui_r = r + 1u;
    return 1;
}

const char *cy_engine_exciter_name(int exc) { return CY_EXC_NAME[exc < 0 ? 0 : exc >= CY_NEXC ? CY_NEXC - 1 : exc]; }

/* =============================================================== render side === */
static void cy_ui_post(uint8_t type, int vel, float u, float v)
{
    uint32_t w = cy_e.ui_w;
    cy_uievent_t *ev;
    if (w - cy_e.ui_r >= CY_UIQ)
        return;                          /* the UI is behind: drop, never block */
    ev = &cy_e.ui[w & (CY_UIQ - 1u)];
    ev->type = type;
    ev->vel = (uint8_t)(vel < 0 ? 0 : vel > 127 ? 127 : vel);
    ev->u = (uint16_t)(fm_clampf(u, 0.0f, 1.0f) * 65535.0f + 0.5f);
    ev->v = (uint16_t)(fm_clampf(v, 0.0f, 1.0f) * 65535.0f + 0.5f);
    CY_BARRIER();
    cy_e.ui_w = w + 1u;
}

static int cy_live(const cy_slot_t *sl) { return sl->state != CY_VS_FREE || sl->fading; }

/* f_note's derived constants for a slot that starts a note */
static void cy_note_init(cy_slot_t *sl, int key, int note, int vel, int xt)
{
    const cy_object_t *o = cy_e.obj;
    uint32_t k;
    cy_slot_clear(sl);
    sl->key = (int8_t)key;
    sl->note = (uint8_t)note;
    sl->vel = (uint8_t)vel;
    sl->xt = (uint8_t)xt;
    sl->fnote = 440.0f * fm_exp2f(((float)note - 69.0f) * (1.0f / 12.0f));
    sl->tfac = fm_powf(261.63f / sl->fnote, 0.5f);
    sl->bbeta = CY_BOW_BETA * fm_powf(sl->fnote * (1.0f / 261.63f), 0.5f);
    sl->has_at = key >= 0 && key < 20;   /* (MIDI notes, key -1, take v_n sw + (1 - v_n sw) P: voice.c) */
    sl->trim = 1.0f;
    sl->M = cy_e.gpitch * (1.0f + 0.06f * cy_e.bloom);
    sl->str = sl->sx = 1.0f;
    sl->B = 1.0f;
    sl->dirty = sl->fresh_slot = 1;
    sl->gsig = -1.0f;
    sl->born = cy_e.rnd;
    for (k = 0; k < CY_N; k++)
        sl->rc[k] = o->ratio[k], sl->lrc[k] = o->log2_ratio[k], sl->r[k] = 0.0f;
}

/* the mode set a slot may start: the modes_per_voice lowest of order[], below 0.45 fs at its current pitch */
static uint32_t cy_eligible(const cy_slot_t *sl)
{
    const cy_object_t *o = cy_e.obj;
    uint32_t j, m = 0, mpv = cy_e.p.modes_per_voice;
    float f0 = sl->fnote * sl->M;
    if (mpv < 1u || mpv > CY_N)
        mpv = CY_N;
    for (j = 0; j < mpv; j++) {
        uint32_t k = o->order[j];
        if (f0 * sl->rc[k] < CY_NYQ)
            m |= 1u << k;
    }
    return m;
}

/* the contact force pulse (spec §3.6's Hertz contact, played rather than summarised): a half-sine of N samples,
 * scaled to unit area, through the EXCITED kernel into the modes with b_k = phi_k. Each mode then rings at exactly
 * the amplitude the spec's impulse A_k = phi_k H_k(tau) would give it (H_k is this pulse's spectrum), but starts as
 * t^3 instead of with a corner: an impulse starts every mode at full slope, a broadband transient that puts energy
 * above 0.45 fs (-61 dB of the total for a lone 3 kHz mode, by golden.py's measure); the pulse's onset leaves the
 * band clean (spec §8.3.2: <= -100 dB). A second contact before the pulse has started (two drops due in one
 * sub-block, a chord of SURFACE keys) joins it: the fields add, weighted by amplitude. Returns 0 only when a pulse
 * is already sounding (a re-strike inside the contact time): the caller then uses the impulse. */
static int cy_pulse(cy_slot_t *sl, const float *phi, float amp, float tau)
{
    uint32_t k, nn = (uint32_t)(tau * CY_FS + 0.5f);
    float c, s;
    if (sl->hp_n && sl->hp_n == sl->hp_len && sl->hp_amp > 0.0f) {   /* not started: join it */
        float g = amp * sl->hp_s / sl->hp_amp;   /* (hp_amp = amp0 sin(pi / 2N); hp_s = sin(pi / 2N) still) */
        for (k = 0; k < CY_N; k++)
            sl->phi[k] += g * phi[k];
        return 1;
    }
    if (sl->hp_n)
        return 0;
    if (nn < 1u)
        nn = 1u;
    if (nn > 512u)
        nn = 512u;
    for (k = 0; k < CY_N; k++)
        sl->phi[k] = phi[k];
    cy_sincos(3.14159265f / (float)nn, &sl->hp_dc, &sl->hp_ds);              /* the step: pi / N */
    cy_sincos(1.57079633f / (float)nn, &c, &s);                             /* the first sample, pi / 2N */
    sl->hp_c = c, sl->hp_s = s;
    sl->hp_amp = amp * s;                /* sum of sin(pi (n + 1/2) / N) over N samples is 1 / sin(pi / 2N) */
    sl->hp_n = sl->hp_len = nn;
    return 1;
}

/* a strike (spec §3.6 MALLET) of the modes_per_voice lowest eligible modes: as a contact pulse (cy_pulse), or, if
 * one is already playing, as the spec's impulse A_k = vamp phi_k H_k(tau). Either way the modes start after the
 * budget pass (cy_budget), and only where it allows. The contact noise starts with it. */
static void cy_strike(cy_slot_t *sl, const float *phi, int vel, float u, float v)
{
    cy_eng_t *e = &cy_e;
    uint32_t k, elig = cy_eligible(sl);
    float vn = (float)vel * (1.0f / 127.0f), vamp = fm_powf(vn, 1.4f), hard = e->p.hardness, it = 1.0f / sl->trim;
    float tau = 0.008f * fm_exp2f(-5.0f * hard) * fm_powf((float)vel * (1.0f / 64.0f), -0.2f);
    float f0 = sl->fnote * sl->M;
    int pulse;
    if (tau < 6.0f / CY_FS)
        tau = 6.0f / CY_FS;
    pulse = cy_pulse(sl, phi, it * vamp, tau);
    for (k = 0; k < CY_N; k++) {
        float a;
        if (!(elig >> k & 1u) || phi[k] == 0.0f)
            continue;
        if (pulse && sl->state != CY_VS_FROZEN) {
            sl->pmask |= 1u << k;
            continue;
        }
        a = it * vamp * phi[k] * cy_hertz(f0 * sl->rc[k], tau);
        if (pulse)
            sl->pmask |= 1u << k, sl->Efrz += a * a;   /* (into a frozen slot: its governor's ceiling rises with it) */
        else
            sl->pend[k] += a, sl->pendm |= 1u << k;
    }
    /* contact noise: 3 (1 - 0.5 hardness) ms of 2 kHz high-passed white noise under a Hann window */
    sl->nz_len = sl->nz_n = (uint32_t)(0.003f * (1.0f - 0.5f * hard) * CY_FS + 0.5f);
    sl->nz_amp = 0.3f * e->noise * vamp * it;
    sl->nz_ec = 1.0f, sl->nz_es = 0.0f;
    sl->ref_pend = 1;                    /* BLOOM: E_ref is E_low one sub-block from now */
    sl->vamp = vamp;
    cy_ui_post(CY_UIEV_STRIKE, vel, u, v);
}

/* ---------------------------------------------------------------- stealing --- */
/* A slot of the steal-fade pool for a voice about to fade out: a free one, or else (a fifth steal inside one fade)
 * the one that is quietest now, cut (counted). */
static cy_slot_t *cy_fade_slot(void)
{
    cy_slot_t *f = 0;
    float q = 0.0f;
    uint32_t i;
    for (i = CY_SFADE; i < CY_SFADE + CY_NFADE; i++) {
        cy_slot_t *s = &cy_e.sl[i];
        if (!cy_live(s))
            return s;
        if (!f || s->lvl < q)            /* (lvl is heard through the gains, the fade included) */
            f = s, q = s->lvl;
    }
    cy_e.steal_cuts++;
    return f;
}

/* v's sound moves into a steal-fade slot and fades out there over secs (or goes on with the fade it is already in,
 * after a PANIC), so v is free at once: the new note starts with zero added latency and no click (spec §3.8) */
static void cy_fade_copy(const cy_slot_t *v, float secs)
{
    cy_slot_t *f = cy_fade_slot();
    float g0 = v->fading ? v->fade_g : 1.0f, d0 = v->fading ? v->fade_dec : 0.0f;
    *f = *v;
    f->fading = 1;
    f->fade_g = g0;
    f->fade_dec = fm_maxf(d0, 1.0f / (secs * CY_FS));
    f->state = CY_VS_RING;
    f->bowing = f->raining = 0;
    f->nz_n = f->hp_n = f->ct_tail = 0;
    f->down = f->lhold = 0;
    f->key = -1;
    f->cpl = f->exc = 0;
    f->gsig = -1.0f;
    f->allow = f->act;
}

/* (the slot is not cleared here: cy_alloc's caller always starts the new note in it with cy_note_init, which does) */
static void cy_steal(int i)
{
    cy_slot_t *v = &cy_e.sl[i];
    if (v->born != cy_e.rnd || v->state == CY_VS_FROZEN || v->fading)   /* it has been heard: fade it out */
        cy_fade_copy(v, CY_STEAL_FADE_S);
}

/* allocation (spec §3.8): same key re-strike, a free slot, the quietest ringing slot, the oldest held one; frozen
 * slots last. BOW: at most CY_MAX_BOWED bowed slots (or the device governor's lower cap, v1.1), one more takes over the
 * oldest. (v1.1) Under a cap the governor lowered, a bowed note past it while every bowed note is younger than
 * CY_GOV_CHORD_R renders is the same chord's, and it does not start (-1): taking over notes that had only just begun
 * cost a 5 ms steal-fade copy and a fresh bow per note, on top of the bows (fm1-emu at 1.6x: a 4-note AETHER chord onto
 * two old tails, 102 % of a block; an 8-note chord, 93 %), and the chord sounds its first notes instead of its last.
 * Returns the slot, re-strike in *same, or -1. */
static int cy_alloc(int key, int note, int bow, int *same)
{
    cy_eng_t *e = &cy_e;
    int i, best = -1, nv = e->p.max_voices, nb = 0, ob = -1;
    float q = 0.0f;
    uint32_t a = 0;
    if (nv < 1 || nv > CY_NVOICES)
        nv = CY_NVOICES;
    *same = 0;
    for (i = 0; i < CY_NVOICES; i++) {
        cy_slot_t *sl = &e->sl[i];
        if (sl->state != CY_VS_FREE && sl->state != CY_VS_FROZEN && !sl->fading && sl->note == note &&
            (key >= 0 ? sl->key == key : sl->key < 0)) {
            *same = 1;
            return i;
        }
    }
    if (bow) {                           /* the bowed-slot cap */
        for (i = 0; i < CY_NVOICES; i++)
            if (e->sl[i].bowing && !e->sl[i].fading) {
                nb++;
                if (ob < 0 || e->sl[i].age < e->sl[ob].age)
                    ob = i;
            }
        if (nb >= (int)e->bow_cap) {
            if (e->bow_cap < CY_MAX_BOWED && e->rnd - e->sl[ob].born < CY_GOV_CHORD_R)
                return -1;               /* (v1.1: never with the caps at CY_MAX_BOWED, as on the host and the web) */
            cy_steal(ob);
            return ob;
        }
    }
    for (i = 0; i < nv; i++)
        if (e->sl[i].state == CY_VS_FREE && !e->sl[i].fading)
            return i;
    for (i = 0; i < nv; i++)             /* the quietest ringing slot */
        if (e->sl[i].state == CY_VS_RING && !e->sl[i].fading && (best < 0 || e->sl[i].lvl < q))
            best = i, q = e->sl[i].lvl;
    if (best < 0)                        /* the oldest held (or bowed) */
        for (i = 0; i < nv; i++)
            if ((e->sl[i].state == CY_VS_HELD || e->sl[i].state == CY_VS_BOWED) && !e->sl[i].fading &&
                (best < 0 || e->sl[i].age < a))
                best = i, a = e->sl[i].age;
    if (best < 0)                        /* the oldest frozen */
        for (i = 0; i < nv; i++)
            if (!e->sl[i].fading && (best < 0 || e->sl[i].age < a))
                best = i, a = e->sl[i].age;
    if (best < 0)                        /* every slot fading (a PANIC this render): the one furthest through its fade,
                                          * which goes on fading in the pool (cy_steal) */
        for (i = 0; i < nv; i++)
            if (best < 0 || e->sl[i].fade_g < q)
                best = i, q = e->sl[i].fade_g;
    cy_steal(best);
    return best;
}

/* ------------------------------------------------------------- note events --- */
static void cy_bow_start(cy_slot_t *sl, const float *phi)
{
    uint32_t k;
    float t = CY_BOW_GAIN * sl->vamp;
    if (!sl->bowing) {
        for (k = 0; k < CY_N; k++)
            sl->bphi[k] = phi[k];
        sl->bF = 0.0f;
        sl->bgate = 0.0f;
        sl->breach = 0xFFFFFFFFu;        /* (what the bow reaches is known after its first sub-block: bow.c) */
    }
    if (sl->act && t != sl->trim) {      /* still ringing at another trim: rescale the state so the level holds */
        float g = sl->trim / t;
        for (k = 0; k < CY_N; k++)
            sl->zr[k] *= g, sl->zi[k] *= g;
        sl->fresh |= sl->act;            /* (and the gains jump with it, so the product never steps) */
    }
    sl->bowing = 1;
    sl->blim = 1;
    sl->trim = t;
    sl->gsig = -1.0f;
}

static void cy_tuned_on(int key, int note, int vel)
{
    cy_eng_t *e = &cy_e;
    const cy_object_t *o = e->obj;
    int xt = e->p.exciter, same, i;
    cy_slot_t *sl;
    i = cy_alloc(key, note, xt == CY_EXC_BOW, &same);
    if (i < 0)
        return;                          /* (v1.1: a chord's bowed note past the device governor's cap) */
    sl = &e->sl[i];
    if (!same)
        cy_note_init(sl, key, note, vel, xt);
    sl->xt = (uint8_t)xt;
    sl->vel = (uint8_t)vel;
    sl->vn = (float)vel * (1.0f / 127.0f);
    sl->vamp = fm_powf(sl->vn, 1.4f);
    sl->age = ++e->serial;
    sl->t_on = 0;
    sl->down = 1;
    sl->lhold = 0;
    sl->Praw = 0.0f;
    if (xt == CY_EXC_MALLET) {
        const float *phi = o->strike;
        float u = o->pos_uv[0], v = o->pos_uv[1];
        uint32_t j;
        int wander = 0;
        for (j = 0; j < CY_WANDER_PTS; j++)   /* WANDER > 0: the points spread away from pos */
            if (fm_fabsf(o->wander_uv[j][0] - u) + fm_fabsf(o->wander_uv[j][1] - v) > 1e-4f)
                wander = 1;
        if (wander) {
            j = cy_rand(&e->rng) >> 28;      /* 16 points */
            phi = o->wander[j], u = o->wander_uv[j][0], v = o->wander_uv[j][1];
        }
        sl->bowing = sl->raining = 0;
        cy_strike(sl, phi, vel, u, v);
        sl->state = CY_VS_HELD;
    } else if (xt == CY_EXC_BOW) {
        sl->raining = 0;
        sl->nz_n = 0;
        cy_bow_start(sl, o->strike);
        sl->state = CY_VS_BOWED;
        cy_ui_post(CY_UIEV_SLIP, vel, o->pos_uv[0], o->pos_uv[1]);
    } else {
        sl->bowing = 0;
        sl->raining = 1;
        sl->rain_cd = 0.0f;                  /* the first drop lands now */
        sl->state = CY_VS_HELD;
    }
}

static void cy_release(cy_slot_t *sl)
{
    if (sl->state == CY_VS_FROZEN)
        return;                          /* frozen slots ignore the key; FREEZE off makes them ring */
    sl->raining = 0;
    sl->state = CY_VS_RING;              /* a bowed slot lifts: its gate falls over 15 ms (bow.c) */
    sl->dirty = 1;                       /* the key-up damper */
}

static void cy_tuned_off(int key, int note)
{
    cy_eng_t *e = &cy_e;
    int i;
    for (i = 0; i < CY_NVOICES; i++) {
        cy_slot_t *sl = &e->sl[i];
        if (!sl->down || sl->fading || sl->note != note || (key >= 0 ? sl->key != key : sl->key >= 0))
            continue;
        sl->down = 0;
        sl->dirty = 1;
        if (e->latched)
            sl->lhold = 1;               /* LATCH: it keeps sounding (a bow keeps its last pressure) */
        else
            cy_release(sl);
    }
}

static void cy_surf_on(int key, int vel, int midi)
{
    cy_eng_t *e = &cy_e;
    const cy_object_t *o = e->obj;
    cy_slot_t *sl = &e->sl[CY_SSURF];
    float note = e->p.surface_root;
    int xt = e->p.exciter, root = (int)(note + 0.5f);
    if (!cy_live(sl) || sl->fading) {
        cy_note_init(sl, -1, root, 100, xt);
    } else if (sl->note != (uint8_t)root && vel >= 24) {   /* OCT moved the root: the next strike, drop or bow takes
                                                            * the new pitch (a touch damps the plate that rings) */
        if (sl->state != CY_VS_FROZEN) {
            /* the plate that rings keeps its pitch and rings out over 100 ms in the steal-fade pool, and a fresh
             * plate at the new root takes the strike. (Retuned in place, every ringing partial jumped an octave at
             * one sub-block edge: heard as the tail leaping, with a tick, -75 dBFS at 8 kHz against -97) */
            cy_fade_copy(sl, CY_SURF_XFADE_S);
            cy_note_init(sl, -1, root, 100, xt);
        } else {                         /* a frozen plate holds its sound: it glides to the new pitch (cy_slot_coeffs) */
            sl->note = (uint8_t)root;
            sl->fnote = 440.0f * fm_exp2f((note - 69.0f) * (1.0f / 12.0f));
            sl->tfac = fm_powf(261.63f / sl->fnote, 0.5f);
            sl->bbeta = CY_BOW_BETA * fm_powf(sl->fnote * (1.0f / 261.63f), 0.5f);
            sl->dirty = 1;
        }
    }
    e->sk_down[key] = 1;
    e->sk_lhold[key] = 0;
    e->sk_midi[key] = (uint8_t)(midi != 0);
    e->sk_vel[key] = (uint8_t)vel;
    e->sk_t[key] = 0;
    e->sk_P[key] = 0.0f;
    e->sk_at[key] = !midi && key < 20;   /* the FM-1's sensing keys (spec §3.6); a MIDI note takes v_n sw + AT */
    sl->age = ++e->serial;
    sl->xt = (uint8_t)xt;
    if (vel < 24) {                      /* a touch: the point damper only */
        e->sk_touch[key] = 1;
        cy_ui_post(CY_UIEV_TOUCH, vel, o->key_uv[key][0], o->key_uv[key][1]);
        if (sl->state == CY_VS_FREE)
            sl->state = CY_VS_RING;
        return;
    }
    e->sk_touch[key] = 0;
    if (xt == CY_EXC_MALLET) {
        if (sl->state != CY_VS_FROZEN)
            sl->state = CY_VS_HELD;
        cy_strike(sl, o->surface[key], vel, o->key_uv[key][0], o->key_uv[key][1]);
    } else if (xt == CY_EXC_RAIN) {
        e->sk_rain[key] = 1;
        e->sk_rcd[key] = 0.0f;
        if (sl->state != CY_VS_FROZEN)
            sl->state = CY_VS_HELD;
    } else if (sl->state != CY_VS_FROZEN) {
        sl->vn = (float)vel * (1.0f / 127.0f);
        sl->vamp = fm_powf(sl->vn, 1.4f);
        if (e->bow_key < 0)
            e->bow_key = (int8_t)key;
        cy_bow_start(sl, o->surface[e->bow_key]);
        sl->state = CY_VS_BOWED;
        cy_ui_post(CY_UIEV_SLIP, vel, o->key_uv[key][0], o->key_uv[key][1]);
    }
}

static void cy_surf_release(int key)
{
    cy_eng_t *e = &cy_e;
    e->sk_down[key] = e->sk_touch[key] = e->sk_rain[key] = e->sk_lhold[key] = e->sk_midi[key] = 0;
    e->sk_P[key] = 0.0f;
}

/* a SURFACE key up: with LATCH on the point stays as it was, its pressure held (as cy_tuned_off holds a TUNED slot;
 * before, the ARP LED said LATCH while a released bowed or rained point stopped at once) */
static void cy_surf_off(int key)
{
    if (cy_e.latched && cy_e.sk_down[key])
        cy_e.sk_lhold[key] = 1;
    else
        cy_surf_release(key);
}

static void cy_panic_now(void)
{
    cy_eng_t *e = &cy_e;
    int i;
    for (i = 0; i < CY_NALL; i++) {      /* everything fades out over one render (~3 ms): silent, not a click */
        cy_slot_t *sl = &e->sl[i];
        if (!cy_live(sl))
            continue;
        if (!sl->fading) {
            sl->fading = 1;
            sl->fade_g = 1.0f;
        }
        sl->panic = 1;
        sl->fade_dec = fm_maxf(sl->fade_dec, 1.0f / (float)CY_BLOCK);
        sl->bowing = sl->raining = 0;
        sl->nz_n = sl->hp_n = sl->ct_tail = 0;
        sl->down = sl->lhold = 0;
        sl->state = CY_VS_RING;
        sl->gsig = -1.0f;
    }
    for (i = 0; i < CY_NKEYS; i++)
        cy_surf_release(i);
    e->bow_key = -1;
    e->frozen = e->latched = e->damping = 0;
}

/* (v1.1) a slot the device governor lets go: it fades out in place over secs, a bow lifted at once (as a PANIC
 * fades, but slower) */
static void cy_gov_fade(cy_slot_t *sl, float secs)
{
    sl->fading = 1;
    sl->fade_g = 1.0f;
    sl->fade_dec = 1.0f / (secs * CY_FS);
    sl->bowing = 0;
    sl->bhold = 0;
    sl->down = sl->lhold = 0;
    sl->state = CY_VS_RING;
    sl->gsig = -1.0f;
}

static void cy_apply(const cy_ev_t *ev)
{
    cy_eng_t *e = &cy_e;
    int i;
    switch (ev->type) {
    case CY_EV_ON:
        if (e->p.surface && ev->key >= 0)
            cy_surf_on(ev->key, ev->vel, ev->arg != 0u);
        else
            cy_tuned_on(ev->key, ev->note, ev->vel);
        break;
    case CY_EV_OFF:                      /* either side may hold it (the mode may have changed since the press) */
        if (ev->key >= 0 && e->sk_down[ev->key])
            cy_surf_off(ev->key);
        cy_tuned_off(ev->key, ev->note);
        break;
    case CY_EV_FREEZE:
        if (ev->arg && !e->frozen) {     /* the slots sounding now freeze; new notes play normally */
            for (i = 0; i < CY_NALL; i++) {
                cy_slot_t *sl = &e->sl[i];
                if (sl->state == CY_VS_FREE || sl->fading || CY_IS_FADE(i))
                    continue;
                sl->state = CY_VS_FROZEN;    /* (a bow lifts through its 15 ms gate: no released-deflection thump) */
                sl->Efrz = fm_maxf(sl->E, 1e-20f);
                sl->raining = 0;
                sl->nz_n = 0;
                sl->dirty = 1;
            }
            for (i = 0; i < CY_NKEYS; i++)
                e->sk_rain[i] = 0;
            e->bow_key = -1;
        } else if (!ev->arg && e->frozen) {
            for (i = 0; i < CY_NALL; i++)
                if (e->sl[i].state == CY_VS_FROZEN) {
                    e->sl[i].state = CY_VS_RING;
                    e->sl[i].dirty = 1;
                }
        }
        e->frozen = (uint8_t)ev->arg;
        break;
    case CY_EV_LATCH:
        e->latched = (uint8_t)ev->arg;
        if (!ev->arg) {
            for (i = 0; i < CY_NVOICES; i++)
                if (e->sl[i].lhold) {
                    e->sl[i].lhold = 0;
                    cy_release(&e->sl[i]);
                }
            for (i = 0; i < CY_NKEYS; i++)   /* and the SURFACE points it held */
                if (e->sk_lhold[i])
                    cy_surf_release(i);
        }
        break;
    case CY_EV_DAMP:
        e->damping = (uint8_t)ev->arg;
        break;
    case CY_EV_PANIC:
        cy_panic_now();
        break;
    case CY_EV_SEED:
        e->rng = ev->arg;
        break;
    case CY_EV_TAILS:                    /* (v1.1, the device governor) the released notes ringing now fade over 5 ms,
                                          * a steal's fade, and the events after this one (the bowed chord's next notes)
                                          * wait CY_TAILS_HOLD renders for it: a fading slot costs what it did ringing,
                                          * so faded together with the chord's onset, the tails still peaked with it */
        for (i = 0; i < CY_NVOICES; i++) {
            cy_slot_t *sl = &e->sl[i];
            if (sl->state == CY_VS_RING && !sl->fading && !sl->down && !sl->lhold) {
                cy_gov_fade(sl, CY_STEAL_FADE_S);
                e->ev_hold = CY_TAILS_HOLD;   /* (a count down, not a render serial: no wrap to stall on) */
            }
        }
        break;
    case CY_EV_RESTART:                  /* (v1.1.1, cy_engine_restart: the boot logo's start and end, and a touch
                                          * that ends it) everything heard fades out over CY_RESTART_FADE renders with
                                          * the patch and the plate it had (cy_engine_render holds the patch), then
                                          * cy_restart_now; the events after this one wait */
        e->restart = 1;
        e->rs_hand = (uint8_t)(ev->arg != 0);
        e->rs_n = 0;
        cy_q.rs_take++;
        break;
    default:
        break;
    }
}

/* (v1.1.1) a restart's fade is done (the output is at 0): the engine as cy_engine_init leaves it, and the room silent
 * (cy_fx_clear); pick: the patch in force taken now as a first one (nothing glides in from what played before), or (a
 * hand-over) the old one kept, unpicked, until the main loop's next. Kept: the queue and its last-value cells (the
 * main loop's), the UI's event ring, the published object, the budget and the caps, the NaN counters, and the clocks
 * that run through silence too (the render serial, the vibrato's phase; the room's drift in fx.c): after the boot
 * logo the engine is then exactly as the same time of silence would have left it. Render side only, so nothing races
 * the main loop */
static void cy_pickup_patch(void);
static void cy_restart_now(int pick)
{
    cy_eng_t *e = &cy_e;
    const cy_object_t *o = e->obj;
    uint32_t oseq = e->obj_seq, nan = e->nan_resets, rnd = e->rnd, i;
    uint16_t bud = e->budget;
    uint8_t bc = e->bow_cap, tc = e->tail_cap;
    float vph = e->vib_ph;
    cy_patch_t p = e->p;
    __builtin_memset(e, 0, __builtin_offsetof(cy_eng_t, ui));
    for (i = 0; i < CY_NALL; i++)
        cy_slot_clear(&e->sl[i]);
    e->rng = 0x9E3779B9u;
    e->bow_key = -1;
    e->obj = o, e->obj_seq = oseq, e->nan_resets = nan, e->rnd = rnd, e->vib_ph = vph;
    e->budget = bud, e->bow_cap = bc, e->tail_cap = tc;
    e->pseq = cy_q.pseq - 1u;            /* (picked up again, as the first: inited is 0) */
    if (pick)
        cy_pickup_patch();
    else
        e->p = p;                        /* (the vibrato's clock runs on at its rate until the next patch) */
    cy_fx_clear();
}

/* -------------------------------------------------------------- the budget --- */
/* Share the budget (spec §3.8). Every live slot wants its eligible modes (if it is bowed or raining it will drive
 * them all; otherwise the ones it has or is about to be struck into). Max-min fair water-filling, leftovers to the
 * newest; each slot keeps the lowest of its wanted modes by order[], so an older slot loses its highest first,
 * through a one-sub-block raised-cosine fade. (The spec's plain newest-first greedy pass would silence the older
 * half of an 8-note chord outright at budget 128; water-filling keeps every voice and still never exceeds the
 * budget.) The steal-fade slot keeps every mode it has for its 5 ms, and they count: the others share what is left
 * (a stolen voice held one voice's share, so a steal costs each other voice a mode or two, faded, never a cut).
 * Then the queued strikes land (their contact pulses' modes start; impulses, when a pulse was busy, are added). */
static void cy_budget(void)
{
    cy_eng_t *e = &cy_e;
    const cy_object_t *o = e->obj;
    uint32_t want[CY_NALL], nw[CY_NALL], give[CY_NALL], tot = 0, lev, sum, left, i, j, k, fade = 0, bud;
    int idx[CY_NALL], n = 0, a, b;
    for (i = 0; i < CY_NALL; i++) {
        cy_slot_t *sl = &e->sl[i];
        uint32_t pm = 0, el;
        nw[i] = give[i] = want[i] = 0;
        if (!cy_live(sl))
            continue;
        if (CY_IS_FADE(i) || sl->fading) {   /* fading out: keeps what it has, and it counts (not after a PANIC: gone
                                              * within the render, and charged, they left a note played in the same
                                              * render nothing, and its strike was lost) */
            if (!sl->panic)
                fade += cy_popcount(sl->act);
            continue;
        }
        pm = sl->pmask;
        if (sl->pendm)                   /* (impulses queued: only a busy contact's re-strike leaves any) */
            for (k = 0; k < CY_N; k++)
                if (sl->pend[k] != 0.0f)
                    pm |= 1u << k;
        el = cy_eligible(sl);
        /* raining: every eligible mode (a drop lands anywhere); bowed: the modes the bow reaches and whatever still
         * rings (the bow drives no other: allowed the rest of its share, a bowed slot only cost every other voice
         * those modes and refreshed them each sub-block for nothing); struck: what rings and what is about to */
        if (sl->raining)
            want[i] = el;
        else if (sl->bowing || (i == CY_SSURF && e->bow_key >= 0))
            want[i] = CY_BOW_MODEL ? (sl->breach | (sl->act & ~sl->kill) | pm) & el : el;
        else
            want[i] = ((sl->act & ~sl->kill) | pm) & el;
        if (i == CY_SSURF)
            for (k = 0; k < CY_NKEYS; k++)
                if (e->sk_rain[k])
                    want[i] = el;
        nw[i] = cy_popcount(want[i]);
        tot += nw[i];
        idx[n++] = (int)i;
    }
    for (a = 1; a < n; a++)              /* newest first */
        for (b = a; b > 0 && e->sl[idx[b]].age > e->sl[idx[b - 1]].age; b--) {
            int t = idx[b];
            idx[b] = idx[b - 1], idx[b - 1] = t;
        }
    bud = e->budget > fade ? e->budget - fade : 0u;
    if (tot <= bud) {
        for (a = 0; a < n; a++)
            give[idx[a]] = nw[idx[a]];
    } else {
        for (lev = CY_N; lev > 0; lev--) {
            for (sum = 0, a = 0; a < n; a++)
                sum += nw[idx[a]] < lev ? nw[idx[a]] : lev;
            if (sum <= bud)
                break;
        }
        for (sum = 0, a = 0; a < n; a++) {
            give[idx[a]] = nw[idx[a]] < lev ? nw[idx[a]] : lev;
            sum += give[idx[a]];
        }
        left = bud - sum;
        for (a = 0; a < n && left; a++)
            if (nw[idx[a]] > give[idx[a]])
                give[idx[a]]++, left--;
    }
    for (i = 0; i < CY_NALL; i++) {
        cy_slot_t *sl = &e->sl[i];
        uint32_t al = 0, c = 0;
        if (!cy_live(sl))
            continue;
        if (CY_IS_FADE(i) || sl->fading) {   /* fading out anyway: keep what it has */
            sl->allow = sl->act;
            continue;
        }
        for (j = 0; j < CY_N && c < give[i]; j++) {
            k = o->order[j];
            if (want[i] >> k & 1u)
                al |= 1u << k, c++;
        }
        sl->allow = al;
        {   /* modes it loses fade out (CY_KILL_SUB); one allowed again comes back (its gains ramp up from where the
             * fade had them). Then the strikes land where they are allowed. (As masks: this runs for every live slot
             * every render, and the walks over all 32 modes were most of its cost) */
            uint32_t nk = sl->act & ~al & ~sl->kill, back = sl->kill & al, m;
            sl->kill |= nk;
            for (k = 0, m = nk; m; k++, m >>= 1)
                if (m & 1u)
                    sl->kf[k] = 1.0f;
            if (back)
                sl->kill &= ~back, sl->gsig = -1.0f;
            for (k = 0, m = sl->pmask | sl->pendm; m; k++, m >>= 1) {
                uint32_t bit = 1u << k;
                if (!(m & 1u))
                    continue;
                if ((sl->pend[k] != 0.0f || (sl->pmask & bit)) && (al & bit)) {
                    sl->zr[k] += sl->pend[k];
                    if (!(sl->act & bit))
                        sl->fresh |= bit, sl->act |= bit, sl->zi[k] = 0.0f;
                }
                sl->pend[k] = 0.0f;
            }
            sl->pendm = 0;
        }
        if (sl->pmask)
            sl->hp_mask = sl->pmask & al;    /* the modes the contact pulse will drive */
        sl->pmask = 0;
    }
}

/* ---------------------------------------------------------------- exciters --- */
static float cy_unit_open(uint32_t *s) { return ((float)(cy_rand(s) >> 8) + 1.0f) * (1.0f / 16777216.0f); }  /* (0,1] */

/* one RAIN drop into a slot: A_k = vamp (0.15 + 0.85 u^3) phi_k H_k(tau_d) / sqrt(1 + rate/10) (spec §3.6) */
static void cy_drop(cy_slot_t *sl, const float *phi, float rate, float vamp, int vel, float u, float v)
{
    cy_eng_t *e = &cy_e;
    uint32_t k, al = sl->allow;
    float uu = cy_randf(&e->rng), a = vamp * (0.15f + 0.85f * uu * uu * uu) * fm_rsqrtf(1.0f + rate * 0.1f) / sl->trim;
    float tau = (0.15f + 0.45f * cy_randf(&e->rng)) * 1e-3f;
    int joined = sl->hp_n && sl->hp_n == sl->hp_len, pulse = cy_pulse(sl, phi, a, tau);
    if (pulse && !joined)                /* (a contact pulse when the channel is free or not started, else the impulse) */
        sl->hp_mask = 0;
    for (k = 0; k < CY_N; k++) {
        uint32_t bit = 1u << k;
        if (!(al & bit) || phi[k] == 0.0f)
            continue;
        if (pulse)
            sl->hp_mask |= bit;
        else
            sl->zr[k] += a * phi[k] * cy_hertz(sl->w[k] * (CY_FS / 6.28318531f), tau);
        if (!(sl->act & bit))
            sl->fresh |= bit, sl->act |= bit, sl->zi[k] = 0.0f;
    }
    cy_ui_post(CY_UIEV_DROP, vel, u, v);
}

static float cy_rain_rate(float peff) { return 2.0f + 120.0f * cy_e.density * cy_e.density * (0.3f + 0.7f * peff); }

/* TUNED rain: drops due inside this sub-block land at its start (determinism), at a random rain point each */
static void cy_rain_tuned(cy_slot_t *sl, uint32_t n)
{
    cy_eng_t *e = &cy_e;
    const cy_object_t *o = e->obj;
    float rate = cy_rain_rate(sl->Peff);
    uint32_t c = 0;
    while (sl->rain_cd < (float)n && c++ < 4u) {
        uint32_t i = cy_rand(&e->rng) >> 27;     /* 32 points */
        cy_drop(sl, o->rain[i], rate, sl->vamp, sl->vel, o->rain_uv[i][0], o->rain_uv[i][1]);
        sl->rain_cd += -fm_logf(cy_unit_open(&e->rng)) / rate * CY_FS;
    }
    if (sl->rain_cd < (float)n)
        sl->rain_cd = (float)n;
    sl->rain_cd -= (float)n;
}

/* SURFACE rain: each raining key drops around its own point, jittered +-0.03 (the field read at the jittered
 * point from geom's field cache). The cache is the one piece of geom state that is not double-buffered: the main
 * loop lerps it in place on every SHAPE change, before it publishes the object, and this render may have
 * interrupted that; then (or while the cache is ahead of the object this render plays) a drop takes its key
 * point's own field from the published object instead, unjittered. The random draws are the same either way. */
static void cy_rain_surface(cy_slot_t *sl, uint32_t n, const float *peff)
{
    cy_eng_t *e = &cy_e;
    const cy_object_t *o = e->obj;
    const cy_fieldcache_t *fc = cy_geom_fields();
    int jit = cy_geom_fields_stable();
    float phi[CY_N];
    uint32_t j, k;
    for (j = 0; j < CY_NKEYS; j++) {
        float rate;
        uint32_t c = 0;
        if (!e->sk_rain[j])
            continue;
        rate = cy_rain_rate(peff[j]);
        while (e->sk_rcd[j] < (float)n && c++ < 4u) {
            float u = o->key_uv[j][0] + 0.03f * cy_rand_pm1(&e->rng), v = o->key_uv[j][1] + 0.03f * cy_rand_pm1(&e->rng);
            for (k = 0; k < CY_N; k++)
                phi[k] = !(sl->allow >> k & 1u) ? 0.0f : jit ? cy_geom_field_at(fc, (int)k, u, v) : o->surface[j][k];
            cy_drop(sl, phi, rate, fm_powf((float)e->sk_vel[j] * (1.0f / 127.0f), 1.4f), e->sk_vel[j], u, v);
            e->sk_rcd[j] += -fm_logf(cy_unit_open(&e->rng)) / rate * CY_FS;
        }
        if (e->sk_rcd[j] < (float)n)
            e->sk_rcd[j] = (float)n;
        e->sk_rcd[j] -= (float)n;
    }
}

/* the contact, as x[] into the struck modes (b_k = phi_k): the force pulse (cy_pulse) plus the contact noise, white
 * -> one-pole HP at 2 kHz -> Hann window, both through a binomial band-limiter (1 + z^-1)^4 / 16 before they reach
 * the modes. A mode passes about b sin(w) / 4 of its input straight through far above its resonance, so whatever
 * the contact holds near Nyquist would be heard there; the band-limiter's fourth-order zero at Nyquist takes the
 * contact 65 dB down at 0.45 fs (-1 dB at 6 kHz, -2.4 dB at 10 kHz: the hardness, and the mode amplitudes the
 * pulse leaves, barely move). Returns the modes it drives: the pulse's while it lasts (and its tail), and for the
 * noise only those above 700 Hz (the high-pass leaves the ones below at -10 dB and less, under their strike by far,
 * so they stay on the FREE kernel: the onset render of a big chord is the engine's costliest). */
static uint32_t cy_contact_fill(cy_slot_t *sl, float *x, uint32_t n)
{
    cy_eng_t *e = &cy_e;
    const float a = 0.75201f;            /* exp(-2 pi 2000 / 44100) */
    uint32_t i, k, m = 0, drv = (sl->hp_n || sl->ct_tail) ? sl->hp_mask : 0u, noise = sl->nz_n != 0u;
    uint32_t in = sl->hp_n || sl->nz_n, np = sl->hp_n < n ? sl->hp_n : n;
    float h0 = sl->ct_h[0], h1 = sl->ct_h[1], h2 = sl->ct_h[2], h3 = sl->ct_h[3];
    /* (the counters and the RNG live in registers here: as struct fields of the same type as cy_e.rng they were
     * reloaded and stored every sample. A chord's onset render runs this for every struck slot in every sub-block,
     * and it cost more there than the strike's kernels) */
    if (np) {
        float c = sl->hp_c, s = sl->hp_s, dc = sl->hp_dc, ds = sl->hp_ds, amp = sl->hp_amp;
        for (i = 0; i < np; i++) {
            float t = c * dc - s * ds;
            x[i] = amp * s;
            s = s * dc + c * ds;
            c = t;
        }
        sl->hp_c = c, sl->hp_s = s, sl->hp_n -= np;
    }
    for (i = np; i < n; i++)
        x[i] = 0.0f;
    if (noise) {
        float x1 = sl->nz_x1, y1 = sl->nz_y1, ec = sl->nz_ec, es = sl->nz_es, sc, ss, amp = sl->nz_amp;
        float f0 = sl->nz_f[0], f1 = sl->nz_f[1];
        uint32_t nn = sl->nz_n < n ? sl->nz_n : n, rng = e->rng;
        float yb[CY_CR];                 /* (two passes, the same arithmetic in the same order: one loop spilled) */
        cy_sincos(6.28318531f / (float)(sl->nz_len ? sl->nz_len : 1u), &sc, &ss);
        for (i = 0; i < nn; i++) {       /* white noise (cy_rand_pm1), one-pole high-pass */
            float w;
            rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5;
            w = (float)(int32_t)rng * (1.0f / 2147483648.0f);
            yb[i] = y1 = a * (y1 + w - x1);
            x1 = w;
        }
        e->rng = rng;
        for (i = 0; i < nn; i++) {       /* Hann window, then the noise's own (1 + z^-1)^2 / 4: white noise holds the
                                          * most near Nyquist; with the common band-limiter only, -97 dB at 0.45 fs */
            float t, v = amp * (0.5f - 0.5f * ec) * yb[i];
            x[i] += 0.25f * (v + f1) + 0.5f * f0;
            f1 = f0, f0 = v;
            t = ec * sc - es * ss;
            es = es * sc + ec * ss;
            ec = t;
        }
        sl->nz_n -= nn;
        sl->nz_x1 = x1, sl->nz_y1 = y1, sl->nz_ec = ec, sl->nz_es = es;
        sl->nz_f[0] = sl->nz_n ? f0 : 0.0f, sl->nz_f[1] = sl->nz_n ? f1 : 0.0f;
    }
    for (i = 0; i < n; i++) {            /* (1 + z^-1)^4 / 16: 1 4 6 4 1 */
        float v = x[i];
        x[i] = 0.0625f * (v + h3) + 0.25f * (h0 + h2) + 0.375f * h1;
        h3 = h2, h2 = h1, h1 = h0, h0 = v;
    }
    sl->ct_h[0] = h0, sl->ct_h[1] = h1, sl->ct_h[2] = h2, sl->ct_h[3] = h3;
    if (in)                              /* the band-limiter rings on 4 samples after its input stops: one more */
        sl->ct_tail = 1u;                /* block drains it (whatever sample the input stopped on) */
    else
        sl->ct_tail = 0u, sl->ct_h[0] = sl->ct_h[1] = sl->ct_h[2] = sl->ct_h[3] = 0.0f;
    if (noise)
        for (k = 0; k < CY_N; k++)
            if (sl->w[k] >= 700.0f * CY_TWO_PI_FS)
                m |= 1u << k;
    m = (m | drv) & sl->act;
    for (k = 0, i = m; i; k++, i >>= 1)  /* (b_k = phi_k for the modes it drives: the EXCITED kernel reads no other) */
        if (i & 1u)
            sl->b[k] = sl->phi[k];
    return m;
}

/* ------------------------------------------------------------------ render --- */
static void cy_pickup_patch(void)
{
    cy_eng_t *e = &cy_e;
    uint32_t s = cy_q.pseq, i;
    int first;
    if (s == e->pseq)
        return;
    e->pseq = s;
    CY_BARRIER();
    e->p = cy_q.pbuf[cy_q.pfront];
    first = !e->inited;
    if (first) {                         /* the first patch is taken as is: nothing glides in from zero */
        e->inited = 1;
        e->decay = e->p.decay_mul, e->tension = e->p.tension_st, e->palm = e->p.palm, e->force = e->p.force;
        e->grit = e->p.grit, e->density = e->p.density, e->noise = e->p.noise, e->damper = e->p.damper;
        e->bloom = e->p.bloom, e->stretch = e->p.stretch, e->vib = e->p.vibrato_cents;
        e->lev = fm_db2lin(e->p.level_db);
        e->gpitch = fm_exp2f((cy_q.bend + e->p.tune_cents * 0.01f) * (1.0f / 12.0f));
        e->last_exc = e->p.exciter;
        e->onorm = e->obj->out_norm;
    }
    if (e->p.exciter != e->last_exc) {   /* ALGORITHM turned: bowed notes stop bowing (the bow lifts through its
                                          * gate), rain stops, all ring out */
        for (i = 0; i < CY_NALL; i++) {
            cy_slot_t *sl = &e->sl[i];
            if (sl->bowing || sl->raining) {
                sl->raining = 0;
                sl->xt = e->p.exciter;
                if (sl->state != CY_VS_FROZEN && !sl->fading)
                    sl->state = CY_VS_RING;
            }
        }
        for (i = 0; i < CY_NKEYS; i++)
            e->sk_rain[i] = 0;
        e->bow_key = -1;
        e->last_exc = e->p.exciter;
    }
    if (first || cy_fxp.drive_db != e->p.drive_db || cy_fxp.tilt_db != e->p.tilt_db || cy_fxp.room != e->p.room ||
        cy_fxp.size_s != e->p.size_s || cy_fxp.damping != e->p.damping || cy_fxp.predelay_ms != e->p.predelay_ms) {
        cy_fxp.drive_db = e->p.drive_db, cy_fxp.tilt_db = e->p.tilt_db, cy_fxp.room = e->p.room;
        cy_fxp.size_s = e->p.size_s, cy_fxp.damping = e->p.damping, cy_fxp.predelay_ms = e->p.predelay_ms;
        cy_fx_set(&cy_fxp);              /* (from the render: fx never sees a half-written set) */
    }
}

/* the patch's continuous values move toward their targets one step a sub-block (spec §2.4.7). (v1.1: CY_HOT, in RAM
 * and call-free, with fastmath's helpers as their verbatim always-inlined copies (cy_engine_int.h): the same
 * results, the host hash unchanged. It runs four times a render, and from the XIP flash its code missed the cache) */
CY_HOT static void cy_smooth(void)
{
    cy_eng_t *e = &cy_e;
    const cy_patch_t *p = &e->p;
    const float k = CY_SMOOTH;
    float gp = cy_i_exp2f((cy_q.bend + p->tune_cents * 0.01f) * (1.0f / 12.0f));
    e->decay += k * (p->decay_mul - e->decay);
    e->tension += k * (p->tension_st - e->tension);
    e->palm += k * (p->palm - e->palm);
    e->force += k * (p->force - e->force);
    e->grit += k * (p->grit - e->grit);
    e->density += k * (p->density - e->density);
    e->noise += k * (p->noise - e->noise);
    e->damper += k * (p->damper - e->damper);
    e->bloom += k * (p->bloom - e->bloom);
    if (cy_i_fabsf(e->bloom) < 1e-5f)
        e->bloom = 0.0f;
    e->stretch += k * (p->stretch - e->stretch);
    if (e->stretch < 1e-5f)
        e->stretch = 0.0f;
    e->vib += k * (p->vibrato_cents - e->vib);
    if (e->vib < 1e-4f)
        e->vib = 0.0f;
    e->lev += k * (cy_i_exp2f(p->level_db * 0.166096405f) - e->lev);   /* (fm_db2lin, written out) */
    e->onorm += 0.15f * (e->obj->out_norm - e->onorm);
    if (cy_i_fabsf(e->onorm - e->obj->out_norm) < 1e-7f * e->obj->out_norm)
        e->onorm = e->obj->out_norm;
    e->gpitch += 0.15f * (gp - e->gpitch);   /* bend / tune: ~5 ms */
    if (cy_i_fabsf(e->gpitch - gp) < 1e-7f)
        e->gpitch = gp;
}

/* (v1.1) a bowed slot's band-limiter, (1 + z^-1)^4 / 16 over its whole output (see the render), from the scratch
 * t[] into the bus, zeroing t[] as it reads it so the next bowed slot finds it clear. CY_HOT (RAM, call-free): before
 * v1.1 this loop and a separate clear of the scratch ran in the render, from the flash, and with four bows they were
 * ~60 % of the render's own instructions there (fm1-emu profile, 21 AETHER). The same operations in the same order
 * (bit-exact), four samples a turn so the history never shuffles through registers */
CY_HOT static void cy_blim_block(float *h, float *tl, float *tr, float *oL, float *oR, uint32_t m)
{
    uint32_t c2, k;
    for (c2 = 0; c2 < 2u; c2++) {
        float *hh = h + 4u * c2, *t = c2 ? tr : tl, *out = c2 ? oR : oL;
        float h0 = hh[0], h1 = hh[1], h2 = hh[2], h3 = hh[3];
        for (k = 0; k + 4u <= m; k += 4u) {
            float v0 = t[k], v1 = t[k + 1u], v2 = t[k + 2u], v3 = t[k + 3u];
            t[k] = t[k + 1u] = t[k + 2u] = t[k + 3u] = 0.0f;
            out[k] += 0.0625f * (v0 + h3) + 0.25f * (h0 + h2) + 0.375f * h1;
            out[k + 1u] += 0.0625f * (v1 + h2) + 0.25f * (v0 + h1) + 0.375f * h0;
            out[k + 2u] += 0.0625f * (v2 + h1) + 0.25f * (v1 + h0) + 0.375f * v0;
            out[k + 3u] += 0.0625f * (v3 + h0) + 0.25f * (v2 + v0) + 0.375f * v1;
            h3 = v0, h2 = v1, h1 = v2, h0 = v3;
        }
        for (; k < m; k++) {
            float v = t[k];
            t[k] = 0.0f;
            out[k] += 0.0625f * (v + h3) + 0.25f * (h0 + h2) + 0.375f * h1;
            h3 = h2, h2 = h1, h1 = h0, h0 = v;
        }
        hh[0] = h0, hh[1] = h1, hh[2] = h2, hh[3] = h3;
    }
}

/* (v1.1) the telemetry's mode energies: one live slot's, weighted (CY_HOT: the same sums, in RAM) */
CY_HOT static void cy_energy_add(float *dst, const float *src, float a)
{
    uint32_t k;
    for (k = 0; k < CY_N; k++)
        dst[k] += src[k] * a;
}

/* SURFACE keys at control rate: smoothed pressure, P_eff, the bow point, the point damper (spec §3.7, 3.4) */
static void cy_surface_ctl(uint32_t n, float *peff)
{
    cy_eng_t *e = &cy_e;
    const cy_object_t *o = e->obj;
    cy_slot_t *sl = &e->sl[CY_SSURF];
    float wsum = 0.0f, d[CY_N], best = -1.0f;
    uint32_t j, k, ch = 0;
    int bow = e->p.exciter == CY_EXC_BOW, bk = -1;
    for (k = 0; k < CY_N; k++)
        d[k] = 0.0f;
    for (j = 0; j < CY_NKEYS; j++) {
        float sw, w, vs;
        peff[j] = 0.0f;
        if (!e->sk_down[j])
            continue;
        if (!e->sk_lhold[j]) {           /* (latched: the last pressure stays, as a latched TUNED bow keeps its own) */
            uint32_t pk = cy_q.press_key[j], ch = e->sk_midi[j] ? cy_q.chan : 0u;   /* (channel AT: MIDI points) */
            e->sk_P[j] += 0.047f * ((float)(pk > ch ? pk : ch) * (1.0f / 127.0f) - e->sk_P[j]);
            if (e->sk_P[j] < 1e-6f)
                e->sk_P[j] = 0.0f;       /* (as sl->P: the one-pole would otherwise settle on a subnormal) */
        }
        sw = 1.0f - fm_expf(-(float)e->sk_t[j] * (1.0f / CY_FS) / fm_maxf(e->p.swell_s, 0.001f));
        if (e->sk_t[j] < 0x7FFFFFFFu)
            e->sk_t[j] += n;
        vs = (float)e->sk_vel[j] * (1.0f / 127.0f) * sw;   /* P_eff (spec §3.6; voice.c cy_slot_ctl the same) */
        peff[j] = e->sk_at[j] ? fm_clampf(0.4f * (float)e->sk_vel[j] * (1.0f / 127.0f) * sw + e->sk_P[j], 0.0f, 1.0f)
                              : vs + (1.0f - vs) * e->sk_P[j];
        if (e->sk_touch[j])
            w = fm_maxf(e->sk_P[j], CY_TOUCH_FLOOR) * CY_FINGER_K;
        else
            w = bow ? 0.0f : e->sk_P[j] * CY_TOUCH_K;
        if (bow && !e->sk_touch[j] && peff[j] > best && sl->bowing)
            best = peff[j], bk = (int)j;
        if (w > 1e-4f) {
            const float *s = o->surface[j];
            for (k = 0; k < CY_N; k++)
                d[k] += w * s[k] * s[k];
            wsum += w;
        }
    }
    for (k = 0; k < CY_N; k++)
        if (fm_fabsf(d[k] - e->dsurf[k]) > 1e-3f * d[k] + 1e-4f) {
            ch = 1;
            break;
        }
    if (ch || (wsum == 0.0f && e->dsurf[0] + e->dsurf[CY_N - 1] != 0.0f)) {
        for (k = 0; k < CY_N; k++)
            e->dsurf[k] = d[k];
        e->dsurf_ver++;
    }
    if (sl->bowing) {
        e->bow_key = (int8_t)bk;
        sl->bhold = bk >= 0;
        if (bk >= 0) {
            sl->Peff = peff[bk];
            sl->vn = (float)e->sk_vel[bk] * (1.0f / 127.0f);
        }
    }
    sl->raining = 0;
    for (j = 0; j < CY_NKEYS; j++)
        if (e->sk_rain[j])
            sl->raining = 1;
    /* the slot's state follows its keys */
    if (sl->state != CY_VS_FROZEN && !sl->fading) {
        int held = 0;
        for (j = 0; j < CY_NKEYS; j++)
            held |= e->sk_down[j] && !e->sk_touch[j];
        sl->down = (uint8_t)held;
        sl->state = sl->bowing && sl->bhold ? CY_VS_BOWED : held ? CY_VS_HELD : CY_VS_RING;
    }
}

/* SURFACE telemetry: key_level[j] = sum_k e_k surface[j][k]^2 / max over j, the vibration ENERGY under each key.
 * The spec writes the square root (the amplitude), but a struck plate rings in dozens of modes at once and their
 * incoherent sum leaves no key below ~0.5 of the brightest, so no LED ever reaches the dark thresholds the SURFACE
 * design is about (measured); the energy keeps the same order and brings the nodal keys under them as soon as the
 * figure is there (a touch, or the high modes dying). Faded to dark from 40 to 60 dB under a vel-100 strike's
 * level, so the keybed goes dark with the plate rather than holding a full map of an inaudible tail. */
static void cy_key_levels_surface(void)
{
    cy_eng_t *e = &cy_e;
    const cy_object_t *o = e->obj;
    const cy_slot_t *sl = &e->sl[CY_SSURF];
    float kl[CY_NKEYS], mx = 0.0f, g, db;
    uint32_t j, k;
    for (j = 0; j < CY_NKEYS; j++) {
        float a = 0.0f;
        const float *s = o->surface[j];
        for (k = 0; k < CY_N; k++)
            a += sl->e[k] * s[k] * s[k];
        kl[j] = a;
        mx = fm_maxf(mx, kl[j]);
    }
    db = 6.0206f * fm_log2f(fm_maxf(sl->lvl, 1e-12f) * (1.0f / (0.12f * CY_OUT_TRIM)));   /* 20 log10 */
    g = fm_clampf((db + 60.0f) * 0.05f, 0.0f, 1.0f);
    for (j = 0; j < CY_NKEYS; j++)
        cy_tel.key_level[j] = mx > 0.0f ? kl[j] / mx * g : 0.0f;
}

static void cy_publish(uint32_t n)
{
    cy_eng_t *e = &cy_e;
    const cy_object_t *o = e->obj;
    cy_fx_meter_t m;
    uint32_t i, k, used = 0, newest = 0;
    float bf = 0.0f, nh = e->newest_hz;
    cy_fx_meter(&m);
    CY_TEL_SEQ++;                        /* odd: a reader that sees this retries */
    CY_BARRIER();
    for (k = 0; k < CY_N; k++)
        cy_tel.mode_energy[k] = 0.0f;
    for (i = 0; i < CY_NALL; i++) {
        const cy_slot_t *sl = &e->sl[i];
        float a = e->onorm * e->lev * CY_OUT_TRIM * sl->trim * (sl->fading ? sl->fade_g : 1.0f);
        if (!cy_live(sl))
            continue;
        a *= a;
        CY_FAR(cy_energy_add)(cy_tel.mode_energy, sl->e, a);
        used += cy_popcount(sl->act);
        if (sl->bowing)
            bf = fm_maxf(bf, sl->bFb * (1.0f / 0.9f));
        if (!sl->fading && sl->age >= newest && (sl->act || sl->bowing)) {
            newest = sl->age;
            nh = sl->fnote * sl->rc[o->order[0]] * sl->M;
        }
    }
    for (i = 0; i < CY_NVOICES; i++) {
        const cy_slot_t *sl = &e->sl[i];
        cy_tel.voice_level[i] = sl->lvl;
        cy_tel.voice_press[i] = (uint8_t)(sl->P * 127.0f + 0.5f);
        cy_tel.voice_key[i] = sl->state == CY_VS_FREE ? -1 : sl->key;
        cy_tel.voice_state[i] = sl->state;
        cy_tel.voice_note[i] = sl->note;
    }
    if (e->p.surface) {
        cy_key_levels_surface();
    } else {                             /* TUNED: each key's newest slot, relative to its own peak (-50 dB = dim) */
        for (k = 0; k < CY_NKEYS; k++)
            cy_tel.key_level[k] = 0.0f;
        for (i = 0; i < CY_NVOICES; i++) {
            const cy_slot_t *sl = &e->sl[i];
            if (sl->state != CY_VS_FREE && sl->key >= 0 && sl->lvl_pk > 0.0f)
                cy_tel.key_level[(int)sl->key] = fm_maxf(cy_tel.key_level[(int)sl->key], sl->lvl / sl->lvl_pk);
        }
    }
    e->newest_hz = nh;
    cy_tel.newest_hz = nh;
    cy_tel.peak_l = m.peak_l, cy_tel.peak_r = m.peak_r, cy_tel.gr_db = m.gr_db;
    cy_tel.bow_force = bf;
    /* the CPU model (spec §3.13, 3.14): the engine's sums plus the fixed chain, per output sample */
    cy_tel.est_instr = e->est / (float)n + 12.0f + 320.0f + 55.0f + 12.0f + 10.0f + (e->p.drive_db >= 0.1f ? 190.0f : 0.0f);
    cy_tel.modes_used = (uint16_t)used;
    cy_tel.mode_budget = e->budget;
    cy_tel.nan_resets = (uint16_t)(e->nan_resets + m.nan_resets);
    cy_tel.frozen = e->frozen, cy_tel.surface = e->p.surface, cy_tel.latched = e->latched;
    cy_tel.damping = e->damping;
    CY_BARRIER();
    CY_TEL_SEQ++;
}

void cy_engine_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    cy_eng_t *e = &cy_e;
    const cy_object_t *o;
    float peff[CY_NKEYS];
    uint32_t off, i, k, r;
    int surf_live;
    if (n > CY_BLOCK)
        n = CY_BLOCK;
    e->rnd++;
    e->est = 0.0f;
    o = e->obj = cy_geom_object();
    if (!e->restart && cy_q.rs_push == cy_q.rs_take)   /* (v1.1.1: a restart, queued or fading, keeps the patch in
                                                        * force when it was queued; it takes the next as a first
                                                        * one, below) */
        cy_pickup_patch();
    {
        int32_t b = cy_q.budget;
        e->budget = (uint16_t)(b < CY_BUDGET_MIN ? CY_BUDGET_MIN : b > CY_BUDGET_MAX ? CY_BUDGET_MAX : b);
        b = cy_q.bow_cap;                /* (v1.1: the device governor's bow cap, CY_MAX_BOWED elsewhere) */
        e->bow_cap = (uint8_t)(b < 1 ? 1 : b > CY_MAX_BOWED ? CY_MAX_BOWED : b);
        b = cy_q.tail_cap;
        e->tail_cap = (uint8_t)(b < 0 ? 0 : b > (int)CY_NVOICES ? (int)CY_NVOICES : b);
    }
    if (o->seq != e->obj_seq) {          /* the object moved: every live slot glides to it (spec §3.3) */
        e->obj_seq = o->seq;
        for (i = 0; i < CY_NALL; i++)
            if (cy_live(&e->sl[i]))
                e->sl[i].glide = 1, e->sl[i].dirty = 1;
    }
    if (e->ev_hold)                      /* (v1.1: events held behind a tails fade, CY_EV_TAILS, this many renders
                                          * more; never on the host or the web, where nothing sends it) */
        e->ev_hold--;
    if (e->restart == 1u && e->rs_n >= CY_RESTART_FADE * CY_BLOCK) {   /* (v1.1.1) a restart's fade done: from cold */
        int hand = e->rs_hand;
        cy_restart_now(!hand);
        if (hand) {                      /* a hand-over: silent until the main loop's next patch (its geometry first) */
            e->restart = 2;
            e->rs_pseq = cy_q.pseq;
            CY_BARRIER();
            cy_q.rs_ready = 1;
        }
    }
    if (e->restart == 2u && cy_q.pseq != e->rs_pseq) {   /* ... and here it is: taken as a first, the room snapped to it
                                                          * too, and the events waiting behind the restart go now */
        cy_q.rs_ready = 0;
        e->restart = 0;
        e->pseq = cy_q.pseq - 1u;
        cy_pickup_patch();
        cy_fx_snap();                    /* (the room is silent since cy_restart_now's clear: its targets at once) */
        e->obj_seq = o->seq;
    }
    for (r = cy_q.ev_r; r != cy_q.ev_w && !e->ev_hold && !e->restart; r++) {   /* the main loop's events, in order,
                                                                             * at this sub-block edge (v1.1.1: none
                                                                             * behind a restart) */
        CY_BARRIER();
        cy_apply(&cy_q.ev[r & (CY_EVQ - 1u)]);
    }
    CY_BARRIER();
    cy_q.ev_r = r;
    /* pressure into the TUNED slots: poly by key or by note, and channel pressure (and CC1) on every held MIDI note.
     * Not on the FM-1's own keys: a MIDI stream on the omni input changed how the panel played (a CC1 of 127 bent
     * every key-played SINGING BOWL note +81 cents) and overrode its own poly pressure upward */
    for (i = 0; i < CY_NVOICES; i++) {
        cy_slot_t *sl = &e->sl[i];
        if (sl->down) {
            uint32_t pk = sl->key >= 0 ? cy_q.press_key[(int)sl->key] : cy_q.press_note[sl->note];
            uint32_t ch = sl->key < 0 ? cy_q.chan : 0u;
            sl->Praw = (float)(pk > ch ? pk : ch) * (1.0f / 127.0f);
        } else if (!sl->lhold) {
            sl->Praw = 0.0f;             /* (latched: the last pressure stays) */
        }
        sl->bhold = (sl->down || sl->lhold) && sl->state != CY_VS_FROZEN && sl->xt == CY_EXC_BOW;
    }
    {   /* the bow cap lowered under held bowed notes (cy_engine_set_bow_cap, v1.1; the device's governor only): the
         * oldest fade out in place over CY_GOV_FADE_S, their bows lifted at once (as a PANIC fades, but slower). A bow
         * costs its junction whatever the mode budget (~13 % of a block in fm1-emu, ~18-21 % on the chip): at the
         * budget's floor four bows were the v1.0 governor's floor, and on the FM-1 they did not fit (21 AETHER chords
         * crackled). New notes past the cap take over the oldest in cy_alloc. With the caps at CY_MAX_BOWED and
         * CY_NVOICES (the host, the browser) cy_alloc keeps the counts at or under them, so nothing here ever runs */
        uint32_t cap = e->bow_cap;
        for (;;) {
            int ob = -1;
            uint32_t nb = 0;
            for (i = 0; i < CY_NVOICES; i++) {
                const cy_slot_t *sl = &e->sl[i];
                if (sl->bowing && !sl->fading && (sl->down || sl->lhold) && sl->state != CY_VS_FROZEN) {
                    nb++;
                    if (ob < 0 || sl->age < e->sl[ob].age)
                        ob = (int)i;
                }
            }
            if (nb <= cap || ob < 0)
                break;
            cy_gov_fade(&e->sl[ob], CY_GOV_FADE_S);
        }
        /* and the released notes still ringing past the tail cap fade the same way, the quietest first (a long
         * AETHER tail costs a third of a bow, and every live slot glides with the plate's morph) */
        cap = e->tail_cap;
        for (;;) {
            int q = -1;
            uint32_t nt = 0;
            for (i = 0; i < CY_NVOICES; i++) {
                const cy_slot_t *sl = &e->sl[i];
                if (sl->state == CY_VS_RING && !sl->fading && !sl->down && !sl->lhold) {
                    nt++;
                    if (q < 0 || sl->lvl < e->sl[q].lvl)
                        q = (int)i;
                }
            }
            if (nt <= cap || q < 0)
                break;
            cy_gov_fade(&e->sl[q], CY_GOV_FADE_S);
        }
    }
    cy_budget();
    {   /* the bow's coupled-mode share (bow.c, CY_BOW_SHARE): the budget's half over the slots bowing now */
        uint32_t nb = 0, cap;
        for (i = 0; i < CY_NALL; i++)
            nb += e->sl[i].bowing != 0;
        cap = nb ? (uint32_t)e->budget / (CY_BOW_SHARE * nb) : CY_N;
        e->bow_ccap = (uint8_t)(cap < CY_BOW_CMIN ? CY_BOW_CMIN : cap > CY_N ? CY_N : cap);
    }
    for (i = 0; i < n; i++)
        e->bus_l[i] = e->bus_r[i] = 0.0f;
    surf_live = cy_live(&e->sl[CY_SSURF]);
    for (off = 0; off < n; off += CY_CR) {
        uint32_t m = n - off < CY_CR ? n - off : CY_CR, any_rain = 0;
        float *L = e->bus_l + off, *R = e->bus_r + off;
        CY_FAR(cy_smooth)();
        e->vib_ph += e->p.vib_rate_hz * (float)m * (1.0f / CY_FS);
        if (e->vib_ph >= 1.0f)
            e->vib_ph -= 1.0f;
        if (surf_live)
            cy_surface_ctl(m, peff);
        for (i = 0; i < CY_NALL; i++) {
            cy_slot_t *sl = &e->sl[i];
            const float *x = e->x;
            if (!cy_live(sl))
                continue;
            CY_FAR(cy_slot_ctl)(sl, (int)i, m);
            if (i == CY_SSURF && sl->bowing && e->bow_key >= 0)
                sl->Peff = peff[(int)e->bow_key];   /* (the slot's own P_eff follows its bow key) */
            sl->exc = sl->cpl = 0;
            if (sl->bowing) {
                const float *phi = i == CY_SSURF ? (e->bow_key >= 0 ? o->surface[(int)e->bow_key] : sl->bphi) : o->strike;
                CY_FAR(cy_bow_prep)(sl, phi, m);
#if !CY_BOW_MODEL
                cy_breath_fill(sl, e->x, m);
                for (k = 0; k < CY_N; k++)
                    sl->b[k] = sl->bphi[k];
                sl->exc = sl->act;
#endif
            } else if (sl->raining) {
                if (i == CY_SSURF)
                    cy_rain_surface(sl, m, peff);
                else
                    cy_rain_tuned(sl, m);
                any_rain = 1;
            }
            if ((sl->nz_n || sl->hp_n || sl->ct_tail) && !sl->bowing)
                sl->exc = cy_contact_fill(sl, e->x, m);
            sl->run = sl->act & ~sl->cpl;
            CY_FAR(cy_slot_gains)(sl, m);
            if (sl->blim)                /* a bowed slot renders apart, then through its band-limiter (below), into
                                          * the scratch, which is always clear here: zeroed at init and by
                                          * cy_blim_block as it reads it (v1.1: the render's own clear went) */
                L = e->tl, R = e->tr;
#if CY_BOW_MODEL
            if (sl->bowing) {
                uint32_t ev0 = sl->bevals;
                CY_FAR(cy_bow_block)(sl, L, R, m);
                /* the CPU model for the junction: ~26 instr per coupled mode-sample (sample-major: rotate, v_h,
                 * inject, out, loads and stores), ~36 per junction evaluation (mu, mu', a divide or two), ~30 a
                 * sample fixed (spec §3.14 budgets "+180 per bowed slot" for this). (The device build measures 29
                 * per static coupled mode-sample, ~35 moving, ~74 per Newton iteration and ~150 a sample fixed: the
                 * model under-reads the bow about 1.5x; it is not recalibrated here because est_instr also feeds the
                 * golden 2,900 / 3,500 gates and the host's UI pacing, which would move with it: see fix-dsp dsp-3) */
                e->est += (float)m * (30.0f + 26.0f * (float)sl->bnc) + 36.0f * (float)(sl->bevals - ev0);
                if (sl->bslips && e->rnd - sl->bslip_t > 22u) {   /* a SLIP event at most every ~64 ms */
                    float u = o->pos_uv[0], v = o->pos_uv[1];
                    if (i == CY_SSURF && e->bow_key >= 0)
                        u = o->key_uv[(int)e->bow_key][0], v = o->key_uv[(int)e->bow_key][1];
                    cy_ui_post(CY_UIEV_SLIP, (int)(sl->bFb * 141.0f), u, v);
                    sl->bslip_t = e->rnd;
                }
                sl->bslips = 0;
            }
#endif
            if (!sl->exc)
                x = 0;
            CY_FAR(cy_modes_block)(sl, L, R, x, m);
            if (sl->blim) {              /* (1 + z^-1)^4 / 16 on the slot's whole output, for its whole life: the
                                          * bow's stick-slip force has corners, and a mode passes some of whatever
                                          * it is driven with straight through, far above its resonance; this takes
                                          * that 65 dB down at 0.45 fs (-2.4 dB at 10 kHz) and never switches path
                                          * mid-note (the slot keeps it until it is free) */
                float *oL = e->bus_l + off, *oR = e->bus_r + off;
                CY_FAR(cy_blim_block)(&sl->bl_h[0][0], e->tl, e->tr, oL, oR, m);   /* (v1.1: in RAM) */
                L = oL, R = oR;
                e->est += 25.0f * (float)m;
            }
            {   /* the CPU model: mode-samples by kernel, spec §3.14 (the bow's coupled modes are counted above);
                 * a gliding mode costs two more */
                uint32_t rr = sl->ramp | sl->glm, ex = cy_popcount(sl->exc & sl->run);
                uint32_t rp = cy_popcount(rr & ~sl->exc & sl->run), gm = cy_popcount(sl->glm & sl->run);
                uint32_t fr = cy_popcount(sl->run) - cy_popcount(sl->run & (sl->exc | rr));
                e->est += (float)m * (15.0f * (float)fr + 17.0f * (float)rp + 18.0f * (float)ex + 2.0f * (float)gm);
            }
            CY_FAR(cy_slot_post)(sl, m);
            if (sl->fading && sl->fade_g <= 0.0f)
                cy_slot_clear(sl);           /* faded out: the slot is free (state 0 = CY_VS_FREE) */
            /* a bowed slot whose bow has lifted (its gate under 1 %, ~70 ms) becomes an ordinary ringing slot */
            if (sl->bowing && !sl->bhold && sl->bgate < 0.01f) {
                sl->bowing = 0;
                sl->bF = 0.0f;
                if (sl->state == CY_VS_BOWED)
                    sl->state = CY_VS_RING;
            }
            if (!sl->fading && sl->state != CY_VS_FREE && !sl->act && !sl->down && !sl->lhold && !sl->bowing &&
                !sl->raining && sl->nz_n == 0 && sl->hp_n == 0 && sl->ct_tail == 0) {
                int keys = 0;
                if (i == CY_SSURF)
                    for (k = 0; k < CY_NKEYS; k++)
                        keys |= e->sk_down[k];
                if (!keys)
                    sl->state = CY_VS_FREE;
            }
        }
        if (any_rain)
            e->est += 5.0f * (float)m;
        if (surf_live)
            e->est += 34.0f * (float)m;
    }
    cy_fx_process(e->bus_l, e->bus_r, n, master_q12, lr);
    if (e->restart == 1u) {              /* (v1.1.1) a restart's fade: a raised cosine to 0 over CY_RESTART_FADE
                                          * renders, on all that is heard (the room's tail too), so nothing clicks */
        const float k = 3.14159265f / (float)(CY_RESTART_FADE * CY_BLOCK);
        for (i = 0; i < n; i++) {
            uint32_t j = e->rs_n + i + 1u;
            float g = j >= CY_RESTART_FADE * CY_BLOCK ? 0.0f : 0.5f + 0.5f * fm_cosf(k * (float)j);
            lr[2u * i] = (int32_t)((float)lr[2u * i] * g);
            lr[2u * i + 1u] = (int32_t)((float)lr[2u * i + 1u] * g);
        }
        e->rs_n += n;
    }
    cy_publish(n);
}
