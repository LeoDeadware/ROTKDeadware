// HD texture packs (Polish 5c): replace game textures by content hash with
// larger PNGs of the same aspect (the D3D8 HLE's texture hook; UVs are
// normalised, so a k-times larger image maps 1:1 and nothing shifts).
//
//   ROTK_HDUI=dir    the UI pack (menus, HUD, font atlases; tools/hdui). Default:
//                   $ROTK_MODS/hd_ui (the .app: built by tools/install.d/30-hd.sh),
//                   else $ROTK_DATA/mods/hd_ui, when it exists; 0 = off. Files
//                   <hash16>.png; <hash16> is the hook's key.
//   ROTK_HDWORLD=dir the world pack (the most visible level textures;
//                   tools/hdworld), the same rules with hd_world. A hash is
//                   looked up in the UI pack first.
//   ROTK_TEST_PROFILE=1  no default pack (the goldens are of the original game);
//                   a test that wants one names it with ROTK_HDUI / ROTK_HDWORLD.
//   ROTK_HDUI_DUMP=dir   one line per uploaded texture in dir/index.tsv
//                   ("hash w h format levels first-frame [nopng]") and
//                   <hash16>.png of level 0 when the format decodes (DXT,
//                   swizzled 32/16/8-bit): the input of tools/hdui/dump.sh.
//
// A pack image must be an integer multiple of the original size (same
// aspect); anything else is refused with a log line and the original kept.

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "hle/d3d8/d3d8.h"
#include "polish/texpack/prompts.h"
#include "polish/texpack/texpack.h"
#include "runtime/rotk_runtime.h"

// D3D8 HLE (Render). Weak: without it there is nothing to replace.
void rotk_d3d8_set_texture_hook(RotkTextureHook hook) __attribute__((weak));

namespace {

constexpr uint32_t kDxt1 = 0x0C, kDxt3 = 0x0E, kDxt5 = 0x0F;  // Xbox formats

struct Image {
  uint32_t w = 0, h = 0;
  std::vector<uint8_t> rgba;
};

struct Pack {
  std::string dir;
  const char* kind;  // "ui" / "world" (log lines, tests/scripts/hd_pack.sh counts them)
  size_t used = 0;
};
std::vector<Pack> g_packs;
std::string g_dump;
size_t g_loaded_bytes = 0;  // pack images uploaded (VRAM, +1/3 mips)
std::mutex g_mu;
std::map<uint64_t, Image> g_cache;  // probe images (kept: the hook returns pointers into them)
std::set<uint64_t> g_missing, g_dumped, g_used;

bool Exists(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0;
}

bool LoadPng(const std::string& path, Image* out) {
  @autoreleasepool {
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
    if (!src) return false;
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    if (!img) return false;
    out->w = uint32_t(CGImageGetWidth(img)), out->h = uint32_t(CGImageGetHeight(img));
    out->rgba.assign(size_t(out->w) * out->h * 4, 0);
    // Straight (non-premultiplied) alpha as the game expects: draw into a
    // premultiplied context, then divide back.
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(out->rgba.data(), out->w, out->h, 8, out->w * 4, cs,
                                             kCGImageAlphaPremultipliedLast | kCGBitmapByteOrderDefault);
    CGColorSpaceRelease(cs);
    if (!ctx) {
      CGImageRelease(img);
      return false;
    }
    CGContextSetBlendMode(ctx, kCGBlendModeCopy);
    CGContextDrawImage(ctx, CGRectMake(0, 0, out->w, out->h), img);
    CGContextRelease(ctx);
    CGImageRelease(img);
    for (size_t i = 0; i < out->rgba.size(); i += 4) {
      uint32_t a = out->rgba[i + 3];
      if (a && a < 255)
        for (int c = 0; c < 3; c++) out->rgba[i + c] = uint8_t(std::min(255u, (out->rgba[i + c] * 255 + a / 2) / a));
    }
    return true;
  }
}

bool SavePng(const std::string& path, uint32_t w, uint32_t h, const uint8_t* rgba) {
  @autoreleasepool {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef dp = CGDataProviderCreateWithData(nullptr, rgba, size_t(w) * h * 4, nullptr);
    CGImageRef img = CGImageCreate(w, h, 8, 32, w * 4, cs, kCGImageAlphaLast | kCGBitmapByteOrderDefault,
                                   dp, nullptr, false, kCGRenderingIntentDefault);
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
    bool ok = false;
    if (dst) {
      CGImageDestinationAddImage(dst, img, nullptr);
      ok = CGImageDestinationFinalize(dst);
      CFRelease(dst);
    }
    CGImageRelease(img);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
    return ok;
  }
}

// ---- BCn (DXT) level-0 decoding, for the dump
void Rgb565(uint16_t v, uint8_t* o) {
  o[0] = uint8_t(((v >> 11) & 31) * 255 / 31);
  o[1] = uint8_t(((v >> 5) & 63) * 255 / 63);
  o[2] = uint8_t((v & 31) * 255 / 31);
}

void ColorBlock(const uint8_t* b, bool dxt1, uint8_t pal[4][4]) {
  uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
  Rgb565(c0, pal[0]), Rgb565(c1, pal[1]);
  pal[0][3] = pal[1][3] = 255;
  for (int c = 0; c < 3; c++) {
    if (c0 > c1 || !dxt1) {
      pal[2][c] = uint8_t((2 * pal[0][c] + pal[1][c]) / 3);
      pal[3][c] = uint8_t((pal[0][c] + 2 * pal[1][c]) / 3);
    } else {
      pal[2][c] = uint8_t((pal[0][c] + pal[1][c]) / 2);
      pal[3][c] = 0;
    }
  }
  pal[2][3] = 255;
  pal[3][3] = (c0 > c1 || !dxt1) ? 255 : 0;
}

bool DecodeDxt(uint32_t fmt, uint32_t w, uint32_t h, const uint8_t* src, size_t size, Image* out) {
  uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bs = fmt == kDxt1 ? 8 : 16;
  if (size < size_t(bw) * bh * bs) return false;
  out->w = w, out->h = h;
  out->rgba.assign(size_t(w) * h * 4, 0);
  for (uint32_t by = 0; by < bh; by++)
    for (uint32_t bx = 0; bx < bw; bx++) {
      const uint8_t* b = src + (size_t(by) * bw + bx) * bs;
      const uint8_t* cb = fmt == kDxt1 ? b : b + 8;
      uint8_t pal[4][4];
      ColorBlock(cb, fmt == kDxt1, pal);
      uint8_t alpha[16];
      if (fmt == kDxt3) {
        for (int i = 0; i < 16; i++) alpha[i] = uint8_t(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
      } else if (fmt == kDxt5) {
        uint8_t a[8] = {b[0], b[1]};
        for (int i = 2; i < 8; i++)
          a[i] = b[0] > b[1] ? uint8_t(((8 - i) * b[0] + (i - 1) * b[1]) / 7)
                 : i < 6     ? uint8_t(((6 - i) * b[0] + (i - 1) * b[1]) / 5)
                             : uint8_t(i == 6 ? 0 : 255);
        uint64_t bits = 0;
        for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (8 * i);
        for (int i = 0; i < 16; i++) alpha[i] = a[(bits >> (3 * i)) & 7];
      }
      uint32_t idx = uint32_t(cb[4] | cb[5] << 8 | cb[6] << 16 | uint32_t(cb[7]) << 24);
      for (int i = 0; i < 16; i++) {
        uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
        if (x >= w || y >= h) continue;
        uint8_t* o = &out->rgba[(size_t(y) * w + x) * 4];
        const uint8_t* c = pal[(idx >> (2 * i)) & 3];
        o[0] = c[0], o[1] = c[1], o[2] = c[2];
        o[3] = fmt == kDxt1 ? c[3] : alpha[i];
      }
    }
  return true;
}

// Swizzled uncompressed formats (power-of-two sizes): texel (x, y) at the
// interleaved bits of x and y (x first), the larger dimension's extra bits on
// top. 32-bit A8R8G8B8 / X8R8G8B8, 16-bit A1R5G5B5 / X1R5G5B5 / A4R4G4B4 /
// R5G6B5 / A8L8, 8-bit L8 / AL8 / A8 (fonts and HUD art use the 16/8-bit ones).
uint32_t SwizzledBytes(uint32_t fmt) {
  switch (fmt) {
    case 0x06: case 0x07: return 4;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x1A: return 2;
    case 0x00: case 0x01: case 0x19: case 0x0B: return 1;
    default: return 0;
  }
}

void TexelRgba(uint32_t fmt, const uint8_t* t, uint8_t* o) {
  auto x5 = [](uint32_t v) { return uint8_t(v * 255 / 31); };
  auto x6 = [](uint32_t v) { return uint8_t(v * 255 / 63); };
  const uint32_t v = SwizzledBytes(fmt) >= 2 ? uint32_t(t[0]) | uint32_t(t[1]) << 8 : t[0];
  switch (fmt) {
    case 0x06: case 0x07: o[0] = t[2], o[1] = t[1], o[2] = t[0], o[3] = fmt == 0x07 ? 255 : t[3]; break;
    case 0x02: case 0x03:
      o[0] = x5((v >> 10) & 31), o[1] = x5((v >> 5) & 31), o[2] = x5(v & 31);
      o[3] = fmt == 0x03 || (v & 0x8000) ? 255 : 0;
      break;
    case 0x04:
      o[0] = uint8_t(((v >> 8) & 15) * 17), o[1] = uint8_t(((v >> 4) & 15) * 17), o[2] = uint8_t((v & 15) * 17);
      o[3] = uint8_t((v >> 12) * 17);
      break;
    case 0x05: o[0] = x5((v >> 11) & 31), o[1] = x6((v >> 5) & 63), o[2] = x5(v & 31), o[3] = 255; break;
    case 0x1A: o[0] = o[1] = o[2] = uint8_t(v & 255), o[3] = uint8_t(v >> 8); break;
    case 0x00: o[0] = o[1] = o[2] = t[0], o[3] = 255; break;
    case 0x01: o[0] = o[1] = o[2] = o[3] = t[0]; break;
    case 0x19: o[0] = o[1] = o[2] = 255, o[3] = t[0]; break;
    case 0x0B: o[0] = o[1] = o[2] = t[0], o[3] = 255; break;  // P8: the index (Decode maps it)
  }
}

bool DecodeSwizzled(uint32_t fmt, uint32_t w, uint32_t h, const uint8_t* src, size_t size, Image* out) {
  const uint32_t bpp = SwizzledBytes(fmt);
  if (!bpp || !w || !h || (w & (w - 1)) || (h & (h - 1)) || size < size_t(w) * h * bpp) return false;
  out->w = w, out->h = h;
  out->rgba.assign(size_t(w) * h * 4, 0);
  for (uint32_t y = 0; y < h; y++)
    for (uint32_t x = 0; x < w; x++) {
      uint32_t off = 0, bit = 0;
      for (uint32_t m = 1; m < w || m < h; m <<= 1) {
        if (m < w) off |= (x & m ? 1u : 0u) << bit++;
        if (m < h) off |= (y & m ? 1u : 0u) << bit++;
      }
      TexelRgba(fmt, src + size_t(off) * bpp, &out->rgba[(size_t(y) * w + x) * 4]);
    }
  return true;
}

bool Decode(const RotkTextureInfo& info, Image* out) {
  const uint32_t f = info.xbox_format;
  if (!info.data) return false;
  if (f == kDxt1 || f == kDxt3 || f == kDxt5) return DecodeDxt(f, info.width, info.height, info.data, info.data_size, out);
  if (!DecodeSwizzled(f, info.width, info.height, info.data, info.data_size, out)) return false;
  if (f == 0x0B) {
    // P8: the palette is not in the hook's info. ROTK's P8 textures are its font atlases with
    // 4-bit indices into a white alpha ramp (index i = white, alpha i * 17; the D3D8 HLE's
    // ROTK_TEX_DUMP shows them all white): decoded that way; other P8 textures are skipped.
    for (size_t i = 0; i < out->rgba.size(); i += 4)
      if (out->rgba[i] > 15) return false;
    for (size_t i = 0; i < out->rgba.size(); i += 4) {
      const uint8_t a = uint8_t(out->rgba[i] * 17);
      out->rgba[i] = out->rgba[i + 1] = out->rgba[i + 2] = 255, out->rgba[i + 3] = a;
    }
  }
  return true;
}

// Every upload gets an index line (once per hash): hash w h format levels
// first-frame [nopng]; a PNG when the format decodes (DXT, swizzled
// 32/16/8-bit). The first frame tells front-end (UI) uploads from a level's.
void Dump(const RotkTextureInfo& info) {
  if (!g_dumped.insert(info.hash).second) return;
  Image img;
  char name[32];
  std::snprintf(name, sizeof name, "%016llx", (unsigned long long)info.hash);
  const bool ok = Decode(info, &img);
  if (ok) SavePng(g_dump + "/" + name + ".png", img.w, img.h, img.rgba.data());
  if (FILE* f = std::fopen((g_dump + "/index.tsv").c_str(), "a")) {
    std::fprintf(f, "%s\t%u\t%u\t%02X\t%u\t%llu%s\n", name, info.width, info.height, info.xbox_format, info.levels,
                 (unsigned long long)rotk_d3d8_frame_count(), ok ? "" : "\tnopng");
    std::fclose(f);
  }
}

// ROTK_HDUI_PROBE=1 (dev): every texture becomes a flat colour that encodes
// its number n as code c = 1 + (n - 1) % 1023: R = 4 + 8 * (c & 31),
// G = 4 + 8 * (c >> 5), B = 255 (a grey vertex colour scales all three, so
// c = (255 * R / B) / 8 + 32 * ((255 * G / B) / 8); bins of 8 tolerate light
// tints), alpha the original's, logged with its hash: one pixel of a
// screenshot names the texture drawn there (tools/hdworld/coverage.py; with
// ROTK_DEV_MAGENTA=0-999999 ROTK_DEV_SHOW=t0 every draw shows its texture
// unlit, so the codes come out exact).
bool g_probe = false;

bool Probe(const RotkTextureInfo& info, uint32_t* w2, uint32_t* h2, const uint8_t** rgba) {
  static uint32_t n = 0;
  auto it = g_cache.find(info.hash);
  if (it == g_cache.end()) {
    n++;
    uint32_t c = 1 + (n - 1) % 1023;
    Image img;
    img.w = info.width, img.h = info.height;
    img.rgba.resize(size_t(img.w) * img.h * 4);
    // The original's alpha where it can be decoded (cut-out foliage, fences
    // keep their shape); else opaque.
    Image orig;
    bool has_a = Decode(info, &orig) && orig.rgba.size() == img.rgba.size();
    for (size_t i = 0; i < img.rgba.size(); i += 4)
      img.rgba[i] = uint8_t(4 + 8 * (c & 31)), img.rgba[i + 1] = uint8_t(4 + 8 * (c >> 5)), img.rgba[i + 2] = 255,
      img.rgba[i + 3] = has_a ? orig.rgba[i + 3] : 255;
    rotk_log("polish: hd_ui probe %u code %u = %016llx %ux%u fmt %02X levels %u", n, c,
            (unsigned long long)info.hash, info.width, info.height, info.xbox_format, info.levels);
    it = g_cache.emplace(info.hash, std::move(img)).first;
  }
  *w2 = it->second.w, *h2 = it->second.h;
  *rgba = it->second.rgba.data();
  return true;
}

bool Hook(const RotkTextureInfo& info, uint32_t* w2, uint32_t* h2, const uint8_t** rgba) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_dump.empty()) Dump(info);
  if (g_probe) return Probe(info, w2, h2, rgba);
  if (rotk_polish_prompt_glyph(info, w2, h2, rgba)) return true;
  if (g_packs.empty() || g_missing.count(info.hash)) return false;
  // Pack images are not kept: the D3D8 HLE copies the image to the GPU during
  // the call, and a texture made again (rare) loads its PNG again. RAM stays
  // flat, the pack costs VRAM only.
  char name[32];
  std::snprintf(name, sizeof name, "%016llx.png", (unsigned long long)info.hash);
  std::string path;
  Pack* pack = nullptr;
  for (Pack& p : g_packs)
    if (Exists(p.dir + "/" + name)) {
      path = p.dir + "/" + name, pack = &p;
      break;
    }
  static Image img;
  if (path.empty() || !LoadPng(path, &img)) {
    g_missing.insert(info.hash);
    return false;
  }
  uint32_t k = info.width ? img.w / info.width : 0;
  if (!k || img.w != k * info.width || img.h != k * info.height) {
    rotk_log("polish: hd_ui %s is %ux%u, not a multiple of %ux%u: ignored", name, img.w, img.h, info.width,
            info.height);
    g_missing.insert(info.hash);
    return false;
  }
  if (g_used.insert(info.hash).second) {
    pack->used++;
    // One line per texture's first replacement (a few hundred per session).
    rotk_log("polish: hd_%s %s %ux%u -> %ux%u", pack->kind, name, info.width, info.height, img.w, img.h);
    g_loaded_bytes += img.rgba.size();
    if (g_used.size() % 25 == 0) {
      std::string per;
      for (const Pack& p : g_packs) per += std::string(per.empty() ? "" : ", ") + p.kind + " " + std::to_string(p.used);
      rotk_log("polish: hd pack: %zu textures replaced so far (%s), %.1f MB uploaded (VRAM ~%.1f MB with mips)",
              g_used.size(), per.c_str(), g_loaded_bytes / 1048576.0, g_loaded_bytes * 4 / 3 / 1048576.0);
    }
  }
  *w2 = img.w, *h2 = img.h;
  *rgba = img.rgba.data();
  return true;
}

// The test profile (ROTK_TEST_PROFILE=1, RULES R-T4) keeps the original
// textures unless ROTK_HDUI / ROTK_HDWORLD names a pack: the goldens are of
// the original game. (Tools that dump the originals pass ROTK_HDUI=0.)
bool TestProfile() {
  const char* tp = getenv("ROTK_TEST_PROFILE");
  return tp && *tp && *tp != '0';
}

}  // namespace

void rotk_polish_texpack_init() {
  if (!rotk_d3d8_set_texture_hook) return;
  // Default packs: $ROTK_MODS/<name> (the .app: Contents/Resources/mods, built
  // by tools/install.d/30-hd.sh), else $ROTK_DATA/mods/<name>.
  const struct {
    const char* var;
    const char* sub;
    const char* kind;
  } kPacks[] = {{"ROTK_HDUI", "hd_ui", "ui"}, {"ROTK_HDWORLD", "hd_world", "world"}};
  for (const auto& k : kPacks) {
    const char* v = getenv(k.var);
    if (v && *v) {
      if (std::strcmp(v, "0")) g_packs.push_back({v, k.kind});
      continue;
    }
    if (TestProfile()) continue;
    for (auto [var, sub] : {std::pair{"ROTK_MODS", "/"}, std::pair{"ROTK_DATA", "/mods/"}}) {
      const char* root = getenv(var);
      if (!root || !*root) continue;
      std::string p = std::string(root) + sub + k.sub;
      if (Exists(p)) {
        g_packs.push_back({p, k.kind});
        break;
      }
    }
  }
  if (const char* d = getenv("ROTK_HDUI_DUMP"); d && *d) {
    g_dump = d;
    mkdir(d, 0755);
  }
  if (const char* p = getenv("ROTK_HDUI_PROBE"); p && *p && *p != '0') {
    g_probe = true;
    rotk_log("polish: hd_ui probe: every texture a flat colour = its number (dev)");
  }
  const bool prompts = rotk_polish_prompts_init();
  if (g_packs.empty() && g_dump.empty() && !g_probe && !prompts) return;
  for (const Pack& p : g_packs) {
    rotk_log("polish: hd_%s pack %s", p.kind, p.dir.c_str());
    // tools/app_smoke.sh checks "mods: loaded <name>" for each name in tools/install.d/expect.txt
    rotk_log("mods: loaded %s", p.dir.substr(p.dir.find_last_of('/') + 1).c_str());
  }
  if (!g_dump.empty()) rotk_log("polish: hd_ui dump of textures to %s", g_dump.c_str());
  rotk_d3d8_set_texture_hook(Hook);
}
