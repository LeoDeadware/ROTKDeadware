#!/usr/bin/env python3
"""ROTK text banks: `subT` resources of the .scx files (GlobScen.scx: UI, <level>NN.scx: subtitles).

Layout (decoded from the bytes, docs/loc/FORMAT.md):
  u32 blocks; per block (language): u32 count, u32 pool size, char tag[4] ('ENG'), 12 bytes 0,
  u32 0x01020304, u32 0x10203040, count x {u32 key, u32 value offset}, pool (NUL-terminated
  8-bit strings, padded to 4).
  GlobScen: key = offset of a key string in the pool ("code1" -> "Restore Health").
  Levels: key = a numeric id (0x0201xxxx), the subtitle's id.
The game uses the ENG block (NTSC disc); the other blocks hold one test string each.

Ids used by the tables in mods/loc: '<file stem>:<key>' (GlobScen:code1, pat01:0x02010000).
"""
import struct

MARK = struct.pack('<II', 0x01020304, 0x10203040)


class Block:
    def __init__(self, tag, meta, entries, strkeys, raw=b''):
        self.raw = raw            # the block's original bytes: kept when entries are unchanged
        self.orig = list(entries)
        self.tag = tag            # 'ENG'
        self.meta = meta          # the 0x18 bytes after count/pool size (tag, zeros, marker)
        self.entries = entries    # [(key, value)]: key str (string keys) or int; value bytes
        self.strkeys = strkeys


def _cstr(pool, off):
    end = pool.index(b'\0', off)
    return pool[off:end]


def parse(raw):
    n = struct.unpack_from('<I', raw, 0)[0]
    o = 4
    blocks = []
    for _ in range(n):
        count, psize = struct.unpack_from('<II', raw, o)
        meta = raw[o + 8:o + 0x20]
        if meta[0x10:0x18] != MARK:
            raise ValueError(f'no text-bank marker at {o:#x}')
        tag = meta[0:4].split(b'\0')[0].decode('latin1')
        ents = [struct.unpack_from('<II', raw, o + 0x20 + 8 * i) for i in range(count)]
        pool = raw[o + 0x20 + 8 * count:o + 0x20 + 8 * count + psize]
        strkeys = bool(ents) and all(k < psize and (k == 0 or pool[k - 1] == 0) for k, _ in ents)
        entries = []
        for k, v in ents:
            key = _cstr(pool, k).decode('latin1') if strkeys else k
            entries.append((key, _cstr(pool, v)))
        end = o + 0x20 + 8 * count + psize
        blocks.append(Block(tag, meta, entries, strkeys, raw[o:end]))
        o = end
    if o != len(raw):
        raise ValueError(f'text bank: {len(raw) - o} trailing bytes')
    return blocks


def build(blocks):
    out = bytearray(struct.pack('<I', len(blocks)))
    for b in blocks:
        if b.raw and b.entries == b.orig:
            out += b.raw
            continue
        pool = bytearray()
        index = {}

        def put(s):
            if s not in index:
                index[s] = len(pool)
                pool.extend(s + b'\0')
            return index[s]

        ents = []
        for key, val in b.entries:
            k = put(key.encode('latin1')) if b.strkeys else key
            ents.append((k, put(val)))
        while len(pool) % 4:
            pool.append(0)
        out += struct.pack('<II', len(ents), len(pool)) + b.meta
        for k, v in ents:
            out += struct.pack('<II', k, v)
        out += pool
    return bytes(out)


def key_str(key):
    return key if isinstance(key, str) else f'{key:#010x}'
