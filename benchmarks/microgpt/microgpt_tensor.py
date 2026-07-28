"""
Tensor port of microgpt (PyTorch), CPU, single-threaded.

Same transformer as microgpt.py (scalar Value autograd), but built on
torch.Tensor so each forward op is a layer-level BLAS call and autograd
lives in the C++ engine, matching the design of microgpt_tensor.cul
(Culebra's native Tensor primitive) closely enough to compare per-step
wall time apples-to-apples.

Reference implementation for the Culebra port benchmark. Reads from
benchmarks/microgpt/names.txt instead of ./input.txt.

Usage: python3 benchmarks/microgpt/microgpt_tensor.py [num_steps] [n_samples]
"""

import os
import random
import sys
import time

import torch

torch.set_num_threads(1)  # match the single-core methodology in README.md
torch.manual_seed(42)
random.seed(42)

DATA_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'names.txt')
if not os.path.exists(DATA_PATH):
    print(f"error: {DATA_PATH} missing. Run `just fetch-names` first.",
          file=sys.stderr)
    sys.exit(1)
docs = [line.strip() for line in open(DATA_PATH) if line.strip()]
random.shuffle(docs)
print(f"num docs: {len(docs)}")

uchars = sorted(set(''.join(docs)))
BOS = len(uchars)
vocab_size = len(uchars) + 1
print(f"vocab size: {vocab_size}")

# --- Hyperparameters (unchanged from microgpt.py / microgpt_tensor.cul) ---
n_layer = 1
n_embd = 16
block_size = 16
n_head = 4
head_dim = n_embd // n_head
attn_scale = head_dim ** -0.5
std_init = 0.08

def param(out_dim, in_dim):
    return torch.nn.Parameter(torch.randn(out_dim, in_dim) * std_init)

wte = param(vocab_size, n_embd)
wpe = param(block_size, n_embd)
lm_head = param(vocab_size, n_embd)

layers = []
for _ in range(n_layer):
    layers.append({
        'attn_wq': param(n_embd, n_embd),
        'attn_wk': param(n_embd, n_embd),
        'attn_wv': param(n_embd, n_embd),
        'attn_wo': param(n_embd, n_embd),
        'mlp_fc1': param(4 * n_embd, n_embd),
        'mlp_fc2': param(n_embd, 4 * n_embd),
    })

params = [wte, wpe, lm_head]
for layer in layers:
    params.extend(layer.values())
print(f"num params: {len(params)} weight matrices")

def rmsnorm(x):
    ms = (x * x).mean()
    return x * (ms + 1e-5).pow(-0.5)

def head_attention(q_h, k_rows, v_rows, scale):
    kmat = torch.stack(k_rows)              # [n_t, hd]
    vmat = torch.stack(v_rows)              # [n_t, hd]
    logits = kmat @ q_h * scale             # [n_t]
    weights = torch.softmax(logits, dim=0)  # [n_t]
    return vmat.t() @ weights               # [hd]

def gpt_forward(token_id, pos_id, keys, values):
    x = rmsnorm(wte[token_id] + wpe[pos_id])

    for li in range(n_layer):
        x_residual1 = x
        x = rmsnorm(x)
        layer = layers[li]

        q = layer['attn_wq'] @ x
        k = layer['attn_wk'] @ x
        v = layer['attn_wv'] @ x
        keys[li].append(k)
        values[li].append(v)

        head_outs = []
        for h in range(n_head):
            hs = h * head_dim
            q_h = q[hs:hs + head_dim]
            k_rows = [kt[hs:hs + head_dim] for kt in keys[li]]
            v_rows = [vt[hs:hs + head_dim] for vt in values[li]]
            head_outs.append(head_attention(q_h, k_rows, v_rows, attn_scale))
        x_attn = torch.cat(head_outs)
        x = layer['attn_wo'] @ x_attn + x_residual1

        x_residual2 = x
        x = rmsnorm(x)
        h1 = torch.relu(layer['mlp_fc1'] @ x)
        x = layer['mlp_fc2'] @ h1 + x_residual2

    return lm_head @ x

# --- Adam optimizer (same hyperparameters as microgpt.py / microgpt_tensor.cul) ---
lr_base, beta1, beta2, eps_adam = 0.01, 0.85, 0.99, 1e-8
optimizer = torch.optim.Adam(params, lr=lr_base, betas=(beta1, beta2), eps=eps_adam)

# --- Training loop ---
num_steps = int(sys.argv[1]) if len(sys.argv) > 1 else 1000
final_loss = 0.0

train_t0 = time.perf_counter()
for step in range(num_steps):
    doc = docs[step % len(docs)]
    tokens = [BOS] + [uchars.index(ch) for ch in doc] + [BOS]
    n = min(block_size, len(tokens) - 1)

    optimizer.zero_grad()
    keys = [[] for _ in range(n_layer)]
    values = [[] for _ in range(n_layer)]

    losses = []
    for pos_id in range(n):
        logits = gpt_forward(tokens[pos_id], pos_id, keys, values)
        target = tokens[pos_id + 1]
        losses.append(-torch.log_softmax(logits, dim=0)[target])
    loss = torch.stack(losses).sum() / n

    loss.backward()

    for g in optimizer.param_groups:
        g['lr'] = lr_base * (1 - step / num_steps)
    optimizer.step()

    l_val = loss.item()
    if (step + 1) % 5 == 0 or step == 0:
        print(f"step {step+1} / {num_steps} | loss {l_val}")
    final_loss = l_val

train_time = time.perf_counter() - train_t0
print(f"final loss: {final_loss} (train {train_time}s, {1000.0 * train_time / num_steps} ms/step)")

# --- Inference: sample new names ---
n_samples = int(sys.argv[2]) if len(sys.argv) > 2 else 5
temperature = 0.5

if n_samples > 0:
    print('--- inference (new, hallucinated names) ---')
    with torch.no_grad():
        for sample_idx in range(n_samples):
            keys = [[] for _ in range(n_layer)]
            values = [[] for _ in range(n_layer)]
            token_id = BOS
            sample = []
            for pos_id in range(block_size):
                logits = gpt_forward(token_id, pos_id, keys, values)
                probs = torch.softmax(logits / temperature, dim=0)
                token_id = random.choices(range(vocab_size), weights=probs.tolist())[0]
                if token_id == BOS:
                    break
                sample.append(uchars[token_id])
            print(f"sample {sample_idx+1}: {''.join(sample)}")
