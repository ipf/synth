#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""Load WAV files into a Felucca user sample slot (USR1..USR3) over USB-MIDI.

  fm1_sample_upload.py info
  fm1_sample_upload.py load SLOT NAME file.wav[:ROOT[:LO-HI]] ...   (SLOT 1..3)
  fm1_sample_upload.py span NAME file.wav[:ROOT[:LO-HI]] ...        (one cut per key, over USR1..3)
  fm1_sample_upload.py erase SLOT
  fm1_sample_upload.py build NAME OUT_PREFIX file.wav[...] ...      (no device: writes OUT_PREFIX.hdr / .bin)

Each file becomes one zone: mono, 22050 Hz, IMA ADPCM (sampleio.py, the same encoder
as the built-in sets and the web editor). ROOT is a MIDI note (default: from the file
name, C4 = 60, else 60); without LO-HI the zones split the keyboard between their roots
(a span set: its own key only). A slot holds 80 KiB (about 7 s at 22050 Hz). --rate HZ
resamples to another rate first (e.g. 11025, twice the cuts in the same space).
A span set is one keyboard over USR1..3 (one cut per key): on the device it plays like a
drum kit, key k of the keyboard = cut k + 1 (the OCT buttons don't move it). The zones
loop: the sound's LOOP knob off = one-shot per press, on = the cut repeats while held.
Protocol: web/EDITOR_PROTOCOL.md, cmds 11..15.
Needs mido (and a backend such as python-rtmidi) for the device commands.
"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sampleio as sio  # noqa: E402

HDR = [0x7D, 0x46, 0x4C]
RC = {1: "size", 2: "header", 3: "data CRC", 4: "flash", 5: "zones"}


def printable(b):
    """bytes from the device as text for the terminal: printable ASCII only (no escape sequences)"""
    return "".join(chr(c) if 32 <= c < 127 else "?" for c in b)


def slot_info(r):
    """the SMP_INFO reply -> one line per slot; ValueError when it is malformed"""
    try:
        n, i, out = r[0], 2, []
        for k in range(n):
            nz = r[i]
            j = r.index(0, i + 1)
            out.append(f"USR{k + 1}: {'empty' if not nz else f'{nz} zones, {printable(r[i + 1:j])}, {r[j + 1]} KiB'}")
            i = j + 2
        return out
    except (IndexError, ValueError):
        raise ValueError("malformed SMP_INFO reply from the device") from None


def pack7(b):
    """groups of up to 7 bytes, each preceded by their top bits"""
    out = []
    for i in range(0, len(b), 7):
        g = b[i:i + 7]
        out.append(sum(((x >> 7) & 1) << j for j, x in enumerate(g)))
        out += [x & 0x7F for x in g]
    return out


class Link:
    def __init__(self):
        try:
            import mido
        except ImportError:
            sys.exit("needs mido: pip install mido python-rtmidi")
        self.mido = mido
        outs = [n for n in mido.get_output_names() if "Felucca" in n]
        ins = [n for n in mido.get_input_names() if "Felucca" in n]
        if not outs or not ins:
            sys.exit("no MIDI port named Felucca: connect the FM-1 (running Felucca) by USB")
        self.o = mido.open_output(outs[0])
        self.i = mido.open_input(ins[0])
        for _ in range(3):                          # the first frame after opening the port can be lost
            try:
                self.req(1, [], 1.0)
                break
            except TimeoutError:
                pass

    def req(self, cmd, args, timeout=3.0):
        for _ in self.i.iter_pending():
            pass
        self.o.send(self.mido.Message("sysex", data=HDR + [cmd] + list(args)))
        t0 = time.time()
        while time.time() - t0 < timeout:
            for m in self.i.iter_pending():
                if m.type == "sysex" and list(m.data[:4]) == HDR + [cmd]:
                    return list(m.data[4:])
            time.sleep(0.002)
        raise TimeoutError(f"no reply to cmd {cmd}")


def read_zones(specs, rate):
    """specs: "file.wav[:ROOT[:LO-HI]]" -> [(int16 samples at rate, root, lo, hi)]"""
    zones = []
    for spec in specs:
        parts = spec.split(":")
        path = Path(parts[0])
        root = int(parts[1]) if len(parts) > 1 and parts[1] else sio.note_from_name(path.stem)
        lo, hi = (int(v) for v in parts[2].split("-")) if len(parts) > 2 else (None, None)
        sr, x = sio.read_any_wav(path)
        x = sio.resample(x, sr, rate)
        if not x:
            raise ValueError(f"{path}: no audio")
        zones.append((sio.to_int16(x), root, lo, hi))
    return zones


def build(name, specs, rate=sio.SLOT_RATE):
    """specs: "file.wav[:ROOT[:LO-HI]]" -> (header, ADPCM data) for SMP_END / SMP_WRITE"""
    return sio.user_slot(name, read_zones(specs, rate), rate)


def build_span(name, specs, rate):
    """-> [(slot, header, ADPCM data)]: one zone per key over USR1..3, USR1's rsv marks them as one keyboard.
    The zones loop (LOOP knob on: the cut repeats while held, off: one-shot)"""
    zs = [(s, r, r, r) if lo is None else (s, r, lo, hi) for s, r, lo, hi in read_zones(specs, rate)]
    return [(k, *sio.user_slot(name, part, rate, 1 if k == 0 else 0, 1))
            for k, part in enumerate(sio.span_slots(zs))]


def write_slot(link, slot, hdr, data):
    """SMP_BEGIN, the data in 256-byte writes, SMP_END -> True when the device took it"""
    print(f"USR{slot + 1}: {len(data)} B ADPCM, {hdr[6]} zones")
    if link.req(11, [slot])[1]:
        sys.exit("begin failed")
    t0 = time.time()
    for off in range(0, len(data), 256):
        a = sio.SLOT_DATA_OFF + off
        r = link.req(12, [slot, a & 0x7F, (a >> 7) & 0x7F, (a >> 14) & 0x7F] + pack7(data[off:off + 256]))
        if r[4]:
            sys.exit(f"write at {a:#x} failed ({r[4]})")
        print(f"\r  {min(off + 256, len(data))} / {len(data)}", end="", flush=True)
    r = link.req(13, [slot] + pack7(hdr), 5)
    print(f"\n  {time.time() - t0:.1f} s, end:", "ok" if r[1] == 0 else f"FAILED ({r[1]}, {RC.get(r[1], '?')})")
    return r[1] == 0


def main():
    a = sys.argv[1:]
    rate = sio.SLOT_RATE
    if "--rate" in a:
        i = a.index("--rate")
        rate = int(a[i + 1])
        del a[i:i + 2]
    if not a or a[0] not in ("info", "load", "span", "erase", "build"):
        sys.exit(__doc__)
    cmd, slots = a[0], []
    if cmd in ("load", "span", "build"):
        if len(a) < (3 if cmd == "span" else 4):
            sys.exit(__doc__)
        try:
            if cmd == "span":
                slots = build_span(a[1], a[2:], rate)
            else:
                slots = [(-1, *build(a[2] if cmd == "load" else a[1], a[3:], rate))]
        except (OSError, ValueError) as e:
            sys.exit(f"cannot build the slot: {e}")
        if cmd == "build":
            Path(a[2] + ".hdr").write_bytes(slots[0][1])
            Path(a[2] + ".bin").write_bytes(slots[0][2])
            print(f"{a[1]}: {len(slots[0][2])} B ADPCM, {slots[0][1][6]} zones")
            return
    if cmd not in ("info", "span"):
        if cmd != "erase" and a[1] not in ("1", "2", "3"):
            sys.exit("SLOT is 1, 2 or 3")
        slots = [(int(a[1]) - 1,) + slots[0][1:]]
    link = Link()
    if cmd == "info":
        try:
            print("\n".join(slot_info(link.req(15, []))))
        except ValueError as e:
            sys.exit(str(e))
        return
    if cmd == "erase":
        print("erase:", "ok" if link.req(14, [int(a[1]) - 1], 10)[1] == 0 else "FAILED")
        return
    sys.exit(0 if all(write_slot(link, slot, hdr, data) for slot, hdr, data in slots) else 1)


if __name__ == "__main__":
    try:
        main()
    except TimeoutError as e:
        sys.exit(f"{e}: is Felucca running, and no other app using its MIDI port?")
