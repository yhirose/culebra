#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace culebra {

// `culebra docs` — read the embedded reference docs. Returns the process exit
// code: 0 found, 1 nothing matched (the grep convention, so a caller can ask
// "does this API exist?" without reading the output), 2 bad usage.
// `version` is passed in so this TU needs nothing from the interpreter: docs
// is a search over embedded text (see tools/checks/check_rt_archive_tls.sh for
// what including culebra.h here would cost the Windows link).
int run_docs(int argc, const char** argv, const char* version);

// The reference entry a name is documented under, for an editor's hover: the
// signature its heading gives (one per line when a heading gives several) and
// the prose beneath it. Found for a heading that names it — `Math.abs`,
// `Math.pi`, `type_of` — and not for a value method (`s.split`), whose table
// depends on the receiver's type.
struct DocEntry {
  std::string signature;
  std::string body;
};
std::optional<DocEntry> find_doc_entry(std::string_view name);

}  // namespace culebra
