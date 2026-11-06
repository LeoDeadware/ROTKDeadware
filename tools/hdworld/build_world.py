#!/usr/bin/env python3
"""build_world.py WORLD_DIR --out DIR [--ui UI2D.tsv] [--top N] [--budget MB] [--min-share S] [--only HASH,...]

Builds the HD world texture pack (src/polish/texpack, ROTK_HDWORLD) from the census of
tools/hdworld/dump.sh: WORLD_DIR/coverage.tsv (tools/hdworld/coverage.py: textures ranked by
their share of the screen in probe runs) and WORLD_DIR/dump. The most visible ones first: the
textures listed in tools/hdworld/must_textures.tsv (seen by the player but not by the coverage
runs, PIT-H02), then by coverage score down to --min-share (default 0.0001 = 0.01 % of a level's
screen), at most --top (default 300), not the UI pack's (--ui, default the census' ui2d.tsv),
at least 16x16. Per texture:
  hdfix pad (16 texels, wrapped when the texture tiles, else edge-repeated, PIT-H04)
  -> hdfix split -> 4x upscale (tools/hdui/hdlib.py: Real-ESRGAN when installed, else MetalFX)
  -> hdfix join (tone corrected to the original) -> hdfix crop (the border off; --half for 2x).
Scale: 4x up to 128 texels, 2x up to 512, larger ones skipped; the total (level 0, RGBA8) kept
under --budget (default 512 MB; texpack keeps no image in RAM, the D3D8 HLE adds mips on the GPU).
Output: <hash>.png + manifest.tsv (hash kind w h k edges score top-level) in --out. Derived from
the game's data: never committed (R-L2); the .app builds it at install (tools/install.d/30-hd.sh).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "hdui"))
import hdlib  # noqa: E402

PAD = 16


def ui_hashes(path):
    out = set()
    if path and os.path.exists(path):
        for line in open(path):
            if line.strip() and not line.startswith("#"):
                out.add(line.split("\t")[2])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("world")
    ap.add_argument("--out", required=True)
    ap.add_argument("--ui", default=os.path.join(hdlib.CACHE, "census", "ui2d.tsv"))
    ap.add_argument("--top", type=int, default=300)
    ap.add_argument("--budget", type=float, default=512)
    ap.add_argument("--min-share", type=float, default=0.0001)
    ap.add_argument("--only", default="")
    ap.add_argument("--must-list", default=os.path.join(HERE, "must_textures.tsv"))
    a = ap.parse_args()
    only = set(filter(None, a.only.split(",")))
    ui = ui_hashes(a.ui)
    dumps = [os.path.join(a.world, "dump")]

    rows = []  # (hash, w, h, score, top level)
    for line in open(os.path.join(a.world, "coverage.tsv")):
        if line.startswith("#") or not line.strip():
            continue
        h, w, hh, fmt, lv, score, per = line.rstrip("\n").split("\t")
        rows.append((h, int(w), int(hh), float(score), per.split(",")[0].rsplit(":", 1)[0]))
    listed = []
    if os.path.exists(a.must_list):
        for line in open(a.must_list):
            if line.strip() and not line.startswith("#"):
                h, w, hh, lvl = line.rstrip("\n").split("\t")[:4]
                listed.append((h, int(w), int(hh), 1.0, lvl + " (must)"))

    todo, total, seen = [], 0, set()
    for i, (h, w, hh, score, top) in enumerate(listed + rows):
        forced = i < len(listed)
        if not forced and (score < a.min_share or len(todo) >= a.top):
            break
        if h in seen or (only and h not in only) or h in ui or min(w, hh) < 16:
            continue
        src = hdlib.find_png(h, dumps)
        k = 4 if max(w, hh) <= 128 else 2 if max(w, hh) <= 512 else 0
        if not src or not k:
            continue
        size = w * hh * k * k * 4
        if total + size > a.budget * 2**20:
            print(f"  budget {a.budget:.0f} MB reached")
            break
        seen.add(h)
        todo.append((h, w, hh, k, score, top, src))
        total += size
    os.makedirs(a.out, exist_ok=True)
    work = tempfile.mkdtemp(prefix="hdworld-")
    hdfix = hdlib.tool("hdfix")
    inp, up = os.path.join(work, "in"), os.path.join(work, "up")
    os.makedirs(inp)
    edges = {}
    for h, w, hh, k, score, top, src in todo:
        padded = os.path.join(work, h + "_p.png")
        edges[h] = subprocess.run([hdfix, "pad", src, padded, str(PAD), "auto"], check=True,
                                  capture_output=True, text=True).stdout.strip()
        subprocess.run([hdfix, "split", padded, os.path.join(inp, h + "_c.png"), os.path.join(inp, h + "_a.png")],
                       check=True)
    eng = hdlib.upscale4(inp, up) if todo else hdlib.engine()
    with open(os.path.join(a.out, "manifest.tsv"), "w") as man:
        man.write("# hash\tkind\tw\th\tk\tedges\tscore\ttop level\tengine\n")
        for h, w, hh, k, score, top, src in todo:
            joined = os.path.join(work, h + "_j.png")
            subprocess.run([hdfix, "join", os.path.join(work, h + "_p.png"), os.path.join(up, h + "_c.png"),
                            os.path.join(up, h + "_a.png"), joined], check=True)
            subprocess.run([hdfix, "crop", joined, os.path.join(a.out, h + ".png"), str(PAD * 4)] +
                           (["--half"] if k == 2 else []), check=True)
            man.write(f"{h}\tworld\t{w}\t{hh}\t{k}\t{edges[h]}\t{score:.4f}\t{top}\t{eng}\n")
    shutil.rmtree(work)
    print(f"hd_world: {len(todo)} textures, engine {eng}, {total / 2**20:.0f} MB level 0 RGBA "
          f"(VRAM with mips {total * 4 / 3 / 2**20:.0f} MB if all were bound at once) -> {a.out}")
    return 0 if todo else 1


if __name__ == "__main__":
    sys.exit(main())
