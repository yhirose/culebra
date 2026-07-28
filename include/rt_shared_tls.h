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
// A feature archive (CULEBRA_RT_FEATURE_BUILD, set by CMake on every
// culebra_rt_* target) turns that into an `extern` declaration and resolves
// against the core at link time; every other build — the driver, the in-process
// JIT, header-only embedding — keeps the inline definition it always had.
//
// State the core has no business holding at all is the other, simpler answer:
// gate the declaration out of the weak-stub build entirely (sqlite.h). Either
// way `tools/check_rt_archive_tls.sh` fails the gate when two archives define
// the same variable.
#pragma once

#if defined(CULEBRA_RT_FEATURE_BUILD)
#define CULEBRA_RT_CORE_OWNED extern
#else
#define CULEBRA_RT_CORE_OWNED inline
#endif
