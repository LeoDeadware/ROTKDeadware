#!/bin/bash
# Test harness for game runs (RULES R-T*). Source it from tests/scripts/*.sh:
#
#   source "$(dirname "$0")/../lib.sh"
#   run_game first_level ROTK_PAD_SCRIPT=tests/scripts/x.pad ROTK_SHOT_FRAMES=6000,6320 ROTK_EXIT_AFTER=6400
#   assert_exit0; assert_no_fatal
#   assert_frames_differ 6000 6320          # input reached the game (PIT-Q01)
#   assert_golden 6320 first_level.png      # pixels, only with a semantic assert above
#   finish                                  # writes result.json, exit 0/1
#
# What run_game guarantees (each line is an HP2 failure):
#   - run dir ROTKPort/.cache/runs/<name>-<stamp>-<pid> (never shared between runs);
#   - silent: SDL dummy audio/video, ROTK_BACKGROUND=1, scratch HOME (RUN_HOME=dir shares one
#     between runs, e.g. save round trip), ulimit -c 0;
#   - ROTK_TEST_PROFILE=1: every feature that changes timing or look (skip, HD packs,
#     look presets, modern camera) is off unless the test turns it on explicitly;
#   - at most 2 game runs at once (tools/lock.py runs);
#   - watchdog (RUN_TIMEOUT s, default 300) -> kill -9 + FAIL;
#   - result.json: verdict, exit code, failures, run dir, busy stats if the log has them.
# FLAKY=1 (default): a failed test is re-run once; PASS on the retry -> "PASS(flaky)" with
# the first failure kept in result.json. Two failures = FAIL.
# UPDATE_GOLDEN=1: golden candidates go to tests/golden/_pending/ + index.html; only
# tests/golden_accept.sh moves them in, after someone LOOKED (commit says what was seen).

set -uo pipefail
TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$TESTS_DIR/.." && pwd)"
BUILD="${BUILD:-$ROOT/out}"
BIN="${BIN:-$BUILD/src/loader/rotk}"
GOLDEN="$ROOT/tests/golden"
DATA="${ROTK_DATA:-$ROOT}"
FAILS=()
SEMANTIC=0
RUN=""
TEST_NAME="${TEST_NAME:-$(basename "${0%.sh}")}"
SCRIPT_ARGS=("$@")   # lib.sh is sourced: these are the test script's arguments

fail() { echo "  FAIL: $*"; FAILS+=("$*"); }
note() { echo "  note: $*"; }

run_game() {  # name [VAR=value ...]
  local name="$1"; shift
  [[ -x "$BIN" ]] || { fail "no $BIN (cmake --build $BUILD)"; return 2; }
  RUN="${ROTK_CACHE:-$HOME/Desktop/Claude/ROTKPort/.cache}/runs/${name}-$(date +%Y%m%d-%H%M%S)-$$-$RANDOM"
  local home="${RUN_HOME:-$RUN/home}" REAL_HOME="$HOME"   # RUN_HOME: share a profile (saves) between runs
  mkdir -p "$RUN" "$home"
  local timeout="${RUN_TIMEOUT:-300}"
  (
    ulimit -c 0
    export HOME="$home" ROTK_LOG_DIR="$RUN" SDL_AUDIO_DRIVER=dummy SDL_VIDEO_DRIVER=dummy \
           ROTK_BACKGROUND=1 ROTK_TEST_PROFILE=1 ROTK_DATA="$DATA"
    for kv in "$@"; do export "$kv"; done
    # lock.py keys its lock dir on ~ : it must see the real HOME, only the game the scratch one
    HOME="$REAL_HOME" exec python3 -I "$ROOT/tools/lock.py" runs -- env HOME="$home" "$BIN" \
      > "$RUN/stdout.log" 2> "$RUN/stderr.log"
  ) &
  local pid=$! mon=""
  echo "$pid" > "$RUN/run.pid"
  # RUN_MONITOR="cmd args": started as `cmd args <run dir> <pid>` beside the game (soak sampler).
  if [[ -n "${RUN_MONITOR:-}" ]]; then $RUN_MONITOR "$RUN" "$pid" & mon=$!; fi
  # The timeout counts from the game's start (runtime.log), not the wait for a lock.py run slot;
  # the wait itself is capped at 3 h.
  local t=0 w=0
  while kill -0 $pid 2>/dev/null && (( t < timeout * 2 && w < 21600 )); do
    sleep 0.5; [[ -e "$RUN/runtime.log" ]] && t=$((t + 1)) || w=$((w + 1))
  done
  if kill -0 $pid 2>/dev/null; then
    pkill -9 -P $pid 2>/dev/null; kill -9 $pid 2>/dev/null
    fail "timeout after ${timeout} s"
  fi
  wait $pid; RUN_EXIT=$?
  [[ -n $mon ]] && wait $mon
  rm -f "$RUN/run.pid"
  LOG="$RUN/runtime.log"   # runtime + d3d8 lines; kernel/xinput/HLE lines ("[krnl] ...") only reach stderr.log
  LOGS=("$LOG" "$RUN/stderr.log")
  echo "$name: exit $RUN_EXIT, run dir $RUN"
}

shot() { echo "$RUN/frame_$(printf %06d "$1").png"; }   # frame -> screenshot path (d3d8 ROTK_SHOT_*)

assert_exit0()   { SEMANTIC=$((SEMANTIC + 1)); [[ "${RUN_EXIT:-1}" == 0 ]] || fail "exit code ${RUN_EXIT:-?}"; }
assert_no_fatal() {
  SEMANTIC=$((SEMANTIC + 1))
  if grep -h -m1 -E "^(\[krnl\] )?FATAL|dispatch miss" "${LOGS[@]}" 2>/dev/null; then fail "FATAL / dispatch miss in log"; fi
}
assert_log() {  # regex [description]
  SEMANTIC=$((SEMANTIC + 1))
  grep -qE "$1" "${LOGS[@]}" 2>/dev/null || fail "${2:-log has no /$1/}"
}
assert_frames_differ() {  # a b: the game reacted between frames a and b (PIT-Q01)
  SEMANTIC=$((SEMANTIC + 1))
  local a b; a="$(shot "$1")"; b="$(shot "$2")"
  [[ -f "$a" && -f "$b" ]] || { fail "missing shots $1/$2"; return; }
  if python3 -I "$TESTS_DIR/scripts/png_diff.py" "$a" "$b" 0.5 0.001 >/dev/null; then
    fail "frames $1 and $2 are identical: input did not reach the game (overlay? pause?)"
  fi
}
assert_any_frames_differ() {  # a1 b1 a2 b2 ...: at least one pair differs (restart-proof)
  SEMANTIC=$((SEMANTIC + 1))
  local why=()
  while (( $# >= 2 )); do
    local a b; a="$(shot "$1")"; b="$(shot "$2")"
    if [[ ! -f "$a" || ! -f "$b" ]]; then why+=("missing shots $1/$2")
    elif python3 -I "$TESTS_DIR/scripts/png_diff.py" "$a" "$b" 0.5 0.001 >/dev/null; then why+=("$1=$2")
    else return 0; fi
    shift 2
  done
  fail "no frame pair differs (${why[*]}): input did not reach the game (overlay? pause?)"
}
assert_check() {  # script.py shot-frame [args]: any per-title semantic checker (overlay, level geo)
  SEMANTIC=$((SEMANTIC + 1))
  local py="$1" f="$2"; shift 2
  python3 -I "$py" "$(shot "$f")" "$@" || fail "$(basename "$py") on frame $f"
}
assert_golden() {  # frame golden.png [max_mean] [max_bad]
  local s; s="$(shot "$1")"
  if (( SEMANTIC == 0 )); then fail "golden $2 without a semantic assert first (R-T5)"; return; fi
  [[ -f "$s" ]] || { fail "no shot for frame $1"; return; }
  if [[ "${UPDATE_GOLDEN:-0}" == 1 ]]; then
    mkdir -p "$GOLDEN/_pending"; cp "$s" "$GOLDEN/_pending/$2"
    touch "$RUN/keep"; note "golden candidate -> tests/golden/_pending/$2 (review, then tests/golden_accept.sh)"
    return
  fi
  [[ -f "$GOLDEN/$2" ]] || { fail "no golden $2 (UPDATE_GOLDEN=1 to propose one)"; return; }
  python3 -I "$TESTS_DIR/scripts/png_diff.py" "$s" "$GOLDEN/$2" "${3:-2.0}" "${4:-0.01}" || fail "frame $1 != $2"
}

busy_stats() {  # prints "avg p95 max" ms from "busy <ms>" log lines, if any
  grep -oE "busy [0-9.]+" "$LOG" 2>/dev/null | awk '{print $2}' | sort -n | awk '
    {v[NR]=$1; s+=$1} END {if (NR) {i=int(NR*0.95+0.999); printf "%.2f %.2f %.2f", s/NR, v[i], v[NR]}}'
}

finish() {
  local verdict="PASS"
  (( ${#FAILS[@]} )) && verdict="FAIL"
  if [[ "$verdict" == FAIL && "${FLAKY:-1}" == 1 && -z "${PORTKIT_RETRY:-}" ]]; then
    echo "  retrying once (FLAKY=1)…"
    local first; first="$(printf '%s; ' "${FAILS[@]}")"
    if PORTKIT_RETRY=1 PORTKIT_FIRST_FAIL="$first" bash "$0" "${SCRIPT_ARGS[@]+"${SCRIPT_ARGS[@]}"}"; then exit 0; fi
    exit 1
  fi
  [[ "$verdict" == PASS && -n "${PORTKIT_FIRST_FAIL:-}" ]] && verdict="PASS(flaky)"
  local fails_json; fails_json="$(printf '%s\n' "${FAILS[@]+"${FAILS[@]}"}" | python3 -c 'import json,sys; print(json.dumps([l for l in sys.stdin.read().splitlines() if l]))')"
  [[ -n "$RUN" ]] && python3 - "$RUN/result.json" "$TEST_NAME" "$verdict" "${RUN_EXIT:-null}" "$fails_json" \
      "${PORTKIT_FIRST_FAIL:-}" "$(busy_stats)" <<'EOF'
import json, sys
p, name, verdict, code, fails, first, busy = sys.argv[1:8]
json.dump({"test": name, "verdict": verdict, "exit": None if code == "null" else int(code),
           "failures": json.loads(fails), "first_failure": first or None,
           "busy_ms": dict(zip(("avg", "p95", "max"), map(float, busy.split()))) if busy else None},
          open(p, "w"), indent=1)
EOF
  echo "$TEST_NAME: $verdict"
  [[ "$verdict" == FAIL ]] && exit 1
  exit 0
}
