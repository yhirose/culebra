Culebra Standard Library
========================

This document specifies the built-in **global functions** and I/O of
the Culebra runtime. Everything described here is available without
any `import` statement.

Methods on built-in types (`String`, `Array`, `Object`) are part of
the language itself; see [§17 of the language spec](language.md).

Conventions used below:

* Types follow the annotations described in [§14 of the language
  spec](language.md). `Any` denotes any value.
* Throws clauses describe runtime errors of the form
  `type error at L:C.` etc. See [§15 of the language spec](language.md).

Index
-----

1. Global functions
2. I/O
3. Design notes
4. Not included (yet)

---

1. Global functions
-------------------

### `puts(x: Any) -> Nil`

Print `x` followed by a newline to standard output. Reference types
are formatted the same way as `Array.str_array()` / `Object.str_object()`,
and strings are printed **with surrounding single quotes**.

```culebra
puts('hi')       # → 'hi'
puts(42)         # → 42
puts([1, 'a'])   # → [1, 'a']
```

### `print(x: Any) -> Nil`

Write `x` to standard output **without a trailing newline**, using
`to_string` formatting (strings are **unquoted**). Useful for
building a single line of output from several writes.

```culebra
print('Hello, ')
print('world!')
puts('')         # → Hello, world!
```

### `assert(cond: Bool) -> Nil`

Evaluate `cond`. If falsy, abort with `assert failed at L:C.`. The
location is the source position of the `assert` call.

```culebra
assert(1 + 1 == 2)
```

**Throws**: `assert failed at L:C.` on falsy; `type error at L:C.` if
`cond` is neither `Bool` nor `Long`.

### `abs(x: Long) -> Long`

Absolute value of `x`.

```culebra
puts(abs(-7))    # 7
```

### `min(a: Long, b: Long) -> Long`

Smaller of `a` and `b`.

### `max(a: Long, b: Long) -> Long`

Larger of `a` and `b`.

### `range(n: Long) -> Array` / `range(start: Long, end: Long) -> Array`

Generate a new `Array` of integers.

* `range(n)` returns `[0, 1, ..., n-1]`. If `n <= 0`, an empty array.
* `range(start, end)` returns `[start, start+1, ..., end-1]`. If
  `start >= end`, an empty array.

```culebra
puts(range(3))         # [0, 1, 2]
puts(range(2, 5))      # [2, 3, 4]
puts(range(5, 2))      # []
```

### `to_long(s: String) -> Long`

Parse `s` as a base-10 signed integer. Leading/trailing whitespace is
allowed; anything else fails.

**Throws**: `type error at L:C.` if `s` does not parse as an integer.

```culebra
puts(to_long('42'))    # 42
puts(to_long('-7'))    # -7
```

### `to_string(v: Any) -> String`

Convert `v` to its display form (same formatting as interpolation
inserts — strings come through unquoted).

```culebra
puts(to_string(42))         # '42'
puts(to_string([1, 2]))     # '[1, 2]'
puts(to_string('hi'))       # 'hi'
```

### `type_of(v: Any) -> String`

Return the runtime type name of `v`. One of
`'Nil'`, `'Bool'`, `'Long'`, `'String'`, `'Array'`, `'Object'`,
`'Function'`.

```culebra
puts(type_of(42))          # 'Long'
puts(type_of('hi'))        # 'String'
puts(type_of([1, 2]))      # 'Array'
```

---

2. I/O
------

### `input() -> String`

Read a single line from standard input. The trailing newline is
stripped. Returns `''` (empty string) on end-of-file.

```culebra
puts('name?')
name = input()
puts("Hello, {name}")
```

### `read_file(path: String) -> String`

Read the entire file at `path` into a `String`.

**Throws**: `type error at L:C.` if the file cannot be opened.

```culebra
contents = read_file('data.txt')
```

### `write_file(path: String, content: String) -> Nil`

Write `content` to the file at `path`, creating or overwriting it.

**Throws**: `type error at L:C.` if the file cannot be opened for
writing.

```culebra
write_file('out.txt', 'hello\n')
```

---

3. Design notes
---------------

### Free function vs method

Global free functions are used when the operation either constructs
a value from nothing (`range`, `input`) or applies uniformly to
multiple types (`type_of`, `to_string`), and when the name reads
better unqualified (`abs(x)`, `min(a, b)`). Operations that are
*about* a specific type use method syntax — see §17 of the language
spec for String/Array/Object methods.

### Error-by-throw versus `nil` returns

The library prefers throwing on unrecoverable type errors
(`to_long('abc')`, `read_file(...)` on a missing file) and returning
sentinel values for "found or not" predicates (`input()` returns
`''` on EOF). This keeps hot paths simple without requiring a
`try`/`catch` mechanism.

---

4. Not included (yet)
---------------------

### Floating-point math (`sqrt`, `sin`, `cos`, `log`, `pow`, `random`)

Culebra has no `Float` type yet. Integer-only math functions would be
awkward (`sqrt` could return the integer square root). Adding a full
math suite is deferred to a future phase that also introduces a
numeric type hierarchy.

### Regular expressions

Deferred; would require either a built-in regex engine or a vendored
dependency.

### Date, time, random, OS introspection

Deferred. Scripts that need these today can call out through
`read_file`/`write_file` with a helper process.

### Collections beyond `Array`/`Object`

No `Set`, `Queue`, `Tuple`, etc. Use `Array` and `Object` for now.

---

See also: [`docs/language.md`](language.md) for the language
specification.
