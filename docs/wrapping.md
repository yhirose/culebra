# Wrapping your C++ classes

`culebra wrap` builds an **extended culebra binary** with your own C++
classes available as builtins — no fork of the interpreter, no plugin
ABI. You write a short declaration TU; the C++ compiler instantiates
the glue (pybind11-style), and the result works identically under the
interpreter, `--jit`, and AOT binaries produced by the extended
`culebra build`.

## 1. Declare

Given a header-only class of yours:

```cpp
// vec2.hpp — plain C++, knows nothing about culebra
namespace demo {
class Vec2 {
 public:
  Vec2(double x, double y);
  double len() const;
  void scale(double k);
  std::string show() const;
  Vec2 unit() const;          // by value
  static long dims();
};
}
```

write one declaration TU:

```cpp
// vec2_binding.cpp
#include <wrap.h>
#include "vec2.hpp"

namespace {
const bool registered = [] {
  culebra::wrap<demo::Vec2>("Geo", "Vec2")
      .ctor<double, double>({"x", "y"})
      .method<&demo::Vec2::len>("len")
      .method<&demo::Vec2::scale>("scale", {"k"})
      .method<&demo::Vec2::show>("show")
      .method<&demo::Vec2::unit>("unit")
      .static_method<&demo::Vec2::dims>("dims");
  return true;
}();
}
```

The member function is a *template* argument (`method<&T::m>`), so each
method gets its own compiled thunk. Parameter names are optional; they
drive error messages and keyword binding on the interpreter.

## 2. Build

```sh
culebra wrap vec2_binding.cpp -o ext-culebra
# linking against a prebuilt library:
culebra wrap mylib_binding.cpp --link "-L/opt/mylib/lib -lmylib" -o ext-culebra
```

`culebra wrap` rebuilds the culebra source tree with your TUs compiled
in (the checkout it was built from, or `$CULEBRA_HOME`), caching under
`~/.cache/culebra-wrap/`. With ccache configured the rebuild is
effectively compile-your-declaration + relink. `--lto` produces an
optimized binary (slower build).

## 3. Use — on every backend

```culebra
# doctest: skip
let v = Geo.Vec2.new(3.0, 4.0)
puts(v.len())          # => 5
v.scale(2.0)
let u = v.unit()       # by-value return -> a NEW owned instance
v.drop()               # ~Vec2 runs NOW (deterministic)
v.len()                # !! ClosedError
```

Wrapped instances are resources with the full lifetime model: scope-exit
deterministic drop (cycles included), an idempotent explicit `drop()`,
and `ClosedError` on use-after-drop. `ext-culebra build script.cul`
produces standalone AOT binaries that carry the binding.

The binding and the wrapped library (`--link`) are pulled into an AOT
binary only when the script names a wrapped namespace — a program built by
`ext-culebra` that uses none of the wrapped classes links none of the
wrapped library, same as a stock `culebra build`. The check is a
conservative identifier match (`Geo` etc.), so it can over-link but never
under-link. This mirrors how `culebra build` links OpenSSL only for
programs that use `Http`.

## Marshalling

| C++ | Culebra |
|---|---|
| `long` / `int` | `Long` |
| `double` / `float` | `Float` |
| `bool` | `Bool` |
| `std::string` / `std::string_view` / `const char*` | `String` |
| `T` by value, `std::unique_ptr<T>` | owned instance of wrapped `T` |
| `std::shared_ptr<T>` | instance holding one share |

A method returning `T&` / `const T&` of a wrapped class is a
**compile-time error** under `.method` — references are not an
ownership shape. Declare it `.borrowed_method` instead:

```cpp
.borrowed_method<&Box::inner>("inner")     // Counter& inner()
```

The result is a **borrowing handle**: it does not own (dropping it is a
no-op), it keeps its parent alive while it exists, and every access is
validated — the parent explicitly dropped, or mutated by a non-const
method since the borrow was taken (each wrapped instance carries a
generation counter, bumped automatically by non-const dispatch), raises
`ClosedError` instead of touching freed or reallocated memory:

```culebra
let b = __Foreign.Box.new(3)
let c = b.inner()
b.reset(9)             # non-const -> generation bump
c.value()              # !! ClosedError
```

A non-const method that provably never invalidates borrows can opt out:
`.method<&T::touch>("touch", {}, culebra::wrap_policy::preserves_borrows)`.
Misdeclaring const-ness or this flag is an author-contract violation
(the sol2/pybind11 deal) — the failure mode is a spurious or missed
stale error, never memory-unsafety on the culebra side.

Containers (`std::vector`/`std::map`) and callbacks are not yet
marshalled.

See `examples/wrap/` for the complete working demo this page is based
on, and `tests/wrap_test.sh` for the end-to-end pipeline check.
