#!/bin/bash
# Button prompts keep their shape at 16:10 (owner test c96b3d1: the "X Parry" key was an oval about
# 2:1, docs/evidence/owner-test-c96b3d1/stretched-prompt-parry.webp).
# The tutorial prompts are fixed-function quads at x 0.87..0.995 of the screen; the widescreen
# epilogue kept vertices at the 4:3 edge (|x| >= 0.99) on the wide edge, so the right side of the
# key stretched out to it. Fix: only large 2D draws (full-screen quads, backdrop tiles) stretch
# (d3d8.cpp WideEdgeStretch).
# Hel01 prologue in the app's screen mode (ROTK_WIDESCREEN=1, ROTK_RENDER_SCALE=4: 3072x1920),
# gamepad_prompts.pad; the "A Speed" prompt is up at ~400..480. Each shot with the green key
# (glyph_aspect.py: largest green blob in the top-right quarter) must measure w/h 0.8..1.35 (4:3 run:
# 1.18; broken: 2.1), >= 3 such shots.
source "$(dirname "$0")/../lib.sh"
FRAMES="400,410,420,430,440,450,460,470,480"
RUN_TIMEOUT=900 run_game prompt_aspect ROTK_SKIP_MOVIES=1 ROTK_PAD_SCRIPT="$TESTS_DIR/scripts/gamepad_prompts.pad" \
  ROTK_SHOT_FRAMES="$FRAMES" ROTK_EXIT_AFTER=485 ROTK_WIDESCREEN=1 ROTK_RENDER_SCALE=4
assert_exit0; assert_no_fatal
SEMANTIC=$((SEMANTIC + 1))
seen=0 bad=0
for f in ${FRAMES//,/ }; do
  s="$(shot $f)"; [[ -f "$s" ]] || { fail "no shot $f"; continue; }
  out="$(python3 -I "$TESTS_DIR/scripts/glyph_aspect.py" "$s" green)"; rc=$?
  echo "  frame $f: $out"
  [[ "$out" == no\ glyph* ]] && continue
  seen=$((seen + 1)); (( rc )) && bad=$((bad + 1))
done
(( seen >= 3 )) || fail "green prompt key in $seen shots (want >= 3; route changed?)"
(( bad == 0 )) || fail "prompt key stretched in $bad of $seen shots (w/h outside 0.8..1.35)"
finish
