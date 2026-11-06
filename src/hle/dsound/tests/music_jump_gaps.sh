#!/bin/bash
RROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"   # the repo
# music_jump_gaps.sh <build_dir> [frames] [env...]: paced silent boot to the
# main menu, idling there (the frontend map jumps sections every ~20 s; 4800
# frames = 3 jumps), mix dumped to WAV; music_jump_gaps.py measures the hole
# at each jump, FAIL above 5 ms. Control: ROTK_DS_NO_JUMP_FIX=1 (~50 ms holes).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="$1" FRAMES="${2:-4800}"; shift; [[ $# -gt 0 ]] && shift
BIN="$BUILD/src/loader/rotk"
[[ -x "$BIN" ]] || { echo "FAIL: no $BIN"; exit 2; }
RUN="${ROTK_CACHE:-$HOME/Desktop/Claude/ROTKPort/.cache}/runs/music_jump_gaps-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$RUN/home"
(
  ulimit -c 0
  export HOME="$RUN/home" ROTK_LOG_DIR="$RUN" ROTK_KTRACE=0 SDL_AUDIO_DRIVER=dummy \
         SDL_VIDEO_DRIVER=dummy ROTK_BACKGROUND=1 ROTK_SKIP_MOVIES=1 \
         ROTK_DATA="${ROTK_DATA:-$RROOT}" \
         ROTK_DS_TRACE_CLOSE=1 ROTK_AUDIO_WAV="$RUN/mix.wav" ROTK_EXIT_AFTER="$FRAMES"
  exec env "$@" "$BIN" > "$RUN/stdout.log" 2> "$RUN/stderr.log"
)
echo "exit $?, run dir $RUN"
python3 "$HERE/music_jump_gaps.py" "$RUN/mix.wav" "$RUN/stderr.log" 5
