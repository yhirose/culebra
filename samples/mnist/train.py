#!/usr/bin/env python3
"""Train a 784-30-10 MLP on MNIST and dump weights as CSV.

Reference: Michael Nielsen, "Neural Networks and Deep Learning",
http://neuralnetworksanddeeplearning.com/chap1.html — defaults match
network.py (sigmoid, MSE loss, mini-batch SGD, 30 epochs, batch=10,
eta=3.0). Implementation here uses vectorized numpy mini-batches for
speed; the algorithm is identical.

Output (in samples/mnist/):
  W1.csv  shape (30, 784)   hidden weights
  b1.csv  shape (30,)        hidden bias
  W2.csv  shape (10, 30)     output weights
  b2.csv  shape (10,)        output bias
"""

import gzip
import struct
import sys
from pathlib import Path
import numpy as np

DATA = Path(__file__).parent / "data"
OUT = Path(__file__).parent

N_IN, N_HID, N_OUT = 784, 30, 10
EPOCHS = 30
BATCH = 10
ETA = 3.0
SEED = 42


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


def sigmoid(z: np.ndarray) -> np.ndarray:
    return 1.0 / (1.0 + np.exp(-z))


def sigmoid_prime(z: np.ndarray) -> np.ndarray:
    s = sigmoid(z)
    return s * (1.0 - s)


def main() -> None:
    rng = np.random.default_rng(SEED)

    train_imgs = DATA / "train-images-idx3-ubyte.gz"
    train_lbls = DATA / "train-labels-idx1-ubyte.gz"
    test_imgs = DATA / "t10k-images-idx3-ubyte.gz"
    test_lbls = DATA / "t10k-labels-idx1-ubyte.gz"
    for p in (train_imgs, train_lbls, test_imgs, test_lbls):
        if not p.exists():
            sys.stderr.write(f"missing {p}; run `just fetch-mnist` first\n")
            sys.exit(1)

    print("loading...")
    X = read_idx_images(train_imgs)
    y = read_idx_labels(train_lbls)
    Xt = read_idx_images(test_imgs)
    yt = read_idx_labels(test_lbls)
    print(f"train: {X.shape} labels {y.shape}")
    print(f"test:  {Xt.shape} labels {yt.shape}")

    # One-hot encoded targets for MSE loss
    Y = np.zeros((X.shape[0], N_OUT))
    Y[np.arange(X.shape[0]), y] = 1.0

    # Nielsen-style init: scale by 1/sqrt(fan_in)
    W1 = rng.standard_normal((N_HID, N_IN)) / np.sqrt(N_IN)
    b1 = np.zeros(N_HID)
    W2 = rng.standard_normal((N_OUT, N_HID)) / np.sqrt(N_HID)
    b2 = np.zeros(N_OUT)

    n = X.shape[0]
    for epoch in range(EPOCHS):
        perm = rng.permutation(n)
        for s in range(0, n, BATCH):
            idx = perm[s:s + BATCH]
            x = X[idx].T            # (N_IN, BATCH)
            t = Y[idx].T            # (N_OUT, BATCH)

            # Forward
            z1 = W1 @ x + b1[:, None]
            a1 = sigmoid(z1)
            z2 = W2 @ a1 + b2[:, None]
            a2 = sigmoid(z2)

            # Backward (MSE loss with sigmoid output)
            d2 = (a2 - t) * sigmoid_prime(z2)
            d1 = (W2.T @ d2) * sigmoid_prime(z1)

            # SGD update (gradients averaged over batch)
            W2 -= ETA * (d2 @ a1.T) / BATCH
            b2 -= ETA * d2.mean(axis=1)
            W1 -= ETA * (d1 @ x.T) / BATCH
            b1 -= ETA * d1.mean(axis=1)

        # Test accuracy at end of each epoch
        z1t = W1 @ Xt.T + b1[:, None]
        a1t = sigmoid(z1t)
        z2t = W2 @ a1t + b2[:, None]
        a2t = sigmoid(z2t)
        preds = a2t.argmax(axis=0)
        acc = (preds == yt).mean()
        print(f"epoch {epoch + 1:2d}/{EPOCHS}: test acc = {acc:.4f}")

    np.savetxt(OUT / "W1.csv", W1, delimiter=",", fmt="%.8e")
    np.savetxt(OUT / "b1.csv", b1, delimiter=",", fmt="%.8e")
    np.savetxt(OUT / "W2.csv", W2, delimiter=",", fmt="%.8e")
    np.savetxt(OUT / "b2.csv", b2, delimiter=",", fmt="%.8e")
    print(f"saved weights to {OUT}/")


if __name__ == "__main__":
    main()
