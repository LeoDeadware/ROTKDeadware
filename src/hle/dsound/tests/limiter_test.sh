#!/bin/bash
# Builds and runs the master limiter test (Limiter, limiter.h).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$HERE/../../.." && pwd)"
OUT="${TMPDIR:-/tmp}/rotk_limiter_test"
mkdir -p "$OUT"
c++ -std=c++20 -O2 -I"$SRC" "$HERE/limiter_test.cpp" -o "$OUT/limiter_test"
"$OUT/limiter_test"
