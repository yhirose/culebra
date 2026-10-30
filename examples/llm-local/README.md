# LLM.local examples

Programs that use a language model run in this process (`Llama` and
`LLM.local`): no server, no API key, no network. They need a build configured
with `-DCULEBRA_ENABLE_LLAMA=ON` and a GGUF chat model. Each takes the model's
path as its first argument.

| Program | What it does |
|---|---|
| `translate.cul` | Translates text given as an argument or on standard input. |
| `chat.cul` | A conversation in the terminal; the answer is printed as it is generated. |
| `summarize.cul` | Summarizes a file after counting its tokens against the model's context. |
| `ask.cul` | One question to a local model or, without a model path, to Anthropic's API. |
| `classify.cul` | Holds the answer to a fixed list of labels with a GBNF grammar. |

```
culebra translate.cul model.gguf Japanese "Good morning"
culebra chat.cul model.gguf
culebra summarize.cul model.gguf notes.txt
culebra ask.cul "Name three prime numbers." model.gguf
culebra classify.cul model.gguf "The battery died after two days."
```

Translation quality depends on the model: in a trial, a 4B-parameter model
translated English and Japanese sentences well, and a 1B one once left a
sentence untranslated.
