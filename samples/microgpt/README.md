# microgpt on Culebra

Karpathy's scalar autograd microgpt, ported to Culebra. Used as a
benchmark and language-coverage exercise (`class` sugar, dunder
overloading, auto-reflection, `Math.*`, `Random.*`).

## Files

- `microgpt.py`     — Karpathy's reference (Python).
- `microgpt.cul`    — Culebra port, HOF-idiomatic style.
- `names.txt`       — Training data (gitignored). Run `just fetch-names`.
- `sidebyside.html` — Static side-by-side viewer of both files.

## Running

```
just fetch-names
python3 samples/microgpt/microgpt.py [num_steps]
./build/culebra       samples/microgpt/microgpt.cul -- [num_steps]
./build/culebra --jit samples/microgpt/microgpt.cul -- [num_steps]
```

`num_steps` defaults to 1000. Pass a second arg to `n_samples` (default
5 / 20); `0` skips inference.

## Numbers

Apple Silicon, single core, mean of 5 runs, training only (`n_samples=0`):

| step  | Python  | Culebra `--jit` | Culebra interp | ratio (Cul/Py) |
|-------|--------:|----------------:|---------------:|---------------:|
|    20 |  1.39 s |          1.99 s |         ~55  s |          1.43× |
|   100 |  6.81 s |          5.77 s |              — |    **0.85×**   |
|   200 | 13.06 s |         10.85 s |              — |    **0.83×**   |

**Below ~50 steps Python wins** because Culebra pays ~1 s of JIT
warmup. Past that the per-step rate dominates: Culebra **~51 ms**
vs Python **~63 ms** (100 → 200 incremental). 1000-step extrapolation:
Culebra ~52 s vs Python ~63 s — Culebra ~17% faster.

Both Culebra backends produce identical loss values bit-for-bit;
Python diverges due to a different Mersenne-Twister shuffle sequence
(expected, not a correctness issue).

## Where time goes (`--jit`, 500-step, 8 s sample post-warmup)

| Category                                 | Inclusive |
|------------------------------------------|----------:|
| `Value.__add__` / `.__mul__` (dispatch + body) | ~53% |
| `build_class_instance` (Value allocation)| ~30% |
| Cycle GC (`_do_collect`)                 | ~17% |
| Refcount drop (`_culebra_value_release_impl`) |  ~5% |

(Inclusive = subtree time, so rows overlap. The genuine bottleneck is
the per-step `Value` object lifecycle.)

Further wins now need structural changes:
- **Bump allocator** for short-lived `Value`s — skip nanov2 entirely.
- **NaN-boxing** — collapse `Value` to an unboxed double.
- **Matrix formulation** — drop per-step Object count by orders of
  magnitude (Phase 5.4).

## Optimization history

The JIT scalar microgpt improved ~25× from the starting baseline,
across four themes (see git log for per-commit detail):

**GC** (was 85–95% of self-time at the start):
adaptive collect threshold, track only `Array`-routed cycles,
generational young / old split, vector-backed `young` with per-object
`gc_slot`, drop `unordered_map` from `_do_collect`.

**Object layout** (Python `__slots__` analogue):
process-interned `Shape` (V8/SpiderMonkey hidden class), prototype
delegation for class methods, vector storage for properties, slot
pre-reserve.

**Inline caches** (skip runtime dispatch on the hot path):
property-read IC and property-write IC, both with the fast path
inlined as IR.

**HOF fusion** (close the gap for functional style):
inlined Array `map`/`filter`/`for_each`/`reduce`, inlined Iterator
`reduce`/`for_each`/`map.collect`, and `Math.range(N).<HOF>(...)`
chains fused into direct counter loops.

Plus narrow wins: skip per-instance `class_name` heap copy, omit
`__ARGS__` allocation when the body never reads it.
