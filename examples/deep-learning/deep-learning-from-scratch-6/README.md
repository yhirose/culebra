# deep-learning-from-scratch-6 (Culebra port)

A Culebra port of the code from *Deep Learning from Scratch 6 — LLMs*
(Koki Saitoh, O'Reilly Japan), which builds a GPT-style language model from
a byte-level tokenizer up to pretraining, alignment and inference.
[Upstream](https://github.com/oreilly-japan/deep-learning-from-scratch-6) is
PyTorch and MIT-licensed; this is a from-scratch reimplementation of its
algorithms in Culebra, pinned to upstream `c9b6e2e`.

Where upstream uses PyTorch's autograd, this port uses
[`examples/dezero`](../dezero) — this repo's port of the framework the
*previous* book builds. The rule for what goes where is one line:

> **What book 3 already taught, borrow from dezero. What book 6 teaches,
> write here.**

So `Linear`, `LayerNorm`, `EmbedID`, `dropout`, `softmax_cross_entropy` and
`Adam` come from dezero, while the attention block, RoPE, the KV cache,
AdamW, the schedulers and the whole BPE tokenizer are written here.

## What is here

| | |
|---|---|
| `ch01/` | tokenizers, from code points to a trained BPE (9 scripts) |
| `ch02/` | attention, from a soft dictionary to the assembled GPT (8 scripts) |
| `ch04/` | making that BPE 681x faster, one idea at a time (6 scripts) |
| `codebot/` | the byte-level BPE, the GPT, and sampling |
| `storybot/` | the same BPE at corpus scale, and the improved GPT: RoPE, RMSNorm, SwiGLU, KV cache |
| `webbot/` | that model with grouped-query attention |
| `train/` | pretraining, generation, SFT, GRPO, AdamW and schedules, and the contrast implementation |
| `common/` | configuration, checkpoints, paths, the `uint16` corpus format, ASCII plots |

Upstream's chapters 3, 5, 6, 7 and 9 are here as `train/` and the three
bots rather than as numbered scripts. Those chapters are a progression of
training regimes over one model, and each of their scripts is a driver;
folding them into one file per technique keeps the technique legible and
loses nothing. Chapters 1, 2 and 4 stay numbered, because there each script
*is* a step of an argument.

## What is not here

Full-scale runs, for reasons that are arithmetic rather than choice, and
which the scripts print rather than hide:

- **Chapter 7's own corpus.** 11.9 GB of text in a Culebra byte Array
  (8 bytes an element) is 95 GB. This machine has 15 GB. Chunking moves the
  text but not the pair-count table. The default is the first 20 MB of
  `owt_valid.txt`; `--full` is all 290 MB of it.
- **Chapter 9 at its own size.** 114M parameters x 100,000 iterations x an
  effective batch of 128 x a context of 1024 is 13.1 billion tokens, about
  9 EFLOP. At the 2.0 TFLOP/s this machine's Metal backend actually sustains
  that is 52 days with dezero's overhead counted as zero. Upstream assumes
  eight A100s and one day.
- **Distributed training.** Culebra has isolates, channels and
  `SharedBuffer`, so single-machine data parallelism is writable, but
  `Tensor.use_gpu()` is a process-wide setting and there is one GPU. Chapter
  9's DDP folds into the gradient accumulation upstream already has.
- **Mixed precision.** Culebra's `Tensor` is f32 with no other dtype, so
  there is no autocast to port. What does port is the part that is not about
  dtype: loss scaling, and skipping a step whose gradients went non-finite.
- **An LLM judge over HTTP.** Preference data is generated locally and
  deterministically instead, so the tests need no network and no key.

## Layout

```
codebot/tokenizer.cul   the shared BPE: pretokenize, train, encode, decode
codebot/model.cul       the GPT the training scripts import
codebot/utils.cul       sampling one token at a time
storybot/tokenizer.cul  the same BPE, deduplicated, incremental and parallel
storybot/model.cul      the improved GPT: RoPE, RMSNorm, SwiGLU
storybot/utils.cul      the same sampling, with chapter 5's KV cache
webbot/model.cul        storybot with grouped-query attention -- only the delta
webbot/utils.cul        re-exports storybot's; the cache needed no change

train/pretrain.cul      the training loop
train/generate.cul      sampling from a checkpoint
train/sft.cul           instruction tuning, on a masked loss
train/grpo.cul          learning from a reward, the group as its own baseline
train/optim.cul         AdamW, warmup + cosine decay
train/losses.cul        cross-entropy that ignores some of its targets
train/pretrain_tensor.cul  the same loop on culebra's own Tensor autograd

common/config.cul       the book's hyperparameters, and the scaled-down default
common/checkpoint.cul   torch.save's job, as CSV plus a manifest
common/paths.cul        data/ and checkpoints/ resolved from the running script
common/data.cul         the uint16 .bin format and batch sampling
common/plot.cul         curves and histograms, drawn in text

data/                   what is small enough to commit; the rest is fetched
ch01/, ch02/, ch04/     one script per section, numbered as upstream numbers them
test_*.cul              every test, flat at the package root
```

`common/` and `train/` are the only directories upstream does not have.
Upstream's chapter scripts each open with `os.chdir(...)` and
`sys.path.append('.')` and then repeat their configuration inline; putting
paths, configuration and checkpoints in one place each is what replaces
that.

## Running

```bash
cd examples/deep-learning-from-scratch-6

culebra ch01/07_tiny_codes.cul      # train a BPE the straightforward way
culebra ch04/02_bpe_cache.cul       # and then make it fast
culebra ch02/08_multi_head.cul      # attention, one section at a time
culebra ch02/10_gpt2.cul            # the assembled GPT, untrained

culebra train/pretrain.cul          # ~15 s at the default size
culebra train/generate.cul          # sample from what it just wrote
culebra train/sft.cul               # then teach it to answer
culebra train/grpo.cul              # then to add up

culebra train/lr_schedule.cul       # what warmup + cosine decay looks like

culebra test .
```

Training and generation take the same settings: `--full` for the book's
own hyperparameters, or `key=value` for one at a time.

```bash
culebra train/pretrain.cul max_iters=2000 n_layer=4
culebra train/generate.cul temperature=0.0 prompt='def '
```

A checkpoint carries the configuration it was trained with, so
`generate.cul` rebuilds the same architecture rather than being told what
to build, and each stage reads the one before it: `sft.cul` starts from
what `pretrain.cul` wrote, `grpo.cul` from what `sft.cul` wrote.

At the default size the model is 0.17M parameters trained for 300 steps on
100k tokens. It does not write working code. It does write things shaped
like code -- `command = input('Enter`, balanced quotes, indentation -- and
the point of the default is that the whole pipeline runs in under a minute,
not that the result is good.

`grpo.cul` at that size will report an accuracy of zero and no gradient,
and that is the arithmetic rather than a fault: a model that is never right
gives every answer in a group the same reward, every advantage is then zero,
and GRPO correctly concludes there is nothing to learn from. The step line
prints how many groups carried a signal so this reads as what it is. It
needs a model good enough to be right sometimes, which is `--full`.

The executor is the right engine for these. `--jit` is fine on ch01, which
imports no tensors (0.42 s against the executor's 0.02 s), but from ch02 on
these scripts import dezero, and lowering that to LLVM costs about 40 s of
startup before the first line runs -- worth it only for a long training run.

Scripts that read a corpus take `--full` to use the real one instead of the
committed sample; that needs `just fetch-llm-data` from the repo root.

## Data

Committed, because they are small and every script needs something to run
against:

| File | Size | What |
|---|---|---|
| `data/tiny_codes_sample.txt` | 256 KB | the first 1,372 documents of upstream's TinyCodes corpus |
| `data/merge_rules_sample.json` | 10 KB | what `ch01/07_tiny_codes.cul` learns from that sample |

`just fetch-llm-data` downloads the rest — about 325 MB, none of it
committed. Anything a script generates (`*.bin`, `merge_rules_1000.json`,
checkpoints) is ignored by git too. [`data/README.md`](data/README.md) says
where each file comes from, how to regenerate the committed ones, and which
of upstream's corpora are deliberately absent.

The vocabulary the later chapters load, `data/merge_rules_1000.json`, comes
from `ch04/04_bpe_parallel.cul --full` in 6.8 s. `ch01/07_tiny_codes.cul
--full` writes the same file with the straightforward trainer, in 4,634 s
(77 minutes) — which is what ch04 exists to fix. Both were measured on the
6.5 MB corpus with an `-O3` build; both produce the same 743 rules.

Upstream ships its merge rules as a pickle, which Culebra cannot read, so
this port retrains them and stores them as JSON. That the two agree is
itself a test of the implementation: the tie-break has to match upstream's
`max(counts, key=lambda p: (counts[p], p[0], p[1]))` exactly, or the two
vocabularies diverge on the first ambiguous pair.

## What maps to what

| Upstream | Here |
|---|---|
| `ch01/01_char_tokenizer.py` … `09_bpe_encode.py` | `ch01/*.cul`, same numbering |
| `ch02/01_soft_dict.py` … `10_gpt2.py` | `ch02/*.cul`, same numbering |
| `ch04/01_bpe_optimize.py` … `07_encode_parallel.py` | `ch04/*.cul`, same numbering |
| `ch03/01_pretrain.py`, `02_generate.py` | `train/pretrain.cul`, `train/generate.cul` |
| `ch03/03_sft.py` | `train/sft.cul` + `train/sft_data.cul` + `train/losses.cul` |
| `ch03/09_grpo.py` | `train/grpo.cul` + `train/reward.cul` |
| `ch05/*` (RoPE, SwiGLU, RMSNorm, KV cache) | `storybot/model.cul`, `storybot/utils.cul` |
| `ch06/*` (AdamW, schedulers) | `train/optim.cul`, `train/lr_schedule.cul` |
| `ch06/04_mixed_precision.py` | loss scaling only; there is no second dtype |
| `ch07/*` (corpus-scale BPE) | `storybot/tokenizer.cul` driven with `--full` |
| `ch09/*` (DDP pretraining) | `train/pretrain.cul` with gradient accumulation |
| `codebot/tokenizer.py`, `model.py`, `utils.py` | `codebot/*.cul` |
| `storybot/tokenizer.py`, `model.py`, `utils.py` | `storybot/*.cul` |
| `webbot/model.py`, `utils.py` | `webbot/*.cul` |
| `ch02/graph.py`, `ch05/graph.py`, `ch06/lr_graph.py` | `common/plot.cul`, drawn in text |
| `torch.save` / `load_state_dict` | `common/checkpoint.cul` |
| `multiprocessing.Pool` | `Parallel.map` over isolates |
| `os.chdir(...)` + `sys.path.append('.')` preamble | `common/paths.cul` |
| `np.fromfile(dtype=np.uint16)` / `.tofile()` | `common/data.cul` |
| `tqdm` | elapsed time, printed |

## Two autograds, side by side

`train/pretrain.cul` runs on [`examples/dezero`](../dezero), whose autograd
is written in Culebra: every operation allocates an object and the tape is
walked by the interpreter. `train/pretrain_tensor.cul` is the same model and
the same loop written directly against `Tensor.backward()`, whose tape is in
C++. Given the same initial weights they agree bit-for-bit on the first loss
and to a relative 1e-7 after eight steps, and a checkpoint written by either
restores into the other.

The C++ tape wins by a wide margin on a small model and the margin narrows
as the tensors grow:

| settings | dezero | native `Tensor` |
|---|---|---|
| `n_layer=1 embed_dim=16 n_head=4 ff_dim=32 context_len=8 batch_size=2` | 2.46 | 0.92 |
| `n_layer=2 embed_dim=32 n_head=4 ff_dim=128 context_len=32 batch_size=4` | 9.03 | 6.73 |
| the default configuration (embed 64, context 64, batch 8, 2 layers) | 42.0 | 42.1 |

ms/step, best of three with the `-O3` build over 60 steps, using the ms/step
each script prints, which excludes validation. The narrowing is not
per-element overhead: dezero's operations call the same C++ kernels, so its
extra cost is per operation and thins out as tensors grow.

The default configuration used to be a loss — 41.7 against 46.4 — and
writing this file is what found the two gaps behind it. Both are now closed,
and both were the same shape of gap: an operation dezero could write a
backward for by hand, where the native side had only the composition.

- **The head split.** `.permute()` had no VJP, so the split could not be
  written the way the dezero model writes it — `.transpose()` reverses every
  axis, which from rank 3 up is a different operation — and it was built out
  of per-head `narrow` and `concat` instead, whose own VJP allocates a zero
  buffer the size of the whole input for each slice. `.permute()` is
  differentiable now: 0.354 ms against the workaround's 1.080 at this
  model's head-split shape, and 48.6 ms/step to 46.4 overall.
- **The loss.** dezero's `SoftmaxCrossEntropy` fuses the softmax and the
  cross-entropy into one `Function` whose backward is the closed form
  `(p - onehot) / n`, the `/ n` being its forward's batch mean.
  Differentiating the composition instead cost 4.2 ms against a 4.0 ms
  forward at this model's logits shape. `.softmax_cross_entropy()` is now a
  fused native op with the same closed form, which takes the file from 46.4
  to 42.1. It answers one loss per row and leaves the mean to the caller, so
  the `/ n` lives in the `.mean(0)` this file writes; a loss that scores only
  some rows — SFT's, which the dezero side writes out by hand in
  `train/losses.cul` — is then a masked sum over the same rows.

## Where this differs from upstream

- **A pair of ids is a packed `Long`**, not a tuple, because Object keys are
  hashed. `id1 * 2^20 + id2` is also monotonic in `(id1, id2)`, which turns
  upstream's three-element tie-break key into a two-scalar comparison.
- **Merge rules are an ordered Array of `[id1, id2, new_id]`**, not a dict.
  `encode` has to replay them in training order, so the order is explicit
  rather than inherited from a map's iteration order.
- **`decode` keeps invalid UTF-8 as raw bytes** where upstream's
  `errors="replace"` substitutes U+FFFD. Culebra Strings carry arbitrary
  bytes, so a token cut mid-character round-trips instead of being lost.
- **`tqdm` progress bars are dropped**; the long-running scripts print
  elapsed time instead.
- **A pre-token census prefixes its keys with `#`.** An Object doubles as
  the dictionary type and an own entry shadows the dict builtins, so a
  corpus containing the pre-token `get` would turn `counts.get(...)` into a
  `TypeError`. A Python corpus contains it. `#` cannot start an identifier,
  so the prefix keeps every builtin reachable.
- **The book's chapters 3, 5, 6, 7 and 9 are one file per technique**, not
  one per section. See "What is here".
- **`webbot/model.cul` imports what it did not change.** Upstream copies
  `storybot/model.py` and edits the attention; here the file is the delta, so
  a reader sees grouped-query attention and nothing else. With
  `n_kv_head == n_head` its logits are bit-identical to storybot's, which a
  test asserts -- though that check cannot see the *order* the groups are
  laid out in, because at one query head per group there is no order to
  see, so a second test pins that separately.
- **RoPE is written out rather than calling `Tensor.rope`.** The native one
  uses the half-split convention where the book uses interleaved. The two
  are the same rotation under a fixed permutation of the head dimension, and
  a test asserts that identity — but permuting Q and K there and back costs
  more than the rotation, and `Tensor.rope` carries no VJP.
- **The embedding tables are scaled to 0.02 at initialisation.** dezero's
  `EmbedID` draws at unit variance, and because the unembedding is that
  table transposed, the untouched scale is the logit scale: at the book's
  width the softmax starts saturated and the first loss sits exactly on
  `-log(1e-15)`, the clamp inside the cross-entropy.
- **An isolate worker sees its own module, and only its own module.** It
  reads same-module globals — a compiled `Regex` included — but a value
  read off an *imported* module is a capture, and a module object is not
  Sendable. So `storybot/tokenizer.cul` stands alone rather than importing
  `codebot`, which is also how upstream writes it.
