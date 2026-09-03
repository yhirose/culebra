#!/usr/bin/env bash
# Iterator-protocol wiring ratchet (docs/language.md §18.5).
#
# Two invariants, same ratchet style as check_rc_discipline.sh (ceilings may
# only shrink; lower one when you convert a site, never raise one without
# review):
#
# 1. Terminal drive discipline: every terminal drain goes through
#    JitIterDrive (rt_iter.inc.h), which owns the protocol closures AND the
#    dispose-on-every-exit contract. A hand-rolled pull loop skips the
#    dispose, so raw uses of the drive primitives are pinned:
#      - `_iter_pull(`   outside JitIterDrive = 0 (hard zero)
#      - `_iter_advance_raw(` / `_iter_has_next_closure(` ceilings cover the
#        audited non-terminal users (lazy step fns, protocol open, the
#        codegen-facing iter_advance, JitIterDrive itself, the two inline
#        closure caches). A new terminal reaching for them fails here.
#
# 2. Combinator upstream wiring: a lazy combinator must hand its upstream(s)
#    to the wrapper factory (`_iter_wrap_fast<Fn>({cells}, n_upstreams)`,
#    the shared-runtime form both lanes go through) or dispose stops
#    forwarding through the chain — the skip_while/flatten/... regression
#    class (every rebase that added combinators forgot at least one). The
#    no-upstream call population is exactly the audited LEAF set (source
#    iterators: range, string iters, File/Net/stdin lines, ...), so a new
#    combinator built without an upstream argument raises the count and
#    fails; a new leaf source is a conscious ceiling bump in review.
set -euo pipefail
cd "$(dirname "$0")/../.."

fail=0
ratchet() { # name actual ceiling
  if ! [[ $2 =~ ^[0-9]+$ ]]; then
    echo "iter-wiring FAIL: $1 produced no count ('$2') — the pattern or the" >&2
    echo "  path it scans moved; this ratchet is measuring nothing." >&2
    fail=1
    return
  fi
  # A nonzero ceiling asserts a population exists. If it has emptied, the code
  # being guarded was renamed, moved or deleted, and the ratchet now passes
  # while measuring nothing. Hard-zero invariants carry ceiling 0 and are
  # unaffected.
  if (( $3 > 0 && $2 == 0 )); then
    echo "iter-wiring FAIL: $1 = 0 against ceiling $3 — the population this" >&2
    echo "  ratchet guards is empty, so it proves nothing. Repoint it at the" >&2
    echo "  code that replaced it, or retire it." >&2
    fail=1
    return
  fi
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
# Recursive: a non-recursive include/*.h glob would scan nothing the moment a
# header moved into a subdirectory, and a 0 satisfies every ceiling below.
files = sorted(glob.glob('include/**/*.h', recursive=True))
if not files:
    sys.stderr.write("iter-wiring FAIL: no headers under include/ — path moved?\n")
    sys.exit(1)
for f in files:
    text = open(f).read()
    for m in re.finditer(pat, text):
        if 'inline' in text[max(0, m.start()-40):m.start()]: continue  # defs
        if nargs(text, m.end()-1) < 2: total += 1
print(total)
PYEOF
}

# Retired 2026-09-02: the `_make_iterator` twin counted the tree-walking
# interpreter's leaf sources, and that engine's body went in d0d00303. The
# name has since matched nothing, so the ratchet passed 0 against a ceiling of
# 22 while proving nothing — the empty-population guard in ratchet() is what
# surfaced it. The surviving population is the shared-runtime `_iter_wrap_fast`
# form below, which both the executor and the JIT go through.

# leaf sources = 15 (9 rt_iter.inc.h + 6 stdlib_rt.h) — grid_new joined
# math_range/iota et al. as a new source factory (no upstream iterator).
# 15 -> 16 (2026-08-13, reviewed): the JIT twin of the FS.watch leaf above.
wf=$(count_no_upstream '_iter_wrap_fast<[^>]*>\s*\(')
ratchet "no-upstream _iter_wrap_fast calls (leaves)" "$wf" 16

if (( fail )); then exit 1; fi
echo "iter-wiring OK (pull=$pull/0 advance_raw=$adv/25 has_next_closure=$hnc/4" \
     "wrap_fast-leaves=$wf/16)"
