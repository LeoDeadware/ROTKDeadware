#!/bin/bash
# The pad after the launcher (owner test dcb2fc2: «геймпад чомусь не працює»; the Xbox pad connected
# and went "active", but the game ignored it). Cause: the launcher window is a separate app
# (osascript) that is frontmost; when it closed, macOS 14 did not hand activation to the game, the
# game window never got focus and SDL drops pad events without focus. As a player would, no
# injected mouse or keyboard:
#   1. `open -n` the installed app (APP, default ~/Desktop/ROTK.app) with a scratch player root;
#   2. the launcher window gets "A" from its gamepad handler (ROTK_LAUNCHER_PAD_SCRIPT) -> Play;
#   3. in the game an SDL virtual Xbox pad (ROTK_VPAD_SCRIPT: SDL joystick events, SDL's focus rule,
#      the gamepad mapping - the real pad's path) presses START at frame 1000 in the prologue fight;
#   4. the log has the pad's first input and the game pauses: frames 1100 / 1160 nearly the same,
#      900 / 960 (fighting) not.
# Before the fix (game without activation and the background-events hint) the log had
# "active gamepad" but no "first input", like the owner's log. The windows show for ~20 s.
source "$(dirname "$0")/../lib.sh"
APP="${APP:-$HOME/Desktop/ROTK.app}"
T="${ROTK_CACHE:-$ROOT/.cache}/runs/launcher_pad-$(date +%Y%m%d-%H%M%S)-$$"; rm -rf "$T"; mkdir -p "$T/shots"
PR="$T/player"; S="$PR/home/Library/Application Support/ROTK"; mkdir -p "$S"
printf '[launcher]\nlanguage = "uk"\n' > "$S/settings.toml"
printf '1000 start 1\n1012 start 0\n' > "$T/vpad.txt"
SEMANTIC=$((SEMANTIC + 1))
open -n --env ROTK_PLAYER_ROOT="$PR" --env ROTK_SHOT_DIR="$T/shots" --env ROTK_SHOT_FRAMES=900,960,1100,1160 \
  --env ROTK_EXIT_AFTER=1170 --env ROTK_SKIP_MOVIES=1 --env ROTK_VPAD_SCRIPT="$T/vpad.txt" \
  --env ROTK_LAUNCHER_PAD_SCRIPT="a" "$APP" || { fail "open -n $APP failed"; finish; }
log() { find "$PR" -name rotk.log | head -1; }
for ((i = 0; i < 400; i++)); do
  [[ -f "$T/shots/frame_001160.png" ]] && ! pgrep -f "$APP/Contents/MacOS/rotk-bin" >/dev/null && break
  (( i == 40 )) && ! grep -q "window -> play" "$(log)" 2>/dev/null && break   # window ignored the pad
  sleep 1
done
pkill -f "Resources/launcher/launcher.js" 2>/dev/null
cp "$(log)" "$T/rotk.log" 2>/dev/null
grep -h "launcher: window\|virtual pad\|focus\|first input" "$T/rotk.log" 2>/dev/null | head -6
grep -q "launcher: window -> play" "$T/rotk.log" 2>/dev/null || fail "A on the pad did not press Play in the launcher"
grep -q "xinput: first input from gamepad" "$T/rotk.log" 2>/dev/null || fail "the pad's input never reached the game"
diff() {  # a.png b.png -> mean absolute difference (0..255) on a 160x120 copy
  python3 -I - "$1" "$2" <<'PY'
import sys, subprocess, struct
def px(p):
    subprocess.run(['sips', '-s', 'format', 'bmp', '-z', '120', '160', p, '--out', p + '.bmp'], capture_output=True, check=True)
    d = open(p + '.bmp', 'rb').read(); return d[struct.unpack('<I', d[10:14])[0]:]
a, b = px(sys.argv[1]), px(sys.argv[2]); n = min(len(a), len(b))
print(round(sum(abs(a[i] - b[i]) for i in range(n)) / n, 2))
PY
}
F="$T/shots/frame_00"
if [[ -f ${F}0900.png && -f ${F}0960.png && -f ${F}1100.png && -f ${F}1160.png ]]; then
  before=$(diff ${F}0900.png ${F}0960.png) after=$(diff ${F}1100.png ${F}1160.png)
  echo "launcher_pad: motion before START $before, after $after"
  python3 -c "import sys; sys.exit(0 if $after < 0.35 * $before and $before > 2 else 1)" ||
    fail "the game did not pause on the pad's START (motion before $before, after $after)"
else
  fail "no frames 900 / 960 / 1100 / 1160"
fi
echo "launcher_pad: run dir $T"
finish
