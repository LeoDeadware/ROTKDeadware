#!/bin/bash
# Polish 5c: HD packs. The .app's install step tools/install.d/30-hd.sh BUILDS an HD UI pack
# (menus, HUD, font atlases, button glyphs; tools/hdui) and an HD world pack (the most visible
# level textures; tools/hdworld) into Resources/mods/hd_ui and hd_world; the game loads them by
# default; ROTK_TEST_PROFILE=1 keeps the originals (goldens unchanged).
#   tests/scripts/hd_pack.sh                 # build dir: runs the install step into a scratch Resources
#   APP=~/Applications/ROTK.app tests/scripts/hd_pack.sh    # the installed app (suite mode app)
# Checks:
#   1. the packs exist with a manifest (hash kind w h k): >= UI_MIN (20: the census finds 25 2D textures in 8 scenes; ROTK draws its level select in 3D) UI textures incl. >= 1 font
#      atlas and >= 1 button glyph, >= WORLD_MIN (40; 46 from 4 levels) world textures; every PNG is k x its original (k >= 2);
#   2. app mode (ROTK_TEST_PROFILE=0, the app's ROTK_MODS): Hel01 prologue + pause + options
#      (tools/hdui/tour.pad to frame 1500): both packs loaded ("mods: loaded hd_ui/hd_world"),
#      >= UI_USED (8) UI and >= WORLD_USED (15) world textures replaced, a font atlas among them,
#      no "not a multiple" refusal;
#   3. test profile (ROTK_TEST_PROFILE=1, same ROTK_MODS): no pack loaded, nothing replaced.
# On main (no install step, packs only from $ROTK_DATA/mods, BACKGROUND rule) 1 and 2 fail.
source "$(dirname "$0")/../lib.sh"
UI_MIN="${UI_MIN:-20}" WORLD_MIN="${WORLD_MIN:-40}" UI_USED="${UI_USED:-8}" WORLD_USED="${WORLD_USED:-15}"
PAD="$ROOT/tools/hdui/tour.pad"
if [[ -n "${APP:-}" ]]; then
  BIN="$APP/Contents/MacOS/ROTK"
  export ROTK_PLAYER_ROOT="${ROTK_CACHE:-$ROOT/.cache}/runs/hd_pack-player"   # never the owner's profile
  [[ -x "$BIN" ]] || { echo "hd_pack: no $BIN (install first)"; exit 2; }
  RES="$APP/Contents/Resources"
  APPDATA=(ROTK_DATA="$RES/game/..")
else
  RES="${HD_CACHE:-$ROOT/out/hd}/test_res-$$"
  APPDATA=()
  rm -rf "$RES"; mkdir -p "$RES/mods"
  STEP="$ROOT/tools/install.d/30-hd.sh"
  if [[ -f "$STEP" ]]; then
    bash "$STEP" "$ROOT" "$BUILD" "$RES" > "$RES/install.log" 2>&1 || fail "install step 30-hd.sh failed: $(tail -3 "$RES/install.log")"
    tail -4 "$RES/install.log" | sed 's/^/  install: /'
  else
    fail "no tools/install.d/30-hd.sh (the .app does not build HD packs)"
  fi
fi
MODS="$RES/mods"

# 1. packs on disk
SEMANTIC=$((SEMANTIC + 1))
check_pack() {  # dir min kinds...
  local name="$1" d="$MODS/$1" min="$2"; shift 2
  [[ -f "$d/manifest.tsv" ]] || { fail "no $d/manifest.tsv"; return; }
  local n; n=$(grep -vc '^#' "$d/manifest.tsv")
  echo "  $name: $n textures ($(grep -v '^#' "$d/manifest.tsv" | cut -f2 | sort | uniq -c | xargs))"
  (( n >= min )) || fail "$name has $n textures (want >= $min)"
  for kind in "$@"; do
    grep -v '^#' "$d/manifest.tsv" | cut -f2 | grep -qx "$kind" || fail "$name has no '$kind' texture"
  done
  local bad; bad=$(awk -F'\t' '!/^#/ && $5 < 2' "$d/manifest.tsv" | wc -l | tr -d ' ')
  (( bad == 0 )) || fail "$name: $bad textures not upscaled (k < 2)"
  local missing; missing=$(awk -F'\t' '!/^#/{print $1}' "$d/manifest.tsv" | while read -r h; do
    [[ -f "$d/$h.png" ]] || echo "$h"; done | wc -l | tr -d ' ')
  (( missing == 0 )) || fail "$name: $missing PNGs of the manifest missing"
}
check_pack hd_ui "$UI_MIN" font glyph
check_pack hd_world "$WORLD_MIN"

# 2. app mode
RUN_TIMEOUT=900 run_game hd_pack_on ROTK_TEST_PROFILE=0 ROTK_MODS="$MODS" "${APPDATA[@]+"${APPDATA[@]}"}" \
  ROTK_SKIP_MOVIES=1 ROTK_PAD_SCRIPT="$PAD" ROTK_SHOT_FRAMES=600,1100,1400 ROTK_EXIT_AFTER=1500
assert_exit0; assert_no_fatal
assert_log "mods: loaded hd_ui" "HD UI pack not loaded in app mode"
assert_log "mods: loaded hd_world" "HD world pack not loaded in app mode"
SEMANTIC=$((SEMANTIC + 1))
ui=$(grep -cE "polish: hd_ui [0-9a-f]{16}\.png .* -> " "$LOG"); world=$(grep -cE "polish: hd_world [0-9a-f]{16}\.png .* -> " "$LOG")
echo "  app mode: $ui UI and $world world textures replaced"
(( ui >= UI_USED )) || fail "$ui UI textures replaced (want >= $UI_USED)"
(( world >= WORLD_USED )) || fail "$world world textures replaced (want >= $WORLD_USED)"
fonts=0
if [[ -f "$MODS/hd_ui/manifest.tsv" ]]; then
  for h in $(awk -F'\t' '$2 == "font" {print $1}' "$MODS/hd_ui/manifest.tsv"); do
    grep -q "polish: hd_ui $h.png" "$LOG" && fonts=$((fonts + 1))
  done
fi
(( fonts >= 1 )) || fail "no HD font atlas in use in app mode"
grep -q "not a multiple" "$LOG" && fail "pack images refused (not a multiple of the original)"
ON_RUN="$RUN"

# 3. test profile
RUN_TIMEOUT=900 run_game hd_pack_test ROTK_MODS="$MODS" "${APPDATA[@]+"${APPDATA[@]}"}" \
  ROTK_SKIP_MOVIES=1 ROTK_PAD_SCRIPT="$PAD" ROTK_SHOT_FRAMES=600,1100,1400 ROTK_EXIT_AFTER=1500
assert_exit0; assert_no_fatal
SEMANTIC=$((SEMANTIC + 1))
grep -qE "polish: hd_(ui|world) pack|mods: loaded hd_" "$LOG" && fail "HD pack loaded under ROTK_TEST_PROFILE=1"
grep -qE "polish: hd_(ui|world) [0-9a-f]{16}\.png" "$LOG" && fail "textures replaced under ROTK_TEST_PROFILE=1"
echo "  runs: on $ON_RUN, test profile $RUN"
[[ -z "${APP:-}" && "${KEEP_RES:-0}" != 1 && ${#FAILS[@]} == 0 ]] && rm -rf "$RES"
finish
