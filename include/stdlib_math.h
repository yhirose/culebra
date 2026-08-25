#pragma once

// Math-namespace kernels shared by the binding layer (stdlib_jit.h) and
// the JIT/AOT runtime (stdlib_jit.h).
//
// Each kernel owns one semantic decision — Long/Float promotion, exact
// 64-bit comparison, edge-case errors — so the two backends cannot drift:
// the backend adapters only translate their value representation (interp
// Value / JIT tag+data) to and from `Num` at the boundary, and the
// parameter lists both binders use already come from the one canonical
// spec (see "Single source of truth for the calling convention" in
// stdlib_jit.h). Errors throw CulebraError through the caller-provided
// position, producing identical kind/message/position on both backends.
//
// `PosFn` is a callable returning std::pair{line, col}; kernels call it
// only on the error path, so an interp adapter can defer its __LINE__ /
// __COLUMN__ environment lookups to the cold path (the JIT adapters pass
// the position they already hold).

#include <shared.h>

#include <cmath>
#include <cstdint>
#include <utility>

namespace culebra::math {

// A Long-or-Float scalar — the boundary currency of these kernels.
struct Num {
  bool is_float;
  int64_t l;  // valid when !is_float
  double d;   // valid when is_float
};

inline Num num_long(int64_t v) { return {false, v, 0.0}; }
inline Num num_float(double v) { return {true, 0, v}; }
inline double to_double(const Num& v) {
  return v.is_float ? v.d : static_cast<double>(v.l);
}

// abs: Long -> Long, Float -> Float.
inline Num abs(const Num& x) {
  if (x.is_float) return num_float(std::fabs(x.d));
  return num_long(x.l < 0 ? -x.l : x.l);
}

// sign over the declared-Long param: -1 / 0 / 1.
inline int64_t sign(int64_t x) { return x > 0 ? 1 : (x < 0 ? -1 : 0); }

// Integer power: 0^0 == 1; a negative exponent is a type error.
template <class PosFn>
int64_t pow_long(int64_t base, int64_t exp, PosFn&& pos) {
  if (exp < 0) {
    auto [line, col] = pos();
    throw_type_error_at(line, col);
  }
  return ipow_nonneg(base, exp);
}

// Floored modulo; wrap by 0 divides by 0.
template <class PosFn>
int64_t wrap(int64_t x, int64_t n, PosFn&& pos) {
  if (n == 0) {
    auto [line, col] = pos();
    throw CulebraError("ZeroDivisionError", "divide by 0 error", line, col);
  }
  return floored_mod(x, n);
}

// clamp(x, lo, hi): Long if x/lo/hi are all Long, else Float (any Float
// input promotes the whole comparison to Float, mirroring min/max).
inline Num clamp(const Num& x, const Num& lo, const Num& hi) {
  if (x.is_float || lo.is_float || hi.is_float) {
    double xd = to_double(x), lod = to_double(lo), hid = to_double(hi);
    return num_float(xd < lod ? lod : (xd > hid ? hid : xd));
  }
  return num_long(x.l < lo.l ? lo.l : (x.l > hi.l ? hi.l : x.l));
}

// min/max over >=1 numeric args (max(5) == 5): Long iff every arg was
// Long, in which case the comparison is exact 64-bit — a double-coerced
// compare would collapse neighbours past 2^53. `get(i)` returns the i-th
// argument as Num and throws (positioned) on a non-numeric one; the first
// pass surfaces that error in argument order before any reduction.
template <class GetNum, class PosFn>
Num reduce_min_max(int64_t n, bool pick_less, GetNum&& get, PosFn&& pos) {
  if (n < 1) {
    auto [line, col] = pos();
    throw_type_error_at(line, col);
  }
  bool any_float = false;
  for (int64_t i = 0; i < n; i++) {
    if (get(i).is_float) any_float = true;
  }
  if (any_float) {
    double acc = to_double(get(0));
    for (int64_t i = 1; i < n; i++) {
      double x = to_double(get(i));
      if (pick_less ? x < acc : x > acc) acc = x;
    }
    return num_float(acc);
  }
  int64_t acc = get(0).l;
  for (int64_t i = 1; i < n; i++) {
    int64_t x = get(i).l;
    if (pick_less ? x < acc : x > acc) acc = x;
  }
  return num_long(acc);
}

// The unary Float family (log/exp/sqrt/trig): any numeric coerces to
// double, the result is always Float — even for Long input.
template <class Fn>
double f2f(const Num& x, Fn&& fn) {
  return fn(to_double(x));
}

// The unary Long family (floor/ceil/round): Long passes through untouched
// (coercing through Float would lose precision past 2^53); Float maps
// through fn and truncates to Long.
template <class Fn>
Num f2l(const Num& x, Fn&& fn) {
  if (!x.is_float) return x;
  return num_long(static_cast<int64_t>(fn(x.d)));
}

// atan2(y, x): two numerics -> Float (radians).
inline double atan2(const Num& y, const Num& x) {
  return std::atan2(to_double(y), to_double(x));
}

}  // namespace culebra::math
