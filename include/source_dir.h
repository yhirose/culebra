#pragma once

namespace culebra {

// The source checkout this binary was built from, or "" if it was built
// without one. A function in its own TU rather than a macro read by main.cc:
// an absolute per-worktree path on main.cc's command line would make that
// (huge) object unshareable between worktrees in ccache. Keep new path-valued
// defines out of main.cc for the same reason.
const char* source_dir();

}  // namespace culebra
