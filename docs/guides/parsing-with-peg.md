# Parsing with PEG

A guide to the `PEG` namespace: reaching for a grammar where a regex runs
out, and going from there to a language of your own with an interpreter
to run it. Every code block here runs — they are executed as doctests on
both engines.

The API reference is [`stdlib.md` §34](../stdlib.md#34-peg). This guide is
about *when* to use it and how to shape a grammar; the reference is about
what each call does.

## What a PEG is

A PEG — a *parsing expression grammar* — is a grammar you write to get a
parser, the job you would once have handed to YACC. Three differences
matter in practice. There is no separate lexer: a PEG describes characters
as readily as tokens, so the tokenizer is just more rules. There is no
build step: `PEG.compile` loads a grammar at run time, and the grammar is
an ordinary string in your program. And choice is **ordered** — `a / b`
commits to `a` the moment `a` matches, rather than reporting a
shift/reduce conflict for you to resolve — which means a PEG is
unambiguous by construction and reads top to bottom in the order it is
written. One classic PEG limitation does not apply here: left recursion,
which a PEG normally cannot express at all, is supported, because the
engine is [cpp-peglib](https://github.com/yhirose/cpp-peglib) — the same
parser culebra's own front end runs on — and it is one of the
implementations extended to handle it.

## Why a grammar

Bert Hubert put the case for PEG well in
[Practical PEG parsing](https://berthub.eu/articles/posts/practical-peg-parsing/):
easy things should be easy, and hard things should be possible. A regex is
excellent at the easy end and has no answer at the hard end, because the
hard end is where structure appears — nesting, escaping, "this token but
not inside that one". The usual next step is a hand-written scanner, and
that is where an afternoon goes.

A PEG sits between them. It costs about as much as a regex to write for a
small job, and unlike a regex it keeps working as the job grows. Three
properties do most of that work:

* **Rules compose.** A rule can refer to another rule, including itself.
  That is what buys nesting, and it is what a regex fundamentally lacks.
* **Choice is ordered.** `a / b` tries `a` first and only then `b`. There
  is no ambiguity to resolve — and no ambiguity means the grammar means
  exactly what it says, in the order it says it.
* **You get a tree.** In culebra the tree is plain `Object`s, so `match`
  takes it apart with no visitor API in between.

The engine is [cpp-peglib](https://github.com/yhirose/cpp-peglib), the same
parser culebra's own front end runs on.

## 1. Extracting from lines, where a regex cannot reach

The everyday shape of a text job is a loop over lines: match a pattern in
each one, pull out its parts. A regex is the right tool for that right up
until the pattern *nests* — and then it is not merely awkward, it is
outside what a regular language can express. Three examples, each a job
you have probably done with a regex, each with one recursive rule in it.

### 1.1 A log line with a structured payload

Here is the input — a log file where each line carries a structured
context object:

```
2026-09-01T10:00:02 WARN retry ctx={user: {id: 7, tags: [a, b]}, path: /x}
2026-09-01T10:00:05 INFO ok ctx={path: /y}
```

and here is what we want out of each line: the level, the message, the
names of the **top-level** keys in `ctx`, and how deeply it nests.

```
WARN retry keys=['user', 'path'] depth=3
INFO ok keys=['path'] depth=1
```

Timestamp, level and message are standard regex work. The payload after
`ctx=` is not, because objects and arrays nest inside it.

```culebra
let g = `
  Line  <- Time Level Msg 'ctx=' Obj
  Time  <- < [0-9:T-]+ >
  Level <- < [A-Z]+ >
  Msg   <- < [a-z_]+ >
  Obj   <- '{' Pair (',' Pair)* '}'    { no_ast_opt }
  Pair  <- Key ':' Value
  Key   <- < [a-z_]+ >
  Value <- Obj / Arr / Word
  Arr   <- '[' Value (',' Value)* ']'  { no_ast_opt }
  Word  <- < [^ ,}\]]+ >
  %whitespace <- [ ]*
`
let p = PEG.compile(g)

fn depth(n) {
  match n {
    {name: 'Obj', nodes} => 1 + nodes.map(|kv| depth(kv.nodes[1])).max(),
    {name: 'Arr', nodes} => 1 + nodes.map(depth).max(),
    _ => 0,
  }
}

let log = '2026-09-01T10:00:02 WARN retry ctx={user: {id: 7, tags: [a, b]}, path: /x}
2026-09-01T10:00:05 INFO ok ctx={path: /y}'

for line in log.lines() {
  let n = p.parse(line)
  let ctx = n.nodes[3]
  let keys = ctx.nodes.map(|kv| kv.nodes[0].token)
  println("{n.nodes[1].token} {n.nodes[2].token} keys={keys} depth={depth(ctx)}")
}
# => |
# WARN retry keys=['user', 'path'] depth=3
# INFO ok keys=['path'] depth=1
```

A regex loses this in two separate ways, and it is worth being precise
about both.

It cannot find where the value **ends**. `ctx=\{(.*)\}` is greedy and runs
to the last `}` on the line; `ctx=\{(.*?)\}` is lazy and stops at the
first one, which on line 1 is the `}` closing `{id: 7, tags: [a, b]}` —
so the captured payload is cut in half.

And it cannot tell **which level** a key is on. "List the top-level keys"
has no regex formulation at all: `id` and `tags` look exactly like `user`
and `path` to a pattern with no notion of depth. The grammar has that
notion for free, which is also why `depth` can be six lines.

### 1.2 A type signature

The input is a list of type names — from a symbol dump, or a compiler
diagnostic:

```
Map<String, List<Pair<Int, String>>>
Result<Vec<u8>, Error>
i32
```

and we want the outer type paired with its arguments, split at the
top-level commas and nowhere else:

```
Map <- ['String', 'List<Pair<Int, String>>']
Result <- ['Vec<u8>', 'Error']
i32 <- []
```

That split is the one thing you always want from a nested type, and the
one thing a regex can never do.

```culebra
let g = `
  Type <- Name ('<' Args '>')?  { no_ast_opt }
  Name <- < [A-Za-z_] [A-Za-z0-9_]* >
  Args <- Type (',' Type)*      { no_ast_opt }
  %whitespace <- [ ]*
`
let p = PEG.compile(g)

let lines = 'Map<String, List<Pair<Int, String>>>
Result<Vec<u8>, Error>
i32'

for src in lines.lines() {
  let n = p.parse(src)
  let args = n.nodes.size() > 1 ? n.nodes[1].nodes : []
  let texts = args.map(|a| src.slice(a.position, a.position + a.length))
  println("{n.nodes[0].token} <- {texts}")
}
# => |
# Map <- ['String', 'List<Pair<Int, String>>']
# Result <- ['Vec<u8>', 'Error']
# i32 <- []
```

Take `<(.+)>` and split the capture on `,` and the first line comes apart
as `String`, `List<Pair<Int`, `String>>`. Every deeper comma is a comma to
a regex. `Type` referring to itself through `Args` is the entire fix, and
the grammar is four lines.

Note the last line: `i32` has no arguments at all, and the same grammar
handles it because the `<...>` group is optional. Handling "and sometimes
the pattern is absent" is where a regex sprouts its second alternative and
its third capture group.

### 1.3 A Markdown link

The input is prose with links in it, and both the link text and the URL
happen to contain brackets of their own:

```
See [the [inner] guide](https://x/a(b).md) now.
Also [plain](https://y/z) and [a] alone.
```

We want the text and the URL of each real link, and nothing for the bare
`[a]`:

```
text=the [inner] guide  url=https://x/a(b).md
text=plain  url=https://y/z
```

The familiar `\[([^\]]*)\]\(([^)]*)\)` handles this until exactly those
brackets show up — and Wikipedia URLs contain parentheses constantly.

```culebra
let g = `
  Doc   <- (Link / Other)*        { no_ast_opt }
  Link  <- '[' Text ']' '(' Url ')'
  Text  <- < Inner* >
  Inner <- '[' Inner* ']' / !']' .
  Url   <- < Part* >
  Part  <- '(' Part* ')' / !')' .
  Other <- < . >
`
let p = PEG.compile(g)

let lines = 'See [the [inner] guide](https://x/a(b).md) now.
Also [plain](https://y/z) and [a] alone.'

for line in lines.lines() {
  for n in p.parse(line).nodes {
    if n.name == 'Link' {
      println("text={n.nodes[0].token}  url={n.nodes[1].token}")
    }
  }
}
# => |
# text=the [inner] guide  url=https://x/a(b).md
# text=plain  url=https://y/z
```

`[^\]]*` stops at the first `]`, so the first link comes out as
`the [inner` with a URL of nothing; `[^)]*` truncates the URL at `a(b`.
Both are the same mistake — a character class cannot count — and both are
fixed by one self-referring rule.

This example also shows the other half of line work: `Other <- < . >`
means the grammar describes the *whole* line, not just the interesting
part, so a line can hold any number of links and any amount of prose
between them. `[a]` on the second line is not a link and is simply not
reported.

## 2. Arithmetic: precedence, and two ways to evaluate

Four-function arithmetic is the smallest job that needs real grammar
design, because precedence has to come from the shape of the rules. The
convention is one rule per precedence level, the looser binding one
referring to the tighter one.

Write those rules the way a grammar textbook does and each one comes out
**left-recursive**: `Expr <- Expr AddOp Term / Term`. A PEG normally
cannot do that — a rule that reaches itself without having consumed
anything loops forever, which is why plain PEG grammars are written
around it. cpp-peglib is one of the implementations extended to handle
left recursion (CPython's own PEG parser is another), so here the
textbook form is simply available:

```culebra
let calc = `
  Expr   <- Expr AddOp Term / Term
  Term   <- Term MulOp Factor / Factor
  Factor <- Number / '(' Expr ')'
  AddOp  <- < '+' / '-' >
  MulOp  <- < '*' / '/' >
  Number <- < '-'? [0-9]+ >
  %whitespace <- [ \t\r\n]*
`

fn eval(n) {
  match n {
    {name: 'Number', token} => to_long(token),
    {nodes: [a, op, b]} => match op.token {
      '+' => eval(a) + eval(b),
      '-' => eval(a) - eval(b),
      '*' => eval(a) * eval(b),
      _ => eval(a) / eval(b),
    },
    _ => throw "unexpected {n.name}",
  }
}

inspect(eval(PEG.parse(calc, '1 + 2 * 3')))    # => 7
inspect(eval(PEG.parse(calc, '(1 + 2) * 3')))  # => 9
inspect(eval(PEG.parse(calc, '1 - 2 - 3')))    # => -4
```

`Expr <- Expr AddOp Term / Term` says what it means: an expression is an
expression, an operator and a term — or just a term. The tree comes back
nested to the *left*, which is why `1 - 2 - 3` is `-4` and not `2`, and
each node is a plain three-child triple, so the evaluator is one arm.

The alternative shape is `Expr <- Term (AddOp Term)*`, which avoids the
left recursion and hands you a flat list — `[Term, op, Term, op, Term]` —
that you fold yourself, choosing the associativity as you go. Reach for
that when you actually want the flat list; otherwise the recursive form
is shorter at both ends.

`%whitespace` is the one piece of syntax sugar that keeps a grammar
readable. Without it every rule has to spell out where spaces may appear.

### The same grammar without a tree

When you only want the answer, `actions` interprets the subject directly:
a map from rule name to a function that receives the rule's reduction and
returns what that rule contributes to its parent.

```culebra
let calc = `
  Expr   <- Expr AddOp Term / Term
  Term   <- Term MulOp Factor / Factor
  Factor <- Number / '(' Expr ')'
  AddOp  <- < '+' / '-' >
  MulOp  <- < '*' / '/' >
  Number <- < '-'? [0-9]+ >
  %whitespace <- [ \t\r\n]*
`

fn apply(sv) {
  if sv.values.size() < 3 {
    return sv.values[0]
  }
  match sv.values[1] {
    '+' => sv.values[0] + sv.values[2],
    '-' => sv.values[0] - sv.values[2],
    '*' => sv.values[0] * sv.values[2],
    _ => sv.values[0] / sv.values[2],
  }
}

let actions = {
  Number: |sv| to_long(sv.token),
  AddOp: |sv| sv.token,
  MulOp: |sv| sv.token,
  Expr: apply,
  Term: apply,
}

inspect(PEG.parse(calc, '1 + 2 * 3 - 4', actions: actions))  # => 3
inspect(PEG.parse(calc, '8 / 4 / 2', actions: actions))      # => 1
```

Use the tree when you want to inspect, transform, or walk the input more
than once; use actions when the parse *is* the computation and the tree
would only be built to be thrown away.

## 3. A whole language

Here is the program we are going to run:

```
def fib(x)
  x < 2 ? 1 : fib(x - 2) + fib(x - 1)

for n from 1 to 10
  puts(fib(n))
```

and here is what it should print:

```
[1, 2, 3, 5, 8, 13, 21, 34, 55, 89]
```

The language is [fiblang](https://github.com/yhirose/fiblang), which
exists to write exactly that program and nothing else. It is a good size
to read in one sitting, and it is real: this grammar is one of
cpp-peglib's own test cases. Everything in it is an expression —
including `for`, which is why the program has no statement separator and
the grammar has no statement rule. A function takes one parameter. There
is one comparison operator and two arithmetic ones. That is the whole
language.

The grammar below is fiblang's, with its two error-recovery operators
(`↑` and `%recover`) removed, since those are a separate feature:

```culebra
let grammar = `
  START             <- STATEMENTS
  STATEMENTS        <- (DEFINITION / EXPRESSION)*   { no_ast_opt }
  DEFINITION        <- 'def' Identifier '(' Identifier ')' EXPRESSION
  EXPRESSION        <- TERNARY
  TERNARY           <- CONDITION ('?' EXPRESSION ':' EXPRESSION)?
  CONDITION         <- INFIX (ConditionOperator INFIX)?
  INFIX             <- CALL (InfixOperator CALL)*
  CALL              <- PRIMARY ('(' EXPRESSION ')')?
  PRIMARY           <- FOR / Identifier / '(' EXPRESSION ')' / Number
  FOR               <- 'for' Identifier 'from' Number 'to' Number EXPRESSION

  ConditionOperator <- < '<' >
  InfixOperator     <- < '+' / '-' >
  Identifier        <- !Keyword < [a-zA-Z][a-zA-Z0-9_]* >
  Number            <- < [0-9]+ >
  Keyword           <- 'def' / 'for' / 'from' / 'to'

  %whitespace       <- [ \t\r\n]*
  %word             <- [a-zA-Z]
`
let fiblang = PEG.compile(grammar)

fn run(src) {
  mut fns = {}
  mut out = []

  fn eval(n, env) {
    match n {
      {name: 'Number', token} => to_long(token),

      {name: 'Identifier', token} => env[token],

      {name: 'CONDITION', nodes: [a, op, b]} => eval(a, env) < eval(b, env),

      {name: 'TERNARY', nodes: [c, t, f]} => eval(c, env) ? eval(t, env) : eval(f, env),

      {name: 'INFIX', nodes} => {
        mut acc = eval(nodes[0], env)
        mut i = 1
        while i < nodes.size() {
          let r = eval(nodes[i + 1], env)
          acc = nodes[i].token == '+' ? acc + r : acc - r
          i += 2
        }
        acc
      },

      {name: 'CALL', nodes: [f, arg]} => {
        let v = eval(arg, env)
        if f.token == 'puts' {
          out.push(v)
          v
        } else {
          let d = fns[f.token]
          mut frame = {}
          frame[d.param] = v
          eval(d.body, frame)
        }
      },

      {name: 'FOR', nodes: [name, lo, hi, body]} => {
        for i in to_long(lo.token)..=to_long(hi.token) {
          mut inner = {...env}
          inner[name.token] = i
          eval(body, inner)
        }
        nil
      },

      {name: 'DEFINITION', nodes: [name, param, body]} => {
        fns[name.token] = {param: param.token, body: body}
        nil
      },

      {name: 'STATEMENTS', nodes} => {
        for c in nodes {
          eval(c, env)
        }
        nil
      },
    }
  }

  eval(fiblang.parse(src, 'fib'), {})
  out
}

inspect(run('def fib(x)
  x < 2 ? 1 : fib(x - 2) + fib(x - 1)

for n from 1 to 10
  puts(fib(n))'))
# => [1, 2, 3, 5, 8, 13, 21, 34, 55, 89]
```

Four things in there are worth naming, because they come up in every
language grammar.

**Everything being an expression is what keeps the grammar this small.**
There is no statement rule, no block, no separator, and no `return`,
because `for` and `def` are expressions like any other and a function
body is one expression. Any language feature you add that is *not* an
expression buys itself a rule and an arm.

**Keywords need excluding twice.** `Identifier <- !Keyword < ... >` stops
`for` being read as a variable name. `%word <- [a-zA-Z]` is the other
half: it tells the parser that a literal like `'for'` in the grammar must
end at a word boundary, so a variable named `format` is not read as `for`
followed by `mat`.

**AST optimization does the pruning, so the evaluator only sees rules
that carry information.** `EXPRESSION <- TERNARY` has exactly one child
and disappears entirely; `TERNARY` survives only when a `?` actually
matched, `CONDITION` only when a `<` did, `CALL` only when there were
parentheses. That is why the evaluator has an arm per *concept* rather
than an arm per rule. The exception is `STATEMENTS`, which carries
`{ no_ast_opt }` because a one-statement program must still arrive as a
list rather than as its single statement.

**The flat list again.** `INFIX <- CALL (InfixOperator CALL)*` is §2's
repetition shape, so `a - b - c` arrives as five children and the
evaluator folds it left, in the same six lines §2 used. Written the other
way — `INFIX <- INFIX InfixOperator CALL / CALL` — it would arrive
already nested and the arm would be three children wide. Both are here
so you can see them side by side.

To grow it, add rules. A second parameter is `Identifier (',' Identifier)*`
in `DEFINITION` and a list in `frame`; more operators are entries in
`InfixOperator` and `ConditionOperator` plus a precedence level each, the
way §2 lays them out. If you want to see the shape the tree actually
takes before writing an arm for it, `PEG.str` prints it (§4).

## 4. Pitfalls

**Left recursion works here, but it is an extension.** `Expr <- Expr
AddOp Term / Term` parses, directly or through another rule, and gives
the left-nested tree you want (§2). Remember that this is not PEG as
originally defined: there, a rule that reaches itself without having
consumed anything loops forever, and many implementations still behave
that way. A grammar you carry elsewhere may need rewriting as
`Expr <- Term (AddOp Term)*`.

**Ordered choice is not alternation.** `'a' / 'ab'` never matches `ab`.
Put longer alternatives, and longer literals, first.

**Repetition does not backtrack into itself.** `[a-z]* 'x'` fails on
`abx`, because `*` has already taken the `x`. Say what you mean with a
lookahead: `(!'x' [a-z])* 'x'`.

**Check what AST optimization did before debugging your walker.**
`PEG.str(node)` prints the tree the way `peglint --ast` does, and it is
usually faster than reasoning about which nodes folded:

```culebra
let g = `
  Wrap  <- Inner
  Inner <- < [0-9]+ >
`

print(PEG.str(PEG.parse(g, '1')))
# => |
# - Inner (1)
```

**Leave packrat on unless you have measured.** A grammar whose
alternatives share a prefix — `A <- B '+' A / B`, the shape a first
grammar usually has — re-parses that prefix once per alternative and goes
exponential without memoization. The reference measures ten levels of
nesting at 0.04 ms memoized against 2.0 s not.

**Compile once, parse many.** `PEG.compile` is the expensive half. The
one-shot `PEG.parse(grammar, text)` caches loaded grammars per thread so
it is not a trap, but a loop over many subjects reads better with the
grammar hoisted out of it.

## Where to go next

* [`stdlib.md` §34](../stdlib.md#34-peg) — every call, the node fields,
  the error and depth bounds.
* [cpp-peglib's syntax reference](https://github.com/yhirose/cpp-peglib#syntax)
  — the full notation, including the parts this guide did not need.
* [Practical PEG parsing](https://berthub.eu/articles/posts/practical-peg-parsing/)
  by Bert Hubert — the same argument from C++, and a worked Prometheus
  metrics parser. His challenge is a fair test of whether a grammar earns
  its place: try writing the hand-rolled version, and time yourself.
