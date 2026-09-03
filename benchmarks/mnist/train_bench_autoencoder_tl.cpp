// cpp-tensorlib-only autoencoder training bench — the C++ baseline that isolates
// how much of Culebra's per-epoch time is the tensor library's own compute cost
// versus Culebra's language-runtime tax (per-object GC/refcount/dispatch).
//
// It replicates the EXACT tl op graph that train_bench_autoencoder.cul emits per
// step (784-512-256-64-256-512-784, sigmoid, MSE, SGD, batch=100, 600 steps = 1
// epoch over 60000 rows), with NO Culebra in the loop. The forward is
// (W.dot(h)+b).sigmoid() — the same expansion Culebra's `linear_sigmoid` lowers
// to (see include/stdlib/tensor.h) — and the backward mirrors the .cul line-for-line,
// including the per-layer ones-tensor allocation, so the only difference from the
// .cul run is Culebra's object model. CPU backend (pixel values don't affect
// timing, so the data is synthetic).
//
// Build (macOS, from the culebra repo root):
//   clang++ -std=c++23 -O2 -I vendor/cpp-tensorlib/include \
//     benchmarks/mnist/train_bench_autoencoder_tl.cpp \
//     -framework Accelerate -framework Metal -framework Foundation \
//     -o /tmp/ae_tl && /tmp/ae_tl 60000
//
// Measured (Apple Silicon, CPU): ~2.8-3.0 s/epoch, versus ~4.3-5.3 s for the
// .cul run and ~0.9 s for silarray's eager-C++ bench on the same shape. So the
// silarray gap is mostly the library (tl is ~3x silarray here), and Culebra's
// runtime adds ~1.5-1.9x on top of tl — not the whole gap. See README.md.

#include <tensorlib.h>

#include <chrono>
#include <cstdio>
#include <vector>

using tl::array;

int main(int argc, char** argv) {
  const int64_t N = argc > 1 ? std::atoll(argv[1]) : 60000;
  const int64_t BATCH = 100;
  const float LR = 0.01f;
  const int L[7] = {784, 512, 256, 64, 256, 512, 784};
  const int N_LAYERS = 6;

  tl::use_cpu();

  std::vector<float> xdata((size_t)N * 784, 0.5f);
  array X = array::from(std::move(xdata), {N, 784});

  auto make_weights = [&](std::vector<array>& W, std::vector<array>& b) {
    W.clear(); b.clear();
    for (int l = 0; l < N_LAYERS; l++) {
      W.push_back(array::full({L[l + 1], L[l]}, 0.01f).eval());
      b.push_back(array::zeros({L[l + 1], 1}).eval());
    }
  };

  auto run_epoch = [&](std::vector<array>& W, std::vector<array>& b) {
    const float inv_n = 2.0f / (float)(BATCH * 784);
    for (int64_t s = 0; s + BATCH <= N; s += BATCH) {
      array xb = X.slice(s, BATCH).transpose();  // [784, BATCH]

      std::vector<array> outs;
      outs.reserve(N_LAYERS);
      array h = xb;
      for (int l = 0; l < N_LAYERS; l++) {
        array o = (W[l].dot(h) + b[l]).sigmoid();  // == Culebra linear_sigmoid
        outs.push_back(o);
        h = o;
      }

      array dout = (h - xb) * inv_n;

      for (int l = N_LAYERS - 1; l >= 0; l--) {
        array out_l = outs[l];
        array ones = array::ones({L[l + 1], BATCH});  // mirrors the .cul
        array dnet = dout * out_l * (ones - out_l);
        array input_l = (l > 0) ? outs[l - 1] : xb;
        array dW = dnet.dot(input_l.transpose());
        array db = dnet.sum(1, true);  // [n_out, 1]
        if (l > 0) dout = W[l].transpose().dot(dnet);
        W[l] = (W[l] - dW * LR).eval();
        b[l] = (b[l] - db * LR).eval();
      }
    }
  };

  std::vector<array> W, b;
  make_weights(W, b);
  run_epoch(W, b);  // warmup (not timed)

  make_weights(W, b);
  auto t0 = std::chrono::steady_clock::now();
  run_epoch(W, b);
  auto t1 = std::chrono::steady_clock::now();

  double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  int64_t steps = N / BATCH;
  std::printf("tensorlib-only autoencoder — CPU, %lld steps\n", (long long)steps);
  std::printf("  epoch  %.3f s   (%.3f ms/step)\n", ms / 1000.0,
              ms / (double)steps);
  return 0;
}
