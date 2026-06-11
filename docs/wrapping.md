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
**compile-time error** — references have no ownership shape yet
(borrowing is a later phase). Containers (`std::vector`/`std::map`) and
callbacks are also not yet marshalled.

See `examples/wrap/` for the complete working demo this page is based
on, and `tests/wrap_test.sh` for the end-to-end pipeline check.
