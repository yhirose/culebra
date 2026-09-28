#pragma once

// The width and height of an RGBA8 image a program asked for. Canvas and
// Scene hand these to code that counts in `int` — raylib's image functions
// size their buffers with int arithmetic, and a product past INT_MAX there
// allocated a few bytes and then filled all w*h pixels into them. The
// constructor is private, so a program's numbers reach an image only through
// `checked`, and an entry that forgets to check fails to compile.

#include <climits>
#include <cstdint>
#include <string_view>
#include <utility>

#include <base/shared.h>  // CulebraError / alloc_or_too_large / culebra::format

namespace culebra {

class PixelSize {
 public:
  // raylib takes an image of `w*h*4 < INT_MAX` bytes and no more.
  static constexpr int64_t kMaxPixels = (INT_MAX - 1) / 4;

  // `ctx` names the entry (`Scene.Image.new`), as its other errors do.
  static PixelSize checked(int64_t w, int64_t h, std::string_view ctx,
                           int64_t line, int64_t col) {
    if (w < 0 || h < 0)
      throw CulebraError(
          "ValueError",
          culebra::format("{}: width and height must not be negative, got {}x{}",
                          ctx, w, h),
          line, col);
    if (w > kMaxPixels || h > kMaxPixels || (w != 0 && h > kMaxPixels / w))
      throw CulebraError("ValueError",
                         culebra::format("{}: {}x{} is too large", ctx, w, h),
                         line, col);
    return PixelSize(static_cast<int>(w), static_cast<int>(h), ctx, line, col);
  }

  int w() const { return w_; }
  int h() const { return h_; }
  size_t count() const { return static_cast<size_t>(w_) * h_; }

  // Run the allocation of these pixels: failing it is the refusal `checked`
  // gives, at the same call, not a C++ exception the program cannot catch.
  template <class F>
  auto alloc(F&& f) const -> decltype(f()) {
    return alloc_or_too_large(culebra::format("{}: {}x{}", ctx_, w_, h_),
                              line_, col_, std::forward<F>(f));
  }

 private:
  PixelSize(int w, int h, std::string_view ctx, int64_t line, int64_t col)
      : w_(w), h_(h), ctx_(ctx), line_(line), col_(col) {}
  int w_, h_;
  std::string_view ctx_;  // a literal naming the entry
  int64_t line_, col_;
};

}  // namespace culebra
