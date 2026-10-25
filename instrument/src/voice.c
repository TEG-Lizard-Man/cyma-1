/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 voices (M2): the mode kernels (CY_HOT) and each slot's control at the 32-sample control rate: pressure,
 * frequencies (glide, stretch, tension, vibrato, bloom), damping (material law, damper, palm, touch, freeze), the
 * coefficient refresh, output gains and ramps, then energies, cull and the governors. Spec §3.3-3.5, 3.8, 3.9.
 *
 * A mode is a complex one-pole in rotation form, z' = r e^{iw} z + b x, heard through Im z. A strike drives Re z
 * with its contact force (engine.c cy_pulse), so the heard part starts from zero and smoothly (t^3); even the
 * spec's bare impulse into Re z starts it as A r sin(w n), from zero with no step. The pole radius is exactly r
 * whatever the frequency does (the refresh renormalises (c, s)), so modulation never changes a mode's level, and a
 * step in r (a damper, a palm) only bends the decay: neither can click. */
#include "cy_engine_int.h"

/* ------------------------------------------------------------- kernels --- */
/* One sub-block of one slot, mode-major: each run mode keeps z, (c, s) and its gains in registers for n samples.
 * The spec's three loops (§3.5), FREE (constant gains), RAMP (gains step linearly: fades, mic moves, budget drops)
 * and EXCITED (input x[] times b_k), each also gliding the coefficients (c, s) linearly to the sub-block's new
 * frequency when it moved (cy_slot_coeffs), split by what actually moves, so no loop adds a zero step: device
 * loop sizes FREE 16, EXCITED steady 19 (27 with steps), RAMP gains-only 18, glide-only 18, both 21 instructions a
 * mode-sample. A strike's onset runs mostly steady EXCITED modes: that is the engine's costliest render. It calls
 * nothing. */
CY_HOT void cy_modes_block(cy_slot_t *sl, float *L, float *R, const float *x, uint32_t n)
{
    uint32_t k, i, run = sl->run, ramp = sl->ramp | sl->glm, exc = sl->exc, glm = sl->glm;
    for (k = 0; k < CY_N; k++) {
        uint32_t bit = 1u << k;
        float zr, zi, c, s, gl, gr, dc, ds;
        if (!(run & bit))
            continue;
        zr = sl->zr[k], zi = sl->zi[k], c = sl->c[k], s = sl->s[k], gl = sl->gl[k], gr = sl->gr[k];
        dc = (glm & bit) ? sl->dc[k] : 0.0f;
        ds = (glm & bit) ? sl->ds[k] : 0.0f;
        if ((exc & bit) && !(ramp & bit)) {   /* excited with steady gains and coefficients: the EXCITED loop less
                                               * its four zero-step adds (review-perf; bit-exact) */
            float b = sl->b[k];
            for (i = 0; i < n; i++) {
                float t = c * zr - s * zi + b * x[i];
                zi = s * zr + c * zi;
                zr = t;
                L[i] += gl * zi;
                R[i] += gr * zi;
            }
        } else if (exc & bit) {   /* (dgl/dgr only while this mode's gains ramp: they keep the last ramp's step, and an
                            * excited sub-block that applied a stale one drifted the gain with nothing to pull it
                            * back; rain's drops did that often enough to grow a mode to +130 dBFS. Integration, M7) */
            float b = sl->b[k], dl = (sl->ramp & bit) ? sl->dgl[k] : 0.0f, dr = (sl->ramp & bit) ? sl->dgr[k] : 0.0f;
            for (i = 0; i < n; i++) {
                float t = c * zr - s * zi + b * x[i];
                zi = s * zr + c * zi;
                zr = t;
                c += dc;
                s += ds;
                gl += dl;
                gr += dr;
                L[i] += gl * zi;
                R[i] += gr * zi;
            }
        } else if ((ramp & bit) && !(glm & bit)) {   /* gains ramp, coefficients steady (review-perf; bit-exact) */
            float dl = sl->dgl[k], dr = sl->dgr[k];
            for (i = 0; i < n; i++) {
                float t = c * zr - s * zi;
                zi = s * zr + c * zi;
                zr = t;
                gl += dl;
                gr += dr;
                L[i] += gl * zi;
                R[i] += gr * zi;
            }
        } else if ((ramp & bit) && !(sl->ramp & bit)) {   /* a glide with steady gains (review-perf; bit-exact) */
            for (i = 0; i < n; i++) {
                float t = c * zr - s * zi;
                zi = s * zr + c * zi;
                zr = t;
                c += dc;
                s += ds;
                L[i] += gl * zi;
                R[i] += gr * zi;
            }
        } else if (ramp & bit) {
            float dl = (sl->ramp & bit) ? sl->dgl[k] : 0.0f, dr = (sl->ramp & bit) ? sl->dgr[k] : 0.0f;
            for (i = 0; i < n; i++) {
                float t = c * zr - s * zi;
                zi = s * zr + c * zi;
                zr = t;
                c += dc;
                s += ds;
                gl += dl;
                gr += dr;
                L[i] += gl * zi;
                R[i] += gr * zi;
            }
        } else {
            for (i = 0; i < n; i++) {
                float t = c * zr - s * zi;
                zi = s * zr + c * zi;
                zr = t;
                L[i] += gl * zi;
                R[i] += gr * zi;
            }
        }
        sl->zr[k] = zr, sl->zi[k] = zi, sl->gl[k] = gl, sl->gr[k] = gr;
        if (glm & bit)
            sl->c[k] = c, sl->s[k] = s;
    }
}

/* ------------------------------------------------------------- helpers --- */
/* (memset: fm1-base's moves unrolled words, about 1,200 instructions for a slot; the byte loop this was took 9,300,
 * twice for every steal and once for every note-on, so a chord's onset render, the engine's costliest, spent a
 * few hundred instructions a sample on it) */
void cy_slot_clear(cy_slot_t *sl)
{
    __builtin_memset(sl, 0, sizeof *sl);
    sl->key = -1;
    sl->fade_g = 1.0f;
}

/* the spectrum of a half-cosine contact of duration tau at f (spec §3.6): 1 at DC, pi/4 at f = 1 / (2 tau), then
 * zeros and a 1/f^2 roll-off. That roll-off is the mallet's whole hardness: no filter runs per sample. */
float cy_hertz(float f, float tau)
{
    float ft = f * tau, den = 1.0f - 4.0f * ft * ft;
    if (fm_fabsf(den) < 1e-3f)
        return 0.785398163f;
    return fm_fabsf(fm_cosf(3.14159265f * ft) / den);
}

/* the lowest mode's frequency now (telemetry, the BREATH band) */
float cy_slot_freq0(const cy_slot_t *sl)
{
    return sl->fnote * sl->rc[cy_e.obj->order[0]] * sl->M;
}

/* ------------------------------------------------------------- control --- */
/* The control path below runs for every live slot in every sub-block, so it lives in .ram_text with the kernels
 * (CY_HOT: called through CY_FAR from the render; it calls nothing, so its helpers are inlined). From XIP flash it
 * was ~6 KB of hot code competing with the UI's ~35 KB for the 32 KB I-cache after every frame (review dsp-2:
 * plausible, not measurable in fm1-emu, which models no cache; the cost here is a pointer call per function). */
#define CY_INL static inline __attribute__((always_inline))
#define fm_fabsf cy_i_fabsf                  /* (the always-inlined copies: cy_engine_int.h) */
#define fm_minf cy_i_minf
#define fm_maxf cy_i_maxf
#define fm_clampf cy_i_clampf
#define fm_exp2f cy_i_exp2f
#define fm_expf cy_i_expf
#define fm_log2f cy_i_log2f
#define fm_sin_turns cy_i_sin_turns
#define fm_sqrtf cy_i_sqrtf

/* The coefficient refresh: w from f_note * ratio_eff * M; a mode above 0.45 fs fades (CY_KILL_SUB) and goes.
 * glide: a sounding mode does not jump to its new (c, s) at the sub-block edge but glides there across it (the
 * kernels' dc, ds), its frequency piecewise linear instead of a staircase. Each step of a staircase is a kink in
 * the phase whose splatter falls off only as 1 / distance, and a fast pitch move (a BLOOM drop of 5 % in 40 ms, a
 * bend) put the top modes' splatter past 0.45 fs at -80 dB of the total; gliding takes it under -110 (modelled and
 * measured, spec §8.3.2), and costs two adds a mode-sample while the pitch moves. Linear in (c, s) rather than an
 * exact rotation: the radius dips by (dw)^2 / 8 mid-block, ~1e-7 here. The bow's junction glides its coupled modes
 * the same way (it solves each sample with the coefficients the modes have then). Only a mode whose frequency
 * moved by more than 2 Hz: a step's splatter grows with its size in Hz, so a vibrato's wobble on the low modes
 * (most of a chord's) can stay a staircase while a bend, a BLOOM drop, a STRETCH lean, or any high mode glides
 * (modelled at G7: 20 c vibrato, staircase -101 dB, glide -137; a BLOOM +85 drop -83 / -116). Not for a mode just
 * woken (it has no old coefficients). */
CY_INL int cy_slot_coeffs(cy_slot_t *sl, float M, float str, uint32_t n, int glide)
{
    uint32_t k, set = sl->allow | sl->act;
    float f0 = sl->fnote * M, inv_n = 1.0f / (float)n;
    int stretch = str != 1.0f, edge = 0;
    for (k = 0; k < CY_N; k++) {
        float f, w, dw, cw, sw2, kn, rr;
        uint32_t bit = 1u << k;
        if (!(set & bit))
            continue;
        f = f0 * (stretch ? fm_exp2f(sl->lrc[k] * str) : sl->rc[k]);
        if (f > CY_NYQ) {
            if ((sl->act & bit) && !(sl->kill & bit))
                sl->kill |= bit, sl->kf[k] = 1.0f;
            sl->allow &= ~bit;
            edge = 1;
            continue;
        }
        w = f * CY_TWO_PI_FS;
        edge |= (w > CY_EDGE_A) | (sl->w[k] > CY_EDGE_A);
        dw = fm_fabsf(w - sl->w[k]);
        sl->w[k] = w;
        cy_sincos(w, &cw, &sw2);
        kn = 1.5f - 0.5f * (cw * cw + sw2 * sw2);   /* 1/|.|: one Newton step from 1 is exact in float here */
        rr = sl->r[k] * kn;
        if (glide && dw > CY_GLIDE_DW && (sl->act & bit) && !(sl->fresh & bit)) {
            sl->dc[k] = (rr * cw - sl->c[k]) * inv_n;
            sl->ds[k] = (rr * sw2 - sl->s[k]) * inv_n;
            sl->glm |= bit;
        } else {
            sl->c[k] = rr * cw;
            sl->s[k] = rr * sw2;
        }
    }
    sl->M = M;
    sl->str = str;
    cy_e.est += (35.0f / 32.0f) * (float)(cy_popcount(set) * n);   /* §3.14: "+35/slot" is 32 modes' sincos and
                                                                     * renormalisation over 32 samples */
    return edge;
}

/* Once per sub-block before the kernels: everything that moves at control rate. Recomputes the pole radii only
 * when the damping signature moved and the (c, s) pairs only when M moved by > 2e-5, a glide runs, or r changed
 * (spec §3.3), so a static ringing slot costs a few dozen instructions here. */
CY_HOT void cy_slot_ctl(cy_slot_t *sl, int idx, uint32_t n)
{
    cy_eng_t *e = &cy_e;
    const cy_object_t *o = e->obj;
    /* (a mode allowed since r was last computed, by the budget growing this slot's share, has r = 0 or a stale r:
     * recompute, or a re-strike, a drop or the bow would wake it with (c, s) = 0, silent, and it would be culled) */
    uint32_t k, set = sl->allow | sl->act, refresh = sl->fresh_slot, damp = sl->dirty || (set & ~sl->rset) != 0u;
    int tuned_mallet = sl->xt == CY_EXC_MALLET && idx != CY_SSURF;
    float M, str, ib, kfac, pp, sw, vs;

    /* pressure: 15 ms one-pole (spec §3.6); P_eff = what the exciter feels. The FM-1's sensing keys (0..19):
     * 0.4 v_n sw + P, a plain press gentle and leaning digging in. Everything else: v_n sw + (1 - v_n sw) P, which
     * is v_n sw with no pressure (keys 20..26 never have any) and only ever adds: a MIDI note keeps its velocity's
     * level and aftertouch leans it toward full. (Spec §3.6 switched MIDI to the keys' formula once aftertouch
     * arrived, so the first light touch, or a mod-wheel nudge, which is channel pressure too, cut a bowed or rained
     * note by 5 dB until the player leaned past AT 56) */
    sl->P += 0.047f * (sl->Praw - sl->P);
    if (sl->P < 1e-6f)
        sl->P = 0.0f;
    sw = 1.0f - fm_expf(-(float)sl->t_on * (1.0f / CY_FS) / fm_maxf(e->p.swell_s, 0.001f));
    vs = sl->vn * sw;
    sl->Peff = sl->has_at ? fm_clampf(0.4f * sl->vn * sw + sl->P, 0.0f, 1.0f) : vs + (1.0f - vs) * sl->P;
    /* the bow's speed ramp (the catch): SWELL's own curve, but never slower than 60 ms (the force follows SWELL) */
    sl->bsw = 1.0f - fm_expf(-(float)sl->t_on * (1.0f / CY_FS) / fm_clampf(e->p.swell_s, 0.001f, 0.06f));
    if (sl->t_on < 0x7FFFFFFFu)
        sl->t_on += n;

    /* STRETCH: log2 ratio * (1 + 0.3 stretch P), the exponent smoothed over ~36 ms beyond P's own 15 ms: a full
     * lean moves the top modes by octaves, and at P's pace they raced into the band edge faster than its taper can
     * fade them cleanly (spec §8.3.2); the metal tightening takes a few tens of ms anyway */
    sl->sx += 0.02f * (1.0f + 0.3f * e->stretch * sl->P - sl->sx);
    if (fm_fabsf(sl->sx - 1.0f) < 1e-7f)
        sl->sx = 1.0f;
    str = sl->sx;
    /* object glide: 0.18 a sub-block (~40 ms) toward the object's ratios */
    if (sl->glide) {
        float dmax = 0.0f;
        for (k = 0; k < CY_N; k++) {
            float d = o->ratio[k] - sl->rc[k];
            sl->rc[k] += 0.18f * d;
            dmax = fm_maxf(dmax, fm_fabsf(d));
        }
        if (dmax <= 1e-5f) {
            for (k = 0; k < CY_N; k++)
                sl->rc[k] = o->ratio[k], sl->lrc[k] = o->log2_ratio[k];
            sl->glide = 0;
        } else if (str != 1.0f) {        /* log2 ratio only feeds STRETCH (cy_slot_coeffs); with no stretch it
                                          * waits (a later sub-block with stretch, still gliding, recomputes it
                                          * here; a finished glide sets it exactly above). review-perf: under a
                                          * morph this was 32 log2s per slot every sub-block for nothing */
            for (k = 0; k < CY_N; k++)
                sl->lrc[k] = fm_log2f(sl->rc[k]);
        }
        refresh = 1;
        damp |= tuned_mallet && sl->P > 0.0f;   /* PALM depends on the ratio */
    }

    /* M = bend * tune * tension (TUNED MALLET) * vibrato * bloom (spec §3.3) */
    M = e->gpitch;
    if (tuned_mallet && e->tension > 0.0f && sl->P > 0.0f)
        M *= fm_exp2f(sl->P * e->tension * (1.0f / 12.0f));
    if (e->vib > 0.01f)
        M *= fm_exp2f(e->vib * (1.0f / 1200.0f) * fm_sin_turns(e->vib_ph + 0.37f * (float)idx));
    if (e->bloom != 0.0f) {
        float B = 1.0f + 0.06f * e->bloom;            /* at the strike E_low / E_ref = 1 */
        if (!sl->ref_pend && sl->Eref > 1e-30f) {
            float el = sl->e[o->order[0]] + sl->e[o->order[1]] + sl->e[o->order[2]] + sl->e[o->order[3]];
            B = 1.0f + 0.06f * e->bloom * fm_sqrtf(fm_minf(el / sl->Eref, 4.0f));
        }
        sl->B = fm_clampf(B, 0.85f, 1.25f);
        M *= sl->B;
    } else {
        sl->B = 1.0f;
    }
    if (fm_fabsf(M - sl->M) > 2e-5f * sl->M)
        refresh = 1;
    if (fm_fabsf(str - sl->str) > 1e-6f)
        refresh = 1;

    /* damping (spec §3.4): 1/T60 = damp_div / (t0 decay (261.63/f)^0.5), clamped to [0.02, 60] s, then the key-up
     * damper, PALM, the SURFACE point damper and DAMP. Signature: the scalars every mode's value follows. */
    ib = 1.0f / fm_maxf(o->t0 * e->decay * sl->tfac, 1e-6f);
    kfac = (!sl->down && !sl->lhold && sl->state != CY_VS_FROZEN) ? 1.0f / fm_maxf(1.0f - 0.98f * e->damper, 0.02f)
                                                                 : 1.0f;
    pp = tuned_mallet ? sl->P * e->palm : 0.0f;
    {
        float s0 = ib * kfac, s1 = pp + (e->damping ? 1.0f : 0.0f) + (sl->state == CY_VS_FROZEN ? 2.0f : 0.0f),
              s2 = idx == CY_SSURF ? (float)e->dsurf_ver : 0.0f;
        if (fm_fabsf(s0 - sl->dsig[0]) > 1e-4f * s0 || fm_fabsf(s1 - sl->dsig[1]) > 1e-5f || s2 != sl->dsig[2])
            damp = 1;
        if (damp) {
            const float kr = -9.96578428f / CY_FS;   /* r = 2^(-log2(1000) / (T60 fs)) */
            sl->dsig[0] = s0, sl->dsig[1] = s1, sl->dsig[2] = s2;
            sl->rset = set;
            for (k = 0; k < CY_N; k++) {
                float inv;
                if (!(set >> k & 1u))
                    continue;
                if (sl->state == CY_VS_FROZEN) {     /* (DAMP, a palm on the plate, chokes a frozen slot too: spec
                                                      * §3.4 "every slot while held"; FREEZE holds again on release) */
                    sl->r[k] = e->damping ? fm_exp2f(kr * 12.5f) : 0.9999999f;
                    continue;
                }
                inv = fm_clampf(o->damp_div[k] * ib, 1.0f / 60.0f, 50.0f) * kfac;
                inv += pp * (1.74f + 0.87f * sl->rc[k]);
                if (idx == CY_SSURF)
                    inv += e->dsurf[k];
                if (e->damping)
                    inv = fm_maxf(inv, 12.5f);    /* DAMP: T60 <= 80 ms */
                sl->r[k] = fm_exp2f(kr * inv);
            }
            refresh = 1;
        }
    }

    sl->glm = 0;
    if (refresh && cy_slot_coeffs(sl, M, str, n, !sl->fresh_slot))
        sl->gsig = -1.0f;                            /* (a mode in or through the band-edge taper, or one killed at
                                                      * 0.45 fs: its gain moved with w. Otherwise a refresh leaves
                                                      * every gain where it was, and vibrato, BLOOM or a glide no
                                                      * longer force a gains pass over 32 modes every sub-block) */
    sl->fresh_slot = 0;
    sl->dirty = 0;
    e->est += 30.0f * (float)n;                      /* §3.14: static slot control */
}

/* a raised cosine of a fade phase p (1 -> 0): the gain a fade has reached. Stepped once a sub-block and ramped
 * linearly between, it spreads far less energy than a straight line of the same length (no corner at either end) */
CY_INL float cy_fade_shape(float p)
{
    float c, s;
    if (p <= 0.0f)
        return 0.0f;
    if (p >= 1.0f)
        return 1.0f;
    cy_sincos(3.14159265f * p, &c, &s);
    return 0.5f - 0.5f * c;
}

/* output gains: g = out_norm * level * trim * mic_k (spec §3.5), times the slot's fade; a mode whose target moved
 * ramps across the sub-block, a fresh one starts at its target, a killed one follows a raised cosine to 0 over
 * CY_KILL_SUB sub-blocks (the spec's one: the cosine's soft corners keep even 0.7 ms free of a tick, tested in
 * test_engine.c; longer would let a dropped mode keep costing after the budget gave its place away) */
CY_HOT void cy_slot_gains(cy_slot_t *sl, uint32_t n)
{
    cy_eng_t *e = &cy_e;
    const cy_object_t *o = e->obj;
    uint32_t k, set = sl->act;
    float fade1 = sl->fade_g, a, inv_n = 1.0f / (float)n;
    if (sl->fading)
        fade1 = cy_fade_shape(sl->fade_g - sl->fade_dec * (float)n);
    a = e->onorm * e->lev * CY_OUT_TRIM * sl->trim * fade1;
    sl->ramp = 0;
    if (a == sl->gsig && o->seq == sl->gseq && !sl->kill && !sl->fresh)
        return;
    for (k = 0; k < CY_N; k++) {
        uint32_t bit = 1u << k;
        float tl, tr, dl, dr;
        if (!(set & bit))
            continue;
        tl = a * o->mic_l[k], tr = a * o->mic_r[k];
        if (sl->w[k] > CY_EDGE_A) {              /* the band-edge taper (cy_engine_int.h) */
            float g = cy_fade_shape((CY_EDGE_B - sl->w[k]) * (1.0f / (CY_EDGE_B - CY_EDGE_A)));
            tl *= g, tr *= g;
        }
        if (sl->kill & bit) {
            float g = cy_fade_shape(sl->kf[k] - 1.0f / (float)CY_KILL_SUB);
            tl *= g, tr *= g;
        }
        if (sl->fresh & bit) {
            sl->gl[k] = tl, sl->gr[k] = tr;
            continue;
        }
        dl = tl - sl->gl[k], dr = tr - sl->gr[k];
        if (fm_fabsf(dl) > 1e-6f * fm_fabsf(tl) + 1e-12f || fm_fabsf(dr) > 1e-6f * fm_fabsf(tr) + 1e-12f) {
            sl->dgl[k] = dl * inv_n, sl->dgr[k] = dr * inv_n;
            sl->ramp |= bit;
        } else {
            sl->gl[k] = tl, sl->gr[k] = tr;           /* (a 1e-6 snap is inaudible and stops endless micro-ramps) */
        }
    }
    sl->fresh = 0;
    sl->gsig = sl->fading ? -1.0f : a;               /* (a fading slot recomputes every sub-block) */
    sl->gseq = o->seq;
}

/* after the kernels: energies, kills, the NaN guard, cull, governors, the level estimate (spec §3.5, 3.4, 3.6) */
CY_HOT void cy_slot_post(cy_slot_t *sl, uint32_t n)
{
    cy_eng_t *e = &cy_e;
    uint32_t k, m, act = sl->act, kill = sl->kill, keep;
    float E = 0.0f, lv = 0.0f, thr;
    /* energies and the level (summed in k order), and the kill fades' advance, in one walk up to the highest mode
     * with anything to do (an inactive mode's state is already zero: whatever deactivates a mode zeroes it) */
    for (k = 0, m = act | kill; m; k++, m >>= 1) {
        uint32_t bit = 1u << k;
        if (!(m & 1u))
            continue;
        if (act & bit) {
            float en = sl->zr[k] * sl->zr[k] + sl->zi[k] * sl->zi[k];
            sl->e[k] = en;
            E += en;
            lv += en * (sl->gl[k] * sl->gl[k] + sl->gr[k] * sl->gr[k]);
        }
        if (kill & bit) {
            sl->kf[k] -= 1.0f / (float)CY_KILL_SUB;
            if (sl->kf[k] <= 1e-3f) {
                kill &= ~bit;
                act &= ~bit;
                sl->zr[k] = sl->zi[k] = sl->e[k] = sl->gl[k] = sl->gr[k] = 0.0f;
            }
        }
    }
    sl->act = act, sl->kill = kill;
    sl->ramp = 0;
    if (!(E < 1e30f)) {                              /* NaN or runaway: reset the slot, count it */
        for (k = 0; k < CY_N; k++)
            sl->zr[k] = sl->zi[k] = sl->e[k] = sl->gl[k] = sl->gr[k] = 0.0f;
        sl->act = sl->kill = 0;
        sl->bF = 0.0f;
        sl->E = sl->Epk = sl->lvl = 0.0f;
        e->nan_resets++;
        return;
    }
    sl->E = E;
    if (E > sl->Epk)
        sl->Epk = E;
    /* cull: 120 dB under the slot's peak (or absolutely tiny), unless something drives it. This is what keeps the
     * states clear of denormals, and why silence ends in exact zeros. A driven or bow-coupled mode stays (a bow at
     * FORCE 0 leaves its coupled modes undriven but coupled), but once its energy is under 1e-30 (-300 dB) its state
     * is zeroed so it can never decay into the subnormals (the device's FTZ is unknown) */
    thr = 1e-12f * sl->Epk + 1e-24f;
    keep = sl->exc | sl->cpl;
    for (k = 0, m = act; m; k++, m >>= 1) {
        uint32_t bit = 1u << k;
        if (!(m & 1u))
            continue;
        if (!(keep & bit)) {
            if (sl->e[k] < thr) {
                sl->zr[k] = sl->zi[k] = sl->e[k] = sl->gl[k] = sl->gr[k] = 0.0f;
                act &= ~bit;
                kill &= ~bit;                        /* (a mode culled mid-fade is simply gone) */
            }
        } else if (sl->e[k] < 1e-30f) {
            sl->zr[k] = sl->zi[k] = sl->e[k] = 0.0f;
        }
    }
    sl->act = act, sl->kill = kill;
    /* governors: a frozen slot may not grow past 2x its energy at freeze, a bowed one past 4x its target */
    if ((sl->state == CY_VS_FROZEN && E > 2.0f * sl->Efrz) || (sl->bowing && sl->Etgt > 0.0f && E > 4.0f * sl->Etgt)) {
        float g = sl->state == CY_VS_FROZEN ? 0.995f : 0.98f;
        for (k = 0, m = act; m; k++, m >>= 1)
            if (m & 1u)
                sl->zr[k] *= g, sl->zi[k] *= g;
    }
    sl->lvl = fm_sqrtf(0.5f * lv);
    if (sl->lvl > sl->lvl_pk)
        sl->lvl_pk = sl->lvl;
    /* BLOOM's reference: E_low once a strike has landed (the whole contact over: pulse, noise, band-limiter tail;
     * taken any earlier, the energy the rest of the contact adds would read as more than the reference and kick
     * the pitch up a step instead of letting it fall), or a running max (2 s release) while bowed or raining */
    {
        const uint8_t *ord = e->obj->order;
        float el = sl->e[ord[0]] + sl->e[ord[1]] + sl->e[ord[2]] + sl->e[ord[3]];
        if (sl->ref_pend && !sl->hp_n && !sl->nz_n && !sl->ct_tail) {
            sl->Eref = el;
            sl->ref_pend = 0;
        } else if (sl->bowing || sl->raining) {
            sl->Eref = fm_maxf(el, sl->Eref * 0.999637f);   /* exp(-32 / (2 s fs)) */
        }
    }
    if (sl->fading)
        sl->fade_g -= sl->fade_dec * (float)n;      /* (at 0 the render clears the slot: free, state CY_VS_FREE) */
}
