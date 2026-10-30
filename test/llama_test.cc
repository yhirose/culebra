// stdlib/llama.h: the value-neutral model handle over cpp-llamalib. What needs no
// model runs always; with CULEBRA_TEST_MODEL set to a GGUF file the rest runs
// too (a small instruct model such as Gemma 3 1B is enough). The output is
// fixed by a grammar wherever it can be, so the checks do not depend on what a
// model happens to say.
#include <cstdio>
#include <cstdlib>
#include <string>

#include <stdlib/llama.h>

namespace L = culebra::llama;

static int failures = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                         \
    }                                                                     \
  } while (0)

static L::GenerateOptions fixed(const std::string& text, int max_tokens = 64) {
  L::GenerateOptions o;
  o.max_tokens = max_tokens;
  o.grammar = "root ::= \"" + text + "\"";
  return o;
}

static void without_a_model() {
  std::string err;
  CHECK(L::load("/nonexistent/model.gguf", {}, &err) == -1);
  CHECK(err.find("Failed to load model") != std::string::npos);
  CHECK(err.find("No such file") != std::string::npos);

  // A handle that was never opened, or is closed, is an error and not a crash.
  for (int64_t id : {int64_t(-1), int64_t(0), int64_t(0x100000007)}) {
    L::Result r;
    err.clear();
    CHECK(!L::generate(id, "hi", {}, nullptr, &r, &err));
    CHECK(err == "the model is closed");
    err.clear();
    CHECK(!L::chat(id, {{"user", "hi"}}, {}, nullptr, &r, &err));
    CHECK(err == "the model is closed");
    std::string text;
    err.clear();
    CHECK(!L::format_chat(id, {{"user", "hi"}}, &text, &err));
    CHECK(err == "the model is closed");
    int64_t n = 0;
    err.clear();
    CHECK(!L::count_tokens(id, "hi", &n, &err));
    CHECK(err == "the model is closed");
    CHECK(L::n_ctx(id) == 0);
    L::close(id);  // ignored
  }
}

static void with_a_model(const char* path) {
  std::string err;
  L::LoadOptions lo;
  lo.n_ctx = 512;
  lo.gpu = false;
  int64_t id = L::load(path, lo, &err);
  CHECK(id >= 0);
  if (id < 0) {
    std::fprintf(stderr, "load: %s\n", err.c_str());
    return;
  }
  CHECK(L::n_ctx(id) == 512);

  L::Result r;
  CHECK(L::generate(id, "Say ok", fixed("ok"), nullptr, &r, &err));
  CHECK(r.text == "ok");
  CHECK(r.finish == L::Finish::End);
  CHECK(r.prompt_tokens > 0);
  CHECK(r.completion_tokens >= 1);

  // The stream and the result agree.
  std::string streamed;
  CHECK(L::generate(id, "Count", fixed("one two three"),
                    [&](std::string_view p) { streamed.append(p); return true; }, &r, &err));
  CHECK(r.text == "one two three");
  CHECK(streamed == r.text);

  // A stop sequence ends it and is not in the text.
  auto stop = fixed("one two three");
  stop.stop = {"two"};
  CHECK(L::generate(id, "Count", stop, nullptr, &r, &err));
  CHECK(r.finish == L::Finish::Stop);
  CHECK(r.text == "one ");

  // A callback that says stop ends it and is not called again.
  int calls = 0;
  CHECK(L::generate(id, "Count", fixed("one two three four"),
                    [&](std::string_view) { ++calls; return false; }, &r, &err));
  CHECK(r.finish == L::Finish::Aborted);
  CHECK(calls == 1);

  // max_tokens ran out.
  L::GenerateOptions few;
  few.max_tokens = 3;
  CHECK(L::generate(id, "Tell me a story", few, nullptr, &r, &err));
  CHECK(r.finish == L::Finish::Length);
  CHECK(r.completion_tokens == 3);

  int64_t n = 0;
  CHECK(L::count_tokens(id, "Hello there", &n, &err));
  CHECK(n > 0);

  // Chat goes through the model's own template. A model without one (the tiny
  // test models have none) is refused with that said, never answered from the
  // messages joined together; one with a template is checked on what it makes.
  std::string prompt;
  if (L::format_chat(id, {{"user", "Hello"}}, &prompt, &err)) {
    CHECK(prompt.find("Hello") != std::string::npos);
    CHECK(prompt != "Hello");
    CHECK(L::chat(id, {{"user", "Say ok"}}, fixed("ok"), nullptr, &r, &err));
    CHECK(r.text == "ok");

    // Without the cache, the same call repeats.
    L::GenerateOptions greedy;
    greedy.max_tokens = 16;
    greedy.temperature = 0.0f;
    L::Result a, b;
    CHECK(L::chat(id, {{"system", "Answer in one word."}, {"user", "Hello"}}, greedy,
                  nullptr, &a, &err));
    CHECK(L::chat(id, {{"system", "Answer in one word."}, {"user", "Hello"}}, greedy,
                  nullptr, &b, &err));
    CHECK(a.text == b.text);
  } else {
    CHECK(err.find("chat template") != std::string::npos);
    std::string chat_err;
    CHECK(!L::chat(id, {{"user", "Hello"}}, {}, nullptr, &r, &chat_err));
    CHECK(chat_err == err);
  }

  // A prompt that does not fit is an error, with the library's wording.
  std::string long_prompt;
  for (int i = 0; i < 700; i++) long_prompt += "word ";
  err.clear();
  CHECK(!L::generate(id, long_prompt, {}, nullptr, &r, &err));
  CHECK(err.find("Prompt too long") != std::string::npos);

  L::close(id);
  err.clear();
  CHECK(!L::generate(id, "hi", {}, nullptr, &r, &err));
  CHECK(err == "the model is closed");
  L::close(id);  // twice is fine
}

int main() {
  without_a_model();
  if (const char* path = std::getenv("CULEBRA_TEST_MODEL"); path && *path) {
    with_a_model(path);
  } else {
    std::printf("llama_test: CULEBRA_TEST_MODEL not set, the model part is skipped\n");
  }
  if (failures) {
    std::fprintf(stderr, "llama_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("llama_test OK\n");
  return 0;
}
