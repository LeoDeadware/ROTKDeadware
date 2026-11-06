#!/bin/bash
# Addendum c (2026-11-06): native resolution. ROTK_WIDESCREEN=native and ROTK_RENDER_SCALE=native
# (the app's defaults) take the aspect (Hor+) and the scale from the display; ROTK_DISPLAY_SIZE
# stands for one: a 21:9 3440x1440 monitor, legal screen at frame 300:
#   1. log "polish: widescreen native 3440x1440 (2.389:1, view x1.792 wider)" and "render scale native: 3x";
#   2. the shot is 3441x1440 (640 x 1.792 rounded, times 3; 480 x 3);
source "$(dirname "$0")/../lib.sh"
size() { python3 -I -c 'import struct,sys; d=open(sys.argv[1],"rb").read(24); print(*struct.unpack(">II", d[16:24]))' "$1"; }
run_game native_res ROTK_WIDESCREEN=native ROTK_RENDER_SCALE=native ROTK_DISPLAY_SIZE=3440x1440 \
  ROTK_SHOT_FRAMES=300 ROTK_EXIT_AFTER=310
assert_exit0; assert_no_fatal
assert_log "polish: widescreen native 3440x1440 \(2.389:1, view x1.792 wider\)" "aspect not taken from the display"
assert_log "render scale native: 3x" "render scale not taken from the display"
SEMANTIC=$((SEMANTIC + 1))
s="$(shot 300)"
[[ -f "$s" ]] && [[ "$(size "$s")" == "3441 1440" ]] || fail "shot is $( [[ -f $s ]] && size "$s") (want 3441 1440)"
finish
