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
      │  AST→AST変換 (algebraic effects)
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
   各モジュールはパースされ、書かれたままの形からスコープの検査を
   読み取り（§10.6）、そのあと変換がASTに対して走る
   （`apply_transforms`）: effectsの変換（§11）と、generatorの検査
   （`yield`を書ける場所の検査、§5.7）である。
   検査の結果は全モジュールを読み終えてから報告する。
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
   組み込みtrait（`Stringer`、`Eq`、`Comparable`、…）も`__Traits`と
   いう名前で同じようにbakeされるが、エントリ一覧への入り方が違う。
   どのプログラムも名指しせずに登録するものなので、そもそもspliceの
   対象ではなく、コンパイルするレーンは自前のprologueとしてlowering
   していた。このprologueが小さなプログラムのIRの大半を占めていた —
   `let x = 1`がloweringする約6,200行のうち約4,800行が5つのデフォルト
   本体（`Eq.neq`と`Comparable.lt/le/gt/ge`）で、プログラム自身の
   コードからはどれも到達しない。`--jit`の起動82msのうち約75msが
   これだった。`with_baked_traits`がこのエントリをstdlibのエントリ
   より前に置き、同時にコンパイラへprologueを省くよう伝える
   （`BakedLane::traits`）。呼び出しを出しつつソースも残すと二重に
   登録してしまうので、この2つは必ず一緒に動かす。
   executorは差し込まれたソースをコンパイルし続けるので、対称性
   ゲートは毎回bakeされたコードをそれと比較する。
   `CULEBRA_PREAMBLE_SOURCE=1`でloweringレーンも再びソースを
   差し込む。traitも同様で、`check_baked_preamble.sh`はこの
   切り替えが生きていることをそこで読み取る。
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
カウント実装（`rt_mem.inc.h`）、generatorがresumeの間に保持するフレーム
（`rt/gen.inc.h`、§5.7）をincludeする。どれもLLVMを名指しない。

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
  自分のchunkをパラメータのメタデータ経由で名指す（§6.1）。loweringが
  構築するクロージャはそこにネイティブ関数を持つ。どちらも同じ*JitFn ABI*
  で呼ばれる: `void (JitValue* ret, JitClosure*, int8_t self_tag,
  int64_t self_data, int64_t n_args, JitValue* args)`。
- `Shape`付き`JitObject` — V8スタイルのhidden classと固定slot、
  プロパティサイトのインラインキャッシュ（`JitPropIC`）、非String
  キー用のany-keyサイドマップ。インスタンスは自分のクラスオブジェクト
  への`+1`を持つ（`JitObject::cls`）。読みサイトのICはヒットの2つの形を
  インラインで答える: 自分のslotと、クラスインスタンスでのメソッド読み
  （データは自分のslot、メソッドは`proto`の先の共有metaにある）で使う
  protoのslot。後者は（受け手のshape、protoのshape）の対としてキャッシュ
  され、ランタイムhelperのmiss経路が埋める。view（packed・Shared・
  SharedBuffer・FixedArray・BoundedArray）はランタイムがslotより先にアクセスに答える
  ので、そのshapeは専用のroot（`ShapeRegistry::view_root`）から伸ばす。
  Objectが温めた読みや書きのキャッシュがviewに一致することはない。
  書きサイトのIC
  （`JitPropSetIC`）は温まった更新を呼び出し1つ、`object_set_update`で
  済ませる。これはtransitionや拒否にはfalseを返し、サイトはそのとき
  `object_set_fast`を呼んでエラーを組み立てさせる。
  複合代入（`o.k op= v`、`Op::PropWr`）の読みの側は、読みサイトの自分の
  slotの枝を持つ。missは`prop_wr`が受け持ち、executorはこれ1つで済ませる。
  サイトのキャッシュを温めるのは、`prop_wr`が扱う普通のObjectだけである。
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

- **capture。** どの名前が外側の関数からcaptureされたものか
  （`free_vars`、ソースの位置順。メソッド呼び出し`v.name(...)`だけが
  名指すUFCS候補は`optional_free_vars`）、自分のlocalsのうちどれを
  ネストしたクロージャがcaptureするか（`captured_locals` — これが
  cellに昇格されるもの）。これらはスコープ規則の唯一の記述である
  `resolve.h`から導く: ある関数が読む名前の変数を別の関数が宣言して
  いれば、読む側からその宣言側の手前までの各関数の自由変数になり、
  宣言側がcaptureする（コンパイラ自身のlookupも§10.6で同じ解決に
  縛られている）。
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
機構から独立している。shadowingは同じ解決結果の上で
`lint::check_shadow`がチェックし、`culebra lint`と単一のソースを共有する。

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
- `if` / `cond` / `?:`の各腕はブロックとしてコンパイルされる
  （`compile_block_into`）。腕は独自のスコープと独自の`defer`スコープを
  持ち、腕が宣言した名前は腕の終わりで消える。スキップされうる式の中の
  宣言だけが条件付きの事前宣言として残る: `if` / `cond`の後ろのテスト、
  短絡評価がスキップするオペランド（`c && (let h = f())`）、`?.`以降の
  チェーンの残り、`??=`の右辺である。このときどの宣言が走ったかは
  この呼び出しの事実である: コンパイラは各宣言の`mut`をその隣のslotに
  記録し（`Binding::mut_slot`）、bareな書き込みはそれを参照する。この
  bindingがcellになるのはクロージャがその名前をcaptureするときだけで、
  それ以外は同じセンチネルを保持する素のslotになる。loweringはheapからの
  loadでなく値そのものを見るので、そのタグは他のローカルと同じように
  畳まれる。
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
| `call_names` | メソッド呼び出しごとに、呼び先を読むのに使ったメンバー名。存在しないメソッド（メンバーは`nil`として読める）の呼び出しは、これで名前を伝える |
| `call_targets` | 呼び出し命令ごとに、その呼び先が解決された唯一の関数chunk、そのchunkがレジスタの値とどう対応するか（`Chunk::Reach`）、そして呼び先をcellから直接読むかどうか（§5.3） |

### 5.2 命令列の中の所有権

参照カウントは明示的である: コンパイラが`MoveRetain`と`Release`を
emitし、消費者はそれを実行するだけである。値移動を担う5つのop —
`LoadConst`（生コピー。定数は参照カウントされない）、`Move`（生
コピー）、`MoveRetain`（自分の`+1`を持つコピー、つまりborrow）、
`Take`（転送: sourceがnilになる）、`Release` — が語彙を作る。
すべての式は結果レジスタ
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

### 5.2.1 コンパイラが死んでいると証明したbookkeeping

上記のbookkeepingは無条件に発行される — どのスコープも抜け際に
slotへ`Release`を、借用のコピーには必ずretainを、どのスコープにも
mark/exitの対を出す — というのも、コンパイラは1回の前方passで
発行しており、あるslotが`Long`以外を一度も持たないこと、あるスコープが
drop可能なリソースを一度も登録しないことを、その時点ではまだ知り得ない
からである。`compile_unit`は1回だけコンパイルし、その後2つの静的解析が
不要と証明したものを不動点まで削除する: 1つの命令を削除すると別の命令が
証明可能に死ぬことがある（解析が「生きている」と仮定するしかなかった
slotの最後の読みだったと判明する`Release`のように）。どちらの解析も
安全側に倒れる — モデル化していないopや分岐は、削除されずにその領域の
bookkeepingを保持させる側に回る — ので、見落としはパフォーマンスの
損にしかならず、正しさを損なうことはない。

**参照カウントの削除**（`plan_rc_elision`）はchunkごとの前方データフロー
で、2点束（`NonRc < Unknown`）を使い、`Release`が指すslot、あるいは
`MoveRetain`がコピー元にするslotが、そこで参照カウント型を持ちうるかを
判定する。非参照カウントの
定数を積む`LoadConst`、非参照カウントのオペランド同士の算術、といった
（すべてexecutor自身のswitchから読み取った）ものは状態を`NonRc`へ
落とし、モデル化していないもの（コンテナ操作、プロパティ書き込み、…）は
すべてのslotを`Unknown`へ引き上げる。

呼び出しはモデル化しており、しかも効くのは戻り値の側ではない。
`Call a, b, c, d`が呼び手のフレームに書くのはちょうど2箇所である:
戻り値が`a`に入り（呼び先が返したものなので`Unknown`）、引数の並び
`c … c+d-1`は**nilにされる** — 呼び先が引数それぞれの`+1`を持って
いったので、呼び手のslotは抜けるときに空になる。どちらの経路もそうし、
それぞれのthrowパスもそうする（`Exec`自身のループと、resolved高速
パスのために`run_resolved`が持つ`Drain`）。`CallM`は1slot幅が広く、
レシーバが引数の手前の`c`に座る。フレームの他は何も動かない — 呼び先は
自分のレジスタで走る — ので、呼び出しがすべてのslotを引き上げる必要は
もう無い。呼び出しの周りのbookkeepingが落とせるようになったのはこれが
理由である: 引数のslotは呼び出しから証明可能に`NonRc`で出てきて、その
前後の定数は持っていた状態をそのまま保つ。`fib`のchunkは`Release`が
10本から4本になり、残る4本は残るべきもの（各呼び出しの戻り値、
`MfSelf`のハンドル、パラメータ）である。

`MoveRetain`のretainを落とす（ただの`Move`が残る）には前方passだけで
足りるが、
`Release`を落とすには後方向のliveness解析がもう1本要る。`Release`は
破壊的（slotをnilにする）で、後続のコードがそのnilに依存しているから
だ — 外側のスコープのladderとthrowパスのunwindは、内側の段が既に
空にしていたおかげで同じ範囲を二重解放せずに済んでおり、`for`-in
カーソルのslotは次の世代がそれを生きたiteratorとして読んでしまわない
よう、古い値を引き継いではならない（`ForSlot`、§5.5）。liveness解析
はまさにそこが問題になるslot（カーソルの12-slot全域、スコープのdefer
mark、handlerのcaught slot）をどこでもliveに固定する。ふつうのslot
はthrowパスに何も借りがない — 非参照カウント値をそこでreleaseするのは
既にno-opだからだ。

**owned stackの削除**（`owned_plan_for_chunk`）は`OwnedMark`/
`OwnedExit`の対（§5.5、`memory.md`の「owned stack」節）を狙う: ネストした
スコープの対（frame自身の対＝深さ0は対象外 — throwパスは
`marks[owned_frame_depth]`を直接読むので、その`OwnedMark`を消すと
その読みはslotが最後に持っていた値を見ることになる）は、`OwnedMark`
から同じ深さの最初の`OwnedExit`までの区間が次の2つを満たすときに
死んでいる。drop可能なオブジェクトを登録しうるもの（インスタンス構築、
`drop`を束縛するプロパティ書き込み（`fixed.inc.h`の
`_jit_owned_bind_drop`呼び出し箇所）、呼び出し。呼び先はこの対だけが
dropできる循環を返しうる）が一切ないこと。そしてその`OwnedExit`以外に
出口がないこと、つまり区間内の分岐が前方へ飛ぶジャンプとテストだけで、
飛び先がその`OwnedExit`を越えないこと。throwパスはframeのmarkだけを解決する
ので、ネストした対は同期パス限定の最適化であり、unwindからは見えない。
このチェックは単純な逐次スキャンでCFG走査ではない: この形の区間の命令は
すべてスキャンが見た命令である。計算だけの`if`の腕はこれで対を失う
（`acc += i`はin-placeの結果かどうかをテストする）。区間内のループや
`OwnedExit`を迂回する分岐があれば対は残る。中に`if`があるループ本体は
最初のラウンドでは保守的なままだが、不動点反復がその大半を取り戻す:
削除可能だった内側の対が削除されると、
かつてネストしていた本体は次のラウンドではフラットなコードとして
読まれ、それを囲む外側の対も削除可能になり得る。

**行き先の合体**が3つ目で、これは参照カウントの話ではない: `Take`の
3分の2は`<producer> X ; Take Y, X`の後半である — 式がコンパイラの
用意した一時スロットへ計算し、その次の命令がそれを値の本来の居場所へ
移すだけ、という形だ。producerが直接`Y`へ書けばよい。変わるのは以降
`X`が何を持つかで、`Take`はそこをnilにしていたが、今やproducerが触ら
ないので以前の値を持ったままになる。その両面とも押さえてある —
liveness解析が「誰も読まない」と言い、かつ古い値が解放されることも
ない（producerは`X`を解放せずに上書きしていたのだから、そこに生きた
値があれば元のコードの時点で漏れていた）。`Take`に飛び込むjumpは
producerを経ずに到達するので、jump先は決して候補にしない。

**転送になる借用**が4つ目。`MoveRetain X, Y`の直後に`Release Y`が続く
ときは、コピーの`+1`と解放の`-1`が打ち消し合う（その間に値が最後の参照に
なることはないので`drop`は走らない）ので、2命令をまとめて1つの
`Take X, Y`にする。`Y`は`Release`のときと同じくnilになる。関数が引数を
そのまま返すときの形がこれである。

**梯子の融合**が5つ目で、上の4つと違って何も証明しない — データフロー
ではなくコードの形の話なので、他の4つが不動点に達した後に1度だけ走る。
スコープを解放するのはコンパイラが既に一括で下した1つの判断であり、その
段ごとに1命令を使うことが`Release`をbytecodeの30%に押し上げていた。
連続する`Release`は、同じスロットを同じ順で並べた1つの`ReleaseMany`に
なる。スロットは`Chunk::release_slots`に端から端まで並べる（サイトごとの
配列ではなくchunkに1本: 梯子はたいてい2段か3段で、その長さでは2段目の
間接参照が融合で浮いたディスパッチより高くつく）。梯子は途中から入れない
ので、jumpの飛び先、およびcleanupのstart・end・handlerにあたるpcで連続を
打ち切る — unwindが走らせる梯子を選ぶのがこの範囲だからである。参照
カウントと所有スタックの解析にはこの新しいopcodeの腕が要らない: どちらも
`default`が全クロバー・全読みという安全側なので、両者が主張する事後条件は
より慎重になるだけである。

命令の削除や書き換えは完成した`Chunk`への1回のpassであって、2回目の
発行ではない: pcを持つ8つの表（`code`のjumpオペランド、`positions`、
`cleanups`、`slot_debug`、`temp_points`、`call_argpos`、`call_names`、
密な`call_targets`）は1つの`pc → pc'`写像とともに移動する。これが閉じて
いるのは、bytecodeが一切シリアライズされない（§5.1）からで、pcには
このコンパイルの外に読み手がいない。`while i < n { i = i + 1 }`は
これによってループ1反復あたり13命令から8命令へ減る: 3本の`Release`
（このループはLongしか運ばない）と、ループ本体の`OwnedMark`/
`OwnedExit`の対（中でdrop可能なオブジェクトを何も構築しない）である。
testsと言語front endを合わせたコーパス全体では、4つのpassが
1,684,313命令を1,608,765命令にする。5つ目はその残りをさらに25%削る —
解放の梯子は2段か3段で、ほとんどのスコープの終わりに1本ある。

### 5.3 opcodeのファミリー

157個のopcodeを分類すると:

| ファミリー | op | 備考 |
|---|---|---|
| 値 | `LoadConst` `Move` `MoveRetain` `Take` `Release` `ReleaseMany` | §5.2。`ReleaseMany`はshape passが発行する、融合された解放の梯子（§5.2.1） |
| 算術・ビット演算・比較 | `Neg` `Not` `Add` … `Pow` `MatMul` `BitAnd` … `Shr` `BitNot` `Eq` … `Ge` `JumpIfSame` | それぞれ1回のランタイムdispatch。算術と比較のopは両Long・両数値の腕をまずinlineで決める（`Neg`はLongとFloatの腕）。算術opの`d=1`は複合代入のin-place Tensorステップを示す |
| コンテナ | `ArrayNew/Append/Push/Extend/Resize` `TupleNew/Push` `SetNew/Add` `ObjectNew/NewShaped/Set/SetAny/Merge` `SlotInit` `RangeNew` `ChkLong` `ChkNum` | コンテナは要素の`+1`を吸収する。`SlotInit`はShapeを事前構築したリテラル向けの、スロット番号による`ObjectSet`（§5.3.5） |
| アクセス | `Index` `IndexWr` `IndexCo` `IndexSet` `PropSet` `PropWr` `PropCo` `PropVal` `PropRaw` `HasProp` `UfcsTakes` `NsWrChk` `NilChk` | 添字とプロパティアクセスの読み/書き/coalescing-write形。`PropVal`はgetterを呼ぶこともある素のプロパティ読み取り |
| 呼び出し | `Call` `CallM` `CallKw` `CallRecv` `Ret` `RecEnter` `RecLeave` `ArgsRest` `KwRest` `JumpIfFilled` `ChkArg` `ChkTypeAt` `PosSnap` `BoundPos` | JitFn ABI。`CallM`はreceiver上のメソッド（ユーザー定義または組み込み）を解決する。`RecEnter`はパラメータが束縛された後、フレームを再帰上限に対してカウントする |
| 組み込みメソッド | `MethGate` `ChkParam` `BMeth` `BArity` `CbType` `ArityChk` `BareMethChk` | §5.4 |
| クロージャと名前 | `MakeClosure` `CellNew` `CellGet` `CellSet` `CellRelease` `BindCapture` `ImmutErr` `UnboundErr` `NsGet` `LazyNsReg` `FnHandle` `ModReg` `ModGet` | §4.1、§4.2。`ModReg`/`ModGet`はモジュールのexportオブジェクトを公開/読み取る |
| 関数とクラス | `MultifnReg` `MfSelf` `ClsSelf` `ClassMeta` `ClassObj` `MakeInst` `FieldsInit` `FieldInit` `BindStatic` `SelfMerge` `DeriveFn` `RegPack` `EnumVariant` `TraitReg` `TraitDefault` `TraitReset` `ClsParamsChk` `ClsParamsWalk` `WkErr` | `MultifnReg`はランタイムのarity-dispatchレジストリに本体を登録する。クラス宣言はmetaを構築しメンバを登録する |
| パターン | `TypeMatch` `SeqChk` `SeqGet` `SeqRest` `ObjGet` `DestrErr` `JumpIfTag` | `match`の腕とdestructuring。テストが失敗すると次の腕へジャンプし、その時点で何も生きていない |
| 制御フロー | `Jump` `JumpIfFalse` `JumpIfTrue` `JumpIfNil` `JumpIfNotNil` `Halt` | `JumpIfFalse`は共有のtruthiness変換を運ぶ（非Bool条件はTypeError） |
| ループ | `ForPrep` `ForLoop` `ForOpen` `ForNext` `ForDispose` `Safepoint` | Long範囲の数え上げ`for`は融合されたペア（sinkの`for _ in 0..n`も含む）。それ以外は12個のslotからなるカーソル（`ForSlot`）でプロトコルを歩く |
| 例外とdefer | `Throw` `Rethrow` `CaughtPos` `RaiseErr` `DeferMark` `DeferPush` `DeferRunTo` `OwnedMark` `OwnedExit` `DropSuppress` `Drop` `DropChk` | §5.5。`OwnedMark`/`OwnedExit`は決定的`drop`のためowned-resourceスタック上でスコープを括る |
| 文字列と出力 | `Fmt` `StrCat` `Disp` `Println` `SetOpPos` | 補間、および`println(<引数1個>)`のpeephole |
| namespace関数 | `NsCall` `ToFloat` | 直接の`Math.f(args)` / `to_float(x)`はresolverもclosureも経ずにhelperへ届く（§5.4） |
| セッションとデバッグ | `ReplCell` `ReplBind` `DbgStmt` | §8.1、§8.3 |
| generatorのフレーム | `GenStart` `Yield` | §5.7 |

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
retainと、それに対応する文末の`Release`が両方消え、executorからは
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
必要がある(`FnAnalysis::value_layouts`): 宣言fieldが1つ以上、全部スカラー、
どれも初期化式を持たない(初期化式はfield-init thunk = フレームを
通る)。layoutはクラスの名前ではなく宣言ごとに持つ。2つのクラスが同じ
名前を持つことはあり(トップレベルと関数の中、あるいはプログラム自身の
`Vector2`とstdlibのもの)、宣言ごとに自分のfieldとその型を持つ。各メンバの本体はsplice可能でなければならない(`inline_body_ok`):
straight-lineな制御フロー、入れ子の`fn`/classリテラルなし、そして
コンストラクタ以外では`self.x =`書き込みなし(boxedインスタンスでは
これはfreezeの`ImmutableError`)。そして本体が読む全ての名前は、
展開された先でも、書かれた場所と同じ意味を持たなければならない。
自由変数を持つ本体は断る: その変数はプログラムのもの(プログラム自身の
`Math`、UFCS呼び出しが名指す関数)で、展開先からは見えないか、別のものが
見える。`fn`を読む本体も断る: `fn`はフレーム自身のハンドルで、spliceすると
展開先のフレームのものになる。それ以外は、残りの識別子を全部歩いて`self`、
パラメータ、本体のlocal、クラス自身の名前、stdlibのglobal・namespaceの
どれかであることを確認する — 歩く必要があるのは、namespaceが変数ではなく
`FuncInfo::free_vars`に一度も現れないからである。

この検査は展開先について何も問わない。spliceが展開先で名前を読まない
からである。本体をspliceしている間、そして本体について問い合わせている
だけの間も、メンバごとのレコードが`Compiler::member_frames_`の先頭にあり、
名前による検索(`lookup_name_mut`)は全てそれを通して答える: splice自身が
開いたスコープ(パラメータとlocal)、次に本体が自分のクラスを呼ぶ名前
(綴りではなく宣言で探す、`Binding::Known::value_class`)、それ以外は
何も見えない。`self`はレコードが指すrunである。束縛の見つからない名前は
したがってstdlibのものになる。だからブロックが自分の`Vector2`や`Math`を
宣言してからstdlibの`Vector2`のメソッドを呼んでも、そのメソッドの中身は
何も変わらない。事前の問い合わせ(`let`でのスコープ全体の走査)とsplice
時点の問い合わせが食い違うこともない。

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

本体の一時スロットは本体と一緒に、そのスコープをpopする前に終わる。
popはスコープのスロットを、呼び出し側の文の途中でアロケータへ返す。
次にそれを取るのは呼び出し側の次のrun、つまり続くステップの結果で、
`let`はそれをコピーなしで自分のホームにする。その時点で文のsweep
リストに残っている一時スロットは、文の終わりで、束縛のフィールドの
下からreleaseされることになる(実際に踏んだ例:
`let v = C.new(1.0, 0.25).scale(C.new(3.0, 4.0).len())`が両方の
フィールドで`nil`を読んだ)。

連鎖はスカラーの末端までunboxの形を保つときにしかそのマーカーを
渡されない — runが他の消費者に届く形はそもそも構築対象外なので、
`let v = C.new(...)`が値として使われる場合は従来どおりコンパイルされる。
この制約が**reification無しで着地できる理由**である: このコードが
生み出すunboxedな値は、必ず同じコンパイル時機構によって再帰的に
消費される。これは同クラスを構築して返すメソッド(`V2.__add__`が
`V2.new(...)`を返す形)を、別途SROAパスを設けずに畳み込む仕組みでも
ある。

そのため、連鎖のステップを歩く判定(`chain_end`)の答えは2通りではなく
3通りある: **却下**(どこかのステップがオブジェクトを必要とする)、**スカラー**
(フィールド読みで終わった、または値が自クラスでないメンバで終わった)、
**run**(インスタンスを保持したまま終わった)。unboxのまま終わる2通りを
分けておくのは、消費者が違うからである。スカラーは1スロットの普通の値。
runはNスロットで、runを求めた消費者しか受け取れない: §5.3.2の演算子の
オペランドとメンバ自身の末尾、§5.3.3の`let`と書き込み。
emitterは呼び出し元がどちらを受け取るかを渡される
(`try_inline_value_chain(ast, want)`。プログラム中のpostfix連鎖は
スカラーを求める)。runの消費者が必ず尋ねる先読み
`chain_resolves_to_class`はrunの答えにだけクラスを返す。
両方を1つの「連鎖は適格」で答えていたときは、スカラーで終わる連鎖が、
途中で構築したクラスのrunとして読まれた(実際に踏んだ例:
`let d = C.new(3.0, 4.0).len()`が`d`を`C`のフィールドとして配置し、
それを表示すると隣のローカルが2つ目のフィールドとしてboxされた)。

### 5.3.2 その値への演算子もspliceされる

`v + g * DT`はboxedの経路では他のあらゆる算術演算と同じruntime dispatch
経由で`__add__`/`__mul__`に届く。両辺がflatな`@value`クラスなら、
`Op::Add`/`Sub`/`Mul`/`Neg`は§5.3.1の連鎖における`.method()`ステップと
同じ形でdunderをspliceする — 1クラス、1インスタンス、1回のsplice、
やはりオブジェクトは1つも作らない。

演算子fold(`ADDITIVE`/`MULTIPLICATIVE`、および1オペランド・演算子1個
に縮小した`UNARY_MINUS`)は、演算子ごとにではなく何もコンパイルする
前に決める: 命令を一切出さない純粋な先読み`fold_resolves_to_class`を
まず演算子列**全体**に対して尋ねる。operand[0]はrunへ解決しなければ
ならず(`chain_resolves_to_class`。§5.3.1の適格性検査をrunを求めて
尋ねるもの)、連鎖中の全演算子が、operand[0]が生成する
であろうクラス上のsplice可能なdunderへ解決しなければならない。これは
splice本体が実際に走らせる3つの検査(そのメソッドが存在する・
適格である・引数を1つ取る)と全く同じ基準で確認する — トークンが
dunder名にマップされるという構文だけの確認ではない。全演算子が
適格だった場合に限り、operand[0]はrunで終わる§5.3.1の連鎖
(スカラーではなくインスタンスそのものを保持したまま連鎖を終える)
としてコンパイルされる。プログラム
中の他の全てのオペランドは常に従来どおりboxed経路へreifyされる。
このfold全体の先読みがあるおかげでfoldループは無条件になる:
accumulatorが一度unboxされれば、その先の演算子は全て既にsplice
できると証明済みなので、既にunboxed化したaccumulatorを
re-materializeし直す必要が生じる「途中での却下」は決して起きない
— §5.3.1自身と同じ「一度だけ決め、出すか出さないか」という規律を、
連鎖ステップの列ではなく演算子の列に対して適用しているだけである。

本体の末尾が演算子**そのもの**であるメンバ — `__add__`の本体が
自クラスの`C.new(...)`で終わる形 — には§5.3.1に無かった要素が
もう1つ要る: その末尾自身もrunとしてspliceされなければならず、
連鎖がクラス自身を保持したまま終わることを決して許さない通常の
`compile_expr`降下でコンパイルしてはならない。メンバの本体が何を
残すかは、そもそもspliceできるかの答えの一部である(`inline_body_end`:
却下・スカラー・run)。メンバの結果の大きさを決めるものは全てそこから
読む: あるステップが次のステップにrunを残すかを決める連鎖の歩みと、
そのステップの結果をNスロットで確保するか1スロットで確保するかを
決めるsplice。runになるのは、末尾がメンバ自身のクラスのconstruction
連鎖で、かつ**runで終わる**ときである。クラスを名指ししているだけでは
足りない: `C.new(...).x`はインスタンスを構築してスカラーを返す。
constructionだけを見て結果の大きさを決めると、最初の1スロットだけが
書き込まれ、それ以降の全スロットはrunのzero-initが残したものを
保持したままになる(実際に踏んだ例: `getx() { C.new(self.x, 1.0).x }`
に対する`C.new(5.0, 6.0).getx().y`が、boxedのクラスならTypeErrorになる
ところで0.0を読んだ)。本体を1回歩けば両方の答えが出るので、他の
メンバを経由して連なる末尾はメンバごとに1回だけ歩かれる。「spliceできるか」
と「何を残すか」を別々に尋ねると、末尾を各段で2回歩くことになり、
そういう末尾の深さに対して指数的になる。

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

- **foldがspliceしようとする全dunderは、それ自体がrunを残さねば
  ならない(`inline_body_end`)**。正しいarityで存在するだけでは足りない。
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
spliceする）。宣言自身のRHSも書き込みと同じ3つの形——runで終わる
construction連鎖・fold・否定——を受け付けるので、
`let d = C.new(...) + C.new(...)`は§5.3.2が組み上げたrunをそのまま
持続させ、`let d = C.new(...).len()`は普通のローカルになる。
素の`let`は単一代入の
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
（`register_value_class_layout`）はクラス宣言自身をコンパイルすることの副作用
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
`v.<field>`/`v.<method>(...)`は`try_inline_name_chain`を通じて
spliceされ、`self`自身の連鎖（メンバの中の`self.len()`）も同じ1本の経路を
通る（どちらの名前も既にrunであり（`name_run`）、`chain_end`を
index 1から問う）。再代入（`compile_assignment`）と複合
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
`Chunk::ValueBoxSpec`から、組み立てるclass objectは`regs[d]`）はこの隙間を、§5.3.1〜
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
（`compile_expr`自身のディスパッチはrunを求めない: foldと否定は
`allow_trailing_class=false`を渡し、construction連鎖はスカラーの
終わり方だけを求められる）ので、`value_boundary_ok`を裸の
識別子より広げても、被覆を増やすのではなく仕事を重複させるだけになる。

instanceはclass object（`regs[d]`）によって組み立てられる。`new`が作った
ものと同じである: `JitObject::cls`がそれを持つので、自分のクラスを名前で
呼ぶメンバは再ボックス化された受け手からも同じ経路で読め
（`culebra_runtime_class_self`）、instance metaはhelperがそこから取る
（`class_meta_of`）。`value_class_object`はrunのある場所でクラスを読む。
runが存在するのはconstructionがそのクラスを名指しした場所だけなので、
宣言がクラスを渡した束縛（`Binding::Known::value_class`）はそこでスコープに
ある。宣言自身のcellか、入れ子の`fn`がその名前のために既に持っている
captureである。stdlibモジュールのクラスは束縛を持たないので、その名前の
namespaceから読む（`Op::NsGet`）。追加のcaptureは無く、クラスに届かない
runも無いので、どのrunも再ボックス化できる。

値として読まれる代入はこれに依っている。unboxedな束縛への宣言や書き込みは
runに評価される。文ならそれで正しい。その値は誰も読まないからだ。値が
読まれるbodyをそれが締める場所では`compile_value_into`が再ボックス化し、
束縛そのものはunboxedのまま残る。（式の中で書かれる束縛はunboxedに
ならないので、代入の値をほかで読む側がrunに出会うことはない。）
文として書かれた`if`・`cond`・`match`・`try`の値も誰にも読まれず、
その腕の値も同じである。`compile_unread`がそれを腕に伝えるので、
`tools/bench/vector_loop.cul`のstepにある`if p.y < 0.0 { ... }`の腕を
締める書き込みは何も確保しない。この両方を`tests/resolve_shape_test.sh`が
バイトコードの上で押さえている。

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
`hash`/`cmp`/`eq`、`__str__`、`__call__`、`__index__`/`__setindex__`）をmetaが
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

**`==`の規則は1つ、それに答える関数も1つ。** どちらかの側の`__eq__`、
次に両側が宣言した`eq`、最後に構造: `_culebra_value_equal`。演算子がこれを
呼び、「等しい」を意味する他の全て — `contains`、`index_of`、導出された
`cmp`、`chunk_by`の区切り — も、その裏にある構造の走査も要素の対ごとに
これを呼ぶ。最後の点が`a == b`から`[a] == [b]`を導く。以前は走査が
自分自身へ再帰していたので、最初の2段は最上位で止まり、Setのメンバーと
Objectのキー（こちらは常に`eq`を通っていた）が、それを入れたコンテナと
食い違っていた。走査の名前は走査の外に現れてはならず、
`check_value_equal_door.sh`がそれを保つ。`eq`を定義しないプログラムに
払わせない代償が2つある: あるObjectが`==`でユーザーメソッドを走らせうるかは
1つの問いで決まり（クラスの特殊メソッド表の2スロット、またはShapeの
`any_special`フラグ）、走査が入り込んでいるコンテナの保持は遅らせる —
各段は自分の対をCスタック上で連結し（`JitEqWalk`）、ユーザーの`__eq__` /
`eq`が走る直前にだけretainする。そのメソッドがどれかへの最後の参照を
落としうるからで、走査自体が毎ステップ両方のサイズを見ながら添字で進むのも
同じ理由による。

**`<`の規則も1つ、それに答える関数も1つ**で、同じ形をしている。左の値が
何かで分かれる — Objectは`__lt__` / `__le__`、次にその`cmp`で答え、Tuple
どうしは`==`でない最初の対で、それ以外はスカラーとして答える:
`_culebra_value_order`（演算子を引数に取る）。4つの演算子がこれを
呼び、「前に並ぶ」を意味する他の全て — `sort` / `sorted`、`sort_by` /
`sorted_by`のキー、`min` / `max`と`min_by` / `max_by`のキー、導出された
`cmp`のフィールド — もこれを呼ぶ。後ろの3つは以前スカラーの段を直接呼んで
いたので、キーやフィールドがそれ自体Comparableだと、同じ対に`<`が答える
ところで`cannot compare Object and Object`になっていた。規則の最後の段
（`_culebra_scalar_order`）の名前は規則の外に現れてはならず、
`check_value_order_door.sh`がそれを保つ。Tupleの走査は`==`と同じ
`JitEqWalk`の連結に繋がるので、要素の`cmp`が走るとき、その要素を持つ
Tupleはretainされている。

**宣言を持たないクラスにも meta がある。** `Range`、`try_recv`や
`ws_receive`が返すバリアント、`wrap<T>`宣言で束ねたC++クラス — どれも
culebraで書かれたクラスを持たない値だが、いずれも meta に届く。Runtimeごとの
表がクラス1つにつき1つ持ち、pinする。表はcycle collectorから見えない根なので、
pinしないとtrial-deletionが「metaの参照数はインスタンスからの分で全部説明が
付く」と判断して、生きているインスタンスの足元でmetaを没収してしまう。
これがあるおかげで、識別を問う経路 — 名前・`==`・演算子・クラス型の引数・
多重ディスパッチの採点・traitの適合 — は meta だけを読めばよい。
ラップしたクラスは meta を2つ持ち（所有ハンドル用と借用ハンドル用。違うのは
`drop`だけ）、インスタンスではなくクラスについて真であることをそこに置く:
どのC++型か、インスタンスが別のisolateへ渡せないこと、そして列挙すべき
フィールドを持たないこと（`keys()`・`size()`・spread・JSON・表示は、id表へ
届くための管理情報を一切見せない）。

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
ある（`Compiler::declared_read_tag`、`culebra::class_fields_by_name`）。
`self.n += 1`の読みである`Op::PropWr`は、同じ答えを`d`で、DOTの位置を
持つconstの番号と並べて運ぶ（`Chunk::PropWrOperand`）。

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

入口の検査は名前を比べるので、読み手が注釈から受け取れるのは**名前**が
約束するものであり、1つの名前を持つものは1つとは限らない。2つのクラス、
あるいはクラスと並んでenum・enumのvariant・traitが同じ名前を持ち、その
インスタンスや適合する値も同じ検査を通る。そこで名前で引くレジストリ
（`culebra::class_fields_by_name`、読むのは`class_field_types_of`と
`class_field_classes_of`）は、名前ごとに、その名前で登録されたクラス全部が
同じ型で宣言しているfieldだけを保ち、クラスでないものがその名前を持てば
何も保たない（`culebra::register_class_fields`）。
`Compiler::settle_class_names`は、chunkを1つもemitする前に、コンパイル
全体の宣言を登録する（宣言で引く`FnAnalysis::class_fields`と
`FnAnalysis::enum_trait_decls`）。だから答えは、どの宣言が先にコンパイル
されるかにも、読み手がクラスより上にあるかどうかにも依らない。クラス自身の
メンバは名前を経由しない: `self`は宣言で読む。

stdlibの型は、どのレーンにも同じ形で届かなければならない。`@value`クラスは
宣言から登録する。焼き込み済みの入口を呼ぶレーンもその宣言はパースする
（`parse_baked_value_decls`）。それ以外の型は名前だけで知る
（`culebra::is_stdlib_type_name`: culebraソースのモジュールのクラス、組み込みの
trait、`Range`や`Generator`のようにネイティブに作られるオブジェクトが持つ
名前、そして`Any`）。プログラムのクラスがその名前、エラーのkindの名前
（`culebra::is_error_kind_name`: そのkindを持つクラスなしのObjectもその名前に
答える。この一覧をソースが発生させるkindに合わせておくのは
`tools/checks/check_error_kinds.sh`）、またはプリミティブ型の名前を持つなら、
その名前は何も約束しない。この一覧をソースに合わせておくのは
`culebra_preamble_cc`で、検査は片方向である: 一覧に無い型を宣言する
モジュールは焼き込みを拒否する。

登録は名前の答えを狭めるだけで、取り消されない。レジストリはコンパイルより
長く生きるので、REPLの後の行は前の行が宣言したクラスを読める。後の行が
同じ名前の2つ目のクラスを宣言すれば、それ以降にコンパイルされる読み手に
対して名前は狭まる。それより前にコンパイルされた読み手は、受け取った答えを
保つ。

`@value`のfieldが書けない名前は、モジュールごとに決まる。そのモジュールが
`@value`でないクラスに付けている名前で、宣言の場所は問わない。コンパイラ
（`Compiler::module_root_`を渡して`FnAnalysis::ordinary_classes`に尋ねる）と
lintは、同じモジュール根から同じ走査（`culebra::collect_ordinary_type_names`）で
それを読むので、両者の判定はずれない。

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
本体、`try`本体、`match`の腕、`if` / `cond` / `?:`の腕は、閉じる際に
1つの`Cleanup`エントリを記録する（innermost-first順）。そのslot範囲、deferを宣言していれば
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

handlerは`catch`節の並びで、`caught_slot`に対する`match`として
コンパイルされる — 腕の頭（`compile_arm_head`）は両者で共通である。
payloadを丸ごと束縛する節（`catch e`、`catch _`）はスロットの参照を
引き取り、検査をする節は借用して、節が値を受けた時点で解放する。
すべてを受ける節が無い場合、handlerは`CaughtPos`で始まる。これは
ガードが自前でthrowしてcatchする前に、値がどう届いたかを書き留める —
ユーザーのthrowならその位置、padがエンジンのエラーから値を作ったなら
その印である。handlerの末尾は`Rethrow`で、届いたときのものをもう一度
上げる（`culebra_runtime_rethrow_caught`）。ユーザーのthrowは同じ値が
その位置で進む。エンジンのエラーは、このhandlerが作ったObjectから
組み直したそのエラーとして進む: `CulebraError`とユーザーのthrowを
見分ける境界には`catch`が無かったときと同じものが届き、割り込みは
割り込みに戻る。これを値の形で決めないのは、エラーオブジェクトが
catchしたエラーの`throw e`で投げられる値でもあるからである。

`defer`本体は0-arityのクロージャであり、ランタイムのグローバルな
LIFO deferスタックに積まれる（`DeferPush`）。マークはフレームごと・
スコープごとに取られ（`DeferMark`）、`DeferRunTo`はそのマークまで
実行する。`try`は本体のfall-throughのdefer実行より前に自分の領域を
終えるので、try本体の正常な出口でthrowするdeferは自分自身の`catch`
から逃れる。

手順2の途中でdeferがthrowすると、その例外が進行中の例外を置き換え、
巻き戻しはそのスコープから置き換え後の例外で続く。`unwind`はそれを
`exception_ptr`として持ち、そのスコープの解放と外側の手順を最後まで
済ませ、途中のhandlerが引き取らなければフレームの末尾でre-raiseする。
`defer_run_to`がすでに置き換え後の例外をキャリアに入れ（置き換えられた
payloadの参照も終わらせ）ており、handlerが読むのはキャリアだけである。
唯一それを見ないhandlerはそのスコープ自身のもの — `try`本体のdeferは、
正常な出口と同じく、throwでの出口でも自分の`catch`の外にある。
プログラム終了はトップレベルの束縛を`drop`を発火させ
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

### 5.7 generatorはフレームを保持する

本体がyieldする関数（`fn name`、`fn`式、クラスのメソッド、traitの既定
メソッド）は書かれたままコンパイラに届き、そのchunkは
generatorになる（`Chunk::is_generator`）: 呼び出しはプロローグを実行して
イテレータを返し、本体は`yield`のたびにスタックを離れ、次のresumeで
戻るフレームの中で走る。effectの本体はそうではなく、コンパイラの手前で
変換される（§11）。

generatorもv0.7.0までは同じように、それぞれ状態クラスへ変換されていた
（§12）。

これを運ぶopcodeは2つ。`GenStart`はプロローグ — パラメータの束縛、
既定値の評価、型検査、`RecEnter` — の後に置かれるので、プロローグが
raiseするものは呼び出しがraiseする。`Yield a`はフレームを中断し、
`regs[a]`の`+1`はresumeした側へ渡る。`yield`式そのものの値はnilである。
どちらも`c`に、chunk内でのその中断点の番号（命令順に1から数える）を
持つ。そこで中断している間のフレームの状態であり、どのレーンでも同じ
番号である。

**closeはreturnである。** generatorは、フレームが中断している間に
イテレータがdisposeまたはdropされるとcloseされる。中断命令はそれぞれ
レジスタ`b`を名指し、resumeがそこに書く: 続行ならfalse、closeならtrue。
コンパイラはその命令の後に`JumpIfFalse b`を置き、その地点に書かれた
`return`がコンパイルされる出口 — 文の途中にある一時値、開いている
各スコープのdeferと束縛を内側から、フレーム自身のdefer、`Ret` — を
飛び越えさせる（`Compiler::emit_gen_suspend`）。したがってcloseは、
本体の`defer`をreturnと同じ順で走らせ、ローカルを同じ順で解放する。
それをするのはchunk自身の命令であり、unwinderは関与せず、3レーンが
1つのladderを共有する。本体からのthrowはフレームが終わるもう1つの
道で、これは通常のthrowである（§5.5）: 本体のdeferが走り、束縛が解放
され、generatorは終わる。

**stack map。** 中断しているフレームが所有するものは、中断の次の命令で
throwが起きたら解放されるものと同じである: そこで途中にある一時値
（`chunk_temps_at`）、続いて`Cleanup`チェーンを上る各スコープのslot
範囲。各slotはcellか値かのどちらかである（`chunk_gen_owned_slots`）。
`Chunk::gen_owned`はこれを中断点ごとに持つ — 点の数、各点のrunの開始
位置、run本体。エントリは`slot * 2 + is_cell` — 。組み立ては
`compile_unit`で、pcを動かしレジスタの所有者を決める除去パス
（§5.2.1）の後に行う。コレクタは中断フレームをこの表を通して読む
（`memory.md` §6.3）。

`yield from e`に専用の命令はない: コンパイラは`e`に対する`for`が行う
走査を、本体の位置に各要素の`Yield`を置いてemitする
（`Compiler::compile_yield_from`）。カーソル（`ForSlot`）は`for`と同じな
ので、その中断点でのcloseは、`for`の本体からの`return`と同じように、
走査中のイテレータを出がけに閉じる。要素の`+1`はカーソルから、
フレームをresumeした側へ直接渡る。

`yield`を拒否する場所は今も2種類ある（`generator_rules.h`）: `defer`
（closeがそれを走らせるので、再び中断できない）と、関数やメソッドの本体
でない場所（トップレベル、`|...|`ラムダ、`new`、`drop`）である。`try`や
`catch`の中の`yield`はフレームの普通のコードである: 領域はhandlerつきの
pc範囲で、resumeは同じpcからそこへ戻り、closeは`return`と同じように
そこを抜ける。yieldするメソッドは、フレームにレシーバを持つ普通のメソッド
としてコンパイルされ、yieldする`@value`のメソッドは呼び出し側に展開されず
呼ばれる（`is_straightline_body`）。

書かれたままコンパイルされた本体は、名前の束縛も文の実行も普通の
`fn`と同じで、§10.7がそれを保つ。変換されたクラスが別の答えを返して
いて、プログラムから見えるものは§12にある。

## 6. executor

`vm::Exec`はラベルのアドレスを並べた表を引いてディスパッチする。各腕は
`switch`が共有する1箇所へ戻るのではなく、腕ごとに自前の間接jumpで次の
opcodeの腕へ飛ぶ。共有された1箇所はプログラムが実行する全opcodeを見るので
どれも学習できないが、腕ごとの分岐点はそのopcodeの次に来るものだけを見る
ので、たいてい行き先は1つに決まる。表はopcodeで引くので、並びは`Op`列挙の順そのものである。行を足さずに
opcodeを増やせばstatic_assertが落ちるが、**行の位置が違う**ことは言語の側では
捕まらない — ラベルのアドレスは定数式ではないので、表が自分で添字を書くことが
できない。並びを保つのは`tools/checks/check_vm_dispatch_table.sh`で、
書き下された唯一の並びである`kNames`と突き合わせる。

各腕は`do { … } while (0)`である。これが、書き換えではなく改名で済ませる
仕掛けになっている: 腕がもともと持っていた`break`は今もその腕を出る意味の
ままで、入れ子のループの`break`はそのループに掛かり、腕を出るのは通常の
スコープ脱出のままなので、腕が作ったもののdestructorも走る。ラベルと
gotoを裸で並べた形にはこの性質がない — 生きたスコープからcomputed gotoで
飛び出すとdestructorが飛ばされ、switchには無かった漏れになる。

`run_frame`はフレームのレジスタウィンドウを機械スタック上の可変長配列として確保
し（chunkの`num_slots`からサイズを決めるので、小さな関数は小さな
フレームしか払わない）、プロローグでパラメータ・receiver・`fn`
ハンドルを束縛して（`bind_params`）`dispatch`に入る。このウィンドウが
C++スタック上にあるのはコレクタのためである: 保守的スキャンが登録なしに
すべてのレジスタをrootとして見つけられる。

コンパイラが解決した呼び出し（`Chunk::call_targets`）は`run_frame`に
入り直さない。`dispatch`が呼び先のフレーム — `VmFrame`の記録、その
レジスタ、所有スタックのmark — をRuntimeの`VmStack`に積み、同じループの
まま走り続ける（`enter_inline`）。`Ret`がそれを降ろして呼び出し側を再開
する（`leave_inline`）。これはLuaと同じ形である: ネイティブコードからの
1回の進入につきC++の活性化は1つで、その下のculebraの呼び出しがどれだけ
深くても変わらない。呼び出しにC++のプロローグも機械スタック上のレジスタ
ウィンドウも要らず、throwはC++のフレームを越えずにculebraのフレームを
越え、再帰の上限は機械スタックの許す量ではなく言語が定めた値になる。
`VmStack`はヒープ上のメモリで機械スタックのスキャンは歩かないので、
`Exec::prepare`が`vm_stack_roots`をインストールする: コレクタはスタックをフレームごとに歩き（記録が自分の
ブロックの長さを持つ）、各レジスタのpayloadをrootの候補として取る。
機械スタックのスキャンと同じ規則であり、タグでは絞らない — cellスロットの
`JitCell`やfor-inカーソルのクロージャは、中身と無関係なタグで載っている
からである。`VmStack`に載せられない
フレーム — デバッグセッション（`run_frame`のguardでフレームスタックを
保つ）と、segmentより大きなchunk — は、解決されなかった呼び出しと同じく
`run_frame`を通る。

ループは命令ポインタだけで回り、unwinderのために各dispatchでそれを
フレームに書き込む。途中で読み戻すことはなく、これがpcを各命令の
クリティカルパスから外している。

### 6.1 クロージャとtrampoline

executorのクロージャは本物の`JitClosure`であり、`fn_ptr`は
`Exec::trampoline`である。executorのクロージャは全部この1つの番地を
共有するので、どのchunkを解釈するかを言うのはクロージャの`meta`である。
キーワード解決器が読むパラメータのメタデータは`VmChunkRef`の先頭
メンバで、その後ろに`VmFnDesc`（`{program, chunk}`）がある。ネイティブ
コードはVM関数をloweringされた関数と全く同じ方法で呼ぶ。クロージャを
部品から組み直す側（isolateの受け取り側、遅延名前空間のレジストリ）は
もともと`fn_ptr`・フラグ・`meta`を写していて、それで全部である。
captureは本体自身のものだけで、loweringされたクロージャと同じ配列に
なる。

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

descriptorは以前`captures[0]`のcellに載っていて、`MakeClosure`ごとに
1つあった（chunkのすべてのクロージャで1つのcellを共有するのは誤り:
1つのプログラムのクロージャは複数のisolateで同時に実行されることが
あり、`JitCell`の参照カウントは単なる`int64_t`である）。そのcellは
loweringのレーンには無いオブジェクトだったので、executorでは
`GC.stat().rc_objects`が生きているクロージャ1つにつき1多かった。
descriptorは不変でプログラムのものなので、クロージャごとのカウントも
持ち主も要らない。いまは両レーンがクロージャに同じオブジェクトを確保し、
refcountレーンのdifftestの許可リストは空である。

### 6.2 throw

`run_frame`は`dispatch`を`catch (...)`で包み`unwind`を呼ぶ。これは
§5.5のテーブルを歩き、handlerで再開するかフレームをカウント解除
してre-raiseする。その上に積まれたインラインのフレームも同じhandlerの
中で、上から順に巻き戻され（`unwind_frames`）、1つずつ降ろされる。
スコープの巻き戻し中に`defer`がthrowすると、その例外が下のフレームに
とっての進行中の例外に置き換わる。かつては`run_frame`の`catch`の入れ子が
これをただで与えていたが、`unwind_frames`は自分のhandlerの内側から
再帰することで同じ入れ子を保つ。捕捉されなかったエラーはエンジン境界
（`run_prepared`）でフォーマットされる: ユーザーの`throw`はどの
レーンも表示する同じ`uncaught: …`の行になり、位置を持たない
`CulebraError`は最後に公開されたop位置から埋め戻される。interruptは
そのどちらにも届かない: `culebra::Interrupted`という基底を持たない
独立の型なので、エラーを報告するために書かれたハンドラはそれを
捕まえる型を名指せない。スクリプト側への届き方は変わらない —
padの分類はC++の型ではなくpending carrier（§5.5）を通す。

ライブラリのうちCulebraで書かれたモジュールは、`__raise(kind, message)`で
エラーを発生させる。これはnativeのモジュールが投げるのと同じ
`CulebraError`を投げるnativeで、モジュールがどちらの言語で書かれていても、
ライブラリが発生させるものは1種類になる。`check_error_kinds.sh`は、
エラーをObjectとして投げるライブラリのソース（モジュール、組み込みtraitの
既定メソッド、変換器が書くコード）と、Stringとして投げるもの（モジュール）
を拒否する。`__raise_at`は位置も受け取る。フレームの出口では位置を
決められないエラーのためのもので、2つある。ハンドラのない`perform`は、
原因のコードがライブラリのフレームに入っていない。`perform`なしで
呼ばれた操作は、スタブがプログラム自身のソースに書き込まれるので、
宣言の位置を報告する。

ライブラリの中で起きたエラーは、ユーザーがそのライブラリを呼んだ位置を
報告する。コンパイラはライブラリのソース（`<stdlib>`のpreamble、
`<builtin>`のtrait、`culebra test`の`<test>`のambient）から取る位置すべての
行に`kLibraryLineBit`を立てるので、印は既存の経路を変更なしで運ばれる: chunkの位置、loweringの定数、
公開される呼び出し位置、引数位置、carrier。ライブラリの関数はprologueで
自分を呼んだ位置を控え（`PosSnap`で`Cleanup::site_slot`へ）、フレームの
段でその位置を`culebra_runtime_reanchor`に渡す: 印の付いた位置のまま
フレームを抜けるエラーは、控えた位置がユーザーのものなら、その位置の
同じエラーに置き換わる（throwする`defer`が置き換えるのと同じ）。控えた
位置に印があれば、その位置が属するライブラリのフレームに任せる。executorの
`unwind`とloweringのframe padは同じ段で同じヘルパーを呼び、ヘルパーは
carrierしか読まないので、JITのpadにC++の再検査は要らない。フォーマッタまで
印が残った位置（上にユーザーのフレームが無いthrow）は、位置なしで表示する。

控える位置は、読む呼び出し位置が正しい限りでしか正しくない。runtimeは
自分でもclosureを起動する: 演算子のdunder、キーの`hash`/`eq`、getter、
`__str__`、イテレータの`next`。そうした起動はどれも、公開されたop位置を
呼び出し位置として貸し（`JitBorrowedCallSite`、`_culebra_invoke_method*`と
反復の起動のすべての周り）、そこへ到達しうる入口はどれも先に自分のop位置を
公開する — 演算子と添字のヘルパーは自分の`line`/`col`から、ネイティブの
trampolineと組み込みメソッドの腕は呼び出しから、`Disp`・getterの読み出し・
`ForNext`は命令から。deferの本体と`drop`は位置を貸さない: それを囲む
ライブラリのフレームが付け替える。

### 6.3 safepoint

ループは`Safepoint`をemitする。これはプロセス全体のwakeフラグ
（Ctrl-Cとisolateごとのcancel）をpollし、`Interrupted`をthrowする。
wasmでは同じpollがコレクタが動く場所でもある（§9）: 閾値超過は
pending flagを立てるだけであり、executorは次の命令境界でcollectを
行う — そこではすべてのフレームの生きた値がスキャンの見えるレジスタ
ウィンドウの中にある（機械スタック上か、rootのフックが覆う`VmStack`
上か）。

### 6.4 デバッグ対応

`Debug::Step`でコンパイルすると、ユーザーソースのすべての文が
`DbgStmt`をemitし、これがスレッドローカルな`DbgState::hook`を
呼ぶ。`run_frame`は入口で`DbgFrame`（program、chunk、レジスタ
ウィンドウ、pc）をpushし、すべての出口でpopする。追跡中のセッションは
インラインのフレームを作らないので、culebraのどのフレームにも
`run_frame`がある。`Debug::Break`は
`debugger`文が必要とするものだけをemitする。通常の実行
（`Debug::Off`）はどちらもemitしない。

### 6.5 generatorのフレーム

`GenStart`（§5.7）はフレームをスタックから外す。レジスタウィンドウと
owned markを`JitGenFrame`（`rt/gen.inc.h`）にコピーし、ウィンドウを
クリアし — 値はもうフレームのものである — 、returnと同じようにその
命令から出る。その値はフレームがぶら下がるオブジェクトで、これが
プログラムの持つイテレータである。フレームはprogram、chunk、戻るべき
pcを記録する。

`Exec::gen_resume`がそれを戻す: 機械スタック上のウィンドウに
レジスタとmarkをコピーして（フレーム側はクリアする）、中断命令の`b`
レジスタにcloseフラグを書き、保存したpcからdispatchに入る。`Yield`では
dispatchがyieldされた値を持って戻り、レジスタはフレームへ帰る。
レジスタに入っていないものが3つ、一緒に動く:

- **defer。** deferスタックは`Runtime`ごとに1本のLIFOで、markは高さで
  ある。中断するフレームの未実行のdeferはスタックから切り取って
  フレームに持たせ、resumeでpushし直し、そのpcで開いているスコープの
  defer markレジスタを高さの差だけ動かす（`gen_rebase_defers`）。
- **再帰の深さ。** resumeは1フレームとして数え、yieldで数え戻す。
  数えるのは何かを動かす前なので、resumeでのRecursionErrorはフレームを
  中断したまま無傷で残す。
- **呼び出し元の位置。** ライブラリのフレームが公開する呼び出し位置は、
  いまresumeした側のものになる（`culebra_runtime_gen_resnap_site`）。

owned stackは何も要らない: そのmarkは高さでなくidである。デバッグ
セッションでは、resumeされたウィンドウを`DbgFrame`としてpushする
（§6.4）ので、本体の中で止まると、本体がresumeした側の上に見える。

イテレータのメソッド（`iter`、`has_next`、`next`、`dispose`、および
オブジェクトがプログラムに出ていくときに束縛される`drop`）は、すべての
generatorが共有する1つのmeta（`_jit_gen_meta`）上のnativeで、それぞれ
レシーバから自分のフレームを引く。`has_next()`は本体をresumeして値を
1つ先読みする。`--jit`でもビルドしたバイナリでも同じもので、違うのは
フレームのresumeの仕方だけである（§7.3）。

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
`CULEBRA_JIT_CACHE`はobject cache（`JIT::FileObjectCache`）を有効に
する。objectには2つのキーから届き、どちらのキーも、それが飛ばさせる
段が読むはずだったものだけでできている:

| キー | 中身 | ヒットで飛ぶもの | 有効な範囲 |
|---|---|---|---|
| ソースキー（`jit_module_name`） | プログラムのソースとオプション、このバイナリのパス・サイズ・時刻 | IRパイプラインとバックエンド | それを書いたバイナリ |
| objectキー（`object_key_for`） | 最適化後のモジュールのテキストと`backend_identity`: バイナリが持つLLVM、ホストCPU、`kBackendKnobs`の設定 | バックエンド | このモジュールをこのバックエンドに渡すすべてのビルド |

`run_program`はloweringの後でソースキーで尋ね
（`JIT::cached_object`）、ヒットすれば保存されたobjectをリンクして
モジュールは読まずに捨てる。ミスならパイプラインを走らせ、object
キーでもう一度尋ねる（`JIT::keyed_object`）。このときobjectキーを
ソースキーの下に記録し、モジュールをその名前にする — バックエンドが
走ることになった場合にcompile layerがobjectを保存する名前である。
lowering・ランタイム・パイプラインへの変更はobjectキーに入れる必要が
ない: それが変えたものはモジュールのテキストに現れる。テキストに
現れないのはバックエンド自身なので、その素性は明示してある — LLVMは
CMakeがリンクしたライブラリの印（`CULEBRA_LLVM_BUILD_ID`。パッケージの
snapshotは同じ版数のままライブラリを差し替える）で、バックエンドの
設定は`tune_backend`がそれを適用するのと同じ表で。エントリは名前の
隣に書いてからrenameで置き、ヒットは時刻を更新するので、追い出される
のは最も長く読まれていないエントリである。
`tests/jit_cache_test.sh`が、どちらのキーでヒットした実行もcold
startの実行と同じであることを、2つのバイナリにまたがって保っている。
そしてfaststartもcacheも、coldな`--jit`の
起動が安い理由ではない: プログラムが名前で呼ぶstdlibモジュールも、
どのプログラムも登録する組み込みtraitも、そもそもモジュールの中に
無い（§2、焼き込みpreamble）ので、loweringされるのはユーザーの
コードである。距離のほとんどはこれで稼いでいる — `let x = 1`が
loweringするIRは6,167行ではなく758行、起動は82msではなく7msになる。
`Lowering::build_preamble_object`
は同じloweringをモジュールごとのエントリ名で走らせたもので、ビルド時
に`culebra_preamble_cc`が実行する。`lower_program`は`__culebra_main`を
プログラムが名前で呼ぶ焼き込みエントリそれぞれへの呼び出しで開き、
シンボルはレーンのリンクが供給する — JITはドライバの表から定義し
（`JIT::define_baked_preambles`）、ビルドされたバイナリはアーカイブ
メンバーを引く。

起動の残りが何に使われているかは`CULEBRA_JIT_TIME_PASSES`で読める:
4つのフェーズ（lower・optimize・codegen・run）と、IRパイプラインと
バックエンドそれぞれについてのLLVM自身のパス別レポートである。
cacheのヒットは`cached`と報告される — ソースキーが答えたときは
`optimize`の代わりに、objectキーが答えたときはその後に — そして
`codegen`はobjectのリンクになる。
`tests/`のどのファイルでも実行は数msで、残りの大きい方の半分は
バックエンドにある。フラットなスクリプト — 1つの関数で、トップ
レベルのスロットとthread-stateポインタがその全体にわたって生きて
いる — はそのほとんどをregister coalescerに使っていた。呼び出し
サイトごとのそれらのコピーを、interval全体に対して1つずつjoinする
からである。`JIT::tune_backend`はこれをLLVMの
`large-interval-freq-threshold`（256から16へ）で抑える。`tools/bench/`
のthroughputの行は動かない。ホットループ自身のコピーが先にjoin
されるからである。この上限はrefcountガードの形に敏感で、
`emit_tag_is_refcounted`でsentinelタグを`& 31`で畳む形（サイトごとに
2命令少なく、モジュールは5%小さい）にすると同じファイルのcoalescerが
1.2sから9.2sに戻る。範囲テストはそのために残している。

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

ステップのdefer実行は、padの中でただ1つの`invoke`である。そのunwind
edgeはrelay（`emit_landingpad`の`replaces`モード）で、領域が運んでいた
例外をそこで終わらせ — その場で開いて閉じる。それが例外オブジェクトを
解放する — 置き換え後の例外を領域のslotに入れてステップに戻るので、
ladderの残りはその例外で走る。`try`スコープはどちらのedgeから来たかを
phiで読み、自分のdeferが投げた置き換えは分類せずに先へ渡す。

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

### 7.3 generatorの本体

generatorのchunk（§5.7）は2つのLLVM関数になる。`__vm_fn_N`は
クロージャが名指すJitFnで、rampである: `culebra_runtime_gen_ramp`を
1回呼ぶだけで、これがレジスタをnilにしたフレームを確保し、本体の
プロローグをその中で走らせ、本体が`GenStart`で出たらイテレータを返す。
`__vm_gen_N`は同じ`lower_chunk`を通したchunkで、フレームの
`JitGenRegs`を指す7番目のパラメータを持つ。普通のchunkとの違いは
5箇所である:

- レジスタは`alloca`でなくフレームのレジスタ配列へのGEPで、owned
  markも同じ。したがって中断時に何もコピーしない
- entryブロックは、フレームに保存した状態での`switch`で終わる。
  中断点ごとに1 case、defaultはプロローグである
- `GenStart`と`Yield`は中断点の番号をstoreして`ret`する
- resumeが入るブロックは、padが戻す先の再帰の深さをstoreし、そのpcの
  cleanupチェーン（中断点ごとに静的に決まる）のdefer mark slotに
  deferスタックの移動量を足し、ライブラリのフレームなら呼び出し位置を
  公開し直し、`b`レジスタにcloseフラグを書いて次の命令へ分岐する
- `for-in`カーソルのnativeな6フィールドは、entryブロックのallocaでは
  なくフレームのscratchワードに置く

loweringがレジスタの外に持つその他のもの — 引数のslab、unwind用の
一時値プール、例外slot — は1命令の中で完結するので`alloca`のままで
ある。cleanup padは変わらない: closeはそこに届かず（§5.7）、本体からの
throwはどの関数とも同じようにpadを使う。chunkのstack mapはprivateな
定数（`__vm_gen_owned_N`）としてrampに渡る。ビルドしたバイナリには
問い合わせる`Chunk`がないからである。

resumeの後にフレームから読み直したレジスタはtagが分からないので、
resumeに続くループは、普通の関数の同じループなら畳み込まれるtag
dispatchを持ち続ける: 300回のyieldの間に3000万回のFloat演算を回す形が
0.55秒、普通のループは0.12秒（`just build`）。

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
`Dir.embedded(...)`が基準にするディレクトリ）、自分のisolate join
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

`vm::Embed`（`vm/embed.h`）はC++ホストAPI（`deployment.md` §2）で
あり、自分が実行するスクリプトより長生きする束縛を持つセッション
である。これによりホストはソースを実行してからグローバルを読んだり
関数を呼んだりできる。`vm::Value`は境界を越える値の所有ハンドルで
あり、すべてのretainとreleaseはその内側に留まる。各`Embed`は自分
自身の`ReplSession`を持ち、すべての呼び出しの間それをswap inする
ので、1つのスレッド上の2つのembedは何も共有しない。

値の操作面はexecutor自身のopcodeが呼ぶのと同じランタイムヘルパを
通る。`Value::operator[]`と`at()`は`Op::Index`の、`set()`は
`Op::IndexSet`のディスパッチであり、ホストの読みはスクリプトの読みと
同じKeyErrorを投げ、ホストの書き込みも同じ`mut`フラグに従う。Object
とArrayは参照なので、読みが返すのはコピーではなく親が持つその値で
ある。`Embed::eval`は失敗を整形テキストではなく`CulebraError`で返す
入口で、`Session::run_reported`が押し込むメッセージの横に構造化された
エラーを残しているために成り立つ（整形すると種別と位置は1行に
畳まれてしまう）。

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
`tools/checks/check_rt_archive_tls.sh`）。さらに、base archiveが
解決するものはそれをリンクするプログラムが供給できるものでなければ
ならない — そこから機能側の外部ライブラリを参照すると、その機能を
名指していないプログラムのリンクが壊れる
（`tools/checks/check_rt_archive_backend_free.sh`）。

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

### 10.6 名前解決をresolve.hと突き合わせる

スコープの規則は`include/frontend/resolve.h`（エディタ支援が読み、
`FnAnalysis`のcapture（§4）の源にもなる解決器）に1度だけ書かれ、
コンパイラは自前のスコープのスタックを持つ。`CULEBRA_SCOPE_CHECK`が
ディレクトリを指すと、コンパイラは引いたすべての名前をresolve.hと
突き合わせて報告する
（`include/frontend/scope_check.h`）。名前のノードを引くときは
`lookup_at`（と`check_use`）を、宣言は`push_binding`（と
`note_declaration`）を通るので、resolve.hと別の変数を読み書きする
出現、クロージャが1つのcellから捕獲した変数に2つ目のcellが
できること、cellでない捕獲がそれぞれ所見になる。resolve.hには、
コンパイラがソースの外から知っていること、つまりstdlibのグローバルな
名前（それへの裸の書き込みは宣言でなく拒否される）と、セッションでは
前の入力が宣言した名前を渡す。
`tools/checks/scope_agreement.py`は、コーパス、生成した格子（スコープ
を開く構文 × 宣言の形、外側に同名の変数がある場合とない場合）、
`culebra test`のセッション、REPLのテストをこの状態でコンパイルし、
`tools/checks/scope_agreement_allow.txt`にない所見、載っているのに
もう出ない所見、成功したのに報告を書かなかったコンパイルのどれかで
失敗する。その行`scope agreement`は`just test`とCIで走る。検査の有無で出力するバイトコードは変わらない。

スコープの規則を読むほかの箇所は、規則を書き直さずresolve.hから
受け取る。ロード時のlint（`include/frontend/lint.h`）はモジュールごとに
1度解決し、スコープに関わる3つの検査をその結果から読む。見える宣言の
ない読みが未定義名の`NameError`、関数が書かれた位置で外側の関数の
変数である名前の宣言が`ShadowError`、`let`の後のその変数への裸の
書き込みが`ImmutableError`になる。解決するのは書かれたままの
モジュールで、effectの変換が本体を状態機械に置き換える前である
（`lint::scope_diagnostics`。ローダーがパースと変換の間で呼ぶ）。
だから変換される本体も、普通の本体と同じ3つの検査を受ける。
節点の位置が許すものの検査（`RuleWalker`）は変換後のモジュールを読む。
effectの変換は、パースし直した本体を書き換えるので、
書き換える木そのものを解決し（`resolve::resolve_body`。本体を引数の
下で、関数の周囲から見える名前を渡して解決する）、名前がどの変数かを
その結果から読む。変数が状態インスタンス上に自分のslotを得るのは、
それを持つスコープに状態機械が入るとき、つまりそのスコープの文が
別々の状態に分かれるときである（`PromotedLocals`）。中断で割られない
スコープは書かれたとおりに出力され、その変数は元のローカルのまま残る。
slotは値を持つだけなので、その変数が拒む代入はコンパイラに返す。
代入は、その名前が再びローカルになるブロック（slotの値で初期化した
`let`）の中に出力され、どのローカルでも出るのと同じ`ImmutableError`を
出す。
スコープであるブロックはその節点の下に記録され
（`Resolution::block_scope`）、関数のものでないスコープがすべてそこに
あることを`resolve_test`が確かめる。

### 10.7 generatorとeffectの本体を普通の関数と突き合わせる

effectの本体はソースからソースへ変換される（§11）ので、同じ文が
普通の`fn`で持つ意味と一致することを、どのエンジンも保証しない。
generatorの本体は書かれたままコンパイルされるが、`yield`をまたいで
持つものは、スタックを離れたフレームの中にある（§5.7）。
`tools/checks/lowering_diff.py`は1つの文を、普通の関数、
generatorの本体、effectの本体の同じ行と列に書き、それぞれが出力する
ものと投げるもの（種類、文面、位置、ロード時か実行時か）を比べる。
掃引するのは、文の形 × 条件 × 値を本体の途中・末尾・唯一の文として
書いたもの、`for`が歩く対象、そして名前が書かれた位置で持つ意味
（宣言か再代入か、届くか届かないか、可変か不変か、どのクロージャが
捕獲するか）。異なると分かっている形は、そのケース数とともに
`tools/checks/lowering_diff_allow.txt`に載せる。載っていない違い、
載っているのにケース数が変わった形、もう一致する形のどれかで失敗する。
載っている形はすべてeffectの変換のものである: generatorの本体が普通の
関数と異なるケースは、その形が載っていてもいなくても失敗する。
その行`lowering diff`は`just test`とCIで走る。

### 10.8 generatorのプローブ

yieldする`tests/*.cul`のファイルは他と同じコーパスで、上のレーンが
すべて掃く。`tests/gen_frames/`にあるのは、assertionでは測りにくい
ものである: closeが各種の中断点でdeferとdropを走らせる順序、中断中の
フレームを通してcollectionが回収するもの、値としてのgeneratorを出力する
プログラムで、それぞれ凍結した出力と突き合わせる
（`tools/checks/gen_frames_probes.sh`。`--freeze`が出力を書き直す）。
`just test-dev`の行はこれをexecutorと`--jit`で回す。`just test`の行は
`CULEBRA_GC_STRESS`・`CULEBRA_GC_REFS`・リーク監査の下で両エンジンで、
さらに`culebra build`を通して回す。WindowsのCIは前者を回す。

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
- **effectsはAST→AST変換で、generatorはフレームを保持する。**
  `effects_transform.h`は`effect fn` / `perform` / `handle`を`__Eff`
  ランタイム上の普通のソースに書き換える。制御フローをflat-dispatchの
  CPS状態機械を通じてloweringし、中断で割られるスコープの変数をstate
  instance上に持つ。エンジンはeffect固有の対応を一切必要としないので、
  構造的に一致する。generatorも以前は同じように、`yield`する関数を
  イテレータプロトコルを実装するクラスへ変換していた。エンジンが
  フレームを中断できるようになって、それをやめた（§5.7）。変換は
  `yield`がスコープから切り離す変数をすべてslotにし、slotは値しか
  持てないので、変換された本体は、同じ文を普通の関数に書いたものと
  いくつかの形で異なっていた（§10.7）。書かれたままコンパイルされた
  本体にはその一覧がなく、bytecodeも少ない（`tests/test_generator.cul`は
  変換では24,364命令だったものが5,714命令になる）。effectの継続は
  複数回resumeできるが、ヒープとスタックの間を移るフレームはそれに
  応えられないので、effectは変換のままである。
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

generatorもかつてはeffectの本体と同じように変換されていた: `yield`する
`fn name`は、ソース上で、イテレータプロトコル（`iter` / `has_next` /
`next` / `dispose`）を実装するクラスと、flat-dispatchの状態機械に書き
換えられ、`yield`で割られる変数はインスタンスに置かれた。v0.7.0まで
はそれが唯一の実装だった。§5.7のgeneratorはそれを3段階で置き換えた:
フレームのランタイムとコンパイラ・エンジン側の経路を環境変数の裏に
置く段階、既定を切り替える段階（古い経路は環境変数
`CULEBRA_GEN_LOWERED`1つ分の距離に残し、2つを比べるために使った）、
そして変換の削除である。最後の段階で、それまでソースを
`for v in (e) { yield v }`に書き換えていた`yield from`もコンパイラへ
移った。フレームを保持する本体が、変換されたクラスと違うこと
（プログラムから見えるもの）:

- `type_of(g)`は`'Generator'`で、値は`Generator {}`と表示され、自前の
  プロパティを持たない（変換版: generator関数ごとに1クラスで、状態の
  フィールドが見える）。`==`は同一性である（変換版: フィールドごとの
  比較なので、作りたての`count(3)`2つは等しい）
- generatorはSendableでない: 送ると`SendError: a generator is not
  Sendable`になる（変換版は状態ごとコピーされる）
- `has_next()`が先読みした値は`dispose()`で解放される（変換版:
  `next()`がまだそれを返す）
- `drop`を持つローカルは、そのスコープの終わりでdropされる（変換版:
  generatorが死ぬとき）
- 本体からのthrowはdeferを走らせてgeneratorを終わらせる（変換版:
  deferは`dispose()`を待ち、次の`has_next()`はthrowした状態をもう一度
  実行する）
- 本体の中から自分自身をresumeすると`ValueError: generator already
  running`になる（変換版: 再帰してRecursionErrorに至る）
- 式の中の文ブロックがyieldしてよい（変換版: SyntaxError
  「unsupported control flow」）

`tests/test_generator.cul`を`--jit`でコンパイルするCPU時間は、変換版の
35.1秒に対してフレーム版は9.5秒で、バイトコードは24,364命令から
5,714命令になった。上の一覧のうち、v0.7.0に対するrelease-diffゲートが
見たケースは`tools/difftest/release_diff_allow.txt`にある。
