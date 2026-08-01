Culebra内部構造
================

本ドキュメントは、ユーザー向けガイドに対する開発者向けの姉妹編で
す。*どのように* (実装戦略、ライブラリ選択、内部データ構造) を記録
します。記すのは現在の実装であって、途中で比較検討した代替案では
ありません。すべてのドキュ
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
17. [Net: 生ソケット](#17-net-生ソケット)

---

## 1. アーキテクチャ概観

1つのAST、3つの実行経路。同じパーサがすべてに供給します。

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

- **ASTの共有**により、バックエンド間で意味論が同一に保たれます。
  新しい言語機能はまずAST + インタプリタに実装され、その後JITと
  AOT経路が追随します。
- **bytecode層は無し。** インタプリタはASTノードを直接ウォークし、
  JITはASTをLLVM IRへlowerします。bytecodeの中間層は、この規
  模では計測可能な利点なしに複雑さを増すため不採用です。
- **ドライババイナリ`culebra`** がCLIです。フラグ (`--jit`、
  `build`、既定のインタプリタ) に応じて3経路のいずれも実行できま
  す。

ヘッダのルート:

- `include/grammar_def.h` — PEG文法。単一の真実源。
- `include/parser.h` — cpp-peglibのパーサと、全backendがノードを読
  むためのASTアクセサ (`view_for`、`view_match`、`view_method`等)。
  `ast.h`は無く、
  ASTは`peg::Ast`そのもの。
- `include/interpreter.h` — ツリーウォーキング型インタプリタ。
- `include/jit.h` — ORC JIT **および** AOTのオブジェクト出力
  (`CULEBRA_ENABLE_JIT`のときのみコンパイル)。loweringを共有している
  ので2経路がドリフトしない。
- `include/jit_*.h` — 関心事で分割したJIT: `jit_value` (タグ付け)、
  `jit_mem` / `jit_slab` / `jit_gc` / `jit_owned` (メモリ)、
  `jit_string`、`jit_iter`、`jit_dispatch`、`jit_runtime` (生成コード
  から呼べるヘルパ)。
- `include/stdlib_interp.h` / `include/stdlib_jit.h` — 標準ライブラリ
  の2つの半分。後述のdriftチェックで対称性を保つ。
- `include/runtime/` — AOT側のランタイム入口 (`runtime_aot.h`、
  `aot_scan.h`、`rt_macros.h`)。

メモリ管理 (参照カウント、tracingバックストップ、JITの構造的リーク
自由の規律) は横断的な関心事であり、Ch.13〜15でまとめて扱います。

## 2. パーサ (cpp-peglib)

文法は`include/grammar_def.h`に単一のPEG仕様として置かれ、
`peg::parser`に供給されます。cpp-peglibが提供するもの:

- PEGの意味論 (greedy、左再帰は禁止ルール)。
- パーサ自身が構築する汎用AST (`peg::Ast`) — プロダクションごとに
  C++ のノードクラスを用意する必要がない。
- ソース位置がすべてのASTノードに自動的に伝播される。

### grammar blob

文法テキストのメタパースには ~10 msかかり、そのままだと毎回の
プロセス起動で払うことになります。`tools/gen_grammar_blob.cc`が
コンパイル済み文法を`include/grammar_blob.h`にシリアライズし
(`just gen-blob`)、`parser.h`はそちらをロードします。ガードは文法
ソースのハッシュ`grammar_blob_key()`: 文法を編集して再生成し忘れた
場合やcpp-peglibのbumpでレイアウトが変わった場合はキーが一致せず、
`load_grammar()`にフォールバックします。どちらの経路でもASTと
packratは同じように有効化されます。

cpp-peglibを選んだ理由 (手書きの再帰下降との比較):

- 文法が1ファイルにまとまり、`language.ja.md`の隣で仕様として読め
  る。
- semantic actionが構文に近い位置に留まり、lexer/parserの二分法に何
  も漏れ出さない。
- 性能はボトルネックになっていない — パース時間はパーサではなくイン
  タプリタ/JITのコンパイルが支配的。

ASTノードはcpp-peglib自身が生成する`shared_ptr<peg::Ast>`です。
識別子解決はパース後のパスへ遅延され、パーサは文脈自由でいられます。

文法に触る前に知っておくべき帰結が2つあります:

- **トークンのテキストはソースバッファへの`string_view`。** ASTを
  持つ側がソース文字列を一緒に生かし続ける必要があります。
  `LoadedModule`が`source` (と、spliceしたpreambleのための
  `aux_sources`) をASTの隣に保持しているのはこのためです。
- **全backendは`ast.nodes[i]`の添字ではなく`parser.h`の`view_*`
  アクセサ経由でノードを読むこと。** cpp-peglibの`AstOptimizer`は
  単一子ノードを畳むので、プロダクションに省略可能要素が増えると生の
  添字がずれます — 「文法を変えたのにwalkerを1つ更新し忘れた」系
  バグの常習犯です。

パース失敗はファイル・行・列とパーサの診断を持つ`SyntaxError`として
表面化し、CLIドライバが`clang`風の固定幅スタイルで整形します。

## 3. インタプリタ

### Value レイアウト

`Value`は`Type`タグ (`type_of`が返しうる12個の名前) と
`std::any`のペイロードです。`Nil` / `Bool` / `Long` / `Float`は
ペイロードに直接入ります。`Long`は必ず`int64_t`で、`long`は使いま
せん — Windowsでは32bitになり、同じプログラムがJITと違う答えを返
してしまいます。`std::any`は幅でなく型で一致するので、値を作る側も読
む側も`int64_t`と書く必要があります (`Value::get`は両者が異なる環境
で`long`をstatic_assertで弾きます)。
`String`は`std::string`を値で持ちます。
コンテナ型は**内部**が`shared_ptr`のstructを持ち
(`ArrayValue::values`は`shared_ptr<vector<Value>>`、Objectのプロパ
ティマップも同様)、これが`Value`自体をcopyableに保ったまま参照
セマンティクスを与えています。

`Value`のmoveコンストラクタは`std::any`を必ず`std::move`する
必要があります — コピーする「move」は、引数バインド・return・swapの
たびにboxingされたペイロードをdeep copyします (moveが多いループで
実測 ~13%)。コピーするmoveは最適化不足ではなく**欠陥**です。

参照カウントは`shared_ptr`の既定に従います — 自動かつ厳密なので、
このバックエンドではリークと二重解放は構造的に起こり得ません。RCだ
けでは回収できないサイクルは、精密なサイクルコレクタ (`InterpGC`) が
回収します。RC/GC/dropの完全なモデル (JITと共有) はCh.13を、
`InterpGC`がC++ スタックをスキャンせずにルートを見つける方法は
Ch.13の「ルーティング」節を参照してください。

### スコープ

`Environment`は連結されたフレームです。各フレームは文字列 → Valueの
マップと、外側フレームへの親ポインタを保持します。クロージャキャプチ
ャは「フレームを閉じ込める」方式で、クロージャは
`shared_ptr<Environment>`を保持し、外側の関数がリターンした後もフレ
ームを生存させ続けます。

### shared_from_this の寿命管理

`Interpreter`は、呼び出し元より長く生存する必要がある4つの長寿命ラ
ムダ (ランタイム値にインストールされるコールバック) を所有します。こ
れらのラムダは`[self = shared_from_this(), ...]`をキャプチャするた
め、escapeしたコールバックが1つでも生存する限りインタプリタオブジ
ェクト自身が生存し続けます。このコードベースで新規のランタイムコール
バックラムダは、必ず同じパターンに従わなければなりません。

### 制御フローとエラー

インタプリタはこの2つを意図的に分離しており、機構も異なります。

**`return` / `break` / `continue`はthread-localスロットで完了しま
す** — ECMAScriptのcompletion recordを、すべての`eval()`シグネチャ
に引き回す代わりに、CPythonがpending例外を保持するのと同じ形で持ち
ます。`eval()`は入口でスロットを見て即座に制御を返すので、値を素通し
するだけの約100箇所の呼び出しサイトは伝播コードを持ちません。関数呼
び出し境界が`return`を消費し (`deliver_call`)、最も近い囲みループが
`break` / `continue`を消費します (`run_loop_body`)。

途中のサイトがしてはいけないのは、完了がpendingの状態で`eval()`の
結果を*変換*することです。`to_bool` / `to_long` / `to_string`は入口
ガードが返すnilに対して`TypeError`を投げるため、それらのサイトは
`eval_operand`を経由してpendingを呼び出し元に報告します。この規則は
`tools/check_flow_discipline.sh`がratchetします。

**ユーザの`throw`はC++ 例外のままです。** 頻度が低くunwindコスト
が問題にならないうえ、throwする`defer`は実行中の例外を置き換えられ
る必要があり、これは戻り値プロトコルでは表現できません。`return`が
unwindしなくなったため、`run_deferred`はスコープのdefer実行中だけ
pendingの完了を退避します (`FlowPark`)。そうしないと入口ガードがすべ
てのdefer本体をno-opにしてしまいます。

この分離により、制御フローは通常の文と同じコストになり、`throw`は
unwindの意味論をそのまま保ちます。

### エラー伝播

2種類のthrowを2つのC++ 例外型が運びます:

- **ユーザの`throw expr`** は`Value`そのものを投げます
  (`eval_throw`は文字どおり`throw eval(...)`)。どんなculebra値も
  そのまま投げて受け取れます。
- **ランタイムエラー**は`culebra::CulebraError` (`kind` / `line` /
  `col`を持つ`std::runtime_error`の派生) を投げます。コンストラクタ
  **とコピー/ムーブコンストラクタ**がthread-localのpendingスロット
  にエラーをpublishするので、JITのcatch padは飛行中の例外を再検査
  せずにObjectを作れます。isolateが保存済みエラーをコピーで再送出
  する経路があるため、コピーコンストラクタはdefaultedにできません。

`catch`はどちらも言語仕様どおりの`{kind, message, line, col}` Object
に変換します。deferは`Environment` (`env->deferred`) に積まれ、
`run_deferred`があらゆる離脱経路でLIFOに実行します。defer自身が
throwした場合はそれが伝播し、そのスコープの残りのdeferは破棄されます
(GoではなくSwiftのルール)。スコープ終端のdrop解決は同じ関数の末尾に
乗っているので、deferは常にスコープの資源解放より先に走ります。

## 4. JIT (LLVM ORC)

JITはASTを関数粒度でLLVM IRへlowerし (スクリプトのトップレベル
はモジュール全体をlower)、そのモジュールをORC v2に渡して`-O2`コン
パイルさせます。ホストプロセス内のシンボル (ランタイムヘルパ、アロケ
ータ、BLAS) は、ORCの`DefinitionGenerator`機構を介してJIT済みコー
ドに公開されます。

### ランタイムシンボル解決

ランタイムヘルパ (`culebra_runtime_*`) はリンク時に可視です。macOSで
は既定で可視です。ELF/Linuxでは`-rdynamic`ではなくculebra自身の
Cリンケージ名のホワイトリスト (`cmake/exported_symbols.txt`) を公開し
ます。全部を公開すると静的リンクしたLLVMも公開され、後からプロセスに
読み込まれるライブラリ — MesaのDRIドライバは自前のLLVMをリンクして
いる — がビルド時のものではなくこちらに束縛されてしまうためです。

### インラインキャッシュ

プロパティアクセスはV8 / SpiderMonkey流の**モノモーフィック**な
インラインキャッシュを使います。呼び出し箇所ごとに`{Shape*,
slot_offset}`を持つ専用のICグローバルがあり (`JitPropIC`。書き込み
側は`JitPropSetIC`で、プロパティの可変性もキャッシュします)、fast
pathはランタイム呼び出しを一切出しません — `obj->shape`をロードして
キャッシュ済みの`Shape*`と比較し、ヒットならスロットベクタへ直接
インデックスします。ミス時は`culebra_runtime_object_get_ic`を呼んで
本来のルックアップを行い、ICを詰め直します。

Shapeはプロセス全体でinternされている (`ShapeRegistry`) ので比較は
ポインタ等価で済み、同じ作り方をしたobjectは1つのshapeを共有します。

### namespace ディスパッチテーブル

`include/stdlib_jit.h`は`kNsMethods`を公開します。これは
`(namespace, method)`からJIT呼び出し可能なランタイム関数ポインタへ
のテーブルです。裸のnamespaceメソッド (`Math.abs`、`IO.inspect`) は
codegen時にここで引かれ、汎用ルックアップのオーバーヘッドを回避しま
す。新しいstdlibメソッドを追加するには、ここに1行と対応するインタ
プリタ実装が必要です。`add-stdlib-namespace` skillを参照。

起動時のdebug専用ドリフトチェックが、`kNsMethods`の全メソッドがイ
ンタプリタテーブルに存在することを検証し、2つのバックエンドがサイレ
ントに乖離するのを防ぎます。

### `self` の束縛

`self`の2段階解決（レシーバ優先・字句フォールバック —
language.md §メソッドとUFCS）は、インタプリタでは自然に成立します:
レシーバ呼出はcallEnvに`self`を束縛し、素の呼出は何も束縛せず、
def_envチェーンが外側メソッドの束縛を供給します。JITはこれを構造
的に再現します。全フレームは`self`を2つのスカラABI引数として
受け取り、本体（またはネストしたクロージャ）が`self`を読むフレーム
はそれを自由変数として列挙するので、クロージャは外側フレームの
selfセルを通常の変数と同じ機構でcaptureします。prologueはこれを
マージします: 到着したレシーバがNO_SELFセンチネルならcaptured
セルの値が代わりに入ります（`culebra_runtime_self_merge`）。メソッド
とtraitメソッドのフレームはこの一切をスキップします — それらを
呼び得る全経路がレシーバを渡すため、解析がフォールバックcaptureを
stripし、prologueは引数を直接束縛します（ホットパス無変化）。読み
取りは引き続きセンチネルをguardし、それが「レシーバも外側selfも
ない」をinterpと同じ読み取り箇所のNameErrorに変えます。

関数値プロパティの値読み（`let m = o.f`）は恒久束縛です:
`culebra_runtime_bind_method_value`はObjectレシーバ上のTAG_FUNC
viewをown slot・protoメソッド・static・ctorの別なく
`[receiver, method]` thunk（interpの`_wrap_method_with_this`の
双子）で包みます。thunkは後続の呼出が渡すレシーバを無視して
（releaseして）自分の束縛を使うため、束縛メソッドの付け替えでは
再束縛されません。introspection系の消費者（callback arity検査）は
逆にthunkをunwrapして下のメソッドを判定します — 共有thunkの
fn_ptrは自前のparam metadataを持たないためです。再帰ハンドル
`fn`も同じモデルです: 直接の`fn(...)`呼出はフレームのマージ済み
selfを渡し直し、値読みはフレームごとに一度だけ束縛ラッパを実体化
します（`culebra_runtime_fn_handle`、owned slotにキャッシュ）。
これによりinterpの「wrapperがfn」と同じく、escapeした`fn`も
レシーバを保持します。

### HOF fusion

`range(n).filter(p).map(f).take(k).collect()`はfusableなチェーンと
して認識され、`p`と`f`をinlineした単一のカウンタループへlowerさ
れます。パターンはパーサ実行後のAST shape上でマッチされ、JITはその
後IRでタイトなループをemitします。インタプリタはfusionしませ
ん。そのイテレータチェーン実装もlazyですが、ステージごとに小さな
ラッパを確保します。bodyに`return` / `defer`を含むcallbackは
inlineされず（本物のcallee frameが必要なため）、runtime helper
経路を通ります。

### iterator 専用メソッドの descriptor table

eagerなArray側の腕を持たないlazy combinator / terminal（`take`、
`scan`、`chunks`、`first`、`zip`など）は1つのdescriptor table
（jit.hの`kIterMethods`）からemitされます。1行がruntimeシンボル、
receiverゲート、引数の形、シンボルが呼び出し位置を取るか、結果の
ラップを指定します。オペレータの追加は「表1行 + runtime fn +
interpラムダ」で、runtime側の上流転送は従来どおり
`tools/check_iter_wiring.sh`がゲートします。receiverディスパッチが
特殊なもの（`iter`、`enumerate`、文字列ソース系）は手書きのままです。

### eager/lazy 両対応メソッドの descriptor table

ArrayとiteratorプロトコルObjectの両方をreceiverに取るメソッドは
2つ目の表（`kDualMethods`）からemitされます。2つの腕はreceiver
オペランド・runtimeシンボル・結果のラップ方だけが違うので、1行は
シンボルと結果種別を腕ごとに持ちます（`flat_map`はeager側がArray・
lazy側が新しいiterator、`find`は両腕ともout-param経由）。行は
inline fusionのemitterも腕ごとに指定できます。callbackがliteral
lambdaのときに使われ、`map` / `filter`はArray側だけfusionし
（lazy側はfactoryに本物のclosureを渡す必要がある）、`for_each`と
`reduce`は両腕ともfusionします（`reduce`のemitterはseedも
受け取ります）。残りの行の軸は`join`（型検査付き引数のポインタを両腕で
borrowし、シンボルは呼び出し位置を取らない）と`sum` / `max`
（`tensor_op`がTAG_TENSORを第3のreceiver腕として共有の軸なし
reductionに振り分ける。`product` / `min`は2-wayのまま）を
カバーします。

### for-in カーソル

`for x in xs`はArray、Tuple、Set、Objectのキー、Range、`iter`プロパ
ティを持つ任意のオブジェクト、Stringの7面を扱う必要があり、進め方は3種
類あります — 添字による配列走査、UTF-8スカラ走査、`has_next`/`next`の
イテレータプロトコルです。これらは1つのカーソルを共有します。ループ先頭
のタグディスパッチはカーソルを *open* するだけ (kindと、そのkindが必
要とする状態を記録) で、単一のループヘッドがkindでswitchして対応する
advanceを選び、どのadvanceも要素を1つのbodyへ渡します。出口も共有で
す。イテレータのdisposeはカーソルのイテレータスロットでガードされ、
array/stringカーソルはそこをnilのままにします。

throwしうる2つの側 — advanceブロックとbody — を1つのcleanup padが
まとめて覆い、再送出の前にイテレータをdisposeします。この覆う範囲が
§18.5の「ループを抜けるあらゆる例外でdispose」を、`next()` / `has_next()`
のthrow、ループsafepointでのinterrupt、中断中にthrowしたジェネレータ
本体 (登録済みdeferはこのdispose経由でしか走らない) について成立させ
ます。インタプリタの`eval_for`がproducer呼び出しとsafepointをtryで
包むのも同じ理由です。

ループが所有する3つの値 — iterable、プロトコルの導出元、`iter()`が返し
たイテレータ — はいずれも文自身のスコープのスロットです。この順で宣言さ
れるので、LIFOの解体はイテレータを導出元より先に落とします。これが出口
を一様にします。drain・`break`・早期`return`・あらゆるthrow (drain途中
の`next()` / `has_next()`からのthrowを含む) で、通常のスコープ機構が
3つともreleaseするため、カーソル自身のcleanupは`dispose()`の呼び出
しだけに縮みます。以前のイテレータはカーソルの素のallocaで、どの
cleanup padからも見えず、producerがthrowするとループ1回につきイテレー
タが1個strandしていました (Ch.15 §15.5の不変条件3。スロット化がこれ
を回復します)。インライン`reduce`ループのアキュムレータも同じ扱いです。
seedはプロトコルopenの前から最初の反復がbodyスコープへ引き渡すまで
生きているので、construction cleanupに登録し、open・`has_next()`・件数式
のいずれかがthrowしたときにreleaseされるようにしています。

bodyを1回だけemitすることがネストを現実的にします。容器の各アームに
bodyをinlineすると1段ごとに6倍され、3重ネストでは最内bodyが216回
emitされていました。IRは1段あたり約6.4倍に増え、4重ネストの4行プログ
ラムが100万行のIRを生みました。

`for v in a..b`（`by <step>`の有無を問わず）はこの経路を通らず、直接の
Longカウンタで回ります。Rangeオブジェクトもヒープイテレータも、ステップ
ごとの`{done,value}`オブジェクトもありません。両backendがこれを行いま
すが、判断の根拠が異なります — JITはコンパイル時にrange *リテラル* を融
合し、インタプリタは渡されたrange *値* を融合します。したがって変数経由で
ループに届いたrangeは、一方のbackendでは融合され他方ではgeneric経路
を通ります。これが安全なのは、カウンタループがrange意味論の第二実装では
ないからです。境界・stepの符号・inclusive端の補正・2つのエラー (unbounded
range、stepゼロ) はgenericなrangeイテレータと同じデコーダから来ており、
ループbody・スコープ・defer・breakの処理はgeneric経路と共有していて複製
していません。インタプリタ側の利得はループの*入口*にあります。genericカー
ソルを開くとRangeオブジェクトとイテレータオブジェクトで約3µsかかり、外
側の反復ごとに入り直す内側ループでは中身の処理より高くついていました。

### stdlib preamble の splice

いくつかのstdlibモジュール (`Time`、`Term`、`Args`、`Regex`、`Log`、
`Path`、`Canvas`、matcherファミリ、`__Eff`) はC++ でなくculebraで書
かれており、3 backendが1つの実装を共有します。インタプリタはこれらを環
境ごとに遅延束縛しますが、JIT/AOT経路はプログラムが使うものをエントリモ
ジュールのASTへ、ユーザ文の前のstatementとしてspliceします。ソース
テキストへの連結ではなくspliceなのは、ユーザノードがパース時の行・列を
保ち、エラー位置がインタプリタと一致するようにするためです。

どのモジュールをspliceするかはエントリモジュールのパース済みトークン集
合から決まるため、名前は厳密一致です。コメント中や、より長い識別子の一部
としてのモジュール名では引き込まれません。1モジュールあたりJITコンパイ
ル時間で約1秒かかるので、この違いは机上の話ではありません。

モジュールが返すのはただのオブジェクトリテラルなので、それ自体は名前空間
マーカーを持ちません。各backendがモジュールを構築する箇所でマーカーを立
てます — インタプリタは`Environment::resolve_from_lazy`、JIT/AOTは
`_jit_namespace_get_or_build` (子isolateも同じ関数で再構築します)。両者
とも`lazy_namespace_static_name` (shared.h) を参照するので経路がドリフト
することはなく、このマーカーによって未定義メンバーの読み取りはC++ 製名前
空間と同じ`AttributeError`になります。`Path`が意図的にこのリストに無い
のは、そのモジュールが返すのがクラスであり、クラスのプロパティmissは
`nil`を返すためです。

インタプリタと異なり、JITが生成するコードはheap値 (Object、
Array、Func、Set、Tensor、Cell、String) を`shared_ptr`ではなく、手書
きでemitしたretain/release IRを通じて管理します。その規律 — 所有
権をどう追跡するか、tracingバックストップがRCの回収できないものを
どう回収するか、リーク/二重解放を場当たり的な修正ではなく構造的に不
可能化した方法 — がCh.13〜15の主題です。

## 5. AOT codegen

`culebra build foo.cul -o foo`は、モジュールグラフ (Ch.10) をウォー
クし、到達可能な各トップレベルをLLVM IRへlowerし、non-PICな`.o`
をemitし、そのオブジェクトとランタイムアーカイブを最終リンクのため
にシステムC++ コンパイラへ渡します。

### tree-shaking

モジュールグラフとASTが合わさって、到達可能なトップレベル名の集合を
与えます。ランタイムヘルパ (~450個) は機能グループごとに分割され、
ユーザープログラムから静的に参照されるグループのみがリンクされます。
`inspect`を使う "hello world" はIOとLongプリンタを引き込み、それ以外
は何も引き込みません。

### ランタイムアーカイブ (base + 機能別)

- `libculebra_rt.a` — baseランタイム。機能のchokeはすべて
  **weak-symbol stub** なので、ここから呼べるコードはtensorカーネル、
  OpenSSL、zlib、SQLite、ウィンドウ/GPUフレームワークのいずれにも
  到達しません。ただし*参照*が無いわけではありません: httplibのTLSと
  gzipのコードはchokeが生きているかに関わらずこのアーカイブに入るので、
  `--gc-sections` / `-dead_strip`が必ず捨てるセクションにOpenSSLとzlib
  の未解決シンボルが残ります。セクションを回収する前にシンボルを解決する
  リンカはそれを報告してしまうので、WindowsとLinuxのWebview軸は
  呼びもしないOpenSSL/zlibをバイナリにリンクします。
- 機能ごとに1アーカイブ。それぞれがweak stubを上書きするstrong
  chokeを持ちます:

  | アーカイブ | ゲートするnamespace | 引き込むもの |
  |---|---|---|
  | `libculebra_rt_tensor.a` | `Tensor` | cpp-tensorlib。macOSではAccelerate + Metal |
  | `libculebra_rt_http.a` | `Http` | cpp-httplib + 静的OpenSSL |
  | `libculebra_rt_compress.a` | `Compress` | zlib |
  | `libculebra_rt_sqlite.a` | `SQLite` | vendoredなSQLite amalgamation |
  | `libculebra_rt_canvas.a` | `Canvas` (窓ありビルド) | raylib + SDL3 |
  | `libculebra_rt_scene.a` | `Scene` | raylib + SDL3 |
  | `libculebra_rt_webview.a` | `Webview` / `Desktop` | OSのWebViewフレームワーク |
  | `libculebra_rt_wrap.a` | ラップしたC++ クラス | ラップ先ライブラリの依存 |

`culebra build`は常にbaseをリンクし、ASTがそのnamespaceを参照した
ときのみ機能アーカイブを **force-load** します — strong chokeがweak
stubを上書きします。したがって未使用の機能は、そのアーカイブも外部
ライブラリもリンクしません。これは2^NのマトリクスではなくN+1の
アーカイブです。

このゲート契約は**外から**壊れやすい: vendoredライブラリが自前の
ランタイムフックを経由せずデバイスアロケータを無条件に呼ぶと、その参照が
全バイナリにコンパイルされ、`Tensor`に一切触れないプログラムがリンク
できなくなります。submoduleをbumpしたら毎回AOT込みのフルゲートを
回すこと — diffが小さくても契約は壊れえます。

### Linux -no-pie

LLVMの`TargetMachine`はnon-PICな`.o`をemitします。Ubuntuの
`gcc`は既定でPIEリンクを行い、"failed to set dynamic section
sizes" で失敗します。ドライバはLinuxでリンカに`-no-pie`を渡してこ
れを解消します。

### クロスコンパイル

`--target=<triple>` + ユーザー提供の`--sysroot=`と`--rt-lib=`。
LLVMの`AllTargets*`コンポーネントがホストの`culebra`ドライバにリ
ンクされているため、任意のLLVMサポートtriple向けにemitできま
す。ランタイムアーカイブ自体はターゲット向けにビルドされている必要が
あります — バンドルされたsysrootはまだありません。

## 6. 文字列 / Unicode

### 現状

文字列は内部的には`std::string` (UTF-8) です。バイトインデックスは
`std::string::operator[]`、スカラー反復はcpp-unicodelibの`utf8`
namespaceを使ってコードポイントを1つずつウォークします。

`split`、`replace`、`trim`などのメソッドは既定でバイトレベルで動作し
ます。`size()`はバイト数を返します。スカラー数を数える`length()`は
**存在しません** — スカラーや書記素クラスタの数え上げはイテレータ
(`code_points()` / `graphemes()`) を通すので、O(n) のコストが呼び出し側
で見えます。ユーザ向けの説明は`guide.ja.md` §4.2です。

### JIT/AOT 表現: インライン長さヘッダ

インタプリタの`std::string`は自身の長さを保持するため、埋め込まれた
NULは普通のバイトです。JIT/AOTバックエンドはそれに一致させなければ
なりませんが、`JitValue`は1つの`{tag, i64}`スロットです — 長さは
値に載せられないため、heap/`.rodata`オブジェクトに存在します。
`TAG_STRING`のpayloadは、長さ前置バッファのバイト列を指します:

```
[ uint64_t len ][ bytes... ][ '\0' ]
                 ^ TAG_STRING data points here; len at data[-8].
```

これはBSTR / Zigのsentinel-slice / CPythonの形です。長さが正典であ
りO(1) で読める (`_str_len`) ため、埋め込みNULは普通のバイトです。
末尾のNULは保持され、NULを持たない文字列でもそのままC API (パス、
`%s`) に渡せます。`{ptr, len}`ディスクリプタは不採用でした — hot
pathに間接参照を追加し、借用名に対してsurfaceごとの確保が発生し、
`TAG_STRINGVIEW`のレイアウトと衝突するためです。

不変条件: すべての`TAG_STRING`はヘッダ付きです。生産者は
`_culebra_heap_str` (ランタイム) と`emit_str_literal` (`.rodata`の
`ConstantStruct`) のみです。リテラル文字列とheap文字列は1つのレイ
アウトを共有するため、読み手は起源で分岐しません。String値として現れ
る借用shape名 (objectキー、`class` / variant / enum名) は
`_intern_str`を通してヘッダを得ます。`_str_len`のdebug `assert`
が、`TAG_STRING`に誤タグ付けされたヘッダなしポインタを捕捉します。

`String`/`StringView`は参照カウントを持ちません — RCの
release-to-zeroではなく、tracingバックストップのみが回収します
(Ch.13〜14で理由と仕組みを扱います。Ch.13の「traced-only」の注記を参
照)。

### StringView、StringLike、lazy graphemes

3つとも実装済みです。

**`StringView`** は他の文字列のバイトに対する借用で、同じバイト/スカラー
APIと独自の`type_of`名を持ちます。パラメータ専用ではなく**第一級の値**
です — ObjectやArrayに格納でき、返り値にでき、slice元の束縛より長生き
できます。バッキングを生かし続ける方法はbackendごとに違いますが、
どちらもviewがdangleしない形です:

- **interp** — `Value::StringView`が`StringViewPayload
  { shared_ptr<const std::string> source; string_view sv; }`を持ちます。
  `shared_ptr`が所有権のエッジで、複数のviewが1つのsourceを共有します。
- **JIT/AOT** — `TAG_STRINGVIEW`はヒープ上の`JitStringView
  { const char* ptr; uint64_t len; const char* owner_base; }` (24バイト。
  codegenが長さをoffset 8で読むので`ptr`@0 / `len`@8は固定) を指します。
  `owner_base`はバッキングStringの登録済みGC baseポインタで、バイト列が
  GC管理外 (リテラルの`.rodata`、intern済みの名前) のときは`nullptr`です。
  文字列はtraced-onlyなので、sweep中にバッキングをrootするのはこの
  エッジです (`_jit_gc_enumerate_children`)。

生成元は`slice` / `split` / `split_iter` / `view` / `iter` / `graphemes`で、
大きな文字列を反復してもステップごとのコピーが発生しないのはこのためです。

**`StringLike`** は型ではなく注釈タグです。`x: StringLike`は`String`と
`StringView`の両方を受けます (`Value::is_stringlike`)。多重ディスパッチの
特異度は注釈でスコアリングするので、「文字列のどちらの形でも」を表す
ディスパッチタグとしても機能します。

**`graphemes()`** はcpp-unicodelibのgrapheme breakテーブルを使った、
拡張書記素クラスタ (UAX #29) のlazyイテレータです。1クラスタ分の
`StringView`をyieldするので、クラスタ単位の走査もステップごとの
アロケーションなしで回ります。

残るコスト: `StringView`に対する`contains` / `starts_with` / `ends_with`
は呼び出しごとにNUL終端の一時コピーをmaterializeします。他のStringと
同じくtracing backstopが回収するのでリークではありませんが、多数のviewを
ホットループで回す場合は先に`.to_string()`で1度だけ実体化してください。

## 7. Regex

### ライブラリ選択

エンジンは`vendor/cpp-regexlib` — culebraのために書き、その後
単一ヘッダの独立ライブラリとして切り出したものです。RE2は検討の上
見送りました: 1つの
stdlib namespaceのために重いvendor依存になり、書記素クラスタ
マッチングを後付けするのは自前エンジンを書くより多くのコードになる
ためです。

### エンジンモデル

- **構造的に線形時間。** マッチングはleftmost-first (Perlの
  alternation / quantifier優先度) で、lazy DFAが走り、意味論の
  フォールバックとしてThompson-NFAのPike VMが走ります。どちらも
  backtrackしないのでcatastrophic backtrackingは起こりえません —
  backreferenceが非対応なのも同じ設計判断の帰結です。
- **マッチ単位はコードポイントではなく拡張書記素クラスタ**: `.`は
  ユーザーが1文字と感じる単位を消費するので、絵文字のZWJ列に対する
  `.`はクラスタ全体にマッチします。オフセットは元のUTF-8 subjectへの
  バイトオフセットで、常にクラスタ境界に落ちます。コードポイント単位も
  選択できます。
- Unicodeテーブルはヘッダに生成済みなので、エンジンは何もリンクしません。

### surface area

`Regex.compile(pattern, flags?)`がRegexオブジェクトを返し、
`re.test/match/find/find_all/split/replace_all`がそのメソッドです。
`Regex.escape`はリテラルをクォートします。`re"..."`リテラルは、flagsを
インライングループとしてパターンに畳み込んだ`compile`の糖衣です。

MatchはただのデータObject — `{value, start, end, groups, named}`で、
オフセットはバイト、名前付きキャプチャは`named`の下です。この形は
`stdlib_interp.h`で組み立てられJITがミラーするので、どのbackendでも
同じように読めます。

## 8. Tensor

### TensorImpl

`Tensor`は`shared_ptr<TensorImpl>` — Opでタグ付けされたautograd
テープのノードで、その値は`tl::array` (vendoredな`cpp-tensorlib`の
配列ハンドル) です:

- `op: Op` + `inputs: vector<shared_ptr<TensorImpl>>` — テープの辺
- `shape: TensorShape`、`dtype: Dtype` (F32のみ)
- `value: tl::array` — 構築のされ方に応じて、遅延グラフノード /
  zero-copyビュー / materialize済みバッファのいずれか
- `grad`、`requires_grad`、`is_view`

循環は構造上できません: `inputs`は入力方向にしか向かわず、`grad`は
常に入力を持たないmaterialize済みの`Const`なので、テープはRCだけ
で回収されます。

ビュー (transpose、reshape、slice) は`is_view`を立ててbaseバッファ
を共有するのでzero-copyのままです。ビュー越しのin-place書き込みは
baseを壊すため、`tensor_inplace_binop`が拒否します。

### カーネルのルーティング

culebra側がautograd (VJP) 規則・`TensorNoGradGuard`・言語バインディング・
broadcast規則を持ち、`cpp-tensorlib`側が遅延グラフ・peephole fusion・
`eval` (トポロジカル)・デバイスバックエンド・buffer poolを持ちます。
評価はすべて`tensor_eval_node`のchoke pointを通るので、バインディング
層に触れずにエンジンを差し替えられました。

デバイスバックエンドはCPU (AVX2 / NEONカーネル。macOSではBLAS形状の
カーネルをAccelerateが担当) とGPU (macOSはMetal、Linux / Windowsは
CUDA) です。AOTビルドは対応するランタイムアーカイブを拾います。バイナリ
単位のゲーティングは`TL_RUNTIME_HOOKS`のopt-inを通るので、Tensorを
使わないプログラムはGPUフレームワークもカーネルもリンクしません。

デバイスの既定はエンジン側ではなくculebra側のものです。`tl`はCPUで
始まり、最初のテンソル生成時に`tensor_rt_bootstrap`が`auto`へ切り替え
ます。load時でなくそこで行うことでTensorを使わないバイナリは無変更の
まま済み、3つの`use_*()`が立てる`tensor_device_chosen`フラグが、
最初のテンソルより前にプログラムが選んだデバイスをbootstrapが上書き
しないようにしています。

ビルド時の手順を持つのはCUDAだけです。`nvcc`が
`kernels/tensorlib_cuda.cu`をPTXにコンパイルし、`bin2c`がそれを
`cuda.h`の埋め込むバイト配列に変換します。CUDA自体はリンクしません —
ドライバは`dlopen`（Windowsは`LoadLibrary`で`nvcuda.dll`）でロード
され、PTXはそのドライバがJITするので、リンク依存は`libdl`だけです。
`CULEBRA_TENSOR_CUDA`が`AUTO`既定なのはこのためで、ビルドには`nvcc`
が要る一方、出来たバイナリはGPUもドライバも無い環境でそのまま動きます。

このスイッチはドライバと`culebra_rt_tensor`に必ず揃えて適用し、core
archiveには付けません。前者2つはtensorlibのバックエンド本体を実体化
するので、揃っていないとinterp/JITとAOTで別デバイスに解決されます。
core archiveのweak choke (`tensor_eval_node`・`tensor_gpu_available`) は
`tl`をodr-useしないので、Tensorを触らないバイナリからバックエンドも
埋め込みPTXも外れたままになります。

### Broadcast

標準的なNumPyスタイルのbroadcasting: shapeは右揃えされ、欠けた次元
はsize-1、size-1次元は伸張します。broadcast軸でstrideをゼロに調整
し、汎用のn次元ループで反復することで実装します。

### lazy shape (計画中のチューニング)

reduction (軸に対する`sum`、`mean`) は、reshape-then-reduceが要求さ
れると現状では中間バッファをmaterializeします。これらをfusionする
lazyなshape-graphパスは、Tensorの定常状態チューニングの候補です。

### dtype

F32のみ。F64は削除されました (2026-07): Metalには存在せず、コンシュ
ーマ向けNVIDIA GPUでは1/32〜1/64の速度で走るため、CPU専用dtype
になってしまうからです。`Dtype` enumは、将来のBF16ストレージ型のた
めのseamとして残ります。スカラーの入口/出口点 (`.item()`、
`.sum()`、`.to_array()`) は`Float`をsurfaceします。

### GPU

GPU専用の型はありません。同じ`Tensor`がどちらのデバイスでも動きます —
バッファを所有しどのデバイス上にあるかを知っているのがculebraではなく
`tl::array`だからです。`Matrix` / `GTensor`として別プリミティブに分ける
案は`TNode`が生の`shared_ptr<float[]>`を持っていた頃の計画で、
`cpp-tensorlib`の採用でその理由が消えました。

選択はプロセスグローバルで実行時に切り替えられます — `Tensor.use_cpu()` /
`use_gpu()` / `use_auto()`、およびbackendがビルドに含まれ到達可能かを返す
`Tensor.gpu_available()`。デフォルトの`use_auto`は演算ごとに問題サイズで
選びます (小さいテンソルはカーネル起動コストに負けるため)。

## 9. HTTP

### ライブラリ選択

cpp-httplib (vendored) は、ブロッキングのHTTP/1.1、SSE、WebSocketを1
つのヘッダで提供します。TLSは静的リンクのOpenSSL (macOSはHomebrewの
`openssl@3`、Linuxはディストリのdevパッケージ) で、`CULEBRA_ENABLE_HTTP`
オプションの背後にあります。async/awaitは採用*しません* — 並行はスレッド
経由です。

### なぜ async/await でなくブロッキング + スレッドなのか

- インタプリタ型の動的言語をまたぐasync/awaitは、我々が狙う規模で
  は、わずかな利得のために大きな意味論的追加 (colored functions、
  executorモデル) になる。
- cpp-httplibはasyncを強制せずに必要なもの (SSE、WS) をすべてカバー
  する。
- 数千同時接続というスケール上限は、Culebraが位置づけられているワー
  クロード (CLIツール、小規模サーバー、ホスト埋め込み) には十分。

### surface area

```
Http.get / post / put / delete / head / request   # 1 回きりのクライアント
Http.client(base_url, **opts)                     # セッション (keep-alive, cookie)
Http.server()                                     # -> srv.get/post/... + listen
Http.sse(...) / Http.ws(...)                      # ストリーミングクライアント
```

`Http.server()`はルーティング付きのサーバーオブジェクトを返します。
`listen`はブロックし、`listen_async`はワーカープール上で走らせます
(`Desktop` facadeはasync側を使います)。どちらの側もブロッキング +
スレッドで、asyncにはなりません。

## 10. モジュールシステム

### ローダ

`ModuleLoader` (`include/module_loader.h`) はinterp / JIT / AOTが
共有します。したがって3者は同じ評価順・同じキャッシュ・同じI/O /
パース / サイクルエラーを観測します。ローダ自身は何も評価せず、パース
済みモジュールを返して各backendに歩かせます。

`load_recursive`は`import`文をたどる深さ優先探索です:

1. パスを正準化する (`resolve_module_path`。全コンポーネントがこのキー
   で一致しないとregisterとlookupがすれ違う)。
2. そのパスが進行中スタックにあればサイクルエラー、既にロード済みなら
   キャッシュ済みインデックスを返す。
3. ソースを**ヒープ確保した** `shared_ptr<string>`に読み込む。囲む
   `LoadedModule`がmoveされても`data()`が生き残る必要があるため —
   ASTのトークンはそのバイト列への`string_view`です。
4. パース → `validate_module` → そのファイルのimportを抽出し、自分を
   記録する**前に**各依存へ再帰する。こうすると依存が低いインデックス
   に並び、結果のベクタがトポロジカルソート済みになります。

探索後、全ロード済みモジュールに`lint::check_module`を走らせます。
健全な静的診断はどのbackendが評価するより前にプログラムを中断する —
これが診断をbackend非依存にしています。

### サイクル検出

進行中の`stack_`がそのままサイクル検出器です。まだスタック上にある
パスに再入すると、サイクルを明示した`ImportError`を投げます。SCCの
専用パスはありません — importグラフのDFSなら無償で得られます。共有の
第3のファイルを介してリファクタしてください。

### モジュールのスコープ

各依存は自分のスコープで評価され、トップレベル束縛は`export`が集めた
名前を除いて非公開のままです。集められた名前は1つのimmutableな
export Objectになり、import側が選んだ名前に束縛されます。エントリ
モジュールは最後に評価され、呼び出し側のスコープを共有するので、その
トップレベルは起動した側から見え続けます。

### なぜ明示的な import なのか

未解決識別子からグラフを導出する方式は試して取り下げました。明示的な
文は、読者にとってもツールにとっても、そのファイルの依存を知るために
見る場所が1箇所で済みます。`culebra lint`の未使用import警告 (と
安全な`--fix`) が曖昧さなく出せるのも、AOTのバンドラとtree-shakerが
推論ではなくパース時に確定したグラフで動けるのも同じ理由です。

## 11. ビルドと vendor

### vendor ツリー (`vendor/`)

| ライブラリ | 目的 | リンケージ |
|---|---|---|
| `cpp-peglib` | PEGパーサ (Ch.2) | header-only |
| `cpp-linenoise` | REPL行エディタ | header-only |
| `cpp-unicodelib` | Unicodeテーブル (scalar、grapheme、case) | header-only |
| `cpp-embedlib` | 静的アーカイブをドライババイナリに焼き込む (`cpp_embedlib_add()`) | header-only |
| `cpp-regexlib` | Regexエンジン (Ch.7) | header-only |
| `cpp-httplib` | HTTPスタック (Ch.9) | header-only (+ TLS用の静的OpenSSL) |
| `cpp-tensorlib` | Tensorエンジンとデバイスバックエンド (Ch.8) | header-only |
| `raylib` + `SDL` | `Canvas`と`Scene`のウィンドウ / 入力バックエンド | ソースからビルドする静的アーカイブ (下記のキャッシュ付き) |
| `webview` | `Webview` / `Desktop`のネイティブWebViewウィンドウ | header-only (+ OSのWebViewフレームワーク) |
| `sqlite` | `SQLite` namespace用のSQLite amalgamation | in-treeでコンパイル |

OpenSSLとOS提供のフレームワークを除き、すべての依存はheader-onlyか
vendoredなソースからのビルドなので、`culebra build`は自己完結型の
バイナリを生成します。

### CMake 構造

- `CMakeLists.txt` (トップレベル) — `culebra`ドライバ、任意のLLVMリ
  ンケージ、base + 機能別のランタイムアーカイブ、embedテストを定義。
- `vendor/cpp-embedlib/cmake/cpp-embedlib.cmake` — `libculebra_rt.a`
  とその機能別アーカイブ (`libculebra_rt_tensor.a`、`_http`、
  `_compress`) をドライバに焼き込むための`cpp_embedlib_add()`を提
  供。
- `tools/compress_asset.cc` — 埋め込み前に各アーカイブをdeflate圧縮
  （ドライバ内で33.8 MBのところ6.9 MB）。圧縮するのはアーカイブだけ
  です: 埋め込みバイトの約95% を占め、かつ展開は`culebra build`時に
  fingerprintあたり1回で起動パスから外れているため。grammar blob・
  stdlib preamble・埋め込みdocsは通常実行で読むので、同じ取引は
  約1 MBと引き換えに起動レイテンシを払うことになります。

機能オプションはnamespaceとそのアーカイブの両方をゲートします:
`CULEBRA_ENABLE_JIT` (LLVMリンケージ。offならドライバは ~23 MBで
LLVM依存なし。onなら ~82 MB)、`CULEBRA_ENABLE_HTTP`、
`CULEBRA_ENABLE_SQLITE`、
`CULEBRA_ENABLE_WEBVIEW` (既定ON。GTK4 / WebKitGTKのdevパッケージ
が無いLinuxでは自動的に無効化。要るのはヘッダだけで、エンジン自体は
ウィンドウ生成時に`dlopen`する)、`CULEBRA_ENABLE_CANVAS_WINDOW` (macOS
とWindows、およびSDL3のビルド依存が入っているLinux（Webviewと同じ
ようにprobeする）では既定ON。環境変数
`CULEBRA_CANVAS_WINDOW_DEFAULT=OFF`はジョブ内の全configureでこの既定
を反転させる。CIはこれでopt-outしている)、そして窓ありのopt-in
`CULEBRA_ENABLE_SCENE`。

窓ありの2つのnamespaceは同じvendored SDL3 + raylibの静的ライブラリ
をリンクします。これらは自分自身のソースとターゲットプラットフォームに
のみ依存し、culebra側のbuild type / LTO / JIT設定には依存しないので、
`~/.cache/culebra/deps/<platform>-sdl<rev>-raylib<rev>/`に一度だけイン
ストールされます (rootは`CULEBRA_DEPS_CACHE`で変更可能)。同じキーに
解決されるbuild dirやworktreeはすべて再ビルドせずにリンクし、
submoduleをbumpすれば新しいキーの下に入るのでstaleな成果物を拾うこ
とはありません。coldビルドはstaging dirにインストールし、
`cmake/publish_dep.cmake`がそれをrenameして置くので、キャッシュエン
トリは原子的に出現し、別worktreeの並行coldビルドが書きかけのアーカ
イブを読むことはありません。

### 依存ポリシー

- パッケージ管理される依存よりもheader-onlyのvendorライブラリを優
  先する。リポジトリは、C++23コンパイラ (とJIT用に任意でLLVM) 以外
  のシステムパッケージなしでclone-and-buildできるべき。HTTP機能の
  OpenSSLだけが唯一の例外。
- `vendor/`はcommitごとにpinされたgit submodule。clone直後は
  `git submodule update --init --recursive`を先に走らせないとビルド
  できない。(`sqlite`と`webview`は単一ファイルなのでsubmoduleでは
  なくコミット済みソース。)
- 新しいvendorライブラリの追加には、この章にWhyエントリが必要。

## 12. テストランナー

`culebra test [paths...]` (`include/test_runner.h`) は、ディレクトリ
引数を`test_*.cul`で走査し、ファイル引数はそのまま採用します。実行中
だけ`test` / `@test` / `@parametrize`をambient globalとして登録
します — matcher一族は言語レベルのglobalなので注入は不要です。
フラグは`--filter` (名前の部分一致)、`--bail [n]`、`--list`、
`--reporter json`。

レポータは2つ (`Reporter::Default` / `Reporter::Json`)。JSON側は
NDJSON — 1イベント1オブジェクト — を吐き、テスト自身のstdoutは
ストリームに混ぜずイベントの中に取り込みます。これが出力を機械可読に
しています。

**fixtureは名前による依存性注入です。** テストの位置パラメータは
周囲の環境に対して解決されるので、スコープ内の任意の`fn`がdecorator
なしでfixtureになります。解決はテストごとにメモ化され (直接参照と
推移参照が同じインスタンスを共有し、テストをまたぐと作り直し)、fixture
の循環は連鎖を添えた`CycleError`で検出されます。

doctestランナー (`include/doctest_runner.h`) がもう半分です。markdown
から` ```culebra ` の fenced ブロックを走査し、ブロック先頭の
`# doctest:` ディレクティブを読み、`# =>` / `# => |` マーカー (throw 期待は
`# !!`) から期待 stdout を組み立てます。
現在解釈されるのは `skip` のみで、`compile-only` と backend フィルタは
予約済み — それらを持つブロックは通常どおり走ります。各ブロックは
新しいインタプリタで走り、ランナーは interp 専用です。

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
  エラーメッセージ、チェックのタイミング/順序を変えてはならない。
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
で保たれます。ジェネレータも同
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
  が模倣するジェネレータ機構から継承されます。プレーンな (エフェクトでない) fn
  も `__Eff.perform_direct` 経由で操作を `perform` でき、エフェクトフル
  な呼び出しは `effect fn` の本体に閉じ込められません。

computation オブジェクトとドライバは return-tag プロトコルを共有します。
各 `_step(rv)` は、何が起きたかをドライバへ伝えるタグを返します。

```
0 = DONE      self._eff_valにcomputationの結果が入る
1 = SUSPEND   self._eff_op / self._eff_argsがperformを表す
2 = DELEGATE  self._eff_delegateがdriving対象のサブcomputation
```

2つの構文は変換時に — 対称的に、どのバックエンドも誤コンパイルしない
よう — 拒否されます: 制御フローの条件やiterableの中の`perform`、およ
び外側の束縛をキャプチャする入れ子の`handle`です。

この機能は完全にsource-to-sourceなので、そのコストはバックエンドの複
雑さではなく、パーサが噛み砕く生成ソース量として現れます。
`CULEBRA_TRANSFORM_STATS=1`は各変換がemitするculebraソース量を報告
します (`=2`はlowering後のソースそのものを出力します)。

## 17. Net: 生ソケット

### ロジックの置き場所

`include/net.h`は`http.h` / `sqlite.h` / `proc.h`と同じ形の値中立
コアです。`Value` / `JitValue` / GC型に依存しないので、
`stdlib_interp.h`と`stdlib_jit.h`が互いを引き込まずにincludeでき
ます。バックエンド側は値のマーシャリングと`IoStatus`の解釈だけを行い
ます。

フレーミング (`read` / `read_line` / `read_exact` / `read_all`) は、
2つのアダプタではなくコア側にソケット単位の読み取りバッファとともに
置いています。これが「両方を注意深く書いたからバックエンドが一致する」
と「実装が1つしかないから一致する」の違いです。重複するのはエラーの
*文言* だけ (呼び出し地点ごとの`ctx`文字列) で、そこは
`tests/test_net.cul`のスイープが固定します。

### ブロッキング、ノンブロッキング、Ctrl+C

ソケットは内部的にノンブロッキングで、すべての操作が「`wait_ready()`
してからリトライ」です。理由は2つ:

- poll/selectのready通知はあくまで助言的 — その裏でブロッキング
  `recv()`を呼ぶとタイムアウトを超えて止まりうる。
- `wait_ready()`は100 ms刻みでポーリングして`throw_if_interrupted()`
  を呼ぶので、ブロック中の`accept` / `read`がinterp・JIT・AOTのいず
  れでも協調的`Interrupted`を送出する。JITには文と文の間のセーフ
  ポイントがないため、呼び出し後のチェックでは対称にならない
  (`proc.h`と同じ理屈)。

### ハンドルテーブル

スクリプト側のハンドルは生fdではなくthread-localテーブルへの
`int64`インデックスです。偽造・失効したインデックスは境界チェックで
穏当なエラーになり、決して参照解決されません (`sqlite.h`やFileと同
じ姿勢)。thread-localで正しいのは、ソケットハンドルが
`__nonsendable__`だからです — アイソレートを越えない、つまりスレッド
を越えません。生fdはinternされるまで`FdGuard`が所有するので、
`wait_ready()`から送出される`Interrupted`を含め、どのエラー経路でも
リークしません。

### 並行 serve: 越えるのは fd で、ハンドルではない

`listener.serve(handler, workers:)`は呼び出しスレッドでacceptし、
ハンドラをプールで実行します。各ワーカーが自分のculebraランタイムを
持つ — httplibを除いた`Http.server`と同じモデルです。要点は、ソケット
ハンドルが`__nonsendable__`でthread-localテーブルに住むため、ワーカー
に *渡せない* ことです。境界を越えるのはacceptした生の **fd** で、
ワーカーはそれを自分のテーブルにinternしてハンドルを作ります。不変条件
は例外扱いにするのではなく、保たれたままです。

ハンドラは`serve`の時点で1度だけシリアライズされ (だから非Sendable
なハンドラは最初の接続時ではなくその場で失敗します)、ワーカーごとに再構築
されます。Httpサーバのルートハンドラとまったく同じです。

バックプレッシャーはキューが担います。ジョブキューが満杯の間`submit`が
ブロックするので、速いacceptループがワーカーを際限なく追い越すことは
ありません (実際のバッファはカーネルのlisten backlogです)。抜けるとき —
Ctrl+Cは`Interrupted`の送出として抜けます — プールのデストラクタが、
まだ始まっていない接続を破棄し、実行中のものをjoinします。

### AOT の usage-gating 軸を増やさない

Tensor / Http / Compress / SQLiteと違い、`Net`は外部ライブラリを一切
引き込みません (素のBSDソケット、Windowsでは`ws2_32`のみ)。weak
スタブにするものもforce-loadするものもないので、baseランタイム
アーカイブに同居し、`aot_scan.h`に`aot_uses_net`は増えません。

### プラットフォーム上の注意

Windowsでは`WSAPoll()`ではなく`select()`で待ちます (`WSAPoll`は
ノンブロッキングconnectの失敗を報告しないため)。SIGPIPEはソケット
単位 (`SO_NOSIGPIPE`) か送信単位 (`MSG_NOSIGNAL`) で抑止します。
Emscripten (Playground) ビルドに生ソケットはないので、エミュレートされた
呼び出しで中途半端に動くのではなく、全エントリポイントが最初にその旨を
報告します。
