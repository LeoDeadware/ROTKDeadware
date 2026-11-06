#!/bin/bash
# HD texture packs (phase 5c): builds mods/hd_ui (menus, HUD, font atlases, button glyphs) and
# mods/hd_world (the most visible level textures) into $RES/mods from the game's own textures:
#   tools/install.d/30-hd.sh ROOT BUILD RES
# The census runs (tools/hdui/dump.sh, tools/hdworld/dump.sh: silent, nice 19, one at a time)
# are cached in out/hd and reused (HD_REDUMP=1 redoes them); the upscale is local
# (tools/hdui/hdlib.py: MetalFX, or Real-ESRGAN when installed). Derived from game data: never
# in git (R-L2). The game loads both packs by default; ROTK_TEST_PROFILE=1 keeps the originals.
set -euo pipefail
ROOT=$1 BUILD=$2 RES=$3
C="${HD_CACHE:-$ROOT/out/hd}"
HD_UA_MODS="${HD_UA_MODS:-$RES/mods/uk}" bash "$ROOT/tools/hdui/dump.sh" "$BUILD" "$C/census"
# All 17 levels (addendum f, 2026-11-06; was Hel01 Pat01 Pel02 Osg01): HD_LEVELS overrides.
read -r -a LEVELS <<< "${HD_LEVELS:-Hel01 Pat01 Pel02 Osg01 Pat03 Min02 Bla01 Ise01 Min00 Min04 She01 Cir00 Cra01 Cir01 Pat02 Pal01 Pal02}"
bash "$ROOT/tools/hdworld/dump.sh" "$BUILD" "$C/world" "${LEVELS[@]}"
rm -rf "$RES/mods/hd_ui" "$RES/mods/hd_world"
# Engines (owner approved Real-ESRGAN 2026-11-06): menus, HUD, fonts and glyphs stay on MetalFX
# (x4plus invents strokes on text and line art); world textures take Real-ESRGAN when installed
# (more detail on stone, wood, cloth: tools/hdworld/compare_sheet.sh shows both).
HD_ENGINE="${HD_UI_ENGINE:-metalfx}" python3 -I "$ROOT/tools/hdui/build_pack.py" "$C/census" --out "$RES/mods/hd_ui"
python3 -I "$ROOT/tools/hdworld/build_world.py" "$C/world" --out "$RES/mods/hd_world" --ui "$C/census/ui2d.tsv" \
  --top "${HD_TOP:-600}"
