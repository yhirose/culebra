// Headless test for the `culebra dap` Debug Adapter Protocol server.
//
// Spawns `culebra dap`, drives scripted DAP sessions over its stdin/stdout
// (Content-Length-framed JSON), and asserts the full feature set: line +
// conditional breakpoints, a named multi-frame call stack with per-frame
// variables / evaluate, setVariable (incl. honouring immutability), forwarding
// the program's stdout as an `output` event, and terminating on continue. No
// JSON library and no culebra linkage — substring checks on the raw DAP stream
// are enough, and the driver waits for the `stopped` event before requesting
// state (the inspection requests must follow the pause).
//
// The same scenarios run against every engine the adapter speaks for (`dap`,
// `dap --vm`): the DAP surface is engine-independent, so a stop, a frame, a
// variable and an evaluated expression must read identically whichever engine
// executed the program.
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

static const char* g_culebra = nullptr;
// The engine flag the adapter is spawned with ("" = the default interpreter).
static const char* g_engine = "";

// Write `src` to a temp file and return its path.
static std::string write_program(const char* name, const char* src) {
  const char* tmpdir = std::getenv("TMPDIR");
  std::string path =
      std::string(tmpdir && *tmpdir ? tmpdir : "/tmp") + "/" + name;
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) fail("cannot write temp program");
  std::fputs(src, f);
  std::fclose(f);
  return path;
}

// Fork a fresh `culebra dap`, wiring its stdio to to_child / from_child and
// resetting the accumulated stream. Each scenario runs against its own server.
static void spawn_adapter() {
  acc.clear();
  int in_pipe[2], out_pipe[2];
  if (::pipe(in_pipe) || ::pipe(out_pipe)) fail("pipe");
  child = ::fork();
  if (child < 0) fail("fork");
  if (child == 0) {
    ::dup2(in_pipe[0], 0);
    ::dup2(out_pipe[1], 1);
    ::close(in_pipe[0]); ::close(in_pipe[1]);
    ::close(out_pipe[0]); ::close(out_pipe[1]);
    if (*g_engine)
      ::execlp(g_culebra, g_culebra, "dap", g_engine, (char*)nullptr);
    else
      ::execlp(g_culebra, g_culebra, "dap", (char*)nullptr);
    _exit(127);
  }
  ::close(in_pipe[0]);
  ::close(out_pipe[1]);
  to_child = in_pipe[1];
  from_child = out_pipe[0];
}

static void disconnect_and_wait() {
  send("{\"type\":\"request\",\"command\":\"disconnect\"}");
  ::close(to_child);
  int status = 0;
  for (int i = 0; i < 20; i++) {
    if (::waitpid(child, &status, WNOHANG) == child) break;
    ::usleep(100000);
  }
}

static std::string S(const std::string& s) { return "\"" + s + "\""; }

// Scenario 1: line breakpoint, single frame, variables, evaluate, setVariable
// (incl. immutability), and live effect on continue.
static void scenario_basic() {
  // Breakpoint goes on line 3 (`let total`); x/y are bound there, total isn't.
  // `x` is mutable so setVariable can change it; `y` stays `let` (immutable).
  std::string path = write_program(
      "culebra_dap_basic.cul",
      "let mut x = 10\nlet y = 20\nlet total = x + y\nIO.inspect(total)\n");
  spawn_adapter();

  send("{\"type\":\"request\",\"command\":\"initialize\"}");
  read_until("\"event\":\"initialized\"");
  send("{\"type\":\"request\",\"command\":\"launch\",\"arguments\":{"
       "\"program\":" + S(path) + ",\"stopOnEntry\":false}}");
  send("{\"type\":\"request\",\"command\":\"setBreakpoints\","
       "\"arguments\":{\"source\":{\"path\":" + S(path) +
       "},\"breakpoints\":[{\"line\":3}]}}");
  send("{\"type\":\"request\",\"command\":\"configurationDone\"}");

  read_until("\"event\":\"stopped\"");
  must_contain("\"reason\":\"breakpoint\"");

  send("{\"type\":\"request\",\"command\":\"stackTrace\","
       "\"arguments\":{\"threadId\":1}}");
  read_until("\"command\":\"stackTrace\"");
  must_contain("\"line\":3");

  send("{\"type\":\"request\",\"command\":\"scopes\","
       "\"arguments\":{\"frameId\":1}}");
  read_until("\"command\":\"scopes\"");
  send("{\"type\":\"request\",\"command\":\"variables\","
       "\"arguments\":{\"variablesReference\":1}}");
  read_until("\"command\":\"variables\"");
  must_contain("\"name\":\"x\"");
  must_contain("\"value\":\"10\"");
  must_contain("\"name\":\"y\"");

  send("{\"type\":\"request\",\"command\":\"evaluate\","
       "\"arguments\":{\"expression\":\"x + y\",\"frameId\":1,"
       "\"context\":\"watch\"}}");
  read_until("\"command\":\"evaluate\"");
  must_contain("\"result\":\"30\"");

  send("{\"type\":\"request\",\"command\":\"setVariable\","
       "\"arguments\":{\"variablesReference\":1,\"name\":\"x\","
       "\"value\":\"99\"}}");
  read_until("\"command\":\"setVariable\"");
  must_contain("\"value\":\"99\"");

  // setVariable on the immutable `let y` is rejected (success=false).
  send("{\"type\":\"request\",\"command\":\"setVariable\","
       "\"arguments\":{\"variablesReference\":1,\"name\":\"y\","
       "\"value\":\"5\"}}");
  read_until("ImmutableError");
  must_contain("\"success\":false");

  send("{\"type\":\"request\",\"command\":\"continue\","
       "\"arguments\":{\"threadId\":1}}");
  read_until("\"event\":\"terminated\"");
  // total = x + y was computed after the edit, so the program prints 119
  // (99 + 20), confirming setVariable changed live execution.
  must_contain("\"event\":\"output\"");
  must_contain("119");
  disconnect_and_wait();
}

// Scenario 2: a named multi-frame call stack with per-frame variables and
// evaluate. Paused inside inner(), called by outer(), called from the top level.
static void scenario_call_stack() {
  std::string path = write_program(
      "culebra_dap_stack.cul",
      "fn inner(n) {\n"        // 1
      "  let z = n + 1\n"      // 2
      "  z\n"                  // 3  <- breakpoint; n + z bound here
      "}\n"                    // 4
      "fn outer(a) {\n"        // 5
      "  let b = inner(a)\n"   // 6
      "  b\n"                  // 7
      "}\n"                    // 8
      "let r = outer(10)\n"    // 9
      "IO.inspect(r)\n");         // 10
  spawn_adapter();

  send("{\"type\":\"request\",\"command\":\"initialize\"}");
  read_until("\"event\":\"initialized\"");
  send("{\"type\":\"request\",\"command\":\"launch\",\"arguments\":{"
       "\"program\":" + S(path) + ",\"stopOnEntry\":false}}");
  send("{\"type\":\"request\",\"command\":\"setBreakpoints\","
       "\"arguments\":{\"source\":{\"path\":" + S(path) +
       "},\"breakpoints\":[{\"line\":3}]}}");
  send("{\"type\":\"request\",\"command\":\"configurationDone\"}");
  read_until("\"event\":\"stopped\"");

  send("{\"type\":\"request\",\"command\":\"stackTrace\","
       "\"arguments\":{\"threadId\":1}}");
  read_until("\"command\":\"stackTrace\"");
  // Three named frames: inner (line 3) <- outer (line 6) <- main (line 9).
  must_contain("\"name\":\"inner\"");
  must_contain("\"name\":\"outer\"");
  must_contain("\"name\":\"main\"");

  // Top frame (inner): n and z are local.
  send("{\"type\":\"request\",\"command\":\"variables\","
       "\"arguments\":{\"variablesReference\":1}}");
  read_until("\"command\":\"variables\"");
  must_contain("\"name\":\"z\"");

  // Frame 2 (outer): its own local `a`, evaluated in that frame, is 10.
  send("{\"type\":\"request\",\"command\":\"evaluate\","
       "\"arguments\":{\"expression\":\"a\",\"frameId\":2,"
       "\"context\":\"watch\"}}");
  read_until("\"command\":\"evaluate\"");
  must_contain("\"result\":\"10\"");
  disconnect_and_wait();
}

// Scenario 3: a conditional breakpoint on the sole statement of a loop body
// (whose STATEMENT wrapper the optimizer collapses) stops only when the
// condition holds.
static void scenario_conditional_bp() {
  std::string path = write_program(
      "culebra_dap_cond.cul",
      "fn run() {\n"           // 1
      "  let mut sum = 0\n"    // 2
      "  for i in 0..5 {\n"    // 3
      "    sum = sum + i\n"    // 4  <- conditional breakpoint (i == 3)
      "  }\n"                  // 5
      "  sum\n"                // 6
      "}\n"                    // 7
      "IO.inspect(run())\n");     // 8
  spawn_adapter();

  send("{\"type\":\"request\",\"command\":\"initialize\"}");
  read_until("\"event\":\"initialized\"");
  send("{\"type\":\"request\",\"command\":\"launch\",\"arguments\":{"
       "\"program\":" + S(path) + ",\"stopOnEntry\":false}}");
  send("{\"type\":\"request\",\"command\":\"setBreakpoints\","
       "\"arguments\":{\"source\":{\"path\":" + S(path) +
       "},\"breakpoints\":[{\"line\":4,\"condition\":\"i == 3\"}]}}");
  send("{\"type\":\"request\",\"command\":\"configurationDone\"}");
  read_until("\"event\":\"stopped\"");

  // The loop only stopped because i reached 3.
  send("{\"type\":\"request\",\"command\":\"evaluate\","
       "\"arguments\":{\"expression\":\"i\",\"frameId\":1,"
       "\"context\":\"watch\"}}");
  read_until("\"command\":\"evaluate\"");
  must_contain("\"result\":\"3\"");

  send("{\"type\":\"request\",\"command\":\"continue\","
       "\"arguments\":{\"threadId\":1}}");
  read_until("\"event\":\"terminated\"");
  must_contain("10");  // sum of 0..4
  disconnect_and_wait();
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: dap_test <culebra>\n");
    return 2;
  }
  g_culebra = argv[1];

  // Every engine the adapter can debug on. The bytecode lane answers from
  // chunk debug tables where the interpreter answers from its environment
  // chain, so running the identical script against both is what keeps the two
  // debuggers reporting the same thing.
  for (const char* engine : {"", "--vm"}) {
    g_engine = engine;
    scenario_basic();
    scenario_call_stack();
    scenario_conditional_bp();
    std::printf("dap_test OK (%s)\n", *engine ? engine : "interp");
  }
  return 0;
}
