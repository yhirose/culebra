#pragma once

// Type-neutral gzip core for the Compress namespace.
//
// No dependency on culebra Value / JitValue / GC: both the interp and JIT
// backends call gzip()/gunzip() and adapt the Result into their own string
// representation (mirrors http.h / proc.h). Errors are reported in a
// value-neutral Result.error so this header needs no culebra error type.
//
// gzip()/gunzip() are the single zlib choke: deflate/inflate (the only code
// that pulls in libz) is reached only from these bodies. Their linkage is
// partitioned across the AOT runtime archives exactly like http_request /
// tensor_eval_node:
//   - core archive     (CULEBRA_RT_COMPRESS_WEAK):   weak stub, no zlib symbol,
//     so a program that never compresses links neither this nor libz.
//   - compress archive (CULEBRA_RT_COMPRESS_STRONG): strong real body,
//     force-loaded only when the AST scan reports Compress use (overrides the
//     stub) and what pulls in the libz link flag.
//   - header-only / in-process JIT (neither): the normal inline body.

#include <cstdint>
#include <string>
#include <string_view>

#if !defined(CULEBRA_RT_COMPRESS_WEAK)
#include <zlib.h>
#endif

namespace culebra::compress {

// On success `error` is empty and `data` holds the output bytes; on failure
// `error` carries a human message and `data` is empty.
struct Result {
  std::string data;
  std::string error;
};

#if defined(CULEBRA_RT_COMPRESS_STRONG)
#define CULEBRA_RT_COMPRESS_LINKAGE
#elif defined(CULEBRA_RT_COMPRESS_WEAK)
#define CULEBRA_RT_COMPRESS_LINKAGE __attribute__((weak))
#else
#define CULEBRA_RT_COMPRESS_LINKAGE inline
#endif

// gzip-compress `data` (RFC 1952 wrapper). Binary-safe: `data` may hold NUL.
CULEBRA_RT_COMPRESS_LINKAGE Result gzip(std::string_view data) {
#if defined(CULEBRA_RT_COMPRESS_WEAK)
  (void)data;
  return {{}, "runtime not linked (no Compress use detected at build)"};
#else
  if (data.size() > 0xFFFFFFFFull) return {{}, "input too large"};
  z_stream zs{};
  // windowBits 15 + 16 selects a gzip wrapper over raw deflate.
  if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8,
                   Z_DEFAULT_STRATEGY) != Z_OK) {
    return {{}, "deflate init failed"};
  }
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
  zs.avail_in = static_cast<uInt>(data.size());
  std::string out;
  // deflateBound is a tight upper bound on the compressed size — reserve it so
  // the chunk-append loop never reallocates.
  out.reserve(deflateBound(&zs, static_cast<uLong>(data.size())));
  unsigned char buf[16384];
  int ret;
  do {
    zs.next_out = buf;
    zs.avail_out = sizeof(buf);
    ret = deflate(&zs, Z_FINISH);
    out.append(reinterpret_cast<char*>(buf), sizeof(buf) - zs.avail_out);
  } while (ret == Z_OK);
  deflateEnd(&zs);
  if (ret != Z_STREAM_END) return {{}, "deflate failed"};
  return {std::move(out), {}};
#endif
}

// Decompress gzip or zlib data (header auto-detected). Returns error on
// malformed/truncated input. Binary-safe.
CULEBRA_RT_COMPRESS_LINKAGE Result gunzip(std::string_view data) {
#if defined(CULEBRA_RT_COMPRESS_WEAK)
  (void)data;
  return {{}, "runtime not linked (no Compress use detected at build)"};
#else
  if (data.size() > 0xFFFFFFFFull) return {{}, "input too large"};
  z_stream zs{};
  // windowBits 15 + 32 enables automatic gzip/zlib header detection.
  if (inflateInit2(&zs, 15 + 32) != Z_OK) {
    return {{}, "inflate init failed"};
  }
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
  zs.avail_in = static_cast<uInt>(data.size());
  std::string out;
  unsigned char buf[16384];
  int ret;
  do {
    zs.next_out = buf;
    zs.avail_out = sizeof(buf);
    ret = inflate(&zs, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END) {
      inflateEnd(&zs);
      return {{}, "invalid gzip data"};
    }
    out.append(reinterpret_cast<char*>(buf), sizeof(buf) - zs.avail_out);
  } while (ret != Z_STREAM_END);
  inflateEnd(&zs);
  return {std::move(out), {}};
#endif
}

#undef CULEBRA_RT_COMPRESS_LINKAGE

}  // namespace culebra::compress
