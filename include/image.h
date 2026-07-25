#pragma once

// Type-neutral PNG decoding for `Canvas.Sprite.from_png`.
//
// No dependency on culebra Value / JitValue / GC: the interp and JIT backends
// both call decode_png() and turn the Result into their own sprite handle
// (mirrors compress.h / http.h). Errors come back in Result.error so this
// header needs no culebra error type.
//
// stb_image is the whole decoder — it carries its own inflate, so unlike the
// Compress namespace there is no external library to link. The weak/strong
// archive split exists to keep an external dependency (zlib, OpenSSL, sqlite3,
// cblas) out of a binary that doesn't use it; a self-contained decoder needs no
// such choke, and this sits with the other self-hosted subsystems (regex, JSON,
// hash) that every binary simply carries. The cost is ~65 KB of code: the
// runtime helper is `used`, so it is retained rather than dead-stripped.
//
// The implementation is compiled STBI_STATIC, so every translation unit that
// includes this header gets its own private copy of stb's internals and no two
// can collide. Only the PNG loader is compiled in (STBI_ONLY_PNG), and stb's
// stdio path is off (STBI_NO_STDIO): input is always a string already in
// memory.

#include <climits>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#define STBI_STATIC
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

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
inline Decoded decode_png(std::string_view data) {
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
}

}  // namespace culebra::image
