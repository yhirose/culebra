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

```culebra
let people = [
  { name: 'Taro', age: 30 },
  { name: 'John', age: 45 },
  { name: 'Ada',  age: 36 },
]

for p in people.sorted_by(|p| p.age) {
  println("{p.name} is {p.age} years old")
}
```

どれも同じソース1ファイルから、プロジェクト構成もコンパイル手順もなしに
行えます:

```bash
culebra script.cul                        # スクリプトとして実行（インタプリタ）
culebra --jit script.cul                  # JITコンパイル
culebra build script.cul -o app && ./app  # 単体バイナリとして配る
```

ダウンロード
------------

いずれも常に最新リリースを指します。各アーカイブにはバイナリとライセンスが
入っており、どのリリースかは`culebra --version`が答えます。

| プラットフォーム | ダウンロード |
|---|---|
| macOS (Apple Silicon) | [culebra-macos-arm64.tar.gz](https://github.com/yhirose/culebra/releases/latest/download/culebra-macos-arm64.tar.gz) |
| Linux (x86-64) | [culebra-linux-x64.tar.gz](https://github.com/yhirose/culebra/releases/latest/download/culebra-linux-x64.tar.gz) |
| Windows (x86-64) | [culebra-windows-x64.zip](https://github.com/yhirose/culebra/releases/latest/download/culebra-windows-x64.zip) |

バイナリは署名していないため、Finderで展開したものにはmacOSが検疫フラグを
付けます。コマンドラインからの展開ならそれを避けられます:

```bash
curl -fsSL https://github.com/yhirose/culebra/releases/latest/download/culebra-macos-arm64.tar.gz | tar xz
sudo mv culebra-*/culebra /usr/local/bin/
culebra --version
```

チェックサムと各リリースのノートは
[リリースページ](https://github.com/yhirose/culebra/releases)にあります。

主な特長
--------

### 小さく、速く、どこでも動く

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

### 端末、ウィンドウ、ゲーム

スクリプトを走らせるのと同じバイナリが描画もします:

- **`Term`。** 色、カーソル制御、代替画面、TUI向けのキー・マウス入力。色は
  端末が対応する範囲へ落とし込まれます（`NO_COLOR`下では何も出しません）。
- **`Canvas`。** 即時モードの2Dフレームバッファ — 描いて`present`し、入力を
  読んで、また描く — スプライト、テキスト、トーン・音楽つき。macOS・Linux・
  Windowsで実際のウィンドウを開きます。ヘッドレスを宣言した実行では同じ
  ピクセル処理をして表示だけしないので、テストや画面のないサーバーでも
  同じプログラムが走ります。
- **`Desktop` / `Webview`。** Web技術で書くデスクトップGUI。ローカルの
  HTTPサーバーがUIを供給し、OS自身のWebViewエンジンが表示し、全体が
  バイナリ1つとして配れます。
- **`Scene`。** 手続き的に構築したジオメトリを物理ベースライティングで描く
  保持モードの3Dレンダラ。オプトイン（`-DCULEBRA_ENABLE_SCENE=ON`）で、
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
Tensor.eval(y)                       # [[5.0, 11.0], [11.0, 25.0]]
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

  std::vector<std::string> msgs;
  auto ast = culebra::parse("<inline>", "1 + 2", 5, msgs);

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

* Culebraガイド: [`docs/guide.ja.md`](docs/guide.ja.md)
  / [English](docs/guide.md)
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
* コンテキストパック — 構文、間違えやすい点、標準ライブラリの全シグネチャを
  1ファイルに凝縮したもの。LLMのプロンプトに貼って使います:
  [`docs/llm.ja.md`](docs/llm.ja.md)
  / [English](docs/llm.md)
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
