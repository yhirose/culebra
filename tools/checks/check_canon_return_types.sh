#!/usr/bin/env bash
# The canonical signature table's declared return types agree with the
# reference docs.
#
# `CanonSig::return_type` (include/stdlib/canon_sigs.h) is the compiled lanes'
# record of what a stdlib call yields. For the value-type built-in methods it
# was left empty when the table stopped being generated, and the reference
# docs are where the real answer lives -- docs/quick-guide.md's signature
# index is itself generated from docs/language.md and docs/stdlib.md and gated
# by `check-quick-guide`, so it is the one spelling both sides can be held to.
#
# This gate compares the two. A row whose docs entry declares `-> T` must
# carry T; a row the docs say nothing about is left alone (see the tail of
# this script for that population, which is a docs backlog rather than a
# table one -- inventing a type here would put a wrong tag in compiled code).
set -euo pipefail
cd "$(dirname "$0")/../.."

python3 - "$@" <<'PY'
import collections
import re
import sys

QG = 'docs/quick-guide.md'
TBL = 'include/stdlib/canon_sigs_table.h'

# The index heading each value-type array is documented under.
GROUPS = {
    'String methods': 'kCanonStringSigs',
    'Array methods': 'kCanonArraySigs',
    'Object methods': 'kCanonObjectSigs',
    'Set methods': 'kCanonSetSigs',
    'Iterator methods': 'kCanonIteratorSigs',
}

SIG = re.compile(r'^(?:[A-Za-z_][A-Za-z0-9_]*\.)?([A-Za-z_][A-Za-z0-9_]*)\(')


def docs_return_types():
    """(array, method) -> declared return type, for the rows docs answer."""
    out = {}
    for line in open(QG):
        m = re.match(r'^\*\*([A-Za-z ]+)\*\* — (.*)$', line.rstrip('\n'))
        if not m or m.group(1) not in GROUPS:
            continue
        arr = GROUPS[m.group(1)]
        for sig in m.group(2).split('; '):
            sig = sig.strip()
            name = SIG.match(sig)
            if not name or ' -> ' not in sig:
                continue
            ret = sig.split(' -> ', 1)[1].strip()
            # `(mutating)` is a note on the call, not part of the type.
            ret = re.sub(r'\s*\(mutating\)\s*$', '', ret).strip()
            out.setdefault((arr, name.group(1)), ret)
    return out


def table_rows():
    """array -> [(method, declared return type)], source order."""
    rows = collections.defaultdict(list)
    cur = None
    for line in open(TBL):
        m = re.match(r'^inline constexpr CanonSig (\w+)\[\]', line)
        if m:
            cur = m.group(1)
            continue
        if not line.startswith('  {"'):
            continue
        f = line.strip().rstrip(',').lstrip('{').rstrip('}').split(', ')
        rows[cur].append((f[2].strip('"'), f[5].strip('"')))
    return rows


docs = docs_return_types()
rows = table_rows()

bad = []
agree = 0
silent = collections.defaultdict(list)
for arr in GROUPS.values():
    for name, rt in rows.get(arr, []):
        want = docs.get((arr, name))
        if want is None:
            if not rt:
                silent[arr].append(name)
            continue
        if rt == want:
            agree += 1
        else:
            bad.append((arr, name, rt, want))

for arr, name, got, want in bad:
    where = 'empty' if not got else repr(got)
    print(f"  {arr}: {name} carries {where}, docs declare {want!r}",
          file=sys.stderr)

n_silent = sum(len(v) for v in silent.values())
if bad:
    print(f"canon-return-types FAIL ({len(bad)} row(s) disagree with the "
          f"docs).", file=sys.stderr)
    print("  The docs are the source: fix the table to match, or fix the "
          "reference first and", file=sys.stderr)
    print("  re-run `just gen-quick-guide`.", file=sys.stderr)
    sys.exit(1)

print(f"canon-return-types OK ({agree} row(s) match the reference, "
      f"{n_silent} the docs do not declare)")
for arr in sorted(silent):
    print(f"  {arr}: {', '.join(sorted(silent[arr]))}")
PY
