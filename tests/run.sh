#!/bin/bash
# Runs the test suite: tests/suite.tsv lists "<mode>\t<script>" (mode: quick | full | soak).
#   tests/run.sh [quick|full|soak] [report.md]
# quick ⊂ full ⊂ soak. Writes a markdown report (PASS / PASS(flaky) / FAIL + busy) and exits 1
# on any FAIL. Game runs are throttled by tools/lock.py runs; unit tests run directly.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${1:-quick}"
REPORT="${2:-/dev/stdout}"
rank() { case "$1" in quick) echo 1;; full) echo 2;; soak) echo 3;; *) echo 9;; esac; }
want=$(rank "$MODE")
fails=0; rows=()
while IFS=$'\t' read -r mode script; do
  [[ -z "$mode" || "$mode" == \#* ]] && continue
  (( $(rank "$mode") <= want )) || continue
  read -ra cmd <<< "$script"   # a script may carry arguments (perf)
  out="$(bash "$ROOT/${cmd[0]}" "${cmd[@]:1}" 2>&1)"; code=$?
  verdict="$(echo "$out" | grep -oE ': (PASS\(flaky\)|PASS|FAIL)$' | tail -1 | cut -c3-)"
  [[ -z "$verdict" ]] && verdict=$([[ $code == 0 ]] && echo PASS || echo FAIL)
  [[ $code != 0 ]] && { verdict=FAIL; fails=$((fails + 1)); }
  rows+=("| $script | $verdict | $(echo "$out" | grep -m3 'FAIL:' | tr '\n' ' ' | cut -c1-200) |")
  echo "$verdict  $script" >&2
done < "$ROOT/tests/suite.tsv"
{
  echo "# Test report ($MODE, $(date '+%F %T'), $(git -C "$ROOT" rev-parse --short HEAD))"
  echo; echo "| test | verdict | first failures |"; echo "|---|---|---|"
  printf '%s\n' "${rows[@]+"${rows[@]}"}"
  echo; echo "FAIL: $fails"
} > "$REPORT"
(( fails == 0 ))
