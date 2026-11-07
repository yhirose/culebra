#pragma once
// Canonical native/built-in signature data, edited by hand (see canon_sigs.h
// for the structs and lookup helpers). Each signature row names its own
// parameter array, declared above its table: kCanonParams_<Ns>_[<sub>_]<name>
// for kCanonSigs_<Ns>, kCanonParams_<Type>Method_<name> for the value-type
// tables (kCanon<Type>Sigs). A parameter added or removed is one line there,
// since CanonSig derives the count, the arity bounds and the
// rest/keyword-only positions from it.
// clang-format off

namespace culebra {

inline constexpr CanonParam kCanonParams_IO_inspect[] = {
  {"x", false, false, false, false, true, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IO_print[] = {
  {"x", false, false, false, false, true, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IO_println[] = {
  {"x", true, false, false, false, true, "", CanonDefault::Str, 0, ""},
};

inline constexpr CanonParam kCanonParams_IO_einspect[] = {
  {"x", false, false, false, false, true, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IO_eprint[] = {
  {"x", false, false, false, false, true, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IO_eprintln[] = {
  {"x", false, false, false, false, true, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IO_capture[] = {
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_IO[] = {
  {"IO", "", "inspect", kCanonParams_IO_inspect, ""},
  {"IO", "", "print", kCanonParams_IO_print, ""},
  {"IO", "", "println", kCanonParams_IO_println, ""},
  {"IO", "", "input", {}, "String"},
  {"IO", "", "stdin", {}, "Object"},
  {"IO", "", "einspect", kCanonParams_IO_einspect, ""},
  {"IO", "", "eprint", kCanonParams_IO_eprint, ""},
  {"IO", "", "eprintln", kCanonParams_IO_eprintln, ""},
  {"IO", "", "stdin_is_terminal", {}, "Bool"},
  {"IO", "", "stdout_is_terminal", {}, "Bool"},
  {"IO", "", "stderr_is_terminal", {}, "Bool"},
  {"IO", "", "capture", kCanonParams_IO_capture, "String"},
};

inline constexpr CanonParam kCanonParams_Math_abs[] = {
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Math_min[] = {
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Math_max[] = {
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Math_pow[] = {
  {"base", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"exp", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Math_sign[] = {
  {"x", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Math_clamp[] = {
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"lo", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"hi", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Math_wrap[] = {
  {"x", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Math_log[] = {
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Math_exp[] = {
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Math_sqrt[] = {
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Math_sin[] = {
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Math_cos[] = {
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Math_tan[] = {
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Math_asin[] = {
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Math_acos[] = {
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Math_atan[] = {
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Math_atan2[] = {
  {"y", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Math_floor[] = {
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Math_ceil[] = {
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Math_round[] = {
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Math_f32[] = {
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Math[] = {
  {"Math", "", "abs", kCanonParams_Math_abs, ""},
  {"Math", "", "min", kCanonParams_Math_min, ""},
  {"Math", "", "max", kCanonParams_Math_max, ""},
  {"Math", "", "pow", kCanonParams_Math_pow, "Long"},
  {"Math", "", "sign", kCanonParams_Math_sign, "Long"},
  {"Math", "", "clamp", kCanonParams_Math_clamp, ""},
  {"Math", "", "wrap", kCanonParams_Math_wrap, "Long"},
  {"Math", "", "log", kCanonParams_Math_log, "Float"},
  {"Math", "", "exp", kCanonParams_Math_exp, "Float"},
  {"Math", "", "sqrt", kCanonParams_Math_sqrt, "Float"},
  {"Math", "", "sin", kCanonParams_Math_sin, "Float"},
  {"Math", "", "cos", kCanonParams_Math_cos, "Float"},
  {"Math", "", "tan", kCanonParams_Math_tan, "Float"},
  {"Math", "", "asin", kCanonParams_Math_asin, "Float"},
  {"Math", "", "acos", kCanonParams_Math_acos, "Float"},
  {"Math", "", "atan", kCanonParams_Math_atan, "Float"},
  {"Math", "", "atan2", kCanonParams_Math_atan2, "Float"},
  {"Math", "", "floor", kCanonParams_Math_floor, "Long"},
  {"Math", "", "ceil", kCanonParams_Math_ceil, "Long"},
  {"Math", "", "round", kCanonParams_Math_round, "Long"},
  {"Math", "", "f32", kCanonParams_Math_f32, "Float"},
};

inline constexpr CanonParam kCanonParams_FS_join[] = {
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_basename[] = {
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_dirname[] = {
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_extension[] = {
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_stem[] = {
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_exists[] = {
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_is_file[] = {
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_is_dir[] = {
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_read[] = {
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_write[] = {
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"content", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_size[] = {
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_list_dir[] = {
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_mkdir[] = {
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_remove[] = {
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"recursive", true, false, false, false, false, "", CanonDefault::Bool, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_stat[] = {
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_chmod[] = {
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"mode", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_chown[] = {
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"owner", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"group", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_rename[] = {
  {"src", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"dst", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_copy[] = {
  {"src", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"dst", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"recursive", true, false, false, false, false, "", CanonDefault::Bool, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_normpath[] = {
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_is_abs[] = {
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_abspath[] = {
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_realpath[] = {
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_is_symlink[] = {
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_symlink[] = {
  {"target", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"link", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_readlink[] = {
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_walk[] = {
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_glob[] = {
  {"pattern", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_watch[] = {
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"recursive", true, false, false, false, false, "", CanonDefault::Bool, 1, {}},
  {"match", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonParam kCanonParams_FS_mkdtemp[] = {
  {"prefix", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_FS[] = {
  {"FS", "", "join", kCanonParams_FS_join, ""},
  {"FS", "", "basename", kCanonParams_FS_basename, "String"},
  {"FS", "", "dirname", kCanonParams_FS_dirname, "String"},
  {"FS", "", "extension", kCanonParams_FS_extension, "String"},
  {"FS", "", "stem", kCanonParams_FS_stem, "String"},
  {"FS", "", "exists", kCanonParams_FS_exists, "Bool"},
  {"FS", "", "is_file", kCanonParams_FS_is_file, "Bool"},
  {"FS", "", "is_dir", kCanonParams_FS_is_dir, "Bool"},
  {"FS", "", "read", kCanonParams_FS_read, "String"},
  {"FS", "", "write", kCanonParams_FS_write, ""},
  {"FS", "", "size", kCanonParams_FS_size, "Long"},
  {"FS", "", "list_dir", kCanonParams_FS_list_dir, "Array"},
  {"FS", "", "mkdir", kCanonParams_FS_mkdir, ""},
  {"FS", "", "remove", kCanonParams_FS_remove, ""},
  {"FS", "", "stat", kCanonParams_FS_stat, "Object"},
  {"FS", "", "chmod", kCanonParams_FS_chmod, ""},
  {"FS", "", "chown", kCanonParams_FS_chown, ""},
  {"FS", "", "rename", kCanonParams_FS_rename, ""},
  {"FS", "", "copy", kCanonParams_FS_copy, ""},
  {"FS", "", "normpath", kCanonParams_FS_normpath, "String"},
  {"FS", "", "is_abs", kCanonParams_FS_is_abs, "Bool"},
  {"FS", "", "abspath", kCanonParams_FS_abspath, "String"},
  {"FS", "", "realpath", kCanonParams_FS_realpath, "String"},
  {"FS", "", "is_symlink", kCanonParams_FS_is_symlink, "Bool"},
  {"FS", "", "symlink", kCanonParams_FS_symlink, ""},
  {"FS", "", "readlink", kCanonParams_FS_readlink, "String"},
  {"FS", "", "walk", kCanonParams_FS_walk, "Array"},
  {"FS", "", "glob", kCanonParams_FS_glob, "Array"},
  {"FS", "", "watch", kCanonParams_FS_watch, "Object"},
  {"FS", "", "temp_dir", {}, "String"},
  {"FS", "", "mkdtemp", kCanonParams_FS_mkdtemp, "String"},
  {"FS", "", "sep", {}, "String"},
};

inline constexpr CanonParam kCanonParams_File_open[] = {
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"mode", true, false, false, false, false, "", CanonDefault::Str, 0, "r"},
};

inline constexpr CanonParam kCanonParams_File_with[] = {
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"mode", true, false, false, false, false, "", CanonDefault::Str, 0, "r"},
  {"fn", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_File[] = {
  {"File", "", "open", kCanonParams_File_open, "Object"},
  {"File", "", "with", kCanonParams_File_with, ""},
};

inline constexpr CanonParam kCanonParams_Random_seed[] = {
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Random_int[] = {
  {"lo", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"hi", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Random_uniform[] = {
  {"lo", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"hi", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Random_gauss[] = {
  {"mu", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"sigma", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Random_shuffle[] = {
  {"a", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Random_weighted_choice[] = {
  {"pop", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"weights", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Random_choice[] = {
  {"pop", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Random[] = {
  {"Random", "", "seed", kCanonParams_Random_seed, ""},
  {"Random", "", "int", kCanonParams_Random_int, "Long"},
  {"Random", "", "uniform", kCanonParams_Random_uniform, "Float"},
  {"Random", "", "gauss", kCanonParams_Random_gauss, "Float"},
  {"Random", "", "shuffle", kCanonParams_Random_shuffle, ""},
  {"Random", "", "weighted_choice", kCanonParams_Random_weighted_choice, ""},
  {"Random", "", "choice", kCanonParams_Random_choice, ""},
};

inline constexpr CanonParam kCanonParams_Sys_exit[] = {
  {"code", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Sys_env[] = {
  {"name", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"fallback", true, false, false, false, false, "", CanonDefault::Str, 0, ""},
};

inline constexpr CanonParam kCanonParams_Sys_chdir[] = {
  {"path", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Sys_set_env[] = {
  {"name", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"value", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Sys_data_dir[] = {
  {"app", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Sys[] = {
  {"Sys", "", "exit", kCanonParams_Sys_exit, ""},
  {"Sys", "", "env", kCanonParams_Sys_env, ""},
  {"Sys", "", "getcwd", {}, "String"},
  {"Sys", "", "chdir", kCanonParams_Sys_chdir, ""},
  {"Sys", "", "set_env", kCanonParams_Sys_set_env, ""},
  {"Sys", "", "data_dir", kCanonParams_Sys_data_dir, "String"},
  {"Sys", "", "time", {}, "Float"},
};

inline constexpr CanonParam kCanonParams_GC_refcount[] = {
  {"value", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_GC[] = {
  {"GC", "", "stat", {}, "Object"},
  {"GC", "", "refcount", kCanonParams_GC_refcount, "Long | Nil"},
};

inline constexpr CanonParam kCanonParams_Regex_native_check[] = {
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Regex_native_test[] = {
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Regex_native_find[] = {
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Regex_native_match[] = {
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Regex_native_find_from[] = {
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"pos", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Regex_native_find_all[] = {
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Regex_native_find_all_str[] = {
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Regex_native_find_all_index[] = {
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Regex_native_count[] = {
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Regex_native_replace_all[] = {
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"repl", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Regex_native_replace_first[] = {
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"repl", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Regex_native_split[] = {
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Regex_native[] = {
  {"_Regex", "", "check", kCanonParams_Regex_native_check, ""},
  {"_Regex", "", "test", kCanonParams_Regex_native_test, "Bool"},
  {"_Regex", "", "find", kCanonParams_Regex_native_find, ""},
  {"_Regex", "", "match", kCanonParams_Regex_native_match, ""},
  {"_Regex", "", "find_from", kCanonParams_Regex_native_find_from, ""},
  {"_Regex", "", "find_all", kCanonParams_Regex_native_find_all, ""},
  {"_Regex", "", "find_all_str", kCanonParams_Regex_native_find_all_str, ""},
  {"_Regex", "", "find_all_index", kCanonParams_Regex_native_find_all_index, ""},
  {"_Regex", "", "count", kCanonParams_Regex_native_count, "Long"},
  {"_Regex", "", "replace_all", kCanonParams_Regex_native_replace_all, "String"},
  {"_Regex", "", "replace_first", kCanonParams_Regex_native_replace_first, "String"},
  {"_Regex", "", "split", kCanonParams_Regex_native_split, ""},
};

inline constexpr CanonParam kCanonParams_PEG_native_load[] = {
  {"grammar", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"start", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_PEG_native_parse[] = {
  {"grammar", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"text", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"start", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"optimize", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
  {"packrat", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
  {"path", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"hooks", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_PEG_native_run[] = {
  {"grammar", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"text", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"start", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"ast", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"packrat", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
  {"hooks", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_PEG_native[] = {
  {"_PEG", "", "load", kCanonParams_PEG_native_load, "Tuple"},
  {"_PEG", "", "parse", kCanonParams_PEG_native_parse, ""},
  {"_PEG", "", "run", kCanonParams_PEG_native_run, "Tuple"},
};

inline constexpr CanonParam kCanonParams_FST_native_compile_set[] = {
  {"keys", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"sorted", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FST_native_compile_auto_index[] = {
  {"keys", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"sorted", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FST_native_compile_map[] = {
  {"entries", false, false, false, false, false, "Object", CanonDefault::None, 0, {}},
  {"sorted", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FST_native_compile_index_map[] = {
  {"entries", false, false, false, false, false, "Object", CanonDefault::None, 0, {}},
  {"sorted", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FST_native_set_check[] = {
  {"bytecode", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FST_native_set_contains[] = {
  {"bytecode", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"key", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FST_native_set_common_prefix_search[] = {
  {"bytecode", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"text", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FST_native_set_longest_common_prefix_search[] = {
  {"bytecode", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"text", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FST_native_set_predictive_search[] = {
  {"bytecode", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"prefix", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FST_native_set_edit_distance_search[] = {
  {"bytecode", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"word", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"max_edits", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"insert_cost", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"delete_cost", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"replace_cost", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FST_native_set_suggest[] = {
  {"bytecode", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"word", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FST_native_map_check[] = {
  {"bytecode", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FST_native_map_get[] = {
  {"bytecode", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"key", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FST_native_map_common_prefix_search[] = {
  {"bytecode", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"text", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FST_native_map_longest_common_prefix_search[] = {
  {"bytecode", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"text", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FST_native_map_predictive_search[] = {
  {"bytecode", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"prefix", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FST_native_map_edit_distance_search[] = {
  {"bytecode", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"word", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"max_edits", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"insert_cost", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"delete_cost", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"replace_cost", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FST_native_map_suggest[] = {
  {"bytecode", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"word", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FST_native_index_check[] = {
  {"bytecode", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FST_native_index_get[] = {
  {"bytecode", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"key", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FST_native_index_common_prefix_search[] = {
  {"bytecode", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"text", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FST_native_index_longest_common_prefix_search[] = {
  {"bytecode", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"text", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FST_native_index_predictive_search[] = {
  {"bytecode", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"prefix", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FST_native_index_edit_distance_search[] = {
  {"bytecode", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"word", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"max_edits", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"insert_cost", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"delete_cost", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"replace_cost", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_FST_native_index_suggest[] = {
  {"bytecode", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"word", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_FST_native[] = {
  {"_FST", "", "compile_set", kCanonParams_FST_native_compile_set, "String"},
  {"_FST", "", "compile_auto_index", kCanonParams_FST_native_compile_auto_index, "String"},
  {"_FST", "", "compile_map", kCanonParams_FST_native_compile_map, "String"},
  {"_FST", "", "compile_index_map", kCanonParams_FST_native_compile_index_map, "String"},
  {"_FST", "", "set_check", kCanonParams_FST_native_set_check, ""},
  {"_FST", "", "set_contains", kCanonParams_FST_native_set_contains, "Bool"},
  {"_FST", "", "set_common_prefix_search", kCanonParams_FST_native_set_common_prefix_search, ""},
  {"_FST", "", "set_longest_common_prefix_search", kCanonParams_FST_native_set_longest_common_prefix_search, ""},
  {"_FST", "", "set_predictive_search", kCanonParams_FST_native_set_predictive_search, ""},
  {"_FST", "", "set_edit_distance_search", kCanonParams_FST_native_set_edit_distance_search, ""},
  {"_FST", "", "set_suggest", kCanonParams_FST_native_set_suggest, ""},
  {"_FST", "", "map_check", kCanonParams_FST_native_map_check, ""},
  {"_FST", "", "map_get", kCanonParams_FST_native_map_get, ""},
  {"_FST", "", "map_common_prefix_search", kCanonParams_FST_native_map_common_prefix_search, ""},
  {"_FST", "", "map_longest_common_prefix_search", kCanonParams_FST_native_map_longest_common_prefix_search, ""},
  {"_FST", "", "map_predictive_search", kCanonParams_FST_native_map_predictive_search, ""},
  {"_FST", "", "map_edit_distance_search", kCanonParams_FST_native_map_edit_distance_search, ""},
  {"_FST", "", "map_suggest", kCanonParams_FST_native_map_suggest, ""},
  {"_FST", "", "index_check", kCanonParams_FST_native_index_check, ""},
  {"_FST", "", "index_get", kCanonParams_FST_native_index_get, ""},
  {"_FST", "", "index_common_prefix_search", kCanonParams_FST_native_index_common_prefix_search, ""},
  {"_FST", "", "index_longest_common_prefix_search", kCanonParams_FST_native_index_longest_common_prefix_search, ""},
  {"_FST", "", "index_predictive_search", kCanonParams_FST_native_index_predictive_search, ""},
  {"_FST", "", "index_edit_distance_search", kCanonParams_FST_native_index_edit_distance_search, ""},
  {"_FST", "", "index_suggest", kCanonParams_FST_native_index_suggest, ""},
};

inline constexpr CanonParam kCanonParams_Search_Index_new[] = {
  {"analyzer", true, false, false, false, false, "Object", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonParam kCanonParams_Search_Index_load[] = {
  {"path", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"analyzer", true, false, false, false, false, "Object", CanonDefault::Nil, 0, {}},
  {"readonly", true, false, false, false, false, "", CanonDefault::Bool, 0, {}},
};

inline constexpr CanonParam kCanonParams_Search_segmenter[] = {
  {"model", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Search[] = {
  {"Search", "Index", "new", kCanonParams_Search_Index_new, "Object"},
  {"Search", "Index", "load", kCanonParams_Search_Index_load, "Object"},
  {"Search", "", "segmenter", kCanonParams_Search_segmenter, "Object"},
};

inline constexpr CanonParam kCanonParams_Net_connect[] = {
  {"host", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"port", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"timeout", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
};

inline constexpr CanonParam kCanonParams_Net_listen[] = {
  {"port", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"host", true, false, false, false, false, "String", CanonDefault::Str, 0, "0.0.0.0"},
  {"backlog", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
};

inline constexpr CanonParam kCanonParams_Net_udp[] = {
  {"port", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  {"host", true, false, false, false, false, "String", CanonDefault::Str, 0, "0.0.0.0"},
};

inline constexpr CanonParam kCanonParams_Net_resolve[] = {
  {"host", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Net[] = {
  {"Net", "", "connect", kCanonParams_Net_connect, "Object"},
  {"Net", "", "listen", kCanonParams_Net_listen, "Object"},
  {"Net", "", "udp", kCanonParams_Net_udp, "Object"},
  {"Net", "", "resolve", kCanonParams_Net_resolve, "Array"},
};

inline constexpr CanonParam kCanonParams_Proc_run[] = {
  {"cmd", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"cwd", true, false, false, false, false, "String?", CanonDefault::Nil, 0, {}},
  {"env", true, false, false, false, false, "Object?", CanonDefault::Nil, 0, {}},
  {"input", true, false, false, false, false, "String", CanonDefault::Str, 0, ""},
  {"check", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  {"timeout", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  {"share", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"inherit_env", true, false, false, false, false, "Bool", CanonDefault::Bool, 1, {}},
  {"stdin", true, false, false, false, false, "String", CanonDefault::Str, 0, "pipe"},
  {"stdout", true, false, false, false, false, "String", CanonDefault::Str, 0, "pipe"},
  {"stderr", true, false, false, false, false, "String", CanonDefault::Str, 0, "pipe"},
};

inline constexpr CanonParam kCanonParams_Proc_all[] = {
  {"commands", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"limit", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  {"timeout", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  {"fail_fast", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  {"retries", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  {"share", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"inherit_env", true, false, false, false, false, "Bool", CanonDefault::Bool, 1, {}},
};

inline constexpr CanonParam kCanonParams_Proc_race[] = {
  {"commands", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"share", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"inherit_env", true, false, false, false, false, "Bool", CanonDefault::Bool, 1, {}},
};

inline constexpr CanonParam kCanonParams_Proc_spawn[] = {
  {"cmd", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"cwd", true, false, false, false, false, "String?", CanonDefault::Nil, 0, {}},
  {"env", true, false, false, false, false, "Object?", CanonDefault::Nil, 0, {}},
  {"input", true, false, false, false, false, "String", CanonDefault::Str, 0, ""},
  {"share", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"inherit_env", true, false, false, false, false, "Bool", CanonDefault::Bool, 1, {}},
  {"stdin", true, false, false, false, false, "String", CanonDefault::Str, 0, "pipe"},
  {"stdout", true, false, false, false, false, "String", CanonDefault::Str, 0, "pipe"},
  {"stderr", true, false, false, false, false, "String", CanonDefault::Str, 0, "pipe"},
};

inline constexpr CanonSig kCanonSigs_Proc[] = {
  {"Proc", "", "run", kCanonParams_Proc_run, "Object"},
  {"Proc", "", "all", kCanonParams_Proc_all, "Array"},
  {"Proc", "", "race", kCanonParams_Proc_race, "Object"},
  {"Proc", "", "spawn", kCanonParams_Proc_spawn, "Object"},
};

inline constexpr CanonParam kCanonParams_Http_get[] = {
  {"url", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"headers", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"timeout", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"follow_redirects", true, false, false, false, false, "", CanonDefault::Bool, 1, {}},
  {"into", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"params", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"events", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonParam kCanonParams_Http_delete[] = {
  {"url", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"headers", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"timeout", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"follow_redirects", true, false, false, false, false, "", CanonDefault::Bool, 1, {}},
  {"into", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"params", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"events", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonParam kCanonParams_Http_head[] = {
  {"url", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"headers", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"timeout", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"follow_redirects", true, false, false, false, false, "", CanonDefault::Bool, 1, {}},
  {"into", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"params", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"events", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonParam kCanonParams_Http_post[] = {
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
  {"events", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonParam kCanonParams_Http_put[] = {
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
  {"events", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonParam kCanonParams_Http_request[] = {
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
  {"events", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonParam kCanonParams_Http_sse[] = {
  {"url", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"on_event", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  {"headers", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"timeout", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"follow_redirects", true, false, false, false, false, "", CanonDefault::Bool, 1, {}},
};

inline constexpr CanonParam kCanonParams_Http_client[] = {
  {"base_url", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"headers", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"timeout", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"follow_redirects", true, false, false, false, false, "", CanonDefault::Bool, 1, {}},
};

inline constexpr CanonParam kCanonParams_Http_ws[] = {
  {"url", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Http[] = {
  {"Http", "", "get", kCanonParams_Http_get, "Object"},
  {"Http", "", "delete", kCanonParams_Http_delete, "Object"},
  {"Http", "", "head", kCanonParams_Http_head, "Object"},
  {"Http", "", "post", kCanonParams_Http_post, "Object"},
  {"Http", "", "put", kCanonParams_Http_put, "Object"},
  {"Http", "", "request", kCanonParams_Http_request, "Object"},
  {"Http", "", "sse", kCanonParams_Http_sse, "Object"},
  {"Http", "", "client", kCanonParams_Http_client, "Object"},
  {"Http", "", "server", {}, "Object"},
  {"Http", "", "ws", kCanonParams_Http_ws, "Object"},
};

inline constexpr CanonParam kCanonParams_Isolate_spawn[] = {
  {"fn", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Isolate[] = {
  {"Isolate", "", "spawn", kCanonParams_Isolate_spawn, "Object"},
};

inline constexpr CanonParam kCanonParams_Channel_new[] = {
  {"cap", true, false, false, false, false, "", CanonDefault::Long, 1, {}},
};

inline constexpr CanonParam kCanonParams_Channel_fan_in[] = {
  {"items", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"fn", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Channel[] = {
  {"Channel", "", "new", kCanonParams_Channel_new, "Tuple"},
  {"Channel", "", "fan_in", kCanonParams_Channel_fan_in, ""},
};

inline constexpr CanonParam kCanonParams_Signal_notify[] = {
  {"tx", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Signal[] = {
  {"Signal", "", "notify", kCanonParams_Signal_notify, "Nil"},
  {"Signal", "", "reset", {}, "Nil"},
};

inline constexpr CanonParam kCanonParams_SharedBuffer_new[] = {
  {"count", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"type", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_SharedBuffer_file[] = {
  {"path", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"count", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"type", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_SharedBuffer_shared[] = {
  {"count", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"type", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_SharedBuffer_receive[] = {
  {"name", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"type", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_SharedBuffer[] = {
  {"SharedBuffer", "", "new", kCanonParams_SharedBuffer_new, "Object"},
  {"SharedBuffer", "", "file", kCanonParams_SharedBuffer_file, "Object"},
  {"SharedBuffer", "", "shared", kCanonParams_SharedBuffer_shared, "Object"},
  {"SharedBuffer", "", "receive", kCanonParams_SharedBuffer_receive, "Object"},
};

inline constexpr CanonParam kCanonParams_Shared_new[] = {
  {"value", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Shared[] = {
  {"Shared", "", "new", kCanonParams_Shared_new, ""},
};

inline constexpr CanonParam kCanonParams_Parallel_map[] = {
  {"items", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"fn", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"limit", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"on_progress", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonParam kCanonParams_Parallel_each[] = {
  {"items", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"fn", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"limit", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"on_progress", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonParam kCanonParams_Parallel_map_settled[] = {
  {"items", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"fn", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"limit", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"on_progress", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonParam kCanonParams_Parallel_race[] = {
  {"items", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"fn", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"limit", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"on_progress", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Parallel[] = {
  {"Parallel", "", "map", kCanonParams_Parallel_map, "Array"},
  {"Parallel", "", "each", kCanonParams_Parallel_each, "Nil"},
  {"Parallel", "", "map_settled", kCanonParams_Parallel_map_settled, "Array"},
  {"Parallel", "", "race", kCanonParams_Parallel_race, "Any"},
};

inline constexpr CanonParam kCanonParams_JSON_stringify[] = {
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"indent", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  {"sort_keys", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  {"lines", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
};

inline constexpr CanonParam kCanonParams_JSON_parse[] = {
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"lines", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  {"number_mode", true, false, false, false, false, "String", CanonDefault::Str, 0, "auto"},
  {"jsonc", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
};

inline constexpr CanonSig kCanonSigs_JSON[] = {
  {"JSON", "", "stringify", kCanonParams_JSON_stringify, "String"},
  {"JSON", "", "parse", kCanonParams_JSON_parse, ""},
};

inline constexpr CanonParam kCanonParams_Encoding_html_escape[] = {
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Encoding_html_unescape[] = {
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Encoding_base64_encode[] = {
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Encoding_base64_decode[] = {
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Encoding_hex_encode[] = {
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Encoding_hex_decode[] = {
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Encoding_url_encode[] = {
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Encoding_url_decode[] = {
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Encoding_utf16_encode[] = {
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"bom", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
};

inline constexpr CanonParam kCanonParams_Encoding_utf16_decode[] = {
  {"bytes", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Encoding[] = {
  {"Encoding", "html", "escape", kCanonParams_Encoding_html_escape, "String"},
  {"Encoding", "html", "unescape", kCanonParams_Encoding_html_unescape, "String"},
  {"Encoding", "base64", "encode", kCanonParams_Encoding_base64_encode, "String"},
  {"Encoding", "base64", "decode", kCanonParams_Encoding_base64_decode, "String"},
  {"Encoding", "hex", "encode", kCanonParams_Encoding_hex_encode, "String"},
  {"Encoding", "hex", "decode", kCanonParams_Encoding_hex_decode, "String"},
  {"Encoding", "url", "encode", kCanonParams_Encoding_url_encode, "String"},
  {"Encoding", "url", "decode", kCanonParams_Encoding_url_decode, "String"},
  {"Encoding", "utf16", "encode", kCanonParams_Encoding_utf16_encode, "String"},
  {"Encoding", "utf16", "decode", kCanonParams_Encoding_utf16_decode, "String"},
};

inline constexpr CanonParam kCanonParams_Compress_gzip[] = {
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Compress_gunzip[] = {
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Compress_deflate[] = {
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"level", true, false, false, false, false, "Long", CanonDefault::Long, -1, {}},
};

inline constexpr CanonParam kCanonParams_Compress_zip[] = {
  {"files", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Compress[] = {
  {"Compress", "", "gzip", kCanonParams_Compress_gzip, "String"},
  {"Compress", "", "gunzip", kCanonParams_Compress_gunzip, "String"},
  {"Compress", "", "deflate", kCanonParams_Compress_deflate, "String"},
  {"Compress", "", "zip", kCanonParams_Compress_zip, "String"},
};

inline constexpr CanonParam kCanonParams_Hash_sha256[] = {
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Hash_sha1[] = {
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Hash_sha512[] = {
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Hash_md5[] = {
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Hash_hmac_sha256[] = {
  {"key", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Hash_hmac_sha1[] = {
  {"key", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Hash_hmac_sha512[] = {
  {"key", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Hash[] = {
  {"Hash", "", "sha256", kCanonParams_Hash_sha256, "String"},
  {"Hash", "", "sha1", kCanonParams_Hash_sha1, "String"},
  {"Hash", "", "sha512", kCanonParams_Hash_sha512, "String"},
  {"Hash", "", "md5", kCanonParams_Hash_md5, "String"},
  {"Hash", "", "hmac_sha256", kCanonParams_Hash_hmac_sha256, "String"},
  {"Hash", "", "hmac_sha1", kCanonParams_Hash_hmac_sha1, "String"},
  {"Hash", "", "hmac_sha512", kCanonParams_Hash_hmac_sha512, "String"},
};

inline constexpr CanonParam kCanonParams_CSV_parse[] = {
  {"text", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"delimiter", true, false, false, false, false, "String", CanonDefault::Str, 0, ","},
  {"header", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  {"types", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonParam kCanonParams_CSV_stringify[] = {
  {"rows", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"delimiter", true, false, false, false, false, "String", CanonDefault::Str, 0, ","},
};

inline constexpr CanonSig kCanonSigs_CSV[] = {
  {"CSV", "", "parse", kCanonParams_CSV_parse, "Array"},
  {"CSV", "", "stringify", kCanonParams_CSV_stringify, "String"},
};

inline constexpr CanonParam kCanonParams_SQLite_open[] = {
  {"path", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_SQLite[] = {
  {"SQLite", "", "open", kCanonParams_SQLite_open, "Object"},
  {"SQLite", "", "version", {}, "String"},
};

#if defined(CULEBRA_LLAMA_ENABLED)
inline constexpr CanonParam kCanonParams_Llama_load[] = {
  {"path", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"n_ctx", true, false, false, false, false, "", CanonDefault::Long, 2048, {}},
  {"gpu", true, false, false, false, false, "", CanonDefault::Bool, 1, {}},
  {"n_batch", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"n_slots", true, false, false, false, false, "", CanonDefault::Long, 1, {}},
};

inline constexpr CanonSig kCanonSigs_Llama[] = {
  {"Llama", "", "load", kCanonParams_Llama_load, "Object"},
};
#endif

inline constexpr CanonParam kCanonParams_TOML_parse[] = {
  {"text", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_TOML_stringify[] = {
  {"v", false, false, false, false, false, "Object", CanonDefault::None, 0, {}},
  {"sort_keys", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
};

inline constexpr CanonSig kCanonSigs_TOML[] = {
  {"TOML", "", "parse", kCanonParams_TOML_parse, "Object"},
  {"TOML", "", "stringify", kCanonParams_TOML_stringify, "String"},
};

inline constexpr CanonParam kCanonParams_XML_parse[] = {
  {"text", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"keep_space", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
};

inline constexpr CanonParam kCanonParams_XML_events[] = {
  {"text", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"keep_space", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
};

inline constexpr CanonParam kCanonParams_XML_find[] = {
  {"el", false, false, false, false, false, "Object", CanonDefault::None, 0, {}},
  {"path", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"namespaces", true, false, false, false, false, "Object", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonParam kCanonParams_XML_find_all[] = {
  {"el", false, false, false, false, false, "Object", CanonDefault::None, 0, {}},
  {"path", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"namespaces", true, false, false, false, false, "Object", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonParam kCanonParams_XML_find_iter[] = {
  {"el", false, false, false, false, false, "Object", CanonDefault::None, 0, {}},
  {"path", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"namespaces", true, false, false, false, false, "Object", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonParam kCanonParams_XML_text[] = {
  {"el", false, false, false, false, false, "Object", CanonDefault::None, 0, {}},
  {"path", true, false, false, false, false, "String", CanonDefault::Str, 0, "."},
  {"namespaces", true, false, false, false, false, "Object", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonParam kCanonParams_XML_stringify[] = {
  {"el", false, false, false, false, false, "Object", CanonDefault::None, 0, {}},
  {"indent", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  {"declaration", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
};

inline constexpr CanonSig kCanonSigs_XML[] = {
  {"XML", "", "parse", kCanonParams_XML_parse, "Object"},
  {"XML", "", "events", kCanonParams_XML_events, "Iterator"},
  {"XML", "", "find", kCanonParams_XML_find, "Object | Nil"},
  {"XML", "", "find_all", kCanonParams_XML_find_all, "Array"},
  {"XML", "", "find_iter", kCanonParams_XML_find_iter, "Iterator"},
  {"XML", "", "text", kCanonParams_XML_text, "String | Nil"},
  {"XML", "", "stringify", kCanonParams_XML_stringify, "String"},
};

inline constexpr CanonParam kCanonParams_Env_parse[] = {
  {"text", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Env_load[] = {
  {"path", true, false, false, false, false, "String", CanonDefault::Str, 0, ".env"},
  {"override", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Env[] = {
  {"Env", "", "parse", kCanonParams_Env_parse, "Object"},
  {"Env", "", "load", kCanonParams_Env_load, "Object"},
};

inline constexpr CanonParam kCanonParams_Set_new[] = {
  {"items", false, false, false, true, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Set[] = {
  {"Set", "", "new", kCanonParams_Set_new, "Set"},
};

inline constexpr CanonSig kCanonSigs_UUID[] = {
  {"UUID", "", "v4", {}, "String"},
  {"UUID", "", "v7", {}, "String"},
};

inline constexpr CanonParam kCanonParams_String_from_code_point[] = {
  {"cp", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_String_from_bytes[] = {
  {"bytes", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_String_from_code_points[] = {
  {"cps", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_String_pack[] = {
  {"type", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"values", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"endian", true, false, false, false, false, "String", CanonDefault::Str, 0, "little"},
};

inline constexpr CanonParam kCanonParams_String_unpack[] = {
  {"type", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"data", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"at", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  {"count", true, false, false, false, false, "Long?", CanonDefault::Nil, 0, {}},
  {"into", true, false, false, false, false, "Array?", CanonDefault::Nil, 0, {}},
  {"endian", true, false, false, false, false, "String", CanonDefault::Str, 0, "little"},
};

inline constexpr CanonParam kCanonParams_String_pack_size[] = {
  {"type", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_String[] = {
  {"String", "", "from_code_point", kCanonParams_String_from_code_point, "String"},
  {"String", "", "from_bytes", kCanonParams_String_from_bytes, "String"},
  {"String", "", "from_code_points", kCanonParams_String_from_code_points, "String"},
  {"String", "", "pack", kCanonParams_String_pack, "String"},
  {"String", "", "unpack", kCanonParams_String_unpack, "Array"},
  {"String", "", "pack_size", kCanonParams_String_pack_size, "Long"},
};

inline constexpr CanonParam kCanonParams_Tensor_zeros[] = {
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Tensor_ones[] = {
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Tensor_randn[] = {
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Tensor_eval[] = {
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Tensor_from_csv[] = {
  {"path", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Tensor_from[] = {
  {"arr", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Tensor_concat[] = {
  {"parts", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"axis", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
};

inline constexpr CanonParam kCanonParams_Tensor_no_grad[] = {
  {"fn", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Tensor_where[] = {
  {"cond", false, false, false, false, false, "Tensor", CanonDefault::None, 0, {}},
  {"a", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"b", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Tensor_index_add[] = {
  {"indices", false, false, false, false, false, "Tensor", CanonDefault::None, 0, {}},
  {"values", false, false, false, false, false, "Tensor", CanonDefault::None, 0, {}},
  {"target_shape", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Tensor_scatter_to_axis[] = {
  {"indices", false, false, false, false, false, "Tensor", CanonDefault::None, 0, {}},
  {"values", false, false, false, false, false, "Tensor", CanonDefault::None, 0, {}},
  {"size", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Tensor_adam_step[] = {
  {"p", false, false, false, false, false, "Tensor", CanonDefault::None, 0, {}},
  {"m", false, false, false, false, false, "Tensor", CanonDefault::None, 0, {}},
  {"v", false, false, false, false, false, "Tensor", CanonDefault::None, 0, {}},
  {"g", false, false, false, false, false, "Tensor", CanonDefault::None, 0, {}},
  {"lr", false, false, false, false, false, "Float", CanonDefault::None, 0, {}},
  {"beta1", false, false, false, false, false, "Float", CanonDefault::None, 0, {}},
  {"beta2", false, false, false, false, false, "Float", CanonDefault::None, 0, {}},
  {"eps", false, false, false, false, false, "Float", CanonDefault::None, 0, {}},
  {"step", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Tensor_profile[] = {
  {"fn", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Tensor_profile_scope[] = {
  {"label", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"fn", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Tensor[] = {
  {"Tensor", "", "zeros", kCanonParams_Tensor_zeros, "Tensor"},
  {"Tensor", "", "ones", kCanonParams_Tensor_ones, "Tensor"},
  {"Tensor", "", "randn", kCanonParams_Tensor_randn, "Tensor"},
  {"Tensor", "", "eval", kCanonParams_Tensor_eval, ""},
  {"Tensor", "", "from_csv", kCanonParams_Tensor_from_csv, "Tensor"},
  {"Tensor", "", "from", kCanonParams_Tensor_from, "Tensor"},
  {"Tensor", "", "concat", kCanonParams_Tensor_concat, "Tensor"},
  {"Tensor", "", "no_grad", kCanonParams_Tensor_no_grad, "Any"},
  {"Tensor", "", "where", kCanonParams_Tensor_where, "Tensor"},
  {"Tensor", "", "index_add", kCanonParams_Tensor_index_add, "Tensor"},
  {"Tensor", "", "scatter_to_axis", kCanonParams_Tensor_scatter_to_axis, "Tensor"},
  {"Tensor", "", "adam_step", kCanonParams_Tensor_adam_step, ""},
  {"Tensor", "", "profile", kCanonParams_Tensor_profile, "Array"},
  {"Tensor", "", "profile_scope", kCanonParams_Tensor_profile_scope, "Any"},
  {"Tensor", "", "use_cpu", {}, ""},
  {"Tensor", "", "use_gpu", {}, ""},
  {"Tensor", "", "use_auto", {}, ""},
  {"Tensor", "", "gpu_available", {}, "Bool"},
  {"Tensor", "", "device", {}, "String"},
};

inline constexpr CanonParam kCanonParams_Time_native_sleep[] = {
  {"secs", false, false, false, false, false, "Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Time_native_from_iso_nanos[] = {
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Time_native_parse_nanos[] = {
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"fmt", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Time_native_iso_nanos[] = {
  {"nanos", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"utc", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Time_native_format_nanos[] = {
  {"nanos", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"fmt", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"utc", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Time_native_parts_nanos[] = {
  {"nanos", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"utc", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Time_native_from_parts_nanos[] = {
  {"p", false, false, false, false, false, "Object", CanonDefault::None, 0, {}},
  {"utc", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Time_native_weekday_nanos[] = {
  {"nanos", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"utc", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Time_native_add_nanos[] = {
  {"nanos", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"years", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"months", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"days", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"hours", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"minutes", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"seconds", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"utc", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Time_native_start_of_nanos[] = {
  {"nanos", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"unit", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"utc", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Time_native[] = {
  {"_Time", "", "now_nanos", {}, "Long"},
  {"_Time", "", "monotonic", {}, "Float"},
  {"_Time", "", "sleep", kCanonParams_Time_native_sleep, ""},
  {"_Time", "", "from_iso_nanos", kCanonParams_Time_native_from_iso_nanos, "Long"},
  {"_Time", "", "parse_nanos", kCanonParams_Time_native_parse_nanos, "Long"},
  {"_Time", "", "iso_nanos", kCanonParams_Time_native_iso_nanos, "String"},
  {"_Time", "", "format_nanos", kCanonParams_Time_native_format_nanos, "String"},
  {"_Time", "", "parts_nanos", kCanonParams_Time_native_parts_nanos, "Object"},
  {"_Time", "", "from_parts_nanos", kCanonParams_Time_native_from_parts_nanos, "Long"},
  {"_Time", "", "weekday_nanos", kCanonParams_Time_native_weekday_nanos, "Long"},
  {"_Time", "", "add_nanos", kCanonParams_Time_native_add_nanos, "Long"},
  {"_Time", "", "start_of_nanos", kCanonParams_Time_native_start_of_nanos, "Long"},
};

inline constexpr CanonParam kCanonParams_Term_native_width[] = {
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Term_native_read_key[] = {
  {"timeout", false, false, false, false, false, "Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Term_native[] = {
  {"_Term", "", "cols", {}, "Long"},
  {"_Term", "", "rows", {}, "Long"},
  {"_Term", "", "raw_on", {}, ""},
  {"_Term", "", "raw_off", {}, ""},
  {"_Term", "", "flush", {}, ""},
  {"_Term", "", "color_level", {}, "Long"},
  {"_Term", "", "width", kCanonParams_Term_native_width, "Long"},
  {"_Term", "", "resized", {}, "Bool"},
  {"_Term", "", "read_key", kCanonParams_Term_native_read_key, "String"},
  {"_Term", "", "attach_tty", {}, "Bool"},
};

inline constexpr CanonParam kCanonParams_Canvas_native_init[] = {
  {"w", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"h", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_ttf_load[] = {
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_ttf_free[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_ttf_glyph[] = {
  {"font", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"codepoint", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"x", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"size", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_ttf_glyph_screen[] = {
  {"font", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"codepoint", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"x", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"size", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_ttf_advance[] = {
  {"font", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"codepoint", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"size", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_ttf_ascent[] = {
  {"font", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"size", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_get_screen_pixel[] = {
  {"x", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_clear[] = {
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_set_pixel[] = {
  {"x", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_get_pixel[] = {
  {"x", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_rect[] = {
  {"x", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"w", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"h", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"fill", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_line[] = {
  {"x1", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y1", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"x2", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y2", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_ellipse[] = {
  {"cx", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"cy", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rx", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"ry", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"fill", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_triangle[] = {
  {"x1", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y1", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"x2", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y2", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"x3", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y3", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"fill", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_polygon[] = {
  {"points", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"fill", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_font_load[] = {
  {"rows", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_glyph[] = {
  {"font", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"index", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"x", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"scale", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_sprite_load[] = {
  {"pixels", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"w", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"h", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_sprite_from_png[] = {
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_sprite_to_png[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_sprite_width[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_sprite_height[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_sprite_blank[] = {
  {"w", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"h", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_sprite_free[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_target[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_blit[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"dx", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"dy", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"sx", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"sy", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"sw", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"sh", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"flags", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_blit_scaled[] = {
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
};

inline constexpr CanonParam kCanonParams_Canvas_native_key[] = {
  {"name", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_title[] = {
  {"name", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_clipboard_set[] = {
  {"text", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_set_resizable[] = {
  {"enabled", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_set_target_fps[] = {
  {"fps", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_pad_available[] = {
  {"index", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_pad_axis[] = {
  {"index", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"axis", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_pad_button[] = {
  {"index", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"button", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_pad_pressed[] = {
  {"index", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"button", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_pad_name[] = {
  {"index", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_pad_rumble[] = {
  {"index", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"left", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"right", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"sec", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Canvas_native_pad_mappings[] = {
  {"db", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Canvas_native[] = {
  {"_Canvas", "", "init", kCanonParams_Canvas_native_init, ""},
  {"_Canvas", "", "ttf_load", kCanonParams_Canvas_native_ttf_load, "Long"},
  {"_Canvas", "", "ttf_free", kCanonParams_Canvas_native_ttf_free, ""},
  {"_Canvas", "", "ttf_glyph", kCanonParams_Canvas_native_ttf_glyph, "Long"},
  {"_Canvas", "", "ttf_glyph_screen", kCanonParams_Canvas_native_ttf_glyph_screen, "Long"},
  {"_Canvas", "", "ttf_advance", kCanonParams_Canvas_native_ttf_advance, "Long"},
  {"_Canvas", "", "ttf_ascent", kCanonParams_Canvas_native_ttf_ascent, "Long"},
  {"_Canvas", "", "screen_width", {}, "Long"},
  {"_Canvas", "", "screen_height", {}, "Long"},
  {"_Canvas", "", "get_screen_pixel", kCanonParams_Canvas_native_get_screen_pixel, "Long"},
  {"_Canvas", "", "screen_scale", {}, "Float"},
  {"_Canvas", "", "clear", kCanonParams_Canvas_native_clear, ""},
  {"_Canvas", "", "set_pixel", kCanonParams_Canvas_native_set_pixel, ""},
  {"_Canvas", "", "get_pixel", kCanonParams_Canvas_native_get_pixel, "Long"},
  {"_Canvas", "", "rect", kCanonParams_Canvas_native_rect, ""},
  {"_Canvas", "", "line", kCanonParams_Canvas_native_line, ""},
  {"_Canvas", "", "ellipse", kCanonParams_Canvas_native_ellipse, ""},
  {"_Canvas", "", "triangle", kCanonParams_Canvas_native_triangle, ""},
  {"_Canvas", "", "polygon", kCanonParams_Canvas_native_polygon, ""},
  {"_Canvas", "", "font_load", kCanonParams_Canvas_native_font_load, "Long"},
  {"_Canvas", "", "glyph", kCanonParams_Canvas_native_glyph, ""},
  {"_Canvas", "", "sprite_load", kCanonParams_Canvas_native_sprite_load, "Long"},
  {"_Canvas", "", "sprite_from_png", kCanonParams_Canvas_native_sprite_from_png, "Long"},
  {"_Canvas", "", "sprite_to_png", kCanonParams_Canvas_native_sprite_to_png, "String"},
  {"_Canvas", "", "sprite_width", kCanonParams_Canvas_native_sprite_width, "Long"},
  {"_Canvas", "", "sprite_height", kCanonParams_Canvas_native_sprite_height, "Long"},
  {"_Canvas", "", "sprite_blank", kCanonParams_Canvas_native_sprite_blank, "Long"},
  {"_Canvas", "", "sprite_free", kCanonParams_Canvas_native_sprite_free, ""},
  {"_Canvas", "", "target", kCanonParams_Canvas_native_target, "Long"},
  {"_Canvas", "", "blit", kCanonParams_Canvas_native_blit, ""},
  {"_Canvas", "", "blit_scaled", kCanonParams_Canvas_native_blit_scaled, ""},
  {"_Canvas", "", "present", {}, ""},
  {"_Canvas", "", "buttons", {}, "Long"},
  {"_Canvas", "", "mouse_x", {}, "Long"},
  {"_Canvas", "", "mouse_y", {}, "Long"},
  {"_Canvas", "", "mouse_buttons", {}, "Long"},
  {"_Canvas", "", "key", kCanonParams_Canvas_native_key, "Bool"},
  {"_Canvas", "", "key_pop", {}, "String"},
  {"_Canvas", "", "char_pop", {}, "String"},
  {"_Canvas", "", "closing", {}, "Bool"},
  {"_Canvas", "", "windowed", {}, "Bool"},
  {"_Canvas", "", "title", kCanonParams_Canvas_native_title, ""},
  {"_Canvas", "", "width", {}, "Long"},
  {"_Canvas", "", "height", {}, "Long"},
  {"_Canvas", "", "toggle_fullscreen", {}, ""},
  {"_Canvas", "", "is_fullscreen", {}, "Bool"},
  {"_Canvas", "", "show_cursor", {}, ""},
  {"_Canvas", "", "hide_cursor", {}, ""},
  {"_Canvas", "", "cursor_hidden", {}, "Bool"},
  {"_Canvas", "", "clipboard_get", {}, "String"},
  {"_Canvas", "", "clipboard_set", kCanonParams_Canvas_native_clipboard_set, ""},
  {"_Canvas", "", "set_resizable", kCanonParams_Canvas_native_set_resizable, ""},
  {"_Canvas", "", "window_resized", {}, "Bool"},
  {"_Canvas", "", "dt", {}, "Float"},
  {"_Canvas", "", "set_target_fps", kCanonParams_Canvas_native_set_target_fps, ""},
  {"_Canvas", "", "fps", {}, "Long"},
  {"_Canvas", "", "mouse_wheel", {}, "Float"},
  {"_Canvas", "", "pad_available", kCanonParams_Canvas_native_pad_available, "Bool"},
  {"_Canvas", "", "pad_axis", kCanonParams_Canvas_native_pad_axis, "Float"},
  {"_Canvas", "", "pad_button", kCanonParams_Canvas_native_pad_button, "Bool"},
  {"_Canvas", "", "pad_pressed", kCanonParams_Canvas_native_pad_pressed, "Bool"},
  {"_Canvas", "", "pad_name", kCanonParams_Canvas_native_pad_name, "String"},
  {"_Canvas", "", "pad_rumble", kCanonParams_Canvas_native_pad_rumble, ""},
  {"_Canvas", "", "pad_mappings", kCanonParams_Canvas_native_pad_mappings, "Long"},
  {"_Canvas", "", "quit", {}, ""},
  {"_Canvas", "", "can_quit", {}, "Bool"},
};

inline constexpr CanonParam kCanonParams_Audio_native_latency_ask[] = {
  {"seconds", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_tone[] = {
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
};

inline constexpr CanonParam kCanonParams_Audio_native_sound_load[] = {
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_sound_free[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_sound_play[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_sound_stop[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_sound_playing[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_sound_volume[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"v", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_sound_pitch[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"p", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_sound_pan[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"p", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_music_load[] = {
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"loop", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_music_free[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_music_play[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_music_stop[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_music_pause[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_music_resume[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_music_playing[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_music_seek[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"seconds", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_music_volume[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"v", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_music_pitch[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"p", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_music_pan[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"p", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_pcm_new[] = {
  {"rate", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"channels", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"buffer", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_pcm_free[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_pcm_ready[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_pcm_needed[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_pcm_push[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"samples", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_pcm_submit[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_pcm_latency[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_pcm_play[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_pcm_stop[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_pcm_pause[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_pcm_resume[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_pcm_playing[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_pcm_volume[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"v", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_pcm_pitch[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"p", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_pcm_pan[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"p", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_capture_new[] = {
  {"rate", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"channels", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_capture_free[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_capture_ready[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_capture_start[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_capture_stop[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_capture_running[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_capture_waiting[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_capture_read[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"frames", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_midi_ports[] = {
  {"output", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_midi_open[] = {
  {"port", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"sysex", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_midi_out_open[] = {
  {"port", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_midi_send[] = {
  {"output", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"bytes", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_midi_free[] = {
  {"input", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_midi_read[] = {
  {"input", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_midi_wait[] = {
  {"input", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"seconds", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_midi_control[] = {
  {"input", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"channel", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"cc", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_midi_dropped[] = {
  {"input", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_schedule_new[] = {
  {"events", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"length", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"loop_at", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_schedule_free[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_schedule_play[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_schedule_stop[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_schedule_volume[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"v", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_schedule_room[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"wet", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_resonate[] = {
  {"samples", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"modes", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"dry", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rate", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_pluck[] = {
  {"seconds", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"tunings", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"strikes", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"samples", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"rate", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_partials[] = {
  {"seconds", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"freq", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"gain", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"spectra", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"amps", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"band", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"steady", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"pulsed", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"rate", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_sound_samples[] = {
  {"samples", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"rate", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_filter[] = {
  {"samples", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"kind", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"cutoff", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"q", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rate", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_envelope[] = {
  {"samples", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"gain", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"rate", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_mix[] = {
  {"sounds", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"heads", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"rate", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_graph_new[] = {
  {"rate", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_graph_part[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"family", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"kind", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"inputs", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"modulators", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"options", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_graph_output[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"part", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_graph_schedule[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"changes", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_graph_render[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"seconds", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_graph_play[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_graph_stop[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_graph_free[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_graph_now[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_sound_loudness[] = {
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_loudness[] = {
  {"samples", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"rate", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_integrated_loudness[] = {
  {"samples", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"rate", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_tone_loudness[] = {
  {"freq", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_schedule_render[] = {
  {"events", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"sounds", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"length", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"wet", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"volume", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"from", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_decode[] = {
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_spectrum[] = {
  {"samples", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"size", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"hop", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_resample[] = {
  {"samples", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"from", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"to", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_pitch[] = {
  {"samples", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"low", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"high", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rate", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_harmonics[] = {
  {"samples", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"f0", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"count", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"rate", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_stft[] = {
  {"samples", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"size", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"hop", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Audio_native_istft[] = {
  {"re", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"im", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"size", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"hop", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Audio_native[] = {
  {"_Audio", "", "available", {}, "Bool"},
  {"_Audio", "", "latency_ask", kCanonParams_Audio_native_latency_ask, "Bool"},
  {"_Audio", "", "latency", {}, "Float?"},
  {"_Audio", "", "tone", kCanonParams_Audio_native_tone, ""},
  {"_Audio", "", "sound_load", kCanonParams_Audio_native_sound_load, "Long"},
  {"_Audio", "", "sound_free", kCanonParams_Audio_native_sound_free, ""},
  {"_Audio", "", "sound_play", kCanonParams_Audio_native_sound_play, ""},
  {"_Audio", "", "sound_stop", kCanonParams_Audio_native_sound_stop, ""},
  {"_Audio", "", "sound_playing", kCanonParams_Audio_native_sound_playing, "Bool"},
  {"_Audio", "", "sound_volume", kCanonParams_Audio_native_sound_volume, ""},
  {"_Audio", "", "sound_pitch", kCanonParams_Audio_native_sound_pitch, ""},
  {"_Audio", "", "sound_pan", kCanonParams_Audio_native_sound_pan, ""},
  {"_Audio", "", "music_load", kCanonParams_Audio_native_music_load, "Long"},
  {"_Audio", "", "music_free", kCanonParams_Audio_native_music_free, ""},
  {"_Audio", "", "music_play", kCanonParams_Audio_native_music_play, ""},
  {"_Audio", "", "music_stop", kCanonParams_Audio_native_music_stop, ""},
  {"_Audio", "", "music_pause", kCanonParams_Audio_native_music_pause, ""},
  {"_Audio", "", "music_resume", kCanonParams_Audio_native_music_resume, ""},
  {"_Audio", "", "music_playing", kCanonParams_Audio_native_music_playing, "Bool"},
  {"_Audio", "", "music_seek", kCanonParams_Audio_native_music_seek, ""},
  {"_Audio", "", "music_volume", kCanonParams_Audio_native_music_volume, ""},
  {"_Audio", "", "music_pitch", kCanonParams_Audio_native_music_pitch, ""},
  {"_Audio", "", "music_pan", kCanonParams_Audio_native_music_pan, ""},
  {"_Audio", "", "pcm_new", kCanonParams_Audio_native_pcm_new, "Long"},
  {"_Audio", "", "pcm_free", kCanonParams_Audio_native_pcm_free, ""},
  {"_Audio", "", "pcm_ready", kCanonParams_Audio_native_pcm_ready, "Bool"},
  {"_Audio", "", "pcm_needed", kCanonParams_Audio_native_pcm_needed, "Long"},
  {"_Audio", "", "pcm_push", kCanonParams_Audio_native_pcm_push, "Long"},
  {"_Audio", "", "pcm_submit", kCanonParams_Audio_native_pcm_submit, "Long"},
  {"_Audio", "", "pcm_latency", kCanonParams_Audio_native_pcm_latency, "Float"},
  {"_Audio", "", "pcm_play", kCanonParams_Audio_native_pcm_play, ""},
  {"_Audio", "", "pcm_stop", kCanonParams_Audio_native_pcm_stop, ""},
  {"_Audio", "", "pcm_pause", kCanonParams_Audio_native_pcm_pause, ""},
  {"_Audio", "", "pcm_resume", kCanonParams_Audio_native_pcm_resume, ""},
  {"_Audio", "", "pcm_playing", kCanonParams_Audio_native_pcm_playing, "Bool"},
  {"_Audio", "", "pcm_volume", kCanonParams_Audio_native_pcm_volume, ""},
  {"_Audio", "", "pcm_pitch", kCanonParams_Audio_native_pcm_pitch, ""},
  {"_Audio", "", "pcm_pan", kCanonParams_Audio_native_pcm_pan, ""},
  {"_Audio", "", "capture_present", {}, "Bool"},
  {"_Audio", "", "capture_new", kCanonParams_Audio_native_capture_new, "Long"},
  {"_Audio", "", "capture_free", kCanonParams_Audio_native_capture_free, ""},
  {"_Audio", "", "capture_ready", kCanonParams_Audio_native_capture_ready, "Bool"},
  {"_Audio", "", "capture_start", kCanonParams_Audio_native_capture_start, ""},
  {"_Audio", "", "capture_stop", kCanonParams_Audio_native_capture_stop, ""},
  {"_Audio", "", "capture_running", kCanonParams_Audio_native_capture_running, "Bool"},
  {"_Audio", "", "capture_waiting", kCanonParams_Audio_native_capture_waiting, "Long"},
  {"_Audio", "", "capture_read", kCanonParams_Audio_native_capture_read, "Array"},
  {"_Audio", "", "midi_ports", kCanonParams_Audio_native_midi_ports, "Array"},
  {"_Audio", "", "midi_open", kCanonParams_Audio_native_midi_open, "Long"},
  {"_Audio", "", "midi_out_open", kCanonParams_Audio_native_midi_out_open, "Long"},
  {"_Audio", "", "midi_send", kCanonParams_Audio_native_midi_send, "String"},
  {"_Audio", "", "midi_free", kCanonParams_Audio_native_midi_free, ""},
  {"_Audio", "", "midi_read", kCanonParams_Audio_native_midi_read, "Array"},
  {"_Audio", "", "midi_wait", kCanonParams_Audio_native_midi_wait, "Bool"},
  {"_Audio", "", "midi_control", kCanonParams_Audio_native_midi_control, "Long"},
  {"_Audio", "", "midi_dropped", kCanonParams_Audio_native_midi_dropped, "Long"},
  {"_Audio", "", "schedule_stride", {}, "Long"},
  {"_Audio", "", "schedule_new", kCanonParams_Audio_native_schedule_new, "Long"},
  {"_Audio", "", "schedule_free", kCanonParams_Audio_native_schedule_free, ""},
  {"_Audio", "", "schedule_play", kCanonParams_Audio_native_schedule_play, ""},
  {"_Audio", "", "schedule_stop", kCanonParams_Audio_native_schedule_stop, ""},
  {"_Audio", "", "schedule_volume", kCanonParams_Audio_native_schedule_volume, ""},
  {"_Audio", "", "schedule_room", kCanonParams_Audio_native_schedule_room, ""},
  {"_Audio", "", "resonate", kCanonParams_Audio_native_resonate, "Array"},
  {"_Audio", "", "pluck", kCanonParams_Audio_native_pluck, "Array"},
  {"_Audio", "", "partials", kCanonParams_Audio_native_partials, "Array"},
  {"_Audio", "", "sound_samples", kCanonParams_Audio_native_sound_samples, "Array"},
  {"_Audio", "", "filter", kCanonParams_Audio_native_filter, "Array"},
  {"_Audio", "", "envelope", kCanonParams_Audio_native_envelope, "Array"},
  {"_Audio", "", "mix", kCanonParams_Audio_native_mix, "Array"},
  {"_Audio", "", "graph_new", kCanonParams_Audio_native_graph_new, "Long"},
  {"_Audio", "", "graph_part", kCanonParams_Audio_native_graph_part, "Long"},
  {"_Audio", "", "graph_output", kCanonParams_Audio_native_graph_output, ""},
  {"_Audio", "", "graph_schedule", kCanonParams_Audio_native_graph_schedule, ""},
  {"_Audio", "", "graph_render", kCanonParams_Audio_native_graph_render, "Array"},
  {"_Audio", "", "graph_play", kCanonParams_Audio_native_graph_play, ""},
  {"_Audio", "", "graph_stop", kCanonParams_Audio_native_graph_stop, ""},
  {"_Audio", "", "graph_free", kCanonParams_Audio_native_graph_free, ""},
  {"_Audio", "", "graph_now", kCanonParams_Audio_native_graph_now, "Float"},
  {"_Audio", "", "sound_loudness", kCanonParams_Audio_native_sound_loudness, "Float?"},
  {"_Audio", "", "loudness", kCanonParams_Audio_native_loudness, "Float?"},
  {"_Audio", "", "integrated_loudness", kCanonParams_Audio_native_integrated_loudness, "Float?"},
  {"_Audio", "", "tone_loudness", kCanonParams_Audio_native_tone_loudness, "Float?"},
  {"_Audio", "", "schedule_render", kCanonParams_Audio_native_schedule_render, "Array"},
  {"_Audio", "", "decode", kCanonParams_Audio_native_decode, "Array"},
  {"_Audio", "", "spectrum", kCanonParams_Audio_native_spectrum, "Array"},
  {"_Audio", "", "resample", kCanonParams_Audio_native_resample, "Array"},
  {"_Audio", "", "pitch", kCanonParams_Audio_native_pitch, "Array"},
  {"_Audio", "", "harmonics", kCanonParams_Audio_native_harmonics, "Array"},
  {"_Audio", "", "stft", kCanonParams_Audio_native_stft, "Array"},
  {"_Audio", "", "istft", kCanonParams_Audio_native_istft, "Array"},
};

inline constexpr CanonParam kCanonParams_Dir_native_normalize[] = {
  {"path", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Dir_native_mark[] = {
  {"dir", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"kind", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Dir_native_embedded_read[] = {
  {"name", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"path", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Dir_native_embedded_list_dir[] = {
  {"name", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"path", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Dir_native_embedded_is_file[] = {
  {"name", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"path", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Dir_native_embedded_is_dir[] = {
  {"name", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"path", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Dir_native_embedded_size[] = {
  {"name", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"path", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Dir_native_disk_read[] = {
  {"real", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"path", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Dir_native_disk_list_dir[] = {
  {"real", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"path", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Dir_native_disk_is_file[] = {
  {"real", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"path", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Dir_native_disk_is_dir[] = {
  {"real", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"path", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Dir_native_disk_size[] = {
  {"real", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"path", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Dir_native_disk_files[] = {
  {"real", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Dir_native_zip_open[] = {
  {"path", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Dir_native_zip_open_bytes[] = {
  {"bytes", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Dir_native_zip_read[] = {
  {"archive", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"path", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Dir_native_zip_is_file[] = {
  {"archive", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"path", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Dir_native_zip_is_dir[] = {
  {"archive", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"path", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Dir_native_zip_list_dir[] = {
  {"archive", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"path", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Dir_native_zip_size[] = {
  {"archive", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"path", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Dir_native_zip_files[] = {
  {"archive", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Dir_native_zip_close[] = {
  {"id", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Dir_native[] = {
  {"_Dir", "", "normalize", kCanonParams_Dir_native_normalize, ""},
  {"_Dir", "", "mark", kCanonParams_Dir_native_mark, ""},
  {"_Dir", "", "embedded_read", kCanonParams_Dir_native_embedded_read, ""},
  {"_Dir", "", "embedded_list_dir", kCanonParams_Dir_native_embedded_list_dir, ""},
  {"_Dir", "", "embedded_is_file", kCanonParams_Dir_native_embedded_is_file, "Bool"},
  {"_Dir", "", "embedded_is_dir", kCanonParams_Dir_native_embedded_is_dir, "Bool"},
  {"_Dir", "", "embedded_size", kCanonParams_Dir_native_embedded_size, ""},
  {"_Dir", "", "disk_read", kCanonParams_Dir_native_disk_read, ""},
  {"_Dir", "", "disk_list_dir", kCanonParams_Dir_native_disk_list_dir, ""},
  {"_Dir", "", "disk_is_file", kCanonParams_Dir_native_disk_is_file, "Bool"},
  {"_Dir", "", "disk_is_dir", kCanonParams_Dir_native_disk_is_dir, "Bool"},
  {"_Dir", "", "disk_size", kCanonParams_Dir_native_disk_size, ""},
  {"_Dir", "", "disk_files", kCanonParams_Dir_native_disk_files, ""},
  {"_Dir", "", "zip_open", kCanonParams_Dir_native_zip_open, "Long"},
  {"_Dir", "", "zip_open_bytes", kCanonParams_Dir_native_zip_open_bytes, "Long"},
  {"_Dir", "", "zip_read", kCanonParams_Dir_native_zip_read, ""},
  {"_Dir", "", "zip_is_file", kCanonParams_Dir_native_zip_is_file, "Bool"},
  {"_Dir", "", "zip_is_dir", kCanonParams_Dir_native_zip_is_dir, "Bool"},
  {"_Dir", "", "zip_list_dir", kCanonParams_Dir_native_zip_list_dir, ""},
  {"_Dir", "", "zip_size", kCanonParams_Dir_native_zip_size, ""},
  {"_Dir", "", "zip_files", kCanonParams_Dir_native_zip_files, ""},
  {"_Dir", "", "zip_close", kCanonParams_Dir_native_zip_close, ""},
};

inline constexpr CanonParam kCanonParams_Bare_inspect[] = {
  {"x", false, false, false, false, true, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Bare_print[] = {
  {"x", false, false, false, false, true, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Bare_println[] = {
  {"x", true, false, false, false, true, "", CanonDefault::Str, 0, ""},
};

inline constexpr CanonParam kCanonParams_Bare_type_of[] = {
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Bare_class_of[] = {
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Bare_to_long[] = {
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"base", true, true, false, false, false, "Long", CanonDefault::Long, 10, {}},
};

inline constexpr CanonParam kCanonParams_Bare_to_float[] = {
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Bare_to_string[] = {
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Bare_hash[] = {
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Bare___eff_copy[] = {
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Bare___eff_recapture[] = {
  {"frames", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"old_boxes", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"new_boxes", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Bare___eff_abort[] = {
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Bare___eff_catch_abort[] = {
  {"fn", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Bare___for_iter[] = {
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Bare___raise[] = {
  {"kind", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"message", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Bare___raise_at[] = {
  {"kind", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"message", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"at", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Bare_range[] = {
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
  {"step", true, true, false, false, false, "", CanonDefault::Long, 1, {}},
};

inline constexpr CanonParam kCanonParams_Bare_iota[] = {
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Bare_repeat[] = {
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"value", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Bare_grid[] = {
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Bare_Range[] = {
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_Bare_assert_true[] = {
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"label", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonParam kCanonParams_Bare_assert_false[] = {
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"label", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonParam kCanonParams_Bare_assert_eq[] = {
  {"a", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"b", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"label", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonParam kCanonParams_Bare_assert_ne[] = {
  {"a", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"b", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"label", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonParam kCanonParams_Bare_assert_lt[] = {
  {"a", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"b", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"label", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonParam kCanonParams_Bare_assert_le[] = {
  {"a", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"b", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"label", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonParam kCanonParams_Bare_assert_gt[] = {
  {"a", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"b", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"label", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonParam kCanonParams_Bare_assert_ge[] = {
  {"a", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"b", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"label", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonParam kCanonParams_Bare_assert_close[] = {
  {"a", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"b", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"tol", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"label", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
};

inline constexpr CanonSig kCanonSigs_Bare[] = {
  {"", "", "inspect", kCanonParams_Bare_inspect, ""},
  {"", "", "print", kCanonParams_Bare_print, ""},
  {"", "", "println", kCanonParams_Bare_println, ""},
  {"", "", "type_of", kCanonParams_Bare_type_of, "String"},
  {"", "", "class_of", kCanonParams_Bare_class_of, ""},
  {"", "", "to_long", kCanonParams_Bare_to_long, "Long"},
  {"", "", "to_float", kCanonParams_Bare_to_float, "Float"},
  {"", "", "to_string", kCanonParams_Bare_to_string, "String"},
  {"", "", "hash", kCanonParams_Bare_hash, "Long"},
  {"", "", "__eff_copy", kCanonParams_Bare___eff_copy, "Object"},
  {"", "", "__eff_recapture", kCanonParams_Bare___eff_recapture, ""},
  {"", "", "__eff_abort", kCanonParams_Bare___eff_abort, ""},
  {"", "", "__eff_catch_abort", kCanonParams_Bare___eff_catch_abort, "Array"},
  {"", "", "__for_iter", kCanonParams_Bare___for_iter, "Object"},
  {"", "", "__raise", kCanonParams_Bare___raise, ""},
  {"", "", "__raise_at", kCanonParams_Bare___raise_at, ""},
  {"", "", "range", kCanonParams_Bare_range, ""},
  {"", "", "iota", kCanonParams_Bare_iota, ""},
  {"", "", "repeat", kCanonParams_Bare_repeat, "Array"},
  {"", "", "grid", kCanonParams_Bare_grid, ""},
  {"", "", "Range", kCanonParams_Bare_Range, "Range"},
  {"", "", "assert_true", kCanonParams_Bare_assert_true, "Nil"},
  {"", "", "assert_false", kCanonParams_Bare_assert_false, "Nil"},
  {"", "", "assert_eq", kCanonParams_Bare_assert_eq, "Nil"},
  {"", "", "assert_ne", kCanonParams_Bare_assert_ne, "Nil"},
  {"", "", "assert_lt", kCanonParams_Bare_assert_lt, "Nil"},
  {"", "", "assert_le", kCanonParams_Bare_assert_le, "Nil"},
  {"", "", "assert_gt", kCanonParams_Bare_assert_gt, "Nil"},
  {"", "", "assert_ge", kCanonParams_Bare_assert_ge, "Nil"},
  {"", "", "assert_close", kCanonParams_Bare_assert_close, "Nil"},
};

inline constexpr CanonParam kCanonParams_ObjectMethod_get[] = {
  {"key", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"fallback", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_ObjectMethod_get_or_put[] = {
  {"key", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"init", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_ObjectMethod_has[] = {
  {"key", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_ObjectMethod_remove[] = {
  {"key", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonObjectSigs[] = {
  {"", "", "empty", {}, "Bool"},
  {"", "", "get", kCanonParams_ObjectMethod_get, "Any"},
  {"", "", "get_or_put", kCanonParams_ObjectMethod_get_or_put, "Any"},
  {"", "", "has", kCanonParams_ObjectMethod_has, "Bool"},
  {"", "", "iter", {}, ""},
  {"", "", "keys", {}, "Array"},
  {"", "", "presence", {}, "Object | Nil"},
  {"", "", "remove", kCanonParams_ObjectMethod_remove, "Nil"},
  {"", "", "size", {}, "Long"},
  {"", "", "values", {}, "Iterator"},
};

inline constexpr CanonParam kCanonParams_ArrayMethod_all[] = {
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_ArrayMethod_any[] = {
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_ArrayMethod_contains[] = {
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_ArrayMethod_extend[] = {
  {"other", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_ArrayMethod_filter[] = {
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_ArrayMethod_find[] = {
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_ArrayMethod_flat_map[] = {
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_ArrayMethod_for_each[] = {
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_ArrayMethod_get[] = {
  {"i", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"fallback", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_ArrayMethod_group_by[] = {
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_ArrayMethod_index_of[] = {
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_ArrayMethod_insert[] = {
  {"i", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_ArrayMethod_join[] = {
  {"sep", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_ArrayMethod_map[] = {
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_ArrayMethod_max_by[] = {
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_ArrayMethod_min_by[] = {
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_ArrayMethod_partition[] = {
  {"p", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_ArrayMethod_push[] = {
  {"arg", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_ArrayMethod_reduce[] = {
  {"init", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_ArrayMethod_remove_at[] = {
  {"i", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_ArrayMethod_slice[] = {
  {"start", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"end", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_ArrayMethod_sort[] = {
  {"reverse", true, true, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
};

inline constexpr CanonParam kCanonParams_ArrayMethod_sort_by[] = {
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  {"reverse", true, true, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
};

inline constexpr CanonParam kCanonParams_ArrayMethod_sorted[] = {
  {"reverse", true, true, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
};

inline constexpr CanonParam kCanonParams_ArrayMethod_sorted_by[] = {
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  {"reverse", true, true, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
};

inline constexpr CanonSig kCanonArraySigs[] = {
  {"", "", "all", kCanonParams_ArrayMethod_all, "Bool"},
  {"", "", "any", kCanonParams_ArrayMethod_any, "Bool"},
  {"", "", "contains", kCanonParams_ArrayMethod_contains, "Bool"},
  {"", "", "empty", {}, "Bool"},
  {"", "", "enumerate", {}, ""},
  {"", "", "extend", kCanonParams_ArrayMethod_extend, "Nil"},
  {"", "", "filter", kCanonParams_ArrayMethod_filter, "Array"},
  {"", "", "find", kCanonParams_ArrayMethod_find, "Any"},
  {"", "", "flat_map", kCanonParams_ArrayMethod_flat_map, "Array"},
  {"", "", "for_each", kCanonParams_ArrayMethod_for_each, "Nil"},
  {"", "", "get", kCanonParams_ArrayMethod_get, "Any"},
  {"", "", "group_by", kCanonParams_ArrayMethod_group_by, "Object"},
  {"", "", "index_of", kCanonParams_ArrayMethod_index_of, "Long"},
  {"", "", "insert", kCanonParams_ArrayMethod_insert, "Nil"},
  {"", "", "iter", {}, ""},
  {"", "", "join", kCanonParams_ArrayMethod_join, "String"},
  {"", "", "map", kCanonParams_ArrayMethod_map, "Array"},
  {"", "", "max", {}, "Any"},
  {"", "", "max_by", kCanonParams_ArrayMethod_max_by, "Any"},
  {"", "", "min", {}, "Any"},
  {"", "", "min_by", kCanonParams_ArrayMethod_min_by, "Any"},
  {"", "", "partition", kCanonParams_ArrayMethod_partition, "Tuple"},
  {"", "", "pop", {}, "Any"},
  {"", "", "presence", {}, "Array | Nil"},
  {"", "", "product", {}, "Long | Float"},
  {"", "", "push", kCanonParams_ArrayMethod_push, "Nil"},
  {"", "", "reduce", kCanonParams_ArrayMethod_reduce, "Any"},
  {"", "", "remove_at", kCanonParams_ArrayMethod_remove_at, "Any"},
  {"", "", "reverse", {}, "Nil"},
  {"", "", "size", {}, "Long"},
  {"", "", "slice", kCanonParams_ArrayMethod_slice, "Array"},
  {"", "", "sort", kCanonParams_ArrayMethod_sort, "Nil"},
  {"", "", "sort_by", kCanonParams_ArrayMethod_sort_by, "Nil"},
  {"", "", "sorted", kCanonParams_ArrayMethod_sorted, "Array"},
  {"", "", "sorted_by", kCanonParams_ArrayMethod_sorted_by, "Array"},
  {"", "", "sum", {}, "Long | Float"},
  {"", "", "to_object", {}, "Object"},
  {"", "", "to_set", {}, "Set"},
  {"", "", "unzip", {}, "Tuple"},
};

inline constexpr CanonParam kCanonParams_StringMethod_contains[] = {
  {"sub", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_StringMethod_count[] = {
  {"sub", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_StringMethod_ends_with[] = {
  {"suffix", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_StringMethod_eq_ignore_case[] = {
  {"other", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_StringMethod_index_of[] = {
  {"sub", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"start", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
};

inline constexpr CanonParam kCanonParams_StringMethod_last_index_of[] = {
  {"sub", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_StringMethod_normalize[] = {
  {"form", true, false, false, false, false, "StringLike", CanonDefault::Str, 0, "NFC"},
};

inline constexpr CanonParam kCanonParams_StringMethod_repeat[] = {
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_StringMethod_rsplit[] = {
  {"sep", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"limit", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
};

inline constexpr CanonParam kCanonParams_StringMethod_slice[] = {
  {"start", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"end", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_StringMethod_split[] = {
  {"sep", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"limit", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
};

inline constexpr CanonParam kCanonParams_StringMethod_split_iter[] = {
  {"sep", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_StringMethod_starts_with[] = {
  {"prefix", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_StringMethod_strip_prefix[] = {
  {"prefix", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_StringMethod_strip_suffix[] = {
  {"suffix", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_StringMethod_tr[] = {
  {"from", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"to", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_StringMethod_trim_end[] = {
  {"chars", true, false, false, false, false, "StringLike", CanonDefault::Str, 0, ""},
};

inline constexpr CanonParam kCanonParams_StringMethod_trim_start[] = {
  {"chars", true, false, false, false, false, "StringLike", CanonDefault::Str, 0, ""},
};

inline constexpr CanonParam kCanonParams_StringMethod_truncate[] = {
  {"max", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"ellipsis", true, false, false, false, false, "StringLike", CanonDefault::Str, 0, "..."},
};

inline constexpr CanonSig kCanonStringSigs[] = {
  {"", "", "bytes", {}, "Iterator<Long>"},
  {"", "", "capitalize", {}, "String"},
  {"", "", "code_points", {}, "Iterator<Long>"},
  {"", "", "contains", kCanonParams_StringMethod_contains, "Bool"},
  {"", "", "count", kCanonParams_StringMethod_count, "Long"},
  {"", "", "empty", {}, "Bool"},
  {"", "", "ends_with", kCanonParams_StringMethod_ends_with, "Bool"},
  {"", "", "eq_ignore_case", kCanonParams_StringMethod_eq_ignore_case, "Bool"},
  {"", "", "graphemes", {}, "Iterator<StringView>"},
  {"", "", "index_of", kCanonParams_StringMethod_index_of, "Long"},
  {"", "", "is_alnum", {}, "Bool"},
  {"", "", "is_alpha", {}, "Bool"},
  {"", "", "is_ascii", {}, "Bool"},
  {"", "", "is_digit", {}, "Bool"},
  {"", "", "is_space", {}, "Bool"},
  {"", "", "iter", {}, "Iterator<StringView>"},
  {"", "", "last_index_of", kCanonParams_StringMethod_last_index_of, "Long"},
  {"", "", "lines", {}, "Array<StringView>"},
  {"", "", "lower", {}, "String"},
  {"", "", "normalize", kCanonParams_StringMethod_normalize, "String"},
  {"", "", "presence", {}, "String | StringView | Nil"},
  {"", "", "repeat", kCanonParams_StringMethod_repeat, "String"},
  {"", "", "reverse", {}, "String"},
  {"", "", "rsplit", kCanonParams_StringMethod_rsplit, "Array<StringView>"},
  {"", "", "sentences", {}, "Iterator<StringView>"},
  {"", "", "size", {}, "Long"},
  {"", "", "slice", kCanonParams_StringMethod_slice, "StringView"},
  {"", "", "split", kCanonParams_StringMethod_split, "Array<StringView>"},
  {"", "", "split_iter", kCanonParams_StringMethod_split_iter, "Iterator<StringView>"},
  {"", "", "split_whitespace", {}, "Array<StringView>"},
  {"", "", "starts_with", kCanonParams_StringMethod_starts_with, "Bool"},
  {"", "", "strip_prefix", kCanonParams_StringMethod_strip_prefix, "String"},
  {"", "", "strip_suffix", kCanonParams_StringMethod_strip_suffix, "String"},
  {"", "", "title", {}, "String"},
  {"", "", "to_string", {}, "String"},
  {"", "", "tr", kCanonParams_StringMethod_tr, "String"},
  {"", "", "trim", {}, "String"},
  {"", "", "trim_end", kCanonParams_StringMethod_trim_end, "String"},
  {"", "", "trim_start", kCanonParams_StringMethod_trim_start, "String"},
  {"", "", "truncate", kCanonParams_StringMethod_truncate, "String"},
  {"", "", "upper", {}, "String"},
  {"", "", "view", {}, "StringView"},
  {"", "", "words", {}, "Iterator<Tuple>"},
};

inline constexpr CanonParam kCanonParams_SetMethod_add[] = {
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_SetMethod_contains[] = {
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_SetMethod_diff[] = {
  {"other", false, false, false, false, false, "Set", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_SetMethod_intersect[] = {
  {"other", false, false, false, false, false, "Set", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_SetMethod_remove[] = {
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_SetMethod_subset[] = {
  {"other", false, false, false, false, false, "Set", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_SetMethod_superset[] = {
  {"other", false, false, false, false, false, "Set", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_SetMethod_sym_diff[] = {
  {"other", false, false, false, false, false, "Set", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_SetMethod_union[] = {
  {"other", false, false, false, false, false, "Set", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonSetSigs[] = {
  {"", "", "add", kCanonParams_SetMethod_add, "Bool"},
  {"", "", "contains", kCanonParams_SetMethod_contains, "Bool"},
  {"", "", "diff", kCanonParams_SetMethod_diff, "Set"},
  {"", "", "empty", {}, "Bool"},
  {"", "", "intersect", kCanonParams_SetMethod_intersect, "Set"},
  {"", "", "iter", {}, ""},
  {"", "", "presence", {}, ""},
  {"", "", "remove", kCanonParams_SetMethod_remove, "Bool"},
  {"", "", "size", {}, ""},
  {"", "", "subset", kCanonParams_SetMethod_subset, "Bool"},
  {"", "", "superset", kCanonParams_SetMethod_superset, "Bool"},
  {"", "", "sym_diff", kCanonParams_SetMethod_sym_diff, "Set"},
  {"", "", "to_array", {}, "Array"},
  {"", "", "union", kCanonParams_SetMethod_union, "Set"},
};

inline constexpr CanonParam kCanonParams_TupleMethod_contains[] = {
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonTupleSigs[] = {
  {"", "", "contains", kCanonParams_TupleMethod_contains, "Bool"},
  {"", "", "empty", {}, ""},
  {"", "", "iter", {}, ""},
  {"", "", "presence", {}, ""},
  {"", "", "size", {}, ""},
  {"", "", "to_array", {}, "Array"},
};

inline constexpr CanonParam kCanonParams_TensorMethod_argmax[] = {
  {"axis", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"keepdims", true, true, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
};

inline constexpr CanonParam kCanonParams_TensorMethod_causal_attention[] = {
  {"k", false, false, false, false, false, "Tensor", CanonDefault::None, 0, {}},
  {"v", false, false, false, false, false, "Tensor", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_TensorMethod_clamp[] = {
  {"lo", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"hi", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_TensorMethod_dot[] = {
  {"other", false, false, false, false, false, "Tensor", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_TensorMethod_eq[] = {
  {"other", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_TensorMethod_fold[] = {
  {"params", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_TensorMethod_ge[] = {
  {"other", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_TensorMethod_gt[] = {
  {"other", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_TensorMethod_index_select[] = {
  {"indices", false, false, false, false, false, "Tensor", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_TensorMethod_layer_norm[] = {
  {"gamma", false, false, false, false, false, "Tensor", CanonDefault::None, 0, {}},
  {"beta", false, false, false, false, false, "Tensor", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_TensorMethod_le[] = {
  {"other", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_TensorMethod_linear_sigmoid[] = {
  {"x", false, false, false, false, false, "Tensor", CanonDefault::None, 0, {}},
  {"b", false, false, false, false, false, "Tensor", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_TensorMethod_lt[] = {
  {"other", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_TensorMethod_max[] = {
  {"axis", true, false, false, false, false, "Long?", CanonDefault::Nil, 0, {}},
  {"keepdims", true, true, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
};

inline constexpr CanonParam kCanonParams_TensorMethod_mean[] = {
  {"axis", true, false, false, false, false, "Long?", CanonDefault::Nil, 0, {}},
  {"keepdims", true, true, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
};

inline constexpr CanonParam kCanonParams_TensorMethod_narrow[] = {
  {"params", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_TensorMethod_ne[] = {
  {"other", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_TensorMethod_pad[] = {
  {"params", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_TensorMethod_permute[] = {
  {"axes", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_TensorMethod_pow[] = {
  {"exp", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_TensorMethod_reshape[] = {
  {"dims", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_TensorMethod_rope[] = {
  {"pos", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"base", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_TensorMethod_slice[] = {
  {"start", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"end", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_TensorMethod_softmax_cross_entropy[] = {
  {"targets", false, false, false, false, false, "Tensor", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_TensorMethod_sum[] = {
  {"axis", true, false, false, false, false, "Long?", CanonDefault::Nil, 0, {}},
  {"keepdims", true, true, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
};

inline constexpr CanonParam kCanonParams_TensorMethod_unfold[] = {
  {"params", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonTensorSigs[] = {
  {"", "", "argmax", kCanonParams_TensorMethod_argmax, "Tensor"},
  {"", "", "backward", {}, "Tensor"},
  {"", "", "causal_attention", kCanonParams_TensorMethod_causal_attention, "Tensor"},
  {"", "", "clamp", kCanonParams_TensorMethod_clamp, "Tensor"},
  {"", "", "clone", {}, "Tensor"},
  {"", "", "cos", {}, "Tensor"},
  {"", "", "detach", {}, "Tensor"},
  {"", "", "dot", kCanonParams_TensorMethod_dot, "Tensor"},
  {"", "", "eq", kCanonParams_TensorMethod_eq, "Tensor"},
  {"", "", "fold", kCanonParams_TensorMethod_fold, "Tensor"},
  {"", "", "ge", kCanonParams_TensorMethod_ge, "Tensor"},
  {"", "", "grad", {}, "Tensor"},
  {"", "", "gt", kCanonParams_TensorMethod_gt, "Tensor"},
  {"", "", "index_select", kCanonParams_TensorMethod_index_select, "Tensor"},
  {"", "", "item", {}, "Float"},
  {"", "", "layer_norm", kCanonParams_TensorMethod_layer_norm, "Tensor"},
  {"", "", "le", kCanonParams_TensorMethod_le, "Tensor"},
  {"", "", "linear_sigmoid", kCanonParams_TensorMethod_linear_sigmoid, "Tensor"},
  {"", "", "log", {}, "Tensor"},
  {"", "", "lt", kCanonParams_TensorMethod_lt, "Tensor"},
  {"", "", "max", kCanonParams_TensorMethod_max, ""},
  {"", "", "mean", kCanonParams_TensorMethod_mean, ""},
  {"", "", "narrow", kCanonParams_TensorMethod_narrow, "Tensor"},
  {"", "", "ne", kCanonParams_TensorMethod_ne, "Tensor"},
  {"", "", "pad", kCanonParams_TensorMethod_pad, "Tensor"},
  {"", "", "permute", kCanonParams_TensorMethod_permute, "Tensor"},
  {"", "", "pow", kCanonParams_TensorMethod_pow, "Tensor"},
  {"", "", "relu", {}, "Tensor"},
  {"", "", "requires_grad", {}, "Tensor"},
  {"", "", "reshape", kCanonParams_TensorMethod_reshape, "Tensor"},
  {"", "", "rope", kCanonParams_TensorMethod_rope, "Tensor"},
  {"", "", "shape", {}, "Array"},
  {"", "", "sigmoid", {}, "Tensor"},
  {"", "", "sin", {}, "Tensor"},
  {"", "", "slice", kCanonParams_TensorMethod_slice, "Tensor"},
  {"", "", "softmax", {}, "Tensor"},
  {"", "", "softmax_cross_entropy", kCanonParams_TensorMethod_softmax_cross_entropy, "Tensor"},
  {"", "", "sum", kCanonParams_TensorMethod_sum, ""},
  {"", "", "tanh", {}, "Tensor"},
  {"", "", "to_array", {}, "Array"},
  {"", "", "transpose", {}, "Tensor"},
  {"", "", "unfold", kCanonParams_TensorMethod_unfold, "Tensor"},
  {"", "", "zero_grad", {}, "Tensor"},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_all[] = {
  {"p", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_any[] = {
  {"p", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_chain[] = {
  {"other", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_chunk_by[] = {
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_chunks[] = {
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_contains[] = {
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_filter[] = {
  {"p", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_find[] = {
  {"p", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_flat_map[] = {
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_for_each[] = {
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_group_by[] = {
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_join[] = {
  {"sep", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_map[] = {
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_max_by[] = {
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_min_by[] = {
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_nth[] = {
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_partition[] = {
  {"p", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_position[] = {
  {"p", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_reduce[] = {
  {"init", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_scan[] = {
  {"init", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_skip[] = {
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_skip_while[] = {
  {"p", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_step_by[] = {
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_take[] = {
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_take_while[] = {
  {"p", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_tap[] = {
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_windows[] = {
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
};

inline constexpr CanonParam kCanonParams_IteratorMethod_zip[] = {
  {"other", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonIteratorSigs[] = {
  {"", "", "all", kCanonParams_IteratorMethod_all, ""},
  {"", "", "any", kCanonParams_IteratorMethod_any, ""},
  {"", "", "chain", kCanonParams_IteratorMethod_chain, ""},
  {"", "", "chunk_by", kCanonParams_IteratorMethod_chunk_by, ""},
  {"", "", "chunks", kCanonParams_IteratorMethod_chunks, ""},
  {"", "", "collect", {}, ""},
  {"", "", "contains", kCanonParams_IteratorMethod_contains, ""},
  {"", "", "count", {}, ""},
  {"", "", "distinct", {}, ""},
  {"", "", "enumerate", {}, ""},
  {"", "", "filter", kCanonParams_IteratorMethod_filter, ""},
  {"", "", "find", kCanonParams_IteratorMethod_find, ""},
  {"", "", "first", {}, ""},
  {"", "", "flat_map", kCanonParams_IteratorMethod_flat_map, ""},
  {"", "", "flatten", {}, ""},
  {"", "", "for_each", kCanonParams_IteratorMethod_for_each, ""},
  {"", "", "group_by", kCanonParams_IteratorMethod_group_by, ""},
  {"", "", "join", kCanonParams_IteratorMethod_join, ""},
  {"", "", "last", {}, ""},
  {"", "", "map", kCanonParams_IteratorMethod_map, ""},
  {"", "", "max", {}, ""},
  {"", "", "max_by", kCanonParams_IteratorMethod_max_by, ""},
  {"", "", "min", {}, ""},
  {"", "", "min_by", kCanonParams_IteratorMethod_min_by, ""},
  {"", "", "nth", kCanonParams_IteratorMethod_nth, ""},
  {"", "", "partition", kCanonParams_IteratorMethod_partition, ""},
  {"", "", "position", kCanonParams_IteratorMethod_position, ""},
  {"", "", "product", {}, ""},
  {"", "", "reduce", kCanonParams_IteratorMethod_reduce, ""},
  {"", "", "scan", kCanonParams_IteratorMethod_scan, ""},
  {"", "", "skip", kCanonParams_IteratorMethod_skip, ""},
  {"", "", "skip_while", kCanonParams_IteratorMethod_skip_while, ""},
  {"", "", "step_by", kCanonParams_IteratorMethod_step_by, ""},
  {"", "", "sum", {}, ""},
  {"", "", "take", kCanonParams_IteratorMethod_take, ""},
  {"", "", "take_while", kCanonParams_IteratorMethod_take_while, ""},
  {"", "", "tap", kCanonParams_IteratorMethod_tap, ""},
  {"", "", "to_object", {}, ""},
  {"", "", "to_set", {}, ""},
  {"", "", "unzip", {}, ""},
  {"", "", "windows", kCanonParams_IteratorMethod_windows, ""},
  {"", "", "zip", kCanonParams_IteratorMethod_zip, ""},
};

}  // namespace culebra
// clang-format on
