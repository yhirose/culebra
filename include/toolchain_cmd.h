#pragma once

// `culebra toolchain` — the AOT link's host requirements, managed by culebra
// rather than by the reader of an error message.
//
// `culebra build` is the one subcommand that reaches outside the binary: the
// codegen is in-process, but a link needs the platform's object-file plumbing.
// What that means per platform, and why Windows carries lld rather than
// shipping a linker beside it, is docs/deployment.md §"Why Windows needs no
// compiler". The rest of this command is private to src/toolchain_cmd.cc.

#include <string>
#include <vector>

namespace culebra::toolchain {

// Whether this link should go through the lld inside the binary rather than an
// external compiler driver. True exactly when a kit for this version is
// installed — the arrangement a downloader has. A machine with MSYS2 on PATH
// (every contributor, every CI job that compiles culebra) keeps the external
// path, so the two never disagree about which libstdc++ a link uses.
bool use_inprocess_link();

// Link with the lld this binary carries. `driver_args` are culebra's own tokens
// in compiler-driver form (`-Wl,--gc-sections`, `-lz`, paths); the kit's recipe
// supplies the toolchain's half and this translates between the two. Returns
// false with `err` set — including in a build that carries no linker, so a
// caller cannot mistake "not compiled in" for a link that happened.
bool link_in_process(const std::vector<std::string>& driver_args,
                     const std::string& output, bool verbose, std::string& err);

// GET `url` into `body`, following redirects, with `false` and `err` on any
// transport failure or a status that is not 200.
//
// Defined in src/main.cc rather than beside its caller: http.h carries `inline
// thread_local` handle registries that exactly one driver TU may instantiate
// (mingw's ld fails the link on a second — tools/check_rt_archive_tls.sh), and
// main.cc is that TU. Declaring it across this seam is what lets the toolchain
// installer share the Http namespace's client instead of building a second one.
// The knobs stay with the caller, which is where the policy is.
bool fetch_url(const std::string& url, int64_t timeout_sec,
               int64_t connect_timeout_sec, const std::string& proxy,
               std::string& body, std::string& err);

// Offer to install, then install, when `culebra build` finds nothing to link
// with. Only asks on a terminal — a script gets the hint and a failure instead
// of a prompt it cannot answer. Returns true when a link can now proceed.
//
// `host_link` is false for a --target build. A kit describes THIS host's
// toolchain, so it cannot serve a cross link and must not be offered for one:
// otherwise a Windows box with a kit passes this check and then dies in the
// external driver it fell through to, with the very "clang++ is not
// recognized" the kit exists to prevent.
bool offer_install_interactively(bool host_link);

// `culebra toolchain [status | install [--from <archive>] | uninstall]`, taking
// the whole command line the way the other subcommands do (argv[1] is
// "toolchain").
int run_toolchain(int argc, const char** argv);

}  // namespace culebra::toolchain
