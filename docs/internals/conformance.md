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
| `tests/test_multidispatch.cul` | §20 |
| `tests/test_object_keys.cul` | §10 (non-String keys) |
| `tests/test_runtime_errors.cul` | §15 (`throw`/`try`/`catch`, all `kind` values catchable) |
| `tests/test_set.cul` | §10 (sets) |
| `tests/test_tuple.cul` | §10 (tuples, destructuring) |
| `tests/test_ufcs.cul` | §10 (methods, UFCS), §19 (`__ARGS__`) |
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
`tools/checks/check_spec_examples.sh` counts the sections of this document
with no runnable ` ```culebra ` block (one not marked
`# doctest: skip`, since `just doctest` runs the rest on both
engines) and compares them against
`tools/checks/spec_unpinned_sections.txt`. A section that loses its example,
or a new one that never had one, fails the check; a listed section
that gains one fails it too, so the file only shrinks.

Whether an individual `Ns.fn` the stdlib reference documents, or a
grammar keyword, has a durable caller at all:
`tools/checks/check_api_coverage.sh` reads the signature index generated
into `docs/quick-guide.md` and the keyword set parsed out of the PEG
grammar, and checks each name against `tests/*.cul`, `tests/*.sh` and
the doctest blocks `just doctest` runs. A documented name with no
caller anywhere fails the check unless it is filed in
`tools/checks/api_untested.txt`; a filed name that gains one fails too, so
the file only shrinks.

Both run as part of `check-generated`, so `just test-dev` and CI both
carry them.
