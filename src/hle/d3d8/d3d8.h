#pragma once
// D3D8 HLE (owner: Render). Replaces the D3D section functions the game
// calls (FUNCTIONS.md) with host functions drawing through Metal. Linked
// whole-archive: the overrides register from static initialisers.
//
// Environment:
//   ROTK_RENDER_SCALE=N      render targets at N x their Xbox size (default: 1 for silent
//                           ROTK_BACKGROUND runs; windowed: window pixel height / 480, <= 4)
//   ROTK_ANISO=N             anisotropic filtering of mipmapped textures (windowed default 8)
//   ROTK_FILTER=bicubic      window present upscale filter (default bilinear); a frame larger
//                           than the window is always area-averaged (ROTK_FILTER=bilinear: one
//                           bilinear tap, for A/B checks)
//   ROTK_FULLSCREEN=0|1      window start mode for this run (default: settings.toml [video]
//                           fullscreen, true if absent; Cmd+F / Alt+Enter switch and save it)
//   ROTK_WBUFFER=0           D3DZB_USEW draws use z/w depth instead of the emulated W-buffer
//   ROTK_SHOT_DIR=dir        where screenshots go (default $ROTK_LOG_DIR)
//   ROTK_SHOT_EVERY=N        PNG of every Nth presented frame
//   ROTK_SHOT_FRAMES=a,b,..  PNG of these frame numbers
//   ROTK_EXIT_AFTER=N        clean exit after N presented frames (tests)
//   ROTK_NO_PACING=1         Swap does not wait for the 60 Hz vblank
//   ROTK_WIDESCREEN=1        16:10 (Hor+ 3D, centred 2D) unless rotk_d3d8_set_widescreen was called
//   ROTK_WIDE_HUD=1          widescreen: corner HUD sprites + their action labels anchored to the
//                           wide edges (default 0: the HUD stays in the centred 4:3 area)
//   ROTK_FRAME_LOG=file      one line per Swap: frame, ms, busy ms (minus vblank waits), GPU wait, draws
//                           (a relative name is in ROTK_LOG_DIR)
//   ROTK_TEX_DUMP=dir        dump every new texture (level 0) as <hash>.png
//   ROTK_TEX_RECHECK=N       re-hash unlocked textures every N frames (default 30; 1 = always)
//   ROTK_D3D_TRACE=1         log D3D calls (targets, clears, textures, declarations)
//   ROTK_SHADER_CHECK=1      translate + compile every vertex/pixel shader at creation
//   ROTK_DUMP_MSL=dir        write every generated MSL program
// Debugging (one frame or draw range; see d3d8.cpp DrawCommon):
//   ROTK_DEV_LOG_DRAWS=f     log every draw of frame f (state, textures)
//   ROTK_DEV_HUD_LOG=f       log frame f's 2D draws: box, text/panel, widescreen anchor shift
//   ROTK_DEV_PROBE=f,x,y     log the draws of frame f that change pixel (x, y)
//   ROTK_DEV_SKIP_DRAWS=a-b  drop draws a..b; ROTK_DEV_SKIP_PS=<hash> drop a fragment program
//   ROTK_DEV_MAGENTA=a-b     draw a..b flat (ROTK_DEV_SHOW=fog|d0|d0a|t0uv|t0 shows that input)
//   ROTK_DEV_DUMP_TARGETS=f  PNG of every colour render target at frame f
//   ROTK_DEV_NOFOG=1         fog off

#include <cstdint>

// Texture replacement hook (Polish). Called once per new texture content:
// `hash` is the content hash of the Xbox data (all levels), w x h its size;
// `data` (data_size bytes, valid during the call) is that Xbox data, level 0
// first (DXTn: plain blocks; other formats swizzled).
// Return true and fill *rgba (w2 x h2 RGBA8) to replace the texture.
struct RotkTextureInfo {
  uint64_t hash;
  uint32_t width, height, xbox_format, levels;
  const uint8_t* data;
  uint32_t data_size;
};
using RotkTextureHook = bool (*)(const RotkTextureInfo& info, uint32_t* w2, uint32_t* h2,
                                const uint8_t** rgba);
void rotk_d3d8_set_texture_hook(RotkTextureHook hook);

// Widescreen (Polish): screen-sized targets aspect_scale x wider, 3D Hor+,
// 2D centred, full-screen quads stretched, movies pillarboxed. Before CreateDevice.
extern "C" void rotk_d3d8_set_widescreen(float aspect_scale);

// Frames presented so far (Swap calls; movie overlay frames do not count).
// Stable contract: the XInput HLE's pad script (ROTK_PAD_SCRIPT) uses it as
// its frame clock.
uint64_t rotk_d3d8_frame_count();

// movies.cpp: called once per Swap; the hold-A skip ring's fill (0 = not shown).
float rotk_movie_hold_progress();
