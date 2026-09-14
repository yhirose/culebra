#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace culebra {

// The runtime archives embedded at build time, read through functions in their
// own TU rather than by including the generated culebra_rt_assets.h in main.cc:
// that header declares one symbol per embedded archive, so a build embedding
// one more (`culebra wrap` adds culebra_rt_wrap) would change main.cc's
// preprocessed input and miss ccache on the driver's largest TU.

// FNV-1a 64-bit over the bytes of every embedded file.
std::uint64_t embedded_rt_hash();

// The packed bytes of one embedded archive, or nullopt with `err` set when
// it is absent or empty.
std::optional<std::string_view> embedded_rt_archive(std::string_view name,
                                                    std::string& err);

}  // namespace culebra
