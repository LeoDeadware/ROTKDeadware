// 120 fps on ProMotion displays (owner phase 2, launcher option; ROTK_FPS=120 with ROTK_FPS60=1).
// The game counts vblank ticks (FrameCounter [0x1E2F14], bumped by the vblank callback 0xBEDE0)
// and its frame step FUN_0009EB20 turns them into seconds: dt [0x1E3344] = ticks x [0x154338]
// (1/60). The kernel ticks at 120 Hz under ROTK_FPS=120, so for the duration of that one call
// the constant reads 1/120; the other reader of 0x154338 (0xA4110) keeps 1/60. The game's time
// then runs at wall speed at either rate. Every 10 s the log gets "fps: <frames/s> game time x<r>"
// (r = game seconds per wall second; tests/scripts/fps120.sh).
#include <chrono>
#include <cstdlib>
#include <cstring>

#include "runtime/rotk_runtime.h"

namespace {

constexpr uint32_t kFrameStep = 0x9EB20, kTickSec = 0x154338, kDt = 0x1E3344;

uint32_t Bits(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}
float Float(uint32_t b) {
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}

void FrameStep(RotkCtx& c) {
  static const int hz = [] {
    const char* f = getenv("ROTK_FPS");
    return f && atoi(f) == 120 ? 120 : 60;
  }();
  static const bool scale = !getenv("ROTK_FPS_NOSCALE");  // tests: the unscaled step (x2 speed)
  static uint32_t game_tick = 0;
  if (hz == 120 && scale) {
    game_tick = rotk_rd32(kTickSec);
    rotk_wr32(kTickSec, Bits(Float(game_tick) * 0.5f));
  }
  rotk_original(kFrameStep)(c);
  if (hz == 120 && scale) rotk_wr32(kTickSec, game_tick);
  // rate report
  using clk = std::chrono::steady_clock;
  static clk::time_point t0 = clk::now();
  static double game = 0;
  static int frames = 0;
  float dt = Float(rotk_rd32(kDt));
  if (dt > 0 && dt < 1) game += dt;
  frames++;
  double wall = std::chrono::duration<double>(clk::now() - t0).count();
  if (wall >= 10) {
    rotk_log("fps: %.1f frames/s game time x%.3f (vblank %d Hz)", frames / wall, game / wall, hz);
    t0 = clk::now(), game = 0, frames = 0;
  }
}

ROTK_OVERRIDE(kFrameStep, FrameStep);

}  // namespace
