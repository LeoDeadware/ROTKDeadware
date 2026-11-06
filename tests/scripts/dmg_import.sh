#!/bin/bash
# Addendum h (2026-11-06): tools/make_dmg.sh makes ROTK.dmg without game data, and its app imports
# the player's own disc on the first run.
#   1. make_dmg.sh succeeds; release_check passes (inside make_dmg); the image has ROTK.app with the
#      Mach-O launcher, "Read me first.txt" (right-click → Open), the Applications link, no game data;
#   2. a copy of the app run with a fresh HOME and ROTK_IMPORT_ISO=<our disc> (the dialog skipped):
#      the disc lands in ~/Library/Application Support/ROTK/data (game + mods/uk), the game starts
#      and exits 0 at frame 300;
#   3. (coordinator 2026-11-06) the image carries no game code: the first run builds it with
#      Command Line Tools into ROTK_INSTALL_APP (here the run dir; the player's ~/Desktop/ROTK.app).
source "$(dirname "$0")/../lib.sh"
ISO="${ROTK_ISO:-$HOME/Desktop/Claude/Deadware/Port ISOs/The Lord of the Rings The Return of the King.iso}"
[[ -f "$ISO" ]] || { echo "dmg_import: no disc image $ISO (skip)"; exit 2; }
T="${ROTK_CACHE:-$ROOT/.cache}/runs/dmg_import-$$"; rm -rf "$T"; mkdir -p "$T/mnt"
DMG="$T/ROTK.dmg"
SEMANTIC=$((SEMANTIC + 1))
if ! nice -n 19 bash "$ROOT/tools/make_dmg.sh" "$BUILD" "$DMG" > "$T/make_dmg.log" 2>&1; then
  fail "make_dmg.sh failed ($T/make_dmg.log)"; finish
fi
hdiutil attach -quiet -nobrowse -readonly -mountpoint "$T/mnt" "$DMG" || { fail "cannot mount $DMG"; finish; }
SEMANTIC=$((SEMANTIC + 1))
file "$T/mnt/ROTK.app/Contents/MacOS/ROTK" | grep -q Mach-O || fail "no Mach-O launcher in the image"
[[ -f "$T/mnt/Read me first.txt" ]] && grep -q "Відкрити" "$T/mnt/Read me first.txt" || fail "no right-click → Open note"
[[ -L "$T/mnt/Applications" ]] || fail "no Applications link"
[[ -e "$T/mnt/ROTK.app/Contents/Resources/game" ]] && fail "game data in the image"
[[ -e "$T/mnt/ROTK.app/Contents/MacOS/rotk-bin" ]] && fail "game code in the image"
[[ -x "$T/mnt/ROTK.app/Contents/Resources/build/build_game.sh" || -f "$T/mnt/ROTK.app/Contents/Resources/build/build_game.sh" ]] \
  || fail "no build kit in the image"
ls "$T/mnt/ROTK.app/Contents/Resources/import/loc/glyphcache/"*.bin >/dev/null 2>&1 \
  || fail "no prebuilt Cyrillic glyphs (a player's Mac may have no swift)"
cp -R "$T/mnt/ROTK.app" "$T/ROTK.app"
hdiutil detach -quiet "$T/mnt"

unset ROTK_PLAYER_ROOT
BIN="$T/ROTK.app/Contents/MacOS/ROTK" RUN_TIMEOUT=900 run_game dmg_import ROTK_DATA= ROTK_IMPORT_ISO="$ISO" \
  ROTK_INSTALL_APP="$T/installed/ROTK.app" ROTK_NO_UI=1 ROTK_TEST_PROFILE=1 ROTK_SHOT_FRAMES=300 ROTK_EXIT_AFTER=300
assert_exit0; assert_no_fatal
D="$RUN/home/Library/Application Support/ROTK"
SEMANTIC=$((SEMANTIC + 1))
[[ -f "$D/data/game/extracted/default.xbe" ]] || fail "the disc was not imported into $D/data"
[[ -d "$D/data/mods/uk" ]] || fail "no Ukrainian layer built on import"
grep -q "import: done" "$D/player/logs/rotk.log" 2>/dev/null || fail "no 'import: done' in the player log"
[[ -f "$(shot 300)" ]] || fail "the game did not reach frame 300"
[[ -x "$T/installed/ROTK.app/Contents/MacOS/rotk-bin" ]] || fail "no game built into ROTK_INSTALL_APP"
[[ -s "$T/installed/ROTK.app/Contents/Resources/ROTK.icns" ]] || fail "no app icon made from the disc (tools/icon/make_icns.sh)"
grep -q "build_game: \[100%\] Готово" "$D/player/logs/rotk.log" 2>/dev/null || note "no build 'done' line in the player log"
rm -rf "$D/data" "$T/ROTK.app" "$T/installed"   # 2 GB of the player's disc: not kept
finish
