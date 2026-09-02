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

## Status

This port is being built chapter by chapter. What runs today:

| | |
|---|---|
| `codebot/tokenizer.cul` | the byte-level BPE the chapters converge on |
| `codebot/model.cul` | the GPT: causal multi-head attention, pre-norm blocks, tied weights |
| `codebot/utils.cul` | sampling, with temperature and top-k |
| `storybot/tokenizer.cul` | the same BPE, fast enough for a whole corpus |
| `ch01/` | tokenizers, from code points to a trained BPE (9 scripts) |
| `ch02/` | attention, from a soft dictionary to the assembled GPT (8 scripts) |
| `ch04/` | making that BPE 681x faster, one idea at a time (6 scripts) |
| `train/` | pretraining and generation |
| `common/` | configuration, checkpoints, paths, the `uint16` corpus format, ASCII plots |

Still to come: SFT and GRPO, the improved model (RoPE, SwiGLU, RMSNorm, KV
cache) as `storybot`, AdamW and learning-rate schedules, DPO, `webbot`'s
grouped-query attention, and the contrast implementation on culebra's own
`Tensor` autograd.

## Layout

```
codebot/tokenizer.cul   the shared BPE: pretokenize, train, encode, decode
storybot/tokenizer.cul  the same BPE, deduplicated, incremental and parallel
codebot/model.cul       the GPT the training scripts import
codebot/utils.cul       sampling one token at a time
common/config.cul       the book's hyperparameters, and the scaled-down default
common/checkpoint.cul   torch.save's job, as CSV plus a manifest
common/paths.cul        data/ and checkpoints/ resolved from the running script
common/data.cul         the uint16 .bin format and batch sampling
common/plot.cul         curves and histograms, drawn in text
train/                  pretraining and generation
data/                   what is small enough to commit; the rest is fetched
ch01/, ch02/, ch04/     one script per section, numbered as upstream numbers them
test_*.cul              every test, flat at the package root
```

## Running

```bash
cd examples/deep-learning-from-scratch-6

culebra ch01/07_tiny_codes.cul      # train a BPE the straightforward way
culebra ch04/02_bpe_cache.cul       # and then make it fast
culebra ch02/08_multi_head.cul      # attention, one section at a time
culebra ch02/10_gpt2.cul            # the assembled GPT, untrained

culebra train/pretrain.cul          # ~20 s at the default size
culebra train/generate.cul          # sample from what it just wrote

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
to build. At the default size the model is 0.17M parameters trained for
300 steps on 100k tokens: it does not write working code, but it does
produce indentation, keywords and balanced quotes, which is the point.

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
| `ch04/01_bpe_optimize.py` … `07_encode_parallel.py` | `ch04/*.cul`, same numbering |
| `codebot/tokenizer.py` | `codebot/tokenizer.cul` |
| `storybot/tokenizer.py` | `storybot/tokenizer.cul` |
| `multiprocessing.Pool` | `Parallel.map` over isolates |
| `os.chdir(...)` + `sys.path.append('.')` preamble | `common/paths.cul` |
| `np.fromfile(dtype=np.uint16)` / `.tofile()` | `common/data.cul` |

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
- **An isolate worker sees its own module, and only its own module.** It
  reads same-module globals — a compiled `Regex` included — but a value
  read off an *imported* module is a capture, and a module object is not
  Sendable. So `storybot/tokenizer.cul` stands alone rather than importing
  `codebot`, which is also how upstream writes it.
