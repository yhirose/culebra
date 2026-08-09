# MNIST digit recognition

Draw a digit in the left pad; a small MLP scores it live and the right
side charts the ten class probabilities. The same idea as Burn's
[mnist-inference-web](https://github.com/tracel-ai/burn/tree/main/examples/mnist-inference-web)
demo, done end to end in culebra: training, preprocessing, inference,
and the UI are all `.cul` scripts.

```bash
culebra examples/tensor/mnist/mnist.cul          # native window
culebra --jit examples/tensor/mnist/mnist.cul    # same, through the JIT
```

The demo also runs in the browser playground (Tensor → "Canvas: MNIST
digits"); the weight CSVs are fetched alongside the source.

## Model

A 784-128-64-10 MLP (ReLU hidden layers, softmax output), 109,386
parameters. The trained weights live in `weights/` as CSV, loaded with
`Tensor.from_csv` at startup. Input preprocessing matches MNIST's own
normalization: the ink's bounding box is scaled into a 20x20 area
(box-average downsampling) and shifted so its centre of mass sits at
the middle of the 28x28 grid.

## Retraining

The weights are reproducible from the raw MNIST files (fixed seed):

```bash
just fetch-mnist                                    # IDX files → benchmarks/mnist/data/
culebra --jit examples/tensor/mnist/train.cul       # ~15s total on an M-class laptop
```

`train.cul` parses the IDX files, trains with Adam (batch 64, 8
epochs) under the native autograd, reports test accuracy per epoch —
**97.3%** on the 10k test set at the end — and rewrites `weights/`.
Pass a number to train on a subset while experimenting
(`culebra --jit train.cul 1000`).
