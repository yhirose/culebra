## Culebra

Source files are `.cul`. There is no manifest and no package manager;
the whole standard library is in scope without an `import`.

```bash
culebra prog.cul          # run (the bytecode VM's executor)
culebra --jit prog.cul    # the same output, that bytecode lowered to LLVM
culebra test              # run every test_*.cul below the cwd
culebra fmt -i .          # format in place (no style options)
culebra lint .            # static checks
```

**Look an API up instead of guessing it.** The whole reference is
inside the binary, so it always matches the build being run:

```bash
culebra docs -g 'Math.wrap'          # print the sections that match
culebra docs -g '<name>' >/dev/null  # exits 1 when nothing matches
```

Exit status is grep's: `0` printed something, `1` nothing matched. A
signature `-g` cannot find does not exist.

**Read `culebra docs quick-guide` before writing Culebra.** It is one
prompt-sized file: the syntax, every standard-library signature, and a
table of the habits from other languages that do not carry over. One
row of that table, for the kind of thing it covers:

```culebra
# !! TypeError
'ab' * 3
```

**Run what you write.** Undefined names are rejected before the program
starts, but that check covers names, not members: `Math.abss(1)` and
`xs.len()` survive `culebra lint` and fail when the line runs, and a
missing property is `nil` rather than an error.
