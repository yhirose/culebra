# MNIST MLP on Culebra

A 784–30–10 sigmoid MLP trained on MNIST, used as a cross-language
**inference** benchmark across four implementations: numpy, pure
Python, Culebra interpreter, and Culebra `--jit`.

The training algorithm follows Michael Nielsen's
[network.py](http://neuralnetworksanddeeplearning.com/chap1.html) —
mini-batch SGD with MSE loss, sigmoid hidden + sigmoid output +
argmax prediction, 30 epochs, batch size 10, η = 3.0.

## Files

- `train.py`         — numpy training; dumps `{W1,b1,W2,b2}.csv`
- `prep_test.py`     — extracts N test samples to `test_{images,labels}.csv`
- `infer_numpy.py`   — numpy inference (BLAS-accelerated baseline)
- `infer_pure.py`    — pure Python inference (numpy-free, scalar loops)
- `infer.cul`        — Culebra inference (matched to `infer_pure.py`)
- `_load_only.cul`   — load-without-inference helper for timing breakdown
- `bench.sh`         — runs all 4 implementations 5× and reports means
- `data/`            — MNIST IDX files (gitignored, populated by `just fetch-mnist`)

## Running

```bash
just fetch-mnist                            # download MNIST IDX files
python3.11 samples/mnist/train.py           # train; produces W1/W2/b1/b2.csv
python3.11 samples/mnist/prep_test.py 1000  # dump 1000 test samples

python3.11 samples/mnist/infer_numpy.py     # numpy baseline
python3    samples/mnist/infer_pure.py      # pure Python (no numpy)
./build/culebra        samples/mnist/infer.cul
./build/culebra --jit  samples/mnist/infer.cul
```

`python3.11` is used for any script that imports numpy. The pure
Python script runs on any 3.x.

## File formats (CSV)

| File              | Shape    | Format    | Content           |
|-------------------|----------|-----------|-------------------|
| `W1.csv`          | 30 × 784 | `%.8e`    | hidden weights    |
| `b1.csv`          | 30 × 1   | `%.8e`    | hidden bias       |
| `W2.csv`          | 10 × 30  | `%.8e`    | output weights    |
| `b2.csv`          | 10 × 1   | `%.8e`    | output bias       |
| `test_images.csv` | N × 784  | `%.6f`    | pixel values [0,1]|
| `test_labels.csv` | N × 1    | `%d`      | integer label 0–9 |

All values are comma-separated within a row, newline-separated
between rows.

## Numbers

Apple Silicon, single core, 1000 test images, total wall time (load
+ inference), mean of 5 runs:

| implementation     |  time   | ratio (vs pure) |
|--------------------|--------:|----------------:|
| numpy (BLAS)       |  0.34 s |          0.27×  |
| pure Python        |  1.24 s |          1.00×  |
| Culebra `--jit`    |  1.24 s |          1.00×  |
| Culebra interp     | 28.03 s |          22.6×  |

Accuracy is **0.954 across all four implementations** (predictions
match bit-for-argmax). Final test accuracy after 30 epochs of training
on the full 60,000 MNIST training set is ~95.5–96.0%.

### Where the time goes (Culebra `--jit`)

Splitting load and inference (via `_load_only.cul`):

| phase     |  time   |
|-----------|--------:|
| load CSVs |  0.17 s |
| inference |  1.07 s |

JIT warmup is ~1 s, so for short runs like this, warmup is a meaningful
fraction of the total. The CSV loader is comparable to Python's
(0.17 s vs 0.06 s — Culebra `to_float` is slightly heavier per call).

### Why JIT only ties pure Python here

Unlike `samples/microgpt` (where JIT is ~17% faster than CPython at
200+ training steps), this benchmark is **pure forward-pass scalar
arithmetic** — no class / dunder dispatch, no autograd graph, no
mutable Object lifecycle. CPython's tight loop over `math.exp` and
arithmetic is therefore competitive: both Python and Culebra spend
the same work on per-element multiply-add and the per-iteration
overhead happens to balance out. The JIT's inline-cache and HOF-fusion
machinery target patterns that don't appear in this code path.

The numpy column uses BLAS for matrix multiplication and is therefore
not directly comparable to the scalar implementations; it is included
as a reference point. Closing this gap is on the Culebra roadmap as a
future built-in matrix primitive (CUDA / MSL backed) — once Culebra
gains a real matrix type, this benchmark should approach the numpy
column rather than the pure-Python column.
