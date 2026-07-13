Culebra 内部構造
================

本ドキュメントは、ユーザー向けガイドに対する開発者向けの姉妹編で
す。*どのように* (実装戦略、ライブラリ選択、内部データ構造) を記録
します。検討したが不採用にした設計は、ここではなく
[`record.ja.md`](record.ja.md) にまとめてあります。すべてのドキュメ
ントはバイリンガルであり、このファイルの英語原本は
[`internals.md`](internals.md) で、両者は常に同期を保つ必要がありま
す。

目次
----

1. [アーキテクチャ概観](#1-アーキテクチャ概観)
2. [パーサ (cpp-peglib)](#2-パーサ-cpp-peglib)
3. [インタプリタ](#3-インタプリタ)
4. [JIT (LLVM ORC)](#4-jit-llvm-orc)
5. [AOT codegen](#5-aot-codegen)
6. [文字列 / Unicode](#6-文字列--unicode)
7. [Regex](#7-regex)
8. [Tensor](#8-tensor)
9. [HTTP](#9-http)
10. [モジュールシステム](#10-モジュールシステム)
11. [ビルドと vendor](#11-ビルドと-vendor)
12. [テストランナー](#12-テストランナー)

不採用・撤回した設計は [`record.ja.md`](record.ja.md) に集約してあり
ます。

---

1. アーキテクチャ概観
---------------------

1 つの AST、3 つの実行経路。同じパーサがすべてに供給します。

```
       .cul source
            │
            ▼
   cpp-peglib parser   ─►   AST (shared_ptr nodes)
            │
   ┌────────┼────────────────────┐
   ▼        ▼                    ▼
Interpreter  JIT (LLVM ORC)      AOT codegen
(tree walk)  (in-process)        (LLVM .o → linker → exe)
```

このレイアウトの理由:

- **AST の共有**により、バックエンド間で意味論が同一に保たれます。
  新しい言語機能はまず AST + インタプリタに実装され、その後 JIT と
  AOT 経路が追随します ([[project_dual_backend_policy]])。
- **bytecode 層は無し。** インタプリタは AST ノードを直接ウォークし、
  JIT は AST を LLVM IR へ lower します。bytecode の中間層は、この規
  模では計測可能な利点なしに複雑さを増すため不採用です。
- **ドライババイナリ `culebra`** が CLI です。フラグ (`--jit`、
  `build`、既定のインタプリタ) に応じて 3 経路のいずれも実行できま
  す。

ヘッダのルート:

- `include/ast.h` — AST ノード階層。
- `include/interp.h` — ツリーウォーキング型インタプリタ。
- `include/jit.h` — ORC JIT (`CULEBRA_ENABLE_JIT` のときのみコンパイ
  ル)。
- `include/aot.h` — LLVM `.o` への AOT コンパイル、システム cc 経由で
  リンク。
- `include/runtime/` — JIT と AOT から見えるランタイムヘルパ。

2. パーサ (cpp-peglib)
----------------------

文法は `include/grammar.h` に単一の PEG 仕様として置かれ、
`peg::parser` に供給されます。cpp-peglib が提供するもの:

- PEG の意味論 (greedy、左再帰は禁止ルール)。
- 各文法プロダクションが semantic action を通じて AST ビルダに対応。
- ソース位置がすべての AST ノードに自動的に伝播される。

cpp-peglib を選んだ理由 (手書きの再帰下降との比較):

- 文法が 1 ファイルにまとまり、`language.ja.md` の隣で仕様として読め
  る。
- semantic action が構文に近い位置に留まり、lexer/parser の二分法に何
  も漏れ出さない。
- 性能はボトルネックになっていない — パース時間はパーサではなくイン
  タプリタ/JIT のコンパイルが支配的。

AST ノードはパースコールバックから `shared_ptr` として構築されま
す。識別子解決はパース後のパスへ遅延され、パーサは文脈自由でいられ
ます。

`ParseError` は `file`、`line`、`col`、および短いメッセージを保持し、
CLI ドライバが `clang` 風の固定幅スタイルで整形します。

3. インタプリタ
---------------

### Value レイアウト

`Value` は 8 つの組み込み型に対する tagged union (実質的には
`std::variant`) です。4 つの参照型 (`String`、`Array`、`Object`、
`Function`) は `shared_ptr` を介して boxing され、4 つのスカラーはイ
ンラインです。

参照カウントは `shared_ptr` の既定に従います。相互参照を保持する
Object を掃除する小さなサイクル GC があり、スケジュールされるのではな
く日和見的に起動されます — ほとんどのプログラムはサイクルを生成しな
いためです。

### スコープ

`Environment` は連結されたフレームです。各フレームは文字列 → Value の
マップと、外側フレームへの親ポインタを保持します。クロージャキャプチ
ャは「フレームを閉じ込める」方式で、クロージャは
`shared_ptr<Environment>` を保持し、外側の関数がリターンした後もフレ
ームを生存させ続けます。

### shared_from_this の寿命管理

`Interpreter` は、呼び出し元より長く生存する必要がある 4 つの長寿命ラ
ムダ (ランタイム値にインストールされるコールバック) を所有します。こ
れらのラムダは `[self = shared_from_this(), ...]` をキャプチャするた
め、escape したコールバックが 1 つでも生存する限りインタプリタオブジ
ェクト自身が生存し続けます。このコードベースで新規のランタイムコール
バックラムダは、必ず同じパターンに従わなければなりません。元となった
インシデントは [[project_interpreter_lifetime]] を参照。

### エラー伝播

`throw` は、投げられた Value とソース位置を保持する
`culebra::ThrowSignal` (C++ 例外) を送出します。`try`/`catch`/`defer`
は、`BlockStmt`、`TryStmt`、`DeferStmt` の AST ノードをウォークする
visitor によって実装され、フレームごとの defer スタックと、あらゆる離
脱経路での LIFO 実行を維持します。

4. JIT (LLVM ORC)
-----------------

JIT は AST を関数粒度で LLVM IR へ lower し (スクリプトのトップレベル
はモジュール全体を lower)、そのモジュールを ORC v2 に渡して `-O2` コン
パイルさせます。ホストプロセス内のシンボル (ランタイムヘルパ、アロケ
ータ、BLAS) は、ORC の `DefinitionGenerator` 機構を介して JIT 済みコー
ドに公開されます。

### ランタイムシンボル解決

ランタイムヘルパ (`culebra_runtime_*`) はリンク時に可視です。macOS で
は既定で可視ですが、ELF/Linux では `-rdynamic` (CMake の
`ENABLE_EXPORTS` プロパティ) が必要です。関連する Linux の
PIE/-fPIC の経緯は [[project_aot_no_pie]] を参照。

### インラインキャッシュ

プロパティアクセスとメソッドディスパッチは、呼び出し箇所ごとの IC を
使います。最初のルックアップで Object レイアウトをウォークし、解決さ
れた offset (または Function ポインタ) を `JITCallSite` スロットに書き
込みます。以降の呼び出しは、レシーバの shape が一致すればスロットを介
して fast path を通ります。ミスすると slow path に戻り、スロットを更
新します。

### namespace ディスパッチテーブル

`include/stdlib_jit.h` は `kNsMethods` を公開します。これは
`(namespace, method)` から JIT 呼び出し可能なランタイム関数ポインタへ
のテーブルです。裸の namespace メソッド (`Math.abs`、`IO.puts`) は
codegen 時にここで引かれ、汎用ルックアップのオーバーヘッドを回避しま
す。新しい stdlib メソッドを追加するには、ここに 1 行と対応するインタ
プリタ実装が必要です。[[project_jit_namespace_dispatch]] と
`add-stdlib-namespace` skill を参照。

起動時の debug 専用ドリフトチェックが、`kNsMethods` の全メソッドがイ
ンタプリタテーブルに存在することを検証し、2 つのバックエンドがサイレ
ントに乖離するのを防ぎます ([[feedback_check_jit_interp_symmetry]])。

### HOF fusion

`range(n).filter(p).map(f).take(k).collect()` は fusable なチェーンと
して認識され、`p` と `f` を inline した単一のカウンタループへ lower さ
れます。パターンはパーサ実行後の AST shape 上でマッチされ、JIT はその
後 IR でタイトなループを emit します。インタプリタは fusion しませ
ん。そのイテレータチェーン実装も lazy ですが、ステージごとに小さな
ラッパを確保します。

5. AOT codegen
--------------

`culebra build foo.cul -o foo` は、モジュールグラフ (Ch.10) をウォー
クし、到達可能な各トップレベルを LLVM IR へ lower し、non-PIC な `.o`
を emit し、そのオブジェクトとランタイムアーカイブを最終リンクのため
にシステム C++ コンパイラへ渡します。

### tree-shaking

モジュールグラフと AST が合わさって、到達可能なトップレベル名の集合を
与えます。ランタイムヘルパ (~200 個) は機能グループごとに分割され、
ユーザープログラムから静的に参照されるグループのみがリンクされます。
`puts` を使う "hello world" は IO と Long プリンタを引き込み、それ以外
は何も引き込みません。

### ランタイムアーカイブ (base + 機能別)

- `libculebra_rt.a` — base ランタイム。`Tensor` / `Http` / `Compress`
  の choke は **weak-symbol stub** なので、単体では BLAS、OpenSSL、
  zlib のいずれも参照しません。
- `libculebra_rt_tensor.a` / `libculebra_rt_http.a` /
  `libculebra_rt_compress.a` — 各機能の strong choke (それぞれ BLAS /
  OpenSSL+zlib / zlib を引き込む)。

`culebra build` は常に base をリンクし、AST がその namespace
(`Tensor` / `Http` / `Compress`) を参照したときのみ機能アーカイブを
**force-load** します — strong choke が weak stub を上書きします。し
たがって未使用の機能は、そのアーカイブも外部ライブラリもリンクしませ
ん。これは 2^N のマトリクスではなく N+1 のアーカイブです。`culebra
wrap` アーカイブ (`libculebra_rt_wrap.a`) も同じ使用ゲートに乗りま
す。

### Linux -no-pie

LLVM の `TargetMachine` は non-PIC な `.o` を emit します。Ubuntu の
`gcc` は既定で PIE リンクを行い、"failed to set dynamic section
sizes" で失敗します。ドライバは Linux でリンカに `-no-pie` を渡してこ
れを解消します。診断の連鎖は [[project_aot_no_pie]] を参照。

### クロスコンパイル

`--target=<triple>` + ユーザー提供の `--sysroot=` と `--rt-lib=`。
LLVM の `AllTargets*` コンポーネントがホストの `culebra` ドライバにリ
ンクされているため、任意の LLVM サポート triple 向けに emit できま
す。ランタイムアーカイブ自体はターゲット向けにビルドされている必要が
あります — バンドルされた sysroot はまだありません
([[project_binary_build_roadmap]] Phase E MVP)。

6. 文字列 / Unicode
-------------------

### 現状

文字列は内部的には `std::string` (UTF-8) です。バイトインデックスは
`std::string::operator[]`、スカラー反復は cpp-unicodelib の `utf8`
namespace を使ってコードポイントを 1 つずつウォークします。

`split`、`replace`、`trim` などのメソッドは既定でバイトレベルで動作し
ます。`length()` はスカラー数を、`size()` はバイト数を返します。この
区別は `guide.ja.md` §4.2 に記載されています。

### JIT/AOT 表現: インライン長さヘッダ

インタプリタの `std::string` は自身の長さを保持するため、埋め込まれた
NUL は普通のバイトです。JIT/AOT バックエンドはそれに一致させなければ
なりませんが、`JitValue` は 1 つの `{tag, i64}` スロットです — 長さは
値に載せられないため、heap/`.rodata` オブジェクトに存在します。
`TAG_STRING` の payload は、長さ前置バッファのバイト列を指します
([[project_jit_string_repr]]):

```
[ uint64_t len ][ bytes... ][ '\0' ]
                 ^ TAG_STRING data points here; len at data[-8].
```

これは BSTR / Zig の sentinel-slice / CPython の形です。長さが正典であ
り O(1) で読める (`_str_len`) ため、埋め込み NUL は普通のバイトです。
末尾の NUL は保持され、NUL を持たない文字列でもそのまま C API (パス、
`%s`) に渡せます。`{ptr, len}` ディスクリプタは不採用でした — hot
path に間接参照を追加し、借用名に対して surface ごとの確保が発生し、
`TAG_STRINGVIEW` のレイアウトと衝突するためです。

不変条件: すべての `TAG_STRING` はヘッダ付きです。生産者は
`_culebra_heap_str` (ランタイム) と `emit_str_literal` (`.rodata` の
`ConstantStruct`) のみです。リテラル文字列と heap 文字列は 1 つのレイ
アウトを共有するため、読み手は起源で分岐しません。String 値として現れ
る借用 shape 名 (object キー、`class` / variant / enum 名) は
`_intern_str` を通してヘッダを得ます。`_str_len` の debug `assert`
が、`TAG_STRING` に誤タグ付けされたヘッダなしポインタを捕捉します。文
字列をサイクル GC に畳み込むこと (現状はリークする) が、このヘッダの
自然な次の用途です。

### 計画中: StringView、StringLike、lazy graphemes

[[project_string_model]] が決定した内容:

- **`StringView`** は `std::string_view` に似た型です — 既存の
  `String` のバイトに対する借用で、同じバイト/スカラー API を持ちま
  す。寿命の危険を避けるため、パラメータ専用 (Object に格納不可) で
  す。
- **`StringLike`** はマルチメソッドのディスパッチタグ (`guide.ja.md` の
  Ch.10) です。`StringLike` に対して定義された関数は、コピーなしで
  `String` と `StringView` の両呼び出し元を拾います。
- **`graphemes()`** は、cpp-unicodelib の grapheme break テーブルを使
  って、書記素クラスタ (UAX #29) に対する lazy なイテレータを返しま
  す。

実装順序: `StringView` (interp + JIT) → `StringLike` マルチメソッドフ
ック (Ch.10 のディスパッチ IC に依存) → `graphemes()`。

7. Regex
--------

> ステータス: 計画中 ([[project_regex_self_hosted]])。

### ライブラリ選択

自前ホストの `cpp-regexlib` が計画です。まずこのリポジトリの
`include/regex/` 内で regex エンジンを育て、API が安定したらスタンドア
ロンの `yhirose/cpp-regexlib` ライブラリへ分離します。RE2 も検討しま
したが、次の理由で見送りました:

- 1 つの stdlib namespace のために Google プロジェクトを vendor ツリ
  ーへ加えるのは重い依存になる。
- 既定で書記素クラスタマッチングを望んでいる。それを RE2 に後付けする
  のは、自前エンジンを書くより多くのコードになる。

### エンジンモデル

- NFA ベースで線形時間保証 (catastrophic backtracking なし)。
- `.` と文字クラスの単位を書記素クラスタとし、`String.graphemes()`
  (Ch.6) と grapheme break テーブルを共有。
- MVP では PCRE 互換のサブセット。lookaround と backreference は先送り
  — これらは線形時間を壊し、ほとんどのユーザーコードは必要としませ
  ん。

### surface area

`Regex.compile(pattern)` はコンパイル済み regex オブジェクトを返しま
す。`re.match(s)` / `re.find_all(s)` / `re.replace(s, repl)`。マッチ結
果はバイトオフセット、スカラーオフセット、キャプチャグループを保持し
ます。

8. Tensor
---------

### TNode

`Tensor` は `shared_ptr<TNode>` です。`TNode` が保持するもの:

- `shape: vector<int64_t>`
- `strides: vector<int64_t>`
- `data: shared_ptr<float[]>` (F32)
- `offset: int64_t`

ビュー (transpose、slice、broadcast) は同じ `data` を shape/stride/
offset を調整して再利用するため、ほとんどの操作は zero-copy です。

### BLAS ルーティング

要素ごとの演算は、autovectorization ヒント付きの小さなインラインルー
プを使います。Matmul (`@`) は、必要なら連続バッファをレイアウトした後
に `cblas_sgemm` を経由します。BLAS プロバイダはプラットフォーム依存で
す:

- macOS: Apple Accelerate (既定でランタイムリンク)。
- Linux: OpenBLAS。

選択は `CMakeLists.txt` にあります。AOT ビルドは適切なランタイムアーカ
イブを介してこれを拾います。

### Broadcast

標準的な NumPy スタイルの broadcasting: shape は右揃えされ、欠けた次元
は size-1、size-1 次元は伸張します。broadcast 軸で stride をゼロに調整
し、汎用の n 次元ループで反復することで実装します。

### lazy shape (計画中のチューニング)

reduction (軸に対する `sum`、`mean`) は、reshape-then-reduce が要求さ
れると現状では中間バッファを materialize します。これらを fusion する
lazy な shape-graph パスは、Tensor の定常状態チューニングの候補です
([[project_roadmap]] §② performance)。

### dtype

F32 のみ。F64 は削除されました (2026-07): Metal には存在せず、コンシュ
ーマ向け NVIDIA GPU では 1/32〜1/64 の速度で走るため、CPU 専用 dtype
になってしまうからです。`Dtype` enum は、将来の BF16 ストレージ型のた
めの seam として残ります。スカラーの入口/出口点 (`.item()`、
`.sum()`、`.to_array()`) は `Float` を surface します。

### GPU (計画中)

[[project_matrix_gpu_roadmap]] — 別の `Matrix` (または `GTensor`) プリ
ミティブが CUDA / Metal Shading Language 経路をホストし、`Tensor` は
CPU/BLAS プリミティブとして残ります。分割の理由は 2 つ:

- CPU と GPU はメモリ所有権の意味論が根本的に異なる。
- Tensor の `shared_ptr<Float[]>` は、host/device を意識したラッパな
  しでは GPU デバイスメモリにマップできない。

9. HTTP
-------

> ステータス: 計画中 (Tier 1、[[project_http_strategy]])。

### ライブラリ選択

cpp-httplib は、ブロッキングの HTTP/1.1、HTTP/2、SSE、WebSocket を 1
つのヘッダで提供します。TLS は静的リンクされた BoringSSL 経由。
async/await は採用*しません* — 並行はスレッド経由です。

### なぜ async/await でなくブロッキング + スレッドなのか

- インタプリタ型の動的言語をまたぐ async/await は、我々が狙う規模で
  は、わずかな利得のために大きな意味論的追加 (colored functions、
  executor モデル) になる。
- cpp-httplib は async を強制せずに必要なもの (SSE、WS) をすべてカバー
  する。
- 数千同時接続というスケール上限は、Culebra が位置づけられているワー
  クロード (CLI ツール、小規模サーバー、ホスト埋め込み) には十分。

### surface area

```
HTTP.get(url, **opts)
HTTP.post(url, body, **opts)
HTTP.request(method, url, **opts)
HTTP.serve(host, port, handler)
HTTP.sse(host, port, handler)
HTTP.websocket(...)
```

サーバー側は、`**opts` でサイズ指定されるスレッドプール上で走りま
す。

10. モジュールシステム
----------------------

### リゾルバ

モジュールビルドは 1 つのエントリファイルから始まります。リゾルバは反
復します:

1. エントリをパースし、未解決のトップレベル識別子を収集する。
2. 未解決の各識別子 `x` について、兄弟の `.cul` ファイル (同一ディレク
   トリ) から `x` という名前のトップレベル束縛を探す。見つかれば、そ
   のファイルをビルドセットに追加する。
3. 新たに追加されたファイルをパースし、それらの未解決識別子を収集す
   る。
4. 不動点に達するまで繰り返す。

グラフは有向です。辺は「参照を含むファイル」から「束縛を含むファイ
ル」へ向かいます。

### サイクル検出

ファイルグラフの強連結成分は Tarjan のアルゴリズムで計算されます。非自
明な SCC はサイクルであり、サイクル内の 1 つの参照を指す正確な file/
line の引用付きで拒否されます。共有の第 3 のファイルを介してリファクタ
してください。

### エントリ env の隔離

エントリファイル内の束縛は、import されたファイルからは可視*ではあり
ません*。import されたファイル内の束縛は、ビルドのどこからでも可視で
す。これは Go のパッケージモデル (main 以外の全ファイルが「そのパッケ
ージ」) に一致し、エントリはトップレベルの `main` のように扱われま
す。

この非対称性は意図的です。エントリスクリプトは、import するヘルパを汚
染することなく、使い捨ての名前 (`mut tmp = ...`) を使えるということで
す。

### なぜ明示的な `import` 文が無いのか

1500 行のプログラムが 30 個のヘルパファイルを引き込むこともありま
す。各 `import` を強制しても何も得られません — ツールは未解決識別子か
ら同じグラフを導出します。コスト (リゾルバの追加パス 1 回) はビルド時
に 1 度支払われ、利得はより速い執筆ループと無償の tree-shaking です
([[project_module_system]])。

11. ビルドと vendor
-------------------

### vendor ツリー (`vendor/`)

| ライブラリ | 目的 | リンケージ |
|---|---|---|
| `cpp-peglib` | PEG パーサ | header-only |
| `cpp-linenoise` | REPL 行エディタ | header-only |
| `cpp-unicodelib` | Unicode テーブル (scalar、grapheme、case) | header-only |
| `cpp-embedlib` | 静的アーカイブをドライババイナリに焼き込む (`cpp_embedlib_add()`) | header-only |
| `cpp-regexlib` *(計画中)* | Regex エンジン (Ch.7) | header-only (計画中) |
| `cpp-httplib` *(計画中)* | HTTP スタック (Ch.9) | header-only (計画中) |
| BoringSSL *(計画中)* | HTTP 用 TLS (Ch.9) | static archive |

非自明な依存はすべて header-only または静的リンクなので、`culebra
build` は自己完結型のバイナリを生成します。

### CMake 構造

- `CMakeLists.txt` (トップレベル) — `culebra` ドライバ、任意の LLVM リ
  ンケージ、base + 機能別のランタイムアーカイブ、embed テストを定義。
- `vendor/cpp-embedlib/cmake/cpp-embedlib.cmake` — `libculebra_rt.a`
  とその機能別アーカイブ (`libculebra_rt_tensor.a`、`_http`、
  `_compress`) をドライバに焼き込むための `cpp_embedlib_add()` を提
  供。

`option(CULEBRA_ENABLE_JIT)` が LLVM リンケージを制御します。JIT を
off にすると、ドライバは ~1 MB で LLVM 依存を持ちません。

### 依存ポリシー

- パッケージ管理される依存よりも header-only の vendor ライブラリを優
  先する。リポジトリは、C++23 コンパイラ (と JIT 用に任意で LLVM) 以外
  のシステムパッケージなしで clone-and-build できるべき。
- git submodule なし — `vendor/` はコミット済み。
- 新しい vendor ライブラリの追加には、この章に Why エントリが必要。

12. テストランナー
------------------

> ステータス: Draft。並行の作業サイクルで設計中
> ([[project_culebra_test_docs_dependency]])。この節は CLI が確定した
> 時点で書き直されます。

方向性 (変更の可能性あり):

- `culebra test [path]` は `*_spec.cul` を発見し、フラットな
  `test "..." { }` ブロックを走らせる。`describe` のネストは無し。階層
  はディレクトリから来る。
- `culebra test --doc <markdown>` は、doctest 規約
  ([[project_doctest_convention]]) に従って ` ```culebra ` ブロックを
  抽出し、各ブロックを隔離スコープで走らせる。
- バックエンド選択: `--interp` / `--jit` / `--aot` (既定: all)。
- レポータ: TAP 互換に加えて、色付きの人間向けフォーマット。

最も興味深い内部は doctest 抽出器です。markdown から `culebra` タグ付
きの fenced ブロックをパースし、ブロック先頭の `# doctest:` 行を走査し
てディレクティブを読み取り、`# =>` と `# => |` マーカーから期待される
stdout を組み立てます。
