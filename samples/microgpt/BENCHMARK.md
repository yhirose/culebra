# Phase 5.1 benchmark: scalar microgpt port

Three implementations run Karpathy's scalar autograd microgpt with
matching hyperparameters (`n_layer=1`, `n_embd=16`, `block_size=16`,
`n_head=4`, 4192 parameters). PRNG seeds differ between Python's
`random` and Culebra's `Random` (both Mersenne-Twister-64 but distinct
initial sequences), so loss values aren't expected to match across
languages — they confirm the algorithm produces sensible numbers in
both, nothing more.

Hardware: Apple Silicon laptop, single core. Each run is wall time of
training only (`num_steps` configured, `n_samples=0` to skip inference).

## Numbers

Wall time of training only (`n_samples=0` to skip inference). Five runs;
mean / min reported.

| Implementation                 | 5 steps |   20 steps   | total/20 | incremental | vs. Python |
|--------------------------------|--------:|-------------:|---------:|------------:|-----------:|
| Python (CPython, scalar)       |  0.42 s | 1.43 / 1.31s |  ~71 ms  |     ~63 ms  |         1× |
| Culebra `--jit` (imperative)   |  1.31 s | 2.27 / 2.19s |  ~114 ms |     ~64 ms  |     ~1.60× |
| Culebra `--jit` (HOF / lambda) |  1.18 s | 2.13 / 2.06s |  ~107 ms |     ~63 ms  |     ~1.58× |
| Culebra interpreter            |  13.4 s |  ~55 s       | ~2,800 ms|   ~2,800 ms |    ~37.6× |

`total/20` averages JIT compilation + 20 training steps; `incremental`
isolates the per-step cost by `(20 step − 5 step) / 15` so JIT warmup
(~1 s) doesn't pollute the rate. At **steady state, both `--jit` styles
match CPython per-step (~63 ms)**; the residual ~1.6× on total wall
time is the JIT-compilation warmup that does not amortize over a
20-step run.

The "HOF / lambda" row is `microgpt_hof.cul` — the same algorithm
written in functional style with `arr.map(|x| ...)` /
`.reduce(init, |a, b| ...)` chains rather than imperative `while`
loops. About 26% shorter than the imperative version. JIT inlines
literal-lambda callbacks for `map`, `filter`, `for_each`, `reduce`
(Array) and `reduce` / `for_each` / `map.collect` (Iterator), and
fuses `Math.range(N).<HOF>(...)` chains into direct counter loops —
so the HOF style now slightly beats the imperative one on this
workload (the imperative version still uses `while`-loop boilerplate
that's not as obviously specialised by the JIT).

The two Culebra backends produce **identical loss values** at every
step (final loss after 20 steps: 2.413316935338878…), confirming
`--jit` matches the interpreter bit-for-bit. Python's loss diverges
because Python `random.shuffle` and Culebra `Random.shuffle` produce
different sequences from the same MT64 seed — expected, not a
correctness issue.

### 1000-step extrapolation (a real training run)

Linear model: `wall ≈ warmup + N × incremental`.

| Implementation              | Wall time |
|-----------------------------|----------:|
| Python                      |    ~67 s  |
| Culebra `--jit` imperative  |    ~65 s  |
| Culebra `--jit` HOF         |    ~64 s  |
| Culebra interpreter         |    ~46 min |

At a real training-run length, **the JIT essentially matches CPython** on
both styles. The 20-step "1.6× of Python" headline is dominated by
JIT-compile warmup (~1 s out of 2.27 s); past 30–40 steps the warmup
is amortized away.

### Where the speedup came from

Earlier numbers (`--jit`: 1m38.9s, interp: 10m04.4s) were dominated
by the cycle collector. Profiling with `sample` showed `InterpGC::
collect()` and friends at 85-95% of self-time. Two narrow changes
to the GC fixed it:

1. **Adaptive collect threshold.** A fixed 10000-allocation threshold
   made collect fire ~30 times per training step on microgpt's
   ~3×10⁵ allocs, and each run scanned the entire live set — total
   GC work was O(N²). After each collect we now re-arm to
   `max(MIN, 2 × survivors)`, the CPython/V8 heuristic. Collect rate
   becomes O(log N) and total work O(N log N). This alone recovered
   ~12× on JIT and ~24× on the interpreter.

2. **Track only ArrayValue's value vector, not Object property maps.**
   microgpt cycles always route through `_children` (an Array), so
   the Array's tracked vector is enough to detect and break them.
   Halves tracker entries and per-collect walk; the GC walk descends
   one level through Object property maps to surface the Arrays
   inside. Generic Object→Object cycles without an Array in between
   no longer break automatically — Culebra has always called cycles
   a hazard and recommends `drop` / weak refs, so this is an
   acceptable narrowing.

Together these took the interpreter 5-step from ~10 minutes to 14 s
and the JIT 5-step from ~1m39s to 4 s — about 43× and 24×
respectively, in five commits totaling roughly 30 lines of net
change.

A subsequent JIT-side optimization round narrowed the gap further:

3. **Prototype delegation in JIT class instances.** Each instance
   used to copy method closure entries into its property map; with
   ~9 dunders × ~10⁵ instances, the per-instance method props
   dominated the cycle collector's per-Object walk. Methods now
   live on a shared per-class meta object pointed at via a
   `JitObject::proto` field, and lookup falls through to it. Cut
   per-instance own-prop count from ~14 to ~5.

4. **Vector storage for `JitObject::props`.** Replaced the
   `std::map<string, JitObjectEntry>` with a thin
   `vector<pair<...>>` wrapper. With ~5 props per object, linear
   search beats a red-black tree on cache traffic.

5. **Index-keyed vectors inside `_do_collect`.** Replaced two
   `unordered_map`/`unordered_set` keyed by raw pointer with dense
   vectors keyed by snapshot index. Removed ~12% of self-time spent
   in hash table operations.

6. **Generational GC.** Split the tracker into `young` and `old`.
   Each collect walks only `young`, then promotes survivors to
   `old` (which is never re-scanned). For workloads with two-tier
   object lifetimes — params + class metadata vs per-step
   intermediates — this drops GC time from ~60% to ~3%.

7. **Vector-backed young + per-object `gc_slot`.** `_gc().add` was
   still 31% of self-time inside `unordered_map::emplace`. Replace
   `young` with a flat vector + per-object slot index field (added
   to JitArray / JitObject / JitCell / JitClosure). Add and remove
   are now O(1) push / swap-pop with no per-call heap allocation.

After the GC round, profiling pointed at the per-instance property
map next: `_platform_memcmp` (string compares inside std::map find)
and `culebra_runtime_object_get` together accounted for ~15% of
self-time. A Python `__slots__`-style hidden-class layout dropped
that further:

8. **Shape-based JitObject layout.** Replace each instance's
   per-object property map with a process-interned `Shape*`
   (V8/SpiderMonkey hidden class) plus a `vector<JitObjectEntry>
   slots` of values only. Two instances with the same property set
   share a `Shape` pointer; a transition cache on each `Shape`
   keeps `transition_add` O(1) on the second (and later) write of
   the same name. This is the JIT analogue of Python's `__slots__`
   — fixed-offset attribute access in place of per-instance
   key/value pairs.

9. **Inline cache for property reads.** Per-callsite IC global
   `{Shape*, slot_offset}` allocated by the JIT. Fast path is
   inlined IR — load `obj->shape`, compare with the cached shape,
   and on hit load `slots.data()[offset].value` directly with no
   runtime call. Slow path (`culebra_runtime_object_get_ic`) does
   the lookup and refreshes the IC. Microgpt's `Value` class has
   a stable shape after construction, so almost every read after
   the first hits the inline path.

10. **Linear scan over `Shape::names`.** With property reads
    keying on shape pointer (cached in the IC), the std::map of
    name → offset on each `Shape` was only consulted on cache
    miss. For the typical 5–15 properties per shape, a flat scan
    over the `names` vector beats both std::map and unordered_map
    on cache traffic.

11. **Pre-reserve 8 slots on first append.** `JitObject::append_slot`
    was triggering 3–4 reallocations as `std::vector<JitObjectEntry>`
    doubled (cap 1→2→4→8) during construction. Reserve 8 up front:
    most JitObjects in this codebase end up with ≤ 8 own properties,
    so the doubling chain collapses to a single allocation.

12. **Property-write inline cache.** Mirror the read-side IC for
    `obj.prop = ...`: each call site caches `{expected_shape,
    result_shape, offset, prop_mut}`. Fast path is a single shape
    compare in IR; on hit, a tiny helper handles either the update
    case (overwrite `slots[offset]`) or the transition case (push
    slot, bump shape). Removes the `find_slot` linear scan and the
    `transition_add` map lookup from the hot path.

13. **Skip `__ARGS__` Array allocation when unused.** The function
    prologue used to unconditionally build an Array of overflow args
    even when the body never referenced `__ARGS__`. New
    `FuncInfo::uses_args` analysis (set when an `__ARGS__` IDENTIFIER
    appears) gates the build; otherwise a cheap conditional release
    of the slab retains replaces it.

The optimization round through (13) took the JIT imperative from ~10× of
CPython to ~1.5× at steady-state. A second round added on later
(2026-04-29) closed the remaining gap and pulled HOF style up to the
same level:

14. **Drop per-instance `class_name` heap copy.** `build_class_instance`
    used to call `_culebra_heap_str(class_name)` for every constructor
    invocation, mallocing + copying a 5-byte string into a fresh
    buffer. `class_name` is already a process-lifetime LLVM module
    global and TAG_STRING values are borrowed (no refcount), so we
    can stash the pointer directly. Cuts ~10⁵ mallocs per step on
    microgpt; ~5% wall-time saving on imperative.

15. **Fuse `iter.map(λ).collect()`.** Postfix-loop AST detection in
    `compile_call`: when a chain matches `<iter>.map(λ).collect()` and
    the lambda is inlinable, emit a single inline loop that allocates
    one output Array and pushes each per-element result, skipping the
    `iter_map` runtime closure wrapper that the lazy path would build.

16. **Fuse `Math.range(N).<HOF>(...)` to a counter loop.** The biggest
    HOF-style win. Detect `Math.range(N).reduce(init, λ)`,
    `.for_each(λ)`, and `.map(λ).collect()` at the AST level and emit
    a direct i64 counter loop (`for i in 0..N`) with the body inlined.
    Skips the `culebra_runtime_math_range` wrapper-iterator + capture
    cells (~17% of HOF microgpt's runtime) and the per-element
    trampoline closure call. With this in place, the HOF style went
    from ~3.0 s to ~2.13 s on 20-step microgpt — ~30% faster, finally
    matching imperative.

Reproduce:

```
just fetch-names                                              # one time
python3 samples/microgpt/microgpt.py 5 0     # 5 steps, no inference
./build/culebra       samples/microgpt/microgpt.cul -- 5 0
./build/culebra --jit samples/microgpt/microgpt.cul -- 5 0

# Or 20 steps for steady-state per-step rate:
python3 samples/microgpt/microgpt.py 20 0
./build/culebra       samples/microgpt/microgpt.cul -- 20 0
./build/culebra --jit samples/microgpt/microgpt.cul -- 20 0
```

## What this measures

The scalar autograd formulation creates one `Value` Object per
intermediate arithmetic result. Each forward pass through 16 tokens
produces ~10⁵ Objects, each carrying a property map, refcount slot,
and cycle-collector registration. Over 5 training steps that's roughly
**10⁶ Object allocations and dunder dispatches** — the dominant cost
on every backend.

CPython's `__slots__` keeps each `Value` in a fixed-layout C struct
allocated and freed in tens of nanoseconds. Culebra's `class`-sugar
Object now does the same thing structurally: a process-interned
`Shape` shared across instances, slot offsets resolved at JIT compile
time via inline caches on both reads and writes, and direct
`slots.data()[offset].value` access on the fast path. After the full
set of optimizations above, the steady-state per-step cost matches
CPython (~63 ms either way), with the residual 20-step total wall
ratio (~1.6×) being JIT compile warmup amortizing over a longer run.

`--jit` is ~25× faster than interp on this workload (2.26 s vs 55.7 s
at 20 steps) because the inner arithmetic on `Float` payloads —
including the `**` peephole for `x**0.5` and the inlined Long fast
paths for Math — runs as native LLVM IR rather than tree-walked,
and after the shape + IC rounds property reads and writes in the hot
path are fixed-offset slot accesses with no runtime dispatch. The
interpreter has not received the JIT-side optimization rounds listed
above and remains the proportional figure (~38× of CPython).

## Conclusion

**Functionally validated**: Culebra fully runs Karpathy's scalar GPT
end-to-end — `class` sugar, dunder operator overloading
(`__add__/__mul__/__pow__/__neg__/__sub__/__div__`), auto-reflection
for `0 + Value` / `(1/n) * Value`, `Math.log/exp/sqrt`, `Random.gauss/
shuffle/weighted_choice`, `IO.exists`, `**` operator. The full
language coverage planned in Phases 1–4 is exercised.

**Practically usable**: extrapolated to 1000 steps, both JIT styles
finish in ~64–65 s vs CPython's ~67 s — Culebra is at parity with
CPython on this workload at real training-run lengths. The interpreter
takes ~46 minutes (used as a correctness reference, not a runtime
target).

**Where the remaining JIT time goes** (re-profiled after the full
optimization round, sampled on a 100-step `--jit` run):

| Category                                            | Self-time |
|-----------------------------------------------------|----------:|
| Allocator churn (`operator new`, `nanov2_*`, free)  |     ~8.8% |
| GC tracker (`add`, `_do_collect`, `enumerate_children`) | ~7.1% |
| Refcount traffic (`retain`, `release_impl`)         |     ~5.0% |
| Allocation creators (`array_new`, `build_class_instance`) | ~3.6% |
| Property write residual (IC fast path call only)    |     ~2.2% |
| Number arithmetic helpers                           |     ~1.1% |
| JIT-compiled native code + system stubs             |    ~72.0% |

Property reads and writes — formerly the dominant `_platform_memcmp`
and `culebra_runtime_object_set` lines — are no longer in the top
profile entries: read and write inline caches both inline the fast
path in IR with no runtime call. What remains is the fundamental
cost of the value model: refcount, the cycle collector's
unavoidable per-allocation registration, and allocator churn. Beyond
this point, candidates for further work include a Matrix-based
formulation that collapses per-step Object count, a bump allocator
for short-lived `Value`s, or a different value representation
(NaN-boxing). At parity with CPython per-step the scalar port has
hit its natural design ceiling; bigger wins now need structural
changes.
