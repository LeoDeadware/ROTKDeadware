// Button prompts for the Xbox Series controller (Polish, 5b): the game's
// on-screen button glyphs are separate 64x64 / 32x32 DXT3 textures (one per
// button, GlobScen / level data; hashes from a ROTK_HDUI_DUMP of Hel01 and the
// menus). The glyphs of buttons the Series pad does not have are redrawn here,
// procedurally (CoreGraphics; nothing derived from game data is stored):
//   Black -> RB, White -> LB (xinput_sdl.cpp: Black = right shoulder, White =
//   left shoulder), the L / R trigger keys -> LT / RT, the L / R stick
//   badges -> LS / RS, Back -> View, Start -> Menu.
// A, B, X, Y stay the game's own (green, red, blue, yellow: the Series colours).
// The new glyph keeps the original's place: it is drawn inside the bounding
// box of the original's alpha (the game samples only part of some of these
// textures), at 4x the original's size (the texture hook allows integer
// multiples; crisp at ROTK_RENDER_SCALE 4).
//
// PlayStation pads (addendum b, 2026-11-06): with a DualShock / DualSense connected (SDL pad type,
// rotk_xinput_pad_family) the glyphs become PlayStation ones: A -> cross, X -> square,
// Y -> triangle (B -> circle once its hash is known), Black -> R1, L trigger -> L2, L stick -> L3.
//   ROTK_PROMPTS=auto     (default) PlayStation glyphs for a PlayStation pad, else Series;
//   ROTK_PROMPTS=series   the above; =ps: PlayStation; =xbox or 0: the original glyphs.
//   ROTK_PROMPTS_DUMP=dir  also write every replacement as <hash>.png (review;
//                         tools/hdui/prompts_sheet.sh).
// On under ROTK_TEST_PROFILE too: no golden contains a prompt (tests/golden),
// and tests/scripts/gamepad_prompts.sh checks it there.

#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "hle/d3d8/d3d8.h"
#include "hle/xinput/xinput.h"
#include "polish/texpack/prompts.h"
#include "runtime/rotk_runtime.h"

namespace {

enum class Shape { kBumper, kTrigger, kStick, kView, kMenu, kFace };

struct Glyph {
  uint64_t hash;
  const char* original;  // the Xbox button, for logs
  Shape shape;
  const char* label;     // drawn text (kBumper / kTrigger / kStick)
};

// Hashes = the D3D8 HLE texture hook's content hash of the Xbox data.
const Glyph kGlyphs[] = {
    {0xa4a43cfe5000c698ull, "Black", Shape::kBumper, "RB"},  // the black disc of "Action" (owner test 3667d6a)
    {0x8004f7870a39cffcull, "Black", Shape::kBumper, "RB"},
    {0x404325db171c074cull, "L trigger", Shape::kTrigger, "LT"},
    {0xa3b3a5d16faa0d86ull, "L stick", Shape::kStick, "LS"},
};

// PlayStation (kFace labels name the symbol: cross / circle / square / triangle).
const Glyph kPsGlyphs[] = {
    {0x085495bbf8e71158ull, "A", Shape::kFace, "cross"},
    {0x6bd0eb93ff86bd04ull, "X", Shape::kFace, "square"},
    {0xf1a8fae2240de667ull, "X", Shape::kFace, "square"},
    {0x8a728e255529d9d9ull, "Y", Shape::kFace, "triangle"},
    {0xa4a43cfe5000c698ull, "Black", Shape::kBumper, "R1"},
    {0x8004f7870a39cffcull, "Black", Shape::kBumper, "R1"},
    {0x404325db171c074cull, "L trigger", Shape::kTrigger, "L2"},
    {0xa3b3a5d16faa0d86ull, "L stick", Shape::kStick, "L3"},
};

constexpr int kScale = 4;

bool g_on = false;
std::string g_mode;   // series / ps / auto
int g_family = -1;    // glyph family the made glyphs use: 0 Series, 1 PlayStation
std::string g_dump;
std::map<uint64_t, std::vector<uint8_t>> g_made;  // kept: the hook returns pointers into them

const Glyph* Find(uint64_t h, bool ps) {
  if (ps) {
    for (const Glyph& g : kPsGlyphs)
      if (g.hash == h) return &g;
    return nullptr;
  }
  for (const Glyph& g : kGlyphs)
    if (g.hash == h) return &g;
  return nullptr;
}


// DXT3/DXT5/DXT1 alpha of level 0 -> bounding box of alpha >= 32 (pixels).
bool AlphaBox(const RotkTextureInfo& info, int* x0, int* y0, int* x1, int* y1) {
  const uint32_t w = info.width, h = info.height, f = info.xbox_format;
  if (!info.data || (f != 0x0C && f != 0x0E && f != 0x0F)) return false;
  const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bs = f == 0x0C ? 8 : 16;
  if (info.data_size < size_t(bw) * bh * bs) return false;
  int ax0 = int(w), ay0 = int(h), ax1 = -1, ay1 = -1;
  for (uint32_t by = 0; by < bh; by++)
    for (uint32_t bx = 0; bx < bw; bx++) {
      const uint8_t* b = info.data + (size_t(by) * bw + bx) * bs;
      uint8_t a[16];
      if (f == 0x0E) {
        for (int i = 0; i < 16; i++) a[i] = uint8_t(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
      } else if (f == 0x0F) {
        uint8_t p[8] = {b[0], b[1]};
        for (int i = 2; i < 8; i++)
          p[i] = b[0] > b[1] ? uint8_t(((8 - i) * b[0] + (i - 1) * b[1]) / 7)
                 : i < 6     ? uint8_t(((6 - i) * b[0] + (i - 1) * b[1]) / 5)
                             : uint8_t(i == 6 ? 0 : 255);
        uint64_t bits = 0;
        for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (8 * i);
        for (int i = 0; i < 16; i++) a[i] = p[(bits >> (3 * i)) & 7];
      } else {
        uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
        uint32_t idx = uint32_t(b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24);
        for (int i = 0; i < 16; i++) a[i] = (c0 <= c1 && ((idx >> (2 * i)) & 3) == 3) ? 0 : 255;
      }
      for (int i = 0; i < 16; i++) {
        int x = int(bx * 4 + (i & 3)), y = int(by * 4 + (i >> 2));
        if (a[i] < 32 || x >= int(w) || y >= int(h)) continue;
        ax0 = std::min(ax0, x), ay0 = std::min(ay0, y), ax1 = std::max(ax1, x), ay1 = std::max(ay1, y);
      }
    }
  if (ax1 < 0) return false;
  *x0 = ax0, *y0 = ay0, *x1 = ax1 + 1, *y1 = ay1 + 1;
  return true;
}

CGColorRef Rgb(CGFloat r, CGFloat g, CGFloat b, CGFloat a = 1) {
  CGFloat c[4] = {r, g, b, a};
  static CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  return CGColorCreate(cs, c);
}

void FillPath(CGContextRef ctx, CGPathRef p, CGColorRef c) {
  CGContextAddPath(ctx, p);
  CGContextSetFillColorWithColor(ctx, c);
  CGContextFillPath(ctx);
  CGColorRelease(c);
}

// A dark, glossy key in the style of the game's own glyphs (rim, body with a
// vertical gradient, soft top highlight, drop shadow).
void Key(CGContextRef ctx, CGPathRef outer, CGRect r, CGFloat rim) {
  CGContextSaveGState(ctx);
  CGColorRef sh = Rgb(0, 0, 0, 0.55);
  CGContextSetShadowWithColor(ctx, CGSizeMake(rim * 0.6, -rim * 0.6), rim * 0.8, sh);
  CGColorRelease(sh);
  FillPath(ctx, outer, Rgb(0.42, 0.42, 0.44));  // rim
  CGContextRestoreGState(ctx);
  CGContextSaveGState(ctx);
  CGContextAddPath(ctx, outer);
  CGContextClip(ctx);
  static CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGFloat stops[] = {0.30, 0.30, 0.32, 1, 0.08, 0.08, 0.09, 1};
  CGFloat loc[] = {0, 1};
  CGGradientRef gr = CGGradientCreateWithColorComponents(cs, stops, loc, 2);
  CGRect in = CGRectInset(r, rim, rim);
  // the body: the outer shape shrunk by the rim
  CGAffineTransform t = CGAffineTransformMakeTranslation(CGRectGetMidX(r), CGRectGetMidY(r));
  t = CGAffineTransformScale(t, in.size.width / r.size.width, in.size.height / r.size.height);
  t = CGAffineTransformTranslate(t, -CGRectGetMidX(r), -CGRectGetMidY(r));
  CGPathRef body = CGPathCreateCopyByTransformingPath(outer, &t);
  CGContextAddPath(ctx, body);
  CGPathRelease(body);
  CGContextClip(ctx);
  CGContextDrawLinearGradient(ctx, gr, CGPointMake(0, CGRectGetMaxY(in)), CGPointMake(0, CGRectGetMinY(in)), 0);
  CGGradientRelease(gr);
  // highlight on the upper part
  CGRect hl = CGRectMake(in.origin.x + in.size.width * 0.12, in.origin.y + in.size.height * 0.55,
                         in.size.width * 0.76, in.size.height * 0.38);
  CGPathRef hp = CGPathCreateWithRoundedRect(hl, hl.size.height * 0.45, hl.size.height * 0.45, nullptr);
  FillPath(ctx, hp, Rgb(1, 1, 1, 0.10));
  CGPathRelease(hp);
  CGContextRestoreGState(ctx);
}

// A PlayStation face button: dark round key with the coloured symbol.
void Face(CGContextRef ctx, CGRect sq, const char* sym) {
  CGPathRef p = CGPathCreateWithEllipseInRect(sq, nullptr);
  Key(ctx, p, sq, sq.size.width * 0.05);
  CGPathRelease(p);
  CGFloat cx = CGRectGetMidX(sq), cy = CGRectGetMidY(sq), r = sq.size.width * 0.24;
  CGFloat c[3] = {0.49, 0.70, 0.93};                                       // cross: blue
  if (!strcmp(sym, "circle")) c[0] = 1.00, c[1] = 0.40, c[2] = 0.42;       // red
  if (!strcmp(sym, "square")) c[0] = 0.88, c[1] = 0.56, c[2] = 0.82;       // pink
  if (!strcmp(sym, "triangle")) c[0] = 0.25, c[1] = 0.86, c[2] = 0.70;     // green
  CGColorRef col = Rgb(c[0], c[1], c[2]);
  CGContextSetStrokeColorWithColor(ctx, col);
  CGColorRelease(col);
  CGContextSetLineWidth(ctx, sq.size.width * 0.075);
  CGContextSetLineCap(ctx, kCGLineCapRound);
  CGContextSetLineJoin(ctx, kCGLineJoinRound);
  if (!strcmp(sym, "cross")) {
    CGContextMoveToPoint(ctx, cx - r, cy - r), CGContextAddLineToPoint(ctx, cx + r, cy + r);
    CGContextMoveToPoint(ctx, cx - r, cy + r), CGContextAddLineToPoint(ctx, cx + r, cy - r);
  } else if (!strcmp(sym, "circle")) {
    CGContextAddEllipseInRect(ctx, CGRectMake(cx - r, cy - r, 2 * r, 2 * r));
  } else if (!strcmp(sym, "square")) {
    CGContextAddRect(ctx, CGRectMake(cx - r * 0.85, cy - r * 0.85, 1.7 * r, 1.7 * r));
  } else {  // triangle, point up (CG is y up)
    CGContextMoveToPoint(ctx, cx, cy + r);
    CGContextAddLineToPoint(ctx, cx + r * 1.0, cy - r * 0.75);
    CGContextAddLineToPoint(ctx, cx - r * 1.0, cy - r * 0.75);
    CGContextClosePath(ctx);
  }
  CGContextStrokePath(ctx);
}

void Text(CGContextRef ctx, const char* s, CGRect box, CGFloat max_h) {
  NSString* str = [NSString stringWithUTF8String:s];
  CGFloat size = max_h;
  for (int it = 0; it < 2; it++) {
    CTFontRef font = CTFontCreateWithName(CFSTR("Helvetica-Bold"), size, nullptr);
    NSDictionary* attr = @{(__bridge id)kCTFontAttributeName : (__bridge id)font,
                           (__bridge id)kCTForegroundColorFromContextAttributeName : @YES};
    CFAttributedStringRef as = (__bridge_retained CFAttributedStringRef)[[NSAttributedString alloc]
        initWithString:str attributes:attr];
    CTLineRef line = CTLineCreateWithAttributedString(as);
    CGRect b = CTLineGetImageBounds(line, ctx);
    if (it == 0 && b.size.width > box.size.width * 0.80) {
      size *= box.size.width * 0.80 / b.size.width;
    } else {
      CGContextSetFillColorWithColor(ctx, CGColorGetConstantColor(kCGColorWhite));
      CGContextSetTextPosition(ctx, CGRectGetMidX(box) - b.size.width / 2 - b.origin.x,
                               CGRectGetMidY(box) - b.size.height / 2 - b.origin.y);
      CTLineDraw(line, ctx);
      it = 2;
    }
    CFRelease(line);
    CFRelease(as);
    CFRelease(font);
  }
}

// Draws glyph g into an RGBA (straight alpha) image of w x h, inside box
// (pixels of that image, y down).
std::vector<uint8_t> Draw(const Glyph& g, uint32_t w, uint32_t h, CGRect box_ydown) {
  std::vector<uint8_t> px(size_t(w) * h * 4, 0);
  @autoreleasepool {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(px.data(), w, h, 8, w * 4, cs,
                                             kCGImageAlphaPremultipliedLast | kCGBitmapByteOrderDefault);
    CGColorSpaceRelease(cs);
    // CG is y up: flip the box.
    CGRect box = CGRectMake(box_ydown.origin.x, h - CGRectGetMaxY(box_ydown), box_ydown.size.width,
                            box_ydown.size.height);
    // leave room for the shadow, like the originals
    CGFloat side = std::min(box.size.width, box.size.height);
    CGRect sq = CGRectMake(CGRectGetMidX(box) - side / 2, CGRectGetMidY(box) - side / 2, side, side);
    sq = CGRectInset(sq, side * 0.04, side * 0.04);
    CGFloat rim = side * 0.05;
    CGContextSetShouldAntialias(ctx, true);
    switch (g.shape) {
      case Shape::kFace:
        Face(ctx, sq, g.label);
        break;
      case Shape::kBumper: {  // a wide pill, the shoulder button seen from above
        CGRect r = CGRectMake(sq.origin.x, sq.origin.y + sq.size.height * 0.18, sq.size.width, sq.size.height * 0.64);
        CGPathRef p = CGPathCreateWithRoundedRect(r, r.size.height * 0.42, r.size.height * 0.42, nullptr);
        Key(ctx, p, r, rim);
        CGPathRelease(p);
        Text(ctx, g.label, r, r.size.height * 0.62);
        break;
      }
      case Shape::kTrigger: {  // a key, slightly taller than wide, rounded top
        CGRect r = CGRectInset(sq, sq.size.width * 0.08, 0);
        CGPathRef p = CGPathCreateWithRoundedRect(r, r.size.width * 0.22, r.size.width * 0.22, nullptr);
        Key(ctx, p, r, rim);
        CGPathRelease(p);
        Text(ctx, g.label, r, r.size.height * 0.46);
        break;
      }
      case Shape::kStick:
      case Shape::kView:
      case Shape::kMenu: {
        CGPathRef p = CGPathCreateWithEllipseInRect(sq, nullptr);
        Key(ctx, p, sq, rim);
        CGPathRelease(p);
        if (g.shape == Shape::kStick) {
          Text(ctx, g.label, sq, sq.size.height * 0.42);
        } else {
          CGContextSetStrokeColorWithColor(ctx, CGColorGetConstantColor(kCGColorWhite));
          CGContextSetFillColorWithColor(ctx, CGColorGetConstantColor(kCGColorWhite));
          CGFloat u = sq.size.width / 16, cx = CGRectGetMidX(sq), cy = CGRectGetMidY(sq);
          CGContextSetLineWidth(ctx, u * 0.9);
          if (g.shape == Shape::kMenu) {  // three lines
            for (int i = -1; i <= 1; i++)
              CGContextFillRect(ctx, CGRectMake(cx - 3.2 * u, cy + i * 2.2 * u - 0.5 * u, 6.4 * u, 1.0 * u));
          } else {  // View: two overlapping windows
            CGContextStrokeRect(ctx, CGRectMake(cx - 3.4 * u, cy - 0.6 * u, 4.6 * u, 3.6 * u));
            CGContextStrokeRect(ctx, CGRectMake(cx - 1.2 * u, cy - 3.0 * u, 4.6 * u, 3.6 * u));
          }
        }
        break;
      }
    }
    CGContextRelease(ctx);
  }
  // premultiplied -> straight
  for (size_t i = 0; i < px.size(); i += 4) {
    uint32_t a = px[i + 3];
    if (a && a < 255)
      for (int c = 0; c < 3; c++) px[i + c] = uint8_t(std::min(255u, (px[i + c] * 255 + a / 2) / a));
  }
  return px;
}

void Save(const std::string& path, uint32_t w, uint32_t h, const uint8_t* rgba) {
  @autoreleasepool {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef dp = CGDataProviderCreateWithData(nullptr, rgba, size_t(w) * h * 4, nullptr);
    CGImageRef img = CGImageCreate(w, h, 8, 32, w * 4, cs, kCGImageAlphaLast | kCGBitmapByteOrderDefault, dp,
                                   nullptr, false, kCGRenderingIntentDefault);
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
    if (dst) {
      CGImageDestinationAddImage(dst, img, nullptr);
      CGImageDestinationFinalize(dst);
      CFRelease(dst);
    }
    CGImageRelease(img);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
  }
}

}  // namespace

bool rotk_polish_prompts_init() {
  const char* v = getenv("ROTK_PROMPTS");
  std::string m = v && *v ? v : "auto";
  g_on = m == "series" || m == "1" || m == "ps" || m == "auto";
  g_mode = m == "1" ? "series" : m;
  if (const char* d = getenv("ROTK_PROMPTS_DUMP"); d && *d) {
    g_dump = d;
    mkdir(d, 0755);
  }
  if (g_on)
    rotk_log("polish: prompts: Xbox Series glyphs (Black -> RB, White -> LB, triggers LT/RT, sticks LS/RS, "
             "Back/Start -> View/Menu; ROTK_PROMPTS=xbox keeps the originals)");
  else
    rotk_log("polish: prompts: original Xbox glyphs (ROTK_PROMPTS=%s)", m.c_str());
  return g_on;
}

// Called by the texture hook (texpack.mm, under its lock).
bool rotk_polish_prompt_glyph(const RotkTextureInfo& info, uint32_t* w2, uint32_t* h2, const uint8_t** rgba) {
  if (!g_on) return false;
  // The family is fixed by the first glyph made (the pad is connected by then; textures are cached).
  if (g_family < 0) {
    g_family = g_mode == "ps" || (g_mode == "auto" && rotk_xinput_pad_family() == 1);
    rotk_log("polish: prompts: %s glyphs (%s)", g_family ? "PlayStation" : "Xbox Series",
             g_mode == "auto" ? "by the connected pad" : "ROTK_PROMPTS");
  }
  const Glyph* g = Find(info.hash, g_family == 1);
  if (!g) return false;
  auto it = g_made.find(info.hash);
  if (it == g_made.end()) {
    int x0 = 0, y0 = 0, x1 = int(info.width), y1 = int(info.height);
    bool boxed = AlphaBox(info, &x0, &y0, &x1, &y1);
    const uint32_t w = info.width * kScale, h = info.height * kScale;
    std::vector<uint8_t> px =
        Draw(*g, w, h, CGRectMake(x0 * kScale, y0 * kScale, (x1 - x0) * kScale, (y1 - y0) * kScale));
    rotk_log("polish: prompts: %016llx (Xbox %s) -> %s, glyph box %d,%d-%d,%d%s of %ux%u, drawn %ux%u",
             (unsigned long long)info.hash, g->original, g->label ? g->label : g->shape == Shape::kView ? "View" : "Menu",
             x0, y0, x1, y1, boxed ? "" : " (no alpha: whole texture)", info.width, info.height, w, h);
    if (!g_dump.empty()) {
      char name[32];
      std::snprintf(name, sizeof name, "/%016llx.png", (unsigned long long)info.hash);
      Save(g_dump + name, w, h, px.data());
    }
    it = g_made.emplace(info.hash, std::move(px)).first;
  }
  *w2 = info.width * kScale, *h2 = info.height * kScale;
  *rgba = it->second.data();
  return true;
}
