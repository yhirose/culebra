#!/usr/bin/env python3
"""Extract the first N MNIST test samples and dump as CSV.

Output (in benchmarks/mnist/):
  test_images.csv  shape (N, 784)  pixel values in [0, 1]
  test_labels.csv  shape (N,)      integer labels in [0, 9]

Usage: prep_test.py [N]   (default N=1000)
"""

import gzip
import struct
import sys
from pathlib import Path
import numpy as np

DATA = Path(__file__).parent / "data"
OUT = Path(__file__).parent
DEFAULT_N = 1000


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

    img_path = DATA / "t10k-images-idx3-ubyte.gz"
    lbl_path = DATA / "t10k-labels-idx1-ubyte.gz"
    for p in (img_path, lbl_path):
        if not p.exists():
            sys.stderr.write(f"missing {p}; run `just fetch-mnist` first\n")
            sys.exit(1)

    X = read_idx_images(img_path)[:n_keep]
    y = read_idx_labels(lbl_path)[:n_keep]

    np.savetxt(OUT / "test_images.csv", X, delimiter=",", fmt="%.6f")
    np.savetxt(OUT / "test_labels.csv", y, delimiter=",", fmt="%d")
    print(f"saved {n_keep} test samples ({X.shape[1]} features each)")


if __name__ == "__main__":
    main()
