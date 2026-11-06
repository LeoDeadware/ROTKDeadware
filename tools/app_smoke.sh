#!/bin/bash
# Smoke test of the INSTALLED app (RULES R-I3; HP2: the .app showed the wrong flag and a
# "default on" HUD option was off — only the build dir had been tested).
# Launched exactly the way Finder does it: `open -n ~/Desktop/ROTK.app` (LaunchServices; the app is
# its own responsible process for macOS privacy, TCC), with the smoke settings passed by `open --env`:
# 600 frames, silent (dummy audio/video), test profile OFF, logs into a run dir. Checks: the bundle's
# executable is a Mach-O (owner bug 2026-11-06: a bash script as CFBundleExecutable could not read the
# app's files under ~/Desktop when opened from Finder: no process, no log), exit 0 after 600 frames,
# no FATAL, every mods/ pack named in tools/install.d/expect.txt was loaded ("mods: loaded <name>").
#   tools/app_smoke.sh            # APP=~/Desktop/ROTK.app by default
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="${APP:-$HOME/Desktop/ROTK.app}"
C="$APP/Contents"
[[ -x "$C/MacOS/rotk-bin" ]] || { echo "app_smoke: FAIL no $APP"; exit 1; }
exe="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$C/Info.plist" 2>/dev/null)"
fails=0
file -b "$C/MacOS/$exe" | grep -q "Mach-O" \
  || { echo "app_smoke: FAIL CFBundleExecutable '$exe' is not a Mach-O ($(file -b "$C/MacOS/$exe" | cut -c1-40))"; fails=1; }
RUN="${ROTK_CACHE:-$ROOT/.cache}/runs/app_smoke-$(date +%Y%m%d-%H%M%S)-$$"; mkdir -p "$RUN"
pkill -f "$C/MacOS/rotk-bin" 2>/dev/null
open -n "$APP" --env ROTK_LOG_DIR="$RUN" --env ROTK_BACKGROUND=1 --env ROTK_EXIT_AFTER=600 \
  --env SDL_AUDIO_DRIVER=dummy --env SDL_VIDEO_DRIVER=dummy --env ROTK_LAUNCHER_UI=0 \
  || { echo "app_smoke: FAIL open -n $APP"; fails=1; }
for _ in $(seq 1 240); do grep -q "rotk: exit" "$RUN/runtime.log" 2>/dev/null && break; sleep 0.5; done
sleep 1
if pgrep -f "$C/MacOS/rotk-bin" >/dev/null && ! grep -q "rotk: exit" "$RUN/runtime.log" 2>/dev/null; then
  pkill -f "$C/MacOS/rotk-bin"; echo "app_smoke: FAIL timeout"
fi
grep -q "rotk: exit 0: ROTK_EXIT_AFTER" "$RUN/runtime.log" 2>/dev/null \
  || { echo "app_smoke: FAIL the app did not run 600 frames via open -n (no clean exit in $RUN/runtime.log)"; fails=1; }
grep -h -m1 -E "^(\[krnl\] )?FATAL" "$RUN"/runtime.log 2>/dev/null && fails=1
if [[ -f "$ROOT/tools/install.d/expect.txt" && -f "$RUN/runtime.log" ]]; then
  while read -r m; do
    [[ -z "$m" || "$m" == \#* ]] && continue
    grep -q "mods: loaded $m" "$RUN/runtime.log" || { echo "app_smoke: FAIL pack $m not loaded"; fails=1; }
  done < "$ROOT/tools/install.d/expect.txt"
fi
(( fails )) && { echo "app_smoke: evidence $RUN"; exit 1; }
echo "app_smoke: PASS ($RUN)"
