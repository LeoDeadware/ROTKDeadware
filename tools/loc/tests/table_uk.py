#!/usr/bin/env python3
"""Unit test of the Ukrainian table mods/loc/uk/text.tsv against the disc, and of the built overlay.

1. Every row's id exists on the disc and its en_crc matches the English string (no stale rows).
2. Each translation keeps the English string's format codes (%1s %d %02d %% ...), its \\n count,
   its '|' and its '+' timing marks, and encodes to the game's 8-bit codes (fnts.encode).
3. Coverage: every English string that has letters is translated, except the native language
   names and the legal notice (kept in English on purpose).
4. The overlay built by build_mod.py (in a temp dir under out/loc, git-ignored) holds, in every bank,
   exactly fnts.encode(uk) for translated ids and the English bytes for the others; the
   resources outside the text banks and fonts are byte-identical to the disc.

  python3 tools/loc/tests/table_uk.py [ROTK_DATA]
"""
import os
import re
import shutil
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
LOC = os.path.dirname(HERE)
sys.path.insert(0, LOC)
import build_mod  # noqa: E402
import extract  # noqa: E402
import fnts  # noqa: E402
import scx  # noqa: E402
import textbank  # noqa: E402

DATA = sys.argv[1] if len(sys.argv) > 1 else extract.DEFAULT_DATA
ROOT = os.path.dirname(os.path.dirname(LOC))
TABLE = os.environ.get('LOC_TABLE') or os.path.join(ROOT, 'mods/loc/uk/text.tsv')
KEEP_EN = {'GlobScen:' + k for k in ('DEU ENG FRA ITA NLD POL POR SPA SWE legal').split()}
FMT = re.compile(r'%[0-9]*[a-z%]')
fails = []


def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond:
        fails.append(what)


ents = extract.entries(DATA)
en = {i: raw for i, raw, *_ in ents}
table = build_mod.load_table(TABLE)
unknown = [i for i in table if i not in en]
stale = [i for i in table if i in en and table[i][0] != extract.crc(en[i])]
check(not unknown, f'{len(table)} rows: all ids exist on the disc ({unknown[:3]})')
check(not stale, f'no stale rows ({len(stale)}: {stale[:3]})')

bad = []
for i, (_, uk) in table.items():
    e = en.get(i, b'').decode('latin1')
    why = []
    if sorted(FMT.findall(e)) != sorted(FMT.findall(uk)):
        why.append(f'format {FMT.findall(e)} -> {FMT.findall(uk)}')
    for mark in ('\n', '|'):
        if e.count(mark) != uk.count(mark):
            why.append(f'{mark!r} x{e.count(mark)} -> x{uk.count(mark)}')
    if Counter(len(m) for m in re.findall(r'\++', e)) != Counter(len(m) for m in re.findall(r'\++', uk)):
        why.append('+ marks differ')
    try:
        fnts.encode(uk)
    except ValueError as x:
        why.append(str(x))
    if why:
        bad.append((i, why))
for i, why in bad[:8]:
    print('  ', i, '; '.join(why))
check(not bad, f'translations keep format codes, newlines, | and + marks, and encode ({len(bad)} bad)')

# Canon names: Astrolabia LOTR (tr. O. Feshovets) as used on uk.wikipedia. Wrong form -> right form.
NON_CANON = {'Ґендальф': 'Ґандальф', 'Гендальф': 'Ґандальф', 'Смеагол': 'Смеаґол', 'Мінас-Тиріт': 'Мінас-Тіріт',
             'Ізенґард': 'Ісенґард', 'Ізенгард': 'Ісенґард', 'Пеленорськ': 'Пеленнорськ', 'Кіріт Унґол': 'Кіріт-Унґол',
             'Король-чаклун': 'Король-Чаклун', 'Гемджі': 'Ґемджі', 'Голум': 'Ґолум', 'Осгіліат': 'Осґіліат',
             # owner 2026-11-06 (680a23b, "переклад іноді написаний якось дивно")
             'Гондор': 'Ґондор', 'Арагорн': 'Араґорн', 'ҐЕНДАЛЬФ': 'ҐАНДАЛЬФ', 'Рохан': 'Роган', 'Прелесть': 'Скарб',
             'прелесть': 'скарбе'}
off = [(i, w) for i, (_, uk) in table.items() for w in NON_CANON if w in uk]
for i, w in off[:8]:
    print('  ', i, w, '->', NON_CANON[w])
check(not off, f'canon names (Astrolabia): {len(off)} non-canon forms')

wordy = [i for i, raw in en.items() if re.search(rb'[A-Za-z]{2}', raw) and i not in KEEP_EN]
missing = [i for i in wordy if i not in table]
check(not missing, f'coverage: {len(wordy) - len(missing)}/{len(wordy)} English strings with words '
      f'translated ({len(table)}/{len(en)} rows in all; missing {missing[:5]})')

out = os.path.join(ROOT, 'out/loc', f'table_uk_test-{os.getpid()}')
try:
    s = build_mod.build(DATA, out, table)
    check(s['stale'] == 0 and s['done'] == len(table), f"build: {s['done']} translated, {s['stale']} stale")
    disc = os.path.join(DATA, 'game/extracted')
    wrong = 0
    other = 0
    for rel, idx in extract.bank_files(DATA):
        p = os.path.join(out, rel)
        d = open(p if os.path.exists(p) else os.path.join(disc, rel), 'rb').read()
        o = open(os.path.join(disc, rel), 'rb').read()
        stem = os.path.splitext(os.path.basename(rel))[0]
        for b in textbank.parse(scx.decode(d, scx.find(d, 'subT', idx))):
            if b.tag != 'ENG':
                continue
            for key, val in b.entries:
                i = f'{stem}:{textbank.key_str(key)}'
                want = fnts.encode(table[i][1]) if i in table else en[i]
                wrong += val != want
        if os.path.exists(p):
            for r in scx.resources(o):
                if r.type in ('subT', 'font') and (r.type == 'font' or r.index == idx):
                    continue
                if r.type == 'subF':  # cinematic subtitles: checked below
                    continue
                other += scx.decode(d, scx.find(d, r.type, r.index)) != scx.decode(o, r)
    for rel, idx in extract.bank_files(DATA, 'subF'):  # cinematic subtitles (other languages may be emptied)
        p = os.path.join(out, rel)
        d = open(p if os.path.exists(p) else os.path.join(disc, rel), 'rb').read()
        for b in textbank.parse(scx.decode(d, scx.find(d, 'subF', idx))):
            if b.tag == 'ENG':
                for key, val in b.entries:
                    i = extract.film_id(rel, idx, key)
                    wrong += val != (fnts.encode(table[i][1]) if i in table else en[i])
    check(wrong == 0, f'built banks hold exactly the table ({wrong} values differ)')
    check(other == 0, f'built files: every other resource unchanged ({other} differ)')
finally:
    shutil.rmtree(out, ignore_errors=True)

print('table_uk:', 'FAIL' if fails else 'PASS')
sys.exit(1 if fails else 0)
