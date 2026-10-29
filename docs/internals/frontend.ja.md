フロントエンド: ソーステキストからコンパイラの入力まで
=====================================================

`.cul`ファイルが`vm::Compiler`に届くまでに起きること。文法、パーサ、
構文エラーのメッセージ、モジュールローダ、AST→ASTの書き換え（lowering）、そして
後段のすべてが読む名前解決を扱います。バイトコードとそれを消費する
3つのレーンは[`vm.md`](vm.ja.md)、`include/frontend/`のディレクトリ規則は
[`layout.md`](layout.ja.md)です。英語版は[`frontend.md`](frontend.md)。

目次
----

1. [フロントエンドの位置](#1-フロントエンドの位置)
2. [文法と保存データ](#2-文法と保存データ)
3. [パース](#3-パース)
4. [構文エラー](#4-構文エラー)
5. [モジュールのロード](#5-モジュールのロード)
6. [コンパイラの前に走る書き換え](#6-コンパイラの前に走る書き換え)
7. [名前とスコープ](#7-名前とスコープ)
8. [同じフロントエンド上のlintとfmt](#8-同じフロントエンド上のlintとfmt)
9. [文法を変えるとき](#9-文法を変えるとき)
10. [この層が決めたこと](#10-この層が決めたこと)

---

## 1. フロントエンドの位置

フロントエンドは1つです。executor、`--jit`、`culebra build`は、この層が
作るASTから`vm::Compiler`が作ったバイトコードを読みます（そうなった経緯は
`docs/essays/one-front-end.md`にあります）。以下はすべてモジュールごとに
1回、この順序で走ります。

```text
source text
  │  culebra::parse                 parser.h
  │    PEG grammar -> AST as written; regex literals and postfix
  │    modifiers desugared
  │
  ├─ lint::scope_diagnostics        lint.h, over resolve.h
  │    reads the AST as written, before any lowering
  │
  │  apply_transforms               effects_transform.h
  │    check_generators_in -> transform_effects_in -> reject_*
  ▼
the AST the compiler reads
  │  FnAnalysis                     fn_analysis.h, over resolve.h
  ▼
vm::Compiler                        include/vm/vm.h
```

`include/frontend/`には15本のヘッダがあり、すべてヘッダオンリーです。
それぞれの役割:

| ファイル | 役割 |
|---|---|
| `grammar_def.h` | PEG文法。1つの生文字列で、構文を定義する唯一の場所 |
| `grammar_blob_key.h`、`grammar_blob.gen.h` | シリアライズした文法と、それがまだ合っているかを示すハッシュ（§2） |
| `parser.h` | パーサ、ASTの上の`view_*`アクセサ、パースの入口、脱糖、いくつかの構造的な拒否 |
| `syntax_error.h` | peglibのエラーレポートをメッセージにする（§4） |
| `module_loader.h`、`embed_paths.h` | プログラムをモジュールの列としてロードする（§5） |
| `effects_transform.h`、`state_lowering.h`、`fragments.h`、`generator_rules.h` | コンパイラの前に走る書き換え（§6） |
| `resolve.h`、`scope_check.h`、`fn_analysis.h` | 名前、スコープ、キャプチャ（§7） |
| `lint.h` | 静的検査。`resolve.h`と書き換えを共有する（§8） |

誰が何をincludeするか: コンパイラ（`vm/vm.h`）は`module_loader.h`、
`fn_analysis.h`、`scope_check.h`を通してフロントエンドに届きます。フォーマッタ、
言語サーバ、REPL、デバッガ、テストランナーは、`parser.h`、`module_loader.h`、
`resolve.h`のどれかを通ります。

---

## 2. 文法と保存データ

`grammar_def.h`は`const auto grammar_ = R"(...)"`が1つあるだけです。ツリーの
中で構文を定義するのはここだけです。エディタ用の文法（`misc/culebra.peg`と、
vim・VS Code・ZedのファイルおよびPlaygroundのキーワードブロック）は、
`just sync-grammar`がここから生成します。

cpp-peglibで文法を読むには文法テキストのメタパースが要り、ソースはそれを
約10msとしています。数ミリ秒で終わるプログラムが毎回それを払うべきではないので、
ビルドは文法の保存データ（blob）を持ちます:

- `tools/gen_grammar_blob.cc`が`grammar_`を素の`peg::parser`に読み込み、
  `serialize_grammar()`して`grammar_blob.gen.h`を書きます。中身はバイト配列、
  `GRAMMAR_BLOB_HASH`、そしてエラーフォーマッタが要る3つのリスト（§4）です。
  `just gen-blob`が実行し、`just check-blob`は`--check`付きで実行します
  （メモリ上で再生成してバイト単位で比べます）。
- `grammar_blob_key()`は、`grammar_`、区切りのバイト、`CPPPEGLIB_VERSION`に
  対するFNV-1aハッシュです。保存データ自身の先頭の識別子（magic）はpeglibのバージョンが変わっても
  変わらないので、レイアウトの変化に気づくのはこの鍵です。
- `get_parser()`（`thread_local`な`peg::parser`）は、`GRAMMAR_BLOB_HASH`が
  鍵と等しければ保存データを読み、そうでなければ`load_grammar(grammar_)`に
  フォールバックします。それも失敗すると`std::logic_error("invalid peg grammar")`
  をthrowします。

**古い保存データは正しく動き、遅いだけです。** フォールバックがそれを隠すので、
パーサのテストは緑のままです。それを見つけられるのは`just check-blob`だけで、
そのため`check-generated`に含まれ、CIにも独立したステップとして入っています。

---

## 3. パース

`get_parser()`はパーサをスレッドごとに1回設定します:

- `enable_ast(true, true, ast_optimizer_keep_rules())`。optimizerは子が1つの
  ノードをその子に畳みますが、keepリストのルールは畳みません。ASTを読むコードが名前で
  探すためにノードのまま残すべき新しいルールは、`ast_optimizer_keep_rules()`に
  足す必要があります。`FOR_BINDING`は意図的にkeepしていません。
- `enable_packrat_parsing()`。保存データはpackratフラグを持っていて、この呼び出しは
  それをリセットせずに再適用します。
- `set_max_depth(kCulebraParseDepthLimit)`、4000。単位はルールのマッチ回数で、
  括弧1段あたり約6です。テストコーパスで最も深いファイルは101に届き、ネイティブ
  スタックが尽きるには約120,000が要ります。peglibは返すASTも同じ深さに
  抑えるので、下流で木をたどるコードすべてがこれで守られます。深さの失敗は
  `reword_parse_depth_error`が言い換えます。
- `INTERPOLATED_STRING`、`TRIPLE_STRING`、`INTERP_EXPR`の`leave`フック。
  文字列がどこで開いたかを記録し、閉じていない文字列に
  「string opened at L:C is not closed」と言わせます。

**ソースはASTより長く生きなければなりません。** `peg::Ast`のトークンは、パースした
バッファへの`string_view`です。`parse_undesugared`はそのバッファを`std::string&`で
受け取ります（CRLFをLFにその場で正規化するため。単独の`\r`は`CulebraError`です）。
だから呼び出し側は文字列を所有し、木が生きている間ずっと保持しなければなりません。
`ModuleLoader`が各モジュールのソースを`LoadedModule`内の`shared_ptr<std::string>`で
固定するのはこのためで、組み込みtraitsのpreambleも、テキストを関数内の
`static`に持ちます。

入口はすべて`culebra`名前空間にあり、失敗時にnullの`shared_ptr<peg::Ast>`を
返します:

| 関数 | 役割 |
|---|---|
| `parse_undesugared` | 共通の核。パースし、改行を正規化し、`vector<ParseFailure>`（`line`、バイト列での`col`、`message`）を埋める |
| `parse` | `parse_undesugared`の後に`desugar_regex_literals`と`desugar_postfix_modifiers`。失敗を`path:line:col: message`に整形するオーバーロードがある |
| `parse_for_format` | 脱糖なし。ここではbraceのSetが構文エラーになる。フォーマッタ専用 |
| `parse_with_transforms` | `parse`に`apply_transforms`（§6）を足したもの。REPL、デバッガ、テストランナー、stdlibのpreamble、lintが使う |

### `view_*`アクセサ

ASTの形についての知識は、ASTを読むコードの外に置かれています。`parser.h`には
22個の`view_*(const peg::Ast&)`関数（`view_assignment`、`view_for`、`view_if`、
`view_match`、`view_function`、`view_lambda`など）があり、それぞれが子を名前で
持つ小さな構造体を返します。`for`の部品が要るコードは`view_for`を通して読み、
子の位置が動く文法変更は、それを読むコードすべてでなくviewを更新します。

ASTを読むコードは、もうインタプリタとJITではありません。`vm::Compiler`、`resolve.h`、
`lint.h`、`fn_analysis.h`、`state_lowering.h`、`effects_transform.h`、
`generator_rules.h`、フォーマッタ、型推論器です。ノードを足したり形を変えたりする
文法の編集は、これらをそれぞれ読み直すまで終わりません（§9）。

---

## 4. 構文エラー

Culebraは、peglibが整形した文字列を使う代わりに、peglibの構造化された
`peg::ErrorReport`から`format_syntax_error`（`syntax_error.h`）でメッセージを
自前で作ります。peglibは最も遠い位置で失敗したものすべてを報告するので、
`x = [1, 2`では空白から始まる数十個の項目のリストになります。フォーマッタは
それを`expecting ',', ']' or an operator.`にします。

```text
x = [1, 2         ->  expecting ',', ']' or an operator.
foo(1 2)          ->  unexpected '2', expecting ',', ')' or an operator.
y = (1 +          ->  expecting an expression.
fn f(a b) { }     ->  unexpected 'b', expecting ':', '=', ',' or ')'.
```

（`tests/lint_test.sh`が固定している期待値です。）規則:

- リテラルは引用し（`','`）、ルールは名前で呼びます（`<IDENTIFIER>`）。リテラルが
  先で、ルールが後。それぞれレポートが与えた順です。
- **silent**な項目（空白、コメントの開始）は落とします。
- 2つの**ひとまとまりの項目**は、並べずに名前で呼びます。任意の式の後に来うる項目は`an operator`、
  式の先頭に来うる項目は`an expression`になります。名前で呼ばれるのは、
  そのまとまりの項目の**半数を厳密に超える**数がレポートに含まれているときだけです。
  それ以下なら1つずつ並べます。パラメータの後には、演算子が来うるわけでもないのに
  `:`や`=`が来うるからです。名づけ方はrustcに倣っています。
- 文法自身が付けたメッセージ（独自メッセージや上限）は、そのまま返します。

3つの集合は手で保守していません。`tools/gen_grammar_blob.cc`が、途中で終わる
プローブソースをパースして見つけ、`grammar_blob.gen.h`に
`GRAMMAR_EXPECT_SILENT`、`GRAMMAR_EXPECT_AFTER_EXPR`、`GRAMMAR_EXPECT_OPERAND`
として保存します:

- *silent*: 名前がアンダースコアで始まる文法ルールすべてのリテラル（peglibは
  そうしたルールをレポートから外します）。
- *式の後*: `x = (f()`、`x = [f()`、`if f()`、`g(f()`の後に期待されるものの
  **共通部分**。
- *オペランド*: `x = 1 + `の後と`x = `の後に期待されるものの**和集合**。

各プローブは入力の終端と、`\x01`バイトの手前の両方で走らせ、2つの失敗を
合併します。集合が空になるとジェネレータはエラーで終了し、silentな項目は
残り2つから取り除かれます。集合は文法から出てくるので、文法の編集は、
保存データを更新するのと同じ`just gen-blob`で再生成されます。

**既知の限界。** 項目はテキストだけで識別されるので、演算子と同じ綴りの閉じ記号
（ラムダのパラメータを閉じる`|`、`a ? b : c`の`:`）は、演算子に名前を付ける
ときはいつも`an operator`に畳まれます。

標準ライブラリの`PEG.parse`はこのまとまりの定義を渡さないので、利用者の文法に対するメッセージは
すべてをエスケープして並べます。

---

## 5. モジュールのロード

`ModuleLoader::load_program`は、エントリファイルを、依存順の`LoadedModule`の
列にします。どのレーンもこれを使います（`main.cc`、テストランナー、デバッガが
呼びます）。`LoadedModule`は絶対パス、ソース（固定される。§3）、AST、ソース順の
依存、scope診断を持ちます。

`load_recursive`は、1モジュールについて次をします:

1. importスタックで循環を検査する（`ImportError`）。
2. ファイルを読むか、エントリのソースを取る。
3. `culebra::parse`（失敗はメッセージを抱えた`SyntaxError`になる）。
4. 書かれたままのASTに`lint::scope_diagnostics`。
5. `apply_transforms`（§6）。
6. エントリでもライブラリでもないモジュールに`rebase_embedded_calls`。
7. `validate_module`（importとexportはトップレベルだけ、exportの重複禁止）。
8. `extract_imports`。
9. 依存を再帰し、それから自分を積む。この順序がトポロジカル順で、モジュール
   キャッシュは正規化した絶対パスをキーにします。

全モジュールのロードが終わると、`load_program`は各モジュールに
`lint::check_module`を走らせます。プログラムを拒否すべき検査は、どのレーンでも
同じです。

**標準ライブラリに`import`が要らない理由。** これはローダの仕事ではありません。
`splice_stdlib_preamble`（`stdlib/preamble.h`）が、全モジュールのトークンから
名前の出た名前空間を探し、該当するpreambleを`src/preambles/*.cul`から
`parse_with_transforms`でパースして、`<stdlib>`というパスで他のモジュールの前に
置きます。`Regex`を名指ししないプログラムは、Regexのpreambleをパースしません。
JITとAOTのレーンは、ビルド時にパースしてコンパイルしたpreamble
（`BakedPreamble`）を使えます。

**埋め込みディレクトリ。** `Dir.embedded(name)`は、呼んだファイルの隣のディレクトリ
を`import`と同じやり方で指しますが、ランタイムと`culebra build`は名前をエントリ
スクリプトのディレクトリから解決します。そこで`rebase_embedded_calls`
（`embed_paths.h`）が、リテラルの名前をエントリディレクトリからの相対パスに、
計算された名前を`Dir._embedded_in(base, ...)`の呼び出しに書き換えます。AOTの
スキャン（`aot/scan.h`）は、焼き込むアセットを見つけるのに同じ
`embedded_literal_slot`を使います。

---

## 6. コンパイラの前に走る書き換え

2つの機能は、書かれたままではコンパイルされません。プレーンなCulebraのソースに
書き換えられ、もう一度パースされ、他のコードと同じようにコンパイルされます。
だからどのレーンも書き換えられたプログラムを走らせ、それらのための特別な
サポートを持ちません。

`apply_transforms`は、書かれたままパースされたモジュールに、この順で走ります:

1. `check_generators_in`: `defer`ブロックの中の`yield`を拒否する。メッセージは、
   `defer`はスコープを出るときに走り、generatorがそこでcloseされるときも含むので
   中断できない、と説明する。`try`と`catch`の本体はyieldしてよい。handleと
   effectの本体は次の段に任せる。
2. `transform_effects_in`: effectsの書き換え（下記）。
3. `reject_orphan_yield`: 関数やメソッドの本体の外（トップレベル、`|...|`ラムダ、
   `new`、`drop`）の`yield`を拒否する。
4. `reject_sized_spread_mix`、`reject_brace_set`、`reject_or_pattern_binding`、
   `reject_invalid_new`: `parser.h`にある構造的な拒否。

`generator_rules.h`が決めるのは、`yield`をどこに書いてよいかだけです。generator
自体は書き換えられません。コンパイラは書かれたままを受け取り、各エンジンが
`Op::GenStart`／`Op::Yield`でフレームを中断します（vm.md §5.7）。述語
`can_be_generator`と`fn_body_has_yield`はコンパイラと共有されていて、チャンクが
generatorだとコンパイラが決めるのはこれによります。

### effectsの書き換え

`effect fn`、`perform`、`handle ... with`は`EffectsLowerer`が書き換えます。本体を
持つ`effect fn`は、computationオブジェクトを返す普通の関数になります。これは、
`_step(rv)`が次の中断まで走るクラスで、状態番号で分岐します。シグネチャだけの
`effect fn`は、throwするスタブになります。`handle`は、computation、操作名、
アダプタを渡す`__Eff.handle`の呼び出しになり、ドライバ`__Eff.drive`は
`src/preambles/effects.cul`にあるCulebraのコードです。resumeは一度きりです。
ステップの結果には`DONE`か`SUSPEND`のタグが付きます。式の中に入れ子になった
`perform`は、まずA正規化のパス（`anf_program`）で文のレベルに持ち上げられます。
設計とコンパイラが提供するintrinsicは、vm.md §11にあります。

この書き換えを支える仕組みが3つあり、フロントエンドの中でほかに文書がない部分です:

- **`PromotedLocals`**（`state_lowering.h`）。中断がステップをまたいで分断する
  ローカルは、1回の呼び出しのローカルのままではいられません。computation
  オブジェクトのフィールドに（クロージャが捕捉していればボックスに）なり、その
  読み書きがすべて書き換えられます。これは`resolve::resolve_body`の上に作られて
  いるので、どの名前がローカルかの判断は`resolve.h`のものです。束縛が走る前に
  promoteされたローカルを読むことは、コンパイラの`__bound`／`__unbound`
  intrinsicが守ります。
- **fragment ledger**（`fragments.h`。書き換えで作ったソースの断片と、その元の
  行の対応表）。書き換えが合成するソースの断片はすべて、パースの前に
  `parse_registered_source`で登録されるので、書き換えられたコードの
  エラーを、利用者が書いた行で報告できます。ledgerがバッファを持ち、合成された
  テキストはアンカー（`/*@culebra:`）を運びます。`SourceResolver`は、それを
  中間のバッファをたどって利用者のファイルの1バイトまで戻ります。コピーされた
  行は元の行を指す末尾の`#@culebra:`マーカーを持ちます。由来のない合成行は、
  宣言の行にフォールバックします。
- **プロセス全体の既定ledgerと、スコープ付きのledger。** 組み込みtraitsの
  preambleは1回パースされてスレッド間で共有されるので、既定のledgerは
  mutexで守られています。エディタやlintの実行は、`parse_with_transforms`の
  `FragmentLedger&`オーバーロードで、スレッドローカルなledgerを選びます。

書き換えは、表現できないもの（短絡演算のオペランドの下、メソッドチェーンの後ろ、
ループ条件の中の`perform`、制御フローに入れ子になった`defer`、`resume`
パラメータのないhandlerの節など）に`SyntaxError`を、自分が合成したソースが
パースできなかったときに`InternalError`を投げます。lintは
`parse_with_transforms`を`try`の中で呼んで同じ拒否に届くので、規則は1回だけ
書かれます。

---

## 7. 名前とスコープ

`resolve.h`は、名前が何を指すかを決める唯一の場所です。書かれたままのソースを
歩き、各使用について、それが束縛される宣言を記録します。ストレージやセルは
割り当てません。それはコンパイラの仕事です。

```text
resolve_module(root, source, Options)   a whole module
resolve_body(body, params, Options)     one function body (the effects lowering)
```

`Resolution`は、`scopes`、`symbols`（宣言された回数と形つき）、`occurrences`、
`unresolved`な名前、import、UFCSのために記録したメソッド呼び出しを持ちます。
コンパイラもlintも立てる`Options::record_nodes`を使うと、ASTノードからシンボル
への対応も持つので、消費者は名前と位置でなくノードについて尋ねられます。
`Options::globals`は標準ライブラリの名前を並べます（それへの素の書き込みは
宣言とみなさず拒否されます）。`Options::session`は、REPLや`culebra test`の
前の入力が宣言した名前を並べます。

### 解決器が符号化している規則

利用者に見える規則は`language.md` §6にあります。以下は解決器も正しく扱わなければ
ならない規則で、言語リファレンスが書いていないものも含みます:

- **スコープ。** 関数本体、ブロック、ループ（反復ごとに新しい束縛）、`if`／`cond`／
  `?:`の各腕、`match`の腕、`try`と`catch`、`handle`ブロック、構文を囲むinit節。
  テストは腕の一部ではありません。
- **宣言は右辺の後から見える。** 関数リテラルは本体の前に束縛されるので、自分自身を
  呼べます。
- **関数本体は、囲む本体の後に解決される**（ジョブキュー）。だからネストした関数は、
  囲む関数がどこで宣言していても、その宣言するすべての名前を見ます。パラメータの
  既定値は例外で、すぐ解決されます。
- **素の`x = v`**は、見える`x`に書きます。なければ、`x`が標準ライブラリのグローバル
  でない限り、最も内側のスコープに`x`を宣言します。
- **クラスのフィールド初期化子**は、初期化子ごとのサブスコープを持つ1つの関数
  スコープです。**staticな値**はクラス宣言にあるスコープで、その宣言はその式の
  中に閉じます。
- `fn`は宣言できず、`self`は宣言された場所でだけ変数になり、コンパイル時に走る
  デコレータは何も読まず、キーワードラベル`f(x: 1)`は呼び出し先のパラメータに
  解決されます。

### 解決を読むもの

- **`fn_analysis.h`**は、`resolve_module`、次に`lint::check_resolution`、次に自前の
  ウォーク、そして`derive_captures`を走らせます。あるシンボルが別の関数に属する関数で
  読まれた名前は、読み手から宣言した関数までの全関数の自由変数になります。
  コンパイラはこれで、どのローカルが捕捉されてセルを要するかを知ります。ウォークは、
  解決器が知る理由のない事実をまだいくつか記録しています: `self`、フィールド初期化子
  のスロット、`fn`、書き換え済みフレームのマークです。
- **`lint.h`**（§8）。
- **言語サーバ、型推論器、エディタのアウトライン**（`resolve::outline`）。
- **`scope_check.h`**（下記）。

### 一致検査

コンパイラはコードをemitしながら、まだ自分でも名前を束縛しているので、2つは
ずれうります。自動検査がそれをつなぎとめます。`CULEBRA_SCOPE_CHECK=<dir>`を設定
すると、コンパイラは自分が束縛または参照するすべての名前ノードについて解決器に
尋ね、4種類の行を持つレポートを書きます:

- `MISMATCH`: コンパイラと解決器が食い違う（宣言、使用、1つのシンボルに2つの
  セル、束縛のない、あるいはセルに入っていない捕捉変数）。
- `MISSING`: コンパイラが、解決器が一度も見ていないノードを参照した。
- `UNSEEN`: 解決器が参照したノードを、どのコンパイル段階も参照しなかった。
- `SUMMARY`。

`tools/checks/scope_agreement.py`は、`tests/`、`examples/`、`tools/`、`docs/`、
`stdlib/`の下の追跡対象の`.cul`すべてと、構文と宣言形の組み合わせを生成した
格子を、実行せずにコンパイルします。`scope_agreement_allow.txt`にない検出、もう
一致しない許可リストの項目、レポートを書かなかったコンパイルのどれでも失敗
します。justfileの`run_scope_agreement`の項目です。仕組みはvm.md §10.6にあります。

---

## 8. 同じフロントエンド上のlintとfmt

**lintは名前について何も決めません。** 解決を読みます。検査は2つに分かれます:

- *書かれたままの*ASTに対して、`scope_diagnostics`を通して: 予約されたデコレータと
  宣言、不変性（`LetRule`）、未定義の読み取り、シャドーイング。
- *書き換え後の*ASTに対して、`RuleWalker`と、未使用名、トップレベルの文、イディオム、
  到達不能コードを見る独立した構文を見るコード。

scope検査のうちシャドーイングと予約された束縛の2つは、`FnAnalysis`から
`check_resolution`を通して、コンパイルのたびにも走るので、REPLを含むどのレーンも
同じプログラムを拒否します。警告はロード経路では走らず、`culebra lint`が
`collect_module`で全部を走らせます。`resolve_options()`は、コンパイラとlintの
両方の`Options`を作る唯一の場所です。

**fmtは文法とパーサを共有しますが、脱糖した木は共有しません。**
`parse_for_format`を呼び、脱糖とtransformを飛ばすので、出力するのは利用者が書いた
テキストです。ASTからドキュメントを組み立て、知らない構文は元のスライスをそのまま
出すことにフォールバックし、自分の出力を再パースして2つの木を比べることを安全網
にします。

---

## 9. 文法を変えるとき

1. `include/frontend/grammar_def.h`を編集する。
2. ASTを読むコードが名前で探すノードを足すなら、`parser.h`の
   `ast_optimizer_keep_rules()`にルールを足す。ノードの形を変えるなら、`view_*`
   アクセサを更新または追加する。
3. 変更について、§3のASTを読むコードをそれぞれ読む。アクセサは形の変更を1か所にしますが、
   ノードが何を意味するかを知っているのはそれを読むコードだけです。
4. `just sync-grammar`が、`misc/culebra.peg`とエディタ文法・Playgroundの
   キーワードブロックを再生成し、`misc/keyword-map.txt`が対応づけていない
   キーワードを警告する。
5. `just gen-blob`が`grammar_blob.gen.h`（エラー整形用の集合を含む）を再生成する。
   コミットする。
6. `just check-generated`（`just test-dev`の一部なので`just land`にも含まれる）が
   `check-grammar-sync`と`check-blob`を走らせる。CIは両方を独立したステップで
   走らせる。反復中は`just phase <語>`で一部だけ走らせられる。
7. エラーメッセージが変わったなら、凍結した`vm_cases`の出力の再凍結と、release-diff
   の許可リストへの1行が要る（vm.md §10）。
8. effectやgeneratorの書き換えを変えたなら、
   `python3 tools/checks/lowering_diff.py ./build-dev/culebra`を走らせる。

`peglint`を走らせるレシピもCIのステップもありません。

---

## 10. この層が決めたこと

- **消費者ごとでなく、解決器は1つ。** コンパイラ、lint、キャプチャ解析は、かつて
  それぞれスコープを自分で求めていました。突き合わせると、フィールド初期化子や
  staticな値でのクラッシュを含む、本物の食い違いが出ました。今は`resolve.h`が
  規則を持ち、キャプチャはそこから導出し、lintはそれを読み、自動検査が
  コンパイラを解決器と比べるので、まだ自分で名前を束縛している1か所が気づかれずに
  ずれることはありません。
- **エラーメッセージは、パーサライブラリでなくここで作る。** peglibが最も遠い位置で
  失敗したもの全部の和集合を報告するようになったとき、`x = [1, 2`は空白から始まる
  38項目のリストになり、どのテストもそれに気づきませんでした。決定は、ライブラリの
  構造化レポートを残し、文言を`syntax_error.h`に書くことで、このまとまりは文法から導出するので
  古くなりません。
- **書き換えはもう1本の木でなく、ソースを作る。** effectsはプレーンな
  Culebraのソースに書き換えてもう一度パースするので、どのレーンも書き換え済みの
  プログラムを走らせ、effects専用に実装するものを持ちません（`effects_transform.h`
  の冒頭がそう述べています）。その代価が§6の来歴の仕組み、つまりledgerとアンカーで、
  再パースしたテキストを利用者が書いた行に結びつけ直す必要があります。
- **文法の保存データは、テストに見えない最適化である。** 古い保存データは遅い経路に
  フォールバックして正しいままです。だからパーサのテストに頼らず、専用の検査を
  持ちます。
