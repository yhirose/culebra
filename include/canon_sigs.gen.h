#pragma once
// Canonical native/built-in signature data — the single source of truth
// since the tree-walker's environment retired (Phase 4 B7-f; this file was
// generated from it by tools/gen_canon_sigs.cc while both existed). A
// signature change edits this table by hand — see canon_sigs.h for the
// structs and lookup helpers.
// clang-format off

namespace culebra {

inline constexpr CanonParam kCanonParamPool[] = {
  // 0: Embed.dir
  {"name", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 1: IO.inspect
  {"arg", false, false, false, false, true, "", CanonDefault::None, 0, {}},
  // 2: IO.print
  {"arg", false, false, false, false, true, "", CanonDefault::None, 0, {}},
  // 3: IO.println
  {"arg", true, false, false, false, true, "", CanonDefault::Str, 0, ""},
  // 4: IO.einspect
  {"arg", false, false, false, false, true, "", CanonDefault::None, 0, {}},
  // 5: IO.eprint
  {"arg", false, false, false, false, true, "", CanonDefault::None, 0, {}},
  // 6: IO.eprintln
  {"arg", false, false, false, false, true, "", CanonDefault::None, 0, {}},
  // 7: Math.abs
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 8: Math.min
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
  // 9: Math.max
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
  // 10: Math.pow
  {"base", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"exp", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 12: Math.sign
  {"x", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 13: Math.clamp
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"lo", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"hi", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 16: Math.wrap
  {"x", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 18: Math.log
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 19: Math.exp
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 20: Math.sqrt
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 21: Math.sin
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 22: Math.cos
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 23: Math.tan
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 24: Math.asin
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 25: Math.acos
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 26: Math.atan
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 27: Math.atan2
  {"y", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 29: Math.floor
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 30: Math.ceil
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 31: Math.round
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 32: FS.join
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
  // 33: FS.basename
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 34: FS.dirname
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 35: FS.extension
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 36: FS.stem
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 37: FS.exists
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 38: FS.is_file
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 39: FS.is_dir
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 40: FS.read
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 41: FS.write
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"content", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 43: FS.size
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 44: FS.list_dir
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 45: FS.mkdir
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 46: FS.remove
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"recursive", true, false, false, false, false, "", CanonDefault::Bool, 0, {}},
  // 48: FS.stat
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 49: FS.chmod
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"mode", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 51: FS.chown
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"owner", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"group", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  // 54: FS.rename
  {"src", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"dst", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 56: FS.copy
  {"src", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"dst", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"recursive", true, false, false, false, false, "", CanonDefault::Bool, 0, {}},
  // 59: FS.normpath
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 60: FS.is_abs
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 61: FS.abspath
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 62: FS.realpath
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 63: FS.is_symlink
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 64: FS.symlink
  {"target", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"link", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 66: FS.readlink
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 67: FS.walk
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  // 68: FS.glob
  {"pattern", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 69: FS.watch
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"recursive", true, false, false, false, false, "", CanonDefault::Bool, 1, {}},
  {"match", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  // 72: File.open
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"mode", true, false, false, false, false, "", CanonDefault::Str, 0, "r"},
  // 74: File.with
  {"path", false, false, false, false, false, "String|Path", CanonDefault::None, 0, {}},
  {"mode", true, false, false, false, false, "", CanonDefault::Str, 0, "r"},
  {"fn", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 77: Random.seed
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 78: Random.int
  {"lo", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"hi", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 80: Random.uniform
  {"lo", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"hi", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 82: Random.gauss
  {"mu", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"sigma", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 84: Random.shuffle
  {"a", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  // 85: Random.weighted_choice
  {"pop", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"weights", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  // 87: Random.choice
  {"pop", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  // 88: Sys.exit
  {"code", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 89: Sys.env
  {"name", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"fallback", true, false, false, false, false, "", CanonDefault::Str, 0, ""},
  // 91: Sys.chdir
  {"path", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 92: Sys.set_env
  {"name", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"value", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 94: Sys.data_dir
  {"app", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 95: _Regex.check
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 96: _Regex.test
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 98: _Regex.find
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 100: _Regex.match
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 102: _Regex.find_from
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"pos", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 105: _Regex.find_all
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 107: _Regex.find_all_str
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 109: _Regex.find_all_index
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 111: _Regex.count
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 113: _Regex.replace_all
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"repl", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 116: _Regex.replace_first
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"repl", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 119: _Regex.split
  {"pattern", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 121: Net.connect
  {"host", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"port", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"timeout", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  // 124: Net.listen
  {"port", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"host", true, false, false, false, false, "String", CanonDefault::Str, 0, "0.0.0.0"},
  {"backlog", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  // 127: Net.udp
  {"port", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  {"host", true, false, false, false, false, "String", CanonDefault::Str, 0, "0.0.0.0"},
  // 129: Net.resolve
  {"host", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 130: Proc.run
  {"cmd", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"cwd", true, false, false, false, false, "String?", CanonDefault::Nil, 0, {}},
  {"env", true, false, false, false, false, "Object?", CanonDefault::Nil, 0, {}},
  {"stdin", true, false, false, false, false, "String", CanonDefault::Str, 0, ""},
  {"check", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  {"timeout", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  {"share", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  // 137: Proc.all
  {"commands", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"limit", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  {"timeout", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  {"fail_fast", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  {"retries", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  {"share", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  // 143: Proc.race
  {"commands", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"share", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  // 145: Proc.spawn
  {"cmd", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"cwd", true, false, false, false, false, "String?", CanonDefault::Nil, 0, {}},
  {"env", true, false, false, false, false, "Object?", CanonDefault::Nil, 0, {}},
  {"stdin", true, false, false, false, false, "String", CanonDefault::Str, 0, ""},
  {"share", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  // 150: Http.get
  {"url", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"headers", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"timeout", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"follow_redirects", true, false, false, false, false, "", CanonDefault::Bool, 1, {}},
  {"into", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"params", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  // 156: Http.delete
  {"url", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"headers", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"timeout", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"follow_redirects", true, false, false, false, false, "", CanonDefault::Bool, 1, {}},
  {"into", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"params", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  // 162: Http.head
  {"url", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"headers", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"timeout", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"follow_redirects", true, false, false, false, false, "", CanonDefault::Bool, 1, {}},
  {"into", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"params", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  // 168: Http.post
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
  // 179: Http.put
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
  // 190: Http.request
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
  // 202: Http.sse
  {"url", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"on_event", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  {"headers", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"timeout", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"follow_redirects", true, false, false, false, false, "", CanonDefault::Bool, 1, {}},
  // 207: Http.client
  {"base_url", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"headers", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  {"timeout", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"follow_redirects", true, false, false, false, false, "", CanonDefault::Bool, 1, {}},
  // 211: Http.ws
  {"url", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 212: Isolate.spawn
  {"fn", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
  // 214: Channel.new
  {"cap", true, false, false, false, false, "", CanonDefault::Long, 1, {}},
  // 215: Channel.fan_in
  {"a", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"fn", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  // 217: Signal.notify
  {"tx", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 218: SharedBuffer.new
  {"count", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"type", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 220: Shared.new
  {"value", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 221: SharedBuffer.file
  {"path", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"count", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"type", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 224: SharedBuffer.shared
  {"count", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"type", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 226: SharedBuffer.receive
  {"name", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"type", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 228: Parallel.map
  {"items", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"fn", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"limit", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"on_progress", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  // 232: Parallel.each
  {"items", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"fn", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"limit", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"on_progress", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  // 236: Parallel.map_settled
  {"items", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"fn", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"limit", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"on_progress", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  // 240: Parallel.race
  {"items", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"fn", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"limit", true, false, false, false, false, "", CanonDefault::Long, 0, {}},
  {"on_progress", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  // 244: JSON.stringify
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"indent", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  {"sort_keys", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  {"lines", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  // 248: JSON.parse
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"lines", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  {"number_mode", true, false, false, false, false, "String", CanonDefault::Str, 0, "auto"},
  {"jsonc", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  // 252: Encoding.html.escape
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 253: Encoding.html.unescape
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 254: Encoding.base64.encode
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 255: Encoding.base64.decode
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 256: Encoding.hex.encode
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 257: Encoding.hex.decode
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 258: Encoding.url.encode
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 259: Encoding.url.decode
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 260: Compress.gzip
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 261: Compress.gunzip
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 262: Compress.deflate
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"level", true, false, false, false, false, "Long", CanonDefault::Long, -1, {}},
  // 264: Hash.sha256
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 265: Hash.sha1
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 266: Hash.sha512
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 267: Hash.md5
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 268: Hash.hmac_sha256
  {"key", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 270: Hash.hmac_sha1
  {"key", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 272: Hash.hmac_sha512
  {"key", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 274: CSV.parse
  {"text", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"delimiter", true, false, false, false, false, "String", CanonDefault::Str, 0, ","},
  {"header", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  {"types", true, false, false, false, false, "", CanonDefault::Nil, 0, {}},
  // 278: CSV.stringify
  {"rows", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"delimiter", true, false, false, false, false, "String", CanonDefault::Str, 0, ","},
  // 280: SQLite.open
  {"path", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 281: TOML.parse
  {"text", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 282: TOML.stringify
  {"v", false, false, false, false, false, "Object", CanonDefault::None, 0, {}},
  {"sort_keys", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  // 284: Env.parse
  {"text", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 285: Env.load
  {"path", true, false, false, false, false, "String", CanonDefault::Str, 0, ".env"},
  {"override", true, false, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  // 287: String.from_code_point
  {"cp", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 288: String.from_bytes
  {"bytes", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  // 289: String.from_code_points
  {"cps", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  // 290: Tensor.zeros
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
  // 291: Tensor.ones
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
  // 292: Tensor.randn
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
  // 293: Tensor.eval
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
  // 294: Tensor.from_csv
  {"path", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 295: Tensor.from
  {"a", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  // 296: Tensor.concat
  {"parts", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  // 297: Tensor.no_grad
  {"fn", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 298: _Time.sleep
  {"secs", false, false, false, false, false, "Float", CanonDefault::None, 0, {}},
  // 299: _Time.from_iso_nanos
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 300: _Time.parse_nanos
  {"s", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"fmt", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 302: _Time.iso_nanos
  {"nanos", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"utc", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
  // 304: _Time.format_nanos
  {"nanos", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"fmt", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"utc", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
  // 307: _Time.parts_nanos
  {"nanos", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"utc", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
  // 309: _Time.from_parts_nanos
  {"p", false, false, false, false, false, "Object", CanonDefault::None, 0, {}},
  {"utc", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
  // 311: _Time.weekday_nanos
  {"nanos", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"utc", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
  // 313: _Time.add_nanos
  {"nanos", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"years", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"months", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"days", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"hours", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"minutes", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"seconds", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"utc", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
  // 321: _Time.start_of_nanos
  {"nanos", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"unit", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"utc", false, false, false, false, false, "Bool", CanonDefault::None, 0, {}},
  // 324: _Term.width
  {"s", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 325: _Term.read_key
  {"timeout", false, false, false, false, false, "Float", CanonDefault::None, 0, {}},
  // 326: _Canvas.init
  {"w", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"h", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 328: _Canvas.ttf_load
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 329: _Canvas.ttf_free
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 330: _Canvas.ttf_glyph
  {"font", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"codepoint", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"x", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"size", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 336: _Canvas.ttf_glyph_screen
  {"font", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"codepoint", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"x", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"size", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 342: _Canvas.ttf_advance
  {"font", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"codepoint", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"size", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 345: _Canvas.ttf_ascent
  {"font", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"size", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 347: _Canvas.get_screen_pixel
  {"x", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  // 349: _Canvas.clear
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 350: _Canvas.set_pixel
  {"x", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 353: _Canvas.get_pixel
  {"x", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  // 355: _Canvas.rect
  {"x", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"w", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"h", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"fill", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 361: _Canvas.line
  {"x1", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y1", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"x2", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y2", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 366: _Canvas.ellipse
  {"cx", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"cy", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rx", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"ry", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"fill", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 372: _Canvas.triangle
  {"x1", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y1", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"x2", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y2", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"x3", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y3", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"fill", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 380: _Canvas.polygon
  {"points", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"fill", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 383: _Canvas.font_load
  {"rows", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  // 384: _Canvas.glyph
  {"font", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"index", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"x", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"y", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"scale", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 390: _Canvas.sprite_load
  {"pixels", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  {"w", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"h", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 393: _Canvas.sprite_from_png
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 394: _Canvas.sprite_to_png
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 395: _Canvas.sprite_width
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 396: _Canvas.sprite_height
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 397: _Canvas.sprite_blank
  {"w", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"h", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"rgba", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 400: _Canvas.sprite_free
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 401: _Canvas.target
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 402: _Canvas.blit
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"dx", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"dy", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"sx", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"sy", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"sw", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"sh", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  {"flags", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 410: _Canvas.blit_scaled
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
  // 421: _Canvas.key
  {"name", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 422: _Canvas.title
  {"name", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 423: _Canvas.tone
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
  // 433: _Canvas.music_play
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  {"loop", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"vol", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"start", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  // 437: _Canvas.music_volume
  {"vol", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 438: _Canvas.music_seek
  {"seconds", false, false, false, false, false, "Long|Float", CanonDefault::None, 0, {}},
  // 439: _Canvas.sound_load
  {"data", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 440: _Canvas.sound_play
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"vol", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 442: _Canvas.sound_stop
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 443: _Canvas.sound_playing
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 444: _Canvas.sound_free
  {"id", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 445: inspect
  {"arg", false, false, false, false, true, "", CanonDefault::None, 0, {}},
  // 446: print
  {"arg", false, false, false, false, true, "", CanonDefault::None, 0, {}},
  // 447: println
  {"arg", true, false, false, false, true, "", CanonDefault::Str, 0, ""},
  // 448: type_of
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 449: to_long
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"base", true, true, false, false, false, "Long", CanonDefault::Long, 10, {}},
  // 451: to_float
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 452: to_string
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 453: hash
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 454: __eff_copy
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 455: __eff_abort
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 456: __eff_catch_abort
  {"fn", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 457: range
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
  {"step", true, true, false, false, false, "", CanonDefault::Long, 1, {}},
  // 459: iota
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
  // 460: repeat
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"value", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 462: grid
  {"args", false, false, false, true, false, "", CanonDefault::None, 0, {}},
  // 463: get
  {"key", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"fallback", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 465: get_or_put
  {"key", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"init", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 467: has
  {"key", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 468: remove
  {"key", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 469: all
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 470: any
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 471: contains
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 472: extend
  {"other", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  // 473: filter
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 474: find
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 475: flat_map
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 476: for_each
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 477: get
  {"i", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"fallback", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 479: group_by
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 480: index_of
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 481: insert
  {"i", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 483: join
  {"sep", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 484: map
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 485: max_by
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 486: min_by
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 487: partition
  {"p", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 488: push
  {"arg", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 489: reduce
  {"init", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 491: remove_at
  {"i", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 492: slice
  {"start", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"end", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 494: sort
  {"reverse", true, true, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  // 495: sort_by
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  {"reverse", true, true, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  // 497: sorted
  {"reverse", true, true, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  // 498: sorted_by
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  {"reverse", true, true, false, false, false, "Bool", CanonDefault::Bool, 0, {}},
  // 500: contains
  {"sub", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 501: count
  {"sub", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 502: ends_with
  {"suffix", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 503: eq_ignore_case
  {"other", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 504: index_of
  {"sub", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"start", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  // 506: last_index_of
  {"sub", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 507: normalize
  {"form", true, false, false, false, false, "StringLike", CanonDefault::Str, 0, "NFC"},
  // 508: repeat
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 509: rsplit
  {"sep", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"limit", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  // 511: slice
  {"start", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"end", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 513: split
  {"sep", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"limit", true, false, false, false, false, "Long", CanonDefault::Long, 0, {}},
  // 515: split_iter
  {"sep", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 516: starts_with
  {"prefix", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 517: strip_prefix
  {"prefix", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 518: strip_suffix
  {"suffix", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 519: tr
  {"from", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  {"to", false, false, false, false, false, "StringLike", CanonDefault::None, 0, {}},
  // 521: trim_end
  {"chars", true, false, false, false, false, "StringLike", CanonDefault::Str, 0, ""},
  // 522: trim_start
  {"chars", true, false, false, false, false, "StringLike", CanonDefault::Str, 0, ""},
  // 523: truncate
  {"max", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"ellipsis", true, false, false, false, false, "StringLike", CanonDefault::Str, 0, "..."},
  // 525: add
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 526: contains
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 527: diff
  {"other", false, false, false, false, false, "Set", CanonDefault::None, 0, {}},
  // 528: intersect
  {"other", false, false, false, false, false, "Set", CanonDefault::None, 0, {}},
  // 529: remove
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 530: subset
  {"other", false, false, false, false, false, "Set", CanonDefault::None, 0, {}},
  // 531: superset
  {"other", false, false, false, false, false, "Set", CanonDefault::None, 0, {}},
  // 532: sym_diff
  {"other", false, false, false, false, false, "Set", CanonDefault::None, 0, {}},
  // 533: union
  {"other", false, false, false, false, false, "Set", CanonDefault::None, 0, {}},
  // 534: contains
  {"x", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 535: argmax
  {"axis", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 536: dot
  {"other", false, false, false, false, false, "Tensor", CanonDefault::None, 0, {}},
  // 537: linear_sigmoid
  {"x", false, false, false, false, false, "Tensor", CanonDefault::None, 0, {}},
  {"b", false, false, false, false, false, "Tensor", CanonDefault::None, 0, {}},
  // 539: max
  {"axis", true, false, false, false, false, "Long?", CanonDefault::Nil, 0, {}},
  // 540: mean
  {"axis", true, false, false, false, false, "Long?", CanonDefault::Nil, 0, {}},
  // 541: pow
  {"exp", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 542: reshape
  {"dims", false, false, false, false, false, "Array", CanonDefault::None, 0, {}},
  // 543: slice
  {"start", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  {"end", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 545: sum
  {"axis", true, false, false, false, false, "Long?", CanonDefault::Nil, 0, {}},
  // 546: all
  {"p", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 547: any
  {"p", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 548: chain
  {"other", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 549: chunk_by
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 550: chunks
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 551: contains
  {"v", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  // 552: filter
  {"p", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 553: find
  {"p", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 554: flat_map
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 555: for_each
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 556: group_by
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 557: join
  {"sep", false, false, false, false, false, "String", CanonDefault::None, 0, {}},
  // 558: map
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 559: max_by
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 560: min_by
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 561: nth
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 562: partition
  {"p", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 563: position
  {"p", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 564: reduce
  {"init", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 566: scan
  {"init", false, false, false, false, false, "", CanonDefault::None, 0, {}},
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 568: skip
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 569: skip_while
  {"p", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 570: step_by
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 571: take
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 572: take_while
  {"p", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 573: tap
  {"f", false, false, false, false, false, "Function", CanonDefault::None, 0, {}},
  // 574: windows
  {"n", false, false, false, false, false, "Long", CanonDefault::None, 0, {}},
  // 575: zip
  {"other", false, false, false, false, false, "", CanonDefault::None, 0, {}},
};

inline constexpr CanonSig kCanonNsSigs[] = {
  {"Embed", "", "dir", kCanonParamPool + 0, 1, "Object", 1, 1, false, -1, -1, -1},
  {"IO", "", "inspect", kCanonParamPool + 1, 1, "", 1, 1, false, -1, -1, -1},
  {"IO", "", "print", kCanonParamPool + 2, 1, "", 1, 1, false, -1, -1, -1},
  {"IO", "", "println", kCanonParamPool + 3, 1, "", 0, 1, false, -1, -1, -1},
  {"IO", "", "input", nullptr, 0, "String", 0, 0, false, -1, -1, -1},
  {"IO", "", "stdin", nullptr, 0, "Object", 0, 0, false, -1, -1, -1},
  {"IO", "", "einspect", kCanonParamPool + 4, 1, "", 1, 1, false, -1, -1, -1},
  {"IO", "", "eprint", kCanonParamPool + 5, 1, "", 1, 1, false, -1, -1, -1},
  {"IO", "", "eprintln", kCanonParamPool + 6, 1, "", 1, 1, false, -1, -1, -1},
  {"IO", "", "stdin_is_terminal", nullptr, 0, "Bool", 0, 0, false, -1, -1, -1},
  {"IO", "", "stdout_is_terminal", nullptr, 0, "Bool", 0, 0, false, -1, -1, -1},
  {"IO", "", "stderr_is_terminal", nullptr, 0, "Bool", 0, 0, false, -1, -1, -1},
  {"Math", "", "abs", kCanonParamPool + 7, 1, "", 1, 1, false, -1, -1, -1},
  {"Math", "", "min", kCanonParamPool + 8, 1, "", 0, 0, true, -1, -1, 0},
  {"Math", "", "max", kCanonParamPool + 9, 1, "", 0, 0, true, -1, -1, 0},
  {"Math", "", "pow", kCanonParamPool + 10, 2, "Long", 2, 2, false, -1, -1, -1},
  {"Math", "", "sign", kCanonParamPool + 12, 1, "Long", 1, 1, false, -1, -1, -1},
  {"Math", "", "clamp", kCanonParamPool + 13, 3, "", 3, 3, false, -1, -1, -1},
  {"Math", "", "wrap", kCanonParamPool + 16, 2, "Long", 2, 2, false, -1, -1, -1},
  {"Math", "", "log", kCanonParamPool + 18, 1, "Float", 1, 1, false, -1, -1, -1},
  {"Math", "", "exp", kCanonParamPool + 19, 1, "Float", 1, 1, false, -1, -1, -1},
  {"Math", "", "sqrt", kCanonParamPool + 20, 1, "Float", 1, 1, false, -1, -1, -1},
  {"Math", "", "sin", kCanonParamPool + 21, 1, "Float", 1, 1, false, -1, -1, -1},
  {"Math", "", "cos", kCanonParamPool + 22, 1, "Float", 1, 1, false, -1, -1, -1},
  {"Math", "", "tan", kCanonParamPool + 23, 1, "Float", 1, 1, false, -1, -1, -1},
  {"Math", "", "asin", kCanonParamPool + 24, 1, "Float", 1, 1, false, -1, -1, -1},
  {"Math", "", "acos", kCanonParamPool + 25, 1, "Float", 1, 1, false, -1, -1, -1},
  {"Math", "", "atan", kCanonParamPool + 26, 1, "Float", 1, 1, false, -1, -1, -1},
  {"Math", "", "atan2", kCanonParamPool + 27, 2, "Float", 2, 2, false, -1, -1, -1},
  {"Math", "", "floor", kCanonParamPool + 29, 1, "Long", 1, 1, false, -1, -1, -1},
  {"Math", "", "ceil", kCanonParamPool + 30, 1, "Long", 1, 1, false, -1, -1, -1},
  {"Math", "", "round", kCanonParamPool + 31, 1, "Long", 1, 1, false, -1, -1, -1},
  {"FS", "", "join", kCanonParamPool + 32, 1, "", 0, 0, true, -1, -1, 0},
  {"FS", "", "basename", kCanonParamPool + 33, 1, "String", 1, 1, false, -1, -1, -1},
  {"FS", "", "dirname", kCanonParamPool + 34, 1, "String", 1, 1, false, -1, -1, -1},
  {"FS", "", "extension", kCanonParamPool + 35, 1, "String", 1, 1, false, -1, -1, -1},
  {"FS", "", "stem", kCanonParamPool + 36, 1, "String", 1, 1, false, -1, -1, -1},
  {"FS", "", "exists", kCanonParamPool + 37, 1, "Bool", 1, 1, false, -1, -1, -1},
  {"FS", "", "is_file", kCanonParamPool + 38, 1, "Bool", 1, 1, false, -1, -1, -1},
  {"FS", "", "is_dir", kCanonParamPool + 39, 1, "Bool", 1, 1, false, -1, -1, -1},
  {"FS", "", "read", kCanonParamPool + 40, 1, "String", 1, 1, false, -1, -1, -1},
  {"FS", "", "write", kCanonParamPool + 41, 2, "", 2, 2, false, -1, -1, -1},
  {"FS", "", "size", kCanonParamPool + 43, 1, "Long", 1, 1, false, -1, -1, -1},
  {"FS", "", "list_dir", kCanonParamPool + 44, 1, "Array", 1, 1, false, -1, -1, -1},
  {"FS", "", "mkdir", kCanonParamPool + 45, 1, "", 1, 1, false, -1, -1, -1},
  {"FS", "", "remove", kCanonParamPool + 46, 2, "", 1, 2, false, -1, -1, -1},
  {"FS", "", "stat", kCanonParamPool + 48, 1, "Object", 1, 1, false, -1, -1, -1},
  {"FS", "", "chmod", kCanonParamPool + 49, 2, "", 2, 2, false, -1, -1, -1},
  {"FS", "", "chown", kCanonParamPool + 51, 3, "", 1, 3, false, -1, -1, -1},
  {"FS", "", "rename", kCanonParamPool + 54, 2, "", 2, 2, false, -1, -1, -1},
  {"FS", "", "copy", kCanonParamPool + 56, 3, "", 2, 3, false, -1, -1, -1},
  {"FS", "", "normpath", kCanonParamPool + 59, 1, "String", 1, 1, false, -1, -1, -1},
  {"FS", "", "is_abs", kCanonParamPool + 60, 1, "Bool", 1, 1, false, -1, -1, -1},
  {"FS", "", "abspath", kCanonParamPool + 61, 1, "String", 1, 1, false, -1, -1, -1},
  {"FS", "", "realpath", kCanonParamPool + 62, 1, "String", 1, 1, false, -1, -1, -1},
  {"FS", "", "is_symlink", kCanonParamPool + 63, 1, "Bool", 1, 1, false, -1, -1, -1},
  {"FS", "", "symlink", kCanonParamPool + 64, 2, "", 2, 2, false, -1, -1, -1},
  {"FS", "", "readlink", kCanonParamPool + 66, 1, "String", 1, 1, false, -1, -1, -1},
  {"FS", "", "walk", kCanonParamPool + 67, 1, "Array", 1, 1, false, -1, -1, -1},
  {"FS", "", "glob", kCanonParamPool + 68, 1, "Array", 1, 1, false, -1, -1, -1},
  {"FS", "", "watch", kCanonParamPool + 69, 3, "Object", 1, 3, false, -1, -1, -1},
  {"File", "", "open", kCanonParamPool + 72, 2, "Object", 1, 2, false, -1, -1, -1},
  {"File", "", "with", kCanonParamPool + 74, 3, "", 2, 3, false, -1, -1, -1},
  {"Random", "", "seed", kCanonParamPool + 77, 1, "", 1, 1, false, -1, -1, -1},
  {"Random", "", "int", kCanonParamPool + 78, 2, "Long", 2, 2, false, -1, -1, -1},
  {"Random", "", "uniform", kCanonParamPool + 80, 2, "Float", 2, 2, false, -1, -1, -1},
  {"Random", "", "gauss", kCanonParamPool + 82, 2, "Float", 2, 2, false, -1, -1, -1},
  {"Random", "", "shuffle", kCanonParamPool + 84, 1, "", 1, 1, false, -1, -1, -1},
  {"Random", "", "weighted_choice", kCanonParamPool + 85, 2, "", 2, 2, false, -1, -1, -1},
  {"Random", "", "choice", kCanonParamPool + 87, 1, "", 1, 1, false, -1, -1, -1},
  {"Sys", "", "exit", kCanonParamPool + 88, 1, "", 1, 1, false, -1, -1, -1},
  {"Sys", "", "env", kCanonParamPool + 89, 2, "", 1, 2, false, -1, -1, -1},
  {"Sys", "", "getcwd", nullptr, 0, "String", 0, 0, false, -1, -1, -1},
  {"Sys", "", "chdir", kCanonParamPool + 91, 1, "", 1, 1, false, -1, -1, -1},
  {"Sys", "", "set_env", kCanonParamPool + 92, 2, "", 2, 2, false, -1, -1, -1},
  {"Sys", "", "data_dir", kCanonParamPool + 94, 1, "String", 1, 1, false, -1, -1, -1},
  {"Sys", "", "time", nullptr, 0, "Float", 0, 0, false, -1, -1, -1},
  {"GC", "", "stat", nullptr, 0, "Object", 0, 0, false, -1, -1, -1},
  {"_Regex", "", "check", kCanonParamPool + 95, 1, "", 1, 1, false, -1, -1, -1},
  {"_Regex", "", "test", kCanonParamPool + 96, 2, "Bool", 2, 2, false, -1, -1, -1},
  {"_Regex", "", "find", kCanonParamPool + 98, 2, "", 2, 2, false, -1, -1, -1},
  {"_Regex", "", "match", kCanonParamPool + 100, 2, "", 2, 2, false, -1, -1, -1},
  {"_Regex", "", "find_from", kCanonParamPool + 102, 3, "", 3, 3, false, -1, -1, -1},
  {"_Regex", "", "find_all", kCanonParamPool + 105, 2, "", 2, 2, false, -1, -1, -1},
  {"_Regex", "", "find_all_str", kCanonParamPool + 107, 2, "", 2, 2, false, -1, -1, -1},
  {"_Regex", "", "find_all_index", kCanonParamPool + 109, 2, "", 2, 2, false, -1, -1, -1},
  {"_Regex", "", "count", kCanonParamPool + 111, 2, "Long", 2, 2, false, -1, -1, -1},
  {"_Regex", "", "replace_all", kCanonParamPool + 113, 3, "String", 3, 3, false, -1, -1, -1},
  {"_Regex", "", "replace_first", kCanonParamPool + 116, 3, "String", 3, 3, false, -1, -1, -1},
  {"_Regex", "", "split", kCanonParamPool + 119, 2, "", 2, 2, false, -1, -1, -1},
  {"Net", "", "connect", kCanonParamPool + 121, 3, "Object", 2, 3, false, -1, -1, -1},
  {"Net", "", "listen", kCanonParamPool + 124, 3, "Object", 1, 3, false, -1, -1, -1},
  {"Net", "", "udp", kCanonParamPool + 127, 2, "Object", 0, 2, false, -1, -1, -1},
  {"Net", "", "resolve", kCanonParamPool + 129, 1, "Array", 1, 1, false, -1, -1, -1},
  {"Proc", "", "run", kCanonParamPool + 130, 7, "Object", 1, 7, false, -1, -1, -1},
  {"Proc", "", "all", kCanonParamPool + 137, 6, "Array", 1, 6, false, -1, -1, -1},
  {"Proc", "", "race", kCanonParamPool + 143, 2, "Object", 1, 2, false, -1, -1, -1},
  {"Proc", "", "spawn", kCanonParamPool + 145, 5, "Object", 1, 5, false, -1, -1, -1},
  {"Http", "", "get", kCanonParamPool + 150, 6, "Object", 1, 6, false, -1, -1, -1},
  {"Http", "", "delete", kCanonParamPool + 156, 6, "Object", 1, 6, false, -1, -1, -1},
  {"Http", "", "head", kCanonParamPool + 162, 6, "Object", 1, 6, false, -1, -1, -1},
  {"Http", "", "post", kCanonParamPool + 168, 11, "Object", 1, 11, false, -1, -1, -1},
  {"Http", "", "put", kCanonParamPool + 179, 11, "Object", 1, 11, false, -1, -1, -1},
  {"Http", "", "request", kCanonParamPool + 190, 12, "Object", 2, 12, false, -1, -1, -1},
  {"Http", "", "sse", kCanonParamPool + 202, 5, "Object", 2, 5, false, -1, -1, -1},
  {"Http", "", "client", kCanonParamPool + 207, 4, "Object", 1, 4, false, -1, -1, -1},
  {"Http", "", "server", nullptr, 0, "Object", 0, 0, false, -1, -1, -1},
  {"Http", "", "ws", kCanonParamPool + 211, 1, "Object", 1, 1, false, -1, -1, -1},
  {"Isolate", "", "spawn", kCanonParamPool + 212, 2, "Object", 1, 1, true, -1, -1, 1},
  {"Channel", "", "new", kCanonParamPool + 214, 1, "Tuple", 0, 1, false, -1, -1, -1},
  {"Channel", "", "fan_in", kCanonParamPool + 215, 2, "", 1, 2, false, -1, -1, -1},
  {"Signal", "", "notify", kCanonParamPool + 217, 1, "Nil", 1, 1, false, -1, -1, -1},
  {"Signal", "", "reset", nullptr, 0, "Nil", 0, 0, false, -1, -1, -1},
  {"SharedBuffer", "", "new", kCanonParamPool + 218, 2, "Object", 2, 2, false, -1, -1, -1},
  {"Shared", "", "new", kCanonParamPool + 220, 1, "", 1, 1, false, -1, -1, -1},
  {"SharedBuffer", "", "file", kCanonParamPool + 221, 3, "Object", 3, 3, false, -1, -1, -1},
  {"SharedBuffer", "", "shared", kCanonParamPool + 224, 2, "Object", 2, 2, false, -1, -1, -1},
  {"SharedBuffer", "", "receive", kCanonParamPool + 226, 2, "Object", 2, 2, false, -1, -1, -1},
  {"Parallel", "", "map", kCanonParamPool + 228, 4, "Array", 2, 4, false, -1, -1, -1},
  {"Parallel", "", "each", kCanonParamPool + 232, 4, "Nil", 2, 4, false, -1, -1, -1},
  {"Parallel", "", "map_settled", kCanonParamPool + 236, 4, "Array", 2, 4, false, -1, -1, -1},
  {"Parallel", "", "race", kCanonParamPool + 240, 4, "Any", 2, 4, false, -1, -1, -1},
  {"JSON", "", "stringify", kCanonParamPool + 244, 4, "String", 1, 4, false, -1, -1, -1},
  {"JSON", "", "parse", kCanonParamPool + 248, 4, "", 1, 4, false, -1, -1, -1},
  {"Encoding", "html", "escape", kCanonParamPool + 252, 1, "String", 1, 1, false, -1, -1, -1},
  {"Encoding", "html", "unescape", kCanonParamPool + 253, 1, "String", 1, 1, false, -1, -1, -1},
  {"Encoding", "base64", "encode", kCanonParamPool + 254, 1, "String", 1, 1, false, -1, -1, -1},
  {"Encoding", "base64", "decode", kCanonParamPool + 255, 1, "String", 1, 1, false, -1, -1, -1},
  {"Encoding", "hex", "encode", kCanonParamPool + 256, 1, "String", 1, 1, false, -1, -1, -1},
  {"Encoding", "hex", "decode", kCanonParamPool + 257, 1, "String", 1, 1, false, -1, -1, -1},
  {"Encoding", "url", "encode", kCanonParamPool + 258, 1, "String", 1, 1, false, -1, -1, -1},
  {"Encoding", "url", "decode", kCanonParamPool + 259, 1, "String", 1, 1, false, -1, -1, -1},
  {"Compress", "", "gzip", kCanonParamPool + 260, 1, "String", 1, 1, false, -1, -1, -1},
  {"Compress", "", "gunzip", kCanonParamPool + 261, 1, "String", 1, 1, false, -1, -1, -1},
  {"Compress", "", "deflate", kCanonParamPool + 262, 2, "String", 1, 2, false, -1, -1, -1},
  {"Hash", "", "sha256", kCanonParamPool + 264, 1, "String", 1, 1, false, -1, -1, -1},
  {"Hash", "", "sha1", kCanonParamPool + 265, 1, "String", 1, 1, false, -1, -1, -1},
  {"Hash", "", "sha512", kCanonParamPool + 266, 1, "String", 1, 1, false, -1, -1, -1},
  {"Hash", "", "md5", kCanonParamPool + 267, 1, "String", 1, 1, false, -1, -1, -1},
  {"Hash", "", "hmac_sha256", kCanonParamPool + 268, 2, "String", 2, 2, false, -1, -1, -1},
  {"Hash", "", "hmac_sha1", kCanonParamPool + 270, 2, "String", 2, 2, false, -1, -1, -1},
  {"Hash", "", "hmac_sha512", kCanonParamPool + 272, 2, "String", 2, 2, false, -1, -1, -1},
  {"CSV", "", "parse", kCanonParamPool + 274, 4, "Array", 1, 4, false, -1, -1, -1},
  {"CSV", "", "stringify", kCanonParamPool + 278, 2, "String", 1, 2, false, -1, -1, -1},
  {"SQLite", "", "open", kCanonParamPool + 280, 1, "Object", 1, 1, false, -1, -1, -1},
  {"SQLite", "", "version", nullptr, 0, "String", 0, 0, false, -1, -1, -1},
  {"TOML", "", "parse", kCanonParamPool + 281, 1, "Object", 1, 1, false, -1, -1, -1},
  {"TOML", "", "stringify", kCanonParamPool + 282, 2, "String", 1, 2, false, -1, -1, -1},
  {"Env", "", "parse", kCanonParamPool + 284, 1, "Object", 1, 1, false, -1, -1, -1},
  {"Env", "", "load", kCanonParamPool + 285, 2, "Object", 0, 2, false, -1, -1, -1},
  {"UUID", "", "v4", nullptr, 0, "String", 0, 0, false, -1, -1, -1},
  {"UUID", "", "v7", nullptr, 0, "String", 0, 0, false, -1, -1, -1},
  {"String", "", "from_code_point", kCanonParamPool + 287, 1, "String", 1, 1, false, -1, -1, -1},
  {"String", "", "from_bytes", kCanonParamPool + 288, 1, "String", 1, 1, false, -1, -1, -1},
  {"String", "", "from_code_points", kCanonParamPool + 289, 1, "String", 1, 1, false, -1, -1, -1},
  {"Tensor", "", "zeros", kCanonParamPool + 290, 1, "Tensor", 0, 0, true, -1, -1, 0},
  {"Tensor", "", "ones", kCanonParamPool + 291, 1, "Tensor", 0, 0, true, -1, -1, 0},
  {"Tensor", "", "randn", kCanonParamPool + 292, 1, "Tensor", 0, 0, true, -1, -1, 0},
  {"Tensor", "", "eval", kCanonParamPool + 293, 1, "", 0, 0, true, -1, -1, 0},
  {"Tensor", "", "from_csv", kCanonParamPool + 294, 1, "Tensor", 1, 1, false, -1, -1, -1},
  {"Tensor", "", "from", kCanonParamPool + 295, 1, "Tensor", 1, 1, false, -1, -1, -1},
  {"Tensor", "", "concat", kCanonParamPool + 296, 1, "Tensor", 1, 1, false, -1, -1, -1},
  {"Tensor", "", "no_grad", kCanonParamPool + 297, 1, "Any", 1, 1, false, -1, -1, -1},
  {"Tensor", "", "use_cpu", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"Tensor", "", "use_gpu", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"Tensor", "", "use_auto", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"Tensor", "", "gpu_available", nullptr, 0, "Bool", 0, 0, false, -1, -1, -1},
  {"Tensor", "", "device", nullptr, 0, "String", 0, 0, false, -1, -1, -1},
  {"_Time", "", "now_nanos", nullptr, 0, "Long", 0, 0, false, -1, -1, -1},
  {"_Time", "", "monotonic", nullptr, 0, "Float", 0, 0, false, -1, -1, -1},
  {"_Time", "", "sleep", kCanonParamPool + 298, 1, "", 1, 1, false, -1, -1, -1},
  {"_Time", "", "from_iso_nanos", kCanonParamPool + 299, 1, "Long", 1, 1, false, -1, -1, -1},
  {"_Time", "", "parse_nanos", kCanonParamPool + 300, 2, "Long", 2, 2, false, -1, -1, -1},
  {"_Time", "", "iso_nanos", kCanonParamPool + 302, 2, "String", 2, 2, false, -1, -1, -1},
  {"_Time", "", "format_nanos", kCanonParamPool + 304, 3, "String", 3, 3, false, -1, -1, -1},
  {"_Time", "", "parts_nanos", kCanonParamPool + 307, 2, "Object", 2, 2, false, -1, -1, -1},
  {"_Time", "", "from_parts_nanos", kCanonParamPool + 309, 2, "Long", 2, 2, false, -1, -1, -1},
  {"_Time", "", "weekday_nanos", kCanonParamPool + 311, 2, "Long", 2, 2, false, -1, -1, -1},
  {"_Time", "", "add_nanos", kCanonParamPool + 313, 8, "Long", 8, 8, false, -1, -1, -1},
  {"_Time", "", "start_of_nanos", kCanonParamPool + 321, 3, "Long", 3, 3, false, -1, -1, -1},
  {"_Term", "", "cols", nullptr, 0, "Long", 0, 0, false, -1, -1, -1},
  {"_Term", "", "rows", nullptr, 0, "Long", 0, 0, false, -1, -1, -1},
  {"_Term", "", "raw_on", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"_Term", "", "raw_off", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"_Term", "", "flush", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"_Term", "", "color_level", nullptr, 0, "Long", 0, 0, false, -1, -1, -1},
  {"_Term", "", "width", kCanonParamPool + 324, 1, "Long", 1, 1, false, -1, -1, -1},
  {"_Term", "", "resized", nullptr, 0, "Bool", 0, 0, false, -1, -1, -1},
  {"_Term", "", "read_key", kCanonParamPool + 325, 1, "String", 1, 1, false, -1, -1, -1},
  {"_Term", "", "attach_tty", nullptr, 0, "Bool", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "init", kCanonParamPool + 326, 2, "", 2, 2, false, -1, -1, -1},
  {"_Canvas", "", "ttf_load", kCanonParamPool + 328, 1, "Long", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "ttf_free", kCanonParamPool + 329, 1, "", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "ttf_glyph", kCanonParamPool + 330, 6, "Long", 6, 6, false, -1, -1, -1},
  {"_Canvas", "", "ttf_glyph_screen", kCanonParamPool + 336, 6, "Long", 6, 6, false, -1, -1, -1},
  {"_Canvas", "", "ttf_advance", kCanonParamPool + 342, 3, "Long", 3, 3, false, -1, -1, -1},
  {"_Canvas", "", "ttf_ascent", kCanonParamPool + 345, 2, "Long", 2, 2, false, -1, -1, -1},
  {"_Canvas", "", "screen_width", nullptr, 0, "Long", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "screen_height", nullptr, 0, "Long", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "get_screen_pixel", kCanonParamPool + 347, 2, "Long", 2, 2, false, -1, -1, -1},
  {"_Canvas", "", "screen_scale", nullptr, 0, "Float", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "clear", kCanonParamPool + 349, 1, "", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "set_pixel", kCanonParamPool + 350, 3, "", 3, 3, false, -1, -1, -1},
  {"_Canvas", "", "get_pixel", kCanonParamPool + 353, 2, "Long", 2, 2, false, -1, -1, -1},
  {"_Canvas", "", "rect", kCanonParamPool + 355, 6, "", 6, 6, false, -1, -1, -1},
  {"_Canvas", "", "line", kCanonParamPool + 361, 5, "", 5, 5, false, -1, -1, -1},
  {"_Canvas", "", "ellipse", kCanonParamPool + 366, 6, "", 6, 6, false, -1, -1, -1},
  {"_Canvas", "", "triangle", kCanonParamPool + 372, 8, "", 8, 8, false, -1, -1, -1},
  {"_Canvas", "", "polygon", kCanonParamPool + 380, 3, "", 3, 3, false, -1, -1, -1},
  {"_Canvas", "", "font_load", kCanonParamPool + 383, 1, "Long", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "glyph", kCanonParamPool + 384, 6, "", 6, 6, false, -1, -1, -1},
  {"_Canvas", "", "sprite_load", kCanonParamPool + 390, 3, "Long", 3, 3, false, -1, -1, -1},
  {"_Canvas", "", "sprite_from_png", kCanonParamPool + 393, 1, "Long", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "sprite_to_png", kCanonParamPool + 394, 1, "String", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "sprite_width", kCanonParamPool + 395, 1, "Long", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "sprite_height", kCanonParamPool + 396, 1, "Long", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "sprite_blank", kCanonParamPool + 397, 3, "Long", 3, 3, false, -1, -1, -1},
  {"_Canvas", "", "sprite_free", kCanonParamPool + 400, 1, "", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "target", kCanonParamPool + 401, 1, "Long", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "blit", kCanonParamPool + 402, 8, "", 8, 8, false, -1, -1, -1},
  {"_Canvas", "", "blit_scaled", kCanonParamPool + 410, 11, "", 11, 11, false, -1, -1, -1},
  {"_Canvas", "", "present", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "buttons", nullptr, 0, "Long", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "mouse_x", nullptr, 0, "Long", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "mouse_y", nullptr, 0, "Long", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "mouse_buttons", nullptr, 0, "Long", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "key", kCanonParamPool + 421, 1, "Bool", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "key_pop", nullptr, 0, "String", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "char_pop", nullptr, 0, "String", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "closing", nullptr, 0, "Bool", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "windowed", nullptr, 0, "Bool", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "title", kCanonParamPool + 422, 1, "", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "tone", kCanonParamPool + 423, 10, "", 10, 10, false, -1, -1, -1},
  {"_Canvas", "", "music_play", kCanonParamPool + 433, 4, "", 4, 4, false, -1, -1, -1},
  {"_Canvas", "", "music_stop", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "music_pause", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "music_resume", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "music_volume", kCanonParamPool + 437, 1, "", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "music_seek", kCanonParamPool + 438, 1, "", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "music_playing", nullptr, 0, "Bool", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "sound_load", kCanonParamPool + 439, 1, "Long", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "sound_play", kCanonParamPool + 440, 2, "", 2, 2, false, -1, -1, -1},
  {"_Canvas", "", "sound_stop", kCanonParamPool + 442, 1, "", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "sound_playing", kCanonParamPool + 443, 1, "Bool", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "sound_free", kCanonParamPool + 444, 1, "", 1, 1, false, -1, -1, -1},
  {"_Canvas", "", "width", nullptr, 0, "Long", 0, 0, false, -1, -1, -1},
  {"_Canvas", "", "height", nullptr, 0, "Long", 0, 0, false, -1, -1, -1},
  {"", "", "inspect", kCanonParamPool + 445, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "print", kCanonParamPool + 446, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "println", kCanonParamPool + 447, 1, "", 0, 1, false, -1, -1, -1},
  {"", "", "type_of", kCanonParamPool + 448, 1, "String", 1, 1, false, -1, -1, -1},
  {"", "", "to_long", kCanonParamPool + 449, 2, "Long", 1, 1, false, -1, 1, -1},
  {"", "", "to_float", kCanonParamPool + 451, 1, "Float", 1, 1, false, -1, -1, -1},
  {"", "", "to_string", kCanonParamPool + 452, 1, "String", 1, 1, false, -1, -1, -1},
  {"", "", "hash", kCanonParamPool + 453, 1, "Long", 1, 1, false, -1, -1, -1},
  {"", "", "__eff_copy", kCanonParamPool + 454, 1, "Object", 1, 1, false, -1, -1, -1},
  {"", "", "__eff_abort", kCanonParamPool + 455, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "__eff_catch_abort", kCanonParamPool + 456, 1, "Array", 1, 1, false, -1, -1, -1},
  {"", "", "range", kCanonParamPool + 457, 2, "", 0, 0, true, -1, 1, 0},
  {"", "", "iota", kCanonParamPool + 459, 1, "", 0, 0, true, -1, -1, 0},
  {"", "", "repeat", kCanonParamPool + 460, 2, "Array", 2, 2, false, -1, -1, -1},
  {"", "", "grid", kCanonParamPool + 462, 1, "", 0, 0, true, -1, -1, 0},
};

inline constexpr CanonSig kCanonObjectSigs[] = {
  {"", "", "empty", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "get", kCanonParamPool + 463, 2, "", 2, 2, false, -1, -1, -1},
  {"", "", "get_or_put", kCanonParamPool + 465, 2, "", 2, 2, false, -1, -1, -1},
  {"", "", "has", kCanonParamPool + 467, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "iter", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "keys", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "presence", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "remove", kCanonParamPool + 468, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "size", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "values", nullptr, 0, "", 0, 0, false, -1, -1, -1},
};

inline constexpr CanonSig kCanonArraySigs[] = {
  {"", "", "all", kCanonParamPool + 469, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "any", kCanonParamPool + 470, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "contains", kCanonParamPool + 471, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "empty", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "enumerate", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "extend", kCanonParamPool + 472, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "filter", kCanonParamPool + 473, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "find", kCanonParamPool + 474, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "flat_map", kCanonParamPool + 475, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "for_each", kCanonParamPool + 476, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "get", kCanonParamPool + 477, 2, "", 2, 2, false, -1, -1, -1},
  {"", "", "group_by", kCanonParamPool + 479, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "index_of", kCanonParamPool + 480, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "insert", kCanonParamPool + 481, 2, "", 2, 2, false, -1, -1, -1},
  {"", "", "iter", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "join", kCanonParamPool + 483, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "map", kCanonParamPool + 484, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "max", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "max_by", kCanonParamPool + 485, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "min", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "min_by", kCanonParamPool + 486, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "partition", kCanonParamPool + 487, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "pop", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "presence", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "product", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "push", kCanonParamPool + 488, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "reduce", kCanonParamPool + 489, 2, "", 2, 2, false, -1, -1, -1},
  {"", "", "remove_at", kCanonParamPool + 491, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "reverse", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "size", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "slice", kCanonParamPool + 492, 2, "", 2, 2, false, -1, -1, -1},
  {"", "", "sort", kCanonParamPool + 494, 1, "", 0, 0, false, -1, 0, -1},
  {"", "", "sort_by", kCanonParamPool + 495, 2, "", 1, 1, false, -1, 1, -1},
  {"", "", "sorted", kCanonParamPool + 497, 1, "", 0, 0, false, -1, 0, -1},
  {"", "", "sorted_by", kCanonParamPool + 498, 2, "", 1, 1, false, -1, 1, -1},
  {"", "", "sum", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "to_object", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "to_set", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "unzip", nullptr, 0, "", 0, 0, false, -1, -1, -1},
};

inline constexpr CanonSig kCanonStringSigs[] = {
  {"", "", "bytes", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "capitalize", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "code_points", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "contains", kCanonParamPool + 500, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "count", kCanonParamPool + 501, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "empty", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "ends_with", kCanonParamPool + 502, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "eq_ignore_case", kCanonParamPool + 503, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "graphemes", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "index_of", kCanonParamPool + 504, 2, "", 1, 2, false, -1, -1, -1},
  {"", "", "is_alnum", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "is_alpha", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "is_ascii", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "is_digit", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "is_space", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "iter", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "last_index_of", kCanonParamPool + 506, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "lines", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "lower", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "normalize", kCanonParamPool + 507, 1, "", 0, 1, false, -1, -1, -1},
  {"", "", "presence", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "repeat", kCanonParamPool + 508, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "reverse", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "rsplit", kCanonParamPool + 509, 2, "", 1, 2, false, -1, -1, -1},
  {"", "", "size", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "slice", kCanonParamPool + 511, 2, "", 2, 2, false, -1, -1, -1},
  {"", "", "split", kCanonParamPool + 513, 2, "", 1, 2, false, -1, -1, -1},
  {"", "", "split_iter", kCanonParamPool + 515, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "split_whitespace", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "starts_with", kCanonParamPool + 516, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "strip_prefix", kCanonParamPool + 517, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "strip_suffix", kCanonParamPool + 518, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "title", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "to_string", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "tr", kCanonParamPool + 519, 2, "", 2, 2, false, -1, -1, -1},
  {"", "", "trim", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "trim_end", kCanonParamPool + 521, 1, "", 0, 1, false, -1, -1, -1},
  {"", "", "trim_start", kCanonParamPool + 522, 1, "", 0, 1, false, -1, -1, -1},
  {"", "", "truncate", kCanonParamPool + 523, 2, "", 1, 2, false, -1, -1, -1},
  {"", "", "upper", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "view", nullptr, 0, "", 0, 0, false, -1, -1, -1},
};

inline constexpr CanonSig kCanonSetSigs[] = {
  {"", "", "add", kCanonParamPool + 525, 1, "Bool", 1, 1, false, -1, -1, -1},
  {"", "", "contains", kCanonParamPool + 526, 1, "Bool", 1, 1, false, -1, -1, -1},
  {"", "", "diff", kCanonParamPool + 527, 1, "Set", 1, 1, false, -1, -1, -1},
  {"", "", "empty", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "intersect", kCanonParamPool + 528, 1, "Set", 1, 1, false, -1, -1, -1},
  {"", "", "iter", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "presence", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "remove", kCanonParamPool + 529, 1, "Bool", 1, 1, false, -1, -1, -1},
  {"", "", "size", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "subset", kCanonParamPool + 530, 1, "Bool", 1, 1, false, -1, -1, -1},
  {"", "", "superset", kCanonParamPool + 531, 1, "Bool", 1, 1, false, -1, -1, -1},
  {"", "", "sym_diff", kCanonParamPool + 532, 1, "Set", 1, 1, false, -1, -1, -1},
  {"", "", "to_array", nullptr, 0, "Array", 0, 0, false, -1, -1, -1},
  {"", "", "union", kCanonParamPool + 533, 1, "Set", 1, 1, false, -1, -1, -1},
};

inline constexpr CanonSig kCanonTupleSigs[] = {
  {"", "", "contains", kCanonParamPool + 534, 1, "Bool", 1, 1, false, -1, -1, -1},
  {"", "", "empty", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "iter", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "presence", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "size", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "to_array", nullptr, 0, "Array", 0, 0, false, -1, -1, -1},
};

inline constexpr CanonSig kCanonTensorSigs[] = {
  {"", "", "argmax", kCanonParamPool + 535, 1, "Tensor", 1, 1, false, -1, -1, -1},
  {"", "", "backward", nullptr, 0, "Tensor", 0, 0, false, -1, -1, -1},
  {"", "", "clone", nullptr, 0, "Tensor", 0, 0, false, -1, -1, -1},
  {"", "", "detach", nullptr, 0, "Tensor", 0, 0, false, -1, -1, -1},
  {"", "", "dot", kCanonParamPool + 536, 1, "Tensor", 1, 1, false, -1, -1, -1},
  {"", "", "grad", nullptr, 0, "Tensor", 0, 0, false, -1, -1, -1},
  {"", "", "item", nullptr, 0, "Float", 0, 0, false, -1, -1, -1},
  {"", "", "linear_sigmoid", kCanonParamPool + 537, 2, "Tensor", 2, 2, false, -1, -1, -1},
  {"", "", "log", nullptr, 0, "Tensor", 0, 0, false, -1, -1, -1},
  {"", "", "max", kCanonParamPool + 539, 1, "", 0, 1, false, -1, -1, -1},
  {"", "", "mean", kCanonParamPool + 540, 1, "", 0, 1, false, -1, -1, -1},
  {"", "", "pow", kCanonParamPool + 541, 1, "Tensor", 1, 1, false, -1, -1, -1},
  {"", "", "relu", nullptr, 0, "Tensor", 0, 0, false, -1, -1, -1},
  {"", "", "requires_grad", nullptr, 0, "Tensor", 0, 0, false, -1, -1, -1},
  {"", "", "reshape", kCanonParamPool + 542, 1, "Tensor", 1, 1, false, -1, -1, -1},
  {"", "", "shape", nullptr, 0, "Array", 0, 0, false, -1, -1, -1},
  {"", "", "sigmoid", nullptr, 0, "Tensor", 0, 0, false, -1, -1, -1},
  {"", "", "slice", kCanonParamPool + 543, 2, "Tensor", 2, 2, false, -1, -1, -1},
  {"", "", "softmax", nullptr, 0, "Tensor", 0, 0, false, -1, -1, -1},
  {"", "", "sum", kCanonParamPool + 545, 1, "", 0, 1, false, -1, -1, -1},
  {"", "", "to_array", nullptr, 0, "Array", 0, 0, false, -1, -1, -1},
  {"", "", "transpose", nullptr, 0, "Tensor", 0, 0, false, -1, -1, -1},
  {"", "", "zero_grad", nullptr, 0, "Tensor", 0, 0, false, -1, -1, -1},
};

inline constexpr CanonSig kCanonIteratorSigs[] = {
  {"", "", "all", kCanonParamPool + 546, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "any", kCanonParamPool + 547, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "chain", kCanonParamPool + 548, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "chunk_by", kCanonParamPool + 549, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "chunks", kCanonParamPool + 550, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "collect", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "contains", kCanonParamPool + 551, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "count", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "distinct", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "enumerate", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "filter", kCanonParamPool + 552, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "find", kCanonParamPool + 553, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "first", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "flat_map", kCanonParamPool + 554, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "flatten", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "for_each", kCanonParamPool + 555, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "group_by", kCanonParamPool + 556, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "join", kCanonParamPool + 557, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "last", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "map", kCanonParamPool + 558, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "max", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "max_by", kCanonParamPool + 559, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "min", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "min_by", kCanonParamPool + 560, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "nth", kCanonParamPool + 561, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "partition", kCanonParamPool + 562, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "position", kCanonParamPool + 563, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "product", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "reduce", kCanonParamPool + 564, 2, "", 2, 2, false, -1, -1, -1},
  {"", "", "scan", kCanonParamPool + 566, 2, "", 2, 2, false, -1, -1, -1},
  {"", "", "skip", kCanonParamPool + 568, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "skip_while", kCanonParamPool + 569, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "step_by", kCanonParamPool + 570, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "sum", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "take", kCanonParamPool + 571, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "take_while", kCanonParamPool + 572, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "tap", kCanonParamPool + 573, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "to_object", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "to_set", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "unzip", nullptr, 0, "", 0, 0, false, -1, -1, -1},
  {"", "", "windows", kCanonParamPool + 574, 1, "", 1, 1, false, -1, -1, -1},
  {"", "", "zip", kCanonParamPool + 575, 1, "", 1, 1, false, -1, -1, -1},
};

}  // namespace culebra
// clang-format on
