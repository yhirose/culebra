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

extern "C" {

// --- Globals ---

__attribute__((used)) inline int64_t culebra_runtime_to_long(
    const char* s, int64_t line, int64_t col) {
  auto fail = [&]() {
    throw std::runtime_error(std::format("type error at {}:{}.", line, col));
  };
  if (!s) fail();
  size_t i = 0, j = std::strlen(s);
  while (i < j && std::isspace(static_cast<unsigned char>(s[i]))) i++;
  while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) j--;
  if (i == j) fail();
  try {
    size_t used = 0;
    std::string t(s + i, j - i);
    long v = std::stol(t, &used, 10);
    if (used != t.size()) fail();
    return static_cast<int64_t>(v);
  } catch (const std::runtime_error&) {
    throw;
  } catch (...) {
    fail();
  }
  return 0;  // unreachable
}

__attribute__((used)) inline const char* culebra_runtime_type_of(int8_t tag) {
  switch (tag) {
    case 0: return "Nil";
    case 1: return "Bool";
    case 2: return "Long";
    case 3: return "Function";
    case 4: return "String";
    case 5: return "Array";
    case 6: return "Object";
  }
  return "Unknown";
}

__attribute__((used)) inline JitArray* culebra_runtime_range(int64_t start,
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
    culebra_runtime_array_push(r, culebra::TAG_STRING,
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
  module_->getOrInsertFunction(rt::type_of, ptrTy,
                               builder_.getInt8Ty());
  module_->getOrInsertFunction(rt::range, ptrTy,
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

  if (name == "range" && argsAst.nodes.size() == 1) {
    auto end = value_to_long(compile(*argsAst.nodes[0]));
    auto arr = builder_.CreateCall(module_->getFunction(rt::range),
                                   {builder_.getInt64(0), end});
    return make_array(arr);
  }

  if (name == "range" && argsAst.nodes.size() == 2) {
    auto start = value_to_long(compile(*argsAst.nodes[0]));
    auto end = value_to_long(compile(*argsAst.nodes[1]));
    auto arr = builder_.CreateCall(module_->getFunction(rt::range),
                                   {start, end});
    return make_array(arr);
  }

  if (name == "to_long" && argsAst.nodes.size() == 1) {
    auto arg = compile(*argsAst.nodes[0]);
    emit_type_check(arg, "String", "to_long argument");
    auto ptr = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
    auto result = builder_.CreateCall(
        module_->getFunction(rt::to_long), {ptr, line, col});
    emit_value_release(arg);
    return make_long(result);
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
