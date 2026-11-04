/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Ingop Vasilj */

/* JUNO106: the Juno-106 as one engine, ported from a GPL-3 Juno-106 DSP
 * port (its sources the ground truth, juno/dsp.h a proven bit-identical C++
 * mirror; the voice below is that, not a revisit).
 *
 * Six voices (the Juno's own count) per part, every state fp32: the DSP is
 * fp64 upstream and the FM-1's FPU is float-only (-mfprev1), so the f64 comes
 * across as f32 (coefficients, phases and envelopes, as ENG_POLYSIX did).
 * No libm: the three expansions it needs (2^x of the cutoff's octave sum and
 * of the PWM-free DCO pitch, the sin / cos of the biquad's angle) come from
 * the small helpers and the host's SINE table below.
 *
 *   DCO    one master free-running phase like the Juno's MC5534: band-limited
 *          (quadratic polyBLEP at every discontinuity) saw + pulse + a sub
 *          pulse an octave below + white noise (xorshift64*), summed and
 *          divided by the count of the sources on. PW 0 is the pulse off;
 *          up from 0.05 the width widens (the panel's flip confirms). PWM
 *          mode centers the width at 50 % and the framework's LFO -> SHAP
 *          routing moves it (the Juno's LFO, its depth PW's amount).
 *   HPF    skipped: tal's position 0 is bypass (all but two presets sit
 *          there).
 *   VCF    the 24 dB cascade: two DF2T biquads (the same solve as Juno's
 *          IR3R01), an odd-symmetric limiter on the way out, res 1 self
 *          oscillates. Coefficients recomputed once per synthesized block
 *          (CUTOFF SMOOTH's 2 ms smear hides the step; a per-sample solve is
 *          the sin / cos budget's). CUTOFF is the framework's knob; the
 *          contour EG and LFO -> FILTER move it in octaves (the LFO's full
 *          framework depth = the Juno's fixed 3.5 oct). Key follow fixed at
 *          tal's default (0.6: 0 oct — the hardware has no knob). The
 *          framework owns the -3 dB filter TYPE knob's shape as before.
 *   EG     the framework's ATK DEC SUS REL drive the first-order ADSR
 *          (RC approach, coefficient 1 - e^(-1/tau·sr), tal's coef()): the
 *          panel keeps the framework's meaning (a setting is 99 % in), so
 *          the engine's tau = setting / 4.6, as ENG_POLYSIX's. The contour
 *          EG's amount is the EG knob (bipolar: up = +oct, down = the
 *          Juno's bias, both to +-14). A re-attack of a sounding voice
 *          restarts the EG only: the DCO's phases and the VCF's carries
 *          keep, as tal's note_on (and the hardware's free-run) does. The
 *          voice ends when done() says so (ownenv, level 1e-4 of tal).
 *
 * The eight EDIT slots carry the panel: SAW, SUB (its volume), PW (pulse
 * width), PWM (mode: the width centers and the LFO moves it), NOISE, CUT,
 * RES, EG. The framework owns the rest, as the panel positions they stand in
 * for: the VOLUME / ENV VOLUME -> LVL, MASTERPITCH / TUNE -> the tune, the
 * TRANSPOSE -> the track's, portamento -> the track GLIDE, MOD DCO -> route
 * the framework LFO -> PITCH (its depth = the OSC LFO INT), MOD VCF's
 * FILTERLFO -> the framework LFO -> FILTER (fixed 3.5 oct at full), LFO1's
 * RATE / WAVE / DELAY / RETRIG -> the track LFO, AMPSUSTAIN -> SUS, the
 * velocity -> the track's velocity of its envelope, voice modes ->
 * KEY ASSIGN (POLY / MONO / UNISON). Not ported: the LFO CLOCK SYNC (no
 * host tempo source upstream either), DUST's own pan / pressure / MPE.
 *
 * The chorus is the port's stereo BBD chorus (chorus.rs), mono here
 * as a track INSERT (IT_JCHO, the P6 section's precedent: fx.c). The eight
 * presets are the port's hand-made ones, compressed onto the eight knobs by
 * tools/gen_juno_presets.py. */

#define J_POLY 6

typedef union { float f; uint32_t u; } j_bits_t;

/* 24000 / 32768: sample * Q15 amp * VOICE_FS as one float multiply (as P6) */
#define J_GAIN (24000.0f / 32768.0f)

/* 2^x, |x| <= 64, no libm: ENG_POLYSIX's p6_exp2f named apart (a test TU
 * carries both the engines and fx.c) */
static inline float j_exp2f(float x)
{
    j_bits_t b;
    int32_t n = 0;
    float f, lf;
    f = x;
    if (f < -0.02f || f > 0.02f) {
        n = (int32_t)f;
        if (n > f)
            n--;                                    /* floor */
        f = x - (float)n;
    }
    lf = 0.693147180559945f * f;
    b.u = (uint32_t)(n + 127) << 23;
    return b.f * (1.0f
        + lf * (0.99999999263f
        + lf * (0.500005834225f
        + lf * (0.166667017177f
        + lf * (0.041665314516f
        + lf * (0.00833379222f
        + lf * (0.00139003245f
        + lf * 0.000199712173f)))))));
}

/* the SINE table's value (Q15, 1024 entries over 2 pi) at x, lerped, and
 * sin / cos of a phase w in 0..pi for the biquad's coefficients (the table's
 * 3e-5 relative error only sways a resonant peak's exact knee) */

static inline float j_sint(float x)
{
    float a, b;
    uint32_t ti = (uint32_t)(x * (1024.0f * 0.159154943f));   /* / 2 pi */
    float fr = x * (1024.0f * 0.159154943f) - (float)ti;
    a = (float)SINE[ti & 1023u];
    b = (float)SINE[(ti + 1u) & 1023u];
    return (a + (b - a) * fr) * (1.0f / 32768.0f);
}

static inline float j_sin(float w)
{
    return j_sint(w < 3.14159265f * 2.0f ? w : 3.14159265f * 2.0f - w);  /* w in 0..2 pi */
}

static inline float j_cos(float w)
{
    return j_sint(w + 1.5707963f < 6.2831853f ? w + 1.5707963f : w - 4.71238898f);
}

/* xorshift64*, osc.rs white(): u64, returns 0..1 */

static inline float j_white(uint64_t *rng)
{
    uint64_t x = *rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *rng = x;
    /* the 53 mantissa bits of (x * 0x2545F4914F6CDD1Du) >> 11, top 32 only: the
     * FM1 linker carries no uint64 -> float helper (__floatundisf) */
    return (float)((uint32_t)((x * 0x2545F4914F6CDD1Du) >> 21)) * (1.0f / 4294967296.0f);
}

/* polyBLEP: the correction at a downward jump of size 1 at p0 (dsp.h jump) */

static inline float j_jump(float p, float p0, float n)
{
    float d = p - p0;
    float t;
    d -= (float)(int32_t)d;                          /* rem_euclid 1 (phases >= 0) */
    if (d < 0.0f)
        d += 1.0f;
    if (d < n) {
        t = 1.0f - d / n;
        return 0.5f * t * t;
    }
    if (d > 1.0f - n) {
        t = 1.0f - (1.0f - d) / n;
        return -0.5f * t * t;
    }
    return 0.0f;
}

/* band-limited saw (base p-0.5 in -0.5..0.5) and pulse (dsp.h saw_raw /
 * pulse_raw) */

static inline float j_saw_raw(float p, float n)
{
    return 2.0f * (p - 0.5f + j_jump(p, 0.0f, n));
}

static inline float j_pulse_raw(float p, float pw, float n)
{
    return (p < pw ? 1.0f : -1.0f) - 2.0f * j_jump(p, 0.0f, n) + 2.0f * j_jump(p, pw, n);
}

/* ------------------------------------------------------------ voices --- */

typedef struct {                         /* one voice's fp32 body: tal Voice's */
    float ph, sub_ph;                    /* cycles, 0..1, free-running (the Juno's
                                          * master clock; a re-attack keeps them) */
    float cut_s;                         /* the smoothed cutoff CV, Hz */
    float s0[4], s1[4];                  /* the two DF2T biquad stages' carries */
    float level;                         /* the engine's own envelope 0..1 */
    uint8_t stage;                       /* env: 1 attack, 2 decay / sustain, 3 release */
    uint64_t rng;                        /* xorshift64*, whitened free-running */
} j_voice_t;

/* a part's: the voice bodies, then the block's coefficient snapshot (tal
 * reads a Globals per sample; the framework's block boundary is the knobs'
 * granularity, and nothing the knobs move steps as fast as its 2 ms smear) */
typedef struct {
    j_voice_t v[NVOICE];                 /* one per framework voice (the cap is J_POLY) */
    float step_a, k_d, k_r;              /* env: the approaches per sample (tal's a / d / r) */
    float sus;                           /* SUS as 0..1 */
    float cut_hz, cut_c;                 /* the cutoff CV's base and its approach */
    float res;                           /* RES 0..1 (1 = oscillation) */
    float env_oct, bias_oct;             /* the contour EG as octaves */
    float subvol, noise;                 /* the DCO's mixer levels */
    float pwm_depth;                     /* PWM mode: 0.405 * PW / 127 */
    uint32_t saw, sub, pw_mode;          /* the block's switches */
    float pw_man;                        /* MAN mode: the width (0 at PW 0: that is
                                          * where PW stands for the pulse's off) */
} j_part_t;

ENG_MEM_FITS("JUNO106", sizeof(j_part_t));

static j_part_t *j_of(track_t *t) { return (j_part_t *)ENG_MEM((uint32_t)(t - trk)); }

/* the block: one snapshot of the part's parameters, coefficients per sample
 * (as ENG_POLYSIX's p6_block; the mapping laws above its comment there) */
static void j_block(track_t *t)
{
    j_part_t *P = j_of(t);
    const int16_t *p = t->p;
    float ms = (float)TIME_MS_X10[p[P_ATK] & 127] / 10.0f;
    float peak = ms > 1.5f ? ms / 1000.0f : 0.0015f;     /* tal's env_time floor */
    P->step_a = 1.0f - j_exp2f(-6.636567f / (peak * FS));
    ms = (float)TIME_MS_X10[p[P_DEC] & 127] / 10.0f;
    P->k_d = 1.0f - j_exp2f(-1.442695f / (ms * 0.000217391f * FS));
    ms = (float)TIME_MS_X10[p[P_REL] & 127] / 10.0f;
    P->k_r = 1.0f - j_exp2f(-1.442695f / (ms * 0.000217391f * FS));
    P->sus = (float)p[P_SUS] / 127.0f;
    P->cut_hz = (float)CUTOFF_HZ[p[P_E5] & 127];
    P->res = (float)p[P_E6] / 127.0f;
    {
        int32_t eg = p[P_E7];                            /* tal: contour +14 oct, the bias -14..0 */
        P->env_oct = eg > 0 ? (float)eg * (14.0f / 63.0f) : 0.0f;
        P->bias_oct = eg < 0 ? (float)eg * (14.0f / 64.0f) : 0.0f;
    }
    P->subvol = (float)p[P_E1] / 127.0f;
    P->sub = p[P_E1] > 0;
    P->noise = (float)p[P_E4] / 127.0f;
    P->saw = p[P_E0] > 63;
    P->pw_mode = p[P_E3] > 0;
    P->pw_man = p[P_E2] == 0 ? 0.0f : 0.05f + 0.9f * ((float)p[P_E2] / 127.0f);
    P->pwm_depth = 0.405f * ((float)p[P_E2] / 127.0f);
    /* the cutoff CV's smear, tal's default CUTOFF SMOOTH (tau 2 ms) */
    P->cut_c = 1.0f - j_exp2f(-1.442695f / (0.002f * FS));
}

static void j_note_on(track_t *t, voice_t *v)
{
    j_part_t *P = j_of(t);
    j_voice_t *S = P->v + (uint32_t)(v - t->v);
    S->level = 0.0f;                         /* (tal's gate_on keeps the level: a
                                              * re-attack of a sounding voice fades its
                                              * chase; a fresh voice's level is already 0) */
    S->stage = 1;
    v->stage = 1;
}

static void j_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    j_part_t *P = j_of(t);
    j_voice_t *S = P->v + (uint32_t)(v - t->v);
    float inc_c = (float)m->inc * (1.0f / 4294967296.0f);  /* m.inc = f / FS * 2^32 */
    float lspread = 3.5f / 63.0f * (float)(m->cutoff >> 8); /* LFO -> FILTER, full = 3.5 oct */
    float b0, b1, a1, a2;                    /* the two biquads' coefficients, this block's */
    uint32_t i;
    if (n > CTL)
        n = CTL;
    if (inc_c > 0.45f)                       /* dsp.h: fc <= sr * 0.45 */
        inc_c = 0.45f;
    /* ---- the VCF's coefficients, once per block: the CV at its start (the
     * dirty trick is tal's own 2 ms smear — the CV the coefficients stand in
     * for moves at most 0.7 ms of the sweep) */
    {
        float oct = (P->env_oct + P->bias_oct) * S->level + lspread;
        float fc = P->cut_hz * j_exp2f(oct);
        if (fc < 5.0f)
            fc = 5.0f;
        if (fc > FS * 0.45f)
            fc = FS * 0.45f;
        /* filter.rs: w = 2 pi fc / sr (at pi * 0.999), then the biquad (its
         * al = sin(w) / (2 (0.5 + 20 res^2)), 0 past res 0.999) */
        float w = 6.2831853f * fc / FS;
        if (w > 3.14159265f * 0.999f)
            w = 3.14159265f * 0.999f;
        {
            float cw = j_cos(w), al;
            float bw = 0.5f * (1.0f - cw);
            al = P->res >= 0.999f ? 0.0f
                 : j_sin(w) / (2.0f * (0.5f + 20.0f * P->res * P->res));
            if (al > 0.999f)
                al = 0.999f;
            {
                float a0 = 1.0f + al;
                b0 = bw / a0;
                b1 = 2.0f * bw / a0;
                a1 = -2.0f * cw / a0;
                a2 = (1.0f - al) / a0;
            }
        }
    }
    for (i = 0; i < n; i++) {
        float sig = 0.0f, ncnt = 0.0f, y, out_f, env;
        /* ---- the EG (env.rs next(), the stage S->stage drives) */
        if (S->stage == 1) {
            S->level += P->step_a * (1.0f - S->level);
            if (S->level >= 0.999f) {
                S->level = 1.0f;
                S->stage = 2;
            }
        }
        if (S->stage == 2 && !v->gate)       /* tal note_off */
            S->stage = 3;
        if (S->stage == 2) {
            S->level += P->k_d * (P->sus - S->level);
            if (S->level - P->sus < 0.001f && P->sus - S->level < 0.001f)
                S->level = P->sus;
        } else if (S->stage == 3) {
            S->level += P->k_r * (0.0f - S->level);
            if (S->level < 1e-4f)
                S->level = 0.0f;
        }
        env = S->level;
        /* ---- the DCO (osc.rs; the phases free-run, the Juno's master clock) */
        S->ph += inc_c;
        if (S->ph >= 1.0f)
            S->ph -= 1.0f;
        S->sub_ph += inc_c * 0.5f;
        if (S->sub_ph >= 1.0f)
            S->sub_ph -= 1.0f;
        {
            float nbl = inc_c * 2.0f;        /* dsp.h n = min(2 f / sr, 1) */
            float saw, pulse, pwv;
            if (nbl > 1.0f)
                nbl = 1.0f;
            /* tal: PW mode LFO centers the width at 50 %, the depth its amount */
            pwv = P->pw_man;
            if (P->pw_mode)
                pwv = 0.02f + 0.96f * (0.5f + (float)((m->shape >> 8) - 64) * (1.0f / 64.0f) * P->pwm_depth);
            saw = j_saw_raw(S->ph, nbl);
            pulse = pwv <= 0.0f ? 0.0f : j_pulse_raw(S->ph, pwv, nbl);
            if (P->saw) {                    /* tal: the sources summed, then / n */
                sig += saw;
                ncnt += 1.0f;
            }
            sig += pulse;
            ncnt += 1.0f;
            if (P->sub) {
                sig += j_pulse_raw(S->sub_ph, 0.5f, inc_c) * P->subvol;
                ncnt += 1.0f;
            }
            sig += (j_white(&S->rng) * 2.0f - 1.0f) * P->noise;
            if (P->noise > 0.0f)
                ncnt += 1.0f;
            if (ncnt > 0.0f)
                sig *= 0.5f / ncnt;
        }
        /* ---- the 24 dB: two DF2T biquads, the odd-symmetric limiter out
         * (tal's 2 ms cv smear hides the block's fixed coefficients) */
        {
            float x0 = sig, x1;
            x1 = b0 * x0 + b1 * S->s0[0] + b0 * S->s0[1] - a1 * S->s0[2] - a2 * S->s0[3];
            S->s0[1] = S->s0[0];
            S->s0[0] = x0;
            S->s0[3] = S->s0[2];
            S->s0[2] = x1;
            {
                float y2 = b0 * x1 + b1 * S->s1[0] + b0 * S->s1[1] - a1 * S->s1[2] - a2 * S->s1[3];
                S->s1[1] = S->s1[0];
                S->s1[0] = x1;
                S->s1[3] = S->s1[2];
                S->s1[2] = y2;
                y = y2 * (1.0f / (1.0f + 2.0f * y2 * y2));
            }
        }
        out_f = y * env;
        if (out_f > 4.0f)
            out_f = 4.0f;
        else if (out_f < -4.0f)
            out_f = -4.0f;
        out[i] += (int32_t)(out_f * (float)amp_at(m, i) * J_GAIN);
    }
}

static int j_done(track_t *t, voice_t *v)
{
    const j_part_t *P = (const j_part_t *)ENG_MEM((uint32_t)(t - trk));
    const j_voice_t *S = P->v + (uint32_t)(v - t->v);
    return S->stage == 3u && S->level == 0.0f;
}

/* ------------------------------------------------------------ presets --- */

/* the factory set: 8 rows generated by tools/gen_juno_presets.py from
 * the port's hand-made presets (juno/presets.cpp), the chorus ones routing
 * the engine's own INSERT (IT_JCHO) */

#include "felucca_juno.h"

/* ------------------------------------------------------------- engine --- */

static const engine_t ENG_JUNO = {
    .name = "JUNO106",
    .page_title = {"DCO", "VCF"},
    .edit = {
        {"SAW", F_PCT, 0, 127, 127, 0, 0},
        {"SUB", F_PCT, 0, 127, 127, 0, 0},
        {"PW", F_PCT, 0, 127, 0, 0, 0},
        {"PWM", F_PCT, 0, 127, 0, 0, 0},
        {"NOISE", F_PCT, 0, 127, 0, 0, 0},
        {"CUTOFF", F_CUTOFF, 0, 127, 127, 0, 0},
        {"RES", F_PCT, 0, 127, 0, 0, 0},
        {"EG", F_BIPCT, -64, 63, 0, 0, 0},
    },
    .presets = JUNO_PRESETS,
    .npresets = NELEM(JUNO_PRESETS),
    .note_on = j_note_on,
    .render = j_render,
    .knob = {P_E5, P_E6, P_ATK, P_REL},
    .poly = J_POLY,
    .block = j_block,
    .ownenv = 1,
    .done = j_done,
    .mem = sizeof(j_part_t),
};
