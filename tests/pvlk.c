/* pvlk: the POLIVOKS engine (ours, debug-only, not suite-linked): the six presets load dry and play
 * bounded, the self-oscillation ends the voice, and the filter unlatches when the resonance drops.
 * compile: cc -O1 -w -Ibuild/gen -Ifirmware/src -Ifirmware/hal -Ifirmware/bsp/chip
 *          -Ifirmware/bsp/boards/fm1 -Itests -o build/host/pvlk tests/pvlk.c -lm   (from the repo root) */
/* the attribute first: hostsim.c below does #define __attribute__(x) empty (attributes from an earlier
 * declaration still merge onto the definition) */
static void pvlk_run(void) __attribute__((constructor));
#include "insert_test.c"

static int32_t pvlk_o[2 * CTL];

static float pvlk_peak(uint32_t blocks)
{
    uint32_t i, n;
    float peak = 0.0f;
    for (n = 0; n < blocks; n++) {
        for (i = 0; i < 2u * CTL; i++)
            pvlk_o[i] = 0;
        mix_block(pvlk_o, CTL);
        for (i = 0; i < 2u * CTL; i++) {
            float a = pvlk_o[i] < 0 ? -(float)pvlk_o[i] : (float)pvlk_o[i];
            if (a > peak)
                peak = a;
        }
    }
    return peak;
}

static void pvlk_run(void)
{
    static const char *const NM[6] = {"BASS", "ACID", "LEAD", "PAD", "PLUCK", "GRIT"};
    const uint32_t e = eng_by_name("POLIVOKS");
    uint32_t k;

    if (e == 0u) {
        printf("pvlk: POLIVOKS not registered\n");
        return;
    }
    /* the six presets: dry (they carry FX(0, 0, 0, 0)), a held note plays, bounded */
    for (k = 0; k < 6u; k++) {
        const uint32_t pi = preset_by_name(e, NM[k]);
        track_t *t;
        int ok;
        float peak;
        if (pi != k) {
            printf("pvlk: the preset names            FAIL\n");
            bad++;
            return;
        }
        fresh(e, pi);
        t = &trk[0];
        ok = t->p[P_DIST] == 0 && t->p[P_CHOR] == 0 && t->p[P_DLY] == 0 && t->p[P_REV] == 0 &&
             t->p[P_E0] == ENGINES[e]->presets[pi].e[0];
        trk_note_on(t, 60, 100);
        peak = pvlk_peak(8u);
        ok = ok && peak > 100.0f && peak < 97000.0f;
        printf("pvlk: %-8s peak %.0f %s\n", NM[k], peak, ok ? "ok" : "FAIL");
        bad += !ok;
    }
    {   /* self oscillation: RES max, the gate up: the loop grows to the rails and the
         * release ends the voice; then RES down: the filter unlatches (the caps cannot
         * wind past the supply) */
        track_t *t;
        uint32_t n;
        int ended = 0;
        float peak;
        fresh(e, preset_by_name(e, "GRIT"));
        t = &trk[0];
        t->p[P_E0] = 85;                           /* ~2 kHz */
        t->p[P_E1] = 127;                          /* RES max */
        t->p[P_E2] = 0;                            /* ENV off */
        trk_note_on(t, 60, 100);
        peak = pvlk_peak(60u);                     /* 2 s: the loop grows to the rails */
        trk_note_off(t, 60);
        for (n = 0; n < 200u && !ended; n++) {     /* the release ends the voice */
            pvlk_peak(1u);
            ended = !t->v[0].active;
        }
        printf("pvlk: %-8s peak %.0f %s, %s\n", "self-osc", peak, peak < 97000.0f ? "ok" : "FAIL",
               ended ? "ended ok" : "NOT ENDED");
        bad += peak >= 97000.0f || !ended;
        /* RES down: the filter unlatches, the last block back in the linear region */
        t->p[P_E1] = 14;
        trk_note_on(t, 60, 100);
        pvlk_peak(20u);
        {
            const float p2 = pvlk_peak(1u);
            printf("pvlk: %-8s peak %.0f %s\n", "unlatch", p2, p2 < 24000.0f ? "ok" : "FAIL");
            bad += p2 >= 24000.0f;
        }
    }
    printf("pvlk: %s\n", bad ? "FAILED" : "all checks passed");
}
