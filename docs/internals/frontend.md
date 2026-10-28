The Front End: From Source Text to the Compiler's Input
========================================================

What happens to a `.cul` file before `vm::Compiler` sees it: the grammar,
the parser, the syntax-error messages, the module loader, the AST-to-AST
lowerings, and the name resolution every later stage reads. The bytecode
and the three lanes that consume it are [`vm.md`](vm.md); the directory
rules for `include/frontend/` are [`layout.md`](layout.md). The Japanese
mirror of this file is [`frontend.ja.md`](frontend.ja.md).

Contents
--------

1. [Where the front end sits](#1-where-the-front-end-sits)
2. [The grammar and its snapshot](#2-the-grammar-and-its-snapshot)
3. [Parsing](#3-parsing)
4. [Syntax errors](#4-syntax-errors)
5. [Loading modules](#5-loading-modules)
6. [Lowerings that run before the compiler](#6-lowerings-that-run-before-the-compiler)
7. [Names and scopes](#7-names-and-scopes)
8. [Lint and fmt on the same front end](#8-lint-and-fmt-on-the-same-front-end)
9. [Changing the grammar](#9-changing-the-grammar)
10. [What this layer decided](#10-what-this-layer-decided)

---

## 1. Where the front end sits

There is one front end. The executor, `--jit` and `culebra build` all
read the bytecode that `vm::Compiler` makes from the AST this layer
produces (`docs/essays/one-front-end.md` is the story of how that came
about). Everything below runs once per module, in this order:

```text
source text
  │  culebra::parse                 parser.h
  │    PEG grammar -> AST as written; regex literals and postfix
  │    modifiers desugared
  │
  ├─ lint::scope_diagnostics        lint.h, over resolve.h
  │    reads the AST as written, before any lowering
  │
  │  apply_transforms               effects_transform.h
  │    check_generators_in -> transform_effects_in -> reject_*
  ▼
the AST the compiler reads
  │  FnAnalysis                     fn_analysis.h, over resolve.h
  ▼
vm::Compiler                        include/vm/vm.h
```

`include/frontend/` holds fifteen headers, all header-only. Their roles:

| file | role |
|---|---|
| `grammar_def.h` | the PEG grammar, one raw string; the only place the syntax is defined |
| `grammar_blob_key.h`, `grammar_blob.gen.h` | the serialized grammar and the hash that says whether it still matches (§2) |
| `parser.h` | the parser, the `view_*` accessors over the AST, the parse entry points, the desugarings and several structural rejections |
| `syntax_error.h` | turns peglib's error report into a message (§4) |
| `module_loader.h`, `embed_paths.h` | loading a program as a list of modules (§5) |
| `effects_transform.h`, `state_lowering.h`, `fragments.h`, `generator_rules.h` | the lowerings that run before the compiler (§6) |
| `resolve.h`, `scope_check.h`, `fn_analysis.h` | names, scopes, captures (§7) |
| `lint.h` | static checks, sharing `resolve.h` and the lowerings (§8) |

Who includes what: the compiler (`vm/vm.h`) reaches the front end through
`module_loader.h`, `fn_analysis.h` and `scope_check.h`; the formatter,
the language server, the REPL, the debugger and the test runner go
through `parser.h`, `module_loader.h` or `resolve.h`.

---

## 2. The grammar and its snapshot

`grammar_def.h` is a single `const auto grammar_ = R"(...)"`. Nothing else
in the tree defines syntax. The editor grammars (`misc/culebra.peg` and
the keyword blocks of the vim, VS Code and Zed files and of the
Playground) are generated from it by `just sync-grammar`.

Reading a grammar with cpp-peglib means a meta-parse of the grammar text,
which the source puts at about 10 ms. A program that runs for a few
milliseconds should not pay that on every start, so the build keeps a
snapshot:

- `tools/gen_grammar_blob.cc` loads `grammar_` into a bare `peg::parser`,
  calls `serialize_grammar()`, and writes `grammar_blob.gen.h`: the byte
  array, `GRAMMAR_BLOB_HASH`, and three lists the error formatter needs
  (§4). `just gen-blob` runs it; `just check-blob` runs it with `--check`,
  which regenerates in memory and compares bytes.
- `grammar_blob_key()` is an FNV-1a hash over `grammar_`, a separator
  byte, and `CPPPEGLIB_VERSION`. The blob's own magic does not change
  between peglib versions, so the key is what notices a layout change.
- `get_parser()` (a `thread_local` `peg::parser`) loads the blob when
  `GRAMMAR_BLOB_HASH` equals the key and falls back to
  `load_grammar(grammar_)` otherwise. If that fails too it throws
  `std::logic_error("invalid peg grammar")`.

**A stale blob is correct and slower.** The fallback hides it, so the
parser tests stay green. `just check-blob` is the only thing that sees
it, which is why it is part of `check-generated` and of CI as a step of
its own.

---

## 3. Parsing

`get_parser()` configures the parser once per thread:

- `enable_ast(true, true, ast_optimizer_keep_rules())`. The optimizer
  collapses a node that has a single child onto that child, except for
  the rules in the keep list. A new rule that must stay a node of its own
  (because a walker looks for it by name) has to be added to
  `ast_optimizer_keep_rules()`. `FOR_BINDING` is deliberately not kept.
- `enable_packrat_parsing()`. The blob carries the packrat flag, and this
  call re-applies it rather than resetting it.
- `set_max_depth(kCulebraParseDepthLimit)`, 4000. The unit is rule
  matches, about six per bracket level; the deepest file in the test
  corpus reaches 101 and the native stack would need about 120,000. peglib
  bounds the AST it returns to the same depth, which is what protects
  every recursive walker downstream. A depth failure is reworded by
  `reword_parse_depth_error`.
- `leave` hooks on `INTERPOLATED_STRING`, `TRIPLE_STRING` and
  `INTERP_EXPR`, which record where a string was opened so that an
  unclosed one can say "string opened at L:C is not closed".

**The source must outlive the AST.** The tokens of a `peg::Ast` are
`string_view`s into the buffer that was parsed. `parse_undesugared` takes
that buffer as `std::string&` because it normalizes CRLF to LF in place
(a bare `\r` is a `CulebraError`), so the caller must own the string and
keep it for as long as the tree. `ModuleLoader` pins each module's source
in a `shared_ptr<std::string>` in `LoadedModule` for this reason, and the
built-in traits preamble keeps its text in a function-local `static`.

The entry points, all in namespace `culebra`, return a
`shared_ptr<peg::Ast>` that is null on failure:

| function | does |
|---|---|
| `parse_undesugared` | the shared core: parse, normalize newlines, fill a `vector<ParseFailure>` (`line`, `col` as a byte column, `message`) |
| `parse` | `parse_undesugared`, then `desugar_regex_literals` and `desugar_postfix_modifiers`; one overload formats failures as `path:line:col: message` |
| `parse_for_format` | no desugaring, and a brace Set is a syntax error here; the formatter only |
| `parse_with_transforms` | `parse` plus `apply_transforms` (§6); used by the REPL, the debugger, the test runner, the stdlib preamble and lint |

### The `view_*` accessors

Knowledge of the AST's shape is kept out of the walkers. `parser.h`
defines 22 `view_*(const peg::Ast&)` functions (`view_assignment`,
`view_for`, `view_if`, `view_match`, `view_function`, `view_lambda` and
so on), each returning a small struct with the children by name. A walker
that needs the parts of a `for` reads them through `view_for`; a grammar
change that moves a child updates the view, not every walker.

The walkers are no longer an interpreter and a JIT. They are
`vm::Compiler`, `resolve.h`, `lint.h`, `fn_analysis.h`,
`state_lowering.h`, `effects_transform.h`, `generator_rules.h`, the
formatter and the type inferrer. A grammar edit that adds or reshapes a
node is not finished until each of those has been read for it (§9).

---

## 4. Syntax errors

Culebra builds the message itself from peglib's structured
`peg::ErrorReport`, in `format_syntax_error` (`syntax_error.h`), instead
of using peglib's formatted string. peglib reports everything that failed
at the furthest position, which for `x = [1, 2` is a list of dozens of
items opening with whitespace. The formatter turns that into
`expecting ',', ']' or an operator.`

```text
x = [1, 2         ->  expecting ',', ']' or an operator.
foo(1 2)          ->  unexpected '2', expecting ',', ')' or an operator.
y = (1 +          ->  expecting an expression.
fn f(a b) { }     ->  unexpected 'b', expecting ':', '=', ',' or ')'.
```

(These are the expectations pinned in `tests/lint_test.sh`.) The rules:

- Literals are quoted (`','`), rules are named (`<IDENTIFIER>`). Literals
  come first, then rules, in the order the report gave them.
- **Silent** items (whitespace, the openers of comments) are dropped.
- Two **crowds** are named instead of listed: the items that may follow
  any expression become `an operator`, and the items that may start an
  expression become `an expression`. A crowd is named only when strictly
  more than half of its members are in the report. Below that, they are
  listed one by one, because a parameter may be followed by `:` or `=`
  without any operator being possible there. The naming follows rustc.
- A message the grammar attached itself (a custom message, or a limit) is
  returned as is.

The three sets are not kept by hand. `tools/gen_grammar_blob.cc` finds
them by parsing probe sources that stop short, and stores them in
`grammar_blob.gen.h` as `GRAMMAR_EXPECT_SILENT`,
`GRAMMAR_EXPECT_AFTER_EXPR` and `GRAMMAR_EXPECT_OPERAND`:

- *silent*: the literals of every grammar rule whose name starts with an
  underscore (peglib leaves such rules out of the report);
- *after an expression*: what is expected after `x = (f()`, `x = [f()`,
  `if f()` and `g(f()`, **intersected**;
- *operand*: what is expected after `x = 1 + ` and after `x = `,
  **united**.

Each probe is run at the end of the input and again before a `\x01`
byte, and the two failures are united. The generator exits with an error
if a set comes out empty, and the silent items are removed from the other
two. Because the sets come from the grammar, a grammar edit regenerates
them in the same `just gen-blob` that refreshes the snapshot.

**A known limit.** Items are identified by their text alone, so a closer
that is spelled like an operator (the `|` that ends lambda parameters,
the `:` of `a ? b : c`) is folded into `an operator` whenever operators
are being named.

The standard library's `PEG.parse` passes no classes, so for a user's
grammar the message lists everything, escaped.

---

## 5. Loading modules

`ModuleLoader::load_program` turns an entry file into a list of
`LoadedModule`s in dependency order, and every lane uses it (`main.cc`,
the test runner and the debugger call it). A `LoadedModule` has the
absolute path, the source (pinned, see §3), the AST, the dependencies in
source order and the scope diagnostics.

`load_recursive` does, for one module:

1. check the import stack for a cycle (`ImportError`);
2. read the file, or take the entry source;
3. `culebra::parse` (a failure becomes a `SyntaxError` carrying the
   messages);
4. `lint::scope_diagnostics` over the AST as written;
5. `apply_transforms` (§6);
6. `rebase_embedded_calls` for modules that are neither the entry nor a
   library;
7. `validate_module` (imports and exports only at the top level, no
   export listed twice);
8. `extract_imports`;
9. recurse into the dependencies, then push itself. That order is the
   topological order, and the module cache is keyed by the canonical
   absolute path.

When every module is loaded, `load_program` runs `lint::check_module` on
each, so the checks that must reject a program are the same on every lane.

**Why a program needs no `import` for the standard library.** It is not
the loader's job. `splice_stdlib_preamble` (`stdlib/preamble.h`) scans
the tokens of every module for the namespaces they name, parses the
matching preambles from `src/preambles/*.cul` with
`parse_with_transforms`, and puts the result in front of the other
modules under the path `<stdlib>`. A program that never names `Regex`
never parses the Regex preamble. The JIT and AOT lanes can use a
preamble that was parsed and compiled at build time (`BakedPreamble`).

**Embedded directories.** `Dir.embedded(name)` names a directory next to
the file that calls it, the way `import` does, but the runtime and
`culebra build` resolve names from the entry script's directory. So
`rebase_embedded_calls` (`embed_paths.h`) rewrites a literal name into a
path relative to the entry directory, and a computed one into a call to
`Dir._embedded_in(base, ...)`. The AOT scan (`aot/scan.h`) uses the same
`embedded_literal_slot` to find the assets to bake in.

---

## 6. Lowerings that run before the compiler

Two features are not compiled as they are written. They are rewritten
into plain Culebra source, which is parsed again and then compiled like
any other code, so every lane runs the lowered program and has no
special support for them.

`apply_transforms` runs, in this order, on a module parsed as written:

1. `check_generators_in`: refuses a `yield` in a `defer` block. The
   message explains that a `defer` runs as its scope is left, including
   when the generator is closed there, so it cannot suspend. `try` and
   `catch` bodies may yield. Handle and effect bodies are left to the
   next step.
2. `transform_effects_in`: the effects lowering, below.
3. `reject_orphan_yield`: a `yield` outside the body of a function or
   method (the top level, a `|...|` lambda, `new`, `drop`) is refused.
4. `reject_sized_spread_mix`, `reject_brace_set`,
   `reject_or_pattern_binding`, `reject_invalid_new`: structural
   rejections that live in `parser.h`.

`generator_rules.h` only decides where a `yield` may be written. A
generator itself is not rewritten: the compiler takes it as written, and
each engine suspends the frame at `Op::GenStart` / `Op::Yield` (vm.md
§5.7). The predicates `can_be_generator` and `fn_body_has_yield` are
shared with the compiler, which is how it decides that a chunk is a
generator.

### The effects lowering

`effect fn`, `perform` and `handle ... with` are lowered by
`EffectsLowerer`. A body-bearing `effect fn` becomes an ordinary
function that returns a computation object: a class whose `_step(rv)`
runs until the next suspension, dispatching on a state number. A
signature-only `effect fn` becomes a stub that throws. `handle` becomes
a call to `__Eff.handle` with the computation, the operation name and an
adapter, and the driver `__Eff.drive` is Culebra code in
`src/preambles/effects.cul`. A resume is one-shot. The result of a step
is tagged `DONE` or `SUSPEND`. A `perform` nested inside an expression
is first hoisted to statement level by an A-normalization pass
(`anf_program`). vm.md §11 gives the design and the intrinsics the
compiler provides for it.

Three pieces of machinery serve that rewriting, and they are the part of
the front end with no other documentation:

- **`PromotedLocals`** (`state_lowering.h`). A local that a suspension
  splits across steps cannot stay a local of one call. It becomes a
  field of the computation object, or a box if a closure captured it,
  and every read and write of it is rewritten. It is built on
  `resolve::resolve_body`, so the decision of which names are locals is
  `resolve.h`'s. Reading a promoted local before its binding ran is
  guarded by the compiler's `__bound` / `__unbound` intrinsics.
- **The fragment ledger** (`fragments.h`). Every piece of source that a
  lowering synthesizes is registered with `parse_registered_source`
  before it is parsed, so that an error in the lowered code can be
  reported at the line the user wrote. The ledger holds the buffers, and
  the synthesized text carries anchors (`/*@culebra:`) that a
  `SourceResolver` follows back, through intermediate buffers, to a byte
  of the user's file. Lines that were copied carry a trailing
  `#@culebra:` marker with the original line. A synthesized line with no
  origin falls back to the declaration's line.
- **A process-wide default ledger, and a scoped one.** The built-in
  traits preamble is parsed once and shared across threads, so the
  default ledger is guarded by a mutex. An editor or lint run selects a
  thread-local ledger through the `FragmentLedger&` overload of
  `parse_with_transforms`.

The lowering raises `SyntaxError` for what it cannot express (a `perform`
under a short-circuit operand, behind a method chain or in a loop
condition, a `defer` nested in control flow, a handler clause with no
`resume` parameter, among others), and `InternalError` if source it
synthesized does not parse. Lint reaches the same rejections by calling
`parse_with_transforms` inside a `try`, so a rule is written once.

---

## 7. Names and scopes

`resolve.h` is the one place that decides what a name refers to. It walks
the source as written and records, for each use, the declaration it
binds to. It does not assign storage or cells; the compiler does that.

```text
resolve_module(root, source, Options)   a whole module
resolve_body(body, params, Options)     one function body (the effects lowering)
```

A `Resolution` holds the `scopes`, the `symbols` (with how often and in
what form each was declared), the `occurrences`, the `unresolved` names,
the imports, and the method calls recorded for UFCS. With
`Options::record_nodes`, which both the compiler and lint set, it also
maps AST nodes to their symbols, so a consumer can ask about a node
rather than about a name and a position. `Options::globals` lists the
standard-library names (a bare write to one is refused, not taken as a
declaration), and `Options::session` lists the names that earlier inputs
of a REPL or a `culebra test` run declared.

### The rules it encodes

`language.md` §6 states the user-visible rules. These are the ones the
resolver also has to get right, including some that the language
reference does not spell out:

- **Scopes.** A function body, a block, a loop (a fresh binding per
  iteration), each arm of `if` / `cond` / `?:`, a `match` arm, `try` and
  `catch`, a `handle` block, and the init clause around its construct.
  A test is not part of an arm.
- **A declaration is visible after its right-hand side.** A function
  literal is bound before its body, so it can call itself.
- **Function bodies are resolved after the enclosing body** (a job queue),
  so a nested function sees every name the enclosing function declares,
  wherever it declares it. A parameter's default is the exception and is
  resolved immediately.
- **A bare `x = v`** writes the visible `x`; if there is none it declares
  `x` in the innermost scope, unless `x` is a standard-library global.
- **A class's field initializers** are one function scope with a
  sub-scope for each initializer. **A static value** is a scope at the
  class declaration, and its declarations are scoped to that expression.
- `fn` cannot be declared, `self` is a variable only where it is
  declared, a decorator that runs at compile time reads nothing, and a
  keyword label `f(x: 1)` resolves to the callee's parameter.

### What reads the resolution

- **`fn_analysis.h`** runs `resolve_module`, then `lint::check_resolution`,
  then a walk of its own, then `derive_captures`. A name read in a
  function whose symbol belongs to another function becomes a free
  variable of every function from the reader out to the declaring one.
  That is how the compiler learns which locals are captured and so need a
  cell. The walk still records a few facts the resolver has no reason to
  know: `self`, the field-initializer slot, `fn`, and the lowered-frame
  marks.
- **`lint.h`** (§8).
- **The language server, the type inferrer and the editor outline**
  (`resolve::outline`).
- **`scope_check.h`** (below).

### The agreement check

The compiler still binds names itself while it emits code, so the two
could drift. An automated check holds them together. With `CULEBRA_SCOPE_CHECK=<dir>`
set, the compiler asks the resolver about every name node it binds or
looks up and writes a report with four kinds of line:

- `MISMATCH`: the compiler and the resolver disagree (a declaration, a use,
  two cells for one symbol, a captured variable with no binding or not in
  a cell);
- `MISSING`: the compiler looked up a node the resolver never saw;
- `UNSEEN`: the resolver looked up a node no compile step did;
- `SUMMARY`.

`tools/checks/scope_agreement.py` compiles, without running, every
tracked `.cul` file under `tests/`, `examples/`, `tools/`, `docs/` and
`stdlib/`, plus a generated grid of constructs crossed with declaring
forms, and fails on any finding that is not in
`scope_agreement_allow.txt`, on an allowlist entry that no longer
matches, and on a compile that wrote no report. It is the
`run_scope_agreement` entry in the justfile. vm.md §10.6 has the mechanism.

---

## 8. Lint and fmt on the same front end

**Lint decides nothing about names.** It reads the resolution. Its checks
come in two groups:

- over the AST *as written*, through `scope_diagnostics`: reserved
  decorators and declarations, immutability (`LetRule`), undefined reads,
  and shadowing;
- over the *lowered* AST, through `RuleWalker`, and a set of independent
  syntactic walkers for unused names, top-level statements, idioms and
  unreachable code.

Two of the scope checks, shadowing and reserved bindings, also run on
every compile, from `FnAnalysis` through `check_resolution`, so every lane
rejects the same programs, the REPL included. The warnings do not run on
the load path; `culebra lint` runs the full set with `collect_module`.
`resolve_options()` is the one place that builds the `Options` for both
the compiler and lint.

**fmt shares the grammar and the parser but not the desugared tree.** It
calls `parse_for_format`, which skips the desugarings and the
transforms, so what it prints is the text the user wrote. It builds a
document from the AST, falls back to a verbatim slice for syntax it does
not know, and re-parses its own output and compares the two trees as a
safety net.

---

## 9. Changing the grammar

1. Edit `include/frontend/grammar_def.h`.
2. If the edit adds a node that a walker looks for by name, add the rule
   to `ast_optimizer_keep_rules()` in `parser.h`. If it reshapes a node,
   update or add the `view_*` accessor.
3. Read each walker in §3 for the change. The accessors make the shape
   one edit, but only the walker knows what the node means.
4. `just sync-grammar` regenerates `misc/culebra.peg` and the keyword
   blocks of the editor grammars and the Playground, and warns about a
   keyword that `misc/keyword-map.txt` does not map.
5. `just gen-blob` regenerates `grammar_blob.gen.h`, which includes the
   error-formatting sets. Commit it.
6. `just check-generated` (part of `just test-dev`, so also of
   `just land`) runs `check-grammar-sync` and `check-blob`; CI runs both
   as steps of their own. `just phase <word>` runs a subset while you
   iterate.
7. If an error message changed, the frozen `vm_cases` outputs need
   re-freezing and the release-diff allowlist needs a line (vm.md §10).
8. If effect or generator lowering changed, run
   `python3 tools/checks/lowering_diff.py ./build-dev/culebra`.

No recipe or CI step runs `peglint`.

---

## 10. What this layer decided

- **One resolver, not one per consumer.** The compiler, lint and the
  capture analysis each used to work out scopes for themselves. Comparing
  them turned up real disagreements, among them crashes in field
  initializers and static values. `resolve.h` now holds the rules, the
  captures are derived from it, lint reads it, and an automated check compares the
  compiler against it so that the one place which still binds names
  itself cannot drift unseen.
- **The error message is built here, not by the parser library.** When
  peglib began reporting the union of everything that failed at the
  furthest position, `x = [1, 2` came back as a list of 38 items opening
  with whitespace, and no test noticed. The decision was to keep the
  library's structured report and write the wording in `syntax_error.h`,
  with the crowds derived from the grammar so that they cannot go stale.
- **Lowerings produce source, not a second tree.** The effects lowering
  rewrites into plain Culebra source and parses it again, so that every
  lane runs the lowered program and none has anything effect-specific to
  implement (the header of `effects_transform.h` says so). The provenance
  machinery of §6, the ledger and the anchors, is the price: the
  re-parsed text has to be mapped back to the lines the user wrote.
- **The grammar snapshot is an optimization the tests cannot see.** A
  stale blob falls back to the slow path and stays correct. That is why
  it has a check of its own rather than relying on the parser tests.
