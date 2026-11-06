#!/usr/bin/env python3
"""Builds the Ukrainian overlay (mods/) from the user's disc + our translation table.

  python3 tools/loc/build_mod.py uk --out DIR [--data ROTK_DATA] [--table mods/loc/uk/text.tsv]
                                    [--alphabet] [--fonts-only]

DIR gets the changed disc files at their disc paths (DIR/Data/Game/GlobScen.scx,
DIR/Data/Game/hel/hel01.scx ...); the game reads them through ROTK_MODS=DIR (src/hle/kernel
file VFS). The files are derived from game data: they are built on the user's machine (install
step) and never committed or published.

- GlobScen.scx: subT #5 (UI text, ENG block) gets the Ukrainian values; fonts 0, 1, 2 get the
  Cyrillic glyphs (fnts.py). Written in place: the resource keeps its pages (scx.replace).
- Level .scx: the subT (subtitles) gets the Ukrainian values, in place. The file is an APFS clone
  of the disc file (cp -c) with only the changed pages written, so it costs a few KiB of disk.
- A row whose en_crc does not match the disc's English string is skipped (stale) and counted.
- --alphabet: every UI string becomes the Ukrainian alphabet (font check without a table).
"""
import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import extract  # noqa: E402
import fnts  # noqa: E402
import scx  # noqa: E402
import textbank  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
unescape = extract.unescape
ALPHABET = 'АБВГҐДЕЄЖЗИІЇЙКЛМНОПРСТУФХЦЧШЩЬЮЯ абвгґдеєжзиіїйклмнопрстуфхцчшщьюя'


def build_fonts(glob_bytes):
    """{font index: new FNTS blob} with the Cyrillic glyphs (fnts.NEW_CODES)."""
    out = {}
    for i in (0, 1, 2):
        f = fnts.Font(scx.decode(glob_bytes, scx.find(glob_bytes, 'font', i)))
        out[i] = f.build(fnts.cyrillic(f, fnts.FACE[i]))
    return out


def load_table(path):
    """{id: (en_crc, uk text)} of the rows that have a translation."""
    t = {}
    for line in open(path, encoding='utf-8'):
        if line.startswith('#') or not line.strip():
            continue
        cols = line.rstrip('\n').split('\t')
        if len(cols) >= 3 and cols[2]:
            t[cols[0]] = (cols[1], unescape(cols[2]))
    return t


def translate_bank(raw, stem, table, stats, alphabet=False, ident=None):
    blocks = textbank.parse(raw)
    for b in blocks:
        if b.tag != 'ENG':
            continue
        ents = []
        for key, val in b.entries:
            i = ident(key) if ident else f'{stem}:{textbank.key_str(key)}'
            if alphabet:
                val = fnts.encode(ALPHABET)
                stats['done'] += 1
            elif i in table:
                crc, uk = table[i]
                if crc != extract.crc(val):
                    stats['stale'] += 1
                else:
                    val = fnts.encode(uk)
                    stats['done'] += 1
            ents.append((key, val))
            stats['total'] += 1
        b.entries = ents
    return textbank.build(blocks)


def write_changed_pages(src, dst, old, new):
    """dst = APFS clone of src, then only the 64 KiB pages where old != new are written."""
    if os.path.exists(dst):
        os.remove(dst)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if subprocess.run(['cp', '-c', src, dst], capture_output=True).returncode != 0:
        shutil.copyfile(src, dst)
    assert len(old) == len(new)
    n = 0
    with open(dst, 'r+b') as f:
        for p in range(0, len(new), scx.PAGE):
            if old[p:p + scx.PAGE] != new[p:p + scx.PAGE]:
                f.seek(p)
                f.write(new[p:p + scx.PAGE])
                n += 1
    return n


def build(data, out, table, alphabet=False, fonts_only=False):
    disc = os.path.join(data, 'game/extracted')
    stats = {'total': 0, 'done': 0, 'stale': 0, 'files': 0, 'pages': 0}
    files = {}
    for rel, idx in extract.bank_files(data):
        files.setdefault(rel, []).append(idx)
    films = {}
    for rel, idx in extract.bank_files(data, 'subF'):
        films.setdefault(rel, []).append(idx)
        files.setdefault(rel, [])
    for rel, idxs in files.items():
        src = os.path.join(disc, rel)
        old = open(src, 'rb').read()
        new = old
        stem = os.path.splitext(os.path.basename(rel))[0]
        is_glob = rel.endswith('GlobScen.scx')
        if not fonts_only:
            for idx in idxs:
                raw = scx.decode(new, scx.find(new, 'subT', idx))
                nb = translate_bank(raw, stem, table, stats, alphabet and is_glob)
                if nb != raw:
                    new = scx.replace(new, 'subT', idx, nb)
            for idx in films.get(rel, []):  # cinematic subtitles (LZ-packed)
                raw = scx.decode(new, scx.find(new, 'subF', idx))
                nb = translate_bank(raw, stem, table, stats,
                                    ident=lambda key, r=rel, i=idx: extract.film_id(r, i, key))
                if nb != raw:
                    packed = any(q.sub == 'Rdat' for q in scx.find(new, 'subF', idx).parts)
                    try:
                        new = scx.replace(new, 'subF', idx, nb, lz=packed)
                    except ValueError:
                        # Cyrillic packs worse than English: the slot is tight. The port runs the
                        # English game, so the other languages' lines of this bank are emptied.
                        blocks = textbank.parse(nb)
                        for b in blocks:
                            if b.tag != 'ENG':
                                b.entries = [(k, b'') for k, _ in b.entries]
                        new = scx.replace(new, 'subF', idx, textbank.build(blocks), lz=packed)
        if is_glob:
            for i, blob in build_fonts(old).items():
                new = scx.replace(new, 'font', i, blob)
        if new != old:
            stats['pages'] += write_changed_pages(src, os.path.join(out, rel), old, new)
            stats['files'] += 1
    return stats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('lang', choices=['uk'])
    ap.add_argument('--out', required=True)
    ap.add_argument('--data', default=extract.DEFAULT_DATA)
    ap.add_argument('--table', default=os.path.join(ROOT, 'mods/loc/uk/text.tsv'))
    ap.add_argument('--alphabet', action='store_true')
    ap.add_argument('--fonts-only', action='store_true')
    a = ap.parse_args()
    table = {} if a.alphabet or a.fonts_only else load_table(a.table)
    s = build(a.data, a.out, table, a.alphabet, a.fonts_only)
    print(f"uk: {s['done']}/{s['total']} strings translated, {s['stale']} stale, "
          f"{s['files']} files, {s['pages']} pages written -> {a.out}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
