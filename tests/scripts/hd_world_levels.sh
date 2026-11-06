#!/bin/bash
# HD world textures for all 17 levels (owner phase 2: "HD для решти 13 рівнів"): the world census
# (tools/hdworld/dump.sh via install.d/30-hd.sh) has every level in coverage.tsv, and the pack
# of the installed app (APP, default ~/Desktop/ROTK.app) has textures.
source "$(dirname "$0")/../lib.sh"
W="${WORLD:-$ROOT/out/hd/world}"; APP="${APP:-$HOME/Desktop/ROTK.app}"
SEMANTIC=$((SEMANTIC + 1))
got=$(grep -v '^#' "$W/coverage.tsv" 2>/dev/null | cut -f7 | tr ',' '\n' | sed 's/^[0-9]*_//; s/:.*//' | sort -u)
echo "  census levels ($(echo "$got" | grep -c .)): $(echo $got)"
for L in Hel01 Pat01 Pel02 Osg01 Pat03 Min02 Bla01 Ise01 Min00 Min04 She01 Cir00 Cra01 Cir01 Pat02 Pal01 Pal02; do
  grep -qx "$L" <<< "$got" || fail "no $L in the world census"
done
n=$(ls "$APP/Contents/Resources/mods/hd_world" 2>/dev/null | wc -l | tr -d ' ')
(( n > 0 )) || fail "no HD world pack in $APP"
finish
