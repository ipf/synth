/* p6cost: one track's per-block cost under the new Polysix INSERT types (debug-only, not suite-linked):
 * BRASS held, MIX 100 %, against TYPE OFF and the framework's own CHOR.
 * compile: cc -O1 -w -Ibuild/gen -Ifirmware/src -Itests -o p6cost tests/p6cost.c -lm   (from the repo root) */
/* the attribute first: hostsim.c below does #define __attribute__(x) empty (attributes from an earlier
 * declaration still merge onto the definition) */
static void p6cost_run(void) __attribute__((constructor));
#include "insert_test.c"

static void p6cost_run(void)
{
    static const int32_t TY[5] = {0, IT_P6CHO, IT_P6PHAS, IT_P6ENS, IT_CHORUS};
    static const char *const NM[5] = {"off", "CHOR6", "PHAS6", "ENSEM", "CHOR (own)"};
    uint32_t k, it;
    int32_t out_buf[2 * CTL];
    fresh(eng_by_name("POLYSIX"), 0u);              /* the boot state, track 1 BRASS */
    trk_note_on(&trk[0], 60, 100);
    for (k = 0; k < 5u; k++) {
        clock_t c0;
        trk[0].p[P_ITYPE] = (int16_t)TY[k];
        trk[0].p[P_IMIX] = 127;
        it = 0;
        c0 = clock();
        do {
            mix_block(out_buf, CTL);
            it++;
        } while (it < 20000u);
        printf("p6cost: %-11s %.2f us/block (CTL samples)\n", NM[k],
               (double)(clock() - c0) * 1e6 / (double)CLOCKS_PER_SEC / 20000.0);
    }
    exit(0);
}
