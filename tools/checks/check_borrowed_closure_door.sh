#!/usr/bin/env bash
# Borrowed-closure door gate.
#
# The runtime calls Functions it was not handed a count of: a method it looks
# up by name or by protocol (an operator's `__add__`, a `for`'s `iter` and
# `next`, a callable instance's `__call__`), a callback in a user's array. The
# rule (docs/internals/vm.md §5.3) is CPython's for a borrowed reference:
# whoever calls or keeps such a Function takes a count of its own, unless what
# holds it can neither change nor die under the call. A method that replaces
# its own slot while it runs, or a trait default whose trait is declared
# again, is otherwise freed under its own call, and the next allocation runs
# in its place.
#
# The count is held by one type, JitHeldMethod (include/rt/runtime.inc.h),
# which every lookup returns. A temporary of it cannot turn into a
# JitClosure*, so `auto* f = _protocol_member(...)` does not compile.
#
# What the compiler cannot see is a JitClosure* made from a raw value:
# `reinterpret_cast<JitClosure*>(v.data)`. Each one is either a sound borrow
# (a call's own argument, the running closure's capture cell, a frame
# register, a class meta) or a hole. So the casts are counted per file, and a
# new one fails here until it is looked at and counted in on purpose. What
# the standard library keeps past a call (a request's or a part's producer,
# an analyzer's hooks, a response's stream) goes through _jit_keep_closure,
# which holds and pins it, instead of a cast.
set -euo pipefail
cd "$(dirname "$0")/../.."

python3 - <<'PY'
import re, sys
sys.path.insert(0, 'tools/checks')
import door_gate

LABEL = 'borrowed-closure-door'
CAST = re.compile(r'reinterpret_cast<\s*JitClosure\s*\*\s*>|\(\s*JitClosure\s*\*\s*\)\s*\w')
match = lambda stmt: CAST.search(stmt)
door_gate.require_scanner(
    LABEL, match,
    'auto* cls =\n    reinterpret_cast<\n      JitClosure*>(v.data);')

# Statements that make a JitClosure* from a raw value, per file. Exact: a new
# cast raises its file's count, a removed one lowers it here.
COUNTED = {
    'include/cli/test_engine.h': 1,
    'include/conc/sendable.h': 8,
    'include/rt/dispatch.inc.h': 12,
    'include/rt/fixed.inc.h': 2,
    'include/rt/iter.inc.h': 52,
    'include/rt/mem.inc.h': 3,
    'include/rt/runtime.inc.h': 4,
    'include/stdlib/bindings.h': 17,
    'include/stdlib/codegen.h': 1,
    'include/vm/embed.h': 1,
    'include/vm/vm.h': 13,
}

text = door_gate.sources()
bad = []
changed = []
for path in sorted(set(text) | set(COUNTED)):
    found = door_gate.sites(text.get(path, ''), match)
    want = COUNTED.get(path, 0)
    if len(found) > want:
        bad += [(path, n, s) for n, s in found]
        changed.append(f'{path}: {len(found)} casts, {want} counted')
    elif len(found) < want:
        changed.append(f'{path}: {len(found)} casts, {want} counted '
                       '(lower the count)')
        bad.append((path, 0, 'fewer casts than counted'))

# ...and it has to be looking at the real thing: one type holds the count,
# and the lookups and both lanes' calls go through it.
use = lambda path, name: text[path].count(name)
population = [
    ('JitHeldMethod defined once',
     sum(t.count('class JitHeldMethod {') for t in text.values()) == 1),
    ('_protocol_member answers a held method',
     use('include/rt/runtime.inc.h',
         'inline JitHeldMethod _protocol_member(') == 1),
    ('the special lookup answers a held method',
     use('include/rt/runtime.inc.h', 'JitHeldMethod operator()(Special s)') == 1),
    ('the lowering calls a probed `__call__` through call_held',
     use('include/jit/lowering.h', 'rt::call_held') >= 1),
    ('the JIT opens a for through the executor\'s iter() call',
     use('include/jit/jit.h', 'rt::for_call_iter') >= 1),
]

door_gate.report(
    LABEL, bad, population,
    '  ' + '\n  '.join(changed) + '\n'
    '  A Function the runtime calls or keeps without the caller\'s count is\n'
    '  held through JitHeldMethod (hold / adopt / lend). A new raw cast must be\n'
    '  a sound borrow — a call\'s own argument, the running closure\'s capture\n'
    '  cell, a frame register, a class meta — and is counted in above.',
    COUNTED, f'{sum(COUNTED.values())} counted casts')
PY
