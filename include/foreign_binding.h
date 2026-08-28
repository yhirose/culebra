#pragma once
// The Phase 3 PoC binding, restated as a Phase 4 declaration: this is
// the thin TU shape a user writes for their own class.
// Compiling it instantiates the glue via wrap.h; the registrar below runs
// before the stdlib setup walks the registry — from a variable each including
// TU declares for itself, which is where a registration has to be rooted (see
// wrap.h). tests/test_foreign.cul (and difftest Dim25) are the spec —
// they pass unchanged against this declaration, which is what proves
// the generator emits the hand-written Phase 3 shape.

#include <foreign_fixture.h>
#include <wrap.h>

namespace culebra {

// Both classes in one registrar: the caller is a TU-level variable, and one
// per header is one thing for an includer to get right.
//
// Phase 5: borrowing. `inner` / `read_inner` return references INTO the Box —
// borrowing handles checked against the parent's closed flag and generation.
// `reset` is non-const (generation bump: existing borrows go stale); `touch`
// is non-const but declared harmless.
inline bool register_foreign_fixture() {
  using foreign_fixture::Box;
  using foreign_fixture::Counter;
  wrap<Counter>("__Foreign", "Counter")
      .ctor<long>({"start"})
      .method<&Counter::value>("value")
      .method<&Counter::add>("add", {"n"})
      .method<&Counter::divide>("divide", {"n"})
      .method<&Counter::label>("label")
      .method<&Counter::clone>("clone")
      .method<&Counter::fork>("fork")
      .method<&Counter::share>("share")
      .static_method<&Counter::live>("live");
  wrap<Box>("__Foreign", "Box")
      .ctor<long>({"start"})
      .method<&Box::peek>("peek")
      .method<&Box::reset>("reset", {"v"})
      .method<&Box::touch>("touch", {}, wrap_policy::preserves_borrows)
      .borrowed_method<&Box::inner>("inner")
      .borrowed_method<&Box::read_inner>("read_inner")
      .borrowed_method<&Box::slot>("slot", {"i"});
  return true;
}

}  // namespace culebra
