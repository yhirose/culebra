# nano-code-mini

A culebra port of [1rgs/nanocode](https://github.com/1rgs/nanocode)
(MIT-spirited, single-file Python, ~270 lines) — a minimal Claude Code
alternative: one REPL loop that calls the Anthropic Messages API, executes
whatever tools the model asks for, and feeds the results back until the
model stops asking. Tools: `read`, `write`, `edit`, `glob`, `grep`, `bash`.

What differs from the original: `bash` captures the whole process via
`Proc.run` instead of streaming output lines live as they're produced, and
stdout/stderr are captured on separate pipes rather than interleaved in
real time (see `nano-code-mini.cul`'s header comment).

This is the first of two planned nano-code ports. It exercises the same
core primitives (blocking HTTP + JSON, subprocess, regex, file I/O) that a
larger port of [laiso/nano-code](https://github.com/laiso/nano-code) — the
~1500-line, multi-provider, multi-file TypeScript/Bun agent from the book
*作って学ぶAIエージェント* — will need, without requiring streaming,
approval flows, or a provider abstraction layer.

## Run

```sh
export ANTHROPIC_API_KEY="..."      # or OPENROUTER_API_KEY (any OpenRouter model)
culebra examples/agents/nano-code-mini/nano-code-mini.cul
culebra --jit examples/agents/nano-code-mini/nano-code-mini.cul
```

Commands inside the REPL: `/c` clears the conversation, `/q` or `exit` quits.

```sh
export OPENROUTER_API_KEY="..."
export MODEL="openai/gpt-5.2"       # any OpenRouter model id
culebra examples/agents/nano-code-mini/nano-code-mini.cul
```
