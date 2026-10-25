/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 parameters (M4): the table of spec §4.1-4.2, the curves, detent stepping with sticky stops, the value
 * formats, the page map of §5.2, the patch builder of §9.5 and the global LFO of §3.9.
 *
 * norm[] holds every parameter as 0..1, the only form that is stored, glided, lerped and morph-scrubbed. Display
 * units exist only at the edges: cy_param_value() for the DSP mapping and the readouts, cy_param_norm() for the
 * host's `param NAME VALUE`. So a preset glide is linear in norm, which for LOG parameters is geometric in value
 * (DECAY x0.5 -> x4 passes x1.41 halfway, as the ear expects).
 *
 * Curves and the detent grid. Each parameter has a grid of D steps across its norm range:
 *   LIN, LOG   D = detents                       value lo + n(hi-lo), lo (hi/lo)^n
 *   MORPH      D = detents (per segment) x (hi-lo)   value lo + n(hi-lo), keyframes at the integers
 *   ENUM       D = detents - 1 (detents = the number of values)   value lo + round(nD)(hi-lo)/D
 * so VOICES reads 4/6/8 and DETAIL 64/96/128 in display units (§4.2), and the labels index by round(nD).
 * MODES, VEL CURVE, PRESS SENS and GRAINS are integers in steps of (hi-lo)/D: their value is quantised, so a glide
 * moves them in whole steps and the engine never sees 23.6 modes.
 *
 * Stepping (cy_param_step) moves to the next grid point in the turn direction, so an off-grid value (a preset at
 * DECAY x1.6) lands on the grid on the first detent instead of carrying its offset forever. Two kinds of stop hold
 * the value while the knob keeps turning the same way:
 *   MORPH keyframes (every integer inside the range): 2 detents of dwell, so each shape and material can be felt;
 *   bipolar zero: 3 detents, where a detent is finer than 5% of the range (BRIGHT, BLOOM, TILT, TUNE). VEL CURVE
 *     and PRESS SENS step whole units of an 11-value range, where landing on 0 is already easy: no dwell.
 * The dwell lives in the caller's per-parameter `sticky` byte: bit 7 the direction it holds against, bits 0-6 the
 * detents left. Turning back leaves a stop at once; a sticky byte left over from a value that changed since (a
 * preset load) is ignored, because it only counts while the value sits exactly on its stop.
 *
 * Formats (cy_param_format). The short form is <= 7 glyphs and <= 52 px in FONT_CY_VAL (a knob cell's value runs
 * from x0+6 to at most x0+57, so x0+58 stays clear of the separator at x0+59). The long form is <= 23 chars
 * (app_say's buffers) for the focus pill and the readouts, and follows one rule the UI can split on:
 *   numeric  starts with one of "0123456789+-.*": the leading run of FONT_CY_HERO glyphs (" %+-.0123456789:*")
 *            is the number, the rest (from the first other char) is the unit or a word in FONT_CY_VAL:
 *            "*1.60", "62%", "+2.0st", "2.80s", "80% PLATE";
 *   text     anything else, upper case only (FONT_CY_MID has 32..95): "HARMONIC", "GLASS > STEEL 40%", "OFF".
 * '>' draws as '›' and '*' as '×' in the CY fonts. Hints are <= 34 chars, upper case, and <= 176 px untracked in
 * FONT_CY_LBL (with the knob labels' +1 px tracking a 34-char hint reaches ~205 px, wider than the plate box).
 *
 * No libc (the device has none in o2 units): the digits are written by hand, by subtraction, without division.
 * Main loop only: none of this runs in the audio ISR. */
#include "cy_params.h"
#include "fastmath.h"

static const char *const L_SHAPE[] = {"DRUM", "CIRCLE", "HEX", "SQUARE", "TRIANGLE", "STAR", "GUITAR", "STADIUM",
                                      "BAR"};
static const char *const L_EXC[] = {"MALLET", "BOW", "RAIN"};
static const char *const L_KEYS[] = {"TUNED", "SURFACE"};
static const char *const L_MAT[] = {"RUBBER", "WOOD", "GLASS", "STEEL", "BRONZE", "AETHER"};
static const char *const L_TARGET[] = {"HARMONIC", "BELL", "BAR"};
static const char *const L_WAVE[] = {"SINE", "TRI", "S&H", "DRIFT"};
static const char *const L_VOICES[] = {"4", "6", "8"};
static const char *const L_THEME[] = {"CYMATIC", "PHOSPHOR"};
static const char *const L_MOTION[] = {"FULL", "REDUCED"};
static const char *const L_DETAIL[] = {"64", "96", "128"};

/* short forms where a label is wider than a knob cell (52 px of FONT_CY_VAL) or longer than 7 glyphs */
static const char *const S_SHAPE[] = {"DRUM", "CIRCLE", "HEX", "SQUARE", "TRI", "STAR", "GUITAR", "STADIUM", "BAR"};
static const char *const S_KEYS[] = {"TUNED", "SURF"};
static const char *const S_TARGET[] = {"HARM", "BELL", "BAR"};
static const char *const S_THEME[] = {"CYMA", "PHOSPH"};
static const char *const S_MOTION[] = {"FULL", "REDUCE"};
/* between two keyframes the short form is "A>B": SHAPE's are cy_geom_shape_short's; WOOD is "WD" because a W is
 * 11 px and "RUB>WOD" would be 56 px */
static const char *const A_SHAPE[] = {"DRM", "CIR", "HEX", "SQR", "TRI", "STR", "GTR", "STD", "BAR"};
static const char *const A_MAT[] = {"RUB", "WD", "GLS", "STL", "BRZ", "AET"};

#define LIN CY_CV_LIN
#define LOG CY_CV_LOG
#define ENU CY_CV_ENUM
#define MOR CY_CV_MORPH
#ifdef PLAT_HOST                                  /* (the web build is PLAT_HOST too: it runs the host budget) */
#define CY_GRAINS_DEF 2000.0f
#define CY_DETAIL_DEF 128.0f
#else
#define CY_GRAINS_DEF 1200.0f
#define CY_DETAIL_DEF 96.0f
#endif
/* name, unit, lo, hi, def, curve, detents, bipolar, page, labels, hint. SHAPE, EXCITER and KEYS have their own
 * controls (SELECT, ALGORITHM, SEQ) and no knob page: page PG_N. */
const cy_param_def_t CY_PARAMS[P_NPARAMS] = {
    {"SHAPE", "", 0.0f, 8.0f, 1.0f, MOR, 16, 0, PG_N, L_SHAPE, "MELT THE PLATE INTO A NEW SHAPE"},
    {"EXCITER", "", 0.0f, 2.0f, 0.0f, ENU, 3, 0, PG_N, L_EXC, "STRIKE IT, BOW IT OR LET IT RAIN"},
    {"KEYS", "", 0.0f, 1.0f, 0.0f, ENU, 2, 0, PG_N, L_KEYS, "SEQ: THE KEYBED BECOMES THE PLATE"},
    {"MATERIAL", "", 0.0f, 5.0f, 3.0f, MOR, 16, 0, PG_PLATE, L_MAT, "RUBBER > BRONZE: HOW LONG IT RINGS"},
    {"STIFFNESS", "%", 0.0f, 100.0f, 80.0f, LIN, 100, 0, PG_PLATE, 0, "SKIN > PLATE > BELL: OVERTONE SPAN"},
    {"CARVE", "%", 0.0f, 100.0f, 0.0f, LIN, 100, 0, PG_PLATE, 0, "TUNE THE OVERTONES TO THE TARGET"},
    {"DECAY", "x", 0.1f, 10.0f, 1.0f, LOG, 100, 0, PG_PLATE, 0, "SCALES HOW LONG THE PLATE RINGS"},
    {"TARGET", "", 0.0f, 2.0f, 0.0f, ENU, 3, 0, PG_PLATE, L_TARGET, "CARVE TOWARD HARMONIC, BELL, BAR"},
    {"BRIGHT", "", -100.0f, 100.0f, 0.0f, LIN, 100, 1, PG_PLATE, 0, "+ HIGH MODES RING ON, - DIE FAST"},
    {"SHIMMER", "c", 0.0f, 15.0f, 3.0f, LIN, 30, 0, PG_PLATE, 0, "SLOW BEATING BETWEEN TWIN MODES"},
    {"MODES", "", 8.0f, 32.0f, 32.0f, LIN, 24, 0, PG_PLATE, 0, "PARTIALS PER NOTE: LESS IS PURER"},
    {"HARDNESS", "%", 0.0f, 100.0f, 55.0f, LIN, 100, 0, PG_EXCITE, 0, "SOFT FELT TO A HARD STEEL MALLET"},
    {"TENSION", "st", 0.0f, 7.0f, 2.0f, LIN, 70, 0, PG_EXCITE, 0, "LEAN IN: THE PITCH BENDS UP"},
    {"FORCE", "%", 0.0f, 100.0f, 60.0f, LIN, 100, 0, PG_EXCITE, 0, "HOW HARD THE BOW DIGS IN"},
    {"GRIT", "%", 0.0f, 100.0f, 40.0f, LIN, 100, 0, PG_EXCITE, 0, "ROSIN: A SMOOTH SONG TO A GROWL"},
    {"DENSITY", "%", 0.0f, 100.0f, 40.0f, LIN, 100, 0, PG_EXCITE, 0, "HOW HARD IT RAINS: LEAN TO POUR"},
    {"SPREAD", "%", 0.0f, 100.0f, 30.0f, LIN, 100, 0, PG_EXCITE, 0, "HOW WIDE THE DROPS FALL"},
    {"POS X", "%", 0.0f, 100.0f, 62.0f, LIN, 100, 0, PG_EXCITE, 0, "WHERE YOU STRIKE: NODES GO QUIET"},
    {"POS Y", "%", 0.0f, 100.0f, 40.0f, LIN, 100, 0, PG_EXCITE, 0, "WHERE YOU STRIKE: NODES GO QUIET"},
    {"NOISE", "%", 0.0f, 100.0f, 15.0f, LIN, 100, 0, PG_EXCITE, 0, "THE CLICK OF EACH CONTACT"},
    {"DAMPER", "%", 0.0f, 100.0f, 0.0f, LIN, 100, 0, PG_EXCITE, 0, "LET GO AND A FELT STOPS IT"},
    {"BLOOM", "", -100.0f, 100.0f, 0.0f, LIN, 100, 1, PG_EXCITE, 0, "+ PITCH DROPS AS IT FADES, - RISES"},
    {"PALM", "%", 0.0f, 100.0f, 30.0f, LIN, 100, 0, PG_EXCITE, 0, "LEAN IN: A PALM MUTES IT"},
    {"SWELL", "s", 0.005f, 2.0f, 0.25f, LOG, 100, 0, PG_EXCITE, 0, "HOW SLOWLY THE BOW OR RAIN BUILDS"},
    {"MICS", "%", 0.0f, 100.0f, 50.0f, LIN, 100, 0, PG_SPACE, 0, "STEREO WIDTH OF THE TWO MICS"},
    {"ROOM", "%", 0.0f, 100.0f, 30.0f, LIN, 100, 0, PG_SPACE, 0, "HOW MUCH OF THE ROOM YOU HEAR"},
    {"SIZE", "s", 0.3f, 12.0f, 2.8f, LOG, 100, 0, PG_SPACE, 0, "HOW LONG THE ROOM RINGS"},
    {"DRIVE", "dB", 0.0f, 24.0f, 0.0f, LIN, 48, 0, PG_SPACE, 0, "WARM SATURATION BEFORE THE ROOM"},
    {"PREDELAY", "ms", 0.0f, 60.0f, 12.0f, LIN, 60, 0, PG_SPACE, 0, "A GAP BEFORE THE ROOM ANSWERS"},
    {"DAMPING", "%", 20.0f, 100.0f, 55.0f, LIN, 80, 0, PG_SPACE, 0, "HIGH KEEPS THE ROOM'S TAIL BRIGHT"},
    {"TILT", "dB", -6.0f, 6.0f, 0.0f, LIN, 48, 1, PG_SPACE, 0, "DARKER OR BRIGHTER OVERALL"},
    {"LEVEL", "dB", -18.0f, 6.0f, 0.0f, LIN, 48, 0, PG_SPACE, 0, "THIS PRESET'S OUTPUT LEVEL"},
    {"WANDER", "%", 0.0f, 100.0f, 10.0f, LIN, 100, 0, PG_MOTION, 0, "EACH HIT LANDS IN A NEW SPOT"},
    {"MORPH", "%", 0.0f, 100.0f, 0.0f, LIN, 100, 0, PG_MOTION, 0, "THE LFO MELTS THE SHAPE"},
    {"LFO RATE", "Hz", 0.01f, 4.0f, 0.2f, LOG, 100, 0, PG_MOTION, 0, "SPEED OF THE SLOW MOTION"},
    {"STRETCH", "%", 0.0f, 100.0f, 0.0f, LIN, 100, 0, PG_MOTION, 0, "LEAN IN: THE METAL TIGHTENS"},
    {"LFO WAVE", "", 0.0f, 3.0f, 3.0f, ENU, 4, 0, PG_MOTION, L_WAVE, "SINE, TRIANGLE, STEPS OR DRIFT"},
    {"LFO>POS", "%", 0.0f, 100.0f, 0.0f, LIN, 100, 0, PG_MOTION, 0, "THE LFO CIRCLES THE STRIKE POINT"},
    {"VIBRATO", "c", 0.0f, 50.0f, 0.0f, LIN, 50, 0, PG_MOTION, 0, "PITCH WOBBLE DEPTH"},
    {"VIB RATE", "Hz", 0.5f, 8.0f, 5.0f, LOG, 60, 0, PG_MOTION, 0, "PITCH WOBBLE SPEED"},
    {"VEL CURVE", "", -5.0f, 5.0f, 0.0f, LIN, 10, 1, PG_GLOBAL, 0, "+ LOUD NEEDS A HARDER HIT"},
    {"PRESS SENS", "", -5.0f, 5.0f, 0.0f, LIN, 10, 1, PG_GLOBAL, 0, "+ FULL PRESSURE WITH LESS LEAN"},
    {"TUNE", "c", -50.0f, 50.0f, 0.0f, LIN, 100, 1, PG_GLOBAL, 0, "MASTER TUNING IN CENTS"},
    {"VOICES", "", 4.0f, 8.0f, 8.0f, ENU, 3, 0, PG_GLOBAL, L_VOICES, "MOST NOTES THAT RING AT ONCE"},
    {"THEME", "", 0.0f, 1.0f, 0.0f, ENU, 2, 0, PG_GLOBAL, L_THEME, "COLOURS: CYMATIC OR PHOSPHOR"},
    {"GRAINS", "", 600.0f, 2000.0f, CY_GRAINS_DEF, LIN, 14, 0, PG_GLOBAL, 0, "SAND GRAINS ON THE PLATE"},
    {"MOTION", "", 0.0f, 1.0f, 0.0f, ENU, 2, 0, PG_GLOBAL, L_MOTION, "REDUCED: A CALMER SCREEN"},
    {"DETAIL", "", 64.0f, 128.0f, CY_DETAIL_DEF, ENU, 3, 0, PG_GLOBAL, L_DETAIL, "MODE BUDGET: RICHNESS VS CPU"},
};

/* ------------------------------------------------------------- the grid --- */
static float cy_p_n(float n) { return n >= 0.0f ? (n <= 1.0f ? n : 1.0f) : 0.0f; }   /* clamp; NaN -> 0 */
static float cy_p_bi(float x) { return x >= -1.0f ? (x <= 1.0f ? x : 1.0f) : x < -1.0f ? -1.0f : 0.0f; }   /* NaN: 0 */

static int cy_p_grid(const cy_param_def_t *d)    /* D: grid steps across norm 0..1 */
{
    if (d->curve == CY_CV_ENUM)
        return d->detents > 1 ? d->detents - 1 : 1;
    if (d->curve == CY_CV_MORPH)
        return d->detents * (int)(d->hi - d->lo + 0.5f);
    return d->detents ? d->detents : 1;
}

static int cy_p_is_int(int id)                   /* whole steps of (hi-lo)/D in value */
{
    return id == P_MODES || id == P_VEL_CURVE || id == P_PRESS_SENS || id == P_GRAINS;
}

/* the stop kind of a grid point k: dwell detents (0 = not a stop) */
static int cy_p_stop(int id, const cy_param_def_t *d, int k, int D)
{
    if (d->curve == CY_CV_MORPH) {
        int seg = d->detents, j = k;
        if (k <= 0 || k >= D || seg <= 0)
            return 0;                             /* the ends clamp anyway */
        while (j >= seg)                          /* (k % seg without a divide; k <= 128) */
            j -= seg;
        return j == 0 ? 2 : 0;
    }
    if (d->bipolar && d->detents >= 20) {
        int zk = (int)(-d->lo / (d->hi - d->lo) * (float)D + 0.5f);
        return k == zk ? 3 : 0;
    }
    (void)id;
    return 0;
}

/* --------------------------------------------------------- value <-> norm --- */
float cy_param_value(int id, float norm)
{
    const cy_param_def_t *d;
    float n, v, r;
    int D;
    if ((unsigned)id >= (unsigned)P_NPARAMS)
        return 0.0f;
    d = &CY_PARAMS[id];
    n = cy_p_n(norm);
    D = cy_p_grid(d);
    switch (d->curve) {
    case CY_CV_LOG:
        v = d->lo * fm_exp2f(n * fm_log2f(d->hi / d->lo));
        return fm_clampf(v, d->lo, d->hi);
    case CY_CV_ENUM:
        return d->lo + fm_floorf(n * (float)D + 0.5f) * (d->hi - d->lo) / (float)D;
    case CY_CV_MORPH:                             /* a hair from a keyframe is the keyframe: presets park exactly */
        v = d->lo + n * (d->hi - d->lo);
        r = fm_floorf(v + 0.5f);
        return fm_fabsf(v - r) < 1e-4f ? r : v;
    default:
        v = d->lo + n * (d->hi - d->lo);
        if (cy_p_is_int(id)) {
            float step = (d->hi - d->lo) / (float)D;
            v = d->lo + fm_floorf((v - d->lo) / step + 0.5f) * step;
        }
        return fm_clampf(v, d->lo, d->hi);
    }
}

float cy_param_norm(int id, float value)
{
    const cy_param_def_t *d;
    float n;
    int D;
    if ((unsigned)id >= (unsigned)P_NPARAMS)
        return 0.0f;
    d = &CY_PARAMS[id];
    D = cy_p_grid(d);
    if (d->curve == CY_CV_LOG) {
        if (!(value > d->lo))                     /* (and NaN) */
            return 0.0f;
        n = fm_log2f(value / d->lo) / fm_log2f(d->hi / d->lo);
    } else {
        n = (value - d->lo) / (d->hi - d->lo);
    }
    n = cy_p_n(n);
    if (d->curve == CY_CV_ENUM || cy_p_is_int(id))   /* discrete: exactly the grid point that value reads as */
        n = fm_floorf(n * (float)D + 0.5f) / (float)D;
    return n;
}

/* ---------------------------------------------------------------- steps --- */
#define CY_STK_UP 0x80u
#define CY_STK_N 0x7Fu
float cy_param_step(int id, float norm, float detents, uint8_t *sticky)
{
    const cy_param_def_t *d;
    float n, x, mag;
    int D, dir, units, i;
    uint8_t stk = sticky ? *sticky : 0u;
    if ((unsigned)id >= (unsigned)P_NPARAMS)
        return cy_p_n(norm);
    d = &CY_PARAMS[id];
    n = cy_p_n(norm);
    if (!(detents > 0.0f || detents < 0.0f))      /* 0 or NaN: nothing to do */
        return n;
    dir = detents > 0.0f ? 1 : -1;
    mag = detents > 0.0f ? detents : -detents;
    D = cy_p_grid(d);
    if (d->curve == CY_CV_ENUM) {                 /* one value per detent, no dwell */
        int idx = (int)(n * (float)D + 0.5f), st = (int)(mag + 0.5f);
        if (st < 1)
            st = 1;
        idx += dir * st;
        idx = idx < 0 ? 0 : idx > D ? D : idx;
        if (sticky)
            *sticky = 0;
        return (float)idx / (float)D;
    }
    units = mag < 1000.0f ? (int)mag : 1000;
    x = n * (float)D;                             /* the position in grid steps */
    for (i = 0; i < units; i++) {
        int k = (int)(x + 0.5f), on = fm_fabsf(x - (float)k) < 1e-3f, dw;
        if (stk & CY_STK_N) {                     /* dwelling: hold while the knob keeps turning into the stop */
            if (on && cy_p_stop(id, d, k, D) && ((stk & CY_STK_UP) != 0) == (dir > 0)) {
                stk--;
                x = (float)k;
                continue;
            }
            stk = 0;                              /* turned back (or the value moved since): leave at once */
        }
        if (dir > 0)
            k = on ? k + 1 : (int)fm_floorf(x) + 1;
        else
            k = on ? k - 1 : -(int)fm_floorf(-x) - 1;
        k = k < 0 ? 0 : k > D ? D : k;
        x = (float)k;
        dw = cy_p_stop(id, d, k, D);
        if (dw)
            stk = (uint8_t)((dir > 0 ? CY_STK_UP : 0u) | (uint8_t)dw);
    }
    if (mag > (float)units && units < 1000) {     /* a fractional rest (an app's acceleration curve): no snap */
        x += (float)dir * (mag - (float)units);
        x = fm_clampf(x, 0.0f, (float)D);
        stk = 0;
    }
    if (sticky)
        *sticky = stk;
    return cy_p_n(x / (float)D);
}

/* ---------------------------------------------------------------- text --- */
static int cy_p_cat(char *buf, int at, int n, const char *s)
{
    while (s && *s && at < n - 1)
        buf[at++] = *s++;
    buf[at] = 0;
    return at;
}

/* |v| with dec decimals (0..3), a leading '-' when it rounds to non-zero, a '+' too if plus. Digits by
 * subtraction (values here are < 10^7 after scaling): no divide, no libc. */
static int cy_p_num(char *buf, int at, int n, float v, int dec, int plus)
{
    static const uint32_t SC[4] = {1u, 10u, 100u, 1000u};
    static const uint32_t PW[7] = {1000000u, 100000u, 10000u, 1000u, 100u, 10u, 1u};
    float a = v < 0.0f ? -v : v;
    uint32_t u;
    char t[16];
    int k = 0, j, started = 0;
    dec = dec < 0 ? 0 : dec > 3 ? 3 : dec;
    a = a * (float)SC[dec] + 0.5f;
    u = a < 9999999.0f ? (uint32_t)a : 9999999u;
    if (u && v < 0.0f)
        t[k++] = '-';
    else if (u && plus)
        t[k++] = '+';
    for (j = 0; j < 7; j++) {
        int place = 6 - j, dg = 0;
        while (u >= PW[j]) {
            u -= PW[j];
            dg++;
        }
        if (!started && !dg && place > dec)
            continue;
        started = 1;
        t[k++] = (char)('0' + dg);
        if (place == dec && dec)
            t[k++] = '.';
    }
    t[k] = 0;
    return cy_p_cat(buf, at, n, t);
}

/* SHAPE / MATERIAL: the keyframe's name, or the pair "HEX>SQR" (short) / "HEX > SQUARE 40%" (long) */
static void cy_p_morph(int id, const cy_param_def_t *d, float v, char *buf, int n, int longform)
{
    const char *const *ab = id == P_SHAPE ? A_SHAPE : A_MAT;
    const char *const *sh = id == P_SHAPE ? S_SHAPE : d->labels;
    int top = (int)(d->hi - d->lo + 0.5f), seg, at, pct;
    float t;
    v -= d->lo;
    seg = (int)fm_floorf(v);
    seg = seg < 0 ? 0 : seg > top ? top : seg;
    t = v - (float)seg;
    if (seg == top || t <= 0.0f) {
        cy_p_cat(buf, 0, n, longform ? d->labels[seg] : sh[seg]);
        return;
    }
    if (!longform) {
        at = cy_p_cat(buf, 0, n, ab[seg]);
        at = cy_p_cat(buf, at, n, ">");
        cy_p_cat(buf, at, n, ab[seg + 1]);
        return;
    }
    pct = (int)(t * 100.0f + 0.5f);               /* never "0%" or "100%" off a keyframe */
    pct = pct < 1 ? 1 : pct > 99 ? 99 : pct;
    at = cy_p_cat(buf, 0, n, d->labels[seg]);
    at = cy_p_cat(buf, at, n, " > ");
    at = cy_p_cat(buf, at, n, d->labels[seg + 1]);
    at = cy_p_cat(buf, at, n, " ");
    at = cy_p_num(buf, at, n, (float)pct, 0, 0);
    cy_p_cat(buf, at, n, "%");
}

void cy_param_format(int id, float norm, char *buf, int n, int longform)
{
    const cy_param_def_t *d;
    float v;
    int at = 0, dec = 0, plus = 0;
    if (!buf || n <= 0)
        return;
    buf[0] = 0;
    if ((unsigned)id >= (unsigned)P_NPARAMS)
        return;
    d = &CY_PARAMS[id];
    v = cy_param_value(id, norm);
    if (d->curve == CY_CV_MORPH) {
        cy_p_morph(id, d, v, buf, n, longform);
        return;
    }
    if (d->curve == CY_CV_ENUM) {
        const char *const *L = d->labels;
        int D = cy_p_grid(d), i = (int)((v - d->lo) / (d->hi - d->lo) * (float)D + 0.5f);
        i = i < 0 ? 0 : i > D ? D : i;
        if (!longform)
            L = id == P_KEYMODE ? S_KEYS : id == P_CARVE_TARGET ? S_TARGET : id == P_THEME ? S_THEME
              : id == P_MOTION ? S_MOTION : L;
        cy_p_cat(buf, 0, n, L ? L[i] : "");
        return;
    }
    switch (id) {
    case P_STIFFNESS: {                           /* the regime is the point: SKIN, PLATE, BELL+ (§4.1) */
        const char *w = v < 15.0f ? "SKIN" : (v >= 75.0f && v <= 85.0f) ? "PLATE" : v > 95.0f ? "BELL+" : 0;
        if (w && !longform) {
            cy_p_cat(buf, 0, n, w);
            return;
        }
        at = cy_p_num(buf, 0, n, v, 0, 0);
        at = cy_p_cat(buf, at, n, w ? "% " : "%");
        cy_p_cat(buf, at, n, w);
        return;
    }
    case P_DECAY:                                 /* "*1.6": a multiplier of the material's own ring */
        at = cy_p_cat(buf, 0, n, "*");
        if (longform)
            dec = v < 9.995f ? 2 : 1;
        else
            dec = v < 0.995f ? 2 : v < 9.95f ? 1 : 0;
        cy_p_num(buf, at, n, v, dec, 0);
        return;
    case P_DRIVE:                                 /* the drive stage is bypassed below 0.1 dB (§3.10) */
        if (v < 0.1f) {
            cy_p_cat(buf, 0, n, "OFF");
            return;
        }
        dec = 1;
        break;
    case P_SWELL:
    case P_LFO_RATE:                              /* LOG over 400x: enough digits to tell detents apart */
        dec = v < 0.0995f ? 3 : 2;
        break;
    case P_SIZE:
        dec = longform ? (v < 9.995f ? 2 : 1) : (v < 0.995f ? 2 : 1);
        break;
    case P_VIB_RATE:
        dec = longform ? 2 : (v < 0.995f ? 2 : 1);
        break;
    case P_TILT:                                  /* 0.25 dB detents: the long form shows them all */
        dec = longform ? 2 : 1;
        plus = 1;
        break;
    case P_TENSION:
        dec = 1;
        plus = 1;
        break;
    case P_LEVEL:
        dec = 1;
        plus = 1;
        break;
    case P_SHIMMER:
        dec = 1;
        break;
    default:                                      /* %, ms, c, plain integers; bipolar ones signed */
        plus = d->bipolar;
        break;
    }
    at = cy_p_num(buf, 0, n, v, dec, plus);
    cy_p_cat(buf, at, n, d->unit);
}

/* ---------------------------------------------------------------- pages --- */
const char *cy_page_name(int page)
{
    static const char *const N[PG_N] = {"PLATE", "EXCITE", "SPACE", "MOTION", "GLOBAL"};
    return N[page < 0 ? 0 : page >= PG_N ? PG_N - 1 : page];
}

/* spec §5.2: [page][layer][knob]; EXCITE's A1, A4 and B4 follow the exciter */
static const int8_t CY_PAGE_MAP[PG_N][2][4] = {
    {{P_MATERIAL, P_STIFFNESS, P_CARVE, P_DECAY}, {P_CARVE_TARGET, P_BRIGHT, P_SHIMMER, P_MODES}},
    {{P_HARDNESS, P_POS_X, P_POS_Y, P_TENSION}, {P_NOISE, P_DAMPER, P_BLOOM, P_PALM}},
    {{P_MICS, P_ROOM, P_SIZE, P_DRIVE}, {P_PREDELAY, P_DAMPING, P_TILT, P_LEVEL}},
    {{P_WANDER, P_MORPH_DEPTH, P_LFO_RATE, P_STRETCH}, {P_LFO_WAVE, P_LFO_POS, P_VIBRATO, P_VIB_RATE}},
    {{P_VEL_CURVE, P_PRESS_SENS, P_TUNE, P_VOICES}, {P_THEME, P_GRAINS, P_MOTION, P_DETAIL}},
};
static const int8_t CY_EXCITE_A1[CY_NEXC] = {P_HARDNESS, P_FORCE, P_DENSITY};
static const int8_t CY_EXCITE_A4[CY_NEXC] = {P_TENSION, P_GRIT, P_SPREAD};
static const int8_t CY_EXCITE_B4[CY_NEXC] = {P_PALM, P_SWELL, P_SWELL};

int cy_page_param(int page, int exciter, int layer, int knob)
{
    int l = layer ? 1 : 0, x = exciter < 0 || exciter >= CY_NEXC ? CY_EXC_MALLET : exciter;
    if (page < 0 || page >= PG_N || knob < 0 || knob > 3)
        return -1;
    if (page == PG_EXCITE) {
        if (!l && knob == 0)
            return CY_EXCITE_A1[x];
        if (!l && knob == 3)
            return CY_EXCITE_A4[x];
        if (l && knob == 3)
            return CY_EXCITE_B4[x];
    }
    return CY_PAGE_MAP[page][l][knob];
}

/* ---------------------------------------------------------------- patch --- */
/* spec §9.5: norm -> the engine's patch and the geometry's input. Percentages are 0..1, BLOOM and BRIGHT -1..1,
 * DAMPING 0.2..1. Pure: it only maps; every value the DSP receives is smoothed there (§2.4.7), and the geometry
 * glides its ratios (§3.3). Called each frame (60 Hz) and by cy_demo_hash. */
void cy_patch_build(const float *norm, const cy_mod_t *mod, cy_patch_t *p, cy_geom_in_t *g)
{
#define V(id) cy_param_value((id), norm[(id)])
#define PC(id) (V(id) / 100.0f)                      /* % -> 0..1: correctly rounded, so 20 % is 0.2f exactly */
    static const cy_mod_t none = {0.0f, 0.0f, 0.0f, 0.0f, 0};
    float lfo, ou, ov, orbit;
    int oct;
    if (!mod)
        mod = &none;
    lfo = cy_p_bi(mod->lfo);
    ou = cy_p_bi(mod->orbit_u);
    ov = cy_p_bi(mod->orbit_v);
    oct = mod->octave < -3 ? -3 : mod->octave > 3 ? 3 : mod->octave;
    orbit = 0.25f * PC(P_LFO_POS);

    g->shape = fm_clampf(V(P_SHAPE) + 2.0f * PC(P_MORPH_DEPTH) * lfo, 0.0f, 8.0f);
    g->stiffness = PC(P_STIFFNESS);
    g->carve = PC(P_CARVE);
    g->carve_target = (uint8_t)V(P_CARVE_TARGET);
    g->surface = (uint8_t)V(P_KEYMODE);
    g->rsv[0] = g->rsv[1] = 0;
    g->shimmer_cents = V(P_SHIMMER);
    g->material = V(P_MATERIAL);
    g->bright = PC(P_BRIGHT);
    g->pos_u = fm_clampf(PC(P_POS_X) + orbit * ou, 0.0f, 1.0f);   /* the orbit stays on the plate square */
    g->pos_v = fm_clampf(PC(P_POS_Y) + orbit * ov, 0.0f, 1.0f);
    g->mic_spread = PC(P_MICS);
    g->rain_spread = PC(P_SPREAD);
    g->wander = PC(P_WANDER);

    p->exciter = (uint8_t)V(P_EXCITER);
    p->surface = g->surface;
    p->max_voices = (uint8_t)V(P_VOICES);         /* 4 / 6 / 8 */
    p->modes_per_voice = (uint8_t)V(P_MODES);     /* an integer already */
    p->decay_mul = V(P_DECAY);
    p->hardness = PC(P_HARDNESS);
    p->tension_st = V(P_TENSION);
    p->palm = PC(P_PALM);
    p->force = PC(P_FORCE);
    p->grit = PC(P_GRIT);
    p->swell_s = V(P_SWELL);
    p->density = PC(P_DENSITY);
    p->noise = PC(P_NOISE);
    p->damper = PC(P_DAMPER);
    p->bloom = PC(P_BLOOM);
    p->stretch = PC(P_STRETCH);
    p->vibrato_cents = V(P_VIBRATO);
    p->vib_rate_hz = V(P_VIB_RATE);
    p->tune_cents = V(P_TUNE);
    p->level_db = V(P_LEVEL);
    p->surface_root = 48.0f + 12.0f * (float)oct;
    p->drive_db = V(P_DRIVE);
    p->tilt_db = V(P_TILT);
    p->room = PC(P_ROOM);
    p->size_s = V(P_SIZE);
    p->damping = PC(P_DAMPING);            /* 20..100 % -> 0.2..1.0 */
    p->predelay_ms = V(P_PREDELAY);
#undef PC
#undef V
}

/* ------------------------------------------------------------------ LFO --- */
/* spec §3.9: the global LFO, stepped by the main loop (dt in seconds); returns -1..1.
 *   SINE, TRI  in phase with each other: 0 at phase 0, rising, +1 at 1/4. So a fresh LFO leaves SHAPE where it is.
 *   S&H        a new random level each cycle (state[0]); 0 until the first wrap.
 *   DRIFT      smoothed random: a smoothstep from state[0] to state[1] across each cycle, then a new target.
 *              C1-continuous (it eases into each point), so MORPH never jerks the geometry.
 * *rng is the app's modulation stream (xorshift32, fixed seed): deterministic, apart from the engine's and UI's. */
float cy_lfo_step(float *phase, uint32_t *rng, float *state, int wave, float rate_hz, float dt)
{
    static float spare[2];
    float ph, s;
    int wrapped;
    if (!state)
        state = spare;
    if (!(dt > 0.0f))
        dt = 0.0f;
    dt = dt < 1.0f ? dt : 1.0f;                   /* a stalled frame skips at most a second */
    rate_hz = fm_clampf(rate_hz, 0.0f, 50.0f);
    ph = *phase;
    if (!(ph >= 0.0f && ph < 1.0f))
        ph = 0.0f;
    ph += rate_hz * dt;
    wrapped = ph >= 1.0f;
    if (wrapped)
        ph -= fm_floorf(ph);
    *phase = ph;
    if (*rng == 0u)                               /* xorshift's one fixed point */
        *rng = 0x6C8E9CF5u;
    switch (wave) {
    case CY_LFO_SINE:                             /* (the polynomial peaks an ulp past 1) */
        return fm_clampf(fm_sin_turns(ph), -1.0f, 1.0f);
    case CY_LFO_TRI:
        return ph < 0.25f ? 4.0f * ph : ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f;
    case CY_LFO_SH:
        if (wrapped)
            state[0] = 2.0f * cy_randf(rng) - 1.0f;
        return state[0];
    default:
        if (state[0] == 0.0f && state[1] == 0.0f) /* fresh: aim somewhere now, not one silent cycle later */
            state[1] = 2.0f * cy_randf(rng) - 1.0f;
        if (wrapped) {
            state[0] = state[1];
            state[1] = 2.0f * cy_randf(rng) - 1.0f;
        }
        s = ph * ph * (3.0f - 2.0f * ph);
        return state[0] + (state[1] - state[0]) * s;
    }
}
