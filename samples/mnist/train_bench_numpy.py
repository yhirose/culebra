#!/usr/bin/env python3
"""numpy training benchmark: 1 epoch over the training subset with
hand-coded backprop. Runs `CYCLES` epochs from identical init weights
in-process, reports cold (cycle 1) vs warm-mean (cycles 2..K)."""

import time
from pathlib import Path
import numpy as np

DIR = Path(__file__).parent

BATCH = 10
ETA = 3.0
N_OUT = 10
CYCLES = 2


def sigmoid(z: np.ndarray) -> np.ndarray:
    return 1.0 / (1.0 + np.exp(-z))


def run_epoch(W1, b1, W2, b2, X, Y, n):
    for s in range(0, n, BATCH):
        x = X[s:s + BATCH].T
        t = Y[s:s + BATCH].T

        z1 = W1 @ x + b1[:, None]
        a1 = sigmoid(z1)
        z2 = W2 @ a1 + b2[:, None]
        a2 = sigmoid(z2)

        d2 = (a2 - t) * a2 * (1.0 - a2)
        d1 = (W2.T @ d2) * a1 * (1.0 - a1)

        W2 -= ETA * (d2 @ a1.T) / BATCH
        b2 -= ETA * d2.mean(axis=1)
        W1 -= ETA * (d1 @ x.T) / BATCH
        b1 -= ETA * d1.mean(axis=1)


def main() -> None:
    t0 = time.perf_counter()
    init_W1 = np.loadtxt(DIR / "init_W1.csv", delimiter=",")
    init_b1 = np.loadtxt(DIR / "init_b1.csv", delimiter=",")
    init_W2 = np.loadtxt(DIR / "init_W2.csv", delimiter=",")
    init_b2 = np.loadtxt(DIR / "init_b2.csv", delimiter=",")
    X = np.loadtxt(DIR / "train_images.csv", delimiter=",")
    y = np.loadtxt(DIR / "train_labels.csv", delimiter=",", dtype=np.int64)
    Xt = np.loadtxt(DIR / "test_images.csv", delimiter=",")
    yt = np.loadtxt(DIR / "test_labels.csv", delimiter=",", dtype=np.int64)
    t_load = time.perf_counter() - t0

    n = X.shape[0]
    Y = np.zeros((n, N_OUT))
    Y[np.arange(n), y] = 1.0

    print(f"[numpy-train] loaded train={n} test={Xt.shape[0]} "
          f"in {t_load:.3f}s")

    times = []
    W1 = b1 = W2 = b2 = None
    for _ in range(CYCLES):
        W1 = init_W1.copy()
        b1 = init_b1.copy()
        W2 = init_W2.copy()
        b2 = init_b2.copy()
        t0 = time.perf_counter()
        run_epoch(W1, b1, W2, b2, X, Y, n)
        times.append(time.perf_counter() - t0)

    z1 = W1 @ Xt.T + b1[:, None]
    a1 = sigmoid(z1)
    z2 = W2 @ a1 + b2[:, None]
    a2 = sigmoid(z2)
    preds = a2.argmax(axis=0)
    acc = float((preds == yt).mean())

    cold = times[0]
    warm = sum(times[1:]) / (CYCLES - 1) if CYCLES > 1 else float("nan")
    print(f"[numpy-train] cold={cold:.3f}s warm={warm:.3f}s "
          f"accuracy={acc:.4f}")
    print(f"BENCH label=numpy_train load={t_load:.4f} cold={cold:.4f} "
          f"warm={warm:.4f} accuracy={acc:.4f}")


if __name__ == "__main__":
    main()
