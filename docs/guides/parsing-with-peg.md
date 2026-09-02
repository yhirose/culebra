# Parsing with PEG

A guide to the `Peg` namespace: reaching for a grammar where a regex runs
out, designing a small language of your own, and writing an interpreter
for one. Every code block here runs — they are executed as doctests on
both engines.

The API reference is [`stdlib.md` §34](../stdlib.md#34-peg). This guide is
about *when* to use it and how to shape a grammar; the reference is about
what each call does.

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

## 1. Search and replace a regex cannot do

These are the jobs where a regex either fails outright or turns into
something nobody wants to maintain. The shape of the solution is the same
each time: describe the *whole* text with a grammar — including the parts
you do not care about — and then walk the pieces.

### 1.1 Change a name, but not inside strings and comments

The classic rename. A regex has no way to know whether a match sits inside
a string literal, so `s/total/sum/g` corrupts data and comments. The
grammar knows, because strings and comments are rules of their own and the
ordered choice tries them first:

```culebra
let g = `
  Doc     <- Chunk*                        { no_ast_opt }
  Chunk   <- Str / Comment / Ident / Other
  Str     <- < '"' ( '\\' . / !'"' . )* '"' >
  Comment <- < '#' (!'\n' .)* >
  Ident   <- < [a-zA-Z_] [a-zA-Z0-9_]* >
  Other   <- < . >
`
fn rename(src, from, to) {
  mut out = ''
  for n in Peg.parse(g, src).nodes {
    out = out + (n.name == 'Ident' && n.token == from ? to : n.token)
  }
  out
}
let src = 'total = total + 1  # total, in a comment
label = "total, in a string"'
println(rename(src, 'total', 'sum'))
# => |
# sum = sum + 1  # total, in a comment
# label = "total, in a string"
```

`Other <- < . >` is the rule that makes this work: every byte of the
subject belongs to some chunk, so re-joining the tokens reproduces the
input exactly except where you change it. A grammar that only described
the interesting parts would need a separate mechanism to carry the gaps.

### 1.2 Nesting — the thing a regex cannot count

Block comments that nest are the standard counterexample to regular
expressions. `/\*.*?\*/` stops at the first `*/`, and `/\*.*\*/` swallows
everything to the last one; neither is right, and no amount of cleverness
fixes it, because counting is outside what a regular language can do. A
rule that mentions itself has no such limit:

```culebra
let g = `
  Doc     <- Piece*                { no_ast_opt }
  Piece   <- Comment / Text
  Comment <- '/*' ( Comment / !'*/' . )* '*/'
  Text    <- < ( !'/*' . )+ >
`
fn strip(src) {
  mut out = ''
  for n in Peg.parse(g, src).nodes {
    if n.name == 'Text' {
      out = out + n.token
    }
  }
  out
}
println(strip('a /* one /* nested */ still a comment */ b'))  # => a  b
```

`Comment <- '/*' ( Comment / !'*/' . )* '*/'` reads as the definition
does: an opener, then any number of *either* a whole nested comment or a
character that does not begin the closer, then the closer.

### 1.3 Escapes

The other place regexes get fragile. `"([^"]*)"` breaks on `"a\"b"`, and
the fixed version is write-only. As a grammar it is two alternatives in the
order you would say them out loud — a backslash takes whatever follows it,
otherwise take any character that is not the closing quote:

```culebra
let g = `
  Str  <- '"' < Char* > '"'
  Char <- '\\' . / !'"' .
`
println(Peg.compile(g).parse('"a\"b\\c"').token)  # => a\"b\\c
```

The order of those two alternatives is the whole trick. Swap them and
`!'"' .` consumes the backslash on its own, the next character is read as
ordinary, and an escaped quote ends the string early. This is Hubert's
point about ordered choice being semantic rather than a formality.

### 1.4 Composing rules: balanced parentheses that know about strings

Here is what "rules compose" buys you. Pulling the argument list out of a
call needs balanced parentheses — already impossible for a regex — and
real input has parentheses inside string literals that must not count.
Adding that is one alternative in one rule:

```culebra
let g = `
  Call     <- Name '(' Args ')'
  Name     <- < [a-zA-Z_] [a-zA-Z0-9_]* >
  Args     <- < Balanced >
  Balanced <- ( Str / '(' Balanced ')' / !')' . )*
  Str      <- '"' ( '\\' . / !'"' . )* '"'
`
let node = Peg.compile(g).parse('f(g(1, h(2)), "x)y")')
println(node.nodes[0].token)  # => f
println(node.nodes[1].token)  # => g(1, h(2)), "x)y"
```

The `)` inside `"x)y"` did not close anything, because `Str` is tried
first and consumes the whole literal in one step.

### 1.5 A structural rewrite

Put those together and you can do refactors that are not text substitution
at all. This one turns `f(a, b)` into `a.f(b)` — the arguments may be
nested calls, and may contain commas and parentheses inside strings:

```culebra
let g = `
  Doc    <- Piece*                          { no_ast_opt }
  Piece  <- Call / Str / Other
  Call   <- Name '(' Arg (',' Arg)* ')'
  Name   <- < [a-zA-Z_] [a-zA-Z0-9_]* >
  Arg    <- < ( Str / '(' Nested ')' / [^,()] )+ >
  Nested <- ( Str / '(' Nested ')' / [^()] )*
  Str    <- < '"' ( '\\' . / !'"' . )* '"' >
  Other  <- < . >
`
fn to_ufcs(src) {
  mut out = ''
  for n in Peg.parse(g, src).nodes {
    match n {
      {name: 'Call', nodes} => {
        let args = nodes.slice(1, nodes.size()).map(|x| x.token.trim())
        let rest = args.slice(1, args.size()).join(', ')
        out = out + "{args[0]}.{nodes[0].token}({rest})"
      },
      _ => out = out + n.token,
    }
  }
  out
}
println(to_ufcs('log(fmt("a,b", x), 2) and keep("f(1, 2)")'))
# => fmt("a,b", x).log(2) and "f(1, 2)".keep()
```

Note what was *not* rewritten: the `f(1, 2)` inside the string. Try
expressing that condition in a regex.

When you would rather edit the original text than rebuild it, every node
carries `position` and `length`, so `src.slice(n.position, n.position +
n.length)` is that node's exact source and you can splice around it.

## 2. Arithmetic: precedence, and two ways to evaluate

Four-function arithmetic is the smallest job that needs real grammar
design, because precedence has to come from the shape of the rules. The
convention is one rule per precedence level, the looser binding one
referring to the tighter one:

```culebra
let calc = `
  Expr   <- Term (AddOp Term)*
  Term   <- Factor (MulOp Factor)*
  Factor <- Number / '(' Expr ')'
  AddOp  <- < '+' / '-' >
  MulOp  <- < '*' / '/' >
  Number <- < '-'? [0-9]+ >
  %whitespace <- [ \t\r\n]*
`
fn eval(n) {
  match n {
    {name: 'Number', token} => to_long(token),
    {nodes} => {
      mut acc = eval(nodes[0])
      mut i = 1
      while i < nodes.size() {
        let r = eval(nodes[i + 1])
        acc = match nodes[i].token {
          '+' => acc + r,
          '-' => acc - r,
          '*' => acc * r,
          _ => acc / r,
        }
        i += 2
      }
      acc
    },
  }
}
inspect(eval(Peg.parse(calc, '1 + 2 * 3')))    # => 7
inspect(eval(Peg.parse(calc, '(1 + 2) * 3')))  # => 9
```

`Expr <- Term (AddOp Term)*` gives a flat list — `[Term, op, Term, op,
Term]` — which is why the evaluator walks it in steps of two and folds
left. That associativity is a choice you are making: writing the rule as
`Term AddOp Expr` instead would nest to the right, and `1 - 2 - 3` would
come out `2` rather than `-4`.

`%whitespace` is the one piece of syntax sugar that keeps a grammar
readable. Without it every rule has to spell out where spaces may appear.

### The same grammar without a tree

When you only want the answer, `actions` interprets the subject directly:
a map from rule name to a function that receives the rule's reduction and
returns what that rule contributes to its parent.

```culebra
let calc = `
  Expr   <- Term (AddOp Term)*
  Term   <- Factor (MulOp Factor)*
  Factor <- Number / '(' Expr ')'
  AddOp  <- < '+' / '-' >
  MulOp  <- < '*' / '/' >
  Number <- < '-'? [0-9]+ >
  %whitespace <- [ \t\r\n]*
`
fn fold(sv) {
  mut acc = sv.values[0]
  mut i = 1
  while i < sv.values.size() {
    let r = sv.values[i + 1]
    acc = match sv.values[i] {
      '+' => acc + r,
      '-' => acc - r,
      '*' => acc * r,
      _ => acc / r,
    }
    i += 2
  }
  acc
}
let actions = {
  Number: |sv| to_long(sv.token),
  AddOp: |sv| sv.token,
  MulOp: |sv| sv.token,
  Expr: fold,
  Term: fold,
}
inspect(Peg.parse(calc, '1 + 2 * 3 - 4', actions: actions))  # => 3
```

Use the tree when you want to inspect, transform, or walk the input more
than once; use actions when the parse *is* the computation and the tree
would only be built to be thrown away.

## 3. Designing a small DSL

A DSL earns its keep when the alternative is a configuration format that
grows conditionals. Here is a filter language — the kind of thing behind a
search box — over a list of records.

Design it by writing the sentences you want first:

    status = open and priority >= 2
    priority = 1 or status = open

then read the precedence off them: `or` binds loosest, then `and`, then a
single comparison. That is the same ladder as the arithmetic above.

```culebra
let grammar = `
  Query  <- Or
  Or     <- And ('or' And)*
  And    <- Cmp ('and' Cmp)*
  Cmp    <- Field Op Value
  Field  <- < [a-z_]+ >
  Op     <- < '>=' / '<=' / '!=' / '=' / '>' / '<' >
  Value  <- Number / Word
  Number <- < '-'? [0-9]+ >
  Word   <- < [a-zA-Z_] [a-zA-Z0-9_]* >
  %whitespace <- [ \t]*
`
let q = Peg.compile(grammar)

fn compare(op, a, b) {
  match op {
    '=' => a == b,
    '!=' => a != b,
    '<' => a < b,
    '<=' => a <= b,
    '>' => a > b,
    _ => a >= b,
  }
}
fn eval(n, row) {
  match n {
    {name: 'Or', nodes} => nodes.any(|c| eval(c, row)),
    {name: 'And', nodes} => nodes.all(|c| eval(c, row)),
    {name: 'Cmp', nodes: [f, op, v]} => {
      if !row.has(f.token) {
        throw "no field named '{f.token}'"
      }
      compare(op.token, row[f.token], v.name == 'Number' ? to_long(v.token) : v.token)
    },
  }
}

let rows = [
  {title: 'write guide', status: 'open', priority: 3},
  {title: 'fix build', status: 'done', priority: 1},
  {title: 'read paper', status: 'open', priority: 1},
]
fn select(rows, text) {
  let tree = q.parse(text, 'filter')
  rows.filter(|r| eval(tree, r)).map(|r| r.title)
}
inspect(select(rows, 'status = open and priority >= 2'))  # => ['write guide']
inspect(select(rows, 'priority = 1 or status = open'))
# => ['write guide', 'fix build', 'read paper']
```

Two things worth copying from this grammar.

**The operator order is load-bearing.** `Op <- < '>=' / '<=' / '!=' / '='
/ '>' / '<' >` lists the two-character operators first. Ordered choice
takes the first alternative that matches, so with `'>'` ahead of `'>='`
the input `>=` matches `>` and the parse then fails on a stray `=`. The
rule of thumb: **longer literals before their own prefixes**.

**Name the subject when you parse it.** `q.parse(text, 'filter')` puts
that name in the error, so a user's typo comes back as a diagnostic
rather than a bare offset:

```culebra
let q = Peg.compile(`
  Query  <- Cmp
  Cmp    <- Field Op Value
  Field  <- < [a-z_]+ >
  Op     <- < '>=' / '<=' / '!=' / '=' / '>' / '<' >
  Value  <- < [a-zA-Z0-9_]+ >
  %whitespace <- [ \t]*
`)
inspect(try { q.parse('status =', 'filter') } catch e { e.message })
# => 'filter:1:9: syntax error, expecting <Value>.'
```

## 4. A small language

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
  Cmp      <- Add (CmpOp Add)?
  Add      <- Mul (AddOp Mul)*
  Mul      <- Unary (MulOp Unary)*
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
      {name: 'Cmp', nodes: [a, op, b]} => binop(op.token, eval(a, scopes), eval(b, scopes)),
      {nodes} => fold(nodes, scopes),
    }
  }
  fn fold(nodes, scopes) {
    mut acc = eval(nodes[0], scopes)
    mut i = 1
    while i < nodes.size() {
      acc = binop(nodes[i].token, acc, eval(nodes[i + 1], scopes))
      i += 2
    }
    acc
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

## 5. Pitfalls

**Left recursion is not allowed.** `Expr <- Expr '+' Term` never
terminates, because the parser tries `Expr` again before consuming
anything. Write repetition instead — `Expr <- Term ('+' Term)*` — and fold
the list yourself, as in §2. This is the one habit that transfers badly
from yacc-style grammars.

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
