// Subtitles at the bottom edge (owner bug 2026-11-06, 680a23b: "саби занадто високо").
// The subtitle draw FUN_000F7BE0 (Ghidra) places its last line at y = 0.76 of the screen
// (0x158D1C; 0.77 at 0x158D20 when DAT_001E0D0A is set) and stacks earlier lines 0.057 above.
// Those two floats have no other reader; before the first draw they are moved down so the last
// line ends in the bottom tenth of the frame. ROTK_SUBS_Y=<0..1> sets the line y; under
// ROTK_TEST_PROFILE=1 the game's own values stay (goldens).
#include <cstdlib>
#include <cstring>

#include "runtime/rotk_runtime.h"

namespace {

constexpr uint32_t kSubsDraw = 0xF7BE0, kLineY = 0x158D1C, kLineYAlt = 0x158D20;

float TargetY() {
  if (const char* v = getenv("ROTK_SUBS_Y"); v && *v) return float(atof(v));
  const char* t = getenv("ROTK_TEST_PROFILE");
  return t && !strcmp(t, "1") ? 0.0f : 0.885f;
}

void SubsDraw(RotkCtx& c) {
  static const float y = TargetY();
  static bool done = false;
  if (!done && y > 0.0f && y < 1.0f) {
    uint32_t b;
    std::memcpy(&b, &y, 4);
    rotk_wr32(kLineY, b);
    float y2 = y + 0.01f;
    std::memcpy(&b, &y2, 4);
    rotk_wr32(kLineYAlt, b);
    rotk_log("polish: subtitles at y %.3f (game 0.760)", y);
  }
  done = true;
  rotk_original(kSubsDraw)(c);
}

ROTK_OVERRIDE(kSubsDraw, SubsDraw);

}  // namespace
