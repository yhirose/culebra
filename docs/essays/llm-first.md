# Writing for a Reader That Has Not Seen the Language

*October 29, 2026*

A language this young has little public code, so a model asked to write
it is a reader that has not seen it. The model writes a program, runs it,
reads what came back, and writes it again. For that reader the loop is
the whole of the experience, and a good part of culebra's design is an
answer to the question of what each turn of it costs.

This essay is about those decisions, and about one small experiment that
tried to put a number on a few of them. The experiment is small. What it
supports is narrower than the intent behind the design, and the sections
below try to keep the two apart.

## Where a language charges for a turn of the loop

Four things make a turn expensive, and they are not about speed.

**A wrong guess made with confidence.** The names most likely to be wrong
are the ordinary ones. `sort`, `take`, `items`, `length`: every language
has them, each with its own spelling and its own argument order, and a
reader who has seen five languages will write the sixth one's version of
the name without thinking. The errors that matter are not the exotic
calls but the plain ones.

**A failure that does not announce itself.** An undefined name is
rejected before the program starts, but a missing property is `nil`, not
an error, so `xs.length` reads as `nil` and the program carries on. The
check covers names, not members. Culebra pays for that choice, and the
cheapest remedy it has is to tell the reader plainly: run what you write.

**Several ways to say one thing.** If a conditional value can be written
four ways, a model will write all four across a program, and a reader
cannot tell which is the intended one.

**An environment to assemble.** A manifest, a package manager, an import
that must name the right module: each is a place to guess wrong before
any of the program's own logic has been tried.

## Look it up instead of guessing

The reference for the whole language and its library is inside the
binary, so it always matches the build being run:

```text
culebra docs -g 'Math.wrap'          print the sections that match
culebra docs -g '<name>' >/dev/null  exits 1 when nothing matches
culebra docs stdlib Scene            print one namespace's chapter whole
```

The exit status is `grep`'s: 0 when something printed, 1 when nothing
matched. A program or an agent can ask "does this exist?" and get a yes
or a no without reading anything. A
signature `-g` cannot find does not exist.

One observation from the experiment below belongs here. In the condition
where the model was given no guidance at all and the `docs` command was
switched off, every one of the ten runs tried `culebra docs` on its own
at least once, fifteen attempts in total, all refused. Nobody had told it
the command existed. In these runs the first move, on meeting a command
line tool the model did not know, was to ask the tool.

## One file that fits in a prompt

The reference is around 150,000 tokens, which is more than a model should
read to write a ten-line function. `culebra docs quick-guide` prints a
single file for that case: the syntax, the signatures a program reaches
for without being told to, and a table of habits from other languages
that do not carry over. The served text is about 42 KB.

Two decisions about it matter more than its contents.

*It is not a new piece of prose.* A hand-written summary of an API is a
third copy of the truth, and it goes stale first. The signature index in
the guide is generated from the reference by `just gen-quick-guide`, and
every code block in it is run by `culebra test --doc`, so a line that says
what a call returns is checked against the call.

*The table is of failures.* Each row is a habit that fails, or quietly
produces something else, in Culebra: `puts` for output, `xs.sort(|a, b|
...)` with a comparator, `xs.take(n)`, `s.ljust(10)`, `x = 1` twice in one
scope. A row says what the habit is and what to write instead.

For an agent that reads a project's instruction file,
[`agent.md`](../agent.md) is the short form: a few lines to append to
`CLAUDE.md` or `AGENTS.md` that say how to run, how to look things up,
to read the quick guide first, and to run what was written.

## The error is the next prompt

The model's next move is decided by the last line it read. So the error
has to carry the three things a reader needs: what kind of failure it
was, what happened, and where. Culebra's executor, `--jit` and AOT give
the same kind, the same text and the same position for the same program;
that sameness is a design requirement of the project, and for this reader
it has a second use, which is that the message does not depend on which
way the program was run.

Syntax errors are built by the front end rather than passed through from
the parser library: `x = [1, 2` says `expecting ',', ']' or an operator.`
and not a list of thirty-eight alternatives
([`internals/frontend.md`](../internals/frontend.md)).

## What a small experiment showed

**The setup.** Five tasks, each a function or two with a set of hidden
tests: counting words, summarizing a JSON document, evaluating a
reverse Polish expression, two generators, and drawing an aligned text
table. Three conditions, ten runs each (five tasks, two repetitions):

- **A.** The task, the `culebra` binary and nothing else. The `docs`
  command refuses.
- **B.** The same, and the quick guide as a file to read first. The
  `docs` command still refuses.
- **C.** The same as A, and the contents of `agent.md` as the project's
  rules. The `docs` command works, so the guide is one command away.

Every run was a fresh `sonnet` agent working in its own directory, and
could run the binary as often as it liked. The binary was a wrapper that
wrote each invocation and its first error lines to a log; that log is
where the counts below come from. A run was graded by appending the
hidden tests to its final file. All transcripts were then checked for
reads outside the run's directory, and none of the thirty touched the
repository's documentation or source.

Each task's hidden tests were checked two ways: a reference solution
passes them, and a deliberately broken version fails them.

**The result.**

| | A: nothing | B: guide, no lookup | C: rules and lookup |
|---|---|---|---|
| passed the hidden tests | 10 of 10 | 10 of 10 | 10 of 10 |
| times the program was run, median (range) | 14.5 (3 to 39) | 1.5 (1 to 3) | 2 (1 to 3) |
| runs that never failed | 0 of 10 | 7 of 10 | 5 of 10 |
| runs that needed one execution | 0 of 10 | 5 of 10 | 3 of 10 |

Every run in every condition ended with a correct program. A model with a
binary and the freedom to retry gets there. What differed between the
conditions was the number of turns it took: about fifteen runs when the model was on its
own, and one or two when it had the guide.

The first failure in a condition-A run was one of three kinds. Three runs
failed on an `ImmutableError`, from assigning to a name that was bound
without `mut`. Three failed on an
undefined name: `puts`, `str`, and a free `sort`. Four failed on a syntax
error, one of them a `var` declaration. The later failures in those runs
included `xs.take(n)` on an Array, a comparator passed to `sort_by`, and
`.length`. Most of these are rows in the guide's table of habits (`puts`, the
comparator, `take`, `.length`, a name assigned twice); `str` and `var` are
not.

B and C did not differ at this size, 1.7 runs against 1.9. The lookup
command was used in every C run, 46 times over ten runs, and some of the
lookups found nothing (`ljust`, `pad_left`); the run then finished anyway.

**What this does not show, and where it is weak.**

- *The tasks were written by someone who had just read the guide.* Two of
  them test things the guide states: that a string's width in characters
  is not its `size()`, and the format specifier for padding. That favors
  the conditions with the guide, and it is likely most of why B and C
  look so close.
- *Ten runs per condition, one model, a single sitting.* The spread in condition A
  is wide (three to thirty-nine runs); a difference between B and C of
  the size seen here would not survive a repeat.
- *Tasks of ten to thirty lines.* Nothing here says anything about a
  program of a thousand lines, or about a library the guide does not
  cover, which is where `culebra docs -g` ought to matter and where this
  experiment did not look.
- *"Number of runs" is a proxy.* It counts turns of the loop, which is
  what the design is about, and not the cost of a turn, which is what the
  user pays.
- *There is no baseline.* The same tasks in a language the model knows
  well were not run, so none of this says how Culebra compares with one.

What it does support is smaller than the design's ambition and is worth
saying plainly: for these tasks, a model that is handed the guide wrote a
correct program in about two runs, and one that was not wrote it in about
fifteen.

## What the examples teach

The obvious answer to non-idiomatic output is more rules. The
measurement that changed the approach was of the examples. On July 30, 2026, before
a cleanup, the programs under `examples/` called `.empty()` zero times and
wrote `.size() == 0` or `> 0` twenty-three times. A reader that learns
from code would conclude that `empty()` does not exist, and from the
evidence it was shown it would be right to. The examples in the guide of
that time (`llm.md`, which was renamed `quick-guide.md` a few days later)
carried the same habit, and because tests run them they look
authoritative.

The fix was to the examples, not the rules. After the change `.empty()`
appeared twenty-three times and the longer form not at all; a count
today, with new examples added since, is 147 and 42. The lesson kept: an
example is a rule that also gets copied.

## Fewer ways to say the same thing

Several decisions look arbitrary until they are read as removing a choice.

- There is no pipeline operator. Chaining methods does the job (the
  uniform call syntax lets any function be called as `x.f()`), which is
  the reason the project recorded for leaving it out.
- A Set is written `Set(a, b)` and in no other way. Braces are an Object's.
- `culebra fmt` has no style options.
- There is no manifest and no `import` for the standard library.

None of this is a claim that the single way is the best way. The thinking
behind it is that a reader who has to choose between two spellings can
choose the wrong one. The experiment did not test that.

## What was not measured

Whether the generated signature index helps more than a hand-written one
would; whether the format of the error text, as opposed to its presence,
changes the next turn; whether a model that has seen the language in
training would behave differently from the one tested; how any of this
holds over a session of hours. Each is a question with a method, and none
has been asked.

## Lineage

Most of this was learned from other people's tools. `go doc` and the
`man` page put the reference where the reader already is, at the command
line, and `gofmt` showed that a formatter with no style options ends an
argument instead of moving it. Elm and the Rust compiler made it an
expectation that an error names what was expected and where, and that
the helpful message is the product, not an extra. Python's `help()` is
the older version of asking the language what a name means. The part
this essay adds is small: a lookup whose exit status says yes or no, a
guide that is generated and run, and the habit of checking the guidance
by giving it to a reader who has never seen the language.
