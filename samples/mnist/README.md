# MNIST MLP on Culebra

A 784–30–10 sigmoid MLP on MNIST, used as a cross-language benchmark
across three implementations: numpy, pure Python, and Culebra `--jit`.
Both **inference** and **training** are measured.

The tree-walking interpreter is excluded from the benchmark tables —
it runs the same scripts correctly (~28 s inference / ~100 s training)
but is two orders of magnitude slower than the JIT, so its numbers
dominate the table without adding insight. Use `./build/culebra
samples/mnist/{infer,train_bench}.cul` if you want to see it run.

The reference algorithm follows Michael Nielsen's
[network.py](http://neuralnetworksanddeeplearning.com/chap1.html) —
mini-batch SGD with MSE loss, sigmoid hidden + sigmoid output +
argmax prediction, η = 3.0.

## Files

Inference:
- `train.py`              — numpy full training; dumps `{W1,b1,W2,b2}.csv`
- `prep_test.py`          — extracts N test samples to `test_{images,labels}.csv`
- `infer_numpy.py`        — numpy inference (BLAS-accelerated baseline)
- `infer_pure.py`         — pure Python inference (numpy-free, scalar loops)
- `infer.cul`             — Culebra inference (matched to `infer_pure.py`)
- `_load_only.cul`        — load-without-inference helper for timing breakdown

Training (benchmark):
- `prep_train.py`         — extracts N training samples + dumps deterministic
                            initial weights `init_{W1,b1,W2,b2}.csv`
- `train_bench_numpy.py`  — numpy training, 1 epoch, hand-coded backprop
- `train_bench_pure.py`   — pure Python training (numpy-free, scalar loops)
- `train_bench.cul`       — Culebra training (matched to `train_bench_pure.py`)

Other:
- `bench.sh`              — runs every implementation 5× and reports means
- `data/`                 — MNIST IDX files (gitignored, populated by `just fetch-mnist`)

## Running

```bash
just fetch-mnist                             # download MNIST IDX files
python3.11 samples/mnist/train.py            # full training; dumps weights
python3.11 samples/mnist/prep_test.py 1000   # dump 1000 test samples
python3.11 samples/mnist/prep_train.py 1000  # dump 1000 train samples + init weights

# Inference
python3.11 samples/mnist/infer_numpy.py
python3    samples/mnist/infer_pure.py
./build/culebra        samples/mnist/infer.cul
./build/culebra --jit  samples/mnist/infer.cul

# Training (single epoch over the 1000-sample subset)
python3.11 samples/mnist/train_bench_numpy.py
python3    samples/mnist/train_bench_pure.py
./build/culebra        samples/mnist/train_bench.cul
./build/culebra --jit  samples/mnist/train_bench.cul

# Or run everything 5× via the bench harness
./samples/mnist/bench.sh
```

`python3.11` is used for any script that imports numpy; pure scripts
run on any 3.x.

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
final accuracy (0.622 after the 1000-sample epoch).

## Numbers

Apple Silicon, single core, total wall time (load + work), mean of 5 runs.

### Inference (1000 test images)

| implementation     |  time   | ratio (vs pure) |
|--------------------|--------:|----------------:|
| numpy (BLAS)       |  0.21 s |          0.17×  |
| pure Python        |  1.21 s |          1.00×  |
| Culebra `--jit`    |  1.10 s |          0.91×  |
| Culebra Tensor     |  0.11 s |          0.09×  |

All four agree on predictions (accuracy 0.954 from `train.py`'s
30-epoch full-MNIST weights). The Tensor backend lands ~2× ahead of
numpy: same BLAS underneath, no Python startup, and the contiguous
CSV reader (`Tensor.from_csv`) avoids the heap-array intermediate
that `numpy.loadtxt` builds.

### Training (1 epoch, mini-batch SGD)

Three subset sizes for the scalar columns, since their per-module
JIT codegen has a fixed cost that only larger workloads amortize.
The Tensor column is shown at N=10000 only since BLAS-shaped work
doesn't have the same warmup curve.

| implementation   | N=1000  | N=5000  | N=10000 |
|------------------|--------:|--------:|--------:|
| numpy (BLAS)     |  0.24 s |  0.44 s |  0.57 s |
| pure Python      |  3.97 s | 15.17 s | 28.77 s |
| Culebra `--jit`  |  7.07 s | 15.39 s | 25.54 s |
| Culebra Tensor   |    —    |    —    |  0.53 s |

All implementations agree on final accuracy (0.622 / 0.867 /
0.886 — Tensor at N=10000 lands at 0.887, FP-epsilon away from
numpy's 0.886). The Tensor row edges past numpy: the loader is
faster, and the inner training loop (BLAS GEMM dominated in both)
runs at numpy parity.

### Where the time goes (inference, Culebra `--jit`)

Splitting load and inference (via `_load_only.cul`):

| phase     |  time   |
|-----------|--------:|
| load CSVs |  0.17 s |
| inference |  1.10 s |

JIT warmup is ~1 s, so for short runs like this it is a meaningful
fraction of the total. The CSV loader is comparable to Python's
(0.17 s vs 0.06 s — Culebra `to_float` is slightly heavier per call).

### How JIT compares to pure Python

Unlike `samples/microgpt` (where JIT pulls ahead via class/dunder
inline-cache amortization), this benchmark is **scalar arithmetic
with no Object dispatch**. The JIT inlines numeric `+` `-` `*` `/`
directly to LLVM `fadd` / `fmul` / etc. when both operands are
statically known to be numeric, so the only fixed cost left is the
JIT's per-module codegen.

That codegen cost is fixed (~3–4 s of warmup), so the comparison
depends on workload size:

- **Inference (1000 images)**: JIT beats pure Python by ~7%.
- **Training, N=1000**: JIT loses by ~1.8× — the codegen cost
  dominates a 100-batch run that finishes in ~7 s.
- **Training, N=5000**: JIT ties pure Python (1.01×).
- **Training, N=10000**: JIT pulls ahead, ~11% faster than pure
  Python; the trend continues for larger N.

The numpy column uses BLAS for matrix multiplication and is therefore
not directly comparable to the scalar implementations; it is included
as a reference point.

### Closing the gap: Culebra Tensor

The "Culebra Tensor" rows use the builtin `Tensor` type added in
Phase 1, which routes matrix multiplication through Accelerate
(macOS) / OpenBLAS (Linux). The same BLAS that numpy uses, called
from Culebra's lazy graph layer with stride-aware kernels — and
because the loader skips the heterogeneous-Array detour, both
inference and training land slightly ahead of numpy on this
workload.

```cul
let X  = Tensor.from(load_2d("test_images.csv"))   # [N, 784]
let Xt = X.transpose()                              # [784, N], zero-copy view
let a1 = W1.linear_sigmoid(Xt, b1)                  # fused: W @ Xt + b → sigmoid
let a2 = W2.linear_sigmoid(a1, b2)
let preds = a2.argmax(0)                            # [N]
Tensor.eval(preds)
```

See `infer_tensor.cul` and `train_bench_tensor.cul` for full ports;
the algorithm is identical to the scalar/numpy versions, written
against numpy-style broadcast and the same trio of `linear_sigmoid`
+ `argmax` + `to_array` patterns. CUDA/MSL backends are future work
behind the same `Tensor` interface.
