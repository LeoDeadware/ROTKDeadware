#!/bin/bash
RROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"   # the repo
# loadloop.sh <n> <tag> [env...]: n paced load runs (Continue -> Potter A), 2 at a time.
SRC=${ROTK_LOADLOOP_PROFILE:?set ROTK_LOADLOOP_PROFILE to a save_roundtrip run home with POTTER A}
BIN=${ROTK_BIN:-${ROTK_CACHE:-$HOME/Desktop/Claude/ROTKPort/.cache}/out/audio-int/out/src/loader/rotk}
N=$1 TAG=$2; shift 2; EXTRA=("$@")
one() {
  local d=ROTKPort/.cache/runs/loadloop_$TAG-$1; rm -rf $d; mkdir -p $d; cp -c -R $SRC $d/home
  printf '0 disconnect\n1 connect\n450 press A\n600 press A\n' > $d/pad.txt
  ( ulimit -c 0; export HOME=$d/home ROTK_LOG_DIR=$d ROTK_KTRACE=0 SDL_AUDIO_DRIVER=dummy SDL_VIDEO_DRIVER=dummy \
     ROTK_BACKGROUND=1 ROTK_SKIP_MOVIES=1 ROTK_DATA="${ROTK_DATA:-$RROOT}" ROTK_PAD_SCRIPT=$d/pad.txt \
     ROTK_EXIT_AFTER=1500; unset ROTK_STUB ROTK_NO_PACING; exec env "${EXTRA[@]}" $BIN >$d/stdout.log 2>$d/stderr.log ) &
  local p=$!; for _ in $(seq 1 180); do kill -0 $p 2>/dev/null || break; sleep 0.5; done
  kill -9 $p 2>/dev/null; wait $p; echo "run $1: exit $? $(grep -m1 '^FATAL' $d/runtime.log)"
}
for ((i=1;i<=N;i+=2)); do one $i & one $((i+1)) & wait; done
