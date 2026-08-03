# Generate editor_assets_embedded.cc from misc/{vscode,vim}'s payload files, so
# `culebra init` can lay them down on a binary-only release with no source
# checkout available — the same gap gen_docs.cmake closed for the reference
# docs (a release archive carries neither docs/ nor misc/).
#
# Run at build time (`cmake -P`), not committed: the payload changes with
# grammar/keyword sync (see misc/sync_grammar.sh), and a committed blob would
# need its own staleness gate. Written in CMake rather than sh for the same
# reason as gen_docs.cmake — the Windows build has no shell dependency to
# lean on.
#
# The file list is explicit, never a glob: misc/vscode and misc/vim also hold
# install.sh / build-vsix.sh, whose *logic* is reimplemented in init_cmd.cc
# (a binary-only user has no guarantee of a `zip` binary on PATH, especially
# on Windows) rather than embedded and shelled out to — only the payload
# files below are data `culebra init` writes as-is.
#
# Usage: cmake -DMISC_DIR=<dir> -DOUT=<file> -P gen_editor_assets.cmake

set(ASSETS
  "vscode|package.json"
  "vscode|language-configuration.json"
  "vscode|extension.js"
  "vscode|syntaxes/culebra.tmLanguage.json"
  "vim|culebra.vim"
  "vim|culebra_ftplugin.vim"
)

set(BODY "")
set(TABLE "")
set(_index 0)

foreach(entry IN LISTS ASSETS)
  string(REPLACE "|" ";" parts "${entry}")
  list(GET parts 0 editor)
  list(GET parts 1 rel_path)

  set(path "${MISC_DIR}/${editor}/${rel_path}")
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "gen_editor_assets: missing ${path}")
  endif()
  file(READ "${path}" text)

  # A raw string ends at its delimiter and nowhere else, so the only way an
  # asset can break the generated TU is by containing the delimiter itself.
  string(FIND "${text}" ")=culasset=" clash)
  if(NOT clash EQUAL -1)
    message(FATAL_ERROR "gen_editor_assets: ${path} contains the raw-string delimiter )=culasset=")
  endif()

  string(APPEND BODY "constexpr const char* kText_${_index} =\n    R\"=culasset=(${text})=culasset=\";\n\n")
  string(APPEND TABLE "    {\"${editor}\", \"${rel_path}\", kText_${_index}},\n")
  math(EXPR _index "${_index} + 1")
endforeach()

file(WRITE "${OUT}"
"// Generated from misc/{vscode,vim}/* by cmake/gen_editor_assets.cmake — do not edit.
#include \"editor_assets_embedded.h\"

namespace culebra::editor_assets {
namespace {

${BODY}}  // namespace

const Asset kAssets[] = {
${TABLE}};

const size_t kAssetCount = sizeof(kAssets) / sizeof(kAssets[0]);

}  // namespace culebra::editor_assets
")
