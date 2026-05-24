#!/usr/bin/env python3
"""Dump training subset and deterministic initial weights for the
cross-language training benchmark.

Outputs (in benchmarks/mnist/):
  train_images.csv  shape (N, 784)   pixels in [0, 1]
  train_labels.csv  shape (N,)       integer labels
  init_W1.csv       shape (30, 784)  initial hidden weights
  init_b1.csv       shape (30,)      initial hidden bias (zeros)
  init_W2.csv       shape (10, 30)   initial output weights
  init_b2.csv       shape (10,)      initial output bias (zeros)

Usage: prep_train.py [N]   (default N=1000)
"""

import gzip
import struct
import sys
from pathlib import Path
import numpy as np

DATA = Path(__file__).parent / "data"
OUT = Path(__file__).parent
DEFAULT_N = 1000
SEED = 42
N_IN, N_HID, N_OUT = 784, 30, 10


def read_idx_images(path: Path) -> np.ndarray:
    with gzip.open(path, "rb") as f:
        magic, n, rows, cols = struct.unpack(">IIII", f.read(16))
        assert magic == 2051, f"bad image magic in {path}"
        buf = f.read(n * rows * cols)
    arr = np.frombuffer(buf, dtype=np.uint8).reshape(n, rows * cols)
    return arr.astype(np.float64) / 255.0


def read_idx_labels(path: Path) -> np.ndarray:
    with gzip.open(path, "rb") as f:
        magic, n = struct.unpack(">II", f.read(8))
        assert magic == 2049, f"bad label magic in {path}"
        buf = f.read(n)
    return np.frombuffer(buf, dtype=np.uint8).astype(np.int64)


def main() -> None:
    n_keep = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_N

    img_path = DATA / "train-images-idx3-ubyte.gz"
    lbl_path = DATA / "train-labels-idx1-ubyte.gz"
    for p in (img_path, lbl_path):
        if not p.exists():
            sys.stderr.write(f"missing {p}; run `just fetch-mnist` first\n")
            sys.exit(1)

    X = read_idx_images(img_path)[:n_keep]
    y = read_idx_labels(lbl_path)[:n_keep]
    np.savetxt(OUT / "train_images.csv", X, delimiter=",", fmt="%.6f")
    np.savetxt(OUT / "train_labels.csv", y, delimiter=",", fmt="%d")
    print(f"saved {n_keep} train samples")

    rng = np.random.default_rng(SEED)
    W1 = rng.standard_normal((N_HID, N_IN)) / np.sqrt(N_IN)
    b1 = np.zeros(N_HID)
    W2 = rng.standard_normal((N_OUT, N_HID)) / np.sqrt(N_HID)
    b2 = np.zeros(N_OUT)
    np.savetxt(OUT / "init_W1.csv", W1, delimiter=",", fmt="%.8e")
    np.savetxt(OUT / "init_b1.csv", b1, delimiter=",", fmt="%.8e")
    np.savetxt(OUT / "init_W2.csv", W2, delimiter=",", fmt="%.8e")
    np.savetxt(OUT / "init_b2.csv", b2, delimiter=",", fmt="%.8e")
    print(f"saved initial weights (seed={SEED})")


if __name__ == "__main__":
    main()
