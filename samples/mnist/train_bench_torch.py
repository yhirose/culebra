#!/usr/bin/env python3
"""PyTorch training benchmark. DEVICE env var selects cpu | mps.

Hand-coded backprop, identical algorithm to train_bench_numpy.py.
"""

import os
import time
from pathlib import Path
import numpy as np
import torch

DIR = Path(__file__).parent
DEVICE = torch.device(os.environ.get("DEVICE", "cpu"))
BATCH = 10
ETA = 3.0
N_OUT = 10
CYCLES = 2


def load_float(path):
    return torch.from_numpy(
        np.loadtxt(path, delimiter=",", dtype=np.float32)
    ).to(DEVICE)


def load_int(path):
    return torch.from_numpy(
        np.loadtxt(path, delimiter=",", dtype=np.int64)
    ).to(DEVICE)


def sync():
    if DEVICE.type == "mps":
        torch.mps.synchronize()


def run_epoch(W1, b1, W2, b2, X, Y, n):
    eta_over_batch = ETA / BATCH
    for s in range(0, n, BATCH):
        x = X[s:s + BATCH].T
        t = Y[s:s + BATCH].T

        z1 = W1 @ x + b1
        a1 = torch.sigmoid(z1)
        z2 = W2 @ a1 + b2
        a2 = torch.sigmoid(z2)

        d2 = (a2 - t) * a2 * (1.0 - a2)
        d1 = (W2.T @ d2) * a1 * (1.0 - a1)

        W2 -= eta_over_batch * (d2 @ a1.T)
        b2 -= ETA * d2.mean(dim=1, keepdim=True)
        W1 -= eta_over_batch * (d1 @ x.T)
        b1 -= ETA * d1.mean(dim=1, keepdim=True)


def main():
    t0 = time.perf_counter()
    init_W1 = load_float(DIR / "init_W1.csv")
    init_b1 = load_float(DIR / "init_b1.csv").unsqueeze(1)
    init_W2 = load_float(DIR / "init_W2.csv")
    init_b2 = load_float(DIR / "init_b2.csv").unsqueeze(1)
    X = load_float(DIR / "train_images.csv")
    y = load_int(DIR / "train_labels.csv")
    Xt = load_float(DIR / "test_images.csv")
    yt = load_int(DIR / "test_labels.csv")
    sync()
    t_load = time.perf_counter() - t0

    n = X.shape[0]
    Y = torch.zeros(n, N_OUT, device=DEVICE)
    Y[torch.arange(n, device=DEVICE), y] = 1.0

    print(f"[torch-{DEVICE.type}-train] loaded train={n} "
          f"test={Xt.shape[0]} in {t_load:.3f}s")

    times = []
    W1 = b1 = W2 = b2 = None
    for _ in range(CYCLES):
        W1 = init_W1.clone()
        b1 = init_b1.clone()
        W2 = init_W2.clone()
        b2 = init_b2.clone()
        sync()
        t0 = time.perf_counter()
        run_epoch(W1, b1, W2, b2, X, Y, n)
        sync()
        times.append(time.perf_counter() - t0)

    z1 = W1 @ Xt.T + b1
    a1 = torch.sigmoid(z1)
    z2 = W2 @ a1 + b2
    a2 = torch.sigmoid(z2)
    preds = a2.argmax(dim=0)
    acc = (preds == yt).float().mean().item()

    cold = times[0]
    warm = sum(times[1:]) / (CYCLES - 1) if CYCLES > 1 else float("nan")
    print(f"[torch-{DEVICE.type}-train] cold={cold:.3f}s warm={warm:.3f}s "
          f"accuracy={acc:.4f}")
    print(f"BENCH label=torch_{DEVICE.type}_train "
          f"load={t_load:.4f} cold={cold:.4f} warm={warm:.4f} "
          f"accuracy={acc:.4f}")


if __name__ == "__main__":
    main()
