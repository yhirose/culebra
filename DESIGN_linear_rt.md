# Linear-scaling AOT runtime archives

Branch: `feat/linear-rt-archives`. Goal: replace the 2^N runtime-archive
matrix (today: rt / no_tensor / no_http / no_tensor_no_http) with an
**N+1** model so adding a heavy optional dependency costs one TU + one
scan flag, not a doubling.

DELETE this file before merging to master.

## Why

`culebra` embeds prebuilt `culebra_rt.cc` archives so `culebra build`
(AOT) can link a CLI without a C++ toolchain. Each optional heavy dep
(tensor→BLAS, http→OpenSSL+zlib) needs its static reachability chain
broken so the AOT link can drop the library. Today that's done by
recompiling the *whole* runtime with `CULEBRA_RT_NO_TENSOR` /
`CULEBRA_RT_NO_HTTP`, producing one archive per subset = 2^N. The
archives are also embedded in the driver, so 2^N bloats binary size too.

## Validated mechanism (macOS ld64, 2026-06-05)

Weak default stub + strong override, scratch experiment:
- core.o alone (weak stub) links WITHOUT the heavy lib. ✓ heavy droppable.
- core.o + libfeat.a (naive archive append): **stub wins** — the weak
  def already satisfies the symbol, so the archive member is NOT pulled.
- core.o + feat.o direct, OR `-Wl,-force_load,libfeat.a`: **strong
  override wins**, heavy linked. ✓

**Critical constraint:** a feature archive must be *force-loaded*
(`-force_load` on ld64, `--whole-archive` / `-u sym` on GNU ld), not
naively appended, or the weak stub silently wins.

## Design

### N+1 archives
- `core.a` — `culebra_rt.cc` with every feature entry compiled as a
  **weak stub** (throws). References neither cblas nor ssl. Always linked.
- `tensor.a` — only the tensor entry points, **strong**, real bodies
  (cblas). Force-loaded iff `uses_tensor`.
- `http.a` — (phase 2) only the http namespace bodies, strong.
- Driver embeds N+1 archives; materializes core + each used feature one.

### SIMPLIFIED (verified 2026-06-05): one choke, not 21

cblas is reached ONLY via `culebra::tensor_eval_node` (tensor.h:1007):
`_tensor_run_*` (the only cblas callers, tensor.h 942/975) are called
ONLY from tensor_eval_node (1030-1047); `tensor_dot` etc. build lazy Op
nodes, no direct cblas. So partitioning **just `tensor_eval_node`** is
enough — the 21 `culebra_runtime_tensor_*` and the Tensor namespace stay
**real in core**. With a stubbed eval_node they reference cblas through a
dead path: `_tensor_run_*` go unreferenced → not emitted → no cblas in
core.o. No need to touch jit.h's 21 functions or any other NO_TENSOR
ifdef.

3-mode gate on `tensor_eval_node` only:

| TU | body | linkage | gate |
|----|------|---------|------|
| core.cc            | stub (throw) | `__attribute__((weak))` | `CULEBRA_RT_TENSOR_EVAL_WEAK` |
| culebra_rt_tensor.cc | real (cblas) | strong (non-inline) | `CULEBRA_RT_TENSOR_EVAL_STRONG` |
| header-only embedder | real | `inline` | (neither) |

core.cc compiles the FULL runtime (no NO_TENSOR) with eval_node forced
weak-stub → zero cblas. culebra_rt_tensor.cc includes just tensor.h with
the STRONG gate → emits strong real eval_node + `_tensor_run_*` + cblas.
Force-loaded iff uses_tensor.

Archives (phase 1): core in {http, no_http} (http axis untouched) +
tensor = 3 (was 4). Phase 2 collapses http → core + tensor + http = 3
for N=2 (vs 2^N=4); the win is asymptotic (N=5 → 6 vs 32).

### Driver link (`src/main.cc` ~444)
`cc obj core.a [-Wl,-force_load,tensor.a] dead_strip no_pie libcxx blas ssl -o out`
`blas`/`ssl` `-l` gating is already linear — keep it. Add force_load of
each used feature archive (platform-conditional flag like dead_strip).

## Symbol partition (tensor axis)

Tensor entry set (weak-stub in core / strong-real in tensor):
- JIT extern-C: ~21 `culebra_runtime_tensor_*` in `jit.h`. Stub bodies
  already exist at `#ifdef CULEBRA_RT_NO_TENSOR` (jit.h ~2644-2680,
  `_no_tensor_abort()`); real at `#else` (~2689-2898). Plus
  `culebra_runtime_tensor_binop` at jit.h:1950.
- interp choke: `culebra::tensor_eval_node` (tensor.h:1007). Stub at
  `#ifdef CULEBRA_RT_NO_TENSOR` (throws), real at `#else`. Single entry to
  every cblas kernel (`_tensor_run_*`).

Everything else stays strong in core.

## Phasing
1. **Tensor axis first** (clean: explicit runtime symbols). Collapse
   tensor variants → weak/strong split. 4 archives → 2 (http axis still
   variant-based temporarily, i.e. core comes in {http, no_http} until
   phase 2). Prove no-tensor AOT links sans BLAS + tensor AOT works.
2. **Http axis** — messier: no extern-C runtime, the ssl/zlib choke is
   `httplib::Client` inside the Http namespace method bodies in
   stdlib_interp.h / stdlib_jit.h. Factor those bodies into an http TU.
   Then N+1 = core + tensor + http = 3.

## Open risks
- Linux force-load equivalent (`--whole-archive`/`-u`) — CI both OSes.
- Active worktrees edit jit.h (packable/c3) — merge conflict risk in the
  tensor region.
- Header-only/JIT in-process path must stay byte-identical (the
  `inline used` mode is the default; only the archive TUs change).
