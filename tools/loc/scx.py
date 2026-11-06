#!/usr/bin/env python3
"""ROTK .scx container: chunks, resources, the game's LZ, and in-place resource replacement.

Format (decoded from FUN_000f3c30 / FUN_000f2b30 / FUN_000f2ce0, see docs/loc/FORMAT.md):
  The file is read in 64 KiB pages. A page is a run of chunks {tag4 (bytes reversed on disk),
  u32 size incl. header, ...}; `FILL` ends the page wherever it stands (it may be only 4 bytes).
  Level files start with a `STOC` page (table of contents by page index), so their pages must
  never move: every edit here is done in place.
  A resource is a `SHOC`/`SONO` chunk with sub-tag `SHDR` (+0x18 type, +0x1c index, +0x20 size)
  followed by data chunks with sub-tag `SDAT` (raw: payload at +0x40, copy capped to the size)
  or `Rdat` (+0x40 u32 decoded length, +0x44 LZ stream; bytes after the stream are ignored).
  Rdat chunks decode independently (no back-reference crosses a chunk: checked on GlobScen).
  Unknown tags (e.g. `PADD`) are skipped by size.

LZ command stream (FUN_000f2ce0), v = b0<<8 | b1:
  (v & 0x8800) == 0x8800, ((b0>>4)&7) == 0 : literal run of (v & 0x7ff) bytes
  (v & 0x8800) == 0x8800, else             : RLE, b1+3 copies of out[-d], d = (b0&7)<<3 | (b0>>4)&7
  b0 & 0x80 (and not both bits)            : backward copy from out[-(v&0xfff)+2] down, n bytes
  else                                     : forward copy from out[-(v&0xfff)], n bytes
  n = ((b0>>4)&7) + 3, or (next byte + 7) + 3 when that field is 7.
  The forward copy is a block memcpy, so an overlapping copy (offset < n) is never emitted here.

  python3 tools/loc/scx.py list <file.scx>
  python3 tools/loc/scx.py dump <file.scx> <type> <index> <out.bin>
  python3 tools/loc/scx.py selftest <file.scx>...   (container + LZ round trip, see tests/)
"""
import struct
import sys

PAGE = 0x10000
DATA_HDR = 0x40
RDAT_PAYLOAD = 0x7FFF  # keep encoded chunks small enough for any page slot


def tag_of(b):
    return bytes(b[::-1]).decode('latin1')


def tag_bytes(t):
    return t.encode('latin1')[::-1]


class Chunk:
    __slots__ = ('off', 'tag', 'size', 'sub')

    def __init__(self, off, tag, size, sub):
        self.off, self.tag, self.size, self.sub = off, tag, size, sub

    @property
    def end(self):
        return self.off + self.size

    def __repr__(self):
        return f'<{self.tag}/{self.sub} @{self.off:#x}+{self.size:#x}>'


def segments(data):
    """Level files: STOC entries {u32 kind, u32 name offset, u16 -, u16 first page, u16 -, u16 pages}.
    Returns [(name, first page, page count)] or None (no STOC: every page is chunked)."""
    if data[0:4] != tag_bytes('STOC'):
        return None
    base = 0x10
    n = struct.unpack_from('<I', data, base + 8)[0]
    segs = []
    for i in range(n):
        e = base + 0x18 + 16 * i
        name_off = struct.unpack_from('<I', data, e + 4)[0]
        first = struct.unpack_from('<H', data, e + 10)[0]
        count = struct.unpack_from('<H', data, e + 14)[0]
        name = data[name_off:name_off + 64].split(b'\0')[0].decode('latin1')
        segs.append((name, first, count))
    return segs


def chunks(data):
    """All chunks in file order; FILL chunks get size = rest of their page. Pages outside the
    STOC segments of a level file (streams) are one 'RAW' chunk each."""
    out = []
    page = 0
    n = len(data)
    segs = segments(data)
    chunked = None
    if segs is not None:
        chunked = {0}
        for _, first, count in segs:
            chunked.update(range(first, first + count))
    while page < n:
        if chunked is not None and page // PAGE not in chunked:
            out.append(Chunk(page, 'RAW', min(PAGE, n - page), ''))
            page += PAGE
            continue
        o = page
        end = min(page + PAGE, n)
        while o < end:
            if end - o < 4:
                raise ValueError(f'{end - o} stray bytes at {o:#x}')
            t = tag_of(data[o:o + 4])
            if t == 'FILL':
                out.append(Chunk(o, 'FILL', end - o, ''))
                break
            if end - o < 8:
                raise ValueError(f'truncated chunk at {o:#x}')
            size = struct.unpack_from('<I', data, o + 4)[0]
            if size < 8 or o + size > end:
                raise ValueError(f'bad chunk {t!r} size {size:#x} at {o:#x}')
            sub = tag_of(data[o + 16:o + 20]) if t in ('SHOC', 'SONO') and size >= 20 else ''
            out.append(Chunk(o, t, size, sub))
            o += size
        page += PAGE
    return out


class Resource:
    def __init__(self, hdr, data):
        self.hdr = hdr          # SHDR chunk
        self.parts = []         # data chunks (SDAT/Rdat)
        d = data
        self.type = tag_of(d[hdr.off + 0x18:hdr.off + 0x1c])
        self.version = struct.unpack_from('<I', d, hdr.off + 0x14)[0]
        self.index, self.size = struct.unpack_from('<II', d, hdr.off + 0x1c)

    @property
    def key(self):
        return (self.type, self.index)

    def __repr__(self):
        kinds = {}
        for p in self.parts:
            kinds[p.sub] = kinds.get(p.sub, 0) + 1
        return f'{self.type}#{self.index:#x} size {self.size:#x} @{self.hdr.off:#x} {kinds}'


def resources(data, cs=None):
    cs = cs or chunks(data)
    res = []
    cur = None
    for c in cs:
        if c.sub == 'SHDR':
            cur = Resource(c, data)
            res.append(cur)
        elif c.sub in ('SDAT', 'Rdat') and cur is not None:
            cur.parts.append(c)
    return res


def find(data, rtype, index=None):
    for r in resources(data):
        if r.type == rtype and (index is None or r.index == index):
            return r
    raise KeyError(f'no resource {rtype} {index}')


# ---------------------------------------------------------------- LZ
def lz_decode(src, n, pos=0):
    """Decodes n bytes from src[pos:]; returns (bytes, bytes of src used)."""
    out = bytearray()
    i = pos
    while len(out) < n:
        b0 = src[i]
        b1 = src[i + 1]
        v = (b0 << 8) | b1
        i += 2
        if v & 0x8800 == 0x8800:
            if (b0 >> 4) & 7 == 0:
                c = v & 0x7FF
                out += src[i:i + c]
                i += c
            else:
                d = ((v >> 5) & 0x38) | ((b0 >> 4) & 7)
                out += bytes([out[-d]]) * (b1 + 3)
        else:
            c = (b0 >> 4) & 7
            if c == 7:
                c = src[i] + 7
                i += 1
            c += 3
            p = len(out) - (v & 0xFFF)
            if p < 0:
                raise ValueError(f'LZ reference before start at {i:#x}')
            if b0 & 0x80:
                p += 2
                for k in range(c):
                    out.append(out[p - k])
            else:
                for k in range(c):
                    out.append(out[p + k])
    if len(out) != n:
        raise ValueError(f'LZ overrun: {len(out)} for {n}')
    return bytes(out), i - pos


def lz_encode(raw):
    """Greedy LZ for the game's decoder: forward copies (no overlap), RLE runs, literals."""
    out = bytearray()
    lit = bytearray()
    n = len(raw)
    heads = {}

    def flush():
        j = 0
        while j < len(lit):
            c = min(0x7FF, len(lit) - j)
            out.append(0x88 | (c >> 8))
            out.append(c & 0xFF)
            out.extend(lit[j:j + c])
            j += c
        lit.clear()

    i = 0
    while i < n:
        # run of one byte (RLE with d = 1 needs a previous byte equal to it)
        run = 0
        if i > 0:
            b = raw[i - 1]
            while i + run < n and run < 258 and raw[i + run] == b:
                run += 1
        best_len, best_off = 0, 0
        if i + 3 <= n:
            k = raw[i:i + 3]
            for p in reversed(heads.get(k, ())[-48:]):
                off = i - p
                if off > 0xFFF:
                    break
                lim = min(265, n - i, off)  # no overlap
                m = 3
                while m < lim and raw[p + m] == raw[i + m]:
                    m += 1
                if m > best_len:
                    best_len, best_off = m, off
                    if m == lim:
                        break
        take = 0
        if run >= 3 and run >= best_len:
            flush()
            out.append(0x80 | 0x10 | 0x08)  # d = 1: (d&7)<<4, (d>>3)&7 = 0
            out.append(run - 3)
            take = run
        elif best_len >= 3:
            flush()
            m = best_len - 3
            if m < 7:
                out.append((m << 4) | (best_off >> 8))
                out.append(best_off & 0xFF)
            else:
                out.append(0x70 | (best_off >> 8))
                out.append(best_off & 0xFF)
                out.append(m - 7)
            take = best_len
        else:
            lit.append(raw[i])
            take = 1
        for q in range(i, min(i + take, n - 2)):
            heads.setdefault(raw[q:q + 3], []).append(q)
        i += take
    flush()
    return bytes(out)


def decode(data, res):
    """The resource's bytes as the game assembles them."""
    buf = bytearray()
    for c in res.parts:
        if c.sub == 'SDAT':
            buf += data[c.off + DATA_HDR:c.end]
        else:
            n = struct.unpack_from('<I', data, c.off + DATA_HDR)[0]
            payload, _ = lz_decode(data, n, c.off + DATA_HDR + 4)
            buf += payload
    if len(buf) < res.size:
        raise ValueError(f'{res}: only {len(buf):#x} bytes')
    return bytes(buf[:res.size])


# ---------------------------------------------------------------- in-place replacement
def _data_hdr(data, res, sub, size):
    tmpl = res.parts[0] if res.parts else res.hdr
    h = bytearray(data[tmpl.off:tmpl.off + DATA_HDR])
    h[0:4] = tag_bytes(res.hdr.tag)
    h[4:8] = struct.pack('<I', size)
    h[8:16] = bytes(8)
    h[16:20] = tag_bytes(sub)
    h[20:24] = bytes(4)
    return h


def _padd(size):
    return tag_bytes('PADD') + struct.pack('<I', size) + bytes(size - 8)


def replace(data, rtype, index, payload, lz=False):
    """Returns a copy of data with resource (rtype, index) re-encoded to `payload`, written over the
    bytes the old resource occupied (its chunks plus FILL slack in their pages): pages never move.
    Raises ValueError when the encoded resource does not fit."""
    cs = chunks(data)
    res = [r for r in resources(data, cs) if r.type == rtype and r.index == index]
    if len(res) != 1:
        raise KeyError(f'{rtype} {index}: {len(res)} matches')
    res = res[0]
    own = [res.hdr] + res.parts
    first, last = cs.index(own[0]), cs.index(own[-1])
    span = cs[first:last + 1]
    for c in span:
        if c not in own and c.tag != 'FILL':
            raise ValueError(f'{rtype} {index}: foreign chunk {c} inside the resource')
    if last + 1 < len(cs) and cs[last + 1].tag == 'FILL' and cs[last + 1].off // PAGE == own[-1].off // PAGE:
        span.append(cs[last + 1])
    # slots: contiguous regions, each either ending at its page end (FILL allowed) or not
    slots = []
    for c in span:
        if slots and slots[-1][1] == c.off and c.off % PAGE:   # a slot never crosses a page
            slots[-1][1] = c.end
        else:
            slots.append([c.off, c.end])
    raw_kind = not lz and bool(res.parts) and all(p.sub == 'SDAT' for p in res.parts)  # SDAT stays SDAT
    out = bytearray(data)
    hdr = bytearray(data[res.hdr.off:res.hdr.end])
    struct.pack_into('<I', hdr, 0x20, len(payload))
    pos = 0          # payload bytes placed
    placed_hdr = False
    for si, (a, b) in enumerate(slots):
        o = a
        at_page_end = b % PAGE == 0 or b == len(data)
        while True:
            room = b - o
            if not placed_hdr:
                if room < len(hdr):
                    break
                out[o:o + len(hdr)] = hdr
                o += len(hdr)
                placed_hdr = True
                continue
            if pos >= len(payload):
                break
            if raw_kind:
                # one SDAT chunk: raw bytes; only the resource's last chunk may carry padding
                # (the reader concatenates whole chunks and caps at the size)
                take = min(len(payload) - pos, (room - DATA_HDR) & ~3)
                if take <= 0:
                    break
                size = DATA_HDR + ((take + 3) & ~3)
                rest = b - (o + size)
                if pos + take >= len(payload) and 0 <= rest < (4 if at_page_end else 8):
                    size = b - o
                chunk = _data_hdr(data, res, 'SDAT', size) + payload[pos:pos + take]
                chunk += bytes(size - len(chunk))
                out[o:o + size] = chunk
                o += size
                pos += take
                continue
            # one Rdat chunk: as much payload as fits in the room (binary search on the raw length)
            avail = room - DATA_HDR - 4
            if avail < 16:
                break
            lo, hi = 1, min(len(payload) - pos, RDAT_PAYLOAD)
            best = None
            while lo <= hi:
                mid = (lo + hi) // 2
                enc = lz_encode(payload[pos:pos + mid])
                if len(enc) <= avail:
                    best = (mid, enc)
                    lo = mid + 1
                else:
                    hi = mid - 1
            if best is None:
                break
            mid, enc = best
            body = struct.pack('<I', mid) + enc
            size = DATA_HDR + len(body)
            size = (size + 3) & ~3
            rest = b - (o + size)
            if pos + mid >= len(payload) or rest < DATA_HDR + 4 + 16:
                # last chunk in this slot: absorb the slack unless a FILL can take it
                if not (at_page_end and rest >= 4) or rest < 0:
                    size = b - o
            chunk = _data_hdr(data, res, 'Rdat', size) + body
            chunk += bytes(size - len(chunk))
            out[o:o + size] = chunk
            o += size
            pos += mid
        rest = b - o
        if rest:
            if at_page_end:
                out[o:o + 4] = tag_bytes('FILL')
                out[o + 4:b] = bytes(rest - 4)
            elif rest >= 8:
                out[o:b] = _padd(rest)
            else:
                raise ValueError(f'{rtype} {index}: {rest} bytes left in a mid-page slot')
    if not placed_hdr or pos < len(payload):
        if raw_kind and not lz:
            return replace(data, rtype, index, payload, lz=True)   # SDAT too big: LZ (Rdat) it
        need = len(payload) - pos
        raise ValueError(f'{rtype} {index}: does not fit in place ({need:#x} raw bytes left over)')
    return bytes(out)


# ---------------------------------------------------------------- CLI
def _main(argv):
    if len(argv) >= 2 and argv[0] == 'list':
        d = open(argv[1], 'rb').read()
        for r in resources(d):
            print(r)
        return 0
    if len(argv) == 5 and argv[0] == 'dump':
        d = open(argv[1], 'rb').read()
        r = find(d, argv[2], int(argv[3], 0))
        open(argv[4], 'wb').write(decode(d, r))
        return 0
    if len(argv) >= 2 and argv[0] == 'selftest':
        bad = 0
        for f in argv[1:]:
            d = open(f, 'rb').read()
            cs = chunks(d)
            covered = sum(c.size for c in cs)
            rebuilt = b''.join(d[c.off:c.end] for c in cs)
            ok = covered == len(d) and rebuilt == d
            nres = 0
            for r in resources(d, cs):
                if not any(p.sub == 'Rdat' for p in r.parts):
                    continue
                raw = decode(d, r)
                enc = lz_encode(raw)
                back, _ = lz_decode(enc, len(raw))
                ok &= back == raw
                nres += 1
            print(f'{"PASS" if ok else "FAIL"} {f}: {len(cs)} chunks, {nres} LZ resources')
            bad += not ok
        return 1 if bad else 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(_main(sys.argv[1:]))
