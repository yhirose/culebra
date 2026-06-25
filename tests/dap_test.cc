// Headless test for the `culebra dap` Debug Adapter Protocol server.
//
// Spawns `culebra dap`, drives a scripted DAP session over its stdin/stdout
// (Content-Length-framed JSON), and asserts: it stops at a line breakpoint,
// reports the frame + in-scope variables, forwards the program's stdout as an
// `output` event, and terminates on continue. No JSON library and no culebra
// linkage — substring checks on the raw DAP stream are enough, and the driver
// waits for the `stopped` event before requesting state (the inspection
// requests must follow the pause).
//
// Usage: dap_test <path-to-culebra>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int to_child = -1, from_child = -1;
static pid_t child = -1;
static std::string acc;  // accumulated DAP stream from the child

static void fail(const char* what) {
  std::fprintf(stderr, "dap_test FAIL: %s\nstream so far:\n%s\n", what,
               acc.c_str());
  if (child > 0) ::kill(child, SIGKILL);
  std::exit(1);
}

static void send(const std::string& json) {
  std::string frame =
      "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json;
  size_t off = 0;
  while (off < frame.size()) {
    ssize_t n = ::write(to_child, frame.data() + off, frame.size() - off);
    if (n <= 0) fail("write to adapter");
    off += static_cast<size_t>(n);
  }
}

// Read from the child until `needle` appears in the accumulated stream, or a
// ~10s budget elapses.
static void read_until(const char* needle) {
  if (acc.find(needle) != std::string::npos) return;
  for (int waited = 0; waited < 10000;) {
    struct pollfd p{from_child, POLLIN, 0};
    int r = ::poll(&p, 1, 500);
    if (r < 0) fail("poll");
    if (r == 0) {
      waited += 500;
      continue;
    }
    char buf[4096];
    ssize_t n = ::read(from_child, buf, sizeof(buf));
    if (n <= 0) fail("adapter closed the stream early");
    acc.append(buf, static_cast<size_t>(n));
    if (acc.find(needle) != std::string::npos) return;
  }
  fail((std::string("timed out waiting for: ") + needle).c_str());
}

static void must_contain(const char* needle) {
  if (acc.find(needle) == std::string::npos)
    fail((std::string("missing in stream: ") + needle).c_str());
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: dap_test <culebra>\n");
    return 2;
  }
  const char* culebra = argv[1];

  // Write the program to debug.
  const char* tmpdir = std::getenv("TMPDIR");
  std::string path = std::string(tmpdir && *tmpdir ? tmpdir : "/tmp") +
                     "/culebra_dap_test.cul";
  {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) fail("cannot write temp program");
    // Breakpoint goes on line 3 (`let total`); x/y are bound there, total isn't.
    // `x` is mutable so setVariable can change it; `y` stays `let` (immutable).
    std::fputs("let mut x = 10\nlet y = 20\nlet total = x + y\nIO.puts(total)\n",
               f);
    std::fclose(f);
  }

  int in_pipe[2], out_pipe[2];
  if (::pipe(in_pipe) || ::pipe(out_pipe)) fail("pipe");
  child = ::fork();
  if (child < 0) fail("fork");
  if (child == 0) {
    ::dup2(in_pipe[0], 0);
    ::dup2(out_pipe[1], 1);
    ::close(in_pipe[0]); ::close(in_pipe[1]);
    ::close(out_pipe[0]); ::close(out_pipe[1]);
    ::execlp(culebra, culebra, "dap", (char*)nullptr);
    _exit(127);
  }
  ::close(in_pipe[0]);
  ::close(out_pipe[1]);
  to_child = in_pipe[1];
  from_child = out_pipe[0];

  std::string q = "\"";  // shorthand
  auto S = [&](const std::string& s) { return q + s + q; };

  send("{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\"}");
  read_until("\"event\":\"initialized\"");

  send("{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{"
       "\"program\":" + S(path) + ",\"stopOnEntry\":false}}");
  send("{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\","
       "\"arguments\":{\"source\":{\"path\":" + S(path) +
       "},\"breakpoints\":[{\"line\":3}]}}");
  send("{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");

  read_until("\"event\":\"stopped\"");
  must_contain("\"reason\":\"breakpoint\"");

  send("{\"seq\":5,\"type\":\"request\",\"command\":\"stackTrace\","
       "\"arguments\":{\"threadId\":1}}");
  read_until("\"command\":\"stackTrace\"");
  must_contain("\"line\":3");

  send("{\"seq\":6,\"type\":\"request\",\"command\":\"scopes\","
       "\"arguments\":{\"frameId\":1}}");
  read_until("\"command\":\"scopes\"");
  send("{\"seq\":7,\"type\":\"request\",\"command\":\"variables\","
       "\"arguments\":{\"variablesReference\":1}}");
  read_until("\"command\":\"variables\"");
  // x and y are in scope at the breakpoint; total is not yet bound.
  must_contain("\"name\":\"x\"");
  must_contain("\"value\":\"10\"");
  must_contain("\"name\":\"y\"");

  // evaluate: an expression in the frame's scope.
  send("{\"seq\":8,\"type\":\"request\",\"command\":\"evaluate\","
       "\"arguments\":{\"expression\":\"x + y\",\"frameId\":1,"
       "\"context\":\"watch\"}}");
  read_until("\"command\":\"evaluate\"");
  must_contain("\"result\":\"30\"");

  // setVariable: change the mutable x to 99 (affects the live program).
  send("{\"seq\":9,\"type\":\"request\",\"command\":\"setVariable\","
       "\"arguments\":{\"variablesReference\":1,\"name\":\"x\","
       "\"value\":\"99\"}}");
  read_until("\"command\":\"setVariable\"");
  must_contain("\"value\":\"99\"");

  // setVariable on the immutable `let y` is rejected (success=false).
  send("{\"seq\":10,\"type\":\"request\",\"command\":\"setVariable\","
       "\"arguments\":{\"variablesReference\":1,\"name\":\"y\","
       "\"value\":\"5\"}}");
  read_until("ImmutableError");
  must_contain("\"success\":false");

  send("{\"seq\":11,\"type\":\"request\",\"command\":\"continue\","
       "\"arguments\":{\"threadId\":1}}");
  read_until("\"event\":\"terminated\"");
  // total = x + y was computed after the edit, so the program prints 119
  // (99 + 20), confirming setVariable changed live execution.
  must_contain("\"event\":\"output\"");
  must_contain("119");

  send("{\"seq\":12,\"type\":\"request\",\"command\":\"disconnect\"}");
  ::close(to_child);

  int status = 0;
  for (int i = 0; i < 20; i++) {
    if (::waitpid(child, &status, WNOHANG) == child) break;
    ::usleep(100000);
  }
  std::printf("dap_test OK\n");
  return 0;
}
