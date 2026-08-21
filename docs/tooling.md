# Culebra Tooling

The `culebra` binary is the whole toolchain: the same executable that
runs a program also carries the test runner, the linter, the formatter,
the debug adapter, and the reference documentation itself. This document
is the reference for those development subcommands.

| Subcommand | What it does | Reference |
|---|---|---|
| `culebra init` | set up this directory and machine's editors | [§4](#quick-setup-culebra-init) |
| `culebra test [paths...]` | run tests and doctests | [§1](#1-testing-culebra-test) |
| `culebra lint [paths...]` | report static problems without running the program | [§2](#2-linting-culebra-lint) |
| `culebra fmt [paths...]` | reformat source to the canonical style | [§3](#3-formatting-culebra-fmt) |
| `culebra dap` | speak the Debug Adapter Protocol over stdio | [§4](#4-debugging-culebra-dap) |
| `culebra docs [topic]` | read and search the embedded reference docs | [§5](#5-reading-the-docs-culebra-docs) |
| `culebra serve [-p PORT] [-d DIR]` | serve a directory of static files | [§6](#6-serving-static-files-culebra-serve) |
| `culebra build <in.cul> -o <out>` | compile ahead-of-time into a standalone executable | [`deployment.md` §1](deployment.md#1-standalone-binary-build-culebra-build) |
| `culebra wrap` | build an extended binary exposing your own C++ classes | [`deployment.md` §3](deployment.md#3-wrapping-c-libraries-culebra-wrap) |

The plain `culebra [flags] script.cul` form — `--jit`, `-O0`..`-O3`,
`--ast`, `--shell` and the rest — is specified in
[§22 of the language spec](language.md#22-command-line-interface).
For a narrative introduction to the same tools see
[`handbook.md` §15](handbook.md#15-tooling-test-lint-fmt-debug).

Contents
--------

1. [Testing (`culebra test`)](#1-testing-culebra-test)
   * [Writing tests](#writing-tests)
   * [Running](#running)
   * [Doctests](#doctests)
2. [Linting (`culebra lint`)](#2-linting-culebra-lint)
3. [Formatting (`culebra fmt`)](#3-formatting-culebra-fmt)
4. [Debugging (`culebra dap`)](#4-debugging-culebra-dap)
   * [Quick setup: `culebra init`](#quick-setup-culebra-init)
   * [VSCode](#vscode)
   * [Vim (vimspector)](#vim-vimspector)
   * [Zed](#zed)
5. [Reading the docs (`culebra docs`)](#5-reading-the-docs-culebra-docs)
6. [Serving static files (`culebra serve`)](#6-serving-static-files-culebra-serve)

---

## 1. Testing (`culebra test`)

`culebra test [paths...]` discovers and runs test files. `test()`,
`@test`, `@parametrize`, the matcher family, and dependency-injected
fixtures are all available.

### Writing tests

Three forms are available — the call form and the `@test` decorator are
equivalent; pick whichever reads better at the call site. `@parametrize`
registers one test per case.

```culebra
# doctest: skip
# tests/test_string.cul

# Call form
test("interpolation embeds Long", fn () {
  let x = 42
  assert_eq("hi {x}", "hi 42")
})

# Decorator form — fn name becomes the test name
@test
fn interpolation_embeds_float() {
  let pi = 3.14
  assert_eq("π = {pi}", "π = 3.14")
}

# Parametrize — one test per case, named `<fn>[i]`
@parametrize([(1, 2, 3), (2, 3, 5), (10, 20, 30)])
fn adds_correctly(a, b, want) {
  assert_eq(a + b, want)
}
```

**No `describe` nesting.** Group by file path (`tests/strings/`) and by
`/` in the test name (`"Array/push: appends element"`).

**Fixtures by DI.** A test's positional parameters are resolved by name
against the surrounding env: any fn in env can be a fixture, no decorator
required. Fixtures can themselves take fixtures.

```culebra
# doctest: skip
fn db() {
  {users: [], next_id: 1}
}
fn user(db) {
  db.users.push({id: 1, name: "alice"})
  db.users[0]
}

@test
fn user_has_name(user) {
  assert_eq(user.name, "alice")
}
```

Within one test, each fixture is evaluated **once** — multiple mentions
(direct + transitive) share the same instance. Across tests, fixtures are
fresh.

**Cleanup via class `drop`.** Resources needing teardown wrap themselves
in a class with a `drop` method
([`language.md` §17](language.md#17-memory-model)). The runtime's
ref-count finalization fires when the per-test fixture cache is released
at test end.

```culebra
# doctest: skip
class TestDB {
  new() {
    self.conn = Database.connect("memory")
  }
  drop() {
    self.conn.close()
  }
  users() {
    self.conn.users
  }
}

fn db() {
  TestDB.new()
}

@test
fn user_count(db) {
  db.users().create("alice")
  assert_eq(db.users().count(), 1)
  # db drops at test end -> conn.close()
}
```

`defer` inside a fixture body would fire when the fixture fn returns
(before the test runs), so class `drop` is the right tool for cleanup.
Long-lived state shared across files (e.g. a model loaded once) goes at
module top level instead, since the module system caches the binding.

**Matchers.** Assertions use the matcher family — there is no `assert`
keyword or builtin. Matchers are **3-backend globals** (bound on every
environment, same as `inspect` / `Math`), so they work identically under
`culebra script.cul`, `culebra --jit script.cul`, `culebra build`, and
`culebra test`:

```culebra
# doctest: skip
assert_eq(arr.len(), 3)  # == ; shows both sides on failure
assert_throws("TypeError", fn () {
  let _ = 1 + 'b'
})
assert_close(0.1 + 0.2, 0.3, 1e-9)  # |a - b| <= tol
```

Full matcher list (`assert_true`/`false`/`ne`/`lt`/`le`/`gt`/`ge` and the
`__eq__`/`__lt__` dispatch rule): [`stdlib.md` §13](stdlib.md#13-matchers).

**Production invariants.** For `if (!cond) throw {...}` checks outside
the test suite, write the `if`/`throw` directly (Go-style, see
[`language.md` §15](language.md#15-error-handling)). There is no separate
`assert` keyword to disable in production builds.

### Running

When invoked through this subcommand, `test` / `@test` / `@parametrize`
become **ambient globals** alongside the always-available matcher family
— no `import` required. This mirrors how `inspect` / `print` are ambient
under script-execution mode but absent from `culebra::environment()`.

```sh
culebra test                       # discover & run from current dir
culebra test tests/strings/        # run a subtree
culebra test --filter "Array/push" # name-substring filter
culebra test --reporter json       # NDJSON output (one JSON per line)
culebra test --bail                # stop after the first failure
culebra test --bail 3              # stop after 3 failures
culebra test --list                # discover only; print test names
culebra test --doc docs            # run the doctests in markdown files
culebra test --doc --vm docs       # ... on the bytecode VM (or --jit)
```

Discovery: any path that is a file is included as-is; any path that is a
directory is walked recursively for files matching `test_*.cul`. Exit
code is `0` when all tests pass, `1` when any fail.

**Reporters.** Default is human-readable. `--reporter json` emits one
JSON object per line (NDJSON) — useful for agent loops and CI:

```
{"event":"test_pass","name":"adds_correctly","source":"tests/test_math.cul","stdout":""}
{"event":"test_fail","name":"divides_correctly","kind":"AssertionError",
 "message":"assert_eq failed:\n  left:  3\n  right: 4","line":12,"col":3,"stdout":""}
{"event":"run_end","passed":42,"failed":1,"errored_files":0,"bailed":false}
```

User `inspect(...)` from inside a test is captured into the event's
`stdout` field rather than interleaved with the NDJSON stream, and
failure events carry a `snippet` with the failing line marked `>` for
context.

### Doctests

`culebra test --doc <path>` extracts every ` ```culebra ` block from the
markdown under `<path>` and runs it in a fresh interpreter, checking its
output against the markers below. Every block in `handbook.md`,
`language.md` and `stdlib.md` follows this convention:

- `# => <value>` — expected stdout (one line)
- `# => |` followed by `# <line>` lines — expected multi-line stdout
- `# !! <pattern>` — expected `throw`, matched as substring
- `# doctest: <directive>` (block-leading line) — modes:
  - `skip` — illustration only, do not run (e.g. *Planned* features)
  - `compile-only` — syntax check only
  - `interp-only` / `jit-only` / `aot-only` — backend filter

Blocks are independent; there is no `setup` / `teardown` across blocks,
so an example needing several steps has to be one block. The runner
currently honors `skip`; `compile-only` and the backend filters are
reserved (such blocks run normally for now).

`--jit` and `--vm` run the same blocks on the JIT and on the bytecode VM
instead of the interpreter, so a documented example is checked against
every engine that will run it — the same output, the same throw. The
unit-test runner (`culebra test` without `--doc`) is the interpreter's.

Two consequences are worth knowing when writing examples. An expression
on its own prints nothing, so a checked example has to go through
`inspect(...)` or `println(...)`. And `//` opens a line comment in
Culebra just as `#` does, so a `// => value` marker parses fine but is
**not** recognized as an expectation — the block then runs unchecked.

**Still planned**: an explicit `import { test } from "std/test"` for code
that doesn't run under `culebra test`; an AOT doctest lane beside `--jit`
and `--vm`; and parallel execution.

---

## 2. Linting (`culebra lint`)

`culebra lint [paths...]` reports static problems **without running** the
program, and exits non-zero so CI can gate on it (0 = clean, 1 = warnings
only, 2 = errors). It reuses the same load-stage static analysis every
backend already runs — so the errors it reports are exactly the ones that
would abort a run — and adds advisory warnings on top.

```bash
culebra lint app.cul
# app.cul:12:7: warning: unused variable 'tmp'
# app.cul:20:3: error: undefined variable 'reuslt'

culebra lint .          # recurse into every .cul under the current directory,
                        # like `culebra fmt -i .`
```

Paths holding no `.cul` file exit 2 rather than reporting a clean run, so a
mistyped directory can't pass the gate.

A file named `test_*.cul` — the same convention `culebra test` discovers by
— is linted as that runner would run it, so `test` and `parametrize` count
as bound there. In any other file they are the undefined names they have
always been, and `culebra lint .` gets both right in one pass.

What it reports today:

- **Errors** — the sound, certain-to-fail set: `break` / `continue` /
  `return` out of place, malformed parameter or assignment forms,
  duplicate parameters or class members, shadowing, and reads of a name
  bound nowhere (the undefined-variable subset). These already abort any
  run; `lint` just surfaces them all at once instead of stopping at the
  first.
- **Warnings** — advisory findings that don't stop a run:
  - **Unused local variable** — a `let` / `mut` binding inside a function
    that is never read.
  - **Unused top-level binding** — a top-level `let` / `mut` that the
    module never reads and never re-exports. Function / class / enum /
    trait declarations are the module's export surface and are never
    flagged.
  - **Unused import** — an `import`ed name the module never uses.
  - **Unreachable code** — a statement that can never run because a
    `return` / `throw` / `break` / `continue` precedes it in the same
    block.
  - **Non-exhaustive enum match** — a `match` whose arms name some but
    not all of an `enum`'s variants, with no catch-all (`_`, a bare
    binding, or a type pattern naming the enum itself). `match` returns
    `nil` on no arm matching rather than raising, so a variant added
    later — or simply forgotten — falls through silently. Only checked
    when every arm's pattern unambiguously names one enum declared in
    the same file; a variant name two enums in the file both declare is
    skipped rather than guessed at. A guarded arm (`Circle(r) if r > 0
    => …`) does not count as covering what it names, since the guard may
    reject at runtime.

  A leading underscore (`_x`, or the bare sink `_`) marks a binding as
  intentionally unused and is never flagged. **Parameters are not
  flagged**: an unused parameter is overwhelmingly intentional in Culebra
  — a multidispatch clause or method signature fixes the arity, and a
  higher-order callback (a route handler `fn(req)`, an `|i| 4.0`) ignores
  an argument it must still declare — so the check would be all noise.
- **Idiom warnings** — rewrites with no exception, so no false positive:
  - **Redundant self-assignment** — `x = x + 1` says `x += 1` the long
    way (also `-=`, `*=`, `/=`).
  - **`.size()` compared to zero** — `xs.size() == 0` / `> 0` / `!= 0`
    asks what `.empty()` / `!xs.empty()` answers more directly.
  - **`range(0, n)`** — the explicit zero start is redundant; `range(n)`
    already means that.

  Two related rewrites — an `if`/`else` a ternary could express, and a
  manual index loop `enumerate()` could replace — stay prose in
  [`quick-guide.md` §3](quick-guide.md#3-what-does-not-carry-over)
  instead: both have real exceptions, so neither clears the zero-false-
  positive bar this check holds to.

`culebra lint --fix <paths...>` mechanically removes unused-import lines —
the only warning safe to autofix unattended, since deleting a dead
`import` can never change behavior (unlike an unused `let`/`mut`, whose
initializer may carry a side effect). Every other warning stays
report-only. After editing, the fixed source is re-parsed and re-linted;
the rewrite is written only if that re-check confirms the imports are
gone and no new error appeared — the same re-parse safety net `culebra
fmt` uses.

The fix deletes whole lines, so it only touches a line holding one
`import` and nothing else. An import `;`-joined to another statement is
reported and left for you: deleting its line would take the neighbour
with it, and the re-check cannot see that, since the file parses and
lints just as cleanly with a statement missing.

```bash
culebra lint --fix app.cul
# app.cul: fixed 1 unused import

culebra lint --fix joined.cul
# joined.cul: --fix skipped 1 unused import sharing a line with other code
# joined.cul:1:8: warning: unused import 'Math'
```

An unrecognized flag is an error (exit 2), not a path: `culebra lint
--fixx app.cul` reports the typo instead of linting the file with the fix
silently disabled.

Planned: a `--format json` mode for editor / LSP integration, and inline
`# lint: ignore` suppression.

---

## 3. Formatting (`culebra fmt`)

`culebra fmt [paths...]` reformats source to one canonical style:
normalized operator spacing, two-space indentation, brace blocks laid out
multi-line, and argument lists / collection literals wrapped when they
exceed the line width. It is opinionated and zero-config (no style flags).

```bash
culebra fmt app.cul          # write the formatted source to stdout
culebra fmt -i app.cul       # rewrite the file in place
culebra fmt -i .             # format every .cul under the current directory
culebra fmt --check app.cul  # exit 1 if it isn't already formatted (CI gate)
culebra fmt -l src/*.cul     # list the files that would change
cat app.cul | culebra fmt -  # stdin -> stdout (editor format-on-save)
```

A directory argument is scanned recursively for `.cul` files, so
`culebra fmt -i .` formats a whole project and `culebra fmt --check .`
gates it in CI. Paths holding no `.cul` file are an error (exit 2), so a
mistyped path fails loudly instead of silently formatting nothing.

The output modes compose: `culebra fmt -i -l .` rewrites the files that
need it *and* prints their names (like `gofmt -l -w`), and `-i --check`
does the same silently. With `-l` or `--check`, the exit code is 1 whenever some
file's formatting differed — so it still gates CI after a rewrite. The
formatted source goes to stdout only when no other output mode was asked
for.

`-` reads stdin and stands alone: combining it with file paths (two
inputs, one stdout) or with `-i` (no file to rewrite) exits 2 rather than
letting one side quietly win. `culebra fmt -i` with no paths is the same
case. An unrecognized flag is an error too, instead of being taken for a
file name — `culebra fmt --wirte app.cul` exits 2 and formats nothing, so
a typo can't look like a successful run.

Comments are preserved: a leading comment stays above the statement it
introduces, a trailing comment stays on the same line, and a single blank
line between statements is kept (runs of blank lines collapse to one).
match / cond arms, class / trait / enum members, destructuring patterns,
and parameter lists are all normalized; long binary expressions and method
chains wrap at the line width. An object or set literal that holds a comment
is written one entry per line, with the comment kept where you put it; one
without comments stays compact.

How it works: the source is parsed, re-printed from the syntax tree, and
then **re-parsed and compared** against the original — if formatting would
change the program's meaning, or would drop or duplicate a comment, `fmt`
refuses and leaves the file untouched rather than risk corrupting it.
Formatting is idempotent: running it twice yields the same result as
running it once.

### Editor integration

The stdin form (`culebra fmt -`) is the format hook: each integration
formats the whole buffer and applies the result only when it exits zero,
leaving the buffer untouched on a parse/safety error.

- **VSCode** — the bundled extension registers a document formatting
  provider, so **Format Document** and `editor.formatOnSave` work for
  `.cul` files out of the box once it's installed (`culebra init`, or
  `misc/vscode/install.sh` from a source checkout).
- **Zed** — add an external formatter in `settings.json`:
  `"formatter": { "external": { "command": "culebra", "arguments": ["fmt", "-"] } }`
  under `"languages": { "Culebra": { ... } }`.
- **Vim/Neovim** — the bundled `ftplugin` provides a `:CulebraFmt`
  command (cursor preserved, untouched on error); set
  `let g:culebra_fmt_autosave = 1` for format-on-save. It deliberately
  skips `gq` / `'formatprg'`, which would replace an unparseable range
  with empty output.
- Any other editor with a format-on-save hook can pipe the buffer through
  `culebra fmt -` the same way.

---

## 4. Debugging (`culebra dap`)

Culebra ships a [Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol/)
(DAP) server, so you can set breakpoints, step, and inspect variables
visually in any DAP-capable editor — VSCode, Vim (vimspector), Zed,
Emacs (dap-mode), Helix, and others. One adapter serves every editor.

```
culebra dap        # speak DAP over stdin/stdout
```

You rarely run this by hand; your editor launches it (see the per-editor
setup below).

**Prerequisites**

- **`culebra` must be runnable** — either on your `PATH` or referenced by
  an absolute path in the editor config below. (Quick check:
  `echo | culebra dap` should hang waiting for DAP input; `Ctrl+C` to
  quit. If it errors, fix the path first.)
- **Debugging runs in the interpreter** — don't pass `--jit`. The adapter
  runs your program in interpreter mode; the JIT/AOT backends compile to
  machine code and aren't source-debuggable.

**How it works.** The adapter uses the interpreter's per-statement hook
to pause on a breakpoint, a `debugger` statement, or a step, while the
DAP loop answers the editor's requests and resumes execution. Your
program's `stdout`/`stderr` is forwarded to the editor's debug console as
`output` events, so it never collides with the protocol.

Supported:

- **Line breakpoints**, **conditional breakpoints** (break only when an
  expression is truthy, e.g. `i == 3`), and the `debugger` statement
- **Stepping**: continue, step over (`next`), step in, step out; stop on
  entry
- **Call stack**: a named multi-frame stack (`inner` ← `outer` ← `main`),
  each frame with its source location
- **Variables**: in-scope locals of the selected frame — pick a frame in
  the call stack to inspect its own locals
- **Watch / evaluate**: evaluate an expression in the selected frame
  (watch panel, hover, debug console) — e.g. `x + y`, `arr[0]`, a
  function call
- **Set variable**: edit a `mut` variable's value in the selected frame
  (an immutable `let` binding is rejected); the change takes effect in
  the running program
- **Program output** in the debug console

Notes that apply to every editor:

- The program to debug and its arguments come from the `launch` request's
  `program` field, not the `culebra dap` command line.
- Breakpoints are matched by canonical (symlink-resolved) path, so a
  breakpoint set on a file under a symlinked directory still binds.
- A `debugger` statement in source forces a stop regardless of
  breakpoints — handy for a one-off pause without configuring anything.

### Quick setup: `culebra init`

Run `culebra init` in your project directory to install or update the
editor integration (syntax highlighting + the `culebra dap` debug
adapter) for whichever of VSCode, Vim, Neovim, or Zed it finds on this
machine, plus AI coding agent instructions — no source checkout
needed, since the payload travels inside the binary. Zed is the one
exception to "no source checkout needed" for its syntax grammar: Zed
can only fetch a Tree-sitter grammar from a git repository, so `init`
points it at this binary's release tag on the public repo instead of
a local checkout (see the Zed section below for what that means and
its one manual step in Zed's UI).

`init` prints what it's about to do and, at an interactive terminal,
asks you to confirm before touching anything; a non-interactive run
(piped, CI) or `--yes`/`-y` skips the prompt and applies immediately.
It's safe to re-run any time; every run overwrites with whatever this
binary carries, so re-running after an upgrade is the update path.

The per-editor steps below build the same integration from a source
checkout instead — for contributing to it, or if `init` can't reach
your setup.

### VSCode

VSCode needs a tiny extension to highlight `.cul` files and register the
`culebra` debug type (debugging is pure registration — all the logic lives
in `culebra dap`). Publishing isn't required: the repo ships a template
under `misc/vscode/` with an installer.

1. Install the extension — `culebra init`, or from a source checkout:

   ```sh
   misc/vscode/install.sh
   ```

   Both package the extension as a `.vsix` and install it with
   `code --install-extension` — the method VS Code supports. (Copying the
   folder into `~/.vscode/extensions` directly is *not* supported and is
   often not detected.) If `culebra` is on your `PATH`, its absolute path
   is baked into the debug adapter config. Both work with
   `code-insiders` / `cursor` / `codium` if that's your CLI; if none is on
   `PATH`, `install.sh` tells you how to install the `.vsix` from the
   Extensions view instead. Syntax highlighting then applies to
   any `.cul` file with no further setup; the steps below are only needed
   for debugging. The grammar's keyword list is generated from the parser
   by `just sync-grammar` (the same source as the Vim syntax file), so it
   never drifts from the language.
2. Fully quit VSCode (<kbd>Cmd</kbd>+<kbd>Q</kbd>) and reopen — a freshly
   installed extension may not be picked up by a window reload alone.
3. In your project, add `.vscode/launch.json`:

   ```jsonc
   {
     "version": "0.2.0",
     "configurations": [{
       "type": "culebra",
       "request": "launch",
       "name": "Debug current file",
       "program": "${file}",
       "cwd": "${workspaceFolder}",
       "stopOnEntry": false
     }]
   }
   ```
4. Open a `.cul` file, click in the gutter to set a breakpoint, and press
   <kbd>F5</kbd>.

> **Iterating on the extension itself?** Instead of reinstalling the
> `.vsix` on every change, open `misc/vscode/` in VSCode and press
> <kbd>F5</kbd> with an `extensionHost` launch config — that opens a
> separate *Extension Development Host* window with the extension loaded
> live, where you debug your `.cul` project. For just *using* the
> extension, `install.sh` above is simpler.

### Vim (vimspector)

No extension needed — install [vimspector](https://github.com/puremourning/vimspector),
then add `.vimspector.json` to the project root (vimspector configs are
per-project, so this isn't something `culebra` can ship for you). With the
Vim syntax files installed (`misc/vim/install.sh`), open any `.cul` file in
the project root and run `:CulebraVimspectorInit` to write it — it won't
overwrite an existing `.vimspector.json`. Or create it by hand:

```json
{
  "configurations": {
    "Debug file": {
      "adapter": { "command": ["culebra", "dap"] },
      "configuration": {
        "request": "launch",
        "program": "${file}",
        "stopOnEntry": false
      }
    }
  }
}
```

vimspector ships **no key mappings by default**, so add this to your
`vimrc` once — otherwise `<F5>`/`<F9>` do nothing and it looks like setup
failed:

```vim
let g:vimspector_enable_mappings = 'HUMAN'
```

Then set a breakpoint with `<F9>` and start with `<F5>` (the `HUMAN`
mappings; `<F10>`/`<F11>`/`<F12>` step over/in/out, `<F3>` or
`:VimspectorReset` to stop). No gadget install (`:VimspectorInstall`) is
needed — the adapter is launched straight from the `command` above over
stdio. Vim must be a `+python3` build.

For syntax highlighting, run `culebra init`, or `misc/vim/install.sh`
from a source checkout.

### Zed

Zed needs an extension for both syntax highlighting (a Tree-sitter
grammar) and debugging (a debug adapter must be *registered* by an
extension — Zed can't point at an arbitrary DAP command from `debug.json`
alone). Both ship as one dev extension.

`culebra init` writes that extension the same way it lays down the
VSCode/Vim payload — no source checkout needed for the adapter shim or
language config, since they travel inside the binary. The one thing it
can't avoid is how Zed fetches a Tree-sitter grammar: a dev extension can
only name a git `repository` + `rev` + `path`, never bundle the parser
source directly. So the extension `culebra init` writes points at this
binary's `vX.Y.Z` release tag on the public repo
(`github.com/yhirose/culebra`) instead of a local path — exact for a
release download, since the binary and the tag always match. Editing the
grammar itself needs a source checkout instead — see "Building from a
source checkout" below.

`init` writes the extension to `~/.local/share/culebra-zed-extension`
and `.zed/debug.json` for the current project. Install it once in Zed:

1. Command palette → **`zed: install dev extension`** → select the
   directory `init` printed. Zed compiles the Rust adapter shim to
   `wasm32-wasip2`, so you need a recent Zed plus Rust with that target:
   `rustup target add wasm32-wasip2` (without it the build fails with
   "can't find crate for core" and the adapter never registers;
   highlighting still works).
2. Re-run `culebra init` and re-select the directory after upgrading —
   this re-pins the release tag.

Then open a `.cul` file (it highlights), set a breakpoint, and run
**"Debug current Culebra file"** from the debug panel. The generated
`.zed/debug.json` is:

```jsonc
[
  {
    "label": "Debug current Culebra file",
    "adapter": "culebra",
    "request": "launch",
    "program": "$ZED_FILE",
    "cwd": "$ZED_WORKTREE_ROOT",
    "stopOnEntry": false
  }
]
```

> Zed's debugger and extension APIs are newer than VSCode's and still
> evolving, so the exact keys / build steps may differ in your version. If
> the dev extension fails to build or the adapter doesn't launch, check
> the Zed version against the `zed_extension_api` version in
> `misc/zed/Cargo.toml`.

#### Building from a source checkout

Editing the grammar or the adapter itself needs `misc/zed/install.sh`
instead of `culebra init` — it points Zed at your local checkout's
`HEAD` (via `file://`), so uncommitted or unpushed changes show up
immediately, which the release-tag path above can't do:

```sh
misc/zed/install.sh
```

This builds the same extension shape (grammar + adapter +
`.zed/debug.json`) from `misc/zed/` directly. Re-run it — and re-select
the directory in Zed — after pulling grammar/adapter changes; it
re-pins the commit each time.

---

## 5. Reading the docs (`culebra docs`)

Every document in this reference set is compiled into the binary, so
`culebra docs` answers from the same build that runs the code — there is
no checked-out tree to fall behind it and no network to reach. The
release archive carries no `docs/` directory precisely because it does
not need one.

```
culebra docs                     # list the topics, with a size estimate
culebra docs stdlib              # print one topic
culebra docs -g 'Math.wrap'      # print the sections that match
culebra docs stdlib -g 'wrap'    # search one topic
culebra docs --ja ...            # the Japanese edition
```

Exit status is the grep convention: `0` printed something, `1` nothing
matched, `2` bad usage. That makes an existence check a one-liner, with
no output to read:

```
culebra docs -g '<name>' >/dev/null || echo "no such API"
```

(A literal name in that position would be searching for itself — this page
is part of the corpus.)

### Searching

`-g` takes a regular expression, matched case-insensitively unless the
pattern contains a capital. A pattern that does not compile — usually a
signature fragment like `get_or_put(` — is searched for literally
instead of rejected, with a note on stderr.

The unit of output is a section: a heading and the text under it. Hits
are ordered by where the match landed, because a heading match in
`stdlib.md` is nearly always a signature, and that is what the reader
was after:

1. the heading, 2. a code block, 3. body text.

Past eight matching sections the output degrades to an index of
headings, so a broad pattern costs a screen rather than the whole
reference. The strongest few heading matches still print in full.
`--full` overrides this, and `--at <line>` prints one section
uncapped — the line number comes from the locator above each hit.

Sections stay small by construction (21 lines at the median), so a hit
is normally shown whole; anything past 60 lines is truncated with a
pointer to `--at`.

### Which topic to read

`culebra docs quick-guide` is the condensed pack: the syntax, the habits from
other languages that do not carry over, and every standard-library
signature, in a file that fits in a prompt. It is the one to read
before writing culebra rather than to search. The rest of the set is
larger than a prompt window, which is what `-g` is for.

`culebra docs agent` is shorter still, and the only topic written to
leave the program: rules to append to whatever file a coding agent
already reads, so it looks signatures up here instead of guessing them.

```
culebra docs agent >> CLAUDE.md                        # Claude Code
culebra docs agent >> .github/copilot-instructions.md  # GitHub Copilot
culebra docs agent >> AGENTS.md                        # Codex, Cursor
```

The destinations are listed on stderr, not stdout, so a redirect gets
the markdown alone. Neither `agent` nor `llm` takes part in a
corpus-wide search — both condense the other topics, and would report
the same API twice — but naming one searches it: `culebra docs agent -g …`.

`culebra init` does this append for you — it detects which of the three
files above already exist (creating `AGENTS.md` if none do) and keeps
the block up to date on every re-run. Reach for `culebra docs agent`
directly only when you want the raw markdown or a destination this
list doesn't cover.

## 6. Serving static files (`culebra serve`)

`culebra serve [-p PORT] [-d DIR]` serves one directory over plain HTTP —
a drop-in for `python3 -m http.server` when you just want a set of files
(a `culebra build` output, a docs preview, a static site) reachable on
`localhost`, with no OpenSSL, routes, or WebSocket in the picture. Port
defaults to `8000`, directory to the current one.

```
culebra serve                  # serve . on :8000
culebra serve -p 3000 -d site  # serve site/ on :3000
```

For routes, WebSocket, or an API alongside static files, reach for the
`Http.server()` stdlib namespace instead — see
[`stdlib.md` §15](stdlib.md#15-http).
