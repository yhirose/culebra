#!/usr/bin/env bash
# Iterator-protocol wiring ratchet (docs/language.md §18.5).
#
# Two invariants, same ratchet style as check_rc_discipline.sh (ceilings may
# only shrink; lower one when you convert a site, never raise one without
# review):
#
# 1. Terminal drive discipline: every terminal drain goes through
#    JitIterDrive (jit_iter.h), which owns the protocol closures AND the
#    dispose-on-every-exit contract. A hand-rolled pull loop skips the
#    dispose, so raw uses of the drive primitives are pinned:
#      - `_iter_pull(`   outside JitIterDrive = 0 (hard zero)
#      - `_iter_advance_raw(` / `_iter_has_next_closure(` ceilings cover the
#        audited non-terminal users (lazy step fns, protocol open, the
#        codegen-facing iter_advance, JitIterDrive itself, the two inline
#        closure caches). A new terminal reaching for them fails here.
#
# 2. Combinator upstream wiring: a lazy combinator must hand its upstream(s)
#    to the wrapper factory (`_make_iterator(step, {upstream})` interp /
#    `_iter_wrap_fast<Fn>({cells}, n_upstreams)` JIT) or dispose stops
#    forwarding through the chain — the skip_while/flatten/... regression
#    class (every rebase that added combinators forgot at least one). The
#    no-upstream call population is exactly the audited LEAF set (source
#    iterators: range, string iters, File/Net/stdin lines, ...), so a new
#    combinator built without an upstream argument raises the count and
#    fails; a new leaf source is a conscious ceiling bump in review.
set -euo pipefail
cd "$(dirname "$0")/.."

fail=0
ratchet() { # name actual ceiling
  if (( $2 > $3 )); then
    echo "iter-wiring FAIL: $1 = $2 (ceiling $3)" >&2
    echo "  Terminals drive through JitIterDrive; lazy combinators pass" >&2
    echo "  their upstream(s) to the wrapper factory (docs §18.5)." >&2
    fail=1
  fi
}

# --- 1. terminal drive discipline -----------------------------------------

# _iter_pull outside JitIterDrive::pull (its only caller) and its own
# definition: hard zero.
pull=$(grep -rn "_iter_pull(" include/ \
       | grep -v "inline bool _iter_pull(" \
       | grep -v "has_next_cls_, next_cls_, iter_" \
       | grep -vcE ":[[:space:]]*//" || true)
ratchet "raw _iter_pull uses (outside JitIterDrive)" "$pull" 0

# _iter_advance_raw: _iter_pull's impl + the lazy combinator step fns + the
# codegen-facing culebra_runtime_iter_advance. All audited non-terminal.
adv=$(grep -rn "_iter_advance_raw(" include/ \
      | grep -v "inline bool _iter_advance_raw(" \
      | grep -vcE ":[[:space:]]*//" || true)
ratchet "raw _iter_advance_raw uses" "$adv" 25

# _iter_has_next_closure: protocol open, the lazy closure caches (flat_map and
# flatten share one), JitIterDrive's ctor. A new hand-resolved drive would add
# one here.
hnc=$(grep -rn "_iter_has_next_closure(" include/ \
      | grep -v "inline JitClosure\* _iter_has_next_closure(" \
      | grep -vcE ":[[:space:]]*//" || true)
ratchet "raw _iter_has_next_closure uses" "$hnc" 4

# --- 2. combinator upstream wiring -----------------------------------------

# Count factory calls WITHOUT an upstream argument (top-level arg count 1),
# scanning balanced parens/quotes — the calls span lines and embed lambdas,
# so this is not line-greppable.
count_no_upstream() { # pattern
  python3 - "$1" <<'PYEOF'
import re, sys, glob
pat = sys.argv[1]
def nargs(text, start):
    depth = 0; i = start; commas = 0; in_str = None
    while i < len(text):
        c = text[i]
        if in_str:
            if c == '\\': i += 2; continue
            if c == in_str: in_str = None
        elif c in '"\'': in_str = c
        elif c in '([{': depth += 1
        elif c in ')]}':
            depth -= 1
            if depth == 0: return commas + 1
        elif c == ',' and depth == 1: commas += 1
        i += 1
    return 0
total = 0
for f in sorted(glob.glob('include/*.h')):
    text = open(f).read()
    for m in re.finditer(pat, text):
        if 'inline' in text[max(0, m.start()-40):m.start()]: continue  # defs
        if nargs(text, m.end()-1) < 2: total += 1
print(total)
PYEOF
}

# interp: leaf sources (range/string/File/Net/stdin/isolate/...) = 21.
mi=$(count_no_upstream '_make_iterator\s*\(')
ratchet "no-upstream _make_iterator calls (interp leaves)" "$mi" 21

# JIT: leaf sources = 15 (9 jit_iter.h + 6 stdlib_jit.h) — grid_new joined
# math_range/iota et al. as a new source factory (no upstream iterator).
wf=$(count_no_upstream '_iter_wrap_fast<[^>]*>\s*\(')
ratchet "no-upstream _iter_wrap_fast calls (JIT leaves)" "$wf" 15

if (( fail )); then exit 1; fi
echo "iter-wiring OK (pull=$pull/0 advance_raw=$adv/25 has_next_closure=$hnc/4" \
     "make_iterator-leaves=$mi/21 wrap_fast-leaves=$wf/15)"
