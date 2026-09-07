<!--
Copy this file to rfcs/0000-<slug>.md (a literal "0000") and replace
every [bracketed] instruction with your answer. Open a PR containing
that one file and nothing else.

README.md in this directory has the rest: when a proposal needs an RFC,
how numbering and Status work, where prototype code and supplementary
material go, and two filled-in examples.
-->

# RFC 0000: [title]

- Status: [Draft | Accepted | Rejected | Deferred]
- Author: [name]
- Date: [YYYY-MM-DD]

## 1. What

[One or two sentences: what is being proposed.]

## 2. Use cases

[At least **two** concrete use cases. For each: the scenario, and why
today's culebra handles it badly. A short "before" snippet of what the
code looks like without this feature helps, but isn't required.]

## 3. Why existing features fall short

[Why the use cases above can't be solved well with what culebra already
has (stdlib, syntax, patterns). Not "it would be nicer": a concrete gap.]

## 4. Syntax

[Two or more options. For each: pros/cons, and a real code example
written against the use cases in #2, as its own fenced `culebra` code
block rather than inlined into prose.]

## 5. Performance

[Any cost this adds to the hot path (allocation, dispatch, compile
time), even if the feature itself is opt-in.]

## 6. Safety

[Anything that can crash, leak, deadlock, or break executor/JIT/AOT
parity if misused. "None that I can see" is a valid answer, but say so
explicitly.]

## 7. Can this be done in a preamble (pure .cul), with no core changes?

[If yes: #8 doesn't apply. Mark it N/A, sketch the preamble here, and
move on to #9. If no: say specifically what forces a core change (new
syntax, a new bytecode op, a capability .cul can't express).]

## 8. Implementation size estimate

[Skip if #7 is yes. Otherwise: rough shape. Which files/layers are
touched, and small/medium/large.]

## 9. Backend symmetry

[Can the executor, `--jit`, and AOT all implement this with the same
behavior, errors, and timing? Note anything that looks structurally hard
for one backend. Answer this even when #7 is yes: pure-.cul code still
inherits backend behavior that's worth a one-line sanity check.]

## Notes

[Anything that didn't fit above: open questions, things you're unsure
about, alternatives you didn't fully explore.]
