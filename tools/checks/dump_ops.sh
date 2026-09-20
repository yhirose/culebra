#!/usr/bin/env bash
# Print `<file> <op> <op> ...` for each argument, read off the compiled
# bytecode. One record per file, in argument order; a file the compiler refuses
# is reported and skipped, since a shape set is about what compiles.
#
# Each job writes its own file and the parent concatenates in argument order.
# A record here is thousands of ops long, and a write that size to a shared pipe
# is not atomic: writing straight to stdout from parallel jobs spliced records
# into each other, which silently dropped a file from the cover (and would have
# dropped whatever op only that file reaches).
set -uo pipefail
BIN="${CULEBRA:-./build-dev/culebra}"
jobs="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}"
work=$(mktemp -d "${TMPDIR:-/tmp}/culebra-dumpops.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT

dump_one() {
    local f="$1" work="$2" out
    out=$("$BIN" --vm-dump "$f" 2>/dev/null) || {
        echo "dump_ops: $f did not compile" >&2
        return 0
    }
    printf '%s %s\n' "$f" \
        "$(printf '%s\n' "$out" | awk '/^ *[0-9]+: [A-Z]/ {print $2}' | tr '\n' ' ')" \
        > "$work/$(printf '%s' "$f" | tr '/' '_').ops"
}
export -f dump_one
export BIN

printf '%s\n' "$@" | xargs -P "$jobs" -I '{}' bash -c 'dump_one "$1" "$2"' _ '{}' "$work"

for f in "$@"; do
    rec="$work/$(printf '%s' "$f" | tr '/' '_').ops"
    [[ -s "$rec" ]] && cat "$rec"
done
