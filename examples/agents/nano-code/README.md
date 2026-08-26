# nano-code

A coding agent in culebra: it reads and writes files inside one directory,
runs a short list of commands, and asks before changing anything. Ported from
[laiso/nano-code](https://github.com/laiso/nano-code) at `80aaba4`, the
companion repository to a book that builds the same agent in TypeScript one
chapter at a time.

The port covers the core of chapters 2 through 6: the message model, the
provider interface, the four tools, the thought loop, the approval gate and
context compression. It follows upstream's structure file by file, so the two
can be read side by side; every place it does not is listed at the end.

## Running it

The workspace is `./workspace` under the working directory, as upstream
resolves it, so run this from the directory holding it.

```bash
cd examples/agents/nano-code

export LLM_PROVIDER=openai
export LLM_MODEL=gpt-5-mini
export LLM_API_KEY=...

culebra nano-code.cul "add a test for Math.wrap and run the tests"
culebra nano-code.cul --yolo "fix the failing test"
```

| Variable | Meaning |
|---|---|
| `LLM_PROVIDER` | required; `openai` or `anthropic` — which format to speak |
| `LLM_MODEL` | required; the model id that provider serves |
| `LLM_API_KEY` | the key, unless the provider's own variable below is set |
| `OPENAI_API_KEY`, `ANTHROPIC_API_KEY` | a provider's own key, which wins |
| `OPENAI_BASE_URL`, `ANTHROPIC_BASE_URL` | where that format is spoken, if not the vendor |

`--yolo` runs the tools that change things without asking. Nothing outside the
workspace can be read or written.

### Through OpenRouter

A provider is a format, not a company: the base URL says where that format is
spoken. One OpenRouter key therefore reaches the same model through either
format, with nothing else changed.

```bash
export LLM_API_KEY=$OPENROUTER_API_KEY
export LLM_MODEL=openai/gpt-oss-20b

LLM_PROVIDER=openai    OPENAI_BASE_URL=https://openrouter.ai/api/v1 \
  culebra nano-code.cul --yolo "create hello.cul and run it"

LLM_PROVIDER=anthropic ANTHROPIC_BASE_URL=https://openrouter.ai/api/v1 \
  culebra nano-code.cul --yolo "create hello.cul and run it"
```

Upstream reaches OpenRouter the same way: its SDKs read those two variables,
and `createOpenAI({ baseURL })` is in its own signature.

## What is here

| | upstream |
|---|---|
| `core/message.cul` | `src/types.ts` — the conversation, one variant per role |
| `core/language_model.cul` | `src/types.ts`, `src/core/generate-text.ts` |
| `core/agent.cul` | `src/core/agent.ts` — the loop, the gate, compression |
| `core/approval.cul` | `src/core/approval.ts` |
| `core/prompt.cul`, `core/prompt.md` | `src/core/prompt.ts`, `src/core/prompt.md` |
| `core/security.cul` | `src/core/security.ts` |
| `core/clean_messages.cul` | the copy each provider carries upstream |
| `core/workspace.cul` | the containment check all four tools share |
| `tools/read_file.cul` … | `src/tools/readFile.ts` … |
| `providers/openai.cul` | `src/providers/openai.ts` |
| `providers/anthropic.cul` | `src/providers/anthropic.ts` |
| `providers/model_factory.cul` | `src/providers/modelFactory.ts` |
| `nano-code.cul` | `bin/cli.ts` |

The tests are ported from upstream's, and all of them run with no API key and
no network:

```bash
culebra test examples/agents/nano-code
```

## Reading the two providers together

The same conversation, said two ways, is the reason both are here. Three
things are spelled differently and nothing else is:

| | `openai.cul` | `anthropic.cul` |
|---|---|---|
| the system prompt | a turn in the array | a field of its own |
| a tool call | a JSON *string* of arguments | a nested object |
| a tool result | its own `tool` role | a user turn of blocks |

A fourth difference is not cosmetic: the Messages format requires
`max_tokens`, so that provider defaults it, and chat-completions does not, so
that one sends nothing unless asked. Making the two agree there would be
making one of them wrong.

## Where this differs from upstream

Language differences aside — a sum type where TypeScript has a union, `match`
where it has a `switch` — these are deliberate:

- **The message model is a real enum.** `match` is exhaustive and `culebra
  lint` names a variant that was not handled, which a `role` string cannot do.
- **`clean_messages` exists once.** Upstream carries a copy inside each
  provider; it depends on nothing a format knows.
- **The guardrails are wired in.** `security.ts` defines `isSensitiveFile`,
  `isDangerousCommand` and `filterEnv`, and upstream's agent calls none of
  them: the only file that imports it is chapter 9's attack demonstration,
  where they are the subject rather than the defence. Here the file tools
  refuse a credential file by name, and `execCommand` refuses a dangerous
  command line and runs with a filtered environment.
- **The workspace is passed in, not read from the process.** Upstream resolves
  `./workspace` from the working directory when each tool module is imported;
  here the tools are made against a workspace, so a test gets a directory of
  its own instead of writing into the one the agent works in. Containment is
  also checked *through* the filesystem, so a symlink out of the workspace is
  refused where upstream's lexical check would allow it.
- **Three things the endpoints do that upstream's SDKs hide.** All were found
  running this against a real endpoint, and without them a run ends in silence:
  - A failure can arrive **inside a 200**: a gateway that routes to a provider
    reports that provider breaking mid-generation in the body, not the status.
    Read as an ordinary turn it looks like a model with nothing left to say.
  - A model that thinks before it answers puts that thinking in a place of its
    own — a `thinking` block in one format, a `reasoning` field in the other —
    and can spend a whole turn there. It is surfaced as `reasoning` and never
    folded into `text`, because it is not what the model said.
  - **The Messages format wants that thinking handed back.** A turn built from
    text and tool calls alone leaves the model looking at a call it does not
    remember making; it thinks the task through again and gives up, which is
    what a run against a thinking model does here. So an assistant turn carries
    `reasoning_blocks` — whatever the provider asked to keep, opaque to
    everything else — and the Messages provider sends them back verbatim, ahead
    of the rest of the turn. Verbatim because they are checked rather than read:
    a block rebuilt from its text alone is rejected. The chat-completions
    provider has nowhere to put them and ignores them.

Not ported: chapter 7 (git and GitHub), chapter 8 (the sandbox and
`webFetch`), appendix A (streaming), appendix B (the Responses API), and the
Google provider, which is a third wire format neither of these is written in
terms of.
