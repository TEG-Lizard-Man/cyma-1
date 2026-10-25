/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 bow (M2): the implicit friction junction (spec §3.6) and the BREATH fallback behind the same UI.
 *
 * The junction. The bow presses at a point bp with force F_b and moves at v_b. The plate's velocity there is the
 * sum of the coupled modes' Re z weighted by their field phi_k(bp). One sample: rotate every coupled mode, read the
 * "history" velocity v_h, then solve for the friction force F that is consistent with the velocity it causes
 * (v = v_h + Y0 F, Y0 = beta sum phi_k^2):
 *     g(F) = F - F_b mu(v_b - v_h - Y0 F) = 0,   mu(d) = d / sqrt(d^2 + eps^2) * (mu_d + (mu_s - mu_d) / (1 + (d/v0)^2))
 * mu is a smoothed Coulomb sign times a Stribeck curve, with no exp in the loop. Then F goes back into every coupled
 * mode at the half-sample phase, z += beta phi_k e^{i w_k / 2} F. That phase is what makes a mode behave like a mass
 * on a spring: the velocity's response to a constant force sums to exactly zero (Re of e^{iw/2} / (1 - e^{iw}) is
 * 0), so a held force can only deflect the plate, never drag it along. Injected into Re z alone (the spec's form),
 * the velocity's DC response is 1/2 whatever the mode, the bow can then hold the plate moving with it forever, and
 * the hard-pressed note comes out as a static offset with no tone (measured). The same phase rolls the junction's
 * admittance off as cos(w/2) toward Nyquist, as a finite contact would.
 *
 * The solve. Newton from the last F, safeguarded: the bracket starts on the last F's side of [-F_b mu_s, F_b mu_s]
 * (g(-F_b mu_s) < 0 < g(F_b mu_s) always, since |mu| < mu_s), every evaluation tightens it, and a Newton step that
 * leaves it, or a slope g' <= 1e-6 (the Stribeck side, where three roots can coexist), bisects instead. Starting on
 * the last F's side keeps the motion on its branch (stick stays stick until it cannot), which is the hysteresis
 * that makes stick-slip sing; it converges to |g| <= 1e-4 F_b in two or three evaluations nearly always, and in at
 * most 9 (the spec's "2 Newton, then 10 bisections" in one loop that wastes neither). |F| <= F_b mu_s by
 * construction; the slot's RMS governor and NaN guard (voice.c) sit behind it.
 *
 * The coupled set. Every active mode the bow reaches, |phi_k(bp)| L_k >= CY_BOW_PHI, not the spec's first 12 of
 * order[]: driving the others open-loop with x = F (the spec's EXCITED path) leaves them without the junction's
 * loading, so whichever sits near a harmonic of the stick-slip resonates up to the fundamental's level and the RMS
 * governor clamps the whole note (measured). Coupled, the junction pins them as it pins the low modes. A mode it
 * does not reach gets no bow input at all (and the slot does not ask the budget for it: engine.c). A mode the budget
 * is fading out leaves the junction at once and finishes its fade as a free mode. The bowed slots share half the
 * budget's modes as coupled modes (CY_BOW_SHARE): at the default budgets more than they reach, at the governor's
 * floor the lowest 6 each with four bows, so the governor's budget reaches the bow, whose cost it otherwise never
 * touched (a coupled mode-sample is 29 device instructions against a free mode's 16, plus ~150 a sample for the
 * junction: four bowed notes kept the device over its half at the budget's floor before).
 *
 * Mode selection. A real bowed plate (or glass, or saw) locks to one mode and sings; with the spec's beta 0.05 the
 * Stribeck slope out-drove every mode's own damping ~400x at once and they all saturated together into chatter. So:
 * beta 0.008 (scaled by sqrt(f_note) to keep that margin pitch-invariant), and a contact low-pass on the coupling,
 * L_k = 1 / (1 + (ratio_k / Rc)^2) with Rc = 1 + 3 grit, a bow hair's width and give: the low modes the bow favours
 * win and lock, and GRIT opens the coupling up toward growl and mode-jumping. On the real plates and the bowed
 * presets (09, 21, 23) every pitch and pressure then locks to the played note's harmonic series. Where the bow sits
 * on a mode's node it picks another mode: that is the Chladni demonstration, and the sand shows it. The bow's speed
 * also follows the lean (louder, not only brighter, when leaned on) and ramps in over a short catch, and a little
 * jitter (hair, rosin) seeds the oscillation; the slot's output runs through a band-limiter while and after it is
 * bowed (engine.c), since the stick-slip force has corners. */
#include "cy_engine_int.h"

/* per sub-block: the coupled set and the junction's constants (CY_HOT, call-free: voice.c's control path) */
CY_HOT void cy_bow_prep(cy_slot_t *sl, const float *phi_t, uint32_t n)
{
    cy_eng_t *e = &cy_e;
    const cy_object_t *o = e->obj;
    uint32_t j, k, nc = 0, wake = 0;
    float force = e->force, grit = e->grit, v0, sum2 = 0.0f, rcut = CY_BOW_RC0 + CY_BOW_RC1 * grit, irc = rcut * rcut;
    (void)n;
    /* the bow point's field glides (~5 ms) so a moving POS or a new SURFACE bow key cannot step the coupling
     * (engine.c sets it outright when a bow starts). Toward a field value of exactly 0 (a nodal line) the glide
     * would stall on a subnormal (0.15 x a subnormal rounds to itself): flushed */
    for (k = 0; k < CY_N; k++) {
        float bp = sl->bphi[k] + 0.15f * (phi_t[k] - sl->bphi[k]);
        sl->bphi[k] = CY_ABS(bp) < 1e-20f ? 0.0f : bp;   /* (fm_flush, written out: CY_HOT calls nothing) */
        if (CY_ABS(sl->bphi[k]) * irc >= CY_BOW_PHI * (irc + sl->rc[k] * sl->rc[k]))
            wake |= 1u << k;
    }
#if CY_BOW_MODEL
    sl->breach = wake;                                /* (engine.c cy_budget: the modes worth allowing a bowed slot) */
#endif
    /* bowed, every allowed mode the bow can reach is driven, up to the slot's share of coupled modes (engine.c:
     * the budget's half over the bowed slots, so the governor's budget reaches the bow too; a coupled mode costs
     * twice a free one, plus the junction's own). Those are woken (from zero, their gains at target); a reachable
     * mode past the share, the highest by order[], is left as it is: free, not driven */
    wake &= sl->allow;
    sl->cpl = 0;
#if CY_BOW_MODEL
    {
        uint32_t cap = e->bow_ccap, left = wake & ~sl->kill;
        for (j = 0; j < CY_N && nc < cap && left; j++) {   /* (stops once every reachable mode is placed) */
            float ch, sh, a;
            k = o->order[j];
            if (!(left >> k & 1u))
                continue;
            left &= ~(1u << k);
            sl->bidx[nc++] = (uint8_t)k;
            sl->cpl |= 1u << k;
            cy_sincos(0.5f * sl->w[k], &ch, &sh);
            a = sl->bbeta * sl->bphi[k] * irc / (irc + sl->rc[k] * sl->rc[k]);   /* contact low-pass 1 / (1 + (ratio/Rc)^2) */
            sl->b[k] = a * ch;
            sl->bh[k] = a * sh;
            sum2 += sl->bphi[k] * sl->b[k];
        }
    }
    wake = sl->cpl;
#else
    (void)j, (void)o;
#endif
    sl->fresh |= wake & ~sl->act;
    sl->act |= wake;
    sl->bnc = (uint8_t)nc;
    /* gate: 15 ms one-pole toward 1 while bowing, toward 0 once lifted */
    sl->bgate += 0.047f * ((sl->bhold ? 1.0f : 0.0f) - sl->bgate);
    sl->bY0 = sum2;                                   /* beta sum phi^2 cos(w/2) */
    /* bow speed: the spec's 0.12 (0.6 + 0.4 force), raised by the lean (0.35 + 0.65 P_eff^2: ~8 dB from a plain
     * press to a full lean; leaning in plays louder, not only brighter, which force alone cannot do once the plate
     * sings, since the stick-slip amplitude follows the bow's speed) and ramped in over the catch (min(SWELL,
     * 60 ms)), so a stroke starts in stick as a real one does */
    sl->bvb = 0.12f * (0.6f + 0.4f * force) * (0.35f + 0.65f * sl->Peff * sl->Peff) * sl->bsw;
    sl->bFb = 0.9f * force * sl->Peff * sl->bgate;
    /* jitter: the bow's speed wanders by a fraction of a percent sample to sample (hair, rosin), more with GRIT. It
     * is also what starts a note: a smooth press puts almost nothing at a mode's frequency, so without a seed a
     * weakly coupled mode would need 25 e-folds of growth before it was heard (measured: a G5 that never spoke) */
    sl->bjit = sl->bvb * (0.004f + 0.02f * grit);
    sl->bmud = 0.25f + 0.2f * (1.0f - grit);
    sl->bdmu = CY_BOW_MUS - sl->bmud;
    v0 = 0.004f + 0.03f * (1.0f - grit);
    sl->biv02 = 1.0f / (v0 * v0);
    /* RMS target for the governor (which acts at 4x): the bow-point velocity stick-slip reaches is about
     * v_b + dmu F_b Y0 (the velocity a full slip releases); spread over the coupled modes with the least energy
     * that is about v^2 beta / Y0; x4 headroom (calibrated on the bow sweep: the governor stays off in normal
     * playing) */
    {
        float v = sl->bvb + sl->bdmu * sl->bFb * sl->bY0 + 0.05f;
        sl->Etgt = CY_BOW_ETGT_K * v * v * sl->bbeta / cy_i_maxf(sl->bY0, 1e-4f);
    }
}

/* mu(d) and mu'(d) for the junction (a macro: CY_HOT may call nothing) */
#define CY_MU(d, MU, MUP)                                                                                           \
    do {                                                                                                            \
        float d2_ = (d) * (d), q_ = d2_ + eps2, rq_, den_, S_, a_;                                                  \
        fm_bits_t bb_;                                                                                              \
        bb_.f = q_;                                                                                                 \
        bb_.u = 0x5F375A86u - (bb_.u >> 1);                                                                         \
        rq_ = bb_.f;                                                                                                \
        rq_ = rq_ * (1.5f - 0.5f * q_ * rq_ * rq_);                                                                 \
        rq_ = rq_ * (1.5f - 0.5f * q_ * rq_ * rq_);                                                                 \
        a_ = (d) * rq_;                                                                                             \
        den_ = 1.0f / (1.0f + d2_ * iv02);                                                                          \
        S_ = mud + dmu * den_;                                                                                      \
        MU = a_ * S_;                                                                                               \
        MUP = eps2 * rq_ * rq_ * rq_ * S_ - 2.0f * dmu * (d) * iv02 * den_ * den_ * a_;                             \
    } while (0)

/* One sub-block of the junction, sample-major over the coupled modes (they must all see F at every sample),
 * compacted into cy_e.bw first. Writes their output into L/R. It calls nothing. */
CY_HOT void cy_bow_block(cy_slot_t *sl, float *L, float *R, uint32_t n)
{
    float *zr = cy_e.bw[0], *zi = cy_e.bw[1], *c = cy_e.bw[2], *s = cy_e.bw[3], *ph = cy_e.bw[4];
    float *br = cy_e.bw[5], *bi = cy_e.bw[6], *gl = cy_e.bw[7], *gr = cy_e.bw[8], *dl = cy_e.bw[9], *dr = cy_e.bw[10];
    float *gc = cy_e.bw[11], *gs = cy_e.bw[12];
    const float eps2 = CY_BOW_EPS * CY_BOW_EPS, mud = sl->bmud, dmu = sl->bdmu, iv02 = sl->biv02;
    const float Fb = sl->bFb, Y0 = sl->bY0, vb = sl->bvb, Fmax = Fb * CY_BOW_MUS, FbY0 = Fb * Y0, tol = 1e-4f * Fb;
    uint32_t i, j, nc = sl->bnc, slips = 0, bis = 0, evals = 0, ramp = sl->ramp, glm = sl->glm;
    uint32_t stick = sl->bstick, rng = cy_e.rng;
    const float jit = sl->bjit * (1.0f / 2147483648.0f);
    float F = CY_CLAMP(sl->bF, -Fmax, Fmax);
    for (j = 0; j < nc; j++) {
        uint32_t k = sl->bidx[j];
        zr[j] = sl->zr[k], zi[j] = sl->zi[k], c[j] = sl->c[k], s[j] = sl->s[k];
        ph[j] = sl->bphi[k], br[j] = sl->b[k], bi[j] = sl->bh[k];
        gl[j] = sl->gl[k], gr[j] = sl->gr[k];
        dl[j] = (ramp >> k & 1u) ? sl->dgl[k] : 0.0f;
        dr[j] = (ramp >> k & 1u) ? sl->dgr[k] : 0.0f;
        gc[j] = (glm >> k & 1u) ? sl->dc[k] : 0.0f;  /* the frequency glide (voice.c cy_slot_coeffs) */
        gs[j] = (glm >> k & 1u) ? sl->ds[k] : 0.0f;
    }
    /* The per-sample "rotate + v_h" pass of sample i+1 is fused onto the "inject + output" pass of sample i: the same
     * operations in the same order per mode, and the same summation order across modes for v_h, l and r, so it is
     * bit-exact with running them as two loops, and each mode's state is loaded and stored once a sample instead of
     * twice. When no coupled mode glides or ramps this sub-block (nearly always) the four zero-step adds go too:
     * 29 instructions a mode-sample on the device instead of 48 (review-perf; fix-dsp fused the moving case too). */
    {
        uint32_t moving = (ramp | glm) & sl->cpl;
        float vh = 0.0f;
        for (j = 0; j < nc; j++) {           /* sample 0's rotation */
            float t = c[j] * zr[j] - s[j] * zi[j];
            zi[j] = s[j] * zr[j] + c[j] * zi[j];
            zr[j] = t;
            vh += ph[j] * t;
        }
        for (i = 0; i < n; i++) {
            float l = 0.0f, r = 0.0f, vn = 0.0f, dv0, d;
            rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5;     /* cy_rand, inline: CY_HOT calls nothing */
            dv0 = vb + jit * (float)(int32_t)rng - vh;
            dv0 = CY_CLAMP(dv0, -1000.0f, 1000.0f);   /* (the friction curve is flat out there; keeps d^2 finite) */
            if (Fb > 1e-9f) {
                float g, gp, mu, mup, lo, hi;
                uint32_t it;
                d = dv0 - Y0 * F;
                CY_MU(d, mu, mup);
                g = F - Fb * mu;
                gp = 1.0f + FbY0 * mup;
                evals++;
                if (g > 0.0f)
                    lo = -Fmax, hi = F;
                else
                    lo = F, hi = Fmax;
                for (it = 0; it < 8u && CY_ABS(g) > tol; it++) {
                    float Fn = gp > 1e-6f ? F - g / gp : lo - 1.0f;
                    if (!(Fn > lo && Fn < hi)) {
                        Fn = 0.5f * (lo + hi);
                        bis++;
                    }
                    F = Fn;
                    d = dv0 - Y0 * F;
                    CY_MU(d, mu, mup);
                    g = F - Fb * mu;
                    gp = 1.0f + FbY0 * mup;
                    evals++;
                    if (g > 0.0f)
                        hi = F;
                    else
                        lo = F;
                }
                /* (no clamp needed: F starts in [-Fmax, Fmax], the bracket [lo, hi] never leaves it, and every step
                 * that would leave the bracket bisects instead; a NaN g ends the loop on the last finite F) */
            } else {
                F = 0.0f;
            }
            d = dv0 - Y0 * F;
            {
                uint32_t st = CY_ABS(d) < 4.0f * CY_BOW_EPS;
                slips += stick & (st ^ 1u);
                stick = st;
            }
            if (i + 1u < n) {
                if (moving) {                /* (gliding or ramping: the steps advance too) */
                    for (j = 0; j < nc; j++) {
                        float a = zr[j] + br[j] * F, b = zi[j] + bi[j] * F, cc = c[j] + gc[j], ss = s[j] + gs[j];
                        float g1 = gl[j] + dl[j], g2 = gr[j] + dr[j], t;
                        c[j] = cc, s[j] = ss, gl[j] = g1, gr[j] = g2;
                        l += g1 * b;
                        r += g2 * b;
                        t = cc * a - ss * b;
                        zi[j] = ss * a + cc * b;
                        zr[j] = t;
                        vn += ph[j] * t;
                    }
                } else {
                    for (j = 0; j < nc; j++) {
                        float a = zr[j] + br[j] * F, b = zi[j] + bi[j] * F, cc = c[j], ss = s[j], t;
                        l += gl[j] * b;
                        r += gr[j] * b;
                        t = cc * a - ss * b;
                        zi[j] = ss * a + cc * b;
                        zr[j] = t;
                        vn += ph[j] * t;
                    }
                }
            } else {
                for (j = 0; j < nc; j++) {
                    zr[j] += br[j] * F;
                    zi[j] += bi[j] * F;
                    c[j] += gc[j];
                    s[j] += gs[j];
                    gl[j] += dl[j];
                    gr[j] += dr[j];
                    l += gl[j] * zi[j];
                    r += gr[j] * zi[j];
                }
            }
            vh = vn;
            L[i] += l;
            R[i] += r;
        }
    }
    for (j = 0; j < nc; j++) {
        uint32_t k = sl->bidx[j];
        sl->zr[k] = zr[j], sl->zi[k] = zi[j], sl->gl[k] = gl[j], sl->gr[k] = gr[j];
        sl->c[k] = c[j], sl->s[k] = s[j];
    }
    cy_e.rng = rng;
    sl->bF = F;
    sl->bslips += slips;
    sl->bisect += bis;
    sl->bevals += evals;
    sl->bstick = (uint8_t)stick;
}

/* BREATH (CY_BOW_MODEL 0): white noise through a 2-pole band-pass at 2 f_note, Q 1.5, times 0.6 force P_eff,
 * into every mode with b_k = phi_k(bp). The level is normalised by the lowest mode's bandwidth so a long DECAY does
 * not turn the breath into a roar. */
void cy_breath_fill(cy_slot_t *sl, float *x, uint32_t n)
{
    cy_eng_t *e = &cy_e;
    float f = fm_minf(2.0f * cy_slot_freq0(sl), 0.4f * CY_FS), w0 = f * CY_TWO_PI_FS, cw, sw, al, a0i, b0, a1, a2;
    float g, x1 = sl->br_x1, x2 = sl->br_x2, y1 = sl->br_y1, y2 = sl->br_y2;
    uint32_t i, k0 = e->obj->order[0];
    cy_sincos(w0, &cw, &sw);
    al = sw * (1.0f / 3.0f);                         /* sin w0 / (2 Q), Q = 1.5 */
    a0i = 1.0f / (1.0f + al);
    b0 = al * a0i, a1 = -2.0f * cw * a0i, a2 = (1.0f - al) * a0i;
    g = 0.6f * e->force * sl->Peff * sl->bgate * fm_sqrtf(2.0f * fm_maxf(1.0f - sl->r[k0], 1e-7f)) * 30.0f;
    for (i = 0; i < n; i++) {
        float w = cy_rand_pm1(&e->rng), y = b0 * (w - x2) - a1 * y1 - a2 * y2;
        x2 = x1, x1 = w, y2 = y1, y1 = y;
        x[i] = g * y;
    }
    sl->br_x1 = fm_flush(x1), sl->br_x2 = fm_flush(x2), sl->br_y1 = fm_flush(y1), sl->br_y2 = fm_flush(y2);
}
