/* SPDX-License-Identifier: MPL-2.0 */
/* CYMA-1 presets (M4): the 24 factory objects of spec §4.4, and load, store, lerp and the defaults.
 *
 * A preset is 14 name bytes + uint16 v[40] = norm x 65535 (§4.3): 94 B, const in flash. The table below is written
 * in display units, exactly as §4.4 voices it ("DECAY *1.6", "POS (58,44)"), and the macros turn each value into
 * its norm at compile time, so the source is the voicing and there is no generated blob to keep in step:
 *   - each preset starts from CY_DEF, the 40 defaults of §4.1 in order, then lists only what differs, as C99
 *     designated initializers (a later initializer of the same element overrides an earlier one, C99 6.7.8p19);
 *   - N_<PARAM>(v) repeats params.c's range and curve per parameter (tests/unit/test_params.c checks every one
 *     against cy_param_norm, and every preset against §4.4);
 *   - LOG needs log2 in a constant expression: CY_LOG2 takes the octave by comparisons and the mantissa m in [1,2)
 *     by 2/ln2 atanh((m-1)/(m+1)) to t^9 (error < 2e-6 in log2, under 0.03 of the 16-bit step). The compiler folds
 *     it; nothing of it reaches the binary, and every literal is float (no double, §2.4.1).
 * Factory names are at most 14 letters (CATHEDRAL GONG is exactly 14 and has no terminator, as app.c expects);
 * "BOWED GLASS PAD" (15) is shortened to BOWED GLASS.
 *
 * LEVEL (§4.4: "M4 and M7 may adjust LEVEL so a velocity-100 C4 strike sits within +-4 dB"). Measured on the host
 * with the engine, fx and geometry of 03:40 (Phase 1, still in progress): C4 velocity 100, held 1.5 s, three strikes
 * isolated by PANIC and 15 s of silence, the power mean of each strike's loudest 100 ms (both channels). Presets more
 * than 3 dB under the median of the others came up to it; those more than 3 dB over came down only to median + 3 dB
 * (inside +-4, and as much peak headroom as possible for M8's -30 dBFS gate: that engine sits 6-10 dB under §3.11's
 * -18 dBFS strike). On LEVEL's 0.5 dB grid; the rest keep §4.4's value, as does MEMBRANE KICK's +2 (it plays two
 * octaves down). Provisional: re-measure when the engine is final (the probe script and table are in
 * build/evidence/params/).
 *
 * Integration voicing (M7, against the final engine, fx and geometry; golden.py's metric, the loudest 50 ms RMS of a
 * C4 velocity-100 strike, both channels): every preset now sits within +-2.7 dB of the median.
 *   - The BAR's "missing fundamental" was two things. (1) Its tracked mode 1 (the fundamental) is antisymmetric
 *     about the bar's centre, the mirrored mics heard it in opposite phase and its far-field signs were opposite
 *     too, so it cancelled exactly in mono (fixed in geom.c). (2) In this Laplacian model the bar's fundamental is
 *     cos(pi x), with its node at the centre where §4.4 strikes MARIMBA (48,50) (strike gain 0.07): C4 sounded at its
 *     3.85x partial. MARIMBA strikes at (20,50) (fundamental 0.86, 4x partial 0.48, 10x at its node); VIBES at (16,50)
 *     (0.94 / 0.75 / 0.48). Both then measured -8.7 / -5.2 dB and take LEVEL +6 / +4.
 *   - The STAR's fundamental (tracked mode 1 again) has its node on the u = .5 axis, where §4.4 strikes GAMELAN STAR
 *     (50,30) (strike 0.00: C4 sounded as C5) and near where it strikes GLASS LOTUS (55,35) (0.10: the 1.24x partial,
 *     a third up, carried the pitch). Both strike in the right arm now: GAMELAN STAR (68,40) (fundamental 0.57, the
 *     3.9x ombak pair 0.26 / 0.32), GLASS LOTUS (66,44) (0.49, the 1.24x partial 0.17).
 *   - FELT PLATE HARDNESS 40 (M4's 25 measured -4.0 dB already at LEVEL +6); TEMPLE BELL HARDNESS 22 (spec 15: the
 *     beam's 4.8 ms contact filtered its C4 hum to -7 dB at LEVEL +6).
 *   - LEVEL: GLASS LOTUS +2, CATHEDRAL GONG +6 (was +4.5), KALIMBA TINE +2.5, BOWED SQUARE +5.5; AETHER -2 (was +4:
 *     +6.4 dB over the median, and its 12 s tail masked the next preset's onset in tests/presets.txt).
 *
 * Review voicing (fix-app): AETHER MORPH 60 -> 25. The LFO sweeps SHAPE by 2 x MORPH x lfo, so §4.4's 60 % took
 * SHAPE 1.5 across 0.3..2.7: 70 % of the way into DRUM (another overtone family) at each trough and 70 % into SQUARE
 * at each crest. 25 % is exactly 1.0..2.0, CIRCLE to HEX, the "slowly morphs between circle and hexagon" §4.4
 * describes. */
#include "cy_params.h"
#include "fastmath.h"

#if defined(__clang__)                            /* CY_DEF then overrides is the design; a 14-letter name is legal */
#pragma clang diagnostic ignored "-Winitializer-overrides"
#if __has_warning("-Wunterminated-string-initialization")
#pragma clang diagnostic ignored "-Wunterminated-string-initialization"
#endif
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Woverride-init"
#if __GNUC__ >= 15
#pragma GCC diagnostic ignored "-Wunterminated-string-initialization"
#endif
#endif

/* ------------------------------------------------- compile-time norms --- */
#define CY_OCT(x) ((float)(((x) >= 2.0f) + ((x) >= 4.0f) + ((x) >= 8.0f) + ((x) >= 16.0f) + ((x) >= 32.0f) + \
                           ((x) >= 64.0f) + ((x) >= 128.0f) + ((x) >= 256.0f) + ((x) >= 512.0f)))
#define CY_P2(x) (((x) >= 2.0f ? 2.0f : 1.0f) * ((x) >= 4.0f ? 2.0f : 1.0f) * ((x) >= 8.0f ? 2.0f : 1.0f) * \
                  ((x) >= 16.0f ? 2.0f : 1.0f) * ((x) >= 32.0f ? 2.0f : 1.0f) * ((x) >= 64.0f ? 2.0f : 1.0f) * \
                  ((x) >= 128.0f ? 2.0f : 1.0f) * ((x) >= 256.0f ? 2.0f : 1.0f) * ((x) >= 512.0f ? 2.0f : 1.0f))
#define CY_AT(t) (2.885390082f * (t) * (1.0f + (t) * (t) * (0.333333333f + (t) * (t) * (0.2f + (t) * (t) * \
                  (0.142857143f + (t) * (t) * 0.111111111f)))))
#define CY_L2M(m) CY_AT(((m) - 1.0f) / ((m) + 1.0f))
#define CY_LOG2(x) (CY_OCT(x) + CY_L2M((x) / CY_P2(x)))          /* x in [1, 1024) */
#define CY_NLIN(v, lo, hi) (((float)(v) - (lo)) / ((hi) - (lo)))
#define CY_NLOG(v, lo, hi) (CY_LOG2((float)(v) / (lo)) / CY_LOG2((hi) / (lo)))
#define CY_U16(n) ((uint16_t)((n) * 65535.0f + 0.5f))             /* n in [0, 1]: no clamp needed */

/* per parameter: params.c's lo, hi and curve (ENUM: the value, as cy_param_value reads it) */
#define N_SHAPE(v) CY_U16(CY_NLIN(v, 0.0f, 8.0f))
#define N_EXCITER(v) CY_U16(CY_NLIN(v, 0.0f, 2.0f))
#define N_KEYMODE(v) CY_U16(CY_NLIN(v, 0.0f, 1.0f))
#define N_MATERIAL(v) CY_U16(CY_NLIN(v, 0.0f, 5.0f))
#define N_STIFFNESS(v) CY_U16(CY_NLIN(v, 0.0f, 100.0f))
#define N_CARVE(v) CY_U16(CY_NLIN(v, 0.0f, 100.0f))
#define N_DECAY(v) CY_U16(CY_NLOG(v, 0.1f, 10.0f))
#define N_CARVE_TARGET(v) CY_U16(CY_NLIN(v, 0.0f, 2.0f))
#define N_BRIGHT(v) CY_U16(CY_NLIN(v, -100.0f, 100.0f))
#define N_SHIMMER(v) CY_U16(CY_NLIN(v, 0.0f, 15.0f))
#define N_MODES(v) CY_U16(CY_NLIN(v, 8.0f, 32.0f))
#define N_HARDNESS(v) CY_U16(CY_NLIN(v, 0.0f, 100.0f))
#define N_TENSION(v) CY_U16(CY_NLIN(v, 0.0f, 7.0f))
#define N_FORCE(v) CY_U16(CY_NLIN(v, 0.0f, 100.0f))
#define N_GRIT(v) CY_U16(CY_NLIN(v, 0.0f, 100.0f))
#define N_DENSITY(v) CY_U16(CY_NLIN(v, 0.0f, 100.0f))
#define N_SPREAD(v) CY_U16(CY_NLIN(v, 0.0f, 100.0f))
#define N_POS_X(v) CY_U16(CY_NLIN(v, 0.0f, 100.0f))
#define N_POS_Y(v) CY_U16(CY_NLIN(v, 0.0f, 100.0f))
#define N_NOISE(v) CY_U16(CY_NLIN(v, 0.0f, 100.0f))
#define N_DAMPER(v) CY_U16(CY_NLIN(v, 0.0f, 100.0f))
#define N_BLOOM(v) CY_U16(CY_NLIN(v, -100.0f, 100.0f))
#define N_PALM(v) CY_U16(CY_NLIN(v, 0.0f, 100.0f))
#define N_SWELL(v) CY_U16(CY_NLOG(v, 0.005f, 2.0f))
#define N_MICS(v) CY_U16(CY_NLIN(v, 0.0f, 100.0f))
#define N_ROOM(v) CY_U16(CY_NLIN(v, 0.0f, 100.0f))
#define N_SIZE(v) CY_U16(CY_NLOG(v, 0.3f, 12.0f))
#define N_DRIVE(v) CY_U16(CY_NLIN(v, 0.0f, 24.0f))
#define N_PREDELAY(v) CY_U16(CY_NLIN(v, 0.0f, 60.0f))
#define N_DAMPING(v) CY_U16(CY_NLIN(v, 20.0f, 100.0f))
#define N_TILT(v) CY_U16(CY_NLIN(v, -6.0f, 6.0f))
#define N_LEVEL(v) CY_U16(CY_NLIN(v, -18.0f, 6.0f))
#define N_WANDER(v) CY_U16(CY_NLIN(v, 0.0f, 100.0f))
#define N_MORPH_DEPTH(v) CY_U16(CY_NLIN(v, 0.0f, 100.0f))
#define N_LFO_RATE(v) CY_U16(CY_NLOG(v, 0.01f, 4.0f))
#define N_STRETCH(v) CY_U16(CY_NLIN(v, 0.0f, 100.0f))
#define N_LFO_WAVE(v) CY_U16(CY_NLIN(v, 0.0f, 3.0f))
#define N_LFO_POS(v) CY_U16(CY_NLIN(v, 0.0f, 100.0f))
#define N_VIBRATO(v) CY_U16(CY_NLIN(v, 0.0f, 50.0f))
#define N_VIB_RATE(v) CY_U16(CY_NLOG(v, 0.5f, 8.0f))

/* §4.1's defaults, P_SHAPE .. P_VIB_RATE in enum order (test_params.c checks them against CY_PARAMS[].def) */
#define CY_DEF                                                                                                     \
    N_SHAPE(1), N_EXCITER(CY_EXC_MALLET), N_KEYMODE(0), N_MATERIAL(3), N_STIFFNESS(80), N_CARVE(0), N_DECAY(1),   \
    N_CARVE_TARGET(CY_CARVE_HARMONIC), N_BRIGHT(0), N_SHIMMER(3), N_MODES(32), N_HARDNESS(55), N_TENSION(2),     \
    N_FORCE(60), N_GRIT(40), N_DENSITY(40), N_SPREAD(30), N_POS_X(62), N_POS_Y(40), N_NOISE(15), N_DAMPER(0),     \
    N_BLOOM(0), N_PALM(30), N_SWELL(0.25f), N_MICS(50), N_ROOM(30), N_SIZE(2.8f), N_DRIVE(0), N_PREDELAY(12),     \
    N_DAMPING(55), N_TILT(0), N_LEVEL(0), N_WANDER(10), N_MORPH_DEPTH(0), N_LFO_RATE(0.2f), N_STRETCH(0),         \
    N_LFO_WAVE(CY_LFO_DRIFT), N_LFO_POS(0), N_VIBRATO(0), N_VIB_RATE(5)

#define V(P, v) [P_##P] = N_##P(v)
#define POS(x, y) V(POS_X, x), V(POS_Y, y)
#define MALLET V(EXCITER, CY_EXC_MALLET)
#define BOW V(EXCITER, CY_EXC_BOW)
#define RAIN V(EXCITER, CY_EXC_RAIN)
#define HARM V(CARVE_TARGET, CY_CARVE_HARMONIC)
#define BELL V(CARVE_TARGET, CY_CARVE_BELL)
#define BAR V(CARVE_TARGET, CY_CARVE_BAR)
#define CY_PRESET(name, ...) {name, {CY_DEF, __VA_ARGS__}}

/* spec §4.4, in display units; anything not listed is the default */
const cy_preset_t CY_FACTORY[CY_NFACTORY] = {
    /* 01 warm bronze bowl, soft suede mallet, slow beating pair, long bloom; lean in and it sings a little sharp */
    CY_PRESET("SINGING BOWL", V(SHAPE, 1), MALLET, V(MATERIAL, 4.0f), V(STIFFNESS, 80), V(DECAY, 1.6f),
              V(SHIMMER, 6), V(HARDNESS, 30), POS(58, 44), V(TENSION, 0.8f), V(PALM, 10), V(MICS, 60), V(ROOM, 35),
              V(SIZE, 4.0f), V(LEVEL, 3.5f)),
    /* 02 steel shell tuned fundamental / octave / fifth: a warm "ding" with skin contact */
    CY_PRESET("HANDPAN", V(SHAPE, 0), MALLET, V(MATERIAL, 3.0f), V(STIFFNESS, 55), V(CARVE, 85), HARM,
              V(DECAY, 1.2f), V(HARDNESS, 45), V(NOISE, 30), V(BLOOM, 10), POS(55, 50), V(ROOM, 28), V(SIZE, 2.6f),
              V(LEVEL, -4)),
    /* 03 five-armed glass star: bright, sparkling, wide (POS (66,44): see the integration note above) */
    CY_PRESET("GLASS LOTUS", V(SHAPE, 5), MALLET, V(MATERIAL, 2.0f), V(STIFFNESS, 85), V(CARVE, 30), BELL,
              V(DECAY, 1.4f), V(SHIMMER, 4), V(HARDNESS, 85), POS(66, 44), V(MICS, 85), V(ROOM, 45), V(SIZE, 5.0f),
              V(PREDELAY, 20), V(LEVEL, 2)),
    /* 04 rosewood bar undercut to 1:4:10, warm and woody (POS (20,50), not (48,50): see the integration note above) */
    CY_PRESET("MARIMBA", V(SHAPE, 8), MALLET, V(MATERIAL, 1.0f), V(STIFFNESS, 80), V(CARVE, 90), BAR,
              V(DECAY, 0.9f), V(HARDNESS, 50), POS(20, 50), V(ROOM, 20), V(SIZE, 1.6f), V(TILT, 1), V(LEVEL, 6)),
    /* 05 aluminium bars with a felt damper: let go and it stops (POS (16,50): see the integration note above) */
    CY_PRESET("VIBES", V(SHAPE, 8), MALLET, V(MATERIAL, 2.6f), V(STIFFNESS, 80), V(CARVE, 90), BAR,
              V(DECAY, 2.5f), V(HARDNESS, 60), V(DAMPER, 85), V(SHIMMER, 2), V(ROOM, 30), V(SIZE, 2.2f), POS(16, 50), V(LEVEL, 4)),
    /* 06 loaded skin tuned harmonic; lean to bend up like a bayan heel: it talks */
    CY_PRESET("TABLA SKIN", V(SHAPE, 0), MALLET, V(MATERIAL, 0.6f), V(STIFFNESS, 0), V(CARVE, 70), HARM,
              V(DECAY, 0.7f), V(HARDNESS, 65), V(TENSION, 5.0f), V(PALM, 50), V(BLOOM, 25), POS(62, 46),
              V(ROOM, 15), V(SIZE, 1.2f)),
    /* 07 huge bronze gong, big soft beater; opera-gong pitch rises as it decays. Play low. */
    CY_PRESET("CATHEDRAL GONG", V(SHAPE, 1), MALLET, V(MATERIAL, 4.3f), V(STIFFNESS, 95), V(DECAY, 3.0f),
              V(HARDNESS, 20), V(BLOOM, -35), V(SHIMMER, 8), POS(60, 42), V(ROOM, 50), V(SIZE, 8.0f),
              V(PREDELAY, 30), V(DAMPING, 45), V(LEVEL, 6)),
    /* 08 rain on a steel hexagon; press harder and it pours */
    CY_PRESET("STEEL RAIN", V(SHAPE, 2), RAIN, V(MATERIAL, 3.0f), V(STIFFNESS, 80), V(DENSITY, 45), V(SPREAD, 60),
              V(SWELL, 0.4f), V(MICS, 90), V(ROOM, 45), V(SIZE, 4.0f), V(LEVEL, -2)),
    /* 09 bowed glass square: the classic Chladni demo, sustained and singing */
    CY_PRESET("BOWED GLASS", V(SHAPE, 3), BOW, V(MATERIAL, 2.0f), V(STIFFNESS, 80), V(CARVE, 20), HARM,
              V(DECAY, 2.0f), V(FORCE, 60), V(GRIT, 30), V(SWELL, 0.6f), POS(12, 50), V(ROOM, 55), V(SIZE, 6.0f),
              V(LEVEL, 4.5f)),
    /* 10 bronze star with ombak beating and bright bar-like overtones (POS (68,40): see the integration note) */
    CY_PRESET("GAMELAN STAR", V(SHAPE, 5), MALLET, V(MATERIAL, 4.0f), V(STIFFNESS, 90), V(CARVE, 50), BAR,
              V(SHIMMER, 12), V(DECAY, 1.8f), V(HARDNESS, 60), POS(68, 40), V(ROOM, 35), V(SIZE, 3.0f)),
    /* 11 triangle plate carved to bell tuning (hum, prime, tierce) */
    CY_PRESET("PRISM BELLS", V(SHAPE, 4), MALLET, V(MATERIAL, 2.6f), V(STIFFNESS, 100), V(CARVE, 80), BELL,
              V(DECAY, 2.0f), V(HARDNESS, 75), V(SHIMMER, 3), V(MICS, 70), V(ROOM, 40), V(SIZE, 4.0f), V(LEVEL, -0.5f)),
    /* 12 a knock on a spruce guitar body: short, woody, percussive */
    CY_PRESET("GUITAR BODY", V(SHAPE, 6), MALLET, V(MATERIAL, 1.0f), V(STIFFNESS, 55), V(DECAY, 0.6f),
              V(HARDNESS, 90), V(NOISE, 35), POS(50, 72), V(ROOM, 15), V(SIZE, 1.2f)),
    /* 13 rain in a chaotic billiard: inharmonic shimmer clouds drift around */
    CY_PRESET("CHAOS STADIUM", V(SHAPE, 7), RAIN, V(MATERIAL, 3.0f), V(DECAY, 1.3f), V(DENSITY, 25),
              V(SPREAD, 90), V(WANDER, 50), V(LFO_POS, 30), V(LFO_RATE, 0.12f), V(ROOM, 50), V(SIZE, 7.0f),
              V(LEVEL, -2.5f)),
    /* 14 muted wooden plate with felt: mellow lo-fi keys. HARD 25, not §4.4's 10: under §3.6's contact law a
     * HARD 10 felt rests ~5 ms on the plate, which filters out even a C4 fundamental (-23 dB) and left the preset
     * 22 dB under the others and below M8's -30 dBFS audibility gate. 25 is the softest felt that still passes the
     * fundamental; everything above 4x stays rolled off, so it is still mellow. (Integration: 40, see the top.) */
    CY_PRESET("FELT PLATE", V(SHAPE, 3), MALLET, V(MATERIAL, 0.8f), V(STIFFNESS, 70), V(DECAY, 0.5f),
              V(HARDNESS, 40), V(DAMPER, 60), V(TILT, -2), V(ROOM, 25), V(SIZE, 1.8f), V(LEVEL, 6)),
    /* 15 a gong dipped in water: strong pitch fall and a wandering strike */
    CY_PRESET("WATER GONG", V(SHAPE, 1), MALLET, V(MATERIAL, 4.0f), V(STIFFNESS, 90), V(DECAY, 2.5f),
              V(HARDNESS, 40), V(BLOOM, 60), V(LFO_POS, 30), V(LFO_RATE, 0.15f), V(ROOM, 55), V(SIZE, 9.0f)),
    /* 16 skin kick with real pitch-drop physics. Play at OCT -2. */
    CY_PRESET("MEMBRANE KICK", V(SHAPE, 0), MALLET, V(MATERIAL, 0.4f), V(STIFFNESS, 0), V(DECAY, 0.45f),
              V(HARDNESS, 70), V(BLOOM, 85), POS(50, 50), V(DRIVE, 9), V(ROOM, 8), V(SIZE, 0.8f), V(LEVEL, 2)),
    /* 17 half-clamped tom (DRUM > CIRCLE 40%): punchy with a short drop */
    CY_PRESET("LOOSE TOM", V(SHAPE, 0.4f), MALLET, V(MATERIAL, 0.7f), V(STIFFNESS, 15), V(DECAY, 0.6f),
              V(HARDNESS, 60), V(BLOOM, 40), V(DRIVE, 4), V(ROOM, 18), V(SIZE, 1.4f)),
    /* 18 a shaken steel sheet: rumbling impacts with drive */
    CY_PRESET("THUNDER SHEET", V(SHAPE, 3.4f), RAIN, V(MATERIAL, 3.0f), V(STIFFNESS, 75), V(DECAY, 1.5f),
              V(DENSITY, 80), V(SPREAD, 100), V(BLOOM, 20), V(NOISE, 50), V(DRIVE, 6), V(ROOM, 60), V(SIZE, 10),
              V(LEVEL, -6.5f)),
    /* 19 hard glass triangle: crystalline clicks, very bright */
    CY_PRESET("ICE", V(SHAPE, 4), MALLET, V(MATERIAL, 2.0f), V(STIFFNESS, 100), V(DECAY, 0.7f), V(HARDNESS, 100),
              V(BRIGHT, 50), V(SHIMMER, 0), V(MICS, 100), V(ROOM, 50), V(SIZE, 3.0f)),
    /* 20 a steel tine struck near its tip: a thumb-piano pluck */
    CY_PRESET("KALIMBA TINE", V(SHAPE, 8), MALLET, V(MATERIAL, 3.0f), V(DECAY, 1.2f), V(HARDNESS, 70),
              V(NOISE, 20), POS(15, 50), V(ROOM, 20), V(SIZE, 1.5f), V(LEVEL, 2.5f)),
    /* 21 an endless bowed drone; the plate slowly morphs between circle and hexagon */
    CY_PRESET("AETHER", V(SHAPE, 1.5f), BOW, V(MATERIAL, 5.0f), V(DECAY, 4.0f), V(FORCE, 35), V(SWELL, 1.5f),
              V(MORPH_DEPTH, 25), V(LFO_RATE, 0.07f), V(LFO_WAVE, CY_LFO_SINE), V(ROOM, 70), V(SIZE, 12),
              V(PREDELAY, 45), V(LEVEL, -2)),
    /* 22 the keybed is one bronze gong: centre keys boom, edge keys shimmer; touch dark keys for harmonics */
    CY_PRESET("SURFACE GONG", V(KEYMODE, 1), V(SHAPE, 1), MALLET, V(MATERIAL, 4.0f), V(STIFFNESS, 90),
              V(DECAY, 3.0f), V(HARDNESS, 50), V(ROOM, 50), V(SIZE, 6.0f), V(LEVEL, -3.5f)),
    /* 23 raw steel square bowed at the edge: growly, and the figures snap into place */
    CY_PRESET("BOWED SQUARE", V(SHAPE, 3), BOW, V(MATERIAL, 3.0f), V(DECAY, 1.5f), V(FORCE, 75), V(GRIT, 60),
              POS(6, 50), V(ROOM, 40), V(SIZE, 3.5f), V(LEVEL, 5.5f)),
    /* 24 a bronze bonsho struck by a wooden beam: bell-carved, with long beating */
    CY_PRESET("TEMPLE BELL", V(SHAPE, 1), MALLET, V(MATERIAL, 4.5f), V(STIFFNESS, 100), V(CARVE, 60), BELL,
              V(DECAY, 4.0f), V(HARDNESS, 22), V(BLOOM, -10), V(SHIMMER, 12), V(ROOM, 50), V(SIZE, 10), V(LEVEL, 6)),
};

/* ------------------------------------------------------------- runtime --- */
/* the detent grid of params.c (cy_p_grid): a stored value within one 16-bit step of a grid point is that point,
 * so SHAPE 1 loads as exactly 1/8 (the CIRCLE keyframe, not 1.00002), STIFFNESS 80 as exactly 0.8, and an enum
 * as exactly k/(N-1). Off-grid values (DECAY *1.6) load as stored. Idempotent: load(store(load(p))) == load(p). */
static float cy_ps_snap(int id, float n)
{
    const cy_param_def_t *d = &CY_PARAMS[id];
    int D = d->curve == CY_CV_ENUM ? (d->detents > 1 ? d->detents - 1 : 1)
          : d->curve == CY_CV_MORPH ? d->detents * (int)(d->hi - d->lo + 0.5f) : (d->detents ? d->detents : 1);
    float g = fm_floorf(n * (float)D + 0.5f) / (float)D;
    return fm_fabsf(n - g) <= 1.0f / 65535.0f ? g : n;
}

void cy_preset_load(const cy_preset_t *p, float *norm)
{
    int i;
    if (!p || !norm)
        return;
    for (i = 0; i < P_NPRESET; i++)
        norm[i] = cy_ps_snap(i, (float)p->v[i] * (1.0f / 65535.0f));
}

void cy_preset_store(const float *norm, const char *name, cy_preset_t *p)
{
    int i, end = 0;
    if (!p)
        return;
    for (i = 0; i < CY_PRESET_NAME; i++) {        /* zero-padded: two stores of one name are the same bytes */
        if (!end && (!name || !name[i]))
            end = 1;
        p->name[i] = end ? 0 : name[i];           /* all 14 bytes usable; a 14-letter name has no terminator */
    }
    for (i = 0; i < P_NPRESET; i++) {
        float n = norm ? norm[i] : 0.0f;
        n = n >= 0.0f ? (n <= 1.0f ? n : 1.0f) : 0.0f;   /* (and NaN -> 0) */
        p->v[i] = (uint16_t)(n * 65535.0f + 0.5f);
    }
}

/* morph-scrub and glides: linear in norm (geometric for LOG values, through the intermediate shapes for SHAPE),
 * enums switch at the halfway point. Exact at both ends, and a parameter both sides share stays bit-identical, so
 * an in-between sound only reads as edited where it really differs. */
void cy_preset_lerp(const float *a, const float *b, float t, float *out)
{
    int i;
    if (!a || !b || !out)
        return;
    t = t >= 0.0f ? (t <= 1.0f ? t : 1.0f) : 0.0f;
    for (i = 0; i < P_NPRESET; i++) {
        if (a[i] == b[i])
            out[i] = a[i];
        else if (CY_PARAMS[i].curve == CY_CV_ENUM)
            out[i] = t < 0.5f ? a[i] : b[i];
        else
            out[i] = (1.0f - t) * a[i] + t * b[i];
    }
}

void cy_params_defaults(float *norm)
{
    int i;
    if (!norm)
        return;
    for (i = 0; i < P_NPARAMS; i++)
        norm[i] = cy_param_norm(i, CY_PARAMS[i].def);
}
