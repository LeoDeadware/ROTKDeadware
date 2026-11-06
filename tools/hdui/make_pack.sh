#!/bin/bash
# Builds the HD UI texture pack from scratch (regenerable, never committed; R-L2):
#   tools/hdui/make_pack.sh [build_dir] [out_dir]     (default out_dir out/hd/pack/hd_ui)
# 1. tools/hdui/dump.sh: the texture census (silent runs, ~6 min; reused when cached),
# 2. tools/hdui/build_pack.py -> out_dir. The .app does the same at install (tools/install.d/30-hd.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$ROOT/out}"
OUT="${2:-${HD_CACHE:-$ROOT/out/hd}/pack/hd_ui}"
bash "$ROOT/tools/hdui/dump.sh" "$BUILD"
python3 -I "$ROOT/tools/hdui/build_pack.py" "${HD_CACHE:-$ROOT/out/hd}/census" --out "$OUT"
