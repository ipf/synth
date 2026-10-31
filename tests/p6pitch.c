/* p6pitch: per-preset, what pitch and knobs the track actually holds */
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <stdio.h>
int main(int argc, char **argv)
{
    int pi = argc > 1 ? atoi(argv[1]) : 0;
    host_tracks_init();
    track_t *t = &trk[0];
    t->eng_req = t->engine = 14u;
    eng_mem_claim(t);
    host_preset(t, 14u, (uint32_t)pi);
    printf("preset %d: E0..7 =", pi);
    for (uint32_t i = 0; i < 8u; i++) printf(" %d", t->p[P_E0 + i]);
    printf(" | ATK %d DEC %d SUS %d REL %d VOICE %d |\n", t->p[P_ATK], t->p[P_DEC], t->p[P_SUS], t->p[P_REL], t->p[P_VOICE]);
    input_on(t, 7, 100);
    int32_t o[2 * CTL];
    mix_block(o, CTL);
    mix_block(o, CTL);
    printf("note pitch_cur %d pitch16 %d gate %d octave %d\n", t->v[0].pitch_cur, t->v[0].pitch16, t->v[0].gate, song.octave);
    return 0;
}
