// The game window (SDL3 + CAMetalLayer) and the host event loop.
//
// macOS only lets the process main thread run windowing and events, while the
// game runs on threads of its own; so the runtime hands the main thread to
// rotk_host_main_loop() once the XBE entry has started the title. It creates
// the window, gives its CAMetalLayer to the GPU layer (the game thread draws
// every presented frame into it, fitted 4:3: gpu::PresentFrame) and pumps
// events: controller/keyboard (XInput HLE's SDL backend), resize, fullscreen
// (Cmd+F, Alt+Enter: desktop fullscreen), quit (Cmd+Q, window close).
// The window starts in borderless (desktop) fullscreen on the display under
// the mouse unless settings.toml [video] fullscreen = false; Cmd+F / Alt+Enter
// switch and save the choice there. ROTK_FULLSCREEN=0|1 overrides it for a run.
//
// ROTK_BACKGROUND=1 (tests): no window, no SDL video, the loop returns at once.
// ROTK_DEV_PRESENT_TEST=WxH (with ROTK_BACKGROUND=1): the frames go to an
// unattached W x H layer instead, and each screenshot frame also writes
// present_NNNNNN.png: the present path checked without taking the screen.

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <QuartzCore/CAMetalLayer.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "hle/d3d8/gpu.h"
#include "hle/d3d8/video_settings.h"
#include "hle/xinput/xinput.h"
#include "runtime/rotk_runtime.h"

extern "C" float rotk_d3d8_output_aspect();  // d3d8.cpp

namespace {

bool EnvOn(const char* n) {
  const char* v = getenv(n);
  return v && *v && *v != '0';
}

void ResizeLayer(SDL_Window* w, CAMetalLayer* layer) {
  int pw = 0, ph = 0;
  SDL_GetWindowSizeInPixels(w, &pw, &ph);
  if (pw > 0 && ph > 0) layer.drawableSize = CGSizeMake(pw, ph);
}

void ToggleFullscreen(SDL_Window* w) {
  bool full = (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) != 0;
  SDL_SetWindowFullscreenMode(w, nullptr);  // desktop fullscreen
  SDL_SetWindowFullscreen(w, !full);
  std::string path = rotkd3d::VideoSettingsPath();
  std::string text = rotkd3d::WithVideoSetting(rotkd3d::ReadTextFile(path), "fullscreen",
                                              full ? "false" : "true");
  if (!rotkd3d::WriteTextFile(path, text)) rotk_log("window: cannot save %s", path.c_str());
}

// Fullscreen at start: ROTK_FULLSCREEN, else settings.toml [video] fullscreen
// (default true).
bool StartFullscreen() {
  if (const char* v = getenv("ROTK_FULLSCREEN"); v && *v) return *v != '0';
  return rotkd3d::VideoSettingBool(rotkd3d::ReadTextFile(rotkd3d::VideoSettingsPath()), "fullscreen",
                                  true);
}

// The display under the mouse (the one the player is looking at).
CGDirectDisplayID ActiveDisplay() {
  CGDirectDisplayID d = CGMainDisplayID();
  if (CGEventRef e = CGEventCreate(nullptr)) {
    CGPoint p = CGEventGetLocation(e);
    CFRelease(e);
    uint32_t n = 0;
    CGDirectDisplayID found;
    if (CGGetDisplaysWithPoint(p, 1, &found, &n) == kCGErrorSuccess && n == 1) d = found;
  }
  return d;
}

// Fullscreen edge to edge (owner bug 2026-11-06 "чорні полоси"): a Spaces fullscreen window on a
// notched display stops below the camera housing (3024x1890 of 3024x1964) and the frame, shaped
// for the whole display, got bars on every side. Non-Spaces fullscreen (hint set before SDL_Init)
// plus the window frame = the screen's whole frame, menu bar and Dock hidden.
NSRect CoverFrame(NSScreen* s) { return s ? s.frame : NSZeroRect; }

void CoverDisplay(SDL_Window* w) {
  if (!(SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN)) return;
  NSWindow* nw = (__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(w),
                                                          SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
  if (!nw || !nw.screen) return;
  [NSApp setPresentationOptions:NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar];
  [nw setFrame:CoverFrame(nw.screen) display:YES];
}

}  // namespace

// ROTK_DEV_FULLSCREEN_PROBE=1: logs the fullscreen window geometry for the main screen without
// opening a window (tests: the frame must be the whole display, notch area included).
extern "C" void rotk_window_fullscreen_probe() {
  NSScreen* s = NSScreen.mainScreen;
  if (!s) return;
  double k = s.backingScaleFactor;
  NSRect f = CoverFrame(s), all = s.frame;
  double safe_h = all.size.height;
  if (@available(macOS 12.0, *)) safe_h -= s.safeAreaInsets.top;
  rotk_log("window: fullscreen probe: screen %.0fx%.0f px, safe area %.0fx%.0f px, fullscreen frame %.0fx%.0f px",
           all.size.width * k, all.size.height * k, all.size.width * k, safe_h * k, f.size.width * k,
           f.size.height * k);
}

// Render scale for windowed play: the pixel height the frame is shown at /
// 480, rounded down (at most 4 by the caller): fullscreen = the active
// display's height; a window = 900 points, <= 90% of the display. Times the
// display's backing scale. CoreGraphics only: callable from the game thread.
extern "C" int rotk_window_auto_scale() {
  CGDisplayModeRef m = CGDisplayCopyDisplayMode(ActiveDisplay());
  if (!m) return 2;
  double points_h = double(CGDisplayModeGetHeight(m));
  double px_per_pt = CGDisplayModeGetHeight(m) ? double(CGDisplayModeGetPixelHeight(m)) / points_h : 1;
  CGDisplayModeRelease(m);
  double shown_pt = StartFullscreen() ? points_h : std::min(900.0, 0.9 * points_h);
  int scale = int(shown_pt * px_per_pt / 480.0);
  return scale < 1 ? 1 : scale;
}

// The active display's size in pixels (widescreen native, render scale native).
// ROTK_DISPLAY_SIZE=WxH stands for a display in tests.
extern "C" bool rotk_window_display_pixels(int* w, int* h) {
  if (const char* v = getenv("ROTK_DISPLAY_SIZE"); v && std::sscanf(v, "%dx%d", w, h) == 2 && *w > 0 && *h > 0)
    return true;
  CGDisplayModeRef m = CGDisplayCopyDisplayMode(ActiveDisplay());
  if (!m) return false;
  *w = int(CGDisplayModeGetPixelWidth(m)), *h = int(CGDisplayModeGetPixelHeight(m));
  CGDisplayModeRelease(m);
  return *w > 0 && *h > 0;
}

extern "C" void rotk_host_main_loop() {
  if (const char* t = getenv("ROTK_DEV_PRESENT_TEST")) {
    unsigned w = 0, h = 0;
    if (std::sscanf(t, "%ux%u", &w, &h) == 2 && w && h) {
      CAMetalLayer* layer = [CAMetalLayer layer];
      layer.drawableSize = CGSizeMake(w, h);
      rotkd3d::gpu::KeepPresentCopies(true);
      rotkd3d::gpu::SetPresentLayer((__bridge void*)layer);
      rotk_log("window: ROTK_DEV_PRESENT_TEST: frames presented to an unattached %ux%u layer", w, h);
    }
    return;
  }
  if (EnvOn("ROTK_DEV_FULLSCREEN_PROBE")) rotk_window_fullscreen_probe();
  if (EnvOn("ROTK_BACKGROUND")) return;

  SDL_SetHint(SDL_HINT_APP_NAME, "The Lord of the Rings: The Return of the King");
  SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "0");
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    rotk_log("window: SDL_Init(VIDEO) failed: %s (running without a window)", SDL_GetError());
    return;
  }
  // Window shape = the output's aspect (4:3, 16:10 in widescreen): 900 points
  // tall, at most 90% of the display's usable area. Other shapes (resized,
  // fullscreen) get black bars from the present.
  float aspect = rotk_d3d8_output_aspect();
  int wh = 900, ww = int(900 * aspect + 0.5f);
  float mx = 0, my = 0;
  SDL_GetGlobalMouseState(&mx, &my);
  SDL_Point mouse{int(mx), int(my)};
  SDL_DisplayID display = SDL_GetDisplayForPoint(&mouse);
  if (!display) display = SDL_GetPrimaryDisplay();
  SDL_Rect usable;
  if (SDL_GetDisplayUsableBounds(display, &usable) && usable.w > 0 && usable.h > 0) {
    float k = std::min({1.0f, 0.9f * usable.w / ww, 0.9f * usable.h / wh});
    ww = int(ww * k + 0.5f), wh = int(wh * k + 0.5f);
  }
  bool full = StartFullscreen();
  SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING,
                        "The Lord of the Rings: The Return of the King");
  SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
  SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
  SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, ww);
  SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, wh);
  SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER,
                        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_METAL |
                            (full ? SDL_WINDOW_FULLSCREEN : 0));
  SDL_Window* win = SDL_CreateWindowWithProperties(props);  // fullscreen: desktop mode (no mode set)
  SDL_DestroyProperties(props);
  if (!win) {
    rotk_log("window: SDL_CreateWindow failed: %s (running without a window)", SDL_GetError());
    return;
  }
  SDL_SetWindowMinimumSize(win, 320, 240);
  SDL_MetalView view = SDL_Metal_CreateView(win);
  CAMetalLayer* layer = view ? (__bridge CAMetalLayer*)SDL_Metal_GetLayer(view) : nil;
  if (!layer) {
    rotk_log("window: no Metal layer: %s (running without a window)", SDL_GetError());
    return;
  }
  CoverDisplay(win);
  ResizeLayer(win, layer);
  // Owner test dcb2fc2 («геймпад чомусь не працює»): started from the launcher window (a separate
  // osascript app that was frontmost), the game never became the active app - macOS 14 hands
  // activation over only when the active app yields it (launcher.js does, to this pid) - so SDL
  // (no keyboard focus) dropped every pad event. Activate and raise the window ourselves.
  if (@available(macOS 14.0, *)) [NSApp activate];
  else [NSApp activateIgnoringOtherApps:YES];
  SDL_RaiseWindow(win);
  rotkd3d::gpu::SetPresentLayer((__bridge void*)layer);
  if (!rotk_xinput_sdl_init()) rotk_log("window: gamepad subsystem failed: %s", SDL_GetError());
  int pw = 0, ph = 0;
  SDL_GetWindowSizeInPixels(win, &pw, &ph);
  rotk_log("window: %dx%d points (aspect %.3f), %dx%d pixels%s (Cmd+F / Alt+Enter fullscreen, "
          "Cmd+Q quit)", ww, wh, aspect, pw, ph, full ? ", fullscreen" : "");

  for (;;) {
    SDL_Event e;
    if (SDL_WaitEventTimeout(&e, 4)) {
      do {
        switch (e.type) {
          case SDL_EVENT_QUIT:
          case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            rotk_exit(0, "window closed");
          case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            ResizeLayer(win, layer);
            break;
          case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
            SDL_HideCursor();
            CoverDisplay(win);
            ResizeLayer(win, layer);
            break;
          case SDL_EVENT_WINDOW_FOCUS_GAINED:
          case SDL_EVENT_WINDOW_FOCUS_LOST: {
            static int logs = 0;
            if (logs++ < 20)
              rotk_log("window: focus %s (app active %d)", e.type == SDL_EVENT_WINDOW_FOCUS_GAINED ? "gained" : "lost",
                       int([NSApp isActive]));
            break;
          }
          case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
            [NSApp setPresentationOptions:NSApplicationPresentationDefault];
            SDL_ShowCursor();
            ResizeLayer(win, layer);
            break;
          case SDL_EVENT_KEY_DOWN:
            if (!e.key.repeat &&
                ((e.key.key == SDLK_F && (e.key.mod & SDL_KMOD_GUI)) ||
                 (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))))
              ToggleFullscreen(win);
            break;
          default:
            break;
        }
      } while (SDL_PollEvent(&e));
    }
    // Keyboard state and gamepads -> XInput port 0..3 (once per ~4 ms).
    rotk_xinput_sdl_update();
  }
}
