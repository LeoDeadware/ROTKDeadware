#!/usr/bin/env python3
"""glyph_aspect.py shot.png [colour] [min_px]

Aspect ratio (width / height) of the largest button-prompt glyph of one colour
in the top-right quarter of a shot (where ROTK draws its tutorial prompts:
"A Speed", "X Parry"). colour: green (A, default), blue (X), red (B), yellow (Y).
A glyph pixel is saturated in that hue (the key face); the blob's bounding box
is measured over its 4-connected component (a fireball next to it is not green).
Prints "aspect <w/h> box x0,y0-x1,y1 px <n>"; exit 0 if a blob of >= min_px
(default 120 at 640x480, scaled with the shot's area) exists and 0.8 <= w/h <= 1.35
(the original keys are round), 1 if not, 2 on error. Used by
tests/scripts/prompt_aspect.sh (owner test c96b3d1: at 16:10 the "X Parry" key
was an oval, about 2:1). No third-party modules (png_diff.read_png).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from png_diff import read_png  # noqa: E402

HUES = {
    "green": lambda r, g, b: g > 60 and g > r + 25 and g > b + 25,
    "blue": lambda r, g, b: b > 110 and b > r + 60 and b > g + 40,
    "red": lambda r, g, b: r > 120 and r > g + 70 and r > b + 70,
    "yellow": lambda r, g, b: r > 150 and g > 130 and b < 0.55 * min(r, g),
}


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    hue = HUES[sys.argv[2] if len(sys.argv) > 2 else "green"]
    w, h, px = read_png(path)
    min_px = int(sys.argv[3]) if len(sys.argv) > 3 else 120
    min_px = int(min_px * (w * h) / (640 * 480))
    x0, y0 = w // 2, 0
    mask = {}
    for y in range(y0, h // 2):
        row = y * w * 3
        for x in range(x0, w):
            i = row + 3 * x
            if hue(px[i], px[i + 1], px[i + 2]):
                mask[(x, y)] = True
    best = None
    seen = set()
    for p in mask:
        if p in seen:
            continue
        stack, comp = [p], []
        seen.add(p)
        while stack:
            q = stack.pop()
            comp.append(q)
            for d in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                n = (q[0] + d[0], q[1] + d[1])
                if n in mask and n not in seen:
                    seen.add(n)
                    stack.append(n)
        if best is None or len(comp) > len(best):
            best = comp
    if not best or len(best) < min_px:
        print(f"no glyph ({len(best) if best else 0} px < {min_px})")
        return 1
    xs = [q[0] for q in best]
    ys = [q[1] for q in best]
    bw, bh = max(xs) - min(xs) + 1, max(ys) - min(ys) + 1
    a = bw / bh
    print(f"aspect {a:.3f} box {min(xs)},{min(ys)}-{max(xs) + 1},{max(ys) + 1} px {len(best)}")
    return 0 if 0.8 <= a <= 1.35 else 1


if __name__ == "__main__":
    sys.exit(main())
