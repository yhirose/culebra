# MiniZip (vendored)

The ZIP reader and writer from zlib's `contrib/minizip`, which `Dir.zip` and
`Compress.zip` drive through `vendor/cpp-zipper/zipper.h` (`include/stdlib/zip.h`). The files are
committed directly (not a submodule): they are a directory of the zlib
source tree, and culebra needs only the reader, the writer and their I/O
layer. zlib itself is the system's (the same libz the `Compress` namespace
links).

- **Version:** zlib 1.3.2 (`contrib/minizip`)
- **Source:** https://github.com/madler/zlib/archive/refs/tags/v1.3.2.tar.gz
- **SHA-512 (tar.gz):** `16fea4df307a68cf0035858abe2fd550250618a97590e202037acd18a666f57afc10f8836cbbd472d54a0e76539d0e558cb26f059d53de52ff90634bbf4f47d4`
- **License:** the zlib license (in the header of each file); `crypt.h` also
  carries the Info-ZIP license (`LICENSE.Info-Zip`)

Files, unchanged from the source:
- `minizip/ioapi.c`, `ioapi.h`: the I/O hooks (cpp-zipper plugs memory into them)
- `minizip/unzip.c`, `unzip.h`: the reader
- `minizip/zip.c`, `zip.h`: the writer
- `minizip/crypt.h`, `ints.h`, `skipset.h`: headers the three include
- `MiniZip64_info.txt`: the project's own notes

They sit under `minizip/` so that `#include <minizip/unzip.h>`, as
`zipper.h` writes it, resolves with `vendor/minizip` on the include path.

## Updating

Download a newer zlib release tag, check its SHA-512, and copy the files
above from its `contrib/minizip`. Bump the version and hash here.
