# regexlib find_all/search DFA acceleration — design

> **Status: Draft (2026-05-30).** Architecture agreed; not yet implemented past
> M0. This is the internal spec for extending the lazy DFA (today: `test()`
> only, see B3) to `find_all`/`search`. User-facing reference stays in
> `docs/regexlib.md`.

## Problem

`find_all`/`search` run the Pike VM. Profiling (B4, 1MB subjects, macOS
`sample`) shows the hot cost is `add_thread`'s ε-closure plus the `Saves`
capture copy-on-write — not grapheme segmentation (B2 already removed that).
That per-step thread/capture bookkeeping is structural to a Pike VM and can
only be removed by a DFA, which keeps the engine 1–2 orders of magnitude behind
RE2 on dense-match patterns.

## Core idea

**The DFA does not produce the match. The DFA finds, fast, where a match can
begin; the Pike VM remains the single source of truth for the leftmost-first
(Perl) end and for captures.**

Rationale: a DFA is naturally **leftmost-longest (POSIX)**; the engine's
contract is **leftmost-first (Perl)**. The two disagree on match *length* even
without captures — `a|ab` on `"ab"` is `"a"` (Perl) vs `"ab"` (POSIX), and the
whole-match span is observable via `m.str`/`m.end`. But the match *start* is
the same under both (both are leftmost). So we let the DFA localize the work
and let the Pike VM decide the exact span/captures.

A DFA is fast because it *forgets* position. The forgotten information
(captures, alternative priority) is recovered by the Pike VM. That division of
labour dissolves the three hard problems at once.

## Three-tier dispatch

1. **Capture-free ∧ longest-safe ∧ ASCII** → pure DFA. Forward DFA finds the
   end `e`; a reverse scan finds the leftmost start `s`; return `[s, e)` as
   group 0 directly. No Pike VM. Targets `\w+`, `\d+`, `[a-z]+`, literals,
   non-capturing alternation. **Fastest path.**
2. **Captures / non-trivial (ASCII, dfa_ok_)** → DFA narrows a window `[s, e]`,
   Pike VM resolves the Perl end + captures inside that window. **Wins on
   sparse matches; weaker on dense (window ≈ whole subject).**
3. **DFA-ineligible (lookaround / `^ $ \b` / empty-loop) or non-ASCII** →
   current Pike VM over the whole subject, with the B1/B4 prefilters.
   **Fallback.**

`longest-safe` = a static, **conservative** property: alternative priority can
never change the match end. Sufficient condition to start with: no priority
branch reaching `Match`, no lazy quantifier, no trailing optional. *When in
doubt, mark unsafe* — the penalty for a false negative is "fall to the Pike
VM and run slower", never a wrong answer.

## Why a DFA cannot report the start

A DFA state is the set of NFA program counters it occupies; it carries no
"where did this begin" information (carrying it would make the state count grow
with the input and stop being a DFA). So the forward scan can say "a match ends
here" but not "it began there". The start is recovered by scanning a
**reversed** program leftward from `e`. Initially this reuse the existing
reverse Pike VM (`sub_match_reverse`, built for lookbehind) over the bounded
one-match window; a dedicated reverse *DFA* is deferred to M3 and built only if
measurement proves the reverse Pike dominates.

## Linearity (the O(n²) trap)

The B1 incident (anchored-per-candidate retry → 650 s hang) is the cautionary
tale. Rules:

- The forward DFA scan is a **single pass**, resumed from where it left off —
  never restarted at `s+1` per candidate.
- The reverse scan is **clamped not to go left of the previous match's end**,
  so the sum of reverse work is bounded by the sum of match lengths.
- After a match, the forward scan resumes from the confirmed Perl end `e'`.

Net: forward scan touches each byte once; reverse work sums to total match
length; the Pike submatch pass (tier 2) runs only inside matched windows. Whole
thing stays linear.

## Safety nets

- **DFA state cap.** Interned states are bounded; exceeding the cap abandons
  the DFA and falls back to the Pike VM (which is unaffected). Keeps a
  pathological `dfa_ok_` pattern (e.g. `.*a.{18}` needs 2^18 states) from
  exhausting memory.
- **Differential fuzzing.** Today's oracle-free invariant `test() ==
  search().matched` cross-checks the DFA against the Pike VM. Extend it to
  `find_all`: **DFA-path find_all == Pike-only find_all**, full agreement on
  match count, every span, every capture. Deterministic, no external oracle.

## Milestones

- **M0 — DFA part + safety net (no new behaviour).** Add the DFA state cap +
  Pike fallback to the existing `test()` DFA. (Generalising the DFA core to an
  arbitrary program is deferred to M1, where the shape of the forward-DFA call
  is known.) `test()` results unchanged; regression-only.
- **M1 — pure DFA for capture-free ∧ longest-safe ∧ ASCII.** Done in three
  sub-steps because the unanchored case needs a reverse scan to find the start:
  - **M1-step1 (done).** `longest-safe` static analysis (`node_longest_safe`:
    no `Alt`, no lazy `Repeat`). Anchored forward DFA `dfa_match_end` returning
    the longest match end. Wired into `match()` (anchored at 0 ⇒ no reverse
    scan needed). Cross-checked by a new fuzzer invariant: DFA `match()` ==
    Pike `search()` when the leftmost match is anchored at 0.
  - **M1-step2.** Reverse-compile the whole program; reverse scan from an end
    `e` to the leftmost start `s` (reuse `sub_match_reverse` infra, generalized
    to return the leftmost start, clamped to the previous match's end).
  - **M1-step3.** Wire `search`/`find_all`: unanchored forward DFA finds an end,
    reverse scan finds `s`, anchored `dfa_match_end` confirms the longest end.
    Add the find_all DFA-vs-Pike differential. **Measure immediately** (`\w+`,
    `\d+`, literals); do not proceed to M2 on speculation.
- **M2 — windowed Pike.** Captures via "DFA window → Pike resolve". Measure
  sparse alternation / log-parse patterns.
- **M3 — reverse DFA (conditional).** Only if M1/M2 measurement shows the
  reverse Pike dominates the window cost. Otherwise not done.

## Invariants (non-negotiable)

- Linear-time guarantee / ReDoS immunity preserved (single forward pass +
  reverse clamp + state cap).
- `longest-safe` analysis is conservative; a false result only ever costs
  speed, never correctness.
- DFA == Pike held by the differential fuzzer at all times.
- The acceleration is pattern-dependent and that limit is documented:
  dramatic on sparse / capture-free greedy, weak on capture-heavy dense.

## B1/B4 prefilters

Kept, not removed. The DFA path (tier 1/2) does its own unanchored skipping, so
B1 (literal-prefix memmem) and B4 (leading byte-set) are dormant there. They
remain live in tier 3 (DFA-ineligible / non-ASCII), e.g. lookaround patterns
and non-ASCII subjects. The dispatch gate is exclusive: `dfa_ok_ && ascii` →
DFA, else Pike + B1/B4.

## Open questions (resolve before M1)

1. Exact definition of `longest-safe` and whether it is decided on the AST or
   the compiled program.
2. Where to clamp the reverse-Pike window floor (find_all loop vs
   `sub_match_reverse`).
3. State-cap value, and whether cap-exceed falls back per-match or for the rest
   of the search.
4. If M1's `\w+` does not reach the expected order of magnitude, the next
   suspect (match-boundary constant overhead).
