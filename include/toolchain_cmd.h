#pragma once

// `culebra toolchain` — the AOT link's host requirements, managed by culebra
// rather than by the reader of an error message.
//
// `culebra build` is the one subcommand that reaches outside the binary: the
// codegen is in-process, but a link needs the platform's object-file plumbing.
// What that means differs enough per platform that this file is mostly three
// separate stories behind one verb:
//
//   Windows — the machine has nothing, and cannot be told to `apt install` its
//     way out. culebra carries lld itself (CMakeLists, CULEBRA_INPROCESS_LLD),
//     so what is missing is only the mingw half of a link: CRT objects,
//     libstdc++/libgcc, the Win32 import libraries. Those are packed at release
//     time out of the same MSYS2 tree that compiled the embedded runtime
//     archives (misc/pack_windows_toolchain.sh), and `install` fetches that kit
//     into %LOCALAPPDATA%. Nothing is elevated, nothing touches PATH or the
//     registry, and `uninstall` is a directory removal.
//   macOS — the Command Line Tools exist as an Apple installer; `install`
//     starts it. It is asynchronous and user-driven, so culebra hands over and
//     says to come back.
//   Linux — the C++ toolchain belongs to the distribution's package manager and
//     needs root. culebra does not run those; `install` prints the one command
//     for the distro and stops.
//
// The version scoping is the whole safety story on Windows: a kit's libstdc++
// has to be the one the embedded runtime archives were compiled against, and
// the release that shipped them together is what says so. Kits therefore live
// under a per-version directory and `install` refuses a kit whose MANIFEST
// names a different version.

#include <filesystem>
#include <string>
#include <vector>

namespace culebra::toolchain {

// The directory a kit for THIS build's version lives in, or empty on a
// platform that has no kit (everything but Windows).
std::filesystem::path kit_dir();

// Whether a link can happen right now, and what to say when it cannot. The two
// go together: a caller that finds no toolchain has to name the fix, and the
// phrasing differs per platform and per reason (no kit vs. no compiler).
bool link_available();
std::string missing_toolchain_hint();

// The linker argv a kit records: what a C++ driver puts around a user object
// on this toolchain. culebra never spells this itself — it splices its own
// tokens between the two halves — so a toolchain change cannot drift away from
// what the driver would have done.
struct Recipe {
  std::string target;
  std::vector<std::string> prefix, suffix;
};
bool load_recipe(const std::filesystem::path& kit, Recipe& out, std::string& err);

#ifdef CULEBRA_INPROCESS_LLD
// Link with the lld this binary carries. `driver_args` are culebra's own
// tokens in compiler-driver form (`-Wl,--gc-sections`, `-lz`, paths); the
// recipe supplies the toolchain's half and this translates between the two.
// Returns false with `err` set; linker diagnostics go to stderr as they occur.
bool link_in_process(const std::vector<std::string>& driver_args,
                     const std::string& output, bool verbose, std::string& err);
#endif

// Offer to install, then install, when `culebra build` finds nothing to link
// with. Only asks on a terminal — a script gets the hint and a failure instead
// of a prompt it cannot answer. Returns true when a link can now proceed.
bool offer_install_interactively();

// `culebra toolchain [install [--from <archive>] | uninstall | status]`,
// taking the whole command line the way the other subcommands do (argv[1] is
// "toolchain").
int main(int argc, char** argv);

}  // namespace culebra::toolchain
