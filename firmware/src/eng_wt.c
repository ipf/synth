/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Ingop Vasilj */
/* WAVTBL: a Microwave I-style wavetable engine. 8 voices (poly 0 = NVOICE).
 * The machine: one wavetable DCO, a 12 bit DA over 8 bit waves, a CEM 3372
 * (a four-pole low pass with resonance and a high-pass in the mix), one
 * amp envelope, one filter envelope with its own decay, a noise source.
 * This engine keeps the house structure: the shared ADSR is the amp
 * envelope, the filter envelope comes in through m->cutoff (the shared
 * ENV -> FILTER routing, a preset's fenv, and LFO -> FILTER), the noise
 * through NOIS. The wavetable data (tools/gen_wt.py) is original,
 * synthesized in the machine's format; the machine's ROM data is
 * copyrighted.
 *
 *   DCO      one table (WAV) of sixteen waves, the position (POS) picks
 *            the wave and crossfades into the next; naive playback, no
 *            band limiting: the aliasing at the top is the machine's
 *            character
 *   OCT      the pitch (semitones, -24 .. +24: the machine's -2 .. +2
 *            octaves and finer)
 *   FILTER   the CEM 3372 as two of the house's trapezoidal SVFs in
 *            cascade (dsp.c tsvf_lp): four poles, the resonance (RES) of
 *            the first; TYP mixes: LP / BP / HP (the first stage's tap
 *            through the second), NOT = the input less the band (the
 *            3372's LP + HP mix idea); the cutoff (CUT, the house law)
 *            sweeps with ENV -> FILTER and LFO -> FILTER, key tracking
 *            (KTRK) around C4
 *
 * Voice state: ph[0] the DCO phase (a fresh voice restarts it, the
 * machine's DCO does), s[0..1] the first SVF's integrators, s[2..3] the
 * second's, s[4] the noise (kept over a retrigger: the filter and the
 * noise are analog). */
#define WT_POLY 0
#define WT_KEEP 0x1fu
#define WT_KTRK_MUL 4825                        /* KTRK: an octave of pitch at 127 is an octave of cutoff
                                                 * (192 * 127 * 4825 >> 15 = 3590 cutoff steps Q8, 14.02 SVF
                                                 * indices, one octave of the SVF's law) */

#include "felucca_wt.h"

static const char *const N_WT_TYP[] = {"LP", "BP", "HP", "NOT"};   /* TYP (the 3372's mixes) */

static void wt_note_on(track_t *t, voice_t *v)
{
    (void)t;
    if (!v->env && !v->env_out)                       /* a fresh voice (not a retrigger) */
        v->ph[0] = 0;                                 /* the DCO restarts (the machine's does) */
}

/* the cutoff in Q8 house steps: CUT, then ENV -> FILTER and LFO -> FILTER
 * (m->cutoff, the shared routing), then key tracking around C4 */
static int32_t wt_cut(const int16_t *p, const vmod_t *m)
{
    int32_t cut = (int32_t)p[P_E3] << 8;
    cut += m->cutoff;
    cut += (int32_t)(((int64_t)(m->pitch16 - 60 * 16) * p[P_E6] * WT_KTRK_MUL) >> 15);
    return clamp(cut, 0, 127 << 8);
}

/* the DCO: the position (POS) picks the wave and crossfades into the next */
static int32_t wt_osc(const int16_t *p, const vmod_t *m, uint32_t *ph, int32_t *nz)
{
    int32_t wpos, wi, wf, i, fr, s, n;
    uint32_t inc, x;
    inc = pitch_inc((uint32_t)clamp((int32_t)m->pitch16 + (int32_t)p[P_E2] * 16, 0, 2047));
    x = *ph + inc;
    *ph = x;
    i = (x >> 26) & (WT_LEN - 1);                     /* the wave's sample: 64, 8 bit interpolation */
    fr = (x >> 18) & 255;
    wpos = (int32_t)(((int32_t)p[P_E1] * (WT_WAVES - 1) * 256) / 127);
    wi = wpos >> 8;                                   /* the wave under the position */
    wf = wpos & 255;
    if (wi >= WT_WAVES - 1) {                         /* the top: the last wave, no crossfade */
        wi = WT_WAVES - 1;
        wf = 0;
    }
    {
        const int8_t *w = WT_DATA[p[P_E0] & (WT_TABLES - 1)][wi];
        int32_t a = w[i] + ((((int32_t)w[(i + 1) & (WT_LEN - 1)] - w[i]) * fr) >> 8);   /* Q7 */
        if (wf && wi + 1 < WT_WAVES) {                /* the crossfade into the next wave */
            const int8_t *wn = WT_DATA[p[P_E0] & (WT_TABLES - 1)][wi + 1];
            int32_t b = wn[i] + ((((int32_t)wn[(i + 1) & (WT_LEN - 1)] - wn[i]) * fr) >> 8);
            a += ((b - a) * wf) >> 8;
        }
        s = a << 8;                                   /* Q15 */
    }
    if (p[P_E7]) {                                    /* the noise (the machine's) */
        n = (int32_t)(noise32(nz) >> 16) - 32768;     /* Q15 */
        s += (n * p[P_E7]) >> 7;
        s = clamp(s, -32768, 32767);
    }
    return s;
}

static void wt_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    tsvf_t f1, f2;
    int32_t cut = wt_cut(p, m), res = p[P_E4], ft = p[P_E5], i;
    uint32_t ph = v->ph[0], nz = (uint32_t)v->s[4];
    int32_t c1a = v->s[0], c1b = v->s[1], c2a = v->s[2], c2b = v->s[3];
    int32_t fk = 8192 - res * 7600 / 127;             /* tsvf_coef's damping, Q12 (ANALOG_K) */
    tsvf_coef(&f1, cut, res);
    tsvf_coef(&f2, cut, 0);                           /* the second stage: no resonance of its own */
    for (i = 0; i < n; i++) {
        int32_t s = wt_osc(p, m, &ph, (int32_t *)&nz), y, y1;
        if (ft == 0u) {                               /* LP: the cascade, four poles */
            y1 = tsvf_lp(&f1, s >> 1, &c1a, &c1b);
            y = tsvf_lp(&f2, y1, &c2a, &c2b);
        } else if (ft == 3u) {                        /* NOT: the input less the first stage's band */
            y = (s >> 1) - analog_tap(&f1, fk, s >> 1, &c1a, &c1b, 1u);
        } else {                                      /* BP / HP: the first stage's tap, then the second */
            y = tsvf_lp(&f2, analog_tap(&f1, fk, s >> 1, &c1a, &c1b, ft), &c2a, &c2b);
        }
        {   /* linear up to half scale, then a soft knee (only resonance peaks saturate), as the house's */
            int32_t a = y < 0 ? -y : y;
            if (a > 16000) {
                a = 16000 + (softclip((a - 16000) * 2) >> 1);
                y = y < 0 ? -a : a;
            }
            y *= 2;
        }
        out[i] += voice_amp(y, m, i) * 2;
    }
    v->ph[0] = ph;
    v->s[0] = c1a;
    v->s[1] = c1b;
    v->s[2] = c2a;
    v->s[3] = c2b;
    v->s[4] = (int32_t)nz;
}

static const preset_t WT_PRESETS[] = {
    /* WAV POS OCT CUT | RES TYP KTRK NOIS */
    {"FAT BASS",   {0, 18, 0, 62, 52, 0, 0, 0}, {0, 55, 0, 70}, 0, 1, FX(0, 0, 0, 15), PAT(8)},
    {"SYNC LEAD",  {4, 40, 0, 78, 78, 0, 12, 0}, {0, 60, 0, 55}, 0, 1, FX(0, 20, 30, 20), PAT(4)},
    {"RESONATOR",  {3, 60, 0, 70, 110, 0, 20, 0}, {0, 62, 0, 40}, 0, 0, FX(0, 15, 20, 40), PAT(3)},
    {"GLASS BELLS",{6, 90, 0, 90, 34, 0, 0, 0}, {0, 70, 0, 45}, 0, 0, FX(0, 25, 35, 55), PAT(6)},
    {"VOX WAVES",  {9, 32, 0, 84, 30, 0, 8, 0}, {35, 70, 100, 80}, 0, 0, FX(0, 45, 20, 70), PAT(5)},
    {"CRYSTAL",    {5, 70, 0, 96, 40, 0, 0, 0}, {0, 65, 0, 50}, 0, 0, FX(0, 30, 45, 70), PAT(6)},
    {"PAN FLUTE",  {8, 45, 0, 74, 44, 0, 16, 0}, {25, 75, 100, 75}, 0, 0, FX(0, 20, 40, 55), PAT(5)},
    {"WILD SWEEP", {12, 64, 0, 68, 90, 0, 0, 0}, {0, 60, 0, 30}, 0, 0, FX(0, 10, 55, 60), PAT(7)},
    {"ELECTRO PLK",{14, 25, 0, 82, 96, 0, 0, 0}, {0, 55, 0, 35}, 0, 0, FX(10, 0, 15, 20), PAT(3)},
    {"DIGI PAD",   {1, 55, 0, 70, 38, 0, 12, 0}, {40, 80, 110, 95}, 0, 0, FX(0, 70, 25, 85), PAT(5)},
    {"METAL HIT",  {11, 80, 0, 88, 60, 0, 0, 0}, {0, 40, 0, 25}, 0, 0, FX(20, 0, 40, 45), PAT(2)},
    {"ORGAN WAVES",{13, 55, 0, 88, 24, 0, 0, 0}, {8, 40, 127, 45}, 0, 0, FX(0, 30, 20, 35), PAT(5)},
};

static const engine_t ENG_WT = {
    .name = "WAVTBL",
    .page_title = {"WAV", "FLT"},
    .edit = {
        {"WAV", F_ENUM, 0, WT_TABLES - 1, 0, WT_NAMES, 0},
        {"POS", F_PCT, 0, 127, 64, 0, 0},
        {"OCT", F_SEMI, -24, 24, 0, 0, 0},
        {"CUT", F_CUTOFF, 0, 127, 100, 0, 0},
        {"RES", F_PCT, 0, 127, 24, 0, 0},
        {"TYP", F_ENUM, 0, 3, 0, N_WT_TYP, 0},
        {"KTRK", F_PCT, 0, 127, 0, 0, 0},
        {"NOIS", F_PCT, 0, 127, 0, 0, 0},
    },
    .presets = WT_PRESETS,
    .npresets = NELEM(WT_PRESETS),
    .note_on = wt_note_on,
    .render = wt_render,
    .knob = {P_E3, P_E4, P_E0, P_E1},   /* HOME's four knobs: CUT RES WAV POS */
    .poly = WT_POLY,
    .keep = WT_KEEP,
};
