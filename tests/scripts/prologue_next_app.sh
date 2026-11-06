#!/bin/bash
# Prologue -> next level in the app's mode (owner bug 2026-11-06: "the game crashes after the
# prologue", app at 60 fps, 16:10, 4x, HD packs). The owner's run (player log 09:55) froze in
# Hel01 ~124 s into play: the frame stayed the same (599 draws), the async-read queue and the
# music feeder stopped, no next scene opened.
# Route: skip movies, Hel01 in play from ~300; at ROUTE_AT the scene-select hook loads Pat01
# (as level_smoke.sh); app env: ROTK_FPS60=1 ROTK_WIDESCREEN=1 ROTK_RENDER_SCALE=4, packs from
# the install step. Wants exit 0, Pat01 opened after the call and in gameplay
# (level_check.py), frames still differ at the end.
source "$(dirname "$0")/../lib.sh"
ROUTE_AT=1500 END=$((1500 + ${LOAD:-3000} + ${PLAY:-2400}))
ROUTE="$ROUTE_AT:poke:16D584:0,$ROUTE_AT:9D770/1,$ROUTE_AT:9C8A0/0"
RES="${HD_CACHE:-$ROOT/out/hd}/prologue_res-$$"; rm -rf "$RES"; mkdir -p "$RES/mods"
bash "$ROOT/tools/install.d/30-hd.sh" "$ROOT" "$BUILD" "$RES" > /dev/null 2>&1 || note "hd packs not built"
PAD="$TESTS_DIR/scripts/hel01_fight.pad"
RUN_TIMEOUT=$((END / 40 + 300)) run_game prologue_next_app ROTK_TEST_PROFILE=0 ROTK_SKIP_HOLD=0 \
  ROTK_SKIP_MOVIES=1 ROTK_FPS60=1 ROTK_WIDESCREEN=1 ROTK_RENDER_SCALE=4 ROTK_MODS="$RES/mods" \
  ROTK_PAD_SCRIPT="$PAD" ROTK_CALL_AT="$ROUTE" ROTK_EXIT_AFTER=$END \
  ROTK_SHOT_FRAMES="$((END - 300)),$((END - 30))"
rm -rf "$RES"
assert_exit0; assert_no_fatal
assert_log "ROTK_CALL_AT 0009C8A0 returned" "route call did not run"
SEMANTIC=$((SEMANTIC + 1))
python3 -I "$TESTS_DIR/scripts/level_check.py" "$RUN" Pat01 50 1200 || fail "Pat01 not in gameplay after the prologue"
assert_frames_differ $((END - 300)) $((END - 30))
finish
