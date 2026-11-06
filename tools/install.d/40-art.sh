#!/bin/bash
# The launcher window (tools/launcher/launcher.js) and its art: the game's title screen shot from
# the app's disc copy (tools/launcher/make_art.sh; game-derived, never in git).
set -euo pipefail
ROOT=$1 BUILD=$2 RES=$3
mkdir -p "$RES/launcher"
cp "$ROOT/tools/launcher/launcher.js" "$RES/launcher/"
bash "$ROOT/tools/launcher/make_art.sh" "$BUILD/src/loader/rotk" "$RES" "$RES/launcher/art.png" || echo "warning: no launcher art"
