#!/bin/bash
# Regression: a level switch while a movie streams calls XAPI CancelIo (0x11E5C0), which walks
# the IRP list of the kernel's IdexChannelObject (+0x28). Zeroed, it faulted reading 0x24
# (sweep-qa1 Hel01, route at frame 2500 / 6800 during a movie). Route to Pat01 during the intro
# movie (START at 1100, movie ~1300..6300), then require the Pat01 scene to open, no FATAL.
source "$(dirname "$0")/../lib.sh"
PAD="$(mktemp -t rotk_rim_pad)"; echo "1100 press START" > "$PAD"
RUN_TIMEOUT=300 run_game route_in_movie ROTK_PAD_SCRIPT="$PAD" ROTK_EXIT_AFTER=4200 \
  "ROTK_CALL_AT=2500:poke:16D584:0,2500:9D770/1,2500:9C8A0/0"
rm -f "$PAD"
assert_exit0
assert_no_fatal
assert_log "ROTK_CALL_AT 0009C8A0 returned" "route call did not run"
assert_log 'open .*Pat01\.scx' "Pat01 scene not opened"
finish
