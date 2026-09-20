#!/usr/bin/env bash
# Protocol-member door gate.
#
# Four property names are protocols the runtime drives behind the scenes:
# `drop`, `iter`, `has_next`, `next`. A Function bound to one must take no
# arguments; anything else bound there is data that happens to use the name —
# `JSON.parse('{"next": "…"}')` is an ordinary Object. So the PRESENCE of one
# of those keys says nothing. The only question a consumer may ask is "is a
# Function there?", and it is answered in one place:
#
#   _protocol_member(obj, name)            include/rt/runtime.inc.h
#   culebra_runtime_is_iterator_shaped     — a `next` member, by name
#   culebra_runtime_has_iter_method        — an `iter` member, by name
#
# A probe that asks for the key instead treats data as a protocol member. The
# mild form is a wrong diagnostic (`for k, v in parsed` failing on an `iter`
# key); the severe one has happened — a `has_next` that was not a Function
# reached a raw closure cast in the JIT, a jump through a Long payload.
set -euo pipefail
cd "$(dirname "$0")/../.."

python3 - <<'PY'
import re, sys
sys.path.insert(0, 'tools/checks')
import door_gate

LABEL = 'protocol-member-door'
PROBE = re.compile(r'\b(?:culebra_runtime_object_has\w*|emit_object_has\w*|'
                   r'_find_property|find_slot|get_or_create_global_str)\s*\(')
NAME = re.compile(r'"(?:drop|iter|has_next|next)"')

match = lambda stmt: PROBE.search(stmt) and NAME.search(stmt)
door_gate.require_scanner(
    LABEL, match,
    'ok = culebra_runtime_object_has(\n'
    '    reinterpret_cast<JitObject*>(r.data),\n    "next");')

text = door_gate.sources()
bad = [(p, n, s) for p in text for n, s in door_gate.sites(text[p], match)]

# ...and it has to be looking at the real thing: the door exists once, and
# both lanes ask through the named questions.
use = lambda path, name: text[path].count(name)
population = [
    ('_protocol_member defined once',
     sum(t.count('inline JitClosure* _protocol_member(')
         for t in text.values()) == 1),
    ('the executor asks is_iterator_shaped',
     use('include/vm/vm.h', 'culebra_runtime_is_iterator_shaped(') >= 1),
    ('the executor asks has_iter_method',
     use('include/vm/vm.h', 'culebra_runtime_has_iter_method(') >= 1),
    ('the lowering asks is_iterator_shaped',
     use('include/jit/lowering.h', 'emit_is_iterator_shaped(') >= 1),
    ('the JIT for-in head asks has_iter_method',
     use('include/jit/jit.h', 'emit_has_iter_method(') >= 2),  # def + use
]

door_gate.report(
    LABEL, bad, population,
    '  A well-known key\'s presence is not a protocol member — ask\n'
    '  _protocol_member (or is_iterator_shaped / has_iter_method).',
    text, 'no key probe on a well-known name')
PY
