#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Ingop Vasilj

"""The GPL-3 port's hand-made presets (juno/presets.cpp) -> the JUNO106 engine's rows.

  tools/gen_juno_presets.py OUT.h

The eight patches, each a full set of 47 VST2 parameter values (only the overrides
are listed; a blank knob sits at its default). The mapping onto the engine's
own knobs (eng_juno.c):

  SAW / SUB (+ its volume) / PW / PWM / NOISE   as authored (PW 0 = the pulse off)
  CUTOFF                                        nearest entry of CUTOFF_HZ (the knob
                                                = the Juno's 5 Hz .. 50 kHz law)
  RES                                           round(127 * resonance)
  EG                                            bipolar: up = the contour's
                                                round(63 * contour), down = the bias's
                                                -round(64 * bias) — a patch setting
                                                both (ACID) takes the difference
  ATK / DEC / REL                               the tal time (1.5 ms * span^v) as
                                                the framework's: its knob is the
                                                time to 99 %, so 4.6 * the ms, in
                                                the TIME knob's law 127*log10(ms)/4
  SUS                                           round(127 * sustain)
  the chorus (CHORUS1 / 2 bits)                 the preset's INSERT: FXI(IT_JCHO,
                                                its mode 1 / 2 / 3, full MIX)
Not carried (no row in the sound format for them; the user sets them, the
engine header's comment documents): the Juno's LFO1 (RATE, WAVE, DELAY,
RETRIG), the LFO -> VCF depth, KEY FOLLOW (the engine fixes it), the HPF
(tal'sINIT default: bypass), CUTOFF SMOOTH (the engine's 2 ms), the VOLUME
faders.
"""
import math
import re
import sys
from pathlib import Path

# juno/presets.cpp (make(): defaults, then the overrides)
DEFS = {"VOLUME": 0.8, "SAW": 1.0, "SUB": 1.0, "SUBVOL": 1.0, "NOISE": 0.0,
        "PULSE": 0.0, "PWMODE": 0.0, "PWINT": 0.0, "CUTOFF": 1.0, "RES": 0.0,
        "BIAS": 0.0, "CONTOUR": 1.0, "A": 0.0, "D": 0.618, "S": 0.6, "R": 0.0,
        "CH1": 0.0, "CH2": 0.0}
PRESETS = [
    ("INIT", {}),
    ("BASS", {"SUB": 0.0, "PULSE": 1.0, "PWINT": 0.45, "CUTOFF": 0.22, "RES": 0.35,
              "CONTOUR": 0.55, "A": 0.02, "D": 0.45, "S": 0.15, "R": 0.1}),
    ("BRASS", {"SAW": 0.0, "SUB": 1.0, "PULSE": 1.0, "PWINT": 0.5, "CUTOFF": 0.45,
               "RES": 0.2, "CONTOUR": 0.5, "A": 0.35, "D": 0.4, "S": 0.75, "R": 0.3,
               "CH1": 1.0}),
    ("STRINGS", {"SUBVOL": 0.5, "CUTOFF": 0.5, "RES": 0.1, "CONTOUR": 0.4,
                 "A": 0.55, "D": 0.5, "S": 0.85, "R": 0.45, "CH2": 1.0}),
    ("POLY PAD", {"SUBVOL": 0.6, "NOISE": 0.06, "CUTOFF": 0.55, "RES": 0.12,
                  "CONTOUR": 0.35, "A": 0.62, "S": 0.9, "R": 0.5, "CH1": 1.0, "CH2": 1.0}),
    ("ACID", {"SUB": 0.0, "PULSE": 1.0, "PWINT": 0.3, "PWMODE": 1.0, "CUTOFF": 0.18,
              "RES": 0.72, "CONTOUR": 0.85, "BIAS": 0.2, "A": 0.0, "D": 0.3,
              "S": 0.0, "R": 0.08}),
    ("BELL", {"SUB": 0.0, "PULSE": 1.0, "PWINT": 0.12, "CUTOFF": 0.75, "RES": 0.05,
              "CONTOUR": 0.3, "A": 0.0, "D": 0.55, "S": 0.0, "R": 0.7, "CH2": 1.0}),
    ("FX", {"SAW": 0.0, "SUB": 0.0, "NOISE": 0.35, "CUTOFF": 0.85, "RES": 0.4,
            "CONTOUR": 0.2, "A": 0.7, "S": 1.0, "R": 0.6, "CH1": 1.0, "CH2": 1.0}),
]
MAX_NAME = 12


def tval(ms):
    return max(0, min(127, int(127.0 * math.log10(max(ms, 1.0)) / 4.0 + 0.5)))


def main(out_path):
    out = Path(out_path)
    gen = out.parent
    tbl = (gen / "felucca_tables.h").read_text()
    tx = None
    for m in re.finditer(r"(?:^|\n)static const \w+ CUTOFF_HZ\[\d*\] = \{([^}]*)\}", tbl):
        tx = [int(x) for x in re.findall(r"\d+", m.group(1))]
    if not tx:
        raise SystemExit("gen_juno_presets: CUTOFF_HZ not found in felucca_tables.h (gen order?)")

    rows = []
    for name, over in PRESETS:
        v = {**DEFS, **over}
        pwv = None
        if v["PULSE"]:
            pwv = max(0, min(127, round(127.0 * (v["PWINT"] - 0.05) / 0.9)))
        e = [round(127.0 * v["SAW"]) if v["SAW"] > 0.5 else 0,
             round(127.0 * v["SUBVOL"]) if v["SUB"] > 0.5 else 0,
             pwv if pwv is not None else 0,
             round(127.0 * v["PWMODE"]),
             round(127.0 * v["NOISE"]),
             min(range(len(tx)), key=lambda i: (abs(tx[i] - 5.0 * 10000.0 ** v["CUTOFF"]), i)),
             round(127.0 * v["RES"]),
             round(63.0 * v["CONTOUR"]) - round(64.0 * v["BIAS"])]
        env = [tval(4.6e3 * 0.0015 * 2000.0 ** v["A"]),      # attack: 1.5 ms .. 3 s
               tval(4.6e3 * 0.0015 * 8000.0 ** v["D"]),      # 1.5 ms .. 12 s
               round(127.0 * v["S"]),
               tval(4.6e3 * 0.0015 * 8000.0 ** v["R"])]
        mode = (v["CH1"] > 0.5) | ((v["CH2"] > 0.5) << 1)    # the Juno's chorus I / II bits
        rows.append((name.upper().replace(" ", ""), e, env, mode))   # POLY PAD -> POLYPAD

    with out.open("w") as w:
        w.write("/* generated by tools/gen_juno_presets.py: JUNO106 presets from the port's\n"
                " * hand-made set (juno/presets.cpp — the mapping at the top of that tool;\n"
                " * the patches' own chorus through the preset's INSERT, IT_JCHO, its mode,\n"
                " * full MIX) */\n")
        w.write("#pragma once\n")
        w.write("static const preset_t JUNO_PRESETS[] = {\n")
        for name, e, env, mode in rows:
            fx = ", FXI(%d, %d, 0, 0)" % (IT_JCHO, mode) if mode else ""
            w.write("    {%-12s {%3d, %3d, %3d, %3d, %3d, %3d, %3d, %3d}, "
                    "{%3d, %3d, %3d, %3d}, 0, 0, FX(0, 0, 0, 0), 0%s},\n"
                    % ('"%s",' % name, *(e + env), fx))
        w.write("};\n")


IT_JCHO = 12     # fx.c: the enum, appended after IT_P6ENS (11)

if __name__ == "__main__":
    main(sys.argv[1])
