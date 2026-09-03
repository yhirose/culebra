# Generate docs_embedded.cc from the reference docs, so the single binary
# carries them and `culebra docs` answers from the same build the user runs.
#
# Run at build time (`cmake -P`), not committed: docs change every cycle, and a
# committed 1.3 MB blob would put that churn in every diff and add a staleness
# gate to every docs edit. Written in CMake rather than sh because the Windows
# build (mingw) has no shell dependency to lean on.
#
# The file list is explicit, never a glob: docs/_history.* and docs/_internals.*
# are gitignored local notes, and a glob would embed whatever private file
# happens to sit there.
#
# Usage: cmake -DDOCS_DIR=<dir> -DOUT=<file> -P gen_docs.cmake

# name|English summary|Japanese summary — `quick-guide` leads because it is
# the one a reader with no context should open first.
set(TOPICS
  "quick-guide|Everything needed to write culebra, in one file|書き始めに必要な全部を 1 ファイルに凝縮"
  "agent|Rules to paste into a coding agent's instructions|コーディングエージェントの指示ファイルに貼る規則"
  "handbook|Task-oriented walkthrough of the language|言語のタスク指向ガイド"
  "language|The language reference|言語リファレンス"
  "stdlib|Every namespace and its signatures|全 namespace とその署名"
  "tooling|test, lint, fmt and the debug adapter|test・lint・fmt とデバッグアダプタ"
  "deployment|Embedding, AOT builds, release archives|埋め込み・AOT ビルド・リリース書庫"
)

set(BODY "")
set(TABLE "")

foreach(entry IN LISTS TOPICS)
  string(REPLACE "|" ";" parts "${entry}")
  list(GET parts 0 name)
  list(GET parts 1 summary_en)
  list(GET parts 2 summary_ja)

  # Topic names are also C++ identifiers below; a hyphen (e.g. quick-guide)
  # is valid in the former but not the latter, so the generated symbol uses
  # a sanitized copy while `name` itself stays what the CLI and file paths use.
  string(REPLACE "-" "_" ident "${name}")

  foreach(lang en ja)
    if(lang STREQUAL en)
      set(path "${DOCS_DIR}/${name}.md")
      set(summary "${summary_en}")
    else()
      set(path "${DOCS_DIR}/${name}.ja.md")
      set(summary "${summary_ja}")
    endif()
    if(NOT EXISTS "${path}")
      message(FATAL_ERROR "gen_docs: missing ${path} (the bilingual pair must be complete)")
    endif()
    file(READ "${path}" text)

    # A raw string ends at its delimiter and nowhere else, so the only way a
    # doc can break the generated TU is by containing the delimiter itself.
    string(FIND "${text}" ")=culdoc=" clash)
    if(NOT clash EQUAL -1)
      message(FATAL_ERROR "gen_docs: ${path} contains the raw-string delimiter )=culdoc=")
    endif()

    string(APPEND BODY "constexpr const char* kText_${ident}_${lang} =\n    R\"=culdoc=(${text})=culdoc=\";\n\n")
    string(APPEND TABLE "    {\"${name}\", \"${lang}\", \"${summary}\", kText_${ident}_${lang}},\n")
  endforeach()
endforeach()

file(WRITE "${OUT}"
"// Generated from docs/*.md by cmake/gen_docs.cmake — do not edit.
#include \"cli/docs_embedded.h\"

namespace culebra::docs {
namespace {

${BODY}}  // namespace

const Topic kTopics[] = {
${TABLE}};

const size_t kTopicCount = sizeof(kTopics) / sizeof(kTopics[0]);

}  // namespace culebra::docs
")
