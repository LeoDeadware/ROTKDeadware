// D3D8 HLE: the D3D section functions the game (and the XMV player) call,
// replaced by host functions (see FUNCTIONS.md for addresses, names and
// evidence). API-level: the NV2A push buffer is never produced.
//
// State model. The game writes some Direct3D state straight into the lib's
// globals (inlined XDK header code): D3D__RenderState (0x13A630),
// D3D__TextureState (0x13A430) and D3D__DirtyFlags. Those arrays stay in
// guest memory as the single source of truth for render and texture stage
// states: the replaced setters write them exactly like the lib, and every
// draw reads them. Everything else (shaders, constants, streams, textures,
// targets) lives here.
//
// Resources keep their Xbox headers in guest memory (Common/Data/Lock/
// Format/Size): the game reads and aliases them. Data is a physical address
// of kernel contiguous memory (CPU address = Data | 0x80000000). Render
// targets exist on the GPU, keyed by Data: a texture whose Data is a render
// target's is sampled from the GPU copy. Other textures are converted from
// guest memory and cached by (Data, Format, Size); a content hash per frame
// of use catches CPU rewrites, and feeds the replacement-pack hook.
//
// Sizes are logical (Xbox) here; gpu:: gets physical ones (x render scale).

#include "hle/d3d8/d3d8.h"

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <dispatch/dispatch.h>
#include <unordered_map>
#include <vector>

#include "hle/d3d8/gpu.h"
#include "hle/d3d8/shaders.h"
#include "hle/d3d8/textures.h"
#include "hle/d3d8/xbox_d3d.h"
#include "hle/kernel/kernel_exports.h"
#include "hle/xinput/xinput.h"
#include "runtime/rotk_runtime.h"

using namespace rotkd3d;

extern "C" int rotk_window_auto_scale();  // window.mm
extern "C" bool rotk_window_display_pixels(int* w, int* h);  // window.mm

namespace {

// ---- configuration --------------------------------------------------------------
struct Config {
  uint32_t scale = 1;
  std::string shot_dir;
  uint32_t shot_every = 0;
  std::set<uint64_t> shot_frames;
  uint64_t exit_after = 0;
  bool pacing = true;
  bool fps60 = false;  // ROTK_FPS60=1: the game runs at 60 (BlockUntilVerticalBlank)
  std::string tex_dump;
  bool trace = false;
  bool shader_check = false;
  bool wbuffer = true;    // ROTK_WBUFFER=0: D3DZB_USEW draws use plain z/w depth (A/B checks)
  bool wide_hud = false;  // ROTK_WIDE_HUD=1: corner HUD elements anchored to the wide edges (donor
                          // heuristics, not tuned for this title: HUD stays 4:3 centred by default)
  uint32_t aniso = 1;     // ROTK_ANISO: anisotropic filtering of mipmapped textures (windowed: 8)
  uint32_t tex_recheck = 30;
  float pixel_offset = 0.5f;
  float wide = 1.0f;  // rotk_d3d8_set_widescreen: screen-sized targets this much wider
};
Config g_cfg;

void LoadConfig() {
  auto env = [](const char* n) -> const char* {
    const char* v = getenv(n);
    return v && *v ? v : nullptr;
  };
  // Windowed play renders at 2x by default (GPU cost at 2x: see handoff);
  // silent test runs (ROTK_BACKGROUND=1) keep the Xbox's 1x for the goldens.
  const char* bg = env("ROTK_BACKGROUND");
  bool windowed = !(bg && *bg != '0');
  // Windowed: render at the window's pixel density (e.g. 3x for a 900-point
  // window on a 2x panel: 1440 rows for 1800), at most 4x.
  if (windowed) g_cfg.scale = uint32_t(std::clamp(rotk_window_auto_scale(), 1, 4));
  if (windowed) g_cfg.aniso = 8;
  if (const char* v = env("ROTK_RENDER_SCALE"); v && !strcmp(v, "native")) {
    // The display's own resolution (addendum c): the frame's rows = the display's rows
    // (fullscreen) or the window's (rotk_window_auto_scale), up to 8x.
    int dw = 0, dh = 0;
    g_cfg.scale = uint32_t(std::clamp(getenv("ROTK_DISPLAY_SIZE") && rotk_window_display_pixels(&dw, &dh)
                                          ? dh / 480 : rotk_window_auto_scale(), 1, 8));
    rotk_log("d3d8: render scale native: %ux", g_cfg.scale);
  } else if (v) {
    g_cfg.scale = std::clamp(atoi(v), 1, 8);
  }
  if (const char* v = env("ROTK_SHOT_DIR"))
    g_cfg.shot_dir = v;
  else if (const char* v = env("ROTK_LOG_DIR"))
    g_cfg.shot_dir = v;
  if (const char* v = env("ROTK_SHOT_EVERY")) g_cfg.shot_every = uint32_t(atoi(v));
  if (const char* v = env("ROTK_SHOT_FRAMES")) {
    std::string s = v;
    size_t p = 0;
    while (p < s.size()) {
      size_t q = s.find(',', p);
      if (q == std::string::npos) q = s.size();
      g_cfg.shot_frames.insert(strtoull(s.substr(p, q - p).c_str(), nullptr, 10));
      p = q + 1;
    }
  }
  if (const char* v = env("ROTK_EXIT_AFTER")) g_cfg.exit_after = strtoull(v, nullptr, 10);
  if (const char* v = env("ROTK_NO_PACING")) g_cfg.pacing = atoi(v) == 0;
  if (const char* v = env("ROTK_FPS60")) g_cfg.fps60 = atoi(v) != 0;
  if (const char* v = env("ROTK_TEX_DUMP")) g_cfg.tex_dump = v;
  if (const char* v = env("ROTK_D3D_TRACE")) g_cfg.trace = atoi(v) != 0;
  // Widescreen: Polish sets it (rotk_d3d8_set_widescreen); ROTK_WIDESCREEN=1
  // alone gives 16:10 for tests of this library without Polish.
  if (const char* v = env("ROTK_WIDESCREEN"); v && atoi(v) && g_cfg.wide <= 1.0f)
    g_cfg.wide = (16.0f / 10.0f) / (4.0f / 3.0f);
  if (const char* v = env("ROTK_ANISO")) g_cfg.aniso = uint32_t(std::clamp(atoi(v), 1, 16));
  if (const char* v = env("ROTK_WBUFFER")) g_cfg.wbuffer = atoi(v) != 0;
  if (const char* v = env("ROTK_WIDE_HUD")) g_cfg.wide_hud = atoi(v) != 0;
  if (const char* v = env("ROTK_TEX_RECHECK")) g_cfg.tex_recheck = std::max(1, atoi(v));
  if (const char* v = env("ROTK_SHADER_CHECK")) g_cfg.shader_check = atoi(v) != 0;
  if (const char* v = env("ROTK_PIXEL_OFFSET")) g_cfg.pixel_offset = float(atof(v));
}

#define TRACE(...) \
  do {             \
    if (g_cfg.trace) rotk_log(__VA_ARGS__); \
  } while (0)

// First call of each entry point is logged once.
void Seen(const char* name) {
  static std::set<const char*> seen;
  if (seen.insert(name).second) rotk_log("d3d8: first call %s", name);
}

float F(uint32_t v) {
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
uint32_t U(float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  return v;
}

// ---- guest memory helpers --------------------------------------------------------
uint32_t RS(uint32_t i) { return rotk_rd32(kRenderStateVa + 4 * i); }
void SetRS(uint32_t i, uint32_t v) { rotk_wr32(kRenderStateVa + 4 * i, v); }
uint32_t TS(uint32_t stage, uint32_t i) {
  return rotk_rd32(kTextureStateVa + stage * kTextureStageStride + 4 * i);
}
void SetTS(uint32_t stage, uint32_t i, uint32_t v) {
  rotk_wr32(kTextureStateVa + stage * kTextureStageStride + 4 * i, v);
}

uint32_t CpuAddr(uint32_t data) { return data < 0x80000000u ? (data | 0x80000000u) : data; }

constexpr uint32_t kOrdMmAllocateContiguousMemoryEx = 166;
constexpr uint32_t kOrdMmFreeContiguousMemory = 171;

uint32_t AllocContiguous(RotkCtx& c, uint32_t size, uint32_t align) {
  uint32_t args[5] = {size, 0, 0x03FFFFFF, align, 0x404};
  uint32_t va = rotk_call_guest(c, rotk_kernel_thunk_va(kOrdMmAllocateContiguousMemoryEx), args, 5);
  if (!va) rotk_log("d3d8: MmAllocateContiguousMemoryEx(%u) failed", size);
  return va;
}
void FreeContiguous(RotkCtx& c, uint32_t va) {
  uint32_t args[1] = {va};
  rotk_call_guest(c, rotk_kernel_thunk_va(kOrdMmFreeContiguousMemory), args, 1);
}

// Resource header helpers.
struct Hdr {
  uint32_t va;
  uint32_t common() const { return rotk_rd32(va); }
  uint32_t data() const { return rotk_rd32(va + 4); }
  uint32_t format() const { return rotk_rd32(va + 0xC); }
  uint32_t size() const { return rotk_rd32(va + 0x10); }
  uint32_t type() const { return common() & COMMON_TYPE_MASK; }
};

// ---- render targets ----------------------------------------------------------------
struct Target {
  uint32_t data = 0;
  uint32_t w = 0, h = 0;  // logical
  uint32_t pw = 0, ph = 0;  // physical (render scale; screen-sized ones x g_cfg.wide)
  bool depth = false;
  gpu::TexId tex = 0;     // physical size
  gpu::TexId opaque = 0;  // view of tex with alpha = 1 (made on first use)
};
std::unordered_map<uint64_t, Target> g_targets;  // key: data << 1 | depth

Target* FindTarget(uint32_t data, bool depth) {
  auto it = g_targets.find(uint64_t(data) << 1 | depth);
  return it == g_targets.end() ? nullptr : &it->second;
}

Target* GetTarget(uint32_t data, uint32_t w, uint32_t h, bool depth) {
  if (Target* t = FindTarget(data, depth)) {
    if (t->w == w && t->h == h) return t;
    gpu::DestroyTexture(t->opaque);
    gpu::DestroyTexture(t->tex);
    g_targets.erase(uint64_t(data) << 1 | depth);
  }
  Target t;
  t.data = data;
  t.w = w;
  t.h = h;
  t.depth = depth;
  // Widescreen: the 640x480 screen (back/front buffer, depth, the game's
  // full-screen copies) is rendered wider; the logical size stays 640x480.
  bool wide = g_cfg.wide > 1.0f && w == 640 && h == 480;
  t.pw = (wide ? uint32_t(w * g_cfg.wide + 0.5f) : w) * g_cfg.scale;
  t.ph = h * g_cfg.scale;
  t.tex = gpu::CreateTexture(depth ? gpu::TexKind::Depth : gpu::TexKind::BGRA8, t.pw, t.ph, 1,
                             false, true);
  rotk_log("d3d8: render target %s %ux%u data %08X -> tex %u", depth ? "depth" : "colour", w, h,
          data, t.tex);
  return &(g_targets[uint64_t(data) << 1 | depth] = t);
}

// Logical size of a surface/texture header.
bool SurfaceSize(uint32_t hdr, uint32_t* w, uint32_t* h, uint32_t* xfmt) {
  Hdr H{hdr};
  TexLayout L;
  if (!DescribeTexture(H.format(), H.size(), &L)) return false;
  *w = L.w, *h = L.h, *xfmt = L.xfmt;
  return true;
}

// ---- textures --------------------------------------------------------------------------
struct CachedTex {
  gpu::TexId tex = 0;
  uint64_t hash = 0;
  uint64_t frame = ~0ull;    // last frame it was bound
  uint64_t checked = 0;      // frame its content was last hashed
  bool dynamic = false;      // CPU-written without a lock: re-hashed every frame it is bound
  TexLayout L;
};
std::unordered_map<uint64_t, CachedTex> g_texcache;
// Frame of the last CPU lock (LockRect) per texture Data: a locked texture
// is re-hashed at its next use. Others are re-hashed every
// g_cfg.tex_recheck frames (staggered), which catches CPU writes without a
// lock; hashing every bound texture every frame cost ~1-2 ms of CPU. Textures the CPU
// rewrites without a lock are re-hashed every frame they are bound (CachedTex::dynamic):
// linear ones from the start (the movie player's ring of 4 YUV plane sets, written in place,
// showed frames up to 30 frames old in a different order per plane), swizzled ones once a
// re-check finds them changed.
std::unordered_map<uint32_t, uint64_t> g_tex_locked;
RotkTextureHook g_tex_hook = nullptr;

// ---- vertex shaders -----------------------------------------------------------------
// Background compile of one vertex program (started at CreateVertexShader:
// all 264 are created at boot, long before their first draw).
struct VsJob {
  std::string msl;
  char tag[32];
  gpu::ProgId prog = 0;
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
};

struct VShader {
  std::vector<uint32_t> func;      // microcode (instructions only)
  uint32_t attr[16][3] = {};       // stream, offset, type per v register (stream 0xFF: unused)
  std::vector<std::pair<uint32_t, std::vector<uint32_t>>> constants;  // declaration consts
  gpu::ProgId prog = 0;
  bool failed = false;
  bool screen_space = false;       // oPos = v0 (2D: positions in screen pixels)
  bool ff = false;                 // fixed-function T&L (FVF or a declaration without a function)
  uint32_t fvf = 0;                // FVF of an FVF shader
  int8_t tc_reg[4] = {-1, -1, -1, -1};  // 2D: oT<s> = v<n> (n), else -1
  uint32_t index;
  std::shared_ptr<VsJob> job;      // pending background compile
};
std::vector<VShader> g_vshaders;
constexpr uint32_t kVsMagic = 0x56534831;  // 'VSH1' in the guest handle object

// ---- pixel shaders (fragment programs by key) ---------------------------------------
std::unordered_map<uint64_t, std::vector<std::pair<PsKey, gpu::ProgId>>> g_fprogs;

// ---- device -----------------------------------------------------------------------------
struct Stream {
  uint32_t vb = 0;  // header VA
  uint32_t stride = 0;
};
struct Device {
  bool created = false;
  uint32_t bb_w = 640, bb_h = 480;
  uint32_t back = 0, front = 0, zbuf = 0;  // surface header VAs
  uint32_t rt = 0, z = 0;                  // current targets (header VAs)
  float vs_c[192][4] = {};
  uint32_t const_mode = 0;
  uint32_t vs_handle = 0;
  // SelectVertexShader: the program runs from slot vs_start of the NV2A program memory
  // (LoadVertexShader); -1: SetVertexShader, the handle's own program.
  int32_t vs_start = -1;
  Stream streams[16];
  uint32_t idx_base = 0;  // SetIndices BaseVertexIndex
  uint32_t idx_data = 0;  // SetIndices pIndexData (unused by draws that pass indices)
  uint32_t tex[4] = {};
  uint32_t palette[4] = {};  // SetPalette: D3DPalette header per stage
  uint32_t ps = 0;        // pixel shader handle (0: fixed function)
  float transforms[10][16] = {};
  uint32_t material[17] = {};
  // D3DVIEWPORT8 {X, Y, Width, Height, MinZ, MaxZ}; vp_w == 0: the whole render target.
  uint32_t vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0;
  float vp_minz = 0, vp_maxz = 1;
  uint64_t frame = 0;
  uint64_t overlay_frames = 0, overlay_at_swap = 0;
  uint64_t last_vblank = 0;
  std::chrono::steady_clock::time_point last_swap;
  double frame_ms_sum = 0, frame_ms_max = 0, frame_busy_sum = 0;
  uint32_t frame_ms_n = 0;
  double block_wait_ms = 0;  // this frame's BlockUntilVerticalBlank waits (not busy)
  FILE* frame_log = nullptr;  // ROTK_FRAME_LOG: one line per Swap
  uint32_t draws = 0, frame_draws = 0;
  std::vector<std::pair<uint32_t, uint64_t>> pending_free;  // (contiguous VA, free at frame)
};
Device g_dev;

// Movie timing statistics (logged per movie): time inside UpdateOverlay and
// in BlockUntilVerticalBlank, and the gap between overlay frames.
struct OverlayStats {
  std::chrono::steady_clock::time_point last, movie_start;
  uint32_t movie_frames = 0;
  double in_ms = 0, wait_ms = 0, gap_ms = 0, gap_max = 0;
  uint32_t n = 0, late = 0;
} g_ov;

// ---- surfaces created by the HLE -----------------------------------------------------
uint32_t NewHeader(uint32_t bytes) { return rotk_hle_alloc(bytes, 16); }

uint32_t MakeSurface(RotkCtx& c, uint32_t w, uint32_t h, uint32_t xfmt) {
  uint32_t fmt, size;
  MakeTextureHeader(w, h, 1, 1, xfmt, false, false, &fmt, &size);
  if (!size) {  // swizzled/depth surfaces still describe their size linearly
    uint32_t pitch = w * FormatInfo(xfmt).bpp / 8;
    size = (w - 1) | (h - 1) << 12 | ((pitch + 63) / 64 - 1) << 24;
    fmt = (fmt & ~0xFFF00000u & ~0xF0000u) | 1u << 16;
  }
  uint32_t bytes = SizePitch(size) * h;
  uint32_t data = AllocContiguous(c, bytes, 0x1000);
  uint32_t hdr = NewHeader(0x20);
  rotk_wr32(hdr + 0, COMMON_TYPE_SURFACE | COMMON_D3DCREATED | 1);
  rotk_wr32(hdr + 4, data & 0x0FFFFFFF);
  rotk_wr32(hdr + 8, 0);
  rotk_wr32(hdr + 0xC, fmt);
  rotk_wr32(hdr + 0x10, size);
  rotk_wr32(hdr + 0x14, 0);
  return hdr;
}

void AddRef(uint32_t res) {
  if (res) rotk_wr32(res, rotk_rd32(res) + 1);
}

void InitPushSink();  // push sink (below)
void SyncDeviceFields();

void InitDefaultStates() {
  for (uint32_t i = RS_ZFUNC; i < RS_COUNT; i++) SetRS(i, 0);
  SetRS(RS_ZFUNC, 0x203);
  SetRS(RS_ALPHAFUNC, 0x207);
  SetRS(RS_SRCBLEND, 1);
  SetRS(RS_DESTBLEND, 0);
  SetRS(RS_ZWRITEENABLE, 1);
  SetRS(RS_SHADEMODE, 0x1D01);
  SetRS(RS_COLORWRITEENABLE, 0x01010101);
  SetRS(RS_STENCILZFAIL, STENCILOP_KEEP);
  SetRS(RS_STENCILPASS, STENCILOP_KEEP);
  SetRS(RS_STENCILFUNC, 0x207);
  SetRS(RS_STENCILMASK, 0xFF);
  SetRS(RS_STENCILWRITEMASK, 0xFF);
  SetRS(RS_BLENDOP, BLENDOP_ADD);
  SetRS(RS_FOGEND, U(1.0f));
  SetRS(RS_FOGDENSITY, U(1.0f));
  SetRS(RS_LIGHTING, 1);
  SetRS(RS_COLORVERTEX, 1);
  SetRS(RS_DIFFUSEMATERIALSOURCE, 1);
  SetRS(RS_SPECULARMATERIALSOURCE, 2);
  SetRS(RS_POINTSIZE, U(1.0f));
  SetRS(RS_POINTSIZE_MAX, U(64.0f));
  SetRS(RS_POINTSCALEENABLE, 0);
  SetRS(RS_FILLMODE, FILL_SOLID);
  SetRS(RS_BACKFILLMODE, FILL_SOLID);
  SetRS(RS_ZENABLE, 1);
  SetRS(RS_STENCILFAIL, STENCILOP_KEEP);
  SetRS(RS_FRONTFACE, CULL_CW);
  SetRS(RS_CULLMODE, CULL_CCW);
  SetRS(RS_TEXTUREFACTOR, 0xFFFFFFFF);
  SetRS(RS_MULTISAMPLEANTIALIAS, 1);
  SetRS(RS_MULTISAMPLEMASK, 0xFFFFFFFF);
  SetRS(RS_LINEWIDTH, U(1.0f));
  SetRS(RS_PRESENTATIONINTERVAL, 1);
  for (uint32_t s = 0; s < 4; s++) {
    for (uint32_t i = 0; i < TSS_COUNT; i++) SetTS(s, i, 0);
    SetTS(s, TSS_ADDRESSU, 1);
    SetTS(s, TSS_ADDRESSV, 1);
    SetTS(s, TSS_ADDRESSW, 1);
    SetTS(s, TSS_MAGFILTER, 1);
    SetTS(s, TSS_MINFILTER, 1);
    SetTS(s, TSS_MIPFILTER, 0);
    SetTS(s, TSS_MAXANISOTROPY, 1);
    SetTS(s, TSS_COLOROP, s == 0 ? TOP_MODULATE : TOP_DISABLE);
    SetTS(s, TSS_COLORARG1, TA_TEXTURE);
    SetTS(s, TSS_COLORARG2, TA_CURRENT);
    SetTS(s, TSS_ALPHAOP, s == 0 ? TOP_SELECTARG1 : TOP_DISABLE);
    SetTS(s, TSS_ALPHAARG1, TA_TEXTURE);
    SetTS(s, TSS_ALPHAARG2, TA_CURRENT);
    SetTS(s, TSS_RESULTARG, TA_CURRENT);
    SetTS(s, TSS_TEXCOORDINDEX, s);
  }
}

// ---- texture binding -------------------------------------------------------------------
struct Bound {
  gpu::TexId tex = 0;
  uint8_t kind = 0;  // 0 none, 1 2D, 2 cube
  float sx = 1, sy = 1;  // texture coordinate scale (LIN_ formats: 1/size)
  uint32_t levels = 1;
};

void DumpTexture(uint64_t hash, const TexLayout& L, const std::vector<uint8_t>& bgra) {
  std::vector<uint8_t> rgba(bgra.size());
  for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
    rgba[i] = bgra[i + 2];
    rgba[i + 1] = bgra[i + 1];
    rgba[i + 2] = bgra[i];
    rgba[i + 3] = bgra[i + 3];
  }
  char path[512];
  std::snprintf(path, sizeof path, "%s/%016llx.png", g_cfg.tex_dump.c_str(),
                (unsigned long long)hash);
  gpu::WritePng(path, L.w, L.h, rgba.data());
}

Bound BindTexture(uint32_t hdr, int stage) {
  Bound b;
  if (!hdr) return b;
  Hdr H{hdr};
  uint32_t data = H.data(), fmt = H.format(), size = H.size();
  TexLayout L;
  if (!DescribeTexture(fmt, size, &L)) {
    static std::set<uint32_t> warned;
    if (warned.insert(FmtFormat(fmt)).second)
      rotk_log("d3d8: texture %08X: unknown format %02X", hdr, FmtFormat(fmt));
    return b;
  }
  if (L.linear) b.sx = 1.0f / L.w, b.sy = 1.0f / L.h;
  b.kind = L.cube ? 2 : 1;
  if (Target* t = FindTarget(data, false)) {
    // The target's storage is BGRA8; an Xbox format without alpha reads 1.
    bool has_alpha = true;
    switch (L.xfmt) {
      case XF_R5G6B5: case XF_LIN_R5G6B5: case XF_X8R8G8B8: case XF_LIN_X8R8G8B8:
      case XF_X1R5G5B5: case XF_LIN_X1R5G5B5:
        has_alpha = false;
    }
    if (!has_alpha && !t->opaque) t->opaque = gpu::OpaqueView(t->tex);
    b.tex = has_alpha ? t->tex : t->opaque;
    b.kind = 1;
    return b;
  }
  if (FormatInfo(L.xfmt).depth) {
    if (Target* t = FindTarget(data, true)) {
      b.tex = 0;  // depth textures are not sampled yet
      static bool once = false;
      if (!once) rotk_log("d3d8: depth texture %08X sampled (shadow maps not supported)", hdr), once = true;
      (void)t;
    }
    return b;
  }
  uint64_t key = (uint64_t(data) << 32) ^ (uint64_t(fmt) * 0x9E3779B1u) ^ size;
  // P8: the palette is part of the texture's look (same texels, other palette = other key).
  const uint32_t* pal = nullptr;
  if (L.xfmt == XF_P8) {
    uint32_t ph = stage >= 0 && stage < 4 ? g_dev.palette[stage] : 0;
    uint32_t pd = ph ? CpuAddr(Hdr{ph}.data()) : 0;
    if (pd && rotk_is_committed(pd) && rotk_is_committed(pd + 1023)) {
      pal = static_cast<const uint32_t*>(rotk_ptr(pd));
      key ^= HashBytes(pal, 1024) * 0x100000001B3ull;
    }
  }
  struct PaletteScope {
    explicit PaletteScope(const uint32_t* p) { SetDecodePalette(p); }
    ~PaletteScope() { SetDecodePalette(nullptr); }
  } palette_scope(pal);
  CachedTex& ct = g_texcache[key];
  // Dev: ROTK_DEV_LOG_TEX=a-b logs every texture cache decision in frames a..b.
  static const std::pair<uint64_t, uint64_t> dev_tex = [] {
    const char* v = getenv("ROTK_DEV_LOG_TEX");
    unsigned long long a = 1, b = 0;
    if (v) std::sscanf(v, "%llu-%llu", &a, &b);
    return std::make_pair(uint64_t(a), uint64_t(b));
  }();
  const bool dev_log = g_dev.frame >= dev_tex.first && g_dev.frame <= dev_tex.second;
  auto lk0 = g_tex_locked.find(data);
  if (dev_log)
    rotk_log("d3d8: tex f%llu %08X %ux%u fmt %02X: cached %d (bound f%llu, checked f%llu), lock f%lld",
             (unsigned long long)g_dev.frame, data, L.w, L.h, L.xfmt, ct.tex != 0,
             (unsigned long long)ct.frame, (unsigned long long)ct.checked,
             lk0 == g_tex_locked.end() ? -1ll : (long long)lk0->second);
  if (ct.tex && ct.frame == g_dev.frame) {
    b.tex = ct.tex;
    b.levels = ct.L.levels;
    return b;
  }
  if (ct.tex) {
    ct.frame = g_dev.frame;
    bool locked = lk0 != g_tex_locked.end() && lk0->second >= ct.checked;
    bool due = ct.dynamic || (g_dev.frame + (key >> 7)) % g_cfg.tex_recheck == 0;
    if (!locked && !due) {
      b.tex = ct.tex;
      b.levels = ct.L.levels;
      return b;
    }
  }
  uint32_t cpu = CpuAddr(data);
  if (!rotk_is_committed(cpu) || !rotk_is_committed(cpu + L.total - 1)) {
    static std::set<uint32_t> warned;
    if (warned.insert(data).second)
      rotk_log("d3d8: texture %08X data %08X (%u bytes) not in guest memory", hdr, data, L.total);
    return Bound{};
  }
  const uint8_t* src = static_cast<const uint8_t*>(rotk_ptr(cpu));
  uint64_t hash = HashBytes(src, L.total) ^ (uint64_t(fmt) << 1) ^ (uint64_t(size) << 33);
  const bool was_locked = lk0 != g_tex_locked.end() && lk0->second >= ct.checked;
  ct.frame = g_dev.frame;
  ct.checked = g_dev.frame;
  if (dev_log)
    rotk_log("d3d8: tex f%llu %08X hashed %016llX (%s)", (unsigned long long)g_dev.frame, data,
             (unsigned long long)hash, ct.tex && ct.hash == hash ? "same" : "new upload");
  if (ct.tex && ct.hash == hash) {
    b.tex = ct.tex;
    b.levels = ct.L.levels;
    return b;
  }
  if (L.linear) ct.dynamic = true;
  if (ct.tex && !ct.dynamic && !was_locked) {
    ct.dynamic = true;
    static uint32_t n = 0;
    if (n++ < 8) rotk_log("d3d8: texture %08X %ux%u changed without a lock: re-hashed every frame", data, L.w, L.h);
  }
  if (ct.tex) gpu::DestroyTexture(ct.tex);
  ct.hash = hash;
  ct.L = L;
  uint32_t levels = L.levels;
  // Hook (replacement packs): whole-texture replacement by content hash.
  if (g_tex_hook) {
    RotkTextureInfo info{hash, L.w, L.h, L.xfmt, L.levels, src, L.total};
    uint32_t w2 = 0, h2 = 0;
    const uint8_t* rgba = nullptr;
    if (g_tex_hook(info, &w2, &h2, &rgba) && rgba && w2 && h2) {
      std::vector<uint8_t> bgra(size_t(w2) * h2 * 4);
      for (size_t i = 0; i < size_t(w2) * h2; i++) {
        bgra[4 * i] = rgba[4 * i + 2];
        bgra[4 * i + 1] = rgba[4 * i + 1];
        bgra[4 * i + 2] = rgba[4 * i];
        bgra[4 * i + 3] = rgba[4 * i + 3];
      }
      // A mipmapped original gets a full mip chain of the replacement (built on
      // the GPU): world textures would shimmer at a distance without one.
      uint32_t lv2 = 1;
      if (levels > 1)
        while ((std::max(w2, h2) >> lv2) > 0) lv2++;
      ct.tex = gpu::CreateTexture(gpu::TexKind::BGRA8, w2, h2, lv2, false, false);
      gpu::UploadTexture(ct.tex, 0, 0, w2, h2, bgra.data(), w2 * 4);
      if (lv2 > 1) gpu::GenerateMips(ct.tex);
      ct.L.levels = lv2;
      b.tex = ct.tex;
      b.levels = lv2;
      return b;
    }
  }
  gpu::TexKind kind = HostKind(L.xfmt);
  ct.tex = gpu::CreateTexture(kind, L.w, L.h, levels, L.cube, false);
  std::vector<uint8_t> buf;
  uint32_t pitch;
  for (uint32_t face = 0; face < (L.cube ? 6u : 1u); face++)
    for (uint32_t lv = 0; lv < levels; lv++) {
      ConvertLevel(L, lv, src + face * L.face_size + L.level_offset[lv], &buf, &pitch);
      gpu::UploadTexture(ct.tex, face, lv, std::max(1u, L.w >> lv), std::max(1u, L.h >> lv),
                         buf.data(), pitch);
      if (lv == 0 && face == 0 && !g_cfg.tex_dump.empty() && kind == gpu::TexKind::BGRA8)
        DumpTexture(hash, L, buf);
    }
  TRACE("d3d8: texture %08X %s %ux%u lv %u -> %u (hash %016llx)", hdr, FormatInfo(L.xfmt).name,
        L.w, L.h, levels, ct.tex, (unsigned long long)hash);
  b.tex = ct.tex;
  b.levels = levels;
  return b;
}

// ---- state -> gpu ------------------------------------------------------------------------
uint8_t MtlBlend(uint32_t v) {
  switch (v) {
    case 0: return 0;
    case 1: return 1;
    case 0x300: return 2;
    case 0x301: return 3;
    case 0x302: return 4;
    case 0x303: return 5;
    case 0x304: return 8;
    case 0x305: return 9;
    case 0x306: return 6;
    case 0x307: return 7;
    case 0x308: return 10;
    case 0x8001: return 11;
    case 0x8002: return 12;
    case 0x8003: return 13;
    case 0x8004: return 14;
  }
  return 1;
}
uint8_t MtlBlendOp(uint32_t v) {
  switch (v) {
    case BLENDOP_SUBTRACT: return 1;
    case BLENDOP_REVSUBTRACT: case 0xF006: return 2;
    case BLENDOP_MIN: return 3;
    case BLENDOP_MAX: return 4;
  }
  return 0;
}
uint8_t MtlCmp(uint32_t v) { return v >= 0x200 && v <= 0x207 ? uint8_t(v - 0x200) : 7; }
uint8_t MtlStencilOp(uint32_t v) {
  switch (v) {
    case STENCILOP_KEEP: return 0;
    case STENCILOP_ZERO: return 1;
    case STENCILOP_REPLACE: return 2;
    case STENCILOP_INCRSAT: return 3;
    case STENCILOP_DECRSAT: return 4;
    case STENCILOP_INVERT: return 5;
    case STENCILOP_INCR: return 6;
    case STENCILOP_DECR: return 7;
  }
  return 0;
}
uint8_t MtlAddr(uint32_t v) {
  switch (v) {
    case 1: return 2;  // wrap -> repeat
    case 2: return 3;  // mirror
    case 3: return 0;  // clamp
    case 4: return 5;  // border
    case 5: return 0;  // clamp to edge
  }
  return 2;
}

void ColorF(uint32_t argb, float* out) {
  out[0] = ((argb >> 16) & 0xFF) / 255.f;
  out[1] = ((argb >> 8) & 0xFF) / 255.f;
  out[2] = (argb & 0xFF) / 255.f;
  out[3] = (argb >> 24) / 255.f;
}

gpu::ProgId FragmentProgram(const PsKey& k) {
  uint64_t h = k.Hash();
  auto& bucket = g_fprogs[h];
  for (auto& [key, prog] : bucket)
    if (key == k) return prog;
  std::string msl = GeneratePsh(k);
  char tag[64];
  std::snprintf(tag, sizeof tag, "ps_%016llx", (unsigned long long)h);
  if (const char* dir = getenv("ROTK_DUMP_MSL")) {
    std::string path = std::string(dir) + "/" + tag + ".metal";
    if (FILE* f = fopen(path.c_str(), "w")) fwrite(msl.data(), 1, msl.size(), f), fclose(f);
  }
  gpu::ProgId p = gpu::CompileFragment(msl, tag);
  rotk_log("d3d8: fragment program %s (%s) -> %u", tag, k.combiners ? "combiners" : "fixed", p);
  bucket.push_back({k, p});
  return p;
}

// Translates vs to MSL (false: malformed program, logged).
bool TranslateVs(VShader& vs, std::string* msl, char (&tag)[32]) {
  uint32_t n = 0;
  if (vs.ff || vs.func.empty()) {
    vs.ff = true;
    std::snprintf(tag, sizeof tag, "vs_ff");
    return TranslateFixedFunctionVs(msl);
  }
  std::snprintf(tag, sizeof tag, "vs_%u", vs.index);
  if (!TranslateVsh(vs.func.data(), uint32_t(vs.func.size() / 4), msl, &n)) return false;
  // A pass-through program (oPos = v0, nothing else writes it) draws 2D in
  // screen pixels: widescreen HUD anchoring reads its positions.
  size_t at = msl->find("float4 mac = (V[0]);\n    R[12].xyzw = mac.xyzw;");
  size_t writes = 0;
  for (size_t q = msl->find("R[12]."); q != std::string::npos; q = msl->find("R[12].", q + 1)) writes++;
  vs.screen_space = at != std::string::npos && writes == 1;
  // Its texture coordinates, when copied straight from an input (oT<s> =
  // O[9 + s]): the clamp below reads their range.
  for (int t = 0; t < 4 && vs.screen_space; t++) {
    char out[40];
    std::snprintf(out, sizeof out, ");\n    O[%d].xyzw = mac.xyzw;", 9 + t);
    size_t q = msl->find(out);
    size_t v = q == std::string::npos ? q : msl->rfind("float4 mac = (V[", q);
    if (v != std::string::npos && v + 20 >= q) vs.tc_reg[t] = int8_t(atoi(msl->c_str() + v + 16));
  }
  if (const char* dir = getenv("ROTK_DUMP_MSL")) {
    std::string path = std::string(dir) + "/" + tag + ".metal";
    if (FILE* f = fopen(path.c_str(), "w")) fwrite(msl->data(), 1, msl->size(), f), fclose(f);
  }
  return true;
}

// Starts compiling vs on one low-priority background queue (one core: the
// boot screens keep their 60 Hz).
void PrecompileVertexProgram(VShader& vs) {
  auto job = std::make_shared<VsJob>();
  if (!TranslateVs(vs, &job->msl, job->tag)) {
    vs.failed = true;
    return;
  }
  vs.job = job;
  static dispatch_queue_t q = dispatch_queue_create(
      "rotk.vs-compile",
      dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
  auto* hold = new std::shared_ptr<VsJob>(job);
  dispatch_async_f(q, hold, [](void* p) {
    auto* h = static_cast<std::shared_ptr<VsJob>*>(p);
    (*h)->prog = gpu::CompileVertex((*h)->msl, (*h)->tag);
    (*h)->msl.clear();
    dispatch_semaphore_signal((*h)->done);
    delete h;
  });
}

gpu::ProgId VertexProgram(VShader& vs) {
  if (vs.prog || vs.failed) return vs.prog;
  if (vs.job) {  // precompiled in the background: wait for it if still running
    dispatch_semaphore_wait(vs.job->done, DISPATCH_TIME_FOREVER);
    vs.prog = vs.job->prog;
    vs.job.reset();
  } else {
    std::string msl;
    char tag[32];
    if (TranslateVs(vs, &msl, tag)) vs.prog = gpu::CompileVertex(msl, tag);
  }
  if (!vs.prog) vs.failed = true;
  return vs.prog;
}

// Current colour/depth targets.
struct TargetsNow {
  Target* color = nullptr;
  Target* depth = nullptr;
  uint32_t w = 0, h = 0;
  uint32_t pw = 0, ph = 0;  // physical
};

TargetsNow BindTargets() {
  TargetsNow t;
  uint32_t w = 0, h = 0, f = 0;
  if (g_dev.rt && SurfaceSize(g_dev.rt, &w, &h, &f)) {
    t.color = GetTarget(Hdr{g_dev.rt}.data(), w, h, false);
    t.w = w, t.h = h, t.pw = t.color->pw, t.ph = t.color->ph;
  }
  if (g_dev.z && SurfaceSize(g_dev.z, &w, &h, &f)) {
    t.depth = GetTarget(Hdr{g_dev.z}.data(), w, h, true);
    if (!t.color) t.w = w, t.h = h, t.pw = t.depth->pw, t.ph = t.depth->ph;
  }
  gpu::SetTargets(t.color ? t.color->tex : 0, t.depth ? t.depth->tex : 0, t.pw, t.ph);
  return t;
}

// ---- drawing ------------------------------------------------------------------------------

// Xbox primitive -> Metal primitive + index list (in vertex numbers 0..n-1)
// for types Metal lacks. Returns false if nothing to draw.
bool Topology(uint32_t prim, uint32_t n, uint32_t* mtl, std::vector<uint16_t>* idx,
              bool* need_idx) {
  *need_idx = false;
  idx->clear();
  switch (prim) {
    case PT_POINTLIST: *mtl = 0; return n > 0;
    case PT_LINELIST: *mtl = 1; return n >= 2;
    case PT_LINESTRIP: *mtl = 2; return n >= 2;
    case PT_TRIANGLELIST: *mtl = 3; return n >= 3;
    case PT_TRIANGLESTRIP: *mtl = 4; return n >= 3;
    case PT_QUADSTRIP: *mtl = 4; return n >= 4;
    case PT_LINELOOP:
      *mtl = 2;
      *need_idx = true;
      for (uint32_t i = 0; i < n; i++) idx->push_back(uint16_t(i));
      idx->push_back(0);
      return n >= 2;
    case PT_TRIANGLEFAN:
    case PT_POLYGON:
      *mtl = 3;
      *need_idx = true;
      for (uint32_t i = 1; i + 1 < n; i++) {
        idx->push_back(0);
        idx->push_back(uint16_t(i));
        idx->push_back(uint16_t(i + 1));
      }
      return n >= 3;
    case PT_QUADLIST:
      *mtl = 3;
      *need_idx = true;
      for (uint32_t q = 0; q + 3 < n; q += 4) {
        uint16_t b = uint16_t(q);
        idx->insert(idx->end(), {b, uint16_t(b + 1), uint16_t(b + 2), b, uint16_t(b + 2),
                                 uint16_t(b + 3)});
      }
      return n >= 4;
  }
  return false;
}

struct StreamSrc {
  uint32_t va = 0;      // guest VA of vertex 0
  uint32_t stride = 0;
};

// Widescreen HUD anchoring: the NDC x offset for a 2D (screen-space) draw.
// HUD elements in a top/bottom corner region of the 640x480 screen move with
// their side's edge of the wide screen; everything else stays centred (menus,
// dialogs, subtitles, mid-height elements). Full-screen quads are stretched by
// the shader instead. z = 1/wide.
//
// An element is drawn in pieces (the health bar: icon, bar, fill, end cap;
// the enemy bar: cap, bar, fill, portrait; the button cluster: ring, buttons,
// label), so the decision is per element, not per draw: the sprites of the
// previous frame that touch (12-pixel margin) form one element, and the
// element's box decides its edge. A piece inherits the decision of the element
// its centre lies in; a piece new this frame decides by its own box.
// Text (a font page: a texture used by many 2D draws; the game draws one glyph
// per draw) follows an element only when it lies inside it or, for a top
// element, under it (the action label: "Open", "Talk / To", "Go!"); text inside
// a panel (an untextured or large 2D draw: dialogs, tutorial boxes) never moves.
struct HudBox {
  float x0, y0, x1, y1;
  bool Contains(float x, float y, float mx, float up, float down) const {
    return x >= x0 - mx && x <= x1 + mx && y >= y0 - up && y <= y1 + down;
  }
};
struct HudElement {
  HudBox box;
  float shift;
  bool text;  // a string (glyphs, maybe with its icon): its glyphs follow it, not other rules
};
struct HudPiece {
  HudBox box;
  bool text;
  bool icon = false;  // a sprite that joins a line of text as its icon
};
std::unordered_map<uint32_t, uint32_t> g_hud_tex_prev, g_hud_tex_now;
std::vector<HudPiece> g_hud_pieces;  // this frame's sprites and glyphs (not panels)
std::vector<HudBox> g_hud_panels, g_hud_panels_prev;
std::vector<HudElement> g_hud_elements;  // from the previous frame's sprites
uint64_t g_hud_frame = 0;
// The frame opens with a full-screen 2D backdrop (the front end's cloudy sky:
// title, menus, inventory scroll, card album, spellbook): a 4:3 composition
// around a 3D scroll or book whose pages hold the 2D text and buttons. Nothing
// in it is anchored; edge anchoring tore page text off the book.
bool g_hud_menu_scene = false;

// The edge an element (or a lone piece) belongs to: -(1 - z) left, 1 - z
// right, 0 centred.
float HudEdge(const HudBox& b, float z) {
  // Corner band by the element's centre (the Remembrall's glow reaches up
  // to y 282 while the icon sits in the bottom-left corner).
  float cx = (b.x0 + b.x1) / 2, cy = (b.y0 + b.y1) / 2;
  if (cy > 480 * 0.38f && cy < 480 * 0.62f) return 0;
  if (cx < 240 && b.x0 < 160 && b.x1 <= 400) return -(1 - z);
  if (cx > 400 && b.x1 > 480 && b.x0 >= 240) return 1 - z;
  return 0;
}

// Two pieces belong to one element: sprites that touch (12-pixel margin), or
// - when either is text - pieces on one line (vertical overlap at least half
// the lower one's height) with a gap under 0.6 of the taller one's height: a
// string's glyphs, its decorative initial (a larger glyph, sometimes from
// another texture) and the button icon in front of it ("(A) Select"; an
// icon at most 1.6x the line's height).
bool HudJoin(const HudPiece& a, const HudPiece& b) {
  const HudBox &p = a.box, &q = b.box;
  // Stacked legend icons ("(A) Select" over "(B) Exit", 7 px apart) each
  // belong to their own line, not to one taller sprite element.
  if (!a.text && !b.text && a.icon && b.icon) return false;
  if (!a.text && !b.text)
    return p.x0 <= q.x1 + 12 && q.x0 <= p.x1 + 12 && p.y0 <= q.y1 + 12 && q.y0 <= p.y1 + 12;
  float hp = p.y1 - p.y0, hq = q.y1 - q.y0;
  // A sprite joins a line of text only as an icon of about the glyphs' size:
  // the button cluster (129 px) must not swallow its label ("Dodge").
  if (a.text != b.text && (a.text ? hq > 1.6f * hp : hp > 1.6f * hq)) return false;
  float overlap = std::min(p.y1, q.y1) - std::max(p.y0, q.y0);
  float gap = std::max(p.x0, q.x0) - std::min(p.x1, q.x1);
  return overlap >= 0.5f * std::min(hp, hq) && gap <= 0.6f * std::max(hp, hq);
}

void HudNewFrame(float z) {
  g_hud_tex_prev.swap(g_hud_tex_now);
  g_hud_tex_now.clear();
  g_hud_panels_prev.swap(g_hud_panels);
  g_hud_panels.clear();
  // Union the previous frame's pieces into elements.
  for (HudPiece& p : g_hud_pieces)
    if (!p.text)
      for (const HudPiece& t : g_hud_pieces)
        if (t.text && HudJoin(p, t)) {
          p.icon = true;
          break;
        }
  std::vector<HudPiece> el;
  for (const HudPiece& b : g_hud_pieces) {
    HudPiece m = b;
    for (bool merged = true; merged;) {
      merged = false;
      for (size_t i = 0; i < el.size(); i++) {
        const HudPiece& e = el[i];
        if (HudJoin(m, e)) {
          m.box = {std::min(m.box.x0, e.box.x0), std::min(m.box.y0, e.box.y0),
                   std::max(m.box.x1, e.box.x1), std::max(m.box.y1, e.box.y1)};
          m.text = m.text || e.text;
          m.icon = m.icon || e.icon;
          el.erase(el.begin() + long(i));
          merged = true;
          break;
        }
      }
    }
    el.push_back(m);
  }
  g_hud_pieces.clear();
  g_hud_elements.clear();
  // A string never picks an edge itself (menu titles, subtitles, the menus'
  // "(A) Select" legends, whose icon is a font glyph on some screens and a
  // sprite on others): it belongs to a centred layout. Only sprite elements
  // (bars, button cluster, icons) are anchored.
  for (const HudPiece& e : el) g_hud_elements.push_back({e.box, e.text ? 0.0f : HudEdge(e.box, z), e.text});
  // A string that lies in, under or (bottom corners) next to an anchored
  // element moves with it, as a whole: the action label "Go!" / "Close" under
  // the button cluster, the bean count by its icon.
  for (HudElement& t : g_hud_elements) {
    if (!t.text || t.shift != 0) continue;
    float cx = (t.box.x0 + t.box.x1) / 2, cy = (t.box.y0 + t.box.y1) / 2;
    for (const HudElement& e : g_hud_elements)
      if (&e != &t && e.shift != 0 &&
          (e.box.y1 <= 480 * 0.40f ? e.box.Contains(cx, cy, 24, 8, 96)
                                   : e.box.Contains(cx, cy, 24, 40, 40))) {
        t.shift = e.shift;
        break;
      }
  }
}

// Widescreen: may this draw keep its vertices on the 4:3 screen's left/right edge at the wide
// edges (shader epilogue, snap.y)? Yes for large 2D pieces: full-screen quads and backdrop
// tiles (the title's side-by-side sky tiles) stretch to cover the wide screen. No for small
// sprites: the tutorial button prompts are fixed-function quads at x 0.87..0.995 of the screen,
// whose right edge then stayed at the wide edge while the left one moved in - the round "A" /
// "X" keys became ovals about 2:1 (owner test c96b3d1). Programs other than 2D / fixed function
// are not evaluated here (their 3D has w != 1 anyway): they keep the old rule.
// 2 (fill) for large 2D that covers the whole height (title and loading pictures, movies, fades):
// stretching them made the 4:3 title 1.2x wide («прям дуже розтягнуто», owner test dcb2fc2), so
// the shader scales them evenly to the wide width instead and crops the top and bottom.
int WideEdgeStretch(const VShader& vs, const StreamSrc* streams, uint32_t first, uint32_t count,
                     const VsUniforms& vu) {
  if (!vs.screen_space && !vs.ff) return 1;
  uint32_t s = vs.attr[0][0], off = vs.attr[0][1];
  if (s >= 16 || !streams[s].va || !count || count > 4096) return 1;
  float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;  // NDC (x, y: -1..1)
  for (uint32_t v = first; v < first + count; v++) {
    uint32_t a = streams[s].va + v * streams[s].stride + off;
    if (!rotk_is_committed(a) || !rotk_is_committed(a + 11)) return 1;
    float p[4] = {F(rotk_rd32(a)), F(rotk_rd32(a + 4)), F(rotk_rd32(a + 8)), 1.0f};
    float x, y;
    if (vs.screen_space) {
      x = p[0] / 320.0f - 1, y = p[1] / 240.0f - 1;
    } else {
      float c[4];
      for (int k = 0; k < 4; k++) c[k] = vu.c[k][0] * p[0] + vu.c[k][1] * p[1] + vu.c[k][2] * p[2] + vu.c[k][3];
      if (std::fabs(c[3] - 1.0f) > 1e-4f) return 1;  // perspective: no edge rule in the shader
      x = c[0], y = c[1];
    }
    x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
  }
  // Large: at least 0.3 of the width (192 of 640 pixels) or half the height.
  if (y0 <= -0.98f && y1 >= 0.98f && x1 - x0 >= 0.3f) return 2;
  return x1 - x0 >= 0.6f || y1 - y0 >= 1.0f ? 1 : 0;
}

int g_clip_logs = 0;

// Screen-space draw entirely left or right of the 640-wide screen.
bool Outside43(const VShader& vs, const StreamSrc* streams, uint32_t first, uint32_t count) {
  uint32_t s = vs.attr[0][0], off = vs.attr[0][1];
  if (s >= 16 || !streams[s].va || !count || count > 4096) return false;
  float x0 = 1e9f, x1 = -1e9f;
  for (uint32_t v = first; v < first + count; v++) {
    uint32_t a = streams[s].va + v * streams[s].stride + off;
    if (!rotk_is_committed(a) || !rotk_is_committed(a + 3)) return false;
    float x = F(rotk_rd32(a));
    x0 = std::min(x0, x), x1 = std::max(x1, x);
  }
  return x0 >= 640.0f || x1 <= 0.0f;
}

float HudAnchor(const VShader& vs, const StreamSrc* streams, uint32_t first, uint32_t count,
                float z) {
  if (g_hud_frame != g_dev.frame) {
    HudNewFrame(z);
    g_hud_frame = g_dev.frame;
    g_hud_menu_scene = false;
  }
  uint32_t s = vs.attr[0][0], off = vs.attr[0][1], type = vs.attr[0][2];
  if (s >= 16 || !streams[s].va || !count || (type != VSDT_FLOAT3 && type != VSDT_FLOAT4) ||
      count > 4096)
    return 0;
  HudBox b{1e9f, 1e9f, -1e9f, -1e9f};
  for (uint32_t v = first; v < first + count; v++) {
    uint32_t a = streams[s].va + v * streams[s].stride + off;
    if (!rotk_is_committed(a) || !rotk_is_committed(a + 7)) return 0;
    float x = F(rotk_rd32(a)), y = F(rotk_rd32(a + 4));
    b = {std::min(b.x0, x), std::min(b.y0, y), std::max(b.x1, x), std::max(b.y1, y)};
  }
  if (b.x0 <= 2 && b.x1 >= 638) {  // full width: the shader stretches it
    if (g_dev.frame_draws == 1 && b.y0 <= 2 && b.y1 >= 478 && !RS(RS_ZENABLE)) g_hud_menu_scene = true;
    return 0;
  }
  uint32_t tex = g_dev.tex[0] ? Hdr{g_dev.tex[0]}.data() : 0;
  bool panel = !tex || b.x1 - b.x0 > 300 || b.y1 - b.y0 > 200;
  uint32_t uses = tex ? ++g_hud_tex_now[tex] : 0;
  auto prev = tex ? g_hud_tex_prev.find(tex) : g_hud_tex_prev.end();
  bool text = !panel && (uses > 8 || (prev != g_hud_tex_prev.end() && prev->second > 8));
  float cx = (b.x0 + b.x1) / 2, cy = (b.y0 + b.y1) / 2;
  float shift = 0;
  if (panel) {
    g_hud_panels.push_back(b);
  } else if (text) {
    g_hud_pieces.push_back({b, true});
    bool in_panel = false, in_string = false;
    for (const auto* list : {&g_hud_panels, &g_hud_panels_prev})
      for (const HudBox& p : *list) in_panel = in_panel || p.Contains(cx, cy, 0, 0, 0);
    if (!in_panel)
      for (const HudElement& e : g_hud_elements)
        if (e.text && e.box.Contains(cx, cy, 2, 2, 2)) {
          shift = e.shift, in_string = true;
          break;
        }
    if (!in_panel && !in_string)
      for (const HudElement& e : g_hud_elements)
        if (e.shift != 0 && e.box.Contains(cx, cy, 24, 8, e.box.y1 <= 480 * 0.40f ? 96 : 8)) {
          shift = e.shift;
          break;
        }
  } else {
    g_hud_pieces.push_back({b, false});
    bool found = false;
    for (const HudElement& e : g_hud_elements)
      if (e.box.Contains(cx, cy, 12, 12, 12)) {
        shift = e.shift, found = true;
        break;
      }
    if (!found) shift = HudEdge(b, z);
  }
  static const uint64_t dev_hud = [] {
    const char* v = getenv("ROTK_DEV_HUD_LOG");
    return v ? strtoull(v, nullptr, 10) : 0ull;
  }();
  // Menu scene: pieces are still recorded (the next frame's elements).
  if (g_hud_menu_scene) shift = 0;
  if (dev_hud && g_dev.frame == dev_hud)
    rotk_log("hud: frame %llu draw %u tex %08X%s%s box %.0f,%.0f-%.0f,%.0f shift %+.3f",
            (unsigned long long)g_dev.frame, g_dev.draws, tex, text ? " text" : "",
            panel ? " panel" : "", b.x0, b.y0, b.x1, b.y1, shift);
  return shift;
}

// A 2D draw whose texture coordinates for stage s stay within the texture
// (0..1 after the LIN_ scale) never repeats it: WRAP and CLAMP differ only in
// the half texel at its edges. Above 1x, pixel centres fall into that half
// texel and WRAP blends in the opposite edge - a dark seam between the title
// screen's side-by-side tiles (sky, logo). Such a draw samples clamped.
void UvRangeInUnit(const VShader& vs, const StreamSrc* streams, uint32_t first, uint32_t count,
                   int stage, float sx, float sy, bool* u_in, bool* v_in) {
  int r = vs.tc_reg[stage];
  if (r < 0 || r >= 16) return;
  uint32_t s = vs.attr[r][0], off = vs.attr[r][1], type = vs.attr[r][2];
  if (s >= 16 || !streams[s].va || !count || count > 4096 ||
      (type != VSDT_FLOAT2 && type != VSDT_FLOAT3 && type != VSDT_FLOAT4))
    return;
  const float lo = -1.0f / 4096, hi = 1 + 1.0f / 4096;
  bool u = true, v = true;
  for (uint32_t i = first; i < first + count && (u || v); i++) {
    uint32_t a = streams[s].va + i * streams[s].stride + off;
    if (!rotk_is_committed(a) || !rotk_is_committed(a + 7)) return;
    float tu = F(rotk_rd32(a)) * sx, tv = F(rotk_rd32(a + 4)) * sy;
    u = u && tu >= lo && tu <= hi;
    v = v && tv >= lo && tv <= hi;
  }
  *u_in = u, *v_in = v;
}

// Four 2D vertices on two x and two y values: a screen-aligned rectangle (a
// background or logo tile, whose edges the 1x pixel snap may move).
bool AxisAlignedQuad(const VShader& vs, const StreamSrc* streams, uint32_t first, uint32_t count) {
  uint32_t s = vs.attr[0][0], off = vs.attr[0][1];
  if (count != 4 || s >= 16 || !streams[s].va) return false;
  float xs[2], ys[2];
  int nx = 0, ny = 0;
  for (uint32_t i = first; i < first + 4; i++) {
    uint32_t a = streams[s].va + i * streams[s].stride + off;
    if (!rotk_is_committed(a) || !rotk_is_committed(a + 7)) return false;
    float x = F(rotk_rd32(a)), y = F(rotk_rd32(a + 4));
    if (!(nx > 0 && x == xs[0]) && !(nx > 1 && x == xs[1])) {
      if (nx == 2) return false;
      xs[nx++] = x;
    }
    if (!(ny > 0 && y == ys[0]) && !(ny > 1 && y == ys[1])) {
      if (ny == 2) return false;
      ys[ny++] = y;
    }
  }
  return nx == 2 && ny == 2;
}

// One draw. `streams` gives the vertex data per Xbox stream; vertices
// first..first+count-1 are read (indexed: the index values, already rebased
// by the caller to start at 0 = vertex `first`).
// Immediate-mode draws (End) feed the current program from their own vertex layout.
VShader* g_vs_input_override = nullptr;
// Draws dropped since the last stats line: no shader, program failed, no target, stream not in
// memory, fragment program failed.
uint64_t g_drop[5] = {};

// FVF -> fixed-function shader with the FVF's stream-0 layout (Xbox D3DFVF bits).
std::unordered_map<uint32_t, VShader> g_fvf_shaders;
VShader& FvfShader(uint32_t fvf) {
  auto it = g_fvf_shaders.find(fvf);
  if (it != g_fvf_shaders.end()) return it->second;
  VShader& vs = g_fvf_shaders[fvf];
  vs.ff = true;
  vs.fvf = fvf;
  vs.index = 0x10000 | fvf;
  for (auto& a : vs.attr) a[0] = 0xFF, a[1] = 0, a[2] = VSDT_NONE;
  uint32_t off = 0;
  auto put = [&](int reg, uint32_t type, uint32_t bytes) {
    vs.attr[reg][0] = 0, vs.attr[reg][1] = off, vs.attr[reg][2] = type;
    off += bytes;
  };
  switch (fvf & 0xE) {
    case 0x2: put(0, VSDT_FLOAT3, 12); break;  // XYZ
    case 0x4: put(0, VSDT_FLOAT4, 16); break;  // XYZRHW
    case 0x6: case 0x8: case 0xA: case 0xC: {  // XYZB1..4: blend weights (v1)
      put(0, VSDT_FLOAT3, 12);
      uint32_t nb = ((fvf & 0xE) - 4) / 2;
      put(1, nb == 1 ? VSDT_FLOAT1 : nb == 2 ? VSDT_FLOAT2 : nb == 3 ? VSDT_FLOAT3 : VSDT_FLOAT4, 4 * nb);
      break;
    }
  }
  if (fvf & 0x010) put(2, VSDT_FLOAT3, 12);    // NORMAL
  if (fvf & 0x020) put(6, VSDT_FLOAT1, 4);     // PSIZE
  if (fvf & 0x040) put(3, VSDT_D3DCOLOR, 4);   // DIFFUSE
  if (fvf & 0x080) put(4, VSDT_D3DCOLOR, 4);   // SPECULAR
  uint32_t ntex = (fvf >> 8) & 0xF;
  for (uint32_t t = 0; t < ntex && t < 4; t++) {
    uint32_t code = (fvf >> (16 + 2 * t)) & 3;
    static const uint32_t kType[4] = {VSDT_FLOAT2, VSDT_FLOAT3, VSDT_FLOAT4, VSDT_FLOAT1};
    static const uint32_t kBytes[4] = {8, 12, 16, 4};
    put(9 + int(t), kType[code], kBytes[code]);
  }
  rotk_log("d3d8: fixed-function shader for FVF %08X (stride %u)", fvf, off);
  return vs;
}

// NV2A transform program memory: 136 instruction slots of 4 dwords. LoadVertexShader copies a
// handle's program to a slot, SelectVertexShader runs from a slot with a handle's declaration
// (the game packs several skinning programs and selects them per draw).
constexpr uint32_t kVsSlots = 136;
uint32_t g_vs_slots[kVsSlots * 4];
// Programs composed from the slots (declaration of one handle + the words from a start slot),
// by content: index of the handle, then the words.
std::map<std::vector<uint32_t>, std::unique_ptr<VShader>> g_vs_selected;
VShader* g_vs_sel_last = nullptr;  // cache of the composition for the current selection
bool g_vs_sel_dirty = true;

VShader* SelectedVs(const VShader& decl) {
  if (!g_vs_sel_dirty && g_vs_sel_last) return g_vs_sel_last;
  uint32_t start = uint32_t(g_dev.vs_start), end = start;
  while (end < kVsSlots && !(g_vs_slots[end * 4 + 3] & 1)) end++;
  if (end == kVsSlots) {
    static bool once = false;
    if (!once) once = true, rotk_log("d3d8: SelectVertexShader slot %u: no FINAL instruction", start);
    return nullptr;
  }
  // The handle's own program at the start slot: its compiled program serves.
  size_t n = size_t(end - start + 1) * 4;
  if (decl.func.size() == n && !std::memcmp(decl.func.data(), &g_vs_slots[start * 4], n * 4)) {
    g_vs_sel_dirty = false;
    return g_vs_sel_last = const_cast<VShader*>(&decl);
  }
  std::vector<uint32_t> key(1 + n);
  key[0] = decl.index;
  std::memcpy(&key[1], &g_vs_slots[start * 4], n * 4);
  auto& slot = g_vs_selected[key];
  if (!slot) {
    slot = std::make_unique<VShader>(decl);
    slot->func.assign(key.begin() + 1, key.end());
    slot->prog = 0, slot->failed = false, slot->job.reset();
    slot->index = 0x20000 | uint32_t(g_vs_selected.size());
    rotk_log("d3d8: vertex program from slot %u (%zu instructions) with the declaration of vs %u -> vs %u",
             start, n / 4, decl.index, slot->index);
  }
  g_vs_sel_dirty = false;
  return g_vs_sel_last = slot.get();
}

// The shader the next draw uses: a created one (handle | 1) or an FVF's; null if invalid.
VShader* CurrentVs() {
  uint32_t h = g_dev.vs_handle;
  if (!(h & 1)) return h ? &FvfShader(h) : nullptr;
  uint32_t i = rotk_rd32((h & ~1u) + 4);
  if (rotk_rd32(h & ~1u) != kVsMagic || i >= g_vshaders.size()) return nullptr;
  if (g_dev.vs_start >= 0) return SelectedVs(g_vshaders[i]);
  return &g_vshaders[i];
}

// Row-major 4x4 product (D3D row vectors: v * a * b).
void MatMul(const float* a, const float* b, float* out) {
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) {
      float s = 0;
      for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
      out[r * 4 + c] = s;
    }
}

float ColorChan(uint32_t argb, int shift) { return float((argb >> shift) & 0xFF) / 255.0f; }

// Parameters of the fixed-function program (vsh.cpp TranslateFixedFunctionVs).
void FillFixedFunction(VsUniforms& vu, const VShader& vs) {
  constexpr int kView = 0, kProj = 1, kWorld = 6;
  float wv[16], wvp[16];
  MatMul(g_dev.transforms[kWorld], g_dev.transforms[kView], wv);
  MatMul(wv, g_dev.transforms[kProj], wvp);
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++) {
      vu.c[c][r] = wvp[r * 4 + c];
      vu.c[4 + c][r] = wv[r * 4 + c];
    }
  for (int c = 0; c < 3; c++) {
    for (int r = 0; r < 3; r++) vu.c[8 + c][r] = g_dev.transforms[kWorld][r * 4 + c];
    vu.c[8 + c][3] = 0;
  }
  bool rhw = (vs.attr[0][2] == VSDT_FLOAT4 && vs.fvf && (vs.fvf & 0xE) == 0x4);
  vu.c[12][0] = rhw ? 1.f : 0.f;
  vu.c[12][1] = vs.attr[3][0] != 0xFF ? 1.f : 0.f;
  vu.c[12][2] = RS(RS_LIGHTING) && !rhw ? 1.f : 0.f;
  vu.c[12][3] = vs.attr[2][0] != 0xFF ? 1.f : 0.f;
  for (int t = 0; t < 4; t++) vu.c[13][t] = float(TS(uint32_t(t), TSS_TEXCOORDINDEX) & 0xFFFF);
  vu.c[14][0] = vs.attr[4][0] != 0xFF ? 1.f : 0.f;
  vu.c[14][1] = RS(RS_FOGENABLE) ? 1.f : 0.f;
  vu.c[14][2] = float(RS(RS_DIFFUSEMATERIALSOURCE));
  vu.c[14][3] = float(RS(RS_AMBIENTMATERIALSOURCE));
  // D3DMATERIAL8: Diffuse, Ambient, Specular, Emissive (float4 each), Power.
  float m[17];
  std::memcpy(m, g_dev.material, sizeof m);
  std::memcpy(vu.c[15], &m[0], 16);
  std::memcpy(vu.c[16], &m[4], 16);
  std::memcpy(vu.c[17], &m[12], 16);
  uint32_t amb = RS(RS_AMBIENT);
  vu.c[18][0] = ColorChan(amb, 16), vu.c[18][1] = ColorChan(amb, 8), vu.c[18][2] = ColorChan(amb, 0);
  vu.c[18][3] = 1;
  // Lights: this game links neither SetLight nor LightEnable nor SetMaterial (no D3D-section
  // function of that shape has a caller in .text; its 3D is lit by its own vertex programs),
  // so no light can be on: D3D lighting is emissive + ambient, as on the Xbox.
  vu.c[19][0] = 0;
  vu.c[19][1] = float(RS(RS_EMISSIVEMATERIALSOURCE));
  vu.c[19][2] = RS(RS_COLORVERTEX) ? 1.f : 0.f;
  if (RS(RS_LIGHTING) && !(vu.c[12][0] != 0)) {
    static bool once = false;
    if (!once)
      once = true, rotk_log("d3d8: first lit fixed-function draw (FVF %X, frame %llu): no lights, emissive + ambient",
                            vs.fvf, (unsigned long long)g_dev.frame);
  }
}

void DrawCommon(uint32_t prim, const StreamSrc* streams, uint32_t first, uint32_t count,
                const uint16_t* indices, uint32_t nidx) {
  g_dev.draws++;
  g_dev.frame_draws++;
  VShader* cur = CurrentVs();
  if (!cur) return (void)g_drop[0]++;
  VShader& vs = g_vs_input_override ? *g_vs_input_override : *cur;
  gpu::Draw d;
  d.vs = VertexProgram(vs);
  if (!d.vs) return (void)g_drop[1]++;

  TargetsNow tg = BindTargets();
  if (!tg.w || !tg.h) return (void)g_drop[2]++;

  // Vertex input.
  VsUniforms vu;
  std::memcpy(vu.c, g_dev.vs_c, sizeof vu.c);
  if (vs.ff) FillFixedFunction(vu, vs);
  // Screen-space transform (c-38 scale, c-37 offset), as D3D sets it for the
  // full-target viewport; the shader epilogue inverts it.
  float zscale = 16777215.0f;
  if (g_dev.z) {
    uint32_t zw, zh, zf;
    if (SurfaceSize(g_dev.z, &zw, &zh, &zf) && (zf == XF_D16 || zf == XF_LIN_D16)) zscale = 65535.0f;
  } else {
    zscale = 1.0f;
  }
  float sc[4] = {tg.w * 0.5f, -(tg.h * 0.5f), zscale, 1.0f};
  float of[4] = {tg.w * 0.5f, tg.h * 0.5f, 0.0f, 0.0f};
  if (g_dev.vp_w) {  // SetViewport: scale/offset as the XDK computes them
    sc[0] = g_dev.vp_w * 0.5f, sc[1] = -(g_dev.vp_h * 0.5f);
    sc[2] = zscale * (g_dev.vp_maxz - g_dev.vp_minz);
    of[0] = g_dev.vp_x + g_dev.vp_w * 0.5f, of[1] = g_dev.vp_y + g_dev.vp_h * 0.5f;
    of[2] = zscale * g_dev.vp_minz;
  }
  if (!(g_dev.const_mode & 0x10)) {
    std::memcpy(vu.c[96 - 38], sc, 16);
    std::memcpy(vu.c[96 - 37], of, 16);
  }
  std::memcpy(vu.scale, vu.c[96 - 38], 16);
  std::memcpy(vu.offset, vu.c[96 - 37], 16);
  for (int i = 0; i < 4; i++)
    if (vu.scale[i] == 0) vu.scale[i] = 1;
  // D3D puts pixel centres on integer coordinates, Metal on +0.5.
  vu.ndc_shift[0] = -2.0f * g_cfg.pixel_offset * g_cfg.scale / tg.pw;
  vu.ndc_shift[1] = 2.0f * g_cfg.pixel_offset * g_cfg.scale / tg.ph;
  // Widescreen: z = 1/wide on wide targets (the shader epilogue scales x).
  vu.ndc_shift[2] = tg.pw * tg.h != tg.ph * tg.w ? float(tg.ph * tg.w) / float(tg.pw * tg.h) : 0.0f;
  vu.ndc_shift[3] = 0;
  vu.snap[0] = vu.snap[1] = vu.snap[2] = vu.snap[3] = 0;
  if (vu.ndc_shift[2] != 0) vu.snap[1] = float(WideEdgeStretch(vs, streams, first, count, vu));
  if (vu.ndc_shift[2] != 0 && g_cfg.wide_hud && vs.screen_space)
    vu.ndc_shift[3] = HudAnchor(vs, streams, first, count, vu.ndc_shift[2]);
  // Streams used by the declaration -> gpu streams 0..3.
  int map[16];
  std::fill(map, map + 16, -1);
  int nstreams = 0;
  for (int r = 0; r < 16; r++) {
    uint32_t s = vs.attr[r][0];
    if (s == 0xFF || s >= 16) {
      vu.attr[r][0] = 0xFF;
      continue;
    }
    if (map[s] < 0) {
      if (nstreams == 4) {
        vu.attr[r][0] = 0xFF;
        continue;
      }
      map[s] = nstreams++;
      const StreamSrc& ss = streams[s];
      if (ss.va) {
        uint32_t stride = ss.stride;
        uint32_t bytes = stride ? (first + count) * stride : 64;
        uint32_t start = stride ? first * stride : 0;
        bytes -= start;
        if (rotk_is_committed(ss.va + start) && rotk_is_committed(ss.va + start + bytes - 1)) {
          d.streams[map[s]].data = rotk_ptr(ss.va + start);
          d.streams[map[s]].size = bytes;
        } else {
          static std::set<uint32_t> warned;
          if (warned.insert(ss.va).second)
            rotk_log("d3d8: stream %d data %08X+%u (%u bytes) not in guest memory: draw skipped", s,
                    ss.va, start, bytes);
          g_drop[3]++;
          return;
        }
      }
    }
    vu.attr[r][0] = uint32_t(map[s]);
    vu.attr[r][1] = vs.attr[r][1];
    vu.attr[r][2] = vs.attr[r][2];
    vu.attr[r][3] = streams[s].stride;
    if (!streams[s].va) vu.attr[r][0] = 0xFF;
  }
  d.vs_uniforms = &vu;
  d.vs_uniforms_size = sizeof vu;

  // Topology.
  std::vector<uint16_t> tidx;
  bool need_idx;
  uint32_t mtl;
  if (indices) {
    if (!Topology(prim, nidx, &mtl, &tidx, &need_idx)) return;
    if (need_idx) {
      for (auto& v : tidx) v = indices[v];
    } else {
      tidx.assign(indices, indices + nidx);
    }
    d.indices = tidx.data();
    d.index_count = uint32_t(tidx.size());
  } else {
    if (!Topology(prim, count, &mtl, &tidx, &need_idx)) return;
    if (need_idx) {
      d.indices = tidx.data();
      d.index_count = uint32_t(tidx.size());
    } else {
      d.vertex_count = count;
    }
  }
  d.mtl_prim = mtl;

  // Textures.
  FsUniforms fu{};
  PsKey key;
  Bound bt[4];
  for (int s = 0; s < 4; s++) {
    bt[s] = BindTexture(g_dev.tex[s], s);
    key.tex_kind[s] = bt[s].kind;
    d.tex[s] = bt[s].tex;
    gpu::SamplerState& sm = d.samp[s];
    sm.addr_u = MtlAddr(TS(s, TSS_ADDRESSU));
    sm.addr_v = MtlAddr(TS(s, TSS_ADDRESSV));
    sm.addr_w = MtlAddr(TS(s, TSS_ADDRESSW));
    uint32_t mag = TS(s, TSS_MAGFILTER), min = TS(s, TSS_MINFILTER), mip = TS(s, TSS_MIPFILTER);
    sm.mag = mag >= 2 ? 1 : 0;
    sm.min = min >= 2 ? 1 : 0;
    sm.mip = bt[s].levels > 1 ? uint8_t(std::min(mip, 2u)) : 0;
    sm.max_aniso = (mag == 3 || min == 3) ? uint8_t(std::clamp(TS(s, TSS_MAXANISOTROPY), 1u, 16u)) : 1;
    // Quality option: mipmapped linear textures get at least ROTK_ANISO.
    if (sm.mip && sm.min == 1 && g_cfg.aniso > sm.max_aniso) sm.max_aniso = uint8_t(g_cfg.aniso);
    sm.border = TS(s, TSS_BORDERCOLOR);
    if (vs.screen_space && bt[s].tex && (sm.addr_u == 2 || sm.addr_v == 2 || (s == 0 && g_cfg.scale > 1))) {
      bool u_in = false, v_in = false;
      UvRangeInUnit(vs, streams, first, count, s, bt[s].sx, bt[s].sy, &u_in, &v_in);
      if (u_in && sm.addr_u == 2) sm.addr_u = 0;
      if (v_in && sm.addr_v == 2) sm.addr_v = 0;
      if (s == 0 && g_cfg.scale > 1 && u_in && v_in && !indices && AxisAlignedQuad(vs, streams, first, count))
        vu.snap[0] = 1;
    }
    fu.texscale[s][0] = bt[s].sx;
    fu.texscale[s][1] = bt[s].sy;
    fu.texscale[s][2] = bt[s].sx;
    fu.texscale[s][3] = bt[s].sy;
    fu.bump[s][0] = F(TS(s, TSS_BUMPENVMAT00));
    fu.bump[s][1] = F(TS(s, TSS_BUMPENVMAT01));
    fu.bump[s][2] = F(TS(s, TSS_BUMPENVMAT10));
    fu.bump[s][3] = F(TS(s, TSS_BUMPENVMAT11));
    fu.bumpl[s][0] = F(TS(s, TSS_BUMPENVLSCALE));
    fu.bumpl[s][1] = F(TS(s, TSS_BUMPENVLOFFSET));
    fu.akill[s] = TS(s, TSS_ALPHAKILL) != 0;
  }
  static const bool dev_nofog = getenv("ROTK_DEV_NOFOG") != nullptr;
  bool fog = RS(RS_FOGENABLE) != 0 && !dev_nofog;
  bool two = RS(RS_TWOSIDEDLIGHTING) != 0;
  if (g_dev.ps) {
    key.combiners = true;
    for (uint32_t i = 0; i <= RS_PSINPUTTEXTURE; i++) key.words[i] = RS(i);
    key.words[57] = RS(RS_PSTEXTUREMODES);
    key.words[58] = fog;
    key.words[59] = two;
    key.nwords = 60;
    // Texture modes that do not sample leave the texture kind out of the key.
    for (int s = 0; s < 4; s++) {
      uint32_t m = (key.words[57] >> (5 * s)) & 0x1F;
      if (m == 0 || m == 4 || m == 5 || m == 0x11) key.tex_kind[s] = 0;
    }
  } else {
    for (uint32_t s = 0; s < 4; s++) {
      key.words[s * 8 + 0] = TS(s, TSS_COLOROP);
      key.words[s * 8 + 1] = TS(s, TSS_COLORARG0);
      key.words[s * 8 + 2] = TS(s, TSS_COLORARG1);
      key.words[s * 8 + 3] = TS(s, TSS_COLORARG2);
      key.words[s * 8 + 4] = TS(s, TSS_ALPHAOP);
      key.words[s * 8 + 5] = TS(s, TSS_ALPHAARG0);
      key.words[s * 8 + 6] = TS(s, TSS_ALPHAARG1);
      key.words[s * 8 + 7] = TS(s, TSS_ALPHAARG2);
      if (TS(s, TSS_RESULTARG) == TA_TEMP) key.words[32] |= 1u << s;
    }
    // Stages after the first disabled one do not matter.
    bool off = false;
    for (uint32_t s = 0; s < 4; s++) {
      if (key.words[s * 8] == TOP_DISABLE || key.words[s * 8] == 0) off = true;
      if (off) {
        for (int i = 0; i < 8; i++) key.words[s * 8 + i] = 0;
        key.tex_kind[s] = 0;
      }
    }
    key.words[33] = fog;
    key.words[34] = RS(RS_SPECULARENABLE) != 0;
    key.words[35] = two;
    key.nwords = 36;
  }
  // D3DZB_USEW: the game's 3D draws use the W-buffer (linear eye w); z/w
  // in a float buffer loses the far range and coplanar decals fight.
  key.wdepth = g_cfg.wbuffer && RS(RS_ZENABLE) == 2 && tg.depth;
  d.fs = FragmentProgram(key);
  if (!d.fs) return (void)g_drop[4]++;
  // Dev: ROTK_DEV_SKIP_PS=<hash> drops draws with that fragment program (bisecting).
  static const uint64_t dev_skip = [] {
    const char* v = getenv("ROTK_DEV_SKIP_PS");
    return v ? strtoull(v, nullptr, 16) : 0ull;
  }();
  if (dev_skip && key.Hash() == dev_skip) return;
  // Dev: ROTK_DEV_SKIP_DRAWS=a-b drops draws a..b of every frame;
  // ROTK_DEV_LOG_DRAWS=<frame> logs every draw of that frame.
  static const std::pair<uint32_t, uint32_t> dev_range = [] {
    const char* v = getenv("ROTK_DEV_SKIP_DRAWS");
    uint32_t a = 0, b = 0;
    if (v) std::sscanf(v, "%u-%u", &a, &b);
    return std::make_pair(a, v ? b : 0u);
  }();
  static const uint64_t dev_log_frame = [] {
    const char* v = getenv("ROTK_DEV_LOG_DRAWS");
    return v ? strtoull(v, nullptr, 10) : ~0ull;
  }();
  uint32_t draw_no = g_dev.frame_draws - 1;
  if (g_dev.frame + 1 == dev_log_frame) {
    char tx[320] = "";
    size_t o = 0;
    for (int s2 = 0; s2 < 4; s2++) {
      uint32_t th = g_dev.tex[s2];
      if (!th) continue;
      TexLayout L;
      DescribeTexture(Hdr{th}.format(), Hdr{th}.size(), &L);
      o += std::snprintf(tx + o, sizeof tx - o, " t%d=%s %ux%u lv %u filt %u/%u/%u an %u ad %u/%u lb %X ml %u ak %u d%08X%s", s2,
                         FormatInfo(L.xfmt).name ? FormatInfo(L.xfmt).name : "?", L.w, L.h,
                         bt[s2].levels, d.samp[s2].mag, d.samp[s2].min, d.samp[s2].mip,
                         d.samp[s2].max_aniso, d.samp[s2].addr_u, d.samp[s2].addr_v, TS(s2, TSS_MIPMAPLODBIAS),
                         TS(s2, TSS_MAXMIPLEVEL), TS(s2, TSS_ALPHAKILL), Hdr{th}.data(), bt[s2].tex ? "" : "(none)");
    }
    char st[96] = "";
    if (RS(RS_STENCILENABLE))
      std::snprintf(st, sizeof st, " stencil %X ref %X mask %X/%X ops %X/%X/%X", RS(RS_STENCILFUNC),
                    RS(RS_STENCILREF), RS(RS_STENCILMASK), RS(RS_STENCILWRITEMASK),
                    RS(RS_STENCILFAIL), RS(RS_STENCILZFAIL), RS(RS_STENCILPASS));
    char sm[96] = "";
    for (int r = 0, o2 = 0; r < 16; r++)
      if (vs.attr[r][0] < 16 && o2 < 80)
        o2 += std::snprintf(sm + o2, sizeof sm - o2, " v%d:%u+%u/%X", r, vs.attr[r][0], vs.attr[r][1],
                            vs.attr[r][2]);
    rotk_log("draw %u: stride %u tci %X lit %u%s", draw_no, streams[0].stride, TS(0, TSS_TEXCOORDINDEX),
             vs.ff ? RS(RS_LIGHTING) : 0u, sm);
    {  // the first 4 positions (v0 as stored: 2D pixels, or model space)
      uint32_t s = vs.attr[0][0], off = vs.attr[0][1];
      char pos[160] = "";
      int o2 = 0;
      for (uint32_t v = first; s < 16 && streams[s].va && v < first + std::min(count, 4u); v++) {
        uint32_t a = streams[s].va + v * streams[s].stride + off;
        if (!rotk_is_committed(a) || !rotk_is_committed(a + 11)) break;
        o2 += std::snprintf(pos + o2, sizeof pos - size_t(o2), " (%g %g %g)", F(rotk_rd32(a)),
                            F(rotk_rd32(a + 4)), F(rotk_rd32(a + 8)));
      }
      rotk_log("draw %u: pos%s", draw_no, pos);
    }
    // ROTK_DEV_LOG_VSC=a-b (D3D numbering, -96..95): those vertex constants at every logged draw.
    static const std::pair<int, int> vsc = [] {
      int a = 0, b = -1;
      if (const char* v = getenv("ROTK_DEV_LOG_VSC")) std::sscanf(v, "%d-%d", &a, &b);
      return std::make_pair(a, b);
    }();
    for (int r = vsc.first; r <= vsc.second; r++)
      if (r + 96 >= 0 && r + 96 < 192)
        rotk_log("draw %u:   c%d = %g %g %g %g", draw_no, r, vu.c[r + 96][0], vu.c[r + 96][1],
                 vu.c[r + 96][2], vu.c[r + 96][3]);
    rotk_log("draw %u: prim %u n %u vs %u ps %016llx%s blend %u %X/%X z %u/%u %X cw %X cull %X at %u %X/%02X%s%s",
            draw_no, prim, indices ? nidx : count, vs.index, (unsigned long long)key.Hash(),
            g_dev.ps ? "(comb)" : "(ff)", RS(RS_ALPHABLENDENABLE), RS(RS_SRCBLEND),
            RS(RS_DESTBLEND), RS(RS_ZENABLE), RS(RS_ZWRITEENABLE), RS(RS_ZFUNC),
            RS(RS_COLORWRITEENABLE), RS(RS_CULLMODE), RS(RS_ALPHATESTENABLE), RS(RS_ALPHAFUNC),
            RS(RS_ALPHAREF) & 0xFF, st, tx);
  }
  if (draw_no >= dev_range.first && draw_no <= dev_range.second && dev_range.second) return;
  // Dev: ROTK_DEV_MAGENTA=a-b draws a..b as opaque magenta (no blend).
  static const std::pair<uint32_t, uint32_t> dev_mag = [] {
    const char* v = getenv("ROTK_DEV_MAGENTA");
    uint32_t a = 0, b = 0;
    if (v) std::sscanf(v, "%u-%u", &a, &b);
    return std::make_pair(a, v ? b : 0u);
  }();
  bool magenta = dev_mag.second && draw_no >= dev_mag.first && draw_no <= dev_mag.second;
  if (magenta) {
    // ROTK_DEV_SHOW=fog|d0|t0uv|t0: show that input instead of magenta (t0: stage 0's
    // texture unlit, texels with alpha < 0.5 discarded; the HD pack's coverage probe).
    static gpu::ProgId mp = [] {
      const char* v = getenv("ROTK_DEV_SHOW");
      if (v && !strcmp(v, "t0"))
        return gpu::CompileFragment(std::string(kMslCommon) +
                                        "fragment float4 fmain(VOut in [[stage_in]], texture2d<float> tx0 "
                                        "[[texture(0)]], sampler sm0 [[sampler(0)]]) {\n"
                                        "  float4 c = tx0.sample(sm0, in.t0.xy);\n"
                                        "  if (c.a < 0.5) discard_fragment();\n"
                                        "  return float4(c.rgb, 1); }\n",
                                    "devshow");
      std::string e = "float4(1, 0, 1, 1)";
      if (v && !strcmp(v, "fog")) e = "float4(saturate(in.fog), saturate(-in.fog), saturate(in.fog / 100), 1)";
      if (v && !strcmp(v, "d0")) e = "float4(in.d0.rgb, 1)";
      if (v && !strcmp(v, "d0a")) e = "float4(in.d0.aaa, 1)";
      if (v && !strcmp(v, "t0uv")) e = "float4(fract(in.t0.xy), 0, 1)";
      return gpu::CompileFragment(std::string(kMslCommon) +
                                      "fragment float4 fmain(VOut in [[stage_in]]) { return " + e +
                                      "; }\n",
                                  "devshow");
    }();
    d.fs = mp;
  }
  for (int i = 0; i < 8; i++) {
    ColorF(RS(RS_PSCONSTANT0_0 + i), fu.c0[i]);
    ColorF(RS(RS_PSCONSTANT1_0 + i), fu.c1[i]);
  }
  ColorF(RS(RS_PSFINALCOMBINERCONSTANT0), fu.fc0);
  ColorF(RS(RS_PSFINALCOMBINERCONSTANT1), fu.fc1);
  ColorF(RS(RS_TEXTUREFACTOR), fu.tfactor);
  ColorF(RS(RS_FOGCOLOR), fu.fogcolor);
  if (fog && g_cfg.trace) {
    static std::set<uint64_t> seen;
    uint64_t k = uint64_t(RS(RS_FOGTABLEMODE)) << 48 ^ uint64_t(RS(RS_FOGSTART)) << 16 ^
                 RS(RS_FOGEND) ^ uint64_t(RS(RS_FOGCOLOR)) << 24;
    if (seen.size() < 40 && seen.insert(k).second)
      rotk_log("d3d8: fog: mode %u start %g end %g density %g color %08X range %u",
              RS(RS_FOGTABLEMODE), F(RS(RS_FOGSTART)), F(RS(RS_FOGEND)), F(RS(RS_FOGDENSITY)),
              RS(RS_FOGCOLOR), RS(RS_RANGEFOGENABLE));
  }
  fu.fogp[0] = float(RS(RS_FOGTABLEMODE));
  fu.fogp[1] = F(RS(RS_FOGSTART));
  fu.fogp[2] = F(RS(RS_FOGEND));
  fu.fogp[3] = F(RS(RS_FOGDENSITY));
  fu.alpha[0] = RS(RS_ALPHATESTENABLE) ? 1.f : 0.f;
  fu.alpha[1] = float(MtlCmp(RS(RS_ALPHAFUNC)));
  fu.alpha[2] = (RS(RS_ALPHAREF) & 0xFF) / 255.f;
  fu.flags[0] = fog;
  fu.flags[1] = RS(RS_SPECULARENABLE) != 0;
  fu.flags[2] = two;
  d.fs_uniforms = &fu;
  d.fs_uniforms_size = sizeof fu;

  // Output merger.
  gpu::BlendState& bs = d.blend;
  bs.enable = RS(RS_ALPHABLENDENABLE) != 0 && !(magenta && !getenv("ROTK_DEV_MAGENTA_BLEND"));
  bs.src = bs.src_a = MtlBlend(RS(RS_SRCBLEND));
  bs.dst = bs.dst_a = MtlBlend(RS(RS_DESTBLEND));
  bs.op = bs.op_a = MtlBlendOp(RS(RS_BLENDOP));
  uint32_t cw = RS(RS_COLORWRITEENABLE);
  bs.write_mask = uint8_t(((cw >> 16) & 1) << 3 | ((cw >> 8) & 1) << 2 | (cw & 1) << 1 |
                          ((cw >> 24) & 1));
  ColorF(RS(RS_BLENDCOLOR), d.blend_color);
  gpu::DepthState& ds = d.depth;
  ds.test = RS(RS_ZENABLE) != 0 && tg.depth;
  ds.write = RS(RS_ZWRITEENABLE) != 0;
  ds.func = MtlCmp(RS(RS_ZFUNC));
  ds.stencil = RS(RS_STENCILENABLE) != 0 && tg.depth;
  ds.s_func = MtlCmp(RS(RS_STENCILFUNC));
  ds.s_fail = MtlStencilOp(RS(RS_STENCILFAIL));
  ds.s_zfail = MtlStencilOp(RS(RS_STENCILZFAIL));
  ds.s_pass = MtlStencilOp(RS(RS_STENCILPASS));
  ds.s_read = uint8_t(RS(RS_STENCILMASK));
  ds.s_write = uint8_t(RS(RS_STENCILWRITEMASK));
  d.stencil_ref = RS(RS_STENCILREF) & 0xFF;
  uint32_t cull = RS(RS_CULLMODE), front = RS(RS_FRONTFACE);
  d.front_ccw = front == CULL_CCW;
  d.cull = cull == CULL_NONE ? 0 : (cull != front ? 2 /* back */ : 1 /* front */);
  d.wireframe = RS(RS_FILLMODE) == FILL_WIREFRAME;
  // Polygon offset per fill mode (D3D8 on Xbox: POINT/WIREFRAME/SOLIDOFFSETENABLE;
  // D3DRS_ZBIAS goes through these too, via SetRenderState_ZBias, which this
  // game neither links nor inlines). Units: depth LSBs, like Metal's for z
  // near 1. Not applied to W-buffer depth (written by the shader); the game
  // sets no offsets at all.
  uint32_t fill = RS(RS_FILLMODE);
  uint32_t offset_on = fill == FILL_WIREFRAME ? RS(RS_WIREFRAMEOFFSETENABLE)
                       : fill == FILL_POINT   ? RS(RS_POINTOFFSETENABLE)
                                              : RS(RS_SOLIDOFFSETENABLE);
  if (offset_on) {
    static bool once = false;
    if (!once)
      rotk_log("d3d8: first polygon offset: %f + slope %f%s", F(RS(RS_POLYGONOFFSETZOFFSET)),
              F(RS(RS_POLYGONOFFSETZSLOPESCALE)), key.wdepth ? " (W-buffer draw: ignored)" : ""),
          once = true;
    d.depth_bias = F(RS(RS_POLYGONOFFSETZOFFSET));
    d.slope_bias = F(RS(RS_POLYGONOFFSETZSLOPESCALE));
  }
  float sw = float(tg.pw), sh = float(tg.ph);
  d.vp_x = 0, d.vp_y = 0, d.vp_w = sw, d.vp_h = sh, d.vp_minz = 0, d.vp_maxz = 1;
  d.sc_x = 0, d.sc_y = 0, d.sc_w = int32_t(sw), d.sc_h = int32_t(sh);
  // Widescreen: 2D that is not stretched to the wide edges or HUD-anchored is clipped to the
  // centred 4:3 screen, as the console's 640-wide screen clipped it (owner bug 2026-11-06: an
  // off-screen thumbnail of the end-of-mission screen showed in the right strip).
  if (vu.ndc_shift[2] != 0 && vs.screen_space && vu.snap[1] == 0 && vu.ndc_shift[3] == 0) {
    float w43 = sw * vu.ndc_shift[2];
    d.sc_x = int32_t((sw - w43) * 0.5f + 0.5f), d.sc_w = int32_t(w43 + 0.5f);
    if (Outside43(vs, streams, first, count)) {
      static uint64_t logged = ~0ull;
      if (logged != g_dev.frame && g_clip_logs < 20)
        rotk_log("d3d8: widescreen: 2D draw outside the 4:3 screen clipped (frame %llu)",
                 (unsigned long long)g_dev.frame), logged = g_dev.frame, ++g_clip_logs;
      return;
    }
  }
  gpu::DoDraw(d);
  // Dev: ROTK_DEV_PROBE=frame,x,y logs every draw of that frame that changes
  // back-buffer pixel (x, y) (reads the target back after each draw: slow).
  static const std::array<int64_t, 3> probe = [] {
    std::array<int64_t, 3> p{-1, 0, 0};
    if (const char* v = getenv("ROTK_DEV_PROBE"))
      std::sscanf(v, "%lld,%lld,%lld", (long long*)&p[0], (long long*)&p[1], (long long*)&p[2]);
    return p;
  }();
  if (probe[0] >= 0 && int64_t(g_dev.frame + 1) == probe[0] && tg.color) {
    static uint32_t last = 0;
    std::vector<uint8_t> px;
    if (gpu::ReadPixels(tg.color->tex, tg.pw, tg.ph, &px)) {
      size_t i = (size_t(probe[2]) * tg.pw + size_t(probe[1])) * 4;
      uint32_t v = i + 3 < px.size() ? (px[i] << 16 | px[i + 1] << 8 | px[i + 2]) : 0;
      if (v != last)
        rotk_log("probe: draw %u -> %06X (target %08X %ux%u)", g_dev.frame_draws - 1, v,
                tg.color->data, tg.w, tg.h);
      last = v;
    }
  }
}

void DrawFromStreams(uint32_t prim, uint32_t first, uint32_t count, const uint16_t* idx,
                     uint32_t nidx) {
  StreamSrc ss[16];
  for (int s = 0; s < 16; s++) {
    uint32_t vb = g_dev.streams[s].vb;
    if (!vb) continue;
    ss[s].va = CpuAddr(Hdr{vb}.data());
    ss[s].stride = g_dev.streams[s].stride;
  }
  DrawCommon(prim, ss, first, count, idx, nidx);
}

// Index data the draw reads: a CPU pointer, or a physical one (Data of a
// D3D-created buffer). Null (logged once) if neither is guest memory.
const uint16_t* IndexPtr(uint32_t va, uint32_t n) {
  uint32_t bytes = n * 2;
  for (uint32_t a : {va, CpuAddr(va)})
    if (a && rotk_is_committed(a) && rotk_is_committed(a + bytes - 1))
      return static_cast<const uint16_t*>(rotk_ptr(a));
  static bool once = false;
  if (!once) rotk_log("d3d8: index data %08X (%u indices) not in guest memory", va, n), once = true;
  return nullptr;
}

// Indexed draw: rebases the indices to the smallest one used.
void DrawIndexed(uint32_t prim, uint32_t count, const uint16_t* raw, const StreamSrc* ss,
                 uint32_t base) {
  if (!count) return;
  uint32_t lo = 0xFFFF, hi = 0;
  for (uint32_t i = 0; i < count; i++) lo = std::min<uint32_t>(lo, raw[i]), hi = std::max<uint32_t>(hi, raw[i]);
  std::vector<uint16_t> idx(raw, raw + count);
  for (auto& v : idx) v = uint16_t(v - lo);
  DrawCommon(prim, ss, base + lo, hi - lo + 1, idx.data(), count);
}

// ---- frame end ----------------------------------------------------------------------------
// Shows a colour surface in the window (fitted 4:3; no-op without one).
uint32_t g_last_presented = 0;  // data address of the last target shown

void Present(uint32_t surface) {
  if (!surface) return;
  if (Target* t = FindTarget(Hdr{surface}.data(), false)) {
    gpu::PresentFrame(t->tex, t->pw, t->ph);
    g_last_presented = t->data;
  }
}

std::vector<std::thread> g_shot_writers;

void JoinShotWriters() {
  for (auto& t : g_shot_writers) t.join();
  g_shot_writers.clear();
}

void Screenshot(uint32_t surface, const char* kind, uint64_t n) {
  if (g_shot_writers.size() > 8) JoinShotWriters();
  if (g_cfg.shot_dir.empty()) return;
  // The surface asked for, else the current render target, else the last one presented;
  // why a shot fell back or was skipped is logged once per reason.
  uint32_t data = surface ? Hdr{surface}.data() : 0;
  Target* t = FindTarget(data, false);
  if (!t) {
    Target* rt = g_dev.rt ? FindTarget(Hdr{g_dev.rt}.data(), false) : nullptr;
    t = rt ? rt : FindTarget(g_last_presented, false);
    static bool once = false;
    if (!once)
      rotk_log("d3d8: screenshot: surface %08X (data %08X) is not a render target; using %s %08X",
              surface, data, rt ? "the current render target" : "the last presented target",
              t ? t->data : 0), once = true;
  }
  if (!t) {
    static bool once = false;
    if (!once) rotk_log("d3d8: screenshot %s %llu skipped: no render target to read", kind,
                       (unsigned long long)n), once = true;
    return;
  }
  std::vector<uint8_t> px;
  uint32_t w = t->pw, h = t->ph;
  if (!gpu::ReadPixels(t->tex, w, h, &px)) {
    static bool once = false;
    if (!once) rotk_log("d3d8: screenshot %s %llu skipped: reading target %08X (%ux%u, tex %u) failed",
                       kind, (unsigned long long)n, t->data, w, h, t->tex), once = true;
    return;
  }
  char path[600];
  std::snprintf(path, sizeof path, "%s/%s_%06llu.png", g_cfg.shot_dir.c_str(), kind,
                (unsigned long long)n);
  // Encode on a worker so the Swap thread does not stall (joined at exit).
  g_shot_writers.emplace_back([px = std::move(px), p = std::string(path), w, h] {
    if (gpu::WritePng(p.c_str(), w, h, px.data())) rotk_log("d3d8: screenshot %s", p.c_str());
  });
}

// ROTK_DEV_PRESENT_TEST: what the window would show for this frame.
void ScreenshotPresent(uint64_t n) {
  uint32_t w, h;
  gpu::TexId t = gpu::LastPresentCopy(&w, &h);
  if (!t || g_cfg.shot_dir.empty()) return;
  std::vector<uint8_t> px;
  if (!gpu::ReadPixels(t, w, h, &px)) return;
  char path[600];
  std::snprintf(path, sizeof path, "%s/present_%06llu.png", g_cfg.shot_dir.c_str(),
                (unsigned long long)n);
  g_shot_writers.emplace_back([px = std::move(px), p = std::string(path), w, h] {
    if (gpu::WritePng(p.c_str(), w, h, px.data())) rotk_log("d3d8: screenshot %s", p.c_str());
  });
}

void FreePending(RotkCtx& c, bool all) {
  auto& v = g_dev.pending_free;
  for (size_t i = 0; i < v.size();) {
    if (all || v[i].second <= g_dev.frame) {
      FreeContiguous(c, v[i].first);
      v[i] = v.back();
      v.pop_back();
    } else {
      i++;
    }
  }
}

// ==== the replaced functions =============================================================
// Names and conventions: FUNCTIONS.md. stdcall unless noted.

// 0x133FA0 Direct3DCreate8(SDKVersion)
void Direct3DCreate8(RotkCtx& c) {
  Seen("Direct3DCreate8");
  // Dev switch until the DSound HLE exists (Audio role): back the APU MMIO
  // window with plain memory so the DSOUND lib's init does not fault.
  if (const char* v = getenv("ROTK_DEV_APU_RAM"); v && atoi(v)) {
    rotk_commit(0xFE800000, 0x80000);  // APU
    rotk_commit(0xFEC00000, 0x10000);  // AC97
    rotk_wr32(0xFEC00130, 0x100);       // AC97 global status: primary codec ready
    rotk_log("d3d8: ROTK_DEV_APU_RAM: APU registers 0xFE800000 backed by RAM (dev only)");
  }
  rotk_ret_stdcall(c, 1, 1);
}

// 0x12F880 D3D::SetPushBufferSize(PushBufferSize, KickOffSize)
void SetPushBufferSize(RotkCtx& c) {
  Seen("SetPushBufferSize");
  rotk_ret_stdcall(c, 0, 2);
}

// 0x12F8A0 Direct3D_CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags,
//                                pPresentationParameters, ppReturnedDeviceInterface)
void CreateDevice(RotkCtx& c) {
  Seen("CreateDevice");
  uint32_t pp = rotk_arg(c, 4), out = rotk_arg(c, 5);
  LoadConfig();
  if (!gpu::Init()) rotk_fatal("d3d8: Metal is not available");
  uint32_t w = rotk_rd32(pp + 0), h = rotk_rd32(pp + 4), fmt = rotk_rd32(pp + 8);
  uint32_t autodepth = rotk_rd32(pp + 0x20), zfmt = rotk_rd32(pp + 0x24);
  rotk_log("d3d8: CreateDevice %ux%u fmt %02X backbuffers %u ms %X autodepth %u zfmt %02X "
          "flags %X refresh %u interval %u; render scale %u, %s",
          w, h, fmt, rotk_rd32(pp + 0xC), rotk_rd32(pp + 0x10), autodepth, zfmt,
          rotk_rd32(pp + 0x28), rotk_rd32(pp + 0x2C), rotk_rd32(pp + 0x30), g_cfg.scale,
          gpu::DeviceName());
  if (!w) w = 640;
  if (!h) h = 480;
  g_dev.bb_w = w, g_dev.bb_h = h;
  // The CDevice the game gets back: the lib's own global (the game never
  // dereferences it; only g_pDevice-reading code would).
  rotk_wr32(kDevicePtrVa, kDeviceVa);
  InitPushSink();
  if (!g_dev.created) {
    g_dev.back = MakeSurface(c, w, h, fmt ? fmt : XF_LIN_A8R8G8B8);
    g_dev.front = MakeSurface(c, w, h, fmt ? fmt : XF_LIN_A8R8G8B8);
    if (autodepth) g_dev.zbuf = MakeSurface(c, w, h, zfmt ? zfmt : XF_D24S8);
    InitDefaultStates();
    g_dev.created = true;
  }
  g_dev.rt = g_dev.back;
  g_dev.z = g_dev.zbuf;
  if (!autodepth) SetRS(RS_ZENABLE, 0);
  SyncDeviceFields();
  g_dev.last_vblank = rotk_vblank_count();
  g_dev.last_swap = std::chrono::steady_clock::now();
  if (out) rotk_wr32(out, kDeviceVa);
  rotk_ret_stdcall(c, 0, 6);
}

// ---- push sink ---------------------------------------------------------------------------
// Lifted XDK state setters that are not replaced (and the game's inlined D3D macros) write
// NV2A methods at CDevice.m_pPut (+0) and call MakeSpace when it reaches m_pThreshold (+4).
// They also store the D3D__RenderState / TextureState cells the HLE reads, so the methods
// themselves carry nothing the HLE needs: they go to a guest scratch ring that MakeSpace
// rewinds. kPushSinkBytes leaves room for the largest single reservation (BeginStateBig).
constexpr uint32_t kPushSinkBytes = 0x40000, kPushSinkMargin = 0x8000;
uint32_t g_push_sink = 0;
uint64_t g_push_rewinds = 0;

void InitPushSink() {
  if (!g_push_sink) g_push_sink = rotk_hle_alloc(kPushSinkBytes, 0x1000);
  rotk_wr32(kDeviceVa + 0, g_push_sink);
  rotk_wr32(kDeviceVa + 4, g_push_sink + kPushSinkBytes - kPushSinkMargin);
}

// ---- CDevice fields the lifted XDK code dereferences (XDK 5558 offsets) -------------------
// Lifted setters that are not replaced read the current targets and follow the device's
// hardware pointers. The HLE keeps them valid: surfaces are the HLE's own headers (XDK
// layout), and every hardware pointer leads to zeroed host-backed memory, so register
// writes land nowhere and "wait while busy" loops see idle at once.
constexpr uint32_t kDevRenderTarget = 0x19F4, kDevZStencil = 0x19F8;
constexpr uint32_t kDevBackBuffer = 0x1A04, kDevFrontBuffer = 0x1A08, kDevAutoDepth = 0x1A10;
constexpr uint32_t kDevGpuRegs = 0x1C18;   // NV2A register window base (+0x100410 PFB, +0x6813C8 RAMDAC...)
constexpr uint32_t kDevDmaCtrl = 0x1C10;   // DMA control block (+0x40 put, +0x44 get)
constexpr uint32_t kDevGetPtr = 0x30;      // -> GPU read pointer
constexpr uint32_t kDevFenceRing = 0x44;   // -> fence ring (8 bytes per entry, mask at +0x38)
constexpr uint32_t kDevStateBlock = 0x790; // -> block whose byte +4 holds state flags
constexpr uint32_t kGpuRegsBytes = 0x800000;
uint32_t g_hw_sink = 0;

void SyncDeviceFields() {
  if (!g_hw_sink) {
    g_hw_sink = rotk_hle_alloc(kGpuRegsBytes, 0x1000);
    uint32_t small = rotk_hle_alloc(0x2000, 0x1000);
    rotk_wr32(kDeviceVa + kDevGpuRegs, g_hw_sink);
    rotk_wr32(kDeviceVa + kDevDmaCtrl, small);
    rotk_wr32(kDeviceVa + kDevGetPtr, small + 0x100);
    rotk_wr32(kDeviceVa + kDevFenceRing, small + 0x400);
    rotk_wr32(kDeviceVa + 0x38, 0x3F);
    rotk_wr32(kDeviceVa + kDevStateBlock, small + 0x1000);
  }
  rotk_wr32(kDeviceVa + kDevRenderTarget, g_dev.rt);
  rotk_wr32(kDeviceVa + kDevZStencil, g_dev.z);
  rotk_wr32(kDeviceVa + kDevBackBuffer, g_dev.back);
  rotk_wr32(kDeviceVa + kDevFrontBuffer, g_dev.front);
  rotk_wr32(kDeviceVa + kDevAutoDepth, g_dev.zbuf);
}

void ImmDrainBeforeRewind();

uint32_t RewindPushSink() {
  ImmDrainBeforeRewind();
  if (!g_push_rewinds++) rotk_log("d3d8: push sink in use (0x%08x, %u KB)", g_push_sink, kPushSinkBytes >> 10);
  rotk_wr32(kDeviceVa + 0, g_push_sink);
  return g_push_sink;
}

// 0x130210 D3D MakeSpace(): the default reservation; returns the new m_pPut (plain ret).
void MakeSpace(RotkCtx& c) {
  Seen("MakeSpace");
  rotk_ret_cdecl(c, RewindPushSink());
}

// A push or reservation larger than the sink (Pat01 BeginPush: 269484 bytes) gets its own
// buffer, grown on demand: the sink is followed by other HLE allocations (the device's surface
// headers), so writing past it corrupted the back buffer header and Swap presented nothing new.
uint32_t g_big_push = 0, g_big_push_bytes = 0;

uint32_t BigPushBuffer(uint32_t bytes) {
  if (bytes > g_big_push_bytes) {
    g_big_push_bytes = (bytes + 0xFFFFF) & ~0xFFFFFu;
    g_big_push = rotk_hle_alloc(g_big_push_bytes, 0x1000);
    rotk_log("d3d8: push of %u bytes: own buffer 0x%08x (%u KB)", bytes, g_big_push,
             g_big_push_bytes >> 10);
  }
  return g_big_push;
}

// 0x1300D0 D3D MakeRequestedSpace(MinimumSpace, RequestedSpace) (stdcall): BeginStateBig etc.
void MakeRequestedSpace(RotkCtx& c) {
  Seen("MakeRequestedSpace");
  uint32_t want = rotk_arg(c, 1);
  if (want > kPushSinkBytes - 0x100) {
    // Own buffer above the sink: m_pPut >= m_pThreshold there, so the next reservation rewinds.
    uint32_t put = BigPushBuffer(want);
    rotk_wr32(kDeviceVa + 0, put);
    rotk_ret_stdcall(c, put, 2);
    return;
  }
  if (want > kPushSinkMargin)
    rotk_log("d3d8: push reservation of %u bytes exceeds the sink margin", want);
  rotk_ret_stdcall(c, RewindPushSink(), 2);
}

// ---- vertical blank callback ---------------------------------------------------------------
// 0x12CE10 SetVerticalBlankCallback(pCallback): the XDK stores it at CDevice+0x1DA8 and its
// vblank interrupt calls pCallback(D3DVBLANKDATA* {VBlank, Swap, Flags}) in DPC context.
// ROTK's (0xBEDE0) copies VBlank to its frame counter 0x1E2F14 and its main loop spins on it.
// The pacer's vblank callback runs the guest function at once when the vblank happens on a
// guest thread (test clock: the thread that waits for it), else wakes a guest worker.
std::atomic<uint32_t> g_vbcb{0};
std::mutex g_vbcb_mu;
std::condition_variable g_vbcb_cv;
uint64_t g_vbcb_pending = 0, g_vbcb_done = 0;
uint32_t g_vbcb_data = 0;
std::condition_variable g_vbcb_ran;  // a callback finished (FrameCounter waits on it)

void RunVblankCallback(RotkCtx& c, uint64_t n) {
  uint32_t cb = g_vbcb.load();
  if (!cb) return;
  rotk_wr32(g_vbcb_data, uint32_t(n));
  rotk_wr32(g_vbcb_data + 4, uint32_t(g_dev.frame));
  rotk_wr32(g_vbcb_data + 8, 0);
  uint32_t arg = g_vbcb_data;
  rotk_call_guest(c, cb, &arg, 1);
}

void VblankWorker(RotkCtx& c, void*) {
  for (;;) {
    uint64_t n;
    {
      std::unique_lock<std::mutex> l(g_vbcb_mu);
      g_vbcb_cv.wait(l, [] { return g_vbcb_pending > g_vbcb_done; });
      n = g_vbcb_done = g_vbcb_pending;
    }
    RunVblankCallback(c, n);
    { std::lock_guard<std::mutex> g(g_vbcb_mu); }  // no lost wakeup vs FrameCounter
    g_vbcb_ran.notify_all();
  }
}

void OnVblank(uint64_t n, void*) {
  if (RotkCtx* c = rotk_current_ctx()) {
    RunVblankCallback(*c, n);
    { std::lock_guard<std::mutex> g(g_vbcb_mu); }  // no lost wakeup vs FrameCounter
    g_vbcb_ran.notify_all();
    return;
  }
  std::lock_guard<std::mutex> l(g_vbcb_mu);
  g_vbcb_pending = n;
  g_vbcb_cv.notify_one();
}

void SetVerticalBlankCallback(RotkCtx& c) {
  Seen("SetVerticalBlankCallback");
  uint32_t cb = rotk_arg(c, 0);
  rotk_wr32(kDeviceVa + 0x1DA8, cb);
  static std::once_flag once;
  std::call_once(once, [] {
    g_vbcb_data = rotk_hle_alloc(16, 16);
    rotk_spawn_thread(0x10000, VblankWorker, nullptr);
    rotk_vblank_add_callback(OnVblank, nullptr);
  });
  g_vbcb.store(cb);
  rotk_log("d3d8: vertical blank callback 0x%08x", cb);
  rotk_ret_stdcall(c, 0, 1);
}

// ---- GPU synchronisation ------------------------------------------------------------------
// Nothing runs behind the push sink, so the GPU is always idle and every fence has passed:
// KickOff (writes the PFIFO DMA put register) does nothing, fences are counted only.
uint32_t g_fence = 0;

// 0x12FD90 D3D KickOff (fastcall ecx = CDevice)
void KickOff(RotkCtx& c) {
  Seen("KickOff");
  rotk_ret_cdecl(c, c.eax);
}

// 0x12FEB0 D3D SetFence(Flags) -> fence (stdcall); CDevice+0x2C holds the last one
void SetFence(RotkCtx& c) {
  Seen("SetFence");
  g_fence += 2;
  rotk_wr32(kDeviceVa + 0x2C, g_fence);
  rotk_ret_stdcall(c, g_fence, 1);
}

// 0x12CDF0 BlockOnFence(Fence), 0x12FF60 D3D BlockOnTime(Time, bFlush): never wait
void BlockOnFence(RotkCtx& c) {
  Seen("BlockOnFence");
  rotk_ret_stdcall(c, 0, 1);
}
void BlockOnTime(RotkCtx& c) {
  Seen("BlockOnTime");
  rotk_ret_stdcall(c, 0, 2);
}

// 0x12D900 IsBusy() -> FALSE
void IsBusy(RotkCtx& c) {
  Seen("IsBusy");
  rotk_ret_cdecl(c, 0);
}

// ---- immediate mode (Begin / SetVertexData* / End) -------------------------------------
// The XDK's SetVertexData* write NV2A methods into the push buffer (here: the push sink).
// Begin marks the sink position; End (and MakeSpace in between) decode the vertex methods,
// and End draws them as a user-pointer draw: every v register the program reads comes from
// its immediate value (FLOAT4 at offset 16 * reg). Writing register 0 emits a vertex.
struct Immediate {
  bool active = false;
  uint32_t prim = 0, cursor = 0;
  uint32_t written = 0;         // v registers set since SET_BEGIN_END / Begin
  float attr[16][4];
  std::vector<float> verts;     // 16 * 4 floats per vertex
  std::vector<uint32_t> inl;    // INLINE_ARRAY words: vertices in the program's stream-0 layout
} g_imm;
constexpr uint32_t kImmMaxVerts = 0x4000;
uint32_t g_imm_buf = 0;

void ImmEmit() {
  if (g_imm.verts.size() / 64 >= kImmMaxVerts) return;
  g_imm.verts.insert(g_imm.verts.end(), &g_imm.attr[0][0], &g_imm.attr[0][0] + 64);
}

void ImmFlush();

void ImmMethod(uint32_t m, uint32_t v) {
  float f;
  std::memcpy(&f, &v, 4);
  if (m == 0x17FC) {  // SET_BEGIN_END: NV primitive = Xbox D3DPRIMITIVETYPE, 0 = end
    if (v) {
      g_imm.prim = v;
      g_imm.written = 0;
      g_imm.verts.clear();
      g_imm.inl.clear();
    } else {
      ImmFlush();
    }
  } else if (m == 0x1818) {  // INLINE_ARRAY
    g_imm.inl.push_back(v);
  } else if (m >= 0x1880 && m < 0x1900) {  // SET_VERTEX_DATA2F_M[16][2]
    uint32_t r = (m - 0x1880) / 8, k = (m / 4) & 1;
    g_imm.attr[r][k] = f;
    g_imm.written |= 1u << r;
    if (k == 1) {
      g_imm.attr[r][2] = 0, g_imm.attr[r][3] = 1;
      if (r == 0) ImmEmit();
    }
  } else if (m >= 0x1A00 && m < 0x1B00) {  // SET_VERTEX_DATA4F_M[16][4]
    uint32_t r = (m - 0x1A00) / 16, k = (m / 4) & 3;
    g_imm.attr[r][k] = f;
    g_imm.written |= 1u << r;
    if (r == 0 && k == 3) ImmEmit();
  } else if (m >= 0x1940 && m < 0x1980) {  // SET_VERTEX_DATA4UB[16]
    uint32_t r = (m - 0x1940) / 4;
    g_imm.written |= 1u << r;
    for (int k = 0; k < 4; k++) g_imm.attr[r][k] = float((v >> (8 * k)) & 0xFF) / 255.0f;
    if (r == 0) ImmEmit();
  } else if (m >= 0x1900 && m < 0x1940) {  // SET_VERTEX_DATA2S[16]
    uint32_t r = (m - 0x1900) / 4;
    g_imm.written |= 1u << r;
    g_imm.attr[r][0] = float(int16_t(v)), g_imm.attr[r][1] = float(int16_t(v >> 16));
    g_imm.attr[r][2] = 0, g_imm.attr[r][3] = 1;
    if (r == 0) ImmEmit();
  } else if (m >= 0x1980 && m < 0x1A00) {  // SET_VERTEX_DATA4S_M[16][2]
    uint32_t r = (m - 0x1980) / 8, k = (m / 4) & 1;
    g_imm.written |= 1u << r;
    g_imm.attr[r][2 * k] = float(int16_t(v)), g_imm.attr[r][2 * k + 1] = float(int16_t(v >> 16));
    if (r == 0 && k == 1) ImmEmit();
  } else if (m >= 0x1500 && m < 0x150C) {  // SET_VERTEX3F
    uint32_t k = (m - 0x1500) / 4;
    g_imm.attr[0][k] = f;
    if (k == 2) g_imm.attr[0][3] = 1, ImmEmit();
  } else if (m >= 0x1518 && m < 0x1528) {  // SET_VERTEX4F
    uint32_t k = (m - 0x1518) / 4;
    g_imm.attr[0][k] = f;
    if (k == 3) ImmEmit();
  }
}

// NV2A push words in [from, to): method headers (count << 18 | method, bit 30 non-increasing).
void DecodeImmediate(uint32_t from, uint32_t to) {
  for (uint32_t p = from; p + 4 <= to;) {
    uint32_t hdr = rotk_rd32(p);
    p += 4;
    if (!hdr) continue;
    if ((hdr & 3) || (hdr & 0xE0000000) == 0x20000000) break;  // jump/call: not in the sink
    uint32_t m = hdr & 0x1FFC, n = (hdr >> 18) & 0x7FF;
    bool ninc = hdr & 0x40000000;
    for (uint32_t i = 0; i < n && p + 4 <= to; i++, p += 4) ImmMethod(ninc ? m : m + 4 * i, rotk_rd32(p));
  }
}

void ImmDrainBeforeRewind() {
  if (!g_imm.active) return;
  DecodeImmediate(g_imm.cursor, rotk_rd32(kDeviceVa));
  g_imm.cursor = g_push_sink;
}

// 0x12E480 Begin(PrimitiveType)
void Begin(RotkCtx& c) {
  Seen("Begin");
  g_imm.active = true;
  g_imm.prim = rotk_arg(c, 0);
  g_imm.cursor = rotk_rd32(kDeviceVa);
  g_imm.written = 0;
  g_imm.verts.clear();
  for (auto& a : g_imm.attr) a[0] = a[1] = a[2] = 0, a[3] = 1;
  rotk_ret_stdcall(c, 0, 1);
}

// Bytes per vertex of the program's stream-0 declaration (INLINE_ARRAY layout).
uint32_t Stream0Stride(const VShader& vs) {
  uint32_t end = 0;
  for (int r = 0; r < 16; r++) {
    if (vs.attr[r][0] != 0) continue;
    uint32_t t = vs.attr[r][2], n = t >> 4, sz;
    switch (t & 0xF) {
      case 2: sz = 4 * n; break;                    // FLOATn
      case 0: sz = 4; break;                        // D3DCOLOR
      case 6: sz = 4; break;                        // NORMPACKED3
      case 1: case 5: sz = 2 * n; break;            // (NORM)SHORTn
      case 4: sz = n; break;                        // PBYTEn
      default: sz = t == 0x72 ? 8 : 0; break;       // FLOAT2H
    }
    end = std::max(end, vs.attr[r][1] + sz);
  }
  return end;
}

// Draws what the decoder gathered since SET_BEGIN_END(prim) (or Begin).
void ImmFlush() {
  uint32_t prim = g_imm.prim;
  VShader* cur = CurrentVs();
  bool prog = cur != nullptr;
  if (!g_imm_buf) g_imm_buf = rotk_hle_alloc(kImmMaxVerts * 256, 16);
  if (!g_imm.inl.empty()) {
    uint32_t stride = prog ? Stream0Stride(*cur) : 0;
    uint32_t bytes = uint32_t(g_imm.inl.size() * 4);
    if (!prog || !stride) {
      static bool once = false;
      if (!once) once = true, rotk_log("d3d8: inline-array draw (%u bytes) without a programmable shader: skipped", bytes);
    } else if (bytes <= kImmMaxVerts * 256) {
      std::memcpy(rotk_ptr(g_imm_buf), g_imm.inl.data(), bytes);
      StreamSrc ss[16];
      ss[0].va = g_imm_buf;
      ss[0].stride = stride;
      DrawCommon(prim, ss, 0, bytes / stride, nullptr, 0);
    }
  } else if (uint32_t n = uint32_t(g_imm.verts.size() / 64)) {
    if (prog) {
      std::memcpy(rotk_ptr(g_imm_buf), g_imm.verts.data(), size_t(n) * 256);
      // Fixed function: the NV2A pipeline reads every attribute register; the FVF only describes
      // vertex arrays. The game's UI quads set FVF 0x10042 (XYZ|DIFFUSE, no TEX) and feed v9 by
      // SetVertexData2f(D3DVSDE_TEXCOORD0): use the FVF program that also reads the texcoord,
      // diffuse and specular registers written in this block.
      VShader* base = cur;
      if (cur->ff) {
        uint32_t fvf = cur->fvf, ntex = (fvf >> 8) & 0xF;
        for (uint32_t t = 0; t < 4; t++)
          if (g_imm.written & (1u << (9 + t))) ntex = std::max(ntex, t + 1);
        if (g_imm.written & (1u << 3)) fvf |= 0x40;
        if (g_imm.written & (1u << 4)) fvf |= 0x80;
        fvf = (fvf & ~0xF00u) | (ntex << 8);
        if (fvf != cur->fvf) base = &FvfShader(fvf);
      }
      VertexProgram(*base);  // compile (once) before the copy: the copy must share the program
      VShader vs = *base;
      for (uint32_t r = 0; r < 16; r++)
        if (vs.attr[r][0] != 0xFF) vs.attr[r][0] = 0, vs.attr[r][1] = r * 16, vs.attr[r][2] = 0x42;
      StreamSrc ss[16];
      ss[0].va = g_imm_buf;
      ss[0].stride = 256;
      g_vs_input_override = &vs;
      DrawCommon(prim, ss, 0, n, nullptr, 0);
      g_vs_input_override = nullptr;
    } else {
      static bool once = false;
      if (!once) once = true, rotk_log("d3d8: immediate-mode draw with a fixed-function shader (%u vertices): skipped", n);
    }
  }
  g_imm.verts.clear();
  g_imm.inl.clear();
}

// 0x12E4C0 End()
void End(RotkCtx& c) {
  Seen("End");
  if (g_imm.active) {
    DecodeImmediate(g_imm.cursor, rotk_rd32(kDeviceVa));
    g_imm.active = false;
    ImmFlush();
  }
  rotk_ret_cdecl(c, 0);
}

// 0x12FD60 BeginPush(Count) -> push pointer (stdcall); 0x12FD80 EndPush(pPush) (stdcall).
// The game writes NV2A methods itself (ROTK: quads as INLINE_ARRAY, 0xCAE40); EndPush decodes
// and draws them. The XDK's lazy state flush before BeginPush is not needed (HLE draws read
// the shadow state).
uint32_t g_user_push = 0, g_user_push_end = 0;
void BeginPush(RotkCtx& c) {
  Seen("BeginPush");
  uint32_t bytes = (rotk_arg(c, 0) + 1) * 4;
  uint32_t put = rotk_rd32(kDeviceVa);
  uint32_t limit = g_push_sink + kPushSinkBytes - 0x100;
  if (put + bytes > limit) put = RewindPushSink();
  if (put + bytes > limit) {
    put = BigPushBuffer(bytes);
    g_user_push_end = put + g_big_push_bytes;
  } else {
    g_user_push_end = g_push_sink + kPushSinkBytes;
  }
  g_user_push = put;
  rotk_ret_stdcall(c, put, 1);
}

void EndPush(RotkCtx& c) {
  Seen("EndPush");
  uint32_t end = rotk_arg(c, 0);
  bool big = g_user_push && g_user_push == g_big_push;
  if (g_user_push && end > g_user_push && end <= g_user_push_end) {
    g_imm.verts.clear();
    g_imm.inl.clear();
    DecodeImmediate(g_user_push, end);
  }
  // Back to the sink after an own-buffer push (m_pPut must stay below m_pThreshold's region).
  if (big) RewindPushSink();
  else rotk_wr32(kDeviceVa, end);
  g_user_push = 0;
  rotk_ret_stdcall(c, 0, 1);
}

// 0x12E5B0 RunPushBuffer(pPushBuffer, pFixup) (stdcall): not decoded yet. Counted and logged
// (per frame: calls, bytes) so the share of the scene it carries is measurable; runs the
// lifted body, which copies into the push sink.
uint64_t g_rpb_calls = 0, g_rpb_bytes = 0;
void RunPushBuffer(RotkCtx& c) {
  Seen("RunPushBuffer");
  uint32_t pb = rotk_arg(c, 0);
  uint32_t size = pb ? rotk_rd32(pb + 0x0C) : 0;  // D3DPushBuffer: Common, Data, Lock, Size, AllocationSize
  g_rpb_calls++;
  g_rpb_bytes += size;
  static uint64_t last = 0;
  if (g_dev.frame >= last + 300) {
    last = g_dev.frame;
    rotk_log("d3d8: RunPushBuffer: %llu calls, %llu KB so far (frame %llu); not drawn yet",
             (unsigned long long)g_rpb_calls, (unsigned long long)(g_rpb_bytes >> 10),
             (unsigned long long)g_dev.frame);
  }
  rotk_ret_stdcall(c, 0, 2);
}

// 0x135A30 D3D lazy state flush (render/texture states -> NV2A methods): nothing to push.
void LazySetState(RotkCtx& c) {
  Seen("LazySetState");
  rotk_ret_cdecl(c, c.eax);
}

// 0x12A6C0 SetRenderState_Simple push stub: fastcall(ecx = method | count << 18,
// edx = value), plain ret. The inlined callers store the render-state cell
// themselves right after; mirror it anyway for the methods seen.
void Push1(RotkCtx& c) {
  Seen("Push1");
  uint32_t m = c.ecx & 0x1FFFC, v = c.edx;
  int rs = -1;
  switch (m) {
    case 0x300: rs = RS_ALPHATESTENABLE; break;
    case 0x304: rs = RS_ALPHABLENDENABLE; break;
    case 0x310: rs = RS_DITHERENABLE; break;
    case 0x33C: rs = RS_ALPHAFUNC; break;
    case 0x340: rs = RS_ALPHAREF; break;
    case 0x344: rs = RS_SRCBLEND; break;
    case 0x348: rs = RS_DESTBLEND; break;
    case 0x350: rs = RS_BLENDOP; break;
    case 0x354: rs = RS_ZFUNC; break;
    case 0x358: rs = RS_COLORWRITEENABLE; break;
    case 0x35C: rs = RS_ZWRITEENABLE; break;
    // ROTK (5558): the game keeps its own state cache and sets these only through the stub
    // (0x12A6C0); Xbox D3D enums are the NV2A values, so the cells take them as they are.
    case 0x30C: rs = RS_ZENABLE; break;
    case 0x32C: rs = RS_STENCILENABLE; break;
    case 0x34C: rs = RS_BLENDCOLOR; break;
    case 0x360: rs = RS_STENCILWRITEMASK; break;
    case 0x364: rs = RS_STENCILFUNC; break;
    case 0x368: rs = RS_STENCILREF; break;
    case 0x36C: rs = RS_STENCILMASK; break;
    case 0x370: rs = RS_STENCILFAIL; break;
    case 0x374: rs = RS_STENCILZFAIL; break;
    case 0x378: rs = RS_STENCILPASS; break;
    case 0x37C: rs = RS_SHADEMODE; break;
  }
  if (rs >= 0)
    SetRS(uint32_t(rs), v);
  else {
    static std::set<uint32_t> warned;
    if (warned.insert(m).second) rotk_log("d3d8: Push1 method %04X value %08X not mapped", m, v);
  }
  rotk_ret_cdecl(c, c.eax);
}

#define RS_SETTER(fnname, idx)              \
  void fnname(RotkCtx& c) {                  \
    Seen(#fnname);                          \
    SetRS(idx, rotk_arg(c, 0));              \
    TRACE("d3d8: " #fnname " %08X", rotk_arg(c, 0)); \
    rotk_ret_stdcall(c, 0, 1);               \
  }
RS_SETTER(SetRenderState_FogColor, RS_FOGCOLOR)          // 0x12AA40
RS_SETTER(SetRenderState_CullMode, RS_CULLMODE)          // 0x130B50
RS_SETTER(SetRenderState_NormalizeNormals, RS_NORMALIZENORMALS)  // 0x12AB40
RS_SETTER(SetRenderState_ZEnable, RS_ZENABLE)            // 0x12B910
RS_SETTER(SetRenderState_StencilEnable, RS_STENCILENABLE)  // 0x12B9A0

// 0x12B040 SetTextureState_BumpEnv(Stage, Type, Value)
void SetTextureState_BumpEnv(RotkCtx& c) {
  Seen("SetTextureState_BumpEnv");
  uint32_t s = rotk_arg(c, 0), t = rotk_arg(c, 1), v = rotk_arg(c, 2);
  if (s < 4 && t < TSS_COUNT) SetTS(s, t, v);
  rotk_ret_stdcall(c, 0, 3);
}

// 0x12B0B0 SetTextureState_BorderColor(Stage, Value)
void SetTextureState_BorderColor(RotkCtx& c) {
  Seen("SetTextureState_BorderColor");
  uint32_t s = rotk_arg(c, 0), v = rotk_arg(c, 1);
  if (s < 4) SetTS(s, TSS_BORDERCOLOR, v);
  rotk_ret_stdcall(c, 0, 2);
}

// 0x12C290 CreateVertexShader(pDeclaration, pFunction, pHandle, Usage)
void CreateVertexShader(RotkCtx& c) {
  Seen("CreateVertexShader");
  uint32_t decl = rotk_arg(c, 0), func = rotk_arg(c, 1), ph = rotk_arg(c, 2);
  VShader vs;
  vs.index = uint32_t(g_vshaders.size());
  for (auto& a : vs.attr) a[0] = 0xFF;
  if (func) {
    uint32_t hdr = rotk_rd32(func);
    uint32_t type = hdr & 0xFFFF, count = hdr >> 16;
    if (type != 0x2078)
      rotk_log("d3d8: CreateVertexShader: function type %04X (expected 0x2078 'xvs')", type);
    vs.func.resize(size_t(count) * 4);
    std::memcpy(vs.func.data(), rotk_ptr(func + 4), vs.func.size() * 4);
  } else {
    rotk_log("d3d8: CreateVertexShader without a function (fixed-function declaration)");
  }
  if (decl) {
    uint32_t stream = 0, offset = 0;
    for (uint32_t p = decl, guard = 0; guard < 256; guard++) {
      uint32_t t = rotk_rd32(p);
      p += 4;
      if (t == 0xFFFFFFFF) break;
      uint32_t kind = t >> 29;
      if (kind == VSD_TOKEN_STREAM) {
        stream = t & 0xF;
        offset = 0;
      } else if (kind == VSD_TOKEN_STREAMDATA) {
        if (t & 0x10000000) {  // skip
          uint32_t n = (t >> 16) & 0xF;
          offset += (t & 0x08000000) ? n : n * 4;
        } else {
          uint32_t reg = t & 0x1F, type = (t >> 16) & 0xFF;
          if (reg < 16) {
            vs.attr[reg][0] = type == VSDT_NONE ? 0xFF : stream;
            vs.attr[reg][1] = offset;
            vs.attr[reg][2] = type;
          }
          offset += VsdtSize(type);
        }
      } else if (kind == VSD_TOKEN_CONSTMEM) {
        uint32_t n = (t >> 25) & 0xF, addr = t & 0xFF;
        std::vector<uint32_t> vals(n * 4);
        for (auto& v : vals) v = rotk_rd32(p), p += 4;
        vs.constants.push_back({addr, std::move(vals)});
      } else if (kind == VSD_TOKEN_EXT) {
        p += 4 * ((t >> 24) & 0x1F);
      } else if (kind == VSD_TOKEN_TESSELLATOR || kind == VSD_TOKEN_NOP) {
        // not used by this game
      }
    }
  }
  if (g_cfg.trace) {
    std::string a;
    char b[48];
    for (int r = 0; r < 16; r++)
      if (vs.attr[r][0] != 0xFF) {
        std::snprintf(b, sizeof b, " v%d=s%u+%u:%02X", r, vs.attr[r][0], vs.attr[r][1], vs.attr[r][2]);
        a += b;
      }
    rotk_log("d3d8: vs %u decl %08X func %08X:%s", vs.index, decl, func, a.c_str());
  }
  uint32_t obj = NewHeader(16);
  rotk_wr32(obj, kVsMagic);
  rotk_wr32(obj + 4, vs.index);
  // +8: the program size in instructions, as in the XDK object (GetVertexShaderSize, 0x12C7D0,
  // stays lifted and reads it: the game allocates program slots by it).
  rotk_wr32(obj + 8, uint32_t(vs.func.size() / 4));
  g_vshaders.push_back(std::move(vs));
  g_vs_sel_dirty = true;  // g_vs_sel_last may point into the old array
  if (!g_cfg.shader_check && !g_vshaders.back().func.empty())
    PrecompileVertexProgram(g_vshaders.back());
  if (g_cfg.shader_check) {
    VShader& v = g_vshaders.back();
    bool ok = VertexProgram(v) != 0;
    static uint32_t n_ok = 0, n_bad = 0;
    (ok ? n_ok : n_bad)++;
    if (!ok) rotk_log("d3d8: shader check: vertex shader %u (function %08X) FAILED", v.index, func);
    rotk_log("d3d8: shader check: %u vertex shaders compiled, %u failed", n_ok, n_bad);
  }
  if (ph) rotk_wr32(ph, obj | 1);
  rotk_ret_stdcall(c, 0, 4);
}

bool DevLogFrame() {
  static const uint64_t f = [] {
    const char* v = getenv("ROTK_DEV_LOG_DRAWS");
    return v ? strtoull(v, nullptr, 10) : ~0ull;
  }();
  return g_dev.frame + 1 == f;
}

// 0x12C3D0 SetVertexShaderConstant1: fastcall(ecx = register 0..191, edx = data)
void SetVertexShaderConstant1(RotkCtx& c) {
  if (DevLogFrame()) rotk_log("  const1 reg %u (c%d) n %u", c.ecx, int(c.ecx) - 96, 1);
  Seen("SetVertexShaderConstant1");
  uint32_t r = c.ecx;
  if (r < 192) std::memcpy(g_dev.vs_c[r], rotk_ptr(c.edx), 16);
  rotk_ret_cdecl(c, c.eax);
}
// 0x12C430 SetVertexShaderConstant4: fastcall, 4 registers
void SetVertexShaderConstant4(RotkCtx& c) {
  if (DevLogFrame()) rotk_log("  const4 reg %u (c%d) n %u", c.ecx, int(c.ecx) - 96, 4);
  Seen("SetVertexShaderConstant4");
  uint32_t r = c.ecx;
  for (uint32_t i = 0; i < 4; i++)
    if (r + i < 192) std::memcpy(g_dev.vs_c[r + i], rotk_ptr(c.edx + 16 * i), 16);
  rotk_ret_cdecl(c, c.eax);
}
// 0x12C4E0 SetVertexShaderConstantNotInline: fastcall(ecx reg, edx data), [esp+4] count.
// The count is in DWORDs (the lib copies it with rep movsd): the game passes
// 8 for two registers, 12 for three.
void SetVertexShaderConstantNotInline(RotkCtx& c) {
  Seen("SetVertexShaderConstantNotInline");
  uint32_t r = c.ecx, n = rotk_arg(c, 0);
  if (DevLogFrame()) rotk_log("  constN reg %u (c%d) %u dwords", r, int(r) - 96, n);
  for (uint32_t i = 0; i < n; i++)
    if (r * 4 + i < 192 * 4) std::memcpy(&g_dev.vs_c[0][0] + r * 4 + i, rotk_ptr(c.edx + 4 * i), 4);
  rotk_ret_stdcall(c, c.eax, 1);
}

// 0x12C600 SetStreamSource(StreamNumber, pStreamData, Stride)
void SetStreamSource(RotkCtx& c) {
  Seen("SetStreamSource");
  uint32_t n = rotk_arg(c, 0);
  if (n < 16) {
    g_dev.streams[n].vb = rotk_arg(c, 1);
    g_dev.streams[n].stride = rotk_arg(c, 2);
  }
  rotk_ret_stdcall(c, 0, 3);
}

// 0x12C7F0 SetShaderConstantMode(Mode)
void SetShaderConstantMode(RotkCtx& c) {
  Seen("SetShaderConstantMode");
  g_dev.const_mode = rotk_arg(c, 0);
  rotk_log("d3d8: SetShaderConstantMode %X", g_dev.const_mode);
  rotk_ret_stdcall(c, 0, 1);
}

// 0x12CA60 SetVertexShader(Handle)
void SetVertexShader(RotkCtx& c) {
  Seen("SetVertexShader");
  uint32_t h = rotk_arg(c, 0);
  g_dev.vs_handle = h;
  g_dev.vs_start = -1;
  if ((h & 1) && rotk_rd32(h & ~1u) == kVsMagic) {
    uint32_t i = rotk_rd32((h & ~1u) + 4);
    if (i < g_vshaders.size()) {
      for (auto& [addr, vals] : g_vshaders[i].constants)
        for (size_t k = 0; k < vals.size() / 4; k++)
          if (addr + k < 192) std::memcpy(g_dev.vs_c[addr + k], &vals[4 * k], 16);
      // The XDK loads a program at slot 0 (LoadVertexShader(h, 0) + Select(h, 0)).
      const auto& f = g_vshaders[i].func;
      std::memcpy(g_vs_slots, f.data(), std::min(f.size(), size_t(kVsSlots * 4)) * 4);
    }
  }
  rotk_ret_stdcall(c, 0, 1);
}

// 0x12C6D0 LoadVertexShader(Handle, Address): the handle's program to slots Address.. (the XDK
// pushes SET_TRANSFORM_PROGRAM_LOAD + the words; its handle layout is not the HLE's).
void LoadVertexShader(RotkCtx& c) {
  Seen("LoadVertexShader");
  uint32_t h = rotk_arg(c, 0), addr = rotk_arg(c, 1);
  if ((h & 1) && rotk_rd32(h & ~1u) == kVsMagic) {
    uint32_t i = rotk_rd32((h & ~1u) + 4);
    if (i < g_vshaders.size() && addr < kVsSlots) {
      // The XDK's handle snippet carries the declaration's constants next to the program
      // (0x12BED0 parses both): they load with it.
      for (auto& [ca, vals] : g_vshaders[i].constants)
        for (size_t k = 0; k < vals.size() / 4; k++)
          if (ca + k < 192) std::memcpy(g_dev.vs_c[ca + k], &vals[4 * k], 16);
      const auto& f = g_vshaders[i].func;
      size_t n = std::min(f.size(), size_t(kVsSlots - addr) * 4);
      std::memcpy(&g_vs_slots[addr * 4], f.data(), n * 4);
      g_vs_sel_dirty = true;
    }
  } else {
    rotk_log("d3d8: LoadVertexShader: handle %08X is not an HLE vertex shader", h);
  }
  rotk_ret_stdcall(c, 0, 2);
}

// 0x12C730 SelectVertexShader(Handle, Address): Handle's declaration (0: keep the current one),
// the program from slot Address (SET_TRANSFORM_PROGRAM_START). Constants are not touched.
void SelectVertexShader(RotkCtx& c) {
  Seen("SelectVertexShader");
  uint32_t h = rotk_arg(c, 0), addr = rotk_arg(c, 1);
  if (h) g_dev.vs_handle = h;
  g_dev.vs_start = int32_t(std::min(addr, kVsSlots - 1));
  g_vs_sel_dirty = true;
  rotk_ret_stdcall(c, 0, 2);
}

// 0x130350 CreateVertexBuffer2(Length) -> IDirect3DVertexBuffer8*
void CreateVertexBuffer2(RotkCtx& c) {
  Seen("CreateVertexBuffer2");
  uint32_t len = rotk_arg(c, 0);
  uint32_t data = AllocContiguous(c, len, 0);
  uint32_t vb = 0;
  if (data) {
    vb = NewHeader(16);
    rotk_wr32(vb, COMMON_TYPE_VERTEXBUFFER | COMMON_D3DCREATED | 1);
    rotk_wr32(vb + 4, data & 0x0FFFFFFF);
    rotk_wr32(vb + 8, 0);
  }
  rotk_ret_stdcall(c, vb, 1);
}

// 0x1303A0 VertexBuffer_Lock2(pVB, Flags) -> BYTE*
void VertexBuffer_Lock2(RotkCtx& c) {
  Seen("VertexBuffer_Lock2");
  uint32_t vb = rotk_arg(c, 0);
  rotk_ret_stdcall(c, CpuAddr(Hdr{vb}.data()), 2);
}

// 0x12D110 SetRenderTarget(pRenderTarget, pNewZStencil)
void SetRenderTarget(RotkCtx& c) {
  Seen("SetRenderTarget");
  uint32_t rt = rotk_arg(c, 0), z = rotk_arg(c, 1);
  if (rt) g_dev.rt = rt;
  g_dev.z = z;
  g_dev.vp_w = 0;  // D3D resets the viewport to the new target
  TRACE("d3d8: SetRenderTarget %08X %08X", rt, z);
  SyncDeviceFields();
  rotk_ret_stdcall(c, 0, 2);
}

// 0x12D400 SetViewport(const D3DVIEWPORT8*) / 0x12CD80 GetViewport(D3DVIEWPORT8*)
// The XDK derives the screen-space constants (c-38 scale, c-37 offset) from it; the draw
// path does the same. Its lifted body reads CDevice render-target fields the HLE does not keep.
void SetViewport(RotkCtx& c) {
  Seen("SetViewport");
  uint32_t p = rotk_arg(c, 0);
  if (p) {
    g_dev.vp_x = rotk_rd32(p), g_dev.vp_y = rotk_rd32(p + 4);
    g_dev.vp_w = rotk_rd32(p + 8), g_dev.vp_h = rotk_rd32(p + 12);
    uint32_t a = rotk_rd32(p + 16), b = rotk_rd32(p + 20);
    std::memcpy(&g_dev.vp_minz, &a, 4);
    std::memcpy(&g_dev.vp_maxz, &b, 4);
    TRACE("d3d8: SetViewport %u,%u %ux%u z %.2f..%.2f", g_dev.vp_x, g_dev.vp_y, g_dev.vp_w,
          g_dev.vp_h, g_dev.vp_minz, g_dev.vp_maxz);
  }
  rotk_ret_stdcall(c, 0, 1);
}

void GetViewport(RotkCtx& c) {
  Seen("GetViewport");
  uint32_t p = rotk_arg(c, 0);
  if (p) {
    uint32_t w = g_dev.vp_w, h = g_dev.vp_h;
    if (!w) {
      uint32_t zf;
      if (!g_dev.rt || !SurfaceSize(g_dev.rt, &w, &h, &zf)) w = g_dev.bb_w, h = g_dev.bb_h;
    }
    rotk_wr32(p, g_dev.vp_w ? g_dev.vp_x : 0);
    rotk_wr32(p + 4, g_dev.vp_w ? g_dev.vp_y : 0);
    rotk_wr32(p + 8, w);
    rotk_wr32(p + 12, h);
    uint32_t a, b;
    float mn = g_dev.vp_w ? g_dev.vp_minz : 0.f, mx = g_dev.vp_w ? g_dev.vp_maxz : 1.f;
    std::memcpy(&a, &mn, 4);
    std::memcpy(&b, &mx, 4);
    rotk_wr32(p + 16, a);
    rotk_wr32(p + 20, b);
  }
  rotk_ret_stdcall(c, 0, 1);
}

// 0x12CC60 SetTransform(State, pMatrix)
void SetTransform(RotkCtx& c) {
  Seen("SetTransform");
  uint32_t s = rotk_arg(c, 0), m = rotk_arg(c, 1);
  if (s < 10) std::memcpy(g_dev.transforms[s], rotk_ptr(m), 64);
  rotk_ret_stdcall(c, 0, 2);
}

// 0x132FD0 SetMaterial(pMaterial)
void SetMaterial(RotkCtx& c) {
  Seen("SetMaterial");
  std::memcpy(g_dev.material, rotk_ptr(rotk_arg(c, 0)), sizeof g_dev.material);
  rotk_ret_stdcall(c, 0, 1);
}

// 0x12CBD0 SetGammaRamp(Flags, const D3DGAMMARAMP* pRamp {BYTE red[256], green[256], blue[256]})
// The XDK writes the ramp into the NV2A RAMDAC (D3DSGR_IMMEDIATE) or at the next vblank. The
// host display has its own gamma: the ramp is kept (polish may apply it in the final pass) and
// a non-identity ramp is logged once.
uint8_t g_gamma_ramp[3][256];
void SetGammaRamp(RotkCtx& c) {
  Seen("SetGammaRamp");
  std::memcpy(g_gamma_ramp, rotk_ptr(rotk_arg(c, 1)), sizeof g_gamma_ramp);
  static bool logged = false;
  if (!logged) {
    bool identity = true;
    for (int ch = 0; ch < 3; ch++)
      for (int i = 0; i < 256; i++) identity &= g_gamma_ramp[ch][i] == i;
    if (!identity) {
      logged = true;
      rotk_log("d3d8: game sets a non-identity gamma ramp (r[128]=%u g[128]=%u b[128]=%u): not applied",
               g_gamma_ramp[0][128], g_gamma_ramp[1][128], g_gamma_ramp[2][128]);
    }
  }
  rotk_ret_stdcall(c, 0, 2);
}

// 0x130270 KickOffAndWaitForIdle()
void KickOffAndWaitForIdle(RotkCtx& c) {
  Seen("KickOffAndWaitForIdle");
  rotk_ret_cdecl(c, 0);
}

// 0x12CE30 BlockUntilVerticalBlank()
void BlockUntilVerticalBlank(RotkCtx& c) {
  Seen("BlockUntilVerticalBlank");
  // The game is built for 30 fps: its present FUN_000BF1A0 waits here for a vblank and the main
  // loop FUN_000D03A0 then spins one more tick past the frame mark (FrameCounter). Its timestep is
  // ticks x 1/60 (FUN_0009EB20), so with this wait skipped the spin alone paces it at 60 fps;
  // Swap still presents at most once per vblank.
  if (g_cfg.fps60) {
    rotk_ret_cdecl(c, 0);
    return;
  }
  auto t0 = std::chrono::steady_clock::now();
  rotk_vblank_wait(rotk_vblank_count());
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  g_ov.wait_ms += ms;
  g_dev.block_wait_ms += ms;
  rotk_ret_cdecl(c, 0);
}

// 0xBEE00 FrameCounter() -> [0x1E2F14] (game code: mov eax,[0x1E2F14]; ret). The main loop
// FUN_000D03A0 (0xD03D3..0xD03EF) spins on it until it moved past the frame mark [0x1E3340]. A
// call that comes back to back (< 0.5 ms) with the value unchanged and still at the mark (the
// spin's 2nd pass) blocks until the vblank callback 0xBEDE0 bumps it, so the spin stops burning a core and ROTK_FRAME_LOG counts it as wait, not busy.
// The deterministic test clock keeps the plain read (its vblanks advance on waits elsewhere).
void FrameCounter(RotkCtx& c) {
  constexpr uint32_t kCounter = 0x1E2F14, kMark = 0x1E3340;  // mark: counter at the last frame end
  static const bool block = !std::getenv("ROTK_TEST_CLOCK");
  static thread_local uint32_t last_v = 0;
  static thread_local auto last_t = std::chrono::steady_clock::time_point{};
  uint32_t v = rotk_rd32(kCounter);
  auto now = std::chrono::steady_clock::now();
  if (block && g_vbcb.load() && v == rotk_rd32(kMark) && v == last_v &&
      now - last_t < std::chrono::microseconds(500)) {
    rotk_vblank_wait(rotk_vblank_count());
    if (rotk_rd32(kCounter) == v) {  // the callback runs on the vblank worker: wait for it
      std::unique_lock<std::mutex> l(g_vbcb_mu);
      g_vbcb_ran.wait_for(l, std::chrono::milliseconds(20), [&] { return rotk_rd32(kCounter) != v; });
    }
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - now).count();
    g_ov.wait_ms += ms;
    g_dev.block_wait_ms += ms;
    now = t1;
    v = rotk_rd32(kCounter);
  }
  last_v = v;
  last_t = now;
  rotk_ret_cdecl(c, v);
}

// 0x12D390 GetBackBuffer2(BackBuffer) -> surface (AddRef'd); -1 = front buffer
void GetBackBuffer2(RotkCtx& c) {
  Seen("GetBackBuffer2");
  int32_t i = int32_t(rotk_arg(c, 0));
  uint32_t s = i == -1 ? g_dev.front : g_dev.back;
  AddRef(s);
  rotk_ret_stdcall(c, s, 1);
}

// 0x133380 GetRenderTarget2() -> surface (AddRef'd)
void GetRenderTarget2(RotkCtx& c) {
  Seen("GetRenderTarget2");
  AddRef(g_dev.rt);
  rotk_ret_cdecl(c, g_dev.rt);
}

// 0x12D3E0 GetDepthStencilSurface2() -> surface (AddRef'd) or 0
void GetDepthStencilSurface2(RotkCtx& c) {
  Seen("GetDepthStencilSurface2");
  AddRef(g_dev.z);
  rotk_ret_cdecl(c, g_dev.z);
}

// 0x12D630 SetTexture(Stage, pTexture)
void SetTexture(RotkCtx& c) {
  Seen("SetTexture");
  uint32_t s = rotk_arg(c, 0);
  if (s < 4) g_dev.tex[s] = rotk_arg(c, 1);
  rotk_ret_stdcall(c, 0, 2);
}

// 0x12D7E0 SetPalette(Stage, pPalette): the XDK pushes the palette DMA address (refcounted
// via Common); the HLE decodes P8 textures with it (BindTexture).
void SetPalette(RotkCtx& c) {
  Seen("SetPalette");
  uint32_t s = rotk_arg(c, 0), pal = rotk_arg(c, 1);
  if (s < 4) g_dev.palette[s] = pal;
  rotk_ret_stdcall(c, 0, 2);
}

// 0x12D870 SetIndices(pIndexBuffer, BaseVertexIndex): like the lib, publishes the
// buffer's Data in D3D__IndexData (0x13A424), which the game reads inline to
// build the pIndexData it passes to DrawIndexedVertices.
void SetIndices(RotkCtx& c) {
  Seen("SetIndices");
  uint32_t ib = rotk_arg(c, 0);
  g_dev.idx_data = ib;
  g_dev.idx_base = rotk_arg(c, 1);
  rotk_wr32(kIndexDataVa, ib ? rotk_rd32(ib + 4) : 0);
  rotk_ret_stdcall(c, 0, 2);
}

// 0x130420 CreatePixelShader(pPSDef, pHandle): {refcount, flags, pDef, copy of def}
void CreatePixelShader(RotkCtx& c) {
  Seen("CreatePixelShader");
  uint32_t def = rotk_arg(c, 0), ph = rotk_arg(c, 1);
  uint32_t obj = NewHeader(0xFC);
  rotk_wr32(obj, 1);
  rotk_wr32(obj + 4, 1);
  rotk_wr32(obj + 8, obj + 0xC);
  std::memcpy(rotk_ptr(obj + 0xC), rotk_ptr(def), 0xF0);
  if (ph) rotk_wr32(ph, obj);
  if (g_cfg.shader_check) {
    PsKey k;
    k.combiners = true;
    for (uint32_t i = 0; i <= RS_PSINPUTTEXTURE; i++) k.words[i] = rotk_rd32(def + 4 * i);
    k.words[57] = rotk_rd32(def + 4 * 54);
    k.words[58] = 1;
    k.nwords = 60;
    for (int s = 0; s < 4; s++) k.tex_kind[s] = ((k.words[57] >> (5 * s)) & 0x1F) ? 1 : 0;
    rotk_log("d3d8: shader check: pixel shader %08X %s", def,
            FragmentProgram(k) ? "compiled" : "FAILED");
  }
  rotk_log("d3d8: CreatePixelShader %08X: combiners %08X texmodes %08X -> %08X", def,
          rotk_rd32(def + 4 * RS_PSCOMBINERCOUNT), rotk_rd32(def + 4 * 54), obj);
  rotk_ret_stdcall(c, 0, 2);
}

// 0x1304D0 SetPixelShader(Handle): copies the definition into the render states.
void SetPixelShader(RotkCtx& c) {
  Seen("SetPixelShader");
  uint32_t h = rotk_arg(c, 0);
  g_dev.ps = h;
  if (h) {
    uint32_t def = rotk_rd32(h + 8);
    for (uint32_t i = 0; i <= RS_PSINPUTTEXTURE; i++) SetRS(i, rotk_rd32(def + 4 * i));
    SetRS(RS_PSTEXTUREMODES, rotk_rd32(def + 4 * 54));
  }
  rotk_ret_stdcall(c, 0, 1);
}

// 0x133CD0 SetPixelShaderConstant(Register, pConstantData, ConstantCount): packs each
// float4 to D3DCOLOR and stores it in every combiner constant slot the
// current shader maps to that register (PSC0Mapping/PSC1Mapping/final).
void SetPixelShaderConstant(RotkCtx& c) {
  Seen("SetPixelShaderConstant");
  uint32_t reg = rotk_arg(c, 0), data = rotk_arg(c, 1), n = rotk_arg(c, 2);
  if (g_dev.ps) {
    uint32_t def = rotk_rd32(g_dev.ps + 8);
    uint32_t m0 = rotk_rd32(def + 0xE4), m1 = rotk_rd32(def + 0xE8), mf = rotk_rd32(def + 0xEC);
    for (uint32_t k = 0; k < n; k++, reg++) {
      float v[4];
      std::memcpy(v, rotk_ptr(data + 16 * k), 16);
      auto ch = [](float f) { return uint32_t(std::lround(std::clamp(f, 0.f, 1.f) * 255.f)); };
      uint32_t argb = ch(v[3]) << 24 | ch(v[0]) << 16 | ch(v[1]) << 8 | ch(v[2]);
      for (uint32_t i = 0; i < 8; i++) {
        if (((m0 >> (4 * i)) & 0xF) == reg) SetRS(RS_PSCONSTANT0_0 + i, argb);
        if (((m1 >> (4 * i)) & 0xF) == reg) SetRS(RS_PSCONSTANT1_0 + i, argb);
      }
      if ((mf & 0xF) == reg) SetRS(RS_PSFINALCOMBINERCONSTANT0, argb);
      if (((mf >> 4) & 0xF) == reg) SetRS(RS_PSFINALCOMBINERCONSTANT1, argb);
    }
  }
  rotk_ret_stdcall(c, 0, 3);
}

// 0x134120 / 0x12EAF0 Resource_AddRef / Release(pThis)
void Resource_Release(RotkCtx& c) {
  Seen("Resource_Release");
  uint32_t r = rotk_arg(c, 0);
  uint32_t common = rotk_rd32(r);
  uint32_t ref = common & COMMON_REFCOUNT_MASK;
  if (ref <= 1) {
    rotk_wr32(r, common & ~COMMON_REFCOUNT_MASK);
    uint32_t type = common & COMMON_TYPE_MASK;
    if (type == COMMON_TYPE_SURFACE) {
      uint32_t parent = rotk_rd32(r + 0x14);
      if (parent) {
        uint32_t pc = rotk_rd32(parent);
        if ((pc & COMMON_REFCOUNT_MASK) > 1)
          rotk_wr32(parent, pc - 1);
      }
    }
    // Free D3D-created data (not surfaces of a parent texture, not the
    // device's own buffers) once the GPU is surely done with it.
    bool own = (common & COMMON_D3DCREATED) && !(type == COMMON_TYPE_SURFACE && rotk_rd32(r + 0x14));
    if (own && r != g_dev.back && r != g_dev.front && r != g_dev.zbuf) {
      uint32_t data = rotk_rd32(r + 4);
      if (data) {
        for (bool dep : {false, true})
          if (Target* t = FindTarget(data, dep)) {
            gpu::DestroyTexture(t->opaque);
            gpu::DestroyTexture(t->tex);
            g_targets.erase(uint64_t(data) << 1 | dep);
          }
        g_dev.pending_free.push_back({CpuAddr(data), g_dev.frame + 4});
        rotk_wr32(r + 4, 0);
      }
    }
    rotk_ret_stdcall(c, 0, 1);
    return;
  }
  rotk_wr32(r, common - 1);
  rotk_ret_stdcall(c, ref - 1, 1);
}

// 0x12EBD0 Resource_Register(pThis, pBase): Data += pBase (physical, except push buffers)
void Resource_Register(RotkCtx& c) {
  Seen("Resource_Register");
  uint32_t r = rotk_arg(c, 0), base = rotk_arg(c, 1);
  uint32_t d = rotk_rd32(r + 4) + base;
  if ((rotk_rd32(r) & COMMON_TYPE_MASK) != COMMON_TYPE_PUSHBUFFER) d &= 0x0FFFFFFF;
  rotk_wr32(r + 4, d);
  rotk_ret_stdcall(c, 0, 2);
}

// 0x134270 Resource_BlockUntilNotBusy(pThis): draws copy their data, nothing to wait for.
void Resource_BlockUntilNotBusy(RotkCtx& c) { rotk_ret_stdcall(c, 0, 1); }

// 0x12F6D0 Swap(Flags)
// Hang watchdog (owner bug 2026-11-06: the prologue froze, frames still presented): frames go
// on but the game stopped reading the pad for 8 s -> log the blocked guest waits (every 30 s).
void HangCheck() {
  if (g_dev.frame % 120) return;
  static uint64_t last_report = 0;
  uint64_t now = rotk_xinput_now_ns(), read = rotk_xinput_last_read_ns();
  if (!read || now - read < 8000000000ull || now - last_report < 30000000000ull) return;
  last_report = now;
  rotk_log("hang: frame %llu: the game has not read the pad for %.1f s", (unsigned long long)g_dev.frame,
           (now - read) / 1e9);
  rotk_kernel_log_waits(2.0);
}

void Swap(RotkCtx& c) {
  Seen("Swap");
  g_dev.frame++;
  HangCheck();
  bool shot = (g_cfg.shot_every && g_dev.frame % g_cfg.shot_every == 0) ||
              g_cfg.shot_frames.count(g_dev.frame);
  if (float hold = rotk_movie_hold_progress(); hold > 0 && g_dev.back)
    if (Target* t = FindTarget(Hdr{g_dev.back}.data(), false)) {  // hold-A skip ring, bottom right
      float r = t->ph * 0.05f;
      gpu::DrawRing(t->tex, t->pw - r * 2.2f, t->ph - r * 2.2f, r, hold);
    }
  Present(g_dev.back);
  if (shot) Screenshot(g_dev.back, "frame", g_dev.frame);
  if (shot) ScreenshotPresent(g_dev.frame);
  // Dev: ROTK_DEV_DUMP_TARGETS=<frame> writes every colour render target.
  static const uint64_t dump_rt = [] {
    const char* v = getenv("ROTK_DEV_DUMP_TARGETS");
    return v ? strtoull(v, nullptr, 10) : ~0ull;
  }();
  if (g_dev.frame == dump_rt)
    for (auto& [k, t] : g_targets) {
      if (t.depth) continue;
      std::vector<uint8_t> px;
      if (gpu::ReadPixels(t.tex, t.pw, t.ph, &px)) {
        char path[600];
        std::snprintf(path, sizeof path, "%s/target_%08X_%ux%u.png", g_cfg.shot_dir.c_str(), t.data,
                      t.w, t.h);
        gpu::WritePng(path, t.pw, t.ph, px.data());
      }
    }
  // Present = the back buffer becomes the front buffer.
  if (g_dev.back && g_dev.front) {
    uint32_t b = rotk_rd32(g_dev.back + 4), f = rotk_rd32(g_dev.front + 4);
    rotk_wr32(g_dev.back + 4, f);
    rotk_wr32(g_dev.front + 4, b);
  }
  gpu::EndFrame();
  FreePending(c, false);
  // 60.000 Hz: at most one Swap per vblank (PresentationInterval 1).
  auto wait0 = std::chrono::steady_clock::now();
  if (g_cfg.pacing) g_dev.last_vblank = rotk_vblank_wait(g_dev.last_vblank);
  double wait_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait0).count();
  auto now = std::chrono::steady_clock::now();
  double ms = std::chrono::duration<double, std::milli>(now - g_dev.last_swap).count();
  g_dev.last_swap = now;
  // A movie (XMV overlay frames, no Swap) between two Swaps is not a stall:
  // report it apart and keep it out of the Swap pacing statistics.
  uint64_t movie_frames = g_dev.overlay_frames - g_dev.overlay_at_swap;
  g_dev.overlay_at_swap = g_dev.overlay_frames;
  if (movie_frames) {
    rotk_log("d3d8: frame %llu: movie in between (%llu overlay frames, %.1f s)",
            (unsigned long long)g_dev.frame, (unsigned long long)movie_frames, ms / 1000);
  } else {
    double gpu_wait = gpu::TakeGpuWaitMs();
    if (ms > 25.0 && g_dev.frame > 3)
      rotk_log("d3d8: slow frame %llu: %.1f ms (%u draws, %.1f ms waiting for the GPU)",
              (unsigned long long)g_dev.frame, ms, g_dev.frame_draws, gpu_wait);
    // busy: the frame minus every vblank wait in it (Swap's and BlockUntilVerticalBlank's).
    double busy = std::max(0.0, ms - wait_ms - g_dev.block_wait_ms);
    static bool frame_log_open = [] {
      if (const char* v = getenv("ROTK_FRAME_LOG"); v && *v) {
        // A relative name goes to ROTK_LOG_DIR (the run dir of a test).
        std::string path = v;
        const char* dir = getenv("ROTK_LOG_DIR");
        if (path[0] != '/' && dir && *dir) path = std::string(dir) + "/" + path;
        g_dev.frame_log = std::fopen(path.c_str(), "w");
        if (g_dev.frame_log) std::fprintf(g_dev.frame_log, "# frame ms busy_ms gpu_wait_ms draws\n");
      }
      return true;
    }();
    (void)frame_log_open;
    if (g_dev.frame_log)
      std::fprintf(g_dev.frame_log, "%llu %.3f %.3f %.3f %u\n", (unsigned long long)g_dev.frame, ms,
                   busy, gpu_wait, g_dev.frame_draws);
    g_dev.frame_ms_sum += ms;
    g_dev.frame_busy_sum += busy;
    g_dev.frame_ms_max = std::max(g_dev.frame_ms_max, ms);
    g_dev.frame_ms_n++;
  }
  g_dev.block_wait_ms = 0;
  if (g_dev.frame_ms_n == 120) {
    if (g_dev.frame_log) std::fflush(g_dev.frame_log);
    // busy = frame time minus the vblank wait (the host's own cost per frame).
    rotk_log("d3d8: frame %llu: %.2f ms avg, %.2f ms max over 120 frames (busy %.2f ms); %u draws "
            "last frame, %zu textures, %zu vs, %zu targets",
            (unsigned long long)g_dev.frame, g_dev.frame_ms_sum / 120, g_dev.frame_ms_max,
            g_dev.frame_busy_sum / 120,
            g_dev.frame_draws, g_texcache.size(), g_vshaders.size(), g_targets.size());
    if (g_drop[0] + g_drop[1] + g_drop[2] + g_drop[3] + g_drop[4]) {
      rotk_log("d3d8: dropped draws (120 frames): no shader %llu, program %llu, target %llu, "
              "stream %llu, fragment %llu", (unsigned long long)g_drop[0], (unsigned long long)g_drop[1],
              (unsigned long long)g_drop[2], (unsigned long long)g_drop[3], (unsigned long long)g_drop[4]);
      std::fill(g_drop, g_drop + 5, 0);
    }
    g_dev.frame_ms_sum = g_dev.frame_ms_max = g_dev.frame_busy_sum = 0;
    g_dev.frame_ms_n = 0;
  }
  if (g_dev.frame <= 3 || g_cfg.trace) rotk_log("d3d8: Swap %llu (%u draws)", (unsigned long long)g_dev.frame, g_dev.frame_draws);
  g_dev.frame_draws = 0;
  if (g_cfg.exit_after && g_dev.frame >= g_cfg.exit_after) {
    gpu::EndFrame();
    JoinShotWriters();
    if (g_dev.frame_log) std::fflush(g_dev.frame_log);
    rotk_exit(0, "ROTK_EXIT_AFTER frames presented");
  }
  rotk_ret_stdcall(c, 0, 1);
}

// 0x12E850 CreateTexture2(Width, Height, Depth, Levels, Usage, Format, D3DResourceType)
void CreateTexture2(RotkCtx& c) {
  Seen("CreateTexture2");
  uint32_t w = rotk_arg(c, 0), h = rotk_arg(c, 1), depth = rotk_arg(c, 2), levels = rotk_arg(c, 3);
  uint32_t usage = rotk_arg(c, 4), xfmt = rotk_arg(c, 5), rtype = rotk_arg(c, 6);
  bool volume = rtype == 4, cube = rtype == 5;
  uint32_t fmt, size;
  MakeTextureHeader(w, h, depth, levels, xfmt, cube, volume, &fmt, &size);
  TexLayout L;
  uint32_t tex = 0;
  if (DescribeTexture(fmt, size, &L)) {
    uint32_t data = AllocContiguous(c, L.total, 0x80);
    if (data) {
      tex = NewHeader(0x14);
      rotk_wr32(tex, COMMON_TYPE_TEXTURE | COMMON_D3DCREATED | 1);
      rotk_wr32(tex + 4, data & 0x0FFFFFFF);
      rotk_wr32(tex + 8, 0);
      rotk_wr32(tex + 0xC, fmt);
      rotk_wr32(tex + 0x10, size);
    }
  }
  rotk_log("d3d8: CreateTexture %ux%ux%u lv %u usage %X %s type %u -> %08X (%u bytes)", w, h,
          depth, levels, usage, FormatInfo(xfmt).name ? FormatInfo(xfmt).name : "?", rtype, tex,
          L.total);
  rotk_ret_stdcall(c, tex, 7);
}

// 0x134FD0 Texture_GetSurfaceLevel2(pThis, Level) -> surface (AddRef'd)
void Texture_GetSurfaceLevel2(RotkCtx& c) {
  Seen("Texture_GetSurfaceLevel2");
  uint32_t tex = rotk_arg(c, 0), lv = rotk_arg(c, 1);
  Hdr T{tex};
  TexLayout L;
  uint32_t s = 0;
  if (DescribeTexture(T.format(), T.size(), &L) && lv < L.levels) {
    uint32_t w = std::max(1u, L.w >> lv), h = std::max(1u, L.h >> lv);
    uint32_t pitch = L.linear ? L.pitch : w * L.bpp / 8;
    // The game asks for render-target / reflection levels every frame: one
    // header per (texture, level), reused (the HLE region is a bump
    // allocator; a new header per call filled it in ~20 min). Texture headers
    // are never reused, so the key stays valid. Still held -> AddRef'd.
    static std::unordered_map<uint64_t, uint32_t> cache;
    static uint64_t calls = 0;
    if (++calls % 20000 == 0)
      rotk_log("d3d8: GetSurfaceLevel: %llu calls, %zu surface headers", (unsigned long long)calls,
              cache.size());
    uint32_t& slot = cache[uint64_t(tex) << 8 | lv];
    if (slot && (rotk_rd32(slot) & COMMON_REFCOUNT_MASK)) {
      AddRef(slot);
      rotk_ret_stdcall(c, slot, 2);
      return;
    }
    if (!slot) slot = NewHeader(0x20);
    s = slot;
    rotk_wr32(s, COMMON_TYPE_SURFACE | COMMON_D3DCREATED | 1);
    rotk_wr32(s + 4, T.data() + L.level_offset[lv]);
    rotk_wr32(s + 8, 0);
    uint32_t fmt = (T.format() & ~0xFFFF0000u & ~FMT_CUBEMAP) | 1u << 16;
    rotk_wr32(s + 0xC, fmt);
    rotk_wr32(s + 0x10, (w - 1) | (h - 1) << 12 | ((pitch + 63) / 64 - 1) << 24);
    rotk_wr32(s + 0x14, tex);
    AddRef(tex);
  }
  rotk_ret_stdcall(c, s, 2);
}

// 0x12E910 Texture_LockRect(pThis, Level, pLockedRect, pRect, Flags)
void Texture_LockRect(RotkCtx& c) {
  Seen("Texture_LockRect");
  uint32_t tex = rotk_arg(c, 0), lv = rotk_arg(c, 1), out = rotk_arg(c, 2), rect = rotk_arg(c, 3);
  Hdr T{tex};
  TexLayout L;
  uint32_t pitch = 0, bits = 0;
  if (DescribeTexture(T.format(), T.size(), &L) && lv < L.levels) {
    uint32_t w = std::max(1u, L.w >> lv);
    pitch = L.linear ? L.pitch : L.compressed ? ((w + 3) / 4) * (L.xfmt == XF_DXT1 ? 8 : 16)
                                              : w * L.bpp / 8;
    bits = CpuAddr(T.data() + L.level_offset[lv]);
    g_tex_locked[T.data()] = g_dev.frame;
    if (rect && L.linear) {
      bits += rotk_rd32(rect + 4) * pitch + rotk_rd32(rect) * (L.bpp / 8);
    }
  }
  rotk_wr32(out, pitch);
  rotk_wr32(out + 4, bits);
  rotk_ret_stdcall(c, 0, 5);
}

// 0x12A2A0 Clear(Count, pRects, Flags, Color, Z, Stencil)
void Clear(RotkCtx& c) {
  Seen("Clear");
  uint32_t n = rotk_arg(c, 0), rects = rotk_arg(c, 1), flags = rotk_arg(c, 2), color = rotk_arg(c, 3);
  float z = rotk_argf(c, 4);
  uint32_t st = rotk_arg(c, 5);
  TargetsNow tg = BindTargets();
  std::vector<int32_t> r;
  float kx = tg.w ? float(tg.pw) / tg.w : float(g_cfg.scale);
  for (uint32_t i = 0; i < n && rects; i++)
    for (int k = 0; k < 4; k++) {
      int32_t v = int32_t(rotk_rd32(rects + 16 * i + 4 * k));
      r.push_back(k & 1 ? v * int32_t(g_cfg.scale) : int32_t(v * kx + 0.5f));
    }
  gpu::Clear((flags & 0xF0) != 0, flags & 1, flags & 2, color, z, st, r.data(),
             uint32_t(r.size() / 4));
  TRACE("d3d8: Clear %u flags %X color %08X z %f", n, flags, color, z);
  (void)tg;
  rotk_ret_stdcall(c, 0, 6);
}

// 0x12FCF0 InsertCallback(Type, pCallback, Context): the GPU is always done here, so
// the callback runs right away (D3DCALLBACK: void __cdecl cb(DWORD Context)).
void InsertCallback(RotkCtx& c) {
  Seen("InsertCallback");
  uint32_t cb = rotk_arg(c, 1), ctx = rotk_arg(c, 2);
  if (cb) {
    uint32_t args[1] = {ctx};
    rotk_call_guest(c, cb, args, 1);
  }
  rotk_ret_stdcall(c, 0, 3);
}

// 0x135E60 DrawVerticesUP(PrimitiveType, VertexCount, pVertexStreamZeroData, Stride)
void DrawVerticesUP(RotkCtx& c) {
  Seen("DrawVerticesUP");
  uint32_t prim = rotk_arg(c, 0), n = rotk_arg(c, 1), data = rotk_arg(c, 2), stride = rotk_arg(c, 3);
  StreamSrc ss[16];
  ss[0].va = data;
  ss[0].stride = stride;
  DrawCommon(prim, ss, 0, n, nullptr, 0);
  rotk_ret_stdcall(c, 0, 4);
}

// 0x135FC0 DrawIndexedVerticesUP(PrimitiveType, VertexCount, pIndexData, pVertexStreamZeroData, Stride)
void DrawIndexedVerticesUP(RotkCtx& c) {
  Seen("DrawIndexedVerticesUP");
  uint32_t prim = rotk_arg(c, 0), n = rotk_arg(c, 1), idx = rotk_arg(c, 2), data = rotk_arg(c, 3),
           stride = rotk_arg(c, 4);
  StreamSrc ss[16];
  ss[0].va = data;
  ss[0].stride = stride;
  const uint16_t* ip = IndexPtr(idx, n);
  if (ip) DrawIndexed(prim, n, ip, ss, 0);
  rotk_ret_stdcall(c, 0, 5);
}

// 0x12E090 DrawVertices(PrimitiveType, StartVertex, VertexCount)
void DrawVertices(RotkCtx& c) {
  Seen("DrawVertices");
  uint32_t prim = rotk_arg(c, 0), start = rotk_arg(c, 1), n = rotk_arg(c, 2);
  DrawFromStreams(prim, start, n, nullptr, 0);
  rotk_ret_stdcall(c, 0, 3);
}

// 0x12E130 DrawIndexedVertices(PrimitiveType, VertexCount, pIndexData)
void DrawIndexedVertices(RotkCtx& c) {
  Seen("DrawIndexedVertices");
  uint32_t prim = rotk_arg(c, 0), n = rotk_arg(c, 1), idx = rotk_arg(c, 2);
  StreamSrc ss[16];
  for (int s = 0; s < 16; s++) {
    uint32_t vb = g_dev.streams[s].vb;
    if (!vb) continue;
    ss[s].va = CpuAddr(Hdr{vb}.data());
    ss[s].stride = g_dev.streams[s].stride;
  }
  const uint16_t* ip = IndexPtr(idx, n);
  if (ip) DrawIndexed(prim, n, ip, ss, g_dev.idx_base);
  rotk_ret_stdcall(c, 0, 3);
}

// 0x136520 UpdateOverlay(pSurface, SrcRect, DstRect, EnableColorKey, ColorKey): the XMV
// player's frames (a YUY2 surface in CPU memory). The NV2A scans the overlay
// out over the frame buffer; here it is drawn into the displayed buffer (the
// front buffer: the movie loop does not Swap), which then counts as a shown
// frame for screenshots. The colour key (UI over video) is not supported.

void UpdateOverlay(RotkCtx& c) {
  Seen("UpdateOverlay");
  auto t_in = std::chrono::steady_clock::now();
  if (g_ov.n) {
    double gap = std::chrono::duration<double, std::milli>(t_in - g_ov.last).count();
    g_ov.gap_ms += gap;
    g_ov.gap_max = std::max(g_ov.gap_max, gap);
  }
  uint32_t surf = rotk_arg(c, 0), sr = rotk_arg(c, 1), dr = rotk_arg(c, 2);
  Hdr H{surf};
  TexLayout L;
  uint32_t cpu = CpuAddr(H.data());
  if (surf && g_dev.front && DescribeTexture(H.format(), H.size(), &L) && rotk_is_committed(cpu) &&
      rotk_is_committed(cpu + L.total - 1)) {
    std::vector<uint8_t> buf;
    uint32_t pitch;
    ConvertLevel(L, 0, static_cast<const uint8_t*>(rotk_ptr(cpu)), &buf, &pitch);
    gpu::TexId t = gpu::CreateTexture(gpu::TexKind::BGRA8, L.w, L.h, 1, false, false);
    gpu::UploadTexture(t, 0, 0, L.w, L.h, buf.data(), pitch);
    auto rect = [](uint32_t r, int32_t def_w, int32_t def_h, int32_t* o) {
      if (r)
        for (int i = 0; i < 4; i++) o[i] = int32_t(rotk_rd32(r + 4 * i));
      else
        o[0] = o[1] = 0, o[2] = def_w, o[3] = def_h;
    };
    int32_t s4[4], d4[4];
    rect(sr, int32_t(L.w), int32_t(L.h), s4);
    rect(dr, int32_t(g_dev.bb_w), int32_t(g_dev.bb_h), d4);
    Target* f = GetTarget(Hdr{g_dev.front}.data(), g_dev.bb_w, g_dev.bb_h, false);
    int32_t k = int32_t(g_cfg.scale);
    // Movies keep their 4:3 shape: on a wide screen they are centred
    // (pillarboxed: clear the bars, the game does not draw there).
    int32_t pad = (int32_t(f->pw) - int32_t(f->w) * k) / 2;
    if (pad > 0) {
      gpu::SetTargets(f->tex, 0, f->pw, f->ph);
      gpu::Clear(true, false, false, 0xFF000000, 1, 0, nullptr, 0);
    }
    gpu::CopyRegion(t, s4[0], s4[1], s4[2] - s4[0], s4[3] - s4[1], f->tex, pad + d4[0] * k,
                    d4[1] * k, (d4[2] - d4[0]) * k, (d4[3] - d4[1]) * k);
    gpu::DestroyTexture(t);
    Present(g_dev.front);
    gpu::EndFrame();
    g_dev.overlay_frames++;
    if (g_cfg.shot_every && g_dev.overlay_frames % g_cfg.shot_every == 0)
      Screenshot(g_dev.front, "overlay", g_dev.overlay_frames);
    TRACE("d3d8: UpdateOverlay %08X %s %ux%u", surf, FormatInfo(L.xfmt).name, L.w, L.h);
  }
  g_ov.last = std::chrono::steady_clock::now();
  if (g_ov.movie_frames++ == 0)
    rotk_log("d3d8: movie: first overlay frame %.3f s after PlayMovie",
            std::chrono::duration<double>(g_ov.last - g_ov.movie_start).count());
  g_ov.in_ms += std::chrono::duration<double, std::milli>(g_ov.last - t_in).count();
  if (++g_ov.n % 300 == 0)
    rotk_log("d3d8: overlay %u frames: %.2f ms in UpdateOverlay, %.2f ms in vblank waits, "
            "%.2f ms between frames (max %.1f) on average",
            g_ov.n, g_ov.in_ms / 300, g_ov.wait_ms / 300, g_ov.gap_ms / 300, g_ov.gap_max),
        g_ov.in_ms = g_ov.wait_ms = g_ov.gap_ms = g_ov.gap_max = 0;
  rotk_ret_stdcall(c, 0, 5);
}

// 0x1366A0 EnableOverlay(Enable)
void EnableOverlay(RotkCtx& c) {
  Seen("EnableOverlay");
  rotk_ret_stdcall(c, 0, 1);
}

// ---- used by the XMV player ------------------------------------------------------------
// 0x12CF90 GetDisplayMode(pMode): D3DDISPLAYMODE {Width, Height, RefreshRate, Flags, Format}
void GetDisplayMode(RotkCtx& c) {
  Seen("GetDisplayMode");
  uint32_t p = rotk_arg(c, 0);
  rotk_wr32(p, g_dev.bb_w);
  rotk_wr32(p + 4, g_dev.bb_h);
  rotk_wr32(p + 8, 60);
  rotk_wr32(p + 12, 0);
  rotk_wr32(p + 16, XF_LIN_A8R8G8B8);
  rotk_ret_stdcall(c, 0, 1);
}

// 0x12D930 GetDisplayFieldStatus(pFieldStatus): {Field, VBlankCount}
void GetDisplayFieldStatus(RotkCtx& c) {
  Seen("GetDisplayFieldStatus");
  uint32_t p = rotk_arg(c, 0);
  rotk_wr32(p, 3 /* D3DFIELD_PROGRESSIVE */);
  rotk_wr32(p + 4, uint32_t(rotk_vblank_count()));
  rotk_ret_stdcall(c, 0, 1);
}

// 0x135C80 Surface_GetDesc(pThis, pDesc): D3DSURFACE_DESC {Format, Type, Usage, Size,
// MultiSampleType, Width, Height}
void Surface_GetDesc(RotkCtx& c) {
  Seen("Surface_GetDesc");
  uint32_t s = rotk_arg(c, 0), p = rotk_arg(c, 1);
  Hdr H{s};
  TexLayout L;
  DescribeTexture(H.format(), H.size(), &L);
  rotk_wr32(p, L.xfmt);
  rotk_wr32(p + 4, 1 /* D3DRTYPE_SURFACE */);
  rotk_wr32(p + 8, 0);
  rotk_wr32(p + 12, L.total);
  rotk_wr32(p + 16, 0x11);
  rotk_wr32(p + 20, L.w);
  rotk_wr32(p + 24, L.h);
  rotk_ret_stdcall(c, 0, 2);
}

// 0x135CA0 Surface_LockRect(pThis, pLockedRect, pRect, Flags)
void Surface_LockRect(RotkCtx& c) {
  Seen("Surface_LockRect");
  uint32_t s = rotk_arg(c, 0), out = rotk_arg(c, 1), rect = rotk_arg(c, 2);
  Hdr H{s};
  TexLayout L;
  DescribeTexture(H.format(), H.size(), &L);
  uint32_t pitch = L.linear ? L.pitch : L.w * L.bpp / 8;
  uint32_t bits = CpuAddr(H.data());
  g_tex_locked[H.data()] = g_dev.frame;
  if (uint32_t parent = rotk_rd32(s + 0x14)) g_tex_locked[Hdr{parent}.data()] = g_dev.frame;
  if (rect) bits += rotk_rd32(rect + 4) * pitch + rotk_rd32(rect) * (L.bpp / 8);
  rotk_wr32(out, pitch);
  rotk_wr32(out + 4, bits);
  rotk_ret_stdcall(c, 0, 4);
}

// ---- registration ------------------------------------------------------------------------
ROTK_OVERRIDE(0x133FA0, Direct3DCreate8);
ROTK_OVERRIDE(0xBEE00, FrameCounter);
ROTK_OVERRIDE(0x12F880, SetPushBufferSize);
ROTK_OVERRIDE(0x12F8A0, CreateDevice);
ROTK_OVERRIDE(0x12A6C0, Push1);
ROTK_OVERRIDE(0x12AA40, SetRenderState_FogColor);
ROTK_OVERRIDE(0x130B50, SetRenderState_CullMode);
ROTK_OVERRIDE(0x12AB40, SetRenderState_NormalizeNormals);
ROTK_OVERRIDE(0x12B040, SetTextureState_BumpEnv);
ROTK_OVERRIDE(0x12B0B0, SetTextureState_BorderColor);
ROTK_OVERRIDE(0x12B910, SetRenderState_ZEnable);
ROTK_OVERRIDE(0x12B9A0, SetRenderState_StencilEnable);
ROTK_OVERRIDE(0x12C290, CreateVertexShader);
ROTK_OVERRIDE(0x12C3D0, SetVertexShaderConstant1);
ROTK_OVERRIDE(0x12C430, SetVertexShaderConstant4);
ROTK_OVERRIDE(0x12C4E0, SetVertexShaderConstantNotInline);
ROTK_OVERRIDE(0x12C600, SetStreamSource);
ROTK_OVERRIDE(0x12C7F0, SetShaderConstantMode);
ROTK_OVERRIDE(0x12CA60, SetVertexShader);
ROTK_OVERRIDE(0x12C6D0, LoadVertexShader);
ROTK_OVERRIDE(0x12C730, SelectVertexShader);
ROTK_OVERRIDE(0x130350, CreateVertexBuffer2);
ROTK_OVERRIDE(0x1303A0, VertexBuffer_Lock2);
ROTK_OVERRIDE(0x12D110, SetRenderTarget);
ROTK_OVERRIDE(0x12CC60, SetTransform);
ROTK_OVERRIDE(0x132FD0, SetMaterial);
ROTK_OVERRIDE(0x130270, KickOffAndWaitForIdle);
ROTK_OVERRIDE(0x12CE30, BlockUntilVerticalBlank);
ROTK_OVERRIDE(0x12D390, GetBackBuffer2);
ROTK_OVERRIDE(0x133380, GetRenderTarget2);
ROTK_OVERRIDE(0x12D3E0, GetDepthStencilSurface2);
ROTK_OVERRIDE(0x12D630, SetTexture);
ROTK_OVERRIDE(0x12D870, SetIndices);
ROTK_OVERRIDE(0x130420, CreatePixelShader);
ROTK_OVERRIDE(0x1304D0, SetPixelShader);
ROTK_OVERRIDE(0x133CD0, SetPixelShaderConstant);
ROTK_OVERRIDE(0x12EAF0, Resource_Release);
ROTK_OVERRIDE(0x12EBD0, Resource_Register);
ROTK_OVERRIDE(0x134270, Resource_BlockUntilNotBusy);
ROTK_OVERRIDE(0x12F6D0, Swap);
ROTK_OVERRIDE(0x12D7E0, SetPalette);
ROTK_OVERRIDE(0x12E5B0, RunPushBuffer);
ROTK_OVERRIDE(0x12FD60, BeginPush);
ROTK_OVERRIDE(0x12FD80, EndPush);
ROTK_OVERRIDE(0x135A30, LazySetState);
ROTK_OVERRIDE(0x12CE10, SetVerticalBlankCallback);
ROTK_OVERRIDE(0x12FD90, KickOff);
ROTK_OVERRIDE(0x12FEB0, SetFence);
ROTK_OVERRIDE(0x12CDF0, BlockOnFence);
ROTK_OVERRIDE(0x12FF60, BlockOnTime);
ROTK_OVERRIDE(0x12D900, IsBusy);
ROTK_OVERRIDE(0x12E480, Begin);
ROTK_OVERRIDE(0x12E4C0, End);
ROTK_OVERRIDE(0x12D400, SetViewport);
ROTK_OVERRIDE(0x12CD80, GetViewport);
ROTK_OVERRIDE(0x130210, MakeSpace);
ROTK_OVERRIDE(0x1300D0, MakeRequestedSpace);
ROTK_OVERRIDE(0x12CBD0, SetGammaRamp);
ROTK_OVERRIDE(0x12E850, CreateTexture2);
ROTK_OVERRIDE(0x134FD0, Texture_GetSurfaceLevel2);
ROTK_OVERRIDE(0x12E910, Texture_LockRect);
ROTK_OVERRIDE(0x12A2A0, Clear);
ROTK_OVERRIDE(0x12FCF0, InsertCallback);
ROTK_OVERRIDE(0x135E60, DrawVerticesUP);
ROTK_OVERRIDE(0x135FC0, DrawIndexedVerticesUP);
ROTK_OVERRIDE(0x12E090, DrawVertices);
ROTK_OVERRIDE(0x12E130, DrawIndexedVertices);
ROTK_OVERRIDE(0x136520, UpdateOverlay);
ROTK_OVERRIDE(0x1366A0, EnableOverlay);
ROTK_OVERRIDE(0x12CF90, GetDisplayMode);
ROTK_OVERRIDE(0x12D930, GetDisplayFieldStatus);
ROTK_OVERRIDE(0x135C80, Surface_GetDesc);
ROTK_OVERRIDE(0x135CA0, Surface_LockRect);

}  // namespace

void rotk_d3d8_set_texture_hook(RotkTextureHook hook) { g_tex_hook = hook; }

// movies.cpp: a movie starts / ends (timing log for audio/video sync).
void rotk_d3d8_movie_event(bool start) {
  auto now = std::chrono::steady_clock::now();
  if (start) {
    g_ov.movie_start = now;
    g_ov.movie_frames = 0;
  } else {
    rotk_log("d3d8: movie: %u overlay frames, last at %.3f s, PlayMovie returned at %.3f s",
            g_ov.movie_frames, std::chrono::duration<double>(g_ov.last - g_ov.movie_start).count(),
            std::chrono::duration<double>(now - g_ov.movie_start).count());
  }
}
uint64_t rotk_d3d8_frame_count() { return g_dev.frame; }

// The presented frame's aspect (4:3, or wider in widescreen); usable before
// CreateDevice (the window is created first): Polish's call or ROTK_WIDESCREEN.
extern "C" float rotk_d3d8_output_aspect() {
  float wide = g_cfg.wide;
  if (wide <= 1.0f) {
    const char* v = getenv("ROTK_WIDESCREEN");
    if (v && atoi(v)) wide = (16.0f / 10.0f) / (4.0f / 3.0f);
  }
  return 4.0f / 3.0f * wide;
}

// Widescreen (Polish, src/polish/widescreen.cpp): call before CreateDevice.
extern "C" void rotk_d3d8_set_widescreen(float aspect_scale) {
  g_cfg.wide = aspect_scale > 1.0f ? aspect_scale : 1.0f;
  rotk_log("d3d8: widescreen, screen targets x%.3f wide", g_cfg.wide);
}
