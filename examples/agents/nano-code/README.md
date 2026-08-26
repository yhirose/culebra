# nano-code

A coding agent in culebra: it reads and writes files inside one directory,
runs a short list of commands, and asks before changing anything. Ported from
[laiso/nano-code](https://github.com/laiso/nano-code), the companion
repository to a book that builds the same agent in TypeScript one chapter at a
time.

The port covers the core of chapters 2 through 6: the message model, the
provider interface, the four tools, the thought loop, the approval gate and
context compression.

## Running it

Two things come from the environment: where to connect, and how to speak.

```bash
export OPENROUTER_API_KEY=...        # or ANTHROPIC_API_KEY / OPENAI_API_KEY
export LLM_MODEL=anthropic/claude-sonnet-5   # whatever id that endpoint serves

culebra nano-code.cul "add a test for Math.wrap and run the tests"
culebra nano-code.cul                # no task: ask one at a time
culebra nano-code.cul --help
```

| Variable | Meaning |
|---|---|
| `LLM_MODEL` | required; the model id the endpoint serves |
| `OPENROUTER_API_KEY` / `ANTHROPIC_API_KEY` / `OPENAI_API_KEY` | the first one set picks the endpoint |
| `LLM_WIRE` | `anthropic` (default) or `openai` |
| `LLM_ENDPOINT` | `openrouter` / `anthropic` / `openai`, to override the choice above |
| `LLM_BASE_URL`, `LLM_API_KEY` | point the same wire at something else (a proxy, a local server) |

`--yes` runs the tools that change things without asking. `--workspace <dir>`
picks the directory the agent may touch; the default is `workspace/` beside
this file, and nothing outside it can be read or written.

## What is here

| | |
|---|---|
| `core/message.cul` | the conversation as a sum type, one variant per role |
| `core/language_model.cul` | the one call a provider must answer |
| `core/agent.cul` | the loop: ask, run what it asked for, ask again |
| `core/approval.cul` | the gate in front of a tool that changes something |
| `core/prompt.cul`, `core/prompt.md` | the instructions, plus the workspace's own |
| `core/workspace.cul` | the directory the agent is confined to |
| `core/security.cul` | credential files, dangerous command lines, the child's environment |
| `core/clean_messages.cul` | repairing the call/result pairing before a history goes out |
| `tools/` | readFile, writeFile, editFile, execCommand, and the schema they advertise |
| `providers/` | the two wire formats, and the table that says where to point them |
| `test_*.cul` | `culebra test` from this directory runs all of them |

Everything runs with no API key: the model is an interface, so the tests drive
the whole loop with a scripted one, and `test_round_trip.cul` puts a real
socket in the middle by talking to a local server.

## What the port changed

Same behaviour, different shape in a few places, and each of them is a note in
the file that does it.

- **A message is an `enum`, not a `role` string.** `match` reads the variant,
  and `culebra lint` names a variant no arm handles, which a string cannot do.
- **The guardrails are wired.** Upstream defines `isSensitiveFile`,
  `isDangerousCommand` and `safeEnv` in one chapter and connects them in a
  later one that this port leaves out; here the tools consult them.
- **A tool declares its parameters, not its JSON Schema.** The schema is
  derived, which is most of what each upstream tool file contained.
- **The confinement lives in one class**, and checks through the filesystem as
  well as lexically, so a symlink inside the workspace cannot point out of it.
  Upstream copies the lexical half into three tools and gives `execCommand`
  the weaker version.
- **`clean_messages` exists once.** Upstream carries a copy in each provider.
- **Where to connect and how to speak are separate.** An endpoint is a base URL
  and its auth headers; a wire is the format. One OpenRouter key therefore
  reaches the same model through either wire, which is what makes the two
  comparable.
- **The agent is silent unless asked.** `generate` reports what happened in its
  return value, and the conversation belongs to the agent rather than to one
  call, which is what makes the follow-up question in the prompt mean
  something.

## What is not ported

The git and GitHub tools (chapter 7), the sandbox and `webFetch` (chapter 8),
streaming (appendix A), the OpenAI Responses API (appendix B), and the Google
provider. The agent runs commands without a shell and with a whitelisted
environment, which is a different answer to chapter 8's question, not the same
one.
