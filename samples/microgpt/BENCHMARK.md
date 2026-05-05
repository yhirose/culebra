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

Wall time of training only (`n_samples=0` to skip inference).

| Implementation                 | 5 steps | 20 steps | per step | vs. Python |
|--------------------------------|--------:|---------:|---------:|-----------:|
| Python (CPython, scalar)       |  0.40 s |   1.48 s |   ~74 ms |         1× |
| Culebra `--jit` (imperative)   |  1.29 s |   2.26 s |  ~113 ms |      ~1.5× |
| Culebra `--jit` (HOF / lambda) |  1.55 s |   2.92 s |  ~146 ms |      ~2.0× |
| Culebra interpreter            | 14.0 s  |  55.7 s  | ~2,800 ms |    ~37.6× |

`per step` is from the 20-step run (5-step is JIT-warmup-heavy
and overstates per-step cost). At steady-state on `--jit`, the
imperative Culebra port runs scalar microgpt within **1.5× of CPython**.

The "HOF / lambda" row is `microgpt_hof.cul` — the same algorithm
written in functional style with `arr.map(|x| ...)` /
`.reduce(init, |a, b| ...)` chains rather than imperative `while`
loops. About 26% shorter than the imperative version (286 vs 384
lines). JIT inlines literal-lambda callbacks for `map`, `filter`,
`for_each`, `reduce` (Array) and `reduce` / `for_each` (Iterator),
so the per-element closure invocation overhead has mostly been
absorbed.

The two Culebra backends produce **identical loss values** at every
step (final loss after 20 steps: 2.413316935338878…), confirming
`--jit` matches the interpreter bit-for-bit. Python's loss diverges
because Python `random.shuffle` and Culebra `Random.shuffle` produce
different sequences from the same MT64 seed — expected, not a
correctness issue.

### 1000-step extrapolation (a real training run)

| Implementation              | Wall time |
|-----------------------------|----------:|
| Python                      |    ~74 s  |
| Culebra `--jit` imperative  |    ~1.9 min |
| Culebra `--jit` HOF         |    ~2.4 min |
| Culebra interpreter         |    ~46 min |

`--jit` imperative is now within ~1.5× of CPython on this workload —
practically usable for actual scalar autograd training runs.

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

The full optimization round took the JIT imperative from ~10× of
CPython to ~1.5× at steady-state.

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
set of optimizations above, the gap to CPython is the honest ~1.5×
shown in the steady-state column rather than the 330–2000× we saw
before.

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

**Practically usable**: extrapolated to 1000 steps, `--jit` finishes
in about ~1.9 minutes (Python ~74 s, ratio ~1.5×); the HOF lambda
variant in ~2.4 minutes; the interpreter in ~46 minutes. The JIT
imperative form is genuinely competitive with CPython for scalar
autograd training.

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
this point, additional speedup requires structural changes — most
notably a Matrix-based formulation that collapses per-step Object
count, or a different value representation. Those are open as
future work; the scalar port is closed at this performance level.
