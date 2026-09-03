# Generate editor_assets_embedded.cc from misc/{vscode,vim,zed}'s payload
# files, so `culebra init` can lay them down on a binary-only release with no
# source checkout available — the same gap gen_docs.cmake closed for the
# reference docs (a release archive carries neither docs/ nor misc/).
#
# Run at build time (`cmake -P`), not committed: the payload changes with
# grammar/keyword sync (see misc/sync_grammar.sh), and a committed blob would
# need its own staleness gate. Written in CMake rather than sh for the same
# reason as gen_docs.cmake — the Windows build has no shell dependency to
# lean on.
#
# The file list is explicit, never a glob: misc/vscode, misc/vim and misc/zed
# also hold install.sh / build-vsix.sh, whose *logic* is reimplemented in
# init_cmd.cc (a binary-only user has no guarantee of a `zip` binary on PATH,
# especially on Windows) rather than embedded and shelled out to — only the
# payload files below are data `culebra init` writes as-is. The Zed grammar
# itself (misc/zed/tree-sitter-culebra/, tens of KB of generated parser) is
# not in this list: culebra init points Zed at it on GitHub instead of
# embedding it (see setup_zed() in init_cmd.cc).
#
# Also embeds CULEBRA_VERSION (extracted from VERSION_HEADER, normally
# include/culebra.h) as editor_assets::kCulebraVersion — culebra init needs it
# to pin the Zed grammar fetch at this binary's matching release tag (see
# init_cmd.cc's setup_zed()), and init_cmd.cc otherwise stays free of
# culebra.h / interpreter internals (see its own comment on that isolation).
#
# Usage: cmake -DMISC_DIR=<dir> -DVERSION_HEADER=<file> -DOUT=<file>
#              -P gen_editor_assets.cmake

set(ASSETS
  "vscode|package.json"
  "vscode|language-configuration.json"
  "vscode|extension.js"
  "vscode|syntaxes/culebra.tmLanguage.json"
  "vim|culebra.vim"
  "vim|culebra_ftplugin.vim"
  "zed|Cargo.toml"
  "zed|src/culebra.rs"
  "zed|debug_adapter_schemas/culebra.json"
  "zed|languages/culebra/config.toml"
  "zed|languages/culebra/highlights.scm"
  "zed|debug.json"
  "zed|extension.toml.template"
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

file(STRINGS "${VERSION_HEADER}" _version_line REGEX "^#define CULEBRA_VERSION ")
if(NOT _version_line)
  message(FATAL_ERROR "gen_editor_assets: no CULEBRA_VERSION in ${VERSION_HEADER}")
endif()
string(REGEX REPLACE "^#define CULEBRA_VERSION \"([^\"]*)\"" "\\1" CULEBRA_VERSION "${_version_line}")

file(WRITE "${OUT}"
"// Generated from misc/{vscode,vim,zed}/* by cmake/gen_editor_assets.cmake — do not edit.
#include \"cli/editor_assets_embedded.h\"

namespace culebra::editor_assets {
namespace {

${BODY}}  // namespace

const Asset kAssets[] = {
${TABLE}};

const size_t kAssetCount = sizeof(kAssets) / sizeof(kAssets[0]);

const char kCulebraVersion[] = \"${CULEBRA_VERSION}\";

}  // namespace culebra::editor_assets
")
