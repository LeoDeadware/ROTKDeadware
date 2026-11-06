#!/bin/bash
# Phase-1 gate (PLAYBOOK): the lifted default.xbe boots (XBE mapped, TLS/stack, kernel
# thunks bound to HLE) and reaches the first call into the D3D section.
# Donor runtime prints "rotk: first call into D3D" when ROTK_STOP_AT=D3D.
source "$(dirname "$0")/../lib.sh"
RUN_TIMEOUT=120 run_game phase1_first_d3d ROTK_STOP_AT=D3D ROTK_KTRACE=1
assert_exit0
assert_no_fatal
assert_log "^rotk: first call into D3D" "did not reach the D3D section"
echo "  kernel calls traced: $(grep -c '^K> ' "$LOG" 2>/dev/null || true)"
finish
