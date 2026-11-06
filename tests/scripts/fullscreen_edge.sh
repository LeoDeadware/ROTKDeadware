#!/bin/bash
# Owner bug 2026-11-06 ("картинка не на весь екран, є чорні полоси"): on the notched 3024x1964
# panel the Spaces fullscreen window stopped below the camera housing, and the frame (shaped for the
# whole display) was fitted into it with bars on all sides. The fullscreen window must cover the
# whole screen: ROTK_DEV_FULLSCREEN_PROBE=1 logs the geometry without opening a window; the
# fullscreen frame must equal the screen, and the widescreen frame must have the screen's aspect.
source "$(dirname "$0")/../lib.sh"
run_game fullscreen_edge ROTK_DEV_FULLSCREEN_PROBE=1 ROTK_TEST_PROFILE=0 ROTK_WIDESCREEN=native ROTK_EXIT_AFTER=60
assert_exit0; assert_no_fatal
SEMANTIC=$((SEMANTIC + 1))
l="$(grep -m1 "window: fullscreen probe" "$LOG")"
[[ -n "$l" ]] || { fail "no fullscreen probe line"; finish; }
read -r sw sh fw fh < <(echo "$l" | sed -E 's/.*screen ([0-9]+)x([0-9]+) px.*fullscreen frame ([0-9]+)x([0-9]+) px.*/\1 \2 \3 \4/')
[[ "$sw $sh" == "$fw $fh" ]] || fail "fullscreen frame ${fw}x${fh} is not the whole screen ${sw}x${sh}"
grep -q "polish: widescreen native ${sw}x${sh}" "$LOG" || fail "widescreen frame not shaped for the screen ${sw}x${sh}"
finish
