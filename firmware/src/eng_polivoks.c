/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Ingop Vasilj */

/* POLIVOKS: the Formanta Polivoks as one engine, ported from the Formanta
 * Polivoks JUCE plugin (~/repo/polingoks, its ground truth; the DSP below is
 * that plugin's, not a revisit).
 *
 * Two voices (the plugin's duophonic pair) per part, every state fp32: the
 * plugin computes in double only where the op-amp model wants it, and the
 * FM-1's FPU is float-only, so a host render and the device compute the same
 * samples. No libm: the one expansion the port calls for (2^x of the cutoff's
 * octave sum and the OSC 2 tune) is the helper beside it, POLYSIX's
 * p6_exp2f named apart (a test TU includes this file without eng_polysix.c).
 *
 *   VCO    band-limited, the plugin's own PolyBLEP (saw, the three squares)
 *          and PolyBLAMP (triangle): TRI, SAW, SQR 50 / 25 / 12 %. VCO 1 at
 *          the voice's pitch, VCO 2 a TUNE semitone interval off it. Both
 *          free-run across notes (the plugin resets them only on all notes
 *          off). The plugin's OSC 2 -> OSC 1 cross-mod: not ported (no knob;
 *          a later change if one frees up).
 *   VCF    the plugin's two op-amp state-variable filter, fp32, 4x
 *          oversampled (the plugin's own comment: the integration coefficient
 *          reaches ~0.3 at the top of the control range, so 4x keeps the
 *          trapezoidal step clean; it builds 16x). The K140UD12 macromodel:
 *          the gm/C cutoff from Iset (3736 Hz/uA, the corner 1.414x below the
 *          -3 dB point, divided back out), the class-B slew, the low-bias
 *          output-stage step (the dead zone), and the input offset and noise
 *          that wake a self-oscillating filter. CUTOFF is the -3 dB point
 *          (20 Hz .. 18 kHz, the plugin's own clamp), RES 0..1.35 (1 = self
 *          oscillation, past it the loop grows until the op-amps saturate),
 *          ENV sweeps the cutoff per sample (the plugin's octave modulation,
 *          the Polivoks's signature). No key tracking (the Polivoks has
 *          none). Low pass out; the plugin's band-pass choice: not ported
 *          (no knob). The op-amps keep their state across notes (the plugin
 *          resets the filter only on all notes off), zeroed with the part's
 *          memory (engines.c eng_mem_claim).
 *   EG     the framework's ATK DEC SUS REL drive it, linear as the plugin's
 *          Adsr (level += step per sample; the table's 1 ms floor is the
 *          plugin's own 0.001 s). The engine owns it (ownenv), the voice ends
 *          when done() says so; one envelope sweeps the filter and opens the
 *          VCA (the plugin's separate filter / amp pair under one framework
 *          envelope). A retrigger keeps the level (the plugin's Adsr keeps
 *          it) and the VCO phases. GLIDE: the engine's own portamento, the
 *          plugin's one-pole on the frequency (snap below 1 ms; the TIME
 *          table's seconds, the plugin's 0..2 s in its low half); it keeps
 *          across notes as the plugin's does.
 *
 * The eight EDIT slots carry the panel: CUTOFF, RES, ENV, TUNE 2 (EDIT 1),
 * GLIDE, WAVE 1, WAVE 2, MIX (EDIT 2, the osc 1 / osc 2 balance). The
 * framework owns the rest, as the panel positions they stand in for: voice
 * mode -> the track's POLY / MONO (2 voices, each its own note; the plugin's
 * duophonic top / bottom split is the framework's polyphony), main /
 * telephon volume -> LVL, the MG -> the track LFO (filter: LFO -> FILTER, one
 * step 0.0713 oct as POLYSIX's; pitch: LFO -> PITCH; amp: LFO -> MASTER), the
 * mixer's noise and input levels -> not ported (no knob, no input; the
 * op-amp's own offset and noise are in), the envelope one-shot / repeat and
 * gate-on switches -> not ported (one framework envelope, the keyboard's
 * gate).
 *
 * Six presets handpicked from the plugin's defaults and the Polivoks's
 * character; they play dry (FX(0,0,0,0), as the plugin has no effects). */

#define PVLK_OSC 4              /* the filter's oversampling (the plugin builds 16x) */
#define PVLK_POLY 2

/* 24000 / 32768: sample * Q15 amp * VOICE_FS as one float multiply (POLYSIX's) */
#define PL_GAIN (24000.0f / 32768.0f)

typedef union { float f; uint32_t u; } pl_bits_t;

/* 2^x, |x| <= 64, no libm: ENG_POLYSIX's p6_exp2f named apart (a test TU
 * includes this file without eng_polysix.c; eng_juno.c's j_exp2f, same) */
static inline float pl_exp2f(float x)
{
    pl_bits_t b;
    int32_t n = 0;
    float f, lf;
    f = x;
    if (f < -0.02f || f > 0.02f) {
        n = (int32_t)f;
        if (n > f)
            n--;                                        /* floor */
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

/* the plugin's blep: the 2*dt-wide parabolic step correction around a
 * discontinuity (Oscillator::blep) */
static inline float pl_blep(float t, float dt)
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

/* the equivalent for a change of slope (Oscillator::blamp), the triangle's */
static inline float pl_blamp(float t, float dt)
{
    float x;
    if (t < dt) {
        x = t / dt;
        return (x * x * x - x) * (1.0f / 3.0f);
    }
    if (t > 1.0f - dt) {
        x = (t - 1.0f) / dt;
        return (x * x * x - x) * (1.0f / 3.0f);
    }
    return 0.0f;
}

/* the plugin's per-call input noise (the plugin's per-instance Random: 0..1 * 1e-5 V,
 * one-sided, one draw per op-amp process call; it and the input offsets wake a
 * self-oscillating filter) */
static inline float pl_noise(int32_t *nz)
{
    uint32_t r = (uint32_t)*nz * 1664525u + 1013904223u;
    *nz = (int32_t)r;
    return (float)(r >> 8) * (1.0f / 8388608.0f) * 1.0e-5f;
}

/* ------------------------------------------------------------ voices --- */

typedef struct {                         /* one voice's fp32 body: the plugin's own fields */
    float phase1, phase2;                /* cycles, 0..1 (free-run across notes) */
    float f;                             /* the glided pitch, cycles per sample (0 = fresh: snap) */
    float cap1, prev1, out1;             /* op-amp 1: the compensation cap, its last error, the output */
    float cap2, prev2, out2;             /* op-amp 2 */
    float lp, bp;                        /* the SVF's carried states (the plugin re-zeros them per
                                          * sample and leans on 16x to converge; 4x carries them) */
    float env;                           /* the engine's own envelope 0..1, linear as the plugin's Adsr */
    uint8_t stage;                       /* 1 attack, 2 decay / sustain, 3 release */
    int32_t nz;                          /* the LCG (POLYSIX's pattern), a fresh stream per note */
} pl_voice_t;

/* a part's: the voice bodies, then the block's coefficient snapshot (the
 * plugin reads a snapshot per block too: updateControlRates) */
typedef struct {
    pl_voice_t v[NVOICE];                /* one per framework voice (the cap is PVLK_POLY) */
    float step_a, step_d, step_r, sus;   /* the envelope: per-sample steps, the sustain floor */
    float env_oct;                       /* ENV as octaves */
    float cut_hz;                        /* CUTOFF(E0) of the block, Hz */
    float damp;                          /* RES(E1): the plugin's damping 2 * (1 - res) */
    float ratio2;                        /* TUNE 2(E3) as a frequency ratio */
    float k_glide;                       /* GLIDE(E4): the per-sample coefficient, 0 = snap */
    float mix2;                          /* MIX(E7): the osc 2 balance, osc 1 is 1 - it */
    uint32_t wave1, wave2;               /* the block's E5 / E6 */
} pl_part_t;

ENG_MEM_FITS("POLIVOKS", sizeof(pl_part_t));

static pl_part_t *pl_of(track_t *t) { return (pl_part_t *)ENG_MEM((uint32_t)(t - trk)); }

/* -------------------------------------------------------- parameters --- */

/* value labels fit their EDIT card's 48 px column (the S face) */
static const char *const N_PL_WAVE[] = {"TRI", "SAW", "SQR50", "SQR25", "SQR12"};

/* the plugin's dutyFor: the three squares of differing duty cycle */
static const float PL_DUTY[3] = {0.5f, 0.25f, 0.125f};

/* the block: one snapshot of the part's parameters, coefficients per sample */
static void pl_block(track_t *t)
{
    pl_part_t *P = pl_of(t);
    const int16_t *p = t->p;
    float ms, spn;
    /* the plugin's Adsr: per-sample steps (1 / jmax(1, seconds * sr)) */
    ms = (float)TIME_MS_X10[p[P_ATK] & 127] / 10.0f;   /* the TIME table's milliseconds */
    spn = ms * (0.001f * (float)FS);                   /* the stage's length in samples */
    P->step_a = 1.0f / (spn < 1.0f ? 1.0f : spn);
    ms = (float)TIME_MS_X10[p[P_DEC] & 127] / 10.0f;
    spn = ms * (0.001f * (float)FS);
    P->step_d = 1.0f / (spn < 1.0f ? 1.0f : spn);
    ms = (float)TIME_MS_X10[p[P_REL] & 127] / 10.0f;
    spn = ms * (0.001f * (float)FS);
    P->step_r = 1.0f / (spn < 1.0f ? 1.0f : spn);
    P->sus = (float)p[P_SUS] / 127.0f;
    P->env_oct = (float)p[P_E2] * (1.0f / 64.0f);      /* ENV: +-1 oct at +-64 */
    P->cut_hz = (float)CUTOFF_HZ[p[P_E0] & 127];
    if (P->cut_hz < 20.0f)                             /* the plugin's setCutoff clamp */
        P->cut_hz = 20.0f;
    else if (P->cut_hz > 18000.0f)
        P->cut_hz = 18000.0f;
    {   /* RES 0..127 -> the plugin's 0..1.35 (1 = self oscillation, past it the loop grows) */
        float res = (float)((uint32_t)p[P_E1] & 127u) * (1.35f / 127.0f);
        P->damp = 2.0f * (1.0f - res);
    }
    P->ratio2 = pl_exp2f((float)p[P_E3] * (1.0f / 12.0f));   /* TUNE 2, semitones */
    {   /* GLIDE: the plugin's one-pole (portamentoCoefficient), snap below 1 ms */
        float sec = (float)TIME_MS_X10[p[P_E4] & 127] / 10000.0f;
        P->k_glide = sec <= 0.001f ? 0.0f : 1.0f - pl_exp2f(-1.442695f / (sec * (float)FS));
    }
    P->wave1 = (uint32_t)p[P_E5] % 5u;
    P->wave2 = (uint32_t)p[P_E6] % 5u;
    P->mix2 = (float)((uint32_t)p[P_E7] & 127u) / 127.0f;    /* the osc 1 / osc 2 balance */
}

static void pl_note_on(track_t *t, voice_t *v)
{
    pl_part_t *P = pl_of(t);
    pl_voice_t *S = P->v + (uint32_t)(v - t->v);
    S->stage = 1;                              /* the plugin's Adsr retriggers, the level keeps */
    S->nz = (int32_t)(0x2545F491u + v->age * 0x9E3779B9u);   /* a fresh LCG stream per note */
}

static void pl_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    pl_part_t *P = pl_of(t);
    pl_voice_t *S = P->v + (uint32_t)(v - t->v);
    float inc_c = (float)m->inc * (1.0f / 4294967296.0f); /* m.inc = f / FS * 2^32 */
    float lfo_oct = (float)(m->cutoff >> 8) * 0.071297527f;   /* LFO -> FILTER: one step 0.0713 oct
                                                               * (POLYSIX's own constant) */
    float m1, dt1, dt2, vco1, vco2;
    uint32_t i;
    if (n > CTL)
        n = CTL;    /* ---- GLIDE: the plugin's one-pole toward the block's pitch (both VCOs
     * follow the glided pitch, TUNE 2 keeps its interval); a fresh voice snaps */
    if (P->k_glide == 0.0f || S->f == 0.0f)
        S->f = inc_c;
    else
        S->f += (inc_c - S->f) * P->k_glide;
    dt1 = S->f;
    dt2 = S->f * P->ratio2;
    if (dt1 > 0.49f)                                   /* the plugin: hz <= sr * 0.49 */
        dt1 = 0.49f;
    if (dt2 > 0.49f)
        dt2 = 0.49f;
    m1 = 1.0f - P->mix2;
    for (i = 0; i < n; i++) {
        float env = S->env;
        float oct, fc, iset, kint, maxstep, dz, mixed, lp, bp, out_f;
        uint32_t j;
        /* ---- the EG (the plugin's Adsr, linear) */
        if (S->stage == 1) {
            env += P->step_a;
            if (env >= 1.0f) {
                env = 1.0f;
                S->stage = 2;
            }
        }
        if (S->stage == 2 && !v->gate)                 /* the plugin's noteOff: the last key up */
            S->stage = 3;
        if (S->stage == 2) {
            if (env > P->sus) {
                env -= P->step_d;
                if (env < P->sus)
                    env = P->sus;
            }
        } else if (S->stage == 3) {
            env -= P->step_r;
            if (env < 0.0f)
                env = 0.0f;
        }
        S->env = env;
        /* ---- the VCOs (the plugin's Oscillator, free-running) */
        S->phase1 += dt1;
        if (S->phase1 >= 1.0f)
            S->phase1 -= 1.0f;
        S->phase2 += dt2;
        if (S->phase2 >= 1.0f)
            S->phase2 -= 1.0f;
        {   /* VCO 1 */
            float t1 = S->phase1;
            if (P->wave1 == 0u)                        /* TRI: PolyBLAMP */
                vco1 = 1.0f - 4.0f * (t1 < 0.5f ? 0.5f - t1 : t1 - 0.5f) + pl_blamp(t1, dt1);
            else if (P->wave1 == 1u)                   /* SAW */
                vco1 = 2.0f * t1 - 1.0f - pl_blep(t1, dt1);
            else {                                     /* SQR 50 / 25 / 12 % */
                float duty = PL_DUTY[P->wave1 - 2u];
                float te = t1 - duty;
                vco1 = (t1 < duty ? 1.0f : -1.0f) + pl_blep(t1, dt1);
                if (te < 0.0f)
                    te += 1.0f;
                vco1 -= pl_blep(te, dt1);
            }
        }
        {   /* VCO 2 */
            float t2 = S->phase2;
            if (P->wave2 == 0u)
                vco2 = 1.0f - 4.0f * (t2 < 0.5f ? 0.5f - t2 : t2 - 0.5f) + pl_blamp(t2, dt2);
            else if (P->wave2 == 1u)
                vco2 = 2.0f * t2 - 1.0f - pl_blep(t2, dt2);
            else {
                float duty = PL_DUTY[P->wave2 - 2u];
                float te = t2 - duty;
                vco2 = (t2 < duty ? 1.0f : -1.0f) + pl_blep(t2, dt2);
                if (te < 0.0f)
                    te += 1.0f;
                vco2 -= pl_blep(te, dt2);
            }
        }
        /* ---- the cutoff: the plugin's setCutoff, the -3 dB point from Iset;
         * the EG sweeps it per sample (the plugin's octave modulation) */
        oct = P->env_oct * env + lfo_oct;
        fc = P->cut_hz * pl_exp2f(oct);
        if (fc < 20.0f)
            fc = 20.0f;
        else if (fc > 18000.0f)
            fc = 18000.0f;
        iset = fc * (1.0f / 5283.49f);                 /* 3736 Hz/uA * 1.4142136 corner */
        if (iset < 0.0054f)                            /* the plugin's setIset clamp */
            iset = 0.0054f;
        else if (iset > 13.76f)
            iset = 13.76f;
        kint = iset * (3.14159265f * 3736.0f / ((float)PVLK_OSC * (float)FS));
        maxstep = iset * (40000.0f / ((float)PVLK_OSC * (float)FS));   /* 0.04 V/us per uA */
        dz = 4.0e-4f / iset;                           /* the output-stage step, wider as the bias falls */
        if (dz > 0.02f)
            dz = 0.02f;
        /* ---- the two op-amp integrators (the plugin's ProgrammableOpAmp, fp32),
         * both driven from the previous step's state; the SVF's low pass and band
         * pass carry across samples (the plugin re-zeros them per sample and leans
         * on 16x to converge; 4x carries them, the same converged loop) */
        mixed = vco1 * m1 + vco2 * P->mix2;
        lp = S->lp;
        bp = S->bp;
        for (j = 0; j < PVLK_OSC; j++) {
            float e, new_bp, new_lp, n1, n2;
            /* the plugin's per-call draws: each op-amp its own */
            n1 = pl_noise(&S->nz);
            n2 = pl_noise(&S->nz);
            /* op-amp 1: the summing node (input, the resonant and damping feedback) */
            e = mixed - P->damp * lp - 2.0f * bp + 6.0e-3f + n1;
            if (e < -2.0f)
                e = -2.0f;
            else if (e > 2.0f)
                e = 2.0f;
            S->cap1 += kint * (e + S->prev1);          /* trapezoidal integration */
            if (S->cap1 > 12.0f)                       /* the compensation node cannot charge past
                                                                * the supply: bounds the over-driven
                                                                * windup the plugin's per-sample zeroing
                                                                * hides (shortcut: not in the plugin) */
                S->cap1 = 12.0f;
            else if (S->cap1 < -12.0f)
                S->cap1 = -12.0f;
            S->prev1 = e;
            e = S->cap1 - S->out1;                     /* the class-B output stage: slew, rails */
            if (e < -maxstep)
                e = -maxstep;
            else if (e > maxstep)
                e = maxstep;
            S->out1 += e;
            if (S->out1 < -10.0f)
                S->out1 = -10.0f;
            else if (S->out1 > 10.0f)
                S->out1 = 10.0f;
            {   /* the low-bias output-stage step (the plugin's deadZone: x - dz^2 / x) */
                float o = S->out1, a = o < 0.0f ? -o : o;
                new_bp = a <= dz ? 0.0f : o - (dz * dz) / o;
            }
            /* op-amp 2: the band pass in, its own offset */
            e = bp - 4.5e-3f + n2;
            if (e < -2.0f)
                e = -2.0f;
            else if (e > 2.0f)
                e = 2.0f;
            S->cap2 += kint * (e + S->prev2);
            if (S->cap2 > 12.0f)
                S->cap2 = 12.0f;
            else if (S->cap2 < -12.0f)
                S->cap2 = -12.0f;
            S->prev2 = e;
            e = S->cap2 - S->out2;
            if (e < -maxstep)
                e = -maxstep;
            else if (e > maxstep)
                e = maxstep;
            S->out2 += e;
            if (S->out2 < -10.0f)
                S->out2 = -10.0f;
            else if (S->out2 > 10.0f)
                S->out2 = 10.0f;
            {
                float o = S->out2, a = o < 0.0f ? -o : o;
                new_lp = a <= dz ? 0.0f : o - (dz * dz) / o;
            }
            bp = new_bp;
            lp = new_lp;
        }
        S->lp = lp;
        S->bp = bp;
        /* ---- the VCA: the plugin's, linear (LFO -> MASTER comes in through amp_at) */
        out_f = lp * env * 0.5f;
        if (out_f > 4.0f)                              /* POLYSIX's headroom clamp */
            out_f = 4.0f;
        else if (out_f < -4.0f)
            out_f = -4.0f;
        out[i] += (int32_t)(out_f * (float)amp_at(m, i) * PL_GAIN);
    }
}

static int pl_done(track_t *t, voice_t *v)
{
    const pl_part_t *P = (const pl_part_t *)ENG_MEM((uint32_t)(t - trk));
    const pl_voice_t *S = P->v + (uint32_t)(v - t->v);
    return S->stage == 3u && S->env == 0.0f;
}

/* ------------------------------------------------------------ presets --- */

/* handpicked from the plugin's defaults and the Polivoks's character; dry
 * (FX(0, 0, 0, 0), the plugin has no effects). E0 CUTOFF (CUTOFF_HZ), E1 RES
 * (0..1.35), E2 ENV (+-1 oct at +-64), E3 TUNE 2 (semitones), E4 GLIDE (the
 * TIME table), E5 / E6 the waves, E7 the osc 1 / osc 2 balance */

static const preset_t PL_PRESETS[] = {
    {"BASS",  {60, 70, 30, -7, 0, 1, 4, 40}, {0, 90, 127, 60}, 0, 1, FX(0, 0, 0, 0), PAT(2)},
    {"ACID",  {55, 110, 50, -7, 40, 1, 4, 0}, {0, 55, 20, 50}, 0, 1, FX(0, 0, 0, 0), PAT(1)},
    {"LEAD",  {85, 40, 25, 7, 55, 3, 1, 48}, {0, 70, 100, 50}, 0, 1, FX(0, 0, 0, 0), PAT(4)},
    {"PAD",   {75, 20, 10, 7, 0, 0, 0, 64}, {80, 90, 110, 95}, 0, 0, FX(0, 0, 0, 0), PAT(5)},
    {"PLUCK", {65, 30, 55, 0, 0, 1, 3, 64}, {0, 60, 0, 60}, 0, 0, FX(0, 0, 0, 0), PAT(3)},
    {"GRIT",  {71, 127, 40, 0, 0, 4, 2, 64}, {20, 80, 110, 60}, 0, 1, FX(0, 0, 0, 0), PAT(13)},
};

/* ------------------------------------------------------------- engine --- */

static const engine_t ENG_POLIVOKS = {
    .name = "POLIVOKS",
    .page_title = {"VCF", "VCO"},
    .edit = {
        {"CUTOFF", F_CUTOFF, 0, 127, 85, 0, 0},
        {"RES", F_PCT, 0, 127, 14, 0, 0},
        {"ENV", F_BIPCT, -64, 63, 26, 0, 0},
        {"TUNE 2", F_SEMI, -7, 7, 0, 0, 0},
        {"GLIDE", F_TIME, 0, 127, 0, 0, 0},
        {"WAVE 1", F_ENUM, 0, 4, 1, N_PL_WAVE, 0},
        {"WAVE 2", F_ENUM, 0, 4, 1, N_PL_WAVE, 0},
        {"MIX", F_PCT, 0, 127, 64, 0, 0},
    },
    .presets = PL_PRESETS,
    .npresets = NELEM(PL_PRESETS),
    .note_on = pl_note_on,
    .render = pl_render,
    .knob = {P_E0, P_E1, P_ATK, P_REL},
    .poly = PVLK_POLY,
    .block = pl_block,
    .ownenv = 1,
    .done = pl_done,
    .mem = sizeof(pl_part_t),
};
