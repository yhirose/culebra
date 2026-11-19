#!/usr/bin/env bash
# String-read door gate.
#
# A StringView is a String (docs/language.md §18.1): whatever asks for a
# `String` takes both representations, and a native reads the bytes of either
# through one of these:
#
#   _culebra_str_view(tag, data)          include/rt/string.inc.h    bytes and length
#   _ns_adapt::require_sv / require_str   include/stdlib/bindings.h  checked; a view, or an
#                                                                    owned NUL-terminated copy
#   _jit_handle_req_string                include/stdlib/bindings.h  a handle method's String
#
# Natives once tested `tag != TAG_STRING` after the binder's check, refusing
# the view on purpose, and read a payload as `const char*`, which for a view
# is a JitStringView* — and even the view's own pointer is no C string, since
# its bytes run on into its source. A policy that new natives take StringLike
# did not hold by convention alone.
#
# So in the native layers (include/stdlib/, include/interop/) no statement
# tests a tag against TAG_STRING without TAG_STRINGVIEW beside it, or reads a
# value's payload as `const char*`, outside the functions listed here:
#
#   culebra_runtime_print     bindings.h  a String's fast arm; the view's follows
#   culebra_runtime_fs_join   bindings.h  a String's fast arm; the view's follows
#   take_path                 bindings.h  a String's fast arm; the view's follows
#   _jit_peg_each_callback    bindings.h  an Object's keys, which are Strings
set -euo pipefail
cd "$(dirname "$0")/../.."

python3 - <<'PY'
import re, sys
sys.path.insert(0, 'tools/checks')
import door_gate

LABEL = 'string-read-door'
ROOTS = ('include/stdlib', 'include/interop')
ALLOWED = [
    ('include/stdlib/bindings.h', 'culebra_runtime_print'),
    ('include/stdlib/bindings.h', 'culebra_runtime_fs_join'),
    ('include/stdlib/bindings.h', 'take_path'),
    ('include/stdlib/bindings.h', '_jit_peg_each_callback'),
]
STRING_ONLY = re.compile(
    r'(?:==|!=|\bcase)\s*(?:::)?TAG_STRING\b|\bTAG_STRING\s*(?:==|!=)')
# A payload is a value's `.data` (through `->` to an element too) or a
# native's bare `data` parameter; a byte buffer's `e->data` and `buf.data()`
# are not one.
CSTR_READ = re.compile(
    r'reinterpret_cast\s*<\s*const\s+char\s*\*\s*>\s*'
    r'\(\s*(?:[\w\[\]>-]*\.)?data\s*\)')


def offends(stmt):
    return ((STRING_ONLY.search(stmt) is not None and
             'TAG_STRINGVIEW' not in stmt) or
            CSTR_READ.search(stmt) is not None)


door_gate.require_scanner(
    LABEL, offends,
    'if (args[0].tag !=\n    TAG_STRING)\n  fail("data", "String", 0);')
door_gate.require_scanner(
    LABEL, offends,
    'std::string path(_str_sv(\n    reinterpret_cast<const char*>(a[0].data)));')
door_gate.require_scanner(
    LABEL, offends, 'switch (v.tag) {\n  case TAG_STRING: return f(v);')
door_gate.require_scanner(
    LABEL, offends, 'if (TAG_STRING == tag) return f(tag, data);')
door_gate.require_scanner(
    LABEL, offends, 'auto s = reinterpret_cast<const char*>(data);')

text = door_gate.sources(ROOTS)
spans, unbalanced = door_gate.listed_spans(text, ALLOWED)
bad = [(p, n, s) for p in sorted(text)
       for n, s in door_gate.sites(text[p], offends)
       if not door_gate.enclosing(spans, p, n)]
bad += [(p, 0, 'braces do not balance once blanked') for p in unbalanced]

BINDINGS = 'include/stdlib/bindings.h'
WRAP = 'include/interop/wrap.h'
code = {p: door_gate.blank(text[p]) for p in (BINDINGS, WRAP)}
body = lambda path, name: next(
    (code[path][s[3]:s[4]] for s in door_gate.function_spans(code[path], name)),
    '')
# Bodies are blanked, so a name inside a string literal does not count.
population = [(f'{name} defined in {path}', bool(spans[(path, name)]))
              for path, name in ALLOWED]
population += [
    ('require_sv takes a StringView',
     'TAG_STRINGVIEW' in body(BINDINGS, 'require_sv')),
    ('_jit_handle_req_string takes a StringView',
     'TAG_STRINGVIEW' in body(BINDINGS, '_jit_handle_req_string')),
    ("wrap's argument check asks the type matcher",
     '_culebra_value_matches_type' in
     body(WRAP, 'jit_arg_matches')),
]

door_gate.report(
    LABEL, bad, population,
    '  A String argument may be a StringView: test both tags (or ask\n'
    '  _culebra_value_matches_type), and read the bytes with\n'
    '  _culebra_str_view / _ns_adapt::require_sv, or require_str for a C API.',
    text, 'a String is tested and read as either representation')
PY
