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
  - **M1-step2 (done).** Reverse-compile the whole program (`reverse_ast` +
    `Compiler::compile`, built only for tier-1 patterns). `dfa_match_start`:
    a byte-DFA over the reverse program scanning leftward from an end `e` to the
    leftmost start `s` (clamped to a floor). NOTE: reverse is a *byte DFA*, not
    the reverse Pike VM the meeting first proposed — a Pike reverse needs the
    `Segmented` grapheme split, which would defeat tier-1's "no segmentation"
    win. `dfa_search` orchestrates the three stages (unanchored forward end →
    reverse start → anchored longest end) and `search()` is wired to it (one
    match per call, so no DFA caching needed yet). The shared `LazyDfa` driver
    (interned states + lazy byte transitions + state cap) backs `dfa_test`,
    `dfa_forward`, and `dfa_match_start`. Differential: the fuzzer's existing
    "Pike find_all[0] == search()" invariant now cross-checks the DFA search.
  - **M1-step3 (done).** Wire `find_all`: the three DFAs (`fu`/`fa`/`rv`) are
    built once per call and reused across every match (`LazyDfa` gained a
    `reseed` member so the unanchored and anchored DFAs are distinct, reusable
    objects). DFA-vs-Pike differential added to the fuzzer by wrapping the
    pattern in a capture group (forces Pike; group 0 == the unwrapped whole
    match). **Measured (1MB find_all): 2–5× over the Pike baseline** — `\w+`
    8.9→44, `[a-z]+` 9.9→47, `\d+` 55→200, literal 60→190 MB/s. (`\w+` was
    ~18 before caching the interned start state; that state is invariant per
    DFA and was being recomputed ~3× per match — a /simplify pass fixed it for
    another ~2.3×.) `\w+` now beats std::regex (~11) decisively. Dense patterns
    no longer cap at ~2×, but this is still not RE2's order of magnitude.
- **M2 — windowed Pike (ATTEMPTED, ABANDONED).** The plan was: for captures /
  longest-unsafe DFA-able patterns, reuse the M1 prefilter (unanchored forward
  → first end `e0` → reverse → leftmost start `s`), then run an anchored Pike
  at `s` for the end + captures. **The fuzzer (DFA-vs-forced-Pike differential)
  immediately found it wrong on alternation.** Root cause: the unanchored
  forward DFA finds the *earliest-ending* match, but the *leftmost-starting*
  match can start earlier and end later — e.g. `\d.|[a]*\w{3}\w` on `"c1BA"`:
  the DFA finds `1B` (`\d.`, ends at 3) first, so reverse localizes start 1 and
  misses the true leftmost match `c1BA` (`[a]*\w{3}\w`, starts at 0, ends at 4).
  Tier 1 was immune only because longest-safe ⇒ no alternation, which happens
  to also rule out earlier-start/later-end; tier 2's target set is exactly the
  patterns where it breaks. Correctly localizing the leftmost-*starting* match
  needs a priority/leftmost-aware DFA (the subset DFA carries no thread
  priority) — that is M3-scale, not a quick win. **Reverted.** Captures /
  longest-unsafe patterns stay on the Pike VM with the B1/B4 prefilters (which
  already skip dead regions by literal prefix / first-byte set).
- **M3 — priority/leftmost-aware DFA (open).** The real prerequisite for
  accelerating tier-2 patterns: a forward DFA whose states carry NFA thread
  priority so it reports the leftmost-first match end directly (RE2-style).
  Large; only worth it if tier-2 patterns prove to be a measured bottleneck in
  a real workload. Not scheduled.

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
