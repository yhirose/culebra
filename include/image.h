#pragma once

// Type-neutral PNG decoding and encoding for `Canvas.Sprite.from_png` /
// `to_png`.
//
// No dependency on culebra Value / JitValue / GC: the interp and JIT backends
// both call decode_png() and turn the Result into their own sprite handle
// (mirrors compress.h / http.h). Errors come back in Result.error so this
// header needs no culebra error type.
//
// stb_image is the whole decoder — it carries its own inflate, so unlike the
// Compress namespace there is no external library to link. It is still behind
// a weak/strong archive split: decode_png is reached from the `_Canvas` rows
// of the dispatch table, so without a choke every AOT binary carried its ~65 KB
// whether or not the program names Canvas. Together with the TTF rasterizer
// (font_ttf.h) it forms the Canvas-assets axis (CULEBRA_RT_CANVAS_ASSETS_*):
// the core archive's stub decodes nothing, the strong body is force-loaded
// on Canvas use.
//
// ENCODING is the other way round: stb_image_write's bundled deflate is far
// weaker than zlib's (measured on the retro-run backdrop: 78 KB against 12.6 KB
// for the same pixels), and pixel art is exactly where that gap shows. So the
// encoder is ~60 lines of PNG container here plus zlib for the deflate, reached
// through compress.h's existing choke — which is why `to_png` is gated on the
// same AST scan as `Compress` and a program that encodes nothing links no libz.
// CRC-32 stays self-contained for the same reason (see _png::crc_table).
//
// The implementation is compiled STB_IMAGE_STATIC, so every translation unit
// that includes this header gets its own private copy of stb's internals and no
// two can collide — including raylib's own bundled stb_image, which is linked
// into the same binary whenever the Canvas window backend is on. Only the PNG
// loader is compiled in (STBI_ONLY_PNG), and stb's stdio path is off
// (STBI_NO_STDIO): input is always a string already in memory.

#include <climits>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <compress.h>

#if !defined(CULEBRA_RT_CANVAS_ASSETS_WEAK)
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#endif

// The axis linkage, shared with font_ttf.h (which includes this header).
#if defined(CULEBRA_RT_CANVAS_ASSETS_STRONG)
#define CULEBRA_RT_CANVAS_ASSETS_LINKAGE
#elif defined(CULEBRA_RT_CANVAS_ASSETS_WEAK)
#define CULEBRA_RT_CANVAS_ASSETS_LINKAGE __attribute__((weak))
#else
#define CULEBRA_RT_CANVAS_ASSETS_LINKAGE inline
#endif

namespace culebra::image {

// On success `error` is empty and `px` holds w*h packed-RGBA pixels; on
// failure `error` carries a human message and `px` is empty.
struct Decoded {
  int w = 0;
  int h = 0;
  std::vector<uint32_t> px;
  std::string error;
};

// Decode a PNG image. stb writes RGBA8 in memory order [r, g, b, a], which is
// exactly the framebuffer's packed-RGBA layout, so the pixels are copied
// wholesale rather than repacked per pixel.
CULEBRA_RT_CANVAS_ASSETS_LINKAGE Decoded decode_png(std::string_view data) {
#if defined(CULEBRA_RT_CANVAS_ASSETS_WEAK)
  (void)data;
  return {0, 0, {}, "Canvas image runtime not linked (no Canvas use at build)"};
#else
  if (data.size() > static_cast<size_t>(INT_MAX))
    return {0, 0, {}, "not a valid PNG image"};
  int w = 0, h = 0, channels = 0;
  stbi_uc* out = stbi_load_from_memory(
      reinterpret_cast<const stbi_uc*>(data.data()),
      static_cast<int>(data.size()), &w, &h, &channels, 4);
  if (out == nullptr) return {0, 0, {}, "not a valid PNG image"};
  Decoded d;
  d.w = w;
  d.h = h;
  d.px.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
  if (!d.px.empty())
    std::memcpy(d.px.data(), out, d.px.size() * sizeof(uint32_t));
  stbi_image_free(out);
  return d;
#endif
}

// On success `error` is empty and `data` holds the PNG bytes.
struct Encoded {
  std::string data;
  std::string error;
};

namespace _png {

// CRC-32 (the PNG polynomial), self-contained on purpose: zlib's crc32 lives
// behind the compress choke, and referencing it from here would put a libz
// symbol outside that choke and defeat the whole gating.
inline const uint32_t* crc_table() {
  static const auto t = [] {
    std::vector<uint32_t> v(256);
    for (uint32_t n = 0; n < 256; n++) {
      uint32_t c = n;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      v[n] = c;
    }
    return v;
  }();
  return t.data();
}

inline uint32_t crc32_of(std::string_view s) {
  const uint32_t* t = crc_table();
  uint32_t c = 0xFFFFFFFFu;
  for (unsigned char b : s) c = t[(c ^ b) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

inline void put_be32(std::string& out, uint32_t v) {
  out.push_back(static_cast<char>((v >> 24) & 0xFF));
  out.push_back(static_cast<char>((v >> 16) & 0xFF));
  out.push_back(static_cast<char>((v >> 8) & 0xFF));
  out.push_back(static_cast<char>(v & 0xFF));
}

inline void put_chunk(std::string& out, const char (&tag)[5],
                      std::string_view body) {
  put_be32(out, static_cast<uint32_t>(body.size()));
  std::string tagged(tag, 4);
  tagged.append(body);
  out.append(tagged);
  put_be32(out, crc32_of(tagged));
}

inline uint8_t paeth(uint8_t a, uint8_t b, uint8_t c) {
  int p = int{a} + int{b} - int{c};
  int pa = p > a ? p - a : a - p;
  int pb = p > b ? p - b : b - p;
  int pc = p > c ? p - c : c - p;
  if (pa <= pb && pa <= pc) return a;
  return pb <= pc ? b : c;
}

// Per-row filter choice by libpng's minimum-sum-of-absolute-differences rule:
// score each candidate by treating its bytes as signed magnitudes and keep the
// smallest. On the flat, dithered art Canvas produces this is worth ~30% over
// filtering everything as None.
inline std::string filter_rows(const uint32_t* px, int w, int h) {
  const size_t stride = static_cast<size_t>(w) * 4;
  std::string out;
  out.reserve((stride + 1) * static_cast<size_t>(h));
  std::vector<uint8_t> cand[5];
  for (auto& c : cand) c.resize(stride);
  std::vector<uint8_t> prev(stride, 0), row(stride, 0);
  for (int y = 0; y < h; y++) {
    std::memcpy(row.data(), px + static_cast<size_t>(y) * w, stride);
    for (size_t i = 0; i < stride; i++) {
      uint8_t a = i >= 4 ? row[i - 4] : 0;
      uint8_t b = prev[i];
      uint8_t c = i >= 4 ? prev[i - 4] : 0;
      cand[0][i] = row[i];
      cand[1][i] = static_cast<uint8_t>(row[i] - a);
      cand[2][i] = static_cast<uint8_t>(row[i] - b);
      cand[3][i] = static_cast<uint8_t>(row[i] - ((int{a} + int{b}) >> 1));
      cand[4][i] = static_cast<uint8_t>(row[i] - paeth(a, b, c));
    }
    int best = 0;
    uint64_t best_score = UINT64_MAX;
    for (int f = 0; f < 5; f++) {
      uint64_t score = 0;
      for (uint8_t v : cand[f]) score += v < 128 ? v : 256u - v;
      if (score < best_score) {
        best_score = score;
        best = f;
      }
    }
    out.push_back(static_cast<char>(best));
    out.append(reinterpret_cast<const char*>(cand[best].data()), stride);
    prev.swap(row);
  }
  return out;
}

}  // namespace _png

// Encode `w`*`h` packed-RGBA pixels as a PNG (8-bit truecolour + alpha).
// Deflation goes through the compress choke, so a program that never encodes
// links no libz — and one built without the compress runtime gets that
// function's "runtime not linked" error rather than a broken file.
inline Encoded encode_png(const uint32_t* px, int w, int h) {
  if (w <= 0 || h <= 0) return {{}, "image has no pixels"};
  if (static_cast<int64_t>(w) * h > (int64_t{1} << 28))
    return {{}, "image too large"};
  auto z = compress::deflate_zlib(_png::filter_rows(px, w, h), 9);
  if (!z.error.empty()) return {{}, z.error};

  std::string out("\x89PNG\r\n\x1a\n", 8);
  std::string ihdr;
  _png::put_be32(ihdr, static_cast<uint32_t>(w));
  _png::put_be32(ihdr, static_cast<uint32_t>(h));
  ihdr.append("\x08\x06\x00\x00\x00", 5);  // 8-bit, truecolour+alpha
  _png::put_chunk(out, "IHDR", ihdr);
  _png::put_chunk(out, "IDAT", z.data);
  _png::put_chunk(out, "IEND", "");
  return {std::move(out), {}};
}

}  // namespace culebra::image
