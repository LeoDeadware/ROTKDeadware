#!/bin/bash
# Owner bugs 2026-11-06 (680a23b): "саби ... в низькій роздільності", "іконки кнопок (А тощо) досі
# в низькій роздільній здатності", the stats screen's decorative font, "іконки персонажів в низькому
# розширенні". The UA layer's GlobScen fonts (Cyrillic glyphs; the A/X glyphs inside text are font 0
# glyphs) have their own hashes, which the English-only census never saw; the hero portraits other
# than Hel01's were never drawn in a census scene. App mode, the pack PACK (default: the app's
# mods/hd_ui) with the Ukrainian layer UA (default: the app's mods/uk):
#   1. Hel01 prologue to the pause menu (tools/hdui/tour.pad), ROTK_LANG=uk: >= 2 font atlases of
#      the pack replaced (UA fonts 0 and 1/2 drawn there);
#   2. Pat01 as Aragorn (scene select, walk pad, HUD at 3200): >= 1 pack texture of a hero_* census
#      scene (the portraits) replaced.
source "$(dirname "$0")/../lib.sh"
APP="${APP:-$HOME/Desktop/ROTK.app}"
PACK="${PACK:-$APP/Contents/Resources/mods/hd_ui}" UA="${UA:-$APP/Contents/Resources/mods/uk}"
[[ -f "$PACK/manifest.tsv" && -d "$UA" ]] || { echo "hd_ua: no $PACK or $UA"; exit 2; }
COMMON=(ROTK_TEST_PROFILE=0 ROTK_MODS="$UA" ROTK_LANG=uk ROTK_HDUI="$PACK" ROTK_HDWORLD=0)
RUN_TIMEOUT=900 run_game hd_ua_menu "${COMMON[@]}" ROTK_SKIP_MOVIES=1 \
  ROTK_PAD_SCRIPT="$ROOT/tools/hdui/tour.pad" ROTK_SHOT_FRAMES=600,1100 ROTK_EXIT_AFTER=1200
assert_exit0; assert_no_fatal
SEMANTIC=$((SEMANTIC + 1))
fonts=0
for h in $(awk -F'\t' '$2 == "font" {print $1}' "$PACK/manifest.tsv"); do grep -q "polish: hd_ui $h.png" "$LOG" && fonts=$((fonts + 1)); done
echo "  ua menus: $fonts HD font atlases in use, $(grep -cE 'polish: hd_ui [0-9a-f]{16}\.png .* -> ' "$LOG") HD UI textures"
(( fonts >= 2 )) || fail "Ukrainian fonts not HD ($fonts font atlases of the pack in use, want >= 2)"
T="$(mktemp -d "$ROOT/.cache/tmp/hdua.XXXX")"
for ((g = 2600; g < 3200; g += 300)); do echo "$g stick L 0 1 120"; echo "$((g + 60)) press X"; done > "$T/pad.txt"
RUN_TIMEOUT=900 run_game hd_ua_hero "${COMMON[@]}" ROTK_SKIP_MOVIES=1 ROTK_PAD_SCRIPT="$T/pad.txt" \
  ROTK_CALL_AT="1600:poke:16D584:0,1600:9D770/1,1600:9C8A0/0" ROTK_SHOT_FRAMES=3200 ROTK_EXIT_AFTER=3210
rm -rf "$T"
assert_exit0; assert_no_fatal
SEMANTIC=$((SEMANTIC + 1))
hero=0
for h in $(awk -F'\t' '!/^#/ && $8 ~ /hero_/ {print $1}' "$PACK/manifest.tsv"); do grep -q "polish: hd_ui $h.png" "$LOG" && hero=$((hero + 1)); done
echo "  Pat01 Aragorn: $hero HD textures of the hero census in use"
(( hero >= 1 )) || fail "hero HUD (portrait) not HD in Pat01 as Aragorn"
finish
