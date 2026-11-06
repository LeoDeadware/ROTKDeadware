#!/usr/bin/env python3
"""build_pack.py CENSUS_DIR --out DIR [--only HASH,...]

Builds the HD UI texture pack (src/polish/texpack, ROTK_HDUI) from the census of
tools/hdui/dump.sh: every texture the game draws in 2D in the census scenes (ui2d.tsv: menus,
HUD, font atlases, button glyphs), per texture
  hdfix split -> 4x upscale (tools/hdui/hdlib.py: Real-ESRGAN when installed, else MetalFX)
  -> hdfix join, by kind:
    font   font atlases (palettized / 8-bit alpha formats, or drawn as text): --crisp (sharp
           glyph edges, no tone correction);
    glyph  button glyphs (small square textures drawn small): --ink;
    ui     everything else (panels, frames, pictures, loading art): tone-corrected to the original.
Scale: 4x up to 512 texels, 2x (4x then halved) up to 1024. The Xbox glyphs that
src/polish/texpack/prompts.mm replaces with drawn Series glyphs are left to it.
Output: <hash>.png + manifest.tsv (hash kind w h k mode engine scenes) in --out. Derived from the
game's data: never committed (R-L2); the .app builds it at install (tools/install.d/30-hd.sh).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hdlib  # noqa: E402

ROOT = os.path.abspath(os.path.join(hdlib.HERE, "..", ".."))
FONT_FORMATS = {"00", "01", "0B", "19", "1A"}  # L8, AL8, P8, A8, A8L8


def prompt_hashes():
    """Glyphs prompts.mm draws itself (its table: {0x<hash>ull, ...})."""
    p = os.path.join(ROOT, "src", "polish", "texpack", "prompts.mm")
    return set(m.lower() for m in re.findall(r"0x([0-9a-fA-F]{16})ull", open(p).read())) if os.path.exists(p) else set()


def ui_textures(census):
    """hash -> dict(w, h, fmt, draws, text, boxes, scenes) from ui2d.tsv."""
    out = {}
    for line in open(os.path.join(census, "ui2d.tsv")):
        if line.startswith("#") or not line.strip():
            continue
        scene, frame, h, w, hh, fmt, draws, text, box = line.rstrip("\n").split("\t")
        if not w.isdigit():
            continue
        x0, y0, x1, y1 = (float(v) for v in re.findall(r"-?[\d.]+", box))
        t = out.setdefault(h, dict(w=int(w), h=int(hh), fmt=fmt, draws=0, text=0, bw=0.0, bh=0.0, scenes=[]))
        t["draws"] += int(draws)
        t["text"] += int(text)
        t["bw"], t["bh"] = max(t["bw"], x1 - x0), max(t["bh"], y1 - y0)
        t["scenes"].append(scene)
    return out


def kind(t):
    if t["fmt"] in FONT_FORMATS or (t["text"] and t["text"] * 2 >= t["draws"]):
        return "font"
    if t["w"] == t["h"] and t["w"] <= 64 and t["bw"] <= 48 and t["bh"] <= 48:
        return "glyph"
    return "ui"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("census")
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", default="")
    a = ap.parse_args()
    only = set(filter(None, a.only.split(",")))
    dirs = [os.path.join(a.census, "dump"), os.path.join(a.census, "texdump")]
    skip = prompt_hashes()
    todo, missing = [], []
    for h, t in sorted(ui_textures(a.census).items()):
        if (only and h not in only) or h in skip or min(t["w"], t["h"]) < 4 or max(t["w"], t["h"]) > 1024:
            continue
        src = hdlib.find_png(h, dirs)
        if not src:
            missing.append(h)
            continue
        k = 4 if max(t["w"], t["h"]) <= 512 else 2
        todo.append((h, t, kind(t), k, src))
    os.makedirs(a.out, exist_ok=True)
    work = tempfile.mkdtemp(prefix="hdui-")
    hdfix = hdlib.tool("hdfix")
    inp, up = os.path.join(work, "in"), os.path.join(work, "up")
    os.makedirs(inp)
    for h, t, kd, k, src in todo:
        subprocess.run([hdfix, "split", src, os.path.join(inp, h + "_c.png"), os.path.join(inp, h + "_a.png")],
                       check=True)
    eng = hdlib.upscale4(inp, up, "realesrgan-x4plus-anime") if todo else hdlib.engine()
    mode = {"font": "--crisp", "glyph": "--ink", "ui": ""}
    with open(os.path.join(a.out, "manifest.tsv"), "w") as man:
        man.write("# hash\tkind\tw\th\tk\tmode\tengine\tscenes\n")
        for h, t, kd, k, src in todo:
            dst = os.path.join(a.out, h + ".png")
            joined = os.path.join(work, h + "_j.png") if k == 2 else dst
            subprocess.run([hdfix, "join", src, os.path.join(up, h + "_c.png"), os.path.join(up, h + "_a.png"),
                            joined] + ([mode[kd]] if mode[kd] else []), check=True)
            if k == 2:
                subprocess.run([hdfix, "crop", joined, dst, "0", "--half"], check=True)
            man.write(f"{h}\t{kd}\t{t['w']}\t{t['h']}\t{k}\t{mode[kd] or 'tone'}\t{eng}\t{','.join(sorted(set(t['scenes'])))}\n")
    shutil.rmtree(work)
    kinds = {}
    for _, _, kd, _, _ in todo:
        kinds[kd] = kinds.get(kd, 0) + 1
    print(f"hd_ui: {len(todo)} textures ({', '.join(f'{v} {k}' for k, v in sorted(kinds.items()))}), engine {eng}, "
          f"{len(missing)} without a decodable dump -> {a.out}")
    return 0 if todo else 1


if __name__ == "__main__":
    sys.exit(main())
