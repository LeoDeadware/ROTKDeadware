#!/usr/bin/env python3
"""Unit test of tools/loc/scx.py on the real disc files (no game run).

1. Container round trip, every .scx of the disc: the chunk walk covers every byte and the
   chunks concatenated give the file back byte for byte.
2. LZ: every Rdat resource of GlobScen.scx decodes to exactly its declared
   size (LoadOnce.scx has none), and lz_encode -> lz_decode gives it back byte for byte.
3. In-place replacement: GlobScen subT re-encoded unchanged and grown by 10 % decodes back
   to the new payload; no page outside the resource's pages changes; the file size is equal.
4. Every level's subT decodes (text bank v2, ENG).

  python3 tools/loc/tests/scx_roundtrip.py [ROTK_DATA]
"""
import glob
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import scx  # noqa: E402

DATA = sys.argv[1] if len(sys.argv) > 1 else os.environ.get('ROTK_DATA', os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..')))
DISC = os.path.join(DATA, 'game/extracted')
fails = []


def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond:
        fails.append(what)


files = sorted(glob.glob(os.path.join(DISC, 'Data/**/*.scx'), recursive=True))
check(len(files) == 44, f'{len(files)} .scx files on the disc (44 expected)')
for f in files:
    d = open(f, 'rb').read()
    cs = scx.chunks(d)
    rebuilt = b''.join(d[c.off:c.end] for c in cs)
    if rebuilt != d:
        check(False, f'container round trip {os.path.relpath(f, DISC)}')
check(not fails, f'container round trip of {len(files)} files byte for byte')

for name in ('Data/Game/GlobScen.scx',):  # LoadOnce.scx has no Rdat
    d = open(os.path.join(DISC, name), 'rb').read()
    n = 0
    for r in scx.resources(d):
        if not any(p.sub == 'Rdat' for p in r.parts):
            continue
        raw = scx.decode(d, r)
        enc = scx.lz_encode(raw)
        back, used = scx.lz_decode(enc, len(raw))
        if len(raw) != r.size or back != raw or used != len(enc):
            check(False, f'LZ round trip {name} {r}')
        n += 1
    check(n > 0, f'LZ round trip of {n} Rdat resources in {name}')

g = open(os.path.join(DISC, 'Data/Game/GlobScen.scx'), 'rb').read()
sub = scx.find(g, 'subT', 5)
raw = scx.decode(g, sub)
check(raw[12:15] == b'ENG', 'GlobScen subT #5 is the ENG text bank')
for label, payload in (('unchanged', raw), ('grown 10%', raw + b'\x00' * (len(raw) // 10))):
    try:
        g2 = scx.replace(g, 'subT', 5, payload)
    except ValueError as e:
        check(False, f'replace subT {label}: {e}')
        continue
    r2 = scx.find(g2, 'subT', 5)
    check(len(g2) == len(g) and scx.decode(g2, r2) == payload, f'replace subT {label}: decodes back, same file size')
    own_pages = {c.off // scx.PAGE for c in [sub.hdr] + sub.parts}
    moved = [p for p in range(len(g) // scx.PAGE) if p not in own_pages and
             g[p * scx.PAGE:(p + 1) * scx.PAGE] != g2[p * scx.PAGE:(p + 1) * scx.PAGE]]
    check(not moved, f'replace subT {label}: only its own pages changed ({len(own_pages)})')
    others = [r for r in scx.resources(g) if r.key != ('subT', 5)]
    same = all(scx.decode(g2, scx.find(g2, r.type, r.index)) == scx.decode(g, r) for r in others)
    check(same, f'replace subT {label}: the other {len(others)} resources decode unchanged')

levels = [f for f in files if '/Data/Game/' in f and not f.endswith('GlobScen.scx')]
ok = 0
for f in levels:
    d = open(f, 'rb').read()
    for r in scx.resources(d):
        if r.type == 'subT':
            t = scx.decode(d, r)
            ok += t[12:15] == b'ENG' and struct.unpack_from('<I', t, 0)[0] >= 1
check(ok >= len(levels), f'subT of {len(levels)} level files decodes ({ok} banks)')

print('scx_roundtrip:', 'FAIL' if fails else 'PASS')
sys.exit(1 if fails else 0)
