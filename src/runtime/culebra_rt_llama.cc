// Llama feature object for the linear-scaling AOT runtime. Emits the strong
// definitions of every culebra::llama::* function that touches llama.cpp (the
// lone anchor), which the AOT link force-loads only when the program uses
// Llama, overriding the weak stubs in the core archive. The statics it needs
// come from CULEBRA_LLAMA_LINK.
//
// CULEBRA_RT_LLAMA_STRONG (set by CMake on this target) makes the functions
// strong, non-inline definitions; llama.h is value-neutral, so this TU stays
// small.

#include <stdlib/llama.h>
