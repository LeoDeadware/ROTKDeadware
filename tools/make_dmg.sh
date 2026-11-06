#!/bin/bash
# ROTK.dmg: an unsigned disk image with ROTK.app WITHOUT game data (addendum h, 2026-11-06).
#   tools/make_dmg.sh [build_dir] [out.dmg]      (default: out/dmg/ROTK.dmg)
# The app is install_app.sh in DMG mode (ROTK_DMG=1): no Resources/game, no disc icon, mods = the ready
# HD packs only (R-L5, below);
# Resources/import has our importer (tools/dmg/import_disc.py, XboxPortKit xiso.py, tools/loc and
# our table). On the first run the launcher asks for the player's own disc image and imports it into
# ~/Library/Application Support/ROTK/data (game + the Ukrainian layer).
# No game code either (coordinator 2026-11-06): Resources/build has our lifter, runtime libraries and
# headers; the first run installs Apple Command Line Tools if missing, lifts and compiles the
# player's own default.xbe and puts the result in ~/Desktop/ROTK.app (tools/dmg/build_game.sh).
# tools/release_check.sh and tests/scripts/dmg_no_game_code.sh gate the image.
# Unsigned: the first open is right-click → Open (README "Read me first.txt" inside the image).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/out}"
DMG="${2:-$ROOT/out/dmg/ROTK.dmg}"
HD_FROM="${HD_FROM:-$HOME/Desktop/ROTK.app/Contents/Resources/mods}"
HD_FILES="${HD_FILES:-[0-9a-f]+\.png|manifest\.tsv}"
STAGE="$ROOT/.cache/dmg-stage"
export TMPDIR="$ROOT/.cache/tmp"; mkdir -p "$TMPDIR" "$(dirname "$DMG")"
rm -rf "$STAGE"; mkdir -p "$STAGE"
ROTK_DMG=1 ROTK_APP="$STAGE/ROTK.app" bash "$ROOT/tools/install_app.sh" "$BUILD" | tail -3

# Ready HD packs (RULES R-L5, owner 2026-11-06: «просто кладемо текстури в архів»; kit
# template/tools/make_dmg.sh): mods/hd_* copied from the app the owner tested (HD_FROM), only the
# packs' own files; the launcher points the game at them (ROTK_HDUI / ROTK_HDWORLD).
APP="$STAGE/ROTK.app"; MODS="$APP/Contents/Resources/mods"; mkdir -p "$MODS"
want="$(grep -oE '^hd_[a-z0-9_]+' "$ROOT/tools/install.d/expect.txt" 2>/dev/null || true)"
for d in "$HD_FROM"/hd_*; do
  [[ -d "$d" ]] || continue
  n="$(basename "$d")"; mkdir -p "$MODS/$n"
  find "$d" -maxdepth 1 -type f | { grep -E "/($HD_FILES)\$" || true; } | while read -r f; do cp -c "$f" "$MODS/$n/"; done
  echo "hd pack $n: $(ls "$MODS/$n" | wc -l | tr -d ' ') files"
done
for n in ${want:-hd_ui hd_world}; do
  [[ -n "$(ls -A "$MODS/$n" 2>/dev/null)" ]] || { echo "make_dmg: FAIL — HD pack $n not in $HD_FROM"; exit 1; }
done
codesign --force --deep -s - "$APP" >/dev/null 2>&1 || echo "warning: ad-hoc signing failed"
ln -s /Applications "$STAGE/Applications"
cat > "$STAGE/Read me first.txt" <<'TXT'
ROTK for Mac (unofficial port, not signed by Apple)

1. Drag ROTK.app to Applications.
2. First open: right-click ROTK.app -> Open -> Open (macOS Gatekeeper: the app is not signed by
   Apple; after that it opens normally).
3. Choose your own The Return of the King disc image (Xbox, .iso, US). The EU (PAL) disc is untested.
4. Apple Command Line Tools are needed: if they are missing, ROTK opens the system dialog - click
   Install. ROTK then builds the game from your disc on this Mac (a few minutes, a progress window)
   and puts ROTK.app on the Desktop. The game data and the game code come only from your disc.
5. The language (English or Українська) is chosen in the launcher window; English by default.

ROTK для Mac (неофіційний порт, без підпису Apple)

1. Перетягніть ROTK.app у «Applications».
2. Перший запуск: клацніть ROTK.app правою кнопкою → «Відкрити» → «Відкрити»
   (Gatekeeper: програма без підпису Apple; далі відкривається звичайно).
3. Оберіть образ свого диска The Return of the King (Xbox, .iso, США).
   Європейський диск (PAL) — не перевірено.
4. Потрібні Command Line Tools від Apple: якщо їх немає, ROTK сам відкриє системне вікно —
   натисніть «Встановити». Далі ROTK збирає гру з вашого диска (кілька хвилин, вікно з
   прогресом) і кладе готову ROTK.app на робочий стіл.
5. Мову (English або Українська) обирають у вікні лаунчера; за замовчуванням — англійська.
TXT
rm -f "$DMG"
hdiutil create -quiet -fs HFS+ -volname "ROTK" -srcfolder "$STAGE" -format UDZO "$DMG"
ALLOW_HD_PACKS=1 bash "$ROOT/tools/release_check.sh" "$DMG"
echo "dmg: $DMG ($(du -h "$DMG" | cut -f1))"
