# Working with language models

A walkthrough of calling a language model from culebra: a hosted one such as
Claude over its API, and one run in the culebra process itself from a GGUF
file, with no server, no key and no network. Both go through the same `LLM`
namespace, so a program written against one runs against the other by changing
one line. The local half uses the `Llama` namespace, which the full downloads
carry (on the GPU through Vulkan on Linux and Windows and through Metal on
macOS) and the slim downloads do not. [`examples/llm-local/`](../../examples/llm-local/)
has five finished programs built from the pieces here.

This guide covers *using* `LLM` and `Llama`. For the API reference see
[`stdlib.md` §42](../stdlib.md#42-llm) and [§43](../stdlib.md#43-llama).

Contents
--------

- Part 1 — the walkthrough
  1. [One question](#1-one-question)
  2. [The provider is a value](#2-the-provider-is-a-value)
  3. [A model on this machine](#3-a-model-on-this-machine)
  4. [A conversation](#4-a-conversation)
  5. [Tools](#5-tools)
  6. [A loop of your own](#6-a-loop-of-your-own)
  7. [Shipping one binary](#7-shipping-one-binary)
- Part 2 — recipes
  - [Testing without a model](#testing-without-a-model)
  - [Holding a local answer to a shape](#holding-a-local-answer-to-a-shape)
  - [Does it fit the context?](#does-it-fit-the-context)
  - [Errors worth catching](#errors-worth-catching)
- [Where to go next](#where-to-go-next)

## Part 1 — the walkthrough

### 1. One question

`LLM.ask` sends one prompt and returns the text of the answer:

```culebra
# doctest: skip — needs ANTHROPIC_API_KEY and the network
let claude = LLM.anthropic()
IO.println(LLM.ask("Name three prime numbers.", provider: claude, model: "claude-haiku-5-5"))
```

`LLM.anthropic()` reads the key from `ANTHROPIC_API_KEY` and fails at once,
before any request, when there is none. Every call names its model: model IDs
change and differ in price, so a hosted provider has no default. `system:` sets
the system prompt, `max_tokens:` caps the answer, and `on_text:` streams it:

```culebra
# doctest: skip — needs ANTHROPIC_API_KEY and the network
LLM.ask("Write a haiku about rain.", provider: LLM.anthropic(), model: "claude-haiku-5-5",
        on_text: |piece| IO.print(piece))
```

The function is called with each piece as it arrives, and `ask` still returns
the whole text.

### 2. The provider is a value

What `provider:` takes is an ordinary value, made once and passed around. The
ones that come with culebra:

| Provider | Talks to |
|---|---|
| `LLM.anthropic()` | Anthropic's Messages API |
| `LLM.openai()`, `LLM.openrouter()` | OpenAI, OpenRouter (Chat Completions) |
| `LLM.ollama()` | an Ollama server on this machine |
| `LLM.openai_compatible(base_url:)` | any server that speaks Chat Completions, such as llama-server |
| `LLM.local(model)` | a model loaded into this process with `Llama.load` |

Everything after the provider is the same code, so the choice can be made at
run time:

```culebra
# doctest: skip — needs a model file or ANTHROPIC_API_KEY
let path = Sys.env("CULEBRA_MODEL")  # '' when it is not set
let provider = path.empty() ? LLM.anthropic() : LLM.local(Llama.load(path))
let model = path.empty() ? "claude-haiku-5-5" : nil  # a local provider has its own
IO.println(LLM.ask("Name three prime numbers.", provider: provider, model: model))
```

A provider is also something you can write: any value with a
`turn(request, on_text)` method. [Testing without a model](#testing-without-a-model)
uses that.

### 3. A model on this machine

`Llama.load` reads a GGUF file, the format llama.cpp uses; chat models in it are
published on Hugging Face under names ending in `-GGUF`. A 4-billion-parameter
model quantized to 4 bits (`Q4_K_M`) is about 2.5 GB and translates and
summarizes well; a 1B one is about 0.8 GB and is enough for short answers.

```culebra
# doctest: skip — needs a GGUF model file
let model = Llama.load("gemma-3-4b-it-Q4_K_M.gguf", n_ctx: 4096)
let local = LLM.local(model)
IO.println(LLM.ask("Name three prime numbers.", provider: local, max_tokens: 30))
```

`n_ctx` is how many tokens the model can see at once, the prompt and the answer
together. The provider has the model it was given, so `model:` is left out.

On Linux and Windows the model goes to the GPU when the machine has a Vulkan
driver and a GPU it can use; otherwise it runs on the CPU, several times slower. `model.device()`
says which, and why:

```culebra
# doctest: skip — needs a GGUF model file
let d = model.device()
if d.reason != nil {
  IO.eprintln("running on the CPU: {d.reason}")
}
```

The [`device()` table](../stdlib.md#modeldevice---object) lists what each
`reason` asks you to install. The first run on a machine spends a few seconds
while the driver compiles the GPU code; later runs do not.

A local provider takes no tools, and the model needs a chat template of its own
(a model published for chat has one). The rest of this walkthrough works with
either kind of provider, except [Tools](#5-tools) and the loop in
[§6](#6-a-loop-of-your-own), which runs them.

### 4. A conversation

`LLM.chat` keeps the history, so each question is asked in the light of the ones
before:

```culebra
# doctest: skip — needs a GGUF model file
let chat = LLM.chat(provider: local, system: "Answer in one short sentence.")
IO.println(chat.ask("What is the capital of France?"))
IO.println(chat.ask("And of Japan?"))  # "And" is read against the first question
```

`chat.messages` is the history, an `Array` of `{role, content}`: here
`user`, `assistant`, `user`, `assistant` (the system prompt is kept apart).
It can be assigned between two asks, which is how a long conversation stays
inside the context: drop the oldest turns, keep the rest. `chat.last` is the
last reply, with its `stop` reason and its `usage` in tokens.

A local provider keeps the model's cache from one turn to the next, so each ask
decodes only what is new in the history.

### 5. Tools

A tool is a culebra function the model may ask to have run. `LLM.tool` makes one
from a function, and reads the schema the model sees off the parameters'
annotations:

```culebra
# doctest: skip — needs ANTHROPIC_API_KEY and the network
fn weather(city: String) {
  "{city}: sunny, 18°C"
}

let chat = LLM.chat(
  provider: LLM.anthropic(),
  model: "claude-haiku-5-5",
  tools: [LLM.tool(weather, "Current weather in a city", city: "a city name")],
)
IO.println(chat.ask("What is the weather in Tokyo?"))
```

When the answer asks for `weather`, `ask` calls `weather(city: "Tokyo")`, adds
what it returned to the history and asks again, until the model answers in text
(at most `max_turns` round trips, 10 unless given). What a tool throws goes back
to the model as an error it can act on.

A tool that changes something, such as a file or a payment, should not run on
the model's word alone. `approve:` is asked about each call first; `true` lets it
run, and `false` or a `String` saying why refuses it and tells the model so:

```culebra
# doctest: skip — needs ANTHROPIC_API_KEY and the network
fn delete_file(path: String) {
  FS.remove(path)
  "deleted {path}"
}

let chat = LLM.chat(
  provider: LLM.anthropic(),
  model: "claude-haiku-5-5",
  tools: [LLM.tool(delete_file, "Delete one file")],
  approve: fn (call) {
    IO.print("run {call.name}({call.args})? [y/N] ")
    IO.input() == "y" ? true : "the user said no"
  },
)
```

The tool is a function of your own around `FS.remove` rather than `FS.remove`
itself: the model can then name one file, and has no `recursive:` to pass.

### 6. A loop of your own

`LLM.chat` is a loop over two smaller pieces you can use directly: `LLM.turn`
asks once and does not run tools, and `LLM.run_tools` runs the ones a reply asked
for and returns the `tool` message to add. A loop of your own is the place for
what `chat` does not do: logging each step, a budget, a different stopping
rule.

```culebra
# doctest: skip — needs ANTHROPIC_API_KEY and the network
let claude = LLM.anthropic()
let tools = [LLM.tool(weather, "Current weather in a city")]
let history = [{role: "user", content: "What is the weather in Tokyo?"}]
mut last = nil
while true {
  let reply = LLM.turn(claude, model: "claude-haiku-5-5", messages: history, tools: tools)
  last = reply
  history.push(reply.message)
  break if reply.tool_calls.empty()
  IO.eprintln("running {reply.tool_calls.map(|c| c.name).join(", ")}")
  history.push(LLM.run_tools(tools, reply.tool_calls))
}
IO.println(last.text)
```

`last.text` is the answer: the message's first part is not always its text,
since a model that thinks puts its thinking there. `reply.message` is kept
exactly as the provider gave it, which matters for such a model: its thinking
blocks are checked when they go back.

### 7. Shipping one binary

`culebra build` turns a program into one executable. A program that names
`Llama` links llama.cpp into it statically, with the Vulkan backend when the
culebra that builds it has one (about 49 MB); `culebra build --llama-cpu` leaves
the backend out (about 6.5 MB) for a binary that will only ever run on the CPU.
The model file stays outside the binary, so it is passed as an argument or read
from a known place. A program that does not name `Llama` carries none of it,
and a program that only talks to a hosted model needs nothing beyond `Http`.

## Part 2 — recipes

### Testing without a model

A provider is any value with a `turn(request, on_text)` method, so a test can
use one that follows a script: no model, no network, the same answer every run.
Each reply is an Object with the fields `LLM.turn` documents.

```culebra
fn scripted(replies) {
  mut i = 0
  {
    name: "scripted",
    capabilities: {tools: true},
    default_model: "script",
    turn: fn (request, on_text) {
      let r = replies[i]
      i += 1
      r
    },
  }
}

fn says(text) {
  {message: {role: "assistant", content: [{type: "text", text: text}]}, text: text,
   tool_calls: [], reasoning: "", stop: "end", usage: nil, raw: nil}
}

fn calls(name, args) {
  let part = {type: "tool_call", id: "c1", name: name, args: args}
  {message: {role: "assistant", content: [part]}, text: "",
   tool_calls: [{id: "c1", name: name, args: args}], reasoning: "", stop: "tool_calls",
   usage: nil, raw: nil}
}

fn weather(city: String) {
  "{city}: sunny, 18°C"
}

let provider = scripted([calls("weather", {city: "Tokyo"}), says("It is sunny in Tokyo.")])
let chat = LLM.chat(provider: provider, tools: [LLM.tool(weather, "Current weather in a city")])
inspect(chat.ask("What is the weather in Tokyo?"))  # => 'It is sunny in Tokyo.'
inspect(chat.messages.map(|m| m.role))  # => ['user', 'assistant', 'tool', 'assistant']
inspect(chat.messages[2].content[0].content)  # => 'Tokyo: sunny, 18°C'
```

The test checks what the program around the model does: that the tool ran with
the arguments the model gave, that its result went back, and that the final
answer is the one returned. `capabilities: {tools: true}` says the stand-in
takes tools; without it a request with tools is refused before `turn` is called,
as it is for `LLM.local`.

### Holding a local answer to a shape

A small model asked to pick a label may answer with a sentence. A loaded model
takes a GBNF grammar, and its output then follows it whatever the model would
have said; `temperature: 0` makes it take the likeliest choice every time:

```culebra
# doctest: skip — needs a GGUF model file
let labels = ["positive", "negative", "neutral"]
let grammar = "root ::= " + labels.map(|l| "\"{l}\"").join(" | ")
let r = model.chat([
  {role: "system", content: "Classify the sentiment of the user's text as one word."},
  {role: "user", content: "The battery died after two days."},
], grammar: grammar, temperature: 0)
IO.println(r.text)  # negative
```

This is the model's own `chat`, not `LLM`'s: `LLM.local` does not pass a grammar.

### Does it fit the context?

`LLM.count_tokens` counts what a turn would send. On a local provider it is
exact, counted by the model's own tokenizer, so a program can check before it
asks:

```culebra
# doctest: skip — needs a GGUF model file
let messages = [{role: "user", content: "Summarize:\n\n" + FS.read("notes.txt")}]
let room = 400  # tokens kept free for the answer
if LLM.count_tokens(local, messages: messages) + room > model.n_ctx() {
  IO.eprintln("notes.txt does not fit; load the model with a larger n_ctx")
  Sys.exit(1)
}
```

On `LLM.anthropic` it is a request to the API; the Chat Completions providers
cannot count (an `LLMError`). A history cut by dropping old turns can leave a tool result whose
call is gone, which a provider refuses; `LLM.repair(messages)` takes such
leftovers out.

### Errors worth catching

| Error | When |
|---|---|
| `LLMError` | no key; the API answered with an error status; the model refused; no answer within `max_turns`; something the provider does not take (tools on `LLM.local`) |
| `HttpError` | the network: DNS, a refused connection, TLS |
| `LlamaError` | the model file cannot be loaded; the prompt does not fit the context; the model has no chat template |
| `Interrupted` | Ctrl+C during a generation or a tool; let it go up |

```culebra
# doctest: skip — needs ANTHROPIC_API_KEY and the network
try {
  IO.println(LLM.ask("Hello", provider: LLM.anthropic(), model: "claude-haiku-5-5"))
} catch e: LLMError {
  IO.eprintln("the model service said: {e.message}")
} catch e: HttpError {
  IO.eprintln("could not reach it: {e.message}")
}
```

## Where to go next

- [`stdlib.md` §42 `LLM`](../stdlib.md#42-llm): every argument, the message
  format, writing a provider, the reply's fields.
- [`stdlib.md` §43 `Llama`](../stdlib.md#43-llama): `generate` without a chat
  template, sampling, the cache, `device()`.
- [`examples/llm-local/`](../../examples/llm-local/): translate, chat,
  summarize, ask and classify, as programs that take a model path.
