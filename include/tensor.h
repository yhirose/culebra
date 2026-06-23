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

#include <shared.h>

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
#include <unordered_set>
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
  Sigmoid, Relu, Softmax, Log,
  LinearSigmoid,
  // Row concatenation (axis 0): stacks inputs vertically into one
  // materialized buffer. inputs = the parts; VJP slices the upstream
  // grad back into each part's row range.
  Concat,
  // Zero-copy views. These carry a non-Const op tag *and* a `base`
  // pointer: forward is short-circuited by is_evaluated() (base !=
  // nullptr), but the op tag lets autograd's reverse pass route the
  // gradient through the right view VJP. `inputs` is set to {base} so
  // the autograd graph is walked uniformly through `inputs`.
  Transpose, Reshape, Slice,
};

struct TensorShape {
  std::vector<int64_t> dims;

  TensorShape() = default;
  explicit TensorShape(std::vector<int64_t> d) : dims(std::move(d)) {
    for (auto x : dims) {
      if (x < 0) {
        throw CulebraError("ValueError", "Tensor: negative dim.");
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

  // --- Autograd ---
  // `grad` accumulates dL/dthis during backward(). It is always a
  // materialized Const tensor (own buffer, no graph) so it can never
  // form a cycle with the forward graph. nullptr until the first
  // accumulation. `requires_grad` is true for leaves the user marked
  // and propagates forward (a node requires grad iff any input does),
  // so backward() only allocates / accumulates grad where it matters.
  std::shared_ptr<TensorImpl> grad;
  bool requires_grad = false;

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

// No-grad scope: while the depth is > 0, ops do not propagate
// requires_grad to their outputs, so no autograd graph is tracked. Used
// for inference (Tensor.no_grad). Nesting is counted so guards compose;
// the guard is RAII so it restores the depth even if the body throws.
inline int& tensor_no_grad_depth() {
  thread_local int depth = 0;
  return depth;
}
struct TensorNoGradGuard {
  TensorNoGradGuard() { tensor_no_grad_depth()++; }
  ~TensorNoGradGuard() { tensor_no_grad_depth()--; }
  TensorNoGradGuard(const TensorNoGradGuard&) = delete;
  TensorNoGradGuard& operator=(const TensorNoGradGuard&) = delete;
};

// Propagate requires_grad forward: a node requires grad iff any of its
// inputs do. Op constructors call this on the node they return so the
// flag flows from leaves (marked by the user) up through the graph.
// Suppressed inside a no-grad scope.
inline const TensorPtr& tensor_propagate_grad(const TensorPtr& out) {
  if (tensor_no_grad_depth() > 0) return out;
  for (auto& in : out->inputs) {
    if (in && in->requires_grad) {
      out->requires_grad = true;
      break;
    }
  }
  return out;
}

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
    throw CulebraError("IOError",
                       "Tensor.from_csv: cannot open " + path);
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
        throw CulebraError("ValueError",
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
          throw CulebraError("ValueError",
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
// shared.h so `Random.seed(n)` makes Tensor.randn reproducible.
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
      throw CulebraError("ValueError",
                         "Tensor: shapes not broadcast-compatible.");
    }
    out[i] = std::max(ad, bd);
  }
  return TensorShape(std::move(out));
}

// Build a lazy elementwise binop node. Shapes are broadcast per numpy
// rules; dtype must match (no implicit promotion in Phase 1).
inline TensorPtr tensor_binop(Op op, TensorPtr a, TensorPtr b) {
  if (a->dtype != b->dtype) {
    throw CulebraError("ValueError", "Tensor: dtype mismatch in binop.");
  }
  auto shape = tensor_broadcast_shape(a->shape, b->shape);
  auto dtype = a->dtype;
  return tensor_propagate_grad(std::make_shared<TensorImpl>(
      op, std::move(shape), dtype,
      std::vector<TensorPtr>{std::move(a), std::move(b)}));
}

// Rank-0 Tensor holding a single scalar — used to lift scalar Float
// operands into the binop graph so broadcast does the rest.
inline TensorPtr tensor_scalar(double v, Dtype d) {
  auto t = std::make_shared<TensorImpl>(TensorShape{}, d);
  if (d == Dtype::F32) t->data_as<float>()[0] = static_cast<float>(v);
  else                 t->data_as<double>()[0] = v;
  return t;
}

// Soft cap so broadcast / kernel helpers can stack-allocate stride
// arrays. Phase 1 / MNIST stays at rank ≤ 4; raise if a future
// workload needs deeper tensors.
inline constexpr size_t kMaxTensorRank = 8;

// Strides into the input aligned to `out_shape` for broadcast.
// Caller provides an `out_strides` buffer (length kMaxTensorRank);
// the function fills the leading `out_shape.rank()` slots. Dims the
// input was broadcast across get stride 0. Avoids the per-call
// vector heap allocation that per-binop accumulates in MNIST.
inline void _broadcast_strides(int64_t* out_strides,
                                const TensorShape& in_shape,
                                const std::vector<int64_t>& in_strides,
                                const TensorShape& out_shape) {
  size_t out_rank = out_shape.dims.size();
  size_t in_rank = in_shape.dims.size();
  size_t leading = out_rank - in_rank;
  for (size_t i = 0; i < out_rank; i++) out_strides[i] = 0;
  for (size_t i = leading; i < out_rank; i++) {
    auto in_dim = in_shape.dims[i - leading];
    if (in_dim != 1) out_strides[i] = in_strides[i - leading];
  }
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
  int64_t astr[kMaxTensorRank];
  int64_t bstr[kMaxTensorRank];
  _broadcast_strides(astr, a_shape, astr_in, out_shape);
  _broadcast_strides(bstr, b_shape, bstr_in, out_shape);
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

// tensor_eval_node is the single cblas choke: every _tensor_run_* kernel
// (the only cblas callers) is reached only from here. To let `culebra
// build` drop BLAS for programs that never evaluate a tensor, its linkage
// is partitioned across the AOT runtime archives:
//   - core archive   (CULEBRA_RT_TENSOR_EVAL_WEAK):   weak throwing stub,
//     so the archive references no cblas symbol at all.
//   - tensor archive (CULEBRA_RT_TENSOR_EVAL_STRONG): strong real body,
//     force-loaded only when the program uses Tensor (overrides the stub).
//   - header-only / in-process JIT (neither): the normal inline body.
#if defined(CULEBRA_RT_TENSOR_EVAL_STRONG)
#define CULEBRA_RT_TENSOR_EVAL_LINKAGE
#elif defined(CULEBRA_RT_TENSOR_EVAL_WEAK)
#define CULEBRA_RT_TENSOR_EVAL_LINKAGE __attribute__((weak))
#else
#define CULEBRA_RT_TENSOR_EVAL_LINKAGE inline
#endif

CULEBRA_RT_TENSOR_EVAL_LINKAGE void tensor_eval_node(TensorImpl& t);

// In-place elementwise binop: `dst = dst OP rhs` writing into dst's
// own buffer. Skips the per-step allocation that `t = t + x` would
// do — meaningful for SGD-style weight updates over large tensors.
// Returns false (caller falls back to the lazy path) if dst is a
// view, lazy, dtype-mismatched, or the broadcast result would not
// fit in dst.shape — keeps semantics identical to the lazy form.
template <typename T>
inline void _tensor_inplace_for_dtype(Op op, TensorImpl& dst,
                                      const TensorImpl& rhs) {
  // dst is both the destination buffer and the lhs operand of the
  // binop, so the existing _tensor_run_binop_* helpers already do the
  // right thing: pass dst as out + a, rhs as b.
  auto* out = dst.data_as<T>();
  auto* a = out;
  const auto* b = rhs.data_as<T>();
  size_t n = dst.shape.num_elements();
  if (dst.shape == rhs.shape && rhs.is_contiguous()) {
    switch (op) {
      case Op::Add: _tensor_run_binop_same_shape<T, Op::Add>(out, a, b, n); break;
      case Op::Sub: _tensor_run_binop_same_shape<T, Op::Sub>(out, a, b, n); break;
      case Op::Mul: _tensor_run_binop_same_shape<T, Op::Mul>(out, a, b, n); break;
      case Op::Div: _tensor_run_binop_same_shape<T, Op::Div>(out, a, b, n); break;
      case Op::Pow: _tensor_run_binop_same_shape<T, Op::Pow>(out, a, b, n); break;
      default: throw std::logic_error("tensor: in-place on non-binop");
    }
    return;
  }
  // dst is contiguous (precondition), so its strides are the contiguous
  // strides for dst.shape — pass them as the lhs strides.
  switch (op) {
    case Op::Add:
      _tensor_run_binop_broadcast<T, Op::Add>(out, a, b, dst.strides,
          rhs.strides, dst.shape, rhs.shape, dst.shape); break;
    case Op::Sub:
      _tensor_run_binop_broadcast<T, Op::Sub>(out, a, b, dst.strides,
          rhs.strides, dst.shape, rhs.shape, dst.shape); break;
    case Op::Mul:
      _tensor_run_binop_broadcast<T, Op::Mul>(out, a, b, dst.strides,
          rhs.strides, dst.shape, rhs.shape, dst.shape); break;
    case Op::Div:
      _tensor_run_binop_broadcast<T, Op::Div>(out, a, b, dst.strides,
          rhs.strides, dst.shape, rhs.shape, dst.shape); break;
    case Op::Pow:
      _tensor_run_binop_broadcast<T, Op::Pow>(out, a, b, dst.strides,
          rhs.strides, dst.shape, rhs.shape, dst.shape); break;
    default: throw std::logic_error("tensor: in-place on non-binop");
  }
}

inline bool tensor_inplace_binop(TensorImpl& dst, Op op, TensorPtr rhs) {
  if (dst.base != nullptr) return false;
  if (!dst.is_contiguous()) return false;
  if (!dst.is_evaluated()) return false;
  if (dst.dtype != rhs->dtype) return false;
  auto out = tensor_broadcast_shape(dst.shape, rhs->shape);
  if (!(out == dst.shape)) return false;
  tensor_eval_node(*rhs);
  if (dst.dtype == Dtype::F32) {
    _tensor_inplace_for_dtype<float>(op, dst, *rhs);
  } else {
    _tensor_inplace_for_dtype<double>(op, dst, *rhs);
  }
  return true;
}

// Build a lazy reduction along `axis`. Output shape drops that axis
// (numpy's keepdims=false). Caller must validate `axis` ∈ [0, rank).
inline TensorPtr tensor_reduce_axis(Op op, TensorPtr a, int64_t axis) {
  if (axis < 0 || axis >= static_cast<int64_t>(a->shape.dims.size())) {
    throw CulebraError("IndexError", "Tensor: reduction axis out of range.");
  }
  std::vector<int64_t> out_dims;
  out_dims.reserve(a->shape.dims.size() - 1);
  for (size_t i = 0; i < a->shape.dims.size(); i++) {
    if (static_cast<int64_t>(i) != axis) out_dims.push_back(a->shape.dims[i]);
  }
  // Read dtype before std::move(a) — function-argument evaluation order
  // is indeterminate in C++17, and gcc happens to sequence the move
  // before the dtype read, leaving `a` moved-from at access time.
  auto dtype = a->dtype;
  auto t = std::make_shared<TensorImpl>(
      op, TensorShape(std::move(out_dims)), dtype,
      std::vector<TensorPtr>{std::move(a)});
  t->op_param = axis;
  return tensor_propagate_grad(t);
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

// Deep copy. Materializes the source if it's still lazy, then either
// memcpy's the contiguous buffer or walks strides for views.
template <typename T>
inline void _tensor_clone_typed(const TensorImpl& src, TensorImpl& dst) {
  auto n = src.shape.num_elements();
  const T* sp = src.data_as<T>();
  T* dp = dst.data_as<T>();
  if (src.is_contiguous() && src.base == nullptr) {
    std::memcpy(dp, sp, n * sizeof(T));
    return;
  }
  const auto& dims = src.shape.dims;
  const auto& str = src.strides;
  if (dims.empty()) {
    dp[0] = sp[0];  // rank-0 scalar (offset already folded into data_as)
  } else if (dims.size() == 1) {
    auto s0 = str[0];
    for (int64_t i = 0; i < dims[0]; i++) dp[i] = sp[i * s0];
  } else if (dims.size() == 2) {
    auto s0 = str[0], s1 = str[1];
    size_t k = 0;
    for (int64_t i = 0; i < dims[0]; i++) {
      for (int64_t j = 0; j < dims[1]; j++) dp[k++] = sp[i * s0 + j * s1];
    }
  } else {
    throw CulebraError("ValueError",
        "Tensor.clone: rank > 2 not supported in Phase 1.");
  }
}

inline TensorPtr tensor_clone(TensorPtr t) {
  tensor_eval_node(*t);
  auto out = std::make_shared<TensorImpl>(t->shape, t->dtype);
  if (t->dtype == Dtype::F32) _tensor_clone_typed<float>(*t, *out);
  else                        _tensor_clone_typed<double>(*t, *out);
  return out;
}

// Reverse all axes (a.T). For 1D it's a no-op view; for 2D it swaps
// rows and columns; for higher rank it fully reverses dims.
// Implicitly evaluates `t` so the view points at materialized data.
inline TensorPtr tensor_transpose(TensorPtr t) {
  tensor_eval_node(*t);
  std::vector<int64_t> new_dims(t->shape.dims.rbegin(), t->shape.dims.rend());
  std::vector<int64_t> new_strides(t->strides.rbegin(), t->strides.rend());
  auto out = std::make_shared<TensorImpl>(
      TensorImpl::View{}, t, TensorShape(std::move(new_dims)),
      std::move(new_strides), 0);
  out->op = Op::Transpose;
  out->inputs = {out->base};
  return tensor_propagate_grad(out);
}

// Slice along axis 0 (rows): [start, end). Matches Array's .slice()
// convention so behaviour is consistent across Culebra. Zero-copy.
inline TensorPtr tensor_slice(TensorPtr t, int64_t start, int64_t end) {
  tensor_eval_node(*t);
  if (t->shape.dims.empty()) {
    throw CulebraError("ValueError", "Tensor: slice on rank-0.");
  }
  if (start < 0 || end < start || end > t->shape.dims[0]) {
    throw CulebraError("IndexError", "Tensor: slice out of bounds.");
  }
  auto new_dims = t->shape.dims;
  new_dims[0] = end - start;
  auto new_strides = t->strides;
  size_t new_offset = static_cast<size_t>(start * t->strides[0]);
  auto out = std::make_shared<TensorImpl>(
      TensorImpl::View{}, t, TensorShape(std::move(new_dims)),
      std::move(new_strides), new_offset);
  out->op = Op::Slice;
  out->op_param = start;  // backward scatters g into rows [start, start+len)
  out->inputs = {out->base};
  return tensor_propagate_grad(out);
}

// Build a lazy matrix multiply A @ B. A is [M, K], B is [K, N], the
// result is [M, N]. Phase 1 supports rank-2 only.
inline TensorPtr tensor_dot(TensorPtr a, TensorPtr b) {
  if (a->dtype != b->dtype) {
    throw CulebraError("ValueError", "Tensor: dtype mismatch in dot.");
  }
  if (a->shape.dims.size() != 2 || b->shape.dims.size() != 2) {
    throw CulebraError("ValueError",
                       "Tensor: dot requires rank-2 inputs.");
  }
  if (a->shape.dims[1] != b->shape.dims[0]) {
    throw CulebraError("ValueError",
        "Tensor: dot inner dims do not match (A.cols != B.rows).");
  }
  std::vector<int64_t> out_dims{a->shape.dims[0], b->shape.dims[1]};
  // Read dtype before std::move(a) — see tensor_reduce_axis for the
  // gcc argument-evaluation-order rationale.
  auto dtype = a->dtype;
  return tensor_propagate_grad(std::make_shared<TensorImpl>(
      Op::Dot, TensorShape(std::move(out_dims)), dtype,
      std::vector<TensorPtr>{std::move(a), std::move(b)}));
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
  throw CulebraError("ValueError",
      "Tensor: dot input layout not supported (M4 handles contiguous and "
      "simple-transpose views only).");
}

// Build a lazy unary node (one input, same shape and dtype).
inline TensorPtr tensor_unary(Op op, TensorPtr a) {
  auto shape = a->shape;
  auto dtype = a->dtype;
  return tensor_propagate_grad(std::make_shared<TensorImpl>(
      op, std::move(shape), dtype,
      std::vector<TensorPtr>{std::move(a)}));
}

inline TensorPtr tensor_log(TensorPtr a) {
  return tensor_unary(Op::Log, std::move(a));
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
    } else if constexpr (op == Op::Log) {
      return std::log(x);
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
    throw CulebraError("ValueError",
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
    case Op::Log:     _tensor_run_unary_typed<T, Op::Log>(t); break;
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
    throw CulebraError("ValueError",
                       "Tensor: dtype mismatch in linear_sigmoid.");
  }
  if (W->shape.dims.size() != 2 || x->shape.dims.size() != 2) {
    throw CulebraError("ValueError",
        "Tensor: linear_sigmoid requires rank-2 W and x.");
  }
  if (W->shape.dims[1] != x->shape.dims[0]) {
    throw CulebraError("ValueError",
        "Tensor: linear_sigmoid inner dims do not match.");
  }
  std::vector<int64_t> out_dims{W->shape.dims[0], x->shape.dims[1]};
  // Read dtype before std::move(W) — see tensor_reduce_axis for the
  // gcc argument-evaluation-order rationale.
  auto dtype = W->dtype;
  return tensor_propagate_grad(std::make_shared<TensorImpl>(
      Op::LinearSigmoid, TensorShape(std::move(out_dims)), dtype,
      std::vector<TensorPtr>{std::move(W), std::move(x), std::move(b)}));
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
  int64_t bstr[kMaxTensorRank];
  _broadcast_strides(bstr, b.shape, b.strides, out.shape);
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
    throw CulebraError("ValueError",
                       "Tensor: reshape element-count mismatch.");
  }
  if (!t->is_contiguous()) {
    throw CulebraError("ValueError",
                       "Tensor: reshape requires a contiguous input.");
  }
  auto new_strides = tensor_contiguous_strides(new_shape);
  auto out = std::make_shared<TensorImpl>(
      TensorImpl::View{}, t, std::move(new_shape), std::move(new_strides), 0);
  out->op = Op::Reshape;
  out->inputs = {out->base};
  return tensor_propagate_grad(out);
}

// Row concat forward. Axis 0 is outermost, so each part's row-major
// element sequence maps to one contiguous output chunk; we read each
// part in logical row-major order (honoring view strides) and append.
template <typename T>
inline void _tensor_run_concat_typed(TensorImpl& out) {
  T* o = out.data_as<T>();
  size_t w = 0;
  for (const auto& inp : out.inputs) {
    const auto& p = *inp;
    size_t total = p.shape.num_elements();
    const T* d = p.data_as<T>();
    if (p.is_contiguous()) {
      for (size_t i = 0; i < total; i++) o[w++] = d[i];
      continue;
    }
    size_t rank = p.shape.dims.size();
    for (size_t i = 0; i < total; i++) {
      size_t rem = i;
      int64_t off = 0;
      for (size_t dim = rank; dim-- > 0;) {
        auto m = static_cast<int64_t>(rem % p.shape.dims[dim]);
        rem /= p.shape.dims[dim];
        off += m * p.strides[dim];
      }
      o[w++] = d[off];
    }
  }
}

inline void _tensor_run_concat(TensorImpl& t) {
  if (t.dtype == Dtype::F32) _tensor_run_concat_typed<float>(t);
  else                       _tensor_run_concat_typed<double>(t);
}

// Stack tensors along axis 0 (rows). All parts must share dtype and all
// dims past axis 0; the output's row count is the sum of the parts'.
inline TensorPtr tensor_concat(std::vector<TensorPtr> parts) {
  if (parts.empty()) {
    throw CulebraError("ValueError", "Tensor.concat: empty list.");
  }
  const auto& d0 = parts[0]->shape.dims;
  Dtype dt = parts[0]->dtype;
  int64_t total_rows = 0;
  for (const auto& p : parts) {
    if (p->dtype != dt) {
      throw CulebraError("ValueError", "Tensor.concat: dtype mismatch.");
    }
    const auto& pd = p->shape.dims;
    if (pd.size() != d0.size() || pd.empty()) {
      throw CulebraError("ValueError", "Tensor.concat: rank mismatch.");
    }
    for (size_t i = 1; i < pd.size(); i++) {
      if (pd[i] != d0[i]) {
        throw CulebraError("ValueError",
                           "Tensor.concat: non-axis dims must match.");
      }
    }
    total_rows += pd[0];
  }
  std::vector<int64_t> out_dims(d0.begin(), d0.end());
  out_dims[0] = total_rows;
  return tensor_propagate_grad(std::make_shared<TensorImpl>(
      Op::Concat, TensorShape(out_dims), dt, std::move(parts)));
}

// Topological evaluation: depth-first walk of inputs, allocate a
// contiguous buf for each unevaluated node, run kernel.
// is_evaluated() short-circuits shared subgraphs so each node runs
// exactly once across an eval batch (and across re-evals).
CULEBRA_RT_TENSOR_EVAL_LINKAGE void tensor_eval_node(TensorImpl& t) {
#ifdef CULEBRA_RT_TENSOR_EVAL_WEAK
  // The core runtime archive links no BLAS. This is the single entry to every
  // tensor kernel (cblas lives below here), so stubbing it keeps cblas out of
  // the archive. The Tensor namespace and the culebra_runtime_tensor_* helpers
  // stay real in core, but they reach cblas only through this function — with a
  // stub body the _tensor_run_* kernels go unreferenced and are never emitted.
  // The strong override (CULEBRA_RT_TENSOR_EVAL_STRONG) is force-loaded when the
  // program uses Tensor; this stub then never runs. Throw defensively in case
  // that invariant ever breaks.
  (void)t;
  throw CulebraError("InternalError",
                     "tensor runtime entered in a no-tensor binary", 0, 0);
#else
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
    case Op::Log:
    case Op::Softmax:
      _tensor_run_unary(t);
      break;
    case Op::LinearSigmoid:
      _tensor_run_linear_sigmoid(t);
      break;
    case Op::Concat:
      _tensor_run_concat(t);
      break;
    case Op::Const:
    case Op::Transpose:
    case Op::Reshape:
    case Op::Slice:
      break;  // unreachable: views / Const are is_evaluated() above
  }
#endif  // CULEBRA_RT_TENSOR_EVAL_WEAK
}

// ===================== Autograd (reverse mode) =====================
//
// The forward graph already recorded in `inputs`/`op` (with `base`
// mirrored into `inputs` for views) doubles as the autograd tape:
// tensor_eval_node leaves it intact and materializes every node's
// `buf`, so each VJP reads the forward values it needs straight from
// the inputs' (and the node's own) buffers. backward() walks the graph
// in reverse-topological order and routes the upstream gradient through
// each op's vector-Jacobian product, accumulating into `grad` — always
// a materialized Const (eager in-place add), so it can never form a
// cycle with the forward graph. All of this is pure C++ on TensorImpl;
// the interp / JIT wrappers are thin, so both backends stay symmetric.

// Sum `g` down to `target`, inverting a numpy broadcast (extra leading
// dims and dims where target is 1 are summed away). `g` must already be
// contiguous — the gradient tensors backward builds always are.
template <typename T>
inline void _tensor_unbroadcast_typed(const TensorImpl& g, TensorImpl& out) {
  const T* gp = g.data_as<T>();
  T* op = out.data_as<T>();
  size_t g_rank = g.shape.dims.size();
  size_t out_rank = out.shape.dims.size();
  size_t leading = g_rank - out_rank;
  size_t total = g.shape.num_elements();
  for (size_t i = 0; i < total; i++) {
    size_t rem = i;
    size_t out_idx = 0;
    for (size_t d = g_rank; d-- > 0;) {
      int64_t coord = static_cast<int64_t>(rem % g.shape.dims[d]);
      rem /= g.shape.dims[d];
      if (d >= leading) {
        size_t od = d - leading;
        int64_t c = (out.shape.dims[od] == 1) ? 0 : coord;
        out_idx += static_cast<size_t>(c) * static_cast<size_t>(out.strides[od]);
      }
    }
    op[out_idx] += gp[i];
  }
}

inline TensorPtr _tensor_unbroadcast(TensorPtr g, const TensorShape& target) {
  tensor_eval_node(*g);
  if (g->shape == target) return g;
  auto out = std::make_shared<TensorImpl>(target, g->dtype);  // zero-filled
  if (g->dtype == Dtype::F32) _tensor_unbroadcast_typed<float>(*g, *out);
  else                        _tensor_unbroadcast_typed<double>(*g, *out);
  return out;
}

// Accumulate `contrib` into node->grad, reducing it back to node's
// shape first. No-op when the node does not track grad. node->grad is
// always a materialized Const so the in-place add path always applies.
inline void _tensor_grad_add(const TensorPtr& node, TensorPtr contrib) {
  if (!node->requires_grad) return;
  contrib = _tensor_unbroadcast(std::move(contrib), node->shape);
  if (!node->grad) {
    node->grad = tensor_clone(contrib);
  } else {
    tensor_inplace_binop(*node->grad, Op::Add, contrib);
  }
}

// da = g * (x > 0), elementwise. Built directly (there is no Tensor
// comparison op) into a fresh contiguous buffer.
template <typename T>
inline void _tensor_relu_backward_typed(const TensorImpl& g,
                                        const TensorImpl& x, TensorImpl& out) {
  const T* gp = g.data_as<T>();
  const T* xp = x.data_as<T>();
  T* op = out.data_as<T>();
  size_t n = out.shape.num_elements();
  for (size_t i = 0; i < n; i++) op[i] = xp[i] > T(0) ? gp[i] : T(0);
}

inline TensorPtr _tensor_relu_backward(const TensorPtr& g, const TensorPtr& x) {
  tensor_eval_node(*g);
  tensor_eval_node(*x);
  auto out = std::make_shared<TensorImpl>(x->shape, x->dtype);
  if (x->dtype == Dtype::F32) _tensor_relu_backward_typed<float>(*g, *x, *out);
  else                        _tensor_relu_backward_typed<double>(*g, *x, *out);
  return out;
}

// Scatter `g` (the gradient of a row-slice) into rows [start, start+len)
// of a zero tensor shaped like the slice's source. Slices are along
// axis 0 of a contiguous source, so the destination rows form one
// contiguous block.
template <typename T>
inline void _tensor_scatter_rows_typed(TensorImpl& dst, const TensorImpl& g,
                                       int64_t start) {
  size_t row_block = static_cast<size_t>(dst.strides[0]);  // elems per row
  size_t off = static_cast<size_t>(start) * row_block;
  const T* gp = g.data_as<T>();
  T* dp = dst.data_as<T>();
  size_t n = g.shape.num_elements();
  for (size_t i = 0; i < n; i++) dp[off + i] = gp[i];
}

inline TensorPtr _tensor_slice_backward(const TensorPtr& g,
                                        const TensorShape& src_shape,
                                        Dtype dt, int64_t start) {
  tensor_eval_node(*g);
  auto out = std::make_shared<TensorImpl>(src_shape, dt);  // zero-filled
  if (dt == Dtype::F32) _tensor_scatter_rows_typed<float>(*out, *g, start);
  else                  _tensor_scatter_rows_typed<double>(*out, *g, start);
  return out;
}

// Reverse-topological DFS over the autograd graph (shared subgraphs —
// reused params, residuals — are visited once by pointer identity).
inline void _tensor_build_topo(const TensorPtr& n,
                               std::vector<TensorPtr>& topo,
                               std::unordered_set<TensorImpl*>& visited) {
  if (!visited.insert(n.get()).second) return;
  for (auto& in : n->inputs) {
    if (in) _tensor_build_topo(in, topo, visited);
  }
  topo.push_back(n);
}

// Route node->grad through one op's VJP into its inputs' grads.
// Local gradient of a sigmoid output y: dy/dx = y * (1 - y). Shared by the
// standalone Sigmoid and the fused LinearSigmoid VJP.
inline TensorPtr _tensor_sigmoid_grad(const TensorPtr& y, Dtype dt) {
  return tensor_binop(Op::Mul, y,
                      tensor_binop(Op::Sub, tensor_scalar(1.0, dt), y));
}

inline void _tensor_vjp(const TensorPtr& n) {
  const TensorPtr& g = n->grad;
  Dtype dt = n->dtype;
  auto neg = [&](TensorPtr t) {
    return tensor_binop(Op::Mul, std::move(t), tensor_scalar(-1.0, dt));
  };
  switch (n->op) {
    case Op::Const:
      break;  // leaf — just accumulates, user reads via .grad()
    case Op::Add:
      _tensor_grad_add(n->inputs[0], g);
      _tensor_grad_add(n->inputs[1], g);
      break;
    case Op::Sub:
      _tensor_grad_add(n->inputs[0], g);
      _tensor_grad_add(n->inputs[1], neg(g));
      break;
    case Op::Mul:
      _tensor_grad_add(n->inputs[0], tensor_binop(Op::Mul, g, n->inputs[1]));
      _tensor_grad_add(n->inputs[1], tensor_binop(Op::Mul, g, n->inputs[0]));
      break;
    case Op::Div: {
      const auto& a = n->inputs[0];
      const auto& b = n->inputs[1];
      _tensor_grad_add(a, tensor_binop(Op::Div, g, b));
      // db = -g * a / (b*b)
      auto bb = tensor_binop(Op::Mul, b, b);
      auto num = tensor_binop(Op::Mul, g, a);
      _tensor_grad_add(b, neg(tensor_binop(Op::Div, num, bb)));
      break;
    }
    case Op::Pow: {
      const auto& a = n->inputs[0];
      const auto& b = n->inputs[1];
      // da = g * b * a^(b-1)
      auto exp_m1 = tensor_binop(Op::Sub, b, tensor_scalar(1.0, dt));
      auto a_pow = tensor_binop(Op::Pow, a, exp_m1);
      _tensor_grad_add(a, tensor_binop(Op::Mul, tensor_binop(Op::Mul, g, b),
                                       a_pow));
      if (b->requires_grad) {
        throw CulebraError("ValueError",
            "Tensor.backward: grad w.r.t. a Pow exponent is unsupported.");
      }
      break;
    }
    case Op::Dot:
      // z = a @ b : da = g @ b^T, db = a^T @ g
      _tensor_grad_add(n->inputs[0],
                       tensor_dot(g, tensor_transpose(n->inputs[1])));
      _tensor_grad_add(n->inputs[1],
                       tensor_dot(tensor_transpose(n->inputs[0]), g));
      break;
    case Op::Sum:
    case Op::Mean: {
      const auto& a = n->inputs[0];
      int64_t axis = n->op_param;
      // Re-insert the reduced axis (size 1) then broadcast g back.
      auto kd = a->shape.dims;
      kd[axis] = 1;
      auto g_kd = tensor_reshape(g, TensorShape(kd));
      TensorPtr contrib =
          tensor_binop(Op::Add, tensor_zeros(a->shape, dt), g_kd);
      if (n->op == Op::Mean) {
        contrib = tensor_binop(
            Op::Mul, contrib,
            tensor_scalar(1.0 / static_cast<double>(a->shape.dims[axis]), dt));
      }
      _tensor_grad_add(a, contrib);
      break;
    }
    case Op::Relu:
      _tensor_grad_add(n->inputs[0], _tensor_relu_backward(g, n->inputs[0]));
      break;
    case Op::Log:
      // y = log(x); dx = g / x.
      _tensor_grad_add(n->inputs[0], tensor_binop(Op::Div, g, n->inputs[0]));
      break;
    case Op::Sigmoid: {
      // y = sigmoid(x); dx = g * y * (1 - y). y is this node's value.
      auto ym = _tensor_sigmoid_grad(n, dt);
      _tensor_grad_add(n->inputs[0], tensor_binop(Op::Mul, g, ym));
      break;
    }
    case Op::Softmax: {
      // y = softmax(x) over last axis; dx = y * (g - sum_axis(g*y)).
      const TensorPtr& y = n;
      int64_t last = static_cast<int64_t>(y->shape.dims.size()) - 1;
      auto gy = tensor_binop(Op::Mul, g, y);
      auto s = tensor_reduce_axis(Op::Sum, gy, last);
      auto kd = y->shape.dims;
      kd[last] = 1;
      auto s_kd = tensor_reshape(s, TensorShape(kd));
      auto diff = tensor_binop(Op::Sub, g, s_kd);
      _tensor_grad_add(n->inputs[0], tensor_binop(Op::Mul, y, diff));
      break;
    }
    case Op::LinearSigmoid: {
      // y = sigmoid(W@x + b); dpre = g * y*(1-y)
      const auto& W = n->inputs[0];
      const auto& x = n->inputs[1];
      const auto& b = n->inputs[2];
      auto ym = _tensor_sigmoid_grad(n, dt);
      auto dpre = tensor_binop(Op::Mul, g, ym);
      _tensor_grad_add(W, tensor_dot(dpre, tensor_transpose(x)));
      _tensor_grad_add(x, tensor_dot(tensor_transpose(W), dpre));
      _tensor_grad_add(b, dpre);  // un-broadcast sums columns back to b
      break;
    }
    case Op::Transpose:
      _tensor_grad_add(n->inputs[0], tensor_transpose(g));
      break;
    case Op::Reshape:
      _tensor_grad_add(n->inputs[0],
                       tensor_reshape(g, n->inputs[0]->shape));
      break;
    case Op::Slice:
      _tensor_grad_add(
          n->inputs[0],
          _tensor_slice_backward(g, n->inputs[0]->shape, dt, n->op_param));
      break;
    case Op::Concat: {
      // Each part owns a contiguous row range of the output; route that
      // slice of the upstream grad straight back to it.
      int64_t off = 0;
      for (const auto& part : n->inputs) {
        int64_t rows = part->shape.dims[0];
        _tensor_grad_add(part, tensor_slice(g, off, off + rows));
        off += rows;
      }
      break;
    }
    case Op::Max:
    case Op::Argmax:
      throw CulebraError("ValueError",
          "Tensor.backward: Max / Argmax are not differentiable.");
  }
}

// backward(): seed dL/droot = 1 and propagate through the whole graph.
inline void tensor_backward(const TensorPtr& root) {
  tensor_eval_node(*root);
  std::vector<TensorPtr> topo;
  std::unordered_set<TensorImpl*> visited;
  _tensor_build_topo(root, topo, visited);
  root->grad = tensor_ones(root->shape, root->dtype);
  for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
    const TensorPtr& n = *it;
    if (!n->requires_grad || !n->grad) continue;
    _tensor_vjp(n);
  }
}

// Read the gradient as a tensor — zeros (matching shape) if backward
// has not populated it yet, so `.grad()` is always a usable tensor.
inline TensorPtr tensor_grad(const TensorPtr& t) {
  if (t->grad) return t->grad;
  return tensor_zeros(t->shape, t->dtype);
}

// Clear the accumulated gradient. backward() reallocates lazily, so a
// null grad reads back as zeros until the next backward.
inline void tensor_zero_grad(const TensorPtr& t) { t->grad = nullptr; }

// Mark a leaf as requiring grad and return it (chainable). Forward op
// constructors propagate the flag to every dependent node.
inline const TensorPtr& tensor_requires_grad(const TensorPtr& t,
                                             bool on = true) {
  t->requires_grad = on;
  return t;
}

// Detach: a materialized copy with no graph and no grad tracking.
inline TensorPtr tensor_detach(const TensorPtr& t) {
  auto out = tensor_clone(t);
  out->requires_grad = false;
  return out;
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
