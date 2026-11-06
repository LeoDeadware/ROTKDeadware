#!/bin/bash
# Ukrainian 5d: Cyrillic in the pause menu and the main menu (game run, ~800 frames).
#   tools/loc/tests/ua_menu.sh            builds the overlay (tools/loc/build_mod.py uk) and runs
#   UA_MODS=<dir> tools/loc/tests/ua_menu.sh   uses that overlay instead (UA_MODS=none: no overlay)
# Route (tools/loc/tests/ua_menu.pad, ROTK_SKIP_MOVIES=1): the pad script starts the Hel01
# prologue; START pauses (shot 350), Exit To Menu -> are you sure (shot 550) -> yes -> main menu
# (shot 750). Checks, by OCR of the shots (tools/loc/ocr.swift, Vision uk-UA + en-US):
#   pause menu: "Продовжити", "Параметри", "Вийти в меню"; main menu: "Один гравець", "Довідка";
#   none of the English "Resume", "Exit To Menu", "single player", "Help" left.
# Without the overlay (UA_MODS=none) the shots show English and the test fails.
source "$(dirname "$0")/../../../tests/lib.sh"
LOC="$ROOT/tools/loc"
TOOLS="$ROOT/out/loc/bin"; mkdir -p "$TOOLS"
for t in "ocr:$LOC/ocr.swift" "contact:$ROOT/tools/hdui/contact.swift"; do
  b="$TOOLS/${t%%:*}"; s="${t#*:}"
  [[ -x "$b" && "$b" -nt "$s" ]] || swiftc -O -o "$b" "$s" 2>&1 | tail -3
done
MODS="${UA_MODS:-}"
if [[ -z "$MODS" ]]; then
  MODS="$ROOT/out/loc/ua_menu_mod-$$"
  nice -n 19 python3 -I "$LOC/build_mod.py" uk --out "$MODS" | tail -1
fi
[[ "$MODS" == none ]] && MODS="$ROOT/out/loc/empty_mods" && mkdir -p "$MODS"
RUN_TIMEOUT=600 run_game loc_ua_menu ROTK_SKIP_MOVIES=1 ROTK_PAD_SCRIPT="$LOC/tests/ua_menu.pad" \
  ROTK_SHOT_FRAMES=350,550,750 ROTK_EXIT_AFTER=800 ROTK_MODS="$MODS"
[[ "$MODS" == "$ROOT/out/loc/ua_menu_mod-$$" ]] && rm -rf "$MODS"   # APFS clones of the disc: not kept
assert_exit0; assert_no_fatal
assert_log "mods dir\(s\) .*" "the game did not report the mods dir"

"$TOOLS/ocr" "$(shot 350)" "$(shot 550)" "$(shot 750)" > "$RUN/ocr.tsv" 2>&1
"$TOOLS/contact" "$RUN/ua_menu.png" --cols 3 --width 640 "$(shot 350)=pause" "$(shot 550)=confirm" \
  "$(shot 750)=main menu" >/dev/null 2>&1
has() {  # frame text
  SEMANTIC=$((SEMANTIC + 1))
  grep -qF "$(basename "$(shot "$1")")	$2" "$RUN/ocr.tsv" || fail "frame $1: no \"$2\" on screen (OCR: $RUN/ocr.tsv)"
}
hasnt() {  # frame text
  SEMANTIC=$((SEMANTIC + 1))
  ! grep -qiF "$(basename "$(shot "$1")")	$2" "$RUN/ocr.tsv" || fail "frame $1: English \"$2\" still on screen"
}
has 350 "Продовжити"; has 350 "Параметри"; has 350 "Вийти в меню"
has 750 "Один гравець"; has 750 "Довідка"
hasnt 350 "Resume"; hasnt 350 "Exit To Menu"; hasnt 750 "single player"; hasnt 750 "Help"
note "contact sheet $RUN/ua_menu.png"
FLAKY=0 finish
