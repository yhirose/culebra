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

Culebra has seven types: `Nil`, `Bool`, `Long`, `String`, `Array`,
`Object`, `Function`.

```culebra
x = 10             # bare assignment (new immutable binding, or reassign outer)
let y = 20         # let: new immutable binding
mut z = 30         # mut: new mutable binding
z = z + 1          # mut allows reassignment
```

A bare assignment searches outward through enclosing scopes and
reassigns the nearest binding; if nothing is found, a new binding is
created in the current scope.

2. Functions and closures
-------------------------

Functions are first-class values. The syntax is `fn (params) { body }`:

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

3. Arrays and objects
---------------------

```culebra
arr = [1, 2, 3]
puts(arr.size())                      # 3
puts(arr.map(fn (x) { x * x }))       # [1, 4, 9]
puts(arr.filter(fn (x) { x % 2 == 1 })) # [1, 3]
puts(arr.reduce(0, fn (acc, x) { acc + x })) # 6
puts(arr.sum())                       # 6   (also: product / min / max)

obj = {name: 'alice', age: 30}
puts(obj.name)                        # 'alice'
puts(obj.keys())                      # ['age', 'name']
obj.age = 31                          # property reassignment
```

4. String interpolation and pattern matching
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

5. Objects from closures (Culebra's signature idiom)
----------------------------------------------------

Culebra has no class syntax, but an **object literal that captures
state through closures** gives you an object-oriented style:

```culebra
Car = {
  new: fn (miles_per_run) {
    mut total_miles = 0
    {
      run:   fn (times) { total_miles = total_miles + miles_per_run * times },
      total: fn ()      { "total: {total_miles} miles." }
    }
  }
}

car = Car.new(5)
car.run(1)
car.run(2)
puts(car.total())                     # 'total: 15 miles.'
```

The bare `total_miles = ...` inside `run` reassigns the `mut
total_miles` declared in the enclosing `new`. That is how the object's
private state is stored and mutated.

6. Shadow prohibition keeps you safe
------------------------------------

Introducing a new binding with `let` / `mut`, as a parameter, or in a
`match` pattern is a **compile-time error** if it would shadow a
variable captured from an enclosing function:

```culebra
make_bumper = fn () {
  mut count = 0
  bump = fn () {
    mut count = 10    # error: cannot shadow outer variable 'count'
  }
}
```

This prevents the classic bug of typing a new local when you meant to
reassign the outer binding. Shadowing a global or rebinding within the
same function's block scope is still allowed. See
[`language.md` §6](language.md)'s "Shadow prohibition" for the full
rule.

7. Standard library
-------------------

Core language built-ins are unqualified (`assert`, `to_long`,
`to_string`, `type_of`). Everything else lives under `Math`, `IO`,
or `Sys`:

```culebra
puts(Math.abs(-7))              # 7
puts(Math.iota(5))              # [0, 1, 2, 3, 4]

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

Next steps
----------

- Samples: [`samples/`](../samples/) (`class.cul`, `closure.cul`,
  `match.cul`, `types.cul`, ...)
- Full language spec: [`language.md`](language.md)
- Standard library reference: [`stdlib.md`](stdlib.md)
- Try interactively: `./build/culebra --shell`
