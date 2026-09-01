// Linkage of thread_local runtime state that the core archive owns.
//
// An AOT link is libculebra_rt.a plus every feature archive the program's
// namespaces select, force-loaded with --whole-archive. A namespace-scope
// `thread_local` with dynamic initialization emits a global "TLS init function
// for X" in every TU that merely *declares* it, so a header shared by the core
// and a feature TU lands the same symbol in both archives. ELF folds the
// duplicates; PE/COFF has no weak external, and mingw's ld fails the link with
// `multiple definition of 'TLS init function for X'`.
//
// So each such variable gets one owner. `CULEBRA_RT_CORE_OWNED` spells "the
// core archive defines this; a feature archive borrows it":
//
//     CULEBRA_RT_CORE_OWNED thread_local std::vector<Handle*> g_handles;
//
// A borrowing TU (CULEBRA_RT_FEATURE_BUILD, set by CMake on every culebra_rt_*
// target and on the binding sources the driver compiles in) turns that into an
// `extern` declaration and resolves against its owner at link time — the core
// archive in an AOT link, main.cc in the driver. Every other build — plain
// embedding, the header-only JIT — keeps the inline definition it always had.
//
// Only the thread_locals travel on this flag. The runtime helpers' emission is
// CULEBRA_RT_FEATURE_ARCHIVE (rt_macros.h), which is archive-only: the driver
// has to keep defining those for the in-process JIT to resolve them.
//
// State the core has no business holding at all is the other, simpler answer:
// gate the declaration out of the weak-stub build entirely (sqlite.h). Either
// way `tools/checks/check_rt_archive_tls.sh` fails the gate when two archives define
// the same variable.
#pragma once

#if defined(CULEBRA_RT_FEATURE_BUILD)
#define CULEBRA_RT_CORE_OWNED extern
#else
#define CULEBRA_RT_CORE_OWNED inline
#endif
