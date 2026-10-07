#!/usr/bin/env bash
# Value-ordering door gate.
#
# `<` has one rule — an Object's `__lt__` / `__le__`, then its `cmp`; two
# Tuples by their first pair that is not `==`; then the scalars — and one
# place that answers it:
#
#   _culebra_value_order(op, t1, d1, t2, d2, line, col)   include/rt/runtime.inc.h
#
# The last step (_culebra_scalar_order) once had callers of its own: the key
# of a `sort_by`, the elements of a `min`, the fields of a derived `cmp`. They
# ordered by the scalars what the language orders by `<`, so `a < b` held
# while `xs.sorted_by(|x| a_of(x))` raised `cannot compare Object and Object`
# for the same pair, and a class that derived Comparable could not hold a
# field that did.
#
# So nothing outside the rule may name its last step: a caller that does
# skips the steps before it.
set -euo pipefail
cd "$(dirname "$0")/../.."

python3 - <<'PY'
import re, sys
sys.path.insert(0, 'tools/checks')
import door_gate

LABEL = 'value-order-door'
LEAF = re.compile(r'\b_culebra_scalar_order\s*\(')
HOME = 'include/rt/runtime.inc.h'
# The leaf's own family, in HOME: its definition and the rule's last step.
LEAF_SITES = 2

match = lambda stmt: LEAF.search(stmt) is not None
door_gate.require_scanner(
    LABEL, match,
    'lt = _culebra_scalar_order(\n    Ord::Lt, a.tag, a.data,\n    b.tag, b.data, 0, 0);')

text = door_gate.sources()
bad = [(p, n, s) for p in text if p != HOME
       for n, s in door_gate.sites(text[p], match)]
home_sites = door_gate.sites(text[HOME], match)

defined_once = lambda sig: sum(
    len(re.findall(sig, t)) for t in text.values()) == 1
body = lambda name, path: re.search(
    r'inline \w[\w:<> ]*\b' + name + r'\([^;{]*\)\s*\{(?:(?!\n\}\n)[\s\S])*',
    text[path])
asks = lambda name, path, what: (
    (m := body(name, path)) is not None and what in m.group(0))

# ...and it has to be looking at the real thing: the door exists once, its
# last step sits behind it, and the comparisons that mean "ordered before"
# ask the door.
ITER = 'include/rt/iter.inc.h'
population = [
    ('_culebra_value_order defined once',
     defined_once(r'inline bool _culebra_value_order\([^;{]*\)\s*\{')),
    ('_culebra_scalar_order defined once',
     defined_once(r'inline bool _culebra_scalar_order\([^;{]*\)\s*\{')),
    (f'the last step named at most {LEAF_SITES} times in its home (found '
     f'{len(home_sites)})', 1 <= len(home_sites) <= LEAF_SITES),
    ('a Tuple orders each pair through the door',
     asks('_culebra_tuple_order', HOME, '_culebra_value_order(op, x.tag')),
    ('the four operators ask the door',
     len(re.findall(r'culebra_runtime_value_\w+_borrow\([^;{]*\)\s*\{\s*'
                    r'return _culebra_value_order\(Ord::', text[HOME])) == 4),
    ('a keyed sort orders its keys by `<`',
     asks('_keyed_sort', ITER, '_culebra_value_less(keys->items')),
    ('min / max order by `<`',
     asks('_beats', ITER, '_culebra_value_less(')),
    ('a derived cmp orders its fields by `<`',
     asks('_jit_derived_cmp3', 'include/rt/dispatch.inc.h',
          '_culebra_value_less(')),
]

door_gate.report(
    LABEL, bad, population,
    '  The scalars are the last step of `<`, not an ordering of their own —\n'
    '  ask _culebra_value_order (or _culebra_value_less).',
    text, 'the scalar ordering is named only behind the door')
PY
