#!/bin/bash
# Launcher art (owner test 049f010: "арт убогий і низької якості, просвічує «Натисніть START»"):
# tools/launcher/make_art.sh shoots the title screen from the disc and upscales it at the player's Mac.
#   1. at least 2048 px wide, 4:3 (the title is a 640x480 picture);
#   2. no START prompt, not even faint (owner test dcb2fc2 asked for a sensitive check): the
#      prompt's box (x 0.33-0.67, y 0.86-0.93) is at most 2/255 brighter than in the darkest of
#      110 title frames (11 blinks: the prompt fully off). Full prompt ~48/255, off ~27/255.
source "$(dirname "$0")/../lib.sh"
T="${ROTK_CACHE:-$ROOT/.cache}/runs/launcher_art-$$"; rm -rf "$T"; mkdir -p "$T"
SEMANTIC=$((SEMANTIC + 1))
nice -n 19 bash "$ROOT/tools/launcher/make_art.sh" "$BIN" "$DATA" "$T/art.png" > "$T/log" 2>&1 \
  || { fail "make_art failed: $(tail -1 "$T/log")"; finish; }
mkdir -p "$T/ref/home"
( ulimit -c 0; env HOME="$T/ref/home" ROTK_LOG_DIR="$T/ref" SDL_AUDIO_DRIVER=dummy SDL_VIDEO_DRIVER=dummy ROTK_BACKGROUND=1 \
    ROTK_DATA="$DATA" ROTK_SKIP_MOVIES=1 ROTK_SHOT_DIR="$T/ref" ROTK_SHOT_FRAMES="$(seq -s, 600 6 1260)" \
    ROTK_EXIT_AFTER=1262 nice -n 19 "$BIN" > "$T/ref/out.log" 2>&1 )
python3 -I - "$T/art.png" <<'PY' || fail "art: $(cat "$T/why" 2>/dev/null)"
import sys, subprocess, struct, os
import glob
p = sys.argv[1]; why = open(os.path.join(os.path.dirname(p), 'why'), 'w')
w, h = struct.unpack('>II', open(p, 'rb').read(24)[16:24])
if w < 2048 or abs(w / h - 4 / 3) > 0.02: why.write(f'{w}x{h}, want >=2048 wide 4:3'); sys.exit(1)
def box(f):
    b = f + '.bmp'; subprocess.run(['sips', '-s', 'format', 'bmp', '-z', '480', '640', f, '--out', b], capture_output=True, check=True)
    d = open(b, 'rb').read(); off = struct.unpack('<I', d[10:14])[0]; bw, bh = struct.unpack('<ii', d[18:26])
    bpp = struct.unpack('<H', d[28:30])[0] // 8; td = bh < 0; bh = abs(bh); row = (bw * bpp + 3) // 4 * 4; s = n = 0
    for y in range(int(bh * 0.86), int(bh * 0.93)):
        yy = y if td else bh - 1 - y
        for x in range(int(bw * 0.33), int(bw * 0.67), 2):
            q = off + yy * row + x * bpp; s += d[q] + d[q + 1] + d[q + 2]; n += 3
    return s / n
refs = [box(f) for f in sorted(glob.glob(os.path.dirname(p) + '/ref/frame_*.png'))]
if len(refs) < 50: why.write(f'only {len(refs)} reference title frames'); sys.exit(1)
m, ref, top = box(p), min(refs), max(refs)
print(f'art {w}x{h}, START box {m:.1f}/255, title frames {ref:.1f} (off) .. {top:.1f} (on)')
if m > ref + 2: why.write(f'START box {m:.1f}/255 vs {ref:.1f} with the prompt off (prompt shows)'); sys.exit(1)
PY
echo "launcher_art: $(tail -1 "$T/log" | cut -c1-90)"
finish
