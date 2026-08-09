Culebraプログラミング言語
=========================

[![CI](https://github.com/yhirose/culebra/actions/workflows/ci.yml/badge.svg)](https://github.com/yhirose/culebra/actions/workflows/ci.yml)

> **ステータス:** 1.0前・活発に開発中 — APIと構文は変わることがあります。

動的型付けのクロスプラットフォームなスクリプト言語です。backendは
3つ — インタプリタ、LLVM JIT、単体バイナリを生成するAOTビルド。スクリプト、
CLIツール、機械学習、デスクトップアプリ、ゲームが書けます。

標準ライブラリ、テストランナー、linter、formatter、デバッガ、ドキュメント
まで、すべてその1つの実行ファイルに入っています。ほかにインストールする
ものはありません！

Quickstart
----------

macOS（Apple Silicon）のターミナルにこれを貼り付けると、culebraを
ダウンロードし、例を`hello.cul`として書き出し、同じソース1ファイルを
スクリプト・JITコンパイル・単体バイナリの3通りで実行します:

```bash
curl -fsSL https://github.com/yhirose/culebra/releases/latest/download/culebra-macos-arm64.tar.gz | tar xz
export PATH="$PWD/culebra-macos-arm64:$PATH"
culebra --version

cat > hello.cul <<'EOF'
let people = [
  { name: 'Taro', greeting: 'Konnichiwa' },
  { name: 'John', greeting: 'Hello' },
  { name: 'Ada',  greeting: 'Bonjour' },
]

for p in people.sorted_by(|p| p.name) {
  println("{p.greeting}, {p.name}!")
}
EOF

culebra hello.cul                            # インタプリタ
culebra --jit hello.cul                      # JIT
culebra build hello.cul -o hello && ./hello  # AOT: 一度コンパイルしてバイナリを配る
cat hello.cul | culebra -                    # stdin（curl ... | culebra - も同様）
```

任意 — これはエディタの設定（VSCode・Vim・Neovim）も書き換え、
カレントディレクトリに`AGENTS.md`/`CLAUDE.md`も書き出すので、上の
ブロックには含めていません:

```bash
culebra init
```

`culebra init`は、このマシンにあるVSCode・Vim・Neovimのうち見つかった
ものにシンタックスハイライトとデバッグアダプタを導入し、`AGENTS.md`
（既に`CLAUDE.md`か`.github/copilot-instructions.md`があればそちら）に
コーディングエージェント向けの指示を追加します —
実行後に`hello.cul`を開き直せばもうハイライトが効いていますし、
`AGENTS.md`を読むClaude Codeなど他のエージェントも規約を把握済みです。

Linux（x86-64）では1行目を`culebra-linux-x64.tar.gz`に差し替えてください。
Windowsと恒久的なシステム全体へのインストールは以下です。

| プラットフォーム | ダウンロード |
|---|---|
| macOS (Apple Silicon) | [culebra-macos-arm64.tar.gz](https://github.com/yhirose/culebra/releases/latest/download/culebra-macos-arm64.tar.gz) |
| Linux (x86-64) | [culebra-linux-x64.tar.gz](https://github.com/yhirose/culebra/releases/latest/download/culebra-linux-x64.tar.gz) |
| Windows (x86-64) | [culebra-windows-x64.zip](https://github.com/yhirose/culebra/releases/latest/download/culebra-windows-x64.zip) |

いずれも常に最新リリースを指し、どのリリースかは`culebra --version`が
答えます。コマンドラインからの展開なら、Finderで展開したものに
macOSが付ける検疫フラグ（バイナリは署名していません）を避けられます。
このシェルだけでなく恒久的に`culebra`を`PATH`に通すには:

```bash
sudo mv culebra-*/culebra /usr/local/bin/
```

チェックサムと各リリースのノートは
[リリースページ](https://github.com/yhirose/culebra/releases)にあります。

主な特長
--------

### 起動速度・単体バイナリ・クロスプラットフォーム

- **CLIスクリプト。** 起動は数十ミリ秒。
- **単体バイナリ。** `culebra build`が実行ファイルを1つ出力します。並べて
  インストールするものはなく、実行時依存もありません。
- **組み込みライブラリ。** インタプリタをC++ホストに埋め込めます。
- **クロスプラットフォーム。** macOS / Linux / Windows対応。どのホストからでも
  任意のLLVMターゲットへクロスコンパイルできます。

### エージェントのループと相性がよい

起動の速さ、1ファイルで完結するプログラム、最初からスコープにある標準
ライブラリ — 毎ターン呼び出しても割に合う実行環境になります。

- **標準ライブラリにimport行が要らない。** `JSON`・`Http`・`FS`・`Tensor`を
  はじめ、すべてプログラム実行前に束縛済みです。
- **1ファイル1プログラム。** プロジェクト構成なしで走ります。
- **成果物をそのまま渡せる。** エージェントが書いたスクリプトを
  `culebra build`がバイナリに変え、そのままユーザーに渡せます。

### 既定で不変、可変はオプトイン

素の`x = 1`は不変束縛で、再代入はエラーです。変える前提の変数は
`mut x = 1`と書きます。よくあるほうに宣言キーワードは要りません。

### 電池つき、バイナリ1つに

標準ライブラリはプログラム実行前にすべて束縛されます。パッケージ
マネージャもロックファイルもありません:

- **データ。** JSON（JSONCのコメントと末尾カンマも任意で受け付けます）、
  CSV、TOML、`.env`、UUID、そしてSQLite — amalgamationを同梱して
  コンパイルしているので、システムライブラリの導入は不要です。
- **テキスト。** 書記素単位の正規表現、エンコーディング（base64 / hex /
  url / HTMLエンティティ）、SHA / MD5ダイジェストとHMAC、gzip / deflate。
- **システム。** パス操作とファイル一括入出力、ストリーミング用の
  ファイルハンドル、サブプロセス、時刻、数学、乱数、コマンドライン引数の
  解析、レベル付き構造化ログ。
- **ネットワーク。** HTTP/HTTPSのクライアントとサーバー（ルーティング、
  静的ファイル、Server-Sent Events、WebSocket）。その下の層として生の
  TCP / UDPソケットと名前解決も入っています。
- **並行処理。** アイソレート、チャネル、`Parallel`、共有バッファ、そして
  チャネルのメッセージとして届くCtrl+C。
- **端末。** `Term` — 色、カーソル制御、代替画面、TUI向けのキー・マウス
  入力。端末が対応する範囲へ落とし込まれます（`NO_COLOR`下では何も
  出しません）。
- **3D。** `Scene` — 手続き的に構築したジオメトリを物理ベースライティングで
  描く保持モードの3Dレンダラ。オプトイン（`-DCULEBRA_ENABLE_SCENE=ON`）で、
  現状はmacOSのみです。

### 組み込みのTensor

`Tensor`はライブラリではなく言語のプリミティブです。算術は遅延グラフ上の
演算子で、`Tensor.eval`が実体化します。行列積は`.dot()`。演算はCPU
（AVX2 / NEONカーネル、macOSではAccelerate）かGPU（macOSではMetal、
`nvcc`が組み込まれていればCUDA）で走り、`Tensor.use_*()`で固定しない
限り演算ごとにサイズで選ばれます。

```culebra
x = Tensor.from([[1.0, 2.0], [3.0, 4.0]])
y = x.dot(x.transpose())
Tensor.eval(y)  # [[5.0, 11.0], [11.0, 25.0]]
```

### 埋め込みアセット

`Embed.dir(name)`は、ディレクトリ1つをbackendを問わず同じコードで
プログラムに渡します。インタプリタとJITはディスクから直接読み込むので
（ファイルを直して再実行するだけ）、`culebra build`はすべてのバイトを
実行ファイルへ焼き込むので、配布するバイナリはそれだけで完結します。

```culebra
let assets = Embed.dir("dist")         # index.html、favicon.icoなど
println(assets.exists("index.html"))   # => true
println(assets.exists("favicon.ico"))  # => true
```

### デスクトップアプリ作成

Web技術で書くデスクトップGUI。ローカルのHTTPサーバーがUIを供給し、
OS自身のWebViewエンジンが表示し、`culebra build`がサーバー・ルート・
埋め込みアセットをまるごとバイナリ1つとして配ります。

```culebra
Desktop.run({
  title: "Hello from culebra",
  assets: Embed.dir("dist"),  # index.html、favicon.icoなど
  routes: fn (srv) {
    srv.get("/api/hello", fn (req) {
      "hi from the embedded server"
    })
  },
})
```

### 2D Canvas

即時モードの2Dフレームバッファ — 描いて`present`し、入力を読んで、また
描く — スプライト、テキスト、トーン・音楽つき。macOS・Linux・Windowsで
実際のウィンドウを開きます。ヘッドレスを宣言した実行では同じピクセル
処理をして表示だけしないので、テストや画面のないサーバーでも同じ
プログラムが走ります。

```culebra
Canvas.run(160, 160, fn () {
  Canvas.clear(Canvas.rgba(20, 24, 40))
  Canvas.rect(20, 76, 8, 8, Canvas.rgba(220, 60, 60))
  false  # 1フレームで終了
})
```

言語機能
--------

- **パターンマッチ。** リテラル、型ガード、分解、restパターンに加えて、
  任意の`if`ガード。
- **直和型。** `enum Shape { Circle(Float), Rect(Float, Float) }`。必要なら
  ジェネリックにでき、バリアント単位でもenum単位でもマッチできます。
- **タプルとセット。** `(3, 4)`は不変でハッシュ可能、`{1, 2, 3}`は挿入順を
  保つ一意な値の集合です。
- **文字列補間。** `"head={head}, rest={tail.size()}"`。フォーマット指定、
  インデントを落とす三重引用符ブロック、`re"…"`正規表現リテラルも
  使えます。
- **漸進的型付け。** 注釈は境界で実行時に検査されます。Union・Optional・
  Tuple・Trait・Genericはいずれも現時点で検査されます。
- **UFCS。** 自由関数をメソッドとして呼べます（`x.f(y)` ≡ `f(x, y)`）。
- **多重ディスパッチ。** 自由関数が引数の型で解決されます。interp / JIT /
  AOTのすべてで同じ挙動です。
- **トレイト。** 組み込みの`Iterable` / `Iterator`に加え、デフォルト
  メソッドつきのユーザー定義トレイト。
- **クロージャと`class`形式。** どちらも使え、カプセル化の意味論は同じです。
- **ジェネレータ。** 本体に`yield`を含む`fn`はIteratorを返します。
  `yield from`は任意のiterableに委譲します。
- **代数的エフェクト。** `effect fn` / `perform` / `handle … with`と
  マルチショットの`resume`。ジェネレータ、例外、依存性注入、バックトラック
  探索を1つの機構で賄います。
- **デコレータ。** `fn`や`class`の前に置いた`@expr`が、束縛される前の値を
  包みます。
- **コメント内のdoctest。** `1 + 1  # => 2`は`culebra test`で実行される
  アサーションです。
- **構造的なassert診断。** `assert_eq`は差分を表示します。
- **defer / RAII。** スコープに紐づく後始末。
- **スレッドによる並行処理。** `async`/`await`はありません。`Isolate.spawn`は
  クロージャを専用のヒープを持つ専用スレッドで走らせ、値はコピーで境界を
  越え、`Channel`が値を運びます。したがって2つのアイソレートが同じ
  オブジェクトを奪い合うことはありません。必要な場所では
  `SharedBuffer`（固定レイアウト、ゼロコピー）と`Shared`（不変、参照渡し）が
  コピーを回避します。

ツールチェーンは同じバイナリ
----------------------------

以下のサブコマンドはどれも、プログラムを実行するのと同じ実行ファイルの一部
です。

| コマンド | 内容 |
|---|---|
| `culebra test [paths...]` | テストとコメント内のdoctestを実行する |
| `culebra lint [paths...]` | プログラムを走らせずに静的な問題を報告する |
| `culebra fmt [paths...]` | ソースを正準スタイルに整形する |
| `culebra dap` | 標準入出力でDebug Adapter Protocolを話す（VSCode / Vim / Zed） |
| `culebra docs [topic]` | 埋め込みのリファレンスを読む・検索する |
| `culebra build <in.cul> -o <out>` | 事前コンパイルして単体実行ファイルにする |
| `culebra wrap` | 自前のC++クラスを公開する拡張バイナリをビルドする |

詳細は[`docs/tooling.ja.md`](docs/tooling.ja.md)にあります。

単体バイナリ
------------

`culebra build`は`.cul`ソースを事前コンパイルして自己完結した実行ファイルに
します。実行時にLLVMは要りません。プログラムが参照しない約500個のランタイム
ヘルパはツリーシェイキングで落ちます。Tensorを使わないプログラムは
Accelerate / Metalフレームワークへの依存も落とします。

```bash
culebra build path/to/script.cul -o ./out
./out
```

クロスコンパイルにも対応しています。機能軸ごとのバイナリサイズは
[`docs/deployment.ja.md`](docs/deployment.ja.md#1-standalone-バイナリビルドculebra-build)に
あります。

C++ホストへの組み込み
---------------------

インタプリタは（LLVMに依存せず）最小限の環境APIでC++23ホストに
埋め込めます:

```cpp
#include <culebra.h>
#include <stdlib_interp.h>

int main() {
  auto env = culebra::environment();  // 標準ライブラリを束縛済み

  // parse() が返す AST は string_view でこのバッファを参照するので、
  // AST より先に破棄されてはいけない — 一時オブジェクトではなく変数に。
  std::string src = "1 + 2";
  std::vector<std::string> msgs;
  auto ast = culebra::parse("<inline>", src, msgs);

  culebra::Value val;
  culebra::interpret(ast, env, val, msgs, culebra::Debugger());
  // val.to_long() == 3
}
```

JIT経路、スレッド、ホスト関数の登録については
[`docs/deployment.ja.md`](docs/deployment.ja.md#2-c-ホストへの-culebra-埋め込み)を
参照してください。

設計の選択
----------

- **2つのバックエンド、1つのAST。** インタプリタとJITはどちらも維持します。
  一本化する予定はありません。
- **予測可能なスレッド並行処理。** `async`/`await`はありません。スタック
  トレースは読めるままで、デバッガはすべてのフレームを見られ、ライブラリ
  作者が関数ごとに色分けされた版を書かずに済みます。狙いは数千接続あたりを
  上限とするサービス — SQLiteやRedis、そして多くの業務バックエンドが
  すでに採っている形です。理由は
  [`docs/essays/concurrency.ja.md`](docs/essays/concurrency.ja.md)に
  書き下しています。
- **決定的な破棄、`weak`なし。** `drop`プロパティを持つオブジェクトは、
  最後の参照が消えた時点でそれが呼ばれます — スコープが残した循環参照も
  含むので、親への逆参照にweak注釈は要りません。タイミングを駆動するのは
  参照カウントで、循環はトレーシングコレクタが引き取ります。理由は
  [`docs/essays/memory.ja.md`](docs/essays/memory.ja.md)に書き下しています。
- **コンパイル手順なしの漸進的型付け。** 注釈は境界で実行時に検査され、
  起動は即座のままです。Union・Optional・Tuple・Trait・Genericの注釈は
  すべて入っています。
- **既定で不変、宣言キーワードなし。** 素の`x = 1`は*不変*束縛を作り、
  再代入はエラーになります。変える前提の変数には`mut`を使うので
  （`mut x = 1; x = 2`）、`mut`は変わるものだけを示します。`let`は不変の形を
  明示したいときの任意のマーカーです。既定の束縛は安全かつ儀式なしで、
  変更はそれが起きる場所に現れます。
- **パイプラインではなくUFCS。** `x.f(...)`は、ユーザー定義型に対する
  自由関数の解決経路も兼ねます。
- **標準ライブラリにimportは要らない。** 名前空間はプログラム実行前に
  束縛されるので、1ファイルのスクリプトにimportブロックはありません。
  ファイル分割にはトップレベルの`import` / `export`を使い、依存グラフを
  構文解析時に確定させます — AOTのバンドルとツリーシェイキングが必要と
  するものです。
- **電池つき。** 日常のスクリプティングに要るものはサードパーティ
  パッケージではなくバイナリに入っています。
- **1.0前。** リリースはsemverの0.x解釈に従うので、マイナーの更新で壊れる
  ことがあります。パッケージレジストリはまだなく、バイナリを落とすか自分で
  ビルドします。

性能
----

Culebraが狙う性能は3種類あります。起動の速さ、バイナリの小ささ、そして
BLAS級の数値計算です。

- TensorはMNIST MLPでnumpy / Julia / PyTorch CPUと同じBLAS律速の集団に
  入ります。[`benchmarks/mnist/`](benchmarks/mnist/)と
  [`benchmarks/microgpt/`](benchmarks/microgpt/)を参照してください。
- 計算律速のループでは、JITはインタプリタの数十倍速く走ります。
- アロケータ負荷の高いコードではインタプリタが有利なことがあります。

ドキュメント
------------

* Culebraハンドブック: [`docs/handbook.ja.md`](docs/handbook.ja.md)
  / [English](docs/handbook.md)
* 言語仕様: [`docs/language.ja.md`](docs/language.ja.md)
  / [English](docs/language.md)
* 標準ライブラリリファレンス: [`docs/stdlib.ja.md`](docs/stdlib.ja.md)
  / [English](docs/stdlib.md)
* ツール — テストランナー、リンタ、フォーマッタ、デバッグアダプタ、埋め込み
  ドキュメント（`culebra test` / `lint` / `fmt` / `dap` / `docs`）:
  [`docs/tooling.ja.md`](docs/tooling.ja.md)
  / [English](docs/tooling.md)
* デプロイ — 単体バイナリのビルド、C++からの埋め込み、C++ライブラリの
  ラップ（`culebra build` / `culebra wrap`）:
  [`docs/deployment.ja.md`](docs/deployment.ja.md)
  / [English](docs/deployment.md)
* ガイド — culebraで特定のものを作るためのタスク指向how-to集:
  [`docs/guides/`](docs/guides/)
* クイックガイド — 構文、間違えやすい点、標準ライブラリの全シグネチャを
  1ファイルに凝縮したもの。LLMのプロンプト用に書いていますが、人が最初から
  最後まで読み切れる分量でもあります:
  [`docs/quick-guide.ja.md`](docs/quick-guide.ja.md)
  / [English](docs/quick-guide.md)
* エージェント規約 — 同じ出発点をコーディングエージェントが読む形にした
  もの。`CLAUDE.md`・`.github/copilot-instructions.md`・`AGENTS.md`に
  追記して使います（`culebra docs agent`）:
  [`docs/agent.ja.md`](docs/agent.ja.md)
  / [English](docs/agent.md)
* エッセイ — 設計判断に至った経緯を、仕様書には収まらない長さで:
  [`docs/essays/`](docs/essays/)

ソースからのビルドとテストの実行は[`CONTRIBUTING.md`](CONTRIBUTING.md)へ。

ライセンス
----------

[MIT](LICENSE)
