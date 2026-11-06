#!/bin/bash
# Render dev run: title -> prologue, screenshots at ROTK_SHOT_FRAMES; extra VAR=value args pass through.
#   tests/scripts/render_dev.sh <name> [VAR=value ...]
source "$(dirname "$0")/../lib.sh"
name="${1:-render_dev}"; shift || true
run_game "$name" ROTK_PAD_SCRIPT="$ROOT/tests/scripts/title_to_level.pad" \
  ROTK_SHOT_FRAMES="${SHOTS:-7200}" ROTK_EXIT_AFTER="${EXIT_AFTER:-7210}" "$@"
assert_exit0; assert_no_fatal
finish
