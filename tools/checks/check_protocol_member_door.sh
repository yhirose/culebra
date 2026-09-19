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
#
# Checked over whole statements, not lines: these calls wrap, and the first
# sweep for them — a grep for the literal — missed half.
set -euo pipefail
cd "$(dirname "$0")/../.."

python3 - <<'PY'
import pathlib, re, sys

PROBE = re.compile(r'\b(?:culebra_runtime_object_has\w*|emit_object_has\w*|'
                   r'_find_property|find_slot|get_or_create_global_str)\s*\(')
NAME = re.compile(r'"(?:drop|iter|has_next|next)"')

def offenders(src):
    src = re.sub(r'//[^\n]*', '', src)
    line, out = 1, []
    for stmt in re.split(r'(?<=[;{}])', src):
        lead = stmt[:len(stmt) - len(stmt.lstrip())]
        here = line + lead.count('\n')  # the statement's own first line
        line += stmt.count('\n')
        if PROBE.search(stmt) and NAME.search(stmt):
            out.append((here, ' '.join(stmt.split())[:140]))
    return out

# The scanner has to be able to fail: a wrapped probe, the shape the tree had.
planted = 'ok = culebra_runtime_object_has(\n    reinterpret_cast<JitObject*>(r.data),\n    "next");'
if not offenders(planted):
    sys.exit('protocol-member-door FAIL: the scanner no longer sees a wrapped '
             'probe, so a green run proves nothing')

files = [p for root in ('include', 'src') for p in pathlib.Path(root).rglob('*')
         if p.suffix in ('.h', '.cc') and 'vendor' not in p.parts]
bad = [(p, n, s) for p in files for n, s in offenders(p.read_text())]

# ...and it has to be looking at the real thing: the door exists once, and
# both lanes ask through the named questions.
text = {p: p.read_text() for p in files}
door = sum(t.count('inline JitClosure* _protocol_member(') for t in text.values())
use = lambda path, name: text[pathlib.Path(path)].count(name)
population = [
    ('_protocol_member defined once', door == 1),
    ('the executor asks is_iterator_shaped',
     use('include/vm/vm.h', 'culebra_runtime_is_iterator_shaped(') >= 1),
    ('the executor asks has_iter_method',
     use('include/vm/vm.h', 'culebra_runtime_has_iter_method(') >= 1),
    ('the lowering asks is_iterator_shaped',
     use('include/jit/lowering.h', 'emit_is_iterator_shaped(') >= 1),
    ('the JIT for-in head asks has_iter_method',
     use('include/jit/jit.h', 'emit_has_iter_method(') >= 2),  # def + use
]
missing = [what for what, ok in population if not ok]

for p, n, s in bad:
    print(f'protocol-member-door FAIL: {p}:{n}: {s}', file=sys.stderr)
for what in missing:
    print(f'protocol-member-door FAIL: expected {what}', file=sys.stderr)
if bad or missing:
    print('  A well-known key\'s presence is not a protocol member — ask\n'
          '  _protocol_member (or is_iterator_shaped / has_iter_method).',
          file=sys.stderr)
    sys.exit(1)
print(f'protocol-member-door OK ({len(files)} files, no key probe on a '
      'well-known name)')
PY
