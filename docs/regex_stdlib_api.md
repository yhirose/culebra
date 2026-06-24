# Regex stdlib API — design (culebra integration)

> **Status: Draft (2026-05-30).** Public API for exposing the vendored
> [cpp-regexlib](https://github.com/yhirose/cpp-regexlib) engine
> (`vendor/cpp-regexlib`) as a culebra stdlib namespace. Engine internals and
> the matching model live in that repository's own docs. This doc is the
> language-level contract.

## API shape

A compiled `Regex` is constructed once with `Regex.compile` and reused (the
compiled program is the expensive part). It is a culebra-source class wrapping
the native `_Regex` primitives — the `_Time` / `Time` split.

```culebra
# doctest: skip
let re = Regex.compile("\\d+")          // compile (reused); bad pattern -> error
let re = Regex.compile("\\w+", "i")     // flags: a string of "i" "m" "s"

re.test(s)            -> Bool           // does it match anywhere
re.find(s)            -> Match | nil    // leftmost match anywhere
re.match(s)           -> Match | nil    // anchored at start (prefix)
re.find_all(s)        -> [Match]        // all non-overlapping matches ([] if none)
re.replace_all(s, r)  -> String         // r: "$1" / "$<name>" / "$$"
re.split(s)           -> [String]       // split the subject on matches
```

**Flags** are passed to `compile` as a string (`"i"`/`"m"`/`"s"`) and folded
into the pattern as an inline `(?…)` group; inline `(?i)` in the pattern works
too.

**JIT note**: method names `find` / `split` / `match` collide with builtin
method names. Calling them on a class instance used to mis-dispatch to the
builtin in the JIT; that was fixed (a class instance's own method now shadows a
same-named builtin in the JIT, matching the interpreter), so the object form
works on all three backends.

**Write patterns as single-quoted raw strings** — `'\d{4}'`, not `"\\d{4}"`.
culebra single-quoted strings do no escape processing and no `{...}`
interpolation, so regex metacharacters (`\d`, `\w`) and `{n}` quantifiers pass
through verbatim (the Python `r"..."` idiom). Double-quoted patterns would need
`\\d` and would have `{4}` eaten by string interpolation. Replacement templates
(`'$2.$1'`) are likewise cleanest single-quoted.

`Match` is a plain object (nil when there is no match). Offsets are **byte
offsets** into the subject (Go-style, matching culebra's String model) and
always fall on grapheme-cluster boundaries.

```culebra
# doctest: skip
m.value          -> String             // the whole-match text
m.start, m.end   -> Int                // byte offsets
m.groups         -> [Group | nil]      // groups[0] is the whole match
m.named          -> {name: Group}      // named captures, by name
// Group: g.value -> String, g.start / g.end -> Int
// subscript = captures accessor (string directly):
m[1]             -> String | nil       // positional group; m[0] = whole match
m["year"]        -> String | nil       // named group
// miss (out of range / unmatched / unknown name) -> nil; composes with ?? ""
// negative i wraps like an array; m["value"] is nil (use m.value / m[0])
// spans still via the Group objects: m.groups[1].start, m.named["year"].end
```

`Match` is a plain **data object** (fields only) so it crosses the interp/JIT
value boundary unchanged — the native primitive builds it directly. The
subscript routing is an O(1) `is_match` flag on the object (interp `ObjectValue`
/ JIT `JitObject`), invisible to `str()`, so it never touches the data shape.

## Design choices (cross-language synthesis)

- **nil for no-match** — Rust `Option`, Kotlin/Swift Optional, Python `None`.
  Composes with culebra `?.` / `??`: `Regex.find(p, s)?.value ?? ""`.
- **`find` / `find_all`** — Rust `find`/`find_iter`, Kotlin `find`/`findAll`
  (`search` → `find`, `str` → `value` for modern naming).
- **No find/captures split** — Rust separates them for performance; regexlib
  picks the fast tier automatically (capture-free pattern → pure-DFA tier 1;
  pattern with captures → tier 2 / Pike), so the user never sees the split.
- **`split` included** — standard in Kotlin/Go/Ruby/JS/Python; absent from the
  C++ engine API, added at the stdlib layer (find_all + slice between matches).
- **Array, not lazy** — `find_all`/`split` return arrays for ergonomics; a lazy
  `find_iter` (generator) can be added later if a workload needs it.
- **Match is data, not a class** — fields only, so it crosses the interp/JIT
  value boundary unchanged (the native primitive builds it directly).

## Implementation (3 backends)

The `_Time` / `Time` split: native stateless primitives + a culebra-source
class wrapper.

- **`_Regex` primitives** (no new `Value` type): `check`/`test`/`find`/`match`/
  `find_all`/`replace_all`/`split`, each taking `(pattern, subject, …)`. A
  pattern→`regexlib::Regex` cache gives reuse. `check` validates eagerly (so a
  bad pattern raises at `Regex.compile` time).
  - **interp**: `make_regex_primitives_namespace()` in `stdlib_interp.h`;
    `regex_match_value` builds the Match object; no-match is `Value()` (nil).
  - **JIT / AOT**: slow-path `kNsMethods` adapters only (`_ns_regex_*` in
    `stdlib_jit.h` + `_Regex` in the table and `is_builtin_var`) — like
    `GC.stat`, no fast-path branch / runtime helper / `declare_runtime`; the
    adapters build the `JitObject`/`JitArray` results.
- **`Regex` class wrapper** (`REGEX_MODULE_SOURCE`, lazy-loaded): holds the
  pattern, delegates each method to `_Regex`; `compile(pat, flags?)` folds flags
  into an inline `(?…)` group.
- **byte offsets**: `MatchResult.begin/end` are already byte offsets → copied
  straight into `m.start`/`m.end`.
- **JIT prerequisite (landed)**: `find`/`split`/`match` collide with builtin
  method names. The JIT now lets a class instance's own method shadow a
  same-named builtin (general fix in `compile_method_call`, matching the
  interpreter), so these methods dispatch to the wrapper on all backends.

## Verification

test-first → interp → JIT → AOT, all three agree
(`feedback_check_jit_interp_symmetry`). `tests/test_regex.cul` covers happy
paths + error paths; `tests/test_regex_extras.cul` covers `escape` /
`replace_all(fn)` / `find_iter` (lazy, early exit, empty-match, grapheme).
docs: `docs/stdlib.md` + `.ja.md`.

## Extras (implemented)

- **`Regex.escape(s)`** — backslash-quote metacharacters; pure culebra in the
  module wrapper (uses a backtick raw string for the metachar set).
- **`replace_all(s, fn)`** — a `Function` `repl` is called per `Match` and its
  return spliced between matches (a `String` `repl` keeps the native template
  path). Pure culebra (`type_of` dispatch + `slice`).
- **`find_iter(s)`** — lazy `Iterator<Match>`. A class method can't be a
  generator (the CPS transform only rewrites top-level `fn`), so it delegates
  to a top-level `fn _regex_find_iter` that drives the native
  `_Regex.find_from(pat, s, pos) -> {m, nxt}`. The native returns absolute
  offsets and a grapheme-correct resume byte (one grapheme past an empty
  match), so iteration always advances. Re-searches the suffix per step — fine
  for early exit; use `find_all` to materialize everything. (Field is named
  `nxt`, not `next`, because the Iterator protocol reserves `next`.)
- **`find_all_str(s) -> [String]`** and **`count(s) -> Int`** — lean bulk APIs
  that skip the per-`Match` object. Profiling a match-dense workload (`sample`
  on `find_all` over a 256 KB subject) showed the bottleneck is **not** the
  engine but the per-match Object structure (Match obj + groups array + group0
  obj + named obj — ~6 allocations each, mostly redundant for capture-free
  patterns); the engine's matching is minor. `find_all_str` (texts only) and
  `count` (no objects) run **~12× faster** than `find_all` on that workload.
  `find_all_index(s) -> [Int]` returns flat byte spans `[s0,e0,s1,e1,…]`;
  Longs are inline in the Array, so the whole result is one allocation (≈ the
  speed of `count`, ~13×). Use `find_all` only when you need offsets *and*
  group/named captures together. (A StringView `value` was
  tried first and reverted: in the JIT the view descriptor is itself
  heap-allocated, so it left the alloc count — and the runtime — unchanged.)

## Open / deferred

- engine-level speedups (SIMD literal prefilter, priority DFA) — only matter for
  *sparse* matches over large text; deferred until a workload shows that shape.
