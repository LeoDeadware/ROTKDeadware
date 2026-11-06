#!/usr/bin/env python3
"""Unit test of the Ukrainian fonts (tools/loc/build_mod.py build_fonts; no game run).

1. Each of the 3 GlobScen fonts gets a glyph for every Cyrillic code (fnts.NEW_CODES); the new
   blob parses back, keeps the atlas width, and its height stays <= 512 (the texture is rounded
   up to a power of two; 512 is the largest the originals use).
2. Every original glyph that is not replaced keeps its pixels and metrics exactly.
3. The new glyphs sit on the font's baseline: the median bottom of the capitals equals the Latin
   one (+-1 px), and the cap height of Б Г Д Л П Ш matches the Latin capitals (+-2 px).
4. GlobScen with the 3 new fonts written in place (scx.replace) decodes them back, same file size.
5. Every string of mods/loc/uk/text.tsv encodes to codes all 3 new fonts have (or 0x0a; '|' is the
   subtitle line break, 0xa3/0xa5 button icons of font 0, "~" the credits padding, as in English).
A preview of the Cyrillic of each font goes to <run dir>/fonts_ua.png when RUN is set.

  python3 tools/loc/tests/fonts_ua.py [ROTK_DATA]
"""
import os
import statistics
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
LOC = os.path.dirname(HERE)
sys.path.insert(0, LOC)
import fnts  # noqa: E402
import scx  # noqa: E402

DATA = sys.argv[1] if len(sys.argv) > 1 else os.environ.get('ROTK_DATA', os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..')))
GLOB = os.path.join(DATA, 'game/extracted/Data/Game/GlobScen.scx')
fails = []


def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond:
        fails.append(what)


try:
    import build_mod  # noqa: E402
except ImportError as e:
    check(False, f'tools/loc/build_mod.py: {e}')
    print('fonts_ua: FAIL')
    sys.exit(1)

g = open(GLOB, 'rb').read()
new = build_mod.build_fonts(g)
check(sorted(new) == [0, 1, 2], 'build_fonts gives fonts 0, 1, 2')
fonts = {}
for i, blob in sorted(new.items()):
    old = fnts.Font(scx.decode(g, scx.find(g, 'font', i)))
    f = fnts.Font(blob)
    fonts[i] = f
    missing = [c for c in fnts.NEW_CODES if c not in f.recs]
    check(not missing, f'font {i}: all {len(fnts.NEW_CODES)} Cyrillic codes ({len(missing)} missing)')
    check(f.W == old.W and f.H <= 512, f'font {i}: atlas {f.W}x{f.H} (was {old.W}x{old.H})')
    kept = [c for c in old.recs if c not in fnts.NEW_CODES]
    bad = [c for c in kept if c not in f.recs or f.recs[c][4:] != old.recs[c][4:] or
           f.glyph(c) != old.glyph(c)]
    check(not bad, f'font {i}: {len(kept)} original glyphs unchanged ({len(bad)} differ)')
    lat = [old.recs[c] for c in range(0x41, 0x5B) if c in old.recs and chr(c) not in 'JQ']
    cyr = [f.recs[fnts.CODEPAGE[ch]] for ch in 'БГДЛПШ']
    lb = statistics.median(r[6] + r[1] for r in lat)
    cb = statistics.median(r[6] + r[1] - (1 if ch == 'Д' else 0) for r, ch in zip(cyr, 'БГДЛПШ'))
    lc = statistics.median(r[1] for r in lat)
    cc = statistics.median(r[1] for r in cyr[:2] + cyr[3:])
    check(abs(cb - lb) <= 1, f'font {i}: Cyrillic baseline {cb} vs Latin {lb}')
    check(abs(cc - lc) <= 2, f'font {i}: Cyrillic cap height {cc} vs Latin {lc}')
    # Owner 2026-11-06 (680a23b): "ГАндАльф", "ДОСвіду" - the uncial fonts' Latin a/e/o/c are
    # capitals in shape; lowercase Cyrillic must use the face's own lowercase at x-height.
    lat_lo = [ch for ch in 'абвгдежзийклмнопрстуфхцчшщьюяієїґ' if fnts.encode(ch)[0] < 0x80]
    check(not lat_lo, f'font {i}: lowercase Cyrillic on Cyrillic glyphs (on Latin: {"".join(lat_lo)})')
    xh = statistics.median(f.recs[fnts.encode(ch)[0]][1] for ch in 'вкмнпт')
    tall = [ch for ch in 'аеосх' if abs(f.recs[fnts.encode(ch)[0]][1] - xh) > 2]
    check(not tall, f'font {i}: а е о с х at the x-height {xh} (off: {"".join(tall)})')

g2 = g
for i, blob in sorted(new.items()):
    try:
        g2 = scx.replace(g2, 'font', i, blob)
    except ValueError as e:
        check(False, f'replace font {i}: {e}')
check(len(g2) == len(g) and all(scx.decode(g2, scx.find(g2, 'font', i)) == new[i] for i in new),
      'GlobScen with the new fonts: decodes back, same file size')

table = os.path.join(os.path.dirname(os.path.dirname(LOC)), 'mods/loc/uk/text.tsv')
have = set.intersection(*(set(f.recs) for f in fonts.values())) | {0x0a}
n = bad = 0
for line in open(table, encoding='utf-8'):
    if line.startswith('#') or not line.strip():
        continue
    cols = line.rstrip('\n').split('\t')
    if len(cols) < 3 or not cols[2]:
        continue
    n += 1
    raw = fnts.encode(build_mod.unescape(cols[2]))
    miss = sorted({b for b in raw if b not in have and b not in (0xa3, 0xa5, 0x7c, 0x7e)})
    if miss:
        bad += 1
        if bad <= 5:
            print('  ', cols[0], ' '.join(f'{b:#x}' for b in miss))
check(n > 0 and not bad, f'{n} Ukrainian strings use only codes the fonts have ({bad} do not)')

run = os.environ.get('RUN')
if run and fonts:
    rows = []
    for i, f in sorted(fonts.items()):
        line_h = max(f.recs[c][1] + max(f.recs[c][6], 0) for c in fnts.NEW_CODES) + 2
        width = sum(f.recs[c][4] for c in fnts.NEW_CODES) + 8
        canvas = [bytearray(width) for _ in range(line_h)]
        x = 4
        for c in fnts.NEW_CODES:
            w, h, _, _, adv, xo, yo, _ = f.recs[c]
            for r, row in enumerate(f.glyph(c)):
                for k, v in enumerate(row):
                    yy, xx = yo + r, x + xo + k
                    if 0 <= yy < line_h and 0 <= xx < width:
                        canvas[yy][xx] = max(canvas[yy][xx], v * 17)
            x += adv
        rows.append(canvas)
    W = max(len(r[0]) for r in rows)
    px = []
    for canvas in rows:
        for r in canvas:
            px.append(bytes(r) + bytes(W - len(r)))
        px.append(bytes(W))
    raw = b''.join(b'\0' + r for r in px)

    def ch(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
    open(os.path.join(run, 'fonts_ua.png'), 'wb').write(
        b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', W, len(px), 8, 0, 0, 0, 0)) +
        ch(b'IDAT', zlib.compress(raw)) + ch(b'IEND', b''))

print('fonts_ua:', 'FAIL' if fails else 'PASS')
sys.exit(1 if fails else 0)
