#!/usr/bin/env python3
"""numpy training benchmark: 1 epoch over the training subset with
hand-coded backprop (matched to the pure-Python and Culebra ports)."""

import time
from pathlib import Path
import numpy as np

DIR = Path(__file__).parent

BATCH = 10
ETA = 3.0
N_OUT = 10


def sigmoid(z: np.ndarray) -> np.ndarray:
    return 1.0 / (1.0 + np.exp(-z))


def main() -> None:
    t0 = time.perf_counter()
    W1 = np.loadtxt(DIR / "init_W1.csv", delimiter=",")
    b1 = np.loadtxt(DIR / "init_b1.csv", delimiter=",")
    W2 = np.loadtxt(DIR / "init_W2.csv", delimiter=",")
    b2 = np.loadtxt(DIR / "init_b2.csv", delimiter=",")
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

    t0 = time.perf_counter()
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
    t_train = time.perf_counter() - t0

    z1 = W1 @ Xt.T + b1[:, None]
    a1 = sigmoid(z1)
    z2 = W2 @ a1 + b2[:, None]
    a2 = sigmoid(z2)
    preds = a2.argmax(axis=0)
    acc = float((preds == yt).mean())

    print(f"[numpy-train] train: {n // BATCH} batches in {t_train:.3f}s "
          f"({1000 * t_train / (n // BATCH):.2f} ms/batch)")
    print(f"[numpy-train] test accuracy: {acc:.4f}")


if __name__ == "__main__":
    main()
