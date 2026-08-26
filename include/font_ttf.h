#pragma once

// TTF/OTF font rasterization for `Canvas.Font`, layered directly on canvas.h's
// shared drawing seam (own_canvas(), Target, put(), blend_over()) so it is
// symmetric across native/WASM/headless the same way Sprite is: there is only
// one compiled implementation of this file, and it never branches on
// CULEBRA_CANVAS_WINDOW or __EMSCRIPTEN__.
//
// stb_truetype is the whole rasterizer — header-only, no external link
// dependency, so like stb_image (image.h) it is simply compiled into every
// binary. Unlike the 8x8 bitmap font (which
// is data, not code: a fixed glyph table walked by canvas.h's own glyph()),
// an outline font needs real rasterization, which stb_truetype does with pure
// integer/float arithmetic and no hinting — the same "one answer per input"
// property that keeps rect/line/ellipse's own rasterizers interp/JIT/AOT
// identical, just traded here for outline shapes instead of straight edges
// and conics. NO SECURITY GUARANTEE: stb_truetype does not range-check a
// malformed font's internal offsets (see its own file header) — Font.new is
// documented as trusted-input-only, the same posture PNG decoding does not
// need (stb_image is more defensive) but sprite_from_png's docs don't either
// have to caveat.
//
// A Font handle is an IdRegistry id like Sprite's, but unlike Sprite there is
// no reserved id 0: a font is never a draw target, so nothing needs a
// sentinel, and ttf_of()'s own_canvas() gate already makes the id value
// irrelevant to a non-owning isolate (every resolve fails before the id is
// even looked at, exactly like sprite_of()).

#include <canvas.h>

#include <climits>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

// Mirrors image.h's STB_IMAGE_STATIC: internal linkage keeps this copy from
// colliding with raylib's own vendored stb_truetype.h, which is linked into
// the same binary whenever the Canvas window backend is on.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

namespace culebra {
namespace _canvas_detail {

struct TtfLoadResult {
  int64_t id;
  std::string error;
};

// One rasterized glyph at one (codepoint, size): an antialiased coverage
// bitmap (stb_truetype's box-filter AA, 0..255 per pixel), its bitmap origin
// relative to the pen position on the baseline, and its advance width,
// rounded once here so a draw and a text_width call over the same string
// always agree.
struct RasterizedGlyph {
  int w = 0, h = 0;          // 0x0 for a blank glyph (space, .notdef with no
                              // outline) — draw and advance still apply
  int xoff = 0, yoff = 0;    // bitmap top-left, relative to (pen_x, baseline_y)
  int64_t advance = 0;       // rounded pixel advance at this size
  std::vector<uint8_t> coverage;  // w*h bytes, row-major, one byte per pixel
};

// A parsed TTF/OTF font. `bytes` must outlive `info`: stbtt_fontinfo holds
// raw pointers into it. Registered through _fonts_ttf() the same way Sprite
// is registered through _sprites() — gated by own_canvas(), so unlike the 8x8
// FontTable (which every isolate reaches unconditionally resolving the Canvas
// module, and so carries its own lock) this needs none: only the owning
// isolate ever touches it.
struct TtfFont {
  std::vector<uint8_t> bytes;
  stbtt_fontinfo info{};
  int ascent = 0, descent = 0, line_gap = 0;  // unscaled font units
  // Key: size<<32 | codepoint (as uint32). v1 has no kerning, so the pair is
  // sufficient; a negative codepoint's bit pattern can't collide with a valid
  // Unicode scalar value's, so no extra validation is needed here.
  std::unordered_map<uint64_t, RasterizedGlyph> cache;
};

inline IdRegistry<TtfFont>& _fonts_ttf() {
  static IdRegistry<TtfFont> r;
  return r;
}

inline TtfFont* ttf_of(int64_t id) {
  if (!own_canvas()) return nullptr;
  return _fonts_ttf().get(id);
}

// Parse TTF/OTF bytes and register the font, or fail with a human message.
// Only the first font in a font collection is used (culebra has no API for
// picking another face).
// stb_truetype's own file-format sniffing reads a fixed prefix (the sfnt/TTC
// tag plus a header) with no bounds check of its own -- an empty buffer's
// std::vector::data() can be null, and stb would dereference it. 12 bytes
// covers the smallest header any of its formats read before this function
// itself has decided the input isn't a font.
inline constexpr size_t kTtfMinBytes = 12;

inline TtfLoadResult ttf_load(std::string_view data) {
  if (!own_canvas()) return {0, ""};
  if (data.size() < kTtfMinBytes || data.size() > static_cast<size_t>(INT_MAX))
    return {0, "not a valid TTF/OTF font"};
  auto f = std::make_unique<TtfFont>();
  f->bytes.assign(data.begin(), data.end());
  int offset = stbtt_GetFontOffsetForIndex(f->bytes.data(), 0);
  if (offset < 0 || !stbtt_InitFont(&f->info, f->bytes.data(), offset))
    return {0, "not a valid TTF/OTF font"};
  stbtt_GetFontVMetrics(&f->info, &f->ascent, &f->descent, &f->line_gap);
  // f stays owning until add() has placed the pointer, so a growth-triggered
  // bad_alloc inside add() doesn't leak it (same order as sprite_adopt).
  int64_t id = _fonts_ttf().add(f.get());
  f.release();
  return {id, ""};
}

inline void ttf_free(int64_t id) {
  TtfFont* f = ttf_of(id);
  if (f == nullptr) return;
  delete f;
  _fonts_ttf().invalidate(id);
}

// A rasterized glyph bitmap is roughly size^2 bytes, so an unclamped size
// argument is an attacker-controlled allocation — the same class of guard as
// canvas.h's kGuard, just sized for a font rather than a whole framebuffer.
inline constexpr int64_t kTtfMaxSize = 2048;
inline int64_t ttf_clamp_size(int64_t size) {
  return size < 1 ? 0 : (size > kTtfMaxSize ? kTtfMaxSize : size);
}

// Rasterize (and cache) codepoint at size, or nullptr when size clamps to 0.
inline const RasterizedGlyph* ttf_rasterize(TtfFont& f, int64_t codepoint,
                                            int64_t size) {
  size = ttf_clamp_size(size);
  if (size == 0) return nullptr;
  uint64_t key = (static_cast<uint64_t>(size) << 32) |
                 static_cast<uint32_t>(static_cast<int32_t>(codepoint));
  auto it = f.cache.find(key);
  if (it != f.cache.end()) return &it->second;

  float scale = stbtt_ScaleForPixelHeight(&f.info, static_cast<float>(size));
  RasterizedGlyph g;
  int adv = 0, lsb = 0;
  stbtt_GetCodepointHMetrics(&f.info, static_cast<int>(codepoint), &adv, &lsb);
  g.advance = static_cast<int64_t>(
      std::lround(static_cast<double>(adv) * static_cast<double>(scale)));
  int w = 0, h = 0, xoff = 0, yoff = 0;
  unsigned char* bmp =
      stbtt_GetCodepointBitmap(&f.info, scale, scale,
                               static_cast<int>(codepoint), &w, &h, &xoff,
                               &yoff);
  g.w = w;
  g.h = h;
  g.xoff = xoff;
  g.yoff = yoff;
  if (bmp != nullptr) {
    g.coverage.assign(bmp, bmp + static_cast<size_t>(w) * static_cast<size_t>(h));
    stbtt_FreeBitmap(bmp, f.info.userdata);
  }
  return &f.cache.emplace(key, std::move(g)).first->second;
}

// Composite a rasterized glyph into `t`, pen position on the baseline (x, y) —
// the stb_truetype convention, not the top-left convention the 8x8 glyph()
// uses; the Font preamble class derives the baseline from ascent() so Font.draw
// still reads as "top-left" the way Canvas.text does. Coverage (0..255) and
// rgba's own alpha combine with the exact rounding rule blit_scaled already
// uses ((cov*a + 127)/255), so this rides put()'s existing blend_over() path
// unmodified — no new compositing rule to keep symmetric across backends. The
// framebuffer and the screen layer differ only in which Target and which
// (x, y, size) resolve before this; the pixels are laid down the same way.
inline void ttf_blit_glyph(const RasterizedGlyph& g, const Target& t, int x,
                           int y, uint32_t rgba) {
  uint32_t base_a = rgba >> 24;
  if (base_a == 0 || g.w == 0 || g.h == 0) return;
  uint32_t rgb = rgba & 0x00FFFFFFu;
  for (int row = 0; row < g.h; row++) {
    for (int col = 0; col < g.w; col++) {
      uint32_t cov = g.coverage[static_cast<size_t>(row) * g.w + col];
      if (cov == 0) continue;
      uint32_t a = (cov * base_a + 127) / 255;
      put(t, x + g.xoff + col, y + g.yoff + row, rgb | (a << 24));
    }
  }
}

// Draw one glyph into the current draw target (framebuffer or sprite).
// Returns the glyph's own pixel advance (0 for an unknown handle/codepoint or
// a non-positive size) — ttf_rasterize already has it, so the preamble's
// draw() loop can step its cursor from this instead of a second lookup
// through ttf_advance for the same (font, codepoint, size).
inline int64_t ttf_glyph(int64_t font_id, int64_t codepoint, int x, int y,
                         uint32_t rgba, int64_t size) {
  TtfFont* f = ttf_of(font_id);
  if (f == nullptr) return 0;
  const RasterizedGlyph* g = ttf_rasterize(*f, codepoint, size);
  if (g == nullptr) return 0;
  ttf_blit_glyph(*g, resolve_target(), x, y, rgba);
  return g->advance;
}

// Draw one glyph into the screen layer (canvas.h) instead of the framebuffer.
// (x, y, size) arrive in the SAME logical units ttf_glyph takes and are scaled
// here by screen_scale(), so the glyph is rasterized AT the presented size —
// which is the whole point: magnifying a small raster is exactly the blockiness
// this path exists to avoid.
//
// The advance returned is the LOGICAL one (ttf_rasterize at the original
// `size`), not the scaled one. The pen a caller steps with it, and the width
// text_width() predicts for the same string, therefore stay in the one
// coordinate system draw() and draw_screen() share — the glyphs land at
// different resolutions, the layout does not diverge.
inline int64_t ttf_glyph_screen(int64_t font_id, int64_t codepoint, int x,
                                int y, uint32_t rgba, int64_t size) {
  TtfFont* f = ttf_of(font_id);
  if (f == nullptr) return 0;
  double scale = screen_scale();
  const RasterizedGlyph* g =
      ttf_rasterize(*f, codepoint, std::llround(size * scale));
  if (g != nullptr) {
    Target t = resolve_screen_target();
    if (t.px != nullptr) {
      ttf_blit_glyph(*g, t, static_cast<int>(std::llround(x * scale)),
                     static_cast<int>(std::llround(y * scale)), rgba);
      _screen_dirty() = true;
    }
  }
  const RasterizedGlyph* logical = ttf_rasterize(*f, codepoint, size);
  return logical == nullptr ? 0 : logical->advance;
}

inline int64_t ttf_advance(int64_t font_id, int64_t codepoint, int64_t size) {
  TtfFont* f = ttf_of(font_id);
  if (f == nullptr) return 0;
  const RasterizedGlyph* g = ttf_rasterize(*f, codepoint, size);
  return g == nullptr ? 0 : g->advance;
}

// Pixel ascent at `size`: how far the tallest glyph reaches above the
// baseline. The Font preamble class uses this to convert a visual top-left
// (x, y) into the baseline position ttf_glyph expects.
inline int64_t ttf_ascent(int64_t font_id, int64_t size) {
  TtfFont* f = ttf_of(font_id);
  if (f == nullptr) return 0;
  size = ttf_clamp_size(size);
  if (size == 0) return 0;
  float scale = stbtt_ScaleForPixelHeight(&f->info, static_cast<float>(size));
  return static_cast<int64_t>(
      std::lround(static_cast<double>(f->ascent) * static_cast<double>(scale)));
}

}  // namespace _canvas_detail
}  // namespace culebra
