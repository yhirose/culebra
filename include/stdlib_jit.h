#pragma once

// JIT-side implementation of the Culebra standard library.
//
// Independent header. Include from main.cc (or any embedder) after
// jit.h. Provides the runtime functions called from JIT'd code and the
// `JitExtension` struct that fills `JIT::ExtensionHooks`. Embedders
// install the stdlib by calling `culebra::install_jit_stdlib()` once
// before `JIT::run()`.

#include <jit.h>
#include <shared.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>

namespace culebra {
// main.cc (or any embedder) populates this before calling JIT::run.
inline std::vector<std::string>& _culebra_sys_argv_holder() {
  static std::vector<std::string> v;
  return v;
}
}  // namespace culebra

using culebra::parse_double_strict;
using culebra::parse_long_strict;
using culebra::throw_type_error_at;

extern "C" {

__attribute__((used)) inline int64_t culebra_runtime_to_long(
    const char* s, int64_t line, int64_t col) {
  if (!s) throw_type_error_at(line, col);
  return parse_long_strict(s, line, col);
}

__attribute__((used)) inline JitValue culebra_runtime_to_long_any(
    int8_t tag, int64_t data, int64_t line, int64_t col) {
  if (tag == TAG_LONG) return {TAG_LONG, data};
  if (tag == TAG_FLOAT) {
    return {TAG_LONG, static_cast<int64_t>(_culebra_float_to_double(data))};
  }
  if (tag == TAG_STRING) {
    return {TAG_LONG,
            parse_long_strict(reinterpret_cast<const char*>(data), line, col)};
  }
  throw_type_error_at(line, col);
}

__attribute__((used)) inline JitValue culebra_runtime_to_float_any(
    int8_t tag, int64_t data, int64_t line, int64_t col) {
  if (tag == TAG_FLOAT) return {TAG_FLOAT, data};
  if (tag == TAG_LONG) {
    return {TAG_FLOAT, _culebra_double_to_bits(static_cast<double>(data))};
  }
  if (tag == TAG_STRING) {
    auto d = parse_double_strict(reinterpret_cast<const char*>(data), line, col);
    return {TAG_FLOAT, _culebra_double_to_bits(d)};
  }
  throw_type_error_at(line, col);
}

__attribute__((used)) inline const char* culebra_runtime_type_of(int8_t tag) {
  return _culebra_tag_name(tag);
}

__attribute__((used)) inline void culebra_runtime_print(int8_t type,
                                                        int64_t data) {
  if (auto s = _try_str_special(type, data)) {
    std::cout << *s;
    return;
  }
  if (type == 4) {
    std::cout << reinterpret_cast<const char*>(data);
  } else {
    std::cout << _culebra_value_to_str_impl(type, data);
  }
}

__attribute__((used)) inline const char* culebra_runtime_input() {
  std::string line;
  if (!std::getline(std::cin, line)) {
    return _culebra_heap_str(std::string(""));
  }
  return _culebra_heap_str(line);
}

__attribute__((used)) inline const char* culebra_runtime_read_file(
    const char* path, int64_t line, int64_t col) {
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) {
    throw culebra::CulebraError("IOError",
        std::format("IO.read: cannot open '{}' at {}:{}.", path, line, col),
        line, col);
  }
  std::string s((std::istreambuf_iterator<char>(ifs)),
                std::istreambuf_iterator<char>());
  return _culebra_heap_str(s);
}

__attribute__((used)) inline void culebra_runtime_write_file(
    const char* path, const char* content, int64_t line, int64_t col) {
  std::ofstream ofs(path, std::ios::binary);
  if (!ofs) {
    throw culebra::CulebraError("IOError",
        std::format("IO.write: cannot open '{}' at {}:{}.", path, line, col),
        line, col);
  }
  auto len = std::strlen(content);
  ofs.write(content, static_cast<std::streamsize>(len));
}

__attribute__((used)) inline int64_t culebra_runtime_math_pow(
    int64_t base, int64_t exp, int64_t line, int64_t col) {
  if (exp < 0) {
    culebra::throw_type_error_at(line, col);
  }
  int64_t r = 1;
  while (exp > 0) {
    if (exp & 1) r *= base;
    base *= base;
    exp >>= 1;
  }
  return r;
}

#define CUL_MATH_F2F(name, call)                                        \
  __attribute__((used)) inline JitValue culebra_runtime_math_##name(    \
      int8_t tag, int64_t data, int64_t line, int64_t col) {            \
    double x;                                                           \
    if (tag == TAG_LONG)        x = static_cast<double>(data);          \
    else if (tag == TAG_FLOAT)  x = _culebra_float_to_double(data);     \
    else                        throw_type_error_at(line, col);         \
    return {TAG_FLOAT, _culebra_double_to_bits(call)};                  \
  }
CUL_MATH_F2F(log,  std::log(x))
CUL_MATH_F2F(exp,  std::exp(x))
CUL_MATH_F2F(sqrt, std::sqrt(x))
#undef CUL_MATH_F2F

// Long input is identity (not coerced through Float, which would lose
// precision on values past 2^53).
#define CUL_MATH_F2L(name, call)                                        \
  __attribute__((used)) inline JitValue culebra_runtime_math_##name(    \
      int8_t tag, int64_t data, int64_t line, int64_t col) {            \
    if (tag == TAG_LONG) return {TAG_LONG, data};                       \
    if (tag != TAG_FLOAT) throw_type_error_at(line, col);               \
    double x = _culebra_float_to_double(data);                          \
    return {TAG_LONG, static_cast<int64_t>(call)};                      \
  }
CUL_MATH_F2L(floor, std::floor(x))
CUL_MATH_F2L(ceil,  std::ceil(x))
CUL_MATH_F2L(round, std::rint(x))
#undef CUL_MATH_F2L

__attribute__((used)) inline JitValue culebra_runtime_math_abs(
    int8_t tag, int64_t data, int64_t line, int64_t col) {
  if (tag == TAG_LONG) return {TAG_LONG, data < 0 ? -data : data};
  if (tag == TAG_FLOAT) {
    double x = _culebra_float_to_double(data);
    return {TAG_FLOAT, _culebra_double_to_bits(std::fabs(x))};
  }
  throw_type_error_at(line, col);
}

// Returns Long if every entry was Long, else Float. Callers handle
// retain/release of `args`; the return is numeric (no ref).
inline JitValue _culebra_numeric_reduce(const JitValue* args, int64_t n,
                                        int64_t line, int64_t col,
                                        bool pick_less) {
  if (n < 2) throw_type_error_at(line, col);
  bool any_float = false;
  for (int64_t i = 0; i < n; i++) {
    if (args[i].tag != TAG_LONG && args[i].tag != TAG_FLOAT) {
      throw_type_error_at(line, col);
    }
    if (args[i].tag == TAG_FLOAT) any_float = true;
  }
  if (any_float) {
    double acc = _culebra_coerce_num(args[0].tag, args[0].data);
    for (int64_t i = 1; i < n; i++) {
      double x = _culebra_coerce_num(args[i].tag, args[i].data);
      if (pick_less ? x < acc : x > acc) acc = x;
    }
    return {TAG_FLOAT, _culebra_double_to_bits(acc)};
  }
  int64_t acc = args[0].data;
  for (int64_t i = 1; i < n; i++) {
    int64_t x = args[i].data;
    if (pick_less ? x < acc : x > acc) acc = x;
  }
  return {TAG_LONG, acc};
}

__attribute__((used)) inline JitValue culebra_runtime_math_min(
    const JitValue* args, int64_t n, int64_t line, int64_t col) {
  return _culebra_numeric_reduce(args, n, line, col, /*pick_less=*/true);
}

__attribute__((used)) inline JitValue culebra_runtime_math_max(
    const JitValue* args, int64_t n, int64_t line, int64_t col) {
  return _culebra_numeric_reduce(args, n, line, col, /*pick_less=*/false);
}

// --- Random ---

__attribute__((used)) inline void culebra_runtime_random_seed(int64_t n) {
  culebra::random_engine().seed(static_cast<uint64_t>(n));
}

__attribute__((used)) inline int64_t culebra_runtime_random_int(
    int64_t lo, int64_t hi, int64_t line, int64_t col) {
  if (hi <= lo) throw_type_error_at(line, col);
  std::uniform_int_distribution<int64_t> d(lo, hi - 1);
  return d(culebra::random_engine());
}

#define CUL_RANDOM_PAIR(name, Dist)                                     \
  __attribute__((used)) inline JitValue culebra_runtime_random_##name(  \
      int8_t lt, int64_t ld, int8_t rt, int64_t rd) {                   \
    auto a = _culebra_coerce_num(lt, ld);                               \
    auto b = _culebra_coerce_num(rt, rd);                               \
    Dist d(a, b);                                                       \
    return {TAG_FLOAT,                                                  \
            _culebra_double_to_bits(d(culebra::random_engine()))};      \
  }
CUL_RANDOM_PAIR(uniform, std::uniform_real_distribution<double>)
CUL_RANDOM_PAIR(gauss,   std::normal_distribution<double>)
#undef CUL_RANDOM_PAIR

__attribute__((used)) inline void culebra_runtime_random_shuffle(
    JitArray* arr) {
  std::shuffle(arr->items, arr->items + arr->size, culebra::random_engine());
}

// weighted_choice(pop, weights): returns one element from pop. Pop
// and weights must be equal-length Arrays. Returned value is +1 owned
// by the caller (we retain the picked element before returning).
__attribute__((used)) inline JitValue culebra_runtime_random_weighted_choice(
    JitArray* pop, JitArray* weights, int64_t line, int64_t col) {
  if (!pop || !weights || pop->size == 0 ||
      pop->size != weights->size) {
    throw_type_error_at(line, col);
  }
  // Reused between calls to avoid a heap allocation per draw (the
  // microgpt inference path calls this per token).
  thread_local std::vector<double> scratch;
  scratch.clear();
  scratch.reserve(weights->size);
  for (size_t i = 0; i < weights->size; i++) {
    const auto& v = weights->items[i];
    if (v.tag != TAG_LONG && v.tag != TAG_FLOAT) {
      throw_type_error_at(line, col);
    }
    scratch.push_back(_culebra_coerce_num(v.tag, v.data));
  }
  std::discrete_distribution<size_t> d(scratch.begin(), scratch.end());
  auto picked = pop->items[d(culebra::random_engine())];
  culebra_runtime_value_retain(picked.tag, picked.data);
  return picked;
}

// --- IO.exists ---

__attribute__((used)) inline int64_t culebra_runtime_io_exists(
    const char* path) {
  if (!path) return 0;
  std::error_code ec;
  return std::filesystem::exists(path, ec) ? 1 : 0;
}

__attribute__((used)) inline void culebra_runtime_sys_exit(int64_t code) {
  std::exit(static_cast<int>(code));
}

__attribute__((used)) inline const char* culebra_runtime_sys_env(
    const char* name) {
  const char* v = std::getenv(name);
  return _culebra_heap_str(std::string(v ? v : ""));
}

__attribute__((used)) inline double culebra_runtime_sys_time() {
  using clock = std::chrono::steady_clock;
  static const auto t0 = clock::now();
  auto now = clock::now();
  return std::chrono::duration<double>(now - t0).count();
}

__attribute__((used)) inline JitArray* culebra_runtime_sys_argv() {
  auto& argv = culebra::_culebra_sys_argv_holder();
  auto* r = culebra_runtime_array_new();
  for (const auto& s : argv) {
    auto* str = _culebra_heap_str(s);
    culebra_runtime_array_push(r, TAG_STRING,
                               reinterpret_cast<int64_t>(str));
  }
  return r;
}

}  // extern "C"

// --- JIT compile-side dispatch (extension hook implementation) ---

namespace culebra {

// JitExtension fills the JIT::ExtensionHooks for the standard library
// (Math/IO/Random/Sys + bare globals like puts/to_long/type_of and the
// `Math.range(N).<HOF>(...)` fusion peephole). Declared as a friend of
// JIT in jit.h so each member can reach the JIT internals it needs
// (builder_/module_/valueType_/make_*/extract_*/...) without the JIT
// class having to expose them publicly.
struct JitExtension {
  static void declare_runtime(JIT& jit);
  static llvm::Value* compile_global(JIT& jit, const std::string& name,
                                       const peg::Ast& argsAst,
                                       const peg::Ast& callAst);
  static llvm::Value* compile_ns_call(JIT& jit,
                                        std::string_view ns,
                                        std::string_view method,
                                        const peg::Ast& argsAst,
                                        const peg::Ast& callAst);
  static llvm::Value* compile_ns_prop(JIT& jit,
                                        std::string_view ns,
                                        std::string_view prop);

  static llvm::Value* emit_output_call(JIT& jit, const char* rt_name,
                                         const peg::Ast& argsAst);

  static bool is_builtin_var(const std::string& name);
  static llvm::Value* compile_ufcs_builtin(JIT& jit,
                                             const std::string& method,
                                             const peg::Ast& argsAst,
                                             llvm::Value* receiver);

  static void install() {
    JIT::install_extension({
        .declare_runtime = &declare_runtime,
        .compile_global = &compile_global,
        .compile_ns_call = &compile_ns_call,
        .compile_ns_prop = &compile_ns_prop,
        .is_builtin_var = &is_builtin_var,
        .compile_ufcs_builtin = &compile_ufcs_builtin,
    });
  }
};

// Convenience wrapper for embedders. Call once before JIT::run().
inline void install_jit_stdlib() { JitExtension::install(); }

inline void JitExtension::declare_runtime(JIT& jit) {
  auto ptrTy = llvm::PointerType::get(jit.ctx_, 0);

  jit.module_->getOrInsertFunction(rt::print, jit.builder_.getVoidTy(),
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::to_long,
                               jit.builder_.getInt64Ty(), ptrTy,
                               jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::to_long_any, jit.valueType_,
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::to_float_any, jit.valueType_,
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::type_of, ptrTy,
                               jit.builder_.getInt8Ty());
  jit.module_->getOrInsertFunction(rt::input, ptrTy);
  jit.module_->getOrInsertFunction(rt::read_file, ptrTy, ptrTy,
                               jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::write_file,
                               jit.builder_.getVoidTy(), ptrTy, ptrTy,
                               jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::math_pow,
                               jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty());
  // Float-domain Math: (tag, data, line, col) -> JitValue. Register
  // the full family in one pass since they share the same signature.
  for (auto name : {rt::math_log, rt::math_exp, rt::math_sqrt,
                    rt::math_floor, rt::math_ceil, rt::math_round,
                    rt::math_abs}) {
    jit.module_->getOrInsertFunction(name, jit.valueType_,
                                 jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty(),
                                 jit.builder_.getInt64Ty(),
                                 jit.builder_.getInt64Ty());
  }
  // Variadic min/max: (args_ptr, n, line, col) -> JitValue.
  for (auto name : {rt::math_min, rt::math_max}) {
    jit.module_->getOrInsertFunction(name, jit.valueType_, ptrTy,
                                 jit.builder_.getInt64Ty(),
                                 jit.builder_.getInt64Ty(),
                                 jit.builder_.getInt64Ty());
  }
  jit.module_->getOrInsertFunction(rt::sys_exit, jit.builder_.getVoidTy(),
                               jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::sys_env, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::sys_argv, ptrTy);
  jit.module_->getOrInsertFunction(rt::sys_time, jit.builder_.getDoubleTy());
  // Random
  jit.module_->getOrInsertFunction(rt::random_seed, jit.builder_.getVoidTy(),
                               jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::random_int, jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty(), jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::random_uniform, jit.valueType_,
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty(),
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::random_gauss, jit.valueType_,
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty(),
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::random_shuffle, jit.builder_.getVoidTy(),
                               ptrTy);
  jit.module_->getOrInsertFunction(rt::random_weighted_choice, jit.valueType_,
                               ptrTy, ptrTy, jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::io_exists, jit.builder_.getInt64Ty(), ptrTy);

  // Tensor: zeros/ones/randn use the variadic (args_ptr, n, line, col)
  // -> ptr signature; from takes a single Array (ptr, line, col);
  // shape is just (Tensor*) -> Array*.
  for (auto name : {rt::tensor_zeros, rt::tensor_ones, rt::tensor_randn}) {
    jit.module_->getOrInsertFunction(name, ptrTy, ptrTy,
                                 jit.builder_.getInt64Ty(),
                                 jit.builder_.getInt64Ty(),
                                 jit.builder_.getInt64Ty());
  }
  jit.module_->getOrInsertFunction(rt::tensor_from, ptrTy, ptrTy,
                               jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::tensor_shape, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::tensor_eval_one,
                               jit.builder_.getVoidTy(), ptrTy);
  jit.module_->getOrInsertFunction(rt::tensor_binop, ptrTy,
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty(),
                               jit.builder_.getInt8Ty(), jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::tensor_transpose, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::tensor_clone, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::tensor_slice, ptrTy, ptrTy,
                               jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::tensor_reshape, ptrTy, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::tensor_reduce_axis, ptrTy, ptrTy,
                               jit.builder_.getInt64Ty(),
                               jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::tensor_reduce_all, jit.valueType_,
                               ptrTy, jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::tensor_to_array, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::tensor_dot, ptrTy, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::tensor_from_csv, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::tensor_unary, ptrTy, ptrTy,
                               jit.builder_.getInt64Ty());
  jit.module_->getOrInsertFunction(rt::tensor_linear_sigmoid, ptrTy,
                               ptrTy, ptrTy, ptrTy);

  // Iterator terminal methods.
  auto i64 = jit.builder_.getInt64Ty();
  auto i8 = jit.builder_.getInt8Ty();
  jit.module_->getOrInsertFunction(rt::iter_collect, ptrTy, i8, i64);
  jit.module_->getOrInsertFunction(rt::iter_count, i64, i8, i64);
  jit.module_->getOrInsertFunction(rt::iter_for_each, jit.builder_.getVoidTy(),
                               i8, i64, i8, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_reduce, jit.builder_.getVoidTy(),
                               i8, i64, i8, i64, i8, i64, i64, i64,
                               ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::iter_find, jit.builder_.getVoidTy(),
                               i8, i64, i8, i64, i64, i64, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::iter_any, i64, i8, i64, i8, i64, i64,
                               i64);
  // sum/product/min/max: (it, id, line, col) -> i64
  jit.module_->getOrInsertFunction(rt::iter_sum, i64, i8, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_product, i64, i8, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_min, i64, i8, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_max, i64, i8, i64, i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_all, i64, i8, i64, i8, i64, i64,
                               i64);

  // Iterator lazy factories.
  jit.module_->getOrInsertFunction(rt::iter_map, ptrTy, i8, i64, i8, i64);
  jit.module_->getOrInsertFunction(rt::iter_filter, ptrTy, i8, i64, i8, i64);
  jit.module_->getOrInsertFunction(rt::iter_take, ptrTy, i8, i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_skip, ptrTy, i8, i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_take_while, ptrTy, i8, i64, i8, i64);
  // chain/zip/flat_map carry line+col for the "not iterable" error.
  jit.module_->getOrInsertFunction(rt::iter_chain, ptrTy, i8, i64, i8, i64,
                               i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_zip, ptrTy, i8, i64, i8, i64,
                               i64, i64);
  jit.module_->getOrInsertFunction(rt::iter_enumerate, ptrTy, i8, i64);
  jit.module_->getOrInsertFunction(rt::iter_flat_map, ptrTy, i8, i64, i8, i64,
                               i64, i64);
  // (next_cls, iter_tag, iter_data, &out_tag, &out_data) -> i64 (1/0)
  jit.module_->getOrInsertFunction(rt::iter_advance, i64, ptrTy, i8, i64, ptrTy,
                               ptrTy);
  jit.module_->getOrInsertFunction(rt::str_code_points, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::str_graphemes, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::array_iter, ptrTy, ptrTy);
  jit.module_->getOrInsertFunction(rt::object_iter, ptrTy, ptrTy);
}

inline llvm::Value* JitExtension::compile_global(JIT& jit,
                                                  const std::string& name,
                                                  const peg::Ast& argsAst,
                                                  const peg::Ast& callAst) {
  auto line = jit.builder_.getInt64(callAst.line);
  auto col = jit.builder_.getInt64(callAst.column);

  if (name == "puts" && argsAst.nodes.size() == 1)
    return emit_output_call(jit, rt::puts, argsAst);
  if (name == "print" && argsAst.nodes.size() == 1)
    return emit_output_call(jit, rt::print, argsAst);

  if (name == "assert" && argsAst.nodes.size() == 1) {
    auto arg = jit.compile(*argsAst.nodes[0]);
    jit.emit_call(
        jit.module_->getFunction(rt::assert_),
        {jit.extract_tag(arg), jit.extract_data(arg), line, col});
    jit.emit_value_release(arg);
    return jit.make_nil();
  }

  if (name == "to_long" && argsAst.nodes.size() == 1) {
    auto arg = jit.compile(*argsAst.nodes[0]);
    // Polymorphic: Long/Float/String. The runtime helper dispatches
    // on tag (Long identity, Float truncate, String parse) and
    // raises `type error` for anything else.
    auto result = jit.emit_call(
        jit.module_->getFunction(rt::to_long_any),
        {jit.extract_tag(arg), jit.extract_data(arg), line, col});
    jit.emit_value_release(arg);
    return result;
  }

  if (name == "to_float" && argsAst.nodes.size() == 1) {
    auto arg = jit.compile(*argsAst.nodes[0]);
    auto result = jit.emit_call(
        jit.module_->getFunction(rt::to_float_any),
        {jit.extract_tag(arg), jit.extract_data(arg), line, col});
    jit.emit_value_release(arg);
    return result;
  }

  if (name == "to_string" && argsAst.nodes.size() == 1) {
    auto arg = jit.compile(*argsAst.nodes[0]);
    auto s = jit.emit_call(
        jit.module_->getFunction(rt::value_to_display),
        {jit.extract_tag(arg), jit.extract_data(arg)});
    jit.emit_value_release(arg);
    return jit.make_string(s);
  }

  if (name == "type_of" && argsAst.nodes.size() == 1) {
    auto arg = jit.compile(*argsAst.nodes[0]);
    auto s = jit.emit_call(jit.module_->getFunction(rt::type_of),
                                 {jit.extract_tag(arg)});
    jit.emit_value_release(arg);
    return jit.make_string(s);
  }

  return nullptr;
}

// Member-style shorthands so each JitExtension::compile_* body can call
// `make_long(v)` / `extract_tag(v)` / `module_->...` directly instead
// of `jit.make_long(v)` / `jit.module_->...`. References for true JIT
// members; forwarding lambdas for member functions (C++ has no member
// aliasing). Undefined at the end of this header.
#define CULEBRA_JIT_EXT_BODY_ALIASES(jit)                                     \
  auto& module_ = jit.module_;                                                \
  auto& builder_ = jit.builder_;                                              \
  auto& valueType_ = jit.valueType_;                                          \
  auto& ctx_ = jit.ctx_;                                                      \
  auto extract_tag = [&](llvm::Value* v) { return jit.extract_tag(v); };      \
  auto extract_data = [&](llvm::Value* v) { return jit.extract_data(v); };    \
  auto make_long = [&](llvm::Value* v) { return jit.make_long(v); };          \
  auto make_string = [&](llvm::Value* v) { return jit.make_string(v); };      \
  auto make_array = [&](llvm::Value* v) { return jit.make_array(v); };        \
  auto make_object = [&](llvm::Value* v) { return jit.make_object(v); };      \
  auto make_tensor = [&](llvm::Value* v) { return jit.make_tensor(v); };      \
  auto make_nil = [&]() { return jit.make_nil(); };                           \
  auto make_bool = [&](llvm::Value* v) { return jit.make_bool(v); };          \
  auto make_float = [&](llvm::Value* v) { return jit.make_float(v); };        \
  auto value_to_long = [&](llvm::Value* v) { return jit.value_to_long(v); };  \
  auto emit_value_release = [&](llvm::Value* v) {                             \
    return jit.emit_value_release(v);                                         \
  };                                                                          \
  auto emit_type_check = [&](llvm::Value* v, std::string_view t,              \
                             const char* w) {                                 \
    return jit.emit_type_check(v, t, w);                                      \
  };                                                                          \
  auto compile = [&](const peg::Ast& a) { return jit.compile(a); };           \
  auto emit_output_call = [&](const char* rt, const peg::Ast& a) {            \
    return JitExtension::emit_output_call(jit, rt, a);                        \
  };                                                                          \
  auto emit_call = [&](llvm::FunctionCallee callee,                           \
                       llvm::ArrayRef<llvm::Value*> args) {                   \
    return jit.emit_call(callee, args);                                       \
  }

inline llvm::Value* JitExtension::compile_ns_call(JIT& jit,
                                                    std::string_view ns,
                                                    std::string_view method,
                                                    const peg::Ast& argsAst,
                                                    const peg::Ast& callAst) {
  CULEBRA_JIT_EXT_BODY_ALIASES(jit);
  auto ptrTy = llvm::PointerType::get(ctx_, 0);
  auto line = builder_.getInt64(callAst.line);
  auto col = builder_.getInt64(callAst.column);

  if (ns == "Math") {
    auto build_block = [&](const char* tag) {
      return llvm::BasicBlock::Create(
          ctx_, tag, builder_.GetInsertBlock()->getParent());
    };

    // Single-arg helper with a Long fast path. If the argument is
    // TAG_LONG at runtime, run `long_fast` inline on its i64 payload
    // (abs: neg-or-self; floor/ceil/round: identity). Otherwise fall
    // back to the runtime helper, which handles Float and raises
    // `type error` on non-numeric input.
    auto math_unary = [&](const char* rt_name,
                          auto long_fast) -> llvm::Value* {
      if (argsAst.nodes.size() != 1) return nullptr;
      auto arg = compile(*argsAst.nodes[0]);
      auto tag = extract_tag(arg);
      auto data = extract_data(arg);
      auto intBB = build_block("math.int");
      auto slowBB = build_block("math.slow");
      auto mergeBB = build_block("math.merge");
      auto isLong =
          builder_.CreateICmpEQ(tag, builder_.getInt8(TAG_LONG));
      builder_.CreateCondBr(isLong, intBB, slowBB);

      builder_.SetInsertPoint(intBB);
      auto intResult = long_fast(data);
      auto intEndBB = builder_.GetInsertBlock();
      builder_.CreateBr(mergeBB);

      builder_.SetInsertPoint(slowBB);
      auto slowResult = emit_call(
          module_->getFunction(rt_name), {tag, data, line, col});
      auto slowEndBB = builder_.GetInsertBlock();
      builder_.CreateBr(mergeBB);

      builder_.SetInsertPoint(mergeBB);
      auto phi = builder_.CreatePHI(valueType_, 2, "math.r");
      phi->addIncoming(intResult, intEndBB);
      phi->addIncoming(slowResult, slowEndBB);
      emit_value_release(arg);
      return static_cast<llvm::Value*>(phi);
    };

    // Single-arg helper without a Long fast path: log/exp/sqrt return
    // Float for every numeric input, so there's no benefit to
    // branching (the runtime's SIToFP happens regardless).
    auto math_unary_runtime = [&](const char* rt_name) -> llvm::Value* {
      if (argsAst.nodes.size() != 1) return nullptr;
      auto arg = compile(*argsAst.nodes[0]);
      auto result = emit_call(
          module_->getFunction(rt_name),
          {extract_tag(arg), extract_data(arg), line, col});
      emit_value_release(arg);
      return result;
    };

    if (method == "abs") {
      if (auto v = math_unary(rt::math_abs, [&](llvm::Value* d) {
            auto isNeg = builder_.CreateICmpSLT(d, builder_.getInt64(0));
            return make_long(
                builder_.CreateSelect(isNeg, builder_.CreateNeg(d), d));
          })) return v;
    }
    // For floor/ceil/round on Long, the result is the input unchanged.
    auto long_identity = [&](llvm::Value* d) { return make_long(d); };
    if (method == "floor") { if (auto v = math_unary(rt::math_floor, long_identity)) return v; }
    if (method == "ceil")  { if (auto v = math_unary(rt::math_ceil,  long_identity)) return v; }
    if (method == "round") { if (auto v = math_unary(rt::math_round, long_identity)) return v; }
    if (method == "log")   { if (auto v = math_unary_runtime(rt::math_log))   return v; }
    if (method == "exp")   { if (auto v = math_unary_runtime(rt::math_exp))   return v; }
    if (method == "sqrt")  { if (auto v = math_unary_runtime(rt::math_sqrt))  return v; }

    // min/max: compile every arg, stash them in a stack array of
    // JitValue, and call the variadic helper with (&args[0], n).
    // 2-arg fast path: if both operands are TAG_LONG, use an inline
    // icmp+select — avoids the alloca+runtime call for the common
    // Math.min(a, b) / Math.max(a, b) shape. Any tag mismatch
    // (including Float) falls through to the variadic helper which
    // handles promotion and error reporting.
    auto reduce_pair = [&](llvm::Value* lhs, llvm::Value* rhs,
                           const char* rt_name,
                           llvm::CmpInst::Predicate pred) {
      auto ltag = extract_tag(lhs);
      auto rtag = extract_tag(rhs);
      auto longTag = builder_.getInt8(TAG_LONG);
      auto lIsLong = builder_.CreateICmpEQ(ltag, longTag);
      auto rIsLong = builder_.CreateICmpEQ(rtag, longTag);
      auto bothLong = builder_.CreateAnd(lIsLong, rIsLong, "both.long");
      auto intBB = build_block("reduce.int");
      auto slowBB = build_block("reduce.slow");
      auto mergeBB = build_block("reduce.merge");
      builder_.CreateCondBr(bothLong, intBB, slowBB);

      builder_.SetInsertPoint(intBB);
      auto ldata = extract_data(lhs);
      auto rdata = extract_data(rhs);
      auto cmp = builder_.CreateICmp(pred, ldata, rdata);
      auto intResult = make_long(builder_.CreateSelect(cmp, ldata, rdata));
      auto intEndBB = builder_.GetInsertBlock();
      builder_.CreateBr(mergeBB);

      builder_.SetInsertPoint(slowBB);
      llvm::IRBuilder<> entryB(
          &builder_.GetInsertBlock()->getParent()->getEntryBlock(),
          builder_.GetInsertBlock()->getParent()->getEntryBlock().begin());
      auto argsAlloca = entryB.CreateAlloca(valueType_,
                                            builder_.getInt64(2),
                                            "num.args2");
      builder_.CreateStore(lhs, argsAlloca);
      auto slot1 = builder_.CreateGEP(valueType_, argsAlloca,
                                      builder_.getInt64(1));
      builder_.CreateStore(rhs, slot1);
      auto slowResult = emit_call(
          module_->getFunction(rt_name),
          {argsAlloca, builder_.getInt64(2), line, col});
      auto slowEndBB = builder_.GetInsertBlock();
      builder_.CreateBr(mergeBB);

      builder_.SetInsertPoint(mergeBB);
      auto phi = builder_.CreatePHI(valueType_, 2, "reduce.r");
      phi->addIncoming(intResult, intEndBB);
      phi->addIncoming(slowResult, slowEndBB);
      return static_cast<llvm::Value*>(phi);
    };

    auto variadic_reduce = [&](const char* rt_name,
                               llvm::CmpInst::Predicate pred) -> llvm::Value* {
      auto n = argsAst.nodes.size();
      if (n < 2) return nullptr;
      if (n == 2) {
        auto a = compile(*argsAst.nodes[0]);
        auto b = compile(*argsAst.nodes[1]);
        auto r = reduce_pair(a, b, rt_name, pred);
        emit_value_release(a);
        emit_value_release(b);
        return r;
      }
      llvm::IRBuilder<> entryB(
          &builder_.GetInsertBlock()->getParent()->getEntryBlock(),
          builder_.GetInsertBlock()->getParent()->getEntryBlock().begin());
      auto argsAlloca = entryB.CreateAlloca(
          valueType_, builder_.getInt64(static_cast<int64_t>(n)),
          "num.args");
      std::vector<llvm::Value*> compiled;
      compiled.reserve(n);
      for (size_t i = 0; i < n; i++) {
        auto v = compile(*argsAst.nodes[i]);
        auto slot = builder_.CreateGEP(valueType_, argsAlloca,
                                       builder_.getInt64(i));
        builder_.CreateStore(v, slot);
        compiled.push_back(v);
      }
      auto result = emit_call(
          module_->getFunction(rt_name),
          {argsAlloca, builder_.getInt64(static_cast<int64_t>(n)),
           line, col});
      for (auto v : compiled) emit_value_release(v);
      return result;
    };
    if (method == "min") {
      if (auto v = variadic_reduce(rt::math_min,
                                   llvm::CmpInst::ICMP_SLT)) return v;
    }
    if (method == "max") {
      if (auto v = variadic_reduce(rt::math_max,
                                   llvm::CmpInst::ICMP_SGT)) return v;
    }

    if (method == "pow" && argsAst.nodes.size() == 2) {
      auto base = value_to_long(compile(*argsAst.nodes[0]));
      auto exp = value_to_long(compile(*argsAst.nodes[1]));
      auto r = emit_call(module_->getFunction(rt::math_pow),
                                   {base, exp, line, col});
      return make_long(r);
    }
    if (method == "sign" && argsAst.nodes.size() == 1) {
      auto x = value_to_long(compile(*argsAst.nodes[0]));
      auto zero = builder_.getInt64(0);
      auto is_neg = builder_.CreateICmpSLT(x, zero);
      auto is_pos = builder_.CreateICmpSGT(x, zero);
      auto pos_or_zero = builder_.CreateSelect(is_pos, builder_.getInt64(1),
                                               zero);
      auto r = builder_.CreateSelect(is_neg, builder_.getInt64(-1),
                                     pos_or_zero);
      return make_long(r);
    }
    if (method == "clamp" && argsAst.nodes.size() == 3) {
      auto x = value_to_long(compile(*argsAst.nodes[0]));
      auto lo = value_to_long(compile(*argsAst.nodes[1]));
      auto hi = value_to_long(compile(*argsAst.nodes[2]));
      auto above_lo = builder_.CreateSelect(builder_.CreateICmpSLT(x, lo),
                                            lo, x);
      auto r = builder_.CreateSelect(builder_.CreateICmpSGT(above_lo, hi),
                                     hi, above_lo);
      return make_long(r);
    }
  }

  if (ns == "IO") {
    if (method == "puts" && argsAst.nodes.size() == 1)
      return emit_output_call(rt::puts, argsAst);
    if (method == "print" && argsAst.nodes.size() == 1)
      return emit_output_call(rt::print, argsAst);
    if (method == "input" && argsAst.nodes.size() == 0) {
      auto s = emit_call(module_->getFunction(rt::input), {});
      return make_string(s);
    }
    if (method == "read" && argsAst.nodes.size() == 1) {
      auto arg = compile(*argsAst.nodes[0]);
      emit_type_check(arg, "String", "IO.read argument");
      auto ptr = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      auto s = emit_call(
          module_->getFunction(rt::read_file), {ptr, line, col});
      emit_value_release(arg);
      return make_string(s);
    }
    if (method == "write" && argsAst.nodes.size() == 2) {
      auto p = compile(*argsAst.nodes[0]);
      emit_type_check(p, "String", "IO.write path");
      auto c = compile(*argsAst.nodes[1]);
      emit_type_check(c, "String", "IO.write content");
      auto pp = builder_.CreateIntToPtr(extract_data(p), ptrTy);
      auto cp = builder_.CreateIntToPtr(extract_data(c), ptrTy);
      emit_call(module_->getFunction(rt::write_file),
                          {pp, cp, line, col});
      emit_value_release(p);
      emit_value_release(c);
      return make_nil();
    }
    if (method == "exists" && argsAst.nodes.size() == 1) {
      auto arg = compile(*argsAst.nodes[0]);
      emit_type_check(arg, "String", "IO.exists argument");
      auto ptr = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      auto i = emit_call(module_->getFunction(rt::io_exists),
                                   {ptr});
      emit_value_release(arg);
      auto b = builder_.CreateICmpNE(i, builder_.getInt64(0));
      return make_bool(b);
    }
  }

  if (ns == "Random") {
    if (method == "seed" && argsAst.nodes.size() == 1) {
      auto n = value_to_long(compile(*argsAst.nodes[0]));
      emit_call(module_->getFunction(rt::random_seed), {n});
      return make_nil();
    }
    if (method == "int" && argsAst.nodes.size() == 2) {
      auto lo = value_to_long(compile(*argsAst.nodes[0]));
      auto hi = value_to_long(compile(*argsAst.nodes[1]));
      auto r = emit_call(
          module_->getFunction(rt::random_int), {lo, hi, line, col});
      return make_long(r);
    }
    auto num_pair_call = [&](const char* rt_name) -> llvm::Value* {
      if (argsAst.nodes.size() != 2) return nullptr;
      auto a = compile(*argsAst.nodes[0]);
      auto b = compile(*argsAst.nodes[1]);
      auto r = emit_call(
          module_->getFunction(rt_name),
          {extract_tag(a), extract_data(a),
           extract_tag(b), extract_data(b)});
      emit_value_release(a);
      emit_value_release(b);
      return r;
    };
    if (method == "uniform") {
      if (auto v = num_pair_call(rt::random_uniform)) return v;
    }
    if (method == "gauss") {
      if (auto v = num_pair_call(rt::random_gauss)) return v;
    }
    if (method == "shuffle" && argsAst.nodes.size() == 1) {
      auto arg = compile(*argsAst.nodes[0]);
      emit_type_check(arg, "Array", "Random.shuffle argument");
      auto ap = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      emit_call(module_->getFunction(rt::random_shuffle), {ap});
      emit_value_release(arg);
      return make_nil();
    }
    if (method == "weighted_choice" && argsAst.nodes.size() == 2) {
      auto pop = compile(*argsAst.nodes[0]);
      emit_type_check(pop, "Array", "Random.weighted_choice population");
      auto wts = compile(*argsAst.nodes[1]);
      emit_type_check(wts, "Array", "Random.weighted_choice weights");
      auto pp = builder_.CreateIntToPtr(extract_data(pop), ptrTy);
      auto wp = builder_.CreateIntToPtr(extract_data(wts), ptrTy);
      auto r = emit_call(
          module_->getFunction(rt::random_weighted_choice),
          {pp, wp, line, col});
      emit_value_release(pop);
      emit_value_release(wts);
      return r;
    }
  }

  if (ns == "Sys") {
    if (method == "exit" && argsAst.nodes.size() == 1) {
      auto code = value_to_long(compile(*argsAst.nodes[0]));
      emit_call(module_->getFunction(rt::sys_exit), {code});
      return make_nil();  // unreachable; sys_exit terminates the process
    }
    if (method == "env" && argsAst.nodes.size() == 1) {
      auto arg = compile(*argsAst.nodes[0]);
      emit_type_check(arg, "String", "Sys.env argument");
      auto ptr = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      auto s = emit_call(module_->getFunction(rt::sys_env), {ptr});
      emit_value_release(arg);
      return make_string(s);
    }
    if (method == "time" && argsAst.nodes.size() == 0) {
      auto t = emit_call(module_->getFunction(rt::sys_time), {});
      return make_float(t);
    }
  }

  if (ns == "Tensor") {
    auto pack_args = [&]() {
      auto n = argsAst.nodes.size();
      llvm::IRBuilder<> entryB(
          &builder_.GetInsertBlock()->getParent()->getEntryBlock(),
          builder_.GetInsertBlock()->getParent()->getEntryBlock().begin());
      auto argsAlloca = entryB.CreateAlloca(
          valueType_, builder_.getInt64(static_cast<int64_t>(std::max<size_t>(n, 1))),
          "tensor.args");
      std::vector<llvm::Value*> compiled;
      compiled.reserve(n);
      for (size_t i = 0; i < n; i++) {
        auto v = compile(*argsAst.nodes[i]);
        auto slot = builder_.CreateGEP(valueType_, argsAlloca,
                                       builder_.getInt64(i));
        builder_.CreateStore(v, slot);
        compiled.push_back(v);
      }
      return std::make_pair(argsAlloca, std::move(compiled));
    };

    if (method == "zeros" || method == "ones" || method == "randn") {
      auto [argsAlloca, compiled] = pack_args();
      const char* rt_name = method == "zeros" ? rt::tensor_zeros
                          : method == "ones"  ? rt::tensor_ones
                                              : rt::tensor_randn;
      auto t = emit_call(
          module_->getFunction(rt_name),
          {argsAlloca,
           builder_.getInt64(static_cast<int64_t>(argsAst.nodes.size())),
           line, col});
      for (auto v : compiled) emit_value_release(v);
      return make_tensor(t);
    }
    if (method == "from" && argsAst.nodes.size() == 1) {
      auto arg = compile(*argsAst.nodes[0]);
      emit_type_check(arg, "Array", "Tensor.from argument");
      auto ap = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      auto t = emit_call(
          module_->getFunction(rt::tensor_from), {ap, line, col});
      emit_value_release(arg);
      return make_tensor(t);
    }
    if (method == "from_csv" && argsAst.nodes.size() == 1) {
      auto arg = compile(*argsAst.nodes[0]);
      emit_type_check(arg, "String", "Tensor.from_csv argument");
      auto pp = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      auto t = emit_call(
          module_->getFunction(rt::tensor_from_csv), {pp});
      emit_value_release(arg);
      return make_tensor(t);
    }
    if ((method == "sigmoid" || method == "relu" || method == "softmax") &&
        argsAst.nodes.size() == 1) {
      auto arg = compile(*argsAst.nodes[0]);
      emit_type_check(arg, "Tensor",
                      method == "sigmoid" ? "Tensor.sigmoid argument"
                    : method == "relu"    ? "Tensor.relu argument"
                                          : "Tensor.softmax argument");
      auto ap = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      int op_id = method == "sigmoid" ? static_cast<int>(culebra::Op::Sigmoid)
                : method == "relu"    ? static_cast<int>(culebra::Op::Relu)
                                      : static_cast<int>(culebra::Op::Softmax);
      auto t = emit_call(
          module_->getFunction(rt::tensor_unary),
          {ap, builder_.getInt64(op_id)});
      emit_value_release(arg);
      return make_tensor(t);
    }
    if (method == "eval") {
      // Variadic: each arg must be a Tensor; we just call eval_one
      // on each and release. No alloca needed.
      for (size_t i = 0; i < argsAst.nodes.size(); i++) {
        auto v = compile(*argsAst.nodes[i]);
        emit_type_check(v, "Tensor", "Tensor.eval argument");
        auto p = builder_.CreateIntToPtr(extract_data(v), ptrTy);
        emit_call(module_->getFunction(rt::tensor_eval_one), {p});
        emit_value_release(v);
      }
      return make_nil();
    }
  }

  return nullptr;
}

inline llvm::Value* JitExtension::compile_ns_prop(JIT& jit,
                                                    std::string_view ns,
                                                    std::string_view prop) {
  CULEBRA_JIT_EXT_BODY_ALIASES(jit);
  if (ns == "Sys" && prop == "argv") {
    auto arr = emit_call(module_->getFunction(rt::sys_argv), {});
    return make_array(arr);
  }
  if (ns == "Math") {
    auto emit = [&](double d) {
      return make_float(llvm::ConstantFP::get(builder_.getDoubleTy(), d));
    };
    if (prop == "pi")  return emit(M_PI);
    if (prop == "e")   return emit(M_E);
    if (prop == "inf") return emit(std::numeric_limits<double>::infinity());
    if (prop == "nan") return emit(std::numeric_limits<double>::quiet_NaN());
  }
  return nullptr;
}

inline llvm::Value* JitExtension::emit_output_call(JIT& jit,
                                                     const char* rt_name,
                                                     const peg::Ast& argsAst) {
  auto arg = jit.compile(*argsAst.nodes[0]);
  jit.emit_call(jit.module_->getFunction(rt_name),
                      {jit.extract_tag(arg), jit.extract_data(arg)});
  jit.emit_value_release(arg);
  return jit.make_nil();
}

inline bool JitExtension::is_builtin_var(const std::string& name) {
  static const std::unordered_set<std::string_view> names = {
      "puts",    "print",     "assert",
      "to_long", "to_float",  "to_string", "type_of",
      "Math",    "IO",        "Random",    "Sys"};
  return names.contains(name);
}

inline llvm::Value* JitExtension::compile_ufcs_builtin(
    JIT& jit, const std::string& method, const peg::Ast& argsAst,
    llvm::Value* receiver) {
  if (argsAst.nodes.size() != 0) return nullptr;
  CULEBRA_JIT_EXT_BODY_ALIASES(jit);
  auto line = jit.current_line_val();
  auto col = jit.current_column_val();
  auto ptrTy = llvm::PointerType::get(ctx_, 0);
  if (method == "puts" || method == "print") {
    auto rt_name = method == "puts" ? rt::puts : rt::print;
    emit_call(module_->getFunction(rt_name),
                        {extract_tag(receiver), extract_data(receiver)});
    emit_value_release(receiver);
    return make_nil();
  }
  if (method == "assert") {
    emit_call(
        module_->getFunction(rt::assert_),
        {extract_tag(receiver), extract_data(receiver), line, col});
    emit_value_release(receiver);
    return make_nil();
  }
  if (method == "to_long") {
    emit_type_check(receiver, "String", "to_long argument");
    auto strPtr =
        builder_.CreateIntToPtr(extract_data(receiver), ptrTy);
    auto r = emit_call(
        module_->getFunction(rt::to_long), {strPtr, line, col});
    emit_value_release(receiver);
    return make_long(r);
  }
  if (method == "to_string") {
    auto s = emit_call(
        module_->getFunction(rt::value_to_display),
        {extract_tag(receiver), extract_data(receiver)});
    emit_value_release(receiver);
    return make_string(s);
  }
  if (method == "type_of") {
    auto s = emit_call(module_->getFunction(rt::type_of),
                                 {extract_tag(receiver)});
    emit_value_release(receiver);
    return make_string(s);
  }
  return nullptr;
}

#undef CULEBRA_JIT_EXT_BODY_ALIASES

}  // namespace culebra
