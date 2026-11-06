#!/bin/bash
# Owner bug 2026-11-06 (680a23b): "саби занадто високо" - subtitles sat at 0.76 of the screen
# height (FUN_000F7BE0's line y, 0x158D1C/0x158D20), well above the bottom edge. App mode
# (ROTK_TEST_PROFILE=0, the Ukrainian layer, subtitles on): the intro cinematic's subtitle lines
# (OCR, Cyrillic) must end in the bottom tenth of the frame (bottom >= 0.90).
source "$(dirname "$0")/../lib.sh"
TOOLS="$ROOT/out/loc/bin"; mkdir -p "$TOOLS"
[[ -x "$TOOLS/ocr" && "$TOOLS/ocr" -nt "$ROOT/tools/loc/ocr.swift" ]] || swiftc -O -o "$TOOLS/ocr" "$ROOT/tools/loc/ocr.swift" 2>&1 | tail -3
MODS="${UA_MODS:-$HOME/Desktop/ROTK.app/Contents/Resources/mods/uk}"
[[ -d "$MODS" ]] || { echo "subs_position: no $MODS (install first)"; exit 2; }
T="$(mktemp -d "$ROOT/.cache/tmp/subs.XXXX")"; printf '1100 press START\n' > "$T/start.pad"
RUN_TIMEOUT=600 run_game subs_position ${SUBS_ENV:-} ROTK_TEST_PROFILE=0 ROTK_HDUI=0 ROTK_HDWORLD=0 ROTK_MODS="$MODS" ROTK_LANG=uk \
  ROTK_PAD_SCRIPT="$T/start.pad" ROTK_SHOT_FRAMES=2950,3250,4150 ROTK_EXIT_AFTER=4160
rm -rf "$T"
assert_exit0; assert_no_fatal
"$TOOLS/ocr" "$(shot 2950)" "$(shot 3250)" "$(shot 4150)" > "$RUN/ocr.tsv" 2>&1
SEMANTIC=$((SEMANTIC + 1))
python3 -I - "$RUN/ocr.tsv" <<'PY' || fail "subtitles not at the bottom edge ($RUN/ocr.tsv)"
import re, sys
b = [float(l.rstrip().rsplit('-', 1)[1]) for l in open(sys.argv[1], encoding='utf-8', errors='replace')
     if re.search('[А-Яа-яІіЇїЄєҐґ]{4,}', l) and re.search(r'\t[\d.]+-[\d.]+$', l.rstrip())]
print('  subtitle line bottoms:', b)
sys.exit(0 if b and min(b) >= 0.90 else 1)
PY
finish
