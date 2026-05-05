#!/usr/bin/env python3
"""Pure Python inference (no numpy, scalar loops).

The reference shape that `infer.cul` is matched against. Both walk
the same nested-loop dot-product structure; differences in timing
between this and `infer.cul` therefore reflect interpreter quality
rather than algorithmic differences.
"""

import math
import time
from pathlib import Path

DIR = Path(__file__).parent
N_IN, N_HID, N_OUT = 784, 30, 10


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

    t0 = time.perf_counter()
    correct = 0
    for k in range(n):
        if predict(X[k], W1, b1, W2, b2) == y[k]:
            correct += 1
    t_infer = time.perf_counter() - t0

    acc = correct / n
    print(f"[pure]  inference: {n} predictions in {t_infer:.3f}s "
          f"({1000 * t_infer / n:.3f} ms/img)")
    print(f"[pure]  accuracy: {acc:.4f}")


if __name__ == "__main__":
    main()
