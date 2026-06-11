#pragma once
// The Phase 3 PoC binding, restated as a Phase 4 declaration: this is
// the thin TU shape a user writes for their own class (design §10.1).
// Compiling it instantiates the glue via wrap.h; the static initializer
// below registers `__Foreign.Counter` before the stdlib setup walks the
// registry. tests/test_foreign.cul (and difftest Dim25) are the spec —
// they pass unchanged against this declaration, which is what proves
// the generator emits the hand-written Phase 3 shape.

#include <foreign_fixture.h>
#include <wrap.h>

namespace culebra {

inline const bool _foreign_counter_wrapped = [] {
  using foreign_fixture::Counter;
  wrap<Counter>("__Foreign", "Counter")
      .ctor<long>({"start"})
      .method("value", &Counter::value)
      .method("add", &Counter::add, {"n"})
      .method("label", &Counter::label)
      .method("clone", &Counter::clone)
      .method("fork", &Counter::fork)
      .method("share", &Counter::share)
      .static_method("live", &Counter::live);
  return true;
}();

}  // namespace culebra
