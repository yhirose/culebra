# MNIST MLP on Culebra

A 784–30–10 sigmoid MLP on MNIST, used as a cross-language benchmark
across six implementations: numpy, pure Python, PyTorch (CPU and
Apple MPS), Julia, and Culebra Tensor. Both **inference** and
**training** are measured.

The reference algorithm follows Michael Nielsen's
[network.py](http://neuralnetworksanddeeplearning.com/chap1.html) —
mini-batch SGD with MSE loss, sigmoid hidden + sigmoid output +
argmax prediction, η = 3.0.

## Cold vs warm

Each script repeats the inner work `CYCLES` times in-process and
reports two numbers:

- **cold** — first cycle, includes JIT compilation, BLAS first-call
  init, GPU context creation, and any other one-shot warmup work.
- **warm** — mean of cycles 2..K, the steady-state cost once those
  one-shot costs have been amortized.

`bench.sh` then averages both across `$RUNS` external process
restarts. `load` (CSV read) is reported separately and never folded
into cold/warm.

The pure-Python training script uses `CYCLES=1` because a single
epoch already takes ~27 s; warm is reported as `-` for that row.

## Files

Inference:
- `train.py`              — numpy full training; dumps `{W1,b1,W2,b2}.csv`
- `prep_test.py`          — extracts N test samples to `test_{images,labels}.csv`
- `infer_numpy.py`        — numpy inference (BLAS-accelerated baseline)
- `infer_pure.py`         — pure Python inference (numpy-free, scalar loops)
- `infer_torch.py`        — PyTorch inference; `DEVICE=cpu|mps`
- `infer.jl`              — Julia inference (LinearAlgebra, hand-coded)
- `infer.cul`             — Culebra Tensor inference (matched to `infer_numpy.py`)

Training (benchmark):
- `prep_train.py`         — extracts N training samples + dumps deterministic
                            initial weights `init_{W1,b1,W2,b2}.csv`
- `train_bench_numpy.py`  — numpy training, 1 epoch, hand-coded backprop
- `train_bench_pure.py`   — pure Python training (numpy-free, scalar loops)
- `train_bench_torch.py`  — PyTorch training; `DEVICE=cpu|mps`
- `train_bench.jl`        — Julia training (LinearAlgebra, hand-coded)
- `train_bench.cul`       — Culebra Tensor training (matched to `train_bench_numpy.py`)
- `train_bench_autoencoder.cul` — Culebra Tensor autoencoder training
                            (784-512-256-64-256-512-784, mini-batch SGD),
                            a shape deep/wide enough to actually cross the
                            GPU/CPU threshold — see "GPU vs CPU" below.
- `train_bench_autoencoder_tl.cpp` — the same autoencoder step graph in raw
                            `cpp-tensorlib` C++ (no Culebra), the baseline that
                            splits the silarray gap into library vs runtime —
                            see "GPU vs CPU" below.

Other:
- `bench.sh`              — runs every implementation $RUNS× and reports means
- `data/`                 — MNIST IDX files (gitignored, populated by `just fetch-mnist`)

## Setup

```bash
# Optional cross-language baselines
brew install julia
python3.11 -m pip install --user torch
```

## Running

```bash
just fetch-mnist                             # download MNIST IDX files
python3.11 benchmarks/mnist/train.py            # full training; dumps weights
python3.11 benchmarks/mnist/prep_test.py 10000  # dump 10000 test samples
python3.11 benchmarks/mnist/prep_train.py 10000 # dump 10000 train samples + init weights

# Inference
python3.11 benchmarks/mnist/infer_numpy.py
python3    benchmarks/mnist/infer_pure.py
DEVICE=cpu python3.11 benchmarks/mnist/infer_torch.py
DEVICE=mps python3.11 benchmarks/mnist/infer_torch.py
julia      benchmarks/mnist/infer.jl
./build/culebra --jit  benchmarks/mnist/infer.cul

# Training (single epoch over the 10000-sample subset)
python3.11 benchmarks/mnist/train_bench_numpy.py
python3    benchmarks/mnist/train_bench_pure.py
DEVICE=cpu python3.11 benchmarks/mnist/train_bench_torch.py
DEVICE=mps python3.11 benchmarks/mnist/train_bench_torch.py
julia      benchmarks/mnist/train_bench.jl
./build/culebra --jit  benchmarks/mnist/train_bench.cul

# Or run everything 3× via the bench harness
./benchmarks/mnist/bench.sh 3

# Autoencoder (needs the full 60000-sample training set, not the 10000 subset)
python3.11 benchmarks/mnist/prep_train.py 60000
./build/culebra --jit benchmarks/mnist/train_bench_autoencoder.cul [cpu|gpu|auto]
```

`python3.11` is used for any script that imports numpy or torch; pure
scripts run on any 3.x.

## File formats (CSV)

| File                | Shape       | Format    | Content              |
|---------------------|-------------|-----------|----------------------|
| `W1.csv`/`init_W1`  | 30 × 784    | `%.8e`    | hidden weights       |
| `b1.csv`/`init_b1`  | 30 × 1      | `%.8e`    | hidden bias          |
| `W2.csv`/`init_W2`  | 10 × 30     | `%.8e`    | output weights       |
| `b2.csv`/`init_b2`  | 10 × 1      | `%.8e`    | output bias          |
| `train_images.csv`  | N × 784     | `%.6f`    | pixel values [0,1]   |
| `train_labels.csv`  | N × 1       | `%d`      | integer label 0–9    |
| `test_images.csv`   | N × 784     | `%.6f`    | pixel values [0,1]   |
| `test_labels.csv`   | N × 1       | `%d`      | integer label 0–9    |

All values are comma-separated within a row, newline-separated between
rows. The training-bench scripts read `init_*.csv` so every
implementation starts from identical weights and produces identical
final accuracy (0.9079 after the 10000-sample epoch from `train.py`'s
deterministic init).

## Numbers

Apple Silicon (M-series), single-machine, measured 2026-07-17. Mean of
3 external runs; each run does `CYCLES` in-process cycles (3 for
inference / 2 for training, except pure Python training which uses 1).

### Inference (10000 test images)

| implementation     |  load   |  cold   |  warm   |
|--------------------|--------:|--------:|--------:|
| numpy (BLAS)       | 0.348 s | 0.014 s | 0.025 s |
| pure Python        | 0.633 s | 11.19 s | 10.86 s |
| PyTorch CPU        | 0.300 s | 0.008 s | 0.003 s |
| PyTorch MPS (GPU)  | 0.407 s | 0.094 s | 0.002 s |
| Julia              | 1.281 s | 0.306 s | 0.007 s |
| Culebra Tensor     | 0.315 s | 0.005 s | 0.004 s |

All seven agree on predictions (accuracy 0.9551 from `train.py`'s
30-epoch full-MNIST weights).

### Training (1 epoch, 10000 samples, mini-batch SGD)

| implementation     |  load   |  cold   |  warm   |
|--------------------|--------:|--------:|--------:|
| numpy (BLAS)       | 0.659 s | 0.095 s | 0.093 s |
| pure Python        | 1.307 s | 27.01 s |    –    |
| PyTorch CPU        | 0.597 s | 0.074 s | 0.064 s |
| PyTorch MPS (GPU)  | 0.708 s | 0.255 s | 0.232 s |
| Julia              | 2.408 s | 0.871 s | 0.086 s |
| Culebra Tensor     | 0.611 s | 0.039 s | 0.038 s |

Culebra Tensor is the fastest of the six on warm training (0.038 s),
ahead of PyTorch CPU (0.064 s) and numpy (0.093 s).

All implementations agree on final accuracy (0.9079 — Culebra Tensor
lands at 0.9081, FP-epsilon away from the F64 reference). Pure Python
uses `CYCLES=1` because a single epoch already takes ~27 s.

### What the cold/warm split shows

- **Julia** has the most visible JIT warmup: cold 0.31 s → warm
  0.007 s on inference (≈40×), cold 0.87 s → warm 0.086 s on training.
  Method specialization on first call dominates the cold cost.
- **PyTorch MPS** pays a one-time GPU context init at cold (~0.05–0.3 s
  depending on workload), then drops to ~2 ms warm on inference.
- **PyTorch CPU and Culebra Tensor** are at the noise floor on
  inference (~2–3 ms) — both call into Apple Accelerate via the
  same sgemm path.

### MNIST size: GPU is slower than CPU

The 30-hidden MLP at batch=10 is small enough that MPS kernel-launch
latency dominates the actual matmul. PyTorch MPS (training) lands at
**0.232 s warm, ~3.6× slower than PyTorch CPU's 0.064 s warm**. This
is the expected scaling — GPU pays off once the matmul is large
enough to amortize launch overhead, which a 30×784 hidden layer does
not hit. Larger models in `benchmarks/microgpt` are a more honest GPU
workload.

### Dtype note

Default dtypes differ across the row:

| implementation     | dtype       |
|--------------------|-------------|
| numpy              | F64         |
| pure Python        | Python float (≈F64) |
| PyTorch (CPU/MPS)  | F32         |
| Julia              | F64         |
| Culebra Tensor     | F32         |

PyTorch CPU running faster than numpy on training is partly the F32
vs F64 difference; on the same Apple Accelerate, F32 sgemm is
roughly 2× the throughput of F64 dgemm, but the MNIST matmuls are too
small for the full ratio to show. Accuracy is unaffected at this
scale (0.9079 vs 0.9079).

### Closing the gap: Culebra Tensor

The "Culebra Tensor" rows use the builtin `Tensor` type added in
Phase 1, which routes matrix multiplication through Accelerate
(macOS) / OpenBLAS (Linux). The same BLAS that numpy uses, called
from Culebra's lazy graph layer with stride-aware kernels — and
because the loader skips the heterogeneous-Array detour, both
inference and training land in the BLAS-bound cluster (numpy /
PyTorch CPU / Julia / Culebra Tensor all within 2× of each other on
warm training).

```cul
let X  = Tensor.from_csv("test_images.csv")  # [N, 784]
let Xt = X.transpose()                       # [784, N], zero-copy view
let a1 = W1.linear_sigmoid(Xt, b1)           # fused: W @ Xt + b → sigmoid
let a2 = W2.linear_sigmoid(a1, b2)
let preds = a2.argmax(0)                     # [N]
Tensor.eval(preds)
```

See `infer.cul` and `train_bench.cul` for full ports;
the algorithm is identical to the scalar/numpy versions, written
against numpy-style broadcast and the same trio of `linear_sigmoid`
+ `argmax` + `to_array` patterns. The training script uses
`Tensor.clone()` to reset weights between cycles, mirroring numpy's
`init_W1.copy()` pattern. `Tensor` also has Metal (macOS) and CUDA
(Linux/Windows) backends via `vendor/cpp-tensorlib`, selected
automatically or via `tl::use_cpu()`/`tl::use_gpu()`; this MNIST-size
MLP stays on CPU by default since it's too small to benefit (see
above).

### GPU vs CPU: a shape that actually crosses the threshold

The classifier above (784-30-10, or even a widened 784-512-10 run in
a single full-batch step) never crosses `cpp-tensorlib`'s per-kernel
GPU/CPU size threshold (`auto_threshold_()` in
`vendor/cpp-tensorlib/include/types.h`, ~2e9 for matmul on Metal):
its output layer is always `M=10` (ten digit classes), so that op's
`M*N*K` stays small regardless of batch size or hidden width, and
forcing `Tensor.use_gpu()` on the whole network makes it *slower*
than CPU (confirmed against `sil-gpu`/`mlx-gpu`/`torch-gpu` on the
same 784-50-10 shape in `~/Projects/silarray/bench`, which all show
the same 4-12x GPU slowdown — this is shape, not a Culebra quirk).

`train_bench_autoencoder.cul` uses a deeper, wider shape instead:
784-512-256-64-256-512-784 (sigmoid, MSE, SGD, batch=100, 1 epoch =
600 steps over the full 60,000-image training set), matching
`~/Projects/silarray/bench/mnist/bench_autoencoder.cpp`. Mini-batch
SGD over many steps also lets the GPU backend pipeline consecutive
kernel dispatches, unlike a single giant full-batch step. Apple
Silicon (M1 Pro), mean of 2 runs:

| device                | warm (1 epoch) |
|------------------------|---------------:|
| `Tensor.use_cpu()`     | ~5.50 s |
| `Tensor.use_gpu()`     | ~5.05 s |
| `Tensor.use_auto()`    | ~4.94 s |

GPU/auto beat CPU here (~8-15%), the opposite of the classifier
shape — but the margin is smaller than silarray's own measurement on
the identical architecture (CPU 914 ms vs GPU 621 ms, ~1.5x), and
Culebra's absolute per-epoch time (~5 s) is 6-9x silarray's
(600-900 ms).

**Where that 6-9x actually comes from (measured, CPU).** It is
tempting to blame the whole gap on Culebra's object model, but a
direct measurement splits it in two, and the *library* is the larger
factor. `train_bench_autoencoder_tl.cpp` runs the exact same tl op
graph this `.cul` emits per step (same forward `linear_sigmoid`
expansion, same backward line-for-line, same per-layer ones-tensor
allocation) with **no Culebra in the loop** — pure `cpp-tensorlib`
C++. On the same machine, back to back:

| layer                                    | CPU epoch | vs silarray |
|------------------------------------------|----------:|------------:|
| silarray (eager C++, `sil::array<float>`) |  ~0.9 s   |    1.0x     |
| `cpp-tensorlib` C++ (`_tl.cpp`, no Culebra) | ~2.8-3.0 s |  ~3.1x    |
| Culebra `.cul` (CPU)                     | ~4.3-5.3 s |  ~5-6x      |

So the bulk of the gap — ~3x — is `cpp-tensorlib` itself being
slower than silarray on this op mix at the C++ level (lazy-graph
build/dispatch per op, and weaker backward fusion than silarray's
single `sigmoid_backward`/`linear` kernels); it has nothing to do
with Culebra. Culebra's own runtime then adds only ~1.5-1.9x on top
of the `cpp-tensorlib` baseline: the `.cul` script creates ~30-40 new
`Tensor` objects per step (forward outputs, `dnet`/`dW`/`db`/`input_l`
per layer, updated `Ws[l]`/`bs[l]`) — each a GC-tracked Object on top
of its `cpp-tensorlib` handle, and `Array` indexing (`Ws[l]`,
`outs[l]`) goes through tagged-value boxing, ~20-25k GC-registered
objects across 600 steps, the same per-object adopt+refcount tax
Culebra's object model pays generally (see the scalar-vs-Python
analysis in `benchmarks/microgpt/README.md`). That runtime tax is
roughly backend-independent, so it compresses the CPU/GPU *ratio*
without changing its direction. (The per-layer `Tensor.ones`
allocation, incidentally, is free at the tl level — the `_tl.cpp`
baseline keeps it and a no-ones variant times identically — so it
only costs at Culebra's object layer, not in the library.)

Compare backends directly with
`Tensor.use_cpu()`/`use_gpu()`/`use_auto()` before assuming a given
shape needs (or doesn't need) the GPU path; and don't read the
absolute per-epoch gap against silarray as a Culebra-runtime cost —
most of it is the tensor library, and only ~1.5-1.9x of it is
Culebra.

**Update — both factors since reduced.** The table above is the
decomposition that motivated two follow-up changes; each attacked one
factor. (1) Culebra's runtime factor: forward-only work like this
manual-backprop loop no longer records an autograd tape (it walks
requires_grad nodes only), removing the per-op wrapper build/teardown —
the `.cul` epoch dropped from ~5.2 s to ~3.2 s. (2) The library factor:
`cpp-tensorlib` gained contiguous fast paths for axis reductions and
rank-2 broadcast (flat pointer loops instead of the generic strided
coordinate walker), ~20-24% faster on this step — the `_tl.cpp` C++
baseline dropped to ~2.6 s and the combined `.cul` epoch to ~2.1-2.3 s
(same-machine mins; cross-run numbers drift with thermal state).
silarray's remaining lead is now mostly kernel fusion (it folds bias +
activation into single passes); that fusion is the next lever on the
library side.
