#!/usr/bin/env python3
"""prompt_check.py shot.png x0,y0,x1,y1 [min_white] [kx0,ky0,kx1,ky1 [min_dark]]

Is a white label stroke of an on-screen button prompt in this box? Boxes in
pixels of a 640x480 shot (scaled for larger shots). Pass when the fraction of
white pixels (all channels >= 195) in the box is >= min_white (default 0.5)
and, with a key box, the fraction of dark pixels (all channels <= 90) there is
>= min_dark (default 0.3): the key face behind the label is dark, so a fireball
or a bright sky in the box does not pass.

Used by tests/scripts/gamepad_prompts.sh on the "S" of the Xbox Series "LS"
glyph and the bar of the "T" of "LT" (src/polish/texpack/prompts.mm): the
original Xbox "L" glyphs have their grey key face there (white 0.000), the
Series glyphs a white stroke. The prompts blink, so the caller counts frames.
Prints the fractions; exit 0 if it passes, 1 if not, 2 on error.
No third-party modules (png_diff.read_png).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from png_diff import read_png  # noqa: E402


def frac(w, h, px, box, k, test):
    x0, y0, x1, y1 = (int(round(v * k)) for v in box)
    hit = n = 0
    for y in range(max(0, y0), min(h, y1)):
        row = y * w * 3
        for x in range(max(0, x0), min(w, x1)):
            n += 1
            if test(px[row + 3 * x:row + 3 * x + 3]):
                hit += 1
    return hit / n if n else None


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    path, box = sys.argv[1], [int(v) for v in sys.argv[2].split(",")]
    min_white = float(sys.argv[3]) if len(sys.argv) > 3 else 0.5
    key = [int(v) for v in sys.argv[4].split(",")] if len(sys.argv) > 4 else None
    min_dark = float(sys.argv[5]) if len(sys.argv) > 5 else 0.3
    try:
        w, h, px = read_png(path)
    except Exception as e:  # noqa: BLE001
        print(f"prompt_check: {e}")
        return 2
    k = w / 640.0
    fw = frac(w, h, px, box, k, lambda p: min(p) >= 195)
    fd = frac(w, h, px, key, k, lambda p: max(p) <= 90) if key else 1.0
    if fw is None or fd is None:
        print("prompt_check: empty box")
        return 2
    ok = fw >= min_white and fd >= min_dark
    print(f"prompt_check: {os.path.basename(path)} box {box}: white {fw:.3f}"
          + (f", key {key}: dark {fd:.3f}" if key else "") + f" -> {'PASS' if ok else 'no'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
