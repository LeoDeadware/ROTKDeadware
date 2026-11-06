#!/bin/bash
# Gamepad 5b: the on-screen button prompts show the Xbox Series controller (src/polish/texpack/prompts.mm).
# Hel01 prologue with tests/scripts/gamepad_prompts.pad, 1000 frames, texture dump, shots 300..990.
# The tutorial prompts depend on the fight ("LS Climb", "A Speed", "Y Fierce", "LT Ranged" in varying
# order), so the check takes whichever of the replaced glyphs the run shows:
#   1. at least one original Xbox glyph of a replaced button is in use (hd_ui dump index):
#      L stick a3b3a5d16faa0d86 ("Climb"), L trigger 404325db171c074c ("Ranged"), Black a4a43cfe5000c698
#      ("Action", always uploaded) / 8004f7870a39cffc;
#   2. each one in use was replaced: "polish: prompts: <hash> (Xbox L stick) -> LS" / "(Xbox L trigger)
#      -> LT" / "(Xbox Black) -> RB"; the init line maps Black -> RB and White -> LB;
#   3. pixels: >= 3 shots show a Series label stroke where the original glyph has its grey key face:
#      the "S" of "LS" (box 76,57,86,70, white >= 0.25) or the bar of the "T" of "LT" (box 80,59,97,64,
#      white >= 0.5), on a dark key face (box 60,48,100,86: >= 30 % dark, so a fireball does not count)
#      (prompt_check.py; 0 on all original-glyph shots; the prompts blink).
# On main (no prompts.mm) 2 and 3 fail.
source "$(dirname "$0")/../lib.sh"
FRAMES=""; for ((f = 300; f <= 990; f += 10)); do FRAMES="$FRAMES${FRAMES:+,}$f"; done
DUMP="${ROTK_CACHE:-$HOME/Desktop/Claude/ROTKPort/.cache}/runs/gamepad_prompts_dump-$$-$RANDOM"   # run_game makes RUN only at start
RUN_TIMEOUT=1500 run_game gamepad_prompts ROTK_SKIP_MOVIES=1 ROTK_PAD_SCRIPT="$TESTS_DIR/scripts/gamepad_prompts.pad" \
  ROTK_SHOT_FRAMES="$FRAMES" ROTK_EXIT_AFTER=1000 ROTK_HDUI_DUMP="$DUMP"
[[ -d "$DUMP" ]] && mv "$DUMP" "$RUN/dump"
assert_exit0; assert_no_fatal

SEMANTIC=$((SEMANTIC + 1))
IDX="$RUN/dump/index.tsv"
seen=0
check_glyph() {  # hash name series-label
  if ! grep -q "^$1" "$IDX" 2>/dev/null; then note "glyph $1 ($2) not used in this run"; return; fi
  seen=$((seen + 1))
  assert_log "polish: prompts: $1 \(Xbox $2\) -> $3" "Xbox $2 glyph not replaced by $3"
}
check_glyph a3b3a5d16faa0d86 "L stick" LS
check_glyph 404325db171c074c "L trigger" LT
stick_trigger=$seen   # LS / LT prompts on screen: the pixel check below needs them
check_glyph 8004f7870a39cffc "Black" RB
# The Black glyph the game draws for "Action" (owner test 3667d6a, prompt-black-action.png): a plain
# black disc, uploaded in every Hel01 run with the A/X/Y/L glyphs, so this one is always checked.
grep -q "^a4a43cfe5000c698" "$IDX" 2>/dev/null || fail "Black glyph a4a43cfe5000c698 not uploaded (route changed?)"
check_glyph a4a43cfe5000c698 "Black" RB
(( seen )) || fail "no replaced-button glyph in use: no prompt on screen (route changed?)"
assert_log "polish: prompts: Xbox Series glyphs \(Black -> RB, White -> LB" "glyph table: Black -> RB / White -> LB missing"

SEMANTIC=$((SEMANTIC + 1))
hits=0
for ((f = 300; f <= 990; f += 10)); do
  s="$(shot $f)"; [[ -f "$s" ]] || continue
  if python3 -I "$TESTS_DIR/scripts/prompt_check.py" "$s" 76,57,86,70 0.25 60,48,100,86 0.3 >/dev/null ||
     python3 -I "$TESTS_DIR/scripts/prompt_check.py" "$s" 80,59,97,64 0.5 60,48,100,86 0.3 >/dev/null; then
    hits=$((hits + 1))
  fi
done
echo "  Series LS/LT glyph in $hits shots"
if (( stick_trigger )); then
  (( hits >= 3 )) || fail "Series LS/LT glyph in $hits shots of 300..990 (want >= 3)"
else
  note "no LS/LT prompt in this run (the tutorial order follows game time): pixel check skipped"
fi
finish
