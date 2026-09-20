#!/usr/bin/env bash
# Regenerate tools/checks/jit_shape_set.txt — the files the landing gate's JIT
# lane sweeps. See jit_shape_cover.py for what "shape" means here and why the
# whole corpus does not belong in that lane.
set -uo pipefail
cd "$(dirname "$0")/../.."
BIN="${CULEBRA:-./build-dev/culebra}"
out=tools/checks/jit_shape_set.txt
work=$(mktemp -d "${TMPDIR:-/tmp}/culebra-shape.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT

# The op population, read off the executor's own name table: the set this lane
# has to keep covered is "every op the VM implements", not a list kept here.
sed -n '/static constexpr const char\* kNames\[\] = {/,/};/p' include/vm/vm.h \
    | tr -d '",' | tr ' ' '\n' | grep -E '^[A-Z][A-Za-z]+$' | sort -u > "$work/ops"
[[ -s "$work/ops" ]] || { echo "gen-jit-shape-set: no ops found in include/vm/vm.h" >&2; exit 1; }

grep -v '^#' tools/checks/codegen_sensitive.txt | grep . > "$work/seeds"
CULEBRA="$BIN" bash tools/checks/dump_ops.sh tests/*.cul > "$work/ops_by_file" || exit 1
# One record per file, or the cover is computed from a corpus with holes in it —
# and a file that went missing takes whatever op only it reaches with it.
want=$(ls tests/*.cul | wc -l | tr -d ' ')
got=$(wc -l < "$work/ops_by_file" | tr -d ' ')
[[ "$want" == "$got" ]] || {
    echo "gen-jit-shape-set: dumped $got of $want files; refusing to cover a corpus with holes" >&2
    exit 1
}

{
    echo "# The tests/*.cul files the landing gate runs on the JIT lane, chosen so"
    echo "# that every bytecode op the executor implements is lowered by the JIT"
    echo "# somewhere in that gate. Regenerate with \`just gen-jit-shape-set\`;"
    echo "# tools/checks/check_jit_shape_set.sh holds the coverage."
    echo "#"
    echo "# The full corpus runs this lane in \`just test\` and in CI's ci-light on"
    echo "# every push. What this set does not cover is op COMBINATIONS — an unwind"
    echo "# edge inside a loop inside a closure — which is what the codegen seeds and"
    echo "# the generated difftest corpus are for."
    echo "#"
    printf '# Generated from %s files.\n' "$(ls tests/*.cul | wc -l | tr -d ' ')"
    python3 tools/checks/jit_shape_cover.py "$work/ops" $(tr '\n' ' ' < "$work/seeds") \
        < "$work/ops_by_file" \
        | sed 's/^#unreachable$/\n# Ops no test file reaches at all. This list may only shrink: an op that\n# gains a test leaves it, and a new op arrives covered or is filed here with\n# the reason.\n#unreachable/'
} > "$out" || exit 1

printf 'gen-jit-shape-set: %s files, %s ops unreached\n' \
    "$(grep -c '^tests/' "$out")" \
    "$(sed -n '/^#unreachable/,$p' "$out" | grep -c '^[A-Z]')"
