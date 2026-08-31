# dezero (Culebra port)

A Culebra port of [DeZero](https://github.com/oreilly-japan/deep-learning-from-scratch-3),
the from-scratch autodiff/deep-learning framework built across
*Deep Learning from Scratch 3* (Koki Saitoh, O'Reilly Japan). Upstream is
MIT-licensed; this is a from-scratch reimplementation of its algorithms in
Culebra, not a transliteration of its Python source.

This ports the finished framework — `Variable`/`Function` autodiff,
operator overloading, `Layer`/`Model` composition, optimizers, and 2D
convolution/RNN/LSTM — rather than the book's 60 incremental tutorial
scripts. Three things are intentionally out of scope: the pretrained
ImageNet showcase models (`VGG16`/`ResNet`/`SqueezeNet`, which need real
downloaded weights to be useful), the generative examples that build on
them (GAN/VAE/style-transfer/grad-cam), and `cuda.py`'s numpy/cupy device
switch (Culebra's own `Tensor` already has a separate, unrelated GPU path).

## Layout

```
dezero/
  core.cul           Variable, Function application, operator overloads
  functions.cul       activations, losses, elementwise math
  functions_conv.cul   im2col/col2im, conv2d, deconv2d, pooling
  layers.cul          Linear, RNN, LSTM, EmbedID, BatchNorm
  layers_conv.cul      Conv2d, Deconv2d
  models.cul          MLP, Sequential
  optimizers.cul       SGD, MomentumSGD, AdaGrad, AdaDelta, Adam
  utils.cul           numerical_grad, dot-graph export, misc helpers
  datasets.cul         Spiral, SinCurve, MNIST loader
  dataloaders.cul      DataLoader, SeqDataLoader
examples/
  *.cul               runnable demos
```

## Design notes

`Variable.data` holds a `Tensor` (Culebra's built-in n-dimensional array)
rather than a numpy `ndarray`; a scalar is a 1-element `[1]` `Tensor`. This
port's autodiff is entirely its own — it does not use `Tensor`'s native
`requires_grad`/`.backward()`, which is a separate, unrelated feature of
the host language. `Tensor` has no elementwise `sin`/`cos`/`tanh`/`exp` and
no comparison operator, so those round-trip through nested Culebra Arrays;
everything shape- and matmul-related rides `Tensor`'s native ops directly.

Culebra has no class inheritance, so `Function` and `Layer` are not base
classes here: every op is its own class with `forward`/`backward` methods
sharing bookkeeping through free functions (`apply1`/`apply2` in
`core.cul`), and every layer/model independently implements `params()`
and `__call__` rather than inheriting them. `Parameter` is `Variable`
itself under a second name (its only upstream role was an `isinstance`
marker this port doesn't need, since `params()` is always written out
explicitly rather than auto-detected).

Weight/bias shapes follow a row-vector convention throughout (a `Linear`
bias is `[1, out]`, not upstream's 1-D `[out]`), so a batch broadcasts
against it the same way this repo's own native-`Tensor` MNIST trainer
(`examples/tensor/mnist/train.cul`) already does. `functions_conv.cul`'s
im2col-based `conv2d`'s padding, window extraction and the NCHW/NHWC axis
move, and `pooling`'s own window gather, all go through native
`Tensor.pad()`/`.unfold()`/`.permute()`/`.max(axis)`/`.argmax(axis)` now
(GPU-dispatched, or a free view for `.permute()`); only the pad-strip crop
after `col2im`'s fold, and `Pooling`/`AveragePooling`'s own backward scatter,
are still plain Culebra loops — the former since `Tensor.slice()` only takes
axis 0, the latter since there's no native gather/scatter-by-index op yet.
`conv_mnist.cul` still trains on a small subset rather than the full 60k
images.

## Examples

| File | Demonstrates |
|---|---|
| `tanh.cul` | Higher-order derivatives via repeated `backward(create_graph=true)` |
| `spiral.cul` | MLP + SGD on the classic 3-class spiral toy dataset |
| `mnist.cul` | MLP + Adam on the full MNIST training set (needs `just fetch-mnist`) |
| `conv_mnist.cul` | A small CNN (conv-relu-pool ×2 + linear head) on an MNIST subset |
| `rnn_sin.cul` | LSTM + truncated BPTT predicting the next point of a noisy sine wave |

## Running

```bash
culebra examples/dezero/examples/spiral.cul
culebra --jit examples/dezero/examples/mnist.cul        # needs `just fetch-mnist` first
culebra --jit examples/dezero/examples/conv_mnist.cul
culebra --jit examples/dezero/examples/rnn_sin.cul
culebra test examples/dezero                             # runs every test_*.cul under here
```
