#!/usr/bin/env bash
# GC-pin door gate.
#
# A pin (Heap::pin) makes an object a root for as long as it lasts. That is
# right for what the runtime itself keeps for the Runtime's life (a meta, a
# namespace object) and for a hold that always ends (a transfer's producer, a
# worker's copy of a handler for the serve it runs). It is wrong for a program
# value a native object keeps for its own life: a root never dies with its
# holder, so a value that reaches back to the holder keeps both alive (an
# Http.server whose handler captured the server). Such a value goes in the
# holder's handle and is reported through JitHandle::each_value, an edge the
# collector, the release path and the trial-deletion walk follow
# (docs/internals/memory.md §6.2).
#
# So every pin, and every caller of _jit_keep_closure (a pinned hold that
# always ends), sits in a function named below with what kind of hold it is.
# A new one fails here until it is looked at and named on purpose.
set -euo pipefail
cd "$(dirname "$0")/../.."

python3 - <<'PY'
import re, sys
sys.path.insert(0, 'tools/checks')
import door_gate

LABEL = 'gc-pin-door'
PIN = re.compile(r'\.pin\s*\(|\b_jit_keep_closure\s*\(')
match = lambda stmt: PIN.search(stmt)
door_gate.require_scanner(LABEL, match, 'x;\n  _gc_heap()\n      .pin(obj);')

# (file, enclosing function): the kind of hold.
ALLOWED = {
    ('include/rt/runtime.inc.h', '_jit_pinned_meta'): "the Runtime's own metas",
    ('include/stdlib/bindings.h', '_jit_namespace_get_or_build'):
        "the Runtime's own namespace objects",
    ('include/stdlib/bindings.h', '_jit_builtin_fn_closure'):
        "the Runtime's own builtin closures",
    ('include/vm/vm.h', 'cell'): "a REPL session's cells, for the session",
    ('include/vm/embed.h', 'pin'):
        'a host-held Value, until the host lets it go (a JNI global ref)',
    ('include/stdlib/bindings.h', '_jit_keep_closure'):
        'a hold that always ends, dropped with its last copy',
    ('include/stdlib/bindings.h', '_jit_peg_keep_hook'):
        "a parse's hooks, dropped when the parse ends",
    ('include/stdlib/bindings.h', '_http_setup_multipart'): 'a transfer hold',
    ('include/stdlib/bindings.h', '_http_setup_body'): 'a transfer hold',
    ('include/stdlib/bindings.h', '_jit_http_try_stream_response'):
        'a transfer hold',
    ('include/stdlib/bindings.h', '_jit_net_listener_serve'):
        "a worker's copy of the handler, for its serve",
    ('include/stdlib/bindings.h', '_jit_http_server_do_serve'):
        "a worker's copies of the handlers, for its serve",
    ('include/vm/source_isolate.h', 'SourceArgv'):
        "a source's Sys.argv, for the source's run",
}

text = door_gate.sources()
spans, unbalanced = door_gate.listed_spans(text, ALLOWED)

bad, used = [], set()
for path in sorted(text):
    for n, s in door_gate.sites(text[path], match):
        key = door_gate.enclosing(spans, path, n)
        if key:
            used.add(key)
        else:
            bad.append((path, n, s))

population = [(f'{p}: braces balanced once comments and literals are blanked',
               False) for p in unbalanced]
population += [(f'a pin in {fn} ({path})', (path, fn) in used)
               for path, fn in ALLOWED]
door_gate.report(
    LABEL, bad, population,
    'A pin is a root: keep a program value a native object holds for its own '
    'life in its handle (JitHandle::each_value) instead, or name a hold that '
    'always ends in ALLOWED (tools/checks/check_gc_pin_door.sh).',
    text, f'{len(used)} holds named')
PY
