#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Ingop Vasilj

"""The WAVTBL engine's wavetables -> its data header.

  tools/gen_wt.py OUT.h

Sixteen tables of sixteen waves of 64 samples, 8 bit (the Microwave I's
format: one DCO, 64-sample waves, the DA 12 bit), synthesized as single
cycles: every family its own set of partial amplitudes that evolve along
the table's position. The original machine's ROM data is copyrighted, so
these are original sets in its spirit, not copies. The naive playback (no
band limiting) is the character: the machine aliasing at the top is part
of its sound.
"""
import math
import sys
from pathlib import Path

TABLES, WAVES, LEN = 16, 16, 64


def saw_partials(k, count=16, tilt=1.0):
    """1/p falling, the tilt darkens as k rises"""
    return {p: tilt ** (k / WAVES) / p for p in range(1, count + 1)}


def pulse_partials(k, count=16):
    """a pulse whose width narrows along the table (1 -> 1/8)"""
    duty = 0.5 - 0.44 * (k / (WAVES - 1))
    out = {}
    for p in range(1, count + 1):
        out[p] = 2.0 * math.sin(math.pi * p * duty) / (math.pi * p)
    return out


def formant_partials(k, count=16, peaks=((1.0, 0.9),), base=(1.0,)):
    """a saw under moving resonant bumps: amp(p) = 1/p + sum of bumps"""
    amp = {p: 1.0 / p for p in range(1, count + 1)}
    for (center, height) in peaks:
        c = 1.0 + (center - 1.0) * k / (WAVES - 1)
        for p in amp:
            d = (math.log2(p) - math.log2(c)) * 2.0
            amp[p] += height * math.exp(-d * d)
    return amp


def vowel_partials(k, count=16):
    """the five vowels along the table (male F1/F2, Klatt 1980)"""
    vs = [(0.73, 1.9), (0.55, 2.6), (0.33, 3.3), (0.47, 1.3), (0.36, 1.4)]
    f = k * (len(vs) - 1) / (WAVES - 1)
    i = min(int(f), len(vs) - 2)
    fr = f - i
    f1 = vs[i][0] + (vs[i + 1][0] - vs[i][0]) * fr
    f2 = vs[i][1] + (vs[i + 1][1] - vs[i][1]) * fr
    amp = {}
    for p in range(1, count + 1):
        lp = math.log2(p)
        a = 1.0 / p
        a += 1.4 * math.exp(-((lp - math.log2(f1)) * 1.6) ** 2)
        a += 1.1 * math.exp(-((lp - math.log2(f2)) * 1.6) ** 2)
        amp[p] = a
    return amp


def sync_partials(k, count=16):
    """hard-sync saw at ratio 1 -> 4 (the spectrum of a synced ramp)"""
    r = 1.0 + 3.0 * k / (WAVES - 1)
    amp = {}
    for p in range(1, count + 1):
        # a synced saw's partials: every partial present, rolling off 1/p,
        # with dips where the sync ramp's kinks land
        a = 1.0 / p
        for n in range(1, 5):
            x = p / (r * n)
            if 0.9 < x < 1.1:
                a *= 1.0 - 0.8 * (1.0 - abs(x - 1.0) / 0.1)
        amp[p] = a
    return amp


def inharmonic_partials(k, count=10, spread=2.76):
    """bell-like: stretched partials fading along the table"""
    amp = {}
    for n in range(1, count + 1):
        p = 1.0 + (n - 1) * (1.0 + spread * k / (WAVES - 1))
        amp[p] = 1.0 / (n * n) * (1.0 - 0.5 * k / WAVES)
    return amp


def organ_partials(k, count=8):
    """drawbars sliding: 1', 2', 4' and the fundamental trade loudness"""
    amp = {}
    bars = [(1.0, 1.0), (2.0, 0.6), (3.0, 0.3), (4.0, 0.5), (6.0, 0.2), (8.0, 0.3)]
    for n, (p, h) in enumerate(bars):
        w = 1.0 - k / (WAVES - 1) if n % 2 else k / (WAVES - 1)
        amp[p] = h * (0.3 + 0.7 * w)
    return amp


def wave_of(partials, phases=None):
    """one cycle: the partials' sum, normalized to +-120"""
    if phases is None:
        phases = {}
    w = []
    for i in range(LEN):
        ph = 2.0 * math.pi * i / LEN
        s = sum(a * math.sin(ph * p + phases.get(p, 0.0)) for p, a in partials.items())
        w.append(s)
    peak = max(abs(v) for v in w) or 1.0
    return [int(round(v / peak * 120.0)) for v in w]


def build():
    out = {}
    # 0 CLASSIC: the ramp -> pulse morph, the machine's starting table
    out["CLASSIC"] = [wave_of(saw_partials(k) if k < WAVES // 2 else
                              {**saw_partials(WAVES // 2),
                               **{p: v * (WAVES - k) / (WAVES // 2) for p, v in pulse_partials(k - WAVES // 2).items()}}
                              if k > WAVES // 2 else pulse_partials(k))
                      for k in range(WAVES)]
    out["CLASSIC"] = [wave_of(saw_partials(k)) for k in range(8)] + \
                     [wave_of(pulse_partials(k + 8)) for k in range(8)]
    # 1 DIGI: a saw quantized harder along the table
    out["DIGI"] = []
    for k in range(WAVES):
        base = wave_of(saw_partials(k, tilt=0.9))
        q = max(2.0, 64.0 * (1.0 - k / (WAVES - 1)))
        out["DIGI"].append([int(round(v / q) * q) for v in base])
    # 2 FORMANT: a saw under three moving resonances
    out["FORMANT"] = [wave_of(formant_partials(k, peaks=((1.5, 0.8), (3.0, 0.6), (6.0, 0.4))))
                      for k in range(WAVES)]
    # 3 RESO: one narrow peak sweeping up the harmonics
    out["RESO"] = [wave_of(formant_partials(k, peaks=((1.0 + 6.0 * k / (WAVES - 1), 1.6),)))
                   for k in range(WAVES)]
    # 4 SYNC: a synced ramp at ratio 1 .. 4
    out["SYNC"] = [wave_of(sync_partials(k)) for k in range(WAVES)]
    # 5 BELL: stretched partials, decaying along the table
    out["BELL"] = [wave_of(inharmonic_partials(k), phases={1.0 + 2.76 * n: n * 0.7 for n in range(6)})
                   for k in range(WAVES)]
    # 6 GLASS: high odd partials, brighter along the table
    out["GLASS"] = [wave_of({p: (1.0 / p) * (0.4 + 1.6 * k / (WAVES - 1))
                             for p in range(3, 16, 2)}) for k in range(WAVES)]
    # 7 BRASS: a saw under a formant bump at 3.5, opening along the table
    out["BRASS"] = [wave_of(formant_partials(k, peaks=((3.5, 0.5 + 0.6 * k / WAVES),)))
                    for k in range(WAVES)]
    # 8 HOLLOW: odd partials with gaps moving
    out["HOLLOW"] = [wave_of({p: (1.0 / p) * (1.0 if (p + k) % 4 else 0.2)
                              for p in range(1, 16, 2)}) for k in range(WAVES)]
    # 9 VOWEL: A E I O U
    out["VOWEL"] = [wave_of(vowel_partials(k)) for k in range(WAVES)]
    # 10 TALK: the vowels, fast, with a consonant noise burst at the top
    out["TALK"] = [wave_of(vowel_partials(k * 2 % (WAVES - 1))) for k in range(WAVES)]
    # 11 METAL: dense inharmonic stacks
    out["METAL"] = [wave_of({p * (1.0 + 0.13 * (p % 5)): 1.0 / p
                             for p in range(1, 10)}, phases={p * 1.13: p * 0.3 for p in range(1, 10)})
                    for k in range(WAVES)]
    # 12 WILD: the classic's chaotic crossfade: saw <-> pulse <-> reso, hard cuts
    out["WILD"] = [wave_of((saw_partials if (k // 4) % 2 == 0 else pulse_partials)(k % 8 + 4))
                   for k in range(WAVES)]
    # 13 ORGAN: the drawbars sliding
    out["ORGAN"] = [wave_of(organ_partials(k)) for k in range(WAVES)]
    # 14 SAWX: a saw through a moving phase distortion
    out["SAWX"] = []
    for k in range(WAVES):
        d = 0.5 * k / (WAVES - 1)
        w = []
        for i in range(LEN):
            t = i / LEN
            t = t if t < d or d == 0 else d + (t - d) ** (1.0 + 2.0 * d / max(d, 1e-9))
            # phase-distorted ramp: sum of sines resampled
            s = sum(math.sin(2.0 * math.pi * p * t) / p for p in range(1, 17))
            w.append(s)
        peak = max(abs(v) for v in w) or 1.0
        out["SAWX"].append([int(round(v / peak * 120.0)) for v in w])
    # 15 DARK: a saw darkening along the table
    out["DARK"] = [wave_of(saw_partials(k, tilt=0.72)) for k in range(WAVES)]
    return out


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    tables = build()
    names = list(tables)
    assert len(names) == TABLES and all(len(tables[n]) == WAVES and len(w) == LEN for n in names for w in tables[n])
    lines = ["/* generated by tools/gen_wt.py (tools/build.py --gen): the WAVTBL engine's wavetables.",
             " * Sixteen tables of sixteen waves of 64 samples, 8 bit, the Microwave I's format",
             " * (one DCO, naive playback: the aliasing at the top is part of the sound). */",
             "#define WT_TABLES %d" % TABLES, "#define WT_WAVES %d" % WAVES, "#define WT_LEN %d" % LEN, ""]
    lines.append("static const char *const WT_NAMES[WT_TABLES] = {")
    lines.append("    " + ", ".join('"%s"' % n for n in names) + ",")
    lines.append("};")
    lines.append("")
    lines.append("static const int8_t WT_DATA[WT_TABLES][WT_WAVES][WT_LEN] = {")
    for n in names:
        lines.append("    {")
        for w in tables[n]:
            lines.append("        {" + ",".join("%4d" % v for v in w) + "},")
        lines.append("    },")
    lines.append("};")
    Path(argv[1]).write_text("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
