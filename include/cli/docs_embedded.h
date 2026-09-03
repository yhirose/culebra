#pragma once

#include <cstddef>

// The reference docs, compiled into the binary by cmake/gen_docs.cmake. Kept
// in its own translation unit so a docs edit relinks rather than rebuilding
// main.cc, and so the search code above it never carries 1.3 MB of literals.
namespace culebra::docs {

struct Topic {
  const char* name;     // "stdlib"
  const char* lang;     // "en" | "ja"
  const char* summary;  // one line, for `culebra docs`
  const char* text;     // the whole document
};

extern const Topic kTopics[];
extern const size_t kTopicCount;

}  // namespace culebra::docs
