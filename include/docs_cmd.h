#pragma once

namespace culebra {

// `culebra docs` — read the embedded reference docs. Returns the process exit
// code: 0 found, 1 nothing matched (the grep convention, so a caller can ask
// "does this API exist?" without reading the output), 2 bad usage.
int run_docs(int argc, const char** argv);

}  // namespace culebra
