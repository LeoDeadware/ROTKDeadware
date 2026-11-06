#!/bin/bash
# 120 fps on ProMotion (owner phase 2; src/polish/fps.cpp): Hel01 fight at ROTK_FPS=120 keeps
# >= 110 fps, <= 5 % of frames over the 8.4 ms budget, and game time at wall speed (0.9..1.1;
# ROTK_FPS_NOSCALE=1, the unscaled step, gives x2.0 and fails).
exec env MIN_FPS="${MIN_FPS:-110}" BUDGET="${BUDGET:-8.4}" MAX_OVER="${MAX_OVER:-5}" \
  bash "$(dirname "$0")/fps_check.sh" Hel01 4800 ROTK_FPS60=1 ROTK_FPS=120 "$@"
