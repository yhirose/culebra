#!/usr/bin/env bash
# The landing gate's JIT lane sweeps a subset (tools/checks/jit_shape_set.txt).
# A subset is only worth trusting while it still covers what it was chosen to
# cover, so: every bytecode op the executor implements must be lowered by some
# file in the set, or be filed as unreached with a reason.
#
# This is the check that keeps the subset from rotting the way a ratchet rots —
# three in this tree once ran green while measuring nothing. A new op in
# include/vm/vm.h fails here until a test reaches it or it is filed; a test file
# the set names and that no longer exists fails here too.
set -uo pipefail
cd "$(dirname "$0")/../.."
BIN="${1:-${CULEBRA:-./build-dev/culebra}}"
set_file=tools/checks/jit_shape_set.txt
work=$(mktemp -d "${TMPDIR:-/tmp}/culebra-shapechk.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT

sed -n '/static constexpr const char\* kNames\[\] = {/,/};/p' include/vm/vm.h \
    | tr -d '",' | tr ' ' '\n' | grep -E '^[A-Z][A-Za-z]+$' | sort -u > "$work/ops"
[[ -s "$work/ops" ]] || { echo "jit-shape-set: no ops found in include/vm/vm.h" >&2; exit 1; }

grep '^tests/' "$set_file" > "$work/files"
sed -n '/^#unreachable/,$p' "$set_file" | grep '^[A-Z]' | sort -u > "$work/filed" || true
[[ -s "$work/files" ]] || { echo "jit-shape-set: $set_file names no file" >&2; exit 1; }

rc=0
while read -r f; do
    [[ -f "$f" ]] || { echo "jit-shape-set: $f is in the set and does not exist" >&2; rc=1; }
done < "$work/files"

# Every codegen-sensitive file is a mandatory seed: the shapes those lanes were
# given must not be missing from the gate's own sweep.
grep -v '^#' tools/checks/codegen_sensitive.txt | grep . | sort > "$work/seeds"
sort "$work/files" > "$work/files.sorted"
if ! comm -23 "$work/seeds" "$work/files.sorted" | grep -q .; then :; else
    echo "jit-shape-set: codegen-sensitive files missing from the set:" >&2
    comm -23 "$work/seeds" "$work/files.sorted" | sed 's/^/  /' >&2
    rc=1
fi
(( rc == 0 )) || { echo "  regenerate with: just gen-jit-shape-set" >&2; exit 1; }

CULEBRA="$BIN" bash tools/checks/dump_ops.sh $(tr '\n' ' ' < "$work/files") \
    > "$work/records" || exit 1
# A file that failed to dump would quietly subtract its ops from the cover and
# read as "the set no longer covers these" — say which it was instead.
want=$(wc -l < "$work/files" | tr -d ' ')
got=$(wc -l < "$work/records" | tr -d ' ')
[[ "$want" == "$got" ]] || {
    echo "jit-shape-set: dumped $got of $want files in the set" >&2
    comm -23 <(sort "$work/files") <(awk '{print $1}' "$work/records" | sort) | sed 's/^/  missing: /' >&2
    exit 1
}
tr ' ' '\n' < "$work/records" | grep -E '^[A-Z][A-Za-z]+$' | sort -u > "$work/covered"

comm -23 "$work/ops" "$work/covered" | sort > "$work/uncovered"
if ! diff -q "$work/uncovered" "$work/filed" > /dev/null; then
    comm -23 "$work/uncovered" "$work/filed" > "$work/new" || true
    comm -13 "$work/uncovered" "$work/filed" > "$work/gone" || true
    if [[ -s "$work/new" ]]; then
        echo "jit-shape-set: ops no file in the set lowers, and not filed:" >&2
        sed 's/^/  /' "$work/new" >&2
    fi
    if [[ -s "$work/gone" ]]; then
        echo "jit-shape-set: ops filed as unreached that the set now covers:" >&2
        sed 's/^/  /' "$work/gone" >&2
    fi
    echo "  regenerate with: just gen-jit-shape-set" >&2
    exit 1
fi

printf 'jit-shape-set OK (%s files cover %s of %s ops; %s filed as unreached)\n' \
    "$(wc -l < "$work/files" | tr -d ' ')" \
    "$(comm -12 "$work/ops" "$work/covered" | wc -l | tr -d ' ')" \
    "$(wc -l < "$work/ops" | tr -d ' ')" \
    "$(wc -l < "$work/filed" | tr -d ' ')"
