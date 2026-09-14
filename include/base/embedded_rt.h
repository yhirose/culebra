#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace culebra {

// The embedded runtime archives, read in their own TU: the generated header
// names each archive, so main.cc including it missed ccache under `culebra wrap`.

// FNV-1a 64-bit over the bytes of every embedded file.
std::uint64_t embedded_rt_hash();

// The packed bytes of one embedded archive, or nullopt with `err` set when
// it is absent or empty.
std::optional<std::string_view> embedded_rt_archive(std::string_view name,
                                                    std::string& err);

}  // namespace culebra
