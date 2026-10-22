#!/usr/bin/env bash
# Native-entry door gate.
#
# The closure ABI is callee-consumes: `self` and every argument arrive at +1,
# however many arguments the caller passed, and the caller does not clean them
# on unwind. A native (C++-bodied) method meets that in one place:
# _jit_native_entry, the code its closure runs, holds both and lets them go on
# every exit, and the body only borrows them. The binders take nothing but a
# JitNativeFn, which only puts a body behind that entry, so the compiler keeps
# every handle method there. Bodies that each released their own operands, in
# their own way, leaked an argument on every call and `self` on a throw.
#
# What the compiler cannot see is a closure built straight from a code pointer.
# So a closure is built only where its code is known, each for its reason:
#
#   _jit_native_closure                  fixed.inc.h     a body behind the entry
#   culebra_runtime_enum_define_variant  dispatch.inc.h  moves its arguments
#                                                        into the instance
#   _jit_make_bound_method               dispatch.inc.h  hands them on to the method
#   _jit_multifn_new_dispatcher          dispatch.inc.h  hands them on to the body
#   _culebra_expect_callback             iter.inc.h      hands them on to `__call__`
#   _jit_make_ns_method_closure          bindings.h      the namespace dispatch
#                                                        consumes them
#   _jit_namespace_get_or_build          bindings.h      a module the compiler built
#   dispatch                             vm.h            a bytecode function's
#   jit_deserialize                      sendable.h      a closure rebuilt over the
#                                                        code it was sent with
#   culebra_runtime_eff_recapture        dispatch.inc.h  a copy over its own code
#
# A native body that releases what it borrowed is not looked for here: C++ has
# too many ways to spell a release for a text scan to close. The tests that
# call the method find it, as a value freed while still in use.
set -euo pipefail
cd "$(dirname "$0")/../.."

python3 - <<'PY'
import re, sys
sys.path.insert(0, 'tools/checks')
import door_gate

LABEL = 'native-entry-door'
CONSTRUCTORS = [
    ('include/rt/fixed.inc.h', '_jit_native_closure'),
    ('include/rt/dispatch.inc.h', 'culebra_runtime_enum_define_variant'),
    ('include/rt/dispatch.inc.h', '_jit_make_bound_method'),
    ('include/rt/dispatch.inc.h', '_jit_multifn_new_dispatcher'),
    ('include/rt/iter.inc.h', '_culebra_expect_callback'),
    ('include/stdlib/bindings.h', '_jit_make_ns_method_closure'),
    ('include/stdlib/bindings.h', '_jit_namespace_get_or_build'),
    ('include/vm/vm.h', 'dispatch'),
    ('include/conc/sendable.h', 'jit_deserialize'),
    ('include/rt/dispatch.inc.h', 'culebra_runtime_eff_recapture'),
]
CONSTRUCT = re.compile(r'\bculebra_runtime_closure_new\s*\(')
# Its own declarations and definition, which build nothing.
DECLARES = re.compile(r'JitClosure\s*\*\s*$')


def offending(code, spans):
    """(path, line, text) for each construction outside a listed function."""
    bad = []
    for path, c in code.items():
        for m in CONSTRUCT.finditer(c):
            if DECLARES.search(c[max(0, m.start() - 40):m.start()]):
                continue
            n = c.count('\n', 0, m.start()) + 1
            inside = any(key[0] == path and lo <= n <= hi
                         for key, ss in spans.items() for lo, hi, *_ in ss)
            if not inside:
                bad.append((path, n, ' '.join(c[m.start():m.start() + 90].split())))
    return bad


# The scanner has to be able to fail, or a green run proves nothing.
planted = door_gate.blank(
    'void f() {\n  auto* c = culebra_runtime_closure_new(\n'
    '      fn, 0, 0, 0, nullptr);\n}\n')
if len(offending({'planted.h': planted}, {})) != 1:
    sys.exit(f'{LABEL} FAIL: the scanner no longer sees a wrapped construction, '
             'so a green run proves nothing')

text = door_gate.sources()
code = {p: door_gate.blank(s) for p, s in text.items()
        if CONSTRUCT.search(s)}
# A span is found by matching braces, so a file the blanking misread (a
# literal it took for code) would stretch one silently. Refuse it instead.
unbalanced = [p for p, c in code.items() if c.count('{') != c.count('}')]
spans = {key: door_gate.function_spans(code.get(key[0], ''), key[1])
         for key in CONSTRUCTORS}
bad = offending(code, spans)

population = [(f'{p}: braces balanced once comments and literals are blanked',
               False) for p in unbalanced]
for (path, name), ss in spans.items():
    population.append((f'{name} defined in {path}', len(ss) >= 1))
    population.append((f'{name} to build a closure', any(
        CONSTRUCT.search(code[path], b0, b1) for _, _, _, b0, b1 in ss)))

door_gate.report(
    LABEL, bad, population,
    '  A closure is built only where its code is known. A native body goes\n'
    '  behind the entry through _jit_native<F> and a binder or\n'
    '  _jit_native_closure; code that passes its operands on to a consumer\n'
    '  is added to this gate with the reason.',
    text, 'closures are built only in the listed functions')
PY
