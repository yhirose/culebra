# microgpt on Culebra

Scalar autograd GPT port of [karpathy/microgpt](https://karpathy.github.io/2026/02/12/microgpt/),
used as a Phase 5.1 exercise and benchmark baseline for Culebra.

## Files

- `microgpt.py`   — Karpathy's reference, verbatim algorithm.
- `microgpt.cul`  — Culebra scalar port. Same hyperparameters.
- `names.txt`     — Training data (gitignored). Fetch with `just fetch-names`.

## Running

```
just fetch-names                                         # one-time: download names.txt
python3 samples/microgpt/microgpt.py [num_steps]
./build/culebra       samples/microgpt/microgpt.cul -- [num_steps]
./build/culebra --jit samples/microgpt/microgpt.cul -- [num_steps]
```

`num_steps` defaults to 1000. Fewer steps train less; inference is unaffected.

## Benchmark note

The scalar autograd formulation creates one `Value` object per intermediate
arithmetic result. Each forward pass produces O(10⁵) objects. Culebra's
`class`-sugar Object allocation is 10–100× slower per object than CPython's
`__slots__` class, so the Culebra scalar port is *expected* to be much slower
than Python on this workload. See `docs/phase5_benchmark.md` for numbers.

The matrix-abstraction port (Phase 5.4) reduces total Object count by orders
of magnitude and should close the gap — that's the entire point of the
experiment.
