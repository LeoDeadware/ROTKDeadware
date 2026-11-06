#!/bin/bash
# Before/after sheet of the world upscaler (addendum f): the N most visible world textures of the
# census (WORLD_DIR/coverage.tsv), each as original (4x nearest) | MetalFX | Real-ESRGAN x4plus.
#   tools/hdworld/compare_sheet.sh [world_dir] [out.png] [N]   (default out/hd/world, out/hd/reports/esrgan_vs_metalfx.png, 8)
# Derived from game data: stays in the git-ignored out/hd (R-L2).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
W="${1:-${HD_CACHE:-$ROOT/out/hd}/world}" OUTP="${2:-${HD_CACHE:-$ROOT/out/hd}/reports/esrgan_vs_metalfx.png}" N="${3:-8}"
T="$(dirname "$OUTP")/compare-$$"; rm -rf "$T"; mkdir -p "$T/in" "$T/metalfx" "$T/esrgan" "$T/near"
grep -v '^#' "$W/coverage.tsv" | awk -F'\t' '$2 >= 32 && $3 >= 32' | head -"$N" | cut -f1 | while read -r h; do
  cp "$W/dump/$h.png" "$T/in/"
  sips -s format png --resampleWidth "$(( $(sips -g pixelWidth "$T/in/$h.png" | awk '/pixelWidth/{print $2}') * 4 ))" \
    "$T/in/$h.png" --out "$T/near/$h.png" >/dev/null   # (sips is bilinear: the "before")
done
cd "$ROOT/tools/hdui"
HD_ENGINE=metalfx python3 -I -c 'import sys; sys.path.insert(0, "."); import hdlib; hdlib.upscale4(sys.argv[1], sys.argv[2])' "$T/in" "$T/metalfx"
HD_ENGINE=esrgan python3 -I -c 'import sys; sys.path.insert(0, "."); import hdlib; hdlib.upscale4(sys.argv[1], sys.argv[2])' "$T/in" "$T/esrgan"
C="${HD_CACHE:-$ROOT/out/hd}/bin/contact"
[[ -x "$C" ]] || swiftc -O -o "$C" "$ROOT/tools/hdui/contact.swift"
args=()
for f in "$T"/in/*.png; do
  h="$(basename "$f" .png)"
  args+=("$T/near/$h.png=$h original" "$T/metalfx/$h.png=MetalFX" "$T/esrgan/$h.png=Real-ESRGAN")
done
"$C" "$OUTP" --cols 3 --width 320 "${args[@]}"
rm -rf "$T"
echo "sheet: $OUTP"
