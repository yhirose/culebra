Culebra 内部構造
================

本ドキュメントは、ユーザー向けガイドに対する開発者向けの姉妹編で
す。*どのように* (実装戦略、ライブラリ選択、内部データ構造) を記録
します。検討したが不採用にした設計は、ここではなく
[`_history.ja.md`](_history.ja.md) にまとめてあります。すべてのドキュ
メントはバイリンガルであり、このファイルの英語原本は
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
13. [メモリモデル: RC、GC、決定的 drop](#13-メモリモデル-rcgc決定的-drop)
14. [JIT GC バックストップ](#14-jit-gc-バックストップ)
15. [JIT 所有権: 構造的リーク自由](#15-jit-所有権-構造的リーク自由)
16. [代数的エフェクト (source transform)](#16-代数的エフェクト-source-transform)

不採用・撤回した設計は [`_history.ja.md`](_history.ja.md) に集約してあり
ます。

---

## 1. アーキテクチャ概観

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

メモリ管理 (参照カウント、tracing バックストップ、JIT の構造的リーク
自由の規律) は横断的な関心事であり、Ch.13〜15 でまとめて扱います。

## 2. パーサ (cpp-peglib)

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

## 3. インタプリタ

### Value レイアウト

`Value` は 8 つの組み込み型に対する tagged union (実質的には
`std::variant`) です。4 つの参照型 (`String`、`Array`、`Object`、
`Function`) は `shared_ptr` を介して boxing され、4 つのスカラーはイ
ンラインです。

参照カウントは `shared_ptr` の既定に従います — 自動かつ厳密なので、
このバックエンドではリークと二重解放は構造的に起こり得ません。RC だ
けでは回収できないサイクルは、精密なサイクルコレクタ (`InterpGC`) が
回収します。RC/GC/drop の完全なモデル (JIT と共有) は Ch.13 を、
`InterpGC` が C++ スタックをスキャンせずにルートを見つける方法は
Ch.13 の「ルーティング」節を参照してください。

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

## 4. JIT (LLVM ORC)

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
のテーブルです。裸の namespace メソッド (`Math.abs`、`IO.inspect`) は
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

### for-in カーソル

`for x in xs` は Array、Tuple、Set、Object のキー、Range、`iter` プロパ
ティを持つ任意のオブジェクト、String の7面を扱う必要があり、進め方は3種
類あります — 添字による配列走査、UTF-8 スカラ走査、`has_next`/`next` の
イテレータプロトコルです。これらは1つのカーソルを共有します。ループ先頭
のタグディスパッチはカーソルを *open* するだけ (kind と、その kind が必
要とする状態を記録) で、単一のループヘッドが kind で switch して対応する
advance を選び、どの advance も要素を1つの body へ渡します。出口も共有で
す。イテレータの dispose はカーソルのイテレータスロットでガードされ、
array/string カーソルはそこを nil のままにします。

body を1回だけ emit することがネストを現実的にします。容器の各アームに
body を inline すると1段ごとに6倍され、3重ネストでは最内 body が 216 回
emit されていました。IR は1段あたり約 6.4 倍に増え、4重ネストの4行プログ
ラムが 100 万行の IR を生みました。

リテラル range 上の `for v in a..b` はこの経路を通らず、直接の Long カウ
ンタループへコンパイルされます。Range オブジェクトもヒープイテレータも、
ステップごとの `{done,value}` オブジェクトもありません。

### stdlib preamble の splice

いくつかの stdlib モジュール (`Time`、`Term`、`Args`、`Regex`、`Log`、
`Path`、`Canvas`、matcher ファミリ、`__Eff`) は C++ でなく culebra で書
かれており、3 backend が1つの実装を共有します。インタプリタはこれらを環
境ごとに遅延束縛しますが、JIT/AOT 経路はプログラムが使うものをエントリモ
ジュールの AST へ、ユーザ文の前の statement として splice します。ソース
テキストへの連結ではなく splice なのは、ユーザノードがパース時の行・列を
保ち、エラー位置がインタプリタと一致するようにするためです。

どのモジュールを splice するかはエントリモジュールのパース済みトークン集
合から決まるため、名前は厳密一致です。コメント中や、より長い識別子の一部
としてのモジュール名では引き込まれません。1モジュールあたり JIT コンパイ
ル時間で約1秒かかるので、この違いは机上の話ではありません。

インタプリタと異なり、JIT が生成するコードは heap 値 (Object、
Array、Func、Set、Tensor、Cell、String) を `shared_ptr` ではなく、手書
きで emit した retain/release IR を通じて管理します。その規律 — 所有
権をどう追跡するか、tracing バックストップが RC の回収できないものを
どう回収するか、リーク/二重解放を場当たり的な修正ではなく構造的に不
可能化した方法 — が Ch.13〜15 の主題です。

## 5. AOT codegen

`culebra build foo.cul -o foo` は、モジュールグラフ (Ch.10) をウォー
クし、到達可能な各トップレベルを LLVM IR へ lower し、non-PIC な `.o`
を emit し、そのオブジェクトとランタイムアーカイブを最終リンクのため
にシステム C++ コンパイラへ渡します。

### tree-shaking

モジュールグラフと AST が合わさって、到達可能なトップレベル名の集合を
与えます。ランタイムヘルパ (~200 個) は機能グループごとに分割され、
ユーザープログラムから静的に参照されるグループのみがリンクされます。
`inspect` を使う "hello world" は IO と Long プリンタを引き込み、それ以外
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

## 6. 文字列 / Unicode

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
が、`TAG_STRING` に誤タグ付けされたヘッダなしポインタを捕捉します。

`String`/`StringView` は参照カウントを持ちません — RC の
release-to-zero ではなく、tracing バックストップのみが回収します
(Ch.13〜14 で理由と仕組みを扱います。Ch.13 の「traced-only」の注記を参
照)。

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

## 7. Regex

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

## 8. Tensor

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

## 9. HTTP

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

## 10. モジュールシステム

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

## 11. ビルドと vendor

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

## 12. テストランナー

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

## 13. メモリモデル: RC、GC、決定的 drop

Culebra のメモリ管理は、両バックエンドにおいて **RC 主体 + tracing
バックストップ**です。この章は、参照カウント、tracing コレクタ、決定
的 `drop` がどう振る舞うかを記述します。Ch.14 は JIT コレクタの実装
を、Ch.15 は JIT がそもそも RC 配置を正しく保つ方法を扱います。3 つの
記述が食い違う場合は、この章が勝ちます。

### 13.1 2 つの層、どちらも恒久的

- **RC がメモリと `drop` のタイミングを所有します。** interp: 値は
  `shared_ptr` (自動的かつ厳密)。JIT: 値はタグ付き `i64` で、RC の配
  置は Ch.15 の所有権規律に従います — 残った裸の retain/release 箇所
  は、`tools/check_rc_discipline.sh` の ratchet が数える監査済みの例
  外です。release-to-zero は直ちに解放し `drop` を発火します — これ
  が `drop` を決定的にしています。
- **Tracing は RC が回収できないものを回収します**: 参照サイクルと、
  RC 配置バグによるリークです。JIT: registry heap 上の保守的な
  mark-sweep (Ch.14)。Interp: `InterpGC`、精密な CPython 流 `gc_refs`
  コレクタ。

どちらの層も退役できません。RC だけでは決してリークフリーになりませ
ん — サイクルは互いのカウントを永遠に正に保ち、手作業の配置ミスはリー
クします (Swift のコンパイラ完璧な ARC でさえ retain サイクルを日常的
にリークします。「体系的な RC で十分」という主張の標準的な反例です)。
Tracing だけでは決定的 `drop` を破壊します — tracer は collect 時にし
か死を発見しないため、`drop` は遅く、順序不定に、非決定的に発火してし
まい (Go/Java の finalizer モデル)、共有所有権下での決定的な
finalization には完全な参照カウントが必要です。したがって tracer は
**恒久的で load-bearing なコンポーネント**であり、一時しのぎではあり
ません。残る問いは、それが RC のバグを*サイレントにマスクする*のをど
う止めるか、だけです (§13.4〜13.5、Ch.15 §15.2)。

### 13.2 決定的 `drop` — 4 経路の union

`drop` は 4 つの経路から発火し、オブジェクトごとの `dropped` フラグ
(JIT: `JitObject::dropped`、interp: `OrderedSymbolMap::dropped`) によ
って**厳密に一度だけ**に重複排除されます。不変条件は呼び出し箇所では
なくこのフラグです:

| 経路 | トリガー | 機構 |
|---|---|---|
| (a) release-to-zero | 最後の参照が解放された | JIT `_culebra_value_release_impl` がまず drop を発火し、その後 proto/slots/sidecar を解体。Interp: prop-map の `shared_ptr` deleter → `_call_drop_if_present`。 |
| (b) 明示的な `obj.drop()` | ユーザーの呼び出し | オブジェクトは生存し続ける。フラグは 2 回目の発火を防ぐだけ。 |
| (c) スコープ終了 | owned リージョンの解決 | 両バックエンドとも、スコープの drop 保有オブジェクトに対して局所的な Bacon-Rajan trial deletion を行う: 外部から到達不能なら今すぐ発火 (サイクルメンバーも含む)。外部から到達可能なら生存し、親スコープへ圧縮。 |
| (d) GC バックストップの finalize | collect が dead set を発見 | 構造がまだ無傷なうちに dead set の `drop` を発火する **PEP-442 流の pre-sweep パス**。その後 sweep は*メモリのみ*を回収する。**サイクルメンバーも `drop` を発火する。** |

順序: スコープ内では LIFO。defer はどの離脱経路でもスロット解放より
前に走る。RC カスケードは親が子より先。

いくつかの load-bearing な細部: `drop` の発火は、**本体の実行中は
refcount をセンチネル値に固定し、終了後に元のカウントへ復元します**
— これにより、無制限の再入的な release (自分自身のサイクルメンバー
を null にする drop 本体) が吸収されます。ゼロではなく*復元*するの
は、経路 (b)/(c)/(d) は生きた参照を保持したまま到達するためです。
`throw` は意図的にペイロードを retain **しません** — キャリアが投げ
た側の `+1` を引き継ぎます。ここで retain するとリークが復活しま
す。トップレベルの束縛はプログラム終了時に意図的に `drop` を発火し
ません (§13.6)。

### 13.3 参照カウントされるもの、traced-only なもの

参照カウントされる (構造体 offset 0 に一様に refcount を持つ):
`Func` (クロージャ)、`Array`、`Tuple`、`Object`、`Tensor`、`Set`、お
よび内部的な capture `Cell`。Nil/Bool/Long/Float は即値であり、決して
参照カウントされません。

**`String`/`StringView` は参照カウントを持ちません** — これらのタグ
では retain/release は no-op であり、**tracing バックストップのみ**が
回収します。2026-07 まで、これは本物の恒久リークでした (JIT の
String は `malloc` されるだけで回収者が存在しませんでした)。2 つの
バリアが String をコレクタから見えるようにしました: owner-edge 追跡
(`StringView` が借用元の `String` を root する) と、コレクタが列挙で
きる per-Runtime のバイト slab を経由して一過性の JIT 文字列バイトを
ルーティングすること。dead な文字列は、他の traced-only オブジェクト
と同様、次の collect で回収されるようになりました — 代償として
String は RC で回収されるのではなくバックストップのみで回収されます
(String は典型的に短命か immortal-intern されているため、サイクルを
作りにくく、これは許容範囲です)。interp には traced-only という概念自
体がありません — `std::string` を含め、interp の heap 値はすべて通常
の `shared_ptr` オブジェクトです。

### 13.4 ルーティング — 2 つのバックエンドは違うやり方をする

- **JIT (出荷時の既定):** *マシンスタック全体* の保守的なスキャンが
  refcount に関わらずすべての in-flight `JitValue` を root します —
  スキャン機構とその正当性の論証は Ch.14 §14.3 を参照。代替の
  `gc_refs` モード (`CULEBRA_GC_REFS=1`、既定 off) は CPython のアル
  ゴリズムです (refcount からヒープ内エッジを引く、スタックスキャン
  なし)。その健全性は RC の会計の正しさそのものです。`gc_refs` の上
  に構築された診断分類器 (§13.5) は、conservative-dead だが
  gc_refs-retained なオブジェクトを *inflated-RC* (確定的な RC 配置リ
  ーク) か *transitively-held* (サイクル) に分類します — 偽陽性ゼロ
  の RC リーク検出器です。
- **Interp:** C++ スタックスキャンは一切ありません。安全性は*スケジ
  ューリング*によるものです — collect は文の境界でのみ走り、root は
  現在の env チェーンと `FrameRootGuard` エントリから手で辿ります。

帰結: 「ルーティングはどの retain からも独立している」というのは
**JIT 限定**です。そして `gc_refs` の下では、ルーティングは RC の会計
と同程度にしか健全ではありません — バランスが取れているが早すぎる
release は、`gc_refs` の下では use-after-free の窓になり、保守的スキ
ャンはそれを (より多く root することで) 暗黙に吸収しますが `gc_refs`
はしません。これが、保守的スキャンが単純に `gc_refs` へ置き換えられ
ない理由です。この分割の所有権設計への帰結は Ch.15 §15.2 を参照して
ください。

### 13.5 カバレッジに頼らず RC バグを検出する

配置バグが単に「バックストップが静かに回収し、誰も気づかない」を意味
していた盲点を、debug/CI 限定の検出器が塞ぎます:
`CULEBRA_GC_LEAK_ABORT=1` は各オブジェクトの確保時バックトレースを記録
し、プログラムにつき 1 回の静穏な safepoint (JIT の teardown、トップ
レベルがリターンした後だが module/namespace の root はまだ配線されて
いる時点 — 「RC が inflated かつ dead」が曖昧さなく「リークした」を意
味する唯一の点) で、inflated-RC 分類器がオブジェクトの birth site 付
きで abort します。これは difftest コーパス全体に対する常設 CI フェー
ズ (`tools/difftest/leak_abort_suite.sh`) として、throw パスも含めて
走り、サイクルのみの allowlist を持ちます。no-LTO/debug 専用のツール
です (LTO はスタックレイアウトを変えて検出漏れを増やすため)。本番は
静かに回収し続けます — これは受け入れられたトレードオフです (クラッ
シュ自体が DoS のベクタになってしまうため)。

### 13.6 受容済みの `drop` タイミング例外

`drop` が*厳密に*決定的でない 3 つのケースは、文書化された言語意味論
として受容されています — Python/Swift の標準に一致し、それぞれ*タイ
ミング*のみを劣化させ、実際の回収そのものは劣化させません: (1) 1 つ
のスコープが極端に多数の drop 保有オブジェクトを同時に解決する場合
(`kNodeBudget` オーバーフロー) は、超過分をバックストップに委ねる。
(2) トップレベルの束縛はプログラム終了時に `drop` を発火しません —
両バックエンドとも設計上こうなっています。stdlib リソース (`File`、
ソケット) は、その下で C++ RAII を使っているため、終了時にやはり
flush/close されます — 影響を受けるのは、トップレベルスコープでの
*ユーザー定義*の `drop` の追加の副作用のみで、そのケースには `defer`
/ 明示的な `.drop()` / `with` が引き続き使えます。(3) 自己キャプチャ
するクロージャのサイクルは、interp では JIT より 1 collect 分遅れて
発火する (drop の*回数*は対称。*タイミング*が 1 collect 分だけ異な
る)。

主流の RC/GC 言語はどれも同等の例外を持ちます (Python のモジュールレ
ベル `__del__` はインタプリタ終了時に保証されない。Go の `defer` は
`os.Exit` では走らない。C++ の static-dtor の順序は
`_exit`/シグナル/`abort` の下では保証されない) — これは「Python/Swift
と同程度に強い決定的 drop」であって「例外ゼロ」ではありません。これ
以上の解消は、既存の `defer`/`with`/`.drop()` という逃げ道が既に得ら
れるものをほとんどカバーしているため追及していません。

## 14. JIT GC バックストップ

JIT の tracing コレクタは、保持され続ける手動 RC (Ch.13) と並走する
**保守的、non-moving、mark-sweep のバックストップ**です — RC の置き
換えではありません。この章はコレクタの実装であり、Ch.13 はそれが
バックストップする RC/drop モデルを、Ch.15 は RC 配置がそもそも正しく
保たれる仕組みを扱います。

### 14.1 なぜバックストップが必要か

生成コード内の手動 RC は、リーク (release し忘れ) と二重解放 (既に消
費済みの値に対する release) を発生させます — あらゆる制御フローパス
(fall-through、`break`、`continue`、早期 `return`、例外 unwind) で配
置を手作業で正確に行うのは困難であり、バックストップなしでは release
し忘れやサイクルは*恒久的*です (ある JIT microgpt の実行では、これに
より RSS が ~5 GB まで成長しました)。純粋な tracing (RC を一切持たな
い) も検討され、§13.1 で述べた理由で不採用となりました: 決定的な
`drop` には完全な RC が必要だからです。したがって RC が主たるマネー
ジャであり続け、コレクタの仕事は RC が回収できない残余 (サイクル、
release し忘れによるリーク) の回収のみです。

### 14.2 オブジェクトモデルと heap

GC が追跡する構造体 (`JitObject`、`JitArray`、`JitCell`、
`JitClosure`、`JitSet`、`JitTensor`) は、構造体 offset 0 に
`int64_t refcount` を維持します (既存の retain/release IR は無傷で
す)。コレクタ自身の per-object メタデータ (mark bit、type tag、
generation) は、in-object ヘッダではなく **registry** (address →
metadata マップ) に置かれます — これはより小さな変更です。既存の
codegen emit サイトを書き換えるのではなく、retain/release IR をその
まま復活させるからです。

可変長バッファ (`JitArray::items`、`JitObject::slots`、クロージャの
capture) は GC 確保ではなく C++ 所有のままです: これらは、
`enumerate_children` を通じてマーキング中に到達される通常の C++ デス
トラクタによって sweep 時に解放されます。これによりコレクタはシンプ
ルに保たれ、sweep のクリーンアップは自動化されます。

registry は当初 `std::unordered_map` でしたが、そのノードごとの確保
は object/array 中心のワークロードでオーバーヘッドの全て
(+12〜21%) と計測されました。open-addressing のフラットハッシュマッ
プ (エントリごとの確保なし、tombstone 再利用) に置き換え、オーバーヘ
ッドを回収しました。size-class/region アロケータ (Go/JSC モデル) は、
確保スループットが*後で*不足していると計測された*場合の*次の一手です
— 計測なしには追求しません。

### 14.3 ルート探索 (保守的)

collect 時、ポインタとして読んだときに有効なオブジェクトの先頭に着地
する任意のマシンワードは、root の候補です。ソースは: (1) 各ミューテ
ータスレッドのマシンスタック、collect 地点の SP からそのスレッドのス
タックベースまで — これは JIT 生成フレームと C++ ランタイムヘルパフ
レームの両方をカバーするため、C++ の builtin ローカル変数に保持され
た `JitValue` は自動的に root される。(2) callee-saved レジスタ、
collect 開始時にスタックへフラッシュされる。(3) コレクタに登録され
た明示的なグローバル root (namespace オブジェクトテーブル、モジュー
ルキャッシュ、REPL のグローバル、例外キャリア、defer スタック)。

**正当性の論証:** collect を引き起こしうる呼び出しをまたいで生存する
GC ポインタは、プラットフォームの呼び出し規約により、その呼び出しの前
にスタックへスピルされるか callee-saved レジスタに保持されているはず
です。collect が実際に走る瞬間 — アロケータの奥深く — には、そのよう
な値はすべて (1)+(2) によって見つかります。これは、最適化コンパイラ
の下で Boehm/Ruby 流の保守的 GC を健全にしている論証と同じものです。

`JitValue` は `{tag, data}` です。スキャナはタグを参照せず、8 バイト
ワードを 1 つずつテストします。したがって heap ポインタはタグに関わ
らず見つかります。たまたま heap アドレスにエイリアスするポインタ以外
のスカラーは*偽*の root です (有界な過剰保持であり、正当性の問題では
ありません)。`data` は常にオブジェクトの先頭を指します (interior
ポインタなし)。実際にデバッグ時間を要した細部が 1 つあります: ルート
スキャンは frame pointer ではなく **stack pointer** から始めなければ
なりません — さもないと `-O2` 以上ではフレームポインタより下にあるロ
ーカル変数/spill が見逃されます。

### 14.4 Collect: mark と sweep

**Mark:** root set からトランジティブに辿ります。生きている各オブジ
ェクトについて mark bit を立て、その子 (`enumerate_children` 経由で
得る) を mark スタックへ push し、不動点まで反復します。保守的な
root は pin されます (何も動かないので、更新すべきものがありません)。

**Sweep:** registry を walk し、mark されていない各オブジェクトにつ
いて C++ デストラクタを厳密に一度実行し、スロットを回収します。実際
の root からの完全な mark-sweep は、古い bookkeeping がどうであれ、到
達不能なオブジェクトを回収します — これは古い、tracing しないコレク
タが欠いていた性質であり、リークがかつて恒久的だった理由です。

Finalization (pre-sweep の drop パス) は Ch.13 §13.2 の経路 (d) で扱っ
ています。drop モデルの一部であり sweep 機構の一部ではないため、ここ
では重複させません。

### 14.5 世代別レイヤ (将来、正当性には無関係)

現在の mark-sweep は非世代別であり、それ単体で正しく安全です。世代別
レイヤ (若い/古い世代、remembered set を辿る minor collect) は、ベー
スが十分だと計測された後にのみ追加するスループット最適化です。将来の
実装を制約するため、ここに唯一の要件を記しておきます: **write
barrier** — 古いオブジェクトのフィールドへの heap 値の store はすべ
て remembered set にその古いオブジェクトを記録しなければならず、さも
ないと minor collect が old→young エッジ経由でしか到達できない生きた
若いオブジェクトを解放してしまう可能性があります。

### 14.6 安全装置

- **`CULEBRA_GC_STRESS=1`** はあらゆる確保のたびに collect し、
  rooting/marking のバグをフレーキーにではなく決定的に露出します
  (SpiderMonkey の `gcZeal` モデル)。テストスイートはこの下でも green
  です。
- **`GC.stat()`** は生存オブジェクト数/heap バイト数を公開します。
- **リーク回帰テスト** (`tests/test_gc_no_leak.cul`) は JIT の受け入
  れゲートであり、インタプリタの green ベースラインを反映していま
  す。
- **Debug fill** は解放済みスロットを毒パターンで埋めます。
  use-after-free は静かにゴミを読む代わりに assert します。
- **Heap verify** (debug ビルド) はすべての生存オブジェクトを walk
  し、各子ポインタが有効な heap オブジェクトに解決することを検証しま
  す。

GC 自身の C++ 実装も、それがランタイムに強制するのと同じ RAII の規律
に従います: teardown は構造体のデストラクタ、stop-the-world 協調はス
コープガード、グローバル root の登録はスコープ寿命です。

### 14.7 バックエンド間: インタプリタの精密コレクタ

インタプリタの `InterpGC` は精密 (CPython 流の `gc_refs` 減算 + BFS +
clear) であり保守的ではありません — refcount が正確な `shared_ptr`
カウントであるため、フォールバックのスタックスキャンを必要としませ
ん。Array の backing vector、キャプチャされた Environment、Object の
property map、Tuple/Set のメンバーをサイクルノードとして追跡するた
め、JIT バックストップが回収するのと同じサイクル形状がインタプリタで
も回収され、2 つのバックエンドが挙動的に対称に保たれます。

精密であるということは、`InterpGC` は**すべての生存値が登録された
root から到達可能な場合にのみ**正しいということです — スタックのみに
存在する 2 種類の生存性は明示的に root しなければならず、さもないと
プログラム途中で値が掃除されてしまいます (見せかけの `NameError` と
して表面化): (1) アクティブな env チェーン — collect は次の文の境界
まで遅延され、その安全点での生存 env は現在の文の `env` と、C++ 呼び
出しスタックにまだ残っているすべての呼び出し元の env であり、各呼び
出しは RAII の `FrameRootGuard` を通じて呼び出し元の env を push す
る。(2) in-flight な C++ スタック一時変数 — 実際の collect を (確保
時点ではなく) 次の文の境界まで遅延させることは、C++ の戻り値として一
時的に保持されているだけの、新しく構築された自己キャプチャクロージャ
もカバーする。次の文の境界の時点では、既に root された env へ格納済
みだからです。

interp/JIT 境界を跨ぐオブジェクト (`Tensor` の `impl` は
`shared_ptr<TensorImpl>`) は、どちらのコレクタの下でも既存のハンドル
のままです — 何も動かないため、C++ 相互運用に渡された生ポインタは
collect をまたいでも有効なままです。

### 14.8 なぜ精密/moving コレクタではないのか

moving コレクタは検討され、先送りではなく不採用となりました。今日の
Culebra が満たせない、2 つの厳しい前提条件があります: (1) **精密な
ルーティング** — 保守的な (かもしれないポインタの) root は、その参照
先が動いたときに更新できないため、moving はまず精密な root を要求し
ます。(2) **GC 非協力コードへ生ポインタが escape しないこと** —
Culebra は Tensor とインタプリタの相互運用のために C++ へ生ポインタを
渡し、タグ付き `JitValue {tag, data}` 表現はポインタを整数へ詰め込ん
でいます。これは LLVM Statepoints では追跡できません。これは
CPython、Lua、Ruby の既定 GC が non-moving のままである理由と同じで
す。moving が*固有に*買う唯一のスループットレバー — compaction と生
存者数に比例する collect コスト — は、今日の Culebra にとって二次的
です。size-class/Immix 流の bump-region アロケータ (§14.2) は、ルー
ティング/相互運用の再設計コストを払わずに、利用可能な non-moving のス
ループットの大部分を得られます。断片化が実際に他の手段で対処できない
コストだと計測された場合、または生ポインタの相互運用がハンドル/
pinning の背後に再設計された場合にのみ再検討します。

### 14.9 不変条件

1. 手動 RC は維持され、メモリと決定的 `drop` を所有し続ける。コレク
   タはバックストップであり、決して置き換えではない。構造体 offset
   0 は `int64_t refcount` のまま。コレクタのメタデータは registry
   に存在する。
2. **健全性 (無条件):** collect は、root または他の生存オブジェクト
   から到達可能なオブジェクトを決して解放しない。
3. **完全性はベストエフォートであり、厳密ではない:** 保守的な
   collect は、古びたスタック/レジスタワードがポインタに見えるとき、
   到達不能なオブジェクトを過剰に保持することがある — 安全で有界であ
   り、想定内。厳密な回収集合を必要とするテストのために
   `collect_precise(roots)` エントリ (明示的な root、スタックスキャ
   ンなし) が存在する。
4. Sweep は各 dead オブジェクトの C++ デストラクタを厳密に一度実行す
   る。
5. (世代別が実装された暁には) すべての old→young store は、次の
   minor collect の前に write barrier によって記録される。
6. フルスイートは `CULEBRA_GC_STRESS=1` の下で green である。

## 15. JIT 所有権: 構造的リーク自由

Ch.13〜14 は RC/drop モデルとバックストップコレクタを記述します。この
章は、*そもそも JIT がどうやって RC の配置を正しく保つのか* — リーク
と二重解放を、1 つずつ潰すバグクラスとしてではなく、生成コードにおい
て構造的に不可能にする常設の設計です。

### 15.1 規則

**リークと二重解放は、構築によって防がなければなりません** — C++ の
RAII や Rust の所有権/`Drop` がそれらを防ぐのと同じやり方で。1 つずつ
リークする codegen サイトを狩るのは明示的に戦略ではありません: そうし
た個別修正はどれも次の未監査のパスという危険を残します。値を生産また
は消費する新しい codegen は、以下の所有権レイヤを使います。新しいコー
ドパスに現れる裸の `emit_value_retain`/`emit_value_release` は設計上
の悪臭であり、レビューで正当化するか、できればリファクタリングで排除
すべきです。

この問題を持つのは JIT だけです — インタプリタの `shared_ptr` 値は既
に構築によってリークと二重解放を不可能にしています。JIT は
`shared_ptr` のアトミック refcount オーバーヘッドを避けるために厳密な
RC を捨てて手書きの retain/release を採用しましたが、それを最初は厳密
に強制された規約にもtracingバックストップにも対応させていませんでし
た。この章がその規約であり、Ch.14 がバックストップです。

### 15.2 所有権とルーティングは別の仕事

refcount はサイレントに 2 つの独立した仕事をしており、これを混同する
ことが「この retain は冗長に見えるから消す」という素朴な修正をクラッ
シュさせる原因です: (1) **所有権/解放** — オブジェクトが*いつ*解放さ
れるかを決める (release-to-zero、同時に `drop` を発火する)。(2)
**ルーティング/生存性** — collect をまたいで*何が*生きたままかを決め
る。

**ルーティングは既に、どの単一の retain からも独立して保証されていま
す**。出荷済みの両コレクタによって (Ch.13 §13.4、Ch.14 §14.3): ネイ
ティブフレームの `+1` はまさにルーティングのエッジそのものなので、
in-flight な一時変数は構築によって root になります (RC の会計が正確
であることが前提であり、それこそがこの章の規律が確立するものです)。
したがって「冗長に見える」retain を取り除くための前提条件は、会計上
の証明 — オブジェクトの経済全体の refcount トレースと、フルテストゲ
ート — であって、新しいルーティング機構ではありません。

デバッグ時に 2 つの故障モードを区別できる常設の診断があります:
`CULEBRA_GC_NEVER=1` はあらゆる collect を無効化します。これを有効に
してもクラッシュが**残る**なら純粋な所有権バグ (過剰解放)、消えるな
らルーティングの穴だったということです。実際にはこの方法で調べられた
クラッシュはすべて前者であることが判明しています — 両コレクタが出荷
されて以来、ルーティングが実際のバグの原因になったことはありません。

### 15.3 先行研究

この設計は、独自に発明したのではなく、確立された RC/所有権モデルを統
合したものです:

| システム | 取り入れたもの |
|---|---|
| Rust (affine 所有権、`Drop`) | move-or-drop を値の規律として採用。borrow は決して解放しない |
| C++ RAII | 実装の乗り物そのもの — デストラクタが release を emit するハンドルで、早期 return/例外をまたいでも正しい |
| Swift ARC | retain/release の配置を手作業ではなく一様な規約から*導出*する |
| Perceus (Koka) | 精密で決定的な drop を持つ RC が、構造的に最も近い一致 |
| Nim ORC | 「RC + サイクルバックストップ」を出荷済みの組み合わせとして検証 |
| MLIR/Swift SIL 所有権 | 目指すべき最終形 — 所有権がコメント規約ではなく IR のチェック可能なプロパティであること |

どれも丸ごと当てはまるわけではありません — codegen に borrow checker
はなく、Culebra は関数型 IR ではなく whole-program 推論を伴わない動的
な AST を lower します — が、業界が収束した形 (**コンパイラが一様な
規約から RC 配置を導出し、誰も手で retain/release を書かない**) を、
§15.4 は LLVM IR 上の C++ RAII ハンドルによって実現しています。

### 15.4 設計、レイヤごとに

各レイヤは 1 つのリーク経路を除去します。合わせると、リークには C++
の型/RAII 不変条件を破ることが必要になります — サイレントなランタイ
ムリークではなく、コンパイルエラーです。

**一様な所有権規約。** `compile(expr)` はすべての式ノードについて
`+1` 所有された値を返します。パラメータ/レシーバは borrow (`+0`) で
す — 呼び出し元が呼び出し前に retain し、callee は手渡された ref を消
費します。スコープスロットに格納された値は、そのスロットに所有されま
す。`Cell` は store 時に retain しません — 呼び出し元は渡す ref を所
有していなければなりません。

**`Owned` — 一過性の `+1` のためのハンドル。** move-only な C++ RAII
ハンドル (`Owned { JIT*, llvm::Value*, bool consumed }`) は
`.borrow()` (consume せずに読む)、`.consume()` (`+1` をスロット・呼び
出し・return へ先へ渡し、ハンドルを消費済みとマークする。**二重
consume は codegen 時の abort** であり、このバグクラスはビルド失敗に
なる、二重解放にはならない)、`.drop()` (スコープ終了時のデストラクタ
では遅すぎる位置のために、生きたまま先へ渡すのではなく死ぬだけの値の
release を現在の挿入位置で今すぐ emit する) を提供します。デストラク
タは、ハンドルがスコープを抜けるときまだ所有されていれば release し
ます。`Owned` がカバーするのは**直線的な一時変数のみ**です — 1 ループ
反復だけ生存する値や、あるアームで consume され別のアームで release
される値には、次のレイヤが必要です。

**スコープスロットが escape する値を所有する。** 現在の直線的リージョ
ンより長く生存しなければならない値は、スコープスロットへ
`.consume()` されます。既存のスコープ unwind 機構が、fall-through、
`break`/`continue`、早期 `return`、例外 unwind という**すべて**の離脱
経路でそれを release します — 例外 unwind はリージョンごとのクリーン
アップ landingpad (throw しないリージョンでは dead-code-eliminate さ
れる) を経由します — サイトごとの場当たり的な release ではなく 1 つ
の機構を再利用します。これは、以前は本物の throw パスリーククラスだ
ったものを塞ぎました (スコープの owned なローカル変数とその
`drop()` は、単純に `throw` では一切走りませんでした)。これはあらゆ
るスコープ状のリージョンに配線されています: レキシカルスコープ、
ループ/match/try 本体、`for` の iterable スコープ、そして関数フレー
ム自身。

**Borrow は release できない。** borrow された値は `.borrow()` を通
じてのみ触れられます — borrow と release の両方ができる API は存在し
ないため、「borrow したオペランドを release してしまう」は表現不可能
です。

**ルーティングは refcount ではなくコレクタの仕事。** §15.2 のとお
り、collect をまたいで in-flight な一時変数を生かし続けるための
scoped-pin/shadow-stack 機構は不要です — 出荷済みの両コレクタが既に
それを保証しています。`Owned`/スロットの所有権はもっぱら*解放タイミ
ング*についてです。

**サイクルはカウントされず sweep される。** 参照サイクルはあらゆる
RC 規律の対象外です (Rust の `Rc` も同一の穴を持ちます)。mark-sweep
バックストップ (Ch.14) がサイクルと残余を回収します。所有権が正しけ
れば、バックストップは定常状態では non-load-bearing になります —
これがそもそもこれをやる性能上の意義です。

**ヘルパー所有権契約。** may-throw なランタイム呼び出しをまたいで生
存する codegen 所有の `+1` はすべて、閉じた契約集合から選ばれた唯一
の releaser を unwind エッジ上に持ちます:

| 契約 | 機構 | 使用箇所 |
|---|---|---|
| Caller-cleans | `ThrowGuard` — RAII のリージョンごとのクリーンアップ pad、throw しえない場合は除去される | builtin メソッドのレシーバ/引数、`compile_call` の callee、代入の lvalue、UFCS の callee |
| Callee-cleans-on-direct-throw | ヘルパー内部のガード、ユーザーディスパッチが辞退した後にのみ武装 | 演算子エントリ、`own_receiver` 下の index/property-get、not-a-function エッジ |
| Callee-consumes-on-every-exit | エントリで宣言される owned-arg ハンドル、通常 return でも unwind でも release | ネイティブメソッドエンドポイント、HOF アキュムレータとコールバック |
| Invoker-cleans | invoker が retain し、callee フレームは通常 return で release し、invoker のガードは callee が throw した場合のみ release | すべてのユーザーディスパッチ窓 (`__op__`/`eq`/`hash`/`cmp`/`__index__`/getter) |
| Transfer | 入ってきた `+1` を結果としてそのまま返すか、capture cell/スロットへ手渡す | iter-self メソッド、lazy-combinator の capture cell |

2 つの契約が同じエッジをカバーしてはいけません (1 つの値に対する 2
つの cleaner は二重解放になります)。相互排他性は、ヘルパーガードでは
常に「ユーザーディスパッチが辞退した」、consume された値では「所有者
に入った」です。

**自動 unwind-temp ウィンドウ。** `Owned` は C++ デストラクタを通じ
て release しますが、*ランタイムの* LLVM レベルの throw はそれを走ら
せません — そのため歴史的には、may-throw な呼び出しをまたいで生存す
る codegen 所有の `+1` は、誰かが手作業でガードを配置しない限りリー
クしていました。コーパスはこれらの形をすべて綴ることはありませんでし
た。代わりに、所有権レイヤが今では構築によって unwind エッジを所有し
ます: 生きているすべての `Owned` は JIT に登録され、`emit_call` (
may-throw な呼び出しがその unwind エッジを得る唯一の地点) は、生きて
いてまだカバーされていないすべての `Owned` を、その 1 回の呼び出しの
間だけ per-function プールスロットへ spill し、すべてのクリーンアッ
プ pad はまずプールを release します。ヘルパー契約で既にカバーされて
いる値は `UnwindCovered` と宣言され、ウィンドウはそれをスキップしま
す — それも spill すると二重解放になります。
`CULEBRA_JIT_NO_UNWIND_TEMPS=1` は `CULEBRA_GC_NEVER` の双子となる診
断としてこれを無効化します。

**Block-pinned raw。** 最後の抜け穴は、`+1` がハンドルを離れた瞬間で
した — `consume()` はかつて裸の `llvm::Value*` を返しており、どのレ
イヤも追跡しないまま基本ブロック境界を跨いで運ばれ得ました。実際に最
近起きたリークはすべてこの形でした。不変条件: 裸の `+1` は、それが
consume されたのと同じ基本ブロックの中でのみ使ってよい。これが健全か
つ完全であるのは、`emit_call` がすべての may-throw な呼び出しを、現
在のブロックを終端させる `invoke` に変えるからです — 同じブロックに
いるということは、値が裸だった間に unwind エッジが走らなかったことを
意味します。`consume()` は今や、自分の pin ブロックを記録した
`Pinned` トークンを返します。そのブロックの外で使うと、あらゆるビル
ドモードで codegen 時の abort になり、difftest コーパスがほぼすべて
の構文をコンパイルするため、違反パターンは最初に*コンパイルされた*瞬
間に捕捉されます。`OwnedPhi` はすべての `%Value` phi が経由するチェ
ック付き構造です (各 incoming はそれ自身のアームブロックで宣言・
consume される)。`consume_unchecked()` は、真にブロック境界を跨ぐ一
握りの形 (相互排他なディスパッチアーム、既にスコープスロットに所有さ
れているクロス、`declare_local` へのプロローグ移譲) のための、正当化
された ratchet 数え上げの逃げ道です。`compile_*` の戻り値の seam はこ
れに合わせて型付けされています: すべての `compile_*` ヘルパーは
`Owned` (辞退時は空のハンドル) を返すため、裸の `+1` が `compile_*`
の C++ return を跨ぐことはありません。

### 15.5 不変条件

これらが成り立てば、リークには C++ の型/RAII 不変条件を破ることが必
要です — ビルド時の失敗であり、ランタイムの事故ではありません:

1. あらゆる `+1` の一過性の値は `Owned` に保持されるか、即座に
   consume される。
2. `Owned` は厳密に一度 consume される**か**厳密に一度 drop される —
   両方でも、どちらでもない、ということはない (move-only + デストラ
   クタ + 二重 consume abort によって強制)。
3. escape するすべての値は厳密に 1 つのスコープスロットへ consume さ
   れる。スコープ unwind はすべての離脱経路ですべてのスロットを
   release する。
4. Borrow された値は決して release されない。
5. ルーティングは所有権の refcount とは独立に GC レイヤによって提供さ
   れる。
6. サイクルと残余はバックストップによって回収される。バックストップ
   は定常状態のメモリのために依存されない。
7. may-throw な呼び出しをまたいで生存するすべての `Owned` `+1` は、
   unwind エッジ上にちょうど 1 つの releaser を持つ — 既定では自動ウ
   ィンドウ、または呼び出しサイトが `UnwindCovered` で宣言するヘルパ
   ー契約のいずれか一方のみ。
8. 裸の `+1` は、それが consume されたのと同じ基本ブロックの中にの
   み存在する (`Pinned`)。すべての `%Value` phi は `OwnedPhi` を通じ
   て構築される。意図的な跨ぎは、宣言された releaser を持つ
   `consume_unchecked` サイトである。
9. どの `compile_*` ヘルパーも裸の `llvm::Value*` を返さない — `+1`
   が compile レイヤの C++ return を跨ぐのは `Owned` の中だけであ
   る。

帰結: 正しい codegen パスは、裸で手作業配置された `retain`/
`release` を一切含みません。残っている裸の呼び出しは監査済みの移行負
債であり、`tools/check_rc_discipline.sh` の ratchet によって数えられ
(決して増加を許されず)、`Owned` レイヤがカバーするものではない、いく
つかの正当なカテゴリに分類されます: 生成されたループ本体内の
per-*iteration* な release、`Owned` レイヤより下に位置するスロット/ス
コープのプリミティブ、直線的な一時変数としてではなくチェーンを下る関
数パラメータとして運ばれる値、そして 1 つのアームが consume しもう
1 つの異なるアームが同じ値を release する分岐をまたいだペア。

### 15.6 Culebra 固有の制約

- **タグ付き `i64` の値であり、ポインタではない。** Root/子の列挙は
  タグを見て `data` が heap ポインタかどうかを決めます。これと、生ポ
  インタが Tensor/インタプリタの相互運用へ escape することが、LLVM
  Statepoints がルーティング機構として不採用となった理由です (Ch.14
  §14.8) — それをサポートするための値 ABI の書き換えは、所有権の作
  業そのものを圧倒してしまいます。
- **2 つのバックエンドは対称であり続けなければならない。** 所有権の
  変更は JIT 内部のものであり、インタプリタと比べた観測可能な挙動、
  エラーメッセージ、チェックのタイミング/順序を変えてはならない
  ([[feedback_check_jit_interp_symmetry]])。
- **動的ディスパッチ。** メソッド/演算子のターゲットは実行時に解決さ
  れるため、所有権の規約は callee の静的な型によってではなく、呼び出
  し境界で動的に強制されます。
- **whole-function な IR 所有権ベリファイアは検討され、不採用となり
  ました。**敵対的にレビューされた結果です: それは C++ ヘルパーの内
  部が見えず、「retain/release のバランスが取れている」ことは「生存
  期間が正しい」ことを意味しません (バランスが取れていても早すぎる
  release は use-after-free になり得ます)。上記の block-pinned な会
  計は意図的にそれより狭い範囲です — codegen 自身の `Owned`/
  `Pinned` ハンドルのみを推論し、heap のエイリアシングやオブジェクト
  の生存期間は扱いません。これが、不採用となったベリファイアの致命的
  な反論を回避できる理由です。

## 16. 代数的エフェクト (source transform)

`effect fn`・`perform`・`handle … with` はどのバックエンドも解釈しませ
ん。これらは*パース時に*プレーンな culebra ソース — 合成されたクラス
と `__Eff` ランタイム preamble への呼び出し — へ書き換えられ、再パース
された上で、インタプリタ・JIT・AOT の各パスがエフェクト固有のサポート
を一切持たずにそのまま実行します。これにより3バックエンドの対称性が無償
で保たれます ([[feedback_check_jit_interp_symmetry]])。ジェネレータも同
じ lowering 機構を使うため、このパスとジェネレータ変換は source-slice /
local-rewrite のヘルパーを共有します。

ヘッダの起点: `include/effects_transform.h` (変換パス本体)。ドライバの
ランタイムは `src/preambles/effects.cul` に `__Eff` モジュールとして存在
します。

lowering は動的スコープ・one-shot resume のモデルに従います:

- **`effect fn f(...) { BODY }`** (本体あり) は、*computation オブジェ
  クトを返す*通常の fn に lowering されます。これは flat-dispatch な状
  態機械で、その `_step(rv)` が本体を次の中断点まで実行し、制御をドラ
  イバへ返します。シグネチャのみの `effect fn op(...)` は操作を宣言し、
  throw するスタブに lowering されます。
- **中断点**は、文レベルの `perform op(args)` (SUSPEND) か、別の
  effect fn への文レベル呼び出し (DELEGATE) です。本体は任意の制御フ
  ロー — `if` / `while` / `for` (`while` へ desugar) と `break` /
  `continue` / `return` — を使えます。これらは flat-dispatch な CPS
  ビルダ (`build_dispatch`、ジェネレータ変換と同じ形) で lowering され
  ます。式の中に埋め込まれた `perform` は、まず A 正規化の前段
  (`anf_program`) で文レベルへ巻き上げられるので、CPS 層が見るのは常に
  文レベルの中断のみです。
- **`handle { BODY } with op(params, resume) { H }`** は
  `__Eff.handle(<computation としての BODY>, "op", <ハンドラアダプタ>)`
  へ lowering されます。ドライバは動的スコープのハンドラスタックを辿り、
  `resume` は one-shot の継続 — 通常の RC 値なので、リーク安全性はこれ
  が模倣するジェネレータ機構から継承されます
  ([[project_rc_gc_correct_model]])。プレーンな (エフェクトでない) fn
  も `__Eff.perform_direct` 経由で操作を `perform` でき、エフェクトフル
  な呼び出しは `effect fn` の本体に閉じ込められません。

computation オブジェクトとドライバは return-tag プロトコルを共有します。
各 `_step(rv)` は、何が起きたかをドライバへ伝えるタグを返します。

```
0 = DONE      self._eff_val に computation の結果が入る
1 = SUSPEND   self._eff_op / self._eff_args が perform を表す
2 = DELEGATE  self._eff_delegate が driving 対象のサブ computation
```

2 つの構文は変換時に — 対称的に、どのバックエンドも誤コンパイルしない
よう — 拒否されます: 制御フローの条件や iterable の中の `perform`、およ
び外側の束縛をキャプチャする入れ子の `handle` です。

この機能は完全に source-to-source なので、そのコストはバックエンドの複
雑さではなく、パーサが噛み砕く生成ソース量として現れます。
`CULEBRA_TRANSFORM_STATS=1` は各変換が emit する culebra ソース量を報告
します (`=2` は lowering 後のソースそのものを出力します)。

## 17. Net: 生ソケット

### ロジックの置き場所

`include/net.h` は `http.h` / `sqlite.h` / `proc.h` と同じ形の値中立
コアです。`Value` / `JitValue` / GC 型に依存しないので、
`stdlib_interp.h` と `stdlib_jit.h` が互いを引き込まずに include でき
ます。バックエンド側は値のマーシャリングと `IoStatus` の解釈だけを行い
ます。

フレーミング (`read` / `read_line` / `read_exact` / `read_all`) は、
2 つのアダプタではなくコア側にソケット単位の読み取りバッファとともに
置いています。これが「両方を注意深く書いたからバックエンドが一致する」
と「実装が 1 つしかないから一致する」の違いです。重複するのはエラーの
*文言* だけ (呼び出し地点ごとの `ctx` 文字列) で、そこは
`tests/test_net.cul` のスイープが固定します。

### ブロッキング、ノンブロッキング、Ctrl+C

ソケットは内部的にノンブロッキングで、すべての操作が「`wait_ready()`
してからリトライ」です。理由は 2 つ:

- poll/select の ready 通知はあくまで助言的 — その裏でブロッキング
  `recv()` を呼ぶとタイムアウトを超えて止まりうる。
- `wait_ready()` は 100 ms 刻みでポーリングして `throw_if_interrupted()`
  を呼ぶので、ブロック中の `accept` / `read` が interp・JIT・AOT のいず
  れでも協調的 `Interrupted` を送出する。JIT には文と文の間のセーフ
  ポイントがないため、呼び出し後のチェックでは対称にならない
  (`proc.h` と同じ理屈)。

### ハンドルテーブル

スクリプト側のハンドルは生 fd ではなく thread-local テーブルへの
`int64` インデックスです。偽造・失効したインデックスは境界チェックで
穏当なエラーになり、決して参照解決されません (`sqlite.h` や File と同
じ姿勢)。thread-local で正しいのは、ソケットハンドルが
`__nonsendable__` だからです — アイソレートを越えない、つまりスレッド
を越えません。生 fd は intern されるまで `FdGuard` が所有するので、
`wait_ready()` から送出される `Interrupted` を含め、どのエラー経路でも
リークしません。

### 並行 serve: 越えるのは fd で、ハンドルではない

`listener.serve(handler, workers:)` は呼び出しスレッドで accept し、
ハンドラをプールで実行します。各ワーカーが自分の culebra ランタイムを
持つ — httplib を除いた `Http.server` と同じモデルです。要点は、ソケット
ハンドルが `__nonsendable__` で thread-local テーブルに住むため、ワーカー
に *渡せない* ことです。境界を越えるのは accept した生の **fd** で、
ワーカーはそれを自分のテーブルに intern してハンドルを作ります。不変条件
は例外扱いにするのではなく、保たれたままです。

ハンドラは `serve` の時点で 1 度だけシリアライズされ (だから非 Sendable
なハンドラは最初の接続時ではなくその場で失敗します)、ワーカーごとに再構築
されます。Http サーバのルートハンドラとまったく同じです。

バックプレッシャーはキューが担います。ジョブキューが満杯の間 `submit` が
ブロックするので、速い accept ループがワーカーを際限なく追い越すことは
ありません (実際のバッファはカーネルの listen backlog です)。抜けるとき —
Ctrl+C は `Interrupted` の送出として抜けます — プールのデストラクタが、
まだ始まっていない接続を破棄し、実行中のものを join します。

### AOT の usage-gating 軸を増やさない

Tensor / Http / Compress / SQLite と違い、`Net` は外部ライブラリを一切
引き込みません (素の BSD ソケット、Windows では `ws2_32` のみ)。weak
スタブにするものも force-load するものもないので、base ランタイム
アーカイブに同居し、`aot_scan.h` に `aot_uses_net` は増えません。

### プラットフォーム上の注意

Windows では `WSAPoll()` ではなく `select()` で待ちます (`WSAPoll` は
ノンブロッキング connect の失敗を報告しないため)。SIGPIPE はソケット
単位 (`SO_NOSIGPIPE`) か送信単位 (`MSG_NOSIGNAL`) で抑止します。
Emscripten (Playground) ビルドに生ソケットはないので、エミュレートされた
呼び出しで中途半端に動くのではなく、全エントリポイントが最初にその旨を
報告します。
