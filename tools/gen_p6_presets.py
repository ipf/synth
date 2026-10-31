#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""polyp's factory patches (patches/02-factory) -> the POLYSIX engine's preset rows.

  tools/gen_p6_presets.py OUT.h

The Korg factory set (files A1..B8, 32 patches per file): every value maps onto the
engine's own knobs as polyp stores, i.e. what the four hand rows in eng_polysix.c
already carry:

  wave / octave / sub      as authored
  PW                       round(127 * pw_pwm)
  PWM SPEED                the patch's MG RATE through the engine's LFO_HZ_X100 law,
                           127*ln(hz/0.05)/ln(40/0.05) (polyp's own PWM_SPEED osc is a
                           separate one — the four hand rows all carry the MG's, so
                           the whole set does: MOON at 2.7 Hz, not polyp's 7.6)
  EG INTENSITY             round(63 * eg_intensity)   (the engine turns that octaves)
  CUTOFF                   nearest entry of CUTOFF_HZ (read from build/gen: the knob
                           = polyp's cutoff, the pole corner)
  RESONANCE                round(127 * resonance)
  ATK                      127*log10(atk_ms)/4   (time to the peak)
  DEC / REL                times-to-99: 127*log10(4.6*tau_ms)/4, clamped (rel 15 s
                           becomes 127 — the panel's 10 s floor, the one compromise)
  VCA gate (vca_mode 1)    SUS 127, REL 32 (~10 ms gate close); unchanged elsewhere
  fenv mono fx pattern     0: the EG INTENSITY already opens the filter; the rest is live
"""
import json
import math
import re
import sys
from pathlib import Path

HAND = {1: "BRASS", 7: "MOON SND", 17: "SYN LEAD", 18: "FAT BASS"}
# 17 (C1 Lead Sound): polyp's file says sustain 0 / vca_mode 0, held full only
# through its 15 s decay — the hand row in eng_polysix.c settled on the gate
# reading instead (full while held, a ~30 ms tail), the other three agree with
# the mechanical mapping
HAND_ENV = {17: (0, 127, 127, 32)}
MAX_NAME = 12
# polyp params::DEFS defaults: a patch only lists what it changes; a blank knob
# (absent from the JSON) sits at the panel default, not at zero
DEFS = {"pw_pwm": 0.25, "pwm_speed": 6.0, "mg_freq": 2.0, "mg_delay": 0.0,
        "attenuator": 0.0, "fx_speed": 0.35}


def tval_ms(ms):
    return max(0, min(127, int(127.0 * math.log10(max(ms, 1.0)) / 4.0 + 0.5)))


def main(out_path):
    out = Path(out_path)
    gen = out.parent
    # CUTOFF_HZ out of the tables generator's own output (the knob's pole corners, 128)
    tbl = (gen / "felucca_tables.h").read_text()
    tx = None
    for m in re.finditer(r"(?:^|\n)static const \w+ CUTOFF_HZ\[\d*\] = \{([^}]*)\}", tbl):
        tx = [int(x) for x in re.findall(r"\d+", m.group(1))]
    if not tx:
        raise SystemExit("gen_p6_presets: CUTOFF_HZ not found in felucca_tables.h (gen order?)")

    src = Path(__file__).parent / "p6_patches"
    files = sorted(src.glob("*.json"))
    if len(files) != 64:
        raise SystemExit(f"gen_p6_presets: {len(files)} patches under {src} (want 64)")
    rows = []
    for p in files:
        pid = int(re.match(r"(\d+)-", p.name).group(1))
        v = json.loads(p.read_text())
        docname = v["name"]                        # "A B4 Harpsicord" — plate + row + name
        v["values"] = {**DEFS, **v["values"]}      # blank knobs keep the panel default
        v = v["values"]
        e = [v["wave"] & 127, v["octave"] & 3, max(0, min(127, int(127.0 * v["pw_pwm"] + 0.5))),
             max(0, min(127, int(127.0 * math.log(v["mg_freq"] / 0.05) / math.log(40.0 / 0.05) + 0.5))),
             v["sub_osc"] & 3,
             min(range(len(tx)), key=lambda k: (abs(tx[k] - v["cutoff"]), k)),
             max(0, min(127, int(127.0 * v["resonance"] + 0.5))),
             max(0, min(63, int(63.0 * v["eg_intensity"] + 0.5)))]
        gate = v["vca_mode"] == 1
        env = [tval_ms(v["attack"] * 1000.0),
               tval_ms(4.6 * v["decay"] * 1000.0),
               127 if gate else max(0, min(127, int(127.0 * v["sustain"] + 0.5))),
               32 if gate else tval_ms(4.6 * v["release"] * 1000.0)]
        if pid in HAND_ENV:
            env = list(HAND_ENV[pid])
        # polyp's fx section per patch: mode 1 chorus, 2 phase, 3 ensemble (the KLM-368's CH / PH / ES), the
        # fx_speed its rate — the engine's INSERT INT knob, DEPTH 127 as authored, FDBK 0 (polyp has none):
        # written so a sound load puts them on the INSERT (ui.c apply_preset_to). IT_P6CHO / IT_P6PHAS /
        # IT_P6ENS are append-only (fx.c): 9, 10, 11
        ins = [(int(v["fx_mode"]) + 8 if v["fx_mode"] else 0,
                max(0, min(127, int(127.0 * v.get("fx_speed", DEFS["fx_speed"]) + 0.5))))]
        name = HAND.get(pid)
        if not name:                               # "B D6 Cave Echo" -> "BD6 CAVE ECHO"? -> "D6 CAVE
                                                   # ECHO": the plate letter, then the row, kept
            plate, _, row = re.match(r"^\d+-(a|b)-([a-d])(\d)", p.name).groups()
            name = (row.upper() + plate.upper() + " " + " ".join([w for w in docname.split()[1:]
                                                                 if not re.match(r"^[ABCD][1-8]$", w)])).strip().upper()
            name = name[:MAX_NAME].strip()
        rows.append((pid, name, e, env, gate, ins))
    rows.sort(key=lambda r: r[0])

    with out.open("w") as w:
        w.write("/* generated by tools/gen_p6_presets.py: POLYSIX presets from polyp's factory set\n"
                " * (patches/02-factory, A file A1..D8 then B file A1..B8 — the mapping at the top of\n"
                " * that tool; each polyp value lands on the engine's own knobs, the VCA-gate patches\n"
                " * through the EG's full level/closed gate here, the patches' own effect through the\n"
                " * preset's INSERT (FXI: the Polysix's CHOR6 / PHAS6 / ENSEM, its rate, full MIX)) */\n")
        w.write("#pragma once\n")
        w.write(f"#define P6_NFACTORY {len(rows)}\n")
        w.write("static const preset_t P6_PRESETS[P6_NFACTORY] = {\n")
        for pid, name, e, env, gate, ins in rows:
            w.write("    /* polyp patches/02-factory %02d */\n" % pid)
            fx = ", FXI(%d, %d, 127, 0)" % (ins[0][0], ins[0][1]) if ins[0][0] else ""
            w.write('    {%-12s {%3d, %3d, %3d, %3d, %3d, %3d, %3d, %3d}, {%3d, %3d, %3d, %3d}, 0, 0, FX(0, 0, 0, 0), 0%s},%s\n'
                    % ('"%s",' % name, *(e + env), fx, "   /* VCA gate */" if gate else ""))
        w.write("};\n")

    # the web editor's mock keeps its own POLYSIX presets: rewrite its presets
    # line from the same rows, so the mock cannot drift from the firmware
    ed = (Path(__file__).parent.parent / "web" / "editor.html")
    if ed.exists():
        js = []
        for pid, name, e, env, gate, ins in rows:
            fx = ", [%d, %d, 127, 0, 127]" % (ins[0][0], ins[0][1]) if ins[0][0] else ""
            js.append('P(%s, [%s], [%s], 0, 0%s)' % (json.dumps(name), ", ".join(map(str, e)),
                                                     ", ".join(map(str, env)), fx))
        text = ",\n        ".join(js)
        new, n = re.subn(r'presets: \[P\("BRASS", \[1, 0, 0, 76.*?\] \},',
                         "presets: [" + text + "] },", ed.read_text(), count=1, flags=re.S)
        if n != 1:
            raise SystemExit("gen_p6_presets: the editor's POLYSIX presets line not found")
        ed.write_text(new)


if __name__ == "__main__":
    main(sys.argv[1])
