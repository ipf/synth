/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Ingop Vasilj */
/* Host test of the WAVTBL engine (firmware/src/eng_wt.c): the presets and
 * engine params, the tables and positions, the filter (the cutoff sweep, the
 * four modes, key tracking), a retrigger's kept filter and DCO phase, a fresh
 * voice's DCO restart, and the DCO's octave. */
#define main hostsim_main
#include "hostsim.c"
#undef main
static int fails;
static void check(const char *label, int ok) { printf("wt: %s %s\n", label, ok ? "ok" : "FAIL"); fails += !ok; }
static uint64_t energy(const int32_t *b) { uint64_t e = 0; for (uint32_t i = 0; i < CTL; i++) e += (int64_t)b[i] * b[i]; return e; }
static int32_t peak(const int32_t *b) { int32_t p = 0; for (uint32_t i = 0; i < CTL; i++) { int32_t a = b[i] < 0 ? -b[i] : b[i]; if (a > p) p = a; } return p; }
static void play(track_t *t, voice_t *v, int32_t *a, const vmod_t *m)
{   /* a note from rest: the filter and the noise state zeroed (a render carries them) */
    v->s[0] = v->s[1] = v->s[2] = v->s[3] = v->s[4] = 0;
    wt_note_on(t, v);
    memset(a, 0, CTL * sizeof(int32_t));
    wt_render(t, v, a, CTL, m);
}
int main(void)
{
    int32_t a[CTL];
    track_t *t = &trk[0];
    voice_t *v = &t->v[0];
    vmod_t m = {0};
    memset(trk, 0, sizeof trk); host_tracks_init();
    m.pitch16 = 60 * 16;                          /* the render reads it (voice.c writes it) */
    m.inc = pitch_inc(m.pitch16);
    m.amp0 = m.amp1 = m.envq15 = 32767; m.shape = 64 << 8;
    v->vel = 96; v->note = 60; v->gate = 1;
    host_preset_req(t, ENGI_WT, 0);
    {   /* every preset: sound, no overflow, every engine param alone too */
        int ok = 1, bounded = 1;
        for (uint32_t pi = 0; pi < ENG_WT.npresets; pi++) {
            host_preset_req(t, ENGI_WT, pi);
            play(t, v, a, &m);
            ok &= energy(a) > 0;
            bounded &= peak(a) < (1 << 20);
            for (uint32_t k = 0; k < 8u; k++) {
                int16_t keep = t->p[P_E0 + k];
                t->p[P_E0 + k] = keep >= 127 ? 0 : keep + 1;
                play(t, v, a, &m);
                ok &= energy(a) > 0;
                bounded &= peak(a) < (1 << 20);
                t->p[P_E0 + k] = keep;
            }
        }
        check("every preset and every engine param alone: sound, no overflow", ok && bounded);
    }
    {   /* every table and position: sound, bounded */
        int ok = 1, bounded = 1;
        for (uint32_t wv = 0; wv < WT_TABLES; wv++) {
            t->p[P_E0] = (int16_t)wv;
            for (uint32_t pos = 0; pos <= 127; pos += 8) {
                t->p[P_E1] = (int16_t)pos;
                play(t, v, a, &m);
                ok &= energy(a) > 0;
                bounded &= peak(a) < (1 << 20);
            }
        }
        check("every table and position: sound, bounded", ok && bounded);
    }
    {   /* the filter: the cutoff sweep, the four modes, key tracking */
        uint64_t e_lo, e_hi, e_lp, e_bp, e_hp, e_not;
        host_preset_req(t, ENGI_WT, 0);
        t->p[P_E5] = 0; t->p[P_E6] = 0;
        t->p[P_E3] = 8; play(t, v, a, &m); e_lo = energy(a);
        t->p[P_E3] = 120; play(t, v, a, &m); e_hi = energy(a);
        check("the cutoff sweep opens (a darker low cutoff)", e_hi > e_lo * 2);
        t->p[P_E3] = 90;
        t->p[P_E5] = 0; play(t, v, a, &m); e_lp = energy(a);
        t->p[P_E5] = 1; play(t, v, a, &m); e_bp = energy(a);
        t->p[P_E5] = 2; play(t, v, a, &m); e_hp = energy(a);
        t->p[P_E5] = 3; play(t, v, a, &m); e_not = energy(a);
        check("LP BP HP NOT: sound, the four differ", e_lp > 0 && e_bp > 0 && e_hp > 0 && e_not > 0 &&
              e_bp != e_lp && e_hp != e_bp && e_not != e_hp);
        {   /* key tracking: 127 brightens a note above C4, darkens one below */
            uint64_t e0, e1, e2;
            t->p[P_E5] = 0; t->p[P_E3] = 60;
            m.pitch16 = 96 * 16;
            t->p[P_E6] = 0; play(t, v, a, &m); e0 = energy(a);
            t->p[P_E6] = 127; play(t, v, a, &m); e1 = energy(a);
            m.pitch16 = 36 * 16;
            play(t, v, a, &m); e2 = energy(a);
            check("key tracking: bright above C4, dark below", e1 > e0 * 2 && e2 < e0);
            m.pitch16 = 60 * 16;
        }
    }
    {   /* a retrigger keeps the filter and the DCO phase; a fresh voice restarts the DCO */
        host_preset_req(t, ENGI_WT, 0);
        v->env = 1 << 24;                             /* the voice sounds: a retrigger keeps everything */
        play(t, v, a, &m);
        v->gate = 0;
        memset(a, 0, sizeof a); wt_render(t, v, a, CTL, &m);   /* the release runs (mutates s, ph) */
        int32_t keep = v->s[0];
        uint32_t ph = v->ph[0];
        v->gate = 1;
        wt_note_on(t, v);
        check("a retrigger keeps the filter and the DCO phase", v->s[0] == keep && v->ph[0] == ph);
        v->env = 0; v->env_out = 0;
        wt_note_on(t, v);
        check("a fresh voice restarts the DCO", v->ph[0] == 0);
    }
    {   /* the DCO's octave: OCT -12 plays an octave down (half the phase advance) */
        uint32_t hi, lo;
        v->env = 0; v->env_out = 0;                   /* fresh voices: the DCO restarts each note */
        t->p[P_E2] = 0; play(t, v, a, &m); hi = v->ph[0];
        t->p[P_E2] = -12; play(t, v, a, &m); lo = v->ph[0];
        check("OCT -12 advances half as far (an octave down)", lo < hi && lo > hi / 3);
        t->p[P_E2] = 0;
    }
    return fails != 0;
}
