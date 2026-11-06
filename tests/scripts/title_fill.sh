#!/bin/bash
# Owner test dcb2fc2 («прям дуже розтягнуто», docs/evidence/owner-test-dcb2fc2): in widescreen the
# 4:3 title picture (Aragorn, the logo) was stretched 1.2x sideways to the wide screen. Full-height
# 2D backdrops must fill instead: scaled evenly to the screen's width, the top and bottom cropped.
# The 16:10 title frame is compared with the 4:3 one (a) stretched to 768x480 and (b) scaled to
# 768x576 and cropped to the middle 480 rows: it must match (b), not (a).
source "$(dirname "$0")/../lib.sh"
T="${ROTK_CACHE:-$ROOT/.cache}/runs/title_fill-$$"; rm -rf "$T"; mkdir -p "$T"
SEMANTIC=$((SEMANTIC + 1))
shoot() {  # out-dir [VAR=value ...]
  local d=$1; shift; mkdir -p "$d/home"
  ( ulimit -c 0; env HOME="$d/home" ROTK_LOG_DIR="$d" SDL_AUDIO_DRIVER=dummy SDL_VIDEO_DRIVER=dummy ROTK_BACKGROUND=1 \
      ROTK_DATA="$DATA" ROTK_SKIP_MOVIES=1 ROTK_SHOT_DIR="$d" ROTK_SHOT_FRAMES=648 ROTK_EXIT_AFTER=652 "$@" \
      nice -n 19 "$BIN" > "$d/out.log" 2>&1 )
  [[ -f "$d/frame_000648.png" ]] || { fail "no title frame ($d: $(tail -1 "$d/out.log"))"; finish; }
}
shoot "$T/n"
shoot "$T/w" ROTK_WIDESCREEN=1
python3 -I - "$T" <<'PY' || fail "title: $(cat "$T/why")"
import sys, subprocess, struct, os
T = sys.argv[1]
def px(p):
    d = open(p, 'rb').read(); off = struct.unpack('<I', d[10:14])[0]; w, h = struct.unpack('<ii', d[18:26])
    bpp = struct.unpack('<H', d[28:30])[0] // 8; td = h < 0; h = abs(h); row = (w * bpp + 3) // 4 * 4
    return w, h, lambda x, y: d[off + (y if td else h - 1 - y) * row + x * bpp: off + (y if td else h - 1 - y) * row + x * bpp + 3]
def bmp(src, out, *args):
    subprocess.run(['sips', *args, src, '--out', out], capture_output=True, check=True)
    subprocess.run(['sips', '-s', 'format', 'bmp', out, '--out', out + '.bmp'], capture_output=True, check=True)
    return px(out + '.bmp')
n, w = T + '/n/frame_000648.png', T + '/w/frame_000648.png'
W = bmp(w, T + '/w.png', '-z', '480', '768')
A = bmp(n, T + '/a.png', '-z', '480', '768')
subprocess.run(['sips', '-z', '576', '768', n, '--out', T + '/b0.png'], capture_output=True, check=True)
B = bmp(T + '/b0.png', T + '/b.png', '-c', '480', '768')
def err(X):
    s = c = 0
    for y in range(20, 400, 3):          # above the prompt line
        for x in range(10, 758, 3):
            p, q = W[2](x, y), X[2](x, y); s += sum(abs(p[k] - q[k]) for k in range(3)); c += 3
    return s / c
ea, eb = err(A), err(B)
print(f'title 16:10 vs stretched {ea:.1f}, vs filled {eb:.1f}')
if not (eb < 0.6 * ea and eb < 12):
    open(T + '/why', 'w').write(f'stretched (error to stretch {ea:.1f}, to fill {eb:.1f})'); sys.exit(1)
PY
finish
