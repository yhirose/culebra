#pragma once

// A yes/no question to the person at the terminal, asked before doing
// something they did not type: installing a toolchain (`culebra build`),
// fetching a model (`Search.segmenter`). Depends on nothing else of culebra's
// but the isatty shim, so both the CLI and the runtime can ask.

#include <cstdio>
#include <string>
#include <string_view>

#include <base/os_compat.h>  // os_isatty

namespace culebra {

// The controlling terminal for reading, or nullptr where there is none.
inline std::FILE* open_controlling_tty() {
#if defined(__EMSCRIPTEN__)
  // The Playground reports itself interactive (see os_isatty) but has no
  // terminal to open, and nothing there to install or fetch.
  return nullptr;
#elif defined(_WIN32)
  return std::fopen("CONIN$", "r");
#else
  return std::fopen("/dev/tty", "r");
#endif
}

// Ask `question`, answering false unless a person types yes. Both streams
// have to be a terminal: stdin so an answer can arrive, stderr so the
// question is seen (stdout may be a pipe the program is feeding). A script,
// a CI job or a pipe therefore gets "no" without blocking, and the caller
// prints what it would have done instead.
//
// The answer is read from the terminal itself, not from stdin: a program
// that asks from inside the runtime (Search.segmenter, in a user's program)
// cannot assume stdin is its own to consume a line of, so the question
// opens the controlling terminal the way `sudo` and `ssh` read a passphrase.
inline bool confirm_on_tty(std::string_view question) {
  if (!os_isatty(0) || !os_isatty(2)) return false;
  std::FILE* tty = open_controlling_tty();
  if (!tty) return false;
  std::fprintf(stderr, "%.*s [y/N] ", int(question.size()), question.data());
  std::fflush(stderr);
  char buf[64];
  std::string answer;
  if (std::fgets(buf, sizeof buf, tty)) answer = buf;
  std::fclose(tty);
  while (!answer.empty() && (answer.back() == '\n' || answer.back() == '\r'))
    answer.pop_back();
  return answer == "y" || answer == "Y" || answer == "yes";
}

}  // namespace culebra
