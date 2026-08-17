# Doctest runner self-test fixture

A regression guard for `culebra test --doc`: every block below must pass
(or be skipped), so the file exits 0. It exercises each marker form and
the string-literal-safety of the marker scanner. Failure detection is
covered interactively, not here (a committed file can't carry blocks
that are meant to fail).

Single-line expected output:

```culebra
print("hello")  # => hello
```

Multi-line expected output:

```culebra
for i in iota(3) {
  print("{i}\n")
}
# => |
# 0
# 1
# 2
```

Expected throw, substring match:

```culebra
to_long('abc')  # !! type error
```

No markers — only needs to run without error:

```culebra
let x = 1 + 2
```

Skip directive — must not run or count (the body is invalid on purpose):

```culebra
# doctest: skip
this_is_not_even_valid_culebra @@@
```

A `#` inside a string literal must NOT be parsed as an expectation
(prints the literal verbatim, no marker detected):

```culebra
print("not a # => marker")
```

A name nothing declares is a run-time error, not a refusal to run the
block — the compiled lanes (`--jit`, `--vm`) have to reach it too:

```culebra
print("before\n")  # => before
no_such_name_in_this_fixture  # !! undefined variable 'no_such_name_in_this_fixture'
```

The lazy stdlib resolves on every engine — a compiled lane that never
registered the builders would not find these at all:

```culebra
assert_eq(1 + 1, 2)
let started = Time.now()
print("{Path.new('a') / 'b'}\n")  # => a/b
```
