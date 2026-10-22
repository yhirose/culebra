#!/usr/bin/env bash
# Portable-closure door gate.
#
# A native (C++-bodied) closure crosses an isolate only when it is marked
# JIT_CLOSURE_PORTABLE, and the serializer then ships it as it ships any
# closure: the code pointer, and a copy of each capture. That is right only
# for a closure whose body reads nothing but `self`, its arguments and its
# captures, and whose captures are cells of its own holding ordinary values.
# The iterator wrappers and the ns-method closures are the counterexamples:
# their captures hold raw pointers into the heap that built them, and a child
# that rebuilt one read the parent's heap and hung or crashed.
#
# So the mark is set only by the constructors that build such closures, and
# read only by the serializer:
#
#   culebra_runtime_make_derived_method   include/rt/dispatch.inc.h
#   culebra_runtime_enum_define_variant   include/rt/dispatch.inc.h
#   _jit_make_bound_method                include/rt/dispatch.inc.h
#   _jit_bind_portable_method             include/rt/fixed.inc.h
#   jit_serialize (the reader)            include/conc/sendable.h
#
# A new kind of closure that should cross is added here, with the reason it
# meets the rule above.
set -euo pipefail
cd "$(dirname "$0")/../.."

python3 - <<'PY'
import re, sys
sys.path.insert(0, 'tools/checks')
import door_gate

LABEL = 'portable-closure-door'
MARK = re.compile(r'\bJIT_CLOSURE_PORTABLE\b')
DEFINITION = re.compile(r'\bconstexpr uint64_t JIT_CLOSURE_PORTABLE =')
DEF_HOME = 'include/rt/value.inc.h'
DOORS = [
    ('include/rt/dispatch.inc.h', 'culebra_runtime_make_derived_method'),
    ('include/rt/dispatch.inc.h', 'culebra_runtime_enum_define_variant'),
    ('include/rt/dispatch.inc.h', '_jit_make_bound_method'),
    ('include/rt/fixed.inc.h', '_jit_bind_portable_method'),
    ('include/conc/sendable.h', 'jit_serialize'),
]

match = lambda stmt: MARK.search(stmt) is not None
door_gate.require_scanner(
    LABEL, match,
    'auto* c = culebra_runtime_closure_new(\n    fn, 0, 0,\n'
    '    JIT_CLOSURE_NATIVE |\n        JIT_CLOSURE_PORTABLE, nullptr);')

text = door_gate.sources()


def definitions(path, name):
    """The 1-based line spans of each top-level definition of `name`."""
    src = text[path]
    head = re.compile(r'^(?:[\w:<>*&]+[ \t]+)*' + re.escape(name) +
                      r'\([^;{]*\)\s*\{', re.M)
    spans = []
    for m in head.finditer(src):
        end = src.find('\n}\n', m.end())
        if end < 0:
            continue
        first = src.count('\n', 0, m.start()) + 1
        spans.append((first, src.count('\n', 0, end) + 2))
    return spans


spans = {door: definitions(*door) for door in DOORS}


def door_at(path, n):
    """The door whose body holds line `n` of `path`, or None."""
    return next((door for door, ss in spans.items() if door[0] == path
                 and any(lo <= n <= hi for lo, hi in ss)), None)


bad, used = [], {door: 0 for door in DOORS}
for path in text:
    for n, s in door_gate.sites(text[path], match):
        if path == DEF_HOME and DEFINITION.search(s):
            continue
        door = door_at(path, n)
        if door is None:
            bad.append((path, n, s))
        else:
            used[door] += 1

defined_once = sum(len(DEFINITION.findall(t)) for t in text.values()) == 1
population = [('JIT_CLOSURE_PORTABLE defined once', defined_once)]
for (path, name), ss in spans.items():
    population.append((f'{name} defined once in {path}', len(ss) == 1))
    population.append((f'{name} to name JIT_CLOSURE_PORTABLE',
                       used[(path, name)] >= 1))

door_gate.report(
    LABEL, bad, population,
    '  A native closure crosses an isolate only when its body reads nothing\n'
    '  but self, its args and its captures, and its captures are cells of its\n'
    '  own holding ordinary values. Mark it in one of the constructors this\n'
    '  gate lists, or add yours here with the reason it meets that rule.',
    text, 'the portable mark is set only by the listed constructors')
PY
