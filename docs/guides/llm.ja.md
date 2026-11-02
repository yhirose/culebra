# 言語モデルを使う

culebraから言語モデルを呼ぶ手順を、順に追います。相手は2種類あります。APIで呼ぶClaudeのような
ホスト型のモデルと、GGUFファイルからculebraのプロセスの中で動かすモデルです。後者はサーバーも、
キーも、ネットワークも要りません。どちらも同じ`LLM`名前空間を通るので、片方に向けて書いたプログラムは
1行変えるだけでもう片方でも動きます。手元で動かすほうは`Llama`名前空間を使います。全部入り版の
ダウンロードには入っていて（LinuxとWindowsではVulkanでGPUを使い、macOSではCPUで動きます）、
スリム版には入っていません。ここで扱う部品から組んだ5つのプログラムが
[`examples/llm-local/`](../../examples/llm-local/)にあります。

このガイドは`LLM`と`Llama`の*使い方*を扱います。APIのリファレンスは
[`stdlib.ja.md` §42](../stdlib.ja.md#42-llm)と[§43](../stdlib.ja.md#43-llama)を見てください。

目次
----

- Part 1 — 手順
  1. [1つの質問](#1-1つの質問)
  2. [providerは値](#2-providerは値)
  3. [このマシンで動くモデル](#3-このマシンで動くモデル)
  4. [会話](#4-会話)
  5. [道具（tools）](#5-道具tools)
  6. [自分で回すループ](#6-自分で回すループ)
  7. [1つのバイナリで配る](#7-1つのバイナリで配る)
- Part 2 — レシピ
  - [モデルなしでテストする](#モデルなしでテストする)
  - [手元のモデルの答えを形に収める](#手元のモデルの答えを形に収める)
  - [コンテキストに収まるか](#コンテキストに収まるか)
  - [捕まえる価値のあるエラー](#捕まえる価値のあるエラー)
- [この先](#この先)

## Part 1 — 手順

### 1. 1つの質問

`LLM.ask`はプロンプトを1つ送り、答えのテキストを返します。

```culebra
# doctest: skip — ANTHROPIC_API_KEYとネットワークが要る
let claude = LLM.anthropic()
IO.println(LLM.ask("Name three prime numbers.", provider: claude, model: "claude-haiku-5-5"))
```

`LLM.anthropic()`はキーを`ANTHROPIC_API_KEY`から読み、無ければリクエストを送る前にすぐ失敗します。
呼び出しのたびにモデルを指定します。モデルのIDは変わりますし、値段も違うので、ホスト型のproviderには
既定のモデルがありません。`system:`でシステムプロンプトを、`max_tokens:`で答えの上限を決め、
`on_text:`で答えを少しずつ受け取れます。

```culebra
# doctest: skip — ANTHROPIC_API_KEYとネットワークが要る
LLM.ask("Write a haiku about rain.", provider: LLM.anthropic(), model: "claude-haiku-5-5",
        on_text: |piece| IO.print(piece))
```

関数は届いた断片ごとに呼ばれ、`ask`はそれでも全体のテキストを返します。

### 2. providerは値

`provider:`に渡すのは普通の値で、1回作って使い回します。culebraに付いてくるものは次のとおりです。

| provider | 相手 |
|---|---|
| `LLM.anthropic()` | AnthropicのMessages API |
| `LLM.openai()`、`LLM.openrouter()` | OpenAI、OpenRouter（Chat Completions） |
| `LLM.ollama()` | このマシンのOllamaサーバー |
| `LLM.openai_compatible(base_url:)` | Chat Completionsを話すサーバーなら何でも（llama-serverなど） |
| `LLM.local(model)` | `Llama.load`でこのプロセスに読み込んだモデル |

providerから後ろは同じコードなので、どれを使うかは実行時に決められます。

```culebra
# doctest: skip — モデルのファイルかANTHROPIC_API_KEYが要る
let path = Sys.env("CULEBRA_MODEL")  # 設定されていなければ''
let provider = path.empty() ? LLM.anthropic() : LLM.local(Llama.load(path))
let model = path.empty() ? "claude-haiku-5-5" : nil  # 手元のproviderは自分のモデルを持つ
IO.println(LLM.ask("Name three prime numbers.", provider: provider, model: model))
```

providerは自分で書くこともできます。`turn(request, on_text)`メソッドを持つ値なら何でもproviderです。
[モデルなしでテストする](#モデルなしでテストする)でこれを使います。

### 3. このマシンで動くモデル

`Llama.load`はllama.cppの形式であるGGUFのファイルを読み込みます。この形式のチャット用モデルは、
Hugging Faceで名前が`-GGUF`で終わるものとして公開されています。40億パラメータのモデルを4ビットに
量子化したもの（`Q4_K_M`）は約2.5 GBで、翻訳や要約をよくこなします。10億パラメータのものは約0.8 GBで、
短い答えなら足ります。

```culebra
# doctest: skip — GGUFのモデルファイルが要る
let model = Llama.load("gemma-3-4b-it-Q4_K_M.gguf", n_ctx: 4096)
let local = LLM.local(model)
IO.println(LLM.ask("Name three prime numbers.", provider: local, max_tokens: 30))
```

`n_ctx`は、モデルが一度に見られるトークン数で、プロンプトと答えの合計です。providerは渡されたモデルを
持っているので、`model:`は書きません。

LinuxとWindowsでは、マシンにVulkanのドライバと使えるGPUがあればモデルはGPUに載り、無ければCPUで動きます
（数倍遅くなります）。どちらで動いているか、その理由は`model.device()`が答えます。

```culebra
# doctest: skip — GGUFのモデルファイルが要る
let d = model.device()
if d.reason != nil {
  IO.eprintln("running on the CPU: {d.reason}")
}
```

`reason`ごとに何を入れればよいかは、[`device()`の表](../stdlib.ja.md#modeldevice---object)にあります。
そのマシンでの最初の実行では、ドライバがGPU用のコードをコンパイルするので数秒かかります。2回目からはかかりません。

手元のproviderは道具を受け取りません。また、モデルが自分のチャットテンプレートを持っている必要があります
（チャット用として公開されたモデルなら持っています）。この手順の残りは、[道具](#5-道具tools)と、道具を動かす
[§6](#6-自分で回すループ)を除いて、どちらのproviderでも動きます。

### 4. 会話

`LLM.chat`は履歴を保つので、それぞれの質問は前の質問を踏まえて尋ねられます。

```culebra
# doctest: skip — GGUFのモデルファイルが要る
let chat = LLM.chat(provider: local, system: "Answer in one short sentence.")
IO.println(chat.ask("What is the capital of France?"))
IO.println(chat.ask("And of Japan?"))  # 「And」は最初の質問を受けて読まれる
```

`chat.messages`が履歴で、`{role, content}`の`Array`です。ここでは`user`、`assistant`、`user`、
`assistant`の順に並びます（システムプロンプトは別に持ちます）。2回の`ask`の間に代入できるので、
長い会話をコンテキストに収めるには、古いターンを落として残りを代入します。`chat.last`は最後の返答で、
止まった理由の`stop`と、トークン数の`usage`を持ちます。

手元のproviderは、モデルのキャッシュをターンからターンへ持ち越すので、`ask`のたびに履歴の新しい部分だけを
デコードします。

### 5. 道具（tools）

道具は、モデルが実行を頼めるculebraの関数です。`LLM.tool`は関数から道具を作り、モデルに見せるスキーマを
引数の型注釈から読み取ります。

```culebra
# doctest: skip — ANTHROPIC_API_KEYとネットワークが要る
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

答えが`weather`を求めると、`ask`は`weather(city: "Tokyo")`を呼び、返った値を履歴に足してもう一度尋ねます。
モデルがテキストで答えるまで続けます（往復は`max_turns`まで。指定しなければ10）。道具が投げたものは、
モデルが対処できるエラーとしてモデルに返ります。

ファイルや支払いのように何かを変える道具は、モデルの言葉だけで動かすべきではありません。`approve:`は
それぞれの呼び出しの前に尋ねられ、`true`なら実行し、`false`か理由を書いた`String`なら実行せずにそのことを
モデルに伝えます。

```culebra
# doctest: skip — ANTHROPIC_API_KEYとネットワークが要る
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

道具を`FS.remove`そのものではなく、それを包んだ自前の関数にしているのは、モデルがファイルを1つだけ
指定でき、`recursive:`を渡せないようにするためです。

### 6. 自分で回すループ

`LLM.chat`は、直接使える2つの小さな部品のループです。`LLM.turn`は1回だけ尋ねて道具は実行せず、
`LLM.run_tools`は返答が求めた道具を実行して、履歴に足す`tool`メッセージを返します。自分で回すループは、
`chat`がしないことを書く場所です。各手順の記録、予算、別の止め方などです。

```culebra
# doctest: skip — ANTHROPIC_API_KEYとネットワークが要る
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

答えは`last.text`です。考えるモデルはメッセージの最初の部分に思考を置くので、最初の部分がテキストとは
限りません。`reply.message`はproviderが返したそのままの形で残ります。そうしたモデルではこれが効きます。
その思考のブロックは、送り返すときに検査されるからです。

### 7. 1つのバイナリで配る

`culebra build`はプログラムを1つの実行ファイルにします。`Llama`を名指しするプログラムには、llama.cppが
静的にリンクされます。ビルドするculebraがVulkanのバックエンドを持っていれば、それも入ります（全体で約49 MB）。
CPUでしか動かさないバイナリなら、`culebra build --llama-cpu`でバックエンドを外せます（約6.5 MB）。
モデルのファイルはバイナリの外に置くので、引数で渡すか、決まった場所から読みます。`Llama`を名指ししない
プログラムには何も入りません。ホスト型のモデルとだけ話すプログラムには、`Http`より多くは要りません。

## Part 2 — レシピ

### モデルなしでテストする

providerは`turn(request, on_text)`メソッドを持つ値なら何でもよいので、テストでは台本どおりに答える
providerを使えます。モデルもネットワークも要らず、毎回同じ答えが返ります。それぞれの返答は、
`LLM.turn`が説明しているフィールドを持つObjectです。

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

このテストが確かめるのは、モデルのまわりのプログラムの動きです。モデルが渡した引数で道具が動いたこと、
その結果が戻ったこと、最後の答えが返ったことです。`capabilities: {tools: true}`は、この代役が道具を
受け取ると言っています。これが無いと、`LLM.local`のときと同じく、道具つきのリクエストは`turn`が呼ばれる前に
断られます。

### 手元のモデルの答えを形に収める

小さなモデルにラベルを選ばせると、文で答えることがあります。読み込んだモデルはGBNFの文法を受け取り、
出力はモデルが何を言おうとしていたかにかかわらず、その文法に従います。`temperature: 0`にすると、
毎回いちばんありそうな選択肢を選びます。

```culebra
# doctest: skip — GGUFのモデルファイルが要る
let labels = ["positive", "negative", "neutral"]
let grammar = "root ::= " + labels.map(|l| "\"{l}\"").join(" | ")
let r = model.chat([
  {role: "system", content: "Classify the sentiment of the user's text as one word."},
  {role: "user", content: "The battery died after two days."},
], grammar: grammar, temperature: 0)
IO.println(r.text)  # negative
```

これは`LLM`ではなく、モデル自身の`chat`です。`LLM.local`は文法を渡しません。

### コンテキストに収まるか

`LLM.count_tokens`は、1回のターンで送るものを数えます。手元のproviderではモデル自身のトークナイザで
数えるので正確です。尋ねる前に確かめられます。

```culebra
# doctest: skip — GGUFのモデルファイルが要る
let messages = [{role: "user", content: "Summarize:\n\n" + FS.read("notes.txt")}]
let room = 400  # 答えのために空けておくトークン数
if LLM.count_tokens(local, messages: messages) + room > model.n_ctx() {
  IO.eprintln("notes.txt does not fit; load the model with a larger n_ctx")
  Sys.exit(1)
}
```

`LLM.anthropic`ではAPIへのリクエストになり、Chat Completionsのproviderは数えられません（`LLMError`）。古いターンを
落として切り詰めた履歴には、呼び出しが消えた道具の結果が残ることがあり、providerはそれを受け付けません。
`LLM.repair(messages)`がそうした残りを取り除きます。

### 捕まえる価値のあるエラー

| エラー | いつ |
|---|---|
| `LLMError` | キーが無い、APIがエラーのステータスを返した、モデルが答えを断った、`max_turns`のうちに答えが無い、providerが受け取らないもの（`LLM.local`への道具） |
| `HttpError` | ネットワーク（DNS、接続の拒否、TLS） |
| `LlamaError` | モデルのファイルを読み込めない、プロンプトがコンテキストに収まらない、モデルにチャットテンプレートが無い |
| `Interrupted` | 生成中や道具の実行中のCtrl+C。そのまま上に通す |

```culebra
# doctest: skip — ANTHROPIC_API_KEYとネットワークが要る
try {
  IO.println(LLM.ask("Hello", provider: LLM.anthropic(), model: "claude-haiku-5-5"))
} catch e: LLMError {
  IO.eprintln("the model service said: {e.message}")
} catch e: HttpError {
  IO.eprintln("could not reach it: {e.message}")
}
```

## この先

- [`stdlib.ja.md` §42 `LLM`](../stdlib.ja.md#42-llm): すべての引数、メッセージの形式、providerの書き方、
  返答のフィールド。
- [`stdlib.ja.md` §43 `Llama`](../stdlib.ja.md#43-llama): チャットテンプレートなしの`generate`、
  サンプリング、キャッシュ、`device()`。
- [`examples/llm-local/`](../../examples/llm-local/): 翻訳、チャット、要約、質問、分類を、モデルの
  パスを受け取るプログラムとして。
