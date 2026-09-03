#!/usr/bin/env bash
# Layering gate.
#
# `include/` is ten directories, and the directory is the layer. This checks
# that every include crosses layers in a direction the design allows, which is
# the property the directories exist to make checkable at all: while the tree
# was flat, "which layer is this file in" was a question only a reader could
# answer, so nothing could hold the answer to anything.
#
# Two lists. ALLOW is the intended shape and is the interesting one to read.
# BACKEDGE is what the tree actually does in spite of it: each is a known
# inversion with a count, and a count may only shrink. They are not
# theoretical — every one of them is load-bearing today, and the comment on
# each says what would have to move for it to go.
set -euo pipefail
cd "$(dirname "$0")/../.."

fail=0

# Layer -> the layers it may include. A layer may always include itself; the
# system headers a file also names are not layers and are ignored below.
declare -A ALLOW=(
  [base]="base"
  [frontend]="frontend base"
  [rt]="rt base"
  [conc]="conc base rt"
  [interop]="interop base rt"
  [stdlib]="stdlib base rt conc interop frontend"
  [vm]="vm base rt conc stdlib frontend"
  [aot]="aot base rt frontend"
  [jit]="jit base rt conc stdlib interop frontend vm aot"
  [cli]="cli base rt conc stdlib interop frontend vm jit aot"
  [ROOT]="frontend vm jit"
)

# Inversions the tree still has. Shrink a count when you remove one; the gate
# fails if a count grows, and fails just as loudly if a count falls without the
# number being lowered, so a fix cannot land without being recorded.
declare -A BACKEDGE=(
  # rt.h includes the parser for peg::codepoint_length in rt/iter.inc.h — it
  # needs peglib, not the parser — and fn_analysis for the FuncInfo/FnAnalysis
  # that jit.h reads back through it. Splitting either would close this.
  [rt:frontend]=2
  # Tensor is a value the runtime knows about, so rt.h includes its header even
  # though Tensor is also a stdlib namespace with its own AOT axis.
  [rt:stdlib]=1
  # One member of stdlib/bindings.h, under CULEBRA_JIT_ENABLED, declares the
  # runtime's helpers on an LLVM module. Moving it into the lowering would
  # close this and take the LLVM allow-list from three files to two.
  [stdlib:jit]=1
)

layer_of() { # path under include/
  local p=${1#include/}
  [[ $p == */* ]] && { echo "${p%%/*}"; return; }
  echo ROOT
}

declare -A seen=()
while IFS= read -r f; do
  from=$(layer_of "$f")
  allowed=" ${ALLOW[$from]:-} $from "
  while IFS= read -r to; do
    [[ -n $to ]] || continue
    # Only the ten layers are layers; anything else is a system or vendor dir.
    [[ -n ${ALLOW[$to]+x} ]] || continue
    [[ $allowed == *" $to "* ]] && continue
    key="$from:$to"
    seen[$key]=$(( ${seen[$key]:-0} + 1 ))
  done < <(grep -hoE '#include *[<"][a-z_]+/[a-zA-Z0-9_.]+\.h[>"]' "$f" 2>/dev/null \
           | sed -E 's|#include *[<"]([a-z_]+)/.*|\1|')
done < <(find include -name '*.h' | sort)

# Every inversion found must be a known one, at or under its recorded count.
for key in "${!seen[@]}"; do
  n=${seen[$key]}
  want=${BACKEDGE[$key]:-}
  if [[ -z $want ]]; then
    echo "layering FAIL: ${key%%:*} -> ${key##*:} is a new inversion ($n edge(s))." >&2
    echo "  Either the include belongs in a lower layer, or this is a design" >&2
    echo "  change that gets a BACKEDGE entry and a comment saying why." >&2
    fail=1
  elif (( n > want )); then
    echo "layering FAIL: ${key%%:*} -> ${key##*:} = $n (recorded $want) — an" >&2
    echo "  inversion that was supposed to shrink grew instead." >&2
    fail=1
  fi
done

# And every recorded inversion must still exist at its count: a fix that lands
# without lowering the number leaves the gate guarding nothing.
for key in "${!BACKEDGE[@]}"; do
  n=${seen[$key]:-0}
  want=${BACKEDGE[$key]}
  if (( n < want )); then
    echo "layering FAIL: ${key%%:*} -> ${key##*:} = $n but $want is recorded." >&2
    echo "  It shrank — lower the number in this gate so the next one is held" >&2
    echo "  to the new floor." >&2
    fail=1
  fi
done

(( fail )) && exit 1
echo "layering OK (${#BACKEDGE[@]} recorded inversion(s), none new)"
