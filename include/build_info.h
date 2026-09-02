#pragma once

namespace culebra {

// What this binary was built from, past the version in culebra.h:
// "-361-g30993ea2", "-dirty", or "" for a build of a release tag with a clean
// tree (and for one with no checkout to ask, such as a source archive).
// `--version` appends it, so a development build never claims to be the
// release that shares its version number.
//
// A function in its own generated TU rather than a define read by main.cc:
// the value changes with every commit, and main.cc's object is the one that
// must stay shareable in ccache (see source_dir.h).
const char* build_suffix();

}  // namespace culebra
