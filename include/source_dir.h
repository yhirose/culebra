#pragma once

#include <string>

namespace culebra {

// The source checkout this binary was built from, or "" if it was built
// without one. A function in its own TU rather than a macro read by main.cc:
// an absolute per-worktree path on main.cc's command line would make that
// (huge) object unshareable between worktrees in ccache. Keep new path-valued
// defines out of main.cc for the same reason.
const char* source_dir();

// The checkout to actually use: $CULEBRA_HOME wins over the baked path, so a
// relocated or copied install can still be pointed at a tree. Every command
// that needs the sources goes through this — `culebra build` (Embed.dir needs
// the headers) and `culebra wrap` (rebuilds the tree) must agree on which
// checkout they mean.
std::string resolved_source_dir();

}  // namespace culebra
