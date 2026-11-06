#!/bin/bash
# Screen defaults of the INSTALLED app (Polish 5a; native since addendum c): its launcher takes the
# aspect (Hor+) and the render scale from the display; under ROTK_TEST_PROFILE=1 both stay off
# (640x480 4:3 goldens). ROTK_DISPLAY_SIZE=2560x1600 stands for the display (16:10, 1600 rows: 3x).
#   tests/scripts/app_screen.sh            # APP=~/Desktop/ROTK.app by default
# Two runs of the bundle's own launcher (Contents/MacOS/ROTK; player files in a scratch root) (300 frames, legal screen), silent:
#   1. test profile off: log "polish: widescreen native 2560x1600", the shot of frame 300 is
#      2304x1440 (640*1.2 x 480, scale 3);
#   2. ROTK_TEST_PROFILE=1: no widescreen line, shot 640x480.
source "$(dirname "$0")/../lib.sh"
APP="${APP:-$HOME/Desktop/ROTK.app}"
LAUNCH="$APP/Contents/MacOS/ROTK"
export ROTK_PLAYER_ROOT="${ROTK_CACHE:-$ROOT/.cache}/runs/app_screen-player"   # never the owner's profile
[[ -x "$LAUNCH" ]] || { echo "app_screen: no $LAUNCH (install first)"; exit 2; }
size() { python3 -I -c 'import struct,sys; d=open(sys.argv[1],"rb").read(24); print(*struct.unpack(">II", d[16:24]))' "$1"; }

BIN="$LAUNCH" run_game app_screen_on ROTK_TEST_PROFILE=0 ROTK_DISPLAY_SIZE=2560x1600 ROTK_SHOT_FRAMES=300 ROTK_EXIT_AFTER=310 \
  ROTK_DATA="$APP/Contents/Resources/game/.." ROTK_MODS="$APP/Contents/Resources/mods"
assert_exit0; assert_no_fatal
assert_log "polish: widescreen native 2560x1600" "app default: native widescreen not on"
assert_log "render target colour 640x480" "no 640x480 screen target"
s="$(shot 300)"
[[ -f "$s" ]] && [[ "$(size "$s")" == "2304 1440" ]] || fail "app default shot is $( [[ -f $s ]] && size "$s") (want 2304 1440)"

BIN="$LAUNCH" run_game app_screen_test ROTK_SHOT_FRAMES=300 ROTK_EXIT_AFTER=310 \
  ROTK_DATA="$APP/Contents/Resources/game/.." ROTK_MODS="$APP/Contents/Resources/mods"
assert_exit0; assert_no_fatal
SEMANTIC=$((SEMANTIC + 1))
grep -q "polish: widescreen" "$LOG" && fail "widescreen on under ROTK_TEST_PROFILE=1"
s="$(shot 300)"
[[ -f "$s" ]] && [[ "$(size "$s")" == "640 480" ]] || fail "test profile shot is $( [[ -f $s ]] && size "$s") (want 640 480)"
finish
