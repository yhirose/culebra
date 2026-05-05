#pragma once

// JIT-side implementation of the Culebra standard library. Fragment header;
// include once at the end of jit.h.

#include <cstdlib>

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------

namespace culebra {
// Holder for Sys.argv under the JIT. main.cc (or any embedder) populates
// this before calling JIT::run; culebra_runtime_sys_argv() materializes
// a fresh JitArray from it on each access.
inline std::vector<std::string>& _culebra_sys_argv_holder() {
  static std::vector<std::string> v;
  return v;
}
}  // namespace culebra

// ---------------------------------------------------------------------------
// Runtime functions callable from JIT'd code
// ---------------------------------------------------------------------------

// Bring the shared numeric helpers into scope so the extern "C"
// runtime wrappers below can use unqualified names.
using culebra::throw_type_error_at;
using culebra::parse_long_strict;
using culebra::parse_double_strict;

extern "C" {

// --- Globals ---

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

__attribute__((used)) inline JitArray* culebra_runtime_iota(int64_t start,
                                                            int64_t end) {
  auto* r = culebra_runtime_array_new();
  for (int64_t i = start; i < end; i++) {
    culebra_runtime_array_push(r, /*tag Long*/ 2, i);
  }
  return r;
}

__attribute__((used)) inline void culebra_runtime_print(int8_t type,
                                                        int64_t data) {
  if (type == 4) {
    std::cout << reinterpret_cast<const char*>(data);
  } else {
    std::cout << _culebra_value_to_str_impl(type, data);
  }
}

// --- I/O ---

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
    throw std::runtime_error(std::format("type error at {}:{}.", line, col));
  }
  std::string s((std::istreambuf_iterator<char>(ifs)),
                std::istreambuf_iterator<char>());
  return _culebra_heap_str(s);
}

__attribute__((used)) inline void culebra_runtime_write_file(
    const char* path, const char* content, int64_t line, int64_t col) {
  std::ofstream ofs(path, std::ios::binary);
  if (!ofs) {
    throw std::runtime_error(std::format("type error at {}:{}.", line, col));
  }
  auto len = std::strlen(content);
  ofs.write(content, static_cast<std::streamsize>(len));
}

// --- Math / Sys ---

__attribute__((used)) inline int64_t culebra_runtime_math_pow(
    int64_t base, int64_t exp, int64_t line, int64_t col) {
  if (exp < 0) {
    throw std::runtime_error(std::format("type error at {}:{}.", line, col));
  }
  int64_t r = 1;
  while (exp > 0) {
    if (exp & 1) r *= base;
    base *= base;
    exp >>= 1;
  }
  return r;
}

__attribute__((used)) inline void culebra_runtime_sys_exit(int64_t code) {
  std::exit(static_cast<int>(code));
}

__attribute__((used)) inline const char* culebra_runtime_sys_env(
    const char* name) {
  const char* v = std::getenv(name);
  return _culebra_heap_str(std::string(v ? v : ""));
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

// ---------------------------------------------------------------------------
// JIT compile-side dispatch (member function definitions)
// ---------------------------------------------------------------------------

namespace culebra {

inline void JIT::declare_stdlib_runtime() {
  auto ptrTy = llvm::PointerType::get(ctx_, 0);

  module_->getOrInsertFunction(rt::print, builder_.getVoidTy(),
                               builder_.getInt8Ty(), builder_.getInt64Ty());
  module_->getOrInsertFunction(rt::to_long,
                               builder_.getInt64Ty(), ptrTy,
                               builder_.getInt64Ty(), builder_.getInt64Ty());
  module_->getOrInsertFunction(rt::to_long_any, valueType_,
                               builder_.getInt8Ty(), builder_.getInt64Ty(),
                               builder_.getInt64Ty(), builder_.getInt64Ty());
  module_->getOrInsertFunction(rt::to_float_any, valueType_,
                               builder_.getInt8Ty(), builder_.getInt64Ty(),
                               builder_.getInt64Ty(), builder_.getInt64Ty());
  module_->getOrInsertFunction(rt::type_of, ptrTy,
                               builder_.getInt8Ty());
  module_->getOrInsertFunction(rt::iota, ptrTy,
                               builder_.getInt64Ty(), builder_.getInt64Ty());
  module_->getOrInsertFunction(rt::math_range, ptrTy,
                               builder_.getInt64Ty(), builder_.getInt64Ty());
  module_->getOrInsertFunction(rt::input, ptrTy);
  module_->getOrInsertFunction(rt::read_file, ptrTy, ptrTy,
                               builder_.getInt64Ty(), builder_.getInt64Ty());
  module_->getOrInsertFunction(rt::write_file,
                               builder_.getVoidTy(), ptrTy, ptrTy,
                               builder_.getInt64Ty(), builder_.getInt64Ty());
  module_->getOrInsertFunction(rt::math_pow,
                               builder_.getInt64Ty(),
                               builder_.getInt64Ty(), builder_.getInt64Ty(),
                               builder_.getInt64Ty(), builder_.getInt64Ty());
  module_->getOrInsertFunction(rt::sys_exit, builder_.getVoidTy(),
                               builder_.getInt64Ty());
  module_->getOrInsertFunction(rt::sys_env, ptrTy, ptrTy);
  module_->getOrInsertFunction(rt::sys_argv, ptrTy);

  // Iterator terminal methods.
  auto i64 = builder_.getInt64Ty();
  auto i8 = builder_.getInt8Ty();
  module_->getOrInsertFunction(rt::iter_collect, ptrTy, i8, i64);
  module_->getOrInsertFunction(rt::iter_count, i64, i8, i64);
  module_->getOrInsertFunction(rt::iter_for_each, builder_.getVoidTy(),
                               i8, i64, i8, i64, i64, i64);
  module_->getOrInsertFunction(rt::iter_reduce, builder_.getVoidTy(),
                               i8, i64, i8, i64, i8, i64, i64, i64,
                               ptrTy, ptrTy);
  module_->getOrInsertFunction(rt::iter_find, builder_.getVoidTy(),
                               i8, i64, i8, i64, i64, i64, ptrTy, ptrTy);
  module_->getOrInsertFunction(rt::iter_any, i64, i8, i64, i8, i64, i64,
                               i64);
  // sum/product/min/max: (it, id, line, col) -> i64
  module_->getOrInsertFunction(rt::iter_sum, i64, i8, i64, i64, i64);
  module_->getOrInsertFunction(rt::iter_product, i64, i8, i64, i64, i64);
  module_->getOrInsertFunction(rt::iter_min, i64, i8, i64, i64, i64);
  module_->getOrInsertFunction(rt::iter_max, i64, i8, i64, i64, i64);
  module_->getOrInsertFunction(rt::iter_all, i64, i8, i64, i8, i64, i64,
                               i64);

  // Iterator lazy factories.
  module_->getOrInsertFunction(rt::iter_map, ptrTy, i8, i64, i8, i64);
  module_->getOrInsertFunction(rt::iter_filter, ptrTy, i8, i64, i8, i64);
  module_->getOrInsertFunction(rt::iter_take, ptrTy, i8, i64, i64);
  module_->getOrInsertFunction(rt::iter_skip, ptrTy, i8, i64, i64);
  module_->getOrInsertFunction(rt::iter_take_while, ptrTy, i8, i64, i8, i64);
  // chain/zip/flat_map carry line+col for the "not iterable" error.
  module_->getOrInsertFunction(rt::iter_chain, ptrTy, i8, i64, i8, i64,
                               i64, i64);
  module_->getOrInsertFunction(rt::iter_zip, ptrTy, i8, i64, i8, i64,
                               i64, i64);
  module_->getOrInsertFunction(rt::iter_enumerate, ptrTy, i8, i64);
  module_->getOrInsertFunction(rt::iter_flat_map, ptrTy, i8, i64, i8, i64,
                               i64, i64);
  // (next_cls, iter_tag, iter_data, &out_tag, &out_data) -> i64 (1/0)
  module_->getOrInsertFunction(rt::iter_advance, i64, ptrTy, i8, i64, ptrTy,
                               ptrTy);
  module_->getOrInsertFunction(rt::str_code_points, ptrTy, ptrTy);
  module_->getOrInsertFunction(rt::str_graphemes, ptrTy, ptrTy);
  module_->getOrInsertFunction(rt::array_iter, ptrTy, ptrTy);
  module_->getOrInsertFunction(rt::object_iter, ptrTy, ptrTy);
}

inline llvm::Value* JIT::try_compile_stdlib_global(const std::string& name,
                                                   const peg::Ast& argsAst,
                                                   const peg::Ast& callAst) {
  auto ptrTy = llvm::PointerType::get(ctx_, 0);
  auto line = builder_.getInt64(callAst.line);
  auto col = builder_.getInt64(callAst.column);

  if (name == "puts" && argsAst.nodes.size() == 1)
    return emit_output_call(rt::puts, argsAst);
  if (name == "print" && argsAst.nodes.size() == 1)
    return emit_output_call(rt::print, argsAst);

  if (name == "assert" && argsAst.nodes.size() == 1) {
    auto arg = compile(*argsAst.nodes[0]);
    builder_.CreateCall(
        module_->getFunction(rt::assert_),
        {extract_tag(arg), extract_data(arg), line, col});
    emit_value_release(arg);
    return make_nil();
  }

  if (name == "to_long" && argsAst.nodes.size() == 1) {
    auto arg = compile(*argsAst.nodes[0]);
    // Polymorphic: Long/Float/String. The runtime helper dispatches
    // on tag (Long identity, Float truncate, String parse) and
    // raises `type error` for anything else.
    auto result = builder_.CreateCall(
        module_->getFunction(rt::to_long_any),
        {extract_tag(arg), extract_data(arg), line, col});
    emit_value_release(arg);
    return result;
  }

  if (name == "to_float" && argsAst.nodes.size() == 1) {
    auto arg = compile(*argsAst.nodes[0]);
    auto result = builder_.CreateCall(
        module_->getFunction(rt::to_float_any),
        {extract_tag(arg), extract_data(arg), line, col});
    emit_value_release(arg);
    return result;
  }

  if (name == "to_string" && argsAst.nodes.size() == 1) {
    auto arg = compile(*argsAst.nodes[0]);
    auto s = builder_.CreateCall(
        module_->getFunction(rt::value_to_display),
        {extract_tag(arg), extract_data(arg)});
    emit_value_release(arg);
    return make_string(s);
  }

  if (name == "type_of" && argsAst.nodes.size() == 1) {
    auto arg = compile(*argsAst.nodes[0]);
    auto s = builder_.CreateCall(module_->getFunction(rt::type_of),
                                 {extract_tag(arg)});
    emit_value_release(arg);
    return make_string(s);
  }

  return nullptr;
}

inline llvm::Value* JIT::try_compile_stdlib_namespace(
    std::string_view ns, std::string_view method,
    const peg::Ast& argsAst, const peg::Ast& callAst) {
  auto ptrTy = llvm::PointerType::get(ctx_, 0);
  auto line = builder_.getInt64(callAst.line);
  auto col = builder_.getInt64(callAst.column);

  if (ns == "Math") {
    if (method == "abs" && argsAst.nodes.size() == 1) {
      auto arg = compile(*argsAst.nodes[0]);
      auto x = value_to_long(arg);
      auto isNeg = builder_.CreateICmpSLT(x, builder_.getInt64(0));
      auto r = builder_.CreateSelect(isNeg, builder_.CreateNeg(x), x);
      return make_long(r);
    }
    if (method == "min" && argsAst.nodes.size() == 2) {
      auto a = value_to_long(compile(*argsAst.nodes[0]));
      auto b = value_to_long(compile(*argsAst.nodes[1]));
      auto r = builder_.CreateSelect(builder_.CreateICmpSLT(a, b), a, b);
      return make_long(r);
    }
    if (method == "max" && argsAst.nodes.size() == 2) {
      auto a = value_to_long(compile(*argsAst.nodes[0]));
      auto b = value_to_long(compile(*argsAst.nodes[1]));
      auto r = builder_.CreateSelect(builder_.CreateICmpSGT(a, b), a, b);
      return make_long(r);
    }
    if (method == "pow" && argsAst.nodes.size() == 2) {
      auto base = value_to_long(compile(*argsAst.nodes[0]));
      auto exp = value_to_long(compile(*argsAst.nodes[1]));
      auto r = builder_.CreateCall(module_->getFunction(rt::math_pow),
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
    if (method == "iota" && argsAst.nodes.size() == 1) {
      auto end = value_to_long(compile(*argsAst.nodes[0]));
      auto arr = builder_.CreateCall(module_->getFunction(rt::iota),
                                     {builder_.getInt64(0), end});
      return make_array(arr);
    }
    if (method == "iota" && argsAst.nodes.size() == 2) {
      auto start = value_to_long(compile(*argsAst.nodes[0]));
      auto end = value_to_long(compile(*argsAst.nodes[1]));
      auto arr = builder_.CreateCall(module_->getFunction(rt::iota),
                                     {start, end});
      return make_array(arr);
    }
    if (method == "range" &&
        (argsAst.nodes.size() == 1 || argsAst.nodes.size() == 2)) {
      llvm::Value* start_v;
      llvm::Value* end_v;
      if (argsAst.nodes.size() == 1) {
        start_v = builder_.getInt64(0);
        end_v = value_to_long(compile(*argsAst.nodes[0]));
      } else {
        start_v = value_to_long(compile(*argsAst.nodes[0]));
        end_v = value_to_long(compile(*argsAst.nodes[1]));
      }
      auto obj = builder_.CreateCall(
          module_->getFunction(rt::math_range), {start_v, end_v});
      return make_object(obj);
    }
  }

  if (ns == "IO") {
    if (method == "puts" && argsAst.nodes.size() == 1)
      return emit_output_call(rt::puts, argsAst);
    if (method == "print" && argsAst.nodes.size() == 1)
      return emit_output_call(rt::print, argsAst);
    if (method == "input" && argsAst.nodes.size() == 0) {
      auto s = builder_.CreateCall(module_->getFunction(rt::input), {});
      return make_string(s);
    }
    if (method == "read" && argsAst.nodes.size() == 1) {
      auto arg = compile(*argsAst.nodes[0]);
      emit_type_check(arg, "String", "IO.read argument");
      auto ptr = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      auto s = builder_.CreateCall(
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
      builder_.CreateCall(module_->getFunction(rt::write_file),
                          {pp, cp, line, col});
      emit_value_release(p);
      emit_value_release(c);
      return make_nil();
    }
  }

  if (ns == "Sys") {
    if (method == "exit" && argsAst.nodes.size() == 1) {
      auto code = value_to_long(compile(*argsAst.nodes[0]));
      builder_.CreateCall(module_->getFunction(rt::sys_exit), {code});
      return make_nil();  // unreachable; sys_exit terminates the process
    }
    if (method == "env" && argsAst.nodes.size() == 1) {
      auto arg = compile(*argsAst.nodes[0]);
      emit_type_check(arg, "String", "Sys.env argument");
      auto ptr = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
      auto s = builder_.CreateCall(module_->getFunction(rt::sys_env), {ptr});
      emit_value_release(arg);
      return make_string(s);
    }
  }

  return nullptr;
}

inline llvm::Value* JIT::try_compile_stdlib_namespace_property(
    std::string_view ns, std::string_view prop) {
  if (ns == "Sys" && prop == "argv") {
    auto arr = builder_.CreateCall(module_->getFunction(rt::sys_argv), {});
    return make_array(arr);
  }
  return nullptr;
}

inline llvm::Value* JIT::emit_output_call(const char* rt_name,
                                           const peg::Ast& argsAst) {
  auto arg = compile(*argsAst.nodes[0]);
  builder_.CreateCall(module_->getFunction(rt_name),
                      {extract_tag(arg), extract_data(arg)});
  emit_value_release(arg);
  return make_nil();
}

}  // namespace culebra
