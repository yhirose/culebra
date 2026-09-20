#!/usr/bin/env bash
# A `# doctest: skip` has to be justified, and there are only two ways to
# justify one: the block cannot run, or it says why it is not run.
#
# The hole this closes: a skipped block is never executed, so a documented form
# that stopped working stays documented. It has happened twice — the
# `test("name", fn)` form that never worked (7e1f0c46) and the formatting
# examples of 2c9421d6 — both found by reading, both skipped and so never run.
#
# So: run the skips that give no reason. A block that fails is a block that
# cannot run, which is what an unreasoned skip claims. A block that runs cleanly
# is a skip with nothing behind it — un-skip it, or say in the doc why it must
# not run here (`# doctest: skip — opens a window`), which is the sentence a
# reader wants anyway.
#
# The executor lane only: the question is whether the block runs at all, and the
# JIT lane would pay one LLVM module per block to answer the same thing.
set -uo pipefail
cd "$(dirname "$0")/../.."
repo=$PWD
BIN="${1:-${CULEBRA:-./build-dev/culebra}}"
case "$BIN" in /*) ;; *) BIN="$repo/${BIN#./}" ;; esac
roots=("$repo/docs")
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}"
work=$(mktemp -d "${TMPDIR:-/tmp}/culebra-skips.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT

# Run from a scratch directory, never the checkout: these blocks are the ones
# that write files (`FS.write('out.txt', ...)`, a PNG, an archive), and a gate
# that leaves four untracked files behind is a gate that stops `just land`.
scratch="$work/cwd"
mkdir -p "$scratch"
cd "$scratch"

lane=(test --doc --only-skipped --vm)

# A hanging doc example once timed every master CI run out at GitHub's six-hour
# limit (8c38fc5b), and this lane runs the examples most likely to hang, so the
# limit is its own rather than a timeout(1) that a macOS runner may not carry.
# Returns 124 on expiry, like timeout does.
run_capped() {
    local secs="$1"; shift
    "$@" & local pid=$!
    local waited=0
    while (( waited < secs )); do
        kill -0 "$pid" 2> /dev/null || { wait "$pid"; return $?; }
        sleep 1
        waited=$((waited + 1))
    done
    kill -9 "$pid" 2> /dev/null
    wait "$pid" 2> /dev/null
    return 124
}

# One process for the whole lane: the executor runs a block in milliseconds, so
# the steady state is a second or two. A block that runs and keeps running (a
# server example) would hang it, which the classify pass below turns into a name.
run_capped 60 "$BIN" "${lane[@]}" --reporter json "${roots[@]}" \
    > "$work/out" 2> "$work/err"
rc=$?

report_ran() {
    echo "doctest-skips: these blocks are skipped and run cleanly anyway:" >&2
    sed "s|^$repo/||; s/^/  /" "$1" >&2
    echo "  Either drop the skip — a block that runs is coverage — or say why it" >&2
    echo "  must not run here: \`# doctest: skip — opens a window\`." >&2
}

# The diagnostic pass: the same lane, one process per block, so a hang is a name
# and not a gate that stopped saying anything. Only reached when the lane above
# did not finish.
if (( rc == 124 )); then
    echo "doctest-skips: the lane did not finish; running it block by block" >&2
    "$BIN" "${lane[@]}" --list "${roots[@]}" 2> /dev/null > "$work/names"
    probe() {
        local name="$1" d="$2"
        local slug
        slug=$(printf '%s' "$name" | tr -c '[:alnum:]' '_')
        run_capped 10 "$BIN2" test --doc --only-skipped --vm --filter "$name" \
            > /dev/null 2>&1
        case $? in
            0)   printf '%s\n' "$name" > "$d/$slug.ran" ;;
            124) printf '%s\n' "$name" > "$d/$slug.hang" ;;
        esac
    }
    export -f probe run_capped
    export BIN2="$BIN"
    xargs -P "$JOBS" -I '{}' bash -c 'probe "$1" "$2"' _ '{}' "$work" \
        < "$work/names"
    shopt -s nullglob
    hangs=("$work"/*.hang)
    rans=("$work"/*.ran)
    if (( ${#hangs[@]} )); then
        echo "doctest-skips: these blocks run without finishing:" >&2
        cat "${hangs[@]}" | sed "s|^$repo/||; s/^/  /" >&2
        echo "  Give each one a reason (\`# doctest: skip — serves until interrupted\`)." >&2
    fi
    if (( ${#rans[@]} )); then
        cat "${rans[@]}" > "$work/ran"
        report_ran "$work/ran"
    fi
    exit 1
fi

grep -o '"event":"doc_pass","name":"[^"]*"' "$work/out" \
    | sed 's/.*"name":"//; s/"$//' > "$work/ran"
total=$(grep -c '"event":"doc_\(pass\|fail\)"' "$work/out")

# A lane that selected nothing passes every check — the failure mode three
# ratchets in this tree have already had.
if (( total == 0 )); then
    echo "doctest-skips: no unreasoned skip was run; the lane selected nothing" >&2
    cat "$work/err" >&2
    exit 1
fi

if [[ -s "$work/ran" ]]; then
    report_ran "$work/ran"
    exit 1
fi

printf 'doctest-skips OK (%s unreasoned skips, none of them runnable)\n' "$total"
