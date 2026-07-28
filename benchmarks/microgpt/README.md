# microgpt on Culebra

Karpathy's scalar autograd microgpt, ported to Culebra. Used as a
benchmark and language-coverage exercise (`class` sugar, operator
overloading via special methods, auto-reflection, `Math.*`, `Random.*`).

## Files

- `microgpt.py`         — Karpathy's reference (Python).
- `microgpt.cul`        — Culebra port, scalar autograd, HOF-idiomatic style.
- `microgpt_tensor.cul` — Tensor port: same architecture, autograd built into the
                          C++ Tensor primitive (native reverse-mode), so the
                          forward records the graph and `loss.backward()` walks it
                          in C++. ~50–100× faster per step than the scalar version.
- `microgpt_tensor.py`  — Same Tensor architecture, PyTorch reference (CPU,
                          single-threaded via `torch.set_num_threads(1)`) for
                          per-step comparison against `microgpt_tensor.cul`.
- `names.txt`           — Training data (gitignored). Run `just fetch-names`.
- `sidebyside.html`     — Static side-by-side viewer: Python ↔ Culebra scalar
                          (algorithm correspondence).
- `build_sidebyside.py` — Regenerator for `sidebyside.html`. Re-run after any
                          edit to `microgpt.py` / `microgpt.cul`; section line
                          ranges live in `SECTIONS_PY_SCALAR` near the top.

## Running

```
just fetch-names
python3 benchmarks/microgpt/microgpt.py [num_steps]
python3 benchmarks/microgpt/microgpt_tensor.py [num_steps]
./build/culebra       benchmarks/microgpt/microgpt.cul [num_steps]
./build/culebra --jit benchmarks/microgpt/microgpt.cul [num_steps]
./build/culebra       benchmarks/microgpt/microgpt_tensor.cul [num_steps]
./build/culebra --jit benchmarks/microgpt/microgpt_tensor.cul [num_steps]
```

`num_steps` defaults to 1000. Pass a second arg to `n_samples` (default
5 / 20); `0` skips inference.

## Numbers

Apple Silicon, single core, training only (`n_samples=0`), measured
2026-07-17.

Compare the **per-step rate** (steady state), not total wall: the scalar
port's wall includes ~1–2 s of JIT warmup, and the Tensor port's wall is
warmup-dominated (compute is in BLAS, not JIT'd code).

### Tensor microgpt (`microgpt_tensor.cul`) — the showcase

Same architecture as the scalar version (n_head=4, head_dim=4), with the
autograd built into the C++ Tensor primitive (native reverse-mode).
Per-step (steady-state train loop, n_samples=0):

| implementation              | ms/step |
|-----------------------------|--------:|
| Culebra Tensor `--jit`      | **~1.15 ms** |
| Culebra Tensor interp       | ~5.3 ms |
| Python (scalar reference)   | ~83 ms |

The Tensor port is **~50–100× faster per step than the scalar port** (one
layer-level op per Tensor node + BLAS, instead of thousands of per-scalar
`Value` objects). interp and JIT produce **bit-identical loss**. The total
wall (~2.5 s for 100 steps) is mostly the up-front JIT compile, not the
loop (~0.1 s) — `--jit-faststart` or the on-disk object cache
(`CULEBRA_JIT_CACHE=auto`) cut that warmup without per-step cost.

### Scalar microgpt — alloc-bound, ~parity with Python

The scalar port builds thousands of `Value` objects per step (one per
scalar arithmetic op), so it is **alloc-bound**: GC + refcount + malloc are
~55% of steady-state time. Current per-step is **slower than Python**
(JIT ~149 ms/step vs Python ~83 ms/step on this machine; absolute figures
drift with load — compare ratios).

An earlier version of this README reported the scalar JIT *beating* Python
(~0.85×). That advantage relied on the old minor-only cycle collector,
which was **fast but leaked** (microgpt grew to ~5 GB). The current
conservative mark-sweep backstop is sound (no leak) but pays a real cost on
this GC-bound workload — roughly the ~1.8× difference. Recovering it
precisely (Julia-style shadow-stack rooting → cycle-only collection) is
tracked separately; the decisive-speed answer for ML on Culebra is the
**Tensor** path above. Both backends still produce bit-identical loss
(Python diverges only via a different Mersenne-Twister shuffle, expected).

## Where time goes (`--jit`, 500-step, 8 s sample post-warmup)

| Category                                 | Inclusive |
|------------------------------------------|----------:|
Leaf self-time (a fresh sample), grouped — memory management dominates:

| Category                                   | Self-time |
|--------------------------------------------|----------:|
| GC (mark-sweep + registry: `collect_impl` / `adopt` / `enumerate_children` / sweep / madvise) | ~28% |
| Refcount (`_culebra_value_release_impl` / `retain` / swap) | ~19% |
| malloc / free (nanov2)                     | ~8% |
| thread-local access (`_tlv_get_addr`)      | ~6% |
| shape / key compare (`memcmp`)             | ~5% |

The genuine bottleneck is the per-step `Value` object lifecycle: each scalar
op creates one heap `Value` (a class instance) + two `JitArray`s = 3
GC-registered objects.

Most of the GC cost is **over-retention by the conservative stack scan**:
each collect marks ~200–400k objects (wildly varying) while the true working
set is ~36k. The high-leverage fix is **precise rooting** (a Julia-style
tagged-value shadow stack) so RC reclaims the bulk and the collector only
handles real cycles.
The structural answer that sidesteps per-scalar Values entirely is the
**Tensor** port above (one node per layer-level op + BLAS).

Levers already measured and *rejected* (do not retry): NaN-boxing /
inline-buffer / generational GC (single-digit % ceiling, see the
value-model measurement notes), and the slab allocator's variable-length
path (locality regression).

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
