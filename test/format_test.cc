// Unit test for the runtime's message formatter (include/rt_format.h).
//
// The formatter exists to keep libstdc++'s out of AOT binaries (see the
// header, and tools/checks/check_aot_feature_axes.sh), so the property that
// matters is that it renders exactly what `std::format` renders: the
// messages it builds are compared across the executor, `--jit` and AOT
// lanes, and against the previous release's binary. Every check below is
// therefore differential — the expected string is whatever std::format
// produces for the same call, never a literal written here.
//
// The format strings cover what the runtime headers actually write: `{}`
// for each argument type, and the width / fill / hex specs. A spec outside
// that subset is a compile error, so it cannot be tested here; the negative
// cases live in the header's own documentation.
//
// Built and run by CTest (see CMakeLists.txt).

#include <rt_format.h>

#include <cstdint>
#include <cstdio>
#include <format>
#include <limits>
#include <string>
#include <string_view>

namespace {

int failures = 0;

// `culebra::format(f, args...)` against `std::format(f, args...)`.
#define SAME(f, ...)                                                      \
  do {                                                                    \
    std::string got = culebra::format(f __VA_OPT__(, ) __VA_ARGS__);      \
    std::string want = std::format(f __VA_OPT__(, ) __VA_ARGS__);         \
    if (got != want) {                                                    \
      std::printf("FAIL %s: '%s' != std::format's '%s'\n", f, got.c_str(), \
                  want.c_str());                                          \
      failures++;                                                         \
    }                                                                     \
  } while (0)

void plain_values() {
  std::string s = "abc";
  std::string_view sv = "xyz";
  const char* cs = "cstr";
  SAME("no fields");
  SAME("{}", s);
  SAME("{}", sv);
  SAME("{}", cs);
  SAME("{}", 'c');
  SAME("{}", true);
  SAME("{}", false);
  SAME("a {} b {} c", s, 42);
  SAME("{}{}{}", 1, 2, 3);
  SAME("{{literal}}");
  SAME("{{{}}}", 7);
  SAME("trailing {} ", sv);
  SAME("{}", "");
}

void numbers() {
  SAME("{}", std::numeric_limits<int64_t>::min());
  SAME("{}", std::numeric_limits<uint64_t>::max());
  SAME("{}", static_cast<int8_t>(-128));
  SAME("{}", static_cast<unsigned char>(200));
  SAME("{}", static_cast<short>(-3));
  SAME("{}", 0);
  SAME("{}", static_cast<size_t>(1) << 40);

  // Shortest round-trip, including the shapes a culebra Float can reach.
  SAME("{}", 0.0);
  SAME("{}", -0.0);
  SAME("{}", 0.1 + 0.2);
  SAME("{}", 1e300 * 10);
  SAME("{}", -1e300 * 10);
  SAME("{}", std::numeric_limits<double>::quiet_NaN());
  SAME("{}", 1.0);
  SAME("{}", 1.5f);
  SAME("{}", 3.14159265358979);
  SAME("{}", 1e-7);
  SAME("{}", 1e21);
}

void specs() {
  std::string s = "abc";
  std::string_view sv = "xyz";
  SAME("{:02d}", 7);
  SAME("{:02d}", 123);  // wider than the field: the field gives way
  SAME("{:04d}", 42);
  SAME("{:09d}", 42);
  SAME("{:08x}", 48879);
  SAME("{:04X}", 48879);
  SAME("{:4}", 7);
  SAME("{:4}", 123456);
  SAME("{:<12}", s);
  SAME("{:>4}", 3);
  // Text left-aligns and numbers right-align when the spec says neither.
  SAME("{:4}", sv);
  SAME("{:4}", 'c');
  SAME("{:8}", true);
  SAME("{:6}", 1.5);
  // The shapes the runtime writes out of several fields at once.
  SAME("{}{:02d}:{:02d}", '+', 9, 30);
  SAME("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:09d}{}", 2026, 8, 28, 1, 2,
       3, 456789, "Z");
  SAME("{}{:08x}{:08x}", "name", 1u, 4294967295u);
}

}  // namespace

int main() {
  plain_values();
  numbers();
  specs();
  if (failures) {
    std::printf("format_test: %d mismatch(es)\n", failures);
    return 1;
  }
  std::printf("format_test OK\n");
  return 0;
}
