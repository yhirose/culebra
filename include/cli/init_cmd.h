#pragma once

namespace culebra {

// `culebra init` — lay down everything a freshly downloaded binary needs to
// start developing in the current directory: an AI coding agent instructions
// block in whichever of CLAUDE.md / AGENTS.md / .github/copilot-instructions.md
// already exist (or a new AGENTS.md if none do), and this machine's editor
// integration (syntax highlighting + the `culebra dap` debug adapter) for
// whichever of VSCode / Vim / Neovim it finds. Every step is idempotent and
// just overwrites with whatever this binary carries, so re-running `init`
// after an upgrade is the update path. No dependency on culebra.h /
// interpreter internals, same rationale as docs_cmd.h.
int run_init(int argc, const char** argv);

}  // namespace culebra
