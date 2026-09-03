#pragma once

#include <cstdint>

namespace culebra {

// Iteration bounds of an integer range (`a..b`, `a..=b`, `by s`; also
// Math.range). Single source of the sequence semantics, shared by the
// interpreter (range_bounds / _get_iterator), the JIT's iterator runtime
// (culebra_runtime_range_iter / _math_range_fast_fn) and — as hand-emitted
// IR — the JIT's counted-range fast path (compile_for_counted_range), so
// every backend yields the identical sequence at the int64 boundary.
//
// The inclusive endpoint is kept as a flag rather than normalized into an
// exclusive `end ± 1`: that normalization overflows for `..=INT64_MAX` /
// `..=INT64_MIN` and turned those ranges into empty sequences. The advance
// is overflow-checked for the same reason — a step past the boundary used
// to wrap and re-enter the range (`INT64_MAX-1..INT64_MAX by 3` looped
// forever through phantom values). Cf. _slice_bounds, which guards the
// same `..=` increment on the slicing side.
struct RangeBounds {
  int64_t cur;
  int64_t end;
  int64_t step;
  bool inclusive;
  bool exhausted = false;

  bool done() const {
    return exhausted || (step > 0 ? (inclusive ? cur > end : cur >= end)
                                  : (inclusive ? cur < end : cur <= end));
  }
  // Surrender the current value and advance. Only valid when !done();
  // an advance past int64 is the end of the sequence.
  int64_t take() {
    int64_t v = cur;
    if (__builtin_add_overflow(cur, step, &cur)) exhausted = true;
    return v;
  }
};

}  // namespace culebra
