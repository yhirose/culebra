#pragma once

// Core extern "C" runtime callable from JIT'd code: printing, errors,
// throw/defer machinery, type checks, numeric / array / set / tuple /
// tensor / object runtime, and @packable SharedBuffer views.
//
// Runtime-layer fragment of rt.h, split out for readability. These
// fragments rely on rt.h's #include block and are included by rt.h in a
// fixed sequence (see rt.h); they are not standalone headers.

// Bitcast the i64 payload of a Float JitValue back to double.
inline double _culebra_float_to_double(int64_t data) {
  double d;
  std::memcpy(&d, &data, sizeof(d));
  return d;
}

inline int64_t _culebra_double_to_bits(double d) {
  int64_t i;
  std::memcpy(&i, &d, sizeof(i));
  return i;
}

// Coerce a numeric JitValue (Long or Float) to double. Any other tag
// throws a terse TypeError — callers that have the operator + both tags
// (the arithmetic helpers below) should guard with `_arith_guard_numeric`
// first so the user sees the detailed `cannot apply 'op' to L and R`.
inline double _culebra_coerce_num(int8_t tag, int64_t data) {
  if (tag == TAG_LONG) return static_cast<double>(data);
  if (tag == TAG_FLOAT) return _culebra_float_to_double(data);
  throw culebra::CulebraError("TypeError", "type error");
}

// Throw the canonical `cannot apply 'op' to L and R` when either operand
// is non-numeric, after special-method dispatch has already declined.
// Single source for every arithmetic helper's type error, with location.
// Borrow-contract: never touches the operands' refs — each caller arms a
// `JitUnwindRelease` over its whole body, which is the sole releaser of the
// codegen-owned operand temps on this throw (callee-cleans-on-throw).
inline void _arith_guard_numeric(const char* op, int8_t lt, int8_t rt,
                                 int64_t line, int64_t col) {
  bool ln = (lt == TAG_LONG || lt == TAG_FLOAT);
  bool rn = (rt == TAG_LONG || rt == TAG_FLOAT);
  if (ln && rn) return;
  culebra::throw_arith_type_error(op, _culebra_tag_name(lt),
                                  _culebra_tag_name(rt), line, col);
}

// Same-tag equality matching the interpreter's operator==. Strings compare by
// contents; reference types (func/array/object) by identity. Long↔Float
// cross-type uses numeric equality (`1 == 1.0`).
inline bool _culebra_value_equal(int8_t t1, int64_t d1, int8_t t2, int64_t d2) {
  if (t1 != t2) {
    // Numeric cross-type: promote to double and compare by value.
    if ((t1 == TAG_LONG || t1 == TAG_FLOAT) &&
        (t2 == TAG_LONG || t2 == TAG_FLOAT)) {
      return _culebra_coerce_num(t1, d1) == _culebra_coerce_num(t2, d2);
    }
    // String/StringView byte-equality across flavors.
    if ((t1 == TAG_STRING || t1 == TAG_STRINGVIEW) &&
        (t2 == TAG_STRING || t2 == TAG_STRINGVIEW)) {
      return _culebra_str_view(t1, d1) == _culebra_str_view(t2, d2);
    }
    return false;
  }
  switch (t1) {
    case TAG_NIL: return true;
    case TAG_BOOL: return (d1 != 0) == (d2 != 0);
    case TAG_LONG: return d1 == d2;
    case TAG_FLOAT:
      return _culebra_float_to_double(d1) == _culebra_float_to_double(d2);
    case TAG_STRING:
    case TAG_STRINGVIEW:
      return _culebra_str_view(t1, d1) == _culebra_str_view(t2, d2);
    case TAG_TUPLE: {
      // Element-wise eq, matching the interp's TupleValue compare.
      auto* a = reinterpret_cast<JitArray*>(d1);
      auto* b = reinterpret_cast<JitArray*>(d2);
      if (a == b) return true;
      if (a->size != b->size) return false;
      // After the size fast-fail so a mismatched probe never pays the
      // frame (same order on every arm here and in the interp).
      culebra::ValueWalkFrame walk;
      for (size_t i = 0; i < a->size; i++) {
        if (!_culebra_value_equal(a->items[i].tag, a->items[i].data,
                                  b->items[i].tag, b->items[i].data)) {
          return false;
        }
      }
      return true;
    }
    case TAG_SET: {
      // Set eq: same size and every element of `a` is present in `b`'s
      // index. Mirrors the interp's _set_eq.
      auto* a = reinterpret_cast<JitSet*>(d1);
      auto* b = reinterpret_cast<JitSet*>(d2);
      if (a == b) return true;
      if (a->members.size() != b->members.size()) return false;
      if (!b->index) return false;
      culebra::ValueWalkFrame walk;
      for (auto& m : a->members) {
        if (!b->index->contains(m)) return false;
      }
      return true;
    }
    case TAG_ARRAY: {
      // Element-wise eq (structural), matching interp's _array_eq.
      auto* a = reinterpret_cast<JitArray*>(d1);
      auto* b = reinterpret_cast<JitArray*>(d2);
      if (a == b) return true;
      if (a->size != b->size) return false;
      culebra::ValueWalkFrame walk;
      for (size_t i = 0; i < a->size; i++) {
        if (!_culebra_value_equal(a->items[i].tag, a->items[i].data,
                                  b->items[i].tag, b->items[i].data)) {
          return false;
        }
      }
      return true;
    }
    case TAG_OBJECT: {
      // Structural eq: same own slots (data fields + tags; methods live
      // on the shared proto, not own slots) and non-String entries, with
      // equal values. Order-independent. Mirrors interp's _object_eq
      // result (same-class same-field instances compare equal).
      auto* a = reinterpret_cast<JitObject*>(d1);
      auto* b = reinterpret_cast<JitObject*>(d2);
      if (a == b) return true;
      if (a->prop_size() != b->prop_size()) return false;
      culebra::ValueWalkFrame walk;
      bool eq = true;
      a->for_each([&](std::string_view name, const JitObjectEntry& e) {
        if (!eq) return;
        auto idx = b->find_slot(name);
        if (idx == static_cast<size_t>(-1)) { eq = false; return; }
        if (!_culebra_value_equal(e.value.tag, e.value.data,
                                  b->slots[idx].value.tag,
                                  b->slots[idx].value.data)) {
          eq = false;
        }
      });
      if (!eq) return false;
      size_t na = a->non_string_props ? a->non_string_props->size() : 0;
      size_t nb = b->non_string_props ? b->non_string_props->size() : 0;
      if (na != nb) return false;
      if (a->non_string_props) {
        for (auto& [k, entry] : *a->non_string_props) {
          if (!b->non_string_props) return false;
          auto it = b->non_string_props->find(k);
          if (it == b->non_string_props->end()) return false;
          if (!_culebra_value_equal(entry.value.tag, entry.value.data,
                                    it->second.value.tag,
                                    it->second.value.data)) {
            return false;
          }
        }
      }
      return true;
    }
    default: return d1 == d2;  // func: identity
  }
}

// Ordering helpers. Each one exactly mirrors the corresponding
// interpreter `Value::operator<` / `<=` / `>` / `>=` so JIT
// semantics match bit-for-bit. Nil is a special case: `nil op nil`
// always returns false (ordering on `nil` is not defined). Cross-type
// numeric (Long↔Float) promotes to double.

template <typename Cmp>
inline bool _culebra_value_ord(int8_t t1, int64_t d1, int8_t t2, int64_t d2,
                               Cmp cmp, int64_t line, int64_t col) {
  // Cross-type (non-numeric) and same-type-unorderable (Array/Object/...)
  // both raise the canonical "cannot compare L and R", mirroring the
  // interpreter's ord_compare. == stays structural on its own path.
  if (t1 != t2) {
    if ((t1 == TAG_LONG || t1 == TAG_FLOAT) &&
        (t2 == TAG_LONG || t2 == TAG_FLOAT)) {
      return cmp(_culebra_coerce_num(t1, d1), _culebra_coerce_num(t2, d2));
    }
    culebra::throw_compare_type_error(_culebra_tag_name(t1),
                                      _culebra_tag_name(t2), line, col);
  }
  switch (t1) {
    case TAG_NIL: return false;
    case TAG_BOOL: return cmp(double(d1 != 0), double(d2 != 0));
    case TAG_LONG: return cmp(double(d1), double(d2));
    case TAG_FLOAT:
      return cmp(_culebra_float_to_double(d1), _culebra_float_to_double(d2));
    case TAG_STRING:
    case TAG_STRINGVIEW: {
      // Same-tag only (the cross-type branch above already threw):
      // String<String and StringView<StringView both order by bytes,
      // mirroring the interpreter's ord_compare cases.
      auto c = _culebra_str_view(t1, d1).compare(_culebra_str_view(t2, d2));
      return cmp(double(c), 0.0);
    }
    default:
      culebra::throw_compare_type_error(_culebra_tag_name(t1),
                                        _culebra_tag_name(t2), line, col);
  }
}

// Stamp a position onto a still-positionless error, as Interpreter::eval does;
// an Interrupted has no position on either backend. Re-notes the pending
// carrier, which is what a catch pad reads for the error Object's line/col.
// (_jit_backfill_op_pos below is the same rule against the published op
// position, minus the re-note — its consumers are the exception boundaries.)
inline void _jit_backfill_error_pos(culebra::CulebraError& e, int64_t line,
                                    int64_t col) {
  if (e.line != 0 || e.col != 0 || e.kind == "Interrupted") return;
  e.line = line;
  e.col = col;
  culebra::culebra_note_pending_error(e);
}

// Run `op` and give whatever positionless error it raises the position (line,
// col). The interpreter backfills at its eval boundary; compiled code has no
// such boundary, so a call class whose helpers throw with no location (hashing,
// a native handle method) wraps itself here instead. (Templates need C++
// linkage, so this sits outside the extern "C" block below.)
template <class F>
inline auto _jit_at_pos(int64_t line, int64_t col, F&& op) -> decltype(op()) {
  try {
    return op();
  } catch (culebra::CulebraError& e) {
    _jit_backfill_error_pos(e, line, col);
    throw;
  }
}

// Source position of the operation currently executing, published by the JIT
// right before a fallible runtime call whose helper raises a *positionless*
// CulebraError (the value-neutral lib helpers in tensor.h / regex.h / ...
// throw with no line/col). This is the JIT/AOT mirror of the interpreter's
// eval() boundary, which stamps the location of the node it evaluates: the
// interp walks nodes so position is implicit, whereas compiled code must
// publish it. The three points where a runtime error leaves compiled control —
// `culebra_runtime_try_translate` (caught), `JIT::exec` (--jit uncaught), and
// `culebra_aot_bootstrap` (AOT uncaught) — backfill a positionless error from
// here, so a helper that forgets to carry a location still comes out symmetric
// instead of position-less. New fallible call classes opt in by publishing
// here (see `emit_set_op_pos`); the consume side is universal. (Globals here,
// not the extern "C" block below, since the helper takes a C++ reference.)
inline thread_local int64_t _jit_op_line = 0;
inline thread_local int64_t _jit_op_col = 0;

// Stamp the published op position onto a positionless runtime error. Shared by
// the three exception boundaries above.
inline void _jit_backfill_op_pos(culebra::CulebraError& e) {
  if (e.line == 0 && e.col == 0) {
    e.line = _jit_op_line;
    e.col = _jit_op_col;
  }
}

// A source position packed into one int64, so a single value can carry it
// through an ABI with room for one: a runtime call's return (param_pos), a
// rodata entry (the codegen's .argpos array), a cell's payload (the lazy
// combinators' call-site capture). (C++ aggregate return, so outside the
// extern "C" block below; JIT'd code never calls these.)
struct _JitPos {
  int64_t line, col;
};
inline int64_t _jit_pack_pos(int64_t line, int64_t col) {
  return (line << 32) | (col & 0xffffffff);
}
inline _JitPos _jit_unpack_pos(int64_t packed) {
  return {packed >> 32, packed & 0xffffffff};
}

// Trait-conformance arity for a method slot: the widest overload if `cls`
// is a multimethod dispatcher, else its own arity. Defined after the
// multimethod registry below; declared here (outside the extern "C" block)
// for the conformance walk, which needs it before that definition.
inline size_t _jit_dispatcher_max_arity(JitClosure* cls);

// Four ordering predicates share `_culebra_value_ord`'s Nil/cross-type
// scaffolding; the public extern "C" trampolines below are the only
// callers. A single `cmp` comparator parameterises the leaf compare.

extern "C" {

// Forward decl; the `__str__` dispatcher lives alongside the other
// special-method helpers further down (it returns std::optional<std::string>,
// so it needs full C++ linkage — can't be declared inside the
// enclosing extern "C" block).
inline std::optional<std::string> _try_str_special(int8_t type, int64_t data);

// Forward decls used by `culebra_runtime_hash_any` and the
// `_jit_object_user_*` helpers (defined alongside the other special-
// method helpers further down).
inline std::optional<JitValue> _try_special_unary(int8_t t, int64_t d,
                                                  const char* name);
inline const JitObjectEntry* _find_property(JitObject* obj,
                                            const char* key);

// The `inspect` repr (top-level strings quoted) as a binary-safe string — no
// trailing newline. Shared by IO.inspect (→ stdout) and IO.einspect
// (→ stderr) so the two format identically.
inline std::string _culebra_inspect_repr(int8_t type, int64_t data) {
  if (auto s = _try_str_special(type, data)) return *s;
  switch (type) {
    case TAG_NIL:  return "nil";
    case TAG_BOOL: return data ? "true" : "false";
    case TAG_LONG: return std::to_string(data);
    case TAG_STRING: {
      auto* p = reinterpret_cast<const char*>(data);
      std::string r = "'";
      r.append(p, _str_len(p));
      return r += "'";
    }
    case TAG_STRINGVIEW: {
      auto* v = reinterpret_cast<const JitStringView*>(data);
      std::string r = "'";
      r.append(v->ptr, v->len);
      return r += "'";
    }
    case TAG_ARRAY:
    case TAG_OBJECT:
    case TAG_FLOAT:
    case TAG_TENSOR:
    case TAG_TUPLE:
    case TAG_SET:
    case TAG_FUNC:
      return _culebra_value_to_str_impl(type, data);
    default:
      return "[unknown]";
  }
}

inline void _culebra_inspect_to(std::ostream& os, int8_t type, int64_t data) {
  auto r = _culebra_inspect_repr(type, data);
  os.write(r.data(), static_cast<std::streamsize>(r.size()));
  os << std::endl;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_inspect(int8_t type,
                                                       int64_t data) {
  _culebra_inspect_to(culebra::program_out(), type, data);
}

// `IO.einspect` — inspect to stderr.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_einspect(int8_t type,
                                                             int64_t data) {
  _culebra_inspect_to(std::cerr, type, data);
}

// str_display equivalent for an uncaught throw value: a top-level String /
// StringView prints raw (it's the error message), everything else uses the
// repr form. Matches the interp's `Value::str_display` (raw string, else
// `str()`), so `throw "boom"` → `uncaught: boom` on both backends. Note it
// does NOT honor `__str__` — the interp's uncaught path uses plain
// str_display, not the _with_special variant, so value_to_display (which
// does honor __str__) is deliberately not reused here.
inline std::string _culebra_uncaught_display(int8_t type, int64_t data) {
  if (type == TAG_STRING) {
    auto* p = reinterpret_cast<const char*>(data);
    return std::string(p, _str_len(p));
  }
  if (type == TAG_STRINGVIEW) {
    auto* v = reinterpret_cast<const JitStringView*>(data);
    return std::string(v->ptr, v->len);
  }
  return _culebra_value_to_str_impl(type, data);
}

// For interpolation / print / to_string: strings unquoted, Objects
// with `__str__` return their custom form.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_value_to_display(
    int8_t type, int64_t data) {
  if (auto s = _try_str_special(type, data)) return _culebra_heap_str(*s);
  if (type == TAG_STRING) return reinterpret_cast<const char*>(data);
  if (type == TAG_STRINGVIEW) {
    auto* v = reinterpret_cast<const JitStringView*>(data);
    return _culebra_heap_str(std::string_view(v->ptr, v->len));
  }
  return _culebra_heap_str(_culebra_value_to_str_impl(type, data));
}

// `IO.eprint` — print (raw display, no newline) to stderr. Defined after
// value_to_display since it reuses that display formatting.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_eprint(int8_t type,
                                                              int64_t data) {
  std::cerr << culebra_runtime_value_to_display(type, data);
}

// `IO.eprintln` — print (raw display) + newline to stderr.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_eprintln(int8_t type,
                                                                int64_t data) {
  std::cerr << culebra_runtime_value_to_display(type, data) << std::endl;
}

// `"{x:spec}"` interpolation format. Mirrors interp's apply_format_spec:
// numeric values honor the spec's type char, everything else formats its
// display string. Returns a heap string (leaked for program lifetime,
// like other interpolation pieces).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_format_value(
    int8_t type, int64_t data, const char* spec_cstr, int64_t line,
    int64_t col) {
  std::string_view spec(spec_cstr);
  if (spec.empty()) return culebra_runtime_value_to_display(type, data);
  if (type == TAG_LONG) {
    if (culebra::format_spec_wants_float(spec))
      return _culebra_heap_str(culebra::format_value_double(
          static_cast<double>(data), spec, line, col));
    return _culebra_heap_str(culebra::format_value_long(data, spec, line, col));
  }
  if (type == TAG_FLOAT) {
    double d;
    std::memcpy(&d, &data, sizeof d);
    if (culebra::format_spec_wants_int(spec))
      return _culebra_heap_str(culebra::format_value_long(
          static_cast<int64_t>(d), spec, line, col));
    return _culebra_heap_str(culebra::format_value_double(d, spec, line, col));
  }
  return _culebra_heap_str(culebra::format_value_string(
      culebra_runtime_value_to_display(type, data), spec, line, col));
}

// A String-valued own slot, or nullopt when absent / not a String.
inline std::optional<std::string_view> _jit_string_slot(JitObject* obj,
                                                        const char* name) {
  auto idx = obj->find_slot(name);
  if (idx == static_cast<size_t>(-1)) return std::nullopt;
  const auto& v = obj->slots[idx].value;
  if (v.tag != TAG_STRING) return std::nullopt;
  return _culebra_str_view(v.tag, v.data);
}
// The two tags an instance carries: `class` on every class-sugar instance
// (empty if absent, defensively) and `__enum` on a variant only — the
// parent enum's name, nullopt for any other object.
inline std::string_view _jit_derived_class_tag(JitObject* obj) {
  return _jit_string_slot(obj, "class").value_or(std::string_view{});
}
inline std::optional<std::string_view> _jit_enum_name(JitObject* obj) {
  return _jit_string_slot(obj, "__enum");
}

// `hash(v)` builtin runtime entry. Routes Object to a user-defined
// `hash()` method (Hashable + Eq structural conformance) or a variant's
// derived hash; primitives go through JitValueHash — same path JitObject's
// AnyKeyMap uses. Throws on unhashable inputs (Array / Set / Function /
// Tensor, Object without `hash`). Returns a raw int64 (Long payload).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_hash_any(
    int8_t type, int64_t data, int64_t line, int64_t col) {
  if (type == TAG_OBJECT) {
    auto r = _try_special_unary(type, data, "hash");
    if (!r) {
      if (auto h = _jit_enum_variant_hash(reinterpret_cast<JitObject*>(data)))
        return *h;
      throw culebra::CulebraError(
          "TypeError",
          "unhashable type: 'Object' (no hash() method)",
          static_cast<int>(line), static_cast<int>(col));
    }
    if (r->tag != TAG_LONG) {
      _culebra_value_release_impl(r->tag, r->data);
      throw culebra::CulebraError("TypeError",
                                  "hash() must return Long",
                                  static_cast<int>(line),
                                  static_cast<int>(col));
    }
    return r->data;
  }
  // Primitives go through the same hash that JitObject's AnyKeyMap uses;
  // unhashable inputs (Array / Set / Function / Tensor) throw there — but
  // positionless, since the container use has no call site. Backfill the
  // call-site line/col so `hash([1,2])` and `[1,2].hash()` carry a position
  // like the interp.
  return _jit_at_pos(line, col, [&] {
    return static_cast<int64_t>(JitValueHash{}(JitValue{type, data}));
  });
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE const char* culebra_runtime_str_concat(
    const char* a, const char* b) {
  auto la = _str_len(a);
  auto lb = _str_len(b);
  char* r = _str_alloc(la + lb);
  std::memcpy(r, a, la);
  std::memcpy(r + la, b, lb);
  return r;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_str_eq(const char* a,
                                                         const char* b) {
  uint64_t la = _str_len(a), lb = _str_len(b);
  return la == lb && std::memcmp(a, b, la) == 0;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int32_t culebra_runtime_str_cmp(const char* a,
                                                             const char* b) {
  // Lexicographic over min length, then shorter-first — matches
  // std::string_view::compare and the interpreter's std::string ordering.
  return static_cast<int32_t>(_str_sv(a).compare(_str_sv(b)));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_type_error(int64_t line,
                                                             int64_t col) {
  culebra::throw_type_error_at(line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_destructure_mismatch(int64_t line, int64_t col) {
  culebra::throw_destructure_mismatch_at(line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_compound_missing_property(int64_t line, int64_t col) {
  culebra::throw_compound_missing_property_at(line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_immutable_assign(const char* name, int64_t line, int64_t col) {
  culebra::throw_immutable_assign_at(name ? name : "", line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_unknown_kwarg(const char* name, int64_t line, int64_t col) {
  culebra::throw_unknown_kwarg_at(name ? name : "", line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_missing_required_arg(const char* name, int64_t line, int64_t col) {
  culebra::throw_missing_required_arg_at(name ? name : "", line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_throw_error(const char* kind, const char* msg,
                             int64_t line, int64_t col) {
  culebra::throw_runtime_error_at(kind ? kind : "", msg ? msg : "",
                                   line, col);
}

// Loop safepoint slow path. The JIT/AOT loop backedge inlines a relaxed load of
// `culebra_g_wake` and calls this only when it is set. `throw_if_interrupted`
// consults the per-thread interrupt flag: a real Ctrl+C is consumed and throws
// "interrupted"; an isolate's cancel throws the sticky "isolate cancelled"; a
// false wake (set for another thread) just returns. The interpreter's poll
// (check_interrupt) mirrors the same decision. Unwinds via the same path as any
// runtime error — the loop's enclosing function carries a personality
// (scan_eh_defer flags loops as has_eh).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_safepoint() {
  culebra::throw_if_interrupted();
}

// JIT module table — keyed by absolute module path, value is the
// module's export Object packed into a JitValue. Lives in the active
// Runtime so it dies with the Runtime (releasing each held +1) rather
// than persisting as thread-local state across embedding sessions.
struct _JitModuleTable {
  std::unordered_map<std::string, JitValue> entries;
  ~_JitModuleTable() {
    for (auto& [_, v] : entries) _culebra_value_release_impl(v.tag, v.data);
  }
};
inline std::unordered_map<std::string, JitValue>& _jit_module_table() {
  return culebra::runtime_substate<_JitModuleTable>(
             culebra::kSlotJitModuleTable).entries;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_module_register(const char* path, int8_t tag, int64_t data) {
  auto& t = _jit_module_table();
  auto key = std::string(path ? path : "");
  auto it = t.find(key);
  if (it != t.end()) {
    // Replace: drop the old entry's +1 before overwriting so the slot
    // never holds two refs at once.
    culebra_runtime_value_release(it->second.tag, it->second.data);
    it->second = JitValue{tag, data};
  } else {
    t.emplace(std::move(key), JitValue{tag, data});
  }
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_module_get(const char* path, int8_t* out_tag,
                            int64_t* out_data, int64_t line, int64_t col) {
  auto& t = _jit_module_table();
  auto it = t.find(std::string(path ? path : ""));
  if (it == t.end()) {
    culebra::throw_runtime_error_at(
        "ImportError",
        std::format("module '{}' was not loaded — `import` statements "
                    "must be reachable from the entry point's "
                    "dependency graph", path ? path : ""),
        line, col);
  }
  // Hand the caller a fresh +1 so the importing scope owns its own
  // reference (table retains the original).
  culebra_runtime_value_retain(it->second.tag, it->second.data);
  *out_tag = it->second.tag;
  *out_data = it->second.data;
}

// Like type_error but includes "expected X, got Y" — caller passes the
// expected type as a string-literal global and the runtime tag of the
// actual value. Used by leaf JIT accessors (value_to_long etc.) where
// both pieces of context are statically available at the throw site.
[[noreturn]] CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_type_error_typed(int64_t line, int64_t col,
                                 const char* expected, int8_t got_tag) {
  const char* got = "?";
  switch (got_tag) {
    case TAG_NIL:    got = "Nil";      break;
    case TAG_BOOL:   got = "Bool";     break;
    case TAG_LONG:   got = "Long";     break;
    case TAG_FLOAT:  got = "Float";    break;
    case TAG_STRING: got = "String";   break;
    case TAG_ARRAY:  got = "Array";    break;
    case TAG_OBJECT: got = "Object";   break;
    case TAG_FUNC:   got = "Function"; break;
    case TAG_TENSOR: got = "Tensor";   break;
    case TAG_TUPLE:  got = "Tuple";    break;
    case TAG_SET:    got = "Set";      break;
    case TAG_STRINGVIEW: got = "StringView"; break;
  }
  throw culebra::CulebraError("TypeError",
      culebra::type_mismatch_message(expected, got), line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_arity_error(
    int64_t got, int64_t declared, int64_t line, int64_t col) {
  // Legacy entry point (kept for ABI continuity in JIT runtimes that
  // were compiled before culebra_runtime_arity_missing existed).
  throw culebra::CulebraError("ArityError", std::format(
      "arguments error: called with {} argument(s), expected at least {}",
      got, declared), line, col);
}

// Call-site position thread-locals (the rest of the family lives with
// set_call_site below). Defined here — not forward-declared extern —
// because a non-inline declaration of an inline variable is an ODR trap:
// with a second jit.h TU in the binary (a wrap.h declaration), each TU
// emitted its own non-odr TLS wrapper and the link failed duplicate.
inline thread_local int64_t _jit_call_site_line = 0;
inline thread_local int64_t _jit_call_site_col = 0;

// The call's BOUNDARY position — where the interp's eval() would stamp a
// positionless error escaping this call: the call's own chain node. For a
// direct or through-value call that IS the call site, so set_call_site
// defaults it there; a UFCS site publishes the chain head as a pending
// override first (set_call_boundary), which the next set_call_site
// consumes. The ns dispatch's final positionless backfill reads this pair
// instead of the call site — its explicit errors (arity, param types) keep
// reporting at the call site, interp's two-channel split.
inline thread_local int64_t _jit_call_boundary_line = 0;
inline thread_local int64_t _jit_call_boundary_col = 0;
inline thread_local int64_t _jit_pending_boundary_line = 0;
inline thread_local int64_t _jit_pending_boundary_col = 0;

// Per-argument source positions for an indirect (as-value) ns-method call,
// threaded by the indirect-call codegen so a wrong-typed argument is rejected
// at that argument's position with the interp binder's wording ("parameter
// '<name>' expects <T>"), exactly like a direct call. `_jit_argpos_n` is the
// count; 0 means "no per-arg positions" — a HOF callback path, where
// set_call_site reset it and the dispatch's body-coercion arg0 check runs
// instead (matching interp's callback wording). Capped at K; a longer arg list
// leaves the overflow positions at the call site.
// 64 covers any realistic positional arity; args past the cap fall back to
// the call-site position (a known, deterministic divergence from interp,
// which has no cap — see the typed-param position notes). Here rather than
// with set_call_site for the same ODR reason as the pair above.
inline constexpr int _JIT_ARGPOS_MAX = 64;
inline thread_local int64_t _jit_argpos_line[_JIT_ARGPOS_MAX] = {};
inline thread_local int64_t _jit_argpos_col[_JIT_ARGPOS_MAX] = {};
inline thread_local int _jit_argpos_n = 0;

// Run `op`, backfilling a positionless error from the published call site.
// The entry-catch idiom for native thunks, whose ABI carries no line/col
// (handle methods, derived-method thunks, ns adapters). A template needs
// C++ linkage, and it must follow the thread-locals above, so the extern
// "C" block pauses around it. JitBorrowedCallSite below needs C++ linkage
// for the same reason and rides the same pause.
}  // extern "C" (resumed below)
template <class F>
inline auto _jit_at_call_site(F&& op) -> decltype(op()) {
  return _jit_at_pos(_jit_call_site_line, _jit_call_site_col,
                     std::forward<F>(op));
}

// Publish a call site for a method the runtime reaches on its own, rather
// than from a codegen call site: the operator forms of `==` / `!=` and the
// ordering ops dispatch to a user or derived `__eq__` / `eq` / `__lt__` /
// `__le__` / `cmp` from inside the comparison helper, so nothing set one.
// Explicit errors raised by that method's binder — an overload set's
// DispatchError, an ArityError, a typed-param TypeError — read the call site
// and would otherwise report wherever the last real call was. The operator's
// own position is where the interp anchors them (its binary-expression node).
// The per-argument positions go with it: the operand is handed over by the
// runtime, not written at a call, so there is no argument expression to point
// at — leaving the previous call's array live would report a typed-param
// error somewhere else entirely. Restores on scope exit, throw included, so
// an outer call site is unaffected.
struct JitBorrowedCallSite {
  int64_t line, col, bline, bcol;
  int argn;
  JitBorrowedCallSite(int64_t l, int64_t c)
      : line(_jit_call_site_line), col(_jit_call_site_col),
        bline(_jit_call_boundary_line), bcol(_jit_call_boundary_col),
        argn(_jit_argpos_n) {
    _jit_call_site_line = l;
    _jit_call_site_col = c;
    _jit_call_boundary_line = l;
    _jit_call_boundary_col = c;
    _jit_argpos_n = 0;
  }
  ~JitBorrowedCallSite() {
    _jit_call_site_line = line;
    _jit_call_site_col = col;
    _jit_call_boundary_line = bline;
    _jit_call_boundary_col = bcol;
    _jit_argpos_n = argn;
  }
};
extern "C" {

// Recursion guard, compiled-code side. The counter itself lives in shared.h
// (`_culebra_call_depth`) so the interp's RecursionFrame and these helpers
// move the same per-thread count. `enter` sits in every user-body prologue
// (after the param type checks — a TypeError there outranks RecursionError,
// interp order); `leave` on the normal return paths. Unwinding skips the
// leaves, so cleanup pads and catch entries `restore` to an absolute depth
// snapshot instead — by the time a catch body (or an unwinding frame's
// defers) runs, the count agrees with what the interp's RAII dtors left.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_recursion_enter() {
  // Reports at the call site the caller published (set_call_site), which is
  // what the interp reads back out of `__LINE__`/`__COLUMN__`. Returns the
  // new depth so the prologue can stash it for its cleanup pads.
  culebra::check_recursion_depth(_jit_call_site_line, _jit_call_site_col);
  return ++culebra::_culebra_call_depth;
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_recursion_leave() {
  --culebra::_culebra_call_depth;
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_recursion_depth() {
  return culebra::_culebra_call_depth;
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_recursion_restore(
    int64_t depth) {
  // A negative depth is the prologue's "never entered" sentinel: the frame's
  // slot is pre-set to -1 and only overwritten once `enter` succeeded, so a
  // RecursionError thrown by the enter itself unwinds through the frame's
  // own cleanup pad without touching the count.
  if (depth >= 0) culebra::_culebra_call_depth = depth;
}

// Name-aware arity error: callee passes its declared parameter name
// table (a const char* array, NUL-terminated entries) and the runtime
// throws "missing required argument 'X'" where X is the first slot
// that wasn't filled. Matches interp's eval-time message exactly so
// `e.message.contains('missing required')` works on both backends.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_arity_missing(const char* const* names, int64_t got,
                               int64_t line, int64_t col) {
  const char* missing = (names && names[got]) ? names[got] : "";
  // The prologue bakes its own def position; interp reports a missing required
  // argument at the CALL site, which set_call_site published before the
  // invocation. Use it when set, falling back to the passed def position.
  int64_t l = _jit_call_site_line ? _jit_call_site_line : line;
  int64_t c = _jit_call_site_line ? _jit_call_site_col : col;
  culebra::throw_missing_required_arg_at(missing, l, c);
}

// C++ exception thrown by `culebra_runtime_throw` when user code runs
// `throw expr`. The payload (tag + data) is NOT read via this object
// in JITed code — that would require exposing the typeinfo symbol
// (`_ZTI16CulebraException`) to the JIT linker, which is fragile
// across platforms. Instead, landingpads use catch-all and read the
// `culebra_thrown_*` globals below. The member fields are only read
// by the host-side catch in JIT::run to format the uncaught-throw
// message.
class CulebraException : public std::exception {
 public:
  int8_t tag;
  int64_t data;
  CulebraException(int8_t t, int64_t d) : tag(t), data(d) {}
  const char* what() const noexcept override { return "CulebraException"; }
};

// Unpack a script-thrown value for a host-facing report: an Object's
// `kind`/`message` slots when present, the display form otherwise. The one
// shape both the test host (test_engine.h) and the embedding API
// (vm_embed.h) surface a raw CulebraException through.
inline void describe_thrown_value(JitValue v, std::string& kind,
                                  std::string& message) {
  auto str_slot = [&](JitObject* o, const char* k) -> std::string {
    size_t i = o->find_slot(k);
    if (i == static_cast<size_t>(-1)) return {};
    auto sv = o->slots[i].value;
    if (sv.tag != TAG_STRING && sv.tag != TAG_STRINGVIEW) return {};
    return std::string(_str_sv(reinterpret_cast<const char*>(sv.data)));
  };
  if (v.tag == TAG_OBJECT) {
    auto* o = reinterpret_cast<JitObject*>(v.data);
    if (auto k = str_slot(o, "kind"); !k.empty()) kind = k;
    message = str_slot(o, "message");
  }
  if (message.empty()) message = _culebra_uncaught_display(v.tag, v.data);
}

CULEBRA_RT_KEEP void culebra_runtime_value_retain(int8_t tag,
                                                        int64_t data);

// Culebra is single-threaded, so plain globals are fine. Populated
// by `culebra_runtime_throw` and read by try/catch landingpads to
// distinguish user throws (`culebra_is_throw == 1`) from internal
// runtime errors (`std::runtime_error` etc., which re-raise).
// JIT IR reaches the carriers through these accessors — ORC's emutls
// can't resolve `__emutls_v.*` from JIT modules, so a regular call
// into C++ (where Runtime lookup just works) is the portable path.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int8_t culebra_runtime_get_is_throw() {
  return culebra::current_runtime().is_throw;
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_clear_is_throw() {
  culebra::current_runtime().is_throw = 0;
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int8_t culebra_runtime_get_thrown_tag() {
  return culebra::current_runtime().thrown_tag;
}
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_get_thrown_data() {
  return culebra::current_runtime().thrown_data;
}

// Save / restore the thrown-value carrier across a cleanup call whose own
// exception is swallowed (a for-in iterator's dispose() on the unwind /
// early-return paths). The carrier is a plain global, so a culebra throw from
// the cleanup overwrites it — swallowing the cleanup's C++ exception alone
// still leaves the wrong payload for the outer catch to read. `save` snapshots
// (retaining the payload so it survives the cleanup); `restore` runs on both
// the normal and the swallowed paths: if the cleanup replaced the payload,
// release the replacement and put the snapshot back (the save's retain
// transfers into the carrier); otherwise just drop the snapshot's retain.
// LIFO stack of pending-error snapshots, paired with save/restore below. A
// runtime error in flight lives only in the pending carrier (is_throw is still
// 0 until a catch pad materializes it), so a swallowed dispose() that throws its
// own CulebraError would overwrite it — the pending analogue of the thrown-value
// clobber the save/restore was written for. The interpreter needs no such thing:
// its in-flight error is a distinct C++ exception object, untouched by dispose's.
struct _PendingSnapshot {
  int8_t error;
  std::string kind, msg;
  int64_t line, col;
};
CULEBRA_RT_CORE_OWNED thread_local std::vector<_PendingSnapshot>
    _pending_save_stack;

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_save_thrown(
    int8_t* out_flag, int8_t* out_tag, int64_t* out_data) {
  auto& rt = culebra::current_runtime();
  *out_flag = rt.is_throw;
  *out_tag = rt.thrown_tag;
  *out_data = rt.thrown_data;
  if (rt.is_throw) culebra_runtime_value_retain(rt.thrown_tag, rt.thrown_data);
  _pending_save_stack.push_back({rt.pending_error, rt.pending_kind,
                                 rt.pending_msg, rt.pending_line,
                                 rt.pending_col});
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_restore_thrown(
    int8_t flag, int8_t tag, int64_t data) {
  auto& rt = culebra::current_runtime();
  bool unchanged =
      flag && rt.is_throw && rt.thrown_tag == tag && rt.thrown_data == data;
  if (rt.is_throw && !unchanged) {
    // The cleanup threw its own culebra payload (now swallowed): we are its
    // catch handler, so release the retain culebra_runtime_throw took.
    _culebra_value_release_impl(rt.thrown_tag, rt.thrown_data);
  }
  // Drop the snapshot's retain in every case: the original throw's retain
  // (consumed by the eventual catch handler) was never touched by the
  // overwrite, so the snapshot only needed to bridge the cleanup call.
  if (flag) _culebra_value_release_impl(tag, data);
  rt.thrown_tag = tag;
  rt.thrown_data = data;
  rt.is_throw = flag;
  // Restore the pending carrier the swallowed cleanup may have overwritten
  // (paired push in save above; LIFO across nested disposes).
  if (!_pending_save_stack.empty()) {
    auto& s = _pending_save_stack.back();
    rt.pending_error = s.error;
    rt.pending_kind = std::move(s.kind);
    rt.pending_msg = std::move(s.msg);
    rt.pending_line = s.line;
    rt.pending_col = s.col;
    _pending_save_stack.pop_back();
  }
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_throw(int8_t tag,
                                                        int64_t data) {
  // The caller hands the payload +1-owned (compile() result / a fresh
  // deserialize) and the carrier takes that reference over; it keeps the
  // payload alive through unwinding and is consumed exactly once by the
  // matching catch (the `catch e` binding absorbs it) or the uncaught path
  // (exec / aot_bootstrap release it). A retain here on top of the caller's
  // +1 double-counted the payload: it could never reach refcount 0, so a
  // caught heap payload leaked and its drop() never fired.
  auto& rt = culebra::current_runtime();
  rt.thrown_tag = tag;
  rt.thrown_data = data;
  rt.is_throw = 1;
  throw CulebraException(tag, data);
}

// Re-throw the currently-in-flight exception. Used by cleanup
// landingpads to let the exception keep unwinding after running
// deferred work, while still allowing the JIT to choose the next
// landingpad via an `invoke` unwind edge (LLVM's `resume` alone
// abandons the current function, skipping outer landingpads in the
// same function).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_rethrow() {
  throw;
}

// --- Defer stack (JIT side) ---
//
// Culebra is single-threaded, so a single global LIFO stack of retained
// closure Values is sufficient. Each lexical scope records a "mark"
// (the stack size) on entry and, on every exit path (fall-through,
// return, throw), unwinds the stack back to that mark — running each
// popped closure as a 0-arg call.
inline std::vector<JitValue>& _culebra_defer_stack() {
  return culebra::runtime_substate<std::vector<JitValue>>(
      culebra::kSlotDeferStack);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_defer_mark() {
  return static_cast<int64_t>(_culebra_defer_stack().size());
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_defer_push(int8_t tag,
                                                             int64_t data) {
  // Retain: stack owns a reference until run_to drops it.
  culebra_runtime_value_retain(tag, data);
  _culebra_defer_stack().push_back(JitValue{tag, data});
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_defer_run_to(int64_t mark) {
  auto& s = _culebra_defer_stack();
  while (static_cast<int64_t>(s.size()) > mark) {
    auto v = s.back();
    s.pop_back();
    auto* c = reinterpret_cast<JitClosure*>(v.data);
    try {
      auto r = _culebra_invoke0(c);
      _culebra_value_release_impl(r.tag, r.data);
    } catch (...) {
      _culebra_value_release_impl(v.tag, v.data);
      // Drop remaining defers for this scope so they don't leak.
      while (static_cast<int64_t>(s.size()) > mark) {
        auto rem = s.back();
        s.pop_back();
        _culebra_value_release_impl(rem.tag, rem.data);
      }
      throw;
    }
    _culebra_value_release_impl(v.tag, v.data);
  }
}

inline const char* _culebra_tag_name(int8_t tag) {
  switch (tag) {
    case TAG_NIL:    return "Nil";
    case TAG_BOOL:   return "Bool";
    case TAG_LONG:   return "Long";
    case TAG_FUNC:   return "Function";
    case TAG_STRING: return "String";
    case TAG_ARRAY:  return "Array";
    case TAG_OBJECT: return "Object";
    case TAG_FLOAT:  return "Float";
    case TAG_TENSOR: return "Tensor";
    case TAG_TUPLE:  return "Tuple";
    case TAG_SET:    return "Set";
    case TAG_STRINGVIEW: return "StringView";
  }
  return "Unknown";
}

// Check that a Value matches a named type ("Any" matches everything).
// For class-instance arguments (TAG_OBJECT with a `class:` String
// property), also accept the matching class name — without this the
// JIT can't validate `s: Square` annotations against `Square.new(...)`.
// Test whether the runtime value at (tag, data) matches a single
// non-Union type name. Mirrors interp's type_matches for the
// primitive + Object/class side; called from culebra_runtime_type_check
// once per Union alternative.
inline bool _culebra_type_matches_single(int8_t tag, int64_t data,
                                          std::string_view expected) {
  if (expected == "Any") return true;
  // Function type `fn(...) -> R`: structural callable match, mirroring
  // interp's type_matches. Params/return are documentation in the MVP, so
  // a closure (TAG_FUNC) or a class instance with an own/proto `__call__`
  // satisfies it. Checked before the `?` sugar so `fn(A) -> B?` (optional
  // return) isn't misread as an optional function.
  if (culebra::is_fn_type(expected)) {
    if (tag == TAG_FUNC) return true;
    if (tag == TAG_OBJECT) {
      auto* obj = reinterpret_cast<JitObject*>(data);
      if (obj->proto) {
        auto* e = _find_property(obj, "__call__");
        if (e && e->value.tag == TAG_FUNC) return true;
      }
    }
    return false;
  }
  // Composite bound (`A + B`) — all-of: conform to every part.
  // Mirrors interp's type_matches.
  if (culebra::has_toplevel_plus(expected)) {
    for (auto part : culebra::split_intersection_types(expected)) {
      if (!_culebra_type_matches_single(tag, data, part)) return false;
    }
    return true;
  }
  // `T?` Optional sugar = `T | Nil`: trailing `?` accepts Nil, else
  // checks the base. Mirrors interp's type_matches.
  if (!expected.empty() && expected.back() == '?') {
    if (tag == TAG_NIL) return true;
    return _culebra_type_matches_single(tag, data,
                                        expected.substr(0, expected.size() - 1));
  }
  // Generic outer-match: `Array<Long>` checks `Array` only. Element
  // type is documentation in the MVP (matches interp's type_matches).
  if (expected.find('<') != std::string_view::npos) {
    expected = culebra::parse_generic_head(expected).outer;
  }
  std::string_view actual = _culebra_tag_name(tag);
  if (actual == expected) return true;
  // Built-in trait conformance: primitives (non-Object tags) can
  // satisfy Stringer / Eq / Comparable via the hard-coded table.
  if (tag != TAG_OBJECT && culebra::lookup_trait(expected) &&
      culebra::builtin_conforms_to_trait(actual, expected)) {
    return true;
  }
  std::string_view class_tag_view;
  if (tag == TAG_OBJECT) {
    auto* obj = reinterpret_cast<JitObject*>(data);
    // A class instance with an own/proto `__call__` satisfies `Function`
    // (Option A: structural callable). Mirrors interp's type_matches and
    // the callback adapter; proto-gated so a plain dict isn't callable.
    if (expected == "Function" && obj->proto) {
      auto* e = _find_property(obj, "__call__");
      if (e && e->value.tag == TAG_FUNC) return true;
    }
    if (auto idx = obj->find_slot("class");
        idx != static_cast<size_t>(-1)) {
      const auto& cls_slot = obj->slots[idx].value;
      if (cls_slot.tag == TAG_STRING) {
        class_tag_view = std::string_view(
            reinterpret_cast<const char*>(cls_slot.data));
        if (class_tag_view == expected) return true;
      }
    }
    // Enum variant: also matches the parent enum name (`__enum` field),
    // so `r: Result` accepts any `Result.*` variant. Mirrors interp.
    if (auto en = _jit_enum_name(obj); en && *en == expected) return true;
  }
  // Structural trait conformance: when `expected` is a registered
  // trait, check (and cache) whether this instance's class supplies
  // every required method (matching arity). Primitive tags can't
  // conform (no class tag, no method table).
  if (auto* trait = culebra::lookup_trait(expected)) {
    if (tag != TAG_OBJECT) return false;
    if (class_tag_view.empty()) {
      // Bare Object literal — ObjectValue::builtins() provides defaults
      // (e.g. `iter()` for Iterable) so the built-in table answers.
      return culebra::builtin_conforms_to_trait("Object", expected);
    }
    std::string class_name(class_tag_view);
    std::string trait_name(expected);
    std::unique_lock lock(culebra::trait_mutex());
    auto& by_trait = culebra::trait_conformance_cache()[class_name];
    auto it = by_trait.find(trait_name);
    if (it != by_trait.end()) return it->second;
    auto* obj = reinterpret_cast<JitObject*>(data);
    // A variant has no method table; what it conforms to is fixed.
    if (_jit_enum_name(obj)) {
      return by_trait[trait_name] =
                 culebra::enum_variant_conforms_to_trait(expected);
    }
    std::unordered_map<std::string, size_t> class_methods;
    auto walk_slots = [&](JitObject* o) {
      if (!o || !o->shape) return;
      for (size_t i = 0; i < o->shape->names.size(); i++) {
        const auto& slot = o->slots[i];
        if (slot.value.tag != TAG_FUNC) continue;
        auto* cls = reinterpret_cast<JitClosure*>(slot.value.data);
        // JitClosure::arity counts user-visible params (excluding __cls__
        // and `self`); for an overloaded method (a dispatcher) report the
        // widest overload so conformance sees the real arities (mirrors the
        // interp walk). Helper is defined after the multimethod registry.
        class_methods.emplace(o->shape->names[i],
                              _jit_dispatcher_max_arity(cls));
      }
    };
    // Methods live on the class meta (proto), data fields on the
    // instance itself. Walk both so e.g. `to_s` defined on the class
    // is visible to the conformance check.
    walk_slots(obj);
    walk_slots(obj->proto);
    bool conforms = culebra::class_conforms_to_trait(class_methods, *trait);
    by_trait[trait_name] = conforms;
    return conforms;
  }
  return false;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_type_matches(
    int8_t tag, int64_t data, const char* expected) {
  return _culebra_type_matches_single(tag, data, std::string_view(expected));
}

// Whether a value satisfies a (possibly Union) type annotation. Empty / "Any"
// matches anything; a top-level `A | B` is any-of. Shared by the throwing
// `culebra_runtime_type_check` and the ns-method-as-value arg check so both
// decide membership identically.
inline bool _culebra_value_matches_type(int8_t tag, int64_t data,
                                        std::string_view expected) {
  if (expected.empty() || expected == "Any") return true;
  // Depth-aware gate so `Array<Long | Float>`'s inner `|` doesn't trigger a
  // Union split that's then never used.
  if (culebra::has_toplevel_pipe(expected)) {
    for (auto cand : culebra::split_union_types(expected))
      if (_culebra_type_matches_single(tag, data, cand)) return true;
    return false;
  }
  return _culebra_type_matches_single(tag, data, expected);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_type_check(
    int8_t tag, int64_t data, const char* expected, const char* context,
    int64_t line, int64_t col) {
  if (expected == nullptr || expected[0] == '\0') return;
  if (_culebra_value_matches_type(tag, data, std::string_view(expected))) return;
  throw culebra::CulebraError("TypeError", std::format(
      "type error: {} expects {}", context, expected), line, col);
}

// Forward declaration — defined with the call-site thread-locals below.
CULEBRA_RT_KEEP int64_t culebra_runtime_param_pos(int64_t idx,
                                                  int64_t def_line,
                                                  int64_t def_col);

// Typed-parameter check for the user-fn prologue: identical hot path to
// culebra_runtime_type_check, but the report position resolves on the
// FAILURE path only (interp-binder style: positional arg → its own
// expression via set_arg_pos, kwarg-/default-filled → the call site via
// set_call_site, def position as last resort). Keeping the resolution
// cold leaves the success path at exactly one runtime call per typed
// param — the pre-existing cost. Only valid while the thread-locals are
// still the caller's; params at/after the first default snapshot eagerly
// instead (see the prologue).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_type_check_param(
    int8_t tag, int64_t data, const char* expected, const char* context,
    int64_t idx, int64_t def_line, int64_t def_col) {
  if (expected == nullptr || expected[0] == '\0') return;
  if (_culebra_value_matches_type(tag, data, std::string_view(expected))) return;
  auto pos = _jit_unpack_pos(culebra_runtime_param_pos(idx, def_line, def_col));
  throw culebra::CulebraError("TypeError", std::format(
      "type error: {} expects {}", context, expected),
      pos.line, pos.col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_div_zero(int64_t line,
                                                           int64_t col) {
  throw culebra::CulebraError("ZeroDivisionError", "divide by 0 error",
                              line, col);
}

// --- Numeric runtime helpers (Float-aware arithmetic slow paths) ---
//
// The binary-op step / power algorithm both engines fixed on. The JIT
// emits an inline "both Long" fast path and only calls these when at
// least one operand is Float or the types are mixed.

// Releases its values IFF the enclosing scope exits via C++ exception unwind
// (std::uncaught_exceptions grew since construction) — the single RAII form of
// the callee-cleans-on-throw convention.
// Arm it over the helper's whole body, user dispatch included: the invoke
// helpers only borrow what they are handed (they mint the callee's own +1), so
// a dispatch throw strands the caller's temps exactly like a direct error does
// and this guard is the one releaser for both. A value that needs no cleanup
// on some path is passed as nil
// (releasing nil is a no-op), which models conditional-ownership gates like
// `own_receiver`. Values release in list order.
struct JitUnwindRelease {
  JitValue v[3];
  int n = 0;
  int exc = std::uncaught_exceptions();
  JitUnwindRelease(std::initializer_list<JitValue> vals) {
    assert(vals.size() <= 3);
    for (auto& x : vals) v[n++] = x;
  }
  JitUnwindRelease(const JitUnwindRelease&) = delete;
  JitUnwindRelease& operator=(const JitUnwindRelease&) = delete;
  ~JitUnwindRelease() {
    if (std::uncaught_exceptions() <= exc) return;
    for (int i = 0; i < n; i++)
      culebra_runtime_value_release(v[i].tag, v[i].data);
  }
};

// Invoke a method closure with an explicit `self`. Returns the +1 result.
//
// The values passed in are BORROWED: each helper mints the +1 the
// callee-consumes ABI requires, and the callee consumes it on EVERY exit — a
// compiled frame releases its slots on a normal return and through its
// fn-level cleanup pad on a throw, and native endpoints hold theirs in RAII
// (see _jit_bound_method_thunk). So these helpers are refcount-neutral on both
// paths, and a caller may hand them elements it only borrows (an array's slots
// during a sort, a dict's stored keys during a lookup) without the user body's
// throw corrupting the owner's refcounts. A caller holding a `+1` temp of its
// own releases that temp itself on the unwind edge.
inline JitValue _culebra_invoke_method1(JitClosure* cls, JitValue self,
                                        JitValue arg) {
  culebra_runtime_value_retain(self.tag, self.data);
  culebra_runtime_value_retain(arg.tag, arg.data);
  JitValue args[1] = {arg};
  return _jit_invoke(cls, self, 1, args);
}

inline JitValue _culebra_invoke_method0(JitClosure* cls, JitValue self) {
  culebra_runtime_value_retain(self.tag, self.data);
  return _jit_invoke(cls, self, 0, nullptr);
}

inline JitValue _culebra_invoke_method2(JitClosure* cls, JitValue self,
                                        JitValue a1, JitValue a2) {
  culebra_runtime_value_retain(self.tag, self.data);
  culebra_runtime_value_retain(a1.tag, a1.data);
  culebra_runtime_value_retain(a2.tag, a2.data);
  JitValue args[2] = {a1, a2};
  return _jit_invoke(cls, self, 2, args);
}

// Resolve user-defined `hash()` on an Object (Hashable structural
// conformance). Returns the Long payload on success; nullopt when the
// method is absent or returns a non-Long value (caller falls back to
// reference identity).
inline std::optional<int64_t> _jit_object_user_hash(JitObject* obj) {
  auto* entry = _find_property(obj, "hash");
  if (!entry || entry->value.tag != TAG_FUNC) return std::nullopt;
  auto* cls = reinterpret_cast<JitClosure*>(entry->value.data);
  auto r = _culebra_invoke_method0(
      cls, {TAG_OBJECT, reinterpret_cast<int64_t>(obj)});
  if (r.tag != TAG_LONG) {
    _culebra_value_release_impl(r.tag, r.data);
    return std::nullopt;
  }
  return r.data;
}

// Resolve user-defined `eq(other)` on a pair of Objects (Eq structural
// conformance). Both sides must expose `eq`; otherwise nullopt so the
// caller keeps reference equality. The return is coerced via `to_bool`
// semantics (Bool/Long/Float), matching the interpreter's `_invoke_user_eq`
// so a non-Bool `eq` agrees across backends (and, like interp, throws on a
// non-coercible return).
inline std::optional<bool> _jit_object_user_eq(JitObject* a, JitObject* b) {
  auto* ea = _find_property(a, "eq");
  auto* eb = _find_property(b, "eq");
  if (!ea || !eb || ea->value.tag != TAG_FUNC || eb->value.tag != TAG_FUNC) {
    return std::nullopt;
  }
  auto* cls = reinterpret_cast<JitClosure*>(ea->value.data);
  return _extract_bool_and_release(_culebra_invoke_method1(
      cls, {TAG_OBJECT, reinterpret_cast<int64_t>(a)},
      {TAG_OBJECT, reinterpret_cast<int64_t>(b)}));
}

// Look up a Function-typed property on a JitObject by name.
// Walk own props then (optionally) the proto chain (one level) for a
// matching property. Returns nullptr if absent. Shared helper for
// every property reader on the JIT side; keeps the proto-fallthrough
// rule in a single place.
inline const JitObjectEntry* _find_property(JitObject* obj,
                                             const char* key) {
  auto idx = obj->find_slot(key);
  if (idx != static_cast<size_t>(-1)) return &obj->slots[idx];
  if (obj->proto) {
    idx = obj->proto->find_slot(key);
    if (idx != static_cast<size_t>(-1)) return &obj->proto->slots[idx];
  }
  return nullptr;
}

inline JitClosure* _lookup_special(int8_t tag, int64_t data, const char* name) {
  if (tag != TAG_OBJECT) return nullptr;
  auto* obj = reinterpret_cast<JitObject*>(data);
  auto* entry = _find_property(obj, name);
  if (!entry || entry->value.tag != TAG_FUNC) return nullptr;
  return reinterpret_cast<JitClosure*>(entry->value.data);
}

// Invoke a special method `recv.<name>(arg)`. Returns the +1 result
// or std::nullopt.
inline std::optional<JitValue> _try_special_binop(int8_t rt, int64_t rd,
                                                 int8_t at, int64_t ad,
                                                 const char* name) {
  auto* cls = _lookup_special(rt, rd, name);
  if (!cls) return std::nullopt;
  return _culebra_invoke_method1(cls, {rt, rd}, {at, ad});
}

inline std::optional<JitValue> _try_special_unary(int8_t t, int64_t d,
                                                 const char* name) {
  auto* cls = _lookup_special(t, d, name);
  if (!cls) return std::nullopt;
  return _culebra_invoke_method0(cls, {t, d});
}

// Invoke `__str__` on an Object, copying the returned String into an
// owned std::string and releasing the method's +1 return. Returns
// nullopt for non-Objects or Objects without `__str__`; throws on
// non-String returns so a buggy method fails loudly.
inline std::optional<std::string> _try_str_special(int8_t type, int64_t data) {
  auto r = _try_special_unary(type, data, "__str__");
  if (!r) return std::nullopt;
  if (r->tag != TAG_STRING && r->tag != TAG_STRINGVIEW) {
    _culebra_value_release_impl(r->tag, r->data);
    throw culebra::CulebraError("TypeError",
                                "__str__ must return a String");
  }
  std::string out(_culebra_str_view(r->tag, r->data));
  _culebra_value_release_impl(r->tag, r->data);
  return out;
}

// Arithmetic binop: try `lhs.__op__(rhs)`; if `reflect` is true and
// nothing matched, try `rhs.__op__(lhs)` (commutative auto-reflection
// for `+` and `*`). Callers fall back to the numeric path otherwise.
inline std::optional<JitValue> _dispatch_arith_special(int8_t lt, int64_t ld,
                                                     int8_t rt, int64_t rd,
                                                     const char* name,
                                                     bool reflect) {
  if (auto r = _try_special_binop(lt, ld, rt, rd, name)) return r;
  if (reflect) {
    if (auto r = _try_special_binop(rt, rd, lt, ld, name)) return r;
  }
  return std::nullopt;
}

// Forward decl — body is further down with the other Tensor runtime entries.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_binop(
    int8_t lt, int64_t ld, int8_t rt_, int64_t rd, int64_t op_id);

// Routes binop through the Tensor runtime when either operand is a
// Tensor. M2.0 requires both operands to be Tensor; scalar broadcast
// lands in M2.1.
inline std::optional<JitValue> _try_tensor_binop(
    int8_t lt, int64_t ld, int8_t rt, int64_t rd, int op_id, const char* op,
    int64_t line, int64_t col) {
  if (lt != TAG_TENSOR && rt != TAG_TENSOR) return std::nullopt;
  // Only Tensor and numeric operands lift. Anything else is the same
  // canonical arithmetic type error the interp reports, not the terse one
  // `_culebra_coerce_num` would raise from inside the lift.
  auto liftable = [](int8_t t) {
    return t == TAG_TENSOR || t == TAG_LONG || t == TAG_FLOAT;
  };
  if (!liftable(lt) || !liftable(rt)) {
    culebra::throw_arith_type_error(op, _culebra_tag_name(lt),
                                    _culebra_tag_name(rt), line, col);
  }
  auto* t = culebra_runtime_tensor_binop(lt, ld, rt, rd, op_id);
  return JitValue{TAG_TENSOR, reinterpret_cast<int64_t>(t)};
}

// `culebra_runtime_num_<name>_borrow` is the full semantics WITHOUT
// touching the operands' refs — the lowering's contract, where operands
// stay owned by the frame's registers and a try handler's release ladder
// is the one releaser.
#define CUL_NUM_BINOP(name, opstr, method, expr, reflect, op_id)        \
  CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue                            \
  culebra_runtime_num_##name##_borrow(                                  \
      int8_t lt, int64_t ld, int8_t rt, int64_t rd,                     \
      int64_t line, int64_t col) {                                      \
    if (auto r = _try_tensor_binop(lt, ld, rt, rd, op_id, opstr, line,  \
                                   col))                                \
      return *r;                                                        \
    if (auto r = _dispatch_arith_special(lt, ld, rt, rd, method,        \
                                         reflect))                      \
      return *r;                                                        \
    _arith_guard_numeric(opstr, lt, rt, line, col);                     \
    auto a = _culebra_coerce_num(lt, ld);                               \
    auto b = _culebra_coerce_num(rt, rd);                               \
    return {TAG_FLOAT, _culebra_double_to_bits(expr)};                  \
  }
CUL_NUM_BINOP(sub, "-", "__sub__", a - b, false, static_cast<int>(culebra::Op::Sub))
CUL_NUM_BINOP(mul, "*", "__mul__", a * b, true,  static_cast<int>(culebra::Op::Mul))
#undef CUL_NUM_BINOP

// Forward decl — body is further down with the other Array runtime entries.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_array_concat(
    JitArray* a, JitArray* b);

// `+` is the only arithmetic op that also concatenates, so it gets a
// hand-written body instead of CUL_NUM_BINOP. String / StringView operands on
// both sides yield a new owned String, two Arrays a new Array; mixed types
// (e.g. String + Long) fall through to _culebra_coerce_num, which throws
// TypeError — use interpolation `"{x}"` for those.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_num_add_borrow(
    int8_t lt, int64_t ld, int8_t rt, int64_t rd, int64_t line, int64_t col) {
  if (auto r = _try_tensor_binop(lt, ld, rt, rd,
                                  static_cast<int>(culebra::Op::Add), "+",
                                  line, col))
    return *r;
  if (auto r = _dispatch_arith_special(lt, ld, rt, rd, "__add__", true))
    return *r;
  bool ls = (lt == TAG_STRING || lt == TAG_STRINGVIEW);
  bool rs = (rt == TAG_STRING || rt == TAG_STRINGVIEW);
  if (ls && rs) {
    return {TAG_STRING,
            reinterpret_cast<int64_t>(culebra_runtime_str_concat(
                culebra_runtime_strlike_to_cstr(lt, ld),
                culebra_runtime_strlike_to_cstr(rt, rd)))};
  }
  if (lt == TAG_ARRAY && rt == TAG_ARRAY) {
    return {TAG_ARRAY,
            reinterpret_cast<int64_t>(culebra_runtime_array_concat(
                reinterpret_cast<JitArray*>(ld),
                reinterpret_cast<JitArray*>(rd)))};
  }
  _arith_guard_numeric("+", lt, rt, line, col);
  auto a = _culebra_coerce_num(lt, ld);
  auto b = _culebra_coerce_num(rt, rd);
  return {TAG_FLOAT, _culebra_double_to_bits(a + b)};
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_num_div_borrow(
    int8_t lt, int64_t ld, int8_t rt, int64_t rd, int64_t line, int64_t col) {
  if (auto r = _try_tensor_binop(lt, ld, rt, rd,
                                  static_cast<int>(culebra::Op::Div), "/",
                                  line, col))
    return *r;
  if (auto r = _dispatch_arith_special(lt, ld, rt, rd, "__div__", false))
    return *r;
  _arith_guard_numeric("/", lt, rt, line, col);
  auto a = _culebra_coerce_num(lt, ld);
  auto b = _culebra_coerce_num(rt, rd);
  if (b == 0.0) throw culebra::CulebraError("ZeroDivisionError", "divide by 0 error");
  return {TAG_FLOAT, _culebra_double_to_bits(a / b)};
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_num_mod_borrow(
    int8_t lt, int64_t ld, int8_t rt, int64_t rd, int64_t line, int64_t col) {
  if (auto r = _dispatch_arith_special(lt, ld, rt, rd, "__mod__", false))
    return *r;
  _arith_guard_numeric("%", lt, rt, line, col);
  auto a = _culebra_coerce_num(lt, ld);
  auto b = _culebra_coerce_num(rt, rd);
  if (b == 0.0) throw culebra::CulebraError("ZeroDivisionError", "divide by 0 error");
  return {TAG_FLOAT, _culebra_double_to_bits(std::fmod(a, b))};
}

// `@` (matmul) has no numeric meaning — always dispatches through
// `__matmul__`. Non-commutative, so no reflection.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_num_matmul_borrow(
    int8_t lt, int64_t ld, int8_t rt, int64_t rd, int64_t line, int64_t col) {
  if (auto r = _try_special_binop(lt, ld, rt, rd, "__matmul__")) return *r;
  culebra::throw_arith_type_error("@", _culebra_tag_name(lt),
                                  _culebra_tag_name(rt), line, col);
}

// Extract a boolean from a special method's +1 return value and release
// the (potentially heap-backed) result. Matches `Value::to_bool`:
// accepts Bool/Long/Float and throws `type error` on anything else, so
// a comparison method that forgets to return a boolean fails loudly.
inline bool _extract_bool_and_release(JitValue v) {
  bool b;
  if (v.tag == TAG_BOOL) b = v.data != 0;
  else if (v.tag == TAG_LONG) b = v.data != 0;
  else if (v.tag == TAG_FLOAT) b = _culebra_float_to_double(v.data) != 0.0;
  else {
    // Match interp's `Value::to_bool()` message so cross-backend
    // try/catch text comparisons stay aligned.
    auto got = std::string(_culebra_tag_name(v.tag));
    _culebra_value_release_impl(v.tag, v.data);
    throw culebra::CulebraError("TypeError",
        culebra::type_mismatch_message("Bool, Long, or Float", got));
  }
  _culebra_value_release_impl(v.tag, v.data);
  return b;
}

// extern "C" entry points for the comparison helpers. One per
// operator so the JIT can look them up by name. The `_borrow` twin never
// touches the operands' refs (the bytecode VM's contract).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_value_equal_borrow(
    int8_t t1, int64_t d1, int8_t t2, int64_t d2) {
  // The dispatches below are calls with no codegen call site; both backends
  // publish the operator's position here before entering (the same hook the
  // positionless backfill reads), so lend it to them.
  JitBorrowedCallSite site{_jit_op_line, _jit_op_col};
  // `==` is commutative, so try either side's `__eq__`.
  if (auto r = _try_special_binop(t1, d1, t2, d2, "__eq__"))
    return _extract_bool_and_release(*r);
  if (auto r = _try_special_binop(t2, d2, t1, d1, "__eq__"))
    return _extract_bool_and_release(*r);
  // Eq-trait fallback: route through a user/derived `eq(other)` so the
  // operator agrees with key equality (JitValueEq dispatches `eq` too).
  if (t1 == TAG_OBJECT && t2 == TAG_OBJECT) {
    if (auto e = _jit_object_user_eq(reinterpret_cast<JitObject*>(d1),
                                     reinterpret_cast<JitObject*>(d2)))
      return *e;
  }
  return _culebra_value_equal(t1, d1, t2, d2);
}

// Comparable-trait fallback: derive ordering from a user/derived
// `cmp(other)` when no `__lt__`/`__le__` dunder is present. Mirrors the
// interpreter's `try_cmp` in compare_values.
inline std::optional<int64_t> _special_cmp(int8_t t1, int64_t d1,
                                          int8_t t2, int64_t d2) {
  if (auto r = _try_special_binop(t1, d1, t2, d2, "cmp")) {
    if (r->tag == TAG_LONG) return r->data;
    _culebra_value_release_impl(r->tag, r->data);
  }
  return std::nullopt;
}

// Try `lhs.__le__(rhs)`, falling back to `__lt__` || `__eq__` to match
// the interpreter's derivation when a class only defines `__lt__`.
inline std::optional<bool> _special_le(int8_t t1, int64_t d1,
                                      int8_t t2, int64_t d2) {
  if (auto r = _try_special_binop(t1, d1, t2, d2, "__le__"))
    return _extract_bool_and_release(*r);
  // Both dunders run before either return is coerced (interp's le_as_bool does
  // the same, so the side-effect order matches). That leaves one `+1` result
  // in flight across the next step, and only _extract_bool_and_release
  // consumes one — so each waiting result gets a guard until its own coercion
  // takes it over. A dunder returning a non-Bool heap value strands it
  // otherwise.
  auto lt = _try_special_binop(t1, d1, t2, d2, "__lt__");
  std::optional<JitValue> eq;
  {
    JitUnwindRelease g{lt ? *lt : JitValue{TAG_NIL, 0}};
    eq = _try_special_binop(t1, d1, t2, d2, "__eq__");
  }
  if (!lt && !eq) return std::nullopt;
  bool l;
  {
    JitUnwindRelease g{eq ? *eq : JitValue{TAG_NIL, 0}};
    l = lt && _extract_bool_and_release(*lt);
  }
  bool e = eq && _extract_bool_and_release(*eq);
  return l || e;
}

// Each ordering operator expands to a borrow-contract core plus its named
// export:
//
//   _value_<name>_borrow  — the full `<`/`<=`/… semantics
//     (user `__lt__`/`__le__`/`cmp` dispatch, then numeric/String/Nil ordering)
//     raising the canonical compare type error WITHOUT touching the operands'
//     refs. The sort comparators call this directly: the array still owns its
//     elements, so releasing them on an incomparable-element throw would corrupt
//     its refcounts.
//
//   culebra_runtime_value_<name>_borrow  — the same core under the extern name
//     the lowering emits calls to.
// The `fast_path` dispatches to a user method with no codegen call site, so
// each expansion lends it the operator's own position (JitBorrowedCallSite).
#define CUL_DEF_ORD_OP(name, cmp_op, fast_path)                         \
  inline bool _value_##name##_borrow(                                   \
      int8_t t1, int64_t d1, int8_t t2, int64_t d2,                     \
      int64_t line, int64_t col) {                                      \
    { JitBorrowedCallSite site{line, col}; fast_path }                  \
    return _culebra_value_ord(t1, d1, t2, d2,                           \
                              [](double a, double b) { return a cmp_op b; }, \
                              line, col);                               \
  }                                                                     \
  CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool                                \
  culebra_runtime_value_##name##_borrow(                                \
      int8_t t1, int64_t d1, int8_t t2, int64_t d2,                     \
      int64_t line, int64_t col) {                                      \
    return _value_##name##_borrow(t1, d1, t2, d2, line, col);           \
  }
CUL_DEF_ORD_OP(less, <,
  if (auto r = _try_special_binop(t1, d1, t2, d2, "__lt__"))
    return _extract_bool_and_release(*r);
  if (auto c = _special_cmp(t1, d1, t2, d2)) return *c < 0;
)
CUL_DEF_ORD_OP(leq, <=,
  if (auto r = _special_le(t1, d1, t2, d2)) return *r;
  if (auto c = _special_cmp(t1, d1, t2, d2)) return *c <= 0;
)
// a > b ≡ !(a <= b)
CUL_DEF_ORD_OP(greater, >,
  if (auto r = _special_le(t1, d1, t2, d2)) return !*r;
  if (auto c = _special_cmp(t1, d1, t2, d2)) return *c > 0;
)
// a >= b ≡ !(a < b)
CUL_DEF_ORD_OP(geq, >=,
  if (auto r = _try_special_binop(t1, d1, t2, d2, "__lt__"))
    return !_extract_bool_and_release(*r);
  if (auto c = _special_cmp(t1, d1, t2, d2)) return *c >= 0;
)
#undef CUL_DEF_ORD_OP

// Power with full Python-style semantics:
//   Long ** non-negative Long  → Long (exp-by-squaring, wraps)
//   Long ** negative Long      → Float (promotes via std::pow)
//   any Float                  → Float
// In-place Tensor compound assignment: `lhs OP= rhs`. When lhs is a
// Tensor that owns its storage and the op is + - * / **, mutate lhs's
// buffer directly (saves the per-step allocation in SGD-style loops).
// Falls back to the regular binop helper for non-Tensor lhs or when
// the in-place precondition fails (so the caller's ABI is identical
// to the regular num_OP helper).
inline JitValue _try_tensor_inplace(int8_t lt, int64_t ld,
                                    int8_t rt, int64_t rd,
                                    culebra::Op op) {
  auto* lhs_t = reinterpret_cast<JitTensor*>(ld);
  culebra::TensorPtr rhs;
  if (rt == TAG_TENSOR) {
    rhs = reinterpret_cast<JitTensor*>(rd)->impl;
  } else {
    // A non-liftable rhs declines here rather than throwing, so the regular
    // binop helper below is the single source of the type error's wording.
    if (rt != TAG_LONG && rt != TAG_FLOAT) return {TAG_NIL, 0};
    rhs = culebra::tensor_scalar(_culebra_coerce_num(rt, rd),
                                 lhs_t->impl->dtype);
  }
  if (culebra::tensor_inplace_binop(*lhs_t->impl, op, std::move(rhs))) {
    culebra_runtime_value_retain(lt, ld);
    return {lt, ld};
  }
  return {TAG_NIL, 0};  // sentinel: in-place did not run
}

// The `_borrow` register-ownership contract, like CUL_NUM_BINOP (the
// Tensor still mutates in place — _try_tensor_inplace only mints the +1
// result ref).
#define CUL_NUM_INPLACE(name, op_enum)                                  \
  CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue                                 \
  culebra_runtime_num_inplace_##name##_borrow(                          \
      int8_t lt, int64_t ld, int8_t rt, int64_t rd,                     \
      int64_t line, int64_t col) {                                      \
    if (lt == TAG_TENSOR) {                                             \
      auto r = _try_tensor_inplace(lt, ld, rt, rd, culebra::op_enum);   \
      if (r.tag != TAG_NIL) return r;                                   \
    }                                                                   \
    return culebra_runtime_num_##name##_borrow(lt, ld, rt, rd, line,    \
                                               col);                    \
  }
CUL_NUM_INPLACE(add, Op::Add)
CUL_NUM_INPLACE(sub, Op::Sub)
CUL_NUM_INPLACE(mul, Op::Mul)
CUL_NUM_INPLACE(div, Op::Div)
// Pow expansion is further down — it needs `culebra_runtime_num_pow_borrow`.

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_num_pow_borrow(
    int8_t lt, int64_t ld, int8_t rt, int64_t rd, int64_t line, int64_t col) {
  if (auto r = _try_special_binop(lt, ld, rt, rd, "__pow__")) return *r;
  if (lt == TAG_LONG && rt == TAG_LONG) {
    int64_t a = ld, e = rd;
    if (e >= 0) return {TAG_LONG, culebra::ipow_nonneg(a, e)};
    if (a == 0)
      throw culebra::CulebraError("ZeroDivisionError", "divide by 0 error",
                                  static_cast<int>(line),
                                  static_cast<int>(col));
    return {TAG_FLOAT,
            _culebra_double_to_bits(std::pow(static_cast<double>(a),
                                             static_cast<double>(e)))};
  }
  _arith_guard_numeric("**", lt, rt, line, col);
  auto a = _culebra_coerce_num(lt, ld);
  auto b = _culebra_coerce_num(rt, rd);
  return {TAG_FLOAT, _culebra_double_to_bits(std::pow(a, b))};
}

CUL_NUM_INPLACE(pow, Op::Pow)
#undef CUL_NUM_INPLACE

// Unary negation: Long → Long (wraps), Float → Float. Non-numeric
// raises type error. Called only from the unary-minus slow path.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_num_neg_borrow(
    int8_t t, int64_t d, int64_t line, int64_t col) {
  if (auto r = _try_special_unary(t, d, "__neg__")) return *r;
  if (t == TAG_LONG) return {TAG_LONG, -d};
  if (t == TAG_FLOAT) {
    auto v = _culebra_float_to_double(d);
    return {TAG_FLOAT, _culebra_double_to_bits(-v)};
  }
  culebra::throw_type_mismatch("Long or Float", _culebra_tag_name(t),
                               line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_debugger_break(
    const char* path, int64_t line, int64_t col) {
  std::println(stderr, "\nBreak in {}:{}:{}", path, line, col);
  // Show a few lines of source around the break point, if we can open it
  std::ifstream ifs(path);
  if (ifs) {
    std::string src((std::istreambuf_iterator<char>(ifs)),
                    std::istreambuf_iterator<char>());
    std::vector<std::string> lines;
    std::string cur;
    for (auto c : src) {
      if (c == '\n') { lines.push_back(cur); cur.clear(); }
      else cur += c;
    }
    if (!cur.empty()) lines.push_back(cur);

    auto start = line > 2 ? static_cast<size_t>(line) - 3 : 0;
    auto end = std::min(lines.size(),
                        static_cast<size_t>(line) + 2);
    for (auto i = start; i < end; i++) {
      auto marker = (i + 1 == static_cast<size_t>(line)) ? ">" : " ";
      std::println(stderr, "{} {:>4} {}", marker, i + 1, lines[i]);
    }
  }
  std::print(stderr, "\ndebug> ");
  std::string s;
  std::getline(std::cin, s);
  // Any input continues execution (minimal: no commands yet)
}

// --- Array runtime ---

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_array_new() {
  auto* arr = new JitArray();
  arr->refcount = 1;
  arr->size = 0;
  arr->capacity = 0;
  arr->items = nullptr;
  _gc_register(arr, GC_TAG_ARRAY);
  return arr;
}

// Allocate an Array with `capacity` slots already reserved. Used by
// inlined HOF loops (see emit_inlined_array_map) so per-iteration
// `array_push` doesn't re-grow the buffer log(N) times. `size`
// stays 0 — push fills it as elements arrive.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_array_new_reserved(
    int64_t capacity) {
  auto* arr = new JitArray();
  arr->refcount = 1;
  arr->size = 0;
  arr->capacity = capacity > 0 ? static_cast<size_t>(capacity) : 0;
  arr->items = capacity > 0 ? new JitValue[arr->capacity] : nullptr;
  _gc_register(arr, GC_TAG_ARRAY);
  return arr;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_set_or_push(
    JitArray* arr, int64_t idx, int8_t tag, int64_t data);

// Tuple sits on the same JitArray storage but is registered under
// GC_TAG_TUPLE so the collector's per-type dispatch treats it as a Tuple.
// Caller-visible immutability is enforced by the IR (mutation ops only
// accept TAG_ARRAY) — there is no mutating runtime entry for Tuple.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_tuple_new() {
  auto* arr = new JitArray();
  arr->refcount = 1;
  arr->size = 0;
  arr->capacity = 0;
  arr->items = nullptr;
  _gc_register(arr, GC_TAG_TUPLE);
  return arr;
}

// Build-time element append. Reuses array_push internals (forward
// declared and defined below) because the layout is identical; only
// the GC tag differs.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_push(JitArray* arr,
                                                             int8_t tag,
                                                             int64_t data);
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_tuple_push(JitArray* arr,
                                                             int8_t tag,
                                                             int64_t data) {
  culebra_runtime_array_push(arr, tag, data);
}

// Set runtime: insertion-ordered with O(1) membership.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitSet* culebra_runtime_set_new() {
  auto* s = new JitSet();
  s->refcount = 1;
  s->index = new JitSetIndex();
  _gc_register(s, GC_TAG_SET);
  return s;
}

// Add `value` to `set` unless already present. The set absorbs the
// +1 reference on hit; on a duplicate we release it so the caller's
// ownership transfer balances out.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_set_add(JitSet* set,
                                                          int8_t tag,
                                                          int64_t data) {
  JitValue v{tag, data};
  if (!set->index->insert(v).second) {
    _culebra_value_release_impl(tag, data);
    return;
  }
  set->members.push_back(v);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE bool culebra_runtime_set_contains(
    JitSet* set, int8_t tag, int64_t data, int64_t line, int64_t col) {
  return _jit_at_pos(line, col, [&] {
    return set->index && set->index->contains(JitValue{tag, data});
  });
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_set_size(JitSet* set) {
  return static_cast<int64_t>(set->members.size());
}

// Returns a fresh +1 Set whose members are the union / intersection /
// (a - b) of `a` and `b`. Each member is retained once into the result.
// Common tail for set_union/intersect/diff: take +1 ownership of `v`
// into `out` if not already present. Source members are unique within
// their own set, so the dedup check is only meaningful for `union`.
inline void _set_take(JitSet* out, const JitValue& v) {
  if (!out->index->insert(v).second) return;
  culebra_runtime_value_retain(v.tag, v.data);
  out->members.push_back(v);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitSet* culebra_runtime_set_union(JitSet* a,
                                                               JitSet* b) {
  auto* out = culebra_runtime_set_new();
  for (auto& v : a->members) _set_take(out, v);
  for (auto& v : b->members) _set_take(out, v);
  return out;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitSet* culebra_runtime_set_intersect(JitSet* a,
                                                                   JitSet* b) {
  auto* out = culebra_runtime_set_new();
  for (auto& v : a->members) {
    if (b->index->contains(v)) _set_take(out, v);
  }
  return out;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitSet* culebra_runtime_set_diff(JitSet* a,
                                                              JitSet* b) {
  auto* out = culebra_runtime_set_new();
  for (auto& v : a->members) {
    if (!b->index->contains(v)) _set_take(out, v);
  }
  return out;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitSet* culebra_runtime_set_sym_diff(JitSet* a,
                                                                  JitSet* b) {
  auto* out = culebra_runtime_set_new();
  for (auto& v : a->members) {
    if (!b->index->contains(v)) _set_take(out, v);
  }
  for (auto& v : b->members) {
    if (!a->index->contains(v)) _set_take(out, v);
  }
  return out;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int8_t culebra_runtime_set_subset(JitSet* a,
                                                               JitSet* b) {
  for (auto& v : a->members) {
    if (!b->index->contains(v)) return 0;
  }
  return 1;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int8_t culebra_runtime_set_superset(JitSet* a,
                                                                 JitSet* b) {
  for (auto& v : b->members) {
    if (!a->index->contains(v)) return 0;
  }
  return 1;
}

// Mutating add: returns 1 on insert, 0 if already present. Hands the
// caller's +1 reference into the set on insert; releases it on dup.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int8_t culebra_runtime_set_add_method(
    JitSet* set, int8_t tag, int64_t data, int64_t line, int64_t col) {
  JitValue v{tag, data};
  // Hashing the element is where an unhashable one raises, and this helper
  // owns the `+1` it was handed on every exit — so the throw path is the
  // guard's (`({1, 2, 3}).add([1])` stranded the array otherwise).
  JitUnwindRelease g{v};
  bool inserted =
      _jit_at_pos(line, col, [&] { return set->index->insert(v).second; });
  if (!inserted) {
    _culebra_value_release_impl(tag, data);
    return 0;
  }
  set->members.push_back(v);
  return 1;
}

// Mutating remove: returns 1 if removed, 0 if absent.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int8_t culebra_runtime_set_remove(
    JitSet* set, int8_t tag, int64_t data, int64_t line, int64_t col) {
  JitValue key{tag, data};
  if (!_jit_at_pos(line, col, [&] { return set->index->erase(key); }))
    return 0;
  // Find and erase from the members vector (O(n) — same as the interp's
  // OrderedSymbolMap erase pattern).
  for (auto it = set->members.begin(); it != set->members.end(); ++it) {
    if (JitValueEq{}(*it, key)) {
      _culebra_value_release_impl(it->tag, it->data);
      set->members.erase(it);
      return 1;
    }
  }
  return 0;  // unreachable if index and members stayed in sync
}

// Materialize a Set as a fresh Array, retaining each element.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_set_to_array(
    JitSet* set) {
  auto* arr = culebra_runtime_array_new();
  for (auto& v : set->members) {
    culebra_runtime_value_retain(v.tag, v.data);
    culebra_runtime_array_push(arr, v.tag, v.data);
  }
  return arr;
}

// Materialize a Tuple as a fresh Array, retaining each element.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_tuple_to_array(
    JitArray* tup) {
  auto* arr = culebra_runtime_array_new();
  for (size_t i = 0; i < tup->size; i++) {
    auto& e = tup->items[i];
    culebra_runtime_value_retain(e.tag, e.data);
    culebra_runtime_array_push(arr, e.tag, e.data);
  }
  return arr;
}

// Tuple element search (linear). Returns 1 if `v` is present, 0 otherwise.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE int8_t culebra_runtime_tuple_contains(
    JitArray* tup, int8_t tag, int64_t data) {
  for (size_t i = 0; i < tup->size; i++) {
    if (_culebra_value_equal(tup->items[i].tag, tup->items[i].data,
                              tag, data)) return 1;
  }
  return 0;
}

}  // extern "C"

// Forward declaration for runtime helpers that need refcount logic
inline void _culebra_value_release_impl(int8_t tag, int64_t data);

extern "C" {

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_push(JitArray* arr,
                                                             int8_t tag,
                                                             int64_t data) {
  if (arr->size >= arr->capacity) {
    size_t new_cap = arr->capacity * 2;
    if (new_cap < 8) new_cap = 8;
    auto* new_items = new JitValue[new_cap];
    if (arr->items) {
      std::memcpy(new_items, arr->items, arr->size * sizeof(JitValue));
      delete[] arr->items;
    }
    arr->items = new_items;
    arr->capacity = new_cap;
  }
  // Array absorbs ownership of the pushed value (+1).
  arr->items[arr->size].tag = tag;
  arr->items[arr->size].data = data;
  arr->size++;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_resize(
    JitArray* arr, int64_t count, int8_t def_tag, int64_t def_data,
    int64_t line, int64_t col) {
  // A negative count would wrap through the size_t casts below into an
  // endless fill loop (the interp guards the same way).
  if (count < 0) {
    throw culebra::CulebraError("ValueError", "array size must not be negative",
                                line, col);
  }
  // The default is BORROWED: every filled slot aliases it (interp shares the
  // same object too) and must own its own ref — array free releases each
  // slot, so an unretained alias over-releases (SIGSEGV with a heap default).
  while (arr->size < static_cast<size_t>(count)) {
    culebra_runtime_value_retain(def_tag, def_data);
    culebra_runtime_array_push(arr, def_tag, def_data);
  }
  while (static_cast<size_t>(count) < arr->size) {
    arr->size--;
    _culebra_value_release_impl(arr->items[arr->size].tag,
                                arr->items[arr->size].data);
  }
}

// Negative `idx` counts from the end, like `a[i]`. Returns false (leaving
// `idx` unnormalized) when out of range, shared by array_get's throw and
// array_get_default's fallback.
inline bool _culebra_normalize_array_index(JitArray* arr, int64_t& idx) {
  if (idx < 0) idx = static_cast<int64_t>(arr->size) + idx;
  return idx >= 0 && static_cast<size_t>(idx) < arr->size;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_get(JitArray* arr,
                                                            int64_t idx,
                                                            int8_t* out_tag,
                                                            int64_t* out_data,
                                                            int64_t line,
                                                            int64_t col) {
  if (!_culebra_normalize_array_index(arr, idx)) {
    throw culebra::CulebraError("IndexError", "index out of range", line, col);
  }
  *out_tag = arr->items[idx].tag;
  *out_data = arr->items[idx].data;
}

// a.get(i, fallback): read-only. Returns the element at `i` (negative
// counts from the end, like `a[i]`) retained +1, or `fallback` if out of
// range. Never throws. Consumes the fallback's +1 — mirroring
// Object.get_default's ownership contract.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_array_get_default(
    JitArray* arr, int64_t idx, int8_t ft, int64_t fd) {
  if (!_culebra_normalize_array_index(arr, idx)) {
    return JitValue{ft, fd};  // transfer the fallback's +1 to the caller
  }
  auto picked = arr->items[idx];
  culebra_runtime_value_retain(picked.tag, picked.data);
  _culebra_value_release_impl(ft, fd);  // fallback unused
  return picked;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_set(JitArray* arr,
                                                            int64_t idx,
                                                            int8_t tag,
                                                            int64_t data,
                                                            int64_t line,
                                                            int64_t col) {
  if (idx < 0 || static_cast<size_t>(idx) >= arr->size) {
    throw culebra::CulebraError("IndexError", "index out of range", line, col);
  }
  _culebra_value_release_impl(arr->items[idx].tag, arr->items[idx].data);
  arr->items[idx].tag = tag;
  arr->items[idx].data = data;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t culebra_runtime_array_size(JitArray* arr) {
  return static_cast<int64_t>(arr->size);
}

// Forward decl (defined later alongside the refcount runtime).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_value_retain(int8_t tag,
                                                               int64_t data);

// Create a new array by copying [start, start+len) from src. Each copied
// element is retained (new array holds another reference).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_array_slice(
    JitArray* src, int64_t start, int64_t len) {
  auto* r = culebra_runtime_array_new();
  for (int64_t i = 0; i < len; i++) {
    auto& e = src->items[start + i];
    culebra_runtime_value_retain(e.tag, e.data);
    culebra_runtime_array_push(r, e.tag, e.data);
  }
  return r;
}

// `a + b`: a fresh +1 Array holding another reference to every element of
// both sides. Shallow, like slice — the operands stay untouched (the IR's
// Owned handles release them).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_array_concat(
    JitArray* a, JitArray* b) {
  auto* r = culebra_runtime_array_new_reserved(
      static_cast<int64_t>(a->size + b->size));
  for (auto* src : {a, b}) {
    for (size_t i = 0; i < src->size; i++) {
      auto& e = src->items[i];
      culebra_runtime_value_retain(e.tag, e.data);
      culebra_runtime_array_push(r, e.tag, e.data);
    }
  }
  return r;
}

// `a.insert(i, x)` — the array absorbs the +1 on `x`, so an out-of-range `i`
// must release it (the guard) rather than strand it.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_insert(
    JitArray* arr, int64_t idx, int8_t tag, int64_t data, int64_t line,
    int64_t col) {
  JitUnwindRelease g{JitValue{tag, data}};
  size_t at = culebra::resolve_position_index(idx, arr->size,
                                              /*allow_end=*/true, line, col);
  culebra_runtime_array_push(arr, tag, data);  // grow, then shift into place
  for (size_t i = arr->size - 1; i > at; i--) arr->items[i] = arr->items[i - 1];
  arr->items[at] = JitValue{tag, data};
}

// `a.remove_at(i)` — the removed element's +1 moves to the caller, like pop.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_remove_at(
    JitArray* arr, int64_t idx, int8_t* out_tag, int64_t* out_data,
    int64_t line, int64_t col) {
  size_t at = culebra::resolve_position_index(idx, arr->size,
                                              /*allow_end=*/false, line, col);
  *out_tag = arr->items[at].tag;
  *out_data = arr->items[at].data;
  for (size_t i = at + 1; i < arr->size; i++) arr->items[i - 1] = arr->items[i];
  arr->size--;
}

// Slice `tag:data` by the range value `range_data` (a `{class:"Range",
// start, end, inclusive}` JitObject; an open start/end is stored Nil).
// Bounds are normalized by the shared _slice_bounds (so JIT/AOT stay
// symmetric with the interpreter): negative indices resolve from the end,
// an open start is 0 and an open end the length, `..=` includes the end,
// then both clamp to [0,len] with start>end yielding empty. Array/Tuple ->
// shallow JitArray copy (tag preserved); String/StringView -> byte-unit
// view into the same leak-bounded bytes. Other tags raise a TypeError.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_slice(
    int8_t tag, int64_t data, int64_t range_data,
    int8_t* out_tag, int64_t* out_data, int64_t line, int64_t col) {
  auto* ro = reinterpret_cast<JitObject*>(range_data);
  auto sv = _jit_slot_or_nil(ro, "start");
  auto ev = _jit_slot_or_nil(ro, "end");
  auto iv = _jit_slot_or_nil(ro, "inclusive");
  bool open_end = ev.tag == TAG_NIL;
  bool inclusive = !open_end && iv.tag == TAG_BOOL && iv.data != 0;
  int64_t lo = sv.tag == TAG_NIL ? 0 : sv.data;
  if (tag == TAG_ARRAY || tag == TAG_TUPLE) {
    auto* src = reinterpret_cast<JitArray*>(data);
    int64_t hi = open_end ? static_cast<int64_t>(src->size) : ev.data;
    auto [s, e] = culebra::_slice_bounds(lo, hi, inclusive, src->size);
    auto* r = culebra_runtime_array_slice(src, static_cast<int64_t>(s),
                                          static_cast<int64_t>(e - s));
    *out_tag = tag;
    *out_data = reinterpret_cast<int64_t>(r);
    return;
  }
  if (tag == TAG_STRING || tag == TAG_STRINGVIEW) {
    auto view = _culebra_str_view(tag, data);
    int64_t hi = open_end ? static_cast<int64_t>(view.size()) : ev.data;
    auto [s, e] = culebra::_slice_bounds(lo, hi, inclusive, view.size());
    auto* v = _culebra_heap_view(view.data() + s, e - s,
                                 _view_owner_base(tag, data));
    *out_tag = TAG_STRINGVIEW;
    *out_data = reinterpret_cast<int64_t>(v);
    return;
  }
  // Non-sliceable receiver: match the interpreter's eval_slice, which falls
  // through to `to_array()` and reports `expected Array, got <type>`.
  culebra::throw_type_mismatch("Array", _culebra_tag_name(tag), line, col);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_array_set_or_push(
    JitArray* arr, int64_t idx, int8_t tag, int64_t data) {
  if (static_cast<size_t>(idx) < arr->size) {
    _culebra_value_release_impl(arr->items[idx].tag, arr->items[idx].data);
    arr->items[idx].tag = tag;
    arr->items[idx].data = data;
  } else {
    culebra_runtime_array_push(arr, tag, data);
  }
}

// --- Tensor runtime ---

// args layout: [optional "f32" string, shape varargs OR single
// Array of Long]. Mirrors the interpreter's parse_tensor_dtype_prefix
// + parse_tensor_shape pair.
inline std::pair<culebra::Dtype, culebra::TensorShape>
_culebra_parse_tensor_ctor_args(const JitValue* args, int64_t n, int64_t line,
                                int64_t col) {
  auto type_err = [&]() {
    culebra::throw_type_error_at(line, col);
  };
  culebra::Dtype dt = culebra::Dtype::F32;
  int64_t off = 0;
  if (n > 0 && args[0].tag == TAG_STRING) {
    auto d = culebra::parse_dtype(reinterpret_cast<const char*>(args[0].data));
    if (!d) type_err();
    dt = *d;
    off = 1;
  }
  std::vector<int64_t> dims;
  auto push_long = [&](const JitValue& v) {
    if (v.tag != TAG_LONG) type_err();
    dims.push_back(v.data);
  };
  if (n - off == 1 && args[off].tag == TAG_ARRAY) {
    auto* a = reinterpret_cast<JitArray*>(args[off].data);
    for (size_t i = 0; i < a->size; i++) push_long(a->items[i]);
  } else {
    for (int64_t i = off; i < n; i++) push_long(args[i]);
  }
  return {dt, culebra::TensorShape(std::move(dims))};
}


inline JitTensor* _culebra_jit_tensor_register(culebra::TensorPtr impl) {
  auto* t = new JitTensor{1, -1, std::move(impl)};
  _gc_register(t, GC_TAG_TENSOR);
  return t;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_zeros(
    const JitValue* args, int64_t n, int64_t line, int64_t col) {
  auto [dt, shape] = _culebra_parse_tensor_ctor_args(args, n, line, col);
  return _culebra_jit_tensor_register(culebra::tensor_zeros(std::move(shape), dt));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_ones(
    const JitValue* args, int64_t n, int64_t line, int64_t col) {
  auto [dt, shape] = _culebra_parse_tensor_ctor_args(args, n, line, col);
  return _culebra_jit_tensor_register(culebra::tensor_ones(std::move(shape), dt));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_randn(
    const JitValue* args, int64_t n, int64_t line, int64_t col) {
  auto [dt, shape] = _culebra_parse_tensor_ctor_args(args, n, line, col);
  return _culebra_jit_tensor_register(culebra::tensor_randn(std::move(shape), dt));
}

// Tensor.from(arr): walk a 1D or 2D nested JitArray. M1 only F32; an
// optional dtype tag arrives in M2.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_from(
    JitArray* a, int64_t line, int64_t col) {
  auto type_err = [&]() {
    culebra::throw_type_error_at(line, col);
  };
  auto coerce = [&](const JitValue& v) {
    if (v.tag == TAG_LONG) return static_cast<double>(v.data);
    if (v.tag == TAG_FLOAT) return _culebra_float_to_double(v.data);
    type_err();
    return 0.0;
  };
  using culebra::TensorImpl;
  using culebra::TensorShape;
  using culebra::Dtype;
  if (a->size == 0) {
    return _culebra_jit_tensor_register(
        std::make_shared<TensorImpl>(TensorShape({0}), Dtype::F32));
  }
  if (a->items[0].tag == TAG_LONG || a->items[0].tag == TAG_FLOAT) {
    auto impl = std::make_shared<TensorImpl>(
        TensorShape({static_cast<int64_t>(a->size)}), Dtype::F32);
    auto* p = impl->data_as<float>();
    for (size_t i = 0; i < a->size; i++) {
      p[i] = static_cast<float>(coerce(a->items[i]));
    }
    return _culebra_jit_tensor_register(std::move(impl));
  }
  if (a->items[0].tag != TAG_ARRAY) type_err();
  auto* row0 = reinterpret_cast<JitArray*>(a->items[0].data);
  size_t cols = row0->size;
  size_t rows = a->size;
  auto impl = std::make_shared<TensorImpl>(
      TensorShape({static_cast<int64_t>(rows), static_cast<int64_t>(cols)}),
      Dtype::F32);
  auto* p = impl->data_as<float>();
  for (size_t i = 0; i < rows; i++) {
    if (a->items[i].tag != TAG_ARRAY) type_err();
    auto* row = reinterpret_cast<JitArray*>(a->items[i].data);
    if (row->size != cols) type_err();
    for (size_t j = 0; j < cols; j++) {
      p[i * cols + j] = static_cast<float>(coerce(row->items[j]));
    }
  }
  return _culebra_jit_tensor_register(std::move(impl));
}

// Tensor.concat([a, b, ...]) — stacks tensors along axis 0 (rows).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_concat(
    JitArray* a, int64_t line, int64_t col) {
  std::vector<culebra::TensorPtr> parts;
  parts.reserve(a->size);
  for (size_t i = 0; i < a->size; i++) {
    if (a->items[i].tag != TAG_TENSOR) culebra::throw_type_error_at(line, col);
    parts.push_back(reinterpret_cast<JitTensor*>(a->items[i].data)->impl);
  }
  return _culebra_jit_tensor_register(culebra::tensor_concat(std::move(parts)));
}

// .shape() — returns a fresh JitArray of Long.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_tensor_shape(
    JitTensor* t) {
  auto* a = culebra_runtime_array_new();
  for (auto d : t->impl->shape.dims) {
    culebra_runtime_array_push(a, TAG_LONG, d);
  }
  return a;
}

// Build a lazy elementwise binop. At least one operand must be
// TAG_TENSOR; scalars (Long/Float) are lifted to a rank-0 Tensor with
// the other side's dtype, then broadcast handles the rest.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_binop(
    int8_t lt, int64_t ld, int8_t rt_, int64_t rd, int64_t op_id) {
  culebra::Dtype dt =
      reinterpret_cast<JitTensor*>(lt == TAG_TENSOR ? ld : rd)->impl->dtype;
  auto lift = [&](int8_t tag, int64_t data) -> culebra::TensorPtr {
    if (tag == TAG_TENSOR) return reinterpret_cast<JitTensor*>(data)->impl;
    return culebra::tensor_scalar(_culebra_coerce_num(tag, data), dt);
  };
  return _culebra_jit_tensor_register(culebra::tensor_binop(
      static_cast<culebra::Op>(op_id), lift(lt, ld), lift(rt_, rd)));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_tensor_eval_one(
    JitTensor* t) {
  culebra::tensor_eval_node(*t->impl);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_transpose(
    JitTensor* t) {
  return _culebra_jit_tensor_register(culebra::tensor_transpose(t->impl));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_clone(
    JitTensor* t) {
  return _culebra_jit_tensor_register(culebra::tensor_clone(t->impl));
}

// --- Autograd. Methods that conceptually return the receiver (mark /
// backward / clear) mutate the shared TensorImpl and hand back a fresh
// +1 JitTensor over the *same* impl, so refcounting stays uniform with
// every other tensor runtime entry (no aliasing of the receiver). ---
// `requires_grad` / `backward` / `zero_grad` are chainable: each returns the
// receiver, not a second handle onto the same impl. Handing back a fresh
// wrapper would break identity (`w == w.requires_grad()` is true to the
// interpreter), so mint a ref on the receiver instead.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_requires_grad(
    JitTensor* t) {
  culebra::tensor_requires_grad(t->impl);
  t->refcount++;
  return t;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_grad(
    JitTensor* t) {
  return _culebra_jit_tensor_register(culebra::tensor_grad(t->impl));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_backward(
    JitTensor* t) {
  culebra::tensor_backward(t->impl);
  t->refcount++;
  return t;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_zero_grad(
    JitTensor* t) {
  culebra::tensor_zero_grad(t->impl);
  t->refcount++;
  return t;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_detach(
    JitTensor* t) {
  return _culebra_jit_tensor_register(culebra::tensor_detach(t->impl));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_slice(
    JitTensor* t, int64_t start, int64_t end) {
  return _culebra_jit_tensor_register(
      culebra::tensor_slice(t->impl, start, end));
}

// Forces eval and returns a Culebra Array. Rank 1 → flat Array of
// Float; rank 2 → Array of Array of Float. Higher ranks are not
// supported in Phase 1.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_tensor_to_array(
    JitTensor* t) {
  culebra::tensor_eval_node(*t->impl);
  const auto& impl = *t->impl;
  auto read_at = [&](int64_t flat_idx) -> int64_t {
    double v = static_cast<double>(impl.data_as<float>()[flat_idx]);
    return _culebra_double_to_bits(v);
  };
  const auto& dims = impl.shape.dims;
  if (dims.size() == 1) {
    auto* out = culebra_runtime_array_new();
    for (int64_t i = 0; i < dims[0]; i++) {
      culebra_runtime_array_push(out, TAG_FLOAT,
                                  read_at(i * impl.strides()[0]));
    }
    return out;
  }
  if (dims.size() == 2) {
    auto* out = culebra_runtime_array_new();
    for (int64_t i = 0; i < dims[0]; i++) {
      auto* row = culebra_runtime_array_new();
      for (int64_t j = 0; j < dims[1]; j++) {
        culebra_runtime_array_push(
            row, TAG_FLOAT,
            read_at(i * impl.strides()[0] + j * impl.strides()[1]));
      }
      culebra_runtime_array_push(out, TAG_ARRAY,
                                  reinterpret_cast<int64_t>(row));
    }
    return out;
  }
  throw culebra::CulebraError("ValueError",
      "Tensor.to_array: rank > 2 not supported.");
}

// Scalar exit point: forces eval and returns the lone element as a
// Float Value. Throws unless the tensor holds exactly one element.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_tensor_item(
    JitTensor* t) {
  culebra::tensor_eval_node(*t->impl);
  const auto& impl = *t->impl;
  if (impl.shape.num_elements() != 1) {
    throw culebra::CulebraError("ValueError",
        "Tensor.item: tensor does not hold exactly one element.");
  }
  double v = static_cast<double>(impl.data_as<float>()[0]);
  return {TAG_FLOAT, _culebra_double_to_bits(v)};
}

// Runs `fn` with autograd graph-building suppressed, returning whatever
// fn returns. The RAII guard restores the depth even if fn throws, so a
// propagated CulebraError leaves the no-grad nesting balanced.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_tensor_no_grad(
    JitClosure* fn) {
  culebra::TensorNoGradGuard guard;
  return _culebra_invoke0(fn);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_from_csv(
    const char* path) {
  return _culebra_jit_tensor_register(
      culebra::tensor_from_csv(std::string(path), culebra::Dtype::F32));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_dot(
    JitTensor* a, JitTensor* b) {
  return _culebra_jit_tensor_register(culebra::tensor_dot(a->impl, b->impl));
}

// Unary activations (sigmoid / relu / softmax). op_id selects which.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_unary(
    JitTensor* a, int64_t op_id) {
  return _culebra_jit_tensor_register(
      culebra::tensor_unary(static_cast<culebra::Op>(op_id), a->impl));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_linear_sigmoid(
    JitTensor* W, JitTensor* x, JitTensor* b) {
  return _culebra_jit_tensor_register(
      culebra::tensor_linear_sigmoid(W->impl, x->impl, b->impl));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_reduce_axis(
    JitTensor* t, int64_t op_id, int64_t axis) {
  return _culebra_jit_tensor_register(culebra::tensor_reduce_axis(
      static_cast<culebra::Op>(op_id), t->impl, axis));
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitValue culebra_runtime_tensor_reduce_all(
    JitTensor* t, int64_t op_id) {
  using culebra::Op;
  double v = 0.0;
  switch (static_cast<Op>(op_id)) {
    case Op::Sum:  v = culebra::tensor_reduce_all<Op::Sum>(t->impl); break;
    case Op::Mean: v = culebra::tensor_reduce_all<Op::Mean>(t->impl); break;
    case Op::Max:  v = culebra::tensor_reduce_all<Op::Max>(t->impl); break;
    default: throw std::runtime_error("tensor: invalid reduce op_id");
  }
  return {TAG_FLOAT, _culebra_double_to_bits(v)};
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitTensor* culebra_runtime_tensor_reshape(
    JitTensor* t, JitArray* dims) {
  std::vector<int64_t> new_dims;
  new_dims.reserve(dims->size);
  for (size_t i = 0; i < dims->size; i++) {
    if (dims->items[i].tag != TAG_LONG) {
      throw culebra::CulebraError("TypeError", "type error");
    }
    new_dims.push_back(dims->items[i].data);
  }
  return _culebra_jit_tensor_register(culebra::tensor_reshape(
      t->impl, culebra::TensorShape(std::move(new_dims))));
}


// --- Object runtime ---

CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_object_new() {
  auto* o = new JitObject();
  o->refcount = 1;
  _gc_register(o, GC_TAG_OBJECT);
  return o;
}

// Shallow-copy a class instance / object: a fresh object with the same shape,
// its own copy of the data slots (each field ref retained), and the shared
// per-class meta (`proto`, method table — not refcounted per instance). Scalars
// are copied independently; referenced heap values are aliased. This is the
// clone the effects runtime forks a suspended continuation from (`__eff_copy`),
// kept native so no method names its own class — a self-referential method
// would form a def_env cycle only the tracing backstop could reclaim.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitObject* culebra_runtime_eff_copy(
    JitObject* src) {
  auto* o = new JitObject();
  o->refcount = 1;
  o->proto = src->proto;   // shared class meta (methods)
  if (o->proto) {
    o->proto->refcount++;   // each instance holds a +1 (see dtor)
    o->cls = src->cls;      // and one on its class object
    if (o->cls) o->cls->refcount++;
  }
  o->shape = src->shape;   // interned, immortal
  o->slots = src->slots;   // per-field value copy
  for (auto& e : o->slots) {
    culebra_runtime_value_retain(e.value.tag, e.value.data);
  }
  o->is_class = src->is_class;
  _gc_register(o, GC_TAG_OBJECT);
  return o;
}

// --- Effects abort signal (spike) ---
// An aborting handler clause (one that never resumes) unwinds from the
// perform point back to its `handle` as a distinct C++ type. User try/catch
// must not observe it: compiled catches classify the in-flight exception via
// the pending carrier and rethrow anything unrecognised, so scope cleanups
// and defers still run on the way out while the signal keeps unwinding.
// The carried value is +1; the catcher takes ownership.
struct CulebraEffAbort {
  JitValue val;
};

// In-flight abort payloads, kept as GC roots for their entire unwind. A user
// throw's value lives in the Runtime's thrown carrier and is enumerated as a
// root (see _jit_gc_enumerate_roots); an abort's value lives ONLY inside the
// in-flight CulebraEffAbort C++ exception object, which sits off the scanned
// machine stack. A collect during the unwind (every allocation under
// GC_STRESS) would then find no root for it and sweep a traced-only payload
// (a String) or an unmarked container out from under the handle driver that is
// about to consume it — a rooting gap the conservative stack scan only masks
// by luck (it crashes on x86-64 Linux, survives on AArch64 macOS). Rooting the
// value here closes it. A stack, not a scalar: _guard_stack catches an abort,
// runs the abandoned frames' finalizers, then re-aborts, so a payload can be
// live while the next is pushed. Pushed in eff_abort, popped in eff_catch_abort
// — the single consumer that stops an abort (cleanup/user-catch landingpads
// only rethrow it, never consume) — so the two stay 1:1.
CULEBRA_RT_CORE_OWNED thread_local std::vector<JitValue> _eff_abort_inflight;

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_eff_abort(int8_t tag,
                                                                 int64_t data) {
  culebra_runtime_value_retain(tag, data);
  _eff_abort_inflight.push_back(JitValue{tag, data});
  throw CulebraEffAbort{JitValue{tag, data}};
}

// Run `fn`, catching only the abort signal. Returns a fresh [aborted, value]
// pair; the value slot takes over the +1 (from the invoke result or the
// signal). Any other exception keeps unwinding untouched.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_eff_catch_abort(
    JitClosure* fn) {
  int64_t aborted = 0;
  JitValue v;
  bool from_abort = false;
  try {
    v = _culebra_invoke0(fn);
  } catch (const CulebraEffAbort& a) {
    aborted = 1;
    v = a.val;
    from_abort = true;  // payload still on _eff_abort_inflight — keep it rooted
  }
  auto* pair = culebra_runtime_array_new();
  culebra_runtime_array_push(pair, TAG_BOOL, aborted);
  culebra_runtime_array_push(pair, v.tag, v.data);
  // Pop only now: array_new/array_push may collect, and until the payload is
  // stored in `pair` (itself stack-rooted here) the inflight entry is its sole
  // root. LIFO with eff_abort's push; this frame caught the abort it pops.
  if (from_abort) _eff_abort_inflight.pop_back();
  return pair;
}

// Build an Array from args[start..n) for binding to `__ARGS__`. Caller
// transferred +1 ownership of each arg via the stack-allocated slab;
// Array takes over (object_set-style: no extra retain, slot owns +1).
// Returns a fresh JitArray with refcount 1.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE JitArray* culebra_runtime_args_slice_to_array(
    const JitValue* args, int64_t start, int64_t n) {
  auto* a = culebra_runtime_array_new();
  for (int64_t i = start; i < n; i++) {
    culebra_runtime_array_push(a, args[i].tag, args[i].data);
  }
  return a;
}

// When the function body never references `__ARGS__`, the prologue
// skips building the overflow Array — but the caller still transferred
// +1 retains on every overflow arg via the slab, so those refs must
// be released to balance the call. Cheap loop, called only when
// `n_args > declaredArity`.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_release_overflow_args(
    const JitValue* args, int64_t start, int64_t n) {
  for (int64_t i = start; i < n; i++) {
    _culebra_value_release_impl(args[i].tag, args[i].data);
  }
}

// Validate the well-known-property contract (see shared.h)
// for a freshly-bound JIT value: must be a 0-arg Function. No-op for
// ordinary names. The arg's +1 is released before throwing — callers
// pass ownership and expect either store-into-slot or release.
inline void _culebra_check_well_known_prop(std::string_view name,
                                           int8_t tag, int64_t data) {
  if (!culebra::is_well_known_prop(name)) return;
  auto bad = [&]() {
    _culebra_value_release_impl(tag, data);
    culebra::throw_well_known_prop_contract_error(name);
  };
  if (tag != GC_TAG_FUNC) bad();
  auto* cls = reinterpret_cast<JitClosure*>(data);
  if (!cls || cls->arity != 0) bad();
}

// Overwrite an existing String-keyed object slot last-wins. Shared by the
// three set paths (plain / IC fast / IC slow). is_init (object-literal
// construction) bypasses the immutable gate and replaces the slot's mut
// flag, like the interp's `initialize`; otherwise an immutable slot raises
// ImmutableError. Caller owns the +1 on (tag, data); it's consumed here.
// check_wk runs the well-known contract check between the immutable gate
// and the store — the interp's `assign` order (ImmutableError wins, and a
// failed check leaves the old value in place). The IC paths pass false:
// codegen routes well-known literal names around the IC entirely.
inline void _jit_overwrite_slot(JitObjectEntry& entry,
                                           const char* key, int8_t tag,
                                           int64_t data, bool mut, bool is_init,
                                           int64_t line, int64_t col,
                                           bool check_wk = false) {
  if (!is_init && !entry.mut) {
    _culebra_value_release_impl(tag, data);
    throw culebra::CulebraError("ImmutableError", std::format(
        "immutable property '{}'", key), line, col);
  }
  if (check_wk) _culebra_check_well_known_prop(key, tag, data);
  _culebra_value_release_impl(entry.value.tag, entry.value.data);
  entry.value.tag = tag;
  entry.value.data = data;
  if (is_init) entry.mut = mut;
}

// Raise the well-known contract error at declaration execution time.
// Emitted by compile_class_decl when a well-known name has an overload
// set — the grouped dispatcher can't satisfy the 0-arg contract (the
// interp rejects it via the dispatcher's params check).
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_wk_contract_error(
    const char* name) {
  culebra::throw_well_known_prop_contract_error(name);
}

// Class-namespace fill (`new`, statics, static fields, markers): a plain
// immutable append mirroring the interp's `properties->emplace`. No
// well-known contract and no owned/drop registration — statics are a
// namespace, not part of the instance drop/iterator protocol.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_object_bind_static(
    JitObject* obj, const char* key, int8_t tag, int64_t data) {
  obj->set_or_append(key, JitValue{tag, data}, /*mut=*/false);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_object_set(
    JitObject* obj, const char* key, bool mut, int8_t tag, int64_t data,
    int64_t line, int64_t col, bool is_init = false) {
  // Shared.new views are immutable (handle construction writes its own
  // marker/method slots via set_or_append, not through here).
  if (obj->is_shared_val) {
    _culebra_value_release_impl(tag, data);
    throw culebra::CulebraError("ImmutableError",
                                "Shared values are immutable", line, col);
  }
  // Runtime chokepoint for the well-known contract (see shared.h): every
  // String-keyed bind lands here — codegen literal names, dynamic subscript
  // keys via object_set_any, and native builders (JSON.parse etc.) — so the
  // check can't be bypassed the way a codegen-only check could.
  auto idx = obj->find_slot(key);
  if (idx == static_cast<size_t>(-1)) {
    _culebra_check_well_known_prop(key, tag, data);
    obj->append_slot(key, JitValue{tag, data}, mut);
  } else {
    _jit_overwrite_slot(obj->slots[idx], key, tag, data, mut, is_init, line,
                        col, /*check_wk=*/true);
  }
  if (std::string_view(key) == "drop") _jit_owned_bind_drop(obj);
}

// --- @packable SharedBuffer: handle objects + raw bytes <-> JitValue -----
// SharedBuffer handles and packed views are plain JitObjects carrying
// hidden marker slots (the same scheme the interp uses); the native byte
// store lives in culebra::shared_buffer_registry(), referenced by id.
// These helpers are called from the object get/set/index runtime helpers
// below, mirroring the interp's eval_property / eval_array_reference hooks
// — same logical interception points in both backends.
inline bool _jit_is_packed_view(JitObject* obj) { return obj->is_packed_view; }
inline bool _jit_is_shared_buffer(JitObject* obj) {
  return obj->is_shared_buffer;
}

inline bool _jit_is_shared_val(JitObject* obj) { return obj->is_shared_val; }
// Shared.new view readers. The bodies live in sendable_jit.h and are reached
// through these hooks rather than by symbol: the generic get/index paths
// would otherwise name the SharedVal reader, and through it the whole
// SendNode / channel / serialize graph, in every binary — a hello-world AOT
// output carried ~115 KB of it. A view cannot exist before
// _jit_make_shared_val_view ran, and that constructor installs both hooks,
// so the reader is reachable only from the adapters that create views
// (Shared / Isolate / Parallel / Net) and a program naming none of them
// links none of it (namespace-group dead-stripping, stdlib_jit.h
// ns_groups()). A null hook on a shared view is a broken invariant, not a
// missing feature, and is reported as such.
extern "C++" {
namespace culebra {
using SharedValPropFn = JitValue (*)(JitObject*, const char*, int64_t,
                                     int64_t);
using SharedValIndexFn = JitValue (*)(JitObject*, int8_t, int64_t, int64_t,
                                      int64_t);
inline std::atomic<SharedValPropFn>& _jit_shared_val_prop_hook() {
  static std::atomic<SharedValPropFn> fn{nullptr};
  return fn;
}
inline std::atomic<SharedValIndexFn>& _jit_shared_val_index_hook() {
  static std::atomic<SharedValIndexFn> fn{nullptr};
  return fn;
}
[[noreturn]] inline void _jit_shared_val_unhooked(int64_t line, int64_t col) {
  throw CulebraError("InternalError",
                     "Shared value read before any Shared value was created",
                     line, col);
}
inline JitValue _jit_shared_val_prop(JitObject* view, const char* name,
                                     int64_t line, int64_t col) {
  auto fn = _jit_shared_val_prop_hook().load(std::memory_order_acquire);
  if (!fn) _jit_shared_val_unhooked(line, col);
  return fn(view, name, line, col);
}
inline JitValue _jit_shared_val_index(JitObject* view, int8_t key_tag,
                                      int64_t key_data, int64_t line,
                                      int64_t col) {
  auto fn = _jit_shared_val_index_hook().load(std::memory_order_acquire);
  if (!fn) _jit_shared_val_unhooked(line, col);
  return fn(view, key_tag, key_data, line, col);
}
}  // namespace culebra
}

// Decode field `f` from a record's raw bytes into a primitive JitValue.
inline JitValue _jit_packable_read_field(const uint8_t* base,
                                         const culebra::PackableField& f) {
  const uint8_t* p = base + f.offset;
  if (f.layout.is_fixed_string) {
    // `[len:i32][byte × N]` -> a String of the first `len` bytes.
    int32_t len; std::memcpy(&len, p, 4);
    const char* s = _culebra_heap_str(std::string_view(
        reinterpret_cast<const char*>(p + f.layout.data_offset),
        static_cast<size_t>(len)));
    return {TAG_STRING, reinterpret_cast<int64_t>(s)};
  }
  if (f.layout.is_bytes) {
    const char* s = _culebra_heap_str(std::string_view(
        reinterpret_cast<const char*>(p), f.layout.capacity));
    return {TAG_STRING, reinterpret_cast<int64_t>(s)};
  }
  if (f.layout.is_optional) {
    if (*p == 0) return {TAG_NIL, 0};
    return _jit_packable_read_field(
        p + f.layout.data_offset,
        culebra::PackableField::scalar(f.layout.elem_type));
  }
  if (f.layout.is_enum) {
    // `[tag:i32][payload]` -> the tagged variant instance (class/__enum +
    // positional `_0.._n` fields), matching culebra_runtime_build_variant.
    const auto* el = culebra::lookup_packable_enum(f.layout.elem_type);
    int32_t tag; std::memcpy(&tag, p, 4);
    if (!el || tag < 0 || tag >= static_cast<int32_t>(el->variants.size()))
      return {TAG_NIL, 0};
    const auto& var = el->variants[tag];
    auto* inst = culebra_runtime_object_new();
    culebra_runtime_object_set(inst, "class", false, TAG_STRING,
        reinterpret_cast<int64_t>(_intern_str(var.name)), 0, 0);
    culebra_runtime_object_set(inst, "__enum", false, TAG_STRING,
        reinterpret_cast<int64_t>(_intern_str(f.layout.elem_type)), 0, 0);
    const uint8_t* payload = p + el->payload_offset;
    for (size_t fi = 0; fi < var.fields.size(); fi++) {
      JitValue fv = _jit_packable_read_field(
          payload + var.fields[fi].offset,
          culebra::PackableField::scalar(var.fields[fi].type));
      auto fn = culebra::positional_field_name(fi);
      culebra_runtime_object_set(inst, fn.data(), false, fv.tag, fv.data, 0, 0);
    }
    for (auto& entry : inst->slots) entry.mut = true;
    return {TAG_OBJECT, reinterpret_cast<int64_t>(inst)};
  }
  if (f.type == "Float32") {
    float v; std::memcpy(&v, p, 4);
    return {TAG_FLOAT, _culebra_double_to_bits(static_cast<double>(v))};
  }
  if (f.type == "Float64" || f.type == "Float") {
    double v; std::memcpy(&v, p, 8);
    return {TAG_FLOAT, _culebra_double_to_bits(v)};
  }
  if (f.type == "Int8")  { int8_t  v; std::memcpy(&v, p, 1); return {TAG_LONG, static_cast<int64_t>(v)}; }
  if (f.type == "Int16") { int16_t v; std::memcpy(&v, p, 2); return {TAG_LONG, static_cast<int64_t>(v)}; }
  if (f.type == "Int32") { int32_t v; std::memcpy(&v, p, 4); return {TAG_LONG, static_cast<int64_t>(v)}; }
  if (f.type == "Int64" || f.type == "Long") {
    int64_t v; std::memcpy(&v, p, 8); return {TAG_LONG, v};
  }
  if (f.type == "Byte") { uint8_t v; std::memcpy(&v, p, 1); return {TAG_LONG, static_cast<int64_t>(v)}; }
  if (f.type == "Bool") { uint8_t v; std::memcpy(&v, p, 1); return {TAG_BOOL, v ? 1 : 0}; }
  return {TAG_NIL, 0};
}

// Move-only owned JitValue for runtime *helper* bodies — the C++-side twin of
// the codegen `Owned` handle (see the `Owned` struct further down). Releases
// its `+1` on scope exit — every exit path, INCLUDING C++ exception unwind —
// unless `consume()`d (handed to an out-param / return / a sink that takes
// ownership). The iterator/HOF helpers held raw `JitValue` across a throwing
// `_culebra_invoke*` and leaked on the throw path (leak-fuzzer P0); wrapping
// a helper's owned locals in this makes the throw path release for free. Also
// the single chokepoint for callee-consumes contracts (a @packable store, a
// native method's `self`/args — see `JitMethodSelf`/`JitMethodArgs`).
struct JitOwnedVal {
  JitValue v;
  bool owned;
  explicit JitOwnedVal(JitValue val) : v(val), owned(true) {}
  JitOwnedVal(int8_t t, int64_t d) : v{t, d}, owned(true) {}
  JitOwnedVal(JitOwnedVal&& o) noexcept : v(o.v), owned(o.owned) { o.owned = false; }
  JitOwnedVal& operator=(JitOwnedVal&& o) noexcept {  // releases the old value
    if (this != &o) {
      if (owned) _culebra_value_release_impl(v.tag, v.data);
      v = o.v;
      owned = o.owned;
      o.owned = false;
    }
    return *this;
  }
  JitOwnedVal(const JitOwnedVal&) = delete;
  JitOwnedVal& operator=(const JitOwnedVal&) = delete;
  JitValue borrow() const { return v; }            // read without consuming
  JitValue consume() { owned = false; return v; }  // hand the +1 onward
  ~JitOwnedVal() { if (owned) _culebra_value_release_impl(v.tag, v.data); }

  // Borrow -> +1, the runtime-helper twin of codegen's emit_borrow_to_owned. A
  // native adapter only borrows its arguments (the dispatcher releases them
  // on return), so one handed back as the *result* needs its own reference.
  static JitOwnedVal from_borrowed(JitValue val) {
    culebra_runtime_value_retain(val.tag, val.data);
    return JitOwnedVal(val);
  }
};

// Encode a primitive JitValue into field `f`'s raw bytes. Numeric coercion
// mirrors the interp (Long<->Float implicit). Pure borrow-and-write: it
// never releases `(tag, data)` — both the storing callers (which consume
// via an owned JitOwnedVal) and the key-encoding callers (FixedSet/Map probe,
// which must NOT consume) rely on that.
inline void _jit_packable_write_field(uint8_t* base,
                                      const culebra::PackableField& f,
                                      int8_t tag, int64_t data) {
  uint8_t* p = base + f.offset;
  if (f.layout.is_fixed_string) {
    if (tag != TAG_STRING && tag != TAG_STRINGVIEW) {
      throw culebra::CulebraError("TypeError", std::format(
          "FixedString field `{}` expects a String, got {}", f.name,
          _culebra_tag_name(tag)));
    }
    auto sv = _culebra_str_view(tag, data);
    if (sv.size() > f.layout.capacity) {
      throw culebra::CulebraError("CapacityError", std::format(
          "FixedString<{}> overflow: `{}` needs {} bytes", f.layout.capacity, f.name,
          sv.size()));
    }
    int32_t len = static_cast<int32_t>(sv.size());
    std::memcpy(p, &len, 4);
    std::memcpy(p + f.layout.data_offset, sv.data(), sv.size());
    return;
  }
  if (f.layout.is_bytes) {
    if (tag != TAG_STRING && tag != TAG_STRINGVIEW) {
      throw culebra::CulebraError("TypeError", std::format(
          "Bytes field `{}` expects a String, got {}", f.name,
          _culebra_tag_name(tag)));
    }
    auto sv = _culebra_str_view(tag, data);
    if (sv.size() != f.layout.capacity) {
      throw culebra::CulebraError("ValueError", std::format(
          "Bytes<{}> field `{}` expects exactly {} bytes, got {}", f.layout.capacity,
          f.name, f.layout.capacity, sv.size()));
    }
    std::memcpy(p, sv.data(), f.layout.capacity);
    return;
  }
  if (f.layout.is_optional) {
    if (tag == TAG_NIL) { *p = 0; return; }
    *p = 1;
    _jit_packable_write_field(p + f.layout.data_offset,
                              culebra::PackableField::scalar(f.layout.elem_type),
                              tag, data);
    return;
  }
  if (f.layout.is_enum) {
    const auto* el = culebra::lookup_packable_enum(f.layout.elem_type);
    JitObject* obj =
        (tag == TAG_OBJECT) ? reinterpret_cast<JitObject*>(data) : nullptr;
    auto en = obj ? _jit_enum_name(obj) : std::nullopt;
    if (!en || *en != f.layout.elem_type) {
      throw culebra::CulebraError("TypeError", std::format(
          "field `{}` expects a `{}` enum value, got {}", f.name, f.layout.elem_type,
          _culebra_tag_name(tag)));
    }
    int idx = el ? el->index_of(_jit_derived_class_tag(obj)) : -1;
    if (idx < 0) {
      throw culebra::CulebraError("TypeError", std::format(
          "field `{}`: unknown variant for enum `{}`", f.name, f.layout.elem_type));
    }
    int32_t vtag = static_cast<int32_t>(idx);
    std::memcpy(p, &vtag, 4);
    uint8_t* payload = p + el->payload_offset;
    const auto& var = el->variants[idx];
    for (size_t fi = 0; fi < var.fields.size(); fi++) {
      culebra::PackableField sf;
      sf.type = var.fields[fi].type;
      sf.offset = 0;
      size_t fs = obj->find_slot(culebra::positional_field_name(fi).data());
      JitValue fv = (fs != static_cast<size_t>(-1)) ? obj->slots[fs].value
                                                    : JitValue{TAG_NIL, 0};
      _jit_packable_write_field(payload + var.fields[fi].offset, sf, fv.tag,
                                fv.data);
    }
    return;
  }
  auto as_double = [&]() -> double {
    if (tag == TAG_LONG) return static_cast<double>(data);
    if (tag == TAG_FLOAT) return _culebra_float_to_double(data);
    throw culebra::CulebraError("TypeError",
        "type error: expected Long or Float");
  };
  auto as_long = [&]() -> int64_t {
    if (tag == TAG_LONG) return data;
    if (tag == TAG_FLOAT) return static_cast<int64_t>(_culebra_float_to_double(data));
    throw culebra::CulebraError("TypeError", "type error: expected Long");
  };
  if (f.type == "Float32") { float v = static_cast<float>(as_double()); std::memcpy(p, &v, 4); return; }
  if (f.type == "Float64" || f.type == "Float") { double v = as_double(); std::memcpy(p, &v, 8); return; }
  if (f.type == "Int8")  { int8_t  v = static_cast<int8_t>(as_long());  std::memcpy(p, &v, 1); return; }
  if (f.type == "Int16") { int16_t v = static_cast<int16_t>(as_long()); std::memcpy(p, &v, 2); return; }
  if (f.type == "Int32") { int32_t v = static_cast<int32_t>(as_long()); std::memcpy(p, &v, 4); return; }
  if (f.type == "Int64" || f.type == "Long") { int64_t v = as_long(); std::memcpy(p, &v, 8); return; }
  if (f.type == "Byte")  { uint8_t v = static_cast<uint8_t>(as_long()); std::memcpy(p, &v, 1); return; }
  if (f.type == "Bool") {
    bool b;
    if (tag == TAG_BOOL || tag == TAG_LONG) b = (data != 0);
    else if (tag == TAG_FLOAT) b = (_culebra_float_to_double(data) != 0.0);
    else throw culebra::CulebraError("TypeError",
             "type error: expected Bool, Long, or Float");
    uint8_t v = b ? 1 : 0; std::memcpy(p, &v, 1); return;
  }
}

// A packed view carries an absolute byte offset (`__packedview_byteoff__`, set
// by a nested @packable field) or a record index (`__packedview_index__`, set
// by `buf[i]`); and the class whose layout describes it (`__packedview_class__`
// for a nested view, else the buffer's class). These mirror the interp.
inline int64_t _jit_packed_view_off(JitObject* view,
                                 culebra::SharedBufferCore& core) {
  size_t bo = view->find_slot("__packedview_byteoff__");
  if (bo != static_cast<size_t>(-1)) return view->slots[bo].value.data;
  int64_t idx = view->slots[view->find_slot("__packedview_index__")].value.data;
  return idx * static_cast<int64_t>(core.layout.stride);
}
inline std::string _jit_packed_view_class(JitObject* view,
                                          culebra::SharedBufferCore& core) {
  size_t cs = view->find_slot("__packedview_class__");
  if (cs != static_cast<size_t>(-1))
    return std::string(_culebra_str_view(view->slots[cs].value.tag,
                                         view->slots[cs].value.data));
  return core.class_name;
}
inline const culebra::PackableLayout& _jit_packed_view_layout(
    JitObject* view, culebra::SharedBufferCore& core) {
  size_t cs = view->find_slot("__packedview_class__");
  if (cs != static_cast<size_t>(-1))
    if (auto* l = culebra::lookup_packable_layout(_culebra_str_view(
            view->slots[cs].value.tag, view->slots[cs].value.data)))
      return *l;
  return core.layout;
}

// Resolve a packed view to (core, record base pointer).
inline std::pair<std::shared_ptr<culebra::SharedBufferCore>, uint8_t*>
_jit_packed_view_record(JitObject* view) {
  int64_t id = view->slots[view->find_slot("__packedview_id__")].value.data;
  auto core = culebra::lookup_shared_buffer(id);
  if (!core) {
    throw culebra::CulebraError("ValueError",
        "packed view references a freed SharedBuffer");
  }
  return {core, core->data + _jit_packed_view_off(view, *core)};
}

// A nested @packable record view over `core` at absolute byte offset `off`.
inline JitObject* _jit_make_nested_view(int64_t id, int64_t off, const char* cls) {
  auto* view = culebra_runtime_object_new();
  view->is_packed_view = true;
  culebra_runtime_object_set(view, "__packedview_id__", false, TAG_LONG, id, 0, 0);
  culebra_runtime_object_set(view, "__packedview_byteoff__", false, TAG_LONG, off, 0, 0);
  culebra_runtime_object_set(view, "__packedview_class__", false, TAG_STRING,
                             reinterpret_cast<int64_t>(_intern_str(cls)), 0, 0);
  return view;
}

// fwd: defined below (after the byte helpers they need); _jit_packed_view_get
// builds a collection view for a FixedArray/FixedSet/FixedMap field.
inline JitValue _jit_make_fixed_array_view(int64_t id, int64_t abs_off,
                                           const culebra::PackableField& f);
inline JitValue _jit_make_fixed_set_view(int64_t id, int64_t abs_off,
                                         const culebra::PackableField& f);
inline JitValue _jit_make_fixed_map_view(int64_t id, int64_t abs_off,
                                         const culebra::PackableField& f);

inline JitValue _jit_packed_view_get(JitObject* view, const char* key,
                                    int64_t line = 0, int64_t col = 0) {
  int64_t id = view->slots[view->find_slot("__packedview_id__")].value.data;
  auto core = culebra::lookup_shared_buffer(id);
  if (!core)
    throw culebra::CulebraError("ValueError",
        "packed view references a freed SharedBuffer", line, col);
  int64_t off = _jit_packed_view_off(view, *core);
  const auto& layout = _jit_packed_view_layout(view, *core);
  const auto* f = layout.find(key);
  if (!f) {
    throw culebra::CulebraError("AttributeError",
        std::format("@packable {} has no field `{}`",
                    _jit_packed_view_class(view, *core), key), line, col);
  }
  int64_t abs_off = off + static_cast<int64_t>(f->offset);
  if (f->layout.is_fixed_array) return _jit_make_fixed_array_view(id, abs_off, *f);
  if (f->layout.is_fixed_set) return _jit_make_fixed_set_view(id, abs_off, *f);
  if (f->layout.is_fixed_map) return _jit_make_fixed_map_view(id, abs_off, *f);
  if (f->layout.is_struct)
    return {TAG_OBJECT, reinterpret_cast<int64_t>(
                            _jit_make_nested_view(id, abs_off, f->layout.elem_type.c_str()))};
  return _jit_packable_read_field(core->data + off, *f);
}

inline void _jit_packed_view_set(JitObject* view, const char* key, int8_t tag,
                                 int64_t data, int64_t line, int64_t col) {
  // A store consumes exactly one ref of the assigned value, on every exit
  // path (validation throw, struct memcpy, or the terminal field write).
  // One guard replaces the per-path releases and makes the contract uniform.
  JitOwnedVal _consume{tag, data};
  int64_t id = view->slots[view->find_slot("__packedview_id__")].value.data;
  auto core = culebra::lookup_shared_buffer(id);
  if (!core) {
    throw culebra::CulebraError("ValueError",
        "packed view references a freed SharedBuffer", line, col);
  }
  int64_t off = _jit_packed_view_off(view, *core);
  const auto& layout = _jit_packed_view_layout(view, *core);
  const auto* f = layout.find(key);
  if (!f) {
    throw culebra::CulebraError("AttributeError",
        std::format("@packable {} has no field `{}`",
                    _jit_packed_view_class(view, *core), key), line, col);
  }
  if (f->layout.is_fixed_array) {
    throw culebra::CulebraError("TypeError",
        std::format("cannot assign to FixedArray field `{}`; mutate it via "
                    ".push(...) / [i] = ...", key),
        line, col);
  }
  if (f->layout.is_fixed_set || f->layout.is_fixed_map) {
    throw culebra::CulebraError("TypeError",
        std::format("cannot assign to {} field `{}`; mutate it through its "
                    "methods", f->layout.is_fixed_set ? "FixedSet" : "FixedMap", key),
        line, col);
  }
  if (f->layout.is_struct) {
    // Copy another @packable record of the same class (memcpy its bytes).
    auto* src = (tag == TAG_OBJECT) ? reinterpret_cast<JitObject*>(data) : nullptr;
    bool ok = src && src->is_packed_view;
    std::string src_cls;
    std::shared_ptr<culebra::SharedBufferCore> src_core;
    int64_t src_off = 0;
    if (ok) {
      int64_t sid = src->slots[src->find_slot("__packedview_id__")].value.data;
      src_core = culebra::lookup_shared_buffer(sid);
      if (src_core) {
        src_off = _jit_packed_view_off(src, *src_core);
        src_cls = _jit_packed_view_class(src, *src_core);
      } else {
        ok = false;
      }
    }
    if (!ok) {
      throw culebra::CulebraError("TypeError", std::format(
          "field `{}` expects a `{}` record value", key, f->layout.elem_type), line, col);
    }
    if (src_cls != f->layout.elem_type) {
      throw culebra::CulebraError("TypeError", std::format(
          "field `{}` expects a `{}` record, got `{}`", key, f->layout.elem_type,
          src_cls), line, col);
    }
    std::memcpy(core->data + off + f->offset, src_core->data + src_off,
                culebra::lookup_packable_layout(f->layout.elem_type)->stride);
    return;  // _consume releases the source record value
  }
  _jit_packable_write_field(core->data + off, *f, tag, data);
  // _consume releases the assigned value (the write only borrowed it).
}

}  // extern "C" (block continues in jit_fixed.h)

