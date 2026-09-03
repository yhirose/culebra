#pragma once

namespace culebra {

// `culebra docs` — read the embedded reference docs. Returns the process exit
// code: 0 found, 1 nothing matched (the grep convention, so a caller can ask
// "does this API exist?" without reading the output), 2 bad usage.
// `version` is passed in so this TU needs nothing from the interpreter: docs
// is a search over embedded text (see rt_shared_tls.h for what including
// culebra.h here would cost the Windows link).
int run_docs(int argc, const char** argv, const char* version);

}  // namespace culebra
