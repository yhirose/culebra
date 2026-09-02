# Parsing with PEG

A guide to the `Peg` namespace: reaching for a grammar where a regex runs
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
build step: `Peg.compile` loads a grammar at run time, and the grammar is
an ordinary string in your program. And choice is **ordered** — `a / b`
commits to `a` the moment `a` matches, rather than reporting a
shift/reduce conflict for you to resolve — which means a PEG is
unambiguous by construction and reads top to bottom in the order it is
written. Left recursion, which plain PEG cannot express, works here: the
engine is [cpp-peglib](https://github.com/yhirose/cpp-peglib), the same
parser culebra's own front end runs on, and it supports it.

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
let p = Peg.compile(g)

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
let p = Peg.compile(g)

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
let p = Peg.compile(g)

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
referring to the tighter one — and each of those rules is left-recursive,
which is exactly how the textbook writes it:

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

inspect(eval(Peg.parse(calc, '1 + 2 * 3')))    # => 7
inspect(eval(Peg.parse(calc, '(1 + 2) * 3')))  # => 9
inspect(eval(Peg.parse(calc, '1 - 2 - 3')))    # => -4
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

inspect(Peg.parse(calc, '1 + 2 * 3 - 4', actions: actions))  # => 3
inspect(Peg.parse(calc, '8 / 4 / 2', actions: actions))      # => 1
```

Use the tree when you want to inspect, transform, or walk the input more
than once; use actions when the parse *is* the computation and the tree
would only be built to be thrown away.

## 3. A small language

Everything above scales to a real interpreter without changing method. The
language below has integer variables, arithmetic, comparison, `if`/`else`,
`while`, functions with parameters, and recursion — enough to write
something you would recognise as a program. Grammar and evaluator together
are about 130 lines.

```culebra
let grammar = `
  Program  <- Stmt+                              { no_ast_opt }
  Stmt     <- Fn / Return / If / While / Assign / ExprStmt
  Fn       <- 'fn' Ident '(' Params ')' Block    { no_ast_opt }
  Params   <- (Ident (',' Ident)*)?              { no_ast_opt }
  Block    <- '{' Stmt* '}'                      { no_ast_opt }
  Return   <- 'return' Expr ';'                  { no_ast_opt }
  If       <- 'if' Expr Block Else?              { no_ast_opt }
  Else     <- 'else' Block
  While    <- 'while' Expr Block                 { no_ast_opt }
  Assign   <- Ident '=' Expr ';'                 { no_ast_opt }
  ExprStmt <- Expr ';'                           { no_ast_opt }

  Expr     <- Cmp
  Cmp      <- Add CmpOp Add / Add
  Add      <- Add AddOp Mul / Mul
  Mul      <- Mul MulOp Unary / Unary
  Unary    <- Neg / Primary
  Neg      <- '-' Unary                          { no_ast_opt }
  Primary  <- Call / Number / Ident / '(' Expr ')'
  Call     <- Ident '(' Args ')'                 { no_ast_opt }
  Args     <- (Expr (',' Expr)*)?                { no_ast_opt }

  CmpOp    <- < '==' / '!=' / '<=' / '>=' / '<' / '>' >
  AddOp    <- < '+' / '-' >
  MulOp    <- < '*' / '/' / '%' >
  Number   <- < [0-9]+ >
  Ident    <- < !Keyword [a-z_] [a-zA-Z0-9_]* >
  Keyword  <- ('fn' / 'return' / 'if' / 'else' / 'while') ![a-zA-Z0-9_]

  %whitespace <- ([ \t\r\n] / '#' (!'\n' .)*)*
`
let lang = Peg.compile(grammar)

fn lookup(scopes, name) {
  mut i = scopes.size() - 1
  while i >= 0 {
    if scopes[i].has(name) {
      return scopes[i][name]
    }
    i -= 1
  }
  throw "undefined variable '{name}'"
}

fn store(scopes, name, v) {
  mut i = scopes.size() - 1
  while i >= 0 {
    if scopes[i].has(name) {
      scopes[i][name] = v
      return nil
    }
    i -= 1
  }
  scopes[scopes.size() - 1][name] = v
}

fn binop(op, a, b) {
  match op {
    '+' => a + b,
    '-' => a - b,
    '*' => a * b,
    '/' => a / b,
    '%' => a % b,
    '==' => a == b,
    '!=' => a != b,
    '<' => a < b,
    '<=' => a <= b,
    '>' => a > b,
    _ => a >= b,
  }
}

fn run(src) {
  let tree = lang.parse(src, 'program')
  mut fns = {}
  mut out = []

  fn eval(n, scopes) {
    match n {
      {name: 'Number', token} => to_long(token),
      {name: 'Ident', token} => lookup(scopes, token),
      {name: 'Neg', nodes: [x]} => -eval(x, scopes),
      {name: 'Call', nodes: [f, args]} => call(f.token, args.nodes.map(|arg| eval(arg, scopes))),
      {nodes: [a, op, b]} => binop(op.token, eval(a, scopes), eval(b, scopes)),
    }
  }

  fn call(name, args) {
    if name == 'print' {
      out.push(args[0])
      return nil
    }
    if !fns.has(name) {
      throw "undefined function '{name}'"
    }
    let f = fns[name]
    mut frame = {}
    mut i = 0
    while i < f.params.size() {
      frame[f.params[i]] = args[i]
      i += 1
    }
    match exec_block(f.body, [frame]) {
      {ret} => ret,
      _ => nil,
    }
  }

  fn exec_block(block, scopes) {
    for st in block.nodes {
      let r = exec(st, scopes)
      if r != nil {
        return r
      }
    }
    nil
  }

  fn exec(n, scopes) {
    match n {
      {name: 'Fn', nodes: [name, params, body]} => {
        fns[name.token] = {params: params.nodes.map(|prm| prm.token), body: body}
        nil
      },
      {name: 'Assign', nodes: [name, e]} => {
        store(scopes, name.token, eval(e, scopes))
        nil
      },
      {name: 'Return', nodes: [e]} => {ret: eval(e, scopes)},
      {name: 'If', nodes} => {
        if eval(nodes[0], scopes) {
          exec_block(nodes[1], scopes)
        } else if nodes.size() > 2 {
          exec_block(nodes[2], scopes)
        } else {
          nil
        }
      },
      {name: 'While', nodes: [cond, body]} => {
        mut r = nil
        while r == nil && eval(cond, scopes) {
          r = exec_block(body, scopes)
        }
        r
      },
      {name: 'ExprStmt', nodes: [e]} => {
        eval(e, scopes)
        nil
      },
    }
  }

  exec_block(tree, [{}])
  out
}

inspect(run('
  # naive recursion, and a loop that uses it
  fn fib(n) {
    if n < 2 { return n; }
    return fib(n - 1) + fib(n - 2);
  }

  i = 0;
  while i < 10 {
    print(fib(i));
    i = i + 1;
  }
'))
# => [0, 1, 1, 2, 3, 5, 8, 13, 21, 34]
```

Four decisions in there are worth naming, because they are the ones that
recur in every language grammar.

**Keywords have to be excluded from identifiers.** `Ident <- < !Keyword
[a-z_] [a-zA-Z0-9_]* >` — without the lookahead, `Primary <- ... / Ident`
happily matches `else` as a variable name, and a parse that should have
stopped instead succeeds with the wrong shape. `Keyword` ends with
`![a-zA-Z0-9_]` so that `iffy` is still an ordinary identifier.

**`no_ast_opt` where the node's own name carries meaning.** A node with a
single child is replaced by that child, which is what you want almost
everywhere — it keeps the tree about structure instead of bookkeeping. It
is wrong exactly where the rule *is* the information: a `Block` with one
statement must stay a `Block`, and a `Return` must not collapse into the
expression it returns. It is also wrong for any rule whose parent reads
children by position, since folding shifts the positions.

**A statement returns a signal, not a value.** `exec` returns `nil` for
"kept going" and an object for "a `return` happened", and `exec_block`
stops on the first non-`nil`. That is the whole of non-local control flow
here; a language with `break` would add another shape to the same channel.

**Scopes are a list, searched from the end.** `store` assigns to an
existing binding wherever it is found and otherwise creates one in the
innermost scope; a function call pushes a fresh frame rather than nesting
inside the caller's, which is what makes the recursion work.

Extending it is mostly grammar. Strings are a `Str` rule and a case in
`eval`; `else if` chains fall out of making `Else <- 'else' (If / Block)`;
first-class functions need `call` to take a value instead of a name.

## 4. Pitfalls

**Left recursion works — but plain PEG says it should not.** `Expr <-
Expr AddOp Term / Term` is fine here, both directly and through another
rule, and it gives you the left-nested tree you want (§2). It is worth
knowing that this is cpp-peglib going beyond textbook PEG, in which a
rule that reaches itself without consuming anything simply loops: a
grammar you carry to another PEG library may need rewriting as
`Expr <- Term (AddOp Term)*`.

**Ordered choice is not alternation.** `'a' / 'ab'` never matches `ab`.
Put longer alternatives, and longer literals, first.

**Repetition does not backtrack into itself.** `[a-z]* 'x'` fails on
`abx`, because `*` has already taken the `x`. Say what you mean with a
lookahead: `(!'x' [a-z])* 'x'`.

**Check what AST optimization did before debugging your walker.**
`Peg.str(node)` prints the tree the way `peglint --ast` does, and it is
usually faster than reasoning about which nodes folded:

```culebra
let g = `
  Wrap  <- Inner
  Inner <- < [0-9]+ >
`

print(Peg.str(Peg.parse(g, '1')))
# => |
# - Inner (1)
```

**Leave packrat on unless you have measured.** A grammar whose
alternatives share a prefix — `A <- B '+' A / B`, the shape a first
grammar usually has — re-parses that prefix once per alternative and goes
exponential without memoization. The reference measures ten levels of
nesting at 0.04 ms memoized against 2.0 s not.

**Compile once, parse many.** `Peg.compile` is the expensive half. The
one-shot `Peg.parse(grammar, text)` caches loaded grammars per thread so
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
