#!/usr/bin/env python3
"""The compiler resolves every name as resolve.h does.

resolve.h is the one statement of culebra's scope rules; vm::Compiler keeps a
scope stack of its own, and the two disagreeing is how a closure ends up
reading a variable other than the one the program names (or, where the
analysis that decides cells disagrees, memory corruption). With
CULEBRA_SCOPE_CHECK set the compiler reports every name it looks up against
resolve.h (include/frontend/scope_check.h); this compiles, without running:

  every tracked .cul under tests/ examples/ tools/ docs/ stdlib/
  a generated grid: each scope-opening construct x each declaring form,
    with and without an outer variable of the same name, read before and
    after, and captured by closures inside and out
  tests/culebra_test_self under `culebra test` (a session) and
    tests/repl_test.sh (REPL lines), once each

and fails on a finding tools/checks/scope_agreement_allow.txt does not list,
on an entry there that no longer fires, and on a compile that succeeded
without reporting (the check not reaching the compiler must not pass).

Usage: scope_agreement.py <culebra-binary>
JOBS (default: the CPU count) bounds the processes run at once.
"""
import concurrent.futures
import glob
import os
import subprocess
import sys
import tempfile
import textwrap

ALLOW = 'tools/checks/scope_agreement_allow.txt'
CORPUS = ('tests/', 'examples/', 'tools/', 'docs/', 'stdlib/')
JOBS = int(os.environ.get('JOBS') or os.cpu_count() or 4)

# ---- the grid ----------------------------------------------------------------

# A declaring statement of `v`, then a closure over it and a read of it.
FORMS = {
    'let': 'let v = 1',
    'mut': 'mut v = 1',
    'bare': 'v = 1',
    'destructure': 'let [v] = [1]',
    'fn': 'fn v() { 1 }',
}
# Where the statements go: BODY is replaced, indented.
CONSTRUCTS = {
    'sequence': 'BODY',
    'block': '{\nBODY\n}',
    'for': 'for _i in [1] {\nBODY\n}',
    'nobreak': 'for _i in [] {\n0\n} nobreak {\nBODY\n}',
    'while': 'mut _w = 0\nwhile _w < 1 {\n_w += 1\nBODY\n}',
    'match_arm': 'match 1 {\n_ => {\nBODY\n}\n}',
    'try': 'try {\nBODY\n} catch _e {\n0\n}',
    'catch': 'try {\nthrow 1\n} catch _e {\nBODY\n}',
    'if_arm': 'if true {\nBODY\n}',
    'else_arm': 'if false {\n0\n} else {\nBODY\n}',
    'cond_arm': 'cond {\ntrue => {\nBODY\n},\n_ => 0,\n}',
    'init_clause': 'if let _c = 1; _c > 0 {\nBODY\n}',
    'defer': 'defer {\nBODY\n}',
    'closure': 'let _g = fn () {\nBODY\n}',
}
# Declarations inside an expression, read by a closure beside them.
EXPRESSIONS = {
    'default': 'fn _d(x = (let v = 1) + (|| v)()) { x }',
    'later_param_default': 'fn _d(x = v, v = 5) { x }',
    'later_param_closure': 'fn _d(x = || v, v = 5) { x() }',
    'initializer': 'class _K {\nf = (let v = 1) + 0\ng = (|| v)()\n}',
    'own_initializer': 'class _K {\nf = (let v = 1) + (|| v)()\n}',
    'static_value': 'class _S {\nstatic s = (let v = 1) + (|| v)()\n}',
    # A static value runs where the class is declared, before the `let`.
    'static_before_declaration': 'class _S {\nstatic s = v\n}\nlet v = 1',
    'conditional_expr': 'true && (let v = 1)\nlet _in = || v',
    'decorator': 'let _dec = fn (f) { f }\n@_dec\nfn _h() { v }',
    # The condition reads `v` before the arm declares one of its own.
    'arm_after_condition': '{\nif v == 0 {\nlet v = 1\n}\n}',
}


def indent(text, by):
    return textwrap.indent(text, ' ' * by)


def grid(dir):
    """Write the grid's programs into `dir`; return their paths."""
    cases = {}
    for (cname, construct) in CONSTRUCTS.items():
        for (fname, form) in FORMS.items():
            body = f'{form}\nlet _inner = || v\nv'
            cases[f'{cname}__{fname}'] = construct.replace('BODY', indent(body, 2))
    cases.update(EXPRESSIONS)
    # In a function, with and without an outer `v`, and at the top level
    # under one (where a parameter may reuse the name: the shadow rule is
    # about enclosing functions).
    variants = {
        '': (False, True),
        '__outer': (True, True),
        '__top': (True, False),
    }
    paths = []
    for (name, text) in cases.items():
        for (suffix, (outer, in_fn)) in variants.items():
            by = 2 if in_fn else 0
            lines = ['fn grid_case() {'] if in_fn else []
            if outer:
                lines += [indent("let v = 'outer'\nlet _before = || v", by)]
            lines.append(indent(text, by))
            if outer:
                lines.append(indent('[v, _before()]', by))
            if in_fn:
                lines.append('}')
            path = os.path.join(dir, f'{name}{suffix}.cul')
            with open(path, 'w') as f:
                f.write('\n'.join(lines) + '\n')
            paths.append(path)
    return paths


# ---- runs ----------------------------------------------------------------------


def corpus():
    out = subprocess.run(['git', 'ls-files', '*.cul'], capture_output=True,
                         text=True, check=True).stdout.split()
    return [p for p in out if p.startswith(CORPUS)]


def compile_only(cul, path, env):
    r = subprocess.run([cul, '--vm-dump', path], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, env=env, timeout=120)
    return path, r.returncode


def read_reports(dir, rename):
    """SUMMARY paths and finding lines across a report directory."""
    summaries, findings = set(), set()
    for f in glob.glob(os.path.join(dir, '*.txt')):
        for line in open(f):
            line = rename(line.rstrip('\n'))
            if line.startswith('SUMMARY '):
                summaries.add(line.split(' ')[1])
            elif line:
                findings.add(line)
    return summaries, findings


def read_allow():
    """Each entry is a finding as reported, then `  # why` it is allowed."""
    allowed = {}
    for n, line in enumerate(open(ALLOW), 1):
        line = line.rstrip('\n')
        if line and not line.startswith('#'):
            allowed[line.split('  #', 1)[0].rstrip()] = n
    return allowed


def compare(findings, allowed, compiled, summaries):
    """The failures: unexpected findings, stale entries, missing reports."""
    errors = []
    for f in sorted(findings - set(allowed)):
        errors.append(f'unexpected: {f}')
    for a, n in sorted(allowed.items(), key=lambda kv: kv[1]):
        if a not in findings:
            errors.append(f'stale ({ALLOW}:{n}): {a}')
    for p in sorted(compiled - summaries):
        errors.append(f'no report from a compile that succeeded: {p}')
    return errors


def selftest():
    """The comparison fails on each kind of drift it exists to catch."""
    f, s = {'MISMATCH use \'x\' a.cul:1:1 resolve=x@1:1 compiler=none'}, {'a.cul'}
    assert compare(f, {}, {'a.cul'}, s), 'an unlisted finding passed'
    assert compare(set(), {'MISSING \'y\' a.cul:2:2': 1}, {'a.cul'}, s), \
        'a stale entry passed'
    assert compare(set(), {}, {'a.cul', 'b.cul'}, s), 'a missing report passed'
    assert not compare(f, {next(iter(f)): 1}, {'a.cul'}, s), \
        'a listed finding failed'


def main():
    cul = os.path.abspath(sys.argv[1])
    root = os.getcwd()
    selftest()
    with tempfile.TemporaryDirectory(prefix='scope-agreement.') as tmp:
        gdir = os.path.join(tmp, 'grid')
        os.makedirs(gdir)
        reports = os.path.join(tmp, 'reports')
        os.makedirs(reports)

        real_gdir = os.path.realpath(gdir) + '/'

        def rename(line):
            line = line.replace(real_gdir, 'grid/')
            line = line.replace(gdir + '/', 'grid/')
            return line.replace(root + '/', '')

        env = dict(os.environ, CULEBRA_SCOPE_CHECK=reports)
        files = corpus() + grid(gdir)
        with concurrent.futures.ThreadPoolExecutor(JOBS) as pool:
            results = list(pool.map(lambda p: compile_only(cul, p, env), files))
        crashed = [p for p, rc in results if rc < 0]
        if crashed:
            print(f'scope agreement: {len(crashed)} compile(s) died on a signal, '
                  f'first {crashed[0]}')
            return 1
        compiled = {rename(os.path.abspath(p)) for p, rc in results if rc == 0}
        for args in ([cul, 'test', '--vm', 'tests/culebra_test_self'],
                     ['bash', 'tests/repl_test.sh', cul]):
            before = len(os.listdir(reports))
            r = subprocess.run(args, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, env=env, timeout=600)
            if r.returncode != 0 or len(os.listdir(reports)) == before:
                print(f'scope agreement: {" ".join(args[:2])} did not run cleanly '
                      f'(rc {r.returncode}) or reported nothing')
                return 1
        summaries, findings = read_reports(reports, rename)
    errors = compare(findings, read_allow(), compiled, summaries)
    for e in errors:
        print(e)
    if errors:
        print(f'scope agreement: {len(errors)} problem(s); a finding is a name the '
              'compiler resolves differently from resolve.h (see '
              'include/frontend/scope_check.h for the line kinds)')
        return 1
    print(f'scope agreement OK ({len(compiled)} compiles, '
          f'{len(findings)} allowed findings)')
    return 0


sys.exit(main())
