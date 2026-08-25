#pragma once

// Single source of truth for the version. `culebra --version` prints it, and
// playground/build.sh greps this line to stamp the Playground title, so keep
// the `#define CULEBRA_VERSION "X.Y.Z"` form on one line.
#define CULEBRA_VERSION "0.3.1"

#include "module_loader.h"

// The compiled lanes run bytecode: vm.h holds the compiler and the executor,
// and needs no LLVM. vm_lowering.h adds the second consumer of the same
// bytecode and defines JIT::run / JIT::build_object over it.
#include "vm.h"

#ifdef CULEBRA_JIT_ENABLED
#include "jit.h"
#include "vm_lowering.h"
#endif
