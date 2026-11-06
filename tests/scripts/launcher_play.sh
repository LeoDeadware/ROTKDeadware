#!/bin/bash
# Play in the launcher window starts the game (owner test 049f010: "натискаю «Грати» — гра
# закривається"; the log said "launcher: window -> quit" every time). As a user would:
#   1. `open -n` the installed app (APP, default ~/Desktop/ROTK.app) from Finder's side, with a
#      scratch player root (never the owner's profile) and the game told to shoot frame 300 and quit;
#   2. wait for the launcher window (tools/launcher/press.swift wait-window), then a real mouse click
#      (a HID event) on «Грати», 122/67 pt from the window's bottom-right corner;
#   3. the player log says "window -> play" and the game writes frame 300, then exits;
#   (the pad after the launcher: tests/scripts/launcher_pad.sh). The click waits until the user has
#   left the mouse and keyboard alone for 20 s (up to 10 min): clicks must not land in their work.
# The window and the game show on the screen for a few seconds.
source "$(dirname "$0")/../lib.sh"
APP="${APP:-$HOME/Desktop/ROTK.app}"
T="${ROTK_CACHE:-$ROOT/.cache}/runs/launcher_play-$(date +%Y%m%d-%H%M%S)-$$"; rm -rf "$T"; mkdir -p "$T/shots"
PRESS="$ROOT/.cache/tools/press"
if [[ ! -x "$PRESS" || "$ROOT/tools/launcher/press.swift" -nt "$PRESS" ]]; then
  swiftc -O -o "$PRESS" "$ROOT/tools/launcher/press.swift" > "$T/swiftc.log" 2>&1 || { fail "press.swift does not build"; finish; }
fi
PR="$T/player"; S="$PR/home/Library/Application Support/ROTK"; mkdir -p "$S"
printf '[launcher]\nlanguage = "uk"\n' > "$S/settings.toml"
idle() { ioreg -c IOHIDSystem -d 4 | awk '/HIDIdleTime/ {print int($NF / 1000000000); exit}'; }
for ((i = 0; i < 120; i++)); do (( $(idle) >= 20 )) && break; sleep 5; done
(( $(idle) >= 20 )) || { fail "the user kept working for 10 min: no click attempted"; finish; }
before=$(pgrep -f "Resources/launcher/launcher.js" | tr '\n' ' ')
open -n --env ROTK_PLAYER_ROOT="$PR" --env ROTK_SHOT_DIR="$T/shots" --env ROTK_SHOT_FRAMES=300 \
  --env ROTK_EXIT_AFTER=330 "$APP" || { fail "open -n $APP failed"; finish; }
SEMANTIC=$((SEMANTIC + 1))
pid=""; for ((i = 0; i < 100; i++)); do
  for p in $(pgrep -f "Resources/launcher/launcher.js"); do [[ " $before " == *" $p "* ]] || pid=$p; done
  [[ -n "$pid" ]] && break; sleep 0.2
done
[[ -n "$pid" ]] || { fail "no launcher window process"; finish; }
"$PRESS" "$pid" wait-window 60 || { fail "the launcher window did not show"; kill "$pid"; finish; }
sleep 1
"$PRESS" "$pid" click -122 -83
L="$PR/logs/rotk.log"; [[ -f "$L" ]] || L=$(find "$PR" -name rotk.log | head -1)
for ((i = 0; i < 180; i++)); do
  [[ -f "$T/shots/frame_000300.png" ]] && ! pgrep -f "ROTK_PLAYER_ROOT|$APP/Contents/MacOS/rotk-bin" >/dev/null && break
  grep -q "window -> quit" "$(find "$PR" -name rotk.log | head -1)" 2>/dev/null && break
  sleep 1
done
kill "$pid" 2>/dev/null   # a window still up after the click
L=$(find "$PR" -name rotk.log | head -1); cp "$L" "$T/rotk.log" 2>/dev/null
grep -h "launcher: window" "$T/rotk.log" 2>/dev/null | tail -1
grep -q "launcher: window -> play" "$T/rotk.log" 2>/dev/null || fail "Play did not answer 'play' ($(grep -h 'launcher: window' "$T/rotk.log" 2>/dev/null | tail -1))"
[[ -f "$T/shots/frame_000300.png" ]] || fail "the game did not reach frame 300"
echo "launcher_play: run dir $T"
finish
