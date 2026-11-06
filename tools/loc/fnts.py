#!/usr/bin/env python3
"""ROTK fonts (`font` resources of GlobScen.scx, blob 'FNTS') and the Cyrillic glyphs for them.

FNTS (decoded from the bytes and LLFont.c's loader at 0xB5E00, docs/loc/FORMAT.md):
  +0  'FNTS', u32 size (= records end + 16 + bitmap bytes)
  +8  u16 version (>= 200: 12-byte records), u16 glyph count, u32 flags, +0x13 s8 line height
  +0x14 u32 records offset (0x20), +0x18 u32 kerning offset, +0x1c u32 bitmap offset (16-aligned)
  record: u16 code, u8 w, u8 h, u16 x, u16 y (atlas), u8 advance, s8 x bearing, s8 y offset, u8 0
  bitmap: 16-byte header (+4 u16 width, +6 u16 height), then 4 bpp, low nibble = left pixel
  kerning: u32 n, n x {u16 left, u16 right, s8 adjust, 3 bytes 0}
The loader keeps a 256-entry table indexed by the record's code (font + 0xC + code*4): codes
must be < 0x100, so Cyrillic goes onto 8-bit codes (CODEPAGE below), never U+04xx.
"""
import hashlib
import os
import struct
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

# Ukrainian -> the game's 8-bit codes. Capitals that look the same as Latin ones reuse the
# font's own Latin glyph (exact style); the rest take their Windows-1251 code, except Ґ
# (0xA5 is a button icon in font 0) -> 0x81. Lowercase letters always get their own glyph
# from the Cyrillic face: the uncial fonts 1 and 2 draw Latin a/e/o/c like capitals (owner
# 2026-11-06, 680a23b: "ГАндАльф", "ДОСвіду"). Punctuation the fonts lack is simplified.
HOMOGLYPH = dict(zip('АВЕКМНОРСТХІ', 'ABEKMHOPCTXI'))
CODEPAGE = {}
for _ch in 'БГДЖЗИЙЛПУФЦЧШЩЬЮЯабвгдежзийклмнопрстуфхцчшщьюяі':
    CODEPAGE[_ch] = _ch.encode('cp1251')[0]
CODEPAGE.update({'Ґ': 0x81, 'ґ': 0xB4, 'Є': 0xAA, 'є': 0xBA, 'Ї': 0xAF, 'ї': 0xBF})
PUNCT = {'’': "'", 'ʼ': "'", '‘': "'", '“': '"', '”': '"', '„': '"', '—': '-', '–': '-',
         '…': '...', '№': 'No.', ' ': ' '}
NEW_CODES = sorted(set(CODEPAGE.values()))
UNICODE_OF = {v: k for k, v in CODEPAGE.items()}


def encode(text):
    """Ukrainian (Unicode) -> the bytes the game's text banks and fonts use."""
    out = bytearray()
    for ch in text:
        ch = PUNCT.get(ch, ch)
        for c in ch:
            if c in HOMOGLYPH:
                out.append(ord(HOMOGLYPH[c]))
            elif c in CODEPAGE:
                out.append(CODEPAGE[c])
            elif ord(c) < 0x80:
                out.append(ord(c))
            elif c in '©®«»°£¥§':
                out += c.encode('latin1')
            else:
                raise ValueError(f'no game code for {c!r} (U+{ord(c):04X}) in {text!r}')
    return bytes(out)


def decode(raw):
    """Game bytes -> Unicode as the UA font shows them (for tests and previews)."""
    return ''.join(UNICODE_OF.get(b, chr(b)) for b in raw)


class Font:
    def __init__(self, blob):
        self.blob = blob
        assert blob[0:4] == b'FNTS'
        self.version, n, self.flags = struct.unpack_from('<HHI', blob, 8)
        assert self.version >= 200, 'only 12-byte records'
        self.line = blob[0x13]
        rec, kern, bm = struct.unpack_from('<III', blob, 0x14)
        self.head = bytearray(blob[0:0x20])
        self.recs = {}
        for i in range(n):
            r = list(struct.unpack_from('<HBBHHBbbB', blob, rec + 12 * i))
            self.recs[r[0]] = r[1:]          # w, h, x, y, adv, xoff, yoff, pad
        self.bm_head = bytearray(blob[bm:bm + 16])
        self.W, self.H = struct.unpack_from('<HH', blob, bm + 4)
        px = bytearray()
        for b in blob[bm + 16:bm + 16 + self.W * self.H // 2]:
            px.append(b & 15)
            px.append(b >> 4)
        self.px = px
        nk = struct.unpack_from('<I', blob, kern)[0]
        self.kern = [struct.unpack_from('<HHb', blob, kern + 4 + 8 * i) for i in range(nk)]

    def glyph(self, code):
        w, h, x, y = self.recs[code][:4]
        return [self.px[(y + r) * self.W + x:(y + r) * self.W + x + w] for r in range(h)]

    def metrics(self):
        import statistics as st
        up = [self.recs[c] for c in range(0x41, 0x5B) if c in self.recs and chr(c) not in 'JQ']
        lo = [self.recs[ord(c)] for c in 'acemnorsuvwxz' if ord(c) in self.recs]
        return {'cap': st.median(r[1] for r in up), 'base': st.median(r[6] + r[1] for r in up),
                'xh': st.median(r[1] for r in lo), 'xoff': st.median(r[5] for r in up)}

    def build(self, glyphs, drop=()):
        """New FNTS with `glyphs` {code: (rows of 0..15, adv, xoff, yoff)} added/replacing, the
        codes in `drop` removed; the atlas is repacked (shelves, width kept, height grown as
        needed) and kerning pairs touching a replaced/dropped code are removed."""
        cells = {}
        for code, r in self.recs.items():
            if code in glyphs or code in drop:
                continue
            cells[code] = (self.glyph(code), r[4], r[5], r[6])
        cells.update(glyphs)
        order = sorted(cells, key=lambda c: (-len(cells[c][0]), c))
        W = self.W
        x = y = shelf = 0
        place = {}
        for c in order:
            rows = cells[c][0]
            h = len(rows)
            w = len(rows[0]) if rows else 0
            if x + w + 1 > W:
                x, y, shelf = 0, y + shelf + 1, 0
            place[c] = (x, y)
            x += w + 1
            shelf = max(shelf, h)
        H = y + shelf + 1
        H += H % 2
        px = bytearray(W * H)
        recs = []
        for c in sorted(cells):
            rows, adv, xoff, yoff = cells[c]
            px0, py0 = place[c]
            for r, row in enumerate(rows):
                px[(py0 + r) * W + px0:(py0 + r) * W + px0 + len(row)] = bytes(row)
            w = len(rows[0]) if rows else 0
            recs.append(struct.pack('<HBBHHBbbB', c, w, len(rows), px0, py0, adv, xoff, yoff, 0))
        kern = [k for k in self.kern if k[0] in cells and k[1] in cells and
                k[0] not in glyphs and k[1] not in glyphs]
        rec_end = 0x20 + 12 * len(recs)
        bm = (rec_end + 15) & ~15
        bits = bytearray((a & 15) | ((b & 15) << 4) for a, b in zip(px[0::2], px[1::2]))
        kern_off = bm + 16 + len(bits)
        head = bytearray(self.head)
        struct.pack_into('<I', head, 4, rec_end + 16 + len(bits))
        struct.pack_into('<H', head, 10, len(recs))
        struct.pack_into('<III', head, 0x14, 0x20, kern_off, bm)
        bmh = bytearray(self.bm_head)
        struct.pack_into('<HH', bmh, 4, W, H)
        out = bytearray(head) + b''.join(recs)
        out += bytes(bm - len(out)) + bmh + bits
        out += struct.pack('<I', len(kern)) + b''.join(struct.pack('<HHbxxx', *k) for k in kern)
        return bytes(out)


# Which system font draws the Cyrillic of each game font (all ship with macOS and cover
# Ukrainian). Font 0 is a flared humanist face, fonts 1 and 2 are uncial.
FACE = {0: 'GillSans-SemiBold', 1: 'Luminari-Regular', 2: 'Luminari-Regular'}


def render(face, cap, codes, cell=None):
    """CoreText glyphs (tools/loc/glyphs.swift) scaled so the face's cap height is `cap` px:
    {code: (rows 0..15 tight, advance, x bearing, top row relative to the baseline)}."""
    cell = cell or int(cap * 3)
    base = int(cap * 2)
    uni = [ord(UNICODE_OF[c]) for c in codes]
    args = [face, str(cap), str(cell), str(base)]
    # Glyph cache (ROTK_GLYPH_CACHE, or glyphcache/ next to this file when present): the DMG app
    # ships the renders, because a player's Mac may have no swift (Xcode command line tools).
    cdir = os.environ.get('ROTK_GLYPH_CACHE') or os.path.join(HERE, 'glyphcache')
    key = os.path.join(cdir, hashlib.sha1(' '.join(args + [str(u) for u in uni]).encode()).hexdigest() + '.bin')
    if os.path.exists(key):
        data = open(key, 'rb').read()
    else:
        with tempfile.TemporaryDirectory() as td:
            out = os.path.join(td, 'g.bin')
            subprocess.run(['swift', os.path.join(HERE, 'glyphs.swift')] + args[:3] + [args[3], out]
                           + [str(u) for u in uni], check=True, capture_output=True)
            data = open(out, 'rb').read()
        if os.environ.get('ROTK_GLYPH_CACHE'):
            os.makedirs(cdir, exist_ok=True)
            open(key, 'wb').write(data)
    res = {}
    o = 4
    for c in codes:
        u, w, h = struct.unpack_from('<III', data, o)
        o += 12
        cov = data[o:o + w * h]
        o += w * h
        rows = [cov[r * w:(r + 1) * w] for r in range(h)]
        ink = [(r, i) for r in range(h) for i in range(w) if rows[r][i] >= 24]
        r0 = min(r for r, _ in ink)
        r1 = max(r for r, _ in ink)
        c0 = min(i for _, i in ink)
        c1 = max(i for _, i in ink)
        tight = [[(v * 15 + 127) // 255 for v in rows[r][c0:c1 + 1]] for r in range(r0, r1 + 1)]
        res[c] = (tight, w - 1, c0, r0 - base)
    return res


def cyrillic(font, face, codes=NEW_CODES):
    """The glyph set for Font.build: Cyrillic letters in the game font's metrics."""
    m = font.metrics()
    g = render(face, m['cap'], codes)
    # A face's cap-height metric is not what its capitals measure (Luminari's are taller):
    # scale by the measured flat-topped capitals so they match the Latin ones.
    probe = [CODEPAGE[ch] for ch in 'БГЛПШ' if CODEPAGE[ch] in g]
    import statistics
    got = statistics.median(len(g[c][0]) for c in probe)
    if abs(got - m['cap']) >= 1:
        g = render(face, m['cap'] * m['cap'] / got, codes)
    out = {}
    for c, (rows, adv, c0, top) in g.items():
        w = len(rows[0])
        out[c] = (rows, max(w + 1, int(round(adv))), int(m['xoff']), int(m['base'] + top))
    return out
