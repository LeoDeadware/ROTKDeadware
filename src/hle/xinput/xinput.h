#pragma once
// XInput HLE (owner: Kernel HLE). default.xbe links the XDK's USB stack
// (section XPP); the game calls eight of its XInput entry points. They are
// replaced at run time (rotk_override) by host versions fed from
// rotk_xinput_set_pad, e.g. by the SDL3 backend below (gamepads + keyboard).
//
//   size_t n; const RotkOverride* o = rotk_xinput_overrides(&n);
//   for (size_t i = 0; i < n; i++) rotk_override(o[i].va, o[i].fn);

#include <cstddef>
#include <cstdint>

#include "hle/kernel/kernel_exports.h"

struct RotkOverride {
  uint32_t va;
  HostFn fn;
  const char* name;
};
// XInitDevices 0x147CF3, XGetDevices 0x148ABB, XGetDeviceChanges 0x148ADD,
// XInputOpen 0x1487B4, XInputClose 0x14880A, XInputGetCapabilities 0x148816,
// XInputGetState 0x1489EE, XInputSetState 0x148A61 (NTSC-US default.xbe).
const RotkOverride* rotk_xinput_overrides(size_t* count);

// Xbox controller state (XINPUT_GAMEPAD semantics).
struct RotkPadState {
  bool connected = false;
  uint16_t buttons = 0;     // XINPUT_GAMEPAD_DPAD_UP 0x1 ... RIGHT_THUMB 0x80
  uint8_t analog[8] = {};   // A, B, X, Y, BLACK, WHITE, LEFT_TRIGGER, RIGHT_TRIGGER
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;  // up = positive
};
enum : uint16_t {
  kPadDpadUp = 0x1, kPadDpadDown = 0x2, kPadDpadLeft = 0x4, kPadDpadRight = 0x8,
  kPadStart = 0x10, kPadBack = 0x20, kPadLeftThumb = 0x40, kPadRightThumb = 0x80,
};
enum { kPadA, kPadB, kPadX, kPadY, kPadBlack, kPadWhite, kPadLT, kPadRT };

// Thread-safe; port 0..3.
void rotk_xinput_set_pad(int port, const RotkPadState& s);
// Same, with the host time (rotk_xinput_now_ns clock) of the device input that
// produced the state, for the input latency measurement (0 = unknown).
void rotk_xinput_set_pad_at(int port, const RotkPadState& s, uint64_t input_ns);
uint64_t rotk_xinput_now_ns();
// Device input -> first XInputGetState that returns it (logged every 600 inputs).
struct RotkInputLatency {
  uint32_t count = 0, min_us = 0, avg_us = 0, max_us = 0;
};
RotkInputLatency rotk_xinput_latency();
// rotk_xinput_now_ns time of the game's last XInputGetState (0: none yet).
uint64_t rotk_xinput_last_read_ns();
// 1 when the connected gamepad is a PlayStation one (SDL type PS3/4/5; ROTK_PAD_KIND=ps|xbox).
int rotk_xinput_pad_family();
RotkPadState rotk_xinput_get_pad(int port);
// The game reads analog button `analog` (kPadA..) of `port` as released until the
// player lets go of it (a hold that skipped a movie must not also press the next menu).
void rotk_xinput_hide_until_release(int port, int analog);
// Rumble requested by the game (0..65535 per motor); for the backend.
void rotk_xinput_get_rumble(int port, uint16_t* left, uint16_t* right);

// ---- Pad script (pad_script.h: syntax) ---------------------------------------
// ROTK_PAD_SCRIPT=<file> is loaded on the first XInput call. While a script is
// active it alone drives port 0 (rotk_xinput_set_pad is ignored), evaluated at
// the current frame on every XInput call. Load errors exit the process (2).
// Path nullptr unloads. Returns false (logged) if the file is missing or bad.
bool rotk_xinput_load_script(const char* path);
bool rotk_xinput_script_active();
// Frame clock of the script. Default: the D3D Swap count (rotk_d3d8_frame_count,
// Render), or the kernel vblank count when no D3D HLE is linked in.
void rotk_xinput_set_frame_source(uint64_t (*frame)());

// ---- SDL3 backend (rotk_xinput_sdl library) -----------------------------------
// Call init once after SDL_Init, and update on the thread that pumps SDL
// events, as often as it can (window.mm: on every event, at least every 4 ms).
// Port 0 is the game's controller: the keyboard plus the gamepad that gave the
// last input (pads hot-plug; the port stays connected). Sticks are shaped by
// input_filter.h (settings.toml). Returns false if SDL's gamepad subsystem failed.
bool rotk_xinput_sdl_init();
void rotk_xinput_sdl_update();
