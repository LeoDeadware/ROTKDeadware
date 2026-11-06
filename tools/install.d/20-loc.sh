#!/bin/bash
# Install step (Localizer): the Ukrainian layer Resources/mods/uk of the one ROTK.app.
# Built from the app's own disc copy (Resources/game/extracted) and our table mods/loc/uk/text.tsv:
# UI text + subtitles + Cyrillic fonts (tools/loc/build_mod.py); level files are APFS clones with
# only the changed pages written. Switched on at run time by ROTK_LANG=uk (the launch script maps it
# to ROTK_MODS=Resources/mods/uk; see docs/requests/localizer-to-infra-2026-11-06.md); en = off.
set -euo pipefail
ROOT="$1"; RES="$3"
python3 -I "$ROOT/tools/loc/build_mod.py" uk --data "$RES" --out "$RES/mods/uk" \
  --table "$ROOT/mods/loc/uk/text.tsv"
