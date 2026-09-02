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
| `codebot/tokenizer.cul` | the finished byte-level BPE the later chapters import |
| `ch01/` | tokenizers, from code points to a trained BPE (9 scripts) |
| `common/` | path resolution and the `uint16` corpus format |

Still to come: ch02 (attention from scratch), ch03 (pretraining, generation,
SFT, GRPO), ch04 (a fast BPE), ch05 (RoPE, SwiGLU, RMSNorm, KV cache), ch06
(AdamW, schedulers, DPO, LLM-as-judge), ch07 and ch09 with `storybot` and
`webbot`.

## Layout

```
codebot/tokenizer.cul   the shared BPE: pretokenize, train, encode, decode
common/paths.cul        data/ and checkpoints/ resolved from the running script
common/data.cul         .txt and uint16 .bin readers, upstream's own formats
data/                   what is small enough to commit; the rest is fetched
ch01/                   one script per section, numbered as upstream numbers them
test_*.cul              every test, flat at the package root
```

## Running

```bash
culebra examples/deep-learning-from-scratch-6/ch01/01_char_tokenizer.cul
culebra examples/deep-learning-from-scratch-6/ch01/06_pretokenize.cul
culebra examples/deep-learning-from-scratch-6/ch01/07_tiny_codes.cul
culebra examples/deep-learning-from-scratch-6/ch01/08_eval.cul
culebra test examples/deep-learning-from-scratch-6
```

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

Fetched by `just fetch-llm-data`, never committed: `tiny_codes.txt` (6.5 MB)
and the corpora the later chapters need. Anything a script generates —
`*.bin`, `merge_rules_1000.json`, checkpoints — is ignored by git.

Upstream ships its merge rules as a pickle, which Culebra cannot read, so
this port retrains them and stores them as JSON. That the two agree is
itself a test of the implementation: the tie-break has to match upstream's
`max(counts, key=lambda p: (counts[p], p[0], p[1]))` exactly, or the two
vocabularies diverge on the first ambiguous pair.

## What maps to what

| Upstream | Here |
|---|---|
| `ch01/01_char_tokenizer.py` … `09_bpe_encode.py` | `ch01/*.cul`, same numbering |
| `codebot/tokenizer.py` | `codebot/tokenizer.cul` |
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
