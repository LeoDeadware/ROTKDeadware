#!/usr/bin/env python3
"""Every string of the game's text banks -> a TSV for translation (game text: never in git).

  python3 tools/loc/extract.py [--data ROTK_DATA] [--out out/loc/en.tsv]

Banks: GlobScen.scx subT #5 (UI, string keys) and the subT of every level .scx (subtitles,
numeric ids); the ENG block of each. Columns: id ('<file stem>:<key>'), en_crc (crc32 of the
English bytes, hex), en (Latin-1, newline as \\n). mods/loc/uk/text.tsv uses the same id and
en_crc, so a changed English string shows up as stale.
"""
import argparse
import glob
import os
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import scx  # noqa: E402
import textbank  # noqa: E402

DEFAULT_DATA = os.environ.get('ROTK_DATA', os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..')))


def escape(s):
    return s.replace('\\', '\\\\').replace('\n', '\\n').replace('\r', '\\r').replace('\t', '\\t')


def unescape(s):
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            out.append({'n': '\n', 'r': '\r', 't': '\t', '\\': '\\'}.get(s[i + 1], '\\' + s[i + 1]))
            i += 2
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def bank_files(data, rtype='subT'):
    """[(relative path, index)] of every `rtype` bank the game shows, GlobScen first.
    subT: UI text and in-level subtitles; subF: the subtitles of the level's cinematics
    (intro / outro narration, LZ-packed `Rdat`)."""
    disc = os.path.join(data, 'game/extracted')
    out = []
    for f in sorted(glob.glob(os.path.join(disc, 'Data/Game/**/*.scx'), recursive=True)):
        rel = os.path.relpath(f, disc)
        d = open(f, 'rb').read()
        for r in scx.resources(d):
            if r.type != rtype:
                continue
            raw = scx.decode(d, r)
            try:
                blocks = textbank.parse(raw)
            except ValueError:
                continue
            if any(b.tag == 'ENG' and b.entries for b in blocks):
                out.append((rel, r.index))
    out.sort(key=lambda t: (not t[0].endswith('GlobScen.scx'), t))
    return out


def entries(data):
    """[(id, en bytes, rel path, subT index, key)] in bank order."""
    disc = os.path.join(data, 'game/extracted')
    out = []
    for rel, idx in bank_files(data):
        d = open(os.path.join(disc, rel), 'rb').read()
        stem = os.path.splitext(os.path.basename(rel))[0]
        for b in textbank.parse(scx.decode(d, scx.find(d, 'subT', idx))):
            if b.tag != 'ENG':
                continue
            for key, val in b.entries:
                out.append((f'{stem}:{textbank.key_str(key)}', val, rel, idx, key))
    for rel, idx in bank_files(data, 'subF'):
        d = open(os.path.join(disc, rel), 'rb').read()
        for b in textbank.parse(scx.decode(d, scx.find(d, 'subF', idx))):
            if b.tag != 'ENG':
                continue
            for key, val in b.entries:
                out.append((film_id(rel, idx, key), val, rel, idx, key))
    return out


def film_id(rel, idx, key):
    """Table id of a cinematic subtitle: <level>:F<subF index>:<key>."""
    return f'{os.path.splitext(os.path.basename(rel))[0]}:F{idx:x}:{textbank.key_str(key)}'


def crc(raw):
    return f'{zlib.crc32(raw):08x}'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default=DEFAULT_DATA)
    ap.add_argument('--out', default=os.path.join(os.path.dirname(os.path.dirname(HERE)), 'out/loc/en.tsv'))  # git-ignored
    a = ap.parse_args()
    ents = entries(a.data)
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, 'w', encoding='utf-8') as f:
        f.write('# id\ten_crc\ten   (game text: do not commit)\n')
        for i, raw, *_ in ents:
            f.write(f'{i}\t{crc(raw)}\t{escape(raw.decode("latin1"))}\n')
    uniq = len({raw for _, raw, *_ in ents})
    print(f'{len(ents)} strings ({uniq} unique, {sum(len(r) for _, r, *_ in ents)} bytes) -> {a.out}')


if __name__ == '__main__':
    main()
