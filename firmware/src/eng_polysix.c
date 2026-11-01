/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Ingop Vasilj */

/* POLYSIX: the Korg Polysix as one engine, ported from the polyp CLAP plugin
 * (src/voice.rs, its ground truth; the voice below is that file's, not a revisit).
 *
 * Six voices (the Polysix's own count) per part, every state fp32: polyp is
 * fp32 and the FM-1's FPU is float-only (-mfprev1), so a host render and the
 * device compute the same samples. No libm: the three expansions polyp calls
 * for (2^x of the cutoff's octave sum, e^(2*env) of the VCA, the tangent of
 * the ladder) are the small helpers beside them.
 *
 *   VCO    pulse, saw+pulse (PW), PWM at PWM SPEED, or saw+PWM; a square sub
 *          an octave or two below. PW 0 is the square; up it narrows the pulse
 *          until it shuts the VCO off (the manual: that is how the self
 *          oscillating VCF is used on its own). The LFO -> SHAP routing moves
 *          the width too, as ANALOG's shape does.
 *   VCF    polyp's four TPT one-poles solved as ONE feedback loop, two Newton
 *          steps from the last solution. CUTOFF is the pole corner: the knob
 *          is where the peak and the self oscillation sit (voice.rs ladder_g;
 *          do NOT re-derive it as the cascade's -3dB knee). k = 4.2 * RES goes
 *          a little past the four poles of unity, which is what lets the
 *          filter sing with the VCO shut off, and a seed (polyp's LCG,
 *          free-running per voice) wakes it. Key tracking fixed at 2/3, the
 *          Polysix's one lug.
 *   EG     the framework's ATK DEC SUS REL drive it, as polyp's panel: ATK =
 *          the time to the peak (an RC curve, 99 % at 4.6 tau), DEC and REL
 *          exponential in the TIME table's sense (99 % of the way in the
 *          setting), so the engine's tau = setting / 4.6 — the panel keeps the
 *          framework's meaning, and a patch ported from polyp's tau knobs
 *          becomes 4.6 times its tau. On top, the OTA VCA's exponential law
 *          (LM13600): sustain 0.5 sits near -6 dB, not halfway in amplitude.
 *          The engine owns both, the voice ends when done() says so (ownenv),
 *          so the framework's amp envelope is not double counting. A re-attack
 *          of a sounding voice restarts the envelope and the VCO's phases and
 *          zeroes the filter's carries, polyp note_on's way; the ladder's
 *          solved feedback value (ring) and the LCG keep.
 *
 * The eight EDIT slots carry the panel: WAVE, OCT', WIDTH (PW/PWM), PWM (its
 * speed: 'PWM SPEED' never fits a card label; the 1.5.5 era's own knobs are
 * 5 capitals wide), SUB,
 * CUT, RES, EG. The framework owns the rest, as the panel positions they
 * stand in for: KEY ASSIGN -> the track's POLY / MONO / UNISON (the unison
 * stack is P_DETUNE), portamento -> the track GLIDE, the MG -> the track LFO
 * (VCO: RATE and FADE, its depth LFO -> PITCH; VCF: LFO -> FILTER, whose full
 * +-64-step depth is +-5.7 oct — the Polysix's fixed +-2 oct sits at ~45;
 * VCA: LFO -> MASTER), volume: LVL.
 *
 * Not ported (polyp's own "Not implemented" class, its README documents them):
 * BEND MODE VCF (the wheel comes in through the host's pitch, which bends the
 * VCO), VCA GATE mode, the ATTENUATOR (LVL stands for it), the Polysex and
 * velocity kits, and polyp fx.rs (chorus / phaser / ensemble — a later
 * change; the presets below carry no sends until then). The four presets are
 * handpicked from polyp's factory banks; porting the whole of them and the FX
 * is a tools/ script's job, still ahead. */

#define P6_POLY 6

typedef union { float f; uint32_t u; } p6_bits_t;

/* 24000 / 32768: sample * Q15 amp * VOICE_FS as one float multiply */
#define P6_GAIN (24000.0f / 32768.0f)

/* 2^x, |x| <= 64, no libm: the integer part into the float exponent field,
 * the fraction through the Taylor series of 2^f (f <= 1, degree 8: the error
 * term is ln2^9/9! ~ 2.6e-7, so the whole runs ~1e-6 relative). Polyp calls
 * for it as e^(2*env) (x = 2 env log2 e) and the cutoff's octave sum. */
static inline float p6_exp2f(float x)
{
    p6_bits_t b;
    int32_t n = 0;
    float f, lf;
    f = x;
    if (f < -0.02f || f > 0.02f) {                      /* small x: skip the floor split — 2^n·2^f rounds its result
                                                        * at ulp(2.0), which cost ~12 % of tau on a 1e-5 exponent
                                                        * (all but the shortest releases live at 1e-5 .. 1e-2). Positive
                                                        * exponents split too: unsplit, the Taylor of +7 octaves
                                                        * was ~12 % (audibly brighter cutoffs at the top of the keyboard) */
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

/* tan(x), 0 < x < pi/2, no libm: one halving into x <= 0.63 and the Taylor of
 * tan there as a series in x^2 (the truncated terms sum to ~1e-7), then one
 * doubling with tan(2t) = 2t/(1 - t^2). For ladder_g only, once per voice
 * per block on the way to g. */
static inline float p6_tanf(float x)
{
    float a2, t;
    x *= 0.5f;
    a2 = x * x;
    t = x * (1.0f
        + a2 * (1.0f / 3.0f
        + a2 * (2.0f / 15.0f
        + a2 * (17.0f / 315.0f
        + a2 * (62.0f / 2835.0f
        + a2 * (1382.0f / 155925.0f
        + a2 * 21844.0f / 6081075.0f))))));
    return 2.0f * t / (1.0f - t * t);
}

/* polyp ftanh, the Pade approximation of tanh, clamped at 3 where it is exact */

static inline float p6_tanhf(float x)
{
    float c = x < -3.0f ? -3.0f : x > 3.0f ? 3.0f : x;
    float c2 = c * c;
    return c * (27.0f + c2) / (27.0f + 9.0f * c2);
}

/* ------------------------------------------------------------ voices --- */

typedef struct {                         /* one voice's fp32 body: polyp Voice's own fields */
    float phase, sub_phase, pwm_phase;   /* cycles, 0..1 */
    float f[4];                          /* the ladder's one-pole carries */
    float dc_x, dc_y;                    /* under the ladder, a 10 Hz DC blocker */
    float ring;                          /* the last solved feedback value, the next Newton's guess */
    float env;                           /* the engine's own envelope 0..1 */
    uint8_t stage;                       /* 1 attack, 2 decay / sustain, 3 release */
    int32_t nz;                          /* polyp's LCG; a 0 seed never leaves it, un-zeroed on start */
} p6_voice_t;

/* a part's: the voice bodies, then the block's coefficient snapshot (polyp
 * reads a snapshot per block too: a knob lands whole in the block that plays
 * while it turned) */
typedef struct {
    p6_voice_t v[NVOICE];                /* one per framework voice (the cap is P6_POLY) */
    float step_a, k_d, k_r;              /* env: the attack's per-sample step, decay's and release's k */
    float k_res, eg_oct;                 /* the ladder: 4.2*RES; EG INT as octaves */
    float pwm_inc;                       /* PWM SPEED as cycles per sample */
    uint32_t wave, sub;                  /* the block's E0 / E4 */
    uint16_t cut;                        /* CUTOFF_HZ(E5) of the block, Hz */
} p6_part_t;

ENG_MEM_FITS("POLYSIX", sizeof(p6_part_t));

static p6_part_t *p6_of(track_t *t) { return (p6_part_t *)ENG_MEM((uint32_t)(t - trk)); }

/* -------------------------------------------------------- parameters --- */

/* value labels fit their EDIT card's 48 px column (the S face: "SAW+PW" is 50 px, "SAW+PWM" 60): the sweeps
 * ellipsis them and ui_render counts every ellipsised value a failing finding. The polyp plugin's full names
 * ("PW", "Saw+PW", "PWM", "Saw+PWM") stay documented here. */
static const char *const N_P6_WAVE[] = {"PW", "SAW+P", "PWM", "S+PWM"};
static const char *const N_P6_OCT[] = {"16'", "8'", "4'"};
static const char *const N_P6_SUB[] = {"OFF", "1 OCT", "2 OCT"};

/* the block: one snapshot of the part's parameters, coefficients per sample */
static void p6_block(track_t *t)
{
    p6_part_t *P = p6_of(t);
    const int16_t *p = t->p;
    float ms = (float)TIME_MS_X10[p[P_ATK] & 127] / 10.0f;   /* the TIME table's milliseconds */
    float peak = ms > 0.5f ? ms / 1000.0f : 0.0005f;      /* polyp's own attack floor */
    /* polyp: attacked a knob 99 % of the way to the peak, and k = exp(-4.6/(
     * knob*sr)), so the per-sample step is (1 - k) with the knob in seconds.
     * DEC and REL in the framework's sense: 99 % of the way in the setting, so
     * the engine's tau = setting / 4.6 — and polyp's exp runs on tau. log2(e)
     * turns the tau exponents over: p6_exp2f is 2^x, polyp's is e^x (before the
     * log2(e) a full-up REL ran tau 3.5 s instead of the panel's 2.2) */
    P->step_a = 1.0f - p6_exp2f(-6.636567f / (peak * FS));
    ms = (float)TIME_MS_X10[p[P_DEC] & 127] / 10.0f;
    P->k_d = p6_exp2f(-1.442695f / (ms * 0.000217391f * FS));   /* 1/4600 ms */
    ms = (float)TIME_MS_X10[p[P_REL] & 127] / 10.0f;
    P->k_r = p6_exp2f(-1.442695f / (ms * 0.000217391f * FS));
    P->wave = (uint32_t)p[P_E0];
    P->sub = (uint32_t)p[P_E4];
    P->cut = CUTOFF_HZ[p[P_E5] & 127];
    P->k_res = 4.2f * ((uint32_t)p[P_E6] & 127u) / 127.0f;
    P->eg_oct = (float)p[P_E7] * (10.0f / 63.0f);
    P->pwm_inc = (float)LFO_HZ_X100[p[P_E3] & 127] / (100.0f * FS);
}

/* the per-voice cutoff (voice.rs: the EG, the MG, key tracking and the bend
 * move it in octaves; 10 Hz .. 0.4 sr) and the ladder's g with it */
static inline float p6_cutoff(const p6_part_t *P, const vmod_t *m, uint8_t key, float *g_out)
{
    /* m.cutoff: the LFO -> FLT routing in 1/256 of the panel's steps;
     * 127 steps span log2(16000/30) = 9.05 oct, one step is 0.0713 oct */
    float oct = P->eg_oct + (float)(m->cutoff >> 8) * 0.071297527f + 0.667f * (float)key / 12.0f;
    float fc = (float)P->cut * p6_exp2f(oct);
    float a;
    if (fc < 10.0f)
        fc = 10.0f;
    if (fc > FS * 0.4f)
        fc = FS * 0.4f;
    /* voice.rs ladder_g: the TPT pole gain g = tan(pi fc / sr) / (1 + tan) */
    a = p6_tanf(3.14159265358979f * fc / FS);
    *g_out = a / (1.0f + a);
    return fc;
}

static void p6_note_on(track_t *t, voice_t *v)
{
    p6_part_t *P = p6_of(t);
    p6_voice_t *S = P->v + (uint32_t)(v - t->v);
    S->phase = S->sub_phase = S->pwm_phase = 0.0f;
    S->f[0] = S->f[1] = S->f[2] = S->f[3] = 0.0f;
    S->dc_x = S->dc_y = 0.0f;
    S->env = 0.0f;
    S->stage = 1;
    S->nz = (int32_t)(0x2545F491u + v->age * 0x9E3779B9u);   /* a fresh LCG stream per note */
}

static void p6_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    p6_part_t *P = p6_of(t);
    p6_voice_t *S = P->v + (uint32_t)(v - t->v);
    static const float OCTV[3] = {0.5f, 1.0f, 2.0f};
    float inc_c = (float)m->inc * (1.0f / 4294967296.0f); /* m.inc = f / FS * 2^32 */
    float knob = (float)p[P_E2] / 127.0f;
    float sus = (float)p[P_SUS] / 127.0f;
    float res = (float)p[P_E6] / 127.0f;
    float dev = (float)((m->shape >> 8) - 64) * (0.45f / 64.0f);   /* LFO -> SHAP: +-0.45 */
    float g, k;
    uint32_t i;
    if (n > CTL)
        n = CTL;
    inc_c *= OCTV[(uint32_t)p[P_E1] % 3u];
    if (inc_c > 0.45f)                                    /* polyp: hz <= sr * 0.45 */
        inc_c = 0.45f;
    p6_cutoff(P, m, v->note, &g);
    k = P->k_res;
    for (i = 0; i < n; i++) {
        float pw, pulse, saw, vco, y, vca, out_f;
        /* ---- the EG */
        if (S->stage == 1) {                              /* the RC charge: 99 % at 4.6 tau */
            S->env += (1.0f - S->env) * P->step_a;
            if (S->env >= 0.99f) {
                S->env = 1.0f;
                S->stage = 2;
            }
        }
        if (S->stage == 2 && !v->gate)                    /* polyp note_off */
            S->stage = 3;
        if (S->stage == 2)
            S->env = sus + (S->env - sus) * P->k_d;
        else if (S->stage == 3) {
            S->env *= P->k_r;
            if (S->env < 1e-2f)                           /* polyp: 5e-4 (past this the voice ends). The panel's
                                                           * TIME knob tops at tau 2.2 s, so a full-up release got
                                                           * past the framework's 12 s free watchdog (regress): the
                                                           * knob's own semantics are time-to-99 (env 1 %), so the
                                                           * release ends there, 10 s at tau 2.2 — right at the knob's
                                                           * headline, 2.5x further down than polyp's floor (a step
                                                           * at -40 dB, inaudible under any note ending) */
                S->env = 0.0f;
        }
        /* ---- the VCO */
        S->phase += inc_c;
        if (S->phase >= 1.0f)
            S->phase -= 1.0f;
        if (P->wave >= 2u) {                              /* PWM at the WIDTH knob's depth */
            S->pwm_phase += P->pwm_inc;
            if (S->pwm_phase >= 1.0f)
                S->pwm_phase -= 1.0f;
            {   /* the SINE table, lerped so PWM at 0.05 Hz does not step */
                uint32_t ti = (uint32_t)(S->pwm_phase * 1024.0f);
                float a = (float)SINE[ti & 1023u], b = (float)SINE[(ti + 1u) & 1023u];
                float sn = (a + (b - a) * (S->pwm_phase * 1024.0f - (float)ti)) * (1.0f / 32768.0f);
                pw = 0.5f + 0.45f * (knob * sn + dev);
            }
        } else
            pw = 0.5f * (1.0f - knob) + dev;              /* the knob narrows, shutoff at 1 */
        /* the manual: at the top of PW the pulse collapses and shuts the VCO off */
        pulse = pw <= 0.0f ? 0.0f : (S->phase < (pw < 0.97f ? pw : 0.97f) ? 1.0f : -1.0f);
        saw = (2.0f * S->phase - 1.0f) * 0.5f;
        vco = P->wave == 0u || P->wave == 2u ? pulse * 0.8f : saw + pulse * 0.5f;
        if (P->sub) {
            S->sub_phase += inc_c * (P->sub == 1u ? 0.5f : 0.25f);
            if (S->sub_phase >= 1.0f)
                S->sub_phase -= 1.0f;
            vco += S->sub_phase < 0.5f ? 0.5f : -0.5f;
        }
        /* ---- the ladder: the loop is algebraic and is solved (two Newton
         * steps from ring, voice.rs), not lagged */
        {
            uint32_t r = (uint32_t)S->nz * 1664525u + 1013904223u;
            float seed = (float)(r >> 8) * (1.0f / 8388608.0f) - 1.0f;   /* polyp noise(): -1..1 */
            float drive = vco * 0.7f + seed * 3e-3f * res;
            float y4 = S->ring, s1 = S->f[0], s2 = S->f[1], s3 = S->f[2], s4 = S->f[3];
            float g4 = g * g * g * g;
            /* the stages' state sum, under the feedback: of this sample's carries */
            float carried = (1.0f - g) * (g * g * g * s1 + g * g * s2 + g * s3 + s4);
            float u, v1, v2, v3, v4;
            int32_t j;
            S->nz = (int32_t)r;
            for (j = 0; j < 2; j++) {
                float c = drive - k * y4;
                float c2;
                float ft, fp;
                if (c < -3.0f)
                    c = -3.0f;
                else if (c > 3.0f)
                    c = 3.0f;
                c2 = c * c;
                ft = c * (27.0f + c2) / (27.0f + 9.0f * c2);
                /* the slope of ftanh (ftanh_prime); the clamp leaves it flat
                 * there, which cannot flip the step's divisor */
                fp = -g4 * k * ((27.0f - 162.0f * c2 + 9.0f * c2 * c2)
                                / ((27.0f + 9.0f * c2) * (27.0f + 9.0f * c2))) - 1.0f;
                y4 -= (g4 * ft + carried - y4) / fp;
            }
            S->ring = y4;
            u = p6_tanhf(drive - k * y4);
            /* the four stages, each y = G*(u - s) + s (the state carries the
             * sum of what came before it) */
            v1 = g * (u - s1);
            v2 = g * (v1 + s1 - s2);
            v3 = g * (v2 + s2 - s3);
            v4 = g * (v3 + s3 - s4);
            y = v4 + s4;
            S->f[0] = v1 + v1 + s1;
            S->f[1] = v2 + v2 + s2;
            S->f[2] = v3 + v3 + s3;
            S->f[3] = v4 + v4 + s4;
        }
        /* the 10 Hz DC blocker: the ladder drifts as the cutoff moves */
        S->dc_y = y - S->dc_x + 0.997f * S->dc_y;
        S->dc_x = y;
        y = S->dc_y;
        /* ---- the VCA: exponential, normalized so env 1 is still unity gain */
        vca = (p6_exp2f(2.88538997f * S->env) - 1.0f) * (1.0f / 6.389056099f);
        out_f = y * vca * 0.5f;
        if (out_f > 4.0f)                                 /* polyp's clamp */
            out_f = 4.0f;
        else if (out_f < -4.0f)
            out_f = -4.0f;
        out[i] += (int32_t)(out_f * (float)amp_at(m, i) * P6_GAIN);
    }
}

static int p6_done(track_t *t, voice_t *v)
{
    const p6_part_t *P = (const p6_part_t *)ENG_MEM((uint32_t)(t - trk));
    const p6_voice_t *S = P->v + (uint32_t)(v - t->v);
    return S->stage == 3u && S->env == 0.0f;
}

/* ------------------------------------------------------------ presets --- */

/* the factory set: 64 rows generated by tools/gen_p6_presets.py from polyp's
 * patches/02-factory (vendored at tools/p6_patches); the four hand rows were
 * bits of the same mapping laws and the generated BRASS/MOON SND/SYN LEAD/FAT
 * BASS rows are identical to them */

#include "felucca_p6.h"

/* ------------------------------------------------------------- engine --- */

static const engine_t ENG_POLYSIX = {
    .name = "POLYSIX",
    .page_title = {"VCO", "VCF"},
    .edit = {
        {"WAVE", F_ENUM, 0, 3, 3, N_P6_WAVE, 0},
        {"OCT", F_ENUM, 0, 2, 1, N_P6_OCT, 0},
        {"WIDTH", F_PCT, 0, 127, 32, 0, 0},
        {"PWM", F_LFOHZ, 0, 127, 91, 0, 0},
        {"SUB", F_ENUM, 0, 2, 2, N_P6_SUB, 0},
        {"CUTOFF", F_CUTOFF, 0, 127, 74, 0, 0},
        {"RES", F_PCT, 0, 127, 32, 0, 0},
        {"EG INT", F_BIPCT, -64, 63, 22, 0, 0},
    },
    .presets = P6_PRESETS,
    .npresets = NELEM(P6_PRESETS),
    .note_on = p6_note_on,
    .render = p6_render,
    .knob = {P_E5, P_E6, P_ATK, P_REL},
    .poly = P6_POLY,
    .block = p6_block,
    .ownenv = 1,
    .done = p6_done,
    .mem = sizeof(p6_part_t),
};
