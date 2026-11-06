#!/bin/bash
# World texture census for the HD world pack (regenerable, never committed; R-C7):
#   tools/hdworld/dump.sh [build_dir] [out_dir] [Level...]   (default out, out/hd/world, Hel01 Pat01 Pel02 Osg01)
# One silent run per level (test profile, nice 19, one at a time): Hel01 from New Game (movies
# skipped, tools/hdui/tour.pad up to the pause), the others by the scene-select route of
# tests/scripts/fps_check.sh (ROTK_CALL_AT at 1600) and a walk/fight pad. Each run is a probe
# run (ROTK_HDUI_PROBE=1: every texture a flat colour naming it; ROTK_DEV_SHOW=t0 + ROTK_DEV_NOFOG:
# unlit, PIT-H01) with a shot every 150 frames, and dumps the original uploads (ROTK_HDUI_DUMP).
# Then tools/hdworld/coverage.py ranks the textures by their share of the screen.
# Output: out_dir/NN_<level>/ (runtime.log, shots), out_dir/dump, out_dir/coverage.tsv.
# Reused when out_dir/coverage.tsv exists (HD_REDUMP=1 redoes it). ~12 min per level.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$ROOT/out}"
OUT="${2:-${HD_CACHE:-$ROOT/out/hd}/world}"
shift 2 2>/dev/null || shift $#
LEVELS=("$@"); (( ${#LEVELS[@]} )) || LEVELS=(Hel01 Pat01 Pel02 Osg01)
BIN="$BUILD/src/loader/rotk"
DATA="${ROTK_DATA:-$ROOT}"
[[ -x "$BIN" ]] || { echo "hdworld/dump.sh: no $BIN"; exit 2; }
# Per level: a level whose dir has its "from" stamp is reused (the census grew from 4 to 17
# levels, 2026-11-06: a whole-census reuse kept the 13 new ones out). HD_REDUMP=1 redoes all.
[[ "${HD_REDUMP:-0}" == 1 ]] && rm -rf "$OUT"
mkdir -p "$OUT/dump"
n=0
for L in "${LEVELS[@]}"; do
  case "$(echo "$L" | tr A-Z a-z)" in
    hel01) IDX=0  CH=3 ;;  pat01) IDX=1  CH=0 ;;  pat03) IDX=2  CH=0 ;;  min02) IDX=3  CH=0 ;;
    pel02) IDX=4  CH=0 ;;  bla01) IDX=5  CH=0 ;;  ise01) IDX=6  CH=3 ;;  min00) IDX=7  CH=3 ;;
    min04) IDX=8  CH=3 ;;  osg01) IDX=9  CH=4 ;;  she01) IDX=10 CH=4 ;;  cir00) IDX=11 CH=4 ;;
    cra01) IDX=12 CH=4 ;;  cir01) IDX=14 CH=4 ;;  pat02) IDX=15 CH=0 ;;  pal01) IDX=16 CH=0 ;;
    pal02) IDX=17 CH=0 ;;
    *) echo "hdworld/dump.sh: unknown level $L"; continue ;;
  esac
  d="$OUT/$(printf %02d $n)_$L"; n=$((n + 1))
  if [[ -f "$d/from" ]]; then echo "hdworld/dump.sh: $L: reused"; continue; fi
  rm -rf "$d"; mkdir -p "$d/home"
  route=() pad="$ROOT/tools/hdui/tour.pad" start=300 end=1000
  if (( IDX )); then
    start=2800 end=4000
    route=(ROTK_CALL_AT="1600:poke:16D584:$CH,1600:9D770/$(printf %X $IDX),1600:9C8A0/0")
    pad="$d/pad.txt"
    for ((f = start - 200; f < end; f += 300)); do
      echo "$f stick L 0 1 120"; echo "$((f + 60)) press X"; echo "$((f + 100)) press Y"
      echo "$((f + 140)) stick L 1 0.4 80"; echo "$((f + 230)) stick R 1 0 40"
    done | sort -n -s -k1,1 > "$pad"
  fi
  (
    ulimit -c 0
    export HOME="$d/home" ROTK_LOG_DIR="$d" SDL_AUDIO_DRIVER=dummy SDL_VIDEO_DRIVER=dummy ROTK_BACKGROUND=1 \
           ROTK_TEST_PROFILE=1 ROTK_DATA="$DATA" ROTK_HDUI=0 ROTK_HDWORLD=0 ROTK_SKIP_MOVIES=1 \
           ROTK_PAD_SCRIPT="$pad" ROTK_EXIT_AFTER=$end ROTK_SHOT_EVERY=150 \
           ROTK_HDUI_PROBE=1 ROTK_DEV_MAGENTA=0-9999999 ROTK_DEV_SHOW=t0 ROTK_DEV_NOFOG=1 \
           ROTK_HDUI_DUMP="$OUT/dump" "${route[@]+"${route[@]}"}"
    exec nice -n 19 "$BIN" > "$d/stdout.log" 2>&1
  )
  echo "hdworld/dump.sh: $L: exit $?, $(ls "$d"/frame_*.png 2>/dev/null | wc -l | tr -d ' ') shots"
  # coverage counts shots from the level's first gameplay frame on
  echo "$start" > "$d/from"
done
python3 -I "$ROOT/tools/hdworld/coverage.py" "$OUT" --out "$OUT/coverage.tsv"
