#!/bin/bash
# Smoke (or soak) of many levels, two game runs at a time (tools/lock.py runs throttles).
#   tests/scripts/level_sweep.sh [levels...]            default: all 17 playable levels
#   PLAY=60000 SOAK=1 tests/scripts/level_sweep.sh Pat01 Ise01
# Writes $SWEEP_OUT (default ROTKPort/.cache/runs/sweep-<stamp>.tsv): level, verdict, run dir,
# first failure. FLAKY=0 unless set: a failure is re-run by hand, not hidden by a retry.
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ALL="Hel01 Pat01 Pat03 Min02 Pel02 Bla01 Ise01 Min00 Min04 Osg01 She01 Cir00 Cra01 Cir01 Pat02 Pal01 Pal02"
LEVELS="${*:-$ALL}"
OUT="${SWEEP_OUT:-${ROTK_CACHE:-$HOME/Desktop/Claude/ROTKPort/.cache}/runs/sweep-$(date +%Y%m%d-%H%M%S)-$$.tsv}"
export FLAKY="${FLAKY:-0}" PLAY="${PLAY:-4500}"
echo -e "level\tverdict\trun\tfirst_failure" > "$OUT"
one() {
  local out verdict run
  out="$(bash "$ROOT/tests/scripts/level_smoke.sh" "$1" "$PLAY" 2>&1)"
  verdict="$(echo "$out" | grep -oE ': (PASS\(flaky\)|PASS|FAIL)$' | tail -1 | cut -c3-)"
  run="$(echo "$out" | grep -oE 'run dir [^ ]+' | tail -1 | cut -c9-)"
  echo "$out" > "${run:-/tmp}/sweep.out"
  printf '%s\t%s\t%s\t%s\n' "$1" "${verdict:-FAIL}" "$run" \
    "$(echo "$out" | grep -m1 -E 'FAIL:|first fault' | sed 's/^ *//' | cut -c1-200)" >> "$OUT"
  echo "$1 ${verdict:-FAIL} $run"
}
export -f one; export ROOT OUT
echo $LEVELS | tr ' ' '\n' | xargs -P "${SWEEP_JOBS:-2}" -I{} bash -c 'one {}'
echo "summary: $OUT"
column -t -s $'\t' "$OUT"
