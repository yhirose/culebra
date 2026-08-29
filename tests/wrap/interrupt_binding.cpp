// A second binding TU for wrap_test.sh, alongside examples/wrap's Vec2: it
// pins what the wrap layer does with the two kinds of exception a wrapped C++
// body can raise.
//
// `rethrow_as_culebra` turns a std::exception into a RuntimeError the script
// can catch, and lets a CulebraError past untouched. An interrupt is neither —
// culebra::Interrupted derives from nothing — so it matches no handler there
// and unwinds on through, which is what a cooperative stop must do. That was
// read off the source rather than run; this runs it.
//
// Registered under `Probe` so a program that names neither namespace still
// links no wrap archive at all (wrap_test asserts that separately).

#include <wrap.h>

#include <stdexcept>

namespace wrap_probe {

class Blocking {
 public:
  // A wrapped body that reaches culebra's cooperative stop from inside its own
  // C++ — a callback into script that hits a safepoint, or a blocking
  // primitive that polls between waits (IO.stdin's read loop is the in-tree
  // one). Both end at throw_if_interrupted(), so this raises it the same way;
  // arming the flag here keeps the test free of signals and sleeps, as
  // tests/embedding/signal_smoke.cc does.
  long wait() const {
    culebra::request_interrupt();
    culebra::throw_if_interrupted();
    return 0;  // not reached
  }

  // The contrast: an ordinary native failure still has to arrive as a
  // catchable RuntimeError, or the conversion above has been lost.
  long boom() const { throw std::runtime_error("native failure"); }
};

}  // namespace wrap_probe

namespace {

const bool registered = [] {
  culebra::wrap<wrap_probe::Blocking>("Probe", "Blocking")
      .ctor<>()
      .method<&wrap_probe::Blocking::wait>("wait")
      .method<&wrap_probe::Blocking::boom>("boom");
  return true;
}();

}  // namespace
