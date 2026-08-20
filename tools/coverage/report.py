#!/usr/bin/env python3
"""Report what only the generated corpus reaches.

Reads the two gcov profiles tools/coverage/run.sh produces and prints the code
that is live in `all` and dead in `durable`, restricted to the surface the two
compiled engines share.

The surface is keyed on the demangled name, not the file: vm.h holds the
bytecode compiler, the executor and the LLVM lowering in one translation unit,
and only the first of the three is shared fate.  A bug in `vm::Compiler`, in a
runtime helper or in the shared emitter makes the executor and the lowering
give the *same* wrong answer, so no differential lane can see it once the
tree-walker is gone (docs/internals/vm.md §7).

Two granularities, because one of them alone would mislead.  Functions are what
the ratchet is keyed on — they survive edits to the file that line numbers do
not.  Lines are the work list, and they are also the check on the function
count: a function both sets reach can still hold a branch only the corpus
takes, so a function-level zero means nothing until the line-level zero agrees.

Usage: report.py <profile-dir>
       (profile-dir holds json/durable/ and json/all/)
"""
import gzip
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

# Prefix -> which side of the fate line it falls on.  A prefix that matches
# nothing is an error, not an empty row: every headline here is a zero, and a
# renamed namespace would otherwise turn "the compiler has nothing corpus-only"
# into a statement about a table that no longer matches anything.
SHARED = (
    ("culebra::vm::Compiler::", "compiler"),
    ("culebra::JIT::", "emitter"),
    ("culebra::_jit_", "runtime"),
    ("culebra_runtime_", "runtime"),
)
# Being an allowlist, this table has a second blind spot the `matched` check
# cannot see: a shared component in a namespace nobody added is absent from
# every number below rather than wrong in any of them. The unclassified tally
# at the end of the report is the counterweight — it names what is neither
# claimed here nor excluded there, so the boundary stays auditable.
#
# Consumers of the bytecode rather than shared fate: a bug in one of these is
# still caught by diffing the two lanes against each other. Named so the report
# can separate "excluded on purpose" from "nobody has classified this yet".
EXCLUDED = (
    "culebra::vm::Exec::",
    "culebra::vm::Lowering::",
    "culebra::Interpreter::",
)


def own_source_test(root):
    """A predicate for "this project wrote this file".

    gcov records absolute paths, and the system headers live under `/include/`
    too, so the test has to be anchored at the checkout rather than spelled as
    a substring. Vendored submodules are this project's dependencies, not its
    code, and nothing in them is shared fate.
    """
    root = str(root)
    def own(path):
        return path.startswith(root) and "/vendor/" not in path
    return own


def bucket(name):
    """The first two `::` segments of a demangled name, for the tally."""
    head = re.split(r"[<(]", name, 1)[0]
    return "::".join(head.split("::")[:2]) or "?"


def classify(name):
    for prefix, kind in SHARED:
        if name.startswith(prefix):
            return kind
    return None


def load(dirpath, own):
    """Read one profile.

    Returns (functions, lines, matched):
      functions  demangled name -> [count, span, file, kind]
      lines      (file, lineno) -> [count, kind, owning function]
      matched    which SHARED prefixes claimed at least one function
      unclass    name bucket -> count, for this repo's own code that no
                 prefix claimed and no exclusion named
    """
    functions, lines, matched = {}, {}, set()
    unclassified = defaultdict(int)
    for gz in sorted(Path(dirpath).glob("*.gcov.json.gz")):
        with gzip.open(gz, "rt") as fh:
            data = json.load(fh)
        for entry in data["files"]:
            src = entry["file"]
            # gcov labels each line with the mangled name of the function that
            # owns it, so the mapping is a lookup rather than a range scan —
            # which also puts a nested lambda's lines on the lambda.
            owners = {}
            for fn in entry.get("functions", ()):
                name = fn.get("demangled_name") or fn["name"]
                kind = classify(name)
                if kind is None:
                    if own(src) and not name.startswith(EXCLUDED):
                        unclassified[bucket(name)] += 1
                    continue
                matched.add(next(p for p, _ in SHARED if name.startswith(p)))
                span = max(1, fn["end_line"] - fn["start_line"] + 1)
                row = functions.setdefault(name, [0, span, src, kind])
                row[0] += fn["execution_count"]
                row[1] = max(row[1], span)
                owners[fn["name"]] = (kind, name)
            if not owners:
                continue
            for ln in entry.get("lines", ()):
                owner = owners.get(ln.get("function_name"))
                if owner is None:
                    continue
                key = (src, ln["line_number"])
                row = lines.get(key)
                if row is None:
                    lines[key] = [ln["count"], owner[0], owner[1]]
                else:
                    row[0] += ln["count"]
    return functions, lines, matched, unclassified


def source_line(path, lineno, cache):
    """The text of one line, for the work list. Best effort."""
    if path not in cache:
        try:
            cache[path] = Path(path).read_text(errors="replace").splitlines()
        except OSError:
            cache[path] = []
    text = cache[path]
    return text[lineno - 1].strip() if 0 < lineno <= len(text) else "?"


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    root = Path(sys.argv[1]).resolve()
    # <checkout>/build-cov/profile -> <checkout>
    own = own_source_test(root.parents[1])
    d_fns, d_lines, _, _ = load(root / "json" / "durable", own)
    a_fns, a_lines, matched, unclass = load(root / "json" / "all", own)
    if not a_fns:
        print("report: no profile in %s" % (root / "json" / "all"),
              file=sys.stderr)
        return 1
    missing = [p for p, _ in SHARED if p not in matched]
    if missing:
        print("report: these SHARED prefixes match nothing — did the namespace "
              "move?\n  " + "\n  ".join(missing), file=sys.stderr)
        return 1

    print("# What only the generated corpus reaches, over the surface the two")
    print("# compiled engines share: the bytecode compiler, the shared runtime")
    print("# helpers, the shared emitter. docs/internals/vm.md §7 (Phase 4).")
    print("# Out of scope on purpose: " + ", ".join(EXCLUDED))

    tally = defaultdict(lambda: [0, 0, 0])
    only = []
    for name, (count, span, src, kind) in a_fns.items():
        row = tally[kind]
        row[0] += 1
        if count:
            row[1] += 1
            if d_fns.get(name, (0,))[0] == 0:
                row[2] += 1
                only.append((span, count, kind, name, Path(src).name))

    line_tally = defaultdict(lambda: [0, 0, 0])
    by_owner = defaultdict(list)
    for (src, no), (count, kind, owner) in a_lines.items():
        row = line_tally[kind]
        row[0] += 1
        if count:
            row[1] += 1
            if d_lines.get((src, no), (0,))[0] == 0:
                row[2] += 1
                by_owner[(owner, src)].append(no)

    print("#")
    print("# %-9s %19s %19s" % ("", "functions", "lines"))
    print("# %-9s %6s %6s %6s %6s %6s %6s"
          % ("kind", "total", "live", "only", "total", "live", "only"))
    for kind in sorted(tally):
        f, l = tally[kind], line_tally[kind]
        print("# %-9s %6d %6d %6d %6d %6d %6d"
              % (kind, f[0], f[1], f[2], l[0], l[1], l[2]))

    print("#")
    print("# Ratchet keys — functions the corpus reaches and the durable suites")
    print("# do not. Each must end up either covered by a durable test or")
    print("# classified as deliberately corpus-only.")
    only.sort(key=lambda r: (-r[0], -r[1]))
    for span, count, kind, name, src in only:
        print("%-7d %-10d %-9s %s  [%s]" % (span, count, kind, name, src))
    print("# %d function(s)" % len(only))

    print("#")
    print("# Work list — the lines themselves, including those inside functions")
    print("# both sets reach.")
    cache = {}
    for (owner, src), nums in sorted(by_owner.items(), key=lambda x: -len(x[1])):
        print("%s  [%s]  %d line(s)" % (owner, Path(src).name, len(nums)))
        for no in sorted(nums):
            print("  %6d  %s" % (no, source_line(src, no, cache)[:96]))
    print("# %d line(s) in %d function(s)"
          % (sum(len(v) for v in by_owner.values()), len(by_owner)))

    print("#")
    print("# Unclassified — this repo's own functions that no SHARED prefix")
    print("# claims and no exclusion names. Every one of them is invisible to")
    print("# the table above, so this is where the surface definition is")
    print("# audited, not a work list.")
    print("# %d function(s) in %d bucket(s); largest:"
          % (sum(unclass.values()), len(unclass)))
    for name, n in sorted(unclass.items(), key=lambda x: -x[1])[:15]:
        print("# %6d  %s" % (n, name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
