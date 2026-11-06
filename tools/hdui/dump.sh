#!/bin/bash
# Texture census for the HD packs (regenerable, never committed; R-C7):
#   tools/hdui/dump.sh [build_dir] [out_dir]        (default out, out/hd/census)
# Silent game runs (test profile, original textures, ROTK_HDUI=0, nice 19, one at a time):
#   UI scenes over tools/hdui/tour.pad (Hel01 prologue with movies skipped): for each scene frame F
#   the D3D8 HLE logs every draw of F (ROTK_DEV_LOG_DRAWS=F: depth test, stage-0 texture data
#   address) and every texture bind of F-31..F with its content hash (ROTK_DEV_LOG_TEX; each
#   texture is re-hashed at least every 30 frames) -> the hashes drawn in 2D (depth test off or
#   ZFUNC ALWAYS) = UI textures (tools/hdui/ui2d.py);
#   world levels (tools/hdworld/dump.sh) add the level textures.
# Every run dumps the uploads (ROTK_HDUI_DUMP: index.tsv + PNGs of DXT / swizzled formats) and the
# D3D8 HLE's converted level 0 (ROTK_TEX_DUMP: palettized fonts, P8, with their palette applied).
# Output: out_dir/dump (index.tsv, <hash>.png), out_dir/texdump, out_dir/ui2d.tsv
# ("scene frame hash w h box"), out_dir/scenes/<scene>/frame_*.png. Reused when out_dir/ui2d.tsv
# exists per scene (HD_REDUMP=1 redoes it). ~15 min, + ~25 for the ua_* and hero_* scenes.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$ROOT/out}"
OUT="${2:-${HD_CACHE:-$ROOT/out/hd}/census}"
BIN="$BUILD/src/loader/rotk"
DATA="${ROTK_DATA:-$ROOT}"
[[ -x "$BIN" ]] || { echo "dump.sh: no $BIN"; exit 2; }
# Ukrainian layer (HD_UA_MODS, the install's Resources/mods/uk): its GlobScen fonts have their own
# hashes (Cyrillic glyphs), so the ua_* scenes rerun whenever that file changes (stamp per scene).
UA="${HD_UA_MODS:-}"; UA_KEY=none
[[ -n "$UA" && -f "$UA/Data/Game/GlobScen.scx" ]] && UA_KEY=$(shasum "$UA/Data/Game/GlobScen.scx" | cut -c1-16)
# A scene is reused when its dir has a done stamp with its key (HD_REDUMP=1 redoes all);
# HD_ONLY="scene ..." reruns only those scenes into an existing census.
[[ "${HD_REDUMP:-0}" == 1 ]] && rm -rf "$OUT"; mkdir -p "$OUT/dump" "$OUT/texdump" "$OUT/scenes"
# scene frame [pad]: HUD in the fight with the "A Speed" prompt, the pause menu, the upgrades
# page, the mission-tips panel (tools/hdui/tour.pad, Hel01); the front end (Fro03 by the
# scene-select route of tests/scripts/fps_check.sh at 1600, tools/hdui/menu.pad): its pause
# menu, the "are you sure?" dialog, the level select with the button-glyph help line.
# Owner 2026-11-06 (680a23b: Ukrainian subtitles, hints, menus and the A/X glyphs inside text
# stayed low-res; "іконки персонажів в низькому розширенні"): the same scenes on the Ukrainian
# layer (ua_*, + the intro cinematic's subtitles, movies on), and the HUD of each playable hero
# by the scene-select route (L:<level index>:<hero>, a walk/fight pad) for the portraits.
SCENES=("hud 300" "fight 600" "pause 1100" "upgrades 1300" "tips 1700"
        "fpause 2800 menu" "confirm 2900 menu" "levels 4150 menu"
        "ua_hud 300" "ua_fight 600" "ua_pause 1100" "ua_upgrades 1300" "ua_tips 1700"
        "ua_fpause 2800 menu" "ua_confirm 2900 menu" "ua_levels 4150 menu" "ua_subs 2950 start"
        "hero_aragorn 3200 L:1:0" "hero_legolas 3200 L:1:1" "hero_gimli 3200 L:1:2"
        "hero_sam 3200 L:10:4" "hero_frodo 3200 L:12:5" "hero_gandalf 3200 L:6:3")
FRONT="1600:poke:16D584:0,1600:9D770/D,1600:9C8A0/0"
run() {  # name frame [pad | menu | start | L:idx:hero]
  local d="$OUT/scenes/$1" f=$2 pad="$ROOT/tools/hdui/${3:-tour}.pad" route=() extra=(ROTK_SKIP_MOVIES=1) key=1
  if [[ "$1" == ua_* ]]; then
    [[ "$UA_KEY" == none ]] && { echo "dump.sh: $1 skipped (no HD_UA_MODS)"; return; }
    key=$UA_KEY extra+=(ROTK_MODS="$UA" ROTK_LANG=uk)
  fi
  if [[ -z "${HD_ONLY:-}" && -f "$d/done" && "$(cat "$d/done")" == "$key" ]]; then return; fi
  rm -rf "$d"; mkdir -p "$d/home"; echo "$f" > "$d/frame"
  case "${3:-}" in
    menu) route=(ROTK_CALL_AT="$FRONT") ;;
    start) pad="$d/pad.txt"; echo "1100 press START" > "$pad"; extra=("${extra[@]/ROTK_SKIP_MOVIES=1/ROTK_SKIP_MOVIES=0}") ;;
    L:*) local idx ch; IFS=: read -r _ idx ch <<< "$3"
       route=(ROTK_CALL_AT="1600:poke:16D584:$ch,1600:9D770/$(printf %X "$idx"),1600:9C8A0/0")
       pad="$d/pad.txt"
       for ((g = 2600; g < f; g += 300)); do
         echo "$g stick L 0 1 120"; echo "$((g + 60)) press X"; echo "$((g + 140)) stick L 1 0.4 80"
       done | sort -n -s -k1,1 > "$pad" ;;
  esac
  (
    ulimit -c 0
    export HOME="$d/home" ROTK_LOG_DIR="$d" SDL_AUDIO_DRIVER=dummy SDL_VIDEO_DRIVER=dummy ROTK_BACKGROUND=1 \
           ROTK_TEST_PROFILE=1 ROTK_DATA="$DATA" ROTK_HDUI=0 ROTK_HDWORLD=0 \
           ROTK_PAD_SCRIPT="$pad" ROTK_EXIT_AFTER=$((f + 2)) ROTK_SHOT_FRAMES=$f \
           ROTK_DEV_LOG_DRAWS=$f ROTK_DEV_LOG_TEX=$((f - 31))-$f \
           ROTK_HDUI_DUMP="$OUT/dump" ROTK_TEX_DUMP="$OUT/texdump" "${extra[@]}" "${route[@]+"${route[@]}"}"
    exec nice -n 19 "$BIN" > "$d/stdout.log" 2>&1
  )
  local rc=$?
  echo "dump.sh: $1 (frame $f): exit $rc, $(grep -cE " z (0/|[0-9]+/[0-9]+ 207)" "$d/runtime.log" 2>/dev/null) 2D draws"
  (( rc == 0 )) && echo "$key" > "$d/done"
}
# a census from before the stamps: its scenes count as done
for d in "$OUT"/scenes/*/; do [[ -f "$d/runtime.log" && ! -f "$d/done" && "$d" != */ua_*/ ]] && echo 1 > "$d/done"; done
for s in "${SCENES[@]}"; do
  [[ -z "${HD_ONLY:-}" || " $HD_ONLY " == *" ${s%% *} "* ]] && run $s
done
python3 -I "$ROOT/tools/hdui/ui2d.py" "$OUT" > "$OUT/ui2d.tsv"
echo "dump.sh: $(cut -f3 "$OUT/ui2d.tsv" | sort -u | wc -l | tr -d ' ') UI textures drawn in 2D, $(wc -l < "$OUT/dump/index.tsv" | tr -d ' ') uploads -> $OUT"
