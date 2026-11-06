#!/usr/bin/env python3
"""Regenerates dsound_entries.inc from a published lift's symbols/functions.tsv.

    src/hle/dsound/gen_entries.py ROTKPort/.cache/lift/<lift_id>/symbols/functions.tsv [default.xbe]

The DSOUND section bounds come from the XBE's section table (default: game/extracted).
"""
import os
import struct
import sys


def dsound_section(xbe_path):
    d = open(xbe_path, "rb").read()
    base, = struct.unpack_from("<I", d, 0x104)
    nsec, sec_va = struct.unpack_from("<II", d, 0x11C)
    for i in range(nsec):
        o = sec_va - base + i * 0x38
        va, vsize, _, _, name_va = struct.unpack_from("<IIIII", d, o + 4)
        name = d[name_va - base:d.index(b"\0", name_va - base)].decode()
        if name == "DSOUND":
            return va, va + vsize
    raise SystemExit("no DSOUND section")


ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
xbe = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "game", "extracted", "default.xbe")
LO, HI = dsound_section(xbe)
tsv = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
lift = os.path.basename(os.path.dirname(os.path.dirname(os.path.abspath(tsv))))
out = [f'// Every lifted function entry in the DSOUND section (0x{LO:06X}..0x{HI:06X}),',
       f'// from lift/symbols/functions.tsv (lift {lift}; the section',
       '// bytes never change between lifts). dsound.cpp guards each entry it does',
       '// not replace. Regenerate: src/hle/dsound/gen_entries.py', '']
for line in open(tsv).read().splitlines()[1:]:
    a = int(line.split('\t')[0], 16)
    if LO <= a < HI:
        out.append(f'DS_ENTRY(0x{a:06X})')
open(os.path.join(here, 'dsound_entries.inc'), 'w').write('\n'.join(out) + '\n')
