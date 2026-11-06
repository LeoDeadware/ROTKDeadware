#!/bin/bash
# Language of the app (owner 2026-11-06, later the same day: «мову обирають в лончері»): English on
# a fresh profile (no saved choice), Ukrainian by the launcher's setting (settings.toml [launcher]
# language = "uk", the language list of the launcher window).
# Runs the bundle's launcher (tools/launcher/launcher.c, built here into a scratch bundle that links
# the build's rotk; LAUNCH=<exe> runs another launcher, e.g. an installed app's) with a fresh
# player root, ROTK_TEST_PROFILE=0 and no ROTK_LANG / ROTK_MODS (the launcher picks them):
#   1. fresh profile: ua_menu.pad (skip movies; pause menu): OCR "Resume", no "Продовжити";
#   2. [launcher] language = "uk": the same pad shows "Продовжити" and "Один гравець", no "Resume";
#   3. Ukrainian subtitles: START at the title, the intro cinematic plays; some shot of 1300..3100
#      shows a Cyrillic subtitle line (OCR) and the log has "loc: subtitles on by default".
source "$(dirname "$0")/../lib.sh"
TOOLS="$ROOT/out/loc/bin"; mkdir -p "$TOOLS"
[[ -x "$TOOLS/ocr" && "$TOOLS/ocr" -nt "$ROOT/tools/loc/ocr.swift" ]] || swiftc -O -o "$TOOLS/ocr" "$ROOT/tools/loc/ocr.swift" 2>&1 | tail -3
T="${ROTK_CACHE:-$ROOT/.cache}/runs/lang_default-app-$$"
rm -rf "$T"
if [[ -z "${LAUNCH:-}" ]]; then
  C="$T/ROTK.app/Contents"; mkdir -p "$C/MacOS" "$C/Resources/mods"
  ln -s "$BUILD/src/loader/rotk" "$C/MacOS/rotk-bin"
  ln -s "$DATA/game" "$C/Resources/game"
  clang -O2 -o "$C/MacOS/ROTK" "$ROOT/tools/launcher/launcher.c" -framework CoreGraphics -framework CoreFoundation || { fail "launcher does not build"; finish; }
  nice -n 19 bash "$ROOT/tools/install.d/20-loc.sh" "$ROOT" "$BUILD" "$C/Resources" | tail -1
  LAUNCH="$C/MacOS/ROTK"
fi
export ROTK_PLAYER_ROOT="$T/player"; mkdir -p "$T/player"
APPENV=(ROTK_TEST_PROFILE=0 ROTK_FPS60=0 ROTK_WIDESCREEN=0 ROTK_RENDER_SCALE=1 ROTK_SKIP_HOLD=0)
ocr() { "$TOOLS/ocr" "$@" > "$RUN/ocr.tsv" 2>&1; }
has() { SEMANTIC=$((SEMANTIC + 1)); grep -qF "$2" "$RUN/ocr.tsv" || fail "$1: no \"$2\" on screen ($RUN/ocr.tsv)"; }
hasnt() { SEMANTIC=$((SEMANTIC + 1)); ! grep -qiF "$2" "$RUN/ocr.tsv" || fail "$1: \"$2\" on screen"; }

BIN="$LAUNCH" RUN_TIMEOUT=600 run_game lang_default_en "${APPENV[@]}" ROTK_SKIP_MOVIES=1 \
  ROTK_PAD_SCRIPT="$ROOT/tools/loc/tests/ua_menu.pad" ROTK_SHOT_FRAMES=350 ROTK_EXIT_AFTER=400
assert_exit0; assert_no_fatal
ocr "$(shot 350)"
has "fresh profile" "Resume"; hasnt "fresh profile" "Продовжити"

S="$T/player/home/Library/Application Support/ROTK/settings.toml"
mkdir -p "$(dirname "$S")"; printf '[launcher]\nlanguage = "uk"\n' >> "$S"
BIN="$LAUNCH" RUN_TIMEOUT=600 run_game lang_default_menu "${APPENV[@]}" ROTK_SKIP_MOVIES=1 \
  ROTK_PAD_SCRIPT="$ROOT/tools/loc/tests/ua_menu.pad" ROTK_SHOT_FRAMES=350,750 ROTK_EXIT_AFTER=800
assert_exit0; assert_no_fatal
ocr "$(shot 350)" "$(shot 750)"
has "language uk, pause menu" "Продовжити"; has "language uk, main menu" "Один гравець"
hasnt "language uk" "Resume"

SH=""; for ((f = 1300; f <= 5800; f += 150)); do SH="$SH${SH:+,}$f"; done
printf '1100 press START\n' > "$T/start.pad"
BIN="$LAUNCH" RUN_TIMEOUT=600 run_game lang_default_subs "${APPENV[@]}" ROTK_PAD_SCRIPT="$T/start.pad" \
  ROTK_SHOT_FRAMES="$SH" ROTK_EXIT_AFTER=5810
assert_exit0; assert_no_fatal
assert_log "loc: subtitles on by default" "subtitles not switched on for Ukrainian"
shots=(); for f in ${SH//,/ }; do shots+=("$(shot $f)"); done
ocr "${shots[@]}"
SEMANTIC=$((SEMANTIC + 1))
python3 -I -c '
import re, sys
n = sum(1 for l in open(sys.argv[1], encoding="utf-8", errors="replace")
        if re.search("[А-Яа-яІіЇїЄєҐґ]{4,}", l.split("\t", 1)[-1]))
print(f"  cyrillic lines in the cinematic: {n}")
sys.exit(0 if n >= 1 else 1)' "$RUN/ocr.tsv" || fail "no Ukrainian subtitle in the intro cinematic ($RUN/ocr.tsv)"
rm -rf "$T"
finish
