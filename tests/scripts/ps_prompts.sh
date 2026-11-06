#!/bin/bash
# Addendum b (2026-11-06): with a PlayStation pad the button prompts show PlayStation glyphs.
# Hel01 prologue as gamepad_prompts.sh, ROTK_PAD_KIND=ps (stands for a DualSense: no pad in tests),
# ROTK_PROMPTS_DUMP of the replacements:
#   1. "polish: prompts: PlayStation glyphs (by the connected pad)";
#   2. A -> cross (085495bbf8e71158) and Black -> R1 (a4a43cfe5000c698, always uploaded) replaced;
#   3. the dumped cross glyph has the PlayStation blue in its centre (ps_glyph_check.py).
source "$(dirname "$0")/../lib.sh"
DUMP="${ROTK_CACHE:-$HOME/Desktop/Claude/ROTKPort/.cache}/runs/ps_prompts_dump-$$-$RANDOM"
RUN_TIMEOUT=1500 run_game ps_prompts ROTK_SKIP_MOVIES=1 ROTK_PAD_SCRIPT="$TESTS_DIR/scripts/gamepad_prompts.pad" \
  ROTK_PAD_KIND=ps ROTK_PROMPTS_DUMP="$DUMP" ROTK_SHOT_FRAMES=600 ROTK_EXIT_AFTER=700
[[ -d "$DUMP" ]] && mv "$DUMP" "$RUN/dump"
assert_exit0; assert_no_fatal
assert_log "polish: prompts: PlayStation glyphs \(by the connected pad\)" "PlayStation pad not detected"
assert_log "polish: prompts: 085495bbf8e71158 \(Xbox A\) -> cross" "A not replaced by cross"
assert_log "polish: prompts: a4a43cfe5000c698 \(Xbox Black\) -> R1" "Black not replaced by R1"
SEMANTIC=$((SEMANTIC + 1))
python3 -I - "$RUN/dump/085495bbf8e71158.png" <<'PY' || fail "cross glyph not blue in its centre ($RUN/dump)"
import sys, subprocess, struct, zlib
p = sys.argv[1]
d = open(p, "rb").read()
w, h = struct.unpack(">II", d[16:24])
raw, i = b"", 8
while i < len(d):
    n, t = struct.unpack(">I4s", d[i:i + 8]); i += 8
    if t == b"IDAT": raw += d[i:i + n]
    i += n + 4
raw = zlib.decompress(raw); bpp = 4; stride = w * bpp
rows, prev = [], bytearray(stride)
for y in range(h):
    f = raw[y * (stride + 1)]; line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
    for x in range(stride):
        a = line[x - bpp] if x >= bpp else 0; b = prev[x]; c = prev[x - bpp] if x >= bpp else 0
        if f == 1: line[x] = (line[x] + a) & 255
        elif f == 2: line[x] = (line[x] + b) & 255
        elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
        elif f == 4:
            pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
            line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
    rows.append(line); prev = line
blue = sum(1 for y in range(h // 3, 2 * h // 3) for x in range(w // 3, 2 * w // 3)
           if rows[y][4 * x + 2] > 180 and rows[y][4 * x + 2] > rows[y][4 * x] + 60 and rows[y][4 * x + 3] > 128)
print("blue pixels in the centre:", blue)
sys.exit(0 if blue >= (w * h) // 200 else 1)
PY
finish
