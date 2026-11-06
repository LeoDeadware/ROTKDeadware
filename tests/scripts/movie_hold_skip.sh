#!/bin/bash
# Holding A ~1 s skips a movie, with a ring drawn while A is held (owner bug 2026-11-06).
# Title -> intro cinematic (~1100..6300 at the test pace), A held from 1400 (movie_hold_skip.pad),
# ROTK_SKIP_HOLD=1 (off by default under the test profile). Wants:
#   - "movies: A held" once (the same hold never skips the next movie too), then "movies: skipped by holding A at frame N" with N <= 1460;
#   - a shot during the hold with the gold ring in the bottom-right corner (ring_pixels.py);
#   - the game moved on: frame 1800 has > 20 draws (the movie frames are 1 draw);
#   - control: no ROTK_SKIP_HOLD under the test profile -> no ring line, the movie still plays.
source "$(dirname "$0")/../lib.sh"
PAD="$TESTS_DIR/scripts/movie_hold_skip.pad"
RUN_TIMEOUT=900 run_game movie_hold_skip ROTK_SKIP_HOLD=1 ROTK_PAD_SCRIPT="$PAD" \
  ROTK_SHOT_FRAMES="1395,1410,1420" ROTK_EXIT_AFTER=1810
assert_exit0; assert_no_fatal
SEMANTIC=$((SEMANTIC + 1))
assert_log "movies: A held at frame" "no hold detected during the movie"
(( $(grep -c "movies: A held at frame" "$LOG") == 1 )) || fail "one hold skipped more than one movie (A must be released first)"
n="$(grep -m1 -o 'skipped by holding A at frame [0-9]*' "$LOG" | grep -o '[0-9]*$')"
[[ -n "$n" && "$n" -le 1460 ]] || fail "movie not skipped by the hold (frame '${n}')"
rings=0
for f in 1410 1420; do
  out="$(python3 -I "$TESTS_DIR/scripts/ring_pixels.py" "$(shot $f)")" && rings=$((rings + 1)); echo "  frame $f: $out"
done
(( rings >= 1 )) || fail "no ring in the shots during the hold"
python3 -I "$TESTS_DIR/scripts/ring_pixels.py" "$(shot 1395)" >/dev/null && fail "ring before the hold"
d="$(grep -o 'frame 1800: .*; [0-9]* draws' "$LOG" | grep -o '[0-9]* draws' | grep -o '[0-9]*')"
[[ -n "$d" && "$d" -gt 20 ]] || fail "frame 1800 has '${d}' draws: the movie still plays"
RUN_TIMEOUT=900 run_game movie_hold_off ROTK_PAD_SCRIPT="$PAD" ROTK_EXIT_AFTER=1500
assert_exit0
grep -q "movies: A held\|skipped by holding" "$LOG" && fail "hold skip on under the test profile"
finish
