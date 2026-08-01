// Deflate a file for embedding: `compress_asset <in> <out>`.
//
// A build step rather than `gzip -9`, because the dependency policy is a
// C++23 compiler and nothing else, and because
// writing the output where we ask lets the embedded entry keep the archive's
// own name — the driver's inflate (materialize_archive, src/main.cc) then
// needs no filename convention to agree with. zlib is already required.
#include <compress.h>

#include <fstream>
#include <print>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
  if (argc != 3) {
    std::println(stderr, "usage: compress_asset <in> <out>");
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::println(stderr, "compress_asset: can't read '{}'", argv[1]);
    return 1;
  }
  std::ostringstream raw;
  raw << in.rdbuf();

  // Level 9, and the zlib wrapper rather than gzip's: gunzip() sniffs either,
  // this one carries no mtime to make the output differ build to build, and
  // its header is 12 bytes shorter.
  auto packed = culebra::compress::deflate_zlib(raw.view(), 9);
  if (!packed.error.empty()) {
    std::println(stderr, "compress_asset: {}: {}", argv[1], packed.error);
    return 1;
  }

  std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
  out.write(packed.data.data(),
            static_cast<std::streamsize>(packed.data.size()));
  if (!out) {
    std::println(stderr, "compress_asset: can't write '{}'", argv[2]);
    return 1;
  }
  return 0;
}
