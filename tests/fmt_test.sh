#!/usr/bin/env bash
# Regression test for `culebra fmt` (include/formatter.h). Three checks:
#   1. Golden fixture: a messy comment-bearing input formats to an exact
#      expected output (operator spacing, blocks, leading/trailing comments).
#   2. Corpus safety: every real `.cul` under tests/ and examples/ formats
#      without the safety net refusing (exit 2) — i.e. the re-parse AST check
#      and the comment-preservation check both pass.
#   3. Idempotency: formatting the output again is a byte-for-byte fixed point.
# Usage: fmt_test.sh <path-to-culebra>
set -u
CULEBRA="${1:?usage: fmt_test.sh <culebra-binary>}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="${TMPDIR:-/tmp}/culebra_fmt_test.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT
fail=0

# --- 1. Golden fixture (comments preserved) -------------------------------
cat > "$TMP/in.cul" <<'EOF'
# header comment
let   x=1+2*3   # trailing
fn add(a,b){
  # leading inside block
  return a+b
}



if x>0{inspect("hi")}else{inspect("lo")}
EOF
cat > "$TMP/want.cul" <<'EOF'
# header comment
let x = 1 + 2 * 3  # trailing
fn add(a, b) {
  # leading inside block
  return a + b
}

if x > 0 {
  inspect("hi")
} else {
  inspect("lo")
}
EOF
"$CULEBRA" fmt "$TMP/in.cul" > "$TMP/got.cul" 2>"$TMP/err"
if ! diff -u "$TMP/want.cul" "$TMP/got.cul" > "$TMP/diff" 2>&1; then
  echo "FAIL golden: formatted output differs from expected"
  cat "$TMP/diff"
  fail=1
fi

# --- 1b. Golden fixture: bare lexical-scope blocks (LEXICAL_SCOPE) ---------
# A `{ ... }` statement block nested inside a fn/loop body must keep its braces
# and reformat its interior (comments included). The optimizer folds the
# enclosing body's `{` onto the scope, which once made fmt strip the inner
# braces (safety net refused) — see include/formatter.h print_lexical_scope.
cat > "$TMP/blk_in.cul" <<'EOF'
fn g() {
  // before block
  {
    let r=1 // trailing
    // standalone
    let s=2
  }
}
for i in 0..3 {
  {
    let a = i
  }
}
EOF
cat > "$TMP/blk_want.cul" <<'EOF'
fn g() {
  // before block
  {
    let r = 1  // trailing
    // standalone
    let s = 2
  }
}
for i in 0..3 {
  {
    let a = i
  }
}
EOF
"$CULEBRA" fmt "$TMP/blk_in.cul" > "$TMP/blk_got.cul" 2>"$TMP/blk_err"
if ! diff -u "$TMP/blk_want.cul" "$TMP/blk_got.cul" > "$TMP/blk_diff" 2>&1; then
  echo "FAIL golden (bare block): formatted output differs from expected"
  cat "$TMP/blk_diff"
  fail=1
fi

# --- 1c. Golden fixture: same-precedence parentheses -----------------------
# The grammar folds a run of same-precedence operators into ONE flat node, so
# `(a * b) / c` and `a * b / c` mean the same thing but are different trees.
# Associativity once made fmt drop those parentheses, which reshaped the AST
# and made the re-parse safety net refuse the whole file — see
# include/formatter.h print_operand.
cat > "$TMP/par_in.cul" <<'EOF'
let a=1
let b=2
let c=3
let p = (a*b)/c
let q = a+(b*c)*a
let r = (a-b)-c
let s = a**(b**c)
let t = a*(b*c)/a
let u = a*b/c
EOF
cat > "$TMP/par_want.cul" <<'EOF'
let a = 1
let b = 2
let c = 3
let p = (a * b) / c
let q = a + (b * c) * a
let r = (a - b) - c
let s = a ** (b ** c)
let t = a * (b * c) / a
let u = a * b / c
EOF
"$CULEBRA" fmt "$TMP/par_in.cul" > "$TMP/par_got.cul" 2>"$TMP/par_err"
if ! diff -u "$TMP/par_want.cul" "$TMP/par_got.cul" > "$TMP/par_diff" 2>&1; then
  echo "FAIL golden (same-precedence parens): formatted output differs from expected"
  cat "$TMP/par_diff"
  fail=1
fi

# --- 1d. Golden fixture: comments inside brace literals --------------------
# An object / set literal opens a brace pair of its own, so a comment written
# inside one belongs to that pair — not to the statement around it, which has
# no slot for it and once made the safety net refuse the WHOLE file. Such a
# literal lays out broken, one element per line, with its comments attached the
# way a block's statements get theirs; a literal without comments keeps the
# compact form. A comment anywhere inside forces the break, nested literals
# included, since a Doc that hard-breaks may not sit inside a group.
# See include/formatter.h print_brace_literal.
cat > "$TMP/brc_in.cul" <<'EOF'
let  o = {a: 1,
  # inside an object literal
  b: 2}
let  s = {1,
  // inside a set literal
  2}
let  plain={a:1,b:2}
let  lead={ # before the first key
  k: 1}
let  tail={k: 1
  # after the last value
}
let  nest={x: {y: 1,
  # only the inner one has it
  z: 2}, w: 3}
inspect(o)
inspect(s)
inspect(plain)
inspect(lead)
inspect(tail)
inspect(nest)
EOF
cat > "$TMP/brc_want.cul" <<'EOF'
let o = {
  a: 1,
  # inside an object literal
  b: 2,
}
let s = {
  1,
  // inside a set literal
  2,
}
let plain = {a: 1, b: 2}
let lead = {
  # before the first key
  k: 1,
}
let tail = {
  k: 1,
  # after the last value
}
let nest = {
  x: {
    y: 1,
    # only the inner one has it
    z: 2,
  },
  w: 3,
}
inspect(o)
inspect(s)
inspect(plain)
inspect(lead)
inspect(tail)
inspect(nest)
EOF
"$CULEBRA" fmt "$TMP/brc_in.cul" > "$TMP/brc_got.cul" 2>"$TMP/brc_err"
if ! diff -u "$TMP/brc_want.cul" "$TMP/brc_got.cul" > "$TMP/brc_diff" 2>&1; then
  echo "FAIL golden (brace-literal comments): formatted output differs from expected"
  cat "$TMP/brc_diff"
  fail=1
fi

# --- 1e. Golden fixture: comment above a lone bare-literal statement -------
# A block whose single statement is a bare literal collapses onto that leaf, so
# the leaf's span carries the block's `{ }` AND any comment inside it.
# tight_span sheds the brackets but used to leave the comment in the literal —
# while the enclosing statement list emitted it too, so it appeared twice and
# the preservation net refused the file. Only bare-atom bodies hit this: a
# `return`/`let`/call/binary statement is a node the block can't collapse onto.
# See include/formatter.h tight_span.
cat > "$TMP/atom_in.cul" <<'EOF'
fn f() {
  # why the literal
  1
}
let g = fn () {
  # lambda body
  "hi"
}
if true {
  # branch
  2
}
fn t() {
  3   # trailing
}
inspect(f())
inspect(g())
inspect(t())
EOF
cat > "$TMP/atom_want.cul" <<'EOF'
fn f() {
  # why the literal
  1
}
let g = fn () {
  # lambda body
  "hi"
}
if true {
  # branch
  2
}
fn t() {
  3  # trailing
}
inspect(f())
inspect(g())
inspect(t())
EOF
"$CULEBRA" fmt "$TMP/atom_in.cul" > "$TMP/atom_got.cul" 2>"$TMP/atom_err"
if ! diff -u "$TMP/atom_want.cul" "$TMP/atom_got.cul" > "$TMP/atom_diff" 2>&1; then
  echo "FAIL golden (bare-literal body comment): formatted output differs from expected"
  cat "$TMP/atom_diff"
  fail=1
fi

# --- 1f. Golden fixture: postfix `if`/`unless` statement modifiers --------
# `stmt if cond` / `stmt unless cond` is its own one-line canonical shape
# (parse_for_format keeps the modifier node intact — see
# include/parser.h view_postfix_modifier — unlike a full `if cond { ... }`
# block, which always expands to multiple lines regardless of source form).
cat > "$TMP/mod_in.cul" <<'EOF'
fn f(v) {
  return    "small"    if v<10
  return "big" unless   v<100
  "medium"
}
break   if x>3
continue unless x==2
EOF
cat > "$TMP/mod_want.cul" <<'EOF'
fn f(v) {
  return "small" if v < 10
  return "big" unless v < 100
  "medium"
}
break if x > 3
continue unless x == 2
EOF
"$CULEBRA" fmt "$TMP/mod_in.cul" > "$TMP/mod_got.cul" 2>"$TMP/mod_err"
if ! diff -u "$TMP/mod_want.cul" "$TMP/mod_got.cul" > "$TMP/mod_diff" 2>&1; then
  echo "FAIL golden (postfix if/unless): formatted output differs from expected"
  cat "$TMP/mod_diff"
  fail=1
fi

# --- 1g. Golden fixture: 1-element Set keeps its trailing comma -----------
# `{a,}` is the only spelling of a 1-element Set (`{a}` doesn't parse; `{}`
# is the empty Object), so print_set must keep the comma the same way a
# 1-tuple keeps `(a,)`. Dropping it turned the literal into a SyntaxError,
# which made the safety net refuse the whole file.
cat > "$TMP/set1_in.cul" <<'EOF'
let one={42,}
let two={1,2}
let s={ 'x' ,}
EOF
cat > "$TMP/set1_want.cul" <<'EOF'
let one = {42,}
let two = {1, 2}
let s = {'x',}
EOF
"$CULEBRA" fmt "$TMP/set1_in.cul" > "$TMP/set1_got.cul" 2>"$TMP/set1_err"
if ! diff -u "$TMP/set1_want.cul" "$TMP/set1_got.cul" > "$TMP/set1_diff" 2>&1; then
  echo "FAIL golden (1-element set): formatted output differs from expected"
  cat "$TMP/set1_diff"
  fail=1
fi

# --- 1h. Golden fixture: a comment inside a keyword statement's own block --
# A statement falls back to a verbatim slice only when it owns a comment its
# printer has no slot for. A comment among the statements of the block the
# statement itself opens is NOT that: the block's printer places it. `try` and
# `catch` are keywords the AST keeps no token for, so the statement's first
# token sits inside its own braces — which once made a depth-counting test read
# such a comment as belonging to the statement, freeze the whole `try` verbatim,
# and preserve a one-line `catch` body that the canonical style always expands.
# See include/formatter.h owner_.
cat > "$TMP/own_in.cul" <<'EOF'
try {
  work()  // why
  more()
} catch e { handle(e) }
try {
  work()
} catch e { handle(e) }
while ready() {
  // note
  step()
}
EOF
cat > "$TMP/own_want.cul" <<'EOF'
try {
  work()  // why
  more()
} catch e {
  handle(e)
}
try {
  work()
} catch e {
  handle(e)
}
while ready() {
  // note
  step()
}
EOF
"$CULEBRA" fmt "$TMP/own_in.cul" > "$TMP/own_got.cul" 2>"$TMP/own_err"
if ! diff -u "$TMP/own_want.cul" "$TMP/own_got.cul" > "$TMP/own_diff" 2>&1; then
  echo "FAIL golden (comment in a keyword statement's block): output differs"
  cat "$TMP/own_diff"
  fail=1
fi

# --- 1i. Golden fixture: a bracket written inside a comment or a string ----
# Shedding the parentheses the optimizer folded onto a collapsed node needs to
# know the pair encloses the whole span, which means matching brackets — and a
# `)` inside a comment or a string is not one. Counting it left the pair
# looking unbalanced, so the parentheses stayed on and dragged the comment into
# the leaf's text while the enclosing block emitted it too, refusing the file.
# See include/formatter.h encloses_whole.
cat > "$TMP/brk_in.cul" <<'EOF'
fn f() { ( # )
  1 ) }
fn g() { ( ")" ) }
fn h() { ( "]" ) }
inspect(f())
inspect(g())
inspect(h())
EOF
cat > "$TMP/brk_want.cul" <<'EOF'
fn f() {
  # )
  1
}
fn g() {
  ")"
}
fn h() {
  "]"
}
inspect(f())
inspect(g())
inspect(h())
EOF
"$CULEBRA" fmt "$TMP/brk_in.cul" > "$TMP/brk_got.cul" 2>"$TMP/brk_err"
if ! diff -u "$TMP/brk_want.cul" "$TMP/brk_got.cul" > "$TMP/brk_diff" 2>&1; then
  echo "FAIL golden (bracket inside a comment/string): output differs"
  cat "$TMP/brk_diff"
  fail=1
fi

# --- 1j. Golden fixture: a list whose block isn't the last element ---------
# A `fn` body (or any block expression) hard-breaks, and a Group may not wrap a
# hardline: doc_fits reports "fits" the moment it reaches one, so everything
# after it renders as if flat and lands glued to the block's closing brace, one
# indent level too deep. A list keeps the block hugged only while at most one
# element follows it; anything else lays out one element per line.
# See include/formatter.h print_delimited.
cat > "$TMP/blk2_in.cul" <<'EOF'
run(1, 2, fn () {
  true
}, frames: 3)
spawn(fn () {
  1
}, 1, 2)
let m = {a: fn () {
  1
}, b: fn () {
  2
}, c: 3}
EOF
cat > "$TMP/blk2_want.cul" <<'EOF'
run(1, 2, fn () {
  true
}, frames: 3)
spawn(
  fn () {
    1
  },
  1,
  2,
)
let m = {
  a: fn () {
    1
  },
  b: fn () {
    2
  },
  c: 3,
}
EOF
"$CULEBRA" fmt "$TMP/blk2_in.cul" > "$TMP/blk2_got.cul" 2>"$TMP/blk2_err"
if ! diff -u "$TMP/blk2_want.cul" "$TMP/blk2_got.cul" > "$TMP/blk2_diff" 2>&1; then
  echo "FAIL golden (non-final block element): formatted output differs"
  cat "$TMP/blk2_diff"
  fail=1
fi

# --- 1k. Golden fixture: a block inside a wrappable enclosing construct -----
# Method chains, binary chains and ternaries each group their parts so they can
# wrap when too wide, and each indents the continuation. A block argument makes
# that Group illegal for the same reason as above — and here the visible symptom
# is the indent: the wrap never happens, but the continuation's indent still
# reaches the block's own lines, so it lands a level deeper than the same call
# written without the chain. Each construct stays flat instead.
# See include/formatter.h print_call / print_binary / print_ternary.
cat > "$TMP/chn_in.cul" <<'EOF'
a = xs.map(fn (x) {
  x
})
b = xs.iter().map(fn (x) {
  x
})
c = xs.iter().map(fn (x) {
  x
}, fn (y) {
  y
})
d = 1 + xs.map(fn (y) {
  y
}).len()
e = flag ? xs.map(fn (y) {
  y
}) : []
EOF
cat > "$TMP/chn_want.cul" <<'EOF'
a = xs.map(fn (x) {
  x
})
b = xs.iter().map(fn (x) {
  x
})
c = xs.iter().map(
  fn (x) {
    x
  },
  fn (y) {
    y
  },
)
d = 1 + xs.map(fn (y) {
  y
}).len()
e = flag ? xs.map(fn (y) {
  y
}) : []
EOF
"$CULEBRA" fmt "$TMP/chn_in.cul" > "$TMP/chn_got.cul" 2>"$TMP/chn_err"
if ! diff -u "$TMP/chn_want.cul" "$TMP/chn_got.cul" > "$TMP/chn_diff" 2>&1; then
  echo "FAIL golden (block inside a wrappable construct): output differs"
  cat "$TMP/chn_diff"
  fail=1
fi

# --- 1l. Golden fixture: a comment past a sliced item's last token ---------
# A comment between two elements has no slot in the printed list, so the whole
# statement is copied verbatim to keep it. That slice runs to the closing
# bracket — so a second comment sitting after the LAST element is already in
# it, and trailing it onto the item as well printed it twice, which the
# comment-preservation net refused. A list with only the late comment has no
# slice and still collapses. See include/formatter.h print_items.
cat > "$TMP/slc_in.cul" <<'EOF'
let pats = [
  re'a',        # first
  re'b',        # second
]
let one = [
  re'c',
  re'd',        # only after the last element
]
println(pats.size()+one.size())
EOF
cat > "$TMP/slc_want.cul" <<'EOF'
let pats = [
  re'a',  # first
  re'b',  # second
]
let one = [re'c', re'd']  # only after the last element
println(pats.size() + one.size())
EOF
"$CULEBRA" fmt "$TMP/slc_in.cul" > "$TMP/slc_got.cul" 2>"$TMP/slc_err"
if ! diff -u "$TMP/slc_want.cul" "$TMP/slc_got.cul" > "$TMP/slc_diff" 2>&1; then
  echo "FAIL golden (comment past a sliced item): output differs"
  cat "$TMP/slc_diff"
  fail=1
fi

# --- 1m. Golden fixture: a comment above a lone `try` / `defer` ------------
# Nothing stands between those keywords and their `{`, so the body's brace was
# located by scanning from the statement's own position — and a `try`/`defer`
# that is the sole statement of a block inherits that block's span (the
# optimizer's single-child widening). The scan then found the ENCLOSING brace
# and gave the body that block's interior, so the comment above the keyword
# was printed a second time inside the body. See include/formatter.h
# print_try / print_defer.
cat > "$TMP/lone_in.cul" <<'EOF'
fn f(a) {
  if a==1 {
    println(1)
  } else {
    # leading a lone try
    try {
      println(a)
    } catch e {
      println('error')
    }
  }
}
fn g(a) {
  if a==1 {
    println(1)
  } else {
    # leading a lone defer
    defer {
      println(a)
    }
  }
}
EOF
cat > "$TMP/lone_want.cul" <<'EOF'
fn f(a) {
  if a == 1 {
    println(1)
  } else {
    # leading a lone try
    try {
      println(a)
    } catch e {
      println('error')
    }
  }
}
fn g(a) {
  if a == 1 {
    println(1)
  } else {
    # leading a lone defer
    defer {
      println(a)
    }
  }
}
EOF
"$CULEBRA" fmt "$TMP/lone_in.cul" > "$TMP/lone_got.cul" 2>"$TMP/lone_err"
if ! diff -u "$TMP/lone_want.cul" "$TMP/lone_got.cul" > "$TMP/lone_diff" 2>&1; then
  echo "FAIL golden (comment above a lone try/defer): output differs"
  cat "$TMP/lone_diff"
  fail=1
fi

# --- 1n. Golden fixture: a comment around a lone `cond` --------------------
# `cond` is the third keyword whose `{` follows it directly, so it inherits an
# enclosing block's span the same way a lone `try` / `defer` does — and it has
# no body node to scan from, since its arms start inside the brace. Both a
# comment above the keyword and one inside the arms once came out twice.
# See include/formatter.h print_cond.
cat > "$TMP/cnd_in.cul" <<'EOF'
fn grade(n) {
  # above a sole cond
  cond {
    n>90 => 'A',
    _ => 'C',
  }
}
fn arms(n) {
  cond {
    # about the first arm
    n>90 => 'A',
    _ => 'C',
  }
}
EOF
cat > "$TMP/cnd_want.cul" <<'EOF'
fn grade(n) {
  # above a sole cond
  cond {
    n > 90 => 'A',
    _ => 'C',
  }
}
fn arms(n) {
  cond {
    # about the first arm
    n > 90 => 'A',
    _ => 'C',
  }
}
EOF
"$CULEBRA" fmt "$TMP/cnd_in.cul" > "$TMP/cnd_got.cul" 2>"$TMP/cnd_err"
if ! diff -u "$TMP/cnd_want.cul" "$TMP/cnd_got.cul" > "$TMP/cnd_diff" 2>&1; then
  echo "FAIL golden (comment around a lone cond): output differs"
  cat "$TMP/cnd_diff"
  fail=1
fi

# --- 1o. Golden fixture: a comment in a literal that is a block's only value
# The fourth shape of the same fold: an object / set literal alone in a block
# wears that block's braces, so scanning from its position found the BLOCK's
# interior and the literal's elements printed the comments the block was
# already printing. A literal bound to a name never folds, so it stayed fine
# either way — both belong here. See include/formatter.h print_brace_literal.
cat > "$TMP/lit_in.cul" <<'EOF'
fn tool() {
  {
    # about the name
    name: 'exec',
    doc: 'run it',
  }
}
fn nested() {
  let t = {
    # about the name
    name: 'exec',
  }
  t
}
EOF
cat > "$TMP/lit_want.cul" <<'EOF'
fn tool() {
  {
    # about the name
    name: 'exec',
    doc: 'run it',
  }
}
fn nested() {
  let t = {
    # about the name
    name: 'exec',
  }
  t
}
EOF
"$CULEBRA" fmt "$TMP/lit_in.cul" > "$TMP/lit_got.cul" 2>"$TMP/lit_err"
if ! diff -u "$TMP/lit_want.cul" "$TMP/lit_got.cul" > "$TMP/lit_diff" 2>&1; then
  echo "FAIL golden (comment in a block's only literal): output differs"
  cat "$TMP/lit_diff"
  fail=1
fi

# --- 1c. Golden fixture: a lambda is never a primary -----------------------
# A lambda's body runs as far right as it can, so parentheses around one are
# load-bearing wherever it is an operand: `(|q| f(q))(x)` reprinted without
# them is a lambda whose body is `f(q)(x)`. prec() used to fall through to its
# default (16, binds tightest) for LAMBDA, and the safety net refused the file
# rather than emit that — see include/formatter.h prec().
cat > "$TMP/lam_in.cul" <<'EOF'
let xs=[1,2,3]
inspect((|q| q.size())(xs))
inspect((|x| x*2)(5)+1)
inspect(1+(|x| x*2)(5))
let table=[|x| x+1,|x| x*2]
inspect(table[1](10))
inspect(xs.map(|x| x*2))
let add=|a| |b| a+b
inspect(add(2)(3))
EOF
cat > "$TMP/lam_want.cul" <<'EOF'
let xs = [1, 2, 3]
inspect((|q| q.size())(xs))
inspect((|x| x * 2)(5) + 1)
inspect(1 + (|x| x * 2)(5))
let table = [|x| x + 1, |x| x * 2]
inspect(table[1](10))
inspect(xs.map(|x| x * 2))
let add = |a| |b| a + b
inspect(add(2)(3))
EOF
"$CULEBRA" fmt "$TMP/lam_in.cul" > "$TMP/lam_got.cul" 2>"$TMP/lam_err"
if ! diff -u "$TMP/lam_want.cul" "$TMP/lam_got.cul" > "$TMP/lam_diff" 2>&1; then
  echo "FAIL golden (lambda): formatted output differs from expected"
  cat "$TMP/lam_diff"
  cat "$TMP/lam_err"
  fail=1
fi

# --- 2 + 3. Corpus safety + idempotency (parallel) ------------------------
# Format every corpus file twice — once to check the re-parse/comment safety
# net doesn't refuse (exit 2), once more to assert idempotency. The files are
# independent and each run is a fresh CPU-bound culebra startup, so this fans
# out near-linearly. Per-file scratch keeps workers from clobbering a shared
# temp; refused/not-idempotent verdicts land as marker files collected below.
CORPUS="$TMP/corpus"; mkdir -p "$CORPUS"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 8)"
export CULEBRA CORPUS
# Enumerate the corpus once (NUL-delimited) so the worker fan-out and the file
# count `n` can't drift apart if the roots/pattern are ever edited.
find "$ROOT/tests" "$ROOT/examples" -name '*.cul' -print0 > "$CORPUS/files.z"
n=$(tr -dc '\0' < "$CORPUS/files.z" | wc -c | tr -d ' ')
xargs -0 -n1 -P "$JOBS" bash -c '
      f="$1"
      slug=$(printf "%s" "$f" | tr -c "[:alnum:]" "_")
      o1="$CORPUS/$slug.o1"; o2="$CORPUS/$slug.o2"
      "$CULEBRA" fmt "$f" > "$o1" 2>"$CORPUS/$slug.err"
      if [[ $? -eq 2 ]]; then printf "%s" "$f" > "$CORPUS/$slug.refused"; exit 0; fi
      "$CULEBRA" fmt "$o1" > "$o2" 2>/dev/null
      cmp -s "$o1" "$o2" || printf "%s" "$f" > "$CORPUS/$slug.notidem"
    ' _ < "$CORPUS/files.z"
refused=0; notidem=0
for m in "$CORPUS"/*.refused; do
  [[ -e "$m" ]] || continue
  refused=$((refused + 1))
  echo "FAIL refused: $(cat "$m") -- $(cat "${m%.refused}.err")"
done
for m in "$CORPUS"/*.notidem; do
  [[ -e "$m" ]] || continue
  notidem=$((notidem + 1))
  echo "FAIL not-idempotent: $(cat "$m")"
done
echo "corpus: $n files, refused=$refused notidem=$notidem"
[[ $refused -ne 0 || $notidem -ne 0 ]] && fail=1

# --- 4. Directory recursion ------------------------------------------------
mkdir -p "$TMP/dir/sub"
printf 'let  x=1\n' > "$TMP/dir/a.cul"
printf 'let  y=2\n' > "$TMP/dir/sub/b.cul"
printf 'plain text\n' > "$TMP/dir/note.txt"
# --check on a dirty tree exits 1
"$CULEBRA" fmt --check "$TMP/dir" >/dev/null 2>&1
[[ $? -eq 1 ]] || { echo "FAIL dir --check: expected exit 1 on dirty tree"; fail=1; }
# -i formats every .cul (and leaves the .txt alone), then the tree is clean
"$CULEBRA" fmt -i "$TMP/dir" >/dev/null 2>&1
"$CULEBRA" fmt --check "$TMP/dir" >/dev/null 2>&1
[[ $? -eq 0 ]] || { echo "FAIL dir -i: tree not clean after write"; fail=1; }
grep -q 'plain text' "$TMP/dir/note.txt" || { echo "FAIL dir: .txt was touched"; fail=1; }

# --- 5. Input selection ----------------------------------------------------
# stdin mode is chosen from the arguments alone, never as a fallback: paths
# that expand to no .cul file used to land here and block on the terminal.
# Every case reads /dev/null so a regression shows up as a wrong exit code
# rather than a hung gate.
mkdir -p "$TMP/emptydir"
"$CULEBRA" fmt "$TMP/emptydir" >/dev/null 2>&1 < /dev/null
[[ $? -eq 2 ]] || { echo "FAIL empty dir: expected exit 2, not stdin mode"; fail=1; }
"$CULEBRA" fmt "$TMP/dir/a.cul" - >/dev/null 2>&1 < /dev/null
[[ $? -eq 2 ]] || { echo "FAIL '-' with a path: expected exit 2"; fail=1; }
printf 'let  z=3\n' | "$CULEBRA" fmt - > "$TMP/stdin_dash" 2>/dev/null
printf 'let  z=3\n' | "$CULEBRA" fmt > "$TMP/stdin_bare" 2>/dev/null
grep -qx 'let z = 3' "$TMP/stdin_dash" || { echo "FAIL fmt -: stdin not formatted"; fail=1; }
cmp -s "$TMP/stdin_dash" "$TMP/stdin_bare" || { echo "FAIL fmt: '-' and no-args disagree"; fail=1; }

# --- 6. Argument handling -------------------------------------------------
# A misspelled flag must stop the run instead of being taken for a file name:
# formatting the *other* arguments while the requested flag silently did
# nothing is how a typo goes unnoticed.
printf 'let  x=1\n' > "$TMP/arg.cul"
for bad in --wirte -x --in_place; do
  out=$("$CULEBRA" fmt "$bad" "$TMP/arg.cul" 2>"$TMP/arg.err"); rc=$?
  err=$(cat "$TMP/arg.err")
  if [[ $rc -ne 2 || -n "$out" || "$err" != *"unknown option '$bad'"* ]]; then
    echo "FAIL unknown-flag [$bad]: rc=$rc out=$out err=$err"; fail=1
  fi
done
# The guard runs before any file is touched, so a typo can't half-apply.
out=$("$CULEBRA" fmt -i --wirte "$TMP/arg.cul" 2>/dev/null); rc=$?
if [[ $rc -ne 2 || "$(cat "$TMP/arg.cul")" != 'let  x=1' ]]; then
  echo "FAIL unknown-flag with -i: rc=$rc file=$(cat "$TMP/arg.cul")"; fail=1
fi

# `-` is stdin; combining it with real paths would mean two inputs and one
# stdout, so it is refused rather than letting either side win silently.
out=$(printf 'let   Z=9\n' | "$CULEBRA" fmt - "$TMP/arg.cul" 2>"$TMP/arg.err"); rc=$?
if [[ $rc -ne 2 || -n "$out" || "$(cat "$TMP/arg.err")" != *"can't mix"* ]]; then
  echo "FAIL stdin-plus-path: rc=$rc out=$out err=$(cat "$TMP/arg.err")"; fail=1
fi
# Likewise `-i` with stdin: there is no file to rewrite. Both the explicit `-`
# and the implicit no-paths form must say so instead of ignoring `-i`.
for args in "-i -" "-i"; do
  out=$(printf 'let   Z=9\n' | "$CULEBRA" fmt $args 2>"$TMP/arg.err"); rc=$?
  if [[ $rc -ne 2 || -n "$out" || "$(cat "$TMP/arg.err")" != *"nothing to rewrite"* ]]; then
    echo "FAIL stdin-in-place [$args]: rc=$rc out=$out err=$(cat "$TMP/arg.err")"; fail=1
  fi
done
# The editor hook itself (`culebra fmt -`, used by misc/vim and misc/vscode):
# stdin formats to stdout and exits 0. Every other stdin case here asserts a
# refusal, so without this one the success path has no coverage at all.
out=$(printf 'let   Z=9\n' | "$CULEBRA" fmt - 2>"$TMP/arg.err"); rc=$?
if [[ $rc -ne 0 || "$out" != 'let Z = 9' ]]; then
  echo "FAIL stdin-to-stdout: rc=$rc out=$out err=$(cat "$TMP/arg.err")"; fail=1
fi

# `-i` composes with the reporting flags (as `gofmt -l -w` does): the file is
# rewritten AND named. Before this, list/check won and nothing was written.
printf 'let  x=1\n' > "$TMP/arg.cul"
out=$("$CULEBRA" fmt -i -l "$TMP/arg.cul" 2>&1); rc=$?
if [[ $rc -ne 1 || "$out" != "$TMP/arg.cul" ]]; then
  echo "FAIL -i -l: rc=$rc out=$out"; fail=1
fi
[[ "$(cat "$TMP/arg.cul")" == "let x = 1" ]] || {
  echo "FAIL -i -l: file not rewritten: $(cat "$TMP/arg.cul")"; fail=1; }
# Already formatted: nothing to list, exit 0.
out=$("$CULEBRA" fmt -i -l "$TMP/arg.cul" 2>&1); rc=$?
if [[ $rc -ne 0 || -n "$out" ]]; then
  echo "FAIL -i -l (clean): rc=$rc out=$out"; fail=1
fi

printf 'let  y=2\n' > "$TMP/arg2.cul"
out=$("$CULEBRA" fmt -i --check "$TMP/arg2.cul" 2>&1); rc=$?
if [[ $rc -ne 1 || -n "$out" ]]; then
  echo "FAIL -i --check: rc=$rc out=$out"; fail=1
fi
[[ "$(cat "$TMP/arg2.cul")" == "let y = 2" ]] || {
  echo "FAIL -i --check: file not rewritten: $(cat "$TMP/arg2.cul")"; fail=1; }
out=$("$CULEBRA" fmt -i --check "$TMP/arg2.cul" 2>&1); rc=$?
if [[ $rc -ne 0 || -n "$out" ]]; then
  echo "FAIL -i --check (clean): rc=$rc out=$out"; fail=1
fi

# Without `-i`, the reporting flags still only report (no stdout dump).
printf 'let  z=3\n' > "$TMP/arg3.cul"
out=$("$CULEBRA" fmt -l "$TMP/arg3.cul" 2>&1); rc=$?
if [[ $rc -ne 1 || "$out" != "$TMP/arg3.cul" ]]; then
  echo "FAIL -l alone: rc=$rc out=$out"; fail=1
fi
[[ "$(cat "$TMP/arg3.cul")" == "let  z=3" ]] || {
  echo "FAIL -l alone: file was rewritten"; fail=1; }

if [[ $fail -eq 0 ]]; then echo "fmt_test OK"; exit 0; fi
echo "fmt_test FAILED"; exit 1
