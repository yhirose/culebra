#!/usr/bin/env python3
"""A statement means the same in a fn, a generator body and an effect body.

Generator and effect bodies are lowered to state machines, source to source;
a plain fn body is not. This places one statement on the same line and
column of the three, and fails when what they print or raise (kind, line,
column, message) differs. EMIT(x) in a statement is spelled
`plain_emit__(x)`, `yield       (x)` and `perform emit(x)`, the same width,
so the columns after it line up.

Three sweeps:
  body  the statement mid-body, over statement forms x conditions x values
  tail  the statement last, so the fn's value is compared too (plain, effect)
  iter  `for x in v` over iterable and non-iterable kinds of `v`

A context that hangs or dies without a word fails the case even when the
others do the same, and each sweep must run at least one case to a value in
every context, so that a check that measures nothing cannot pass.

Usage: lowering_diff.py <culebra-binary> [--jit]
JOBS (default: the CPU count) bounds the processes run at once, and
CULEBRA_TEST_TIMEOUT (default 60) each one's seconds.
"""
import concurrent.futures
import itertools
import os
import subprocess
import sys
import tempfile

CUL = sys.argv[1]
ENGINE = '--jit' if '--jit' in sys.argv[2:] else '--vm'
JOBS = int(os.environ.get('JOBS') or os.cpu_count() or 4)
TIMEOUT = int(os.environ.get('CULEBRA_TEST_TIMEOUT') or 60)

CATCH = 'catch e { "ERR {e.kind} {e.line}:{e.col} {e.message}" }'
EMITS = {'plain': 'plain_emit__', 'gen': 'yield       ', 'eff': 'perform emit'}

# The statement among others; its value is not looked at.
BODY = {
    'plain': """mut out = [-1]; fn plain_emit__(x) { out.push(x); nil }
fn run(z) { mut a = 0; mut b = 0
  {S}
  out
}
print(try { run({Z}) } """ + CATCH + """)
""",
    'gen': """// gen
fn run(z) { mut a = 0; mut b = 0; yield -1
  {S}
}
print(try { run({Z}).collect() } """ + CATCH + """)
""",
    'eff': """effect fn emit(x)
effect fn run(z) { mut a = 0; mut b = 0; perform emit(-1)
  {S}
  nil
}
mut out = []
print(try { handle { run({Z}) } with emit(x, k) { out.push(x); k(nil) }; out } """ + CATCH + """)
""",
}

# The statement last: the fn's value is compared too (a generator has none).
TAIL = {
    'plain': """mut out = [-1]; fn plain_emit__(x) { out.push(x); nil }
fn run(z) { mut a = 0; mut b = 0
  {S}
}
print(try { [run({Z}), out] } """ + CATCH + """)
""",
    'eff': """effect fn emit(x)
effect fn run(z) { mut a = 0; mut b = 0; perform emit(-1)
  {S}
}
mut out = []
print(try { [handle { run({Z}) } with emit(x, k) { out.push(x); k(nil) }, out] } """ + CATCH + """)
""",
}

BODY_FORMS = [
    'EMIT(1) if C',
    'EMIT(1) unless C',
    'if C { EMIT(1) }',
    'if C { EMIT(1) } else { EMIT(2) }',
    'if false { EMIT(0) } else if C { EMIT(1) }',
    'if C { EMIT(1) } else if C { EMIT(2) } else { EMIT(3) }',
    'while C { EMIT(1); break }',
    'mut i = 0; while i < 2 && C { EMIT(i); i = i + 1 }',
    'outer: while true { while C { EMIT(1); break outer }; break }',
    '{ EMIT(1) if C }',
    '{ EMIT(1) unless C }',
    '{ let q = 1; EMIT(q) if C }',
    'if C { { EMIT(1) } }',
    '{ { EMIT(1) unless C } }',
    'for x in [1, 2] { EMIT(x) if C }',
    'for x in [1, 2] { EMIT(x) unless C }',
    'for x in z { EMIT(x) }',
    'for x in [z] { EMIT(x) unless C }',
    '(a, b) = (1, 2) if C; EMIT(a)',
    '(a, b) = (1, 2) unless C; EMIT(a)',
    '(a, b) = (1, 2) unless C; (a, b) = (b, a) if C; EMIT(a)',
    'if C { (a, b) = (3, 4); EMIT(a) }',
    '(a, (b,)) = (1, (2,)); (a, (b,)) = (3, (4,)); EMIT(a + b)',
    'EMIT(C)',
    'EMIT(1) if C && true',
    'EMIT(1) unless C || false',
    'EMIT(1) unless !(C)',
    'EMIT(if C { 1 } else { 2 })',
    'EMIT([C, 1][1]) if true',
    'EMIT(1) if C; EMIT(2)',
]
TAIL_FORMS = [
    '{ 5 }',
    '{ EMIT(1); 5 }',
    '{ { EMIT(1) } }',
    'if C { 5 }',
    'if C { EMIT(1); 5 } else { 6 }',
    'if C { { EMIT(1); 5 } } else { 6 }',
    'EMIT(1)',
    'EMIT(1) if C',
    'EMIT(1) unless C',
    'while C { EMIT(1); break }',
    'for x in [1] { EMIT(x) }',
    '(a, b) = (1, 2) if C',
    'if C { 1 } else if !(C) { EMIT(2); 2 }',
    'C && true',
    'true && C',
]
CONDS = ['z', '(z)', '((z))', '!z', 'z == 1']
VALUES = ['true', 'false', 'nil', '1', '"s"']

# What a `for` walks, and what it refuses.
ITER_PRELUDE = """class Counted {
  new(.n) {}
  iter() { Walk(0, self.n) }
}
class Walk {
  new(.i, .n) {}
  has_next() { self.i < self.n }
  next() { self.i = self.i + 1; self.i }
}
class Bare {
  new() {}
}
class OnlyStep {
  new() {}
  has_next() { false }
  next() { 1 }
}
class RetFive {
  new() {}
  iter() { 5 }
}
class RetBare {
  new() {}
  iter() { {a: 1} }
}
fn gen2() { yield 1; yield 2 }
"""
ITER_VALUES = [
    '[1, 2]', '(1, 2)', '"ab"', '{1, 2}', '{a: 1}', '{}', '0..2', 'range(2)',
    'gen2()', 'Counted(2)', 'Bare()', 'OnlyStep()', '[1, 2].map(|x| x * 10)',
    'RetFive()', 'RetBare()', 'Tensor.zeros(2)', 'nil', '1', 'true', '1.5',
    'fn () { 1 }',
]

SWEEPS = [  # name, templates, statement forms, values of z, prelude
    ('body', BODY, BODY_FORMS, VALUES, ''),
    ('tail', TAIL, TAIL_FORMS, VALUES, ''),
    ('iter', BODY, ['for x in z { EMIT(x) }'], ITER_VALUES, ITER_PRELUDE),
]


def spell(stmt, ctx):
    """EMIT(x) -> the context's emitter applied to x."""
    out, i = '', 0
    while (j := stmt.find('EMIT(', i)) >= 0:
        out += stmt[i:j]
        depth, k = 0, j + 4
        while True:
            depth += {'(': 1, ')': -1}.get(stmt[k], 0)
            if depth == 0:
                break
            k += 1
        out += EMITS[ctx] + '(' + stmt[j + 5:k] + ')'
        i = k + 1
    return out + stmt[i:]


def cases():
    """(sweep, label, {context: program}) for every distinct case."""
    for sweep, templates, forms, values, prelude in SWEEPS:
        stmts = dict.fromkeys(f.replace('C', c) for f in forms for c in CONDS)
        for stmt, val in itertools.product(stmts, values):
            yield (sweep, f'{sweep} {stmt!r} z={val}',
                   {ctx: prelude + t.replace('{S}', spell(stmt, ctx))
                                    .replace('{Z}', val)
                    for ctx, t in templates.items()})


def run(src):
    """All the program printed, on one line, or why it printed nothing."""
    with tempfile.NamedTemporaryFile('w', suffix='.cul', delete=False) as f:
        f.write(src)
        path = f.name
    try:
        r = subprocess.run([CUL, ENGINE, path], capture_output=True, text=True,
                           timeout=TIMEOUT)
        out = (r.stdout + r.stderr).strip()
        # The binary names the file by its real path (/private/tmp on macOS).
        for p in (os.path.realpath(path), path):
            out = out.replace(p, '<file>')
        return ' | '.join(out.splitlines()) or f'<no output, rc={r.returncode}>'
    except subprocess.TimeoutExpired:
        return '<timeout>'
    finally:
        os.unlink(path)


def main():
    jobs = list(cases())
    bad, live = 0, set()
    with concurrent.futures.ThreadPoolExecutor(JOBS) as pool:
        futures = [{ctx: pool.submit(run, src) for ctx, src in progs.items()}
                   for _, _, progs in jobs]
        for (sweep, label, _), futs in zip(jobs, futures):
            got = {ctx: f.result() for ctx, f in futs.items()}
            if all(o.startswith('[') for o in got.values()):
                live.add(sweep)
            if len(set(got.values())) > 1 or any(o.startswith('<')
                                                 for o in got.values()):
                bad += 1
                print(f'lowering-diff FAIL: {label}', file=sys.stderr)
                for ctx, out in got.items():
                    print(f'    {ctx:5}: {out}', file=sys.stderr)
    dead = [sweep for sweep, *_ in SWEEPS if sweep not in live]
    if dead:
        print(f'lowering-diff FAIL: no case of {", ".join(dead)} ran to a value '
              f'in every context ({ENGINE})', file=sys.stderr)
        return 1
    if bad:
        print(f'lowering-diff FAIL: {bad} of {len(jobs)} cases disagree '
              f'({ENGINE})', file=sys.stderr)
        return 1
    print(f'lowering-diff OK ({len(jobs)} cases, {ENGINE})')
    return 0


sys.exit(main())
