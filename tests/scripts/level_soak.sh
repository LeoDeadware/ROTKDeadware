#!/bin/bash
# Soak of every level (PLAYBOOK phase 4: >= 60k frames per level, ~35 min each paced, 2 at a time).
#   tests/scripts/level_soak.sh [levels...]
exec env SOAK=1 PLAY="${PLAY:-60000}" bash "$(dirname "$0")/level_sweep.sh" "$@"
