#!/usr/bin/env python3
"""Pure Python training benchmark: 1 epoch, hand-coded backprop, no numpy.

CYCLES=1 — pure Python has no JIT/BLAS warmup and the run is heavy
(~30s on N=10000), so we skip the warm-mean and only emit cold."""

import math
import time
from pathlib import Path

DIR = Path(__file__).parent

N_IN, N_HID, N_OUT = 784, 30, 10
BATCH = 10
ETA = 3.0
CYCLES = 1


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


def zeros_2d(rows: int, cols: int) -> list[list[float]]:
    return [[0.0] * cols for _ in range(rows)]


def zeros_1d(n: int) -> list[float]:
    return [0.0] * n


def predict(x, W1, b1, W2, b2) -> int:
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


def run_epoch(W1, b1, W2, b2, X, y, n):
    lr = ETA / BATCH
    for s in range(0, n, BATCH):
        gW1 = zeros_2d(N_HID, N_IN)
        gb1 = zeros_1d(N_HID)
        gW2 = zeros_2d(N_OUT, N_HID)
        gb2 = zeros_1d(N_OUT)

        for k in range(BATCH):
            x = X[s + k]
            label = y[s + k]

            a1 = [0.0] * N_HID
            for i in range(N_HID):
                z = b1[i]
                row = W1[i]
                for j in range(N_IN):
                    z += row[j] * x[j]
                a1[i] = sigmoid(z)

            a2 = [0.0] * N_OUT
            for i in range(N_OUT):
                z = b2[i]
                row = W2[i]
                for j in range(N_HID):
                    z += row[j] * a1[j]
                a2[i] = sigmoid(z)

            d2 = [0.0] * N_OUT
            for i in range(N_OUT):
                t = 1.0 if i == label else 0.0
                d2[i] = (a2[i] - t) * a2[i] * (1.0 - a2[i])

            d1 = [0.0] * N_HID
            for i in range(N_HID):
                z = 0.0
                for kk in range(N_OUT):
                    z += W2[kk][i] * d2[kk]
                d1[i] = z * a1[i] * (1.0 - a1[i])

            for i in range(N_OUT):
                gb2[i] += d2[i]
                row = gW2[i]
                for j in range(N_HID):
                    row[j] += d2[i] * a1[j]
            for i in range(N_HID):
                gb1[i] += d1[i]
                row = gW1[i]
                for j in range(N_IN):
                    row[j] += d1[i] * x[j]

        for i in range(N_OUT):
            b2[i] -= lr * gb2[i]
            row = W2[i]
            grow = gW2[i]
            for j in range(N_HID):
                row[j] -= lr * grow[j]
        for i in range(N_HID):
            b1[i] -= lr * gb1[i]
            row = W1[i]
            grow = gW1[i]
            for j in range(N_IN):
                row[j] -= lr * grow[j]


def main() -> None:
    t0 = time.perf_counter()
    init_W1 = load_2d(DIR / "init_W1.csv")
    init_b1 = load_1d(DIR / "init_b1.csv")
    init_W2 = load_2d(DIR / "init_W2.csv")
    init_b2 = load_1d(DIR / "init_b2.csv")
    X = load_2d(DIR / "train_images.csv")
    y = load_int_1d(DIR / "train_labels.csv")
    Xt = load_2d(DIR / "test_images.csv")
    yt = load_int_1d(DIR / "test_labels.csv")
    t_load = time.perf_counter() - t0
    n = len(X)
    print(f"[pure-train]  loaded train={n} test={len(Xt)} in {t_load:.3f}s")

    times = []
    W1 = b1 = W2 = b2 = None
    for _ in range(CYCLES):
        W1 = [row[:] for row in init_W1]
        b1 = init_b1[:]
        W2 = [row[:] for row in init_W2]
        b2 = init_b2[:]
        t0 = time.perf_counter()
        run_epoch(W1, b1, W2, b2, X, y, n)
        times.append(time.perf_counter() - t0)

    correct = 0
    for k in range(len(Xt)):
        if predict(Xt[k], W1, b1, W2, b2) == yt[k]:
            correct += 1
    acc = correct / len(Xt)

    cold = times[0]
    warm = sum(times[1:]) / (CYCLES - 1) if CYCLES > 1 else float("nan")
    print(f"[pure-train]  cold={cold:.3f}s warm={warm:.3f}s "
          f"accuracy={acc:.4f}")
    print(f"BENCH label=pure_train load={t_load:.4f} cold={cold:.4f} "
          f"warm={warm:.4f} accuracy={acc:.4f}")


if __name__ == "__main__":
    main()
