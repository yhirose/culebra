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
  layers.cul          Linear, RNN, LSTM, EmbedID, BatchNorm, LayerNorm,
                      MultiHeadAttention, GPTBlock
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
the host language. Elementwise `sin`/`cos`/`tanh`/`exp`/comparisons all
ride a native `Tensor` op directly now, as do `Dropout`'s keep mask and
`Max`/`Min`'s backward mask; `LeakyReLU`'s piecewise forward is the one
holdout still round-tripping through a nested Culebra Array (see
`functions.cul`'s own header comment). Two of those needed a detour around
a missing `Tensor` primitive: the keep mask wants a uniform draw and
`Tensor` only generates normals, so it inverts Box-Muller (for independent
`z1`, `z2` the value `exp(-(z1^2 + z2^2)/2)` is exactly uniform, and
comparing against a scalar threshold skips the `exp` entirely); and `Min`
reduces the negation and negates back, because `Tensor` has `.max()` but
no `.min()`.

Batched (rank >= 3) shapes work throughout. `matmul`'s VJP transposes only
the last two axes rather than reversing all of them, so a `[B, T, D]`
operand backprops, and `LayerNorm`, `Dropout`, `Max` and `Min` all handle
rank 3 and above — enough for a batched multi-head attention built on
`transpose(x, axes)` + batched `matmul`.

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
move, `pooling`'s own window gather, and `Pooling`'s own backward scatter,
all go through native `Tensor.pad()`/`.unfold()`/`.permute()`/
`.max(axis)`/`.argmax(axis)`/`.scatter_to_axis()` now (GPU-dispatched, or a
free view for `.permute()`). Still plain Culebra loops: the pad-strip crop
after `col2im`'s fold and after `Pooling`'s own scatter-fold (since
`Tensor.slice()` only takes axis 0), and `AveragePooling`'s own backward
(a smaller, separately-scoped follow-up — its scatter-add shape is the same
one `Pooling`'s own backward just moved onto `.scatter_to_axis()`/`.fold()`).
`conv_mnist.cul` still trains on a small subset rather than the full 60k
images.

### Graph lifetime

Upstream holds `Function.outputs` as `weakref`s, which keeps the forward
graph acyclic: refcounting reclaims a spent graph the moment the caller
drops the output. Culebra has no weak reference, so `Variable.backward`
breaks the cycle from the other side instead. It clears each visited
`Function`'s `output` field once that node's backward has run, and it
scopes `enable_backprop` over the gradient accumulation as well as over
`backward` itself — upstream's `with using_config('enable_backprop',
create_graph)` block spans both, so `x.grad + gx` records no graph of its
own. Either edge left in place makes every op (the first) or every diamond
(the second) leave a cycle behind that only the collector can reclaim.
That is invisible on the small examples here and expensive on a
transformer-sized training loop, where the culebra objects are few and tiny
but each one owns megabytes of `Tensor` payload the collector's own byte
threshold cannot see: measured on a 11.4M-parameter GPT step, dropping both
edges cut wall clock 2.5x and peak memory 8x. Both are skipped when
`create_graph` is on, where the graph is deliberately kept for a
higher-order derivative.

## Examples

| File | Demonstrates |
|---|---|
| `tanh.cul` | Higher-order derivatives via repeated `backward(create_graph=true)` |
| `spiral.cul` | MLP + SGD on the classic 3-class spiral toy dataset |
| `mnist.cul` | MLP + Adam on the full MNIST training set (needs `just fetch-mnist`) |
| `conv_mnist.cul` | A small CNN (conv-relu-pool ×2 + linear head) on an MNIST subset |
| `rnn_sin.cul` | LSTM + truncated BPTT predicting the next point of a noisy sine wave |
| `gpt_block.cul` | A from-scratch GPT decoder block (causal multi-head attention + RoPE + a gated MLP) + Adam on a tiny next-token toy task |

## Running

```bash
culebra examples/dezero/examples/spiral.cul
culebra --jit examples/dezero/examples/mnist.cul        # needs `just fetch-mnist` first
culebra --jit examples/dezero/examples/conv_mnist.cul
culebra --jit examples/dezero/examples/rnn_sin.cul
culebra --jit examples/dezero/examples/gpt_block.cul
culebra test examples/dezero                             # runs every test_*.cul under here
```
