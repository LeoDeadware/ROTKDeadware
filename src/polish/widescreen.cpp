// 16:10 widescreen (Polish): ROTK_WIDESCREEN=1 (the .app's launch script sets it unless
// ROTK_TEST_PROFILE=1; unset or 0 = no change at all, so goldens are unaffected).
//
// The game is 4:3 only and keeps its 640x480 logical screen. The D3D8 HLE
// (rotk_d3d8_set_widescreen) renders that screen kWide = (16/10)/(4/3) = 1.2
// times wider and draws the 4:3 view kWide narrower inside it: 3D shows more
// at the sides (Hor+: the clip volume is wider), 2D keeps its proportions,
// centred; full-screen quads still cover the whole width.
//
// Here: the game's own view culling is widened to match, or objects in the
// extra side strips would be culled (pop in at the 4:3 edge). The view setup
// is FUN_000A32F0 (decompiled, recon 2026-11-02), run once per frame per view
// from FUN_000A2A50 and on view creation (FUN_000A3740):
//   camera = view+4: +0xE0 horizontal FOV in degrees (60), +0xE4 near, +0xE8 far
//   view+0x318 = tan(fov/2), +0x31C = 1/tan  (fov is horizontal: the projection
//     FUN_000B41E0 gets m00 = -cot, m11 = cot * 4/3)
//   view+0x2F0 = tanX (rect/viewport scaled tan), +0x2F4 = tanY
//   view+0x2F8/+0x300 = normalize(1, tanX) (y, x): the left/right plane normal
//   view+0x2FC/+0x304 = normalize(1, tanY): top/bottom
//   view+0x308/+0x30C = normalize(1, 2 * camera[+0x80] * tanX), +0x310/+0x314 same for Y
// The projection matrix (view+0x50, inverse +0x90) stays 4:3: the HLE's
// shader epilogue does the horizontal squeeze for every draw on the wide target.

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "polish/postfx/postfx.h"
#include "polish/texpack/texpack.h"
#include "runtime/rotk_runtime.h"

// D3D8 HLE (Render): widen screen-sized render targets by `aspect_scale`
// and keep 2D at 4:3. Weak: without it the 3D view would be squeezed into
// the 4:3 target, so widescreen stays off.
extern "C" void rotk_d3d8_set_widescreen(float aspect_scale) __attribute__((weak));
// window.mm: the active display's pixel size (ROTK_DISPLAY_SIZE=WxH overrides it).
extern "C" bool rotk_window_display_pixels(int* w, int* h) __attribute__((weak));

namespace {

float kWide = (16.0f / 10.0f) / (4.0f / 3.0f);  // 1.2; ROTK_WIDESCREEN=native: the display's
constexpr uint32_t kViewSetup = 0xA32F0;                     // FUN_000A32F0(view), cdecl

bool EnvOn(const char* name, bool def) {
  const char* v = std::getenv(name);
  if (!v || !*v) return def;
  return std::strcmp(v, "0") != 0;
}

bool Enabled() {
  static const bool on = [] {
    bool want = EnvOn("ROTK_WIDESCREEN", false);
    if (want && !rotk_d3d8_set_widescreen) {
      rotk_log("polish: ROTK_WIDESCREEN ignored (the D3D8 HLE has no widescreen support)");
      return false;
    }
    const char* v = std::getenv("ROTK_WIDESCREEN");
    int dw = 0, dh = 0;
    if (want && v && !std::strcmp(v, "native") && rotk_window_display_pixels &&
        rotk_window_display_pixels(&dw, &dh) && dh > 0) {
      // Hor+ to the display's own aspect (addendum c): 4:3 and taller stay 4:3, at most 32:9.
      kWide = std::fmin(std::fmax(float(dw) / float(dh) / (4.0f / 3.0f), 1.0f), (32.0f / 9.0f) / (4.0f / 3.0f));
      rotk_d3d8_set_widescreen(kWide);
      rotk_log("polish: widescreen native %dx%d (%.3f:1, view x%.3f wider)", dw, dh, float(dw) / float(dh), kWide);
    } else if (want) {
      rotk_d3d8_set_widescreen(kWide);
      rotk_log("polish: widescreen 16:10 (view x%.3f wider)", kWide);
    }
    return want;
  }();
  return on;
}

// Dev A/B: ROTK_WIDE_CULL=0 keeps the game's 4:3 culling (objects pop at the 4:3 edge);
// ROTK_WIDE_CULL=alt widens every other view setup (one per frame): compare the draw counts of
// odd and even frames in ROTK_FRAME_LOG within one run.
bool WideCull() {
  static const char* v = std::getenv("ROTK_WIDE_CULL");
  static const bool alt = v && !std::strcmp(v, "alt");
  static const bool on = EnvOn("ROTK_WIDE_CULL", true);
  static uint32_t calls = 0;
  if (alt) return (calls++ & 1) != 0;
  return on;
}

bool Trace() {
  static const bool on = std::getenv("ROTK_POLISH_TRACE") != nullptr;
  return on;
}

float RdF(uint32_t va) {
  uint32_t u = rotk_rd32(va);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
void WrF(uint32_t va, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  rotk_wr32(va, u);
}

// The game's FUN_000C58D0 on (1, t, 0): writes y to `y_at` and x to `x_at`.
void PlaneNormal(uint32_t y_at, uint32_t x_at, float t) {
  float inv = 1.0f / std::sqrt(1.0f + t * t);
  WrF(y_at, t * inv);
  WrF(x_at, inv);
}

// Draw distance (addendum d): ROTK_DRAW_DISTANCE=<factor> (1 = the game's; the app's launcher
// passes settings.toml [launcher] draw_distance, default 1.5; clamped 1..3). The camera's far
// plane (+0xE8) is scaled only while the view is set up, so the projection and the far culling
// plane reach further; the camera itself (and game logic reading it) keeps the original value.
// Depth is 32-bit float on the host, so the longer range costs no z precision worth seeing.
float DrawDistance() {
  static const float f = [] {
    const char* v = std::getenv("ROTK_DRAW_DISTANCE");
    float x = v && *v ? float(std::atof(v)) : 1.0f;
    x = std::fmin(std::fmax(x, 1.0f), 3.0f);
    if (x != 1.0f) rotk_log("polish: draw distance x%.2f", x);
    return x;
  }();
  return f;
}

void ViewSetup(RotkCtx& c) {
  static const HostFn original = rotk_original(kViewSetup);
  const uint32_t view = rotk_arg(c, 0);
  const uint32_t cam0 = view ? rotk_rd32(view + 4) : 0;
  const float far0 = cam0 ? RdF(cam0 + 0xE8) : 0;
  const bool far_up = cam0 && DrawDistance() != 1.0f && far0 > 0;
  if (far_up) WrF(cam0 + 0xE8, far0 * DrawDistance());
  original(c);
  if (far_up) {
    WrF(cam0 + 0xE8, far0);
    static uint32_t logged = 0;
    if (logged++ < 1) rotk_log("polish: draw distance: far %.0f -> %.0f", far0, far0 * DrawDistance());
  }
  if (!Enabled() || !WideCull() || !view) return;
  const uint32_t cam = rotk_rd32(view + 4);
  const float tan_x = RdF(view + 0x2F0) * kWide;
  WrF(view + 0x2F0, tan_x);
  PlaneNormal(view + 0x2F8, view + 0x300, tan_x);
  PlaneNormal(view + 0x308, view + 0x30C, 2.0f * RdF(cam + 0x80) * tan_x);
  static uint32_t n = 0;
  if (Trace() && (n++ < 6 || n % 1800 == 0))
    rotk_log("polish: view setup #%u view %08X cam %08X fov %.2f tan %.4f tanX %.4f (x%.2f) tanY %.4f",
             n, view, cam, RdF(cam + 0xE0), RdF(view + 0x318), tan_x, kWide, RdF(view + 0x2F4));
}

}  // namespace

ROTK_OVERRIDE(kViewSetup, ViewSetup);

// Called once at startup (main.cpp) so the D3D8 HLE knows the target width
// before CreateDevice; also sets up the post-processing look and the HD UI pack.
void rotk_polish_init() {
  (void)Enabled();
  rotk_polish_postfx_init();
  rotk_polish_texpack_init();
}
