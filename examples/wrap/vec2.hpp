#pragma once
// The "user's own C++ asset" for the wrap demo (design §1): a plain
// header-only class culebra never heard of. Nothing here knows about
// culebra — the binding lives entirely in vec2_binding.cpp.

#include <cmath>
#include <format>
#include <string>

namespace demo {

class Vec2 {
 public:
  Vec2(double x, double y) : x_(x), y_(y) {}

  double x() const { return x_; }
  double y() const { return y_; }
  double len() const { return std::sqrt(x_ * x_ + y_ * y_); }
  void scale(double k) {
    x_ *= k;
    y_ *= k;
  }
  std::string show() const { return std::format("Vec2({}, {})", x_, y_); }
  // By-value return — the §10.3 owning shape.
  Vec2 unit() const {
    double l = len();
    return Vec2(x_ / l, y_ / l);
  }
  static long dims() { return 2; }

 private:
  double x_;
  double y_;
};

}  // namespace demo
