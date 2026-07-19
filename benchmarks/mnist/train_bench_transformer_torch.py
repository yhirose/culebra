#!/usr/bin/env python3
"""PyTorch single-transformer-block benchmark. DEVICE env var selects cpu | mps.

Ported op-for-op from train_bench_transformer.cul (which itself follows
silarray's bench/composite/bench_transformer.cpp), so Culebra Tensor's
cpu/gpu/auto backends can be compared against PyTorch on the same
matmul-dominated shape.

Block: LayerNorm -> Self-Attention -> Residual
    -> LayerNorm -> FFN(ReLU) -> Residual

Usage: DEVICE=cpu python3.11 benchmarks/mnist/train_bench_transformer_torch.py
       DEVICE=mps python3.11 benchmarks/mnist/train_bench_transformer_torch.py
"""

import math
import os
import time

import torch

DEVICE = torch.device(os.environ.get("DEVICE", "cpu"))
RUNS = int(os.environ.get("RUNS", "6"))  # min over RUNS outer repetitions
EPS = 1e-5

# seq, d_model, d_ff, iters — same configs as train_bench_transformer.cul
CONFIGS = [
    (256, 512, 2048, 20),
    (256, 768, 3072, 15),
    (256, 1024, 4096, 10),
    (512, 1024, 4096, 5),
]


def sync():
    if DEVICE.type == "mps":
        torch.mps.synchronize()
    elif DEVICE.type == "cuda":
        torch.cuda.synchronize()


def layer_norm(x, gamma, beta):
    mu = x.mean(1, keepdim=True)
    diff = x - mu
    var = (diff * diff).mean(1, keepdim=True)
    normed = diff * (var + EPS).rsqrt()
    return normed * gamma + beta


def main():
    for seq, d, dff, iters in CONFIGS:
        scale = 1.0 / math.sqrt(float(d))

        x = torch.randn(seq, d, device=DEVICE)
        Wq = torch.randn(d, d, device=DEVICE) * scale
        Wk = torch.randn(d, d, device=DEVICE) * scale
        Wv = torch.randn(d, d, device=DEVICE) * scale
        Wo = torch.randn(d, d, device=DEVICE) * scale
        bq = torch.zeros(d, device=DEVICE)
        bk = torch.zeros(d, device=DEVICE)
        bv = torch.zeros(d, device=DEVICE)
        bo = torch.zeros(d, device=DEVICE)
        W1 = torch.randn(d, dff, device=DEVICE) * (1.0 / math.sqrt(float(d)))
        b1 = torch.zeros(dff, device=DEVICE)
        W2 = torch.randn(dff, d, device=DEVICE) * (1.0 / math.sqrt(float(dff)))
        b2 = torch.zeros(d, device=DEVICE)
        gamma1 = torch.ones(d, device=DEVICE)
        beta1 = torch.zeros(d, device=DEVICE)
        gamma2 = torch.ones(d, device=DEVICE)
        beta2 = torch.zeros(d, device=DEVICE)

        def block():
            h = layer_norm(x, gamma1, beta1)
            Q = torch.addmm(bq, h, Wq)
            K = torch.addmm(bk, h, Wk)
            V = torch.addmm(bv, h, Wv)
            scores = torch.mm(Q, K.t()) * scale
            attn = torch.softmax(scores, -1)
            context = torch.mm(attn, V)
            attn_out = torch.addmm(bo, context, Wo)
            r1 = x + attn_out
            h2 = layer_norm(r1, gamma2, beta2)
            ff = torch.relu(torch.addmm(b1, h2, W1))
            out = r1 + torch.addmm(b2, ff, W2)
            return out

        best = float("inf")
        with torch.no_grad():
            # warmup (GPU clock + first-call init), not timed
            for _ in range(3):
                block()
            sync()

            for _ in range(RUNS):
                total = 0.0
                for _ in range(iters):
                    t0 = time.perf_counter()
                    block()
                    sync()
                    total += time.perf_counter() - t0
                best = min(best, 1000.0 * total / iters)  # ms/iter

        print(f"BENCH label=torch_{DEVICE.type}_transformer "
              f"device={DEVICE.type} seq={seq} d={d} d_ff={dff} "
              f"ms_per_iter={best:.4f}")


if __name__ == "__main__":
    main()
