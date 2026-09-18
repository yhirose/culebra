#pragma once

// `culebra embed-flags` — the compiler flags a C++ host needs to embed this
// engine (docs/deployment.md §2). The list is nine include entries plus the
// platform's link line, and every one of them is load-bearing, so a host that
// types them by hand gets them wrong once and then debugs the header the
// stdlib reaches unconditionally. Asking the binary that will run the scripts
// is also what keeps the flags and the engine the same age.
//
// Header-only embedding builds against a culebra checkout, so this reports
// the same tree `culebra wrap` and `Embed.dir` use (base/source_dir.h) and
// fails with the same "set CULEBRA_HOME" when there is none.

namespace culebra {

// Returns the process exit code: 0 printed, 1 no checkout, 2 bad usage.
int run_embed_flags(int argc, const char** argv);

}  // namespace culebra
