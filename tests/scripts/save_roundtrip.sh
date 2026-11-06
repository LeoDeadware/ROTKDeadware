#!/bin/bash
# Save round trip (PLAYBOOK phase 4): save -> exit -> load in a 2nd run with the same profile.
#   tests/scripts/save_roundtrip.sh          (~2 x 2.5 min, movies skipped)
#
# Route (probes mp2..mp6, 2026-11-02): ROTK_SKIP_MOVIES=1 boots straight into the Hel01
# prologue. Pause (START) -> "Exit To Menu" (4 x DOWN, A) -> "are you sure?" (DOWN = yes, A)
# -> main menu by ~1600. In the front end the hook FUN_0009BC70(6) sets UI mode 6 = the save
# screen (what the scene-select "want to save? yes" does, FUN_00107EC0 case 5); DOWN into the
# slot list, A saves slot 1 (no name prompt) -> UDATA/4541003e/<id>/rotk (0x5F74 bytes: up to
# 10 slots of the 0x988-byte game state block 0x1C4FE0, FUN_0009A950) and the game goes on to
# scene select. Before the save the hook writes two markers into the state block (after mode 6: entering the front end resets it) (stats 0xB /
# 0xC of player 0, +0x2C/+0x30; +0x220..0x240 are written by the save itself: date, flags).
# Run 2 (same HOME; with a save present the game boots into the main menu): main menu -> single player -> load game -> slot 1 -> A; the state block is
# dumped after the load and must equal slot 1 of the save file, markers included, and must
# differ from the same block dumped before the load (so the load, not chance, set it).
source "$(dirname "$0")/../lib.sh"
STATE=1C4FE0 STATE_LEN=988 SLOT_LEN=$((0x988))
M1=1C500C M1V=51A7 M2=1C5010 M2V=2E   # state block +0x2C / +0x30 (stats 0xB / 0xC, player 0)
SHARED="${ROTK_CACHE:-$HOME/Desktop/Claude/ROTKPort/.cache}/runs/save_home-$(date +%Y%m%d-%H%M%S)-$$-$RANDOM"
mkdir -p "$SHARED"
UDATA="$SHARED/Library/Application Support/ROTKPort/default/hdd/partition1/UDATA/4541003e"

to_menu() {  # pause -> Exit To Menu -> yes
  printf '%s\n' "600 press START" "700 press DOWN" "760 press DOWN" "820 press DOWN" \
    "880 press DOWN" "960 press A" "1060 press DOWN" "1120 press A"
}

# --- run 1: save -----------------------------------------------------------------------
PAD1="$(mktemp -t rotk_save_pad)"
{ to_menu; printf '%s\n' "2400 press DOWN" "2550 press A"; } > "$PAD1"
RUN_HOME="$SHARED" RUN_TIMEOUT=400 run_game save_rt1 ROTK_SKIP_MOVIES=1 \
  ROTK_PAD_SCRIPT="$PAD1" ROTK_SHOT_FRAMES=2000,2350,3300 ROTK_EXIT_AFTER=3400 \
  "ROTK_CALL_AT=2100:9BC70/6,2300:poke:$M1:$M1V,2300:poke:$M2:$M2V" \
  "ROTK_DUMP_AT=2350:$STATE:$STATE_LEN,3300:$STATE:$STATE_LEN"
cp "$PAD1" "$RUN/pad.txt"; rm -f "$PAD1"
RUN1="$RUN"
assert_exit0
assert_no_fatal
assert_log "Fro03.scx" "front end (Fro03) never loaded: Exit To Menu failed"
assert_log "ROTK_CALL_AT 0009BC70 returned" "save mode call did not run"
SAVE="$(find "$UDATA" -name rotk -type f 2>/dev/null | head -1)"
SEMANTIC=$((SEMANTIC + 1))
if [[ -z "$SAVE" ]]; then
  fail "no save container written (UDATA/4541003e/*/rotk)"
  finish
fi
size=$(stat -f %z "$SAVE")
(( size == 0x5F74 )) || fail "save file is $size bytes, expected 24436"
echo "  saved: ${SAVE#"$SHARED/"} ($size bytes)"
cp "$SAVE" "$RUN1/save_rotk.bin"

# --- run 2: load -----------------------------------------------------------------------
PAD2="$(mktemp -t rotk_load_pad)"
# with a save on the disk the game boots into the main menu (no prologue)
# title "press start" -> START -> main menu -> A single player -> A load game -> DOWN, A slot 1 -> A
printf '%s\n' "900 press START" "1200 press A" "1500 press A" "1800 press DOWN" "1950 press A" "2250 press A" > "$PAD2"
RUN_HOME="$SHARED" RUN_TIMEOUT=400 run_game save_rt2 ROTK_SKIP_MOVIES=1 \
  ROTK_PAD_SCRIPT="$PAD2" ROTK_SHOT_FRAMES=850,1150,1450,1750,1900,2200,2500,2800,3100 ROTK_EXIT_AFTER=3200 \
  "ROTK_DUMP_AT=850:$STATE:$STATE_LEN,2200:$STATE:$STATE_LEN,2500:$STATE:$STATE_LEN,3150:$STATE:$STATE_LEN"
cp "$PAD2" "$RUN/pad.txt"; rm -f "$PAD2"
assert_exit0
assert_no_fatal
SEMANTIC=$((SEMANTIC + 1))
python3 -I - "$RUN1/save_rotk.bin" "$RUN/dump_00${STATE}_"*.bin <<'EOF' || fail "loaded state != saved slot (see above)"
import struct, sys, re
save, dumps = sys.argv[1], sorted(sys.argv[2:], key=lambda p: int(re.search(r"_(\d+)\.bin$", p).group(1)))
slot = open(save, "rb").read()[:0x988]
if len(dumps) < 2:
    print(f"  load: {len(dumps)} state dumps in run 2"); sys.exit(1)
before = open(dumps[0], "rb").read()
after = max((open(p, "rb").read() for p in dumps[1:]), key=lambda b: sum(x == y for x, y in zip(b, slot)))
m = lambda b: (struct.unpack_from("<I", b, 0x2C)[0], struct.unpack_from("<I", b, 0x30)[0])
diff = [i for i in range(0x988) if slot[i] != after[i]]
print(f"  load: markers saved {tuple(map(hex, m(slot)))} before-load {tuple(map(hex, m(before)))} "
      f"after-load {tuple(map(hex, m(after)))}; {len(diff)} bytes differ from the slot"
      + (f" (first at +{diff[0]:#x})" if diff else ""))
print(f"  load: {len(dumps) - 1} dumps after the load, best match used")
ok = m(slot) == (0x51A7, 0x2E) and m(after) == m(slot) and m(before) != m(slot) and len(diff) <= 16
sys.exit(0 if ok else 1)
EOF
finish
