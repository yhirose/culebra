# microgpt on Culebra

Karpathy's scalar autograd microgpt, ported to Culebra. Used as a
benchmark and language-coverage exercise (`class` sugar, operator
overloading via special methods, auto-reflection, `Math.*`, `Random.*`).

## Files

- `microgpt.py`         — Karpathy's reference (Python).
- `microgpt.cul`        — Culebra port, scalar autograd, HOF-idiomatic style.
- `microgpt_tensor.cul` — Tensor port: same architecture but single-head
                          attention, autograd built on Phase 1 Tensor + TNode.
                          ~15× faster per step than the scalar version.
- `names.txt`           — Training data (gitignored). Run `just fetch-names`.
- `sidebyside.html`     — Static side-by-side viewer with two tabs:
                          Python ↔ Culebra scalar (algorithm correspondence)
                          and Culebra scalar ↔ Tensor (port progression).
- `build_sidebyside.py` — Regenerator for `sidebyside.html`. Re-run after
                          any edit to the three source files; section
                          line ranges live in `SECTIONS_*` near the top.

## Running

```
just fetch-names
python3 benchmarks/microgpt/microgpt.py [num_steps]
./build/culebra       benchmarks/microgpt/microgpt.cul [num_steps]
./build/culebra --jit benchmarks/microgpt/microgpt.cul [num_steps]
```

`num_steps` defaults to 1000. Pass a second arg to `n_samples` (default
5 / 20); `0` skips inference.

## Numbers

Apple Silicon, single core, training only (`n_samples=0`).

### Scalar microgpt (mean of 5 runs)

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

### Tensor microgpt (`microgpt_tensor.cul`)

Same architecture as the scalar version (n_head=4, head_dim=4) —
no remaining differences. Single-run wall time, 100 training steps,
JIT, n_samples=0:

| implementation         |  total wall |  ms/step |
|------------------------|------------:|---------:|
| Culebra Tensor (`--jit`) |    ~4.5 s |   ~2.8 ms |
| Culebra scalar (`--jit`) |     6.21 s |    ~52 ms |

The Tensor wall is now JIT-compile-warmup-dominated: ~4.2 s compiling
the module up front, then only ~0.28 s in the 100-step train loop
(2.8 ms/step). Compare per-step rate, not total wall — at this size
warmup swamps the loop, so total-wall is mostly a compile-time figure.

Most of that warmup is the LLVM backend (instruction selection +
register allocation), not the IR passes. `--jit-fast` runs the backend
with FastISel, roughly halving warmup; because the Tensor port's hot
compute is in BLAS rather than JIT-emitted code, per-step is unchanged.
It is the opposite trade for the scalar port — there all arithmetic is
JIT'd, so `--jit-fast` slows per-step.

`--jit-fast` is **experimental** and not yet interp-symmetric: it
miscompiles `for-in` over iterator objects (FastISel materializes a
reachable `undef`-data Value as garbage). Use it only for BLAS-bound
runs that avoid that pattern. The cheaper, byte-identical way to cut
warmup is the on-disk object cache (`CULEBRA_JIT_CACHE=auto`), which
reloads the optimized O2 code with no per-step penalty.

The Tensor port is **~18× faster per step** than the scalar version.
The scalar microgpt builds thousands of `Value` objects per training
step (one per scalar arithmetic op); the Tensor port builds a few
hundred `TNode`s per step (one per layer-level op + per-head slice
+ concat) and routes each linear into BLAS.

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
`reduce`/`for_each`/`map.collect`, and `range(N).<HOF>(...)`
chains fused into direct counter loops.

Plus narrow wins: skip per-instance `class_name` heap copy, omit
`__ARGS__` allocation when the body never reads it.
