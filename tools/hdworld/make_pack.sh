#!/bin/bash
# Builds the HD world texture pack from scratch (regenerable, never committed; R-L2):
#   tools/hdworld/make_pack.sh [build_dir] [out_dir]  (default out_dir out/hd/pack/hd_world)
# 1. tools/hdui/dump.sh (the UI census: its ui2d.tsv keeps UI textures out of this pack),
# 2. tools/hdworld/dump.sh: probe-mode coverage runs over Hel01, Pat01, Min02 (cached),
# 3. tools/hdworld/build_world.py -> out_dir. The .app does the same at install (tools/install.d/30-hd.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$ROOT/out}"
OUT="${2:-${HD_CACHE:-$ROOT/out/hd}/pack/hd_world}"
bash "$ROOT/tools/hdui/dump.sh" "$BUILD"
bash "$ROOT/tools/hdworld/dump.sh" "$BUILD"
python3 -I "$ROOT/tools/hdworld/build_world.py" "${HD_CACHE:-$ROOT/out/hd}/world" --out "$OUT" \
  --ui "${HD_CACHE:-$ROOT/out/hd}/census/ui2d.tsv"
