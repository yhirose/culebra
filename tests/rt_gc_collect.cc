// Phase 0 standalone validation of the conservative mark-sweep
// (include/rt/gc.h). Two properties, tested at the right strength:
//
//   * mark+sweep is exact when roots are precise  -> collect_precise()
//     asserts the unreachable set is reclaimed deterministically.
//   * conservative collect() is SOUND but not complete -> it must never
//     free a reachable object, but MAY over-retain an unreachable one
//     (a stale stack/register word that looks like a pointer). So we only
//     assert "reachable survives" for collect(), never an exact count.
//
//   clang++ -std=c++20 -O2 -Iinclude tests/rt_gc_collect.cc -o /tmp/gcc && /tmp/gcc

#include "rt/gc.h"

#include <cassert>
#include <cstdio>
#include <vector>

using culebra::gc::GcHeader;
using culebra::gc::Heap;

struct Node {
  GcHeader h;
  void* kids[2];
};
static constexpr uint8_t kNodeTag = 1;

static void node_children(void* obj, uint8_t /*tag*/, std::vector<void*>& out) {
  auto* n = static_cast<Node*>(obj);
  if (n->kids[0]) out.push_back(n->kids[0]);
  if (n->kids[1]) out.push_back(n->kids[1]);
}

static Node* new_node(Heap& h, void* k0 = nullptr, void* k1 = nullptr) {
  auto* n = static_cast<Node*>(h.alloc(sizeof(Node), kNodeTag));
  n->kids[0] = k0;
  n->kids[1] = k1;
  return n;
}

int main() {
  Heap heap;
  heap.set_children_fn(&node_children);

  // Reachable from `root`: R -> {A, D}, A -> B.  Orphans: X -> Y.
  Node* B = new_node(heap);
  Node* A = new_node(heap, B);
  Node* D = new_node(heap);
  Node* root = new_node(heap, A, D);
  Node* Y = new_node(heap);
  Node* X = new_node(heap, Y);
  void* Xp = X;
  void* Yp = Y;
  assert(heap.live_count() == 6);

  // --- deterministic mark+sweep from explicit roots = {root} ---
  size_t freed = heap.collect_precise({root});
  assert(freed == 2);  // X, Y (unreachable from root)
  assert(heap.live_count() == 4);
  assert(heap.is_object(root) && heap.is_object(A) && heap.is_object(B) &&
         heap.is_object(D));
  assert(!heap.is_object(Xp) && !heap.is_object(Yp));

  // idempotent: nothing else to free
  assert(heap.collect_precise({root}) == 0);

  // sever A -> B; B becomes unreachable and is reclaimed
  void* Bp = B;
  A->kids[0] = nullptr;
  assert(heap.collect_precise({root}) == 1);
  assert(!heap.is_object(Bp));
  assert(heap.live_count() == 3);  // root, A, D

  // --- conservative collect(): SOUNDNESS only (reachable must survive) ---
  Node* P = new_node(heap);
  Node* Q = new_node(heap, P);
  Node* root2 = new_node(heap, Q);
  heap.collect();  // may over-retain, must not free anything reachable
  assert(heap.is_object(root) && heap.is_object(A) && heap.is_object(D));
  assert(heap.is_object(root2) && heap.is_object(Q) && heap.is_object(P));
  assert(heap.live_count() >= 6);  // 6 reachable objects all survive

  // keep roots live to end so they're genuine roots during collect()
  asm volatile("" : : "r"(root), "r"(A), "r"(D), "r"(root2), "r"(Q), "r"(P)
               : "memory");
  std::printf("jit_gc collect: OK  (precise frees exact; conservative sound)\n");
  return 0;
}
