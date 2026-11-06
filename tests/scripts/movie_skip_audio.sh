#!/bin/bash
# Owner bug 2026-11-06 (680a23b): "я затиснув і пропустив заставку, почалася місія, але голос
# гендальфа із заставки все ще звучав". The intro's soundtrack is a looping 44.1 kHz ring pair the
# game streams; skipping the video left it playing. Title -> intro, A held from 1400 (skip ~1430),
# run to 2400 with the mixer's per-voice stats: the 44.1 kHz voices of the window before the skip
# must not be audible in any window after it (listed as muted, or gone).
source "$(dirname "$0")/../lib.sh"
RUN_TIMEOUT=900 run_game movie_skip_audio ROTK_SKIP_HOLD=1 ROTK_PAD_SCRIPT="$TESTS_DIR/scripts/movie_hold_skip.pad" \
  ROTK_EXIT_AFTER=2400
assert_exit0; assert_no_fatal
SEMANTIC=$((SEMANTIC + 1))
assert_log "skipped by holding A" "the movie was not skipped"
python3 -I - "$LOG" <<'PY' || fail "the movie's soundtrack is still audible after the skip"
import re, sys
lines = open(sys.argv[1], errors='replace').read().splitlines()
skip = next(i for i, l in enumerate(lines) if 'skipped by holding A' in l)
before, after, cur = set(), [], None
for i, l in enumerate(lines):
    m = re.match(r'dsound:   voice (0x[0-9a-f]+) fmt \S+ (\d+) Hz .*source rms ([0-9.]+)(.*)', l)
    if l.startswith('dsound: mixer window'): cur = i
    if not m: continue
    va, hz, rms, tail = m.group(1), int(m.group(2)), float(m.group(3)), m.group(4)
    if i < skip and hz == 44100: before.add(va) if cur and cur < skip else None
    elif i > skip: after.append((va, rms, 'muted' in tail))
movie = {va for va in before}
loud = [(va, r) for va, r, mu in after if va in movie and not mu and r > 0.002]
print('  soundtrack voices', sorted(movie), 'audible after the skip:', loud[:4])
sys.exit(1 if not movie or loud else 0)
PY
finish
