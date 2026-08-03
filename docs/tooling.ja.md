# Culebra ツール

`culebra`バイナリはツールチェーンそのものです。プログラムを実行する同じ
実行ファイルが、テストランナー・リンタ・フォーマッタ・デバッグアダプタ、
そしてリファレンス文書そのものも兼ねています。この文書はそれらの
開発用サブコマンドのリファレンスです。

| サブコマンド | 役割 | 参照 |
|---|---|---|
| `culebra init` | このディレクトリとエディタをセットアップする | [§4](#手早いセットアップ-culebra-init) |
| `culebra test [paths...]` | テストとdoctestを実行する | [§1](#1-テスト-culebra-test) |
| `culebra lint [paths...]` | 実行せずに静的な問題を報告する | [§2](#2-lint-culebra-lint) |
| `culebra fmt [paths...]` | ソースを正準スタイルに整形する | [§3](#3-フォーマット-culebra-fmt) |
| `culebra dap` | Debug Adapter Protocolをstdioで話す | [§4](#4-デバッグ-culebra-dap) |
| `culebra docs [topic]` | 埋め込まれたリファレンスを読む・検索する | [§5](#5-ドキュメントを読む-culebra-docs) |
| `culebra build <in.cul> -o <out>` | AOTコンパイルして単体実行ファイルを作る | [`deployment.ja.md` §1](deployment.ja.md#1-standalone-バイナリビルドculebra-build) |
| `culebra wrap` | 自前のC++ クラスを組み込んだ拡張バイナリを作る | [`deployment.ja.md` §3](deployment.ja.md#3-c-ライブラリのラッピングculebra-wrap) |

素の`culebra [flags] script.cul`形式 — `--jit`・`-O0`..`-O3`・`--ast`・
`--shell`など — は
[言語仕様 §22](language.ja.md#22-コマンドラインインタフェース) が規定します。
同じツール群の読み物としての導入は
[`guide.ja.md` §15](guide.ja.md#15-ツール-test-lint-fmt-デバッグ) にあります。

目次
----

1. [テスト (`culebra test`)](#1-テスト-culebra-test)
   * [テストの書き方](#テストの書き方)
   * [実行](#実行)
   * [doctest](#doctest)
2. [Lint (`culebra lint`)](#2-lint-culebra-lint)
3. [フォーマット (`culebra fmt`)](#3-フォーマット-culebra-fmt)
4. [デバッグ (`culebra dap`)](#4-デバッグ-culebra-dap)
   * [手早いセットアップ: `culebra init`](#手早いセットアップ-culebra-init)
   * [VSCode](#vscode)
   * [Vim (vimspector)](#vim-vimspector)
   * [Zed](#zed)
5. [ドキュメントを読む (`culebra docs`)](#5-ドキュメントを読む-culebra-docs)

---

## 1. テスト (`culebra test`)

`culebra test [paths...]`はテストファイルを探索して実行します。`test()`・
`@test`・`@parametrize`・マッチャ群・依存注入によるフィクスチャがすべて
使えます。

### テストの書き方

書き方は3通りあります。呼び出し形式と`@test`デコレータは等価なので、
呼び出し側が読みやすい方を選んでください。`@parametrize`はケースごとに
1つのテストを登録します。

```culebra
# doctest: skip
# tests/test_string.cul

# 呼び出し形式
test("interpolation embeds Long", fn() {
  let x = 42
  assert_eq("hi {x}", "hi 42")
})

# デコレータ形式 — 関数名がテスト名になる
@test
fn interpolation_embeds_float() {
  let pi = 3.14
  assert_eq("π = {pi}", "π = 3.14")
}

# パラメタライズ — ケースごとに 1 テスト、名前は `<fn>[i]`
@parametrize([(1, 2, 3), (2, 3, 5), (10, 20, 30)])
fn adds_correctly(a, b, want) {
  assert_eq(a + b, want)
}
```

**`describe`のネストはありません。** ファイルパス（`tests/strings/`）と
テスト名中の`/`（`"Array/push: appends element"`）でグループ化します。

**フィクスチャはDIで。** テストの位置引数は周囲の環境から名前で解決され
ます。envにある任意の関数がフィクスチャになれ、デコレータは不要です。
フィクスチャ自身がフィクスチャを取ることもできます。

```culebra
# doctest: skip
fn db()       { { users: [], next_id: 1 } }
fn user(db)   { db.users.push({ id: 1, name: "alice" }); db.users[0] }

@test
fn user_has_name(user) {
  assert_eq(user.name, "alice")
}
```

1つのテストの中では各フィクスチャは**一度だけ**評価され、直接・推移的な
複数の言及が同じインスタンスを共有します。テストをまたぐと新しく作られます。

**後始末はクラスの`drop`で。** 後始末が要るリソースは`drop`メソッドを
持つクラスに包みます（[`language.ja.md` §17](language.ja.md#17-メモリモデル)）。
テスト終了時にフィクスチャキャッシュが解放されると、ランタイムの参照カウント
による確定処理が走ります。

```culebra
# doctest: skip
class TestDB {
  new()    { self.conn = Database.connect("memory") }
  drop()   { self.conn.close() }
  users()  { self.conn.users }
}

fn db() { TestDB.new() }

@test
fn user_count(db) {
  db.users().create("alice")
  assert_eq(db.users().count(), 1)
  # テスト終了時に db が drop -> conn.close()
}
```

フィクスチャ本体に書いた`defer`はフィクスチャ関数がreturnした時点
（テスト実行より前）で発火してしまうので、後始末にはクラスの`drop`が
適切です。ファイルをまたいで共有する長命な状態（一度だけ読み込むモデル等）は
モジュールのトップレベルに置きます。モジュールシステムが束縛をキャッシュする
ためです。

**マッチャ。** 表明にはマッチャ群を使います。`assert`キーワードや組み込みは
ありません。マッチャは **3バックエンド共通のグローバル**（`inspect`や
`Math`と同じく全環境に束縛される）なので、`culebra script.cul`・
`culebra --jit script.cul`・`culebra build`・`culebra test`のいずれでも
同じように動きます:

```culebra
# doctest: skip
assert_eq(arr.len(), 3)                 # == ; 失敗時に両辺を表示
assert_throws("TypeError", fn() { let _ = 1 + 'b' })
assert_close(0.1 + 0.2, 0.3, 1e-9)      # |a - b| <= tol
```

マッチャの全一覧（`assert_true`/`false`/`ne`/`lt`/`le`/`gt`/`ge`と
`__eq__`/`__lt__`のディスパッチ規則）は
[`stdlib.ja.md` §13](stdlib.ja.md#13-matchers)。

**本番の不変条件。** テストスイート外の`if (!cond) throw {...}`検査は
`if`/`throw`を直接書きます（Go流。
[`language.ja.md` §15](language.ja.md#15-エラー処理) 参照）。本番ビルドで
無効化する`assert`キーワードのようなものはありません。

### 実行

このサブコマンド経由で起動すると、`test` / `@test` / `@parametrize`が常時
使えるマッチャ群と並んで**環境に備わったグローバル**になります（import不要）。
これは`inspect` / `print`がスクリプト実行モードでは備わっている一方
`culebra::environment()`には無いのと同じ考え方です。

```sh
culebra test                       # カレントディレクトリから探索して実行
culebra test tests/strings/        # サブツリーだけ実行
culebra test --filter "Array/push" # 名前の部分一致で絞る
culebra test --reporter json       # NDJSON 出力（1 行 1 JSON）
culebra test --bail                # 最初の失敗で停止
culebra test --bail 3              # 3 件失敗したら停止
culebra test --list                # 探索のみ。テスト名を表示
culebra test --doc docs            # markdown 中の doctest を実行
```

探索規則: ファイルを指すパスはそのまま対象になり、ディレクトリを指すパスは
`test_*.cul`にマッチするファイルを再帰的に探します。終了コードは全て通れば
`0`、1つでも失敗すれば`1`です。

**レポータ。** 既定は人間向けです。`--reporter json`は1行1 JSON
オブジェクト（NDJSON）を出力します。エージェントループやCIに向きます:

```
{"event":"test_pass","name":"adds_correctly","source":"tests/test_math.cul","stdout":""}
{"event":"test_fail","name":"divides_correctly","kind":"AssertionError",
 "message":"assert_eq failed:\n  left:  3\n  right: 4","line":12,"col":3,"stdout":""}
{"event":"run_end","passed":42,"failed":1,"errored_files":0,"bailed":false}
```

テスト内のユーザ`inspect(...)`はNDJSONストリームに混ざらず、イベントの
`stdout`フィールドに取り込まれます。失敗イベントには失敗行を`>`で示した
`snippet`が付きます。

### doctest

`culebra test --doc <path>`は`<path>`配下のmarkdownからすべての
` ```culebra ` ブロックを取り出し、新しいインタプリタで実行して、下記の
マーカーと出力を突き合わせます。`guide.ja.md`・`language.ja.md`・
`stdlib.ja.md` の全ブロックがこの規約に従っています:

- `# => <value>` — 標準出力の期待値（1 行）
- `# => |` に続く `# <line>` 行 — 複数行の期待値
- `# !! <pattern>` — `throw` の期待値（部分一致）
- `# doctest: <directive>`（ブロック先頭行）— モード:
  - `skip` — 説明用。実行しない（*Planned* 機能など）
  - `compile-only` — 構文検査のみ
  - `interp-only` / `jit-only` / `aot-only` — バックエンド指定

ブロック間は独立で `setup` / `teardown` はありません。複数ステップが必要な
例は 1 ブロックに収めてください。ランナーが現在解釈するのは `skip` のみで、
`compile-only` とバックエンド指定は予約済み（該当ブロックは通常どおり実行
されます）。

例を書くときに知っておく価値のある帰結が 2 つあります。式を単に置いても
何も出力されないので、検証される例は `inspect(...)` か `println(...)` を
通す必要があります。そして Culebra では `#` と同様に `//` も行コメントを
開始するため、`// => 値` というマーカーは構文としては通っても**期待値として
認識されず**、そのブロックは無検証で実行されます。

**予定**: `culebra test` 経由で走らないコード向けの明示的な
`import { test } from "std/test"`、`--backend interp|jit|aot`（現在ランナーは
インタプリタを使います）、並列実行。

---

## 2. Lint (`culebra lint`)

`culebra lint [paths...]` はプログラムを**実行せずに**静的な問題を報告し、
CI がゲートにできるよう非ゼロで終了します（0 = 問題なし、1 = 警告のみ、
2 = エラー）。各バックエンドが既に走らせているロード段階の静的解析を再利用
するので、報告されるエラーは実行時に中断する原因とちょうど同じものです。
その上に助言的な警告が乗ります。

```bash
culebra lint app.cul
# app.cul:12:7: warning: unused variable 'tmp'
# app.cul:20:3: error: undefined variable 'reuslt'

culebra lint .          # カレントディレクトリ配下の .culを再帰的に
                        # (`culebra fmt -i .`と同じ)
```

`.cul` を 1 つも含まないパスは clean ではなく exit 2 になります。ディレクトリ
の打ち間違いがゲートを素通りしないためです。

現在報告する内容:

- **エラー** — 確実に失敗する健全な集合: 場所が不正な `break` / `continue` /
  `return`、不正なパラメータ・代入形式、パラメータやクラスメンバの重複、
  シャドーイング、どこにも束縛されていない名前の読み取り（未定義変数の
  部分集合）。これらは元々どの実行も中断させるもので、`lint` は最初の 1 件で
  止まる代わりに全件を一度に見せます。
- **警告** — 実行を止めない助言:
  - **未使用のローカル変数** — 関数内の `let` / `mut` で、一度も読まれない
    もの。
  - **未使用のトップレベル束縛** — モジュールが読みも再 export もしない
    トップレベルの `let` / `mut`。関数・クラス・enum・trait 宣言はモジュールの
    export 面なので対象外です。
  - **未使用の import** — `import` した名前をモジュールが使っていない。
  - **到達不能コード** — 同じブロック内で先に `return` / `throw` / `break` /
    `continue` があるため決して実行されない文。

  先頭のアンダースコア（`_x`、または裸のシンク `_`）は「意図的に未使用」の
  印で、決してフラグされません。**パラメータは対象外です**: Culebra で
  未使用のパラメータは圧倒的に意図的だからです — 多重ディスパッチの節や
  メソッドのシグネチャがアリティを固定しますし、高階のコールバック
  （ルートハンドラの `fn(req)`、`|i| 4.0`）は宣言だけして使わない引数を
  持ちます。検査してもノイズにしかなりません。

`culebra lint --fix <paths...>` は未使用 import 行を機械的に削除します。
無人で自動修正して安全なのはこの警告だけです。死んだ `import` を消しても
振る舞いは変わり得ない一方、未使用の `let`/`mut` は初期化式が副作用を持ち
うるからです。他の警告はすべて報告のみです。編集後は修正済みソースを
再パース・再 lint し、import が消えて新しいエラーも出ていないと確認できた
場合にだけ書き出します — `culebra fmt` と同じ再パースの安全網です。

削除は行単位なので、対象になるのは import 1 つだけがある行に限られます。
`;` で他の文とつながった import は報告のみで手つかずのまま残します:
その行を消すと隣の文まで巻き添えになり、しかも文が 1 つ消えてもファイルは
同じようにパースも lint も通るため、再チェックでは気づけないからです。

```bash
culebra lint --fix app.cul
# app.cul: fixed 1 unused import

culebra lint --fix joined.cul
# joined.cul: --fix skipped 1 unused import sharing a line with other code
# joined.cul:1:8: warning: unused import 'Math'
```

未知のフラグはパスではなくエラー（exit 2）です: `culebra lint --fixx app.cul`
は、修正が無効なまま lint を続けるのではなく打ち間違いを報告します。

予定: エディタ／LSP 統合向けの `--format json` モードと、インラインの
`# lint: ignore` による抑制。

---

## 3. フォーマット (`culebra fmt`)

`culebra fmt [paths...]` はソースを 1 つの正準スタイルに整形します:
演算子まわりの空白の正規化、2 スペースインデント、ブレースブロックの複数行
配置、行幅を超えた引数リスト／コレクションリテラルの折り返し。方針は固定で
設定は不要です（スタイルフラグはありません）。

```bash
culebra fmt app.cul          # 整形結果を標準出力へ
culebra fmt -i app.cul       # ファイルをその場で書き換え
culebra fmt -i .             # カレントディレクトリ配下の .culを全て整形
culebra fmt --check app.cul  # 未整形ならexit 1（CIゲート）
culebra fmt -l src/*.cul     # 変更されるファイルを列挙
cat app.cul | culebra fmt -  # stdin -> stdout（エディタの保存時整形）
```

ディレクトリ引数は `.cul` を再帰的に走査するので、`culebra fmt -i .` で
プロジェクト全体を整形し、`culebra fmt --check .` で CI をゲートできます。
`.cul` を 1 つも含まないパスはエラー（exit 2）なので、パスの打ち間違いは
黙って何も整形せずに成功するのではなく、その場で失敗します。

出力モードは組み合わせられます: `culebra fmt -i -l .` は書き換えが必要な
ファイルを書き換え**かつ**その名前を列挙し（`gofmt -l -w` と同じ）、
`-i --check` は同じことを出力なしで行います。`-l` / `--check` を付けた場合、整形が異なるファイルが
1 つでもあれば終了コードは 1 なので、書き換えた後でも CI のゲートになります。
整形結果が標準出力に出るのは、他の出力モードを何も指定しなかったときだけです。

`-` は stdin を読み、単独でのみ使えます: ファイルパスとの併用（入力 2 つに
stdout 1 つ）や `-i` との併用（書き換える対象が無い）は、どちらかを黙って
優先するのではなく exit 2 になります。パスを指定しない `culebra fmt -i` も
同じ扱いです。未知のフラグもファイル名と解釈せずエラーにします —
`culebra fmt --wirte app.cul` は exit 2 で何も整形しないので、打ち間違いが
成功した実行に見えることはありません。

コメントは保存されます: 前置コメントは導入する文の上に、行末コメントは同じ
行に残り、文の間の空行 1 行も保たれます（連続する空行は 1 行に畳まれます）。
match / cond のアーム、クラス・trait・enum のメンバ、分配パターン、
パラメータリストはすべて正規化され、長い二項式やメソッドチェーンは行幅で
折り返されます。要素の間にコメントを持つオブジェクト/セットリテラルは
ソースのまま出力されます — その内部の空白は書いたとおりに残り、ファイルの
残りは正規化されます。

仕組み: ソースをパースし、構文木から再出力し、それを**再パースして元と
比較**します。整形がプログラムの意味を変える場合や、コメントを落とす・
複製する場合、`fmt` は破壊を避けてファイルに触れず拒否します。整形は冪等で、
2 回走らせた結果は 1 回の結果と同じです。

### エディタ統合

stdin 形式 (`culebra fmt -`) が整形フックです。各統合はバッファ全体を整形し、
終了コードが 0 のときだけ結果を反映するので、パース／安全性エラーの時は
バッファに触れません。

- **VSCode** — 同梱拡張が document formatting provider を登録するので、
  **Format Document** と `editor.formatOnSave` が `.cul` でそのまま動きます
  （導入は `culebra init`、またはソースチェックアウトから
  `misc/vscode/install.sh`）。
- **Zed** — `settings.json` の
  `"languages": { "Culebra": { ... } }` の下に外部フォーマッタを追加:
  `"formatter": { "external": { "command": "culebra", "arguments": ["fmt", "-"] } }`
- **Vim/Neovim** — 同梱の `ftplugin` が `:CulebraFmt` コマンドを提供します
  （カーソル位置を保持、エラー時は無変更）。保存時整形は
  `let g:culebra_fmt_autosave = 1`。`gq` / `'formatprg'` は意図的に使いません
  — パースできない範囲を空出力で置き換えてしまうためです。
- 保存時フックを持つ他のエディタも、同じようにバッファを `culebra fmt -` に
  通せます。

---

## 4. デバッグ (`culebra dap`)

Culebra は [Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol/)
(DAP) サーバを同梱しています。これにより、DAP 対応エディタ — VSCode・Vim
(vimspector)・Zed・Emacs (dap-mode)・Helix など — でブレークポイント・
ステップ実行・変数の確認をビジュアルに行えます。1 つのアダプタで全エディタに対応します。

```
culebra dap        # stdin/stdoutでDAPを話す
```

手で実行することは稀で、通常はエディタが起動します（下記のエディタ別セットアップ参照）。

**前提**

- **`culebra` を実行できること** — `PATH` を通すか、下記の各設定で絶対パスを指定。
  （動作確認: `echo | culebra dap` が DAP 入力待ちでハングすれば OK、`Ctrl+C` で抜ける。
  エラーになる場合はまずパスを直す。）
- **デバッグはインタプリタで動く** — `--jit` を付けない。アダプタはプログラムを
  インタプリタモードで実行する。JIT/AOT は機械語にコンパイルされソースレベルデバッグ
  できない。

**仕組み。** アダプタはインタプリタの statement 単位のフックでブレークポイント・
`debugger` 文・ステップ時に一時停止します。その間 DAP ループがエディタの要求に
応答し、再開します。プログラムの `stdout`/`stderr` はエディタのデバッグコンソール
に `output` イベントとして転送されるので、プロトコルと混ざりません。

対応している機能:

- **行ブレークポイント**・**条件付きブレークポイント**（式が真のときだけ停止、
  例 `i == 3`）・`debugger` 文
- **ステップ**: continue・step over (`next`)・step in・step out・エントリ停止
- **コールスタック**: 名前付きの多フレームスタック（`inner` ← `outer` ←
  `main`）+ 各フレームのソース位置
- **変数**: 選択したフレームで参照中のローカル — コールスタックでフレームを
  選ぶとそのフレームのローカルを参照できる
- **watch / evaluate**: 選択フレームで式を評価（watch パネル・hover・デバッグ
  コンソール）— 例 `x + y`・`arr[0]`・関数呼び出し
- **変数の編集 (set variable)**: 選択フレームの `mut` 変数の値を変更（`let`
  （不変）は拒否）。変更は実行中のプログラムに反映される
- **プログラム出力** をデバッグコンソールに

全エディタ共通の補足:

- デバッグ対象ファイルとその引数は `culebra dap` のコマンドラインではなく `launch`
  要求の `program` フィールドで渡します。
- ブレークポイントは正準（シンボリックリンク解決済み）パスで照合するので、シンボリック
  リンク下のファイルに置いたブレークポイントも有効です。
- ソース中の `debugger` 文はブレークポイントに関係なく停止します（設定不要の一時停止に
  便利）。

### 手早いセットアップ: `culebra init`

プロジェクトディレクトリで `culebra init` を実行すると、このマシンにある
VSCode・Vim・Neovim のうち見つかったものに対して、エディタ統合（シンタックス
ハイライト＋`culebra dap` デバッグアダプタ）と AI コーディングエージェント向け
指示を導入／更新します。ペイロードはバイナリ内に同梱されているのでソース
チェックアウトは不要です。何度でも再実行して構いません — 実行のたびに
このバイナリが持つ内容で上書きするので、アップグレード後の再実行がそのまま
更新手順になります。Zed は今のところソースチェックアウトが必要です（後述）
— `init` は Zed の存在を検出して知らせるだけです。

以下のエディタ別手順は、同じ統合をソースチェックアウトから作る手順です
（拡張自体への contribute、または `init` が届かない環境向け）。

### VSCode

VSCode は `.cul` のハイライトと `culebra` デバッグタイプ登録のための小さな拡張が必要です
（デバッグは登録のみで、ロジックは全て `culebra dap` 側にある）。
公開は不要。リポジトリに雛形とインストーラを `misc/vscode/` に同梱しています。

1. 拡張をインストール — `culebra init`、またはソースチェックアウトから:

   ```sh
   misc/vscode/install.sh
   ```

   どちらも拡張を `.vsix` にパッケージし、`code --install-extension`
   でインストールします＝VS Code が公式にサポートする方法。（`~/.vscode/extensions` に
   フォルダを直接コピーする方法は**非サポート**で、認識されないことが多い。）`culebra` が
   `PATH` 上にあればデバッグアダプタ設定に絶対パスを埋め込みます。`code-insiders`/`cursor`/
   `codium` でも動作し、いずれの CLI も無ければ Extensions ビューから `.vsix` を入れる手順を
   案内します。シンタックスハイライトは `.cul` を開くだけで有効（以降の手順はデバッグ用のみ）。
   文法のキーワード一覧は `just sync-grammar` がパーサから生成（Vim 構文ファイルと同一ソース）
   するため言語からドリフトしません。
2. VSCode を**完全終了**（<kbd>Cmd</kbd>+<kbd>Q</kbd>）して再起動 — 入れたての拡張は
   ウィンドウ再読み込みだけでは拾われないことがあります。
3. プロジェクトに `.vscode/launch.json`:

   ```jsonc
   {
     "version": "0.2.0",
     "configurations": [{
       "type": "culebra",
       "request": "launch",
       "name": "Debug current file",
       "program": "${file}",
       "cwd": "${workspaceFolder}",
       "stopOnEntry": false
     }]
   }
   ```
4. `.cul` を開き、ガター（行番号の左）をクリックでブレークポイント → <kbd>F5</kbd>。

> **拡張自体を作り込む場合**は、変更のたびに `.vsix` を入れ直す代わりに、`misc/vscode/` を
> VSCode で開き `extensionHost` の launch 構成で <kbd>F5</kbd> を押すと、拡張がライブロード
> された別ウィンドウ（*Extension Development Host*）が開き、そこで `.cul` をデバッグできます。
> 単に*使いたいだけ*なら上の `install.sh` の方が簡単です。

### Vim (vimspector)

拡張は不要 — [vimspector](https://github.com/puremourning/vimspector) を導入し、
プロジェクトルートに `.vimspector.json` を置きます（vimspector はプロジェクト
単位の設定なので、culebra 側からは配布できません）。Vim のシンタックスファイルを
インストール済み（`misc/vim/install.sh`）なら、プロジェクトルートで `.cul` ファイルを
開いて `:CulebraVimspectorInit` を実行すれば生成されます — 既存の
`.vimspector.json` は上書きしません。手動で作る場合は次の内容を書きます:

```json
{
  "configurations": {
    "Debug file": {
      "adapter": { "command": ["culebra", "dap"] },
      "configuration": {
        "request": "launch",
        "program": "${file}",
        "stopOnEntry": false
      }
    }
  }
}
```

vimspector は**デフォルトでキーマッピングを一切設定しない**ため、`vimrc` に
次の1行を追加してください。これが無いと `<F5>`/`<F9>` を押しても何も起きず、
設定に失敗したように見えます:

```vim
let g:vimspector_enable_mappings = 'HUMAN'
```

これでブレークポイントは `<F9>`、開始は `<F5>`（`HUMAN` マッピング。
`<F10>`/`<F11>`/`<F12>` で step over/in/out、`<F3>` または `:VimspectorReset` で終了）。
gadget のインストール（`:VimspectorInstall`）は不要 — 上記 `command` を stdio で
直接起動します。Vim は `+python3` ビルドが必要です。

シンタックスハイライトは `culebra init`、またはソースチェックアウトから
`misc/vim/install.sh` を実行してください。

### Zed

`culebra init` は Zed の存在を検出してこの節へ誘導するだけです — 理由は
上の「手早いセットアップ」を参照（拡張は Zed が git チェックアウトから
取得する仕組みで、バイナリ単体のダウンロードにはそれがありません）。

Zed はシンタックスハイライト（tree-sitter 文法）にもデバッグ（デバッグアダプタは
**拡張による登録が必須**＝`debug.json` から任意の DAP コマンドを直接指せない）にも
拡張が必要です。両方を1つの dev extension で提供します:

```sh
misc/zed/install.sh
```

これで拡張（リポジトリ内 tree-sitter 文法 `misc/zed/tree-sitter-culebra` ＋ `culebra`
デバッグアダプタを `culebra dap` に登録する小さな Rust シム）を生成し、このプロジェクトの
`.zed/debug.json` を書き出します。Zed への導入（初回のみ）:

1. コマンドパレット → **`zed: install dev extension`** → スクリプトが表示した
   ディレクトリ（既定 `~/.local/share/culebra-zed-extension`）を選択。Zed は Rust シムを
   `wasm32-wasip2` にビルドするので、新しめの Zed に加え Rust とそのターゲットが必要です:
   `rustup target add wasm32-wasip2`（無いと "can't find crate for core" でビルドが落ち、
   アダプタが登録されません。ハイライトは無くても動きます）。`install.sh` が未導入なら警告します。
2. 文法/アダプタを更新したら `misc/zed/install.sh` を再実行して同ディレクトリを選び直す
   （ピン留めコミットが更新されます）。

その後 `.cul` を開く（ハイライトされる）→ ブレークポイントを置き、デバッグパネルから
**"Debug current Culebra file"** を実行。生成される `.zed/debug.json`:

```jsonc
[
  {
    "label": "Debug current Culebra file",
    "adapter": "culebra",
    "request": "launch",
    "program": "$ZED_FILE",
    "cwd": "$ZED_WORKTREE_ROOT",
    "stopOnEntry": false
  }
]
```

> Zed のデバッガ／拡張 API は VSCode より新しく流動的なので、キーやビルド手順がバージョンで
> 異なることがあります。dev extension がビルドできない／アダプタが起動しない場合は、Zed の
> バージョンと `misc/zed/Cargo.toml` の `zed_extension_api` バージョンの対応を確認してください。

---

## 5. ドキュメントを読む (`culebra docs`)

このリファレンス一式は全てバイナリにコンパイルされて入っています。
`culebra docs` はコードを実行しているのと同じビルドから答えるので、
遅れたチェックアウトも、届かないネットワークもありません。リリース
書庫に `docs/` ディレクトリが無いのは、必要ないからです。

```
culebra docs                     # トピック一覧（サイズの目安つき）
culebra docs stdlib              # 1トピックを表示
culebra docs -g 'Math.wrap'      # 一致したセクションを表示
culebra docs stdlib -g 'wrap'    # 1トピックだけ検索
culebra docs --ja ...            # 日本語版
```

終了ステータスは grep の慣習に従います。`0` = 何か表示した、`1` = 一致
なし、`2` = 使い方の誤り。おかげで実在確認が、出力を読まずに 1 行で
書けます:

```
culebra docs -g '<名前>' >/dev/null || echo "そんなAPIは無い"
```

（ここに実際の名前を書くと、それ自身を検索することになります。この
ページも corpus の一部です。）

### 検索

`-g` は正規表現を取り、パターンに大文字が含まれない限り大小を区別
しません。コンパイルできないパターン — たいていは `get_or_put(` の
ような署名の断片 — は拒否せずリテラルとして検索し、その旨を stderr に
出します。

出力の単位はセクション、つまり見出しとその下の本文です。ヒットは
一致した場所の順に並びます。`stdlib.md` の見出し一致はほぼ必ず署名で
あり、それが読み手の求めていたものだからです:

1. 見出し、2. コードブロック、3. 本文。

一致セクションが 8 個を超えると出力は見出しの索引に降格するので、広い
パターンのコストが 1 画面で収まり、リファレンス全体にはなりません。
見出し一致の上位数件はそれでも全文で表示されます。`--full` はこれを
上書きし、`--at <行>` は 1 セクションを打ち切りなしで表示します（行
番号は各ヒットの上に出るロケータのものです）。

セクションは構造上小さく保たれている（中央値 21 行）ので、ヒットは
通常まるごと表示されます。60 行を超える分は `--at` への案内をつけて
打ち切ります。

### どのトピックを読むか

`culebra docs llm` が凝縮パックです。構文・他言語から転移しない癖・
標準ライブラリの全署名が、プロンプトに収まる 1 ファイルに入っています。
これは検索するものではなく、culebra を書く前に読むものです。残りは
プロンプト窓より大きく、そのために `-g` があります。

`culebra docs agent` はさらに短く、プログラムの外に出ていくことを
前提にした唯一のトピックです。コーディングエージェントが既に読んで
いるファイルに追記する規則で、署名を推測せずここで引くようにさせます。

```
culebra docs agent >> CLAUDE.md                        # Claude Code
culebra docs agent >> .github/copilot-instructions.md  # GitHub Copilot
culebra docs agent >> AGENTS.md                        # Codex, Cursor
```

追記先の一覧はstdoutではなくstderrに出るので、リダイレクトには
markdownだけが入ります。`agent`も`llm`も横断検索には参加しません
（どちらも他のトピックを凝縮したもので、同じAPIが二重に出ます）。
名前を指定すれば検索できます: `culebra docs agent -g …`。

`culebra init`はこの追記を代わりにやってくれます — 上記3ファイルの
うちどれが既に存在するかを検出し（どれも無ければ`AGENTS.md`を新規作成）、
再実行するたびにブロックを最新に保ちます。`culebra docs agent`を直接
使うのは、生のmarkdownが欲しいときか、このリストに無い追記先を使う
ときだけで十分です。
