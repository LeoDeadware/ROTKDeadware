#!/usr/bin/env python3
"""Counts the hold-to-skip ring's gold pixels (r>200, 140<g<225, b<90) in the bottom-right
corner (x > 0.8 w, y > 0.75 h) of a PNG. Prints the count; exit 0 if >= min (default 30 px
at 640x480, scaled by area)."""
import sys, zlib, struct

def png_rgba(path):
    d = open(path, 'rb').read()
    w, h, bd, ct = struct.unpack('>IIBB', d[16:26])
    i, idat = 8, b''
    while i < len(d):
        n, t = struct.unpack('>I4s', d[i:i + 8])
        if t == b'IDAT': idat += d[i + 8:i + 8 + n]
        i += 12 + n
    bpp = {6: 4, 2: 3}[ct]
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]; c = prev[x - bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line)); prev = line
    return w, h, bpp, rows

w, h, bpp, rows = png_rgba(sys.argv[1])
mn = float(sys.argv[2]) if len(sys.argv) > 2 else 30
mn *= (w * h) / (640 * 480)
n = 0
for y in range(int(h * 0.75), h):
    r = rows[y]
    for x in range(int(w * 0.8), w):
        R, G, B = r[x * bpp], r[x * bpp + 1], r[x * bpp + 2]
        if R > 200 and 140 < G < 225 and B < 90: n += 1
print(f"ring pixels {n} (min {mn:.0f})")
sys.exit(0 if n >= mn else 1)
