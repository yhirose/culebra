#pragma once
// Canonical native/built-in signature data — the single source of truth
// since the tree-walker's environment retired (Phase 4 B7-f; this file was
// generated from it by tools/gen_canon_sigs.cc while both existed). A
// signature change edits this table by hand — see canon_sigs.h for the
// structs and lookup helpers.
// clang-format off

namespace culebra {

inline constexpr CanonParam kCanonParams_Embed[] = {
  // 0: Embed.dir
  {"name", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Embed[] = {
  {"Embed", "", "dir", kCanonParams_Embed + 0, 1, "Object", 1, 1, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_IO[] = {
  // 0: IO.inspect
  {"arg", false, false, false, false, true, "", CanonDefault::None, 0, {}},
  // 1: IO.print
  {"arg", false, false, false, false, true, "", CanonDefault::None, 0, {}},
  // 2: IO.println
  {"arg", true, false, false, false, true, "", CanonDefault::Str, 0, ""},
  // 3: IO.einspect
  {"arg", false, false, false, false, true, "", CanonDefault::None, 0, {}},
  // 4: IO.eprint
  {"arg", false, false, false, false, true, "", CanonDefault::None, 0, {}},
  // 5: IO.eprintln
  {"arg", false, false, false, false, true, "", CanonDefault::None, 0, {}},
  // 6: IO.capture
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_IO[] = {
  {"IO", "", "inspect", kCanonParams_IO + 0, 1, "", 1, 1, false, -1, -1, -1},
  {"IO", "", "print", kCanonParams_IO + 1, 1, "", 1, 1, false, -1, -1, -1},
  {"IO", "", "println", kCanonParams_IO + 2, 1, "", 0, 1, false, -1, -1, -1},
  {"IO", "", "input", nullptr, 0, "String", 0, 0, false, -1, -1, -1},
  {"IO", "", "stdin", nullptr, 0, "Object", 0, 0, false, -1, -1, -1},
  {"IO", "", "einspect", kCanonParams_IO + 3, 1, "", 1, 1, false, -1, -1, -1},
  {"IO", "", "eprint", kCanonParams_IO + 4, 1, "", 1, 1, false, -1, -1, -1},
  {"IO", "", "eprintln", kCanonParams_IO + 5, 1, "", 1, 1, false, -1, -1, -1},
  {"IO", "", "stdin_is_terminal", nullptr, 0, "Bool", 0, 0, false, -1, -1, -1},
  {"IO", "", "stdout_is_terminal", nullptr, 0, "Bool", 0, 0, false, -1, -1, -1},
  {"IO", "", "stderr_is_terminal", nullptr, 0, "Bool", 0, 0, false, -1, -1, -1},
  {"IO", "", "capture", kCanonParams_IO + 6, 1, "String", 1, 1, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_Math[] = {
  // 0: Math.abs
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 1: Math.min
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
  // 2: Math.max
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
  // 3: Math.pow
  {"base", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"exp", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 5: Math.sign
  {"x", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 6: Math.clamp
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"lo", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"hi", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 9: Math.wrap
  {"x", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 11: Math.log
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 12: Math.exp
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 13: Math.sqrt
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 14: Math.sin
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 15: Math.cos
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 16: Math.tan
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 17: Math.asin
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 18: Math.acos
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 19: Math.atan
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 20: Math.atan2
  {"y", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 22: Math.floor
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 23: Math.ceil
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 24: Math.round
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Math[] = {
  {"Math", "", "abs", kCanonParams_Math + 0, 1, "", 1, 1, false, -1, -1, -1},
  {"Math", "", "min", kCanonParams_Math + 1, 1, "", 0, 0, true, -1, -1, 0},
  {"Math", "", "max", kCanonParams_Math + 2, 1, "", 0, 0, true, -1, -1, 0},
  {"Math", "", "pow", kCanonParams_Math + 3, 2, "Long", 2, 2, false, -1, -1, -1},
  {"Math", "", "sign", kCanonParams_Math + 5, 1, "Long", 1, 1, false, -1, -1, -1},
  {"Math", "", "clamp", kCanonParams_Math + 6, 3, "", 3, 3, false, -1, -1, -1},
  {"Math", "", "wrap", kCanonParams_Math + 9, 2, "Long", 2, 2, false, -1, -1, -1},
  {"Math", "", "log", kCanonParams_Math + 11, 1, "Float", 1, 1, false, -1, -1, -1},
  {"Math", "", "exp", kCanonParams_Math + 12, 1, "Float", 1, 1, false, -1, -1, -1},
  {"Math", "", "sqrt", kCanonParams_Math + 13, 1, "Float", 1, 1, false, -1, -1, -1},
  {"Math", "", "sin", kCanonParams_Math + 14, 1, "Float", 1, 1, false, -1, -1, -1},
  {"Math", "", "cos", kCanonParams_Math + 15, 1, "Float", 1, 1, false, -1, -1, -1},
  {"Math", "", "tan", kCanonParams_Math + 16, 1, "Float", 1, 1, false, -1, -1, -1},
  {"Math", "", "asin", kCanonParams_Math + 17, 1, "Float", 1, 1, false, -1, -1, -1},
  {"Math", "", "acos", kCanonParams_Math + 18, 1, "Float", 1, 1, false, -1, -1, -1},
  {"Math", "", "atan", kCanonParams_Math + 19, 1, "Float", 1, 1, false, -1, -1, -1},
  {"Math", "", "atan2", kCanonParams_Math + 20, 2, "Float", 2, 2, false, -1, -1, -1},
  {"Math", "", "floor", kCanonParams_Math + 22, 1, "Long", 1, 1, false, -1, -1, -1},
  {"Math", "", "ceil", kCanonParams_Math + 23, 1, "Long", 1, 1, false, -1, -1, -1},
  {"Math", "", "round", kCanonParams_Math + 24, 1, "Long", 1, 1, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_FS[] = {
  // 0: FS.join
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
  // 1: FS.basename
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 2: FS.dirname
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 3: FS.extension
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 4: FS.stem
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 5: FS.exists
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 6: FS.is_file
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 7: FS.is_dir
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 8: FS.read
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 9: FS.write
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"content", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 11: FS.size
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 12: FS.list_dir
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 13: FS.mkdir
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 14: FS.remove
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"recursive", true, false, false, false, false, "", CanonDefault::Bool, 0, {}},
  // 16: FS.stat
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 17: FS.chmod
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"mode", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 19: FS.chown
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"owner", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"group", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  // 22: FS.rename
  {"src", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"dst", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 24: FS.copy
  {"src", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"dst", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"recursive", true, false, false, false, false, "", CanonDefault::Bool, 0, {}},
  // 27: FS.normpath
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 28: FS.is_abs
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 29: FS.abspath
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 30: FS.realpath
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 31: FS.is_symlink
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 32: FS.symlink
  {"target", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"link", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 34: FS.readlink
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 35: FS.walk
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 36: FS.glob
  {"pattern", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 37: FS.watch
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"recursive", true, false, false, false, false, "", CanonDefault::Bool, 1, {}},
  {"match", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  // 40: FS.mkdtemp
  {"prefix", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_FS[] = {
  {"FS", "", "join", kCanonParams_FS + 0, 1, "", 0, 0, true, -1, -1, 0},
  {"FS", "", "basename", kCanonParams_FS + 1, 1, "String", 1, 1, false, -1, -1, -1},
  {"FS", "", "dirname", kCanonParams_FS + 2, 1, "String", 1, 1, false, -1, -1, -1},
  {"FS", "", "extension", kCanonParams_FS + 3, 1, "String", 1, 1, false, -1, -1, -1},
  {"FS", "", "stem", kCanonParams_FS + 4, 1, "String", 1, 1, false, -1, -1, -1},
  {"FS", "", "exists", kCanonParams_FS + 5, 1, "Bool", 1, 1, false, -1, -1, -1},
  {"FS", "", "is_file", kCanonParams_FS + 6, 1, "Bool", 1, 1, false, -1, -1, -1},
  {"FS", "", "is_dir", kCanonParams_FS + 7, 1, "Bool", 1, 1, false, -1, -1, -1},
  {"FS", "", "read", kCanonParams_FS + 8, 1, "String", 1, 1, false, -1, -1, -1},
  {"FS", "", "write", kCanonParams_FS + 9, 2, "", 2, 2, false, -1, -1, -1},
  {"FS", "", "size", kCanonParams_FS + 11, 1, "Long", 1, 1, false, -1, -1, -1},
  {"FS", "", "list_dir", kCanonParams_FS + 12, 1, "Array", 1, 1, false, -1, -1, -1},
  {"FS", "", "mkdir", kCanonParams_FS + 13, 1, "", 1, 1, false, -1, -1, -1},
  {"FS", "", "remove", kCanonParams_FS + 14, 2, "", 1, 2, false, -1, -1, -1},
  {"FS", "", "stat", kCanonParams_FS + 16, 1, "Object", 1, 1, false, -1, -1, -1},
  {"FS", "", "chmod", kCanonParams_FS + 17, 2, "", 2, 2, false, -1, -1, -1},
  {"FS", "", "chown", kCanonParams_FS + 19, 3, "", 1, 3, false, -1, -1, -1},
  {"FS", "", "rename", kCanonParams_FS + 22, 2, "", 2, 2, false, -1, -1, -1},
  {"FS", "", "copy", kCanonParams_FS + 24, 3, "", 2, 3, false, -1, -1, -1},
  {"FS", "", "normpath", kCanonParams_FS + 27, 1, "String", 1, 1, false, -1, -1, -1},
  {"FS", "", "is_abs", kCanonParams_FS + 28, 1, "Bool", 1, 1, false, -1, -1, -1},
  {"FS", "", "abspath", kCanonParams_FS + 29, 1, "String", 1, 1, false, -1, -1, -1},
  {"FS", "", "realpath", kCanonParams_FS + 30, 1, "String", 1, 1, false, -1, -1, -1},
  {"FS", "", "is_symlink", kCanonParams_FS + 31, 1, "Bool", 1, 1, false, -1, -1, -1},
  {"FS", "", "symlink", kCanonParams_FS + 32, 2, "", 2, 2, false, -1, -1, -1},
  {"FS", "", "readlink", kCanonParams_FS + 34, 1, "String", 1, 1, false, -1, -1, -1},
  {"FS", "", "walk", kCanonParams_FS + 35, 1, "Array", 1, 1, false, -1, -1, -1},
  {"FS", "", "glob", kCanonParams_FS + 36, 1, "Array", 1, 1, false, -1, -1, -1},
  {"FS", "", "watch", kCanonParams_FS + 37, 3, "Object", 1, 3, false, -1, -1, -1},
  {"FS", "", "temp_dir", nullptr, 0, "String", 0, 0, false, -1, -1, -1},
  {"FS", "", "mkdtemp", kCanonParams_FS + 40, 1, "String", 1, 1, false, -1, -1, -1},
  {"FS", "", "sep", nullptr, 0, "String", 0, 0, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_File[] = {
  // 0: File.open
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"mode", true, false, false, false, false, "", CanonDefault::Str, 0, "r"},
  // 2: File.with
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"mode", true, false, false, false, false, "", CanonDefault::Str, 0, "r"},
  {"fn", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_File[] = {
  {"File", "", "open", kCanonParams_File + 0, 2, "Object", 1, 2, false, -1, -1, -1},
  {"File", "", "with", kCanonParams_File + 2, 3, "", 2, 3, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_Random[] = {
  // 0: Random.seed
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 1: Random.int
  {"lo", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"hi", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 3: Random.uniform
  {"lo", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"hi", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 5: Random.gauss
  {"mu", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"sigma", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 7: Random.shuffle
  {"a", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  // 8: Random.weighted_choice
  {"pop", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"weights", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  // 10: Random.choice
  {"pop", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Random[] = {
  {"Random", "", "seed", kCanonParams_Random + 0, 1, "", 1, 1, false, -1, -1, -1},
  {"Random", "", "int", kCanonParams_Random + 1, 2, "Long", 2, 2, false, -1, -1, -1},
  {"Random", "", "uniform", kCanonParams_Random + 3, 2, "Float", 2, 2, false, -1, -1, -1},
  {"Random", "", "gauss", kCanonParams_Random + 5, 2, "Float", 2, 2, false, -1, -1, -1},
  {"Random", "", "shuffle", kCanonParams_Random + 7, 1, "", 1, 1, false, -1, -1, -1},
  {"Random", "", "weighted_choice", kCanonParams_Random + 8, 2, "", 2, 2, false, -1, -1, -1},
  {"Random", "", "choice", kCanonParams_Random + 10, 1, "", 1, 1, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_Sys[] = {
  // 0: Sys.exit
  {"code", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 1: Sys.env
  {"name", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"fallback", true, false, false, false, false, "", CanonDefault::Str, 0, ""},
  // 3: Sys.chdir
  {"path", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 4: Sys.set_env
  {"name", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"value", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 6: Sys.data_dir
  {"app", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Sys[] = {
  {"Sys", "", "exit", kCanonParams_Sys + 0, 1, "", 1, 1, false, -1, -1, -1},
  {"Sys", "", "env", kCanonParams_Sys + 1, 2, "", 1, 2, false, -1, -1, -1},
  {"Sys", "", "getcwd", nullptr, 0, "String", 0, 0, false, -1, -1, -1},
  {"Sys", "", "chdir", kCanonParams_Sys + 3, 1, "", 1, 1, false, -1, -1, -1},
  {"Sys", "", "set_env", kCanonParams_Sys + 4, 2, "", 2, 2, false, -1, -1, -1},
  {"Sys", "", "data_dir", kCanonParams_Sys + 6, 1, "String", 1, 1, false, -1, -1, -1},
  {"Sys", "", "time", nullptr, 0, "Float", 0, 0, false, -1, -1, -1},
};

inline constexpr CanonSig kCanonSigs_GC[] = {
  {"GC", "", "stat", nullptr, 0, "Object", 0, 0, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_Regex_native[] = {
  // 0: _Regex.check
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 1: _Regex.test
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 3: _Regex.find
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 5: _Regex.match
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 7: _Regex.find_from
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"pos", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 10: _Regex.find_all
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 12: _Regex.find_all_str
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 14: _Regex.find_all_index
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 16: _Regex.count
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 18: _Regex.replace_all
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"repl", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 21: _Regex.replace_first
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"repl", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 24: _Regex.split
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Regex_native[] = {
  {"_Regex", "", "check", kCanonParams_Regex_native + 0, 1, "", 1, 1, false, -1, -1, -1},
  {"_Regex", "", "test", kCanonParams_Regex_native + 1, 2, "Bool", 2, 2, false, -1, -1, -1},
  {"_Regex", "", "find", kCanonParams_Regex_native + 3, 2, "", 2, 2, false, -1, -1, -1},
  {"_Regex", "", "match", kCanonParams_Regex_native + 5, 2, "", 2, 2, false, -1, -1, -1},
  {"_Regex", "", "find_from", kCanonParams_Regex_native + 7, 3, "", 3, 3, false, -1, -1, -1},
  {"_Regex", "", "find_all", kCanonParams_Regex_native + 10, 2, "", 2, 2, false, -1, -1, -1},
  {"_Regex", "", "find_all_str", kCanonParams_Regex_native + 12, 2, "", 2, 2, false, -1, -1, -1},
  {"_Regex", "", "find_all_index", kCanonParams_Regex_native + 14, 2, "", 2, 2, false, -1, -1, -1},
  {"_Regex", "", "count", kCanonParams_Regex_native + 16, 2, "Long", 2, 2, false, -1, -1, -1},
  {"_Regex", "", "replace_all", kCanonParams_Regex_native + 18, 3, "String", 3, 3, false, -1, -1, -1},
  {"_Regex", "", "replace_first", kCanonParams_Regex_native + 21, 3, "String", 3, 3, false, -1, -1, -1},
  {"_Regex", "", "split", kCanonParams_Regex_native + 24, 2, "", 2, 2, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_Net[] = {
  // 0: Net.connect
  {"host", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"port", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"timeout", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  // 3: Net.listen
  {"port", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"host", true, false, false, false, false, "String", CanonDefault::Str, 0, "0.0.0.0"},
  {"backlog", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  // 6: Net.udp
  {"port", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  {"host", true, false, false, false, false, "String", CanonDefault::Str, 0, "0.0.0.0"},
  // 8: Net.resolve
  {"host", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Net[] = {
  {"Net", "", "connect", kCanonParams_Net + 0, 3, "Object", 2, 3, false, -1, -1, -1},
  {"Net", "", "listen", kCanonParams_Net + 3, 3, "Object", 1, 3, false, -1, -1, -1},
  {"Net", "", "udp", kCanonParams_Net + 6, 2, "Object", 0, 2, false, -1, -1, -1},
  {"Net", "", "resolve", kCanonParams_Net + 8, 1, "Array", 1, 1, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_Proc[] = {
  // 0: Proc.run
  {"cmd", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"cwd", true, false, false, false, false, "String?", CanonDefault::Nil, 0, {}},
  {"env", true, false, false, false, false, "Object?", CanonDefault::Nil, 0, {}},
  {"stdin", true, false, false, false, false, "String", CanonDefault::Str, 0, ""},
  {"check", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  {"timeout", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  {"share", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"inherit_env", true, false, false, false, false, "Bool", CanonDefault::Bool, 1, {}},
  // 8: Proc.all
  {"commands", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"limit", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  {"timeout", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  {"fail_fast", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  {"retries", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  {"share", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"inherit_env", true, false, false, false, false, "Bool", CanonDefault::Bool, 1, {}},
  // 15: Proc.race
  {"commands", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"share", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"inherit_env", true, false, false, false, false, "Bool", CanonDefault::Bool, 1, {}},
  // 18: Proc.spawn
  {"cmd", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"cwd", true, false, false, false, false, "String?", CanonDefault::Nil, 0, {}},
  {"env", true, false, false, false, false, "Object?", CanonDefault::Nil, 0, {}},
  {"stdin", true, false, false, false, false, "String", CanonDefault::Str, 0, ""},
  {"share", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"inherit_env", true, false, false, false, false, "Bool", CanonDefault::Bool, 1, {}},
};

inline constexpr CanonSig kCanonSigs_Proc[] = {
  {"Proc", "", "run", kCanonParams_Proc + 0, 8, "Object", 1, 8, false, -1, -1, -1},
  {"Proc", "", "all", kCanonParams_Proc + 8, 7, "Array", 1, 7, false, -1, -1, -1},
  {"Proc", "", "race", kCanonParams_Proc + 15, 3, "Object", 1, 3, false, -1, -1, -1},
  {"Proc", "", "spawn", kCanonParams_Proc + 18, 6, "Object", 1, 6, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_Http[] = {
  // 0: Http.get
  {"url", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"headers", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"timeout", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"follow_redirects", true, false, false, false, false, "", CanonDefault::Bool, 1, {}},
  {"into", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"params", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  // 6: Http.delete
  {"url", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"headers", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"timeout", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"follow_redirects", true, false, false, false, false, "", CanonDefault::Bool, 1, {}},
  {"into", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"params", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  // 12: Http.head
  {"url", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"headers", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"timeout", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"follow_redirects", true, false, false, false, false, "", CanonDefault::Bool, 1, {}},
  {"into", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"params", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  // 18: Http.post
  {"url", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"body", true, false, false, false, false, "", CanonDefault::Str, 0, ""},
  {"content_type", true, false, false, false, false, "", CanonDefault::Str, 0, "text/plain"},
  {"headers", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"timeout", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"follow_redirects", true, false, false, false, false, "", CanonDefault::Bool, 1, {}},
  {"into", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"params", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"json", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"form", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"files", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  // 29: Http.put
  {"url", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"body", true, false, false, false, false, "", CanonDefault::Str, 0, ""},
  {"content_type", true, false, false, false, false, "", CanonDefault::Str, 0, "text/plain"},
  {"headers", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"timeout", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"follow_redirects", true, false, false, false, false, "", CanonDefault::Bool, 1, {}},
  {"into", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"params", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"json", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"form", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"files", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  // 40: Http.request
  {"method", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"url", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"body", true, false, false, false, false, "", CanonDefault::Str, 0, ""},
  {"content_type", true, false, false, false, false, "", CanonDefault::Str, 0, "text/plain"},
  {"headers", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"timeout", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"follow_redirects", true, false, false, false, false, "", CanonDefault::Bool, 1, {}},
  {"into", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"params", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"json", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"form", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"files", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  // 52: Http.sse
  {"url", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"on_event", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  {"headers", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"timeout", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"follow_redirects", true, false, false, false, false, "", CanonDefault::Bool, 1, {}},
  // 57: Http.client
  {"base_url", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"headers", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"timeout", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"follow_redirects", true, false, false, false, false, "", CanonDefault::Bool, 1, {}},
  // 61: Http.ws
  {"url", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Http[] = {
  {"Http", "", "get", kCanonParams_Http + 0, 6, "Object", 1, 6, false, -1, -1, -1},
  {"Http", "", "delete", kCanonParams_Http + 6, 6, "Object", 1, 6, false, -1, -1, -1},
  {"Http", "", "head", kCanonParams_Http + 12, 6, "Object", 1, 6, false, -1, -1, -1},
  {"Http", "", "post", kCanonParams_Http + 18, 11, "Object", 1, 11, false, -1, -1, -1},
  {"Http", "", "put", kCanonParams_Http + 29, 11, "Object", 1, 11, false, -1, -1, -1},
  {"Http", "", "request", kCanonParams_Http + 40, 12, "Object", 2, 12, false, -1, -1, -1},
  {"Http", "", "sse", kCanonParams_Http + 52, 5, "Object", 2, 5, false, -1, -1, -1},
  {"Http", "", "client", kCanonParams_Http + 57, 4, "Object", 1, 4, false, -1, -1, -1},
  {"Http", "", "server", nullptr, 0, "Object", 0, 0, false, -1, -1, -1},
  {"Http", "", "ws", kCanonParams_Http + 61, 1, "Object", 1, 1, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_Isolate[] = {
  // 0: Isolate.spawn
  {"fn", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Isolate[] = {
  {"Isolate", "", "spawn", kCanonParams_Isolate + 0, 2, "Object", 1, 1, true, -1, -1, 1},
};

inline constexpr CanonParam kCanonParams_Channel[] = {
  // 0: Channel.new
  {"cap", true, false, false, false, false, "", CanonDefault::Long, 1, {}},
  // 1: Channel.fan_in
  {"a", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"fn", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Channel[] = {
  {"Channel", "", "new", kCanonParams_Channel + 0, 1, "Tuple", 0, 1, false, -1, -1, -1},
  {"Channel", "", "fan_in", kCanonParams_Channel + 1, 2, "", 1, 2, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_Signal[] = {
  // 0: Signal.notify
  {"tx", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Signal[] = {
  {"Signal", "", "notify", kCanonParams_Signal + 0, 1, "Nil", 1, 1, false, -1, -1, -1},
  {"Signal", "", "reset", nullptr, 0, "Nil", 0, 0, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_SharedBuffer[] = {
  // 0: SharedBuffer.new
  {"count", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"type", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 2: SharedBuffer.file
  {"path", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"count", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"type", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 5: SharedBuffer.shared
  {"count", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"type", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 7: SharedBuffer.receive
  {"name", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"type", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_SharedBuffer[] = {
  {"SharedBuffer", "", "new", kCanonParams_SharedBuffer + 0, 2, "Object", 2, 2, false, -1, -1, -1},
  {"SharedBuffer", "", "file", kCanonParams_SharedBuffer + 2, 3, "Object", 3, 3, false, -1, -1, -1},
  {"SharedBuffer", "", "shared", kCanonParams_SharedBuffer + 5, 2, "Object", 2, 2, false, -1, -1, -1},
  {"SharedBuffer", "", "receive", kCanonParams_SharedBuffer + 7, 2, "Object", 2, 2, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_Shared[] = {
  // 0: Shared.new
  {"value", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Shared[] = {
  {"Shared", "", "new", kCanonParams_Shared + 0, 1, "", 1, 1, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_Parallel[] = {
  // 0: Parallel.map
  {"items", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"fn", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"limit", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"on_progress", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  // 4: Parallel.each
  {"items", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"fn", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"limit", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"on_progress", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  // 8: Parallel.map_settled
  {"items", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"fn", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"limit", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"on_progress", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  // 12: Parallel.race
  {"items", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"fn", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"limit", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"on_progress", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Parallel[] = {
  {"Parallel", "", "map", kCanonParams_Parallel + 0, 4, "Array", 2, 4, false, -1, -1, -1},
  {"Parallel", "", "each", kCanonParams_Parallel + 4, 4, "Nil", 2, 4, false, -1, -1, -1},
  {"Parallel", "", "map_settled", kCanonParams_Parallel + 8, 4, "Array", 2, 4, false, -1, -1, -1},
  {"Parallel", "", "race", kCanonParams_Parallel + 12, 4, "Any", 2, 4, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_JSON[] = {
  // 0: JSON.stringify
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"indent", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  {"sort_keys", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  {"lines", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  // 4: JSON.parse
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"lines", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  {"number_mode", true, false, false, false, false, "String", CanonDefault::Str, 0, "auto"},
  {"jsonc", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
};

inline constexpr CanonSig kCanonSigs_JSON[] = {
  {"JSON", "", "stringify", kCanonParams_JSON + 0, 4, "String", 1, 4, false, -1, -1, -1},
  {"JSON", "", "parse", kCanonParams_JSON + 4, 4, "", 1, 4, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_Encoding[] = {
  // 0: Encoding.html.escape
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 1: Encoding.html.unescape
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 2: Encoding.base64.encode
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 3: Encoding.base64.decode
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 4: Encoding.hex.encode
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 5: Encoding.hex.decode
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 6: Encoding.url.encode
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 7: Encoding.url.decode
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Encoding[] = {
  {"Encoding", "html", "escape", kCanonParams_Encoding + 0, 1, "String", 1, 1, false, -1, -1, -1},
  {"Encoding", "html", "unescape", kCanonParams_Encoding + 1, 1, "String", 1, 1, false, -1, -1, -1},
  {"Encoding", "base64", "encode", kCanonParams_Encoding + 2, 1, "String", 1, 1, false, -1, -1, -1},
  {"Encoding", "base64", "decode", kCanonParams_Encoding + 3, 1, "String", 1, 1, false, -1, -1, -1},
  {"Encoding", "hex", "encode", kCanonParams_Encoding + 4, 1, "String", 1, 1, false, -1, -1, -1},
  {"Encoding", "hex", "decode", kCanonParams_Encoding + 5, 1, "String", 1, 1, false, -1, -1, -1},
  {"Encoding", "url", "encode", kCanonParams_Encoding + 6, 1, "String", 1, 1, false, -1, -1, -1},
  {"Encoding", "url", "decode", kCanonParams_Encoding + 7, 1, "String", 1, 1, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_Compress[] = {
  // 0: Compress.gzip
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 1: Compress.gunzip
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 2: Compress.deflate
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"level", true, false, false, false, false, "Long", CanonDefault::Long, -1, {}},
};

inline constexpr CanonSig kCanonSigs_Compress[] = {
  {"Compress", "", "gzip", kCanonParams_Compress + 0, 1, "String", 1, 1, false, -1, -1, -1},
  {"Compress", "", "gunzip", kCanonParams_Compress + 1, 1, "String", 1, 1, false, -1, -1, -1},
  {"Compress", "", "deflate", kCanonParams_Compress + 2, 2, "String", 1, 2, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_Hash[] = {
  // 0: Hash.sha256
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 1: Hash.sha1
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 2: Hash.sha512
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 3: Hash.md5
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 4: Hash.hmac_sha256
  {"key", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 6: Hash.hmac_sha1
  {"key", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 8: Hash.hmac_sha512
  {"key", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Hash[] = {
  {"Hash", "", "sha256", kCanonParams_Hash + 0, 1, "String", 1, 1, false, -1, -1, -1},
  {"Hash", "", "sha1", kCanonParams_Hash + 1, 1, "String", 1, 1, false, -1, -1, -1},
  {"Hash", "", "sha512", kCanonParams_Hash + 2, 1, "String", 1, 1, false, -1, -1, -1},
  {"Hash", "", "md5", kCanonParams_Hash + 3, 1, "String", 1, 1, false, -1, -1, -1},
  {"Hash", "", "hmac_sha256", kCanonParams_Hash + 4, 2, "String", 2, 2, false, -1, -1, -1},
  {"Hash", "", "hmac_sha1", kCanonParams_Hash + 6, 2, "String", 2, 2, false, -1, -1, -1},
  {"Hash", "", "hmac_sha512", kCanonParams_Hash + 8, 2, "String", 2, 2, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_CSV[] = {
  // 0: CSV.parse
  {"text", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"delimiter", true, false, false, false, false, "String", CanonDefault::Str, 0, ","},
  {"header", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  {"types", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  // 4: CSV.stringify
  {"rows", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"delimiter", true, false, false, false, false, "String", CanonDefault::Str, 0, ","},
};

inline constexpr CanonSig kCanonSigs_CSV[] = {
  {"CSV", "", "parse", kCanonParams_CSV + 0, 4, "Array", 1, 4, false, -1, -1, -1},
  {"CSV", "", "stringify", kCanonParams_CSV + 4, 2, "String", 1, 2, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_SQLite[] = {
  // 0: SQLite.open
  {"path", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_SQLite[] = {
  {"SQLite", "", "open", kCanonParams_SQLite + 0, 1, "Object", 1, 1, false, -1, -1, -1},
  {"SQLite", "", "version", nullptr, 0, "String", 0, 0, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_TOML[] = {
  // 0: TOML.parse
  {"text", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 1: TOML.stringify
  {"v", false, false, false, false, false, "Object", CanonDefault::None, 0, {}},
  {"sort_keys", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
};

inline constexpr CanonSig kCanonSigs_TOML[] = {
  {"TOML", "", "parse", kCanonParams_TOML + 0, 1, "Object", 1, 1, false, -1, -1, -1},
  {"TOML", "", "stringify", kCanonParams_TOML + 1, 2, "String", 1, 2, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_Env[] = {
  // 0: Env.parse
  {"text", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 1: Env.load
  {"path", true, false, false, false, false, "String", CanonDefault::Str, 0, ".env"},
  {"override", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Env[] = {
  {"Env", "", "parse", kCanonParams_Env + 0, 1, "Object", 1, 1, false, -1, -1, -1},
  {"Env", "", "load", kCanonParams_Env + 1, 2, "Object", 0, 2, false, -1, -1, -1},
};

inline constexpr CanonSig kCanonSigs_UUID[] = {
  {"UUID", "", "v4", nullptr, 0, "String", 0, 0, false, -1, -1, -1},
  {"UUID", "", "v7", nullptr, 0, "String", 0, 0, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_String[] = {
  // 0: String.from_code_point
  {"cp", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 1: String.from_bytes
  {"bytes", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  // 2: String.from_code_points
  {"cps", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_String[] = {
  {"String", "", "from_code_point", kCanonParams_String + 0, 1, "String", 1, 1, false, -1, -1, -1},
  {"String", "", "from_bytes", kCanonParams_String + 1, 1, "String", 1, 1, false, -1, -1, -1},
  {"String", "", "from_code_points", kCanonParams_String + 2, 1, "String", 1, 1, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_Tensor[] = {
  // 0: Tensor.zeros
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
  // 1: Tensor.ones
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
  // 2: Tensor.randn
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
  // 3: Tensor.eval
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
  // 4: Tensor.from_csv
  {"path", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 5: Tensor.from
  {"a", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  // 6: Tensor.concat
  {"parts", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  // 7: Tensor.no_grad
  {"fn", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Tensor[] = {
  {"Tensor", "", "zeros", kCanonParams_Tensor + 0, 1, "Tensor", 0, 0, true, -1, -1, 0},
  {"Tensor", "", "ones", kCanonParams_Tensor + 1, 1, "Tensor", 0, 0, true, -1, -1, 0},
  {"Tensor", "", "randn", kCanonParams_Tensor + 2, 1, "Tensor", 0, 0, true, -1, -1, 0},
  {"Tensor", "", "eval", kCanonParams_Tensor + 3, 1, "", 0, 0, true, -1, -1, 0},
  {"Tensor", "", "from_csv", kCanonParams_Tensor + 4, 1, "Tensor", 1, 1, false, -1, -1, -1},
  {"Tensor", "", "from", kCanonParams_Tensor + 5, 1, "Tensor", 1, 1, false, -1, -1, -1},
  {"Tensor", "", "concat", kCanonParams_Tensor + 6, 1, "Tensor", 1, 1, false, -1, -1, -1},
  {"Tensor", "", "no_grad", kCanonParams_Tensor + 7, 1, "Any", 1, 1, false, -1, -1, -1},
  {"Tensor", "", "use_cpu", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"Tensor", "", "use_gpu", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"Tensor", "", "use_auto", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"Tensor", "", "gpu_available", nullptr, 0, "Bool", 0, 0, false, -1, -1, -1},
  {"Tensor", "", "device", nullptr, 0, "String", 0, 0, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_Time_native[] = {
  // 0: _Time.sleep
  {"secs", false, false, false, false, false, "Float", CanonDefault::None, 0, {}},
  // 1: _Time.from_iso_nanos
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 2: _Time.parse_nanos
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"fmt", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 4: _Time.iso_nanos
  {"nanos", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"utc", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
  // 6: _Time.format_nanos
  {"nanos", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"fmt", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"utc", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
  // 9: _Time.parts_nanos
  {"nanos", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"utc", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
  // 11: _Time.from_parts_nanos
  {"p", false, false, false, false, false, "Object", CanonDefault::None, 0, {}},
  {"utc", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
  // 13: _Time.weekday_nanos
  {"nanos", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"utc", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
  // 15: _Time.add_nanos
  {"nanos", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"years", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"months", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"days", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"hours", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"minutes", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"seconds", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"utc", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
  // 23: _Time.start_of_nanos
  {"nanos", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"unit", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"utc", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Time_native[] = {
  {"_Time", "", "now_nanos", nullptr, 0, "Long", 0, 0, false, -1, -1, -1},
  {"_Time", "", "monotonic", nullptr, 0, "Float", 0, 0, false, -1, -1, -1},
  {"_Time", "", "sleep", kCanonParams_Time_native + 0, 1, "", 1, 1, false, -1, -1, -1},
  {"_Time", "", "from_iso_nanos", kCanonParams_Time_native + 1, 1, "Long", 1, 1, false, -1, -1, -1},
  {"_Time", "", "parse_nanos", kCanonParams_Time_native + 2, 2, "Long", 2, 2, false, -1, -1, -1},
  {"_Time", "", "iso_nanos", kCanonParams_Time_native + 4, 2, "String", 2, 2, false, -1, -1, -1},
  {"_Time", "", "format_nanos", kCanonParams_Time_native + 6, 3, "String", 3, 3, false, -1, -1, -1},
  {"_Time", "", "parts_nanos", kCanonParams_Time_native + 9, 2, "Object", 2, 2, false, -1, -1, -1},
  {"_Time", "", "from_parts_nanos", kCanonParams_Time_native + 11, 2, "Long", 2, 2, false, -1, -1, -1},
  {"_Time", "", "weekday_nanos", kCanonParams_Time_native + 13, 2, "Long", 2, 2, false, -1, -1, -1},
  {"_Time", "", "add_nanos", kCanonParams_Time_native + 15, 8, "Long", 8, 8, false, -1, -1, -1},
  {"_Time", "", "start_of_nanos", kCanonParams_Time_native + 23, 3, "Long", 3, 3, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_Term_native[] = {
  // 0: _Term.width
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 1: _Term.read_key
  {"timeout", false, false, false, false, false, "Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Term_native[] = {
  {"_Term", "", "cols", nullptr, 0, "Long", 0, 0, false, -1, -1, -1},
  {"_Term", "", "rows", nullptr, 0, "Long", 0, 0, false, -1, -1, -1},
  {"_Term", "", "raw_on", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"_Term", "", "raw_off", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"_Term", "", "flush", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"_Term", "", "color_level", nullptr, 0, "Long", 0, 0, false, -1, -1, -1},
  {"_Term", "", "width", kCanonParams_Term_native + 0, 1, "Long", 1, 1, false, -1, -1, -1},
  {"_Term", "", "resized", nullptr, 0, "Bool", 0, 0, false, -1, -1, -1},
  {"_Term", "", "read_key", kCanonParams_Term_native + 1, 1, "String", 1, 1, false, -1, -1, -1},
  {"_Term", "", "attach_tty", nullptr, 0, "Bool", 0, 0, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_Canvas_native[] = {
  // 0: _Canvas.init
  {"w", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"h", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 2: _Canvas.ttf_load
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 3: _Canvas.ttf_free
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 4: _Canvas.ttf_glyph
  {"font", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"codepoint", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"x", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"size", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 10: _Canvas.ttf_glyph_screen
  {"font", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"codepoint", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"x", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"size", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 16: _Canvas.ttf_advance
  {"font", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"codepoint", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"size", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 19: _Canvas.ttf_ascent
  {"font", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"size", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 21: _Canvas.get_screen_pixel
  {"x", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  // 23: _Canvas.clear
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 24: _Canvas.set_pixel
  {"x", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 27: _Canvas.get_pixel
  {"x", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  // 29: _Canvas.rect
  {"x", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"w", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"h", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"fill", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 35: _Canvas.line
  {"x1", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y1", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"x2", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y2", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 40: _Canvas.ellipse
  {"cx", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"cy", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rx", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"ry", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"fill", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 46: _Canvas.triangle
  {"x1", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y1", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"x2", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y2", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"x3", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y3", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"fill", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 54: _Canvas.polygon
  {"points", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"fill", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 57: _Canvas.font_load
  {"rows", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  // 58: _Canvas.glyph
  {"font", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"index", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"x", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"scale", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 64: _Canvas.sprite_load
  {"pixels", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"w", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"h", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 67: _Canvas.sprite_from_png
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 68: _Canvas.sprite_to_png
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 69: _Canvas.sprite_width
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 70: _Canvas.sprite_height
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 71: _Canvas.sprite_blank
  {"w", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"h", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 74: _Canvas.sprite_free
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 75: _Canvas.target
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 76: _Canvas.blit
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"dx", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"dy", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"sx", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"sy", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"sw", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"sh", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"flags", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 84: _Canvas.blit_scaled
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"dx", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"dy", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"dw", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"dh", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"sx", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"sy", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"sw", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"sh", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"flags", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"alpha", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 95: _Canvas.key
  {"name", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 96: _Canvas.title
  {"name", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 97: _Canvas.tone
  {"start_freq", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"end_freq", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"attack", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"decay", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"sustain", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"release", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"vol", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"peak", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"channel", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"duty", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 107: _Canvas.music_play
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"loop", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"vol", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"start", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  // 111: _Canvas.music_volume
  {"vol", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 112: _Canvas.music_seek
  {"seconds", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  // 113: _Canvas.sound_load
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 114: _Canvas.sound_play
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"vol", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 116: _Canvas.sound_stop
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 117: _Canvas.sound_playing
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 118: _Canvas.sound_free
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 119: _Canvas.clipboard_set
  {"text", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 120: _Canvas.set_resizable
  {"enabled", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
  // 121: _Canvas.set_target_fps
  {"fps", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 122: _Canvas.pad_available
  {"index", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 123: _Canvas.pad_axis
  {"index", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"axis", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 125: _Canvas.pad_button
  {"index", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"button", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 127: _Canvas.pad_pressed
  {"index", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"button", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 129: _Canvas.pad_name
  {"index", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 130: _Canvas.pad_rumble
  {"index", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"left", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"right", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"sec", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  // 134: _Canvas.pad_mappings
  {"db", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Canvas_native[] = {
  {"_Canvas", "", "init", kCanonParams_Canvas_native + 0, 2, "", 2, 2, false, -1, -1, -1},
  {"_Canvas", "", "ttf_load", kCanonParams_Canvas_native + 2, 1, "Long", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "ttf_free", kCanonParams_Canvas_native + 3, 1, "", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "ttf_glyph", kCanonParams_Canvas_native + 4, 6, "Long", 6, 6, false, -1, -1, -1},
  {"_Canvas", "", "ttf_glyph_screen", kCanonParams_Canvas_native + 10, 6, "Long", 6, 6, false, -1, -1, -1},
  {"_Canvas", "", "ttf_advance", kCanonParams_Canvas_native + 16, 3, "Long", 3, 3, false, -1, -1, -1},
  {"_Canvas", "", "ttf_ascent", kCanonParams_Canvas_native + 19, 2, "Long", 2, 2, false, -1, -1, -1},
  {"_Canvas", "", "screen_width", nullptr, 0, "Long", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "screen_height", nullptr, 0, "Long", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "get_screen_pixel", kCanonParams_Canvas_native + 21, 2, "Long", 2, 2, false, -1, -1, -1},
  {"_Canvas", "", "screen_scale", nullptr, 0, "Float", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "clear", kCanonParams_Canvas_native + 23, 1, "", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "set_pixel", kCanonParams_Canvas_native + 24, 3, "", 3, 3, false, -1, -1, -1},
  {"_Canvas", "", "get_pixel", kCanonParams_Canvas_native + 27, 2, "Long", 2, 2, false, -1, -1, -1},
  {"_Canvas", "", "rect", kCanonParams_Canvas_native + 29, 6, "", 6, 6, false, -1, -1, -1},
  {"_Canvas", "", "line", kCanonParams_Canvas_native + 35, 5, "", 5, 5, false, -1, -1, -1},
  {"_Canvas", "", "ellipse", kCanonParams_Canvas_native + 40, 6, "", 6, 6, false, -1, -1, -1},
  {"_Canvas", "", "triangle", kCanonParams_Canvas_native + 46, 8, "", 8, 8, false, -1, -1, -1},
  {"_Canvas", "", "polygon", kCanonParams_Canvas_native + 54, 3, "", 3, 3, false, -1, -1, -1},
  {"_Canvas", "", "font_load", kCanonParams_Canvas_native + 57, 1, "Long", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "glyph", kCanonParams_Canvas_native + 58, 6, "", 6, 6, false, -1, -1, -1},
  {"_Canvas", "", "sprite_load", kCanonParams_Canvas_native + 64, 3, "Long", 3, 3, false, -1, -1, -1},
  {"_Canvas", "", "sprite_from_png", kCanonParams_Canvas_native + 67, 1, "Long", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "sprite_to_png", kCanonParams_Canvas_native + 68, 1, "String", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "sprite_width", kCanonParams_Canvas_native + 69, 1, "Long", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "sprite_height", kCanonParams_Canvas_native + 70, 1, "Long", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "sprite_blank", kCanonParams_Canvas_native + 71, 3, "Long", 3, 3, false, -1, -1, -1},
  {"_Canvas", "", "sprite_free", kCanonParams_Canvas_native + 74, 1, "", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "target", kCanonParams_Canvas_native + 75, 1, "Long", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "blit", kCanonParams_Canvas_native + 76, 8, "", 8, 8, false, -1, -1, -1},
  {"_Canvas", "", "blit_scaled", kCanonParams_Canvas_native + 84, 11, "", 11, 11, false, -1, -1, -1},
  {"_Canvas", "", "present", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "buttons", nullptr, 0, "Long", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "mouse_x", nullptr, 0, "Long", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "mouse_y", nullptr, 0, "Long", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "mouse_buttons", nullptr, 0, "Long", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "key", kCanonParams_Canvas_native + 95, 1, "Bool", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "key_pop", nullptr, 0, "String", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "char_pop", nullptr, 0, "String", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "closing", nullptr, 0, "Bool", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "windowed", nullptr, 0, "Bool", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "title", kCanonParams_Canvas_native + 96, 1, "", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "tone", kCanonParams_Canvas_native + 97, 10, "", 10, 10, false, -1, -1, -1},
  {"_Canvas", "", "music_play", kCanonParams_Canvas_native + 107, 4, "", 4, 4, false, -1, -1, -1},
  {"_Canvas", "", "music_stop", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "music_pause", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "music_resume", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "music_volume", kCanonParams_Canvas_native + 111, 1, "", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "music_seek", kCanonParams_Canvas_native + 112, 1, "", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "music_playing", nullptr, 0, "Bool", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "sound_load", kCanonParams_Canvas_native + 113, 1, "Long", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "sound_play", kCanonParams_Canvas_native + 114, 2, "", 2, 2, false, -1, -1, -1},
  {"_Canvas", "", "sound_stop", kCanonParams_Canvas_native + 116, 1, "", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "sound_playing", kCanonParams_Canvas_native + 117, 1, "Bool", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "sound_free", kCanonParams_Canvas_native + 118, 1, "", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "width", nullptr, 0, "Long", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "height", nullptr, 0, "Long", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "toggle_fullscreen", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "is_fullscreen", nullptr, 0, "Bool", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "show_cursor", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "hide_cursor", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "cursor_hidden", nullptr, 0, "Bool", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "clipboard_get", nullptr, 0, "String", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "clipboard_set", kCanonParams_Canvas_native + 119, 1, "", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "set_resizable", kCanonParams_Canvas_native + 120, 1, "", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "window_resized", nullptr, 0, "Bool", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "dt", nullptr, 0, "Float", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "set_target_fps", kCanonParams_Canvas_native + 121, 1, "", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "fps", nullptr, 0, "Long", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "mouse_wheel", nullptr, 0, "Float", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "pad_available", kCanonParams_Canvas_native + 122, 1, "Bool", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "pad_axis", kCanonParams_Canvas_native + 123, 2, "Float", 2, 2, false, -1, -1, -1},
  {"_Canvas", "", "pad_button", kCanonParams_Canvas_native + 125, 2, "Bool", 2, 2, false, -1, -1, -1},
  {"_Canvas", "", "pad_pressed", kCanonParams_Canvas_native + 127, 2, "Bool", 2, 2, false, -1, -1, -1},
  {"_Canvas", "", "pad_name", kCanonParams_Canvas_native + 129, 1, "String", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "pad_rumble", kCanonParams_Canvas_native + 130, 4, "", 4, 4, false, -1, -1, -1},
  {"_Canvas", "", "pad_mappings", kCanonParams_Canvas_native + 134, 1, "Long", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "quit", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "can_quit", nullptr, 0, "Bool", 0, 0, false, -1, -1, -1},
};

inline constexpr CanonParam kCanonParams_Bare[] = {
  // 0: inspect
  {"arg", false, false, false, false, true, "", CanonDefault::None, 0, {}},
  // 1: print
  {"arg", false, false, false, false, true, "", CanonDefault::None, 0, {}},
  // 2: println
  {"arg", true, false, false, false, true, "", CanonDefault::Str, 0, ""},
  // 3: type_of
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 4: to_long
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"base", true, true, false, false, false, "Long", CanonDefault::Long, 10, {}},
  // 6: to_float
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 7: to_string
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 8: hash
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 9: __eff_copy
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 10: __eff_abort
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 11: __eff_catch_abort
  {"fn", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 12: range
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
  {"step", true, true, false, false, false, "", CanonDefault::Long, 1, {}},
  // 14: iota
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
  // 15: repeat
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"value", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 17: grid
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Bare[] = {
  {"", "", "inspect", kCanonParams_Bare + 0, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "print", kCanonParams_Bare + 1, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "println", kCanonParams_Bare + 2, 1, "", 0, 1, false, -1, -1, -1},
  {"", "", "type_of", kCanonParams_Bare + 3, 1, "String", 1, 1, false, -1, -1, -1},
  {"", "", "to_long", kCanonParams_Bare + 4, 2, "Long", 1, 1, false, -1, 1, -1},
  {"", "", "to_float", kCanonParams_Bare + 6, 1, "Float", 1, 1, false, -1, -1, -1},
  {"", "", "to_string", kCanonParams_Bare + 7, 1, "String", 1, 1, false, -1, -1, -1},
  {"", "", "hash", kCanonParams_Bare + 8, 1, "Long", 1, 1, false, -1, -1, -1},
  {"", "", "__eff_copy", kCanonParams_Bare + 9, 1, "Object", 1, 1, false, -1, -1, -1},
  {"", "", "__eff_abort", kCanonParams_Bare + 10, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "__eff_catch_abort", kCanonParams_Bare + 11, 1, "Array", 1, 1, false, -1, -1, -1},
  {"", "", "range", kCanonParams_Bare + 12, 2, "", 0, 0, true, -1, 1, 0},
  {"", "", "iota", kCanonParams_Bare + 14, 1, "", 0, 0, true, -1, -1, 0},
  {"", "", "repeat", kCanonParams_Bare + 15, 2, "Array", 2, 2, false, -1, -1, -1},
  {"", "", "grid", kCanonParams_Bare + 17, 1, "", 0, 0, true, -1, -1, 0},
};

// The value-type built-in methods' parameters (the namespace tables carry
// their own — see kCanonParams_<Ns> above).
inline constexpr CanonParam kCanonParamPool[] = {
  // 0: get
  {"key", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"fallback", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 2: get_or_put
  {"key", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"init", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 4: has
  {"key", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 5: remove
  {"key", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 6: all
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 7: any
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 8: contains
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 9: extend
  {"other", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  // 10: filter
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 11: find
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 12: flat_map
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 13: for_each
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 14: get
  {"i", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"fallback", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 16: group_by
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 17: index_of
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 18: insert
  {"i", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 20: join
  {"sep", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 21: map
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 22: max_by
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 23: min_by
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 24: partition
  {"p", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 25: push
  {"arg", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 26: reduce
  {"init", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 28: remove_at
  {"i", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 29: slice
  {"start", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"end", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 31: sort
  {"reverse", true, true, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  // 32: sort_by
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  {"reverse", true, true, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  // 34: sorted
  {"reverse", true, true, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  // 35: sorted_by
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  {"reverse", true, true, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  // 37: contains
  {"sub", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 38: count
  {"sub", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 39: ends_with
  {"suffix", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 40: eq_ignore_case
  {"other", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 41: index_of
  {"sub", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"start", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  // 43: last_index_of
  {"sub", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 44: normalize
  {"form", true, false, false, false, false, "StringLike", CanonDefault::Str, 0, "NFC"},
  // 45: repeat
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 46: rsplit
  {"sep", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"limit", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  // 48: slice
  {"start", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"end", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 50: split
  {"sep", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"limit", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  // 52: split_iter
  {"sep", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 53: starts_with
  {"prefix", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 54: strip_prefix
  {"prefix", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 55: strip_suffix
  {"suffix", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 56: tr
  {"from", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"to", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 58: trim_end
  {"chars", true, false, false, false, false, "StringLike", CanonDefault::Str, 0, ""},
  // 59: trim_start
  {"chars", true, false, false, false, false, "StringLike", CanonDefault::Str, 0, ""},
  // 60: truncate
  {"max", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"ellipsis", true, false, false, false, false, "StringLike", CanonDefault::Str, 0, "..."},
  // 62: add
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 63: contains
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 64: diff
  {"other", false, false, false, false, false, "Set", CanonDefault::None, 0, {}},
  // 65: intersect
  {"other", false, false, false, false, false, "Set", CanonDefault::None, 0, {}},
  // 66: remove
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 67: subset
  {"other", false, false, false, false, false, "Set", CanonDefault::None, 0, {}},
  // 68: superset
  {"other", false, false, false, false, false, "Set", CanonDefault::None, 0, {}},
  // 69: sym_diff
  {"other", false, false, false, false, false, "Set", CanonDefault::None, 0, {}},
  // 70: union
  {"other", false, false, false, false, false, "Set", CanonDefault::None, 0, {}},
  // 71: contains
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 72: argmax
  {"axis", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 73: dot
  {"other", false, false, false, false, false, "Tensor", CanonDefault::None, 0, {}},
  // 74: linear_sigmoid
  {"x", false, false, false, false, false, "Tensor", CanonDefault::None, 0, {}},
  {"b", false, false, false, false, false, "Tensor", CanonDefault::None, 0, {}},
  // 76: max
  {"axis", true, false, false, false, false, "Long?", CanonDefault::Nil, 0, {}},
  // 77: mean
  {"axis", true, false, false, false, false, "Long?", CanonDefault::Nil, 0, {}},
  // 78: pow
  {"exp", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 79: reshape
  {"dims", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  // 80: slice
  {"start", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"end", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 82: sum
  {"axis", true, false, false, false, false, "Long?", CanonDefault::Nil, 0, {}},
  // 83: all
  {"p", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 84: any
  {"p", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 85: chain
  {"other", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 86: chunk_by
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 87: chunks
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 88: contains
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 89: filter
  {"p", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 90: find
  {"p", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 91: flat_map
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 92: for_each
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 93: group_by
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 94: join
  {"sep", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 95: map
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 96: max_by
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 97: min_by
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 98: nth
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 99: partition
  {"p", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 100: position
  {"p", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 101: reduce
  {"init", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 103: scan
  {"init", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 105: skip
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 106: skip_while
  {"p", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 107: step_by
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 108: take
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 109: take_while
  {"p", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 110: tap
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 111: windows
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 112: zip
  {"other", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonObjectSigs[] = {
  {"", "", "empty", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "get", kCanonParamPool + 0, 2, "", 2, 2, false, -1, -1, -1},
  {"", "", "get_or_put", kCanonParamPool + 2, 2, "", 2, 2, false, -1, -1, -1},
  {"", "", "has", kCanonParamPool + 4, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "iter", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "keys", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "presence", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "remove", kCanonParamPool + 5, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "size", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "values", nullptr, 0, "", 0, 0, false, -1, -1, -1},
};

inline constexpr CanonSig kCanonArraySigs[] = {
  {"", "", "all", kCanonParamPool + 6, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "any", kCanonParamPool + 7, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "contains", kCanonParamPool + 8, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "empty", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "enumerate", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "extend", kCanonParamPool + 9, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "filter", kCanonParamPool + 10, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "find", kCanonParamPool + 11, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "flat_map", kCanonParamPool + 12, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "for_each", kCanonParamPool + 13, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "get", kCanonParamPool + 14, 2, "", 2, 2, false, -1, -1, -1},
  {"", "", "group_by", kCanonParamPool + 16, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "index_of", kCanonParamPool + 17, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "insert", kCanonParamPool + 18, 2, "", 2, 2, false, -1, -1, -1},
  {"", "", "iter", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "join", kCanonParamPool + 20, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "map", kCanonParamPool + 21, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "max", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "max_by", kCanonParamPool + 22, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "min", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "min_by", kCanonParamPool + 23, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "partition", kCanonParamPool + 24, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "pop", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "presence", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "product", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "push", kCanonParamPool + 25, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "reduce", kCanonParamPool + 26, 2, "", 2, 2, false, -1, -1, -1},
  {"", "", "remove_at", kCanonParamPool + 28, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "reverse", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "size", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "slice", kCanonParamPool + 29, 2, "", 2, 2, false, -1, -1, -1},
  {"", "", "sort", kCanonParamPool + 31, 1, "", 0, 0, false, -1, 0, -1},
  {"", "", "sort_by", kCanonParamPool + 32, 2, "", 1, 1, false, -1, 1, -1},
  {"", "", "sorted", kCanonParamPool + 34, 1, "", 0, 0, false, -1, 0, -1},
  {"", "", "sorted_by", kCanonParamPool + 35, 2, "", 1, 1, false, -1, 1, -1},
  {"", "", "sum", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "to_object", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "to_set", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "unzip", nullptr, 0, "", 0, 0, false, -1, -1, -1},
};

inline constexpr CanonSig kCanonStringSigs[] = {
  {"", "", "bytes", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "capitalize", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "code_points", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "contains", kCanonParamPool + 37, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "count", kCanonParamPool + 38, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "empty", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "ends_with", kCanonParamPool + 39, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "eq_ignore_case", kCanonParamPool + 40, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "graphemes", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "index_of", kCanonParamPool + 41, 2, "", 1, 2, false, -1, -1, -1},
  {"", "", "is_alnum", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "is_alpha", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "is_ascii", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "is_digit", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "is_space", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "iter", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "last_index_of", kCanonParamPool + 43, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "lines", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "lower", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "normalize", kCanonParamPool + 44, 1, "", 0, 1, false, -1, -1, -1},
  {"", "", "presence", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "repeat", kCanonParamPool + 45, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "reverse", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "rsplit", kCanonParamPool + 46, 2, "", 1, 2, false, -1, -1, -1},
  {"", "", "size", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "slice", kCanonParamPool + 48, 2, "", 2, 2, false, -1, -1, -1},
  {"", "", "split", kCanonParamPool + 50, 2, "", 1, 2, false, -1, -1, -1},
  {"", "", "split_iter", kCanonParamPool + 52, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "split_whitespace", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "starts_with", kCanonParamPool + 53, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "strip_prefix", kCanonParamPool + 54, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "strip_suffix", kCanonParamPool + 55, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "title", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "to_string", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "tr", kCanonParamPool + 56, 2, "", 2, 2, false, -1, -1, -1},
  {"", "", "trim", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "trim_end", kCanonParamPool + 58, 1, "", 0, 1, false, -1, -1, -1},
  {"", "", "trim_start", kCanonParamPool + 59, 1, "", 0, 1, false, -1, -1, -1},
  {"", "", "truncate", kCanonParamPool + 60, 2, "", 1, 2, false, -1, -1, -1},
  {"", "", "upper", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "view", nullptr, 0, "", 0, 0, false, -1, -1, -1},
};

inline constexpr CanonSig kCanonSetSigs[] = {
  {"", "", "add", kCanonParamPool + 62, 1, "Bool", 1, 1, false, -1, -1, -1},
  {"", "", "contains", kCanonParamPool + 63, 1, "Bool", 1, 1, false, -1, -1, -1},
  {"", "", "diff", kCanonParamPool + 64, 1, "Set", 1, 1, false, -1, -1, -1},
  {"", "", "empty", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "intersect", kCanonParamPool + 65, 1, "Set", 1, 1, false, -1, -1, -1},
  {"", "", "iter", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "presence", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "remove", kCanonParamPool + 66, 1, "Bool", 1, 1, false, -1, -1, -1},
  {"", "", "size", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "subset", kCanonParamPool + 67, 1, "Bool", 1, 1, false, -1, -1, -1},
  {"", "", "superset", kCanonParamPool + 68, 1, "Bool", 1, 1, false, -1, -1, -1},
  {"", "", "sym_diff", kCanonParamPool + 69, 1, "Set", 1, 1, false, -1, -1, -1},
  {"", "", "to_array", nullptr, 0, "Array", 0, 0, false, -1, -1, -1},
  {"", "", "union", kCanonParamPool + 70, 1, "Set", 1, 1, false, -1, -1, -1},
};

inline constexpr CanonSig kCanonTupleSigs[] = {
  {"", "", "contains", kCanonParamPool + 71, 1, "Bool", 1, 1, false, -1, -1, -1},
  {"", "", "empty", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "iter", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "presence", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "size", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "to_array", nullptr, 0, "Array", 0, 0, false, -1, -1, -1},
};

inline constexpr CanonSig kCanonTensorSigs[] = {
  {"", "", "argmax", kCanonParamPool + 72, 1, "Tensor", 1, 1, false, -1, -1, -1},
  {"", "", "backward", nullptr, 0, "Tensor", 0, 0, false, -1, -1, -1},
  {"", "", "clone", nullptr, 0, "Tensor", 0, 0, false, -1, -1, -1},
  {"", "", "detach", nullptr, 0, "Tensor", 0, 0, false, -1, -1, -1},
  {"", "", "dot", kCanonParamPool + 73, 1, "Tensor", 1, 1, false, -1, -1, -1},
  {"", "", "grad", nullptr, 0, "Tensor", 0, 0, false, -1, -1, -1},
  {"", "", "item", nullptr, 0, "Float", 0, 0, false, -1, -1, -1},
  {"", "", "linear_sigmoid", kCanonParamPool + 74, 2, "Tensor", 2, 2, false, -1, -1, -1},
  {"", "", "log", nullptr, 0, "Tensor", 0, 0, false, -1, -1, -1},
  {"", "", "max", kCanonParamPool + 76, 1, "", 0, 1, false, -1, -1, -1},
  {"", "", "mean", kCanonParamPool + 77, 1, "", 0, 1, false, -1, -1, -1},
  {"", "", "pow", kCanonParamPool + 78, 1, "Tensor", 1, 1, false, -1, -1, -1},
  {"", "", "relu", nullptr, 0, "Tensor", 0, 0, false, -1, -1, -1},
  {"", "", "requires_grad", nullptr, 0, "Tensor", 0, 0, false, -1, -1, -1},
  {"", "", "reshape", kCanonParamPool + 79, 1, "Tensor", 1, 1, false, -1, -1, -1},
  {"", "", "shape", nullptr, 0, "Array", 0, 0, false, -1, -1, -1},
  {"", "", "sigmoid", nullptr, 0, "Tensor", 0, 0, false, -1, -1, -1},
  {"", "", "slice", kCanonParamPool + 80, 2, "Tensor", 2, 2, false, -1, -1, -1},
  {"", "", "softmax", nullptr, 0, "Tensor", 0, 0, false, -1, -1, -1},
  {"", "", "sum", kCanonParamPool + 82, 1, "", 0, 1, false, -1, -1, -1},
  {"", "", "to_array", nullptr, 0, "Array", 0, 0, false, -1, -1, -1},
  {"", "", "transpose", nullptr, 0, "Tensor", 0, 0, false, -1, -1, -1},
  {"", "", "zero_grad", nullptr, 0, "Tensor", 0, 0, false, -1, -1, -1},
};

inline constexpr CanonSig kCanonIteratorSigs[] = {
  {"", "", "all", kCanonParamPool + 83, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "any", kCanonParamPool + 84, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "chain", kCanonParamPool + 85, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "chunk_by", kCanonParamPool + 86, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "chunks", kCanonParamPool + 87, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "collect", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "contains", kCanonParamPool + 88, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "count", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "distinct", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "enumerate", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "filter", kCanonParamPool + 89, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "find", kCanonParamPool + 90, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "first", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "flat_map", kCanonParamPool + 91, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "flatten", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "for_each", kCanonParamPool + 92, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "group_by", kCanonParamPool + 93, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "join", kCanonParamPool + 94, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "last", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "map", kCanonParamPool + 95, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "max", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "max_by", kCanonParamPool + 96, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "min", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "min_by", kCanonParamPool + 97, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "nth", kCanonParamPool + 98, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "partition", kCanonParamPool + 99, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "position", kCanonParamPool + 100, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "product", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "reduce", kCanonParamPool + 101, 2, "", 2, 2, false, -1, -1, -1},
  {"", "", "scan", kCanonParamPool + 103, 2, "", 2, 2, false, -1, -1, -1},
  {"", "", "skip", kCanonParamPool + 105, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "skip_while", kCanonParamPool + 106, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "step_by", kCanonParamPool + 107, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "sum", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "take", kCanonParamPool + 108, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "take_while", kCanonParamPool + 109, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "tap", kCanonParamPool + 110, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "to_object", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "to_set", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "unzip", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "windows", kCanonParamPool + 111, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "zip", kCanonParamPool + 112, 1, "", 1, 1, false, -1, -1, -1},
};

}  // namespace culebra
// clang-format on
