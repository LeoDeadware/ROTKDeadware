#!/bin/bash
# Installs the current build as ~/Desktop/ROTK.app ("The Lord of the Rings: The Return of the King", native):
# a real bundle whose executable is the Mach-O launcher tools/launcher/launcher.c (owner bug 2026-11-06).
# Player files (settings.toml, saves, logs) stay in ROTKPort/.cache/player (baked into the launcher).
# Removes the old layout: ROTKPort/dist and ~/Desktop/ROTK.command. tools/app_smoke.sh opens the result.
# Run by the orchestrator only: tools/lock.py install -- tools/install_app.sh [build_dir]
# (or via tools/integrate.sh --install, which then runs tools/app_smoke.sh on the result).
# - game data is an APFS clone (cp -c): zero bytes (until the first-run importer exists, R-L3);
# - overlays/packs are BUILT by tools/install.d/*.sh, never copied as sources (R-I3).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/out}"
BIN="$BUILD/src/loader/rotk"
APP="${ROTK_APP:-$HOME/Desktop/ROTK.app}"   # ROTK_APP: a test install elsewhere (keeps the old layout)
C="$APP/Contents"
RES="$C/Resources"
DATA="${ROTK_DATA:-$ROOT}"
[[ -x "$BIN" ]] || { echo "no $BIN — build first"; exit 2; }

mkdir -p "$C/MacOS" "$C/Frameworks" "$RES/game"
cp "$BIN" "$C/MacOS/rotk-bin"
otool -L "$C/MacOS/rotk-bin" | awk 'NR>1{print $1}' | grep -vE '^(/usr/lib|/System)' | while read -r lib; do
  real="${lib/@rpath\//$BUILD/}"
  [[ -f "$real" ]] || real="$(find "$BUILD" /opt/homebrew/lib -name "$(basename "$lib")" -print -quit 2>/dev/null)"
  [[ -f "$real" ]] || { echo "warning: cannot find $lib"; continue; }
  dst="$C/Frameworks/$(basename "$lib")"; rm -f "$dst"; cp -L "$real" "$dst"; chmod u+w "$dst"
  install_name_tool -change "$lib" "@executable_path/../Frameworks/$(basename "$lib")" "$C/MacOS/rotk-bin" 2>/dev/null || true
done
DMG="${ROTK_DMG:-0}"   # 1: the DMG build (tools/make_dmg.sh): no game data, first-run import instead
if [[ $DMG == 1 ]]; then
  rm -rf "$RES/game" "$RES/mods"; mkdir -p "$RES/import/loc"
  # No game code in the DMG (coordinator 2026-11-06): the player's Mac lifts and compiles it on
  # the first run (Resources/build: our lifter, runtime libraries and headers; tools/dmg/kit.py).
  rm -f "$C/MacOS/rotk-bin"
  python3 -I "$ROOT/tools/dmg/kit.py" "$BUILD" "$RES/build"
  cp "$ROOT"/tools/dmg/{build_game.sh,clt.sh,progress.js} "$ROOT/tools/launcher/make_art.sh" "$RES/build/"
  # the art upscaler (MetalFX; Real-ESRGAN is not shipped): tools/launcher/make_art.sh looks next to itself
  swiftc -O -o "$RES/build/upscale" "$ROOT/tools/hdui/upscale.swift"
  # the app icon: made on the first run from the player's disc (build_game.sh -> make_icns.sh)
  cp "$ROOT"/tools/icon/{make_icns.sh,xbe_title_image.py} "$RES/build/"
  swiftc -O -o "$RES/build/squircle" "$ROOT/tools/icon/squircle.swift"
  mkdir -p "$RES/launcher"; cp "$ROOT/tools/launcher/launcher.js" "$RES/launcher/"   # art: made on the first run
  cp "$ROOT"/tools/dmg/import_disc.py "$RES/import/"
  cp "$ROOT/third_party/xiso.py" "$RES/import/" 2>/dev/null ||
    cp "$ROOT/../XboxPortKit/portkit/xiso.py" "$RES/import/"   # ROTKPort lives in Deadware/ (2026-11-06)
  cp "$ROOT"/tools/loc/{build_mod,extract,fnts,scx,textbank}.py "$RES/import/loc/"
  cp "$ROOT"/tools/loc/glyphs.swift "$RES/import/loc/"
  cp "$ROOT/mods/loc/uk/text.tsv" "$RES/import/"
  # Cyrillic glyph renders (our CoreText output, no game data): a player's Mac may lack swift.
  T="$(mktemp -d "$ROOT/.cache/tmp/glyphs.XXXX")"
  ROTK_GLYPH_CACHE="$RES/import/loc/glyphcache" python3 -I "$ROOT/tools/loc/build_mod.py" uk \
    --data "$DATA" --out "$T/uk" --table "$ROOT/mods/loc/uk/text.tsv" > "$T/log" 2>&1 || { tail -5 "$T/log"; exit 1; }
  rm -rf "$T"
elif [[ ! -f "$RES/game/extracted/default.xbe" ]]; then
  rm -rf "$RES/game/extracted"; cp -c -R "$DATA/game/extracted" "$RES/game/extracted"
fi
[[ $DMG == 1 ]] || { rm -rf "$RES/mods"; mkdir -p "$RES/mods"; }
[[ $DMG == 1 ]] || for step in "$ROOT"/tools/install.d/*.sh; do
  [[ -f "$step" ]] || continue
  echo "install step $(basename "$step")"; bash "$step" "$ROOT" "$BUILD" "$RES"
done

# The bundle executable is a small Mach-O (tools/launcher/launcher.c): env, log, language, then
# execv(rotk). A bash script there could not read the app's files when opened from Finder (TCC).
rm -f "$C/MacOS/launch" "$C/MacOS/rotk"   # old layout; rotk vs ROTK clash on a case-insensitive disk
PLAYER="$ROOT/.cache/player"; [[ $DMG == 1 ]] && PLAYER=""   # DMG: ~/Library/Application Support/ROTK
clang -O2 -DROTK_PLAYER_ROOT="\"$PLAYER\"" -o "$C/MacOS/ROTK" "$ROOT/tools/launcher/launcher.c" -framework CoreGraphics -framework CoreFoundation
# The icon: the disc's title image (XBE $$XTIMAGE), built here, never committed.
[[ $DMG == 1 ]] || bash "$ROOT/tools/icon/make_icns.sh" "$RES/game/extracted/default.xbe" "$RES/ROTK.icns" || echo "warning: no icon"
cat > "$C/Info.plist" <<EOS
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>ROTK</string>
  <key>CFBundleDisplayName</key><string>The Lord of the Rings: The Return of the King</string>
  <key>CFBundleIdentifier</key><string>local.rotkport.game</string>
  <key>CFBundleExecutable</key><string>ROTK</string>
  <key>CFBundleIconFile</key><string>ROTK</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>GCSupportsControllerUserInteraction</key><true/>
</dict></plist>
EOS
touch "$APP"   # Finder re-reads the icon
codesign --force --deep -s - "$APP" >/dev/null 2>&1 || echo "warning: ad-hoc signing failed"
if [[ -z "${ROTK_APP:-}" ]]; then   # the old layout: dist/ROTK.app + the Terminal shortcut
  rm -f "$HOME/Desktop/ROTK.command"
  rm -rf "$ROOT/dist"
fi
echo "installed $APP ($(git -C "$ROOT" rev-parse --short HEAD))"
