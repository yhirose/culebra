バイトコードVM: アーキテクチャ
=============================

この文書はculebraの実行エンジンがどう作られているかを説明する。仕様書
ではない — 観測可能な言語契約は[`language.md`](../language.ja.md)が
規範であり、両者が食い違う場合は`language.ja.md`が勝つ。ランタイムの
メモリ管理側 — 参照カウント、LLVM loweringの所有権規律、tracing
backstop — は別の文書[`memory.md`](memory.ja.md)にある。

英語原本は[`vm.md`](vm.md)。

目次
----

1. [概要](#1-概要)
2. [1回の実行のパイプライン](#2-1回の実行のパイプライン)
3. [ランタイム層](#3-ランタイム層)
4. [共有フロントエンド](#4-共有フロントエンド)
5. [バイトコード](#5-バイトコード)
6. [executor](#6-executor)
7. [LLVM lowering](#7-llvm-lowering)
8. [セッションとホスト](#8-セッションとホスト)
9. [ビルド構成](#9-ビルド構成)
10. [検証](#10-検証)
11. [設計判断](#11-設計判断)
12. [経緯](#12-経緯)

---

## 1. 概要

フロントエンドは1つ、消費者は2つ。パーサがASTを作り、バイトコード
コンパイラがそれをレジスタベースでslot解決済みのバイトコードに変える。
そして同じバイトコードを、インタプリタループ（*executor*、既定
エンジン）とLLVM lowering（`--jit`、および事前ビルドバイナリ用の
`culebra build`）のどちらかが消費する。

```text
  .cul source
      │  parse (peglibの文法)
      ▼
     AST
      │  AST→AST変換 (generator、algebraic effects)
      ▼
  FnAnalysis      locals / slot、capture、EH + defer領域           fn_analysis.h
      ▼
  vm::Compiler    →  VmProgram (バイトコードのchunk群)              vm.h
      │
      ├──► vm::Exec        インタプリタループ (既定、`--vm`)         vm.h
      │
      └──► vm::Lowering    LLVM IR → ORC JIT (`--jit`)               vm_lowering.h
                           LLVM IR → オブジェクトファイル (`culebra build`)
```

両方の消費者は1つのランタイム層（`rt.h`）の上で動く: 値表現、演算子・
コンテナ・dispatch・標準ライブラリを実装する`extern "C"`ヘルパー、
スラブアロケータ、コレクタ。executorはそれらのヘルパーを直接呼び、
loweringはそれらへの呼び出しを生成する。2つのレーンが同じヘルパーの
上で同じ命令列を実行するので、このプロジェクトの中心的な要件 —
振る舞い・エラーのkind/文面/位置・チェックの*順序*がすべてのレーンで
一致すること — は手作業で保つ規律ではなく、パイプラインの構造的な
性質になっている。

各要素の置き場所:

| 要素 | ヘッダ | 備考 |
|---|---|---|
| ランタイム値表現とヘルパー | `rt.h`とそれがincludeする`jit_*.h`断片 | LLVM非依存。stdlib全体がこの上に載る（`stdlib_jit.h`） |
| フロントエンド解析 | `fn_analysis.h` | `FuncInfo` / `FnAnalysis`。両消費者が共有 |
| バイトコード形式・コンパイラ・executor | `vm.h` | `Op`、`Chunk`、`VmProgram`、`vm::Compiler`、`vm::Exec` |
| LLVM lowering、`--jit`、AOT | `vm_lowering.h` | LLVMを必要とする唯一のVMヘッダ |
| LLVM codegenコンテキスト | `jit.h` | `struct JIT`: emitter群、所有権ハンドル、ORC/`exec`、object cache |
| セッション（REPL、`culebra test`、embedding） | `vm_session.h`、`vm_repl.h`、`test_engine.h`、`vm_embed.h` | プログラムより長生きするトップレベル束縛 |
| デバッガ | `debug_engine.h`、`vm_debug.h`、`dap.h` | 6問のエンジンインターフェースの上のDAPプロトコル |
| 正典stdlibシグネチャ | `canon_sigs.h`、`canon_sigs.gen.h` | 全レーンが束縛の根拠にするパラメータ名/型/デフォルト |
| コレクタ、スラブ | `jit_gc.h`、`jit_slab.h` | [`memory.md`](memory.ja.md)参照 |

ランタイム断片群の`jit_`という接頭辞は歴史的なものである: これらは
ランタイムが切り出される前は`jit.h`の先頭約1万行だった名残で、いまは
両エンジンが共有している。

## 2. 1回の実行のパイプライン

`culebra prog.cul`（`src/main.cc`）:

1. **ロード。** `ModuleLoader::load_program`がエントリファイルと
   そこからimportされる全モジュールをパースし、`LoadedModule`の
   リストをトポロジカル順（依存先が先、エントリが最後）で返す。
   パースは`parse_with_transforms`を通り、generatorとeffectsの変換
   （§11）がASTに対して走る。
2. **stdlib preambleを差し込む。** `splice_stdlib_preamble`がAST群の
   トークンをスキャンしてstdlib名（`Time`、`Regex`、`Path`、
   `assert_*`ファミリー、…）を探し、プログラムが名指ししている遅延
   モジュールだけのビルダーを持つ合成`<stdlib>`モジュールを先頭に
   付け加える（`stdlib_preamble.h`、ソースは`stdlib_preambles.gen.h`）。
   したがってstdlib名はコンパイル時に、コンパイラがその名前を見た
   時点で解決される。
3. **コンパイル。** `vm::Compiler::compile_modules`が先頭のpreamble
   を剥がし、各依存モジュールをそれぞれのスコープでコンパイルした後、
   エントリモジュールをコンパイルして1つの`VmProgram`にする。
   プログラムのchunk 0はエントリモジュールのトップレベルであり、
   関数リテラル・メソッド本体・コンストラクタはそれぞれchunkを1つ
   追加する。
4. **実行。** `vm::Exec::run(prog)`か
   `vm::run_modules_via_llvm(modules, …)`のどちらか — 後者は同じ
   プログラムをコンパイルして`vm::Lowering`に渡す。

`--vm-dump`は実行せずバイトコードを表示し（`vm::dump(prog)`）、
`--jit --emit-llvm`はloweringされたIRを表示する。どちらも2つの
レーンが食い違ったときに最初に手を伸ばすツールである。

他のエントリポイントは、この4ステップの薄いバリエーションである:

| エントリ | コンパイルに使う | 実行先 | 備考 |
|---|---|---|---|
| `culebra prog.cul` | `compile_modules` | executor | 既定 |
| `culebra --jit prog.cul` | `compile_modules` | lowering (ORC) | `--jit-faststart`、`-O<n>`、`--emit-llvm` |
| `culebra build prog.cul` | `compile_modules` | lowering (オブジェクトファイル) | `libculebra_rt.a`とリンク |
| REPL（ファイルなしの`culebra`） | 入力ごとに`compile_repl_line` | executor | セッションcell、§8.1 |
| `culebra test` | ファイルごとに`compile_session_modules` | executor | §8.2 |
| `culebra test --doc` | ブロックごとに`compile_modules` | executorまたはlowering | ブロックごとに新しい`Runtime` |
| `culebra dap` | `Debug::Step`付き`compile_modules` | executor | §8.3 |
| `vm::Embed`（C++ホスト） | `compile_session_modules` | executor | §8.4 |
| Playground (wasm) | `compile_modules` | executor | §9 |

エンジン選択はリポジトリ内では明示的である: すべての`just`レシピと
CIワークフローは`CULEBRA_REQUIRE_EXPLICIT_ENGINE=1`を設定しており、
この下では既定エンジンを選ぶ起動はabortする（`main.cc`の
`require_explicit_engine`）。これによりテストレーンは自分が何を
測っているかを名指しせざるを得ず、既定の変更が黙ってゲートを別
エンジンに動かすことができなくなる。唯一の意図的な例外は
release-diffゲート（§10.3）で、そこでは既定そのものが対象である。

## 3. ランタイム層

`rt.h`は両消費者が立つ土台である。固定順で、値表現
（`jit_value.h`）、決定的な`drop`のためのowned-resourceスタック
（`jit_owned.h`）、文字列（`jit_string.h`）、中核の`extern "C"`
ヘルパー（`jit_runtime.h`）、固定レイアウトのビューとクラス構築
（`jit_fixed.h`）、マルチメソッドdispatchとキーワード呼び出し機構
（`jit_dispatch.h`）、イテレータプロトコル（`jit_iter.h`）、参照
カウント実装（`jit_mem.h`）をincludeする。どれもLLVMを名指しない。

### 3.1 値表現

値は16バイト: `JitValue { int64_t tag; int64_t data; }`。tagが
`int8`でなく`int64`なのはABI上の理由からである: `{i8, i64}`の戻り値
はCコンパイラとLLVMで異なる形に強制変換されるため、loweringが生成
する呼び出しはヘルパーのCシグネチャと厳密に一致していなければ
ならない。

| tag | ペイロード |
|---|---|
| `TAG_NIL`、`TAG_BOOL`、`TAG_LONG`、`TAG_FLOAT` | 即値（Floatは`double`のビットパターン） |
| `TAG_STRING`、`TAG_STRINGVIEW` | インラインの長さヘッダ+バイト列へのポインタ。参照カウントされずコレクタがトレースする |
| `TAG_ARRAY`、`TAG_TUPLE`、`TAG_SET`、`TAG_OBJECT`、`TAG_FUNC`、`TAG_TENSOR` | 参照カウント付きヒープ構造体（`JitArray`、`JitObject`、`JitClosure`、…）へのポインタ |
| `TAG_UNFILLED`、`TAG_KWREST`、`TAG_NO_SELF` | 呼び出し規約で使うセンチネル（呼び手が省略したデフォルト付きパラメータ、`**rest`マーカー、「receiverなし」） |

参照カウント付き構造体はすべて`int64_t refcount`をoffset 0に持つので、
retainまたはreleaseはどちらのレーンが発行しても1回のメモリ操作で
済む。コレクタ自身のper-objectメタデータはオブジェクトの外側、
アドレスをキーとするレジストリに置かれる（§`memory.md` 6.3）。

VMが必要とするヒープオブジェクトはランタイムの既存のものである:

- `JitCell` — 値を1つ持つ参照カウント付きボックス。captureされた
  変数は1つのcellに住む（LuaのupvalueにあたるC形状）。cellは前方
  参照とREPLセッション束縛の表現でもある（§4.2、§8.1）。
- `JitClosure` — `fn_ptr` + `JitCell*`のcapture配列 + arity。
  executorが構築するクロージャは`fn_ptr`にVM trampolineを持ち、
  `captures[0]`に自分のchunk descriptorを持つ。loweringが構築する
  クロージャはそこにネイティブ関数を持つ。どちらも同じ*JitFn ABI*
  で呼ばれる: `void (JitValue* ret, JitClosure*, int8_t self_tag,
  int64_t self_data, int64_t n_args, JitValue* args)`。
- `Shape`付き`JitObject` — V8スタイルのhidden classと固定slot、
  プロパティサイトのインラインキャッシュ（`JitPropIC`）、非String
  キー用のany-keyサイドマップ。インスタンスは自分のクラスオブジェクト
  への`+1`を持つ（`JitObject::cls`）。
- `JitArray`、`JitSet`、タプル、`JitTensor`。

オブジェクトはper-`Runtime`のスラブアロケータ（`jit_slab.h`）から
割り当てられる: サイズ別に分離され、moveせず、ロックしない —
なぜなら`Runtime`（ヒープ、名前空間キャッシュ、クラス・オーバー
ロードレジストリ、deferスタック、例外キャリアのスレッドローカルな
ルート）はisolateごとに1つだからである。2つのisolateが共有するのは
コードだけで他は何も共有しない（§3.4）。

### 3.2 ヘルパー

コンパイル済みコードとexecutorは`culebra_runtime_*`関数を通じて
言語の意味論に到達する: 算術・比較のdispatch（演算子オーバーロード
込み）、添字・プロパティアクセス、コンテナ構築、マルチメソッド
レジストリ、クラスmetaとインスタンス構築、キーワード引数の解決
（`JitParamMeta`）、イテレータプロトコル、`throw`/`try`の変換と
deferスタック、`drop`の解決。ヘルパーは自分が投げる診断 — kind、
メッセージ、どの位置を報告するかの方針 — を自分で持つので、両レーン
は同じ関数を呼ぶ結果として同じエラーを報告する。

マルチメソッドレジストリは必要なときだけ解決する。テーブルに型注釈
なしのオーバーロードがちょうど1つしかないdispatcherは、それを
monomorphicショートカットとして記録し（`JitMultifnDispatcher`、
テーブル変更のたびに更新）、そのarityが受け付ける位置引数呼び出しは
直接それを呼ぶ。注釈のない素の`fn name`がこれに当たる。候補が1つ
しかない選択のために型スコアリングとtrait走査を払っていた経路である。

ヘルパーは渡される値について小さな所有権契約の集合（borrow、
consume-on-every-exit、transfer）に従う。`memory.md` §4.3が一覧に
している。ヘルパー自身のC++コードには2つのRAII形式が繰り返し現れる:
`JitOwnedVal`（すべての出口で解放される所有ずみ引数）と
`JitUnwindRelease`（ヘルパーがthrowしたときだけ解放する）。

### 3.3 標準ライブラリ

`stdlib_jit.h`が標準ライブラリを束縛する: ネイティブ名前空間
（`Math`、`IO`、`Random`、`FS`、`Net`、`Canvas`、…）を実行時が名前
で解決する名前空間ごとの`kNsRows_*`テーブルとして — 名前空間単位に
まとめてあるのは、AOTバイナリがソースの名指しした名前空間だけをlink
するため（`ns_groups()`、`deployment.md` §4）— 組み込みグローバル
（`to_string`、`type_of`、`range`、…）を`kBuiltinFns`として、値型
メソッドとして。プログラムは`install_jit_stdlib()`を1回呼んでこれを
インストールし、これが`install_extension`（`rt.h`）が登録する
`ExtensionHooks`を埋める — 同じ差し込み口をembedderが自分の名前空間
を追加するのにも使う（`deployment.md` §2）。フックはASTを一切運ば
ない: 拡張への呼び出しが何にemitされるかを決めるのはバイトコード
コンパイラなので、フックはbuildされているモジュールにヘルパーを
*宣言*するだけであり（`declare_runtime`、LLVMを必要とする唯一の
メンバ）、`is_builtin_var`に答えるだけである。

Culebraで書かれたstdlibモジュール（`Time`、`Regex`、`Path`、
`Vector`、assertionヘルパー群、effectsランタイム`__Eff`、…）は、
§2のpreamble差し込みを通じて、ユーザーコードと同じコンパイラで
ソースからコンパイルされる。

すべてのnativeが宣言するパラメータリスト — 名前・型・デフォルト・
keyword-onlyおよびrestマーカー・arity境界 — は1つの生成テーブル
`canon_sigs.gen.h`であり、`canon_sigs.h`経由でコンパイラ（コンパイル
時チェックと`f.params`のintrospection用）、ランタイムのバインダー
（キーワード呼び出しと型付きパラメータエラー用）、AOTアーカイブが
読む。シグネチャの変更はこのテーブルを直接編集する。

### 3.4 isolate

`Isolate.spawn`、`Channel`、`Parallel`は`isolate_core.h`（channel
レジストリ、fan-in、worker pool、teardownのjoin — すべてエンジン
非依存で`SendNode`を語る）と`sendable_jit.h`（値シリアライザ）の
上に構築されている。クロージャはスレッド境界を、自分のコード参照
に位置ベースでコピーされたcaptureを添えて越える: 子は同じ順序で
cellを自分の`Runtime`上に再構築する。`mut`束縛をcaptureしている
クロージャは拒否され（`Chunk::mut_capture_names`がメッセージ用に
名前を運ぶ）、descriptorがネイティブコンストラクタを名指す
クロージャも同様に拒否される。

## 4. 共有フロントエンド

`FnAnalysis`（`fn_analysis.h`）は各関数のAST — モジュールトップ
レベルを含む — に対してコンパイルの前に走り、`FuncInfo`を生成する:

- **localsと自由変数。** どの名前がこの関数自身のものか、どれが
  外側の関数からcaptureされたものか（`free_vars`、並行する
  `free_var_mut`と`free_var_lazy`つき）、自分のlocalsのうちどれを
  ネストしたクロージャがcaptureするか（`captured_locals` — これが
  cellに昇格されるもの）。
- **EHとdeferのフラグ。** 本体が`try`かdeferを持つスコープを含む
  か（`has_eh`）、任意の深さでdeferを含むか（`has_any_defer`。これが
  `return`/`break`/`continue`に保留中のdeferを実行させる）、defer
  を持つスコープと`try`領域（`scope_has_defer`、
  `try_region_has_defer`）。
- **本体自身の名前。** 装飾なしの`fn name`や`let name = fn …`
  リテラルは自分自身の名前をプロローグで束縛されるlocalとして見る
  （`own_name`、`own_name_source`）。宣言スコープのcellをcapture
  するのではない — captureしてしまうと参照カウントの環（cell →
  closure → body → cell）が閉じてしまい、tracing backstopでしか
  回収できなくなるからである。
- `uses_fn`、`uses_args`: `fn`再帰ハンドルやoverflow引数Arrayが
  読まれることがあるかどうか。それらに一度も言及しないフレームが
  何も払わずに済むようにする。

この解析は`is_builtin_var`という1つの述語で注入されており、stdlib
機構から独立している。shadowingは`lint::check_shadow`でチェック
され、`culebra lint`と単一のソースを共有する。

### 4.1 宣言の意味論

宣言はそれが*実行される*時点から効力を持つのであって、文リスト全体
にわたって効くのではない。コンパイラはこれをcellと実行時の
mutabilityビットでモデル化する:

- 文リストのすべての`fn name`、そして後のクロージャが宣言される
  前にcaptureするすべての`let`は、スコープ入口で**事前宣言**され、
  unboundセンチネルを保持する所有cellになる。リストの前の方で
  構築されたクロージャは本物のcellをcaptureする（相互再帰が動く）。
  宣言文が走る前の読み取りは`UnboundErr`（遅延cellの読み取り
  ガード）経由で`NameError`を送出する。
- `if` / `cond`の各腕はスコープを開かないので、複数の腕が宣言する
  名前は**1つのcell**を共有し、どの宣言が走ったかはこの呼び出しの
  事実である: コンパイラは各宣言の`mut`をcellの隣のslotに記録し
  （`Binding::mut_slot`）、bareな書き込みはそれを参照する。
- 宣言はborrowされたcaptureを通して書き込むことは決してない:
  `fn () { let sh = sh + 1 }`は外側の`sh`を代入するのではなく
  shadowする。cellを所有していることが両者を区別する。
- captureされたループ変数はイテレーションごとに新しいcellを得る。

### 4.2 関数の外にある名前

bareなstdlibグローバル（`println`、`to_string`、…）は`NsGet`に
コンパイルされる。これは名前を`culebra_runtime_namespace_get`経由で
`Runtime`ごとに解決し、ふつうの関数値を返す — したがって直接呼び出し
と値経由の呼び出し（`let f = to_string; f()`）は1つのコードパスと
1組の診断を共有する。字句上の束縛は依然として組み込みをshadowする。
stdlibグローバル名へのbareな代入は`ImmutableError`である。

## 5. バイトコード

### 5.1 形式

1命令は固定幅である: `Op`と4つの`int32`オペランド（`Insn { Op op;
int32_t a, b, c, d; }`）。レジスタはフレームのslotであり、それぞれ
`JitValue`である。chunkは必要な数を宣言する（`num_slots`、最大
`kMaxSlots` = 8192 — これはexecutorのフレームが使う機械スタック量
の上限であって、形式自体の上限ではない）。シリアライズは存在しない:
バイトコードはコンパイラと2つの消費者の間のインメモリ契約であり、
どのコミットでも自由に変えてよい。

`VmProgram`は`Chunk`の配列である（chunk 0がトップレベル。関数
リテラルは生成順に予約されるので、ネストしたリテラルは自由に
入り組める）。加えてキーワード解決器が読むchunkごとの
`JitParamMeta`（`param_metas`）と、executorが準備した後に、chunk
ごとに自分のクロージャが指す1つの`VmFnDesc`を持つ。

`Chunk`は`code`の他に以下を運ぶ:

| フィールド | 用途 |
|---|---|
| `consts`、`str_arena` | スカラー値と、ヘルパーがヒープ文字列と同様に読めるようランタイムの文字列形式でレイアウトされた文字列定数 |
| `positions` | run-lengthのサイドテーブル`insn → (line, col)`。エラーパスがこれを読むので、位置は手作業で運ぶのでなく構造的である |
| `slot_names`、`slot_debug` | デバッグテーブル: 常に持つslotごとの名前と、デバッグセッション向けにコンパイルされたときのbindingごとの生存区間（`SlotDebug`） |
| `arity`、`required`、`param_names/types/has_default/mut`、`kwargs_rest_idx`、`first_kw_only_idx`、`cb_min/cb_max`、`variadic`、`return_type`、`multifn_name` | シグネチャ: 呼び手が束縛の根拠にするもの、`f.params`が報告するもの |
| `self_slot`、`fn_slot`、`fn_bound_slot`、`is_getter`、`forwards_args`、`counts_frame` | フレームの形: receiverと`fn`ハンドルの置き場所、getter本体か合成コンストラクタthunkかどうか |
| `capture_src_slots`、`mut_capture_names` | capture list: 各自由変数について、生成側フレームでそのcellを保持するslot |
| `slot_rank`、`slot_cell_rank` | 宣言順（release ladderは新しい方から歩く）と、各slotがいつcellになったか — インデックスは初めは一時値で、後にcaptureされた束縛のcellになることがある |
| `cleanups`、`temp_points`/`temp_slots`、`defer_mark_slot`、`owned_depths` | unwindテーブル（§5.5） |
| `call_argpos`、`kwcalls`、`arity_checks`、`name_tables` | 呼び出しごとの引数位置、キーワード呼び出しのレイアウト、組み込みのarity腕、クラスのメソッド名テーブル |

### 5.2 命令列の中の所有権

参照カウントは明示的である: コンパイラが`Retain`と`Release`を
emitし、消費者はそれを実行するだけである。値移動を担う4つのop —
`LoadConst`（生コピー。定数は参照カウントされない）、`Move`（生
コピー。borrowのために`Retain`と対にする）、`Take`（転送: source
がnilになる）、`Release` — が語彙を作る。すべての式は結果レジスタ
に`+1`を残し、文の一時値は文末のsweepで解放される。

**borrowオペランド契約**がthrowを支配する: 演算子やヘルパーは
オペランドをborrowするので、throwしたときすべてのレジスタは依然
フレームが所有しており、unwindテーブル（§5.5）が唯一の解放者に
なる。これがthrowパスの後始末をサイト単位の判断ではなくテーブル
の巡回にしている理由である。

captureされた変数は6つのop — `CellNew`、`CellGet`、`CellSet`、
`CellRelease`、`BindCapture`、`ImmutErr` — を通じてcellに住むので、
共有される可変状態の参照カウントは命令列の中で可視のままである。
`MakeClosure`は呼び先chunkの`capture_src_slots`から新しいクロージャ
のcaptureを埋める。

### 5.3 opcodeのファミリー

143個のopcodeを分類すると:

| ファミリー | op | 備考 |
|---|---|---|
| 値 | `LoadConst` `Move` `Take` `Retain` `Release` | §5.2 |
| 算術・ビット演算・比較 | `Neg` `Not` `Add` … `Pow` `MatMul` `BitAnd` … `Shr` `BitNot` `Eq` … `Ge` `JumpIfSame` | それぞれ1回のランタイムdispatch。算術opの`d=1`は複合代入のin-place Tensorステップを示す |
| コンテナ | `ArrayNew/Append/Push/Extend/Resize` `TupleNew/Push` `SetNew/Add` `ObjectNew/Set/SetAny/Merge` `RangeNew` `ChkLong` | コンテナは要素の`+1`を吸収する |
| アクセス | `Index` `IndexWr` `IndexCo` `IndexSet` `PropSet` `PropWr` `PropCo` `PropVal` `PropRaw` `HasProp` `NsWrChk` `NilChk` | 添字とプロパティアクセスの読み/書き/coalescing-write形。`PropVal`はgetterを呼ぶこともある素のプロパティ読み取り |
| 呼び出し | `Call` `CallM` `CallKw` `CallRecv` `Ret` `RecEnter` `RecLeave` `ArgsRest` `KwRest` `JumpIfFilled` `ChkArg` `ChkTypeAt` `PosSnap` `BoundPos` | JitFn ABI。`CallM`はreceiver上のメソッド（ユーザー定義または組み込み）を解決する。`RecEnter`はパラメータが束縛された後、フレームを再帰上限に対してカウントする |
| 組み込みメソッド | `MethGate` `ChkParam` `BMeth` `BArity` `CbType` `ArityChk` `BareMethChk` | §5.4 |
| クロージャと名前 | `MakeClosure` `CellNew` `CellGet` `CellSet` `CellRelease` `BindCapture` `ImmutErr` `UnboundErr` `NsGet` `LazyNsReg` `FnHandle` `ModReg` `ModGet` | §4.1、§4.2。`ModReg`/`ModGet`はモジュールのexportオブジェクトを公開/読み取る |
| 関数とクラス | `MultifnReg` `MfSelf` `ClsSelf` `ClassMeta` `ClassObj` `MakeInst` `FieldInit` `BindStatic` `RegGetter` `SelfMerge` `DeriveFn` `RegPack` `EnumVariant` `TraitReg` `TraitDefault` `TraitReset` `ClsParamsChk` `ClsParamsWalk` `WkErr` | `MultifnReg`はランタイムのarity-dispatchレジストリに本体を登録する。クラス宣言はmetaを構築しメンバを登録する |
| パターン | `TypeMatch` `SeqChk` `SeqGet` `SeqRest` `ObjGet` `DestrErr` `JumpIfTag` | `match`の腕とdestructuring。テストが失敗すると次の腕へジャンプし、その時点で何も生きていない |
| 制御フロー | `Jump` `JumpIfFalse` `JumpIfTrue` `JumpIfNil` `JumpIfNotNil` `Halt` | `JumpIfFalse`は共有のtruthiness変換を運ぶ（非Bool条件はTypeError） |
| ループ | `ForPrep` `ForLoop` `ForOpen` `ForNext` `ForDispose` `Safepoint` | Long範囲の数え上げ`for`は融合されたペア。それ以外は12個のslotからなるカーソル（`ForSlot`）でプロトコルを歩く |
| 例外とdefer | `Throw` `RaiseErr` `DeferMark` `DeferPush` `DeferRunTo` `OwnedMark` `OwnedExit` `DropSuppress` `Drop` | §5.5。`OwnedMark`/`OwnedExit`は決定的`drop`のためowned-resourceスタック上でスコープを括る |
| 文字列と出力 | `Fmt` `StrCat` `Disp` `Println` `SetOpPos` | 補間、および`println(<引数1個>)`のpeephole |
| セッションとデバッグ | `ReplCell` `ReplBind` `DbgStmt` | §8.1、§8.3 |

### 5.4 組み込みメソッドはテーブルである

値型メソッド（`'ab'.upper()`、`xs.map(f)`、`it.count()`、…）は1つの
テーブル`bmeth_specs()`で駆動される: `(name, argc)`ごとに1行の
`BMethSpec`が、receiverのタグマスク、引数ごとの宣言型、末尾の
オプション引数のデフォルト、任意のkeyword-onlyパラメータ、idを
持つ。コンパイラはこの行からreceiverゲートとパラメータチェックを
読み取り（`MethGate` → `ChkParam` → `BMeth`）、executorとlowering
はidでswitchする。どのreceiverがどのarityでその名前を解決するかが
`ArityError`とメソッドmissのどちらになるかを決めるので、**specの
receiverマスクはその名前・arityを解決するreceiverの集合と厳密に
一致していなければならない** — マスクの外はすべてmissとして答え
られる。高階な形式（`map`、`filter`、`sorted(by:)`、…）は、
バインダーがパラメータを順に歩くのと同じ順で、まずcallbackパラメータ
を型チェックする。

### 5.5 例外、`defer`、unwind

`try`領域は静的である: `Chunk::cleanups`内のスコープエントリが
`handler`のpcと`caught_slot`を持つ。すべての字句スコープ、ループ
本体、`try`本体、`match`の腕は、閉じる際に1つの`Cleanup`エントリを
記録する（innermost-first順）。そのslot範囲、deferを宣言していれば
そのdeferマークslot、その時点で存在していたcellの数
（`cells_before`）、親を伴う。

`pc`でのthrowはスコープごとに解体される:

1. その文の実行中の一時値（`chunk_temps_at(pc)`。catchする`try`が
   確立するfloorより上）が解放される。
2. 各囲みスコープについて、内側から外側へ: 保留中のdeferをそのマーク
   まで実行し、自分自身のslotを宣言の新しい順で解放し
   （`release_order_by_rank`、通常出口と同じ順序）、`for-in`
   イテレータをそのrungで破棄する。
3. handlerを持つスコープでは、`culebra_runtime_try_translate`が
   例外を分類する: culebraのエラーはエラーオブジェクトとして
   実体化し、ユーザーのthrowは既に値を運んでいる — どちらも
   `caught_slot`に収まり実行はhandlerで再開する。foreignなC++例外は
   unwindを続ける。
4. フレームを去るときはフレーム自身のdeferを実行し、owned領域を
   1回解決し（`culebra_runtime_owned_scope_exit`）、再帰深度を
   減算する。

`defer`本体は0-arityのクロージャであり、ランタイムのグローバルな
LIFO deferスタックに積まれる（`DeferPush`）。マークはフレームごと・
スコープごとに取られ（`DeferMark`）、`DeferRunTo`はそのマークまで
実行する。`try`は本体のfall-throughのdefer実行より前に自分の領域を
終えるので、try本体の正常な出口でthrowするdeferは自分自身の`catch`
から逃れる。プログラム終了はトップレベルの束縛を`drop`を発火させ
ずに解放する（`suppress_frame_drop`、`language.md` §17）。

### 5.6 コンパイラが拒否するもの

コンパイラはコンパイルできない構文に対して`Unsupported`をthrowし、
`compile_unit`はそれを`VmError`に変換する。関数リテラルの内側では
拒否は`compile_fn_chunk`が捕捉し、本体全体がそれをraiseするchunkに
なる。したがってモジュール自体はコンパイルを続け、その構文に実際に
到達する呼び出しだけが失敗する。残る拒否は一握りの構造的な形 —
or-patternの中のbinding alternative、制御フローの条件式の中の
`perform`など — であり、これらはどのレーンでも言語仕様として拒否
される。

## 6. executor

`vm::Exec`はswitch-dispatch型のインタプリタである。`run_frame`は
フレームのレジスタウィンドウを機械スタック上の可変長配列として確保
し（chunkの`num_slots`からサイズを決めるので、小さな関数は小さな
フレームしか払わない）、プロローグでパラメータ・receiver・`fn`
ハンドルを束縛して`dispatch`に入る。このウィンドウがC++スタック上に
あるのはコレクタのためである: 保守的スキャンが登録なしにすべての
レジスタをrootとして見つけられる。

### 6.1 クロージャとtrampoline

executorのクロージャは本物の`JitClosure`であり、`fn_ptr`は
`Exec::trampoline`（またはgetterでは`getter_trampoline` — これに
より`fn_ptr`をキーとするランタイムのgetterレジストリがgetter本体を
見分けられる）である。その`captures[0]`は`VmFnDesc`
（`{program, chunk}`）を保持するcellであり、本物のcaptureはその
後に続く。ネイティブコードはVM関数をloweringされた関数と全く同じ
方法で呼び、より多くを知る必要があるランタイムのヘルパー — キーワード
解決器（`_jit_closure_meta_hook`）、遅延名前空間の再構築器、
sendability検査、native-constructor検査 — は`Exec::prepare`が
インストールするフック経由でdescriptorを読む。

descriptor cellはクロージャに属し、`MakeClosure`ごとに1つである。
chunkのすべてのクロージャで1つのcellを共有する方式も試されたが
誤りであった: 1つのプログラムのクロージャは複数のisolateで同時に
実行されることがあり、`JitCell`の参照カウントは単なる`int64_t`
である — なぜならランタイムは`Runtime`ごとにシングルスレッドだから
である。クロージャ間で何も共有しないことがこの不変条件を保つ。

### 6.2 throw

`run_frame`は`dispatch`を`catch (...)`で包み`unwind`を呼ぶ。これは
§5.5のテーブルを歩き、handlerで再開するかフレームをカウント解除
してre-raiseする。捕捉されなかったエラーはエンジン境界
（`run_prepared`）でフォーマットされる: ユーザーの`throw`はどの
レーンも表示する同じ`uncaught: …`の行になり、位置を持たない
`CulebraError`は最後に公開されたop位置から埋め戻される。

### 6.3 safepoint

ループは`Safepoint`をemitする。これはプロセス全体のwakeフラグ
（Ctrl-Cとisolateごとのcancel）をpollし、`Interrupted`をthrowする。
wasmでは同じpollがコレクタが動く場所でもある（§9）: 閾値超過は
pending flagを立てるだけであり、executorは次の命令境界でcollectを
行う — そこではすべてのフレームの生きた値がスキャンの見えるレジスタ
ウィンドウの中にある。

### 6.4 デバッグ対応

`Debug::Step`でコンパイルすると、ユーザーソースのすべての文が
`DbgStmt`をemitし、これがスレッドローカルな`DbgState::hook`を
呼ぶ。`run_frame`は入口で`DbgFrame`（program、chunk、レジスタ
ウィンドウ、pc）をpushし、すべての出口でpopする。`Debug::Break`は
`debugger`文が必要とするものだけをemitする。通常の実行
（`Debug::Off`）はどちらもemitしない。

## 7. LLVM lowering

`vm::Lowering`（`vm_lowering.h`）は`VmProgram`をchunkごとにlowering
する: 各chunkはJitFn ABIを持つ1つのLLVM関数になり、各レジスタは
mem2regがSSAに昇格させるエントリブロックの`alloca`になり、各`Insn`
はいくつかのIR命令かランタイムへの呼び出しになる。executorがヘルパー
を呼ぶところで、loweringは`struct JIT`上の対応するemitter
（`emit_arith_step`、`emit_comparison_i1`、`value_to_bool`、
プロパティのインラインキャッシュ）を呼ぶ — したがってある構文の
dispatchは1回定義され2回消費される。

loweringを共有する2つのエントリポイントがある: `run_program`は
モジュールを構築し最適化し（`JIT::optimize_module`、既定の`-O2`
パイプライン）、`JIT::exec`経由でORCに渡す — isolate-joinと
teardown-collectのガード、捕捉されなかったエラーの変換はここに
ある。`build_object`は`TargetMachine`のオブジェクトファイルと、
`__culebra_main`を`libculebra_rt.a`内の`culebra_aot_bootstrap`に
渡すC言語の`main`をemitする（`deployment.md` §1）。
`deployment.md`が公開するembedding名 — `JIT::run`、
`JIT::run_modules`、`JIT::build_object` — はこれらの上で定義
されている。

loweringされたプログラムに対してホスト側に登録されるものは何も
ない: キーワード呼び出しが解決の根拠にするパラメータメタデータは
モジュールのグローバルとしてemitされ（`param_meta_global`、それを
必要とする最初の`MakeClosure`サイトで — `CreateGlobalString`は
挿入ブロックを必要とするため）、これが同じloweringをオブジェクト
ファイルに対しても有効にしている。

`--jit-faststart`はIRパイプラインを飛ばしバックエンドの高速パス
を使い（`JIT::apply_fast_codegen`。2つのレベルは一緒に動く）、
`CULEBRA_JIT_CACHE`は`JIT::jit_module_name`（ソースとオプション）
でキーづけられたobject cacheを有効にする。

### 7.1 loweringの中の所有権

loweringのC++は、すべての一時的な`+1`を`JIT::Owned`に保持する。
これは正確に1回消費されなければならないmove-onlyのRAIIハンドル
である。そこから読み出された値は消費されたベーシックブロックに
`Pinned`される。throwしうる呼び出しは`invoke`としてemitされ、
呼び出しの期間だけ生きているハンドルを関数ごとのcleanup slotへ
spillする。`memory.md` §4が全体像である。cleanup padは
`JIT::CleanupPad`で構築され、そのデストラクタがunwindを継続する
edgeをemitするので、領域は継続されずに開かれることがない。
handlerだけが例外を開き（`emit_handler_prologue`）、
`tools/check_eh_balance.sh`がそれをemitされたIR上で検証する。

### 7.2 unwindの形

loweringはexecutorのテーブル（§5.5）をブロック単位で写す:
スコープごとに1つのcleanupステップが、throwサイトで生きている
束縛のためのrungに入り、共有チェーン（`fn.release.3 →
fn.release.2 → … → fn.unwind`）を下る。これにより各slotの解放は
1回だけ存在する。throwが放棄する文の一時値は、異なる集合ごとに
1つのpadを得て、prefixによってrungを共有し、足元で1回だけ
re-raiseする。`try`スコープのステップは、自分の解放の後に例外を
分類する（`emit_classify_tail`）。これはexecutorが使う順序と同じ
である。

landing padに生きたまま入る値はspillされなければならない —
unwinderはcallee-savedレジスタしか復元しないので — したがって
throwする可能性のある呼び出しを多く持つ関数は、`-O2`で退役した
ASTコードジェンにはなかったレジスタ圧を払う（そのスコープslotは
決してSSAではなかった）。これはloweringされたコードがループで
2〜4.5倍速く走るのと同じ事実である: バイトコードのレジスタファイル
がSSAに昇格されている。`tests/test_core.cul`での実測では、`-O2`の
コンパイルは旧コードジェンの約1.2倍で、`-O0`のコンパイルは遅くなら
ない。4通りのpadの形が試され、木にあるものがその中で最良である
（スコープごとに1つのpadはIRを最小化するが、`llc`に30秒かかる
幅700のphiを生む）。

## 8. セッションとホスト

executorは、プログラムのトップレベルの束縛がそれを作ったプログラム
より長生きしなければならない場面、あるいはすべての本体をコンパイル
することがレイテンシしか買わない場面のエンジンである。lowering
レーンにはREPLもデバッガもユニットテストホストもない。

### 8.1 セッション

`vm::ReplSession`（`vm.h`）はトップレベル名ごとに1つのcellを持ち、
unboundセンチネルを保持した状態で鋳造され、GC rootとしてpinされる。
`repl = true`でコンパイルされた1単位（`compile_repl_line`、
`compile_session_modules`）は自分のトップレベル名を`ReplCell`
（ある名前のセッションのcellをロードする）と`ReplBind`（3つの
mutabilityモードで宣言/代入する）を通じて束縛する。これにより後の
入力はそれらを見ることができ、前に構築されたクロージャは後の入力
がそこに格納するものを見る。`ReplCell`は各使用箇所で持ち上げず
再emitされる: 束縛はスコープ全体に及ぶがこの命令はそうではなく、
ある`if`の腕の内側で最初に言及された名前は、別の腕が実行されても
なおロードされなければならないからである。

`vm::Session`（`vm_session.h`）は両方のセッション消費者が必要と
するものを加える: 保持されたプログラム群（クロージャは自分の
プログラムを指すdescriptorを通じてそのバイトコードに到達するので、
セッションが実行したプログラムは生き続けなければならない）、
1回限りのbuilt-in traitsプロローグ、そして**stdlibデルタ** —
セッションは一度に1つの入力しか見ないので、各入力は前のどの入力
も名指していない遅延モジュールだけを登録する。ビルダーを2回登録
すると名前空間の2つ目のインスタンスが鋳造されてしまう。

REPL（`vm_repl.h`）は1行ずつ`Session`に送り込まれるものであり、
最後の文の値はセッションの結果cellから反響される。

### 8.2 `culebra test`

ユニットテストランナー（`test_runner.h`）は9個のメソッドからなる
`TestHost`インターフェースの上でエンジン非依存である — ファイルを
実行する、グローバルを読む、ArrayやObjectを歩く、関数を呼ぶ、現在の
throwを説明する。そして`VmTestHost`（`test_engine.h`）は自分の
`Session`からそれらに答える。`test`と`parametrize`はculebraソース
（`src/preambles/test_ambient.cul`）であり、したがってホストが読み
戻すレジストリはプログラムが構築した普通のArrayである。各ファイルは
セッション単位としてコンパイルされ、これがランナーが実行後にその
ファイルへコールバックできる理由である。値はホスト自身のstoreへの
インデックスとしてインターフェースを越え、使用したテストが終わると
マークまで解放されるので、fixtureの`drop`はそのテストが捕捉した
出力の中で正しく発火する。

doctestランナー（`doctest_runner.h`）はセッションを必要としない:
`(name, code) → {ok, kind, message}`という`BlockRunner`を取り、
`main.cc`がエンジンごとに1つ供給し、各ブロックに新しい`Runtime`を
与える。

### 8.3 デバッガ

`dap.h`はDAPプロトコル、ブレークポイントテーブル、pause/resumeの
状態機械、出力の転送を保持し、`DebugEngine`（`debug_engine.h`）を
通じてエンジンに6つの質問をする: run、frames、variables、has_name、
evaluate、set_variable。`VmDebugEngine`は`Debug::Step`でコンパイル
し、chunkのテーブルから答える: `slot_debug`の生存区間は、`pc`で
停止しているフレームがどの名前を見られるか、その値がどこにあるか
を語る（slot単独では答えられない — 同じインデックスがあるスコープ
では一時値で次のスコープでは束縛になることがあるため）。
`DbgState::frames`がコールスタックである。

すべてのクエリは*停止しているデバッギースレッド上で*、stopフックの
内側から実行される: フレームのレジスタウィンドウはそのスレッドの
機械スタックであり、`Runtime`とコレクタはスレッドごとだからである。
`evaluate`と`set_variable`は式をフレームの束縛に対するREPLの1行
としてコンパイルする（`vm_debug.h`）。これが`set_variable`の
`ImmutableError`がただで手に入る理由である。

### 8.4 embedding

`vm::Embed`（`vm_embed.h`）はC++ホストAPI（`deployment.md` §2）で
あり、自分が実行するスクリプトより長生きする束縛を持つセッション
である。これによりホストはソースを実行してからグローバルを読んだり
関数を呼んだりできる。`vm::Value`は境界を越える値の所有ハンドルで
あり、すべてのretainとreleaseはその内側に留まる。各`Embed`は自分
自身の`ReplSession`を持ち、すべての呼び出しの間それをswap inする
ので、1つのスレッド上の2つのembedは何も共有しない。

## 9. ビルド構成

`CULEBRA_JIT_ENABLED`は「LLVMがリンクされている」ことを意味する。
これは`jit.h`、`vm_lowering.h`、`stdlib_jit.h`の`declare_runtime`
メンバ、AOT bootstrapをガードする。それ以外 — ランタイム層、
コンパイラ、executor、stdlib、セッション — はこれなしでビルド
できるので、LLVMなしのビルド（JITオプションoffの`cmake`、
`just build-no-jit`）はレーン1つの完全なエンジンであり、
`--version`はそのバイナリがどのレーンを持つか（`vm`か`vm+jit`か）
を告げる。`just test-no-jit`は単にリンクを確認するのではなく、
その構成を実際に実行する。

AOTランタイムアーカイブ（`libculebra_rt*.a`、`src/runtime/`）は
ドライバと同じヘッダを、ビルドされたプログラムがリンクするライブラリ
にコンパイルする。loweringが名指す`culebra_runtime_*`シンボル集合
は両方に存在しなければならない（`tools/check_jit_host_symbols.sh`、
`tools/check_rt_archive_tls.sh`）。

Playground（`playground/wasm_main.cc`、`em++`でビルド）はwasm上の
executorである。2つのプラットフォーム上の事実がこれを形作る。
`Runtime`はページごとではなく実行ごとに作られる — 名前空間キャッシュ
がそれを構築したプログラムを指すからである。そして保守的コレクタ
はwasm localsを見ることができない — それらは線形メモリの外側に
生き、`setjmp`はそこには何もspillしないからである — そこで
`gc::kDeferToSafepoint`（`__EMSCRIPTEN__`でのみon）の下では、
inlineでのcollectは一切走らない: 閾値超過はフラグを立てるだけで、
executorは命令境界で`safepoint_collect()`をpollする。そして
helper-to-userのすべての呼び出しは`_jit_invoke`を通り、その
`SafepointUnsafeScope`が、2つのVMフレームの間で中断しているヘルパー
が自分のlocalsに唯一の参照を保持しているかもしれない間、pollを
延期する。すべてをレジスタに保つよう監査されたdispatchサイトは
`_jit_invoke_rooted`を使い、collectableであり続ける。
`just check-playground`はコミット済みのwasmをnode上で走らせ、各
ケースを1つのインスタンス内で2回、ネイティブのexecutorと比較する。

## 10. 検証

エンジン同士、frozenな期待値、そして前のリリースに対して互いを
突き合わせる。すべてのレーンは自分のエンジンを名指しし（§2）、
すべての比較はexit codeを畳み込む — stdoutだけではsegfaultが一致
として読まれてしまう。

### 10.1 レーン間の対称性

| ゲート | 何を比較するか | どこで |
|---|---|---|
| vm/jit対称性 | すべての`tests/*.cul`を`--vm`と`--jit`で、stdout + exit code | `just test-dev`、`just test` |
| isolateスイート | `tests/isolate/*.cul`を両レーンで | 同上 |
| `vm_cases` | `tools/bench/vm_cases/`: 両レーンをfrozenな`expected/`の出力とexit codeに対して、さらに`CULEBRA_GC_STRESS=1`下でも | 同上（`compare.sh`。`--freeze`が意図的な変更を再記録する） |
| doctest | すべてのドキュメントブロックを両レーンで | `just doctest` |
| difftest | 生成されたtemplate-combinatorコーパス（`tools/difftest/gen.cul`、約1万7千ケース）を`--vm`対`--jit`でバイト単位一致 | `just test`（`tools/difftest/run.sh`） |
| AOT | `culebra build`の出力 == テストごとの`--jit`の出力 | `just test` |
| codegenバックエンド | `-O0`と`--jit-faststart`を`--vm`に対して | `just test` |

`misc/run_all_backends.sh`は対称性チェックの単一スクリプト版で
あり、Windows CIジョブが使う。

### 10.2 emitされたIRへのチェック

- `culebra --jit --emit-llvm f.cul | opt -passes=verify` — lowering
  作業の常設チェック。`run_program`も自分が構築するすべての
  モジュールを検証し失敗時にthrowする。
- **IR diffing。** codegenを変えてはいけないリファクタは、
  `tests/*.cul`全体に対する`--jit -O0 --emit-llvm`を前後で比較し、
  stderrを含めてバイト単位で一致することで検証する。
- `tools/check_eh_balance.sh`（すべての`__cxa_begin_catch`が
  閉じられているか。unwind edgeのないrethrowがないか）、
  `tools/check_alloca_discipline.sh`（一時slotがエントリブロック
  に留まっているか — ループ中の非エントリ`alloca`は毎パス
  スタックを伸ばす）、`tools/check_rc_discipline.sh`（`jit.h`内の
  手書きretain/releaseサイトの数は減る一方であるべき）。

### 10.3 前のリリースをオラクルとして使う

両消費者は1つのコンパイラからバイトコードを渡されるので、
コンパイラのバグは両レーンに同じ誤答をさせ、§10.1は緑のままに
なる。独立した第二の実装は前のリリースのバイナリである: 既に
ビルド済みで、固定されており、3つのプラットフォーム向けに
ダウンロード可能である。`tools/difftest/release_diff.sh`は生成
コーパスをbaselineバイナリとこのビルドの両方で実行し、振る舞いが
変わったすべてのケースを報告する。両側とも既定エンジンで、フラグ
なしで実行される。すべての差分は`tools/difftest/release_diff_allow.txt`
にケースラベルに対するglobとして名指しされなければならない —
一覧にない変更はゲートを失敗させ、何にもマッチしない一覧された
パターンはファイルを縮められるよう報告される — これによりこの
ファイルはリリースノートの下書きになる。コーパスが今使っている構文
より前のbaselineはケースごとに処理される（`::: unsupported`）。
comparator自身のセルフテスト（`release_diff_selftest.sh`）があり、
静かな報告は検査済みの比較であることを保証する。CIはmasterへの
すべてのpushで、最新公開リリースに対してこれを実行する。

### 10.4 共有サーフェスのカバレッジ

`just coverage`（`tools/coverage/run.sh`、`-DCULEBRA_COVERAGE=ON`）
は、共有運命サーフェス — `vm::Compiler`、`culebra_runtime_*`
ヘルパー、`JIT`のemitter — のうち、生成コーパスだけが到達し手書き
テストが1つも到達しない関数を測定する。`tools/coverage/corpus_only_coverage.txt`
がその集合をratchetとして保持する: 新しいcorpus-only関数は報告を
失敗させ、このファイルは空である。35分かかる計装ビルドでの計測で
あり、PRごとのゲートの外にある。

### 10.5 メモリ

リークゲート（leak-fuzz、leak-abort、rc-leakバッテリー、GC stress、
assertレーン）はコレクタと一緒に`memory.md` §5〜6で説明されている。

## 11. 設計判断

- **スタックベースでなくレジスタベース。** レジスタは解析が既に
  計算しているフレームレイアウトと、loweringのSSA値に直接対応
  する。インタプリタループは式あたりのdispatch回数が減る。
- **RCは命令列の中で明示的。** 1つのemitter、2つの消費者:
  リークゲートはコンパイラの配置を1回だけ検証し、executorと
  loweringはそれを継承する。代替案 — 各消費者が自分でretainを
  決める — は一致させておくべき配置が2つになってしまう。
- **位置とデバッグテーブルがバイトコードに乗る。** chunkごとの
  サイドテーブルが、エラー位置とデバッガをすべてのemitterが手で
  運ぶのでなく構造的なものにする。
- **バイトコードは内部専用。** シリアライズなし、バージョンなし、
  ディスクに書かれることもない。これが、ある構文が必要とするたびに
  形式を自由に変えられる理由である。
- **generatorとeffectsはAST→AST変換である。**
  `generator_transform.h`は`yield`する関数をイテレータプロトコル
  を実装するクラスに書き換え、`effects_transform.h`は
  `effect fn` / `perform` / `handle`を`__Eff`ランタイム上の普通の
  ソースに書き換える。どちらも制御フローをstate instance上に
  localsを持つflat-dispatchのCPS状態機械を通じてloweringする。
  エンジンはgenerator固有やeffect固有の対応を一切必要としないので、
  構造的に一致する。VMでのフレーム中断化は要件ではなく単純化に
  なるだろう。
- **組み込みメソッドはデータである。** `(name, argc)`ごとの
  テーブル行が、拒否の判断、executor、loweringを1つの定義の上に
  保つ（§5.4）。
- **セッションはcellであって第二の名前解決器ではない。** REPL、
  テストホスト、embedding API、デバッガの`evaluate`はすべて同じ
  機構を再利用する（§8.1）。
- **Culebraで書かれたstdlibはユーザーコードと同じようにコンパイル
  される。** 遅延preambleモジュールは同じコンパイラを通るので、
  stdlibモジュールが同じことを言うユーザーモジュールと違う振る舞い
  をすることはあり得ない。
- **先行研究。** interpreterとJITの両方を持つ成熟した動的言語実装は
  すべてその間でバイトコードを共有している — CPython、Ruby
  (YARV + YJIT)、Lua/LuaJIT、V8 (Ignition + TurboFan)、
  SpiderMonkey。ランタイム値表現は`memory.md` §3〜4に記載されて
  おり、その設計の系譜は`memory.md` §7にある。

受け入れられている既知のコスト: `--jit`は§7.2の理由により`-O2`で
退役したASTコードジェンの約1.2倍の時間でコンパイルする。複数の
遅延stdlibモジュールを名指ししてから何もしないスクリプトは、
tree-walkerより約7ms遅くexecutor上で起動する — preambleが歩かれる
のでなくコンパイルされるからである（実プログラムはそれを上回る —
だからこそpreambleのバイトコードはキャッシュされない）。lowering
レーンにはREPLもデバッガもない。

## 12. 経緯

バイトコードVMは2026年にtree-walkingインタプリタを置き換えた。VM
はインタプリタとAST-walkingなJITと並ぶ第3のエンジンとして参入し、
生成コーパスが乖離を見つけなくなるまで両方に対して差分を取られ、
その後JITのフロントエンドになり（ASTコードジェンは削除された）、
次に既定エンジンになり（v0.3.0）、最終的にインタプリタとその
オラクル群が退役して唯一のエンジンになった（v0.3.1が両エンジンを
持つ最後のリリースである）。§10.3のrelease-diffゲートが、独立した
第二の意見としてインタプリタに代わるものである。`include/vm.h`、
`include/vm_lowering.h`、`include/rt.h`のコミット履歴が移行の記録
を残している。それを始めた設計提案とフェーズごとの知見は、ここで
なくその履歴の中に生きている。
