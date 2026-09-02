# data/

Two files are committed, because every script needs something to run
against and 256 KB is small enough to carry:

| File | Size | How it was made |
|---|---|---|
| `tiny_codes_sample.txt` | 256 KB | the first 1,372 documents of upstream's `codebot/tiny_codes.txt`, cut on `<\|endoftext\|>` boundaries |
| `merge_rules_sample.json` | 10 KB | `culebra ch01/07_tiny_codes.cul` over that sample |

`just fetch-llm-data` downloads the rest into this directory (about
325 MB); none of it is committed:

| File | Size | Used by |
|---|---|---|
| `tiny_codes.txt` | 6.5 MB | ch01, ch03, ch04 with `--full` |
| `tiny_codes.bin` | 5.3 MB | upstream's own tokenization of the above, for comparison |
| `tiny_codes_sft.json` | 126 KB | ch03's supervised fine-tuning |
| `tiny_stories_dpo.json` | 73 KB | ch06's DPO |
| `tiny_stories_valid.txt` | 22.5 MB | ch06's `storybot` |
| `owt_valid.txt` | 290 MB | ch07 and ch09's `webbot` |

Everything a script writes is ignored by git:

| File | Made by |
|---|---|
| `merge_rules_1000.json` | `ch04/04_bpe_parallel.cul --full` (6.8 s), or `ch01/07_tiny_codes.cul --full` (4,634 s, same 743 rules) |
| `merge_rules_sample.json` | `ch01/07_tiny_codes.cul` — committed, but reproducible |
| `*.bin` | `ch01/09_bpe_encode.cul` or `ch04/07_encode_parallel.cul` |

## Regenerating the committed files

```bash
just fetch-llm-data
culebra examples/deep-learning-from-scratch-6/ch01/07_tiny_codes.cul
```

The sample itself was cut with this, run once from the repo root:

```python
text = open("examples/deep-learning-from-scratch-6/data/tiny_codes.txt").read()
sep, out, total = "<|endoftext|>", [], 0
for doc in text.split(sep):
    if total >= 256 * 1024:
        break
    out.append(doc)
    total += len(doc.encode()) + len(sep)
open("examples/deep-learning-from-scratch-6/data/tiny_codes_sample.txt", "w").write(
    sep.join(out) + sep)
```

## What is deliberately not here

Upstream's own training splits, which this port cannot use:

| File | Size | Why not |
|---|---|---|
| `tiny_stories_train.txt` | 2.2 GB | the 22.5 MB validation split makes the same point |
| `owt_train.txt` | 11.9 GB | as Culebra byte Arrays that is 95 GB of memory |
| `owt_train.bin` | 5.3 GB | 2.65 billion tokens; ch09's full run is ~52 days here |

Also absent: upstream's `merge_rules.pkl` and `model_*.pt`. Culebra reads
neither pickle nor a PyTorch checkpoint, so this port retrains the merge
rules and writes its own checkpoints as CSV.
