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
python3.11 samples/mnist/prep_test.py 10000  # dump 10000 test samples
python3.11 samples/mnist/prep_train.py 10000 # dump 10000 train samples + init weights

# Inference
python3.11 samples/mnist/infer_numpy.py
python3    samples/mnist/infer_pure.py
DEVICE=cpu python3.11 samples/mnist/infer_torch.py
DEVICE=mps python3.11 samples/mnist/infer_torch.py
julia      samples/mnist/infer.jl
./build/culebra --jit  samples/mnist/infer.cul

# Training (single epoch over the 10000-sample subset)
python3.11 samples/mnist/train_bench_numpy.py
python3    samples/mnist/train_bench_pure.py
DEVICE=cpu python3.11 samples/mnist/train_bench_torch.py
DEVICE=mps python3.11 samples/mnist/train_bench_torch.py
julia      samples/mnist/train_bench.jl
./build/culebra --jit  samples/mnist/train_bench.cul

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
final accuracy (0.9079 after the 10000-sample epoch from `train.py`'s
deterministic init).

## Numbers

Apple Silicon (M-series), single-machine. Mean of 3 external runs;
each run does `CYCLES` in-process cycles (3 for inference / 2 for
training, except pure Python training which uses 1).

### Inference (10000 test images)

| implementation     |  load   |  cold   |  warm   |
|--------------------|--------:|--------:|--------:|
| numpy (BLAS)       | 0.385 s | 0.024 s | 0.022 s |
| pure Python        | 0.615 s | 10.93 s | 10.90 s |
| PyTorch CPU        | 0.294 s | 0.008 s | 0.003 s |
| PyTorch MPS (GPU)  | 0.364 s | 0.147 s | 0.002 s |
| Julia              | 1.268 s | 0.303 s | 0.007 s |
| Culebra Tensor     | 0.221 s | 0.003 s | 0.002 s |

All seven agree on predictions (accuracy 0.9551 from `train.py`'s
30-epoch full-MNIST weights).

### Training (1 epoch, 10000 samples, mini-batch SGD)

| implementation     |  load   |  cold   |  warm   |
|--------------------|--------:|--------:|--------:|
| numpy (BLAS)       | 0.635 s | 0.091 s | 0.080 s |
| pure Python        | 1.224 s | 27.25 s |    –    |
| PyTorch CPU        | 0.592 s | 0.077 s | 0.065 s |
| PyTorch MPS (GPU)  | 0.646 s | 0.300 s | 0.273 s |
| Julia              | 2.284 s | 0.779 s | 0.075 s |
| Culebra Tensor     | 0.442 s | 0.077 s | 0.077 s |

All implementations agree on final accuracy (0.9079 — Culebra Tensor
lands at 0.9081, FP-epsilon away from the F64 reference). Pure Python
uses `CYCLES=1` because a single epoch already takes ~27 s.

### What the cold/warm split shows

- **Julia** has the most visible JIT warmup: cold 0.30 s → warm
  0.007 s on inference (≈40×), cold 0.78 s → warm 0.075 s on training.
  Method specialization on first call dominates the cold cost.
- **PyTorch MPS** pays a one-time GPU context init at cold (~0.05–0.3 s
  depending on workload), then drops to ~2 ms warm on inference.
- **PyTorch CPU and Culebra Tensor** are at the noise floor on
  inference (~2–3 ms) — both call into Apple Accelerate via the
  same sgemm path.

### MNIST size: GPU is slower than CPU

The 30-hidden MLP at batch=10 is small enough that MPS kernel-launch
latency dominates the actual matmul. PyTorch MPS (training) lands at
**0.273 s warm, ~4.2× slower than PyTorch CPU's 0.065 s warm**. This
is the expected scaling — GPU pays off once the matmul is large
enough to amortize launch overhead, which a 30×784 hidden layer does
not hit. Larger models in `samples/microgpt` are a more honest GPU
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

PyTorch CPU running ~20% faster than numpy on training is partly the
F32 vs F64 difference; on the same Apple Accelerate, F32 sgemm is
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
`init_W1.copy()` pattern. CUDA/MSL backends are future work behind
the same `Tensor` interface.
