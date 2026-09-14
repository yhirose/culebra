#pragma once

// Content-Length-framed messages over a pair of file descriptors: the base
// protocol the Debug Adapter Protocol (`culebra dap`) and the Language Server
// Protocol (`culebra lsp`) both speak on stdio. Each message is a header block
// ending in a blank line, whose `Content-Length:` gives the size of the body
// that follows.

#include <cstdint>
#include <cstdlib>
#if defined(_WIN32)
#include <io.h>  // _read / _write
#else
#include <unistd.h>
#endif

#include <mutex>
#include <string>
#include <string_view>

namespace culebra {

// Byte-stream read/write on a raw fd. On Windows the POSIX ::read/::write live
// in <io.h> under an underscore; the fd values (0/1) are the same CRT
// descriptors.
namespace _framed_io {
inline int64_t read_fd(int fd, char* buf, size_t n) {
#if defined(_WIN32)
  return _read(fd, buf, static_cast<unsigned>(n));
#else
  return ::read(fd, buf, n);
#endif
}
inline int64_t write_fd(int fd, const char* buf, size_t n) {
#if defined(_WIN32)
  return _write(fd, buf, static_cast<unsigned>(n));
#else
  return ::write(fd, buf, n);
#endif
}
}  // namespace _framed_io

class FramedStdio {
 public:
  FramedStdio(int in_fd, int out_fd) : in_(in_fd), out_(out_fd) {}

  // The next message's body. False at end of input or on a read error; a
  // message cut short by either is dropped.
  bool read(std::string& body) {
    size_t hdr_end;
    while ((hdr_end = inbuf_.find("\r\n\r\n")) == std::string::npos) {
      if (!fill()) return false;
    }
    size_t len = 0;
    auto p = std::string_view(inbuf_).substr(0, hdr_end).find("Content-Length:");
    if (p != std::string::npos)
      len = std::strtoul(inbuf_.c_str() + p + 15, nullptr, 10);
    size_t start = hdr_end + 4;
    while (inbuf_.size() < start + len) {
      if (!fill()) return false;
    }
    body = inbuf_.substr(start, len);
    inbuf_.erase(0, start + len);
    return true;
  }

  // Frame and write one message. Safe from several threads: a debug adapter
  // answers requests on one and forwards the debuggee's output on another.
  void write(std::string_view body) {
    std::string frame =
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    frame.append(body);
    std::lock_guard<std::mutex> lk(write_mu_);
    size_t off = 0;
    while (off < frame.size()) {
      int64_t n = _framed_io::write_fd(out_, frame.data() + off,
                                       frame.size() - off);
      if (n <= 0) break;
      off += static_cast<size_t>(n);
    }
  }

  // Where messages go. A debug adapter that redirects fd 1 to capture the
  // debuggee's output keeps writing to a duplicate of the original.
  int out_fd() {
    std::lock_guard<std::mutex> lk(write_mu_);
    return out_;
  }
  void set_out_fd(int fd) {
    std::lock_guard<std::mutex> lk(write_mu_);
    out_ = fd;
  }

 private:
  bool fill() {
    char buf[4096];
    int64_t n = _framed_io::read_fd(in_, buf, sizeof(buf));
    if (n <= 0) return false;
    inbuf_.append(buf, static_cast<size_t>(n));
    return true;
  }

  int in_;
  int out_;
  std::string inbuf_;
  std::mutex write_mu_;
};

}  // namespace culebra
