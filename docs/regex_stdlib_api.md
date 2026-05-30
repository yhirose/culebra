# Regex stdlib API — design (culebra integration)

> **Status: Draft (2026-05-30).** Public API for exposing `include/regexlib.h`
> as a culebra stdlib namespace. Engine internals: see `docs/regexlib.md` and
> `docs/regex_dfa_design.md`. This doc is the language-level contract.

## API shape

A compiled `Regex` is constructed once with `Regex.compile` and reused (the
compiled program is the expensive part). It is a culebra-source class wrapping
the native `_Regex` primitives — the `_Time` / `Time` split.

```culebra
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
m.value          -> String             // the whole-match text
m.start, m.end   -> Int                // byte offsets
m.groups         -> [Group | nil]      // groups[0] is the whole match
m.named          -> {name: Group}      // named captures, by name
// Group: g.value -> String, g.start / g.end -> Int
// positional: m.groups[1].value ; named: m.named["year"].value
```

`Match` is a plain **data object** (fields only, no methods) so it crosses the
interp/JIT value boundary unchanged — the native primitive builds it directly.

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
paths + error paths (bad pattern, no-match nil, named groups, flags, split,
replace, grapheme, reuse). docs: `docs/stdlib.md` + `.ja.md`.

## Open / deferred

- `replace_all` with a function argument.
- lazy `find_iter` generator.
- `Regex.escape(s)` (quote metacharacters) — small, add if needed.
