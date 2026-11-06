#!/bin/bash
# HD packs in the DMG (owner 2026-11-06: «просто кладемо текстури в архів»): the image carries the
# ready-made mods/hd_ui + hd_world; after the DMG install hd_world_levels passes on the installed
# app (all 17 levels) and the game loads both packs from the app.
#   ROTK_DMG_FILE=<dmg>: test that image instead of building one (make_dmg.sh).
source "$(dirname "$0")/../lib.sh"
ISO="${ROTK_ISO:-$HOME/Desktop/Claude/Deadware/Port ISOs/The Lord of the Rings The Return of the King.iso}"
[[ -f "$ISO" ]] || { echo "dmg_hd: no disc image $ISO (skip)"; exit 2; }
T="${ROTK_CACHE:-$ROOT/.cache}/runs/dmg_hd-$$"; rm -rf "$T"; mkdir -p "$T/mnt"
DMG="${ROTK_DMG_FILE:-$T/ROTK.dmg}"
SEMANTIC=$((SEMANTIC + 1))
if [[ -z "${ROTK_DMG_FILE:-}" ]] && ! nice -n 19 bash "$ROOT/tools/make_dmg.sh" "$BUILD" "$DMG" > "$T/make_dmg.log" 2>&1; then
  fail "make_dmg.sh failed ($T/make_dmg.log)"; finish
fi
hdiutil attach -quiet -nobrowse -readonly -mountpoint "$T/mnt" "$DMG" || { fail "cannot mount $DMG"; finish; }
cp -R "$T/mnt/ROTK.app" "$T/ROTK.app"
hdiutil detach -quiet "$T/mnt"
unset ROTK_PLAYER_ROOT
export RUN_HOME="$T/home"
t0=$(date +%s)
BIN="$T/ROTK.app/Contents/MacOS/ROTK" RUN_TIMEOUT=900 run_game dmg_hd_install ROTK_DATA= ROTK_IMPORT_ISO="$ISO" \
  ROTK_INSTALL_APP="$T/installed/ROTK.app" ROTK_NO_UI=1 ROTK_TEST_PROFILE=1 ROTK_EXIT_AFTER=300
assert_exit0
echo "  install (import + build + 300 frames): $(( $(date +%s) - t0 )) s"
SEMANTIC=$((SEMANTIC + 1))
APP="$T/installed/ROTK.app" bash "$ROOT/tests/scripts/hd_world_levels.sh" > "$T/levels.log" 2>&1 ||
  fail "hd_world_levels on the installed app: $(grep -m3 FAIL "$T/levels.log" | tr '\n' ' ')"
# the game loads the packs from the app (the normal profile; the test profile keeps the originals)
BIN="$T/installed/ROTK.app/Contents/MacOS/ROTK" RUN_TIMEOUT=300 run_game dmg_hd ROTK_DATA= ROTK_NO_UI=1 \
  ROTK_TEST_PROFILE=0 ROTK_EXIT_AFTER=300
assert_exit0
SEMANTIC=$((SEMANTIC + 1))
L="$(cat "$RUN"/*.log "$RUN_HOME/Library/Application Support/ROTK/player/logs/"*.log 2>/dev/null)"
for p in hd_ui hd_world; do
  grep -q "pack $T/installed/ROTK.app/Contents/MacOS/../Resources/mods/$p" <<< "$L" || fail "the game did not load $p from the app"
done
du -sh "$RUN_HOME/Library/Application Support/ROTK/data" "$T/installed/ROTK.app" | sed 's/^/  disk: /'
(( ${#FAILS[@]} )) || rm -rf "$T"
finish
