#!/usr/bin/env bash
# Value-equality door gate.
#
# `==` has one rule — `__eq__` on either side, then an `eq` both sides carry,
# then structure — and one place that answers it:
#
#   _culebra_value_equal(t1, d1, t2, d2)       include/rt/runtime.inc.h
#
# The structural walk behind it (_culebra_structure_equal and the helpers that
# walk for it) compares each pair of ELEMENTS by asking the door again. It
# once recursed into itself instead, which skipped the first two steps below
# the top level: `a == b` held while `[a] == [b]`, `[a].contains(b)` and
# `[a].index_of(b)` did not — though `a` and `b` were one Set member and one
# Object key, which always went through `eq`.
#
# So nothing outside the walk may name the walk: a caller that does compares
# by structure what the language compares by `==`.
#
# Checked over whole statements, not lines (a call wraps).
set -euo pipefail
cd "$(dirname "$0")/../.."

python3 - <<'PY'
import pathlib, re, sys

WALK = re.compile(r'\b_culebra_structure_equal\s*\(')
HOME = pathlib.Path('include/rt/runtime.inc.h')
# The walk's own family, in HOME: its definition and the door's last step.
WALK_SITES = 2

def sites(src):
    src = re.sub(r'//[^\n]*', '', src)
    line, out = 1, []
    for stmt in re.split(r'(?<=[;{}])', src):
        lead = stmt[:len(stmt) - len(stmt.lstrip())]
        here = line + lead.count('\n')  # the statement's own first line
        line += stmt.count('\n')
        if WALK.search(stmt):
            out.append((here, ' '.join(stmt.split())[:140]))
    return out

# The scanner has to be able to fail: a wrapped call, as a caller would write it.
planted = 'hit = _culebra_structure_equal(\n    v.tag, v.data,\n    nt, nd);'
if not sites(planted):
    sys.exit('value-equal-door FAIL: the scanner no longer sees a wrapped call, '
             'so a green run proves nothing')

files = [p for root in ('include', 'src') for p in pathlib.Path(root).rglob('*')
         if p.suffix in ('.h', '.cc') and 'vendor' not in p.parts]
text = {p: p.read_text() for p in files}
bad = [(p, n, s) for p in files if p != HOME for n, s in sites(text[p])]
home_sites = sites(text[HOME])

# ...and it has to be looking at the real thing: the door exists once, the
# walk sits behind it, and the comparisons that mean "equal" ask the door.
use = lambda path, name: text[pathlib.Path(path)].count(name)
population = [
    ('_culebra_value_equal defined once',
     sum(len(re.findall(r'inline bool _culebra_value_equal\([^;{]*\)\s*\{', t))
         for t in text.values()) == 1),
    ('_culebra_structure_equal defined once',
     sum(len(re.findall(r'inline bool _culebra_structure_equal\([^;{]*\)\s*\{', t))
         for t in text.values()) == 1),
    (f'the walk named at most {WALK_SITES} times in its home (found '
     f'{len(home_sites)})', 1 <= len(home_sites) <= WALK_SITES),
    ('the items walk asks the door for each pair',
     re.search(r'inline bool _culebra_items_equal\([^)]*\)\s*\{.*?'
               r'_culebra_value_equal\(x\.tag', text[HOME], re.S) is not None),
    ('contains / index_of / chunk_by ask the door',
     use('include/rt/iter.inc.h', '_culebra_value_equal(') >= 3),
    ("a derived cmp asks the door",
     use('include/rt/dispatch.inc.h', '_culebra_value_equal(') >= 1),
]
missing = [what for what, ok in population if not ok]

for p, n, s in bad:
    print(f'value-equal-door FAIL: {p}:{n}: {s}', file=sys.stderr)
for what in missing:
    print(f'value-equal-door FAIL: expected {what}', file=sys.stderr)
if bad or missing:
    print('  Structure is the last step of `==`, not a comparison of its own —\n'
          '  ask _culebra_value_equal.', file=sys.stderr)
    sys.exit(1)
print(f'value-equal-door OK ({len(files)} files, the structural walk is named '
      'only behind the door)')
PY
