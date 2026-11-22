#!/usr/bin/env bash
# Key-name door gate.
#
# An Object key is every byte of its String, a NUL included, and C++ names
# one with JitKeyName (include/rt/value.inc.h), which keeps the length and
# cannot be made from a bare `const char*`. The runtime's C ABI takes codegen's
# names as `const char*` instead, measured by strlen; a name from data that
# reached it was cut at its first NUL (two JSON keys that differed after one
# became one, `o["drop\0x"] = f` installed a `drop` hook).
#
# So C++ hands a C-ABI object function a name only as a string literal, and
# turns a `const char*` into a JitKeyName (JitKeyName::source) only where it
# is codegen's: inside the ABI's own functions (culebra_runtime_*, forwarding
# the name they were given) and include/vm/vm.h (the compiler and the
# executor, whose names are the compiler's constants). A name from anywhere
# else goes through a JitKeyName door (_jit_object_set_named, ...).
set -euo pipefail
cd "$(dirname "$0")/../.."

python3 - <<'PY'
import re, sys
sys.path.insert(0, 'tools/checks')
import door_gate

LABEL = 'key-name-door'
# function -> index of its name argument
NAME_ARG = {
    'object_set': 1, 'object_set_fast': 1, 'object_set_ic': 1,
    'object_get': 1, 'object_get_ic': 1, 'object_has': 1,
    'object_has_own_field': 2, 'object_has_or_trait_default': 1,
    'object_remove': 1, 'object_bind_static': 1, 'object_slot_init': 1,
    'prop_get': 2, 'prop_value': 2, 'prop_wr': 2,
}
CALL = re.compile(r'\bculebra_runtime_(' + '|'.join(NAME_ARG) + r')\s*\(')
LITERAL = re.compile(r'"(?:[^"\\]|\\.)*"')
SOURCE = re.compile(r'\bJitKeyName::source(?:_unmeasured)?\s*\(')
DECLARED = re.compile(r'CULEBRA_RT_(?:KEEP|INLINE)')
COMPILER = 'include/vm/vm.h'
KEY_NAME = 'include/rt/value.inc.h'  # where JitKeyName and special_key live
# Listed with why the name is not data.
ALLOWED = [
    # A Dir's method, read as a member read so a trait default answers; its
    # callers spell the name.
    ('include/stdlib/bindings.h', '_jit_dir_method'),
    # The namespace tables' rows, each name a literal in the table.
    ('include/stdlib/bindings.h', '_jit_build_namespace_object'),
]


def args_of(stmt, at):
    """The top-level arguments of the call whose '(' ends at `at`."""
    depth, cur, out = 0, [], []
    for c in stmt[at:]:
        if c in '([{':
            depth += 1
        elif c in ')]}':
            if depth == 0:
                out.append(''.join(cur).strip())
                return out
            depth -= 1
        elif c == ',' and depth == 0:
            out.append(''.join(cur).strip())
            cur = []
            continue
        cur.append(c)
    return out


def offends(stmt):
    if SOURCE.search(stmt):
        return True
    if DECLARED.search(stmt):
        return False  # a declaration
    for m in CALL.finditer(stmt):
        args = args_of(stmt, m.end())
        k = NAME_ARG[m.group(1)]
        if len(args) > k and not LITERAL.fullmatch(args[k]):
            return True
    return False


door_gate.require_scanner(
    LABEL, offends,
    'culebra_runtime_object_set(o, key.c_str(),\n    false, v.tag, v.data, 0, 0);')
door_gate.require_scanner(
    LABEL, offends,
    'set_field(o, name);\nculebra_runtime_object_get(\n    o, name, &t, &d);')
door_gate.require_scanner(
    LABEL, offends, 'o->find_slot(JitKeyName::source(s.c_str()));')

text = door_gate.sources()
spans, unbalanced = door_gate.listed_spans(text, ALLOWED)
bad = []
for p in sorted(text):
    if p.startswith('include/jit/') or p in (COMPILER, KEY_NAME):
        continue  # the lowering names the ABI as strings, never calls it
    found = door_gate.sites(text[p], offends)
    if not found:
        continue
    abi = door_gate.function_spans_re(door_gate.blank(text[p]),
                                      r'culebra_runtime_\w+')
    for n, s in found:
        if door_gate.enclosing(spans, p, n):
            continue
        if any(a <= n <= b for a, b, *_ in abi):
            continue
        bad.append((p, n, s))
bad += [(p, 0, 'braces do not balance once blanked') for p in unbalanced]

VALUE = door_gate.blank(text['include/rt/value.inc.h'])
FIXED = door_gate.blank(text['include/rt/fixed.inc.h'])
population = [(f'{name} defined in {path}', bool(spans[(path, name)]))
              for path, name in ALLOWED]
population += [
    ('JitKeyName refuses a bare pointer',
     re.search(r'class JitKeyName\b[\s\S]*?JitKeyName\(P\)\s*=\s*delete',
               VALUE) is not None),
    ('a key value names its slot through a JitKeyName',
     re.search(r'std::optional<JitKeyName>\s+_jit_key_name\(', FIXED)
     is not None),
]

door_gate.report(
    LABEL, bad, population,
    '  Hand a name that is not a string literal to a JitKeyName door\n'
    '  (_jit_object_set_named, _set_keyed, _jit_object_get, _jit_object_has,\n'
    "  _jit_object_remove) rather than to the C ABI, whose `const char*`\n"
    '  is cut at the first NUL.',
    text, 'C++ names a C-ABI object key only by a literal, a pointer only in the ABI')
PY
