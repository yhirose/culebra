// The thin declaration TU a user writes to wrap their own class
// (design §10.1 / §14.3 Phase 4). Build an extended culebra with:
//
//   culebra wrap examples/wrap/vec2_binding.cpp -o ext-culebra
//
// then `Geo.Vec2` exists on every backend of the extended binary
// (interp / --jit / `ext-culebra build` AOT outputs):
//
//   let v = Geo.Vec2.new(3.0, 4.0)
//   v.len()       # => 5.0
//   v.drop()      # ~Vec2 runs NOW; further methods raise ClosedError
//
// The anonymous namespace keeps the registrar local to this TU; with
// several binding TUs each just needs its own registrar variable.

#include <wrap.h>

#include "vec2.hpp"

namespace {

const bool registered = [] {
  culebra::wrap<demo::Vec2>("Geo", "Vec2")
      .ctor<double, double>({"x", "y"})
      .method<&demo::Vec2::x>("x")
      .method<&demo::Vec2::y>("y")
      .method<&demo::Vec2::len>("len")
      .method<&demo::Vec2::scale>("scale", {"k"})
      .method<&demo::Vec2::show>("show")
      .method<&demo::Vec2::unit>("unit")
      .static_method<&demo::Vec2::dims>("dims");
  return true;
}();

}  // namespace
