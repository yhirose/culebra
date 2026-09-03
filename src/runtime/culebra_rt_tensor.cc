// Tensor feature object for the linear-scaling AOT runtime. Emits the
// single strong definition of
// `culebra::tensor_eval_node` — the lone cblas choke point — which the
// AOT link force-loads only when the program uses Tensor, overriding the
// weak throwing stub in the core archive. cblas / Accelerate symbols are
// referenced exclusively from here (via the _tensor_run_* kernels that
// tensor_eval_node dispatches to), so a program that never evaluates a
// tensor links neither this object nor BLAS.
//
// CULEBRA_RT_TENSOR_EVAL_STRONG makes tensor_eval_node a strong,
// non-inline definition; tensor.h is otherwise self-contained (pure C++,
// no interpreter/JIT Value layer), so this TU stays small.

#define CULEBRA_RT_TENSOR_EVAL_STRONG
#include <stdlib/tensor.h>
