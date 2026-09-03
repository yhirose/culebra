// Tensor device-default smoke: culebra evaluates on `auto` unless the
// program pins a device, and pinning one before the first tensor exists
// has to survive the bootstrap that installs the default.
//
// The install is a function-local static, so it fires once per process —
// each ordering therefore needs its own ctest entry (see the two
// tensor_device_* tests in CMakeLists.txt), which is why run() takes the
// case to exercise rather than checking both here.

#include <iostream>

#include <culebra.h>
#include <stdlib/tensor.h>

namespace tensor_device_smoke_ns {

namespace {

bool check(bool cond, const char* what) {
  if (!cond) std::cerr << "FAIL: " << what << "\n";
  return cond;
}

}  // namespace

int run(bool pin_cpu) {
  bool ok = true;
  ok &= check(tl::device_ == tl::device_type::cpu,
              "tl starts on cpu before culebra applies its own default");

  if (pin_cpu) {
    culebra::tensor_use_cpu();
    culebra::tensor_rt_bootstrap();
    ok &= check(tl::device_ == tl::device_type::cpu,
                "use_cpu() before the first tensor survives the bootstrap");
  } else {
    culebra::tensor_rt_bootstrap();
    ok &= check(tl::device_ == tl::device_type::auto_,
                "an untouched program bootstraps to auto");
    // A later pin still wins, the ordinary case.
    culebra::tensor_use_cpu();
    ok &= check(tl::device_ == tl::device_type::cpu,
                "use_cpu() after the bootstrap pins the cpu");
  }

  if (ok) std::cout << "tensor_device smoke OK\n";
  return ok ? 0 : 1;
}

}  // namespace tensor_device_smoke_ns
