#!/usr/bin/env python3
"""A statement means the same in a fn, a generator body and an effect body.

Generator and effect bodies are lowered to state machines, source to source;
a plain fn body is not. This places one statement on the same line and
column of the three, and fails when what they print or raise (kind, line,
column, message) differs. EMIT(x) in a statement is spelled
`plain_emit__(x)`, `yield       (x)` and `perform emit(x)`, the same width,
so the columns after it line up.

The sweeps:
  body     the statement mid-body, over statement forms x conditions x values
  tail     the statement last, so the fn's value is compared too (plain, effect)
  iter     `for x in v` over iterable and non-iterable kinds of `v`
  sole     the statement as the whole body, which declares nothing ahead of it
  binding  what a name means where it is written: declared or reassigned, in
           reach or not, mutable or not, captured by which closure
  binding-fn, binding-defer
           the same over the forms one lowering refuses by design, in the two
           contexts that take them

A context that hangs or dies without a word fails the case even when the
others do the same, and each sweep must run at least one case to a value in
every context, so that a check that measures nothing cannot pass.

The forms known to disagree are listed in lowering_diff_allow.txt, each with
how many of its cases do. A form that disagrees and is not listed fails, and
so does a listed one whose count moved or that agrees now: a fix removes its
line.

Usage: lowering_diff.py <culebra-binary> [--jit] [--sweep name,...]
JOBS (default: the CPU count) bounds the processes run at once, and
CULEBRA_TEST_TIMEOUT (default 60) each one's seconds.
"""
import collections
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

# The statement is the whole body, which no declaration precedes: what it
# binds is the body's own, and a block around it collapses onto it.
SOLE = {
    'plain': """mut out = []; fn plain_emit__(x) { out.push(x); nil }
fn run(z) {
  {S}
}
print(try { [run({Z}), out] } """ + CATCH + """)
""",
    'gen': """// gen
fn run(z) {
  {S}
}
print(try { [nil, run({Z}).collect()] } """ + CATCH + """)
""",
    'eff': """effect fn emit(x)
effect fn run(z) {
  {S}
}
mut out = []
print(try { [handle { run({Z}) } with emit(x, k) { out.push(x); k(nil) }, out] } """ + CATCH + """)
""",
}


def without(templates, ctx):
    return {c: t for c, t in templates.items() if c != ctx}


# What a name means where it is written. Each form ends in an EMIT, so a
# plain fn's value is nil as a generator's is. `top` and `mtop` are top-level
# names a local may hide or the body may write; `log` is what a defer writes.
BINDING_PRELUDE = 'top = 5\nmut mtop = 5\nmut log = []\n'
BINDING_FORMS = [
    # Reassigning what cannot be reassigned...
    'a = 1; a = 2; EMIT(a)',
    'a = 1; EMIT(a); a = 2; EMIT(a)',
    'a = 1; a += 2; EMIT(a)',
    'a = 1; { a = 2 }; EMIT(a)',
    'a = 1; if C { a = 2 }; EMIT(a)',
    'a = 1; f = fn () { a = 2 }; f(); EMIT(a)',
    'let a = 1; a = 2; EMIT(a)',
    'let a = 1; EMIT(a); a = 2; EMIT(a)',
    'let a = 1; while false { a = 2 }; EMIT(a)',
    'z = 5; EMIT(z)',
    'z += 5; EMIT(z)',
    'println = 1; EMIT(1)',
    'top = 6; EMIT(top)',
    '(a, b) = (1, 2); a = 3; EMIT(a)',
    '(a, b) = (1, 2); EMIT(a); (a, b) = (3, 4); EMIT(a)',
    'let (a, b) = (1, 2); a = 3; EMIT(a)',
    'mut a = 1; b = 2; EMIT(a); (a, b) = (3, 4); EMIT(a)',
    'let a = nil; a ??= 1; EMIT(a)',
    'let a = 1; a ??= 2; EMIT(a)',
    'a = 1; a: String = "s"; EMIT(a)',
    'a = 1; EMIT(a); a: Long = "s"; EMIT(a)',
    # ...and what can.
    'mut a = 1; a = 2; EMIT(a)',
    'mut (a, b) = (1, 2); a = 3; EMIT(a)',
    'mut a = nil; a ??= 1; EMIT(a)',
    'mtop = 6; EMIT(mtop)',
    'EMIT(1); mtop += 1; EMIT(mtop)',
    'mut a = 1; f = fn () { a = a + 1 }; f(); EMIT(a); f(); EMIT(a)',
    # What a loop, a match arm or a catch binds.
    'for x in [1] { x = 2 }; EMIT(1)',
    'for x in [1] { EMIT(x); x = 2 }',
    'for x in [1, 2] { x += 10; EMIT(x) }',
    'for (x, y) in [(1, 2)] { x = 5 }; EMIT(1)',
    'match z { q => { q = 2 } }; EMIT(1)',
    'try { throw "t" } catch e { e = 1 }; EMIT(1)',
    # A name read where no declaration reaches.
    'EMIT(a); a = 1',
    'q += 1; EMIT(1)',
    '{ a = 1 }; EMIT(a)',
    '{ a = 1; EMIT(a) }; EMIT(a)',
    '{ a = 1 }; a = 2; EMIT(a)',
    'if C { a = 1 }; EMIT(a)',
    'if C { a = 1; EMIT(a) }; EMIT(a)',
    'if C { a = 1 }; a = 2; EMIT(a)',
    'if C { a = 1 } else { a = 2 }; EMIT(a)',
    'if C { a = 1 } else { a = 2 }; a = 3; EMIT(a)',
    'if C { mut a = 1 }; a = 2; EMIT(a)',
    'if C { (a, b) = (1, 2) }; EMIT(a)',
    'if C { (a, b) = (1, 2) }; a = 3; EMIT(a)',
    'a = 1 if C; EMIT(a)',
    'a = 1 if C; a = 2; EMIT(a)',
    'while C { a = 1; break }; EMIT(a)',
    'while C { a = 1; EMIT(a); break }; EMIT(a)',
    'for x in [1] { EMIT(x) }; EMIT(x)',
    'match z { q => 0 }; EMIT(q)',
    'try { a = 1 } catch e { 0 }; EMIT(a)',
    'try { throw "t" } catch e { 0 }; EMIT(e)',
    'if let d = 1; C { EMIT(d) }; EMIT(d)',
    # A nested scope's declaration over an outer name.
    'a = 1; { let a = 2; EMIT(a) }; EMIT(a)',
    'let a = "outer"; if C { let a = "inner"; EMIT(a) }; EMIT(a)',
    'let a = "outer"; if C { EMIT(0) } else { let a = "inner"; EMIT(a) }; EMIT(a)',
    'let a = "outer"; EMIT(C ? (let a = "inner") : 0); EMIT(a)',
    'let a = "outer"; while C { let a = "inner"; EMIT(a); break }; EMIT(a)',
    'let a = "outer"; for q in [1] { let a = "inner"; EMIT(a) }; EMIT(a)',
    'let a = "outer"; for a in [7] { EMIT(a) }; EMIT(a)',
    'let a = "outer"; match z { a => 0 }; EMIT(a)',
    'let a = "outer"; try { let a = "inner" } catch e { 0 }; EMIT(a)',
    '{ let top = 1 }; EMIT(top)',
    'if C { let top = 1 }; EMIT(top)',
    'if C { let top = 1; EMIT(top) }; EMIT(top)',
    'if C { let z = 9; EMIT(z) }; EMIT(z)',
    '{ mut z = 9; z += 1; EMIT(z) }; EMIT(z)',
    # Sibling arms: one name, two variables.
    'if C { mut a = 1; a = 2; EMIT(a) } else { a = 3; a = 4; EMIT(a) }',
    'if C { a = 1; EMIT(a) } else { mut a = 3; a = 4; EMIT(a) }; EMIT(0)',
    # The variable a closure captures.
    'mut fs = []; for i in [1, 2, 3] { fs.push(|| i) }; EMIT(fs.map(|f| f()))',
    'mut fs = []; for i in [1, 2, 3] { let x = i * 10; fs.push(|| x) }; EMIT(fs.map(|f| f()))',
    'mut fs = []; for i in [1, 2, 3] { let x = i * 10; fs.push(|| x); EMIT(i) }; EMIT(fs.map(|f| f()))',
    'mut fs = []; mut i = 0; while i < 3 { let x = i; fs.push(|| x); i += 1 }; EMIT(fs.map(|f| f()))',
    'mut fs = []; mut i = 0; while i < 3 { let x = i; fs.push(|| x); EMIT(i); i += 1 }; EMIT(fs.map(|f| f()))',
    'mut f = nil; if C { let a = 1; f = || a }; EMIT(f == nil ? 0 : f())',
    'mut f = nil; a = 0; if C { let a = 1; f = || a; EMIT(a) }; EMIT([a, f == nil ? 0 : f()])',
    'f = fn () { a }; a = 1; EMIT(f())',
    'f = fn () { a }; EMIT(0); { a = 1 }; EMIT(1)',
    'c = || x; EMIT(try { c() } catch e { e.kind }); let x = 5; EMIT(x)',
    # A function's own declaration over an enclosing one.
    'a = 1; f = fn () { let a = 2; a }; EMIT(f())',
    'a = 1; f = fn (a) { a }; EMIT(f(2))',
    '{ let println = 1 }; EMIT(1)',
    # A declaration in an expression that may not run is the scope's around it.
    'C && (let h = 1); EMIT(h)',
    'if false { EMIT(0) } else if (let m = C) { EMIT(m) }; EMIT(m)',
    # An init clause's.
    'if mut d = 1; C { d += 1; EMIT(d) }; EMIT(0)',
    'while let d = 1; C { EMIT(d); break }; EMIT(0)',
    # A class of the body's own.
    'class K { new() {} }; EMIT(1); EMIT(K() == nil)',
]
# A generator body takes no named fn, and an effect body no nested defer.
BINDING_FN_FORMS = [
    'fn h() { 1 }; EMIT(1); EMIT(h())',
    'if C { fn h() { 1 }; EMIT(h()) }; EMIT(h())',
    'a = 1; if C { fn h() { a }; EMIT(h()) }; EMIT(a)',
]
BINDING_DEFER_FORMS = [
    '{ defer { log.push("d") }; log.push("body") }; log.push("after"); EMIT(log)',
    'if C { defer { log.push("d") }; log.push("body") }; log.push("after"); EMIT(log)',
]

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

SWEEPS = [  # name, templates, statement forms, conditions, values of z, prelude
    ('body', BODY, BODY_FORMS, CONDS, VALUES, ''),
    ('tail', TAIL, TAIL_FORMS, CONDS, VALUES, ''),
    ('iter', BODY, ['for x in z { EMIT(x) }'], CONDS, ITER_VALUES, ITER_PRELUDE),
    ('sole', SOLE, BODY_FORMS, CONDS, VALUES, ''),
    ('binding', SOLE, BINDING_FORMS, ['z'], VALUES, BINDING_PRELUDE),
    ('binding-fn', without(SOLE, 'gen'), BINDING_FN_FORMS, ['z'], VALUES,
     BINDING_PRELUDE),
    ('binding-defer', without(SOLE, 'eff'), BINDING_DEFER_FORMS, ['z'], VALUES,
     BINDING_PRELUDE),
]
if '--sweep' in sys.argv:
    only = sys.argv[sys.argv.index('--sweep') + 1].split(',')
    SWEEPS = [s for s in SWEEPS if s[0] in only]

ALLOW = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     'lowering_diff_allow.txt')


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
    """(sweep, form, label, {context: program}) for every distinct case."""
    for sweep, templates, forms, conds, values, prelude in SWEEPS:
        # A form no condition is written in is one statement, not one per
        # condition; one two conditions spell alike is the first's.
        stmts = {}
        for f, c in itertools.product(forms, conds):
            stmts.setdefault(f.replace('C', c), f)
        for (stmt, form), val in itertools.product(stmts.items(), values):
            yield (sweep, form, f'{sweep} {stmt!r} z={val}',
                   {ctx: prelude + t.replace('{S}', spell(stmt, ctx))
                                    .replace('{Z}', val)
                    for ctx, t in templates.items()})


def allowed():
    """{(sweep, form): cases known to disagree}, from lowering_diff_allow.txt."""
    out = {}
    with open(ALLOW) as f:
        for line in f:
            if line.strip() and not line.startswith('#'):
                sweep, count, form = line.rstrip('\n').split(None, 2)
                out[sweep, form] = int(count)
    return out


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
    live = set()
    diffs = collections.defaultdict(list)  # (sweep, form) -> its cases that disagree
    with concurrent.futures.ThreadPoolExecutor(JOBS) as pool:
        futures = [{ctx: pool.submit(run, src) for ctx, src in progs.items()}
                   for *_, progs in jobs]
        for (sweep, form, label, _), futs in zip(jobs, futures):
            got = {ctx: f.result() for ctx, f in futs.items()}
            if all(o.startswith('[') for o in got.values()):
                live.add(sweep)
            if len(set(got.values())) > 1 or any(o.startswith('<')
                                                 for o in got.values()):
                diffs[sweep, form].append(f'  {label}\n' + ''.join(
                    f'    {ctx:5}: {out}\n' for ctx, out in got.items()))
    if '--list' in sys.argv:
        for (sweep, form), cases_ in diffs.items():
            print(f'{sweep} {len(cases_)} {form}')
        return 0
    ran = {sweep for sweep, *_ in SWEEPS}
    allow = {k: n for k, n in allowed().items() if k[0] in ran}
    bad = 0
    for key in dict.fromkeys([*diffs, *allow]):
        n, known = len(diffs.get(key, [])), allow.get(key)
        if n == known:
            continue
        bad += 1
        why = (f'{n} cases disagree' if known is None else
               f'{n} cases disagree, {os.path.basename(ALLOW)} says {known}' if n else
               f'agrees now: remove its line from {os.path.basename(ALLOW)}')
        print(f'lowering-diff FAIL: {key[0]} {key[1]!r}: {why}', file=sys.stderr)
        sys.stderr.write(''.join(diffs.get(key, [])))
    dead = [sweep for sweep in ran if sweep not in live]
    if dead:
        print(f'lowering-diff FAIL: no case of {", ".join(dead)} ran to a value '
              f'in every context ({ENGINE})', file=sys.stderr)
        return 1
    if bad:
        print(f'lowering-diff FAIL: {bad} forms differ from what is known '
              f'({ENGINE})', file=sys.stderr)
        return 1
    print(f'lowering-diff OK ({len(jobs)} cases, {sum(allow.values())} of them '
          f'in {len(allow)} forms known to disagree, {ENGINE})')
    return 0


sys.exit(main())
