#!/bin/bash
# The launcher window (owner phase 2: "лаунчер: мова UA/EN, 120 fps на ProMotion, гамма, Play,
# арт гри"; tools/launcher/launcher.js):
#   1. drawn off screen (ROTK_LAUNCHER_SNAPSHOT) on a 120 Hz display: a PNG the window's size with
#      the art in its upper part (not black); on a 60 Hz display "120 fps" is not offered;
#      owner test 049f010: a ProMotion display set to 60 Hz (ROTK_DISPLAY_HZ=<current>,<max> = 60,120)
#      still offers 120 and shows one hint line on how to switch ProMotion on; no label is cut off
#      («…ProMot»), in Ukrainian and English;
#   2. Play (ROTK_LAUNCHER_AUTO=play) writes language / fps / gamma into settings.toml and keeps
#      the other keys; Quit writes nothing;
#   3. the installed app's launcher (APP, default ~/Desktop/ROTK.app) with settings en / 120 / warm:
#      the game starts with ROTK_LANG=en, ROTK_FPS=120, ROTK_GAMMA=warm, exit 0.
#   4. owner test 049f010: gamma is a choice of three, Оригінал / Сучасна / Тепла (2.5)
#      (settings gamma = original | modern | warm; anything else reads as original).
source "$(dirname "$0")/../lib.sh"
APP="${APP:-$HOME/Desktop/ROTK.app}"
JS="$APP/Contents/Resources/launcher/launcher.js"; [[ -f "$JS" ]] || JS="$ROOT/tools/launcher/launcher.js"
ART="$APP/Contents/Resources/launcher/art.png"
T="${ROTK_CACHE:-$ROOT/.cache}/runs/launcher_ui-$$"; rm -rf "$T"; mkdir -p "$T"
SEMANTIC=$((SEMANTIC + 1))
[[ -f "$ART" ]] || fail "no launcher art in $APP"
printf '[launcher]\ndraw_distance = "2.0"\n' > "$T/s.toml"
ROTK_DISPLAY_HZ=120,120 ROTK_LAUNCHER_SNAPSHOT="$T/ui120.png" osascript -l JavaScript "$JS" "$T/s.toml" "$ART" > "$T/o1" 2>&1 \
  || fail "window script failed: $(tail -1 "$T/o1")"
python3 -I - "$T/ui120.png" <<'PY' || fail "window snapshot: no art or wrong size"
import sys, subprocess, struct
d = open(sys.argv[1], 'rb').read(24); w, h = struct.unpack('>II', d[16:24])
assert abs(w / h - 768 / 706) < 0.02, (w, h)
# the art half must not be flat: sample the middle row's bytes of a downscaled BMP
out = sys.argv[1] + '.bmp'
subprocess.run(['sips', '-s', 'format', 'bmp', '-z', '58', '77', sys.argv[1], '--out', out], capture_output=True, check=True)
b = open(out, 'rb').read(); off = struct.unpack('<I', b[10:14])[0]
px = b[off:]; vals = set(px[i] for i in range(0, len(px), 7))
assert len(vals) > 40, len(vals)
PY
grep -q "^snapshot" "$T/o1" || fail "snapshot mode did not answer"
# the art keeps its aspect: the view has the picture's shape (owner test dcb2fc2: «розтягнуто»)
read -r vw vh iw ih < <(sed -nE 's/^art: view ([0-9.]+)x([0-9.]+), image ([0-9.]+)x([0-9.]+)$/\1 \2 \3 \4/p' "$T/o1")
[[ -n "$ih" ]] && python3 -c "import sys; sys.exit(0 if abs($vw/$vh - $iw/$ih) < 0.01 else 1)" ||
  fail "launcher art view and picture differ in shape ($(grep '^art:' "$T/o1"))"
grep -q "^gamma: Оригінал|Сучасна|Тепла (2.5)$" "$T/o1" || fail "gamma is not the choice of three ($(grep gamma "$T/o1"))"
grep -q "^fps: 60,120$" "$T/o1" && grep -q "^hint: no$" "$T/o1" || fail "120 Hz display: want 60,120 and no hint ($(tr '\n' ' ' < "$T/o1"))"
for lang in uk en; do
  printf '[launcher]\nlanguage = "%s"\n' $lang > "$T/p.toml"
  ROTK_DISPLAY_HZ=60,120 ROTK_LAUNCHER_SNAPSHOT="$T/pm-$lang.png" osascript -l JavaScript "$JS" "$T/p.toml" "$ART" > "$T/o-$lang" 2>&1
  grep -q "^fps: 60,120$" "$T/o-$lang" && grep -q "^hint: yes$" "$T/o-$lang" ||
    fail "ProMotion at 60 Hz ($lang): want 120 offered and the hint ($(tr '\n' ' ' < "$T/o-$lang"))"
  grep -q "^clipped: none$" "$T/o-$lang" || fail "cut-off labels ($lang): $(grep clipped "$T/o-$lang")"
done
# 60 Hz display: one rate only (checked through the auto answer: fps stays 60 though asked 120)
printf '[launcher]\nfps = "120"\n' > "$T/s60.toml"
ROTK_DISPLAY_HZ=60,60 ROTK_LAUNCHER_AUTO=play osascript -l JavaScript "$JS" "$T/s60.toml" > /dev/null 2>&1
grep -q '^fps = "60"' "$T/s60.toml" || fail "60 Hz display kept fps 120"
# 2. Play / Quit
ROTK_DISPLAY_HZ=120,120 ROTK_LAUNCHER_AUTO=play osascript -l JavaScript "$JS" "$T/s.toml" > "$T/o2" 2>&1
grep -q "^play" "$T/o2" || fail "Play did not answer 'play'"
for kv in 'language = "en"' 'fps = "120"' 'gamma = "original"' 'draw_distance = "2.0"'; do
  grep -qF "$kv" "$T/s.toml" || fail "settings.toml lacks $kv after Play"
done
cp "$T/s.toml" "$T/before"; ROTK_LAUNCHER_AUTO=quit osascript -l JavaScript "$JS" "$T/s.toml" > "$T/o3" 2>&1
grep -q "^quit" "$T/o3" && cmp -s "$T/before" "$T/s.toml" || fail "Quit changed the settings or did not answer 'quit'"
for g in modern warm original; do
  printf '[launcher]\ngamma = "%s"\n' $g > "$T/g.toml"
  ROTK_LAUNCHER_AUTO=play osascript -l JavaScript "$JS" "$T/g.toml" > /dev/null 2>&1
  grep -q "^gamma = \"$g\"" "$T/g.toml" || fail "gamma $g not kept: $(grep gamma "$T/g.toml")"
done
printf '[launcher]\ngamma = "1.30"\n' > "$T/g.toml"; ROTK_LAUNCHER_AUTO=play osascript -l JavaScript "$JS" "$T/g.toml" > /dev/null 2>&1
grep -q '^gamma = "original"' "$T/g.toml" || fail "old numeric gamma did not read as original"
# 5. owner test dcb2fc2: the window works with a pad (D-pad picks a row, left/right changes it, A
#    plays, B quits): the presses go through the window's gamepad handler (ROTK_LAUNCHER_PAD_SCRIPT;
#    a real GameController pad is not attached here). The window shows for about a second.
padrun() {  # settings script -> answer
  ROTK_DISPLAY_HZ=120,120 ROTK_LAUNCHER_PAD_SCRIPT="$2" osascript -l JavaScript "$JS" "$1" "$ART" > "$T/pad.out" 2>&1 &
  local p=$! i; for ((i = 0; i < 40; i++)); do kill -0 $p 2>/dev/null || break; sleep 0.25; done
  kill $p 2>/dev/null; wait $p 2>/dev/null; head -1 "$T/pad.out"
}
printf '[launcher]\ngamma = "original"\n' > "$T/pad.toml"
[[ "$(padrun "$T/pad.toml" "up right a")" == play ]] && grep -q '^gamma = "modern"' "$T/pad.toml" ||
  fail "pad in the launcher: up, right, A did not pick gamma Modern and play ($(head -1 "$T/pad.out"))"
# the language and frame-rate lists save what they show (AppKit indexes reach JXA as strings, PIT-D02:
# Українська -> English and 60 -> 120 were saved as uk / 60)
printf '[launcher]\nlanguage = "uk"\nfps = "60"\n' > "$T/pad2.toml"
[[ "$(padrun "$T/pad2.toml" "up up up right down right a")" == play ]] && grep -q '^language = "en"' "$T/pad2.toml" &&
  grep -q '^fps = "120"' "$T/pad2.toml" || fail "pad in the launcher: English + 120 fps not saved ($(grep -E '^(language|fps)' "$T/pad2.toml" | tr '\n' ' '))"
cp "$T/pad.toml" "$T/pad0"
[[ "$(padrun "$T/pad.toml" "up up b")" == quit ]] && cmp -s "$T/pad0" "$T/pad.toml" || fail "pad in the launcher: B did not quit"
# 3. the app's launcher applies them
export ROTK_PLAYER_ROOT="$T/player"
S="$ROTK_PLAYER_ROOT/home/Library/Application Support/ROTK"; mkdir -p "$S"
printf '[launcher]\nlanguage = "en"\nfps = "120"\ngamma = "warm"\n' > "$S/settings.toml"
BIN="$APP/Contents/MacOS/ROTK" run_game launcher_ui ROTK_TEST_PROFILE=0 ROTK_LAUNCHER_AUTO=play ROTK_DISPLAY_HZ=120,120 ROTK_LANG= \
  ROTK_DATA="$APP/Contents/Resources" ROTK_SHOT_FRAMES=300 ROTK_EXIT_AFTER=310
assert_exit0; assert_no_fatal
L="$S/../ROTK/player/logs/rotk.log"; LOGS+=("$RUN/stdout.log" $(find "$ROTK_PLAYER_ROOT" -name rotk.log))
assert_log "launcher: window -> play" "the launcher did not ask its window"
assert_log "launcher: language en, .*fps 120, gamma warm" "launcher did not apply en / 120 / warm"
# (the gamma itself is applied in the postfx present, which a silent run does not draw: not checked here)
finish
