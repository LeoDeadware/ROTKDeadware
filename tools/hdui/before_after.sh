#!/bin/bash
# Before/after contact sheet of the HD packs (the owner's taste call):
#   tools/hdui/before_after.sh MODS_DIR [build_dir] [out_dir]   (default out/hd/reports/hd-p5c)
# Two silent runs at ROTK_RENDER_SCALE=4 over tools/hdui/tour.pad (Hel01 fight 600, pause 1100):
# packs off (ROTK_HDUI=0 ROTK_HDWORLD=0) and on (ROTK_MODS=MODS_DIR), then crops of the HUD,
# the pause menu text and the world, before above after. Packs only swap textures: layout,
# proportions and anchors are the game's own at every resolution.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MODS=$1 BUILD="${2:-$ROOT/out}" OUT="${3:-${HD_CACHE:-$ROOT/out/hd}/reports/hd-p5c}"
BIN="$BUILD/src/loader/rotk" DATA="${ROTK_DATA:-$ROOT}"
mkdir -p "$OUT"
run() {  # name env...
  local d="$OUT/$1"; shift
  rm -rf "$d"; mkdir -p "$d/home"
  (
    ulimit -c 0
    export HOME="$d/home" ROTK_LOG_DIR="$d" SDL_AUDIO_DRIVER=dummy SDL_VIDEO_DRIVER=dummy ROTK_BACKGROUND=1 \
           ROTK_TEST_PROFILE=0 ROTK_DATA="$DATA" ROTK_SKIP_MOVIES=1 ROTK_RENDER_SCALE=4 \
           ROTK_PAD_SCRIPT="$ROOT/tools/hdui/tour.pad" ROTK_SHOT_FRAMES=600,1100 ROTK_EXIT_AFTER=1102 "$@"
    exec nice -n 19 "$BIN" > "$d/stdout.log" 2>&1
  )
}
run off ROTK_HDUI=0 ROTK_HDWORLD=0 ROTK_MODS=/nonexistent
run on ROTK_MODS="$MODS"
C="$(cd "$ROOT/tools/hdui" && python3 -I -c 'import sys; sys.path.insert(0, "."); import hdlib; print(hdlib.tool("contact"))')"
sheet() {  # name frame crop
  "$C" "$OUT/$1.png" --cols 2 --width 900 --crop "$3" \
    "$OUT/off/frame_00$2.png=original" "$OUT/on/frame_00$2.png=HD_packs"
}
sheet hud 0600 0,0,0.5,0.35
sheet pause_text 1100 0.25,0.25,0.75,0.6
sheet world 0600 0.2,0.35,0.8,0.95
"$C" "$OUT/contact.png" --cols 2 --width 900 "$OUT/off/frame_001100.png=original_pause" \
  "$OUT/on/frame_001100.png=HD_pause" "$OUT/off/frame_000600.png=original_fight" "$OUT/on/frame_000600.png=HD_fight"
echo "before_after: $OUT/{contact,hud,pause_text,world}.png; replaced: $(grep -c 'polish: hd_' "$OUT/on/runtime.log") lines"
