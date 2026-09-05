# hm — Algorithm W, next to linz

This example exists to answer a specific question: after writing
`examples/linz/lin.cul` (a type *checker* — every type is already written
down; the job is confirming it), would `examples/linz`'s `typing.cul` shape
generalize into a reusable "type system toolkit" for a second, differently
shaped language? `hm.cul` is a type *reconstructor* — Hindley-Milner
inference (Algorithm W) over `Int`/`Bool`/`if`/`fn`/application/`let`, with
no type annotations anywhere in the source — chosen because it stresses a
genuinely different part of the problem: unification and substitution, not
just environment bookkeeping.

## What actually transferred

- **The grammar recipe.** `%whitespace` + `%word` + a `RESERVED` exclusion
  in `IDENT` is the same three pieces in both files, changed only in which
  keywords are listed. This is a `PEG` lesson, not a type-system one — it
  would transfer to any keyword-based DSL, typed or not.
- **`fail(node, msg)` → `throw {kind, message, line, column}`.** Third
  project to use this shape (after `pl0.cul`). Worth knowing as the house
  style for a `PEG`-driven tool's errors; not worth extracting into a
  function shared across files, since the `kind` string is what makes each
  language's errors distinguishable to its own driver.
- **`bound(env, name, value)`.** A 3-line copy-then-subscript helper,
  identical in both files, because Object literals have no computed-key
  form and every binding here is by a name read from the source — a
  compile-time-unknown key. This is real, recurring friction (see the
  `%word` / computed-keys conversation this example grew out of), but the
  helper is 3 lines; a stdlib function would save less than writing it.
- **The driver.** `import`, strip `--ast` out of `Sys.argv`, `PEG.parse`
  with `path:`, catch by `kind`. ~15 lines, identical shape to `pl0.cul`
  and `lin.cul`. Not worth factoring — the two lines that differ (which
  module to import, which error `kind` to match) are the only ones that
  matter to a reader.

## What didn't

Everything that is actually type-system logic is shaped by which of the two
problems it's solving, not by "type systems" as a category:

- **What gets threaded through the return value differs in kind, not just
  content.** `lin.cul`'s `typing` returns `(type, env')` — one piece of
  state, the environment, updated by use. `hm.cul`'s `infer` returns
  `(type, subst)` — *two* pieces, and every multi-step case has to
  `compose` the new substitution into the ones already collected before
  it can even look up a variable's current type. A generic "thread state
  through" helper would have to already know how many things there are
  to thread and how to combine them, which is most of the design problem,
  not infrastructure for it.
- **The environment doesn't have the same algebra.** `lin.cul`'s env
  supports insert / remove / filter-by-qualifier and nothing else — a
  variable's type is exactly what was stored, unchanged, until consumed.
  `hm.cul`'s env needs `apply_env`, which walks every stored scheme and
  rewrites the types inside it through the current substitution — an
  operation that only makes sense because HM types can still change after
  they're stored. An "environment module" generic enough to cover both
  would have to be generic over "what a stored value even means," which
  makes it Object again, not a library.
- **Half of `hm.cul` has no counterpart in `lin.cul` at all**: `apply`,
  `compose`, `unify`, `bind`/occurs-check, `generalize`/`instantiate`. This
  is Algorithm W's actual content — a checker that never invents a type
  (linz's) has nothing here to share, because it never needs to solve for
  an unknown.
- **Consumption vs. accumulation.** `lin.cul`'s env only shrinks
  (`unbound` on use); `hm.cul`'s env only grows or gets rewritten in place,
  never shrinks. Opposite lifecycles for the same-looking `Object`.
- **Fresh names.** `hm.cul` keeps a module-level `mut next_id` counter for
  fresh type variables — deliberately not threaded functionally, unlike
  everything else in either file — because nothing else here needs to
  synthesize a name that didn't come from the source. `lin.cul` has no
  equivalent at all.

## Conclusion

A "PEG + type inference + VM → easy DSLs" stdlib addition doesn't hold up:
the reusable ~20% (grammar shape, error shape, the `bound` idiom, the
driver skeleton) is either already established convention or too small to
be worth a library function: the ~80% that's actual type-system logic is
irreducibly specific to which type system you're building. `PEG` (grammar
→ tree) and `CodeGen` (IR → VM/AOT) already are the reusable, stdlib-level
pieces of "write your own language" — see `examples/pl0/pl0_codegen.cul`.
The type system in between is the part every language author has always
had to write for themselves, and that isn't specific to culebra.
