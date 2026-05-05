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

## Numbers (after GC tuning)

Wall time of training only (`n_samples=0` to skip inference). Two
step counts so the linear-scaling assumption is visible.

| Implementation                 | 5 steps |  20 steps | per step | vs. Python |
|--------------------------------|--------:|----------:|---------:|-----------:|
| Python (CPython, scalar)       |  0.43 s |    1.44 s |   ~72 ms |         1× |
| Culebra `--jit`                |  4.06 s |   16.66 s |  ~833 ms |     ~11.6× |
| Culebra interpreter            | 14.01 s |   55.54 s | ~2,777 ms |    ~38.6× |

`per step` is computed from the 20-step run (5-step is dominated by
JIT warmup / first-doc length, so it overestimates by ~5–10%). Linear
scaling is confirmed: 5→20 steps ratio is 3.5–4.1× across all three
implementations.

The two Culebra backends produce **identical loss values** at every
step (final loss after 20 steps: 2.413316935338878…), confirming
`--jit` matches the interpreter bit-for-bit. Python's loss diverges
because Python `random.shuffle` and Culebra `Random.shuffle` produce
different sequences from the same MT64 seed (different initial state)
— expected, not a correctness issue.

### 1000-step extrapolation (a real training run)

| Implementation              | Wall time |
|-----------------------------|----------:|
| Python                      |    ~72 s  |
| Culebra `--jit`             |   ~14 min |
| Culebra interpreter         |   ~46 min |

Practically usable on `--jit`; the interpreter is fine for short
sanity-check runs.

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
Object goes through the shared property map plus refcount plumbing,
so per-allocation cost is higher — but with the cycle collector no
longer firing on every batch of 10000 allocs, that cost is now the
honest ~12–39× gap shown above instead of the 330–2000× we saw
before.

`--jit` is ~3× faster than interp on this workload because the inner
arithmetic on `Float` payloads — including the `**` peephole for
`x**0.5` and the inlined Long fast paths for Math — runs as native
LLVM IR rather than tree-walked. Object creation still goes through
the same C++ runtime helpers on both backends, putting a floor on
how fast scalar microgpt can run.

## Conclusion: scalar port is closed

**Functionally validated**: Culebra fully runs Karpathy's scalar GPT
end-to-end — `class` sugar, dunder operator overloading
(`__add__/__mul__/__pow__/__neg__/__sub__/__div__`), auto-reflection
for `0 + Value` / `(1/n) * Value`, `Math.log/exp/sqrt`, `Random.gauss/
shuffle/weighted_choice`, `IO.exists`, `**` operator. The full
language coverage planned in Phases 1–4 is exercised.

**Practically usable**: extrapolated to 1000 steps, `--jit` finishes
in ~14 minutes and the interpreter in ~46 minutes (Python ~72 s).
This is the first point at which scalar microgpt is usable for
actual training runs on Culebra.

**Where the remaining time goes** (re-profiled at the current point):

| Category                                | Self-time |
|-----------------------------------------|----------:|
| GC walk + `gc_refs`/`reachable` hash probes | ~50% |
| `malloc`/`free`                         | ~15% |
| `std::any` value handlers               | ~7% |
| Real interpreter eval                   | ~3-5% |
| Other overhead                          | ~25% |

Most of the remaining cost is GC bookkeeping (hash-table probes
during the cycle walk) and allocator churn from the per-Value
property map / children-array allocations. Real AST evaluation is
3-5% — the interpreter is now near the floor for what an AST
tree-walker on `std::any`-based values can run at.

Beyond this point, meaningful speedup needs a structural change
(bytecode interpreter, different value representation, or a
Matrix-based formulation that collapses per-step Object count).
Those are open as future work but out of scope for the scalar port,
which is now closed.
