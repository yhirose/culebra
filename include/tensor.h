#pragma once

// Tensor primitive — culebra-side wrapper over cpp-tensorlib (M4).
//
// This header is independent of the interpreter / JIT Value layer. Since M4
// the execution engine is cpp-tensorlib (`tl::array`): lazy graph, fusion,
// and the CPU/Metal/CUDA backends all live there. What stays here is what
// is culebra's to own:
//   - the Op tape (`op` / `inputs` / `op_param`) that autograd's reverse
//     walk routes VJPs through,
//   - reverse-mode autograd itself (backward / VJPs / grad accumulation),
//   - dtype and shape surfaces, validation with culebra error kinds, and
//     the CSV / randn / fill entry points the stdlib and JIT call.
//
// Layering:
//   tensorlib (tl::)      <- storage, lazy graph + fusion, evaluation,
//                            backends (Accelerate/Metal now, CUDA in M6)
//   tensor.h              <- this file (TensorImpl = Op tape + tl::array)
//   stdlib_jit.h          <- Tensor namespace registration
//
// AOT feature gating (mirrors the Http/OpenSSL pattern): tensorlib is
// compiled in TL_RUNTIME_HOOKS mode, so creation and graph building
// reference no backend symbol; allocation/evaluation dispatch through
// hooks. `tensor_rt_bootstrap` — a weak no-op in the core archive, strong
// in the tensor feature object, inline in-process — installs the hooks on
// first tensor construction. A program that never touches Tensor links a
// core archive with zero Metal/Accelerate/evaluator references, and
// -dead_strip drops the helpers themselves.

#include <shared.h>

#define TL_RUNTIME_HOOKS
#include <tensorlib.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace culebra {

// F32 only. F64 was removed 2026-07: it does not exist on Metal and is
// 1/32-1/64 speed on consumer NVIDIA GPUs, so it would be a CPU-only
// dtype. The enum stays as the seam for a future BF16 storage type.
enum class Dtype { F32 };

inline const char* dtype_name(Dtype) { return "f32"; }

inline size_t dtype_size(Dtype) { return 4; }

inline std::optional<Dtype> parse_dtype(std::string_view s) {
  if (s == "f32") return Dtype::F32;
  return std::nullopt;
}

// Tensor op tag. `Const` means the tensor's value is (or will be, for
// gradient intermediates) materialized with no autograd routing; other tags
// tell backward() which VJP to apply. Forward computation itself is the
// tl::array graph — these tags exist for the reverse walk only.
enum class Op {
  Const,
  Add, Sub, Mul, Div, Pow,
  // Elementwise comparisons: y = (a OP b) ? 1 : 0. Not differentiable (the
  // VJP switch throws, matching Max/Argmax) -- ReLU/LeakyReLU/Clip's
  // backward gate is the concrete caller, and it composes the result with
  // Mul, never .backward()'s through a comparison itself.
  Gt, Lt, Ge, Le, Eq, Ne,
  // Axis reductions: op_param holds the reduction axis. The axis-less
  // form (`.sum()` / `.mean()` / `.max()`) skips the graph and runs
  // eagerly because it returns a scalar Float, not a Tensor.
  Sum, Mean, Max, Argmax,
  Dot,
  Sigmoid, Relu, Softmax, Log,
  Tanh, Sin, Cos,
  // clip(x, lo, hi). Forward-only (see the VJP switch) -- dezero's own
  // Clip class writes its own backward from Tensor.ge/le/*, the same
  // reason Unfold/Pad/Fold/Permute/Narrow/ScatterAxis are forward-only.
  Clip,
  LinearSigmoid,
  Concat,
  // Zero-copy views: forward is a tl view; the tag routes the gradient
  // through the right view VJP. `inputs` is set to {base} so the autograd
  // graph is walked uniformly through `inputs`.
  Transpose, Reshape, Slice,
  Unfold,  // also zero-copy (shares storage), grouped with the views above
           // in spirit; backward not implemented yet (see the VJP switch).
  Permute,  // general axis reorder — Transpose above is the "reverse every
            // axis" special case; this is any permutation. Also zero-copy,
            // also no backward yet (the inverse needs the axes back, which
            // nothing stores today — see the VJP switch).
  // im2col's own ops: both materialize a new buffer (never a view). No VJP
  // yet either — dezero's own conv layer (examples/dezero) runs its own
  // autograd around these rather than native .backward().
  Pad, Fold,
  // Elementwise select: y = cond != 0 ? a : b. Has a real VJP (da = g*cond,
  // db = g*(1-cond); cond itself gets no gradient, matching numpy/PyTorch's
  // own where()) — unlike the views/im2col ops above, this one differs
  // through cleanly since it's just two masked binops.
  Where,
  // Row gather/scatter along axis 0 (embedding-table lookup) and one-hot
  // scatter into a new trailing axis (a pooling-style backward's native
  // primitive). IndexSelect/IndexAdd are exact duals with real VJPs;
  // ScatterAxis is forward-only (see the VJP switch).
  IndexSelect, IndexAdd, ScatterAxis,
  Narrow,  // `.slice()` generalized to any axis. Zero-copy view, no
           // backward yet (see the VJP switch).
  // Rotary position embedding: pairs (j, j+D/2) of the last axis rotate by
  // a position/frequency-dependent angle. Forward-only for now (see the VJP
  // switch) -- same reason Unfold/Pad/Fold/Permute/Narrow/ScatterAxis are:
  // a training-time RoPE composes from existing differentiable primitives
  // (slice the two halves, multiply by precomputed cos/sin constant
  // tensors, concat back), so this fused native op is a decode/inference
  // fast path, not something .backward() needs to see through.
  Rope,
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

// Translate tensorlib exceptions to culebra error kinds. The agreed split:
// std::invalid_argument = user-reachable (shape/broadcast/axis) →
// ValueError; anything else escaping tl is a bug in tensor.h or tensorlib →
// InternalError.
template <typename F>
inline auto _tl_guard(F&& f) -> decltype(f()) {
  try {
    return f();
  } catch (const std::invalid_argument& e) {
    throw CulebraError("ValueError", std::string("Tensor: ") + e.what());
  } catch (const std::logic_error& e) {
    throw CulebraError("InternalError", std::string("Tensor: ") + e.what());
  }
}

// Feature-gate chokes (see the header comment). Same linkage partition as
// the original cblas choke:
//   - core archive   (CULEBRA_RT_TENSOR_EVAL_WEAK):   weak stubs — the
//     archive references no tensorlib backend symbol at all.
//   - tensor archive (CULEBRA_RT_TENSOR_EVAL_STRONG): strong real bodies,
//     force-loaded only when the program uses Tensor.
//   - header-only / in-process JIT (neither): normal inline bodies.
#if defined(CULEBRA_RT_TENSOR_EVAL_STRONG)
#define CULEBRA_RT_TENSOR_EVAL_LINKAGE
#elif defined(CULEBRA_RT_TENSOR_EVAL_WEAK)
#define CULEBRA_RT_TENSOR_EVAL_LINKAGE __attribute__((weak))
#else
#define CULEBRA_RT_TENSOR_EVAL_LINKAGE inline
#endif

// Install tensorlib's allocation/evaluation/barrier hooks (once). Called
// from every TensorImpl constructor so the very first tensor a program
// creates gets device-backed storage.
CULEBRA_RT_TENSOR_EVAL_LINKAGE void tensor_rt_bootstrap();

// Device selection (Tensor.use_cpu/use_gpu/use_auto/gpu_available). The
// setters only write tl's global device enum — backend-free, so they stay
// plain inline. gpu_available() reaches tl::gpu_available() ->
// metal::available(), which references the Metal device; it is choked like
// the eval entry so a no-tensor binary (weak stub -> false) links no Metal.
//
// `tl` defaults to cpu, the conservative default for a library that must not
// reach for a GPU behind its caller's back. Culebra's documented default is
// auto (docs/stdlib.md §8), applied in tensor_rt_bootstrap rather than at load
// time so a tensor-free binary still pulls in nothing. This flag is what keeps
// the two compatible: a use_*() before the first tensor has to survive that
// bootstrap instead of being overwritten by it.
inline bool tensor_device_chosen = false;
inline void tensor_use_cpu() { tensor_device_chosen = true; tl::use_cpu(); }
inline void tensor_use_gpu() { tensor_device_chosen = true; tl::use_gpu(); }
inline void tensor_use_auto() { tensor_device_chosen = true; tl::use_auto(); }
CULEBRA_RT_TENSOR_EVAL_LINKAGE bool tensor_gpu_available();

// Tensor.device — the selection in effect, not the device a given op ran on
// (under "auto" that is decided per op). Bootstraps first: the default is
// installed lazily, so reading tl directly would answer "cpu" for a program
// that has not built a tensor yet and "auto" for the same program one line
// later. Going through the choke keeps one source of truth for the default.
inline const char* tensor_device() {
  tensor_rt_bootstrap();
  switch (tl::device_) {
    case tl::device_type::gpu:
      return "gpu";
    case tl::device_type::auto_:
      return "auto";
    default:
      return "cpu";
  }
}

struct TensorImpl;

// CPython-trashcan teardown for TensorImpl chains: a long unbroken
// requires_grad graph (RNN-style unrolling — `x = x + step` in a loop, never
// detached) is a linear parent->input chain, and dropping it recursed the
// implicit destructor through `inputs` to a C-stack overflow past a few
// hundred thousand nodes (measured: ~160-180k). Mirrors interp's ~Value and
// the JIT's release_impl: past kValueTeardownDepthBudget, a node's `inputs`
// moves whole into a thread-local deferred list drained at the outermost
// teardown level. A dtor cannot throw, so this bound is structural, not a
// catchable error like the graph-walk fix below.
using TensorInputs = std::vector<std::shared_ptr<TensorImpl>>;

// Clearing the moved-out inputs is the release: each shared_ptr that hits
// refcount 0 recurses back into ~TensorImpl, which is depth-bounded the
// same way.
inline void _tensor_teardown_release(TensorInputs& inputs) { inputs.clear(); }

using TensorTrashcan = Trashcan<TensorInputs, &_tensor_teardown_release>;

// Tensor — an Op-tagged autograd tape node whose value is a tl::array
// (lazy graph node, zero-copy view, or materialized buffer).
//
// Lifetime: shared via shared_ptr. Cycles are impossible because `inputs`
// points input-ward and `grad` is always a Const with no inputs.
struct TensorImpl {
  Op op = Op::Const;
  TensorShape shape;              // mirror of value.shape(); set at build
  Dtype dtype = Dtype::F32;
  tl::array value;
  TensorInputs inputs;

  // Op-specific scalar parameter. Reduction axis for Sum/Mean/Max/Argmax;
  // start row for Slice.
  int64_t op_param = 0;

  // Op-specific scalar parameters, generic float slots for whichever op
  // needs one back post-forward (mirrors cpp-tensorlib's own node::arg0/
  // arg1). Clip stores its lo/hi here for its VJP mask; Narrow stores
  // start/end (op_param already holds axis) for its own VJP, a zero-pad
  // back to the original axis length — op_param above is a single int64_t
  // already spoken for, and the forward graph itself only keeps these
  // inside the tl::array node.
  float extra0 = 0.0f;
  float extra1 = 0.0f;

  // --- Autograd ---
  // `grad` accumulates dL/dthis during backward(). It is always a
  // materialized Const tensor (own buffer, no graph) so it can never
  // form a cycle with the forward graph. nullptr until the first
  // accumulation. `requires_grad` is true for leaves the user marked
  // and propagates forward (a node requires grad iff any input does).
  std::shared_ptr<TensorImpl> grad;
  bool requires_grad = false;

  // True for zero-copy views (Transpose/Reshape/Slice): in-place writes
  // through a view would mutate the base, so tensor_inplace_binop refuses.
  bool is_view = false;

  // Materialized ctor: shape + dtype + zero-initialized buffer.
  TensorImpl(TensorShape s, Dtype d) : shape(std::move(s)), dtype(d) {
    tensor_rt_bootstrap();
    value = _tl_guard([&] { return tl::array::zeros(shape.dims); });
  }

  // Engine-node ctor: op tag + built tl expression + autograd inputs.
  // Shape is read back from the tl graph (known at build time).
  TensorImpl(Op o, tl::array v, Dtype d,
             TensorInputs ins, bool view = false)
      : op(o),
        dtype(d),
        value(std::move(v)),
        inputs(std::move(ins)),
        is_view(view) {
    tensor_rt_bootstrap();
    shape = TensorShape(std::vector<int64_t>(value.shape()));
  }

  // See TensorTrashcan above. A leaf (Const, no inputs — every `grad` tensor
  // is one by construction, so this never re-enters for those) returns
  // immediately; only a chain node pays the budget check. Handing `inputs`
  // over as an rvalue before it would run its own (recursive) destruction is
  // what lets the deferred case skip that recursion entirely — the member's
  // post-body destruction, now empty, is a no-op.
  ~TensorImpl() {
    if (inputs.empty()) return;
    TensorTrashcan::release(std::move(inputs));
  }

  // Element strides (tl's, no mirror — a per-op vector copy was measurable
  // in tiny-tensor workloads).
  const std::vector<int64_t>& strides() const { return value.strides(); }

  bool is_evaluated() const { return value.materialized(); }
  bool is_contiguous() const { return value.contiguous(); }

  // Strided base pointer (forces evaluation; offset folded in). Writable
  // for creation-time fills; mutating storage shared with a pending lazy
  // graph is the caller's hazard, same as it always was.
  void* data() { return data_as<float>(); }
  const void* data() const {
    return const_cast<TensorImpl*>(this)->data();
  }

  template <typename T>
  T* data_as() {
    return const_cast<float*>(value.raw());
  }
  template <typename T>
  const T* data_as() const {
    return value.raw();
  }
};

using TensorPtr = std::shared_ptr<TensorImpl>;

// Force evaluation of a tensor's graph. Kept as the named entry point the
// interp/JIT call before reading buffers; data_as() also evaluates, so this
// is now about intent (and the historical choke name), not reachability —
// that job moved to tensor_rt_bootstrap.
CULEBRA_RT_TENSOR_EVAL_LINKAGE void tensor_eval_node(TensorImpl& t);

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

// Single source for every op-result node. A node requires grad iff any of
// its inputs does (and we are not in a no-grad scope); the flag flows from
// leaves the user marked up through the graph.
//
// The autograd tape (op tag + `inputs` + op_param) is recorded ONLY when the
// result requires grad — backward walks requires_grad nodes only, so a
// forward-only result never needs it. A non-grad result is built as a plain
// Const leaf: no `inputs` chain to keep alive or tear down (the tl::array
// value carries the real compute graph independently), and op=Const so a
// later `.requires_grad()` + `.backward()` sees a well-formed leaf rather
// than an op tag with no operands. `is_view` is preserved regardless — it
// governs in-place-write refusal, not autograd.
inline TensorPtr tensor_make_op(Op op, tl::array value, Dtype dtype,
                                std::vector<TensorPtr> inputs,
                                int64_t op_param = 0, bool is_view = false) {
  bool rg = false;
  if (tensor_no_grad_depth() == 0) {
    for (auto& in : inputs) {
      if (in && in->requires_grad) {
        rg = true;
        break;
      }
    }
  }
  if (!rg) {
    return std::make_shared<TensorImpl>(Op::Const, std::move(value), dtype,
                                        std::vector<TensorPtr>{}, is_view);
  }
  auto out = std::make_shared<TensorImpl>(op, std::move(value), dtype,
                                          std::move(inputs), is_view);
  out->op_param = op_param;
  out->requires_grad = true;
  return out;
}

// Wrap a tl value as a Const node (gradient intermediates, clones).
inline TensorPtr _tensor_wrap_const(tl::array v, Dtype d) {
  return std::make_shared<TensorImpl>(Op::Const, std::move(v), d,
                                      std::vector<TensorPtr>{});
}

inline TensorPtr tensor_zeros(TensorShape s, Dtype d) {
  return std::make_shared<TensorImpl>(std::move(s), d);
}

// Shared between the interp `tensor_ones` and the JIT runtime entry —
// keeps the fill in one place.
inline void tensor_fill_ones_inplace(TensorImpl& t) {
  std::fill_n(t.data_as<float>(), t.shape.num_elements(), 1.0f);
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
    t->data_as<float>()[idx] = static_cast<float>(v);
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
  auto* p = t->data_as<float>();
  for (size_t i = 0; i < n; i++) p[i] = static_cast<float>(dist(engine));
  return t;
}

// Allocation-free validation twin of tensor_broadcast_shape: hot op
// builders only need the throw, not the shape (tensorlib recomputes it).
inline void tensor_broadcast_check(const TensorShape& a,
                                   const TensorShape& b) {
  if (a == b || a.dims.empty() || b.dims.empty()) return;
  size_t ar = a.dims.size();
  size_t br = b.dims.size();
  size_t out_rank = std::max(ar, br);
  for (size_t i = 0; i < out_rank; i++) {
    int64_t ad = (i + ar >= out_rank) ? a.dims[i - (out_rank - ar)] : 1;
    int64_t bd = (i + br >= out_rank) ? b.dims[i - (out_rank - br)] : 1;
    if (ad != bd && ad != 1 && bd != 1) {
      throw CulebraError("ValueError",
                         "Tensor: shapes not broadcast-compatible.");
    }
  }
}

// numpy / silarray broadcast rules: align trailing dims; each pair
// must be equal or one side must be 1. Output dim is the max. Empty
// (rank-0) shapes act as scalars. Kept culebra-side for the error kind
// and message; tensorlib revalidates internally.
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

inline tl::array _tl_binop(Op op, const tl::array& a, const tl::array& b) {
  switch (op) {
    case Op::Add: return a + b;
    case Op::Sub: return a - b;
    case Op::Mul: return a * b;
    case Op::Div: return a / b;
    case Op::Pow: return tl::pow(a, b);
    case Op::Gt: return a > b;
    case Op::Lt: return a < b;
    case Op::Ge: return a >= b;
    case Op::Le: return a <= b;
    case Op::Eq: return a == b;
    case Op::Ne: return a != b;
    default:
      throw std::logic_error("tensor: non-binop op in binop dispatch");
  }
}

// Rank-0 materialized operand: read its value and use tl's scalar
// overloads. This is not a micro-optimization — the scalar forms compose
// into affine epilogues (e.g. `dot(...) * lr` folds into the GEMM store;
// scalar chains collapse to one node), where the lifted rank-0 tensor
// form runs a broadcast kernel over the large side. `t * 2.0` from the
// language and every scalar the VJPs build (lr, -1, 1/n) hit this.
// The Op tape keeps the rank-0 tensor as a real input either way, so
// autograd is unaffected.
inline tl::array _tl_binop_vs_scalar(Op op, const tl::array& a, float s,
                                     bool scalar_on_left) {
  if (!scalar_on_left) {
    switch (op) {
      case Op::Add: return a + s;
      case Op::Sub: return a - s;
      case Op::Mul: return a * s;
      case Op::Div: return a / s;
      default: break;
    }
  } else {
    switch (op) {
      case Op::Add: return s + a;
      case Op::Sub: return s - a;
      case Op::Mul: return s * a;
      case Op::Div: return s / a;
      default: break;
    }
  }
  throw std::logic_error("tensor: bad op in scalar binop dispatch");
}

// Ops _tl_binop_vs_scalar has no case for: Pow needs tl::pow(a,b), not an
// operator, and the comparisons need correct operand-order flipping for
// the scalar_on_left direction (`s > b` is `b < s`, not `b > s`) that
// isn't worth the risk for a path that is a fusion micro-optimization,
// not a correctness requirement -- these just take the general _tl_binop
// path below instead.
inline bool _tl_binop_has_scalar_fast_path(Op op) {
  switch (op) {
    case Op::Pow:
    case Op::Gt:
    case Op::Lt:
    case Op::Ge:
    case Op::Le:
    case Op::Eq:
    case Op::Ne:
      return false;
    default:
      return true;
  }
}

// Build a lazy elementwise binop node. Shapes are broadcast per numpy
// rules; dtype must match (no implicit promotion in Phase 1).
inline TensorPtr tensor_binop(Op op, TensorPtr a, TensorPtr b) {
  if (a->dtype != b->dtype) {
    throw CulebraError("ValueError", "Tensor: dtype mismatch in binop.");
  }
  tensor_broadcast_check(a->shape, b->shape);  // culebra-worded error
  bool fast_path_ok = _tl_binop_has_scalar_fast_path(op);
  bool b_scalar = fast_path_ok && b->shape.rank() == 0 &&
                  b->value.materialized();
  bool a_scalar = fast_path_ok && a->shape.rank() == 0 &&
                  a->value.materialized() && b->shape.rank() != 0;
  auto v = _tl_guard([&] {
    if (b_scalar) return _tl_binop_vs_scalar(op, a->value, b->value.raw()[0],
                                             /*scalar_on_left=*/false);
    if (a_scalar) return _tl_binop_vs_scalar(op, b->value, a->value.raw()[0],
                                             /*scalar_on_left=*/true);
    return _tl_binop(op, a->value, b->value);
  });
  auto dtype = a->dtype;
  return tensor_make_op(op, std::move(v), dtype,
                        std::vector<TensorPtr>{std::move(a), std::move(b)});
}

// Rank-0 Tensor holding a single scalar — used to lift scalar Float
// operands into the binop graph so broadcast does the rest.
inline TensorPtr tensor_scalar(double v, Dtype d) {
  auto t = std::make_shared<TensorImpl>(TensorShape{}, d);
  t->data_as<float>()[0] = static_cast<float>(v);
  return t;
}

// In-place elementwise binop: `dst = dst OP rhs` writing into dst's
// own buffer. Skips the per-step allocation that `t = t + x` would
// do — meaningful for SGD-style weight updates over large tensors.
// Returns false (caller falls back to the lazy path) if dst is a
// view, lazy, dtype-mismatched, or the broadcast result would not
// fit in dst.shape — keeps semantics identical to the lazy form.
inline bool tensor_inplace_binop(TensorImpl& dst, Op op, TensorPtr rhs) {
  if (dst.is_view) return false;
  if (!dst.value.materialized()) return false;
  if (!dst.is_contiguous()) return false;
  if (dst.dtype != rhs->dtype) return false;
  auto out = tensor_broadcast_shape(dst.shape, rhs->shape);
  if (!(out == dst.shape)) return false;
  _tl_guard([&] {
    if (op == Op::Add) {
      dst.value.add_(rhs->value);
    } else {
      // Materialize OP into its own buffer first, then overwrite dst —
      // the lazy node reads dst as a constant input, so ordering matters.
      auto r = _tl_binop(op, dst.value, rhs->value);
      const float* src = r.raw();  // forces evaluation
      std::memcpy(dst.value.data(), src, dst.shape.num_elements() * 4);
    }
    return 0;
  });
  return true;
}

// Build a lazy reduction along `axis`. Output shape drops that axis
// (numpy's keepdims=false). Caller must validate `axis` ∈ [0, rank).
inline TensorPtr tensor_reduce_axis(Op op, TensorPtr a, int64_t axis) {
  if (axis < 0 || axis >= static_cast<int64_t>(a->shape.dims.size())) {
    throw CulebraError("IndexError", "Tensor: reduction axis out of range.");
  }
  auto v = _tl_guard([&]() -> tl::array {
    int ax = static_cast<int>(axis);
    switch (op) {
      case Op::Sum: return a->value.sum(ax);
      case Op::Mean: return a->value.mean(ax);
      case Op::Max: return a->value.max(ax);
      case Op::Argmax: return a->value.argmax(ax);
      default:
        throw std::logic_error("tensor: non-reduction op in reduce dispatch");
    }
  });
  auto dtype = a->dtype;
  return tensor_make_op(op, std::move(v), dtype,
                        std::vector<TensorPtr>{std::move(a)}, axis);
}

// Eager axis-less reduction → scalar double. Used by the .sum() /
// .mean() / .max() methods that return Culebra Float.
template <Op op>
inline double tensor_reduce_all(TensorPtr a) {
  tensor_eval_node(*a);
  return _tl_guard([&]() -> double {
    if constexpr (op == Op::Sum) {
      return static_cast<double>(a->value.sum());
    } else if constexpr (op == Op::Mean) {
      return static_cast<double>(a->value.mean());
    } else {  // Max — empty tensors read as 0, matching the pre-M4 kernel.
      if (a->value.size() == 0) return 0.0;
      return static_cast<double>(a->value.max());
    }
  });
}

// Deep copy: a materialized Const with no graph and no grad tracking.
inline TensorPtr tensor_clone(TensorPtr t) {
  auto v = _tl_guard([&] { return t->value.clone(); });
  return _tensor_wrap_const(std::move(v), t->dtype);
}

// Reverse all axes (a.T). For 1D it's a no-op view; for 2D it swaps
// rows and columns; for higher rank it fully reverses dims.
// Implicitly evaluates `t` so the view points at materialized data.
inline TensorPtr tensor_transpose(TensorPtr t) {
  auto v = _tl_guard([&] { return t->value.transpose(); });
  auto dtype = t->dtype;
  return tensor_make_op(Op::Transpose, std::move(v), dtype,
                        std::vector<TensorPtr>{std::move(t)},
                        /*op_param=*/0, /*is_view=*/true);
}

// A permutation is packed four bits per axis into `op_param` so the VJP can
// read it back; -1 marks "not packed" (rank too wide), which is the only
// case that stays forward-only. Fifteen axes stop at bit 59, leaving the
// sign bit alone so the packed value is always non-negative and cannot
// collide with the sentinel.
inline constexpr size_t kPermuteMaxPackedRank = 15;
inline constexpr int64_t kPermuteNotPacked = -1;

// General axis reorder: `axes[i]` names which of `t`'s own axes becomes the
// result's axis `i`. Zero-copy, like transpose() above. tensorlib's own
// array::transpose(axes) only checks the count matches rank — it does not
// check the values are a genuine permutation, and indexes shape/strides
// with them directly, so an out-of-range or repeated axis there is
// undefined behavior rather than a clean throw. That validation belongs
// here, at the boundary, before it ever reaches tensorlib.
inline TensorPtr tensor_permute(TensorPtr t, std::vector<int64_t> axes) {
  size_t r = t->shape.rank();
  if (axes.size() != r) {
    throw CulebraError("ValueError",
                       "Tensor.permute: expected " + std::to_string(r) +
                           " axes, got " + std::to_string(axes.size()) + ".");
  }
  std::vector<int> iaxes(r);
  std::vector<bool> seen(r, false);
  for (size_t i = 0; i < r; i++) {
    int64_t a = axes[i];
    if (a < 0) a += static_cast<int64_t>(r);
    if (a < 0 || a >= static_cast<int64_t>(r) || seen[static_cast<size_t>(a)]) {
      throw CulebraError("ValueError",
                         "Tensor.permute: axes must be a permutation of "
                         "0.." + std::to_string(r - 1) + ".");
    }
    seen[static_cast<size_t>(a)] = true;
    iaxes[i] = static_cast<int>(a);
  }
  // The VJP is the inverse permutation, so it needs these axes back, and
  // four bits each is enough to pack one into op_param (see the constants
  // above). A rank past that keeps the old forward-only behaviour rather
  // than storing a truncated permutation.
  int64_t packed = kPermuteNotPacked;
  if (r <= kPermuteMaxPackedRank) {
    packed = 0;
    for (size_t i = 0; i < r; i++) {
      packed |= static_cast<int64_t>(iaxes[i]) << (4 * i);
    }
  }
  auto v = _tl_guard([&] { return t->value.transpose(std::move(iaxes)); });
  auto dtype = t->dtype;
  return tensor_make_op(Op::Permute, std::move(v), dtype,
                        std::vector<TensorPtr>{std::move(t)}, packed,
                        /*is_view=*/true);
}

// Slice along axis 0 (rows): [start, end). Matches Array's .slice()
// convention so behaviour is consistent across Culebra. Zero-copy.
inline TensorPtr tensor_slice(TensorPtr t, int64_t start, int64_t end) {
  if (t->shape.dims.empty()) {
    throw CulebraError("ValueError", "Tensor: slice on rank-0.");
  }
  if (start < 0 || end < start || end > t->shape.dims[0]) {
    throw CulebraError("IndexError", "Tensor: slice out of bounds.");
  }
  auto v = _tl_guard([&] { return t->value.slice(start, end - start); });
  auto dtype = t->dtype;
  // op_param = start: backward scatters g into rows [start, start+len).
  return tensor_make_op(Op::Slice, std::move(v), dtype,
                        std::vector<TensorPtr>{std::move(t)},
                        /*op_param=*/start, /*is_view=*/true);
}

// `.slice()`'s own axis-0 reach, generalized to any axis (PyTorch's
// `Tensor.narrow`) — a distinct name rather than an overload of `.slice()`,
// since that name is also shared with Array/String's own axis-0-only
// `.slice()` through one polymorphic BMeth id. Zero-copy view: tensorlib's
// own `array::slice(axis, start, count)` has always supported this: only
// this binding was axis-0-only. Forward-only for now (no caller needs a
// gradient through it yet — it's a host-loop replacement inside a hand-
// derived dezero backward, the same role permute/unfold/pad/fold play in
// im2col/col2im/pooling).
inline TensorPtr tensor_narrow(TensorPtr t, int64_t axis, int64_t start,
                               int64_t end) {
  int64_t rank = static_cast<int64_t>(t->shape.dims.size());
  if (axis < 0) axis += rank;
  if (axis < 0 || axis >= rank) {
    throw CulebraError("IndexError", "Tensor: narrow: axis out of range.");
  }
  if (start < 0 || end < start || end > t->shape.dims[axis]) {
    throw CulebraError("IndexError", "Tensor: narrow: out of bounds.");
  }
  auto v = _tl_guard([&] {
    return t->value.slice(static_cast<int>(axis), start, end - start);
  });
  auto dtype = t->dtype;
  // op_param/extra0/extra1 keep axis/start/end for the VJP (a zero-pad
  // back out to the original axis length) -- see its own comment. start/
  // end ride the same float slots Clip's lo/hi use; both are small
  // integer shape indices, well inside float's exact-integer range.
  auto out = tensor_make_op(Op::Narrow, std::move(v), dtype,
                            std::vector<TensorPtr>{std::move(t)},
                            /*op_param=*/axis, /*is_view=*/true);
  out->extra0 = static_cast<float>(start);
  out->extra1 = static_cast<float>(end);
  return out;
}

// Sliding-window view along `axis`: shape gains a trailing size-`win` axis,
// the `axis` dim becomes the window count. Zero-copy (shares storage with
// `t`) when `t` is contiguous, same as tensorlib's own array::unfold.
// im2col's own building block — see examples/dezero/dezero/functions_conv.cul.
inline TensorPtr tensor_unfold(TensorPtr t, int64_t axis, int64_t win,
                               int64_t step) {
  auto v = _tl_guard([&] { return t->value.unfold(axis, win, step); });
  auto dtype = t->dtype;
  return tensor_make_op(Op::Unfold, std::move(v), dtype,
                        std::vector<TensorPtr>{std::move(t)},
                        /*op_param=*/0, /*is_view=*/true);
}

// Places `t` into a zero buffer, shifted by `before` along `axis` — im2col's
// own padding step. Never a view: pad always materializes a new buffer.
inline TensorPtr tensor_pad(TensorPtr t, int64_t axis, int64_t before,
                            int64_t after) {
  auto v = _tl_guard([&] { return t->value.pad(axis, before, after); });
  auto dtype = t->dtype;
  return tensor_make_op(Op::Pad, std::move(v), dtype,
                        std::vector<TensorPtr>{std::move(t)});
}

// unfold's inverse: scatter-add `t` (shaped like some x.unfold(axis, win,
// step)) back into a buffer of size `orig_size` along `axis`, accumulating
// every window overlap — col2im's own building block.
inline TensorPtr tensor_fold(TensorPtr t, int64_t axis, int64_t orig_size,
                             int64_t step) {
  auto v = _tl_guard([&] { return t->value.fold(axis, orig_size, step); });
  auto dtype = t->dtype;
  return tensor_make_op(Op::Fold, std::move(v), dtype,
                        std::vector<TensorPtr>{std::move(t)});
}

// Elementwise select, broadcasting `cond`/`a`/`b` against each other the
// same way a binop does: y[i] = cond[i] != 0 ? a[i] : b[i]. The building
// block for masking (attention masks, padding masks) — differentiable, see
// the VJP switch. tl::where has no GPU dispatch yet (always a host loop
// over the CPU buffer, even in GPU mode) — see docs/internals/vm.md's
// cpp-tensorlib gaps note.
inline TensorPtr tensor_where(TensorPtr cond, TensorPtr a, TensorPtr b) {
  if (a->dtype != b->dtype || cond->dtype != a->dtype) {
    throw CulebraError("ValueError", "Tensor: where: mismatched dtypes.");
  }
  auto dtype = a->dtype;
  auto v = _tl_guard(
      [&] { return tl::where(cond->value, a->value, b->value); });
  return tensor_make_op(Op::Where, std::move(v), dtype,
                        std::vector<TensorPtr>{cond, a, b});
}

// Row gather along axis 0: table[indices[i]] for each i (indices, a 1-D
// Tensor, float-valued like argmax's own output — no bounds check here,
// matching every other Tensor op's policy of validating shapes/dtypes, not
// element values). The embedding-table lookup. Differentiable w.r.t.
// `table` via tensor_index_add below, its exact dual.
inline TensorPtr tensor_index_select(TensorPtr table, TensorPtr indices) {
  auto dtype = table->dtype;
  auto v = _tl_guard(
      [&] { return table->value.index_select(indices->value); });
  return tensor_make_op(Op::IndexSelect, std::move(v), dtype,
                        std::vector<TensorPtr>{table, indices});
}

// index_select's dual: scatter-add `values` (shaped like some
// t.index_select(indices)) into a fresh zero buffer of `target_shape` —
// the embedding-table gradient. Repeated indices accumulate.
inline TensorPtr tensor_index_add(TensorPtr indices, TensorPtr values,
                                  TensorShape target_shape) {
  auto dtype = values->dtype;
  auto v = _tl_guard([&] {
    return tl::index_add(indices->value, values->value,
                         std::move(target_shape.dims));
  });
  return tensor_make_op(Op::IndexAdd, std::move(v), dtype,
                        std::vector<TensorPtr>{indices, values});
}

// One-hot scatter into a new trailing axis of size `size`: out[..., k] =
// values[...] where indices[...] == k, else 0 — the native primitive a
// pooling layer's own hand-derived backward composes with .fold() to
// reconstruct a padded input gradient (max/argmax pick one element out of
// a window; this scatters a gradient back into that same window shape).
// Forward-only for now: its own dual (gather one element back out of the
// window axis) has no caller yet, so it isn't built speculatively.
inline TensorPtr tensor_scatter_to_axis(TensorPtr indices, TensorPtr values,
                                        int64_t size) {
  auto dtype = values->dtype;
  auto v = _tl_guard(
      [&] { return tl::scatter_to_axis(indices->value, values->value, size); });
  return tensor_make_op(Op::ScatterAxis, std::move(v), dtype,
                        std::vector<TensorPtr>{indices, values});
}

// Build a lazy matrix multiply A @ B. A is [M, K], B is [K, N], the result
// is [M, N] — or, batched, [..., M, K] @ [..., K, N] -> [..., M, N] with
// every leading dim matching exactly (no broadcasting yet).
// Rank 1 (vector) and 2 (matrix) inputs promote/combine the same way
// tensorlib's own array::dot always has; rank >= 3 is a batched matmul
// (every leading dim must match exactly between `a` and `b` — no
// broadcasting yet). All of that validation — including the batched case's
// batch-dims-must-match and inner-dims-must-match checks — lives in
// tensorlib's own array::dot now (graph::dot, array.h), so this wrapper
// only checks the one thing tensorlib doesn't know about: culebra's own
// Dtype tag.
inline TensorPtr tensor_dot(TensorPtr a, TensorPtr b) {
  if (a->dtype != b->dtype) {
    throw CulebraError("ValueError", "Tensor: dtype mismatch in dot.");
  }
  auto v = _tl_guard([&] { return a->value.dot(b->value); });
  auto dtype = a->dtype;
  return tensor_make_op(Op::Dot, std::move(v), dtype,
                        std::vector<TensorPtr>{std::move(a), std::move(b)});
}

// Build a lazy unary node (one input, same shape and dtype).
inline TensorPtr tensor_unary(Op op, TensorPtr a) {
  auto v = _tl_guard([&]() -> tl::array {
    switch (op) {
      case Op::Sigmoid: return a->value.sigmoid();
      case Op::Relu: return a->value.relu();
      case Op::Log: return a->value.log();
      case Op::Softmax: return a->value.softmax();
      case Op::Tanh: return a->value.tanh();
      case Op::Sin: return a->value.sin();
      case Op::Cos: return a->value.cos();
      default:
        throw std::logic_error("tensor: non-unary op in unary dispatch");
    }
  });
  auto dtype = a->dtype;
  return tensor_make_op(op, std::move(v), dtype,
                        std::vector<TensorPtr>{std::move(a)});
}

inline TensorPtr tensor_log(TensorPtr a) {
  return tensor_unary(Op::Log, std::move(a));
}

// clip(x, lo, hi) needs two scalars tensor_unary's (op, a) signature has
// nowhere to carry, so its own builder (same reason tensor_dot/
// tensor_index_select aren't routed through a generic dispatcher either).
inline TensorPtr tensor_clamp(TensorPtr a, float lo, float hi) {
  auto v = _tl_guard([&] { return a->value.clamp(lo, hi); });
  auto dtype = a->dtype;
  auto out = tensor_make_op(Op::Clip, std::move(v), dtype,
                            std::vector<TensorPtr>{std::move(a)});
  out->extra0 = lo;
  out->extra1 = hi;
  return out;
}

// rope(x, pos, base) needs an int64 and a float tensor_unary's (op, a)
// signature has nowhere to carry, so its own builder (same reason
// tensor_clamp above has one). `pos` rides in op_param (Slice's own
// "one scalar" slot); `base` needs nothing stored back since Rope is
// forward-only (see the Op enum's own comment and the VJP switch).
inline TensorPtr tensor_rope(TensorPtr x, int64_t pos, float base = 10000.0f) {
  auto v = _tl_guard([&] { return tl::array::rope(x->value, pos, base); });
  auto dtype = x->dtype;
  return tensor_make_op(Op::Rope, std::move(v), dtype,
                        std::vector<TensorPtr>{std::move(x)}, pos);
}

// Fused MLP forward: sigmoid(W @ x + b). Bias is broadcast against
// the output [M, N] (typical: b is [M, 1] or [M]). The tl graph is
// dot + broadcast-add + sigmoid; backend fusion (bias/activation GEMM
// epilogues) is tensorlib's concern, not this layer's.
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
  auto v = _tl_guard(
      [&] { return (W->value.dot(x->value) + b->value).sigmoid(); });
  auto dtype = W->dtype;
  return tensor_make_op(
      Op::LinearSigmoid, std::move(v), dtype,
      std::vector<TensorPtr>{std::move(W), std::move(x), std::move(b)});
}

// View with a different shape when `t` is contiguous; a materializing copy
// otherwise. Used to reject the non-contiguous case outright ("Phase 1 only
// allows contiguous inputs... tensorlib can already do it — loosen when a
// workload needs it") — loosened now that one does (`.permute()` feeding a
// `.reshape()`, e.g. examples/dezero's col2im): tensorlib's own
// array::reshape already clones-then-reshapes internally when the source
// isn't contiguous, so this wrapper no longer needs to forbid it, just
// report it honestly via `is_view` below.
inline TensorPtr tensor_reshape(TensorPtr t, TensorShape new_shape) {
  if (new_shape.num_elements() != t->shape.num_elements()) {
    throw CulebraError("ValueError",
                       "Tensor: reshape element-count mismatch.");
  }
  // No eval needed for the check: an unevaluated op result is always
  // contiguous (tl lays lazy results out contiguously); only materialized
  // views (transpose/slice) can be strided, and those report directly.
  bool is_view = t->is_contiguous();
  auto v = _tl_guard([&] { return t->value.reshape(new_shape.dims); });
  auto dtype = t->dtype;
  return tensor_make_op(Op::Reshape, std::move(v), dtype,
                        std::vector<TensorPtr>{std::move(t)},
                        /*op_param=*/0, is_view);
}

// Stack tensors along `axis` (default 0, rows). All parts must share dtype,
// rank, and every dim but `axis`; the output's `axis` length is the sum of
// the parts'. `axis` rides in op_param (VJP re-derives each part's slice
// from it and each part's own shape) — tl::concat itself validates rank/
// shape/axis range, culebra only checks the one thing it doesn't know
// about: Dtype.
inline TensorPtr tensor_concat(std::vector<TensorPtr> parts,
                               int64_t axis = 0) {
  if (parts.empty()) {
    throw CulebraError("ValueError", "Tensor.concat: empty list.");
  }
  Dtype dt = parts[0]->dtype;
  for (const auto& p : parts) {
    if (p->dtype != dt) {
      throw CulebraError("ValueError", "Tensor.concat: dtype mismatch.");
    }
  }
  auto v = _tl_guard([&] {
    std::vector<tl::array> vs;
    vs.reserve(parts.size());
    for (const auto& p : parts) vs.push_back(p->value);
    return tl::concat(vs, static_cast<int>(axis));
  });
  return tensor_make_op(Op::Concat, std::move(v), dt, std::move(parts), axis);
}

// ===================== Autograd (reverse mode) =====================
//
// The Op tape recorded in `inputs`/`op` doubles as the autograd tape:
// forward values live in each node's tl::array. backward() walks the
// graph in reverse-topological order and routes the upstream gradient
// through each op's vector-Jacobian product, accumulating into `grad`
// — always a materialized Const (eager in-place add via tl add_), so
// it can never form a cycle with the forward graph. VJPs are built
// with the culebra-level tensor_* functions above, so they run on
// whatever backend tensorlib picks (the sum_to / mask / scatter
// primitives were added to tensorlib for exactly this).

// Sum `g` down to `target`, inverting a numpy broadcast. tl::sum_to is
// the device-ready primitive for this (the VJP of broadcasting).
inline TensorPtr _tensor_unbroadcast(TensorPtr g, const TensorShape& target) {
  if (g->shape == target) return g;
  auto v = _tl_guard([&] { return g->value.sum_to(target.dims); });
  return _tensor_wrap_const(std::move(v), g->dtype);
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
    _tl_guard([&] {
      node->grad->value.add_(contrib->value);
      return 0;
    });
  }
}

// da = g * (x > 0), elementwise — the tl comparison mask makes this a
// lazy device op.
inline TensorPtr _tensor_relu_backward(const TensorPtr& g, const TensorPtr& x) {
  auto v = _tl_guard([&] { return g->value * (x->value > 0.0f); });
  return _tensor_wrap_const(std::move(v), x->dtype);
}

// Scatter `g` (the gradient of a row-slice) into rows [start, start+len)
// of a zero tensor shaped like the slice's source: zeros + in-place add
// through a row view.
inline TensorPtr _tensor_slice_backward(const TensorPtr& g,
                                        const TensorShape& src_shape,
                                        Dtype dt, int64_t start) {
  auto v = _tl_guard([&] {
    auto out = tl::array::zeros(src_shape.dims);
    out.slice(start, g->shape.dims[0]).add_(g->value);
    return out;
  });
  return _tensor_wrap_const(std::move(v), dt);
}

// Reverse-topological DFS over the autograd graph (shared subgraphs —
// reused params, residuals — are visited once by pointer identity).
//
// Explicit-stack post-order DFS, not C++ recursion: an RNN-style unrolled
// graph (`x = x + step` in a loop, never detached) is a linear chain, and
// the straightforward recursive walk overflowed the C stack past ~70-80k
// nodes. Grad accumulation for a shared subgraph (a residual feeding two
// paths) sums in whatever order `topo` visits it, and float addition is
// not associative — so this reproduces the recursive version's push order
// EXACTLY (frame-by-frame: same node, same next-unvisited-input index,
// same visited-before-descend check) rather than just "a" valid
// topological order, to keep backward()'s numerics bit-for-bit unchanged.
// Frames live on this vector (heap), not the C stack, so depth is no
// longer bounded by stack size at all.
inline void _tensor_build_topo(const TensorPtr& root,
                               std::vector<TensorPtr>& topo,
                               std::unordered_set<TensorImpl*>& visited) {
  struct Frame {
    const TensorPtr* n;
    size_t next_input;
  };
  if (!visited.insert(root.get()).second) return;
  std::vector<Frame> stack;
  stack.reserve(8);  // covers most real graphs; skips the first grow-malloc
  stack.push_back({&root, 0});
  while (!stack.empty()) {
    Frame& top = stack.back();
    const TensorPtr& n = *top.n;
    if (top.next_input < n->inputs.size()) {
      const TensorPtr& in = n->inputs[top.next_input++];
      if (in && visited.insert(in.get()).second) {
        stack.push_back({&in, 0});  // may reallocate `stack` — `top`/`n`
                                     // unused after this in the iteration
      }
    } else {
      topo.push_back(n);
      stack.pop_back();
    }
  }
}

// Route node->grad through one op's VJP into its inputs' grads.
// Local gradient of a sigmoid output y: dy/dx = y * (1 - y). Shared by the
// standalone Sigmoid and the fused LinearSigmoid VJP.
inline TensorPtr _tensor_sigmoid_grad(const TensorPtr& y, Dtype dt) {
  return tensor_binop(Op::Mul, y,
                      tensor_binop(Op::Sub, tensor_scalar(1.0, dt), y));
}

// Dot's own VJP needs "transpose the matmul's own two axes, leave any batch
// axes alone" — tensor_transpose() reverses *every* axis instead, which only
// coincides with this at rank 2 (where there's nothing but the two matmul
// axes to reverse). Rank >= 3 (batched) needs the real thing: permute() with
// only the last two axes swapped.
inline TensorPtr _tensor_swap_last_two(TensorPtr t) {
  size_t r = t->shape.rank();
  if (r < 2) return t;  // a vector has nothing to swap — its own "transpose"
                        // is itself, matching tensor_transpose()'s rank-1
                        // no-op (dot's vec_m/vec_n promotion can hand this a
                        // rank-1 operand).
  std::vector<int64_t> axes(r);
  for (size_t i = 0; i < r; i++) axes[i] = static_cast<int64_t>(i);
  std::swap(axes[r - 2], axes[r - 1]);
  return tensor_permute(std::move(t), std::move(axes));
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
      // z = a @ b : da = g @ b^T, db = a^T @ g — "^T" here means swap the
      // matmul's own last two axes only (see _tensor_swap_last_two), not
      // tensor_transpose()'s full reversal; the two coincide at rank 2.
      _tensor_grad_add(
          n->inputs[0],
          tensor_dot(g, _tensor_swap_last_two(n->inputs[1])));
      _tensor_grad_add(
          n->inputs[1],
          tensor_dot(_tensor_swap_last_two(n->inputs[0]), g));
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
      // Each part owns a contiguous window of the output along op_param
      // (the concat axis); route that window of the upstream grad straight
      // back to it. tensor_narrow's own VJP is forward-only, but that
      // guards *its* callers going through .backward() — used here as a
      // plain extraction tool inside Concat's own VJP, it's fine.
      int64_t axis = n->op_param;
      int64_t off = 0;
      for (const auto& part : n->inputs) {
        int64_t len = part->shape.dims[axis];
        _tensor_grad_add(part, tensor_narrow(g, axis, off, off + len));
        off += len;
      }
      break;
    }
    case Op::Max:
    case Op::Argmax:
      throw CulebraError("ValueError",
          "Tensor.backward: Max / Argmax are not differentiable.");
    case Op::Gt:
    case Op::Lt:
    case Op::Ge:
    case Op::Le:
    case Op::Eq:
    case Op::Ne:
      throw CulebraError("ValueError",
          "Tensor.backward: comparisons are not differentiable.");
    case Op::Tanh: {
      // y = tanh(x); dx = g * (1 - y*y). y is this node's value. dezero's
      // own Tanh class (examples/dezero) still writes this same rule by
      // hand — both paths are supported, this one just lets a caller who
      // isn't building a dezero-style graph call .backward() directly.
      auto y2 = tensor_binop(Op::Mul, n, n);
      auto dydx = tensor_binop(Op::Sub, tensor_scalar(1.0, dt), y2);
      _tensor_grad_add(n->inputs[0], tensor_binop(Op::Mul, g, dydx));
      break;
    }
    case Op::Sin: {
      // y = sin(x); dx = g * cos(x).
      auto cosx = tensor_unary(Op::Cos, n->inputs[0]);
      _tensor_grad_add(n->inputs[0], tensor_binop(Op::Mul, g, cosx));
      break;
    }
    case Op::Cos: {
      // y = cos(x); dx = -g * sin(x).
      auto sinx = tensor_unary(Op::Sin, n->inputs[0]);
      _tensor_grad_add(n->inputs[0], neg(tensor_binop(Op::Mul, g, sinx)));
      break;
    }
    case Op::Clip: {
      // y = clamp(x, lo, hi); dx = g * (lo <= x <= hi ? 1 : 0). The two
      // comparisons compose via multiply (0/1 masks), the same trick
      // dezero's own Clip.backward uses via masked_gate.
      const auto& x = n->inputs[0];
      auto mask = _tl_guard(
          [&] { return (x->value >= n->extra0) * (x->value <= n->extra1); });
      _tensor_grad_add(x, tensor_binop(Op::Mul, g,
                                       _tensor_wrap_const(std::move(mask), dt)));
      break;
    }
    case Op::Unfold:
    case Op::Pad:
    case Op::Fold:
      // unfold's VJP is fold with the same params (and vice versa); pad's is
      // a crop. Not yet wired up: examples/dezero's own conv layer runs its
      // own autograd around these rather than native .backward(), so this
      // arm is unreached today — thrown rather than silently wrong once
      // something does reach it.
      throw CulebraError("ValueError",
          "Tensor.backward: unfold / pad / fold are not differentiable yet.");
    case Op::Permute: {
      // y = x.permute(axes); dx = g.permute(axes^-1), where the inverse
      // sends axis `axes[i]` back to position `i`. The forward packed those
      // axes into op_param (see tensor_permute); only a rank past what fits
      // there is still forward-only.
      if (n->op_param == kPermuteNotPacked) {
        throw CulebraError("ValueError",
            "Tensor.backward: permute of rank > " +
                std::to_string(kPermuteMaxPackedRank) +
                " is not differentiable.");
      }
      size_t r = n->shape.rank();
      std::vector<int64_t> inverse(r);
      for (size_t i = 0; i < r; i++) {
        auto axis = static_cast<size_t>((n->op_param >> (4 * i)) & 0xF);
        inverse[axis] = static_cast<int64_t>(i);
      }
      _tensor_grad_add(n->inputs[0], tensor_permute(g, std::move(inverse)));
      break;
    }
    case Op::Narrow: {
      // The VJP is a zero-pad back out to the original axis length --
      // op_param/extra0/extra1 (axis/start/end, stashed by tensor_narrow)
      // are exactly what that needs. `.pad()` is forward-only itself
      // (see Op::Pad's own case below) but that only guards *its* callers
      // going through .backward() -- used here as a plain value-builder
      // inside Narrow's own VJP, it's fine (same idea as Concat's VJP
      // using tensor_narrow as a plain extraction tool above).
      int64_t axis = n->op_param;
      int64_t start = static_cast<int64_t>(n->extra0);
      int64_t end = static_cast<int64_t>(n->extra1);
      int64_t after = n->inputs[0]->shape.dims[axis] - end;
      auto gv = _tl_guard(
          [&] { return g->value.pad(static_cast<int>(axis), start, after); });
      _tensor_grad_add(n->inputs[0], _tensor_wrap_const(std::move(gv), dt));
      break;
    }
    case Op::Where: {
      // y = cond != 0 ? a : b; da = g*cond, db = g*(1-cond). cond gets no
      // gradient (matches numpy/PyTorch's own where()).
      const auto& cond = n->inputs[0];
      auto da = tensor_binop(Op::Mul, g, cond);
      auto one_minus_cond =
          tensor_binop(Op::Sub, tensor_scalar(1.0, dt), cond);
      auto db = tensor_binop(Op::Mul, g, one_minus_cond);
      _tensor_grad_add(n->inputs[1], da);
      _tensor_grad_add(n->inputs[2], db);
      break;
    }
    case Op::IndexSelect: {
      // y = table[indices]; d_table = index_add(indices, g, table.shape).
      // indices get no gradient.
      const auto& table = n->inputs[0];
      const auto& indices = n->inputs[1];
      _tensor_grad_add(table, tensor_index_add(indices, g, table->shape));
      break;
    }
    case Op::IndexAdd: {
      // y = index_add(indices, values, target_shape); d_values =
      // index_select(indices, g), index_select's own dual. indices get no
      // gradient.
      const auto& indices = n->inputs[0];
      const auto& values = n->inputs[1];
      _tensor_grad_add(values, tensor_index_select(g, indices));
      break;
    }
    case Op::ScatterAxis:
      // The VJP is scatter_to_axis's own dual (gather one element back out
      // of the window axis by the same indices), which nothing builds yet
      // — no caller needs it (a pooling layer's own backward uses this op
      // forward-only, writing its own gradient by hand around it).
      throw CulebraError("ValueError",
          "Tensor.backward: scatter_to_axis is not differentiable yet.");
    case Op::Rope:
      // The VJP is the transpose rotation (angle negated), which needs the
      // same cos/sin table the forward used and nothing here keeps it
      // (op_param is pos, sized for Slice's one scalar; base isn't stored
      // back at all — see the Op enum's own comment). Unreached today: a
      // training-time RoPE composes from existing differentiable
      // primitives instead of this fused decode/inference fast path.
      throw CulebraError("ValueError",
          "Tensor.backward: rope is not differentiable yet.");
  }
}

// backward(): seed dL/droot = 1 and propagate through the whole graph.
inline void tensor_backward(const TensorPtr& root) {
  // VJPs build gradient tensors with the same op builders as forward; culebra
  // has no double-backward (grad is always a Const), so those intermediates
  // never need their own tape. Suppress it for the whole reverse pass.
  TensorNoGradGuard no_grad;
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

// "Tensor 3x4 f32" — used by interp and JIT inspect/str paths.
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

// --- Choke definitions (see the declarations above for the linkage map) ---

CULEBRA_RT_TENSOR_EVAL_LINKAGE void tensor_rt_bootstrap() {
#ifdef CULEBRA_RT_TENSOR_EVAL_WEAK
  // Core-archive stub: referencing tl::install_runtime_hooks here would pull
  // the whole execution engine (evaluator, Accelerate/Metal backends, MSL
  // source) into the core archive. Programs that use Tensor force-load the
  // strong body from the tensor feature object, which overrides this.
#else
  static const bool installed = [] {
    tl::install_runtime_hooks();
    if (!tensor_device_chosen) tl::use_auto();
    return true;
  }();
  (void)installed;
#endif
}

CULEBRA_RT_TENSOR_EVAL_LINKAGE void tensor_eval_node(TensorImpl& t) {
#ifdef CULEBRA_RT_TENSOR_EVAL_WEAK
  (void)t;
  throw CulebraError("InternalError",
                     "tensor runtime entered in a no-tensor binary", 0, 0);
#else
  tensor_rt_bootstrap();
  _tl_guard([&] {
    t.value.eval();
    return 0;
  });
#endif
}

CULEBRA_RT_TENSOR_EVAL_LINKAGE bool tensor_gpu_available() {
#ifdef CULEBRA_RT_TENSOR_EVAL_WEAK
  return false;  // no backend linked in a no-tensor binary
#else
  return tl::gpu_available();
#endif
}

}  // namespace culebra
