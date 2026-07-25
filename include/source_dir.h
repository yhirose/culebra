#pragma once

namespace culebra {

// The source checkout this binary was built from, or "" if it was built
// without one. Deliberately a function in its own TU (src/source_dir.cc)
// rather than a macro: baking CULEBRA_SOURCE_DIR into main.cc would put an
// absolute, per-worktree path on that TU's command line, and main.cc is the
// one huge translation unit whose object we most want ccache to share
// between worktrees of the same commit.
const char* source_dir();

}  // namespace culebra
