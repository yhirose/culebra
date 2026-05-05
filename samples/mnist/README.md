# MNIST MLP on Culebra

A 784–30–10 sigmoid MLP on MNIST, used as a cross-language benchmark
across seven implementations: numpy, pure Python, PyTorch (CPU and
Apple MPS), Julia, Culebra `--jit` (scalar), and Culebra Tensor. Both
**inference** and **training** are measured.

The tree-walking interpreter is excluded from the benchmark tables —
it runs the same scripts correctly (~28 s inference / ~100 s training)
but is two orders of magnitude slower than the JIT, so its numbers
dominate the table without adding insight. Use `./build/culebra
samples/mnist/{infer,train_bench}.cul` if you want to see it run.

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

The pure-Python and Culebra-`--jit`-scalar training scripts use
`CYCLES=1` because a single epoch already takes 20–30 s; for those,
warm is reported as `-`.

## Files

Inference:
- `train.py`              — numpy full training; dumps `{W1,b1,W2,b2}.csv`
- `prep_test.py`          — extracts N test samples to `test_{images,labels}.csv`
- `infer_numpy.py`        — numpy inference (BLAS-accelerated baseline)
- `infer_pure.py`         — pure Python inference (numpy-free, scalar loops)
- `infer_torch.py`        — PyTorch inference; `DEVICE=cpu|mps`
- `infer.jl`              — Julia inference (LinearAlgebra, hand-coded)
- `infer.cul`             — Culebra scalar inference (matched to `infer_pure.py`)
- `infer_tensor.cul`      — Culebra Tensor inference (matched to `infer_numpy.py`)
- `_load_only.cul`        — load-without-inference helper for timing breakdown

Training (benchmark):
- `prep_train.py`         — extracts N training samples + dumps deterministic
                            initial weights `init_{W1,b1,W2,b2}.csv`
- `train_bench_numpy.py`  — numpy training, 1 epoch, hand-coded backprop
- `train_bench_pure.py`   — pure Python training (numpy-free, scalar loops)
- `train_bench_torch.py`  — PyTorch training; `DEVICE=cpu|mps`
- `train_bench.jl`        — Julia training (LinearAlgebra, hand-coded)
- `train_bench.cul`       — Culebra scalar training (matched to `train_bench_pure.py`)
- `train_bench_tensor.cul`— Culebra Tensor training (matched to `train_bench_numpy.py`)

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
python3.11 samples/mnist/train.py            # full training; dumps weights
python3.11 samples/mnist/prep_test.py 1000   # dump 1000 test samples
python3.11 samples/mnist/prep_train.py 10000 # dump 10000 train samples + init weights

# Inference
python3.11 samples/mnist/infer_numpy.py
python3    samples/mnist/infer_pure.py
DEVICE=cpu python3.11 samples/mnist/infer_torch.py
DEVICE=mps python3.11 samples/mnist/infer_torch.py
julia      samples/mnist/infer.jl
./build/culebra        samples/mnist/infer.cul
./build/culebra --jit  samples/mnist/infer.cul
./build/culebra --jit  samples/mnist/infer_tensor.cul

# Training (single epoch over the 10000-sample subset)
python3.11 samples/mnist/train_bench_numpy.py
python3    samples/mnist/train_bench_pure.py
DEVICE=cpu python3.11 samples/mnist/train_bench_torch.py
DEVICE=mps python3.11 samples/mnist/train_bench_torch.py
julia      samples/mnist/train_bench.jl
./build/culebra        samples/mnist/train_bench.cul
./build/culebra --jit  samples/mnist/train_bench.cul
./build/culebra --jit  samples/mnist/train_bench_tensor.cul

# Or run everything 3× via the bench harness
./samples/mnist/bench.sh 3
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
final accuracy (0.886 after the 10000-sample epoch from `train.py`'s
deterministic init).

## Numbers

Apple Silicon (M-series), single-machine. Mean of 3 external runs;
each run does `CYCLES` in-process cycles (3 for inference / 2 for
training, except pure Python and Culebra `--jit` scalar training
which use 1).

### Inference (1000 test images)

| implementation     |  load   |  cold   |  warm   |
|--------------------|--------:|--------:|--------:|
| numpy (BLAS)       |  0.04 s |  0.01 s |  0.01 s |
| pure Python        |  0.06 s |  1.13 s |  1.12 s |
| PyTorch CPU        |  0.03 s |  0.00 s |  0.00 s |
| PyTorch MPS (GPU)  |  0.05 s |  0.09 s |  0.00 s |
| Julia              |  0.42 s |  0.32 s |  0.00 s |
| Culebra `--jit`    |  0.09 s |  0.80 s |  0.78 s |
| Culebra Tensor     |  0.02 s |  0.00 s |  0.00 s |

All seven agree on predictions (accuracy 0.954 from `train.py`'s
30-epoch full-MNIST weights).

### Training (1 epoch, 10000 samples, mini-batch SGD)

| implementation     |  load   |  cold   |  warm   |
|--------------------|--------:|--------:|--------:|
| numpy (BLAS)       |  0.39 s |  0.09 s |  0.08 s |
| pure Python        |  0.68 s | 27.11 s |    –    |
| PyTorch CPU        |  0.32 s |  0.09 s |  0.07 s |
| PyTorch MPS (GPU)  |  0.39 s |  0.29 s |  0.26 s |
| Julia              |  1.36 s |  0.79 s |  0.07 s |
| Culebra `--jit`    |  0.96 s | 20.33 s |    –    |
| Culebra Tensor     |  0.24 s |  0.08 s |  0.08 s |

All implementations agree on final accuracy (0.886 — Culebra Tensor
lands at 0.887, FP-epsilon away from the F64 reference). Pure Python
and Culebra `--jit` scalar use `CYCLES=1` because a single epoch
already takes 20–30 s.

### What the cold/warm split shows

- **Julia** has the most visible JIT warmup: cold 0.32 s → warm
  0.001 s on inference (≈300×), cold 0.87 s → warm 0.07 s on training.
  Method specialization on first call dominates the cold cost.
- **PyTorch MPS** pays a one-time GPU context init at cold (~0.05–0.3 s
  depending on workload), then drops to ~1 ms warm on inference.
- **PyTorch CPU and Culebra Tensor** are at the noise floor on
  inference (under 1 ms) — both call into Apple Accelerate via the
  same sgemm path.
- **Culebra `--jit`** does not show a cold/warm gap on the inner
  cycle: per-module codegen runs once at process startup (visible in
  the per-run wall time, not in the in-process cycles).

### MNIST size: GPU is slower than CPU

The 30-hidden MLP at batch=10 is small enough that MPS kernel-launch
latency dominates the actual matmul. PyTorch MPS (training) lands at
**0.26 s warm, ~3.7× slower than PyTorch CPU's 0.07 s warm**. This is
the expected scaling — GPU pays off once the matmul is large enough
to amortize launch overhead, which a 30×784 hidden layer does not
hit. Larger models in `samples/microgpt` are a more honest GPU
workload.

### Dtype note

Default dtypes differ across the row:

| implementation     | dtype       |
|--------------------|-------------|
| numpy              | F64         |
| pure Python        | Python float (≈F64) |
| PyTorch (CPU/MPS)  | F32         |
| Julia              | F64         |
| Culebra `--jit`    | Float (F64) |
| Culebra Tensor     | F32         |

PyTorch CPU running ~20% faster than numpy on training is partly the
F32 vs F64 difference; on the same Apple Accelerate, F32 sgemm is
roughly 2× the throughput of F64 dgemm, but the MNIST matmuls are too
small for the full ratio to show. Accuracy is unaffected at this
scale (0.886 vs 0.886).

### Where the time goes (Culebra `--jit`, scalar)

Splitting load and inference (via `_load_only.cul`):

| phase     |  time   |
|-----------|--------:|
| load CSVs |  0.09 s |
| inference |  0.78 s |

Inference cold and warm are almost identical (0.80 s vs 0.78 s):
Culebra's per-module JIT codegen runs once at process startup and is
charged to the **process wall time**, not to the in-process cycles.
That cost shows up if you compare the bench harness's external runs
against a single in-process loop.

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

See `infer_tensor.cul` and `train_bench_tensor.cul` for full ports;
the algorithm is identical to the scalar/numpy versions, written
against numpy-style broadcast and the same trio of `linear_sigmoid`
+ `argmax` + `to_array` patterns. The training script uses
`Tensor.clone()` to reset weights between cycles, mirroring numpy's
`init_W1.copy()` pattern. CUDA/MSL backends are future work behind
the same `Tensor` interface.
