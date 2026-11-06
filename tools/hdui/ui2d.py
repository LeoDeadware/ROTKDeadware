#!/usr/bin/env python3
"""ui2d.py CENSUS_DIR > ui2d.tsv

The textures the game draws in 2D (menus, HUD, fonts, button glyphs) in the scenes of
tools/hdui/dump.sh: per scene run, the D3D8 HLE's lines
  draw N: prim P n C ... z ZEN/ZWR ZFUNC ... t0=FMT WxH ... dDATA      (ROTK_DEV_LOG_DRAWS=F)
  d3d8: tex fM DATA hashed HASH (...)                           (ROTK_DEV_LOG_TEX=a-b)
2D = a draw with the depth test off or ZFUNC ALWAYS (z 0/.. or z ../.. 207): the game's HUD,
menus, text and glyphs (pre-transformed XYZRHW quads; its 3D world is drawn with z LESS/GEQUAL). Each such draw's stage-0 texture data address -> its content hash
(the last hash logged for that address up to F). Output: scene frame hash w h format draws
text box per texture (text and box are not known from this log: 0 and 0,0-0,0).
"""
import os
import re
import sys

DRAW = re.compile(r"draw \d+: prim .* z (\d+)/\d+ ([0-9A-F]+) .* t0=\S+ \d+x\d+ .*? d([0-9A-F]{8})")
HASHED = re.compile(r"d3d8: tex f(\d+) ([0-9A-F]{8}) hashed ([0-9A-F]{16})")


def main():
    out = sys.argv[1]
    index = {}
    for line in open(os.path.join(out, "dump", "index.tsv")):
        f = line.rstrip("\n").split("\t")
        index.setdefault(f[0], (f[1], f[2], f[3]))
    scenes = os.path.join(out, "scenes")
    print("# scene\tframe\thash\tw\th\tformat\tdraws\ttext\tbox")
    for scene in sorted(os.listdir(scenes)):
        log = os.path.join(scenes, scene, "runtime.log")
        if not os.path.exists(log):
            continue
        last = {}  # data -> hash
        frame = open(os.path.join(scenes, scene, "frame")).read().strip() if os.path.exists(
            os.path.join(scenes, scene, "frame")) else "?"
        use = {}   # hash -> [draws, text, x0, y0, x1, y1, frame]
        for line in open(log, errors="replace"):
            m = HASHED.search(line)
            if m:
                last[m.group(2)] = m.group(3).lower()
                continue
            m = DRAW.search(line)
            if not m or (m.group(1) != "0" and m.group(2) != "207") or m.group(3) == "00000000":
                continue
            h = last.get(m.group(3))
            if not h:
                continue
            u = use.setdefault(h, [0, 0, 0, 0, 0, 0, frame])
            u[0] += 1
        for h, u in sorted(use.items()):
            w, hh, fmt = index.get(h, ("?", "?", "?"))
            print(f"{scene}\t{u[6]}\t{h}\t{w}\t{hh}\t{fmt}\t{u[0]}\t{u[1]}\t{u[2]:.0f},{u[3]:.0f}-{u[4]:.0f},{u[5]:.0f}")


if __name__ == "__main__":
    main()
