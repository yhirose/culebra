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

  // --- a finalizer hands a dead object to the living: that object stays,
  // with what it references, and the rest of the dead are still swept ---
  {
    Heap h2;
    h2.set_children_fn(&node_children);
    static Heap* heap2;
    static Node* live;
    static Node* taken;
    static Node* boxed;
    static void* box;
    static int finalize_calls;
    heap2 = &h2;
    live = new_node(h2);
    Node* kid = new_node(h2);
    taken = new_node(h2, kid);
    boxed = new_node(h2);
    void* gone = new_node(h2);
    finalize_calls = 0;
    h2.set_finalize_fn([](const std::vector<Heap::Counted>& dead) {
      finalize_calls++;
      assert(dead.size() == 4);  // taken, kid, boxed, gone
      live->kids[0] = taken;
      // Stored in an object the finalizer made, which no root reaches: this
      // sweep leaves that object alone, so what it references stays too.
      box = new_node(*heap2, boxed);
      return true;  // user code ran
    });
    assert(h2.collect_precise({live}) == 1);
    assert(finalize_calls == 1);
    assert(h2.is_object(taken) && h2.is_object(kid) && !h2.is_object(gone));
    assert(h2.is_object(box) && h2.is_object(boxed));

    // A pass that ran nothing is believed: the dead are swept as found.
    live->kids[0] = nullptr;
    h2.set_finalize_fn([](const std::vector<Heap::Counted>&) { return false; });
    assert(h2.collect_precise({live}) == 4);  // taken, kid, box, boxed
    assert(h2.live_count() == 1);
  }

  // --- the dead give back what they held on survivors: one release per
  // edge that leaves the dead set, none for an edge inside it ---
  {
    Heap h3;
    h3.set_children_fn(&node_children);
    static std::vector<void*> released;
    static Heap* heap3;
    static void* swept[2];
    heap3 = &h3;
    released.clear();
    Node* survivor = new_node(h3);
    Node* g1 = new_node(h3, survivor, survivor);
    Node* g2 = new_node(h3, g1, survivor);
    swept[0] = g1;
    swept[1] = g2;
    h3.set_release_fn([](void* o, uint8_t tag) {
      assert(tag == kNodeTag);
      // After the sweep: a release may run user code, which must find no
      // half-swept object.
      assert(!heap3->is_object(swept[0]) && !heap3->is_object(swept[1]));
      released.push_back(o);
    });
    assert(h3.collect_precise({survivor}) == 2);
    assert(released == std::vector<void*>(3, survivor));
    assert(h3.is_object(survivor));
  }

  // keep roots live to end so they're genuine roots during collect()
  asm volatile("" : : "r"(root), "r"(A), "r"(D), "r"(root2), "r"(Q), "r"(P)
               : "memory");
  std::printf("jit_gc collect: OK  (precise frees exact; conservative sound)\n");
  return 0;
}
