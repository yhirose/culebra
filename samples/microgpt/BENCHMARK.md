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
| Culebra `--jit` (imperative)   |  1.51 s |   3.94 s |  ~197 ms |      ~2.7× |
| Culebra `--jit` (HOF / lambda) |  2.11 s |   6.76 s |  ~338 ms |      ~4.6× |
| Culebra interpreter            | 14.0 s  |  55.7 s  | ~2,800 ms |    ~37.6× |

`per step` is from the 20-step run (5-step is JIT-warmup-heavy
and overstates per-step cost). At steady-state on `--jit`, the
imperative Culebra port runs scalar microgpt within **2.7× of CPython**.

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
| Culebra `--jit` imperative  |    ~3.3 min |
| Culebra `--jit` HOF         |    ~5.6 min |
| Culebra interpreter         |    ~46 min |

`--jit` imperative is now within ~2.7× of CPython on this workload —
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

The full optimization round took the JIT imperative from ~10× of
CPython to ~2.7× at steady-state.

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
Object still goes through the shared property map plus refcount
plumbing — but after the optimization rounds above (proto delegation,
generational GC, vector-backed tracker), the gap to CPython is the
honest ~2.7× shown in the steady-state column rather than the 330–2000×
we saw before.

`--jit` is ~14× faster than interp on this workload (3.94 s vs 55.7 s
at 20 steps) because the inner arithmetic on `Float` payloads —
including the `**` peephole for `x**0.5` and the inlined Long fast
paths for Math — runs as native LLVM IR rather than tree-walked.
The interpreter has not received the JIT-side optimization rounds
listed above and remains the proportional figure (~38× of CPython).

## Conclusion

**Functionally validated**: Culebra fully runs Karpathy's scalar GPT
end-to-end — `class` sugar, dunder operator overloading
(`__add__/__mul__/__pow__/__neg__/__sub__/__div__`), auto-reflection
for `0 + Value` / `(1/n) * Value`, `Math.log/exp/sqrt`, `Random.gauss/
shuffle/weighted_choice`, `IO.exists`, `**` operator. The full
language coverage planned in Phases 1–4 is exercised.

**Practically usable**: extrapolated to 1000 steps, `--jit` finishes
in about ~3.3 minutes (Python ~74 s, ratio ~2.7×); the HOF lambda
variant in ~5.6 minutes; the interpreter in ~46 minutes. The JIT
imperative form is genuinely competitive with CPython for scalar
autograd training.

**Where the remaining JIT time goes** (re-profiled after the most
recent round on a 500-step `--jit` run):

| Category                                            | Self-time |
|-----------------------------------------------------|----------:|
| `_gc().add` (vector push, was hash insert pre-fix)  |       ~5% |
| `_GcTracker::_do_collect` body                      |       ~4% |
| Native JIT-compiled code (`<unknown binary>` blocks) |     ~10% |
| `culebra_runtime_object_get` / `_set`               |       ~5% |
| `_culebra_value_release_impl` + `value_retain`      |       ~3% |
| `nanov2_*` (allocator churn)                        |       ~9% |
| Real interpreter eval helpers (dunder, num ops)     |       ~5% |
| Other (system, dyld stubs, stdlib)                  |     ~59% |

Most of the per-step cost has shifted out of the GC and into the
ordinary Object-construction / property-access path. Beyond this
point, additional speedup requires structural changes — most
notably a Matrix-based formulation that collapses per-step Object
count, or a different value representation. Those are open as
future work; the scalar port is closed at this performance level.
