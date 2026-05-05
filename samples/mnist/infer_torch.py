#!/usr/bin/env python3
"""PyTorch inference benchmark. DEVICE env var selects cpu | mps."""

import os
import time
from pathlib import Path
import numpy as np
import torch

DIR = Path(__file__).parent
DEVICE = torch.device(os.environ.get("DEVICE", "cpu"))
CYCLES = 3


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


def main():
    t0 = time.perf_counter()
    W1 = load_float(DIR / "W1.csv")
    b1 = load_float(DIR / "b1.csv").unsqueeze(1)
    W2 = load_float(DIR / "W2.csv")
    b2 = load_float(DIR / "b2.csv").unsqueeze(1)
    X = load_float(DIR / "test_images.csv")
    y = load_int(DIR / "test_labels.csv")
    sync()
    t_load = time.perf_counter() - t0

    n = X.shape[0]
    print(f"[torch-{DEVICE.type}] loaded N={n} in {t_load:.3f}s")

    times = []
    preds = None
    for _ in range(CYCLES):
        sync()
        t0 = time.perf_counter()
        z1 = W1 @ X.T + b1
        a1 = torch.sigmoid(z1)
        z2 = W2 @ a1 + b2
        a2 = torch.sigmoid(z2)
        preds = a2.argmax(dim=0)
        sync()
        times.append(time.perf_counter() - t0)

    acc = (preds == y).float().mean().item()
    cold = times[0]
    warm = sum(times[1:]) / (CYCLES - 1) if CYCLES > 1 else float("nan")
    print(f"[torch-{DEVICE.type}] cold={cold:.3f}s warm={warm:.3f}s "
          f"accuracy={acc:.4f}")
    print(f"BENCH label=torch_{DEVICE.type}_infer "
          f"load={t_load:.4f} cold={cold:.4f} warm={warm:.4f} "
          f"accuracy={acc:.4f}")


if __name__ == "__main__":
    main()
