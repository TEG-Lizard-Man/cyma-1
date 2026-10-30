/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 engine internals shared by engine.c (API, events, voices, exciters, telemetry), voice.c (the mode kernels and
 * per-slot control: frequencies, damping, gains, cull) and bow.c (the friction junction and the BREATH fallback).
 * Not a frozen interface: M2 owns it. Spec docs/CYMA-1-spec.md §3.3-3.9, 3.12-3.14.
 *
 * Threading. The main loop calls the cy_engine_* setters; the audio interrupt calls cy_engine_render, which preempts
 * the main loop and always finishes before the main loop runs again. So everything the main loop hands over goes
 * through one of three single-writer paths, and the render picks all of it up at its start (a sub-block edge, which
 * is what makes host == wasm == device hashes possible):
 *   - an SPSC event queue (notes, freeze, latch, damp, panic, seed), in order;
 *   - the patch in two buffers and a front index (the render copies the front one);
 *   - last-value cells for things that only ever need their newest value (pressures, bend, budget).
 * The render hands back telemetry under a seqlock and UI events through a second SPSC ring. */
#pragma once
#include "cy_engine.h"
#include "cy_fx.h"
#include "fastmath.h"

/* 1: the implicit friction junction (Newton, then bisection). 0: BREATH, band-passed noise behind the same UI (the
 * spec's fallback; cy_engine_exciter_name(CY_EXC_BOW) then says so). The junction passed its acceptance, so 1 ships. */
#ifndef CY_BOW_MODEL
#define CY_BOW_MODEL 1
#endif

#define CY_N CY_NMODES
#define CY_NSUB (CY_BLOCK / CY_CR)       /* 4 control sub-blocks per render */
#ifndef CY_KILL_SUB
#define CY_KILL_SUB 1                    /* a mode the budget drops fades out over this many sub-blocks (see voice.c) */
#endif
/* Slots: 0..7 the TUNED voices, 8..11 the steal-fade pool, 12 the SURFACE slot. A pool, not the spec's one fade
 * slot: every event in a render is applied together, so a MIDI chord into a full pool steals several voices at once,
 * and with one fade slot each steal hard-cut the fade before it (a click on every chord; measured: a 4-note chord
 * into 8 ringing voices cut 3, an 8 kHz click at -39 dBFS). 4 keep a 4-note chord clean; a fifth steal inside 5 ms
 * cuts the quietest fade. (cy_config.h's CY_NSLOTS still says 9: nothing here uses it.) */
#define CY_NFADE 4u
#define CY_SFADE CY_NVOICES              /* the first steal-fade slot */
#define CY_SSURF (CY_NVOICES + CY_NFADE) /* the SURFACE slot */
#define CY_NALL (CY_SSURF + 1)
#define CY_IS_FADE(i) ((uint32_t)(i) - CY_SFADE < CY_NFADE)
#define CY_STEAL_FADE_S 0.005f           /* a stolen voice fades over 5 ms (spec §3.8) */
#define CY_GOV_FADE_S 0.05f              /* a held bowed note past a lowered bow cap fades over 50 ms (v1.1) */
#define CY_GOV_CHORD_R 17u               /* (v1.1) renders (~50 ms): bowed notes this young are one chord (cy_alloc) */
#define CY_TAILS_HOLD 2u                 /* (v1.1) renders (5.8 ms) the events after CY_EV_TAILS wait for its 5 ms fade */
#define CY_RESTART_FADE 3u               /* (v1.1.1) renders (8.7 ms) a restart's raised-cosine fade of the output lasts,
                                          * with the patch and the plate it began with; the next render starts the
                                          * engine and the room again from cold (cy_engine_restart) */
#define CY_SURF_XFADE_S 0.1f             /* the SURFACE plate an OCT change leaves behind rings out over 100 ms */
#define CY_BOW_PHI 0.02f                 /* a mode couples to the bow when |phi(bp)| >= this (see bow.c) */
#define CY_EVQ 128u                      /* main -> render events (power of two) */
#define CY_UIQ 64u                       /* render -> UI events (power of two) */
#define CY_TWO_PI_FS (6.28318530717959f / CY_FS)
#define CY_NYQ (0.45f * CY_FS)           /* nothing is synthesised above 0.45 fs (spec §3.3) */
/* and nothing is heard near it: a mode's output gain falls along a raised cosine from 0.36 fs to 0.44 fs (15.9 to
 * 19.4 kHz; voice.c), so a mode sitting at the edge is not smeared past 0.45 fs by an analysis window, and one
 * racing into it (a full STRETCH lean, a bend, vibrato) fades in gentle steps: the corners of steep per-sub-block
 * gain steps splatter past 0.45 fs too (measured: a 0.40-0.44 taper left -89 dB on a leaned STRETCH at G7) */
#define CY_EDGE_A (0.36f * 6.28318530717959f)
#define CY_EDGE_B (0.44f * 6.28318530717959f)
#define CY_GLIDE_DW (2.0f * 6.28318530717959f / CY_FS)   /* a mode whose frequency moved > 2 Hz glides (voice.c) */

/* §3.6 BOW constants, tuned on the real plates and the bowed presets (09, 21, 23), then frozen: every case locks
 * to the played note's harmonic series, starts, and stays under the RMS governor (tests/unit/test_engine.c).
 * beta is the spec's starting 0.05 cut to 0.008: at 0.05 the Stribeck slope out-drove the plate's own damping
 * ~400x in every mode at once and the note dissolved into chaotic chatter; at 0.008 the modes the bow favours win
 * and lock (bow.c). The ifndefs let a test or sweep override them. */
#ifndef CY_BOW_BETA
#define CY_BOW_BETA 0.008f               /* modal admittance at 261.63 Hz; scaled by sqrt(f_note / 261.63) */
#endif
#ifndef CY_BOW_RC0
#define CY_BOW_RC0 1.0f                  /* contact low-pass corner, in mode ratios: RC0 + RC1 grit (bow.c) */
#endif
#ifndef CY_BOW_RC1
#define CY_BOW_RC1 3.0f
#endif
#ifndef CY_BOW_EPS
#define CY_BOW_EPS 1e-3f
#endif
#define CY_BOW_MUS 0.9f
/* The bowed slots share budget / CY_BOW_SHARE coupled modes (engine.c, bow.c): a coupled mode costs about twice a
 * free one (sample-major, 29 instructions a mode-sample against 16) on top of the junction's ~150 a sample, and
 * without a share the governor's budget never reached the bow (measured: four bowed notes kept the device over its
 * half at the budget's floor). At the default budgets the share is more than the bow reaches; at the floor (48)
 * four bows keep 6 coupled modes each, the lowest */
#define CY_BOW_SHARE 2u
#define CY_BOW_CMIN 4u
#ifndef CY_BOW_ETGT_K
#define CY_BOW_ETGT_K 4.0f               /* bow RMS target scale (bow.c); the governor acts at 4x it */
#endif
#define CY_BOW_GAIN 8.0f                 /* a bowed slot's trim: a plain press near a strike's level, a lean above */
/* Every slot's output gain is out_norm * 10^(level/20) * CY_OUT_TRIM (spec §3.5; strikes carry (vel/127)^1.4 in
 * their impulses). Geom's out_norm aims the summed mode energy at 0.12, but a strike's peak also carries the
 * velocity curve (-2.9 dB at vel 100) and the Hertz roll-off: measured on the real plates, vel-100 C4 strikes
 * peaked 6 dB under §3.11's "about -18 dBFS on the bus". The trim puts them there. */
#define CY_OUT_TRIM 2.0f
/* SURFACE point dampers, 1/T60 per unit pressure on an antinode, weighted by phi^2 (a point damper's projection
 * onto each mode). Pressure on a struck key: the spec's formula says 5.8, but its own target is "about T60 = 50 ms
 * on an antinode at full pressure", which is 1/0.05 = 20: the target wins. A light touch (vel < 24) is a finger
 * resting on the plate, a far stronger damper: at least 0.4 x 40 = 16/s on an antinode (T60 63 ms) even with no
 * pressure reading, so within a few hundred ms every mode with more than ~0.3 of its peak under the finger is gone
 * and the ones with a node there ring on: the flageolet the SURFACE design promises. */
#define CY_TOUCH_K 20.0f
#define CY_FINGER_K 40.0f
#define CY_TOUCH_FLOOR 0.4f

enum { CY_EV_ON, CY_EV_OFF, CY_EV_FREEZE, CY_EV_LATCH, CY_EV_DAMP, CY_EV_PANIC, CY_EV_SEED, CY_EV_TAILS,
       CY_EV_RESTART };
typedef struct { uint8_t type, note, vel; int8_t key; uint32_t arg; } cy_ev_t;

/* One resonator slot: up to 32 complex one-poles z' = r e^{iw} z + b x, heard through Im z (spec §3.5). Structure of
 * arrays, so each kernel loop walks one array. The masks say which modes do what in the current sub-block. */
typedef struct {
    /* the kernels' state: these are all cy_modes_block and cy_bow_block touch */
    float zr[CY_N], zi[CY_N];            /* Re z (modal velocity, the bow reads it) and Im z (heard) */
    float c[CY_N], s[CY_N];              /* r cos w, r sin w */
    float gl[CY_N], gr[CY_N];            /* output gains now */
    float dgl[CY_N], dgr[CY_N];          /* per-sample gain steps while a mode's gains ramp */
    float dc[CY_N], ds[CY_N];            /* per-sample coefficient steps while a mode's frequency glides */
    float b[CY_N];                       /* input gain for x[] (EXCITED); bowed: beta phi cos(w/2) into Re z */
    /* control */
    float r[CY_N];                       /* pole radius */
    float rc[CY_N], lrc[CY_N];           /* ratio_cur (glides to the object's) and its log2 (STRETCH) */
    float e[CY_N];                       /* |z|^2 after the last sub-block */
    float w[CY_N];                       /* rad/sample at the last refresh */
    float phi[CY_N];                     /* the field at the last strike point (contact noise b_k) */
    float bphi[CY_N];                    /* the field at the bow point, smoothed (bow / breath b_k) */
    float bh[CY_N];                      /* bow injection into Im z: beta phi sin(w/2) (b[] holds beta phi cos(w/2)) */
    float pend[CY_N];                    /* strike impulses queued this render (a busy contact), after the budget */
    uint32_t pendm;                      /* ... the modes pend[] was written for (cy_budget walks only those) */
    float kf[CY_N];                      /* a killed mode's fade phase: 1 at the kill, 0 when it is gone */
    uint32_t act;                        /* modes with state */
    uint32_t allow;                      /* modes the budget lets this slot run (spec §3.8) */
    uint32_t run;                        /* modes cy_modes_block runs this sub-block */
    uint32_t ramp, exc;                  /* ... with ramping gains / with input x[] */
    uint32_t glm;                        /* ... with gliding coefficients (the frequency moved: see voice.c) */
    uint32_t kill;                       /* modes fading to zero (budget, Nyquist) over CY_KILL_SUB sub-blocks */
    uint32_t fresh;                      /* modes woken this sub-block: their gains start at target */
    uint32_t pmask;                      /* modes a queued contact pulse will drive (woken after the budget) */
    uint32_t hp_mask;                    /* modes the contact pulse drives */
    uint32_t cpl;                        /* the bow's coupled modes (run sample-major in cy_bow_block) */
    uint32_t breach;                     /* the modes the bow point reaches (bow.c), what a bowed slot asks the budget
                                          * for (with what still rings): all of them until its first sub-block */
    uint32_t rset;                       /* modes whose r[] was computed for the current damping signature: a mode
                                          * the budget allows later gets its r (and so its c, s) before it can wake */
    /* identity and state */
    uint8_t state;                       /* CY_VS_* */
    uint8_t xt;                          /* exciter it was played with */
    uint8_t vel, note, down, lhold;      /* key down; held by LATCH after the key came up */
    uint8_t has_at;                      /* one of the FM-1's pressure-sensing keys (0..19): P_eff = 0.4 v_n sw + P */
    uint8_t fading;                      /* steal-fade or panic: gains fall to 0, then the slot clears */
    uint8_t panic;                       /* fading from PANIC: gone within a render, so not charged to the budget */
    int8_t key;                          /* 0..26, -1 MIDI */
    uint8_t bowing, raining, glide, ref_pend, dirty, fresh_slot, bnc;
    uint8_t bhold;                       /* the bow is pressed (key down, latched, or a SURFACE bow key held) */
    uint8_t bstick;                      /* the junction was sticking at the end of the last sub-block */
    uint32_t age;                        /* note-on serial: bigger is newer */
    uint32_t t_on;                       /* samples since the last note-on (SWELL) */
    uint32_t born;                       /* render serial of its note-on (a steal before any output needs no fade) */
    float fnote, tfac;                   /* f_note, (261.63 / f_note)^0.5 */
    float vamp, vn;                      /* (vel/127)^1.4, vel/127 */
    float trim;                          /* slot gain over out_norm * level (1, or the bow's) */
    float P, Praw, Peff;                 /* smoothed pressure, its target, the exciter's effective pressure */
    float bsw;                           /* the bow's catch ramp, 0..1 (its speed follows it; min(SWELL, 60 ms)) */
    float M, str;                        /* frequency multiplier and stretch at the last refresh */
    float sx;                            /* the STRETCH exponent, smoothed (~36 ms) */
    uint8_t blim;                        /* bowed at some point: its whole output runs through the band-limiter */
    float bl_h[2][4];                    /* that band-limiter's history, L and R */
    float nz_f[2];                       /* the contact noise's extra band-limiter history */
    float B, Eref;                       /* BLOOM factor and its reference energy */
    float E, Epk, Efrz, Etgt;            /* energy now, peak (cull), at freeze (governor), bow target (governor) */
    float lvl, lvl_pk;                   /* output level estimate and its peak (key LEDs) */
    float dsig[3];                       /* damping signature at the last r recompute */
    float gsig;                          /* gain scale at the last gains pass */
    uint32_t gseq;                       /* object seq at the last gains pass */
    float fade_g, fade_dec;              /* fade multiplier now, its drop per sample */
    /* exciters */
    uint32_t hp_n, hp_len;               /* contact force pulse (strike, drop): samples left, length */
    uint32_t ct_tail;                    /* samples of the band-limiter's tail still to run after pulse and noise */
    float ct_h[4];                       /* the contact band-limiter's history ((1 + z^-1)^4 / 16) */
    float hp_amp, hp_c, hp_s, hp_dc, hp_ds;     /* its scale and the half-sine phasor (cos, sin, step) */
    uint32_t nz_n, nz_len;               /* contact noise: samples left, burst length */
    float nz_amp, nz_x1, nz_y1, nz_ec, nz_es;   /* level, HP state, Hann envelope phasor */
    float rain_cd;                       /* samples from this sub-block's start to the next drop */
    float bF, bgate;                     /* bow: last junction force, lift/press gate (15 ms) */
    float bY0, bvb, bFb, bmud, bdmu, biv02, bbeta;   /* bow constants for this sub-block */
    float bjit;                          /* bow-speed jitter depth (hair and rosin noise; GRIT) */
    uint8_t bidx[CY_N];
    uint32_t bslips, bslip_t, bisect;    /* stick->slip transitions this sub-block; last SLIP event; bisections */
    uint32_t bevals;                     /* junction evaluations (the CPU model) */
    float br_x1, br_x2, br_y1, br_y2;    /* BREATH band-pass state */
} cy_slot_t;

/* the main loop's side: written only by the cy_engine_* setters */
typedef struct {
    cy_ev_t ev[CY_EVQ];
    volatile uint32_t ev_w, ev_r;        /* written by main / by the render */
    cy_patch_t pbuf[2];
    volatile uint32_t pfront, pseq;
    volatile uint8_t press_key[CY_NKEYS];    /* poly pressure, 0..127, by key */
    volatile uint8_t press_note[128];        /* ... by note (MIDI notes have key -1) */
    volatile uint8_t chan;                   /* channel pressure / CC1 */
    volatile float bend;                     /* semitones */
    volatile int32_t budget;
    volatile int32_t bow_cap;                /* (v1.1) held bowed notes allowed: CY_MAX_BOWED unless the governor cuts it */
    volatile int32_t tail_cap;               /* (v1.1) released notes left ringing: CY_NVOICES unless the governor cuts it */
    uint32_t dropped;                        /* events lost to a full queue */
    volatile uint8_t rs_ready;               /* (v1.1.1, the render's) a hand-over restart has faded out and waits for
                                              * the main loop's patch (cy_engine_restart_ready) */
    volatile uint32_t rs_push, rs_take;      /* (v1.1.1) restarts queued (the main loop's count) and applied (the
                                              * render's): while they differ the render takes no new patch, so a
                                              * restart fades out on the patch in force when it was queued */
} cy_q_t;

/* the render's side */
typedef struct {
    cy_slot_t sl[CY_NALL];
    float bus_l[CY_BLOCK], bus_r[CY_BLOCK];
    float x[CY_CR];                      /* exciter scratch: one slot at a time */
    float tl[CY_CR], tr[CY_CR];          /* a band-limited slot renders here first */
    float bw[13][CY_N];                  /* the bow's coupled modes, compacted (one bowed slot at a time) */
    float dsurf[CY_N];                   /* SURFACE point damper, 1/T60 per mode */
    cy_patch_t p;                        /* the patch in use */
    uint32_t pseq, obj_seq, rnd;         /* patch picked up; object seen; render serial */
    const cy_object_t *obj;
    uint32_t rng;                        /* xorshift32, seed 0x9E3779B9 (spec §3.12) */
    uint32_t serial;                     /* note-on counter */
    uint16_t budget;
    uint8_t frozen, latched, damping, inited, last_exc;
    uint8_t bow_ccap;                    /* coupled modes a bowed slot may run this render (bow.c; engine.c sets it) */
    uint8_t bow_cap;                     /* (v1.1) bowed notes allowed this render: cy_q.bow_cap, read with the budget */
    uint32_t ev_hold;                    /* (v1.1) renders the queued events still wait (a CY_EV_TAILS fade; 0: none) */
    uint8_t tail_cap;                    /* (v1.1) released notes left ringing this render: cy_q.tail_cap */
    uint8_t restart;                     /* (v1.1.1, cy_engine_restart) 1: fading the output out (CY_EV_RESTART), the
                                          * patch held; 2: faded and cleared, silent until the main loop's next patch */
    uint8_t rs_hand;                     /* ... 1: it waits for that patch (a hand-over); 0: the patch in force then */
    uint32_t rs_n, rs_pseq;              /* ... samples of its fade done; cy_q.pseq when it began to wait */
    /* smoothed patch values, one step a sub-block (~10 ms) */
    float decay, tension, palm, force, grit, density, noise, damper, bloom, stretch, vib, lev, onorm, gpitch;
    float vib_ph;                        /* turns */
    uint32_t dsurf_ver;                  /* bumps whenever dsurf changes */
    /* SURFACE keys */
    uint8_t sk_down[CY_NKEYS], sk_touch[CY_NKEYS], sk_vel[CY_NKEYS], sk_rain[CY_NKEYS], sk_at[CY_NKEYS];
    uint8_t sk_lhold[CY_NKEYS];          /* released while LATCH is on: the point stays as it was (pressure too) */
    uint8_t sk_midi[CY_NKEYS];           /* played from MIDI (cy_engine_note_on_midi): no lean step, channel AT */
    float sk_P[CY_NKEYS], sk_rcd[CY_NKEYS];  /* smoothed pressure 0..1; rain countdown per key */
    uint32_t sk_t[CY_NKEYS];             /* samples since each key's note-on (SWELL) */
    int8_t bow_key;                      /* the SURFACE bow point, -1 none */
    /* the CPU model (spec §3.13): instruction estimates summed over this render */
    float est;
    uint32_t nan_resets, steal_cuts;
    float newest_hz;
    /* UI events */
    cy_uievent_t ui[CY_UIQ];
    volatile uint32_t ui_w, ui_r;
} cy_eng_t;

extern cy_eng_t cy_e;
extern cy_q_t cy_q;

/* Also in cy_engine.h (added by the integrator, review round; this copy is the same prototype): cy_engine_note_on with the note marked as played from
 * MIDI. Only SURFACE uses the mark (a MIDI note there arrives as a keybed point 0..26, and without it a point 0..19
 * took the sensing keys' P_eff = 0.4 v_n sw + P and ignored channel pressure; spec §3.6 gives MIDI v_n sw). For
 * TUNED notes (key -1) it is cy_engine_note_on. */
void cy_engine_note_on_midi(int key, int note, int vel);
/* geom.c: whether the field cache matches the published object and is not being rewritten right now (the main loop
 * lerps it in place on every SHAPE change, and the render may interrupt that). SURFACE rain reads the cache only
 * then. Declared here only: the integrator kept cy_plates.h frozen, as nothing outside src/ calls it. */
int cy_geom_fields_stable(void);

/* voice.c */
void cy_modes_block(cy_slot_t *sl, float *L, float *R, const float *x, uint32_t n);   /* CY_HOT: call via CY_FAR */
void cy_slot_clear(cy_slot_t *sl);
void cy_slot_ctl(cy_slot_t *sl, int idx, uint32_t n);                      /* CY_HOT: call via CY_FAR */
void cy_slot_gains(cy_slot_t *sl, uint32_t n);                             /* CY_HOT: call via CY_FAR */
void cy_slot_post(cy_slot_t *sl, uint32_t n);                              /* CY_HOT: call via CY_FAR; at fade_g <= 0
                                                                            * the caller clears the slot */
float cy_slot_freq0(const cy_slot_t *sl);
float cy_hertz(float f, float tau);
static inline __attribute__((always_inline)) uint32_t cy_popcount(uint32_t m)
{
    m = m - ((m >> 1) & 0x55555555u);
    m = (m & 0x33333333u) + ((m >> 2) & 0x33333333u);
    m = (m + (m >> 4)) & 0x0F0F0F0Fu;
    return (m * 0x01010101u) >> 24;
}
/* bow.c */
void cy_bow_prep(cy_slot_t *sl, const float *phi_target, uint32_t n);    /* CY_HOT: call via CY_FAR */
void cy_bow_block(cy_slot_t *sl, float *L, float *R, uint32_t n);          /* CY_HOT: call via CY_FAR */
void cy_breath_fill(cy_slot_t *sl, float *x, uint32_t n);

/* The CY_HOT kernels live in .ram_text (0x01C0xxxx) and the rest of the code in flash (XIP, 0x02xxxxxx), beyond the
 * pi32v2's 23-bit call range: the render calls them through a pointer the compiler cannot fold back into a direct
 * call (fm1-base's FL_FAR idiom, bsp/chip/fm1_flash.h). On the host it is just a call. */
static inline void *cy_far(void *p) { void *volatile q = p; return q; }
#define CY_FAR(fn) ((__typeof__(&fn))cy_far((void *)&fn))

/* sin and cos of w in [0, pi] (spec §3.3): x = w - pi/2, cos w = -sin x, sin w = cos x, with degree-9 / degree-10
 * polynomials fitted on |x| <= pi/2 (Lawson-weighted least squares, then checked in float32 without FMA: |error|
 * <= 1.4e-7, |c^2 + s^2 - 1| <= 2.8e-7). No range reduction: the callers keep w in range. */
static inline __attribute__((always_inline)) void cy_sincos(float w, float *c, float *s)   /* (always inlined: the
                                                                                         * CY_HOT control path calls nothing) */
{
    float x = w - 1.57079632679f, x2 = x * x, ps, pc;
    ps = 2.5904894303e-06f;
    ps = ps * x2 - 1.9800898251e-04f;
    ps = ps * x2 + 8.3328998318e-03f;
    ps = ps * x2 - 1.6666647635e-01f;
    ps = ps * x2 + 9.9999997659e-01f;
    pc = -2.6051496068e-07f;
    pc = pc * x2 + 2.4760161428e-05f;
    pc = pc * x2 - 1.3888361402e-03f;
    pc = pc * x2 + 4.1666636258e-02f;
    pc = pc * x2 - 4.9999999358e-01f;
    pc = pc * x2 + 9.9999999978e-01f;
    *c = -ps * x;
    *s = pc;
}

/* fastmath.h's helpers, copied verbatim (fm1-base, MPL-2.0) but always inlined: the device's clang 4 does not inline
 * the static inline originals into a function as big as the CY_HOT control path (voice.c cy_slot_ctl), and a call
 * from .ram_text into the flash does not link. Same code, same results (the host hash is unchanged). */
#define CY_AI static inline __attribute__((always_inline))
CY_AI float cy_i_fabsf(float x) { fm_bits_t b; b.f = x; b.u &= 0x7FFFFFFFu; return b.f; }
CY_AI float cy_i_minf(float a, float b) { return a < b ? a : b; }
CY_AI float cy_i_maxf(float a, float b) { return a > b ? a : b; }
CY_AI float cy_i_clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
CY_AI float cy_i_floorf(float x)
{
    int32_t i = (int32_t)x;
    float f = (float)i;
    return f > x ? f - 1.0f : f;
}
CY_AI float cy_i_exp2_nf(int32_t n, float f)
{
    fm_bits_t b;
    float p;
    if (n < -126)
        return 0.0f;
    p = 1.534580891e-04f;
    p = p * f + 1.339993158e-03f;
    p = p * f + 9.618488972e-03f;
    p = p * f + 5.550328776e-02f;
    p = p * f + 2.402264689e-01f;
    p = p * f + 6.931472057e-01f;
    p = p * f + 1.0f;
    b.f = p;
    b.i += n << 23;
    return b.f;
}
CY_AI float cy_i_exp2f(float x)
{
    float fl;
    if (x < -126.0f)
        return 0.0f;
    if (x > 127.0f)
        x = 127.0f;
    fl = cy_i_floorf(x + 0.5f);
    return cy_i_exp2_nf((int32_t)fl, x - fl);
}
CY_AI float cy_i_expf(float x)
{
    float n;
    if (x < -87.3f)
        return 0.0f;
    if (x > 88.7f)
        x = 88.7f;
    n = cy_i_floorf(x * FM_LOG2E + 0.5f);
    x = (x - n * 0.693145752f) - n * 1.42860677e-6f;
    return cy_i_exp2_nf((int32_t)n, x * FM_LOG2E);
}
CY_AI float cy_i_log2f(float x)
{
    fm_bits_t b;
    float m, t, t2, p;
    int32_t e;
    b.f = x;
    if (b.i <= 0)
        return -127.0f;
    e = ((b.i >> 23) & 0xFF) - 127;
    b.i = (b.i & 0x007FFFFF) | 0x3F800000;
    m = b.f;
    if (m > 1.41421356f) {
        m *= 0.5f;
        e += 1;
    }
    t = (m - 1.0f) / (m + 1.0f);
    t2 = t * t;
    p = 3.428278367e-01f;
    p = p * t2 + 4.115337810e-01f;
    p = p * t2 + 5.770866525e-01f;
    p = p * t2 + 9.617966483e-01f;
    p = p * t2 + 2.885390082e+00f;
    return (float)e + p * t;
}
CY_AI float cy_i_sin_pi(float x)
{
    float x2 = x * x, p;
    p = 1.345176048e-10f;
    p = p * x2 - 2.467706523e-08f;
    p = p * x2 + 2.752946668e-06f;
    p = p * x2 - 1.984015614e-04f;
    p = p * x2 + 8.333310417e-03f;
    p = p * x2 - 1.666666459e-01f;
    p = p * x2 + 9.999999946e-01f;
    return p * x;
}
CY_AI float cy_i_sin_turns(float ph)
{
    float x = ph - cy_i_floorf(ph + 0.5f);
    return cy_i_sin_pi(x * FM_TWO_PI);
}
CY_AI float cy_i_sqrtf(float x)
{
    fm_bits_t b;
    float y;
    if (x <= 0.0f)
        return 0.0f;
    b.f = x;
    b.u = 0x5F375A86u - (b.u >> 1);
    y = b.f;
    y = y * (1.5f - 0.5f * x * y * y);
    y = y * (1.5f - 0.5f * x * y * y);
    y = y * (1.5f - 0.5f * x * y * y);
    return x * y;
}

/* for CY_HOT code: plain expressions, never calls (the device's clang 4 does not always inline even static inline
 * helpers, and a call out of .ram_text fails the build) */
#define CY_ABS(x) ((x) < 0.0f ? -(x) : (x))
#define CY_CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))

static inline float cy_rand_pm1(uint32_t *s) { return (float)(int32_t)cy_rand(s) * (1.0f / 2147483648.0f); }   /* [-1,1) */
