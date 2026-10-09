#!/usr/bin/env python3
"""Hold `@value` unboxing to the boxed class, over random programs.

A flat `@value` class's instance may live as its fields in frame slots, and
whether it does must not change what a program prints.  The reference for
that is the same program with the decorator gone: its boxed twin.  This
generates programs around one such class (members that end in a scalar and
in an instance, operators, `let` / `let mut` bindings, writes, compound
steps, loops, branches, closures, calls that take an instance, a class
declared inside a function), runs each beside its twin, and keeps the ones
that differ.

The twin has the `@value` line replaced by a comment, so every position in
an error message is the same on both sides.  The twin's instances print
their fields as `mut` and a `@value` class's never do; that is the one
difference taken out, and only out of the twin's output.

Not a gate: nothing runs this on its own.  It is for the cycle that changes
the unboxing, before and after.

Usage: value_twin.py <culebra> <out-dir> [--programs N] [--seed S]
                     [--jit-every K] [--jobs J]

A program is its seed: this runs those of seeds S .. S+N-1 (N 200, S 1), J
at a time (4), and the ones K divides (5; 0: none) under --jit as well.
Each differing program is left in <out-dir> as p<seed>.cul with p<seed>.diff
beside it, and `--seed <seed> --programs 1` runs that one again.
"""
import argparse
import os
import random
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

# The members a class draws from: name -> (argument, source). The argument is
# 0 for none, "s" for a Float, "r" for an instance. One table for the members
# whose value is a Float, one for those that hand back an instance.
SCALAR_MEMBERS = {
    "len": (0, "len() { self.x * self.x + self.y }"),
    "g0": (0, "g0() { self.x + self.y }"),
    "dot": ("r", "dot(o) { self.x * o.x + self.y * o.y }"),
    "first": (0, "first() { P.new(self.y, self.x).x }"),
    "sc": ("s", "sc(k) { self.scale(k).x }"),
    "lenlen": (0, "lenlen() { self.len() + self.g0() }"),
}
RUN_MEMBERS = {
    "scale": ("s", "scale(k) { P.new(self.x * k, self.y * k) }"),
    "swap": (0, "swap() { P.new(self.y, self.x) }"),
    "add": ("r", "add(o) { P.new(self.x + o.x, self.y + o.y) }"),
    "twice": (0, "twice() { self.scale(2.0) }"),
    "mix": ("r", "mix(o) { self.add(o).swap() }"),
    "me": (0, "me() { self.swap().swap() }"),
}
MEMBERS = {**SCALAR_MEMBERS, **RUN_MEMBERS}
# what a member needs declared beside it
NEEDS = {"sc": ["scale"], "twice": ["scale"], "mix": ["add", "swap"],
         "me": ["swap"], "lenlen": ["len", "g0"]}
OPERATORS = {
    "+": "__add__(o) { P.new(self.x + o.x, self.y + o.y) }",
    "-": "__sub__(o) { P.new(self.x - o.x, self.y - o.y) }",
    "*": "__mul__(k) { P.new(self.x * k, self.y * k) }",
    "neg": "__neg__() { P.new(-self.x, -self.y) }",
}
CTORS = [
    "  new(.x: Float, .y: Float) {}",
    "  x: Float\n  y: Float\n  new(x: Float, y: Float) {\n    self.x = x\n    self.y = y\n  }",
]


class Gen:
    def __init__(self, rng):
        self.r = rng
        self.n = 0

    def fresh(self, p):
        self.n += 1
        return f"{p}{self.n}"

    def klass(self):
        r = self.r
        members = set(r.sample(sorted(SCALAR_MEMBERS), r.randint(1, 3)) +
                      r.sample(sorted(RUN_MEMBERS), r.randint(1, 3)))
        grew = True
        while grew:
            grew = False
            for m in list(members):
                for d in NEEDS.get(m, []):
                    if d not in members:
                        members.add(d)
                        grew = True
        self.smem = sorted(m for m in members if m in SCALAR_MEMBERS)
        self.rmem = sorted(m for m in members if m in RUN_MEMBERS)
        self.ops = sorted(r.sample(sorted(OPERATORS), r.randint(0, 4)))
        body = [r.choice(CTORS)]
        decls = [MEMBERS[m][1] for m in self.smem + self.rmem]
        decls += [OPERATORS[o] for o in self.ops]
        r.shuffle(decls)
        body += ["  " + d for d in decls]
        return "@value\nclass P {\n" + "\n".join(body) + "\n}\n"

    def lit(self):
        return self.r.choice(["1.0", "2.0", "0.5", "3.0", "0.25", "4.0", "1.5"])

    # runs: names of P-valued locals in scope; boxed: names holding an
    # instance that went through a boundary; scal: Float locals.
    # chain=True, here and in run(): the expression goes where a fold would
    # bind apart (a postfix head, an operand of `*`), so a fold is bracketed.
    def scalar(self, env, d=0, chain=False):
        r = self.r
        k = r.random()
        if d > 2 or k < 0.25:
            if env["scal"] and r.random() < 0.4:
                return r.choice(env["scal"])
            return self.lit()
        if k < 0.45:
            return f"{self.run(env, d + 1, chain=True)}.{r.choice(['x', 'y'])}"
        if k < 0.8 and self.smem:
            return self.call(self.smem, env, d)
        e = f"{self.scalar(env, d + 1)} {r.choice(['+', '-', '*'])} {self.scalar(env, d + 1)}"
        return f"({e})" if chain else e

    # one of the members `names`, called on an instance
    def call(self, names, env, d):
        m = self.r.choice(names)
        return f"{self.run(env, d + 1, chain=True)}.{m}({self.args(MEMBERS[m][0], env, d)})"

    def args(self, kind, env, d):
        if kind == 0:
            return ""
        if kind == "s":
            return self.scalar(env, d + 1)
        return self.run(env, d + 1)

    def run(self, env, d=0, chain=False):
        r = self.r
        k = r.random()
        names = env["runs"] + env["boxed"]
        if names and (d > 2 or k < 0.3):
            return r.choice(names)
        if d <= 2 and k >= 0.5:
            if k < 0.75 and self.rmem:
                return self.call(self.rmem, env, d)
            if self.ops:
                o = r.choice(self.ops)
                if o == "neg":
                    e = f"-{self.run(env, d + 1, chain=True)}"
                elif o == "*":
                    e = f"{self.run(env, d + 1, chain=True)} * {self.scalar(env, d + 1, chain=True)}"
                else:
                    e = f"{self.run(env, d + 1, chain=True)} {o} {self.run(env, d + 1, chain=True)}"
                return f"({e})" if chain else e
        return f"P.new({self.scalar(env, d + 1)}, {self.scalar(env, d + 1)})"

    def show(self, e):
        return f"println({e})"

    def stmt(self, env, ind, depth):
        r = self.r
        k = r.random()
        pad = "  " * ind
        if k < 0.22 or not env["runs"]:
            v = self.fresh("v")
            mut = r.random() < 0.6
            s = f"{pad}let {'mut ' if mut else ''}{v} = {self.run(env)}"
            env["runs"].append(v)
            if mut:
                env["mut"].append(v)
            return [s]
        if k < 0.36 and env["mut"]:
            v = r.choice(env["mut"])
            return [f"{pad}{v} = {self.run(env)}"]
        if k < 0.46 and env["mut"] and self.ops and self.ops != ["neg"]:
            v = r.choice(env["mut"])
            o = r.choice([o for o in self.ops if o != "neg"])
            rhs = self.scalar(env) if o == "*" else self.run(env)
            return [f"{pad}{v} {o}= {rhs}"]
        if k < 0.58:
            return [pad + self.show(self.scalar(env))]
        if k < 0.66:
            return [pad + self.show(r.choice(env["runs"] + env["boxed"]))]
        if k < 0.74:
            w = self.fresh("w")
            s = f"{pad}let {w} = id({r.choice(env['runs'] + env['boxed'])})"
            env["boxed"].append(w)
            return [s]
        if k < 0.80:
            return [f"{pad}bag.push({self.run(env)})"]
        if k < 0.84:
            s = self.fresh("s")
            out = f"{pad}let {s} = {self.scalar(env)}"
            env["scal"].append(s)
            return [out]
        if depth < 2 and k < 0.88:
            g = self.fresh("g")
            inner = {"runs": [], "boxed": list(env["boxed"]), "mut": [],
                     "scal": list(env["scal"])}
            body = self.block(inner, ind + 1, depth + 1, r.randint(1, 3))
            tail = self.run(inner) if r.random() < 0.5 else self.scalar(inner)
            return ([f"{pad}let {g} = fn () {{"] + body + [f"{pad}  {tail}", f"{pad}}}",
                    pad + self.show(f"{g}()")])
        if depth < 2 and k < 0.94:
            i = self.fresh("i")
            inner = self.scope(env)
            body = self.block(inner, ind + 1, depth + 1, r.randint(1, 3))
            return [f"{pad}for {i} in 0..{r.randint(1, 3)} {{"] + body + [f"{pad}}}"]
        if depth < 2:
            a = self.block(self.scope(env), ind + 1, depth + 1, r.randint(1, 3))
            b = self.block(self.scope(env), ind + 1, depth + 1, r.randint(1, 3))
            return ([f"{pad}if {self.scalar(env)} > {self.lit()} {{"] + a +
                    [f"{pad}}} else {{"] + b + [f"{pad}}}"])
        return [pad + self.show(self.scalar(env))]

    @staticmethod
    def scope(env):
        return {k: list(v) for k, v in env.items()}

    def block(self, env, ind, depth, n):
        out = []
        for _ in range(n):
            out += self.stmt(env, ind, depth)
        return out

    def function(self):
        r = self.r
        f = self.fresh("f")
        env = {"runs": [], "boxed": [], "mut": [], "scal": []}
        body = self.block(env, 1, 0, r.randint(3, 8))
        # every local's final state, then a tail of each kind
        for v in env["runs"] + env["boxed"]:
            body.append(f"  println({v}.x + {v}.y)")
        k = r.random()
        if k < 0.25:
            body.append(f"  let {self.fresh('t')} = {self.run(env)}")
        elif k < 0.45 and env["mut"]:
            body.append(f"  {r.choice(env['mut'])} = {self.run(env)}")
        elif k < 0.6 and env["runs"]:
            body.append(f"  {r.choice(env['runs'] + env['boxed'])}")
        elif k < 0.8:
            body.append("  " + self.scalar(env))
        return f, f"fn {f}() {{\n" + "\n".join(body) + "\n}\n"

    def program(self):
        body = self.klass()
        calls = []
        for _ in range(self.r.randint(1, 2)):
            f, text = self.function()
            body += text
            calls.append(f"println({f}())")
        # a stretch outside any function too
        env = {"runs": [], "boxed": [], "mut": [], "scal": []}
        top = self.block(env, 0, 0, self.r.randint(0, 5))
        body += "\n".join(calls + top) + "\n"
        src = "fn id(o) { o }\nlet bag = []\n"
        if self.r.random() < 0.25:
            # the class as a local of a function, read from closures below it
            inner = "".join("  " + ln + "\n" for ln in body.splitlines())
            src += "fn outer() {\n" + inner + "}\nouter()\n"
        else:
            src += body
        src += "for b in bag { println(b.x + b.y) }\n"
        return src


def write(path, text):
    with open(path, "w") as f:
        f.write(text)


def output_of(binary, path, jit):
    cmd = [binary] + (["--jit"] if jit else []) + [path]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=20)
        out = p.stdout + p.stderr + f"[exit {p.returncode}]"
    except subprocess.TimeoutExpired:
        out = "[timeout]"
    return out.replace(path, "FILE")


def differ(binary, work, seed, jit):
    """The program of `seed` beside its twin: None when every lane agrees,
    else (source, lane, want, got) for the first lane that does not."""
    src = Gen(random.Random(seed)).program()
    twin = os.path.join(work, f"{seed}.twin.cul")
    value = os.path.join(work, f"{seed}.value.cul")
    write(twin, src.replace("@value\n", "#\n", 1))
    write(value, src)
    want = output_of(binary, twin, False).replace("{mut ", "{").replace(", mut ", ", ")
    for lane in ["exec"] + (["jit"] if jit else []):
        got = output_of(binary, value, lane == "jit")
        if got != want:
            return src, lane, want, got
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("outdir")
    ap.add_argument("--programs", type=int, default=200)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--jit-every", type=int, default=5)
    ap.add_argument("--jobs", type=int, default=4)
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    seeds = range(a.seed, a.seed + a.programs)
    bad = 0
    # The real path, which is how a SyntaxError names a file. Each program has
    # its own pair of files and map() hands the results back in order, so what
    # is reported does not depend on --jobs.
    with tempfile.TemporaryDirectory(dir=os.path.realpath(a.outdir)) as work:
        pool = ThreadPoolExecutor(a.jobs)
        try:
            found = pool.map(
                lambda s: differ(a.binary, work, s, a.jit_every and s % a.jit_every == 0), seeds)
            for seed, hit in zip(seeds, found):
                if hit is None:
                    continue
                src, lane, want, got = hit
                bad += 1
                base = os.path.join(a.outdir, f"p{seed}")
                write(base + ".cul", src)
                write(base + ".diff", f"lane: {lane}\n--- want\n{want}\n--- got\n{got}\n")
                first = next((g for w, g in zip(want.split("\n"), got.split("\n")) if w != g), got[-80:])
                print(f"DIFF seed={seed} {lane}: {first[:100]}")
        finally:
            # an interrupt drops what is queued instead of running it out
            pool.shutdown(cancel_futures=True)
    print(f"{bad} of {a.programs} differ")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
