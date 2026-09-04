# linz — a linear-type checker

A type checker for a tiny lambda calculus with linear types: every value is
tagged `lin` (must be used exactly once) or `un` (used freely), and checking
a program means proving that tagging honest — a `lin` value that's read
twice, or never, is a type error. Ported from
[rust_zero](https://github.com/ytakano/rust_zero)'s `linz`
(`ch09/linz`), a hand-rolled [nom](https://github.com/rust-bakery/nom)
parser over a mutable, depth-indexed type-environment stack. See `lin.cul`'s
own header and inline comments for the language itself and the checking
algorithm; this file is about what changed in the port.

Run it:

```
culebra examples/linz/linz.cul examples/linz/samples/ex1.lin
culebra examples/linz/linz.cul --ast examples/linz/samples/ex1.lin
```

`samples/ex*.lin` type-check; `err*.lin` are rejected for a linearity or
type reason; `parse_err.lin` doesn't even parse. All 16 are the original
Rust project's own examples, unchanged, and `test_lin.cul` checks the
ported checker against all of them.

## What changed from the Rust original

- **PEG instead of a hand-rolled parser.** The BNF in the original
  `parser.rs`'s doc comment translates almost directly into a `Peg`
  grammar — see `docs/stdlib.md` §34. The one real difference is keyword
  boundaries: `%word` (a `Peg`/cpp-peglib directive — see
  `examples/pl0/pl0.cul`'s grammar for the same technique) makes every
  quoted literal in the grammar reject a longer identifier sharing its
  prefix, which the original's read-a-word-then-compare-strings dispatch
  gets for free.
- **An immutable environment, threaded as a return value.** The original's
  `TypeEnv` is two mutable, depth-indexed stacks (one for `lin`, one for
  `un`) with explicit push/pop and a `mem::take`/restore dance for an `un`
  closure's restricted view. `lin.cul`'s `typing(node, env) -> (type,
  env')` instead takes an environment and returns the one that comes out
  the other side, unmutated — see "Why an immutable environment" in
  `lin.cul` for what that buys structurally. `examples/hm/README.md` is
  the same question asked about a second, differently-shaped type system
  (Hindley-Milner inference instead of checking): how much of this
  generalizes turned out to be "less than it looks like."
- **Keywords are reserved.** The original's dispatch only special-cases
  `let`/`if`/`split`/`free`/`lin`/`un` — a bare `true`, `false` or `bool`
  falls through and parses as a variable *reference* (the parser never
  special-cased them, since they only ever appear after a `lin`/`un`
  qualifier in valid programs). `lin.cul`'s `RESERVED` closes that off
  entirely: none of the nine keywords can name a variable, which is
  simpler to state than "some of these are keywords depending on
  position."
- **More precise errors.** The original discards source positions
  entirely (`main.rs` prints `AST:\n{ast:#?}` and a bare message);
  `lin.cul` carries `line`/`column` from the `Peg` node throughout (see
  `pl0.cul`'s `fail(node, msg)` convention, reused here). It also splits
  one error the original conflates: `err5.lin` is an `if` whose branches
  agree on their *type* but disagree on which `lin` variables they
  consumed, and the original reports both cases as "thenとelseの式の型が
  異なる" (mismatched types) — misleading for `err5`, where the types
  match fine. `lin.cul` gives each its own message.

## Companion

`examples/hm` is the same kind of language, checked a different way —
Hindley-Milner inference instead of checking an author's own annotations —
written to test whether any of `lin.cul`'s shape generalizes. It mostly
doesn't; see that example's README for why.
