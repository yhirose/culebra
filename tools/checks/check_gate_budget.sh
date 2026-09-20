#!/usr/bin/env bash
# The gate's own cost, ratcheted.
#
# Every lane's price is a population times a per-item cost, and the population
# is the half that grows quietly: tests/*.cul went from 203 files to 279 in six
# weeks, which is 174 of the 610 CPU seconds one --jit sweep of the corpus now
# costs, and nothing said so until the gate felt slow. This holds each swept
# population to a number written down beside it, so growth arrives as a
# decision with a reason rather than as a gate that is mysteriously slower.
#
# Growth is allowed — it is how a language gets tested. What is not allowed is
# growth nobody chose: update tools/checks/gate_budget.txt in the same commit,
# and say in the message what the new tests buy.
set -uo pipefail
cd "$(dirname "$0")/../.."

budget=tools/checks/gate_budget.txt
[[ -r "$budget" ]] || { echo "gate-budget: missing $budget" >&2; exit 1; }

# Each population is one line: <key> <count>. Keep the probes cheap — this runs
# inside check-generated, which the landing gate waits on.
measure() {
    case "$1" in
        tests.cul)        ls tests/*.cul | wc -l ;;
        tests.isolate)    ls tests/isolate/*.cul | wc -l ;;
        vm_cases)         ls tools/bench/vm_cases/*.cul | wc -l ;;
        ctest.entries)    grep -c '^ *add_test(' CMakeLists.txt ;;
        docs.blocks)      grep -rhc '^```culebra' docs/*.md docs/*/*.md | paste -sd+ - | bc ;;
        languages.samples) ls examples/languages/*/samples/* 2>/dev/null | wc -l ;;
        examples.suites)  find examples -name 'test_*.cul' | wc -l ;;
        *)                echo "gate-budget: unknown population '$1'" >&2; return 1 ;;
    esac
}

fail=0
n=0
while read -r key want; do
    [[ -n "$key" && "$key" != \#* ]] || continue
    got=$(measure "$key" | tr -d ' ') || exit 1
    n=$((n + 1))
    if [[ "$got" != "$want" ]]; then
        printf 'gate-budget: %s is %s, budget says %s\n' "$key" "$got" "$want" >&2
        fail=1
    fi
done < "$budget"

# A budget file that measures nothing passes every check — the failure mode
# three ratchets in this tree have already had.
(( n > 0 )) || { echo "gate-budget: $budget declared no population" >&2; exit 1; }

if (( fail )); then
    echo "gate-budget: the gate sweeps more (or less) than it is budgeted for." >&2
    echo "  Update $budget in this commit and say what the change buys." >&2
    exit 1
fi
echo "gate-budget OK ($n populations)"
