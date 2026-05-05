#pragma once

// Tensor primitive — backend-agnostic core (Phase 1, M1).
//
// This header is independent of the interpreter / JIT Value layer. It
// defines the dtype/shape representation and the raw-buffer kernels
// (zeros, ones) that the interpreter and JIT both call into.
//
// Layering:
//   tensor.h              <- this file (pure C++, no Culebra types)
//   tensor_backend.h      <- (M2+) backend dispatch table
//   tensor_cpu.h          <- (M2+) CPU kernels (Accelerate / OpenBLAS)
//   interpreter.h         <- TensorValue (Culebra-side), uses tensor.h
//   stdlib_{interp,jit}.h <- Tensor namespace registration
//
// In M1 the only ops are zeros/ones/from(array). Elementwise/BLAS land
// in M2/M4.

#include <support.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef CULEBRA_TENSOR_ENABLED
#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#else
#include <cblas.h>
#endif
#endif

namespace culebra {

enum class Dtype { F32, F64 };

inline const char* dtype_name(Dtype d) {
  return d == Dtype::F32 ? "f32" : "f64";
}

inline size_t dtype_size(Dtype d) {
  return d == Dtype::F32 ? 4 : 8;
}

inline std::optional<Dtype> parse_dtype(std::string_view s) {
  if (s == "f32") return Dtype::F32;
  if (s == "f64") return Dtype::F64;
  return std::nullopt;
}

// Tensor op tag. `Const` means the tensor's buffer is materialized
// (no graph dependency); other ops are lazy nodes whose buffer is
// populated by tensor_eval(). Extended in later milestones.
enum class Op {
  Const,
  Add, Sub, Mul, Div, Pow,
  // Axis reductions: op_param holds the reduction axis. The axis-less
  // form (`.sum()` / `.mean()` / `.max()`) skips the graph and runs
  // eagerly because it returns a scalar Float, not a Tensor.
  Sum, Mean, Max, Argmax,
  // Linear algebra and activations. Dot uses cblas; activations are
  // elementwise (Softmax operates on the last axis). LinearSigmoid
  // is the fused (W @ x + b) → sigmoid kernel for MLP forward.
  Dot,
  Sigmoid, Relu, Softmax,
  LinearSigmoid,
};

struct TensorShape {
  std::vector<int64_t> dims;

  TensorShape() = default;
  explicit TensorShape(std::vector<int64_t> d) : dims(std::move(d)) {
    for (auto x : dims) {
      if (x < 0) {
        throw std::runtime_error("Tensor: negative dim.");
      }
    }
  }

  size_t rank() const { return dims.size(); }

  // Empty shape (rank 0) is a scalar tensor: one element by convention.
  size_t num_elements() const {
    size_t n = 1;
    for (auto d : dims) n *= static_cast<size_t>(d);
    return n;
  }

  bool operator==(const TensorShape& rhs) const { return dims == rhs.dims; }
};

// Standard row-major strides for a shape (element units, not bytes).
inline std::vector<int64_t> tensor_contiguous_strides(const TensorShape& s) {
  if (s.dims.empty()) return {};
  std::vector<int64_t> out(s.dims.size(), 1);
  for (size_t i = s.dims.size(); i-- > 1;) {
    out[i - 1] = out[i] * s.dims[i];
  }
  return out;
}

// Tensor — materialized buffer, zero-copy view, or lazy graph node.
//   Const + buf populated   : owned materialized tensor
//   Const + base != nullptr : zero-copy view into base (transpose / slice / reshape)
//   non-Const op            : lazy graph node, buf populated by eval
//
// Lifetime: shared via shared_ptr. Cycles are impossible because both
// `inputs` (graph) and `base` (view) point input-ward.
struct TensorImpl {
  Op op = Op::Const;
  TensorShape shape;
  Dtype dtype;
  std::vector<std::shared_ptr<TensorImpl>> inputs;

  // Storage: either we own `buf`, or we read from `base->data() + offset`.
  std::vector<std::byte> buf;
  std::shared_ptr<TensorImpl> base;
  std::vector<int64_t> strides;  // element units, length == shape.rank()
  size_t offset = 0;             // element units from base->data() start

  // Op-specific scalar parameter. Currently used by reduction ops to
  // carry the axis index. Add more if a future op needs more state.
  int64_t op_param = 0;

  // Materialized ctor: shape + dtype + zero-initialized buffer.
  TensorImpl(TensorShape s, Dtype d)
      : shape(std::move(s)),
        dtype(d),
        buf(shape.num_elements() * dtype_size(d)),
        strides(tensor_contiguous_strides(shape)) {}

  // Lazy ctor: op + shape + dtype + inputs. Caller does shape/dtype
  // inference. `buf` and `strides` are populated by tensor_eval().
  TensorImpl(Op o, TensorShape s, Dtype d,
             std::vector<std::shared_ptr<TensorImpl>> ins)
      : op(o),
        shape(std::move(s)),
        dtype(d),
        inputs(std::move(ins)) {}

  // Zero-copy view ctor: shares storage with `b` under a different
  // shape / strides / offset. transpose / slice / reshape produce these.
  struct View {};
  TensorImpl(View, std::shared_ptr<TensorImpl> b, TensorShape s,
             std::vector<int64_t> str, size_t off)
      : op(Op::Const),
        shape(std::move(s)),
        dtype(b->dtype),
        buf(),
        base(std::move(b)),
        strides(std::move(str)),
        offset(off) {}

  bool is_evaluated() const {
    return op == Op::Const || !buf.empty() || base != nullptr;
  }

  // strides match a standard row-major contiguous layout? Allocation-
  // free: walks the strides from the right and checks each against
  // the running product. Hot path — called from binop / dot / softmax
  // / blas-input dispatch.
  bool is_contiguous() const {
    if (strides.size() != shape.dims.size()) return false;
    int64_t expected = 1;
    for (size_t i = strides.size(); i-- > 0;) {
      if (strides[i] != expected) return false;
      expected *= shape.dims[i];
    }
    return true;
  }

  void* data() {
    if (base) {
      return static_cast<std::byte*>(base->data()) +
             offset * dtype_size(dtype);
    }
    return buf.data();
  }
  const void* data() const {
    return const_cast<TensorImpl*>(this)->data();
  }

  template <typename T>
  T* data_as() {
    return static_cast<T*>(data());
  }
  template <typename T>
  const T* data_as() const {
    return static_cast<const T*>(data());
  }
};

using TensorPtr = std::shared_ptr<TensorImpl>;

inline TensorPtr tensor_zeros(TensorShape s, Dtype d) {
  return std::make_shared<TensorImpl>(std::move(s), d);
}

// Shared between the interp `tensor_ones` and the JIT runtime entry —
// keeps the F32/F64 fill in one place.
inline void tensor_fill_ones_inplace(TensorImpl& t) {
  auto n = t.shape.num_elements();
  if (t.dtype == Dtype::F32) std::fill_n(t.data_as<float>(), n, 1.0f);
  else                       std::fill_n(t.data_as<double>(), n, 1.0);
}

inline TensorPtr tensor_ones(TensorShape s, Dtype d) {
  auto t = std::make_shared<TensorImpl>(std::move(s), d);
  tensor_fill_ones_inplace(*t);
  return t;
}

// Direct CSV loader — reads the entire file into a single contiguous
// Tensor without going through nested Array intermediates. About 3x
// faster than `Tensor.from(load_2d(path))` on MNIST-shaped data
// because it avoids the per-cell Value allocations.
//
// Always rank-2: a single-column CSV becomes [rows, 1] so bias
// vectors keep their column shape for broadcast against [M, batch]
// outputs. Cell parsing uses std::strtod (C-locale; embedders relying
// on alternate LC_NUMERIC need to switch the locale themselves).
inline TensorPtr tensor_from_csv(const std::string& path, Dtype d) {
  std::ifstream ifs(path, std::ios::binary | std::ios::ate);
  if (!ifs) {
    throw std::runtime_error("Tensor.from_csv: cannot open " + path);
  }
  auto size = static_cast<size_t>(ifs.tellg());
  ifs.seekg(0);
  std::string contents(size, '\0');
  ifs.read(contents.data(), static_cast<std::streamsize>(size));
  // Pass 1: count rows + determine cols from first row.
  size_t cols = 0;
  size_t rows = 0;
  bool first_row_done = false;
  size_t cur_cols = 0;
  for (size_t i = 0; i < contents.size(); ) {
    size_t nl = contents.find('\n', i);
    if (nl == std::string::npos) nl = contents.size();
    if (nl > i) {
      cur_cols = 1;
      for (size_t j = i; j < nl; j++) if (contents[j] == ',') cur_cols++;
      if (!first_row_done) { cols = cur_cols; first_row_done = true; }
      else if (cur_cols != cols) {
        throw std::runtime_error(
            "Tensor.from_csv: ragged row in " + path);
      }
      rows++;
    }
    i = nl + 1;
  }
  // Always rank-2: a single-column CSV becomes [rows, 1], which is
  // the shape downstream code expects for bias vectors and similar.
  std::vector<int64_t> dims{static_cast<int64_t>(rows),
                             static_cast<int64_t>(cols)};
  auto t = std::make_shared<TensorImpl>(TensorShape(std::move(dims)), d);
  // Pass 2: parse and fill directly into the buffer.
  auto store = [&](size_t idx, double v) {
    if (d == Dtype::F32) t->data_as<float>()[idx] = static_cast<float>(v);
    else                 t->data_as<double>()[idx] = v;
  };
  size_t idx = 0;
  for (size_t i = 0; i < contents.size(); ) {
    size_t nl = contents.find('\n', i);
    if (nl == std::string::npos) nl = contents.size();
    if (nl > i) {
      size_t s = i;
      while (s < nl) {
        size_t e = s;
        while (e < nl && contents[e] != ',') e++;
        char* parse_end = nullptr;
        double v = std::strtod(contents.data() + s, &parse_end);
        if (parse_end == contents.data() + s) {
          throw std::runtime_error(
              "Tensor.from_csv: parse error in " + path);
        }
        store(idx++, v);
        s = e + 1;
      }
    }
    i = nl + 1;
  }
  return t;
}

// Standard normal (mean 0, std 1). Uses the process-wide PRNG from
// support.h so `Random.seed(n)` makes Tensor.randn reproducible.
inline TensorPtr tensor_randn(TensorShape s, Dtype d) {
  auto t = std::make_shared<TensorImpl>(std::move(s), d);
  std::normal_distribution<double> dist{0.0, 1.0};
  auto& engine = random_engine();
  auto n = t->shape.num_elements();
  if (d == Dtype::F32) {
    auto* p = t->data_as<float>();
    for (size_t i = 0; i < n; i++) p[i] = static_cast<float>(dist(engine));
  } else {
    auto* p = t->data_as<double>();
    for (size_t i = 0; i < n; i++) p[i] = dist(engine);
  }
  return t;
}

// numpy / silarray broadcast rules: align trailing dims; each pair
// must be equal or one side must be 1. Output dim is the max. Empty
// (rank-0) shapes act as scalars.
inline TensorShape tensor_broadcast_shape(const TensorShape& a,
                                          const TensorShape& b) {
  if (a == b) return a;
  if (a.dims.empty()) return b;
  if (b.dims.empty()) return a;
  size_t ar = a.dims.size();
  size_t br = b.dims.size();
  size_t out_rank = std::max(ar, br);
  std::vector<int64_t> out(out_rank);
  for (size_t i = 0; i < out_rank; i++) {
    int64_t ad = (i + ar >= out_rank) ? a.dims[i - (out_rank - ar)] : 1;
    int64_t bd = (i + br >= out_rank) ? b.dims[i - (out_rank - br)] : 1;
    if (ad != bd && ad != 1 && bd != 1) {
      throw std::runtime_error("Tensor: shapes not broadcast-compatible.");
    }
    out[i] = std::max(ad, bd);
  }
  return TensorShape(std::move(out));
}

// Build a lazy elementwise binop node. Shapes are broadcast per numpy
// rules; dtype must match (no implicit promotion in Phase 1).
inline TensorPtr tensor_binop(Op op, TensorPtr a, TensorPtr b) {
  if (a->dtype != b->dtype) {
    throw std::runtime_error("Tensor: dtype mismatch in binop.");
  }
  auto shape = tensor_broadcast_shape(a->shape, b->shape);
  auto dtype = a->dtype;
  return std::make_shared<TensorImpl>(
      op, std::move(shape), dtype,
      std::vector<TensorPtr>{std::move(a), std::move(b)});
}

// Rank-0 Tensor holding a single scalar — used to lift scalar Float
// operands into the binop graph so broadcast does the rest.
inline TensorPtr tensor_scalar(double v, Dtype d) {
  auto t = std::make_shared<TensorImpl>(TensorShape{}, d);
  if (d == Dtype::F32) t->data_as<float>()[0] = static_cast<float>(v);
  else                 t->data_as<double>()[0] = v;
  return t;
}

// Strides into the input aligned to `out_shape` for broadcast.
// Caller passes the input's actual strides (which may be non-standard
// for a view). Dims the input was broadcast across get stride 0.
inline std::vector<int64_t> _broadcast_strides(
    const TensorShape& in_shape, const std::vector<int64_t>& in_strides,
    const TensorShape& out_shape) {
  size_t out_rank = out_shape.dims.size();
  size_t in_rank = in_shape.dims.size();
  size_t leading = out_rank - in_rank;
  std::vector<int64_t> out(out_rank, 0);
  for (size_t i = leading; i < out_rank; i++) {
    auto in_dim = in_shape.dims[i - leading];
    if (in_dim != 1) out[i] = in_strides[i - leading];
  }
  return out;
}

// Op-specialized binop applicator. Keeps the switch out of the inner
// loop so the same-shape path autovectorizes cleanly.
template <typename T, Op op>
struct _tensor_binop {
  static T apply(T a, T b);
};
template <typename T> struct _tensor_binop<T, Op::Add> {
  static T apply(T a, T b) { return a + b; }
};
template <typename T> struct _tensor_binop<T, Op::Sub> {
  static T apply(T a, T b) { return a - b; }
};
template <typename T> struct _tensor_binop<T, Op::Mul> {
  static T apply(T a, T b) { return a * b; }
};
template <typename T> struct _tensor_binop<T, Op::Div> {
  static T apply(T a, T b) { return a / b; }
};
template <typename T> struct _tensor_binop<T, Op::Pow> {
  static T apply(T a, T b) { return static_cast<T>(std::pow(a, b)); }
};

template <typename T, Op op>
inline void _tensor_run_binop_same_shape(T* out, const T* a, const T* b,
                                          size_t n) {
  for (size_t i = 0; i < n; i++) out[i] = _tensor_binop<T, op>::apply(a[i], b[i]);
}

template <typename T, Op op>
inline void _tensor_run_binop_broadcast(T* out, const T* a, const T* b,
                                         const std::vector<int64_t>& astr_in,
                                         const std::vector<int64_t>& bstr_in,
                                         const TensorShape& a_shape,
                                         const TensorShape& b_shape,
                                         const TensorShape& out_shape) {
  auto astr = _broadcast_strides(a_shape, astr_in, out_shape);
  auto bstr = _broadcast_strides(b_shape, bstr_in, out_shape);
  auto rank = out_shape.dims.size();
  auto total = out_shape.num_elements();
  for (size_t i = 0; i < total; i++) {
    size_t rem = i;
    int64_t ai = 0, bi = 0;
    for (size_t d = rank; d-- > 0;) {
      auto m = static_cast<int64_t>(rem % out_shape.dims[d]);
      rem /= out_shape.dims[d];
      ai += m * astr[d];
      bi += m * bstr[d];
    }
    out[i] = _tensor_binop<T, op>::apply(a[ai], b[bi]);
  }
}

template <typename T, Op op>
inline void _tensor_run_binop_typed(TensorImpl& t) {
  auto& a = *t.inputs[0];
  auto& b = *t.inputs[1];
  auto* out = t.data_as<T>();
  // Fast path: same shape AND both inputs contiguous → linear loop,
  // autovectorizable. Output is freshly allocated and contiguous.
  if (a.shape == b.shape && a.is_contiguous() && b.is_contiguous()) {
    _tensor_run_binop_same_shape<T, op>(out, a.data_as<T>(), b.data_as<T>(),
                                          t.shape.num_elements());
    return;
  }
  _tensor_run_binop_broadcast<T, op>(out, a.data_as<T>(), b.data_as<T>(),
                                       a.strides, b.strides, a.shape, b.shape,
                                       t.shape);
}

template <typename T>
inline void _tensor_run_binop_for_dtype(TensorImpl& t) {
  switch (t.op) {
    case Op::Add: _tensor_run_binop_typed<T, Op::Add>(t); break;
    case Op::Sub: _tensor_run_binop_typed<T, Op::Sub>(t); break;
    case Op::Mul: _tensor_run_binop_typed<T, Op::Mul>(t); break;
    case Op::Div: _tensor_run_binop_typed<T, Op::Div>(t); break;
    case Op::Pow: _tensor_run_binop_typed<T, Op::Pow>(t); break;
    default:
      throw std::logic_error("tensor: non-binop op in binop dispatch");
  }
}

inline void _tensor_run_binop(TensorImpl& t) {
  if (t.dtype == Dtype::F32) _tensor_run_binop_for_dtype<float>(t);
  else                       _tensor_run_binop_for_dtype<double>(t);
}

inline void tensor_eval_node(TensorImpl& t);

// Build a lazy reduction along `axis`. Output shape drops that axis
// (numpy's keepdims=false). Caller must validate `axis` ∈ [0, rank).
inline TensorPtr tensor_reduce_axis(Op op, TensorPtr a, int64_t axis) {
  if (axis < 0 || axis >= static_cast<int64_t>(a->shape.dims.size())) {
    throw std::runtime_error("Tensor: reduction axis out of range.");
  }
  std::vector<int64_t> out_dims;
  out_dims.reserve(a->shape.dims.size() - 1);
  for (size_t i = 0; i < a->shape.dims.size(); i++) {
    if (static_cast<int64_t>(i) != axis) out_dims.push_back(a->shape.dims[i]);
  }
  auto t = std::make_shared<TensorImpl>(
      op, TensorShape(std::move(out_dims)), a->dtype,
      std::vector<TensorPtr>{std::move(a)});
  t->op_param = axis;
  return t;
}

// Iterate over output positions and accumulate along the reduction
// axis of the input. Strides on input are respected (so views work);
// output is contiguous (allocated by tensor_eval_node).
template <typename T, Op op>
inline void _tensor_run_reduce_axis(TensorImpl& out, const TensorImpl& in) {
  const T* in_data = in.data_as<T>();
  T* out_data = out.data_as<T>();
  size_t out_total = out.shape.num_elements();
  size_t out_rank = out.shape.dims.size();
  int64_t axis = out.op_param;
  int64_t axis_size = in.shape.dims[axis];
  int64_t axis_stride = in.strides[axis];

  for (size_t i = 0; i < out_total; i++) {
    size_t rem = i;
    int64_t in_idx = 0;
    for (size_t out_d = out_rank; out_d-- > 0;) {
      auto m = static_cast<int64_t>(rem % out.shape.dims[out_d]);
      rem /= out.shape.dims[out_d];
      // Map output dim out_d to input dim (skip the reduced axis).
      size_t in_d =
          (static_cast<int64_t>(out_d) >= axis) ? out_d + 1 : out_d;
      in_idx += m * in.strides[in_d];
    }
    if constexpr (op == Op::Sum || op == Op::Mean) {
      T s = T{};
      for (int64_t j = 0; j < axis_size; j++) {
        s += in_data[in_idx + j * axis_stride];
      }
      if constexpr (op == Op::Mean) s /= static_cast<T>(axis_size);
      out_data[i] = s;
    } else if constexpr (op == Op::Max) {
      T best = in_data[in_idx];
      for (int64_t j = 1; j < axis_size; j++) {
        T v = in_data[in_idx + j * axis_stride];
        if (v > best) best = v;
      }
      out_data[i] = best;
    } else if constexpr (op == Op::Argmax) {
      T best_v = in_data[in_idx];
      int64_t best_j = 0;
      for (int64_t j = 1; j < axis_size; j++) {
        T v = in_data[in_idx + j * axis_stride];
        if (v > best_v) { best_v = v; best_j = j; }
      }
      out_data[i] = static_cast<T>(best_j);
    }
  }
}

template <typename T>
inline void _tensor_run_reduce_for_dtype(TensorImpl& out) {
  const auto& in = *out.inputs[0];
  switch (out.op) {
    case Op::Sum:    _tensor_run_reduce_axis<T, Op::Sum>(out, in); break;
    case Op::Mean:   _tensor_run_reduce_axis<T, Op::Mean>(out, in); break;
    case Op::Max:    _tensor_run_reduce_axis<T, Op::Max>(out, in); break;
    case Op::Argmax: _tensor_run_reduce_axis<T, Op::Argmax>(out, in); break;
    default:
      throw std::logic_error("tensor: non-reduction op in reduce dispatch");
  }
}

inline void _tensor_run_reduce(TensorImpl& t) {
  if (t.dtype == Dtype::F32) _tensor_run_reduce_for_dtype<float>(t);
  else                       _tensor_run_reduce_for_dtype<double>(t);
}

// Eager axis-less reduction → scalar double. Forces eval of the input
// then walks every element (respecting strides). Used by the .sum()
// / .mean() / .max() methods that return Culebra Float.
template <typename T, Op op>
inline T _tensor_reduce_all_typed(TensorImpl& in) {
  const T* d = in.data_as<T>();
  size_t total = in.shape.num_elements();
  // Fast path on contiguous storage: linear walk.
  if (in.is_contiguous()) {
    if constexpr (op == Op::Sum || op == Op::Mean) {
      T s = T{};
      for (size_t i = 0; i < total; i++) s += d[i];
      if constexpr (op == Op::Mean) s /= static_cast<T>(total);
      return s;
    } else {  // Max
      if (total == 0) return T{};
      T best = d[0];
      for (size_t i = 1; i < total; i++) if (d[i] > best) best = d[i];
      return best;
    }
  }
  // Strided walk: decompose linear index into multi-index.
  size_t rank = in.shape.dims.size();
  T s = T{};
  T best = T{};
  bool first = true;
  for (size_t i = 0; i < total; i++) {
    size_t rem = i;
    int64_t idx = 0;
    for (size_t dim = rank; dim-- > 0;) {
      auto m = static_cast<int64_t>(rem % in.shape.dims[dim]);
      rem /= in.shape.dims[dim];
      idx += m * in.strides[dim];
    }
    T v = d[idx];
    if constexpr (op == Op::Sum || op == Op::Mean) s += v;
    else if (first || v > best) { best = v; first = false; }
  }
  if constexpr (op == Op::Sum) return s;
  if constexpr (op == Op::Mean) return s / static_cast<T>(total);
  return best;  // Max
}

template <Op op>
inline double tensor_reduce_all(TensorPtr a) {
  tensor_eval_node(*a);
  if (a->dtype == Dtype::F32) {
    return static_cast<double>(_tensor_reduce_all_typed<float, op>(*a));
  }
  return _tensor_reduce_all_typed<double, op>(*a);
}

// Reverse all axes (a.T). For 1D it's a no-op view; for 2D it swaps
// rows and columns; for higher rank it fully reverses dims.
// Implicitly evaluates `t` so the view points at materialized data.
inline TensorPtr tensor_transpose(TensorPtr t) {
  tensor_eval_node(*t);
  std::vector<int64_t> new_dims(t->shape.dims.rbegin(), t->shape.dims.rend());
  std::vector<int64_t> new_strides(t->strides.rbegin(), t->strides.rend());
  return std::make_shared<TensorImpl>(
      TensorImpl::View{}, t, TensorShape(std::move(new_dims)),
      std::move(new_strides), 0);
}

// Slice along axis 0 (rows): [start, end). Matches Array's .slice()
// convention so behaviour is consistent across Culebra. Zero-copy.
inline TensorPtr tensor_slice(TensorPtr t, int64_t start, int64_t end) {
  tensor_eval_node(*t);
  if (t->shape.dims.empty()) {
    throw std::runtime_error("Tensor: slice on rank-0.");
  }
  if (start < 0 || end < start || end > t->shape.dims[0]) {
    throw std::runtime_error("Tensor: slice out of bounds.");
  }
  auto new_dims = t->shape.dims;
  new_dims[0] = end - start;
  auto new_strides = t->strides;
  size_t new_offset = static_cast<size_t>(start * t->strides[0]);
  return std::make_shared<TensorImpl>(
      TensorImpl::View{}, t, TensorShape(std::move(new_dims)),
      std::move(new_strides), new_offset);
}

// Build a lazy matrix multiply A @ B. A is [M, K], B is [K, N], the
// result is [M, N]. Phase 1 supports rank-2 only.
inline TensorPtr tensor_dot(TensorPtr a, TensorPtr b) {
  if (a->dtype != b->dtype) {
    throw std::runtime_error("Tensor: dtype mismatch in dot.");
  }
  if (a->shape.dims.size() != 2 || b->shape.dims.size() != 2) {
    throw std::runtime_error("Tensor: dot requires rank-2 inputs.");
  }
  if (a->shape.dims[1] != b->shape.dims[0]) {
    throw std::runtime_error(
        "Tensor: dot inner dims do not match (A.cols != B.rows).");
  }
  std::vector<int64_t> out_dims{a->shape.dims[0], b->shape.dims[1]};
  return std::make_shared<TensorImpl>(
      Op::Dot, TensorShape(std::move(out_dims)), a->dtype,
      std::vector<TensorPtr>{std::move(a), std::move(b)});
}

// True if `t` is a rank-2 transpose view of a contiguous rank-2
// matrix. cblas_*gemm can consume such inputs directly via a Trans
// flag — no materialization required.
inline bool _tensor_is_transpose_view(const TensorImpl& t) {
  if (!t.base || t.offset != 0) return false;
  if (t.shape.dims.size() != 2 || t.base->shape.dims.size() != 2) return false;
  if (t.strides.size() != 2 || t.base->strides.size() != 2) return false;
  return t.strides[0] == t.base->strides[1] &&
         t.strides[1] == t.base->strides[0] &&
         t.base->is_contiguous();
}

// Resolve a rank-2 input into a (data, lda, trans) trio for cblas.
// Throws if the input is neither contiguous nor a contiguous-transpose.
template <typename T>
struct _BlasInput {
  const T* data;
  int lda;
  CBLAS_TRANSPOSE trans;
};
template <typename T>
inline _BlasInput<T> _tensor_blas_input(const TensorImpl& t) {
  // Contiguous strides for the shape — direct NoTrans. Works for owned
  // buffers, slice views, and double-transposed views (which restore
  // contiguous layout). data_as<T>() folds in any base offset.
  if (t.shape.dims.size() == 2 && t.is_contiguous()) {
    return {t.data_as<T>(), static_cast<int>(t.shape.dims[1]), CblasNoTrans};
  }
  if (_tensor_is_transpose_view(t)) {
    return {t.base->data_as<T>(),
            static_cast<int>(t.base->shape.dims[1]), CblasTrans};
  }
  throw std::runtime_error(
      "Tensor: dot input layout not supported (M4 handles contiguous and "
      "simple-transpose views only).");
}

// Build a lazy unary node (one input, same shape and dtype).
inline TensorPtr tensor_unary(Op op, TensorPtr a) {
  auto shape = a->shape;
  auto dtype = a->dtype;
  return std::make_shared<TensorImpl>(
      op, std::move(shape), dtype,
      std::vector<TensorPtr>{std::move(a)});
}

template <typename T, Op op>
inline void _tensor_run_unary_typed(TensorImpl& out) {
  const auto& in = *out.inputs[0];
  size_t total = in.shape.num_elements();
  T* o = out.data_as<T>();
  auto apply = [](T x) -> T {
    if constexpr (op == Op::Sigmoid) {
      return T{1} / (T{1} + std::exp(-x));
    } else if constexpr (op == Op::Relu) {
      return x > T{0} ? x : T{0};
    } else {
      return T{};
    }
  };
  if (in.is_contiguous()) {
    const T* d = in.data_as<T>();
    for (size_t i = 0; i < total; i++) o[i] = apply(d[i]);
    return;
  }
  size_t rank = in.shape.dims.size();
  const T* d = in.data_as<T>();
  for (size_t i = 0; i < total; i++) {
    size_t rem = i;
    int64_t in_idx = 0;
    for (size_t dim = rank; dim-- > 0;) {
      auto m = static_cast<int64_t>(rem % in.shape.dims[dim]);
      rem /= in.shape.dims[dim];
      in_idx += m * in.strides[dim];
    }
    o[i] = apply(d[in_idx]);
  }
}

// Online stable softmax over the last axis. Equivalent to
// `exp(x - max(x)) / sum(exp(x - max(x)))` per row, with the max
// subtraction folded into a single pass.
template <typename T>
inline void _tensor_run_softmax_typed(TensorImpl& out) {
  const auto& in = *out.inputs[0];
  // Softmax walks rows of the last axis; that requires the row to
  // be contiguous in memory. is_contiguous() covers slice / reshape /
  // double-transpose views that happen to land on a contiguous layout.
  if (!in.is_contiguous()) {
    throw std::runtime_error(
        "Tensor: softmax requires contiguous input.");
  }
  const T* d = in.data_as<T>();
  T* o = out.data_as<T>();
  if (in.shape.dims.empty()) return;
  size_t axis_size = in.shape.dims.back();
  size_t total = in.shape.num_elements();
  size_t num_rows = axis_size > 0 ? total / axis_size : 0;
  for (size_t r = 0; r < num_rows; r++) {
    const T* row_in = d + r * axis_size;
    T* row_out = o + r * axis_size;
    T row_max = row_in[0];
    for (size_t i = 1; i < axis_size; i++) {
      if (row_in[i] > row_max) row_max = row_in[i];
    }
    T row_sum = T{0};
    for (size_t i = 0; i < axis_size; i++) {
      row_out[i] = std::exp(row_in[i] - row_max);
      row_sum += row_out[i];
    }
    for (size_t i = 0; i < axis_size; i++) {
      row_out[i] /= row_sum;
    }
  }
}

template <typename T>
inline void _tensor_run_unary_for_dtype(TensorImpl& t) {
  switch (t.op) {
    case Op::Sigmoid: _tensor_run_unary_typed<T, Op::Sigmoid>(t); break;
    case Op::Relu:    _tensor_run_unary_typed<T, Op::Relu>(t); break;
    case Op::Softmax: _tensor_run_softmax_typed<T>(t); break;
    default:
      throw std::logic_error("tensor: non-unary op in unary dispatch");
  }
}

inline void _tensor_run_unary(TensorImpl& t) {
  if (t.dtype == Dtype::F32) _tensor_run_unary_for_dtype<float>(t);
  else                       _tensor_run_unary_for_dtype<double>(t);
}

// Fused MLP forward: sigmoid(W @ x + b). Bias is broadcast against
// the output [M, N] (typical: b is [M, 1] or [M]).
inline TensorPtr tensor_linear_sigmoid(TensorPtr W, TensorPtr x, TensorPtr b) {
  if (W->dtype != x->dtype || W->dtype != b->dtype) {
    throw std::runtime_error("Tensor: dtype mismatch in linear_sigmoid.");
  }
  if (W->shape.dims.size() != 2 || x->shape.dims.size() != 2) {
    throw std::runtime_error(
        "Tensor: linear_sigmoid requires rank-2 W and x.");
  }
  if (W->shape.dims[1] != x->shape.dims[0]) {
    throw std::runtime_error(
        "Tensor: linear_sigmoid inner dims do not match.");
  }
  std::vector<int64_t> out_dims{W->shape.dims[0], x->shape.dims[1]};
  return std::make_shared<TensorImpl>(
      Op::LinearSigmoid, TensorShape(std::move(out_dims)), W->dtype,
      std::vector<TensorPtr>{std::move(W), std::move(x), std::move(b)});
}

template <typename T>
inline void _tensor_run_linear_sigmoid_typed(TensorImpl& out) {
  const auto& W = *out.inputs[0];
  const auto& x = *out.inputs[1];
  const auto& b = *out.inputs[2];
  int M = static_cast<int>(out.shape.dims[0]);
  int N = static_cast<int>(out.shape.dims[1]);
  int K = static_cast<int>(W.shape.dims[1]);
  // 1) GEMM: out = W @ x
  auto wA = _tensor_blas_input<T>(W);
  auto xB = _tensor_blas_input<T>(x);
  T* o = out.data_as<T>();
  if constexpr (std::is_same_v<T, float>) {
    cblas_sgemm(CblasRowMajor, wA.trans, xB.trans, M, N, K, 1.0f,
                wA.data, wA.lda, xB.data, xB.lda, 0.0f, o, N);
  } else {
    cblas_dgemm(CblasRowMajor, wA.trans, xB.trans, M, N, K, 1.0,
                wA.data, wA.lda, xB.data, xB.lda, 0.0, o, N);
  }
  // 2) Add broadcast bias + sigmoid in a single pass.
  auto bstr = _broadcast_strides(b.shape, b.strides, out.shape);
  const T* bd = b.data_as<T>();
  for (int i = 0; i < M; i++) {
    for (int j = 0; j < N; j++) {
      int64_t bi = i * bstr[0] + j * bstr[1];
      T v = o[i * N + j] + bd[bi];
      o[i * N + j] = T{1} / (T{1} + std::exp(-v));
    }
  }
}

inline void _tensor_run_linear_sigmoid(TensorImpl& t) {
  if (t.dtype == Dtype::F32) _tensor_run_linear_sigmoid_typed<float>(t);
  else                       _tensor_run_linear_sigmoid_typed<double>(t);
}

inline void _tensor_run_dot(TensorImpl& out) {
  const auto& A = *out.inputs[0];
  const auto& B = *out.inputs[1];
  int M = static_cast<int>(out.shape.dims[0]);
  int N = static_cast<int>(out.shape.dims[1]);
  int K = static_cast<int>(A.shape.dims[1]);
  if (out.dtype == Dtype::F32) {
    auto a = _tensor_blas_input<float>(A);
    auto b = _tensor_blas_input<float>(B);
    cblas_sgemm(CblasRowMajor, a.trans, b.trans, M, N, K, 1.0f,
                a.data, a.lda, b.data, b.lda, 0.0f, out.data_as<float>(), N);
  } else {
    auto a = _tensor_blas_input<double>(A);
    auto b = _tensor_blas_input<double>(B);
    cblas_dgemm(CblasRowMajor, a.trans, b.trans, M, N, K, 1.0,
                a.data, a.lda, b.data, b.lda, 0.0, out.data_as<double>(), N);
  }
}

// View with a different shape. M3.1 only allows contiguous inputs;
// reshaping a transposed/sliced view requires materialization, which
// is on the M3+ todo list.
inline TensorPtr tensor_reshape(TensorPtr t, TensorShape new_shape) {
  tensor_eval_node(*t);
  if (new_shape.num_elements() != t->shape.num_elements()) {
    throw std::runtime_error("Tensor: reshape element-count mismatch.");
  }
  if (!t->is_contiguous()) {
    throw std::runtime_error("Tensor: reshape requires a contiguous input.");
  }
  auto new_strides = tensor_contiguous_strides(new_shape);
  return std::make_shared<TensorImpl>(
      TensorImpl::View{}, t, std::move(new_shape), std::move(new_strides), 0);
}

// Topological evaluation: depth-first walk of inputs, allocate a
// contiguous buf for each unevaluated node, run kernel.
// is_evaluated() short-circuits shared subgraphs so each node runs
// exactly once across an eval batch (and across re-evals).
inline void tensor_eval_node(TensorImpl& t) {
  if (t.is_evaluated()) return;
  for (auto& in : t.inputs) tensor_eval_node(*in);
  t.buf.assign(t.shape.num_elements() * dtype_size(t.dtype), std::byte{});
  t.strides = tensor_contiguous_strides(t.shape);
  switch (t.op) {
    case Op::Add:
    case Op::Sub:
    case Op::Mul:
    case Op::Div:
    case Op::Pow:
      _tensor_run_binop(t);
      break;
    case Op::Sum:
    case Op::Mean:
    case Op::Max:
    case Op::Argmax:
      _tensor_run_reduce(t);
      break;
    case Op::Dot:
      _tensor_run_dot(t);
      break;
    case Op::Sigmoid:
    case Op::Relu:
    case Op::Softmax:
      _tensor_run_unary(t);
      break;
    case Op::LinearSigmoid:
      _tensor_run_linear_sigmoid(t);
      break;
    case Op::Const:
      break;  // unreachable: Const is_evaluated() above
  }
}

// "Tensor 3x4 f32" — used by interp and (in M1.1) JIT puts/str paths.
inline std::string tensor_str(const TensorImpl& t) {
  std::string s = "Tensor ";
  if (t.shape.dims.empty()) {
    s += "()";
  } else {
    for (size_t i = 0; i < t.shape.dims.size(); i++) {
      if (i > 0) s += "x";
      s += std::to_string(t.shape.dims[i]);
    }
  }
  s += " ";
  s += dtype_name(t.dtype);
  return s;
}

}  // namespace culebra
