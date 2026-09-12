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
| Python (scalar reference)   | ~83 ms |

The Tensor port is **~50–100× faster per step than the scalar port** (one
layer-level op per Tensor node + BLAS, instead of thousands of per-scalar
`Value` objects). The VM and JIT produce **bit-identical loss**. The total
wall (~2.5 s for 100 steps) is mostly the up-front JIT compile, not the
loop (~0.1 s) — `--jit-faststart` cuts that compile by ~5x at no
per-step cost here, since the hot work is in BLAS rather than in
JIT-emitted code. The on-disk object cache (`CULEBRA_JIT_CACHE=auto`)
helps less than it looks: it caches what the *backend* emits, so a warm
`-O2` run still pays the IR pipeline in full — on a Linux box, this
program's compile went 1.58 s cold, 0.70 s warm, against 0.31 s for a
cold `--jit-faststart`.

### Scalar microgpt — alloc-bound, ahead of Python on the JIT

The scalar port builds ~60k `Value` objects per step (one per scalar
arithmetic op, each a class instance plus two Arrays), so it is
**alloc-bound**. Linux (20-thread Zen, one core used), 2026-09-12,
ms/step over the first two 50-step windows — the per-step cost moves ~20%
with document length, so compare the same window, not one wall-clock
average:

| implementation   | steps 1–50 | steps 51–100 |
|------------------|-----------:|-------------:|
| Python 3.11      |      75 ms |        81 ms |
| Culebra `--jit`  |  **60 ms** |    **71 ms** |
| Culebra `--vm`   |     107 ms |       122 ms |

Until 2026-09-12 the JIT sat at 102 / 115 ms, behind Python, and this
README blamed the collector. Profiling (sampling, self time) showed the
collector at under 1% and the gap somewhere else: `backward()` spelled
its inner loop as `range(n).for_each(fn (j) ...)`, which builds a
`range`, an iterator and a closure for every node — `range` is a
namespace method call with an argument slab, not a loop — and that alone
was 40% of the step. The port now writes it as the counted loop Python's
`zip` walk is; `dot`/`sum_v` keep their `reduce` spelling, since the
closure *calls* are cheap and moving them to loops measured no gain.
Both engines still produce bit-identical loss (Python diverges only via
a different Mersenne-Twister shuffle, expected).

## Where time goes (`--jit`, self time, sampling profiler, 150 steps)

| Category                                                        | Self-time |
|-----------------------------------------------------------------|----------:|
| JIT-emitted code                                                |      ~28% |
| Refcount (`_culebra_value_release_impl` / `_release_node`)      |      ~13% |
| Allocation (`array_new` / `object_new` / malloc / free)         |      ~12% |
| GC registry insert/erase (`Heap::adopt` / `_gc_note_free`)      |      ~10% |
| Property/index access (`array_get` / `object_set_fast` / slots) |       ~7% |
| Collector proper (`collect_impl`)                               |       <1% |

The remaining cost is the per-step `Value` lifecycle: three heap objects
per scalar op, each registered with the collector on birth and
de-registered on death. Turning the collector off (`CULEBRA_GC_NEVER=1`)
does not speed the run up, and the heap at each collect stays between
50k and 280k objects across hundreds of steps — the collection itself is
not the cost, the bookkeeping around allocation is. The `--vm` lane adds
dispatch (~25%) and an out-of-line `culebra_runtime_value_retain`
(~17%) that the JIT inlines as IR.

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
chains fused into direct counter loops. This lived in the tree-walking
interpreter and went with it (v0.3.1 was its last release); the bytecode
VM and the JIT call these methods as written, which is why the port's
`backward()` spells its hot loop as `for` today.

Plus narrow wins: skip per-instance `class_name` heap copy, omit
`__ARGS__` allocation when the body never reads it.
