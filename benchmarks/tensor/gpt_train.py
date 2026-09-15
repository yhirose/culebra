#!/usr/bin/env python3
"""One GPT training step in PyTorch, the reference for gpt_train.cul.

Written the way a PyTorch user writes it (nn.Linear, nn.LayerNorm,
F.scaled_dot_product_attention on [B, H, T, D], F.cross_entropy,
torch.optim.Adam with its default implementation), not op-for-op against
the Culebra port: the comparison is against what PyTorch programs run.
Model, init (normal 0.02, zero biases), data and hyperparameters match
gpt_train.cul.

Usage: python3 benchmarks/tensor/gpt_train.py [cpu|cuda|mps] [small|medium|large]
"""

import sys
import time

import torch
import torch.nn as nn
import torch.nn.functional as F

DEVICE = torch.device(sys.argv[1] if len(sys.argv) > 1 else "cpu")
CONFIG = sys.argv[2] if len(sys.argv) > 2 else "small"

# vocab, width, heads, layers, seq, batch, warmup steps, timed steps —
# identical to gpt_train.cul
CONFIGS = {
    "small": (4096, 256, 4, 4, 256, 8, 3, 10),
    "medium": (8192, 512, 8, 6, 512, 8, 3, 10),
    "large": (16384, 768, 12, 8, 1024, 4, 3, 5),
}
V, C, H, L, T, B, WARMUP, STEPS = CONFIGS[CONFIG]
N = B * T
LR, BETA1, BETA2, EPS = 3.0e-4, 0.9, 0.95, 1.0e-8
LN_EPS = 1.0e-5  # Culebra's layer_norm epsilon is fixed at 1e-5


def sync():
    if DEVICE.type == "cuda":
        torch.cuda.synchronize()
    elif DEVICE.type == "mps":
        torch.mps.synchronize()


class Block(nn.Module):
    def __init__(self):
        super().__init__()
        self.ln1 = nn.LayerNorm(C, eps=LN_EPS)
        self.q = nn.Linear(C, C)
        self.k = nn.Linear(C, C)
        self.v = nn.Linear(C, C)
        self.o = nn.Linear(C, C)
        self.ln2 = nn.LayerNorm(C, eps=LN_EPS)
        self.fc1 = nn.Linear(C, 4 * C)
        self.fc2 = nn.Linear(4 * C, C)

    @staticmethod
    def heads(t):
        return t.view(B, T, H, C // H).transpose(1, 2)  # [B, H, T, D]

    def forward(self, x):
        h = self.ln1(x)
        q, k, v = self.heads(self.q(h)), self.heads(self.k(h)), self.heads(self.v(h))
        ctx = F.scaled_dot_product_attention(q, k, v, is_causal=True)
        x = x + self.o(ctx.transpose(1, 2).reshape(B, T, C))
        return x + self.fc2(F.relu(self.fc1(self.ln2(x))))


class GPT(nn.Module):
    def __init__(self):
        super().__init__()
        self.wte = nn.Embedding(V, C)
        self.wpe = nn.Embedding(T, C)
        self.blocks = nn.ModuleList(Block() for _ in range(L))
        self.lnf = nn.LayerNorm(C, eps=LN_EPS)
        for mod in self.modules():
            if isinstance(mod, (nn.Linear, nn.Embedding)):
                nn.init.normal_(mod.weight, std=0.02)
            if isinstance(mod, nn.Linear):
                nn.init.zeros_(mod.bias)

    def forward(self, idx, targets):
        pos = torch.arange(T, device=idx.device)
        x = self.wte(idx) + self.wpe(pos)
        for block in self.blocks:
            x = block(x)
        logits = F.linear(self.lnf(x), self.wte.weight)  # tied head
        return F.cross_entropy(logits.view(N, V), targets.view(N))


def id_tensor(mul, add, n):
    ids = [(i * mul + add) % n for i in range(N)]
    return torch.tensor(ids, device=DEVICE).view(B, T)


def main():
    torch.manual_seed(42)
    model = GPT().to(DEVICE)
    opt = torch.optim.Adam(model.parameters(), lr=LR, betas=(BETA1, BETA2), eps=EPS)
    # One fixed batch, the same ids gpt_train.cul builds.
    tokens = id_tensor(7919, 13, V)
    targets = id_tensor(104729, 7, V)

    def train_step():
        opt.zero_grad(set_to_none=True)
        loss = model(tokens, targets)
        loss.backward()
        opt.step()
        return loss

    loss0 = train_step().item()
    for _ in range(WARMUP - 1):
        train_step()
    sync()

    t0 = time.perf_counter()
    for _ in range(STEPS):
        last = train_step()
    sync()
    per = 1000.0 * (time.perf_counter() - t0) / STEPS

    print(f"BENCH label=torch_gpt_train device={DEVICE.type} config={CONFIG} "
          f"tokens={N} ms_per_step={per:.3f} loss0={loss0:.4f} loss={last.item():.4f}")


if __name__ == "__main__":
    main()
