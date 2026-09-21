#!/usr/bin/env bash
# Value-equality door gate.
#
# `==` has one rule — `__eq__` on either side, then an `eq` both sides state,
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
# Key equality is the second comparison that means equal, and it has a door of
# its own: JitValueEq. It is deliberately stricter — `eq` and not `__eq__`, no
# cross-type — because a key's equality has to agree with JitValueHash. What
# this gate holds for it is the same shape: one definition, and the places
# keyed on it (a Set's members, a derived `eq`'s fields) asking it rather than
# writing the comparison again.
#
# A derived `eq` states no equality: it says the instances match by their
# fields, as an enum variant's do, so `==` reaches structure and keys go field
# by field. Whether an `eq` is derived is decided once (_special_closure), for
# the class table and the by-name lookup both — two places deciding it is how
# an instance with a special-named slot of its own once answered `==` by the
# derived `eq` while its classmates answered by structure.
set -euo pipefail
cd "$(dirname "$0")/../.."

python3 - <<'PY'
import re, sys
sys.path.insert(0, 'tools/checks')
import door_gate

LABEL = 'value-equal-door'
WALK = re.compile(r'\b_culebra_structure_equal\s*\(')
HOME = 'include/rt/runtime.inc.h'
# The walk's own family, in HOME: its definition and the door's last step.
WALK_SITES = 2

match = lambda stmt: WALK.search(stmt) is not None
door_gate.require_scanner(
    LABEL, match,
    'hit = _culebra_structure_equal(\n    v.tag, v.data,\n    nt, nd);')

text = door_gate.sources()
bad = [(p, n, s) for p in text if p != HOME
       for n, s in door_gate.sites(text[p], match)]
home_sites = door_gate.sites(text[HOME], match)

# Whether an `eq` is derived is read in one place: every read of the flag is
# a site, and a second one is reported where it stands.
FILL = 'include/rt/fixed.inc.h'
derived_read = lambda stmt: re.search(r'&\s*JIT_CLOSURE_DERIVED\b', stmt)
door_gate.require_scanner(
    LABEL, derived_read,
    'if (e &&\n    (fn->flags &\n     JIT_CLOSURE_DERIVED)) return nullptr;')
derived_reads = [(p, n, s) for p in text
                 for n, s in door_gate.sites(text[p], derived_read)]
if len(derived_reads) > 1:
    bad += derived_reads
asks = lambda path, name: any(
    True for _ in door_gate.sites(text[path], lambda st: name in st))

# ...and it has to be looking at the real thing: the door exists once, the
# walk sits behind it, and the comparisons that mean "equal" ask the door.
defined_once = lambda sig: sum(
    len(re.findall(sig, t)) for t in text.values()) == 1
population = [
    ('_culebra_value_equal defined once',
     defined_once(r'inline bool _culebra_value_equal\([^;{]*\)\s*\{')),
    ('_culebra_structure_equal defined once',
     defined_once(r'inline bool _culebra_structure_equal\([^;{]*\)\s*\{')),
    (f'the walk named at most {WALK_SITES} times in its home (found '
     f'{len(home_sites)})', 1 <= len(home_sites) <= WALK_SITES),
    ('the items walk asks the door for each pair',
     re.search(r'inline bool _culebra_items_equal\([^)]*\)\s*\{.*?'
               r'_culebra_value_equal\(x\.tag', text[HOME], re.S) is not None),
    ('contains / index_of / chunk_by ask the door',
     text['include/rt/iter.inc.h'].count('_culebra_value_equal(') >= 3),
    ('a derived cmp asks the door',
     text['include/rt/dispatch.inc.h'].count('_culebra_value_equal(') >= 1),
    # Key equality: the other door, held to the same shape.
    ('JitValueEq defined once',
     defined_once(r'struct JitValueEq\s*\{')),
    ('a Set compares its members through its index',
     re.search(r'case TAG_SET: \{(?:(?!case TAG_)[\s\S])*?index->contains\(',
               text[HOME]) is not None),
    ('a derived eq compares its fields as keys',
     text['include/rt/dispatch.inc.h'].count('JitValueEq{}(') >= 1),
    # A derived `eq` states nothing, decided in one place.
    (f'JIT_CLOSURE_DERIVED read once, in _derived_method (found '
     f'{len(derived_reads)})',
     len(derived_reads) == 1 and derived_reads[0][0] == HOME and
     re.search(r'inline bool _derived_method\([^)]*\)\s*\{[^}]*'
               r'JIT_CLOSURE_DERIVED', text[HOME]) is not None),
    ('the class table and the by-name lookup both ask _special_closure',
     asks(FILL, '_special_closure(') and
     re.search(r'struct SpecialLookup\s*\{(?:(?!\n\};)[\s\S])*?'
               r'_special_closure\(', text[HOME]) is not None),
    ('JitValueEq matches by fields through _jit_eq_by_fields',
     re.search(r'struct JitValueEq\s*\{(?:(?!\n\};)[\s\S])*?'
               r'_jit_eq_by_fields\(', text['include/rt/value.inc.h'])
     is not None),
]

door_gate.report(
    LABEL, bad, population,
    '  Structure is the last step of `==`, not a comparison of its own —\n'
    '  ask _culebra_value_equal.',
    text, 'the structural walk is named only behind the door')
PY
