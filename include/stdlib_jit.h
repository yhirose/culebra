#pragma once

// JIT-side implementation of the Culebra standard library. Fragment header;
// include once at the end of jit.h.

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

}  // extern "C"

// ---------------------------------------------------------------------------
// JIT compile-side dispatch (member function definitions)
// ---------------------------------------------------------------------------

namespace culebra {

inline void JIT::declare_stdlib_runtime() {
  auto ptrTy = llvm::PointerType::get(ctx_, 0);

  module_->getOrInsertFunction("culebra_runtime_print", builder_.getVoidTy(),
                               builder_.getInt8Ty(), builder_.getInt64Ty());
  module_->getOrInsertFunction("culebra_runtime_to_long",
                               builder_.getInt64Ty(), ptrTy,
                               builder_.getInt64Ty(), builder_.getInt64Ty());
  module_->getOrInsertFunction("culebra_runtime_type_of", ptrTy,
                               builder_.getInt8Ty());
  module_->getOrInsertFunction("culebra_runtime_range", ptrTy,
                               builder_.getInt64Ty(), builder_.getInt64Ty());
  module_->getOrInsertFunction("culebra_runtime_input", ptrTy);
  module_->getOrInsertFunction("culebra_runtime_read_file", ptrTy, ptrTy,
                               builder_.getInt64Ty(), builder_.getInt64Ty());
  module_->getOrInsertFunction("culebra_runtime_write_file",
                               builder_.getVoidTy(), ptrTy, ptrTy,
                               builder_.getInt64Ty(), builder_.getInt64Ty());
}

inline llvm::Value* JIT::try_compile_stdlib_global(const std::string& name,
                                                   const peg::Ast& argsAst,
                                                   const peg::Ast& callAst) {
  auto ptrTy = llvm::PointerType::get(ctx_, 0);
  auto line = builder_.getInt64(callAst.line);
  auto col = builder_.getInt64(callAst.column);

  if (name == "puts" && argsAst.nodes.size() == 1) {
    auto arg = compile(*argsAst.nodes[0]);
    builder_.CreateCall(module_->getFunction("culebra_runtime_puts"),
                        {extract_tag(arg), extract_data(arg)});
    emit_value_release(arg);
    return make_nil();
  }

  if (name == "print" && argsAst.nodes.size() == 1) {
    auto arg = compile(*argsAst.nodes[0]);
    builder_.CreateCall(module_->getFunction("culebra_runtime_print"),
                        {extract_tag(arg), extract_data(arg)});
    emit_value_release(arg);
    return make_nil();
  }

  if (name == "assert" && argsAst.nodes.size() == 1) {
    auto arg = compile(*argsAst.nodes[0]);
    builder_.CreateCall(
        module_->getFunction("culebra_runtime_assert"),
        {extract_tag(arg), extract_data(arg), line, col});
    emit_value_release(arg);
    return make_nil();
  }

  if (name == "abs" && argsAst.nodes.size() == 1) {
    auto arg = compile(*argsAst.nodes[0]);
    auto x = value_to_long(arg);
    auto isNeg = builder_.CreateICmpSLT(x, builder_.getInt64(0));
    auto r = builder_.CreateSelect(isNeg, builder_.CreateNeg(x), x);
    return make_long(r);
  }

  if (name == "min" && argsAst.nodes.size() == 2) {
    auto a = value_to_long(compile(*argsAst.nodes[0]));
    auto b = value_to_long(compile(*argsAst.nodes[1]));
    auto r = builder_.CreateSelect(builder_.CreateICmpSLT(a, b), a, b);
    return make_long(r);
  }

  if (name == "max" && argsAst.nodes.size() == 2) {
    auto a = value_to_long(compile(*argsAst.nodes[0]));
    auto b = value_to_long(compile(*argsAst.nodes[1]));
    auto r = builder_.CreateSelect(builder_.CreateICmpSGT(a, b), a, b);
    return make_long(r);
  }

  if (name == "range" && argsAst.nodes.size() == 1) {
    auto end = value_to_long(compile(*argsAst.nodes[0]));
    auto arr = builder_.CreateCall(module_->getFunction("culebra_runtime_range"),
                                   {builder_.getInt64(0), end});
    return make_array(arr);
  }

  if (name == "range" && argsAst.nodes.size() == 2) {
    auto start = value_to_long(compile(*argsAst.nodes[0]));
    auto end = value_to_long(compile(*argsAst.nodes[1]));
    auto arr = builder_.CreateCall(module_->getFunction("culebra_runtime_range"),
                                   {start, end});
    return make_array(arr);
  }

  if (name == "to_long" && argsAst.nodes.size() == 1) {
    auto arg = compile(*argsAst.nodes[0]);
    emit_type_check(arg, "String", "to_long argument");
    auto ptr = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
    auto result = builder_.CreateCall(
        module_->getFunction("culebra_runtime_to_long"), {ptr, line, col});
    emit_value_release(arg);
    return make_long(result);
  }

  if (name == "to_string" && argsAst.nodes.size() == 1) {
    auto arg = compile(*argsAst.nodes[0]);
    auto s = builder_.CreateCall(
        module_->getFunction("culebra_runtime_value_to_display"),
        {extract_tag(arg), extract_data(arg)});
    emit_value_release(arg);
    return make_string(s);
  }

  if (name == "type_of" && argsAst.nodes.size() == 1) {
    auto arg = compile(*argsAst.nodes[0]);
    auto s = builder_.CreateCall(module_->getFunction("culebra_runtime_type_of"),
                                 {extract_tag(arg)});
    emit_value_release(arg);
    return make_string(s);
  }

  if (name == "input" && argsAst.nodes.size() == 0) {
    auto s = builder_.CreateCall(module_->getFunction("culebra_runtime_input"),
                                 {});
    return make_string(s);
  }

  if (name == "read_file" && argsAst.nodes.size() == 1) {
    auto arg = compile(*argsAst.nodes[0]);
    emit_type_check(arg, "String", "read_file argument");
    auto ptr = builder_.CreateIntToPtr(extract_data(arg), ptrTy);
    auto s = builder_.CreateCall(
        module_->getFunction("culebra_runtime_read_file"), {ptr, line, col});
    emit_value_release(arg);
    return make_string(s);
  }

  if (name == "write_file" && argsAst.nodes.size() == 2) {
    auto p = compile(*argsAst.nodes[0]);
    emit_type_check(p, "String", "write_file path");
    auto c = compile(*argsAst.nodes[1]);
    emit_type_check(c, "String", "write_file content");
    auto pp = builder_.CreateIntToPtr(extract_data(p), ptrTy);
    auto cp = builder_.CreateIntToPtr(extract_data(c), ptrTy);
    builder_.CreateCall(module_->getFunction("culebra_runtime_write_file"),
                        {pp, cp, line, col});
    emit_value_release(p);
    emit_value_release(c);
    return make_nil();
  }

  return nullptr;
}

}  // namespace culebra
