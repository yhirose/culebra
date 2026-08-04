#pragma once

#include <cstddef>

// The VSCode/Vim editor-integration payload files, compiled into the binary
// by cmake/gen_editor_assets.cmake, so `culebra init` can lay them down with
// no source checkout available (a release archive carries none of misc/).
// Kept in its own translation unit for the same reason as docs_embedded.h: an
// asset edit relinks rather than rebuilding init_cmd.cc.
namespace culebra::editor_assets {

struct Asset {
  const char* editor;    // "vscode" | "vim" | "zed"
  const char* rel_path;  // path under misc/<editor>/, e.g.
                         // "syntaxes/culebra.tmLanguage.json"
  const char* text;
};

extern const Asset kAssets[];
extern const size_t kAssetCount;

// CULEBRA_VERSION (include/culebra.h), re-exported here so init_cmd.cc can
// pin the Zed grammar fetch at this binary's release tag without including
// culebra.h itself.
extern const char kCulebraVersion[];

}  // namespace culebra::editor_assets
