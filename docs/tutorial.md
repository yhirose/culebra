Culebra Tutorial
================

A five-minute quick-start for people who have never used Culebra. For
the full language spec see [`language.md`](language.md); for the
standard library see [`stdlib.md`](stdlib.md).

Setup
-----

```bash
just build
echo "puts('hello')" > hello.cul
./build/culebra hello.cul          # → 'hello'
./build/culebra --jit hello.cul    # JIT produces the same output
```

1. Values and bindings
----------------------

Culebra has eight types: `Nil`, `Bool`, `Long`, `Float`, `String`,
`Array`, `Object`, `Function`.

```culebra
x = 10             # bare assignment (new immutable binding, or reassign outer)
let y = 20         # let: new immutable binding
mut z = 30         # mut: new mutable binding
z = z + 1          # mut allows reassignment
z += 1             # compound assignment: sugar for `z = z + 1`
                   # (`-= *= /= %= **= @=` work the same way)
```

A bare assignment searches outward through enclosing scopes and
reassigns the nearest binding; if nothing is found, a new binding is
created in the current scope.

2. Functions and closures
-------------------------

Functions are first-class values. The syntax is `fn (params) { body }`,
or `|x| expr` for short lambdas:

```culebra
add = fn (a, b) { a + b }
puts(add(2, 3))                    # 5

# Type annotations are optional
add_typed = fn (a: Long, b: Long) -> Long { a + b }

# Use `self` for recursion
fib = fn (x) {
  if x < 2 { x } else { self(x - 2) + self(x - 1) }
}
puts(fib(10))                      # 55
```

**Closures** capture outer variables:

```culebra
make_counter = fn () {
  mut n = 0
  fn () { n = n + 1; n }   # bare `n = ...` reassigns the captured outer
}
c = make_counter()
puts(c())                  # 1
puts(c())                  # 2
```

**Keyword arguments and `**splat`** let callers pass named options
without remembering positional order. Parameters can declare default
values; a `*` marker after the last positional makes the rest
keyword-only; `**rest` collects unknown keywords into an Object:

```culebra
greet = fn (name, *, greeting = 'hi', **opts) {
  prefix = if opts.formal { 'Mr./Ms. ' } else { '' }
  "{greeting}, {prefix}{name}"
}
puts(greet('alice'))                      # 'hi, alice'
puts(greet('alice', greeting: 'hello'))   # 'hello, alice'
puts(greet('bob', formal: true))          # 'hi, Mr./Ms. bob'

# `**` splats an Object into a call:
let common = {greeting: 'yo', formal: false}
puts(greet('carol', **common))            # 'yo, carol'
```

See [language spec §11](language.md) for the full kwargs / splat rules.

3. Control flow
---------------

`if` (and `match`, see §5) is an expression — it evaluates to the
value of the chosen branch. `while` and `for` are statements; their
value is `nil`.

```culebra
sign = if x > 0 { 1 } else if x < 0 { -1 } else { 0 }

mut i = 0
while i < 5 { puts(i); i = i + 1 }

for c in 'abc'   { puts(c) }    # iterate a string (UTF-8 scalars)
for n in 0..10   { puts(n) }    # exclusive range
for n in 0..=10  { puts(n) }    # inclusive range
for k in obj     { puts("{k}={obj[k]}") }   # iterate object keys (sorted)
```

`break` and `continue` work in `while` and `for`.

4. Arrays, objects, iterator chains
-----------------------------------

```culebra
arr = [1, 2, 3]
puts(arr.size())                      # 3
puts(arr.sum())                       # 6   (also: product / min / max)

# Method chains compose; lambdas use the |x| form
puts([1, 2, 3, 4].map(|x| x * x)
                 .filter(|x| x > 4)
                 .reduce(0, |a, b| a + b))  # 25

obj = {name: 'alice', mut age: 30}    # `mut` per-property; otherwise immutable
puts(obj.name)                        # 'alice'
puts(obj.keys())                      # ['age', 'name']
obj.age = 31                          # property reassignment (only on `mut` props)
```

`range(N)` returns a lazy iterator; combined with `.map`,
`.filter`, `.reduce`, `.for_each`, etc., it avoids building
intermediate arrays. The JIT fuses many of these into bare counter
loops — see [`language.md` §17](language.md).

5. String interpolation and pattern matching
--------------------------------------------

```culebra
name = 'Culebra'
puts("hello {name}!")                 # 'hello Culebra!'

describe = fn (v) {
  match v {
    0                  => 'zero',
    1 | 2 | 3          => 'small',
    n: Long if n > 100 => "big ({n})",
    s: String          => "str ({s})",
    [head, ...tail]    => "head={head}, rest={tail.size()}",
    {name, age}        => "{name}, {age}",
    _                  => 'other'
  }
}
puts(describe(2))                     # 'small'
puts(describe([1, 2, 3, 4]))          # 'head=1, rest=3'
puts(describe({name: 'bob', age: 25})) # 'bob, 25'
```

6. `class` sugar
----------------

`class` declares a constructor + methods. Fields set via `this.x = ...`
are mutable by default. Instances carry a `class:` tag readable from
`obj.class` or via `match`.

```culebra
class Car {
  new(mpr)  { this.miles = 0; this.mpr = mpr }
  run(n)    { this.miles = this.miles + this.mpr * n }
  total()   { "total: {this.miles} miles." }
}
car = Car.new(5)
car.run(1); car.run(2)
puts(car.total())                     # 'total: 15 miles.'
puts(car.class)                       # 'Car'
```

Methods include special methods (`__add__`, `__mul__`, `__pow__`,
`__matmul__`, …) for operator overloading, and the well-known `drop`
RAII hook:

```culebra
class V {
  new(x)        { this.x = x }
  __add__(o)    { V.new(this.x + o.x) }
  drop()        { puts("releasing {this.x}") }
}
puts((V.new(2) + V.new(3)).x)         # 5
# 'releasing ...' fires when each V is collected
```

Closures over a `mut` outer also work as objects (no `class` keyword);
see [`language.md` §10](language.md) for both styles side by side.

7. Error handling
-----------------

`throw` raises; `try ... catch` catches. The thrown value can be any
Culebra value — string, object, anything.

```culebra
parse_pos = fn (s) {
  let n = to_long(s)
  if n <= 0 { throw "expected positive, got {s}" }
  n
}

result = try { parse_pos('-3') } catch err { "fallback ({err})" }
puts(result)                          # 'fallback (expected positive, got -3)'
```

Use `defer { cleanup() }` to register a cleanup that fires on every
exit path (normal, `return`, or `throw`).

8. Standard library
-------------------

Core language built-ins are unqualified (`assert`, `to_long`,
`to_string`, `type_of`). Everything else lives under `Math`, `IO`,
`Sys`, `Random`, `String` namespaces:

```culebra
puts(Math.abs(-7))              # 7
puts(iota(5))              # [0, 1, 2, 3, 4]

name = IO.input()               # read one line from stdin
IO.write('out.txt', 'hello')    # file I/O

# $ culebra script.cul -- alice bob
puts(Sys.argv)                  # ['alice', 'bob']
```

`puts` and `print` are CLI conveniences that alias `IO.puts` /
`IO.print`, so `puts(x)` and `IO.puts(x)` are equivalent when
running scripts through the `culebra` binary. Embedders that
construct the environment themselves get the `IO` namespace only.

Full reference: [`stdlib.md`](stdlib.md).

> **Sidebar — UFCS.** Any free function `f(x, ...)` can also be
> called as `x.f(...)`. This is purely syntactic, but lets you mix
> custom helpers into method chains naturally. Useful for adding
> ad-hoc operations to types you don't own.

> **Sidebar — Shadow prohibition.** Introducing a new binding (`let`,
> `mut`, parameter, or `match` pattern) is a compile-time error if it
> would shadow a captured outer variable. This catches the bug of
> typing a new local when you meant to reassign. See
> [`language.md` §6](language.md) for the full rule.

Next steps
----------

- Samples: [`samples/`](../samples/) (`class.cul`, `closure.cul`,
  `match.cul`, `types.cul`, `microgpt/`, ...)
- Full language spec: [`language.md`](language.md)
- Standard library reference: [`stdlib.md`](stdlib.md)
- Try interactively: `./build/culebra --shell`
