#!/bin/bash
# Mix levels of the intro cinematic + the prologue (title_to_level.pad), by numbers:
# the mixer's 10 s windows (peak before the limiter, rms, limited samples, voices) and the
# whole-run line. Runs through tests/lib.sh (silent, paced, own run dir)
#
#   src/hle/dsound/tests/levels.sh [frames=9000] [VAR=value ...]
#   e.g. levels.sh 9000 ROTK_AUDIO_NO_EG=1      (control: no amplitude envelopes)
# rotk_exit leaves with _Exit (no atexit), so the last "mixer running" line is
# the whole-run line.
set -uo pipefail
source "$(cd "$(dirname "$0")/../../../.." && pwd)/tests/lib.sh"
FRAMES="${1:-9000}"
shift || true
RUN_TIMEOUT=$((FRAMES / 20 + 120)) run_game audio_levels ROTK_PAD_SCRIPT="$ROOT/tests/scripts/title_to_level.pad" \
  ROTK_EXIT_AFTER="$FRAMES" "$@"
assert_exit0
assert_no_fatal
assert_log "dsound: mixer running" "no mixer stats"
touch "$RUN/keep"
grep -E "dsound: mixer window" "$LOG" | sed 's/^.*dsound: //'
grep -E "dsound: mixer running" "$LOG" | tail -1 | sed 's/^.*dsound: //'
finish
