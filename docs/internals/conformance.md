# Conformance: what pins the spec

`docs/language.md` and `docs/stdlib.md` say what the language is. This is
the other half — what runs against them, and what fails when a rule stops
being executed anywhere. It lives here rather than in the spec itself
because a reader who has only the binary has none of these files.

## 1. Test files, by chapter

Every section of the language spec has at least one corresponding test
file under `tests/`. `just test` runs VM/JIT diff, AOT diff, and
embedding C++ smoke in one pass; `just test aot` runs only the AOT
diff. The mapping below points to the primary owner; some tests
touch multiple sections, marked "(broad)".

| Test file | Spec sections verified |
|---|---|
| `tests/test_core.cul` | §6, §7, §8, §9, §10, §11, §12, §15, §18, §19 (broad — primary unit-test catch-all) |
| `tests/test_class.cul` | §10 (class sugar, operator overloading, `__str__`, auto-reflection, static methods), §11 |
| `tests/test_class_parameters.cul` | §10 (auto-synthesized `parameters()`) |
| `tests/test_decorator.cul` | §21 |
| `tests/test_defer.cul` | §15 (`defer`, scope-guard pattern) |
| `tests/test_forward_ref.cul` | §6 (scope), §11 (closures), §20 |
| `tests/test_iter.cul` | §12 (`for ... in`), §18 (iterator protocol, String methods), §19 (`range`, `iota`) |
| `tests/test_iter_combinators.cul` | §18 (lazy combinator families, unbounded-source laziness) |
| `tests/test_iter_combinators_group.cul` | §18 (`min_by`/`max_by`, `to_set`/`to_object`/`group_by`/`partition`) |
| `tests/test_iter_combinators_stream.cul` | §18 (`unzip`/`flatten`/`scan`/`distinct`/`tap`/`step_by`/`chunk_by`, unbounded-source laziness) |
| `tests/test_iter_terminal.cul` | §18 (terminal iterator methods, §18.5 protocol contract) |
| `tests/test_kwargs.cul` | §11 (keyword arguments, `**` splat), §20 (kwargs in multimethods), §7 (evaluation order for mixed calls) |
| `tests/test_match_class.cul` | §13 (type patterns) |
| `tests/test_type_pattern.cul` | §13 (a bare type name as a pattern; declaring patterns bind every name) |
| `tests/test_multidispatch.cul` | §20 |
| `tests/test_object_keys.cul` | §10 (non-String keys) |
| `tests/test_runtime_errors.cul` | §15 (`throw`/`try`/`catch`, all `kind` values catchable) |
| `tests/test_catch_pattern.cul` | §15 (`catch` clauses: patterns, guards, order, what no clause takes) |
| `tests/test_error_kind_types.cul` | §15 (an error's kind as a type name: clause, arm, annotation, dispatch) |
| `tests/test_set.cul` | §10 (sets) |
| `tests/test_tuple.cul` | §10 (tuples, destructuring) |
| `tests/test_ufcs.cul` | §10 (methods, UFCS), §19 (`__ARGS__`) |
| `tests/test_ufcs_first_param.cul` | §10 (UFCS: the first parameter decides) |
| `tests/test_args.cul` | stdlib §10 (`Args`) |
| `tests/test_fs.cul` | stdlib §3 (`FS`) |
| `tests/test_json.cul` | stdlib §9 (`JSON`) |
| `tests/test_tensor.cul` | stdlib §8 (`Tensor`) |
| `tests/test_tensor_nn.cul` | stdlib §8 (`Tensor`) |
| `tests/test_tensor_nn_xent.cul` | stdlib §8 (`Tensor`) |
| `tests/test_tensor_nn_ops.cul` | stdlib §8 (`Tensor`) |
| `tests/test_tensor_nn_graph.cul` | stdlib §8 (`Tensor`) |
| `tests/test_tensor_ops.cul` | stdlib §8 (`Tensor`) |
| `tests/test_tensor_ops_rope.cul` | stdlib §8 (`Tensor`) |
| `tests/test_time.cul` | stdlib §5 (`Time`) |
| `tests/test_import.cul` | §24 (Modules) — uses `tests/test_import_helpers/*.cul` as dependencies |

Every test file in `tests/` is required to pass on both backends
with identical stdout — `just test` enforces it. Interactive
features that are inherently backend-specific (debugger hooks
exercised by REPL state) are tested through `tests/embedding/`
C++ smoke tests rather than as `.cul` scripts.

The module chapter is covered by one file and its helpers:

`tests/test_import.cul` exercises the happy path on both
backends (basic import, mixed function / class exports, multiple
`export` statements, chained imports). The supporting modules
live under `tests/test_import_helpers/`. Error cases
(circular imports, top-level violations, duplicate exports) are
covered by inline `try { ... } catch { ... }` in the same file
where the failing source is itself another helper.

## 2. Ratchets

The table above is maintained by hand and points at chapters. Two
finer questions are held by ratchets instead.

Whether an individual section states a rule that nothing executes:
`tools/checks/check_spec_examples.sh` counts the sections of `docs/language.md`
with no runnable ` ```culebra ` block (one not marked
`# doctest: skip`, since `just doctest` runs the rest on both
engines) and compares them against
`tools/checks/spec_unpinned_sections.txt`. A section that loses its example,
or a new one that never had one, fails the check; a listed section
that gains one fails it too, so the file only shrinks.

Whether an individual `Ns.fn` the stdlib reference documents, or a
grammar keyword, has a durable caller at all:
`tools/checks/check_api_coverage.sh` reads `tools/checks/api_surface.txt`
— every signature the reference documents, written by the same pass that
generates the quick-guide index — and the keyword set parsed out of the PEG
grammar, and checks each name against `tests/*.cul`, `tests/*.sh` and
the doctest blocks `just doctest` runs. A documented name with no
caller anywhere fails the check unless it is filed in
`tools/checks/api_untested.txt`; a filed name that gains one fails too, so
the file only shrinks. The surface file is deliberately not the index in
`docs/quick-guide.md`: that one lists only the namespaces the context pack
carries inline, so reading the population from it would have dropped half
of this gate's names the moment a namespace moved to being named there.

Both run as part of `check-generated`, so `just test-dev` and CI both
carry them.

## 3. Tiers: where a check runs

Every check above costs a population times a per-item price, and the two
lanes an engine check can run on are not close to each other in price:
the files of `tests/` (327, recorded in `gate_budget.txt`) cost far more
under `--jit` than under the executor, because what the JIT lane pays for
is LLVM rather than execution (a heavy file spends seconds in the IR
pipeline and in the backend, and milliseconds running). An axis applied to the whole corpus on that
lane therefore costs about as much as everything else here put together,
which is why where a check runs is part of what it is.

The justfile's `gate_rows` is the single declaration of that placement:
one row per phase, saying what it needs (the checkout, the binary, a build
tree), which local tiers run it, which CI shard runs it, and what it
measured. Every lane — `just check`, `just test-dev`, `just test`, and each
CI shard — is a filter over that table, so a phase reaches the gates by
declaring itself rather than by being pasted into each of them, and
`gate_table_selftest` refuses a phase that names no shard, a build-tree
phase riding a binary-only shard, and a lane that selects nothing.

| tier | question it answers | what it holds |
|---|---|---|
| `just check` | did this edit break what the tree already asserts | the source and IR ratchets, and every assertion on the executor, twenty files to a process |
| `just test-dev` | may this reach master (`just land` runs this and nothing else) | the above, plus the JIT lane over the op cover, the `-O0` and faststart codegen axes, the CLI half of `ctest`, the language front ends, isolate |
| `just test` | may this be pushed | every axis over every corpus, AOT included |
| CI | both operating systems, and what a laptop cannot see | the full sweeps, the refcount and leak lanes, the platform and window builds |

What the landing gate holds is decided by where breaks have actually got
through rather than by what is cheapest to run: the CLI `ctest` entries
(two escapes, `jit_error_pos_test` and `search_model_test`) and the
`-O0` / faststart codegen axis (three bugs invisible at the default `-O2`)
are in it, while an axis that re-sweeps the corpus under a GC or refcount
setting — where the detection belongs to the axis rather than to the
corpus — runs once, in `just test` and in CI.

### The JIT lane's subset

`tools/checks/jit_shape_set.txt` is what the landing gate's `--jit` leg
sweeps: the smallest set of test files that still lowers every bytecode op
the executor implements, seeded with the files
`tools/checks/codegen_sensitive.txt` names, plus whatever test files the
branch touches. It is 31 files covering 152 of the 155 ops; the three it cannot reach
are filed in the set with the reason, and that list may only shrink.
`tools/checks/check_jit_shape_set.sh` recomputes the coverage from the
compiled bytecode on every run of the gate, so a new op fails the gate
until a test reaches it.

What the cover does not hold is op *combinations* — an unwind edge inside
a loop inside a closure. That is what the codegen seeds carry by hand, what
the generated corpus (`tools/difftest`, ~21k cases on both lanes) covers by
construction, and why the full sweep still runs in `just test` and
in CI's `ci-gc` on every push.

CI's macOS job sweeps the cover as well, in place of the full sweep: the
full one is five minutes of its three cores, nearly all of it LLVM. The
full sweep therefore runs on x86-64 Linux only, and a break that needs
both the arm64 backend and a file outside the cover is seen when
`just test` is run on a Mac, not on a push.

### The gate's own cost

`tools/checks/gate_budget.txt` records each swept population — the corpus,
the isolate files, the generator probes, the `vm_cases`, the ctest
entries, the doc blocks, the language samples — and `check-gate-budget` holds them exactly. Growth is
normal and is made here, in the commit that grows it, so that a gate that
has become slow shows where its time went. Seconds are
deliberately not the ratchet — the same lane varies by 1.5–2× on a loaded
machine — so the table's per-phase costs are reported against the measured
budget at the end of a lane and gate nothing.

A lane compiles a file once. The phases that put the corpus through
`--jit` again under a collector setting — gc-stress's two legs, the three
modes of `vm_cases` — share one JIT object cache with the sweep that
compiled it (`jit_cache` in the justfile),
so what they repeat is the run and not LLVM: the collector's settings
change how the code runs, not what it is. The compiles were 87% of
gc-stress's CPU. This is why the full vm/jit sweep and gc-stress share a
CI shard.

That cache is kept from run to run — under the user's cache directory,
`culebra/gate-jit`, shared by every worktree — so a later run starts
from what an earlier one compiled: an object is keyed by the optimized
module and the backend ([vm.md](vm.md) §7), a rebuild that changes
neither finds it, and what it skips is the backend — 53% of the corpus's
compile, the IR pipeline being run every time to arrive at the key.
`CULEBRA_GATE_JIT_CACHE` names another directory, or `off` for one that
lasts the run; a directory the gate cannot write is said in one line and
passed over the same way. Two things follow from keeping it. The
per-file compile budget of the vm/jit sweep reads a full compile only
the first time a module is seen. And an object outlives the run that
made it, so a JIT failure that a rerun does not explain is rerun with
`off` before it is believed.

### What is exempt, and why it has to say so

A check that does not run is a check that cannot fail, and two of the ways
this tree lost coverage were exemptions nobody was holding:

**`# doctest: skip`.** A skipped block is never executed, so a documented
form that stops working stays documented. A skip is justified one of two
ways: the block cannot run at
all, or it says why it must not run here (`# doctest: skip — opens a
window`). `check_doctest_skips.sh` runs the skips that give no reason on the
executor, and a block that runs cleanly fails the gate — un-skip it, or give
it the reason a reader wants anyway. Blocks that need a window, a server, a watcher, stdin, the network, an
external program or this machine's own paths carry a reason; an
illustration that simply runs has no skip.

Some skips hold on one machine and not on another, which is why the lane
runs on every platform: an example that cannot run on one machine can run
on another. `Net.listen(7000)` and its accept loop hang on a runner and
failed on this laptop; `Proc.spawn(["python", ...])` runs where `python` is
on PATH and fails where only `python3` is. A skip that holds for the machine
you are on is not a skip that holds.

**`examples/`.** The suites under `examples/` (47, recorded in
`gate_budget.txt`) run in every lane: `run_examples_sweep` runs them in one
executor process, and they are held to the optional-namespace rule
alongside `tests/*.cul`.
