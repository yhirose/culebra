// A standalone `ld.lld` for the Windows AOT toolchain kit.
//
// This exists to answer one sizing question, and may not survive it. `culebra
// build` on Windows needs a linker, and there are two places to put one:
// inside culebra.exe (where every download pays for it, measured at +47%), or
// beside the mingw libraries in the kit, where only someone who runs `culebra
// build` ever fetches it.
//
// MSYS2's own ld.lld.exe cannot go in a kit — it links against a 147 MB
// libLLVM DLL, which is what made a linker-carrying kit 55 MB zipped against
// 6 MB for one carrying libraries alone. This links the same lld against the
// same per-component LLVM statics culebra.exe uses, so it needs no DLL beyond
// what Windows ships, and its whole cost lands in the kit.
//
// Deliberately not a copy of lld's own driver: no flavor dispatch, no `-flavor`
// argument, no COFF or ELF entry points. The kit's recipe always produces a
// MinGW-flavoured argv (`-m i386pep`), so mingw::link is the only driver a kit
// can ever need, and naming just that one keeps every other driver's code out
// of the binary being measured.

#include <lld/Common/Driver.h>
#include <llvm/Support/raw_ostream.h>

#include <vector>

LLD_HAS_DRIVER(mingw)

int main(int argc, char** argv) {
  std::vector<const char*> args(argv, argv + argc);
  // exitEarly: this process is the linker and has nothing to return to, so
  // letting lld exit on a fatal error is right here — where the in-process
  // caller in src/toolchain_cmd.cc must say false instead.
  return lld::mingw::link(args, llvm::outs(), llvm::errs(),
                          /*exitEarly=*/true, /*disableOutput=*/false)
             ? 0
             : 1;
}
