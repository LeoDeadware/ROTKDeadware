#!/bin/bash
# The launcher window's art: the game's own title screen (Aragorn, the logo) shot from the player's
# disc by a silent run and upscaled 4x on the player's Mac; never committed or shipped:
#   make_art.sh <game binary> <data dir (game/extracted inside)> <out.png>
# Owner test 049f010: "арт убогий і низької якості, просвічує «Натисніть START»". So: the 640x480
# title (4:3, render scale 1: the picture is 2D, scale 2 adds no detail), frames 600..720 (two blinks
# of the prompt, 60 frames each), keep the frame whose prompt box is darkest (the prompt is off), 4x
# to 2560x1920 with Real-ESRGAN when installed (dev Mac), else MetalFX (`upscale` next to this script
# in the DMG kit, or tools/hdui/upscale.swift in the repo).
set -uo pipefail
BIN=$1 DATA=$2 OUT=$3
HERE="$(cd "$(dirname "$0")" && pwd)"
T="$(mktemp -d "${TMPDIR:-/tmp}/rotk_art.XXXXXX")"; trap 'rm -rf "$T"' EXIT
mkdir -p "$T/home" "$T/in" "$T/up"
FR=$(seq -s, 600 6 720)
( ulimit -c 0
  env HOME="$T/home" ROTK_LOG_DIR="$T" SDL_AUDIO_DRIVER=dummy SDL_VIDEO_DRIVER=dummy ROTK_BACKGROUND=1 \
    ROTK_DATA="$DATA" ROTK_SKIP_MOVIES=1 ROTK_SHOT_DIR="$T" ROTK_SHOT_FRAMES="$FR" ROTK_EXIT_AFTER=730 \
    nice -n 10 "$BIN" > "$T/out.log" 2>&1 ) &
pid=$!; t=0
while kill -0 $pid 2>/dev/null && (( t < 300 )); do sleep 0.5; t=$((t + 1)); done
kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null
ls "$T"/frame_*.png > /dev/null 2>&1 || { echo "make_art: no title shot ($(tail -1 "$T/out.log"))"; exit 1; }
best=$(python3 -I - "$T" <<'PY'
import sys, glob, subprocess, struct
best = None
for f in sorted(glob.glob(sys.argv[1] + '/frame_*.png')):
    b = f + '.bmp'; subprocess.run(['sips', '-s', 'format', 'bmp', '-z', '480', '640', f, '--out', b], capture_output=True)
    d = open(b, 'rb').read(); off = struct.unpack('<I', d[10:14])[0]; w, h = struct.unpack('<ii', d[18:26])
    bpp = struct.unpack('<H', d[28:30])[0] // 8; td = h < 0; h = abs(h); row = (w * bpp + 3) // 4 * 4; s = 0
    for y in range(int(h * 0.86), int(h * 0.93)):
        yy = y if td else h - 1 - y
        for x in range(int(w * 0.33), int(w * 0.67), 2):
            p = off + yy * row + x * bpp; s += d[p] + d[p + 1] + d[p + 2]
    if best is None or s < best[0]: best = (s, f)
print(best[1])
PY
)
[[ -f "$best" ]] || { echo "make_art: could not pick a frame"; exit 1; }
cp "$best" "$T/in/art.png"
ESR="${ROTK_CACHE:-$HERE/../../.cache}/tools/realesrgan/x"
if [[ -x "$ESR/realesrgan-ncnn-vulkan" && -z "${ROTK_ART_METALFX:-}" ]]; then
  eng=esrgan; (cd "$T/up" && "$ESR/realesrgan-ncnn-vulkan" -i "$T/in/art.png" -o "$T/up/art.png" \
    -n realesrgan-x4plus -m "$ESR/models" -f png > /dev/null 2>&1)
else
  eng=metalfx; UP="$HERE/upscale"
  if [[ ! -x "$UP" ]]; then UP="$T/upscale"; swiftc -O -o "$UP" "$HERE/../hdui/upscale.swift" > /dev/null 2>&1; fi
  "$UP" "$T/in" "$T/up" 4 metalfx > /dev/null 2>&1 || "$UP" "$T/in" "$T/up" 4 lanczos > /dev/null 2>&1
fi
src="$T/up/art.png"; [[ -f "$src" ]] || { eng=none; src="$best"; }
mkdir -p "$(dirname "$OUT")"; mv "$src" "$OUT"
echo "make_art: $(basename "$best") $eng $(sips -g pixelWidth -g pixelHeight "$OUT" | awk '/pixel/ {printf "%s ", $2}')-> $OUT"
