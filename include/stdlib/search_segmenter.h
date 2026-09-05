#pragma once

// The Search.segmenter model loader: search.h's choke applied once more, one
// archive further in. A program that names Search links the Search archive,
// and cpp-segmentlib -- the model backends behind Search.segmenter, ~140 KB --
// is code only a program that loads a model should carry. So the loader
// alone is partitioned again on the same terms:
//   - search archive    (CULEBRA_RT_SEGMENTER_WEAK):   a weak throwing stub;
//     searchlib_segment.h is never included, so the archive references no
//     segmentlib:: symbol.
//   - segmenter archive (CULEBRA_RT_SEGMENTER_STRONG): the strong body,
//     force-loaded only when the AST scan reports `segmenter`.
//   - header-only / in-process JIT (neither): the normal inline body.
// Only the loader lives here. The registry and the handle functions stay in
// search.h, so this archive defines no thread_local of its own -- a second
// definition is what breaks the Windows AOT link.

#include <string>
#include <string_view>

#include <base/shared.h>  // CulebraError
#include <searchlib.h>

#if !defined(CULEBRA_RT_SEGMENTER_WEAK)
#include <searchlib_segment.h>
#endif

#if defined(CULEBRA_RT_SEGMENTER_STRONG)
#define CULEBRA_RT_SEGMENTER_LINKAGE
#elif defined(CULEBRA_RT_SEGMENTER_WEAK)
#define CULEBRA_RT_SEGMENTER_LINKAGE __attribute__((weak))
#else
#define CULEBRA_RT_SEGMENTER_LINKAGE inline
#endif

namespace culebra::search {

// The model at `model_path` as searchlib's splitter. Throws what segmentlib
// throws; segmenter_load (search.h) owns the handle and shapes the error.
CULEBRA_RT_SEGMENTER_LINKAGE searchlib::TextSplitter segmenter_open(
    std::string_view model_path);

#if defined(CULEBRA_RT_SEGMENTER_WEAK)

CULEBRA_RT_SEGMENTER_LINKAGE searchlib::TextSplitter segmenter_open(
    std::string_view) {
  throw CulebraError("InternalError",
                     "Search segmenter runtime entered in a binary that never "
                     "names it",
                     0, 0);
}

#else  // the real body

CULEBRA_RT_SEGMENTER_LINKAGE searchlib::TextSplitter segmenter_open(
    std::string_view model_path) {
  return searchlib::load_segmenting_splitter(std::string(model_path));
}

#endif  // CULEBRA_RT_SEGMENTER_WEAK

}  // namespace culebra::search
