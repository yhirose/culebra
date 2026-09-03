// Compress feature object for the linear-scaling AOT runtime. Emits the
// single strong definition of culebra::compress::gzip / gunzip — the lone
// zlib choke (the only code that calls deflate/inflate) — which the AOT link
// force-loads only when the program uses Compress, overriding the weak stubs
// in the core archive. libz symbols are referenced exclusively from here, so a
// program that never compresses links neither this object nor zlib.
//
// CULEBRA_RT_COMPRESS_STRONG makes gzip/gunzip strong, non-inline definitions;
// compress.h is value-neutral (no interpreter/JIT types), so this TU stays
// small.

#define CULEBRA_RT_COMPRESS_STRONG
#include <stdlib/compress.h>
