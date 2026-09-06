バイトコードVM: アーキテクチャ
=============================

この文書はculebraの実行エンジンがどう作られているかを説明する。仕様書
ではない — 観測可能な言語契約は[`language.md`](../language.ja.md)が
規範であり、両者が食い違う場合は`language.ja.md`が勝つ。ランタイムの
メモリ管理側 — 参照カウント、LLVM loweringの所有権規律、tracing
backstop — は別の文書[`memory.md`](memory.ja.md)にある。以下の各ファイルが
どの層に属し、その名前が何を意味してよいかは[`layout.md`](layout.ja.md)に
ある。

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
| ランタイム値表現とヘルパー | `rt.h`とそれがincludeする`rt_*.inc.h`断片 | LLVM非依存。stdlib全体がこの上に載る（`stdlib_rt.h`） |
| フロントエンド解析 | `fn_analysis.h` | `FuncInfo` / `FnAnalysis`。両消費者が共有 |
| バイトコード形式・コンパイラ・executor | `vm.h` | `Op`、`Chunk`、`VmProgram`、`vm::Compiler`、`vm::Exec` |
| LLVM lowering、`--jit`、AOT | `vm_lowering.h` | LLVMを必要とする唯一のVMヘッダ |
| LLVM codegenコンテキスト | `jit.h` | `struct JIT`: emitter群、所有権ハンドル、ORC/`exec`、object cache |
| セッション（REPL、`culebra test`、embedding） | `vm_session.h`、`vm_repl.h`、`test_engine.h`、`vm_embed.h` | プログラムより長生きするトップレベル束縛 |
| デバッガ | `debug_engine.h`、`vm_debug.h`、`dap.h` | 6問のエンジンインターフェースの上のDAPプロトコル |
| 正典stdlibシグネチャ | `canon_sigs.h`、`canon_sigs_table.h` | 全レーンが束縛の根拠にするパラメータ名/型/デフォルト |
| コレクタ、スラブ | `rt_gc.h`、`rt_slab.h` | [`memory.md`](memory.ja.md)参照 |

名前が境界を表す。`jit`はLLVMが絡むことを意味し、その層に属するのは
`jit.h`だけである。両エンジンが共有するランタイムは`rt.h`と、それが
includeする`rt_*.inc.h`断片群になる。`.inc.h`は「独立したヘッダでは
ない」ことを言っている: これらは`rt.h`の本体であり、この層が`jit.h`の
先頭約1万行から切り出されたときにサイズの都合で分割したものである。
1つの`extern "C"`ブロックが4ファイルにまたがるため、`rt.h`が固定順で
includeし、他のどこからもincludeしない。

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
   2つのloweringレーンは、このバイナリが**bake**したモジュールを
   preambleから再び取り出す（`resolve_baked_preamble`）: ビルドが
   各stdlibモジュールを独立したネイティブオブジェクトにコンパイル
   してあり（`culebra_preamble_cc`、モジュールごとに1つの
   `culebra_preamble_<Name>`エントリ、driverと`libculebra_rt.a`が
   持ち運ぶ）、loweringされたプログラムは各エントリの呼び出しで
   始まる — 差し込まれたソースが行うのと同じ`_lazy_ns_register`を
   実行する — ので、モジュールあたり約2万行のIRを抱え込まずに済む。
   executorは差し込まれたソースをコンパイルし続けるので、対称性
   ゲートは毎回bakeされたコードをそれと比較する。
   `CULEBRA_PREAMBLE_SOURCE=1`でloweringレーンも再びソースを
   差し込む。
   この差し替えのあとにも1つだけ、コンパイル時の情報が要る。bakeされた
   モジュールの`@value`クラス宣言である。`parse_baked_value_decls`
   がソースに`@value`を含む各bakedモジュールをparseし
   （コンパイルはしない）、コンパイラがその宣言だけを登録する
   （`register_stdlib_value_decls`）— これがないと§5.3の
   スコープ一括unboxはこれらのレーンでstdlibクラスに対して決して
   発火できない。spliceはinlineする宣言そのものを必要とするからだ。
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
（`rt_value.inc.h`）、決定的な`drop`のためのowned-resourceスタック
（`rt_owned.inc.h`）、文字列（`rt_string.inc.h`）、中核の`extern "C"`
ヘルパー（`rt_runtime.inc.h`）、固定レイアウトのビューとクラス構築
（`rt_fixed.inc.h`）、マルチメソッドdispatchとキーワード呼び出し機構
（`rt_dispatch.inc.h`）、イテレータプロトコル（`rt_iter.inc.h`）、参照
カウント実装（`rt_mem.inc.h`）をincludeする。どれもLLVMを名指しない。

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
  への`+1`を持つ（`JitObject::cls`）。読みサイトのICはヒットの2つの形を
  インラインで答える: 自分のslotと、クラスインスタンスでのメソッド読み
  （データは自分のslot、メソッドは`proto`の先の共有metaにある）で使う
  protoのslot。後者は（受け手のshape、protoのshape）の対としてランタイム
  helperのmiss経路が埋めていたが、IRにこの腕が入るまでは`obj.f()`が毎回
  helperに達していた。
- `JitArray`、`JitSet`、タプル、`JitTensor`。

オブジェクトはper-`Runtime`のスラブアロケータ（`rt_slab.h`）から
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

呼び出しフレームが触るスレッドローカルな状態は、2つのオブジェクトに
まとめてある: `_jit_thread`（`rt_runtime.inc.h`）が呼び出しの発行する
ソース位置すべてと再帰深さを持ち、`_culebra_rt`（`shared.h`）が
このスレッドの現在の`Runtime`と既定`Runtime`のキャッシュを持つ。
これは整頓ではない。Mach-Oにはinitial-exec TLSモデルが無く、
ヘルパーが触る`thread_local`変数は1つごとにdyldの`_tlv_get_addr`
呼び出しを払う — 呼び出し位置の発行だけで12個に触っていた。
1行の関数をループで呼ぶと時間の4分の3がそこへ行っていた。
2つのオブジェクトにまとめる（どちらの持つ値も変えない）ことで、
culebraの呼び出しコストは半分になった。

その後に残っていたのはヘルパーごとの`_tlv_get_addr`1回で、呼び出し1つは
なおそれを4つ踏んでいた: 呼び出し側の位置発行、呼ばれた側の再帰ガードの
enterとleave、owned stackのホットポインタである。コンパイルされたフレームは
いま、エントリブロックで`_jit_thread`のアドレスを1度だけ取り
（`culebra_runtime_thread_state`、JIT側は`thread_state_ptr`）、残りは
そのポインタ経由で済ませる: 再帰ガードはload・上限との比較・storeで、
ヘルパーはthrowのためだけに残す（`RecursionError`はexecutorとバイト単位で
同じ）。位置発行はポインタを受け取る形（`culebra_runtime_set_call_site_at`、
`..._positions_at`）になり、`JitThreadState::owned`が現在の`Runtime`の
owned stackをキャッシュして、`RuntimeScope`の切り替えごとに捨てられ
（`RuntimeTls::on_switch`）、次のフレームの取得で解決し直される。
executorとランタイム自身からユーザーコードへの呼び戻しはポインタ無しの
形のままで、同じオブジェクトを自分で解決する。フレームあたりの参照は
4回から1回になり、上の1行関数の呼び出しは14.7nsから約11nsになった。

### 3.3 標準ライブラリ

`stdlib_rt.h`が標準ライブラリを束縛する: ネイティブ名前空間
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
`canon_sigs_table.h`であり、`canon_sigs.h`経由でコンパイラ（コンパイル
時チェックと`f.params`のintrospection用）、ランタイムのバインダー
（キーワード呼び出しと型付きパラメータエラー用）、AOTアーカイブが
読む。シグネチャの変更はこのテーブルを直接編集する。

### 3.4 isolate

`Isolate.spawn`、`Channel`、`Parallel`は`isolate_core.h`（channel
レジストリ、fan-in、worker pool、teardownのjoin — すべてエンジン
非依存で`SendNode`を語る）と`sendable_rt.h`（値シリアライザ）の
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
  名前は**1つのbinding**を共有し、どの宣言が走ったかはこの呼び出しの
  事実である: コンパイラは各宣言の`mut`をその隣のslotに記録し
  （`Binding::mut_slot`）、bareな書き込みはそれを参照する。この
  bindingがcellになるのはクロージャがその名前をcaptureするか、宣言が
  `fn` / class / enumのときだけで、それ以外は同じセンチネルを保持する
  素のslotになる。loweringはheapからのloadでなく値そのものを見るので、
  そのタグは他のローカルと同じように畳まれる。
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
| `call_targets` | 呼び出し命令ごとに、その呼び先が解決された唯一の関数chunk、そのchunkがレジスタの値とどう対応するか（`Chunk::Reach`）、そして呼び先をcellから直接読むかどうか（§5.3） |

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

148個のopcodeを分類すると:

| ファミリー | op | 備考 |
|---|---|---|
| 値 | `LoadConst` `Move` `Take` `Retain` `Release` | §5.2 |
| 算術・ビット演算・比較 | `Neg` `Not` `Add` … `Pow` `MatMul` `BitAnd` … `Shr` `BitNot` `Eq` … `Ge` `JumpIfSame` | それぞれ1回のランタイムdispatch。算術と比較のopは両Long・両数値の腕をまずinlineで決める（`Neg`はLongとFloatの腕）。算術opの`d=1`は複合代入のin-place Tensorステップを示す |
| コンテナ | `ArrayNew/Append/Push/Extend/Resize` `TupleNew/Push` `SetNew/Add` `ObjectNew/NewShaped/Set/SetAny/Merge` `SlotInit` `RangeNew` `ChkLong` | コンテナは要素の`+1`を吸収する。`SlotInit`はShapeを事前構築したリテラル向けの、スロット番号による`ObjectSet`（§5.3.5） |
| アクセス | `Index` `IndexWr` `IndexCo` `IndexSet` `PropSet` `PropWr` `PropCo` `PropVal` `PropRaw` `HasProp` `NsWrChk` `NilChk` | 添字とプロパティアクセスの読み/書き/coalescing-write形。`PropVal`はgetterを呼ぶこともある素のプロパティ読み取り |
| 呼び出し | `Call` `CallM` `CallKw` `CallRecv` `Ret` `RecEnter` `RecLeave` `ArgsRest` `KwRest` `JumpIfFilled` `ChkArg` `ChkTypeAt` `PosSnap` `BoundPos` | JitFn ABI。`CallM`はreceiver上のメソッド（ユーザー定義または組み込み）を解決する。`RecEnter`はパラメータが束縛された後、フレームを再帰上限に対してカウントする |
| 組み込みメソッド | `MethGate` `ChkParam` `BMeth` `BArity` `CbType` `ArityChk` `BareMethChk` | §5.4 |
| クロージャと名前 | `MakeClosure` `CellNew` `CellGet` `CellSet` `CellRelease` `BindCapture` `ImmutErr` `UnboundErr` `NsGet` `LazyNsReg` `FnHandle` `ModReg` `ModGet` | §4.1、§4.2。`ModReg`/`ModGet`はモジュールのexportオブジェクトを公開/読み取る |
| 関数とクラス | `MultifnReg` `MfSelf` `ClsSelf` `ClassMeta` `ClassObj` `MakeInst` `FieldsInit` `FieldInit` `BindStatic` `SelfMerge` `DeriveFn` `RegPack` `EnumVariant` `TraitReg` `TraitDefault` `TraitReset` `ClsParamsChk` `ClsParamsWalk` `WkErr` | `MultifnReg`はランタイムのarity-dispatchレジストリに本体を登録する。クラス宣言はmetaを構築しメンバを登録する |
| パターン | `TypeMatch` `SeqChk` `SeqGet` `SeqRest` `ObjGet` `DestrErr` `JumpIfTag` | `match`の腕とdestructuring。テストが失敗すると次の腕へジャンプし、その時点で何も生きていない |
| 制御フロー | `Jump` `JumpIfFalse` `JumpIfTrue` `JumpIfNil` `JumpIfNotNil` `Halt` | `JumpIfFalse`は共有のtruthiness変換を運ぶ（非Bool条件はTypeError） |
| ループ | `ForPrep` `ForLoop` `ForOpen` `ForNext` `ForDispose` `Safepoint` | Long範囲の数え上げ`for`は融合されたペア（sinkの`for _ in 0..n`も含む）。それ以外は12個のslotからなるカーソル（`ForSlot`）でプロトコルを歩く |
| 例外とdefer | `Throw` `RaiseErr` `DeferMark` `DeferPush` `DeferRunTo` `OwnedMark` `OwnedExit` `DropSuppress` `Drop` | §5.5。`OwnedMark`/`OwnedExit`は決定的`drop`のためowned-resourceスタック上でスコープを括る |
| 文字列と出力 | `Fmt` `StrCat` `Disp` `Println` `SetOpPos` | 補間、および`println(<引数1個>)`のpeephole |
| namespace関数 | `NsCall` `ToFloat` | 直接の`Math.f(args)` / `to_float(x)`はresolverもclosureも経ずにhelperへ届く（§5.4） |
| セッションとデバッグ | `ReplCell` `ReplBind` `DbgStmt` | §8.1、§8.3 |

**コンパイラが名指しできる呼び先。** その文リストが1度だけ宣言する
`let name = fn …`は、その関数リテラルに束縛されたままである:
`mut`を取らず、再代入もできず、変えうるものは同じスコープでの同名の
2度目の宣言だけである。コンパイラはこれを`Binding::Known`として追跡する
— chunk、そこへの到達のしかた、答えを繋ぎ止めるcell、そして（後述）その
名前への`.new`が入るコンストラクタを、1つのレコードにまとめたものである。
captureはこれを引き継ぐ。borrowするcellはその束縛が所有する当のcellだから
であり、後の宣言がそのcellを書き換えたときは、それを通して解決済みの
呼び出しサイトを取り消す。呼び出し命令ごとの答えは`call_targets`に記録する。

同じレコードは、リテラルから1度だけ書かれた`let`が持つスカラーも運ぶ
（`Known::constant`）。そうした名前をcell越しに読む — captureされた
`DT = 0.016` — と、読みはその定数そのものになり、`CellGet`があった場所に
`LoadConst`が置かれる: どちらのエンジンもcellを辿らず、タグがコードの中に
あるので、loweringのSCCPが下流の算術・比較のdispatchを畳める。plainな
slotにはこの事実は要らない — loweringはそこに何が格納されたかを既に見て
いる。取り消しも同じ1つである: 再宣言は、畳まれた読みをそれが代わりを
務めていたcellの読みへ書き戻す（`const_sites_by_cell`）。
静的なのはコードだけである: closure自体は
レジスタに乗ったままで、そのcaptureは呼び手のものだからだ。両consumer
は同じ3つを飛ばす — 2つのcold probeを伴う`TAG_FUNC`ゲート、
`check_pos_count_cls`の背後にあるパラメータmetaの引き当て（上限は
呼び先chunkのもの、位置引数の個数はサイトのものなので、答えは
コンパイル時に出る）、そして`fn_ptr`の間接である。executorは名指し
されたchunkで`run_frame`に入り、loweringはそのchunkの関数への直接
callを出す。

この形に実行時のフォールバックは無い: サイトは渡された値を検査しない。
それを検査するのはexecutorの解決済み腕にある`assert`で、実際に現れた
closureと突き合わせる。つまりassertレーン（§10、`just test-assert`と
CIのlinux-assertジョブ）が予測をarmedにしたままスイープ全体を回し、
releaseビルドは何も払わない。

`Chunk::Reach`は解決済みサイトが3つの形のどれかを名指しする。3つはフラグ
ではなく択一である: `Direct`が今述べた形、`Mono`と`Guarded`が以下の2つ。
それぞれが実行時に問う質問は高々1つなので、executorはそれを1箇所で問い
（`resolved_entry`、`Call`と`CallM`が共有する）、loweringは質問を持つ形に
共通のダイヤモンドを1つ出す。

**`fn name`の場合。** `fn name`が束縛するのはclosureではなくdispatcher
であり、overloadはランタイムのregistryの中にあるので、body自体は呼び出し
サイトから辿れない。それでも辿れる形が1つある — その文リストがちょうど
1度だけ宣言し、どのパラメータにも注釈が無い名前である。dispatcherの
テーブルに追記するのは同じスコープでの2度目の宣言だけ（`MultifnReg`の
`into`オペランド）なので、この形のテーブルはdispatcherが生きている限り
無注釈のエントリを1つ持ち続け、そのエントリはdispatchが選びうる唯一の
メソッドである。コンパイラはこれを`Known::chunk`＋`via_mono`として記録し、
サイトには`Reach::Mono`の印を付ける。dispatcherはそのbodyを2つ目のcapture
cellに持ち、テーブルが書き換わるたびにこのcellも書き換わる。解決済み
サイトはそれを読み — ロード3段、registryは引かない — bodyをフレームの
closureとして、名指しされたchunkに入る。記録するのは唯一のoverloadが
受け付ける引数個数だけなので、それ以外の個数は今までどおりdispatcherに
届き、その`DispatchError`になる。body自身の名前も同じ扱いである:
`MfSelf`はこのbodyが登録されたdispatcherを返すので、素朴な再帰が直接
callになるのはこれによる。

この形が問うのは、そのcellがまだbodyを持っているかである。payloadが
nullなら、そのサイトが元々取っていた動的な腕に落ちる。上のassertはこの腕も
見ている: 渡されるのはdispatcherが導いたbodyそのものだからである。

**クラス名の後ろのコンストラクタ。** `C.new(args)`はpostfix連鎖の中で解決
できる唯一の一歩である: コンストラクタは頭そのものではなくクラスオブジェクト
を経由して届くので、`head_callee`はこれを一度も見ない。クラス宣言はその名前を
同じ「ちょうど1度だけ書かれる」規則で束縛するので、`Known::ctor`は新しい根拠を
要さずその論証を引き継ぎ、同じcellに繋ぎ止められ、同じ取り消しで消える。
実行時に答えが動きうる場合は付与しない — overload集合はコンストラクタを
chunkではなくdispatcherにするし、コンパイル時マーカー以外のデコレータは
クラスでないものを返しうる。

メンバの中では、クラス名は**レシーバ**から読まれる（`ClsSelf`）ので、chunkは
分かっていても値は実行時の問いである: メソッド値が別のオブジェクトへ移されて
いれば、同じ名前が別のクラスに解決されうる。そうしたサイトは`Reach::Guarded`
になる — chunkは保ったまま、入る前に「この呼び先は本当にそのchunkのclosureか」
を問う。executorは`call_target_holds`（上のassertが使う述語そのもの）で問い、
loweringはclosureの`fn_ptr`をターゲットが名指しする関数と比較する。外れれば
元々取っていた動的な腕へ落ちる。宣言スコープ由来やcapture由来のクラス名は
ガード無しの答えのままである。

**呼び先の借用。** 呼び出しは呼び先を借用する: 呼び手の`+1`が呼び先に
渡ることは、どの腕でもどのレーンでも無い。だから、呼び出しの最中に
値が変わりえないとコンパイラが既に知っている名前なら、呼び先には
レジスタすら要らない。これが`call_targets`の運ぶ3つ目の事実である:
`b`オペランドが指すのは**cell**で、呼び先はその中の値である。読みの
`Retain`と、それに対応する文末の`Release`が両方消え、executorからは
命令が1つ丸ごと消える — そのサイトは`Call`だけになる。この印は
サイトのchunkが解決されたかどうかとは独立に同じ行に乗る: 呼び先を
どこから読むかは命令自身の事実であって、届くコードの事実ではないので、
chunkを取り消す再宣言もこのビットには触らない。

借用が健全である理由は2つあり、どちらも値ではなくcellの性質である。
呼び出しの最中にcellを書く者がいないこと: その束縛は`mut`を取らず、
条件付きでもsession cellでもないので、書き手は宣言だけになる。そして
宣言はこのフレームの文であり、そのフレームは呼び出しの中で止まって
いる。もう1つは、呼び出しの最中にcellが解放されないこと: cellを解放
するのはslotを所有するフレームのladderで、それが同じこのフレームだ
からである。**capture**は前者を満たすが後者を満たさない — cellは
走っているclosureのもので、そのclosureが呼び出しより長生きするとは
サイトのどこにも書かれていない — ので、捕獲された名前は従来どおり
コピーする。判定が`is_cell`ではなく`slot_cell_`なのはこのためである。
`Exec::BorrowWitness`がassertレーン（§10.2）でこの主張を検査する:
呼び出しが終わった時点でcellが同じ値を持っていること、throw経路も
含めて。

### 5.3.1 flatな`@value`連鎖は呼ばれず、コンパイルされる

呼び先を名指しするのは§5.3の解決の到達点だが、コードは静的でも呼び出し
自体は起きる。宣言field全部がmachine scalarな`@value`クラスはもう一歩
先へ行く: `C.new(args)`とその結果へのfield読み取り・同クラスメソッド
呼び出しの連鎖は、**連鎖全体がその形を保つ限り**、インスタンスを1つも
作らず呼び出しも一切出さずにスロットへコンパイルされる。

適格性は`try_inline_value_chain`が命令を1つも出す前に3つの問いで決め、
どれか1つでも`no`なら従来どおりのboxed形をそのままコンパイルする —
半端な状態も実行時フォールバックも無い。クラスはflatなlayoutを持つ
必要がある(`value_flat_layout`): 宣言fieldが1つ以上、全部スカラー、
どれも初期化式を持たない(初期化式はfield-init thunk = フレームを
通る)。各メンバの本体はsplice可能でなければならない(`inline_body_ok`):
straight-lineな制御フロー、入れ子の`fn`/classリテラルなし、そして
コンストラクタ以外では`self.x =`書き込みなし(boxedインスタンスでは
これはfreezeの`ImmutableError`)。そして本体が読む全ての名前は、
呼び出し元でも呼び出し先と同じ意味を持たなければならない:
`self`とクラス自身の名前は`Binding`ではなく小さなinline専用の
レコード(`Compiler::inlines_`)経由で解決し、それ以外の識別子は全部
歩いてパラメータか、呼び出し元でshadowされていないstdlib global・
namespaceであることを確認する — `FuncInfo::free_vars`を使うのは
誤りである。namespaceは変数ではないのでそこには一度も現れない。

spliceそのもの(`emit_inline_body`)は普通の文コンパイラ
`compile_statement`/`compile_expr`をそのまま再利用する — `if`や`for`の
本体が周囲のフレームへ「そのまま」コンパイルするのに既に使っている
機構と同じである。手作りが要るのはパラメータ束縛だけ: 各引数は
既にコンパイル済みの`ExprResult`なので、束縛は単なるslot storeであり、
型付きパラメータは`Op::ChkArg`ではなく`Op::ChkTypeAt`で検査する —
`ChkArg`の失敗経路は実呼び出しがpublishするthread-localから報告
位置を解決するが(`culebra_runtime_param_pos`)、インラインされたサイト
は何もpublishしない。インラインされたサイトでは位置は静的(引数自身の
式)なので、そこに`ChkTypeAt`をスタンプする。2つのruntime helperは
同じformat stringを共有するので、診断文言はどちらの経路でも同一。

連鎖はスカラーの末端までunboxの形を保つときにしかそのマーカーを
渡されない — runが他の消費者に届く形はそもそも構築対象外なので、
`let v = C.new(...)`が値として使われる場合は従来どおりコンパイルされる。
この制約が**reification無しで着地できる理由**である: このコードが
生み出すunboxedな値は、必ず同じコンパイル時機構によって再帰的に
消費される。これは同クラスを構築して返すメソッド(`V2.__add__`が
`V2.new(...)`を返す形)を、別途SROAパスを設けずに畳み込む仕組みでも
ある。

### 5.3.2 その値への演算子もspliceされる

`v + g * DT`はboxedの経路では他のあらゆる算術演算と同じruntime dispatch
経由で`__add__`/`__mul__`に届く。両辺がflatな`@value`クラスなら、
`Op::Add`/`Sub`/`Mul`/`Neg`は§5.3.1の連鎖における`.method()`ステップと
同じ形でdunderをspliceする — 1クラス、1インスタンス、1回のsplice、
やはりオブジェクトは1つも作らない。

演算子fold(`ADDITIVE`/`MULTIPLICATIVE`、および1オペランド・演算子1個
に縮小した`UNARY_MINUS`)は、演算子ごとにではなく何もコンパイルする
前に決める: `chain_resolves_to_class`(§5.3.1の適格性検査を、命令を
一切出さない純粋な先読みとして単独で公開したもの)をまず演算子列
**全体**に対して尋ねる。連鎖中の全演算子が、operand[0]が生成する
であろうクラス上のsplice可能なdunderへ解決しなければならず、これは
splice本体が実際に走らせる3つの検査(そのメソッドが存在する・
適格である・引数を1つ取る)と全く同じ基準で確認する — トークンが
dunder名にマップされるという構文だけの確認ではない。全演算子が
適格だった場合に限り、operand[0]は§5.3.1の連鎖の緩和形
(`allow_trailing_class`。スカラーではなくインスタンスそのものを
保持したまま連鎖を終えることを許す)でコンパイルされる。プログラム
中の他の全てのオペランドは常に従来どおりboxed経路へreifyされる。
このfold全体の先読みがあるおかげでfoldループは無条件になる:
accumulatorが一度unboxされれば、その先の演算子は全て既にsplice
できると証明済みなので、既にunboxed化したaccumulatorを
re-materializeし直す必要が生じる「途中での却下」は決して起きない
— §5.3.1自身と同じ「一度だけ決め、出すか出さないか」という規律を、
連鎖ステップの列ではなく演算子の列に対して適用しているだけである。

本体の末尾が演算子**そのもの**であるメンバ — `__add__`の本体が
自クラスの`C.new(...)`で終わる形 — には§5.3.1に無かった要素が
もう1つ要る: その末尾自身も同じ緩和形でspliceされなければならず、
連鎖がクラス自身を保持したまま終わることを決して許さない通常の
`compile_expr`降下でコンパイルしてはならない。`member_own_tail`は、
呼び出し元が結果をスカラー1スロットではなく複数スロットのrunとして
確保する**前に**尋ねるべきものである: 末尾が`C.new(...)`という**形**
(`member_returns_own`の構文チェック)を持つだけでは足りず、他の連鎖
に対して`chain_resolves_to_class`が証明するのと同じ適格性を通らねば
ならない。これを逆にする — 形だけの確認でrunを確保し、後になって
末尾が実際にはspliceされなかったと分かる — と、最初の1スロット
だけが書き込まれ、それ以降の全スロットはrunのzero-initが残した
ものを保持したままになる。

**spliceが消費するオペランドもrunでよい。** ここまでの記述は
**もう一方の**オペランドがどう届くかを何も言っていない——それは
通常の`compile_expr`降下を通っていたので、`v = v + C.new(...)`は
ステップごとにインスタンスを1つ組み上げ続けていた。accumulatorは
確保をやめたのに、オペランドはやめていなかった。次の2つが成り立つ
ときオペランドもrunのままでいられ、どちらもコンパイル前に安く
問える:

- オペランド自身がunboxedに解決すること——書き込みのRHSが受ける
  のと同じ3形（`C.new(...)`連鎖・`g * DT`のようなfold・否定）
- dunderのパラメータが、runで賄える使われ方しかしていないこと

2つ目は新しい規則ではない。runに束縛されたパラメータはsplice
の間ふつうのunboxed bindingなので、body内の`o.x`はマークされた
ローカル自身の連鎖が通るのと同じ機構でコンパイルされる。適格性の
問い自体も§5.3.3の全スコープwalkが既に答えているものを、文の列
ではなくdunderのbodyについて尋ねるだけである。パラメータが
どこかで裸に読まれる（`o == nil`）か、型注釈を持つ（その検査は
1スロットにtagged Valueを要求する）場合は却下される。

この却下は**refinementであって拒否権ではない**: foldの事前scanは
boxedパラメータでdunderがspliceすることを既に証明済みなので、
ここでの却下は「このオペランドを従来どおりコンパイルする」以上の
意味を持たない。これがscanと発行側を食い違えなくしている——scanの
約束はどちらでも成り立ち、オペランドが最終的にどちらの束縛形を
取るかをscanは知らなくてよい。両側がunboxedになると
`v = v + C.new(...)`のステップは確保をまったく行わず、それが載る
ループ本体は自身の算術に還元される。

この着地後に、consumer側の規則が2つ、レビューではなく実際の
escapeの発見によって厳格化された:

- **foldがspliceしようとする全dunderは`member_own_tail`自体を
  通らねばならない**。正しいarityで存在するだけでは足りない。
  spliceされた演算子の結果のあらゆるconsumer——fold自身の
  accumulator、再代入のcopy-back、後続連鎖のフィールド読み——は
  結果をNスロットのrunとして扱うが、スカラーを返すdunderは
  1スロットしか渡さない（実機で発見: スカラーを返す`__add__`の
  `(a + b).x`は結果の**手前**のスロットを読んでいた）。そのような
  dunderを持つクラスはその演算子を決してspliceせず、boxed経路が
  答える——スカラー結果へのフィールド読みが投げるTypeErrorも含めて。
- **メンバ本体内の裸の`self + o`や`-self`は決して分類されない。**
  `receiver_refs_stay_unboxed`は以前、receiverをオペランドに持つ
  foldや否定を出現場所を問わず受理していた。しかし本体の内側には
  それが生むrunのconsumerが存在しない——メンバのoutスロットがrun
  になるのは`C.new(...)`末尾の場合だけであり、本体内のfold起点連鎖
  にはそれを消費する先読みがなく（`try_inline_fold_chain`は
  operand[0]を`Binding`経由で解決するが、`self`はBindingを持たない）、
  それ以外の場所はすべてtagged Valueを期待する。1つの原因から
  4通りの誤動作として実機で発見: `addp(o) { self + o }`は自分の
  第1**フィールド**を返し、`-self`は生フィールドに`Op::Neg`を
  かけ、`println(self + o)`はそれを印字し、`(self + o).len()`は
  TypeErrorを投げた。そのようなメンバは今は丸ごと却下され、その
  全consumerはboxedでコンパイルされる。

この過程で露呈した、機構そのものとは別の正当性前提が1つある:
メソッド内でのクラス自身の名前(`FuncInfo::own_name`。captureした
cellではなくreceiver経由で読む)は、decoratorが付いた**どのクラス
でも**無効化されていたが、本来区別すべきなのはコンパイラ自身が
読むcompile-time marker(`@value`/`@packable`/`@derive`)と、実際の
decorator(そのbindingはdecoratorの返り値であり、クラスそのもので
ない場合すらある)である。この修正以前は`@value`のメソッドは自分の
クラス名を衛生的に参照できず、つまり§5.3.1が既に説明している
同クラス末尾再帰(`V2.__add__`が`V2.new(...)`を返す形)も、この修正
前にはinlineされ得なかった — boxedフォールバック経由で正しく
コンパイル・実行されてはいたにもかかわらず。

### 5.3.3 ローカルはスコープ全体でunboxedのままでいられる

§5.3.1/§5.3.2がunboxできるのは**1つの式の中で今まさに手にしている値**
だけだ — リテラルの`C.new(...)`連鎖、あるいは演算子foldの
operand[0]自身。`v = v + g * DT`をループの反復をまたいで再利用する
形は、同じ値が1つの文から次の文へ生き延びる必要があり、どちらの
機構もそこには届かない: `v`の裸読みは通常のboxedな`ExprResult`を
生成するので、foldの先読みはoperand[0]として適格なものを何も
見出せず、毎反復reifyする。

`let [mut] v = <unboxed rhs>`はこの隙間を**whole-scope eligibility、
reification不要**という形で埋める: `v`のスコープの文を1つも
コンパイルする前に、`precheck_value_bindings_at`が同じ文リスト内の`v`への
以降の全参照を歩き（`value_ref_ok`）、それぞれがこのsplice機構が
既に理解している形であることを証明する——`v.<field>`/
`v.<method>(...)`という連鎖、スカラーで終わるfold・否定起点のpostfix
連鎖（`(v + g).len()`）、RHSがfold・否定・construction形である
書き込み`v = <rhs>`（`value_write_ok`）、そして複合ステップ
`v += e`/`v -= e`/`v *= e`（desugarした再代入と同じdunderを
spliceする）。宣言自身のRHSも書き込みと同じ3つの形——construction
連鎖・fold・否定——を受け付けるので、`let d = C.new(...) + C.new(...)`
は§5.3.2が組み上げたrunをそのまま持続させる。素の`let`は単一代入の
退化ケースである: 歩みはそれへのいかなる書き込みも分類しないので、
後からの`v = …`——今日ImmutableErrorになるもの——はbindingを却下し、
通常経路が同一のエラーをそのまま保つ。

未分類の出現が1つでもあれば——引数・`println`・比較・captureされた
クロージャ・shadowする再宣言・未対応の複合演算子——スコープ**全体**
が却下される。`v`は宣言の時点からずっと通常のboxedローカルのまま、
この段階が存在しなかった場合と全く同じになる。中間状態も実行時判断
も存在しない: 全参照が既に適格であるか、1つも適格でないかの
どちらかである。特に、`v`を含む裸のfold・否定が分類されるのは
その結果のconsumerがrunを受け取れると証明できる場所——`v`自身への
書き戻し、スカラーで終わるpostfix連鎖、そして§5.3.2のオペランド
規則以降は、**同じクラスの別のbinding**をrunに保つステップの中の
演算子オペランド——**だけ**である。それ以外（`println(v + g)`・
配列要素）ではrunがtagged Valueを期待するコードへ届いてしまうため、
その出現がbindingを却下する——§5.3.2のescape一覧と同じものが1段上
で実機発見された。

この3つ目の文脈が、1つのスコープのbindingを列ではなく**グラフ**に
する。`p = p + v * DT`で`v`がオペランドになれるかは`p`がrunである
ことに依存し、`p`自身の適格性も同じように`v`に依存しうるので、
どんな順序でも決着しない: 宣言順に決めれば前方を指す組はすべて
「不可」になり、それは物理ステップの普通の書き方そのものである。
代わりに全候補を**まとめて**、最大不動点として決める——その文の列
の候補を全部「適格」と仮定して始め、walkが失敗したものを落とし、
何も変わらなくなるまでラウンドを繰り返す。live集合は有限の候補
リスト上で縮む一方なので停止し、生き残らなかった仮定は次のラウンド
でそれに寄りかかっていた出現をまとめて道連れにする——それらの文が
1つもコンパイルされる前に、である。真の循環（`v = v + w * DT`と
`w = w + v * DT`が並ぶ形）は最大不動点にとって問題ではなく、それを
生き残る答えそのものだ: 両方ともrunであり、各々のconsumerが他方の
spliceになる。

同名の候補が2つある場合はどちらも仮定されない——walkが2つの
bindingの出現を区別できないからで、shadowが却下されるのと同じ理由
である。またラウンドは自分の開始位置以降の全候補について答えるので、
その文の列のコンパイルループが後で問い直してはならない: 後から始まる
ラウンドは候補が少なく、そのうち1つに違う答えを出しうるが、その
時点で最初の答えに寄りかかったbindingは既にコンパイルされている。

これは§5.3.1/§5.3.2と同じ規律を、1つの式からbindingの字句的な
広がり全体へ拡張したものだ——`value_ref_ok`は
`receiver_refs_stay_unboxed`（§5.3.2の`self`安全策）の構造的な
近縁種であり、1回のspliceにつき固定1個の名前という制約を、その
スコープがunboxedだと証明済みの任意のbindingへ一般化したものである。
ここから2点が導かれる:

- **クロージャが生きた参照をこの歩みの外へ持ち出すことは決して
  できない。** 入れ子の`FUNCTION`/`CLASS_DECL`/`ENUM_DECL`/
  `MULTIFN_DECL`リテラルは、`receiver_refs_stay_unboxed`が`self`に
  対してそうするのと同じ理由で丸ごとスキップされる——根拠は
  `info_->captured_locals`で、歩みを始める前に一度だけ確認する:
  この関数内のどのリテラルも`v`という名前をcaptureしていなければ、
  その内側にある**この**bindingへの生きた参照はあり得ない（shadowする
  か、そもそも一切言及しないかのどちらか）ので、内側へ踏み込むのは
  shadowが持つ独自の出現をこちらの出現と誤読するリスクしかない。
- **`v`自身がfoldのoperand[0]であるという適格性の問いは、
  `chain_resolves_to_class`の通常の`lookup()`ベースのIDENTIFIER
  caseを経由できない**——あのcaseは`Binding::unboxed_class`を読んで
  答えるが、この歩み全体の目的はそのフィールドをそもそも立てるか
  どうかを決めることにある。`value_write_ok`は同じ問いを
  名前ベースで直接尋ねる（`value_run_ok`）。
  `chain_resolves_to_class`/`fold_resolves_to_class`
  に処理を委ねるのは、その形が明らかに「`v`が自分自身を読む」の
  **ではない**場合（新規construction、あるいは既に実在する別の
  bindingを起点とする連鎖）に限られる——実機で発見: 全てのfold RHS
  を通常の先読み経由にすると`v = v + g * DT`が常に却下された。
  `v`のbindingはこの問いに答えるべき時点でまだ存在し得ないからだ。

**循環だけでなく順序も重要になる。** `@value class`の登録
（`value_flat_layout`）はクラス宣言自身をコンパイルすることの副作用
なので、この歩みは`predeclare_forward_refs`がやるようにブロック
全体を1パスでどれもコンパイルする前に処理することはできない——
自クラスの宣言より後ろに書かれた`let mut v = C.new(...)`は、その
宣言が一度も走っていない時点で問うことになってしまう。
`precheck_value_bindings_at`は代わりに文ごとに1回、それをコンパイルする
のと同じループの中で（`compile_block`、`compile_body_into`、
トップレベルスクリプト、`for`の本体）呼ばれる。これは§5.3.1/
§5.3.2が自分の問いを`compile_expr`の中で遅延して尋ねることで
タダで手に入れている順序保証を、そのまま保つ。**ラウンド**は最初に
候補となった文で開かれ、そこから先の、その時点でクラスが登録済みの
全候補について答える。途中で宣言されたクラスの候補はまだ候補ではなく
誰もそれを仮定できない。その宣言に到達した時点で自分のラウンドを
開く（ラウンドは同じパスの先行ラウンドの答えを問い直さずそのまま
引き継ぐ）。

宣言が一度通ると、`Binding`自身がその答えを運ぶ
（`unboxed_layout`/`unboxed_class`。一度立てたら二度と取り消されない）:
裸の識別子読み（`compile_expr`の`IDENTIFIER`case）はrunをそのまま
返す——inlineフレーム内で`self`がそうするのと同じ無条件のやり方で。
`v.<field>`/`v.<method>(...)`は`try_inline_value_binding_chain`を通じて
spliceされ、これは`self`自身の連鎖（`chain_stays_unboxed`をindex 1
から）と構造的に同一である。再代入（`compile_assignment`）と複合
ステップ（`compile_compound_assign`）はRHSを§5.3.2が既に持つのと
同じfold・否定・連鎖の機構でコンパイルし
（`compile_unboxed_value_expr`、`try_inline_operator`）、その結果の
各フィールドを`v`の永続slotへコピーする——これがこの機構が支払う
唯一のコピーであり、その間の全ての読み（`v`自身の再代入RHSを含む）
はそれらのslotへ直接届く。`v`の**宣言**はコピーを一切支払わない:
新規construction自身の名前付きrun（`alloc_zeroed_run`は文一時値では
なく`alloc_slot`を使うので、既に永続的である）がそのままbindingの
本拠地になる。

計測（`tools/bench/value_inline.cul`の`mut, reused`行を、名前を
辿れないconstruction経由で同じ算術を行う`mut, boxed`行と比較）:
ステップのもう半分に§5.3.2のオペランド規則が効くので、1反復が
かつて支払っていた2つの確保はどちらも消え、ループ本体に残るのは
算術だけになる。`mut, compound`行は同じループを`v += …`で書いた
もので、`mut, reused`と一致することがその行の眼目である。

`tools/bench/value_physics.cul`は、この一連の作業が目指していた
ループそのものを測る——`v = v + g * DT; p = p + v * DT`で、どの
bindingも別のbindingのオペランドなので、3つがまとめて決着するまで
1つもunboxされない。その`@value, boxed`行は、各ベクタが
constructionを辿れない呼び出し経由で届く同一のループであり、
全行が手書きfloatと同じchecksumを出す。それがこのファイルの
オラクルである。

### 5.3.4 境界での再ボックス化

§5.3.3の規則はall-or-nothingである: `v`の出現が1つでもfield/method
連鎖・run-nativeな書き込み・適格なオペランドのいずれでもなければ、
他の全出現が適格だったとしても**binding全体**がスコープ全体で
boxedになる。`takes_untyped(v)`・`[v, other]`・`arr[i] = v`——通常の
呼び出し引数・コンテナリテラルの要素・コンテナへの格納——はまさに
この形である。どれもrunを理解しないが、理解する必要もない。欲しいのは
通常のfrozenなinstanceだからだ。

`Op::ValueBox`（`regs[a] = regs[b..b+N)をmaterialize`。Nはそのサイトの
`Chunk::ValueBoxSpec`から、metaは`regs[d]`）はこの隙間を、§5.3.1〜
§5.3.3のdecide-once機構に一切手を触れず**加算的に**埋める:
`value_ref_ok`は既存の形に加えて、呼び出し引数・コンテナリテラルの
要素（array/tuple/set/object）・コンテナへの格納のRHS（`arr[i] = v`、
`obj.prop = v`）として到達した`name`の**裸の出現**を、却下する代わりに
materialization siteとして印を付ける（`value_boundary_ok`、
`Compiler::materialize_at_`）。`compile_expr`の`IDENTIFIER`case——
`Binding::unboxed_class`に対して既にrunをそのまま返している同じ
チョークポイント——はこの印をまず確認し、代わりに`materialize_run`を
呼ぶ: `culebra_runtime_materialize_value`はrunの既に計算済みの
フィールド値からfrozenなinstanceを新しく組み立てる——`new`なし、
field-initなし、何も実行しない——同じクラスのboxed constructionが
解決するのと同一のShapeを使うので、結果は構造的に区別が付かない。
runそのもののslotはスナップショット**読み**であり消費ではない: 呼び出し
後もbindingは以前と全く同じように動き続ける。`v`自身の適格性
（あるいはそのslot）は何も変わらないからだ。

これは真のreificationより意図的に狭い: materialization siteになるのは
`name`の**裸の**出現だけであり（`send(v)`）、同じクラスにunboxed解決
する任意の式（`send(v + g)`）ではない——fold・否定・constructionが
通常のconsumerへ届く場合は既にそれ自身でboxedな値へコンパイルされる
（これら以外の`compile_expr`のディスパッチは全て
`allow_trailing_class=false`を渡す）ので、`value_boundary_ok`を裸の
識別子より広げても、被覆を増やすのではなく仕事を重複させるだけになる。

materializationには実行時のクラスのmeta objectが要り、
`Compiler::value_meta_cell_`——クラスのASTを、`compile_class_decl`が
そのコンストラクタクロージャのcaptureのために既に作っているcell
（`meta_cell`）へ写像するもの——は自然にchunk単位のスコープになる:
入れ子の`fn`/クロージャは新しい`Compiler`でコンパイルされ、それは
最初は空の独自mapを持つ。これを同一chunk限定の機構のままにする代わりに、
`resolve_captures`は、`ClassName.new(...)`を参照すること自体が既に
要求している**通常の**自由変数captureに便乗させて、参照されたクラスの
meta cellを内側のchunkへ運ぶ: `Binding::Known`が、このcompilerが既に
meta cellを解決できる`value_class`を持つ自由変数それぞれについて、
通常のcaptureの後ろに1つ追加のcaptureが続く
（`CaptureList::meta_classes`/`meta_slots`、`Chunk::capture_src_slots`
へ追加）。呼び出され側はそれを自分自身の`value_meta_cell_`へ同じ
クラスASTの下で束縛する——`materialize_run`から見れば、そのchunkが
自らそのクラスを宣言した場合と区別が付かない。（本体自身の
`value_ref_ok`歩みが境界の出現を見つけた場合にのみ登録するのではなく）
この形で登録するのは意図的な過大近似である: bodyが実際に必要とするか
どうかに関わらずクロージャ生成のたびに1個のcell retainを払うが、
under-captureは決して起こさない。呼び出され側自身の`value_meta_cell_`
が、自分でそのクラスを宣言したかのようにentryを得るので、さらに
入れ子のクロージャも同じやり方でもう一度captureする——再帰専用の
コードなしに、任意の深さの入れ子に届く。裸の出現がこの経路でも
届かないクラス(そもそも自由変数ですらない)は、run自体が
`ClassName.new(...)`という名指し可能な形で構築されたことが一度も
無い場合にしか起こり得ず(§5.3.1自身の適格性が既にそれを要求している)、
単に印が付かないだけで、既存の却下経路がこの機構が存在しなかった場合と
全く同じに走る: crashもしなければ誤った値になることもない。
`tests/test_value_materialize.cul`は、このファイル自身のトップレベル・
それを使う関数の中でローカルに宣言されたクラス・そして**外側**の
関数で宣言され入れ子の`fn`1段・2段から読まれるクラスの全てを検証する
——どれも上記の境界の形すべてでmaterializeし、`--vm-dump`で
`ValueBox`が出ることを出力の一致だけでなくバイトコード上でも
裏付けている。

### 5.3.5 構築はレイアウトを実行前に確定する

ここまでは値をunboxする話だった。この節はboxed経路そのものを、必要以上に
遅くしないための話である。プログラムが作るオブジェクトの大半（リテラル、
配列に保持されるクラスインスタンス）はrunの資格を満たさない。そうした
オブジェクトについて、宣言時点で分かっているのに構築のたびに名前から
導き直していたことが3つあった。

**リテラルのスロットは番号である。** `{class: 'V', x: a, y: b}`は最終
Shapeを一度に確保していた（`ObjectNewShaped`）が、各プロパティの格納は
`object_set`経由で、Shapeの名前列を走査してスロットを探し、名前に依存する
2つの契約検査（well-known名、`drop`）をキー文字列から導き直していた。
Shapeを構築したキー列そのものが各キーの番号を与えるので、キーが全て
識別子のリテラルは番号で格納する: `SlotInit`はスロット番号・プロパティの
可変性・2つの答え（`culebra::prop_key_kind`）を1オペランドに載せ、ランタイム
側（`culebra_runtime_object_slot_init`）は`object_set`のis_init経路が
していた上書きから検索だけを除いたものである。重複キーは今も最初の出現に
解決され、後勝ちで上書きされる。

**クラスの宣言フィールドは1つのレイアウトである。** `class C { x: Float;
y: Float }`は`new`のたびに、フィールドごとに1つの`ObjectSet`、すなわち
レジストリのロック下でのshape遷移を1回ずつ払っていた。初期化子を持たない
フィールドの極大な連続区間は、いま1つの`FieldsInit`である
（`Chunk::FieldLayoutSpec`: 名前、各型のゼロ値、そしてサイトが初回実行時に
キャッシュするShapeの対、すなわち新しいインスタンスが必ず持ってくる
class-onlyのShapeと、その区間が遷移させる先のShape）。初期化子を持つ
フィールドはその場で独立した格納のままなので、宣言順と、初期化子から
見える下のフィールド（`nil`、docs/language.md §10）は変わらない。ただし
宣言型がスカラのときは、その格納の直前に1フィールドだけの`FieldsInit`が
入る。これが、格納が上書きするslotに宣言型を載せる経路である。それ以外の
状態で到着したインスタンスは、ランタイムが保持するフィールドごとの格納に
落ちる。specの中でビットパターンでなくアドレスになるエントリは`String`の
ゼロ値だけなので、`culebra build`はこれを自分が書き出すmoduleへ出し直す
（コンパイル時プロセスへのポインタは、ビルドされたバイナリ自身の実行では
死んだアドレスになる）。

**クラスのmetaは特殊メソッドを表で答える。** インスタンスへの演算子
（`v + w`、`a == b`、`str(x)`）は`_lookup_special`を通ってクラスのdunderに
達するが、これは評価のたびにインスタンス自身の名前列とmetaの名前列を
歩いていた。`build_class_meta`はいま`Special`の全集合（演算子dunder、
`hash`/`cmp`、`__str__`、`__call__`、`__index__`/`__setindex__`）をmetaが
所有する`JitSpecialTable`に一度だけ解決し、クラスが`drop`を束縛しているか
（`methods_drop`）も同時に答える。後者は構築のたびにメソッド名を走査して
訊いていた。インスタンス側の優先順位は保たれる: Shapeは自分の名前列に
特殊名が含まれるかを記録し（`Shape::any_special`）、自分のShapeがそれを
含むインスタンスか辞書モードのオブジェクトだけが名前の走査を取るので、
あるインスタンスへの`c.__add__ = f`は従来どおりクラス側を隠す。同じ理屈で、
メンバが自分のクラス名を読む束縛は、本体内のclosureがそれを捕獲しない限り
cellでなくフレームの素のslotになる（`ClsSelf`でフレームに読み、lazyなslot
同様`UnboundErr`で守る）: 演算子本体の中の`Name.new(...)`は呼び出しごとの
cell確保を払わなくなった。

3つとも、§5.3.1の意味でコンパイル側の「一度決めたら戻らない」形である:
命令が既に言っていることをランタイムが名前から導き直すことはなく、両
エンジンは同じヘルパを呼ぶ。`tests/test_object_layout.cul`が、置き換えた
検索それぞれが決めていた観測可能な振る舞いを固定する。

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

namespace関数も1段上で同じ形を取る。直接の`Math.f(args)` — namespace
識別子がshadowされておらず、keywordがなく、位置引数の個数を正準
シグネチャが認める — は`NsGet` + `PropRaw` + `CallRecv` + `CallM`の
代わりに`nsfn_specs()`のidを持つ`NsCall`にコンパイルされ、両エンジンは
namespace closureのadapterが届いたであろうhelperへ1つのdispatch
（`culebra_runtime_ns_call`）で届く。helperのエラーには呼び出し自身の
位置が渡る。行が持つのは名前とidだけである: arityとパラメータの宣言型は
`canon_sigs_table.h`から読み、型付きパラメータは引数リスト全体が走った後に
その引数の位置で`ChkTypeAt`で検査する — closure trampolineと同じ順で
ある。loweringはFloat系（`sqrt`、`sin`、`exp`、…、`abs`、`atan2`）を
数値タグの検査の背後にinlineする — LLVM intrinsicがあればそれを使い、
これはhelperが呼ぶlibmの当の呼び出しに落ちる — ので、タグが既知なら
呼び出しは1命令に畳まれる。`min`、`max`、`clamp`、`f32`にも腕がある
（両方Longと、いずれかFloat）: helperの規則（`reduce_min_max`、`clamp`、
`_culebra_f32_round`）を比較とselectで綴ったもので、
`tests/test_math_inline_arms.cul`が、emitterにタグの見えないオペランドを
通してhelperとビット単位で一致することを固定する。それ以外の綴り
（`Math?.f`、`let m = Math`、値としての`Math.f`、keyword）は汎用経路と
その診断のままである。

#### 宣言fieldの型と、読みがそれをどう使うか

クラスの*スカラ*宣言field（`Float`・`Long`・`Bool`）の型は書き込みの
たびに検査される（`docs/language.md` §10）ので、型は推測ではなく答えで
ある。それ以外の注釈は`FieldType::Any`で、何も答えない。`Op::PropVal`はそれを`d`で運ぶ:
受け手が「宣言クラスがそのfieldをスカラ型で宣言している名前」のとき、
コンパイラがこれを埋める — クラスを名指す注釈を持つパラメータかローカル、
そのクラス自身のメンバ内の`self`、あるいはクラス型fieldを辿った次の段で
ある（`Compiler::declared_read_tag`、`culebra::class_field_types`、
`culebra::class_field_classes`）。

loweringがそれで何をするかが要点である。読みはslotのペイロードを
**定数**のtagとともに作るので、tag付きの値が負っていたものが全て
emit時に畳まれる: 読みが行うはずだったretain、statement末尾のrelease、
そして消費側のtag検査。`acc += p.x`はdouble phi上の`fadd`になる。
読み2回と乗算は8.2 nsから3.6 nsへ、読み1回は5.8 nsから2.0 nsへ。

入口の検査は値が*名乗る*クラスを見るが、Objectはどんなクラス名でも
名乗れる。そこでemitされる読みは、約束されたtagと実際のtagを比較し、
違えば冷たい`[[noreturn]]`のrejectを呼ぶ — 予測可能な分岐1つで、
2.0 nsのうち0.08 nsである。実行器は同じ命令に同じ問いを立てるので、
偽装された受け手は両レーンで同じ`TypeError`になる。
`tests/test_typed_fields.cul`がこれを固定し、`remove`がスカラ宣言field
を拒否すること（消えうるfieldは契約にならない）も併せて固定する。

#### パラメータの宣言型を、捨てずに保つ

同じ形の1段手前。パラメータの注釈は入口で検査される（`docs/language.md`
§14）が、検査はその答えを捨てていたので、本体は問い続けていた。
`Op::ArgTag`がそれを保つ: emitterはこれを検査の後、かつ「tagが既に
合っていれば検査を飛ばす」`JumpIfTag`のゲートの合流点より後に置くので、
どちらの経路もここに到達する。loweringはtagを定数として書き戻し、
ペイロードには触れない。実行器は値からtagを読むので何もすることがなく、
その腕は配置が正しいことのassertである（`just test-assert`がスイープ全体を
これに対して回す）。

定数が買うのは「switchの代わりに腕1本」である。組み込みメソッド名は受け手の
型をまたいで共有される — `size`はString/Array/Object/Set/Tupleで解決するので
`emit_size_probe`は5腕とその合流を出し、合流を跨ぐ値は全て生存させられる。
tagを差し込むと型が名指す腕1本に畳まれる。`-O3`ビルド、300万反復での実測:
パラメータへの`s.size()`が4.28 → 1.31 ns、`Long`パラメータの`n * 2 + 1`が
2.99 → 0.49 ns。

`Function`は除外する（`__call__`を持つクラスが構造的に満たすので、単一の
tagを名指さない）。`mut`パラメータも除外する（再代入は再チェックされない）。
`tools/checks/check_param_tag_fold.sh`が生成IRで畳み込みを固定し、無注釈の
対照を自分で持つので、何も測っていない状態では通らない。

限界も記しておく。次の一手として明らかに見える案が効かないからである:
**連鎖が畳まれるのは頭のtagが分かっているときだけ**。`trim`は既に結果を
`make_string`で作っているので、組み込みメソッドの戻りtagはloweringの側に、
`CanonSig`が言えるより正確に既にある。頭が不明だと最初の呼び出しがmiss腕を
持ち、その結果はphiになる。定数と非定数のphiは定数ではない。だから
`CanonSig::return_type`を埋めてもここでは何も買えず、攻めるべきは頭だけである。

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
`Exec::trampoline`である。その`captures[0]`は`VmFnDesc`
（`{program, chunk}`）を保持するcellであり、本物のcaptureはその
後に続く。ネイティブコードはVM関数をloweringされた関数と全く同じ
方法で呼び、より多くを知る必要があるランタイムのヘルパー — キーワード
解決器（`_jit_closure_meta_hook`）、遅延名前空間の再構築器、
`mut`捕獲の検査 — は`Exec::prepare`がインストールするフック経由で
descriptorを読む。

そのクロージャが何であるかを知るのに、フックも第二の入口も要らない。
getter本体であることも構築子thunkであることもchunkの性質なので、
`MakeClosure`がchunkから読み取り（`chunk_closure_flags`）、両レーン
ともクロージャの構築子に渡す。クロージャは`JIT_CLOSURE_GETTER`/
`JIT_CLOSURE_NATIVE`を生まれた時から持っている。

以前はどちらもコンパイル済み本体の番地をキーとする横表だった。
そのためこのレーンはtrampolineをもう1つ持たされていた — 解釈実行の
chunkは`fn_ptr`が1つしかなく、番地では2種類のchunkを区別できない。
そして全レーンが「番地は誰かが握っている限り同じものを指す」という
前提の上に立っていた。実際にはそうならない。JITのアリーナは
`Runtime`と一緒に解放され、次の`Runtime`が同じページを受け取るので、
前の`Runtime`で作った項目が、後からそこに載った別物の答えになって
しまう。クロージャ自身が持てば答えは値と一緒にisolate境界も越える。
受け取り側の`Runtime`にある表では、そこまでは追えない。

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
`CulebraError`は最後に公開されたop位置から埋め戻される。interruptは
そのどちらにも届かない: `culebra::Interrupted`という基底を持たない
独立の型なので、エラーを報告するために書かれたハンドラはそれを
捕まえる型を名指せない。スクリプト側への届き方は変わらない —
padの分類はC++の型ではなくpending carrier（§5.5）を通す。

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
でキーづけられたobject cacheを有効にする。この2つは代替関係に
ない: cacheはIR→objectのcompile layerに載るので、ヒットすれば
バックエンドは飛ぶが`JIT::optimize_module`は飛ばない — それは
`run_program`がその手前で既に走らせている。つまり`-O2`は温まった
cacheでもIRパイプラインを毎回まるごと払う。そしてどちらも`--jit`の
起動が安い理由ではない: プログラムが名前で呼ぶstdlibモジュールは
そもそもモジュールの中に無い（§2、焼き込みpreamble）ので、lowering
されるのはユーザーのコードである。`Lowering::build_preamble_object`
は同じloweringをモジュールごとのエントリ名で走らせたもので、ビルド時
に`culebra_preamble_cc`が実行する。`lower_program`は`__culebra_main`を
プログラムが名前で呼ぶ焼き込みエントリそれぞれへの呼び出しで開き、
シンボルはレーンのリンクが供給する — JITはドライバの表から定義し
（`JIT::define_baked_preambles`）、ビルドされたバイナリはアーカイブ
メンバーを引く。

このパイプラインのうち1つのパスはlowering自身のものである。
4つのrefcountヘルパーはそれぞれガードで始まる — 値の2つは
`_is_refcounted_value_tag`とnullペイロードに対して、cellの2つは
nullのcellに対して。したがってそれを満たす定数で到達する呼び出しは
何もしない。emitされたモジュールの中でランタイムは不透明な宣言なので、
LLVMはそれを見ることができない。しかもそうした定数の多くはemitter側
ではなくオプティマイザ自身の産物である。`-O0`ではテストファイル1本が
1つも出さない — その時点でタグはまだレジスタslotからのloadだからで、
パイプラインが確定させるのはSROAがそのslotから昇格した`Long`、素の
呼び出しが渡す`TAG_NO_SELF`のレシーバ、直接参照だと判明した捕獲の
nullのcellである。そこで`JIT::DropSettledRefcounts`はパイプラインの
中でそれらを落とす（LLVMのARC optimizerの縮小版）。各`InstCombine`の
後と、末尾でもう1度 — ループのpeelingとvectorizerはどのpeephole回も
見ないタグを確定させるからである。消えるのはコードだけで、その
呼び出しがしたはずのことは全レーンで何もない。peepholeが黙って
止まっても他の誰も気づかないので、`optimize_module`は生き残りが
ないことをassertする（§10.2）。

これは定数で確定するタグの話である。実行時まで分からないタグこそが
フレーム経路の常態で — 解放される仮引数、レシーバ、スコープが畳む
slot — そのどれもが「この値は何も所有していない」と教わるためだけに
不透明なヘルパーを呼んでいた。そこで解放のemitter
（`emit_value_release`）はヘルパー自身の検査をIRで先に置く:
参照カウントするタグはすべて32bitのマスクに収まるので、所属判定は
シフト1回であり、呼び出しはその後ろに入る。ヘルパーは自分の検査を
持ち続ける — これは速い経路であって契約ではない — し、emitterの時点で
分かるタグはその場で答えるので、確定した解放は畳むべき分岐すら出さない。

その分岐は全解放サイトのIRになる。これが代償の側で、
`tests/test_core.cul`では最適化後のモジュールが3割ほど増え、`--jit`が
最初の命令に到達するまでの時間が1.5倍近くになる。executorは何もlower
しないので、払うのは`--jit`と`culebra build` — スループットのために
選ばれた2レーンである。

パイプライン自前のもう1つのpassはFloatの運ばれ方の話である。`Value`は
Floatを`i64`のペイロードに持つので、ループがそれを運ぶphiは`i64`で、
入ってくる辺は`bitcast double`、その使用側はすぐ`bitcast`で戻す —
整数レジスタと浮動小数レジスタが別のマシンでは、これは毎反復・
ループのクリティカルパス上でのレジスタmove往復である。`InstCombine`は
このfoldを持つが、すべてのincomingが他に使用者のいないbitcastである
場合しか取らない。ここでは同じbitcastを2つ目のphi（解放のために値を
保持する文の一時変数）も使うので、最も効くはずのループで発火しない。
`JIT::PromoteFloatPhis`は代わりにphiの連結成分単位でこれを行い、
入り口には定数とpoisonを許し、bitcast to double以外の使用箇所には
bitcastを1つ戻して払う — LLVM自身のfoldが発火したときに残すのと同じ
境界で、この形ではunwind経路に落ちる。ループpassがphiの形を確定
させた後、最後に走り、出ていくbitcastが入ってくるものと同数以上の
ときだけ変換する。ただし数えるときは、それぞれがどこに置かれているかで
重みを付ける — ブロック頻度が言うとおり、ループ本体のbitcastは毎反復
払うが、unwind経路のものは一度も払わないかもしれない。1つずつ数えると、
最も素朴な形で変換を見送っていた: 関数の中の`while`がFloatを1つ運ぶ
だけの形で、冷たい2つの使用箇所（戻り値のstoreとunwindのrelease）が
ループ自身の1つを上回り、そのループは`for`で書いた同じループの4倍
遅いままだった。健全性はincoming側の検査だけで担保される:
どの辺も既にdoubleを運んでいるか、ビット単位でdoubleとして
解釈し直せる定数である。つまりphiの型は変わるが値は変わらない —
どこかで整数やポインタとして読まれるペイロードは`i64`のphiのまま
残る。`optimize_module`は変換後もモジュールがverifyを通ることを
assertし、`tools/checks/check_float_carry.sh`がその結果をemitされた
IR上に固定する — このpassが黙って止まっても他の何も気づかない
からである（§10.2）。

もう1つ、バックエンド自体に触るノブがある。AArch64のearly
if-conversionは小さな`if`の腕を分岐の確率に関係なく`fcsel`に
投機し、その腕がループの持ち回るFloatへ代入していると`fcmp`と
`fcsel`がループのクリティカルパスに乗る。`tools/bench/vector_loop.cul`
のscalars行はこれに1stepの6分の1を払っていた一方、Vector2行の腕は
大きすぎて変換されず、払っていなかった。`JIT::tune_backend`がこの
passをtarget initの2経路の両方で切る。x86は元から走らせていない。
実際に払っていたのはJITだけで、JITはhostのCPU向けにコンパイルする一方
`culebra build`はCPUを指定せずgenericになりこのpassは発火しない。
つまりAOT側の呼び出しは今何かを直すためではなく、レーンがずれないように
するためにある。切れているかどうかはIRに現れないので、
`tools/checks/check_early_ifcvt.sh`がJITの出したobjectを読み戻して
確かめる（§10.2）。

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
`tools/checks/check_eh_balance.sh`がそれをemitされたIR上で検証する。

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

セッション単位の関数リテラルは、自分が束縛しない名前について
**セルを捕獲する**。本体に名前で引かせない。名前で引く本体は、それを
走らせるスレッドの上で引くことになり、isolateや`Parallel`のワーカーには
セッションも、そのセルを鋳造した`Runtime`も無い — クロージャがスレッド
境界を越える形はfn_ptrとキャプチャであって、名前は運ばない（§3.4）。
キャプチャは束縛の`shadowed_builtin`フラグを一緒に運ぶので、まだ未束縛
のセルは「その名前のstdlibグローバル」を意味し、読むスレッド自身の上で
解決される。sendableの表現にはそのセンチネルの種別があり、だからその
ようなキャプチャも越えられる。代償は相異なる自由名ごとに1つのキャプチャ
で、stdlib名も含む — 実測でクロージャの生成1回あたり約30ns/キャプチャ。

破棄は構築と逆順に走る。セッションのcellは、保持されたプログラムより先、
`Runtime`より先に返される。cellの解放はculebraのコード（`drop`の本体）
を走らせ得て、そのコードはプログラムを指すdescriptorを通じてバイトコード
に到達するからである。`release_all`はマップを走査せずdrainする。`drop`が
カーソルの手前にcellを鋳造し得るためである。

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

**ファイルは1つのプログラムである。** 各ファイルは自分の`Runtime`
（名前空間キャッシュとクラス/オーバーロード登録簿が住む場所）、自分の
セッションとcell、自分のエントリスクリプト（`Sys.script`、および
`Embed.dir(...)`が基準にするディレクトリ）、自分のisolate join
ガードを持つ。そしてランナーは、全ファイルを先に読み込むのではなく、
そのスコープが開いている間にそのファイル自身のテストを走らせる。
ファイルがトップレベルに書いたものは次のファイルには届かない。
ファイルは`import`できる: そのモジュール一式が1つのセッション単位
として走り、リストにpreambleがsplice済みでなければ
`Session::run_modules`が自分でstdlibデルタを要求する。

ゲートは`tests/*.cul`の全部をランナーに通し、終了コードとファイル数の
両方を検査する — これらのファイルは`test(...)`を登録しないので、
`passed`はカバレッジについて何も言わない。スイープ全体でセッションの
スコープ規則を通す唯一のレーンである。対称性スイープは同じファイルを
スクリプトとして走らせる。

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
これは`jit.h`、`vm_lowering.h`、`stdlib_rt.h`の`declare_runtime`
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
は両方に存在しなければならない（`tools/checks/check_jit_host_symbols.sh`、
`tools/checks/check_rt_archive_tls.sh`）。

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

### 10.2 loweringの出力へのチェック

- `culebra --jit --emit-llvm f.cul | opt -passes=verify` — lowering
  作業の常設チェック。`run_program`も自分が構築するすべての
  モジュールを検証し失敗時にthrowする。
- **IR diffing。** codegenを変えてはいけないリファクタは、
  `tests/*.cul`全体に対する`--jit -O0 --emit-llvm`を前後で比較し、
  stderrを含めてバイト単位で一致することで検証する。
- `tools/checks/check_eh_balance.sh`（すべての`__cxa_begin_catch`が
  閉じられているか。unwind edgeのないrethrowがないか）、
  `tools/checks/check_alloca_discipline.sh`（一時slotがエントリブロック
  に留まっているか — ループ中の非エントリ`alloca`は毎パス
  スタックを伸ばす）、`tools/checks/check_float_carry.sh`
  （`tools/bench/vector_loop.cul`のscalar行とVector2行を写したprobeで、
  どの辺もdoubleを運ぶphiがdoubleのphiになっているか。`i64`のphiのまま
  bitcastで読み戻されていないか — `PromoteFloatPhis`が消す形そのもので、
  戻ってもテストは何も見ずにループが3割遅くなるだけ）、
  `tools/checks/check_early_ifcvt.sh`（同じ行の機械語を
  `CULEBRA_JIT_CACHE`と`objdump`で読み戻し、ループに`fcsel`が無いこと
  でAArch64のearly if-conversionが切れたままだと確かめる — IRには
  現れないノブなので、そのcodegenがあるホストでだけ検査する）、
  `tools/checks/check_rc_discipline.sh`（`jit.h`内の
  手書きretain/releaseサイトの数は減る一方であるべき）。
- **assert。** 出力からは決して分からない3つの不変条件がassert
  レーン（`just test-assert`、CIの`linux-assert`。`NDEBUG`なしで
  同じスイープを回す）に乗っている: 解決済み呼び出しの予測chunkが
  実際に現れるクロージャと一致すること（§5.4）、借用された呼び先の
  cellが呼び出し終了時にも同じ値を持っていること（§5.4）、そして
  確定したrefcount呼び出しがパイプラインを通ったあとに1つも
  残っていないこと（§7）。

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
- **呼び先を名指ししても買えるのはdispatchであって本体ではない。**
  呼び出しを1つのchunkに解決すると（§5.3）、単相呼び出しは`--jit`で
  約1/5、executorで数%速くなる。呼び先がinlineされるようには
  ならない: `-O2`のコストモデルは自前のlandingpadを持つ本体を断り、
  強制的にinlineさせても数字は同じである。呼び出しごとに残るのが
  不透明なランタイムヘルパー — 呼び出し位置のpublish、再帰カウンタの
  enter/leave、owned scopeの括り、引数のretain/release対 — であり、
  外部シンボルへのcallをまたいでSROAがこれらを打ち消せないからだ。
  `fn name`の宣言形は解決対象にすらならない: そのcellが持つのは
  マルチメソッドdispatcherで、本体closureはレジストリの中にしか
  居ない。
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
第二の意見としてインタプリタに代わるものである。`include/vm/vm.h`、
`include/jit/lowering.h`、`include/rt/rt.h`のコミット履歴が移行の記録
を残している。それを始めた設計提案とフェーズごとの知見は、ここで
なくその履歴の中に生きている。
