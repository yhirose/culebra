#!/usr/bin/env python3
"""Pure Python inference (no numpy, scalar loops). Runs `CYCLES` cycles
in-process and reports cold/warm — pure Python has no JIT/BLAS warmup
so cold ≈ warm here, but the format matches the other implementations."""

import math
import time
from pathlib import Path

DIR = Path(__file__).parent
N_IN, N_HID, N_OUT = 784, 30, 10
CYCLES = 3


def load_2d(path: Path) -> list[list[float]]:
    with open(path) as f:
        return [
            [float(x) for x in line.split(",")]
            for line in f if line.strip()
        ]


def load_1d(path: Path) -> list[float]:
    with open(path) as f:
        return [float(line.strip()) for line in f if line.strip()]


def load_int_1d(path: Path) -> list[int]:
    with open(path) as f:
        return [int(line.strip()) for line in f if line.strip()]


def sigmoid(z: float) -> float:
    return 1.0 / (1.0 + math.exp(-z))


def predict(
    x: list[float],
    W1: list[list[float]], b1: list[float],
    W2: list[list[float]], b2: list[float],
) -> int:
    a1 = [0.0] * N_HID
    for i in range(N_HID):
        s = b1[i]
        row = W1[i]
        for j in range(N_IN):
            s += row[j] * x[j]
        a1[i] = sigmoid(s)

    best_v = -1.0
    best_i = 0
    for i in range(N_OUT):
        s = b2[i]
        row = W2[i]
        for j in range(N_HID):
            s += row[j] * a1[j]
        v = sigmoid(s)
        if v > best_v:
            best_v = v
            best_i = i
    return best_i


def main() -> None:
    t0 = time.perf_counter()
    W1 = load_2d(DIR / "W1.csv")
    b1 = load_1d(DIR / "b1.csv")
    W2 = load_2d(DIR / "W2.csv")
    b2 = load_1d(DIR / "b2.csv")
    X = load_2d(DIR / "test_images.csv")
    y = load_int_1d(DIR / "test_labels.csv")
    t_load = time.perf_counter() - t0

    n = len(X)
    print(f"[pure]  loaded N={n} in {t_load:.3f}s "
          f"(W1={len(W1)}x{len(W1[0])}, W2={len(W2)}x{len(W2[0])})")

    times = []
    correct = 0
    for cycle in range(CYCLES):
        t0 = time.perf_counter()
        c = 0
        for k in range(n):
            if predict(X[k], W1, b1, W2, b2) == y[k]:
                c += 1
        times.append(time.perf_counter() - t0)
        correct = c

    acc = correct / n
    cold = times[0]
    warm = sum(times[1:]) / (CYCLES - 1) if CYCLES > 1 else float("nan")
    print(f"[pure]  cold={cold:.3f}s warm={warm:.3f}s accuracy={acc:.4f}")
    print(f"BENCH label=pure_infer load={t_load:.4f} cold={cold:.4f} "
          f"warm={warm:.4f} accuracy={acc:.4f}")


if __name__ == "__main__":
    main()
