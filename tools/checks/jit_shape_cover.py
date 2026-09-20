"""Pick the smallest set of test files that lowers every bytecode op.

The landing gate cannot afford the whole corpus on the JIT lane: 279 files cost
610 CPU seconds there against 17 under the executor, because what the lane pays
for is LLVM, not execution. What it has to keep is coverage of the thing the
lane exists to check — that every op the executor implements is also lowered,
somewhere, by the JIT.

So: read each file's ops off `--vm-dump`, then take a cost-aware greedy cover of
the op population, seeded with the files a human already picked as
codegen-sensitive. The cost of a file is its instruction count, which is what
its LLVM time tracks; the greedy step takes the file with the most new ops per
instruction, and ties break on the name so the answer is the same everywhere.

Input on stdin: one record per file, `<path> <op> <op> ...` (dump_ops.sh).
Arguments: the op population file, then any mandatory seed paths.
Output: the chosen paths, then the ops no file in the corpus reaches at all.
"""

import sys


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: jit_shape_cover.py <ops-file> [seed ...]", file=sys.stderr)
        return 2
    population = set()
    with open(sys.argv[1]) as handle:
        for line in handle:
            if line.strip():
                population.add(line.strip())
    seeds = sys.argv[2:]

    ops = {}
    size = {}
    for record in sys.stdin:
        fields = record.split()
        if not fields:
            continue
        path, names = fields[0], fields[1:]
        # A file's cost tracks the instructions it emits, and its coverage is
        # the distinct ops among them.
        size[path] = max(len(names), 1)
        ops[path] = set(names) & population

    reachable = set().union(*ops.values()) if ops else set()
    chosen = [s for s in seeds if s in ops]
    covered = set().union(*(ops[c] for c in chosen)) if chosen else set()

    while covered < reachable:
        best, best_score = None, None
        for path in sorted(ops):
            if path in chosen:
                continue
            gain = len(ops[path] - covered)
            if not gain:
                continue
            score = gain / size[path]
            if best_score is None or score > best_score:
                best, best_score = path, score
        if best is None:
            break
        chosen.append(best)
        covered |= ops[best]

    for path in sorted(chosen):
        print(path)
    print("#unreachable")
    for op in sorted(population - reachable):
        print(op)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
