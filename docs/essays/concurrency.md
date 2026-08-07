# Anders Hejlsberg on Concurrency, and culebra

*August 1, 2026*

Chapter 13 of *Masterminds of Programming* (Federico Biancuzzi and Shane
Warden, eds., O'Reilly Media, 2009) contains a passage where Anders Hejlsberg
(designer of C#, later TypeScript) talks about how a programming language
should approach concurrency. The interview itself predates the book's
publication, so the remarks were likely made sometime in the latter half of
the 2000s. What follows is a summary based on an excerpt, not a verbatim
quotation — I've reordered the points in my own words, and I'll flag this
translation's provenance more carefully in the closing note.

- Automatic parallelization from a single compiler switch like `/Parallel`
  won't happen, because imperative programming depends on side effects.
  People have tried it, and it hasn't worked out in mainstream imperative
  languages like C++, C#, and Java.
- Getting there requires clearing at least two hurdles. First, concurrency
  needs a modern API that sits at a higher level than threads, locks, and
  monitors. Second, that programming style needs to be made easy and safe —
  guaranteeing object immutability, providing side-effect-free pure
  functions, analyzing object graph independence (checking whether a
  reference to some object graph is shared with anything else; if it isn't,
  it can be mutated without affecting anyone else). He expected compiler
  analysis to eventually deliver a kind of safety similar to the type safety
  and memory safety we already have.
- He also describes a path where people fluent in a specific domain (data
  transformation, numerical processing, signal processing, image
  processing) write APIs that internally achieve large-scale concurrency
  while looking synchronous from the outside — concurrency enclosed inside
  the API.
- For data-parallel workloads, he suggests a high-level API model along the
  lines of: "here's the data, here's the computation I want performed, use
  as many CPUs as you can to do it as fast as possible." He adds that the
  code fragment (a lambda) passed to such an API should ideally be analyzed
  by the compiler, which could guarantee or warn about the absence of side
  effects.
- Finally, after explicitly noting that this is only a small slice of
  concurrency, he mentions that Erlang-style languages — built on
  asynchronous agents and message passing, a quite different programming
  model — are already used in highly scalable distributed systems.

Looking back more than fifteen years later, several of these predictions
look fairly on target. The magic of automatic parallelization never
happened. What's a little more tangled is what "a modern high-level API"
actually turned out to mean: async/await and structured concurrency became
standard in major languages, while Rust and Swift opened up an entirely
different axis — compile-time safety guarantees. Whether either of those is
really the same thing as the "something beyond threads, locks, and
monitors" he had in mind isn't so clear-cut to me, and I'll come back to
that below.

This essay uses that outlook as a yardstick for looking at culebra's
concurrency design. culebra's concurrency model takes the shape of
"isolation units (process/isolate) + a copy boundary + channels," and does
not offer a shared mutable heap as a language feature. What follows is five
representative pieces of parallel code, each considered against where it
falls relative to what Hejlsberg said.

## Example 1: agent glue — `Proc.all` (process parallelism and partial failure)

```culebra
# doctest: skip
# Fire the same prompt at three LLM providers in parallel and collect every result
let results = Proc.all(
  [
    ["llm-cli", "--provider", "openai", prompt],
    ["llm-cli", "--provider", "anthropic", prompt],
    ["llm-cli", "--provider", "google", prompt],
  ],
  limit: 3,
  timeout: 30000,
)

for r in results {
  if r.ok {
    println(r.stdout)
  } else {
    println("failed: " + r.stderr)
  }
}

# just want the fastest one?
let fastest = Proc.race([cmd_a, cmd_b, cmd_c])
```

Against the demand for an API "above the level of threads, locks, and
monitors," this looks like the most conservative possible answer. There's
no shared memory at all, let alone threads — the OS guarantees isolation at
the process boundary. What's interesting is that the return value is an
array of results tagged `ok`, not an exception (a semantics close to JS's
`Promise.allSettled`), which lets "run three, two succeed, one fails" be
handled as an ordinary value. A single-exception model — a try/catch that
unwinds everything on one failure — can't express partial failure in a
concurrent setting well; culebra is designed from the start to avoid that.
This allSettled-style pragmatism is close in spirit to Erlang's
let-it-crash philosophy, and the message-passing worldview he touched on
while flagging it as "just a small slice" seeps into process parallelism
here too.

## Example 2: data parallelism — `Parallel.map`

```culebra
# doctest: skip
# "here's the data, here's the computation, use the CPUs to go fast" (paraphrased)
let thumbs = Parallel.map(image_paths, |p| make_thumbnail(p), limit: 8)

# with progress reporting, running every element to completion
let settled = Parallel.map_settled(
  urls,
  |u| Http.get(u).body,
  on_progress: |done, total| IO.print("\r" + done.to_string() + "/" + total.to_string()),
)
```

This traces fairly faithfully over the high-level API model he described
for data-parallel processing: "here's the data, here's the computation, use
as many CPUs as you can to go as fast as possible." On top of that, a call
like `limit: 8` has a worker pool running underneath it, and from the
caller's side it looks almost like a synchronous call — "the results of the
parallel run just come back together." That's fairly close to the path he
described, where a domain expert encloses concurrency inside the API.

Where the actual mechanism takes a quite different route from what he had
in mind is how the lambda passed to the API gets made safe. He wanted the
compiler to analyze a code fragment handed to an API (a lambda like `|p|
make_thumbnail(p)`) and guarantee, or at least warn about, the absence of
side effects. What culebra does instead of that kind of analysis is
**securing independence by construction**. This lambda is serialized at the
moment it's spawned, and each worker receives its own copied object graph.
Independence isn't something the compiler proves; it holds automatically
because of the copy. The "guarantee of no side effects" he expected from
the compiler shrinks, in culebra, down to a single point: a runtime
boundary check (Sendable checking, with a `SendError` on violation).
Checking only the values that cross a boundary, as they cross it, is a lot
easier than statically analyzing whether values intersect as a graph, and
it's implementable even in a dynamically typed language — this is a
different road from the one he had in mind, but it amounts to one answer to
the same question.

## Example 3: message passing — `Channel` and `fan_in`

```culebra
# doctest: skip
# producer-consumer: offload parsing to a separate isolate, consume the stream in the body
let (tx, rx) = Channel.new(64)
let prod = Isolate.spawn(fn () {
  for line in File.open("big.log").lines() {
    tx.send(parse(line))
  }
})  # tx drops when the isolate ends → the channel auto-closes → the for-in ends

for record in rx {
  index.add(record)
}
prod.join()

# fan-in: merge N worker streams into one
let merged = Channel.fan_in(shards, fn (shard, tx) {
  for hit in search(shard, query) {
    tx.send(hit)
  }
})
for hit in merged {
  report(hit)
}
merged.join()
```

Hejlsberg brought up Erlang-style message passing strictly as one example
already in use in the world of distributed systems, and even qualified it
himself by saying "this is only a small slice of concurrency." In culebra
this positioning shifts: instead of being a side path for distributed
systems, it's the default communication mechanism within a single process.
Since culebra doesn't provide a shared heap, message passing isn't one
option among several — it's the only means of communication available.
Another difference is that it picked Go/Rust-style CSP channels
(pipe-centric) over Erlang's mailboxes (addressed by PID).

And what I think matters more than it might seem, quietly, is that `send`
and `recv` block and carry no "color." Since an isolate is a real thread, it
can afford to block. Being able to write this code without introducing
async/await — which has dominated the last fifteen-plus years (a function
gets colored `async`, and that color propagates to its callers) — is a
direct consequence of that structure. As I touched on at the top, whether
async/await is actually the realization of the "modern high-level API" he
was talking about is genuinely unclear to me; Java's pivot from
CompletableFuture to Loom's virtual threads (a return to colorless
blocking) looks like evidence that async/await wasn't the only answer.
Auto-close — the guarantee that a receiver's for-in always ends even if the
sender terminates abnormally — is the language's answer to a problem
Hejlsberg didn't name but that's fairly fatal in production: partial
failure turning into a deadlock.

## Example 4: shared memory's survival — `@packable` and `SharedBuffer`

```culebra
# doctest: skip
@packable class Particle { x: Float32; y: Float32; vx: Float32; vy: Float32 }
let world = SharedBuffer.new(100000, Particle)

# each worker writes only its disjoint range — zero synchronization, zero copying
let chunk = 12500
Parallel.each(iota(0, 8), fn (w) {
  for i in (w * chunk)..((w + 1) * chunk) {
    world[i].x = world[i].x + world[i].vx
    world[i].y = world[i].y + world[i].vy
  }
})

# the one and only lock shows up only when workers really do touch the same cell
@packable class Counter { n: Int64 = 0 }
let tally = SharedBuffer.new(1, Counter)
Parallel.each(iota(0, 8), fn (w) {
  tally.with_lock(fn () { tally[0].n = tally[0].n + 1 })
})
```

The line `world[i].x = world[i].x + world[i].vx` looks like nothing more
than an ordinary field assignment. But underneath it, zero-copy sharing is
happening across an isolate boundary — this is a fairly concrete
realization of the path he described, enclosing concurrency inside the API
so that it looks synchronous from outside. As long as the user is aware
that each worker is writing a disjoint range, everything else reads like
ordinary assignment to array elements.

This also answers the hurdle he described as "object graph independence
analysis." His idea was that the compiler checks whether a reference to
some graph is shared with anything else; culebra recasts that into the
shape of the data instead. `@packable` is a constraint declaring "a fixed
layout that contains no pointers" — without pointers there's no graph to
begin with, and without a graph there's nothing left for independence
analysis to analyze. The independence of disjoint elements is expressed not
through type checking but through index arithmetic, in a form anyone can
verify just by looking. The question he posed stays the same; only the
answer to it differs.

Worth a look, too, is what happens to locks. culebra has no first-class
Mutex value; `with_lock` appears only as a scoped method attached to the one
structure where sharing actually exists (`SharedBuffer`). He said "an API
above the level of threads, locks, and monitors" is needed; culebra's
answer, in that list, resembles a monitor (the idea of binding data and its
lock together, going back to Concurrent Pascal) — meeting the demand for
"an API above locks" not by removing locks, but by tying a lock to the data
it guards.

## Example 5: guaranteeing immutability — `Shared.new`

```culebra
# doctest: skip
# share one copy of huge read-only data across all workers (Rust's Arc<T>, roughly)
let model = Shared.new(load_weights())
let vocab = Shared.new(JSON.parse(FS.read("vocab.json")))

let outs = Parallel.map(batches, fn (b) {
  infer(model, vocab, b)  # every worker reads the same frozen tree. no copying
})
# vocab["hello"] = 1  →  ImmutableError (every write surface is rejected at runtime)
```

Hejlsberg put "guaranteeing object immutability" at the top of his list of
hurdles, and assumed it was the compiler's job. C# has advanced there
incrementally at the type level with `readonly`, `init`, and records, but
still hasn't reached deep, transitive immutability (a `List` inside a
record can still be freely mutated). culebra's answer is to treat
immutability as a property of the value, not of the type: `Shared.new`
transitively freezes the entire value tree, and every write to it after
that is rejected at runtime as `ImmutableError`. That falls short of a
static guarantee (Rust's `&`), but it does seem fair to say it achieves —
by paying a different cost, dynamic checking — the deep, transitive,
inescapable immutability that C# still doesn't have after fifteen-plus
years.

And the traversal that does the freezing uses exactly the same code as the
serialization used to cross an isolate boundary. So "being immutable" and
"being safe to share" line up by definition. It's a cruder approach than
Rust's, which splits Send and Sync into separate traits, but it's a
reasonably coherent single principle.

## Summary: where culebra stands against what Hejlsberg said

| What he said | culebra's answer | Where the means differ |
|---|---|---|
| An API above the level of threads, locks, and monitors | `Proc.all` / `Parallel.*` / `Channel` — thread/lock/monitor never surface | Closer to removal than abstraction (locks are confined to the one place attached to `SharedBuffer`) |
| Compiler support for immutability, purity, and object-graph independence | Sendable boundary checking + `Shared.new` freezing + `@packable` layout constraints | Static analysis replaced wholesale by runtime structural constraints |
| Enclosing concurrency inside the API | `Parallel.map`'s worker-pool concealment, `SharedBuffer`'s zero-copy sharing that takes the shape of a field assignment | "Looks synchronous, is parallel underneath" built on ordinary assignment syntax rather than a dedicated one |
| A high-level API aimed at data parallelism | `Parallel.map` (script layer) + Tensor (C++/GPU kernel layer) | A fairly close realization, though the user has to declare "here's the data" across three explicit lanes |
| Message passing (as one example among others) | Channel (CSP) + Proc | What was "one example from distributed systems" gets promoted to the default in-process communication mechanism |

Taken together, there's something like a single pattern running through
all of this. Hejlsberg's proposals, on the whole, started from the premise
of keeping the imperative, shared-heap language as it is and layering a
compiler and an API on top to make it safe — a natural premise, presumably,
for the designer of a widely used language like C#. culebra, being
unreleased, has the freedom to pick largely the opposite move at every one
of these points: instead of analyzing, it removes sharing entirely; instead
of writing immutability into the type, it freezes the value; instead of
abstracting the lock away, it constrains the whole structure that would
need one. That's closer to the answer Erlang gave in the 1980s — change the
semantics of the language itself — and it puts the model he set aside as
"just a small slice" at the center of the design instead.

Let me acknowledge honestly, as a last point, one place where his outlook
gets ahead of culebra. Deciding "here" in "here's the data, here's the
computation" turns out to be the genuinely hard part. culebra's three
lanes — copying (channel), a frozen reference (`Shared`), a fixed layout
(`SharedBuffer`) — are a scheme that requires the user to declare a
placement and a property for their data, and that hasn't reached the "API
that doesn't make you think about placement" he envisioned. Making the user
think about it is the price paid, in a dynamically typed language, to make
data races disappear — that's culebra's design call — but the reality more
than fifteen years on, that no language has gotten away without paying some
price (Rust makes you pay in type annotations, Swift in actor isolation),
might be the deepest confirmation of his very first prediction: the magic
compiler switch was never going to come.

## Source

Federico Biancuzzi, Shane Warden (eds.), *Masterminds of Programming:
Conversations with the Creators of Major Programming Languages*, O'Reilly
Media, 2009. 494 pages, ISBN 978-0-596-51517-1. Chapter 13, "C#," is the
Anders Hejlsberg interview. A Japanese translation exists as *言語設計者たち
が考えること* (Masakazu Murakami, Yoshikazu Sato, Masahiro Ito, Kazuyoshi
Kaesue, Yukitoshi Suzuki, trans.; O'Reilly Japan, published September 25,
2010; 536 pages, ISBN 978-4-87311-471-2).

This essay's summary is a re-translation once removed from the original
English: it works from that Japanese translation, restates the excerpt in
my own words in Japanese, and this English version is a translation of
*that* restatement — not a direct rendering of Hejlsberg's original
English. The exact wording at any of those steps may not match his actual
words.
