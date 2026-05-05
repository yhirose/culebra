#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace culebra {

// Object property layout description, V8/SpiderMonkey "hidden class"
// style. Each Shape is interned in the process-wide ShapeRegistry;
// JitObjects with the same property set share the same Shape* and a
// fixed-offset slots array, replacing the per-instance hash/vector
// of (name, value) pairs. This is what brings "Python __slots__"
// semantics to plain Culebra Objects (and class instances, since
// `class` desugars to Object).
//
// Shapes are immutable. Adding a property to an Object transitions
// it to a new Shape via `transition_add`; the source Shape caches
// the transition so identical (parent, name) pairs always resolve
// to the same target Shape. Property reads use the offset map to
// translate name -> slot index.
struct Shape {
  std::vector<std::string> names;          // insertion order
  std::map<std::string_view, size_t> offsets;
  Shape* parent = nullptr;                  // not used yet; reserved for proto chains
  std::map<std::string_view, Shape*> add_transitions;

  bool has(std::string_view name) const {
    return offsets.contains(name);
  }
  // Position of `name` in `slots`, or static_cast<size_t>(-1) if absent.
  size_t offset(std::string_view name) const {
    auto it = offsets.find(name);
    return it != offsets.end() ? it->second : static_cast<size_t>(-1);
  }
};

// Process-wide intern table. Shapes live for the full program;
// the set is bounded by the number of distinct property-name sets
// the program ever uses (typically tiny — every class instance
// shares one shape, every {a, b, c} object literal shares one).
struct ShapeRegistry {
  static ShapeRegistry& instance() {
    static ShapeRegistry r;
    return r;
  }
  Shape* root() { return root_.get(); }

  // Return the Shape obtained by adding `name` to `current`'s
  // property set. Cached on `current->add_transitions` so identical
  // transitions collide on the same Shape pointer.
  Shape* transition_add(Shape* current, std::string_view name) {
    auto it = current->add_transitions.find(name);
    if (it != current->add_transitions.end()) return it->second;
    auto next = std::make_unique<Shape>();
    next->names = current->names;
    next->offsets = current->offsets;
    next->names.push_back(std::string(name));
    auto& stored_name = next->names.back();
    next->offsets[std::string_view(stored_name)] =
        next->names.size() - 1;
    auto* raw = next.get();
    owned_.push_back(std::move(next));
    // Key the cache by the new shape's stored name view so the
    // string_view stays live for the lifetime of the Shape.
    current->add_transitions[std::string_view(stored_name)] = raw;
    return raw;
  }

 private:
  ShapeRegistry() : root_(std::make_unique<Shape>()) {}
  std::unique_ptr<Shape> root_;
  std::vector<std::unique_ptr<Shape>> owned_;  // keeps non-root shapes alive
};

inline ShapeRegistry& shape_registry() { return ShapeRegistry::instance(); }

}  // namespace culebra
