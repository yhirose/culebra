#!/usr/bin/env python3
"""numpy inference baseline (BLAS path).

Loads CSV weights and test set, runs forward pass for all images,
prints timing and accuracy. Times loading and inference separately.
"""

import time
from pathlib import Path
import numpy as np

DIR = Path(__file__).parent


def sigmoid(z: np.ndarray) -> np.ndarray:
    return 1.0 / (1.0 + np.exp(-z))


def main() -> None:
    t0 = time.perf_counter()
    W1 = np.loadtxt(DIR / "W1.csv", delimiter=",")
    b1 = np.loadtxt(DIR / "b1.csv", delimiter=",")
    W2 = np.loadtxt(DIR / "W2.csv", delimiter=",")
    b2 = np.loadtxt(DIR / "b2.csv", delimiter=",")
    X = np.loadtxt(DIR / "test_images.csv", delimiter=",")
    y = np.loadtxt(DIR / "test_labels.csv", delimiter=",", dtype=np.int64)
    t_load = time.perf_counter() - t0

    n = X.shape[0]
    print(f"[numpy] loaded N={n} in {t_load:.3f}s "
          f"(W1={W1.shape}, W2={W2.shape})")

    t0 = time.perf_counter()
    z1 = W1 @ X.T + b1[:, None]
    a1 = sigmoid(z1)
    z2 = W2 @ a1 + b2[:, None]
    a2 = sigmoid(z2)
    preds = a2.argmax(axis=0)
    t_infer = time.perf_counter() - t0

    acc = float((preds == y).mean())
    print(f"[numpy] inference: {n} predictions in {t_infer:.3f}s "
          f"({1000 * t_infer / n:.3f} ms/img)")
    print(f"[numpy] accuracy: {acc:.4f}")


if __name__ == "__main__":
    main()
