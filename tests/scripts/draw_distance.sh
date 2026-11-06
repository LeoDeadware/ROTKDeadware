#!/bin/bash
# Addendum d (2026-11-06): ROTK_DRAW_DISTANCE raises the far plane while the view is set up.
# Hel01 prologue (no movies), frame 500, twice: x1 (the game's) and x2:
#   1. log "polish: draw distance: far A -> 2A" in the x2 run, none in the x1 run;
#   2. both exit 0 without fatal lines; the x2 frame is not blank (shot_check.py).
source "$(dirname "$0")/../lib.sh"
run_game draw_distance_1 ROTK_SKIP_MOVIES=1 ROTK_PAD_SCRIPT="$TESTS_DIR/scripts/gamepad_prompts.pad" ROTK_SHOT_FRAMES=500 ROTK_EXIT_AFTER=510
assert_exit0; assert_no_fatal
SEMANTIC=$((SEMANTIC + 1))
grep -q "polish: draw distance" "$LOG" && fail "draw distance changed without ROTK_DRAW_DISTANCE"
run_game draw_distance_2 ROTK_SKIP_MOVIES=1 ROTK_PAD_SCRIPT="$TESTS_DIR/scripts/gamepad_prompts.pad" ROTK_DRAW_DISTANCE=2 ROTK_SHOT_FRAMES=500 ROTK_EXIT_AFTER=510
assert_exit0; assert_no_fatal
assert_log "polish: draw distance: far [0-9]+ -> [0-9]+" "far plane not raised"
b="$(shot 500)"
SEMANTIC=$((SEMANTIC + 1))
python3 -I "$TESTS_DIR/scripts/shot_check.py" "$b" >/dev/null || fail "x2 frame blank ($b)"
finish
