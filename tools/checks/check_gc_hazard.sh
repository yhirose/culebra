#!/usr/bin/env bash
# GC-hazard gate: no value native code holds without a count of its own may
# live across a call that can run program code, but for the holds named below
# (vm.md §5.3 states why; tools/gc_hazard/gc_hazard.cc defines the rules).
# Not in check-generated (it builds a clang tool); CI runs it as its own job.
set -euo pipefail
cd "$(dirname "$0")/../.."
root=$PWD
tool=$(bash tools/gc_hazard/build.sh)
work=build-gc-hazard

# The tool tests itself first: what it must report and nothing more.
st=$root/tools/gc_hazard/selftest
mkdir -p $work/selftest
printf '[{"directory":"%s","command":"c++ -std=c++20 -I%s/include -c %s/cases.cc","file":"%s/cases.cc"}]\n' \
  "$st" "$st" "$st" "$st" > $work/selftest/compile_commands.json
"$tool" $work/selftest "$st" "$st/cases.cc" > $work/selftest.out

# main.cc's compile command, from a configure alone (nothing is built, so
# cmake never reconfigures by itself: redo it when the build files change).
db=$work/cmake/compile_commands.json
if [[ ! -f $db || -n "$(find CMakeLists.txt cmake -newer $db -print -quit)" ]]; then
  cmake -S . -B $work/cmake -DCMAKE_BUILD_TYPE=Release -DCULEBRA_ENABLE_JIT=ON \
    -DCULEBRA_LTO=OFF -DCULEBRA_DEV_NO_RT=ON \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > $work/cmake.log
fi
"$tool" $work/cmake "$root" "$root/src/main.cc" > $work/main.out

python3 - "$work/selftest.out" "$work/main.out" <<'PY'
import collections, re, sys
sys.path.insert(0, 'tools/checks')
import door_gate

LABEL = 'gc-hazard'

def read(path):
    """{(rule, file, function, var): (declarations, [(site, tier, callee)])}
    and the files the tool analyzed."""
    decls, sites, seen = collections.defaultdict(set), collections.defaultdict(list), set()
    for line in open(path):
        f = line.rstrip('\n').split('\t')
        if f[0] == 'seen':
            seen.add(f[1])
        elif f[0] == 'H':
            _, rule, file, fn, var, decl, site, tier, callee = f
            decls[(rule, file, fn, var)].add(decl)
            sites[(rule, file, fn, var)].append((site, tier, callee))
    return {k: (len(decls[k]), sites[k]) for k in decls}, seen

# The self-test: (rule, function, variable): declarations (selftest/include/cases.h).
SELFTEST = {
    ('R1', 'yes_r1', 'f'): 1, ('R1', 'yes_r1_lambda', 'm'): 1,
    ('R1', 'yes_r1_other_capture', 'fv'): 1,
    ('R2', 'yes_r2_return', 'cb'): 1, ('R2', 'yes_r2_field', 'producer'): 1,
    ('R3', 'yes_r3', 'x'): 1, ('R3', 'yes_r3_in_lambda', 'x'): 1,
    ('R3', 'yes_r3_handler', 'x'): 1, ('R3', 'yes_r3_two_v', 'v'): 2,
}
got, _ = read(sys.argv[1])
found = {(r, fn, v): n for (r, _, fn, v), (n, _) in got.items()}
if found != SELFTEST:
    for k in sorted(SELFTEST.keys() | found.keys()):
        if SELFTEST.get(k) != found.get(k):
            print(f'{LABEL} FAIL: self-test {k}: expected {SELFTEST.get(k)} '
                  f'declarations, got {found.get(k)}', file=sys.stderr)
    print(f'{LABEL} FAIL: the tool no longer reports what it must '
          '(tools/gc_hazard/selftest/include/cases.h)', file=sys.stderr)
    sys.exit(1)

# Why a hold is sound.
RUNNING = ("the running closure's own state: the runtime made it and a step "
           "entered again raises (iter.inc.h _iter_run_step)")
GUARDED = ("a runtime-built closure's captures, read after its fn_ptr says "
           "which one it is")
NO_CODE = 'the calls between run no program code'
TABLE = 'held by a table the program cannot change under the call'
OWNED = 'held by what owns it for the call'

# (rule, file, enclosing function, variable): (declarations, why it is sound).
ALLOWED = {
    ('R1', 'include/rt/dispatch.inc.h', '_jit_multifn_dispatcher_thunk', '<inline>'):
        (1, TABLE + ": a table replaces a body only in the activation that "
            "declares it, and the running dispatcher's table holds it"),
    ('R1', 'include/rt/dispatch.inc.h', 'culebra_runtime_call_with_kwargs', 'method'):
        (1, GUARDED + ' (a bound method)'),
    ('R1', 'include/stdlib/codegen.h', 'culebra::codegen::natives::shim', '<inline>'):
        (1, TABLE + ': BoundNative::guard holds the closure'),
    ('R2', 'include/rt/dispatch.inc.h', 'culebra_runtime_multifn_self', '<inline>'):
        (1, 'a cache cleared whenever its uplink is written or erased '
            '(_jit_multifn_uplink_set / _erase)'),
    ('R3', 'include/rt/dispatch.inc.h', 'culebra_runtime_fn_introspect_get', 'cls'):
        (1, GUARDED + ' (a bound method)'),
    ('R3', 'include/rt/fixed.inc.h', '_jit_find_trait_default', 'found'):
        (1, NO_CODE + ' (trait conformance is a table lookup)'),
    ('R3', 'include/rt/iter.inc.h', '_arr_minmax', 'run_best'):
        (1, 'only a Long, Float or String, none of which a release frees'),
    ('R3', 'include/rt/iter.inc.h', '_iter_advance_raw', 'state_cell'):
        (1, GUARDED + ' (an iterator with a fast step)'),
    ('R3', 'include/rt/iter.inc.h', '_iter_flatten_step', 'inner_v'): (1, RUNNING),
    ('R3', 'include/rt/iter.inc.h', '_jit_obj_snapshot_next', 'arr'): (1, RUNNING),
    ('R3', 'include/rt/iter.inc.h', '_jit_obj_snapshot_next', 'idx_cell'): (1, RUNNING),
    ('R3', 'include/rt/iter.inc.h', '_jit_obj_snapshot_next', 'obj'): (1, RUNNING),
    ('R3', 'include/rt/iter.inc.h', 'culebra_runtime_object_remove_any', 'value'):
        (1, 'taken out of the table before the call, so the function owns it'),
    ('R3', 'include/rt/runtime.inc.h', '_culebra_tuple_order', 'x'):
        (1, OWNED + ': a Tuple, which the program cannot change'),
    ('R3', 'include/rt/runtime.inc.h', '_culebra_tuple_order', 'y'):
        (1, OWNED + ': a Tuple, which the program cannot change'),
    ('R3', 'include/rt/runtime.inc.h', 'culebra_runtime_array_get_default', 'picked'):
        (1, 'retained before the release that follows'),
    ('R3', 'include/rt/runtime.inc.h', 'culebra_runtime_tensor_from', 'row'):
        (1, NO_CODE + ' (a number coercion that throws)'),
    ('R3', 'include/stdlib/bindings.h', '_http_setup_multipart', 'arr'):
        (1, NO_CODE + ' (add_part keeps a producer, it does not call one)'),
    ('R3', 'include/stdlib/bindings.h', 'culebra::_jit_http_apply_response_meta', 'h'):
        (1, NO_CODE + ' (setting a header)'),
    ('R3', 'include/stdlib/bindings.h', 'culebra::_jit_http_try_stream_response', 'stream_v'):
        (1, NO_CODE + ' (_jit_http_apply_response_meta) before _jit_keep_closure holds it'),
    ('R3', 'include/stdlib/bindings.h', 'culebra::_ns_compress_zip', 'p'):
        (1, 'a path String, which a release does not free'),
    ('R3', 'include/stdlib/bindings.h', 'culebra::_search_adapt::make_analyzer', 'o'):
        (1, OWNED + ': an element of the result the hook returned, held for the call'),
    ('R3', 'include/vm/embed.h', 'culebra::vm::Value::items', 'obj'):
        (1, OWNED + ': the Value, under a CollectPause'),
    ('R3', 'include/vm/embed.h', 'culebra::vm::Value::set', 'obj'):
        (1, OWNED + ': the Value'),
}

# Holds not shown sound, kept apart until each is fixed (or shown sound and
# moved to ALLOWED): (rule, file, function, variable): (declarations, what).
KNOWN_HOLES = {}

named = {**ALLOWED, **KNOWN_HOLES}
got, seen = read(sys.argv[2])
bad = []
for key, (n, sites) in sorted(got.items()):
    rule, _, fn, var = key
    site_file, line = sites[0][0].rsplit(':', 1)
    if key not in named:
        shown = '; '.join(f'{s} ({t} {c})' for s, t, c in sites[:3])
        bad.append((site_file, line, f'{rule} {fn}: `{var}` is borrowed; {shown}'))
    elif named[key][0] != n:
        bad.append((site_file, line, f'{rule} {fn}: {n} declarations of '
                    f'`{var}`, {named[key][0]} named'))

# Every file that calls into program code was analyzed: a header main.cc does
# not include would be passed over in silence.
text = door_gate.sources(('include',))
CALLS = re.compile(r'\b(_jit_invoke\w*|_culebra_invoke\w*|culebra_runtime_call\w*)\s*\(')
callers = sorted(p for p, src in text.items() if CALLS.search(door_gate.blank(src)))
population = [(f'{p} analyzed (it calls into program code)', p in seen) for p in callers]
population += [(f'a hold of `{v}` in {fn} ({p}, {r})', k in got)
               for k in named for (r, p, fn, v) in [k]]
door_gate.report(
    LABEL, bad, population,
    'Hold a count across the call (JitOwnedVal, JitHeldMethod), or name the '
    'hold and why it is sound in ALLOWED (tools/checks/check_gc_hazard.sh). '
    'A renamed function or variable moves its entry.',
    text, f'{len(ALLOWED)} holds named sound, {len(KNOWN_HOLES)} known holes')
PY
