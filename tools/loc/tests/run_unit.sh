#!/bin/bash
# Runs one tools/loc unit test with a run dir for evidence (ROTKPort/.cache/runs/loc_<name>-<stamp>):
#   tools/loc/tests/run_unit.sh <name>      (tools/loc/tests/<name>.py)
# The log is <run dir>/test.log; the last line says PASS or FAIL.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
name="$1"; shift
RUN="${ROTK_CACHE:-$HOME/Desktop/Claude/ROTKPort/.cache}/runs/loc_${name}-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$RUN"
DATA="${ROTK_DATA:-$ROOT}"
RUN="$RUN" nice -n 19 python3 -I "$ROOT/tools/loc/tests/$name.py" "$DATA" "$@" > "$RUN/test.log" 2>&1
code=$?
grep -E "^FAIL" "$RUN/test.log" | head -5
tail -1 "$RUN/test.log"
echo "run dir $RUN"
echo "$name: $([[ $code == 0 ]] && echo PASS || echo FAIL)"
exit $code
