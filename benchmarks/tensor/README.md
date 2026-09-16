# Culebra Tensor against PyTorch

Three workloads, each written once in Culebra and once in PyTorch, run on
the same machine by one script that reports the ratio per row:

| workload | files | what a gap here means |
|---|---|---|
| **GPT training step** | `gpt_train.cul`, `gpt_train.py` | a program that trains a model pays it: forward, backward, optimizer, and the host work between kernel launches |
| **single ops** | `ops.cul`, `ops.py` | one op on one shape, so a gap points at that op's kernel or launch |
| **transformer block forward** | `../mnist/train_bench_transformer.cul`, `../mnist/train_bench_transformer_torch.py` | one inference-shaped block, dominated by matrix products |

The training step is the row to read first. A single op or one forward
block can match PyTorch while a training loop built from the same ops does
not, because the backward pass, the optimizer and the bookkeeping between
steps are not in those rows.

## Running

```bash
just build                      # timings need the -O3 + LTO build
just bench-tensor               # GPU, 5 rounds
just bench-tensor cpu 3         # CPU, 3 rounds (GPT small only)

benchmarks/tensor/run.sh --rounds 5 --only gpt --configs small,medium
CULEBRA=./build-dev/culebra PYTHON=python3.11 benchmarks/tensor/run.sh
```

PyTorch is optional: without it, or without a GPU PyTorch can reach, the
table reports the Culebra column alone. On Apple silicon the PyTorch side
runs on `mps`.

`gpt_phases.cul` asks a different question of the same step — where does it
go? It runs the model of `gpt_train.cul` with a host read between the
forward, the backward and Adam, so each phase's kernels have finished before
the clock moves on, and `PHASES_ABLATE` swaps one op for a cheap stand-in of
the same shape, which makes the drop that op's share of the phase. It reports
Culebra alone; a ratio against PyTorch is what the table above is for.

```bash
./build/culebra --jit benchmarks/tensor/gpt_phases.cul gpu medium
PHASES_ABLATE=attn ./build/culebra --jit benchmarks/tensor/gpt_phases.cul gpu medium
```

## Method

- **Rounds, alternating, minimum.** Each round runs both sides back to
  back, Culebra first in odd rounds and PyTorch first in even ones; a cell
  is the minimum over rounds. A GPU's timing drifts with temperature and
  with whatever else the machine is doing, and two runs taken back to back
  see the same drift.
- **Both sides wait for the device.** `Tensor.eval` and `.detach()` return
  after the GPU has finished; the PyTorch scripts call
  `torch.cuda.synchronize()` (or `torch.mps.synchronize()`) before reading
  the clock.
- **The model learns.** The GPT step trains on one fixed batch, so its loss
  must fall from about `ln(vocab)`; a row whose loss did not fall on either
  side is flagged.
- **Same model, not the same code.** The GPT step matches in architecture,
  initialization (normal 0.02, zero biases), token ids and Adam
  hyperparameters. The PyTorch side is written as PyTorch programs are
  (`nn.Linear`, `F.scaled_dot_product_attention`, `F.cross_entropy`,
  `torch.optim.Adam` with its default implementation); the Culebra side
  follows the training loop the Tensor chapter of the stdlib reference
  describes (a leaf per parameter, `.detach()` on each update).

Sizes (vocab / width / heads / layers / sequence / batch):

| config | V | C | H | L | T | B | tokens per step |
|---|---:|---:|---:|---:|---:|---:|---:|
| small | 4096 | 256 | 4 | 4 | 256 | 8 | 2048 |
| medium | 8192 | 512 | 8 | 6 | 512 | 8 | 4096 |
| large | 16384 | 768 | 12 | 8 | 1024 | 4 | 4096 |

### Pitfalls this suite avoids

- `F.scaled_dot_product_attention` given `[H, T, D]` instead of
  `[B, H, T, D]` skips its fused kernels for the math path, two to three
  times slower (1024 rows, 8 heads of 64: 640 µs against 302 µs). An
  attention comparison against that path compares against something no
  PyTorch model runs.
- A CUDA timing without `torch.cuda.synchronize()` stops the clock when the
  kernels are queued, not when they finish. The MNIST scripts in
  `../mnist/` synchronized only for `mps` until 2026-09-14.
- A single transformer block reported as one process's mean against
  PyTorch's minimum over six repetitions is not the same statistic; both
  now report the same one.

## Numbers

RTX 3090 (driver 591.86) and i7-12700KF under WSL2, 2026-09-16. Culebra
0.6.0 built from this tree (`just build`), PyTorch 2.9.1+cu128. The ratio
is Culebra's time over PyTorch's, so below 1 means Culebra is faster.

### GPT training step (ms/step, minimum of 5 rounds)

| config | Culebra GPU | PyTorch CUDA | ratio |
|---|---:|---:|---:|
| small | 20.8 | 6.59 | 3.16 |
| medium | 79.2 | 45.4 | 1.75 |
| large | 206.8 | 132.6 | 1.56 |

The same step on the CPU (minimum of 3 rounds):

| config | Culebra CPU | PyTorch CPU | ratio |
|---|---:|---:|---:|
| small | 299.1 | 143.5 | 2.08 |

Both sides' losses fall by the same amount (small: 8.37 to 6.31 in
Culebra, 8.37 to 6.30 in PyTorch). The GPU ratio falls as the step grows
— 3.2 at small, 1.6 at large — while the single ops below sit between
0.76 and 1.16 of PyTorch, the cross-entropy aside. What is left is spread
across the backward pass rather than sitting in one op: its matrix
products, the LayerNorm pullback, and the optimizer's elementwise work.
The attention pullback used to be the exception — it rebuilt the
[H, T, T] scores the fused forward does not keep, and walked them — and
is now two kernels that rebuild them in registers instead: 1.38 ms
against that forward's 0.33 at one medium layer's shape.

### Single ops on the GPU (µs, minimum of 5 rounds)

| op | Culebra | PyTorch | ratio |
|---|---:|---:|---:|
| mm 256×1024×4096 | 138.5 | 131.0 | 1.06 |
| addmm 256×1024×4096 | 141.3 | 133.2 | 1.06 |
| mm 1024×1024×1024 | 149.1 | 138.1 | 1.08 |
| causal attention, 8 heads × 1024 rows × 64 | 244.6 | 322.9 | 0.76 |
| layer_norm 4096×1024 | 66.2 | 76.9 | 0.86 |
| softmax 1024×1024 | 35.4 | 33.7 | 1.05 |
| relu(x·y + z) 4096×1024 | 183.7 | 184.5 | 1.00 |
| sum over axis 1, 4096×1024 | 42.9 | 46.0 | 0.93 |
| index_select 4096 rows of 16384×768 | 66.2 | 57.3 | 1.16 |
| cross-entropy 4096×16384 | 4053.7 | 780.1 | 5.20 |

### Transformer block forward on the GPU (ms/iter, minimum of 5 rounds)

| seq × d_model | Culebra | PyTorch | ratio |
|---|---:|---:|---:|
| 256 × 512 | 0.247 | 0.260 | 0.95 |
| 256 × 768 | 0.383 | 0.368 | 1.04 |
| 256 × 1024 | 0.521 | 0.495 | 1.05 |
| 512 × 1024 | 0.981 | 0.889 | 1.10 |
