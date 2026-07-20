// Driver for landingpad_phi.ll. `probe` invokes may_throw, which throws,
// so the landing pad runs with every phi taking its %b1 incoming value —
// the arguments passed in here. Each should come back unchanged.
#include <cstdint>
#include <cstdio>

extern "C" void may_throw() { throw 1; }

static int64_t seen[64];
static int nseen = 0;
extern "C" void sink(int64_t v) { seen[nseen++] = v; }

extern "C" void probe(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                      int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

int main() {
  probe(100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, /*n=*/0);
  int bad = 0;
  for (int i = 0; i < nseen; i++) {
    if (seen[i] != 100 + i) {
      std::printf("arg %d: expected %d, got %lld\n", i, 100 + i,
                  (long long)seen[i]);
      bad = 1;
    }
  }
  std::printf(bad ? "MISCOMPILE\n" : "ok\n");
  return bad;
}
