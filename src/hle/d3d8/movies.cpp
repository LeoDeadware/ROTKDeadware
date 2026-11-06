// Dev switch ROTK_SKIP_MOVIES=1: every movie ends at its first decoded frame.
// Player: holding A for kHoldSeconds during a movie ends it (owner bug 2026-11-06), with a
// ring drawn into the frame (d3d8 Swap -> rotk_movie_hold_progress, gpu::DrawRing) that fills
// while A is held. On by default; off under ROTK_TEST_PROFILE=1 unless ROTK_SKIP_HOLD=1
// (ROTK_SKIP_HOLD=0 turns it off). After the skip the game reads A as up until it is
// released, so the same hold does not also press the next menu.
//
// ROTK's movies are scene-script objects (Ghidra, this XBE): FUN_00111e90
// builds Data/Movies/%s (or Data/Special/%s) from a "#name[:n]" argument;
// FUN_00111c10 opens it, FUN_00111370 starts it (state obj+0xC0 = 3). Each
// frame FUN_001134b0 decodes the frames that are due and after each one asks
// the thunk 0xBCB90 (-> FUN_000bcf10: current frame >= last frame); true runs
// FUN_00112fb0, the normal end of the video (FUN_00112f40, state 5), which the
// scene script waits for. 0xBCB90 has no other caller, so answering "last
// frame" ends every movie the game's own way.
//
// Not used: making the video open fail (thunk 0xBCAF0) takes the game's
// missing-movie path, whose black-screen fallback FUN_000bf2b0 kills the run
// (skipmov2-20261102-143941-*: exit 251 right after its Surface_LockRect).
// The donor's PlayMovie 0x18D40 and XMV hooks 0x14ED3D / 0x14EB6F were HP2
// addresses; ROTK plays its own .vid format and has neither.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include "hle/d3d8/d3d8.h"
#include "hle/xinput/xinput.h"
#include "runtime/rotk_runtime.h"

extern "C" int rotk_dsound_mute_movie_rings() __attribute__((weak));  // dsound.cpp

namespace {

constexpr uint32_t kVideoAtEnd = 0xBCB90;  // () -> AL: the last frame was decoded
constexpr double kHoldSeconds = 1.0;

std::atomic<uint64_t> g_movie_frame{0};  // d3d8 frame of the last 0xBCB90 call + 1 (0: none yet)
std::atomic<bool> g_skip_now{false};
double g_held = 0;                       // Swap thread only
bool g_need_release = false;             // after a skip: A must go up before the next hold
std::chrono::steady_clock::time_point g_last;

bool HoldEnabled() {
  static const bool on = [] {
    const char* h = getenv("ROTK_SKIP_HOLD");
    if (h && *h) return atoi(h) != 0;
    const char* t = getenv("ROTK_TEST_PROFILE");
    return !(t && !strcmp(t, "1"));
  }();
  return on;
}

void VideoAtEnd(RotkCtx& c) {
  static const bool skip = [] {
    const char* v = getenv("ROTK_SKIP_MOVIES");
    return v && atoi(v) != 0;
  }();
  g_movie_frame = rotk_d3d8_frame_count() + 1;
  if (g_skip_now.exchange(false)) {
    rotk_log("movies: skipped by holding A at frame %llu", (unsigned long long)rotk_d3d8_frame_count());
    rotk_xinput_hide_until_release(0, kPadA);
    if (rotk_dsound_mute_movie_rings) rotk_dsound_mute_movie_rings();  // its soundtrack too
    rotk_ret_cdecl(c, 1);
    return;
  }
  if (!skip) {
    rotk_original(kVideoAtEnd)(c);
    return;
  }
  static uint64_t n = 0;
  if (n++ == 0) rotk_log("movies: skipping (ROTK_SKIP_MOVIES): every movie ends at its first frame");
  rotk_ret_cdecl(c, 1);
}

ROTK_OVERRIDE(kVideoAtEnd, VideoAtEnd);

}  // namespace

float rotk_movie_hold_progress() {
  if (!HoldEnabled()) return 0;
  auto now = std::chrono::steady_clock::now();
  double dt = std::chrono::duration<double>(now - g_last).count();
  g_last = now;
  uint64_t mf = g_movie_frame, f = rotk_d3d8_frame_count();
  bool playing = mf && mf + 3 >= f;  // the movie asked "at end?" within the last frames
  bool a = rotk_xinput_get_pad(0).analog[kPadA] > 30;
  if (!a) g_need_release = false;
  if (!playing || !a || g_need_release || g_skip_now) {
    g_held = 0;
    return 0;
  }
  if (g_held == 0) rotk_log("movies: A held at frame %llu: ring shown", (unsigned long long)f);
  g_held += dt > 0.1 ? 0.1 : dt;
  if (g_held >= kHoldSeconds) {
    g_skip_now = true;
    g_need_release = true;
    g_held = 0;
    return 1;
  }
  return float(g_held / kHoldSeconds);
}
