#include "base/embedded_rt.h"

#include <format>

#include "culebra_rt_assets.h"

namespace culebra {

std::uint64_t embedded_rt_hash() {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (const auto& entry : CulebraRT::FS) {
    if (!entry.is_file()) continue;
    auto data = entry.bytes();
    if (!data) continue;
    for (auto b : *data) {
      h ^= b;
      h *= 0x100000001b3ULL;
    }
  }
  return h;
}

std::optional<std::string_view> embedded_rt_archive(std::string_view name,
                                                    std::string& err) {
  auto it = CulebraRT::FS.find(name);
  if (it == CulebraRT::FS.end()) {
    err = std::format("embedded runtime archive '{}' not found", name);
    return std::nullopt;
  }
  auto packed = (*it).text();
  if (!packed) {
    err = std::format("embedded runtime archive '{}' has no data", name);
    return std::nullopt;
  }
  return packed;
}

}  // namespace culebra
