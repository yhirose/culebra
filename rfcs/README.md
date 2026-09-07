RFCs
====

A feature proposal is written up before it is implemented, so the design
is reviewed while it is still cheap to change. This directory holds the
form to fill in, two worked examples, and every proposal that has been
decided on, including the ones that were turned down.

When to write one
-----------------

Write an RFC for anything that changes what the language or its standard
library *is*: new syntax, a new stdlib namespace or type, a change to
existing semantics, a new backend capability. This holds for a proposal
from anyone, the maintainer included.

Skip it for bug fixes, documentation, refactors, performance work that
preserves behavior, and tests. Those are ordinary PRs. If you are not
sure, open the PR and ask; being told "this needs an RFC" costs less
than writing one that wasn't needed.

How to propose
--------------

1. Copy [`template.md`](template.md) to `0000-<slug>.md` in this
   directory. The `0000` is literal: don't guess the next number and
   don't wait to be given one. The slug is a short kebab-case name, so
   a proposal titled "Functional state machines" becomes
   `0000-functional-state-machines.md`.
2. Replace every `[bracketed]` instruction with your answer.
3. Open a PR containing that one file.
4. Discussion happens as review comments on the PR.

The nine questions
------------------

The template asks for these, in this order. They are ordered so the
cheap answers come first, and #7 in particular can end the discussion
early.

1. **What** it is, in a sentence or two.
2. **Use cases**, at least two, concrete.
3. **Why existing features fall short** - a specific gap, not a
   preference.
4. **Syntax**, two or more options, each with pros/cons and real code.
5. **Performance**, including cost paid by programs that don't use it.
6. **Safety**: crashes, leaks, deadlocks, backend parity.
7. **Whether a preamble (pure `.cul`) is enough**, with no core changes.
8. **Implementation size**, when #7 is no.
9. **Backend symmetry**: can the executor, `--jit` and AOT all behave
   identically.

Plus a free-form `Notes` section for whatever didn't fit.

Numbering and status
--------------------

Every proposal carries a `Status` of `Draft`, `Accepted`, `Rejected` or
`Deferred`. A PR under discussion is `Draft`.

On merge, in the same PR: rename the file from `0000-` to the next
number actually free in this directory (the first proposal to land
becomes `0001-`), change the `# RFC 0000:` heading to match it, and set
`Status` to what was decided.

`Date` is when the proposal was written, and stays as it was; the rest
of its timeline is in the git history and the PR.

Rejected and deferred proposals are merged too, with that status. A
closed PR is easy to lose; a file in this directory is not, and the
reasoning for turning something down is worth as much later as the
reasoning for taking it.

If two RFC PRs are open at once, both are `0000-` and they don't
collide. Whichever merges second picks the next free number.

What belongs in the PR
----------------------

**The RFC file, and nothing else.** No implementation, no prototype
diff. That keeps the proposal mergeable on its own, whatever is decided:
extracting one file out of a mixed PR in order to record a rejection is
work nobody wants to do.

If you have working code that supports the proposal, put it on a branch
or a gist and link to it. Short illustrative snippets belong inline in
the RFC itself, which is what questions #4 and #7 ask for. Implementation
lands as a separate PR after the proposal is accepted.

Supplementary material that isn't part of the proposal itself, such as
full benchmark output, screenshots, alternatives you explored and
discarded, or the conversation that prompted the idea, goes in the PR
description or in comments. None of that is part of the diff, so none of
it is merged. Anything a future reader would actually need should be
condensed into `Notes` in the file, even as a single line pointing at
the PR.

Examples
--------

Two real features, written up after the fact, to show what a filled-in
RFC looks like at both ends of question #7:

- [`example-state-machine.md`](example-state-machine.md) - a feature
  that stayed entirely in a preamble, so #7 is yes and #8 is N/A.
- [`example-algebraic-effects.md`](example-algebraic-effects.md) - a
  feature that needed new grammar, a transform pass and JIT runtime
  changes, so #8 and #9 carry real weight.
