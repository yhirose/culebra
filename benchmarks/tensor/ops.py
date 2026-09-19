#!/usr/bin/env python3
"""Single tensor ops in PyTorch, the reference for ops.cul (same ops, shapes,
timing loop). Attention takes [1, H, T, D]: given [H, T, D],
scaled_dot_product_attention skips its fused kernels for the math path,
two to three times slower, which would compare against a path no
PyTorch model runs.

Usage: python3 benchmarks/tensor/ops.py [cpu|cuda]
"""

import sys
import time

import torch
import torch.nn.functional as F

DEVICE = torch.device(sys.argv[1] if len(sys.argv) > 1 else "cuda")
ITERS = 20
RUNS = 5


def sync():
    if DEVICE.type == "cuda":
        torch.cuda.synchronize()
    elif DEVICE.type == "mps":
        torch.mps.synchronize()


def time_op(name, body):
    for _ in range(3):
        body()
    sync()
    best = float("inf")
    for _ in range(RUNS):
        t0 = time.perf_counter()
        for _ in range(ITERS):
            body()
            sync()
        best = min(best, 1e6 * (time.perf_counter() - t0) / ITERS)
    print(f"BENCH label=torch_op device={DEVICE.type} op={name} us={best:.2f}")


def ids(count, n):
    return torch.tensor([(i * 7919) % n for i in range(count)], device=DEVICE)


def randn(*shape):
    return torch.randn(*shape, device=DEVICE)


def main():
    with torch.no_grad():
        a, b, bias = randn(256, 1024), randn(1024, 4096), torch.zeros(4096, device=DEVICE)
        time_op("mm_256x1024x4096", lambda: torch.mm(a, b))
        time_op("addmm_256x1024x4096", lambda: torch.addmm(bias, a, b))

        sq = randn(1024, 1024)
        time_op("mm_1024x1024x1024", lambda: torch.mm(sq, sq))

        q, k, v = randn(1, 8, 1024, 64), randn(1, 8, 1024, 64), randn(1, 8, 1024, 64)
        time_op("causal_attention_H8_T1024_D64",
                lambda: F.scaled_dot_product_attention(q, k, v, is_causal=True))

        x, y, z = randn(4096, 1024), randn(4096, 1024), randn(4096, 1024)
        gamma, beta = torch.ones(1024, device=DEVICE), torch.zeros(1024, device=DEVICE)
        time_op("layer_norm_4096x1024", lambda: F.layer_norm(x, (1024,), gamma, beta, 1e-5))
        time_op("elementwise_relu_xy_plus_z_4096x1024", lambda: torch.relu(x * y + z))
        time_op("sum_axis1_4096x1024", lambda: x.sum(1))

        scores = randn(1024, 1024)
        time_op("softmax_1024x1024", lambda: torch.softmax(scores, -1))

        table, rows = randn(16384, 768), ids(4096, 16384)
        time_op("index_select_4096_of_16384x768", lambda: F.embedding(rows, table))

        logits, targets = randn(4096, 16384), ids(4096, 16384)
        time_op("cross_entropy_4096x16384", lambda: F.cross_entropy(logits, targets))


if __name__ == "__main__":
    main()
