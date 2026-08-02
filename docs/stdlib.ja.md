# Culebra 標準ライブラリ

本書はCulebraの**組み込みライブラリのAPIリファレンス**です。
ランタイムユーティリティをまとめた名前空間オブジェクト
（`Math`, `IO`, `Sys`, `FS`, `Time`, `Args`, `Random`, `String`）
を対象とします。ここに記載のものは`import`文なしで利用できます。

実例つきの導入とイディオムは [`guide.ja.md` §14](guide.ja.md#14-標準ライブラリ)
を参照してください。

言語レベルの組み込み関数（`to_long`, `to_float`, `to_string`,
`type_of`, `range`, `iota`）は [言語仕様 §19](language.ja.md)
を参照してください。matcher一族 (`assert_true` / `assert_eq` /
`assert_throws`等) は [§13 Matchers](#13-matchers) で扱います。
組み込み型（`String`, `Array`, `Object`）のメソッドは
[言語仕様 §18](language.ja.md) に規定されています。

CLI（`src/main.cc`）はこれに加え、`inspect`・`print`・`println`を
`IO.inspect` / `IO.print` / `IO.println`のエイリアスとしてグローバルに
配置します（[言語仕様 §22](language.ja.md) 参照）。`culebra::environment()`
を直接呼び出す埋め込み用途では、これらのエイリアスは導入されず
名前空間はクリーンなままです。

本書で用いる記法:

* 型注釈は[言語仕様 §14](language.ja.md) に従います。`Any`は任意の
  値を示します。
* 例外条項は`type error at L:C.`等の実行時エラーを示します。
  [言語仕様 §15](language.ja.md) を参照。
* 名前空間は**閉じて**います：名前空間が持たないメンバーを読むと、`nil`を
  返すのではなくアクセス地点で
  `AttributeError: namespace 'IO' has no member 'read_all'`を送出します。
  これにより、タイプミスや廃止APIの使用が、後続の呼び出しでの分かりにくい
  失敗ではなく、即座に捕捉可能なエラーになります。（プレーンなdictは寛容な
  ルールのまま — 未定義キーは`nil`。）dictの組み込みメソッド
  `keys`/`values`/`has`/`get`/`size`は名前空間でも従来通り使えます。唯一の
  例外は`Path`で、これは名前空間ではなくクラスなので、未定義プロパティは
  他のクラスと同様に`nil`を返します。

## 目次

1. [`Math`](#1-math) — 数値ユーティリティ・定数・整数列
2. [`IO`](#2-io) — 出力・標準入力（ファイルI/Oは`FS`）
3. [`FS`](#3-fs) — パス操作・ファイル/ディレクトリ問い合わせ・更新
4. [`File`](#4-file) — ストリーミング読み書き/seekの状態付きハンドル
5. [`Time`](#5-time) — `Instant` / `Duration`クラス、ISO 8601、カレンダー算術、ナノ秒精度
6. [`Random`](#6-random) — シード可能なPRNG（uniform / gauss / shuffle / weighted_choice）
7. [`Sys`](#7-sys) — argv / exit / env / executable。`GC`ヒープ情報の取得も同節
8. [`Tensor`](#8-tensor) — N次元数値テンソル、BLAS対応lazy graph
9. [`JSON`](#9-json) — stringify / parseの相互変換
10. [`Args`](#10-args) — 宣言的なCLI引数パーサ (positional / option / subcommand / `--help`)
11. [`Proc`](#11-proc) — 外部コマンドを同期実行しstdout/stderr/終了コードを取得
12. [`Isolate`](#12-isolate) — クロージャを別スレッド（独立ヒープ）で実行、値は境界でコピー。`Channel` / `Parallel` / `Signal`（Ctrl+Cをチャネルへ振り向け）/ `SharedBuffer`（zero-copy共有する固定レイアウトデータ）/ `Shared`（参照共有するimmutable値）も同節
13. [Matchers](#13-matchers) — `assert_true` / `assert_eq` / `assert_throws` / `assert_close`一族
14. [`Regex`](#14-regex) — 線形時間・grapheme単位の正規表現
15. [`Http`](#15-http) — 同期HTTP/HTTPSクライアント（get/post/put/delete/head/request）、サーバー（ルーティング・静的ファイル・WebSocket）、Server-Sent Events
16. [`Encoding`](#16-encoding) — スキーム別のテキストコーデック（`Encoding.html`、`Encoding.base64`、`Encoding.hex`、`Encoding.url`）
17. [`Compress`](#17-compress) — データ・ファイルのgzip / deflate圧縮/展開
18. [`Hash`](#18-hash) — SHA-256/SHA-1/SHA-512/MD5ダイジェストとHMAC（hex出力）
19. [`CSV`](#19-csv) — RFC 4180流のCSVをparse / stringify
20. [`Env`](#20-env) — dotenv形式の`.env`をparse / load
21. [`UUID`](#21-uuid) — v4（ランダム）/ v7（時刻順）UUID生成
22. [`Term`](#22-term) — TUI向けの端末の色・カーソル制御・サイズ・キー/マウス入力
23. [`Log`](#23-log) — stderrへのレベル付き構造化ログ（text / JSON、child logger）
24. [`TOML`](#24-toml) — TOML設定をparse / stringify
25. [`SQLite`](#25-sqlite) — 組み込みSQLデータベース（query / execute / プリペアド文 / トランザクション）
26. [`Canvas`](#26-canvas) — ゲーム向けイミディエイトモード2Dフレームバッファ（図形 / スプライト / オフスクリーン描画先 / テキスト / キー・マウス / tone / 効果音 / music）
27. [`Scene`](#27-scene) — 手続きジオメトリ向けのretained-mode 3Dレンダラ（opt-in、macOS限定）
28. [`Net`](#28-net) — 生のTCP / UDPソケットと名前解決（`Http`の下位レイヤ）
29. [`Desktop` / `Webview`](#29-desktop--webview) — ネイティブWebViewのデスクトップアプリ: ローカルHTTPサーバ + ウィンドウを1呼び出しで
30. [設計上の注記](#30-設計上の注記)
31. [未収録（将来検討）](#31-未収録将来検討)

**目的別索引**

| やりたいこと | 参照先 |
|---|---|
| 定数（π、e、inf、nan） | [§1 Math 定数](#math-pi) |
| スカラー演算（abs / min / max / log / exp / sqrt / floor / ceil / round） | [§1 Math](#1-math) |
| 三角関数（sin / cos / tan / asin / acos / atan / atan2、ラジアン） | [§1 Math](#1-math) |
| 標準出力 | `IO.inspect`（改行 + クォート付き） / `IO.println`（改行、生） / `IO.print`（生、改行なし） |
| ファイル全体を読む | `FS.read`（失敗時throw） |
| ファイルをストリーム（行 / チャンク / seek） | [§4 File](#4-file) — `File.open` / `File.with` |
| パス操作（join / basename / dirname / stem / extension） | [§3 FS](#3-fs)；流暢な`Path`ラッパ: [§3 `Path`](#path--流暢なラッパ) |
| stat / walk / glob / copy / rename / symlink / chmod / chown | [§3 FS](#3-fs) |
| ディレクトリ列挙・作成・削除 | `FS.list_dir`、`FS.mkdir`、`FS.remove` |
| `Instant` / `Duration`クラス、ISO 8601、カレンダー算術 | [§5 Time](#5-time) |
| 負になりうるインデックスを`0..n`に巻き戻す | [§1 Math](#1-math) — `Math.wrap(i, n)`（`%`は切り捨てなので負のまま） |
| 乱数 | `Random.int`、`.uniform`、`.gauss`、`.shuffle`、`.weighted_choice` |
| CLI引数解析 | [§10 Args](#10-args) |
| プロセス情報 | `Sys.argv`、`Sys.exit`、`Sys.env`、`Sys.set_env`、`Sys.getcwd`、`Sys.chdir`、`Sys.executable`、`Sys.script` |
| 外部コマンド実行 | [§11 Proc](#11-proc) — `Proc.run(["git", "status"])` |
| HTTP/HTTPS APIを呼ぶ | [§15 Http](#15-http) — `Http.get("https://api.example/x")` |
| HTTPを提供する（ルーティング・静的ファイル・WebSocket） | [§15 `Http.server()`](#httpserver---object) — `Http.server().get("/", h).listen(8080)` |
| 生のTCP / UDPを話す、ホスト名を解決する | [§28 Net](#28-net) — `Net.connect(host, port)` / `Net.listen(port)` / `Net.udp()` / `Net.resolve(host)` |
| HTMLエンティティのescape / unescape | [§16 Encoding](#16-encoding) — `Encoding.html.unescape("a &amp; b")` |
| base64 / hex / urlのエンコード・デコード | [§16 Encoding](#16-encoding) — `Encoding.base64.encode(s)` |
| データ・ファイルのgzip / gunzip | [§17 Compress](#17-compress) — `Compress.gzip(s)` / `Compress.gunzip(z)` |
| gzipの封筒なしで圧縮 | [§17 Compress](#17-compress) — `Compress.deflate(s, level: 9)`（展開は`Compress.gunzip`） |
| ハッシュ / チェックサム / HMAC | [§18 Hash](#18-hash) — `Hash.sha256(s)` / `Hash.hmac_sha256(key, s)` |
| CSVのパース / 生成 | [§19 CSV](#19-csv) — `CSV.parse(text)` / `CSV.stringify(rows)` |
| TOMLのパース / 生成 | [§24 TOML](#24-toml) — `TOML.parse(text)` / `TOML.stringify(obj)` |
| `.env`設定ファイルの読込 | [§20 Env](#20-env) — `Env.load(".env")` / `Env.parse(text)` |
| UUIDの生成 | [§21 UUID](#21-uuid) — `UUID.v4()` / `UUID.v7()` |
| レベル付き / 構造化ログ | [§23 Log](#23-log) — `Log.info("msg", {k: v})` / `Log.with({req: id})` |
| 別スレッドで処理を実行（CPU並列） | [§12 Isolate](#12-isolate) — `Isolate.spawn(\|\| fib(40))` |
| 固定レイアウトデータをスレッド/プロセス間で共有（zero copy） | [§12 SharedBuffer](#sharedbuffer--zero-copy-で共有する固定レイアウトデータ) — `SharedBuffer.new(n, Vec2)` / `.file` / `.shared` |
| 可変長のread-onlyデータをスレッド間で共有（コピーなし） | [§12 Shared](#shared--参照共有する-immutable-値) — `Shared.new(value)` |
| Ctrl+C / SIGINTを綺麗に扱う | [§12 Signal](#signal--signalnotify--signalreset) — `Signal.notify(tx)` / `Signal.reset()` |
| デスクトップGUI（ネイティブWebView + ローカルサーバ） | [§29 Desktop](#29-desktop--webview) — `Desktop.run({title, assets, routes})` |
| ヒープ情報・リークチェック | [§7 GC](#gc--ヒープ情報の取得) — `GC.stat()` → `{live_objects, rc_objects, heap_bytes}` |
| 行列・テンソル演算（BLAS対応） | [§8 Tensor](#8-tensor) |
| String / Array / Objectのメソッド | [言語仕様 §18](language.ja.md) |
| 整数列（`range`, `iota`） | [言語仕様 §19](language.ja.md) |
| 変換（`to_long`、`to_float`、`to_string`、`type_of`） | [言語仕様 §19](language.ja.md) |

---

## 1. `Math`

数値ユーティリティ群。整数専用ルーチン（`pow`・`sign`・`clamp`・
`wrap`）は`Long`入力を保ち、浮動小数点ルーチン（`log`ほか）は`Long` /
`Float`のいずれかを受け取ります。`Long`と`Float`の相互作用は
言語仕様 §4 / §7を参照。

このセクションのサブグループ: **定数**（`Math.pi`、`Math.e`、
`Math.inf`、`Math.nan`） — **スカラー演算**（`abs`、`min`、`max`、
`log`、`exp`、`sqrt`、`floor`、`ceil`、`round`、`pow`、`sign`、
`clamp`、`wrap`） — **三角関数**（`sin`、`cos`、`tan`、`asin`、`acos`、
`atan`、`atan2`、ラジアン）。整数列ファクトリ`range` / `iota`は
言語コアグローバルで、[言語仕様 §19](language.ja.md#19-コア組み込み関数) を参照。

### 定数

`Math.pi` / `Math.e` / `Math.inf` / `Math.nan`は`Float`の
プロパティです。`--jit`でもコンパイル時定数として展開されます。

<a id="math-pi"></a>
#### `Math.pi`

`π` ≈ `3.141592653589793`。

#### `Math.e`

ネイピア数 ≈ `2.718281828459045`。

#### `Math.inf`

正の無限大（`Math.inf > 1e308 == true`）。負の場合は`-Math.inf`。

#### `Math.nan`

quiet NaN。`Math.nan == Math.nan`はIEEE-754通り`false`。

```culebra
inspect(Math.pi)              # => 3.141592653589793
inspect(Math.e)               # => 2.718281828459045
inspect(Math.inf > 1e308)     # => true
inspect(Math.nan == Math.nan) # => false
```

### スカラー演算

### `Math.abs(x: Long|Float) -> Long|Float`

絶対値。`Long`入力なら`Long`、`Float`入力なら`Float`を返します。

```culebra
inspect(Math.abs(-7))     # => 7
inspect(Math.abs(-7.5))   # => 7.5
```

### `Math.min(a, b, ...) -> Long|Float`、`Math.max(a, b, ...) -> Long|Float`

2つ以上の数値引数から最小 / 最大を取ります。全て`Long`なら`Long`、
1つでも`Float`が含まれれば結果は`Float`。引数1個以下、または
数値以外が混じれば`type error`。

```culebra
inspect(Math.min(3, 1, 4, 1, 5))   # => 1
inspect(Math.max(1.5, 2, 0.5))     # => 2.0
```

### `Math.log(x: Long|Float) -> Float`

自然対数。`Math.log(0)`は`-inf`、負の値は`nan`を返します。
整数値を返す場合でも戻り値は常に`Float`。

### `Math.exp(x: Long|Float) -> Float`

`e`の`x`乗。

### `Math.sqrt(x: Long|Float) -> Float`

主平方根。`Math.sqrt(-1.0)`は`nan`。

### `Math.sin(x) -> Float`、`Math.cos(x) -> Float`、`Math.tan(x) -> Float`

三角関数。`x`は**ラジアン**（`Long`または`Float`）。

```culebra
inspect(Math.sin(Math.pi / 2))   # => 1.0
inspect(Math.cos(0))             # => 1.0
```

### `Math.asin(x) -> Float`、`Math.acos(x) -> Float`、`Math.atan(x) -> Float`、`Math.atan2(y, x) -> Float`

逆三角関数。戻り値はラジアン。`asin` / `acos`は`x`が`[-1, 1]`
範囲（外は`nan`）。`Math.atan2(y, x)`は`y / x`の象限を考慮した
逆正接。

```culebra
inspect(Math.atan2(1.0, 1.0))    # => 0.7853981633974483
# (= pi/4)
```

### `Math.floor(x: Long|Float) -> Long`、`Math.ceil(...) -> Long`、`Math.round(...) -> Long`

整数への丸め。`Long`入力はそのまま返します。`Math.floor`は
`-∞`方向、`Math.ceil`は`+∞`方向、`Math.round`は
**偶数丸め（bankers' rounding）**。

```culebra
inspect(Math.floor(-1.5))   # => -2
inspect(Math.ceil(-1.5))    # => -1
# ちょうど半分は偶数側へ丸めるので 2.5 も 3.5 も偶数になる:
inspect(Math.round(2.5))    # => 2
inspect(Math.round(3.5))    # => 4
```

### `Math.pow(base: Long, exp: Long) -> Long`

整数累乗。繰り返し二乗法で`base ** exp`を計算します。
`Math.pow(x, 0)`は`x`に関わらず`1`（`0`を含む）。

**例外**: `exp < 0`のとき`type error at L:C.`。

後方互換のため残してあります。**基本は`**`演算子を使ってください**
（`Float`・負指数も扱えます。言語仕様 §7）。

```culebra
inspect(Math.pow(2, 10))    # => 1024
inspect(Math.pow(7, 0))     # => 1
inspect(Math.pow(-3, 3))    # => -27
```

### `Math.sign(x: Long) -> Long`

負数で`-1`、ゼロで`0`、正数で`1`を返します。

```culebra
inspect(Math.sign(-5))      # => -1
inspect(Math.sign(0))       # => 0
inspect(Math.sign(42))      # => 1
```

### `Math.clamp(x: Long, lo: Long, hi: Long) -> Long`

`x`を閉区間`[lo, hi]`に収めます。`lo > hi`の場合はエラーに
ならず`hi`を返します。

```culebra
inspect(Math.clamp(5, 0, 10))   # => 5
inspect(Math.clamp(-5, 0, 10))  # => 0
inspect(Math.clamp(15, 0, 10))  # => 10
```

### `Math.wrap(x: Long, n: Long) -> Long`

`x`を幅`n`に巻き戻します。`%`では得られない **floor剰余** です
（`%`は切り捨てなので結果は`x`の符号を持つ。[言語仕様 §7](language.ja.md#算術)）。
`Math.wrap`の結果は`n`の符号を持つので、`n`が正なら必ず`[0, n)`
に入ります。巡回インデックスが欲しいのはこちらで、インデックス0の
1つ手前は負の添字ではなく末尾の要素になります。

```culebra
inspect(Math.wrap(3, 320))     # => 3
inspect(Math.wrap(-3, 320))    # => 317
inspect(-3 % 320)              # => -3
inspect(Math.wrap(320, 320))   # => 0
```

`x`が非負の範囲では両者は一致するので、`Math.wrap`が効くのは`x`が
負になりうるとき — スクロール量、折り返すタイル座標、逆向きに進めた
角度 — だけです。

```culebra
let frames = ['a', 'b', 'c']
let prev = fn (i) { frames[Math.wrap(i - 1, frames.size())] }
inspect(prev(0))               # => 'c'
```

`n`が負なら全体が反転し、結果は`(n, 0]`に入ります。`n`が`0`の
場合は`x % 0`と同じく`divide by 0 error`を送出します。

---

## 2. `IO`

出力と標準入力。ファイルの読み書きは`FS`（`FS.read` / `FS.write` /
`FS.exists`）にあります。

### `IO.inspect(x: Any) -> Nil`

`x`を改行付きで標準出力に書き出します。参照型は
`Array.str_array()` / `Object.str_object()`と同じ書式で整形され、
文字列は**シングルクォートで囲んで**出力されます。

```culebra
IO.inspect('hi')       # → 'hi'
IO.inspect(42)         # → 42
IO.inspect([1, 'a'])   # → [1, 'a']
```

### `IO.print(x: Any) -> Nil`

`x`を**末尾改行なし**で標準出力へ書き出します。書式は`to_string`
と同じで、文字列は**引用符なし**で出力されます。複数回の書き込みで
1行を組み立てたい場合に便利です。

```culebra
IO.print('Hello, ')
IO.print('world!')
IO.println('')      # → Hello, world!
```

### `IO.println(x: Any) -> Nil`

`x`を改行付きで標準出力に書き出します。書式は`to_string`と同じで、
文字列は**引用符なし**で出力されます — `inspect`の生表示版であり、
`print`の改行付き版です。

```culebra
IO.println('hi')       # → hi
IO.println(42)         # → 42
```

### `IO.input() -> String`

標準入力から1行読み取ります。末尾の改行は除かれます。EOFのとき
`''`（空文字列）を返します。

```culebra
# doctest: skip
println('name?')
name = IO.input()
println("Hello, {name}")
```

### `IO.stdin() -> reader`

標準入力に対する読み取り専用ハンドルを返します。`File`ハンドルと同じreader
インターフェース（だからソース非依存のコードが両方で動く）:

| メソッド | 結果 |
| --- | --- |
| `.read()` | 標準入力をEOFまで全て（`FS.read("/dev/stdin")`の移植可能な代替。POSIX専用ではない）。即EOFなら空文字列 |
| `.read(n: Long)` | 最大`n`バイト（EOF時のみ少なく返る） |
| `.lines()` | 入力行の遅延iterator。末尾改行を除去し、EOFで停止 |

`.lines()`は定数メモリでストリーミングする = 巨大・無限入力（`tail -f \| script`）に対する
Unixフィルタのイディオム。読み取りはブロッキングかつ割り込み可能（Ctrl+C 1回で待機解除）。
標準入力は単一消費者で、各メソッドは内部バッファを共有するので`.read(n)`の後の`.lines()`
はバイト読みの続きから続行します。

```culebra
# doctest: skip
# フィルタ: "error" を含む行を大文字化
for line in IO.stdin().lines() {
    if line.contains("error") { IO.println(line.upper()) }
}

let src = if IO.stdin_is_terminal() { read_clipboard() } else { IO.stdin().read() }
```

### `IO.einspect(x: Any) -> Nil` / `IO.eprint(x: Any) -> Nil` / `IO.eprintln(x: Any) -> Nil`

標準エラーへ書き出します（`inspect` / `print` / `println`の双子）。
`einspect`は文字列をクォートし改行を付けます（`inspect`同様）、
`eprint`は末尾改行なしの生の表示形（`print`同様）、`eprintln`は
改行付きの生の表示形（`println`同様）。stdoutに混ぜたくない診断
出力に使います。

```culebra
# doctest: skip
IO.einspect("warning: retrying")     # → stderr
if !ok { IO.eprint("error: {msg}\n") }
IO.eprintln("done")
```

### `IO.stdin_is_terminal() -> Bool` / `IO.stdout_is_terminal() -> Bool` / `IO.stderr_is_terminal() -> Bool`

指定した標準ストリームが端末に接続されているか（POSIX `isatty`）を返し
ます。対話性に応じた分岐に使えます。stdinならプロンプト表示かパイプ
読み取りか、stdout / stderrなら色付けかプレーン出力か。スト
リームがファイルやパイプにリダイレクトされている場合は`false`を返し
ます。

```culebra
# doctest: skip
let src = if IO.stdin_is_terminal() { read_clipboard() } else { FS.read("/dev/stdin") }
if IO.stdout_is_terminal() { println(colorize(msg)) } else { println(msg) }
```

`IO`は標準ストリームとコンソールの名前空間です。ファイルの読み書きは
`FS`（`FS.read` / `FS.write` / `FS.exists`）にあります。

---

## 3. `FS`

ファイルシステムのパス操作とディレクトリ操作。実装は
`std::filesystem`。変更系の呼び出しは失敗時に構造化された
`IOError`（`{kind: "IOError", message, line, col}`）をthrowし、
埋め込み側でソース位置に紐づけてエラー処理できます。

### パス操作

#### `FS.join(parts...: String) -> String`

プラットフォームの区切り文字でパス要素を結合します。引数0個は
`""`を返します。`std::filesystem::path::operator/=`と同じく、
途中要素の末尾区切り文字は尊重されます。

```culebra
inspect(FS.join('a', 'b', 'c.txt'))      # => 'a/b/c.txt'
inspect(FS.join('/usr', 'local', 'bin')) # => '/usr/local/bin'
inspect(FS.join())                       # => ''
```

#### `FS.basename(path: String) -> String`

最終要素（ファイル名＋拡張子）。末尾区切り文字のみのパスは`""`。

```culebra
inspect(FS.basename('a/b/c.txt'))  # => 'c.txt'
inspect(FS.basename('/'))          # => ''
```

#### `FS.dirname(path: String) -> String`

親パス。親が無い場合は`""`（`'c.txt' -> ''`）。

#### `FS.extension(path: String) -> String`

拡張子（先頭ドット込み）。無ければ`""`。ドットファイル
（`.hidden`）は拡張子なしとして扱います — `std::filesystem`仕様
通り。

```culebra
inspect(FS.extension('a/b/c.txt'))  # => '.txt'
inspect(FS.extension('.hidden'))    # => ''
```

#### `FS.stem(path: String) -> String`

拡張子を除いたbasename。

```culebra
inspect(FS.stem('a/b/c.txt'))  # => 'c'
```

### ファイル全体の読み書き

#### `FS.read(path: String) -> String`

`path`のファイル全体を`String`として読み込みます（open + read +
closeを1呼び出しで）。常にバイナリ: 戻り値は任意の内容を往復できる
byte string。逐次／ストリーミング読みは`File`ハンドルを使います。
ファイルが存在しない・読込不可・ディレクトリの場合`IOError`をthrow。

```culebra
# doctest: skip
contents = FS.read('data.txt')
```

#### `FS.write(path: String, content: String) -> Nil`

`content`を`path`に書き込みます（作成または上書き）。バイナリ、
改行変換なし。親ディレクトリが無い・書込不可の場合`IOError`をthrow。

```culebra
# doctest: skip
FS.write('out.txt', 'hello\n')
```

### 問い合わせ

#### `FS.exists(path: String) -> Bool`

`path`に何かが存在するか。ファイル／ディレクトリ／シンボリック
リンクの区別なし。空文字列や不正なパスは`false`。

#### `FS.is_file(path: String) -> Bool`

`path`が通常ファイルならtrue。シンボリックリンクはfollow。

#### `FS.is_dir(path: String) -> Bool`

`path`がディレクトリならtrue。シンボリックリンクはfollow。

#### `FS.size(path: String) -> Long`

ファイルサイズ（バイト）。`path`が存在しない／通常ファイルで
ない場合は`IOError`をthrow。

### ディレクトリ変更系

#### `FS.list_dir(path: String) -> Array<String>`

`path`の直下エントリをファイル名（プレフィックスなし、`.` /
`..`除く）の配列で返します。順序はファイルシステム任せなので
必要なら明示的にソートしてください。`path`がディレクトリでなけ
れば`IOError`をthrow。

```culebra
# doctest: skip
let names = FS.list_dir('/tmp/build')
assert_true(names.contains('out.o'))
```

#### `FS.mkdir(path: String) -> Nil`

ディレクトリを作成。途中の親ディレクトリも含めて作成
（`mkdir -p`セマンティクス）。既存ならno-op。パスがファイル
として存在する／作成に失敗した場合は`IOError`をthrow。

#### `FS.remove(path: String, recursive: Bool = false) -> Nil`

ファイルまたは空ディレクトリを削除（既定）。非空ディレクトリは
`IOError`。`recursive: true`でディレクトリツリーを削除（`rm -rf`）。
対象が存在しない／削除できない場合は`IOError`。

```culebra
# doctest: skip
FS.remove('/tmp/build/out.o')
FS.remove('/tmp/build', recursive: true)
```

#### `FS.rename(src: String, dst: String) -> Nil`

`src`を`dst`にリネーム／移動（同一ファイルシステム内はatomic）。
失敗時`IOError`。

#### `FS.copy(src: String, dst: String, recursive: Bool = false) -> Nil`

ファイルをコピー（`dst`既存なら上書き）。`recursive: true`で
ディレクトリツリーをコピー。失敗時`IOError`。

#### `FS.chmod(path: String, mode: Long) -> Nil`

`path`の権限ビットを`mode`（整数、通常は8進リテラル`0o755`/`0o644`）に
設定する。`mode`は下位12ビット（所有者/グループ/その他の`rwx` +
setuid/setgid/sticky）にマスクされ、既存ビットを置き換える。現在のビットは
`FS.stat(path).mode`で読める。存在しない、または権限変更に失敗すると
`IOError`。

```culebra
# doctest: skip
FS.chmod('deploy.sh', 0o755)       # 実行可能にする
inspect(FS.stat('deploy.sh').mode)    # 493  (0o755)
```

#### `FS.chown(path: String, owner = nil, group = nil) -> Nil`

`path`の所有者・グループを変更する。`owner`/`group`はそれぞれ名前（`String`）・
数値id（`Long`）・`nil`（その項目は変更しない）を受ける。現在のidは
`FS.stat(path).uid` / `.gid`で読める。所有者の変更は通常rootが必要、グループは
所属グループへなら一般ユーザでも可。存在しないパス・不明な名前・権限失敗で
`IOError`、String/Long/Nil以外の引数は`TypeError`。

```culebra
# doctest: skip
FS.chown('app.log', group: 'staff')      # グループだけ名前で設定、所有者は維持
FS.chown('data', 'deploy', 'deploy')     # 両方を名前で（root）
```

### stat / メタデータ

#### `FS.stat(path: String) -> Object`

`{size, is_dir, is_file, is_symlink, mtime, mode, uid, gid}`を返す。`size`は
バイト（非通常ファイルは0）、`mtime`はUnix epoch秒、`mode`は権限ビットの整数
（8進と比較: `st.mode == 0o644`）、`uid`/`gid`は所有者・グループid、
`is_symlink`は
リンク自体を、他フィールドはリンク先を見ます。存在しなければ
`IOError`。

```culebra
# doctest: skip
let st = FS.stat('config.toml')
inspect(st.size)
```

### 再帰探索

#### `FS.walk(path: String) -> Array<String>`

`path`配下の全パスを再帰・深さ優先で。各要素はフルパス。
ディレクトリでなければ`IOError`。

#### `FS.glob(pattern: String) -> Array<String>`

glob `pattern`にマッチするパス（ソート済み）。セグメント単位で
`*` / `?` / `[...]`、`**`で再帰下降。シェル流globで`Regex`とは
別物です。

```culebra
# doctest: skip
let sources = FS.glob('src/**/*.cul')
```

### パス解決

#### `FS.abspath(path: String) -> String`

`path`の絶対・正規化形（カレントディレクトリ基準）。シンボリック
リンクは解決しません。

#### `FS.realpath(path: String) -> String`

シンボリックリンクを解決した正準パス（`weakly_canonical`、末尾の
存在しない要素は保持）。

#### `FS.normpath(path: String) -> String`

ファイルシステムに触れず字句的に正規化（`.` / `..` / 重複区切りを
畳む）。

#### `FS.is_abs(path: String) -> Bool`

`path`が絶対パスか。

### シンボリックリンク

#### `FS.symlink(target: String, link: String) -> Nil`

`target`を指すシンボリックリンクを`link`に作成。

#### `FS.readlink(path: String) -> String`

シンボリックリンクの参照先を読む。`path`がリンクでなければ
`IOError`。

#### `FS.is_symlink(path: String) -> Bool`

`path`がシンボリックリンク自体か（参照先でなく）。

### `Path` — 流暢なラッパ

`Path`は`FS.*`ヘルパの上の薄い**不変**ラッパです。`FS`が原始層
（生の`String`パスを受け、`String`／値を返す）で、`Path`はパスを持ち
回る糖衣層 — パス文字列を手で引き回さずに済みます。各操作は新しい
`Path`（成分は`String`）を返し、`Path`は決して破壊的に変更されません。

短いスクリプトでは`FS.*`を、パスを段階的に組み立てたり複数の操作へ
渡したりする所では`Path`を。`/`演算子とプロパティ連鎖の方が読みやすく
なります:

```culebra
# doctest: skip
let root = Path.new(FS.dirname(Sys.script)).resolve()
for src in root.glob("*/content.src.js") {
  let dst = src.parent / "content.js"         # vs FS.join(FS.dirname(src), …)
  dst.write(transform(src.read()))
  IO.print("{src.parent.name}/content.js\n")  # vs FS.basename(FS.dirname(src))
}
```

`Path.new(s)`で構築します（`s`は`String`または別の`Path`）。パスを
取るメソッド（`join`, `/`, `rename`, `==`）はいずれも`String`か`Path`
を受けます。文字列補間（`"{p}"`）と`to_string(p)`は生のパス文字列を
返します。

| メンバー | 返り値 | 委譲先 |
|---|---|---|
| `p / other` / `p.join(other)` | `Path` | `FS.join` |
| `p.name` | `String`（末尾成分） | `FS.basename` |
| `p.stem` | `String`（拡張子なしの名前） | `FS.stem` |
| `p.suffix` | `String`（ドット込み拡張子） | `FS.extension` |
| `p.parent` | `Path` | `FS.dirname` |
| `p.resolve()` | `Path`（絶対） | `FS.abspath` |
| `p.exists()` / `p.is_file()` / `p.is_dir()` | `Bool` | `FS.*` |
| `p.read()` / `p.write(s)` | `String` / `Nil` | `FS.read` / `FS.write` |
| `p.mkdir()` | `Nil`（親も作成） | `FS.mkdir` |
| `p.remove(recursive=false)` | `Nil` | `FS.remove` |
| `p.rename(dst)` | `Path`（移動先） | `FS.rename` |
| `p.list()` | `Array<Path>`（ディレクトリ項目） | `FS.list_dir` |
| `p.glob(pattern)` | `Array<Path>` | `FS.glob` |
| `p.walk()` | `Array<Path>`（再帰） | `FS.walk` |
| `p.str()` | `String`（エスケープハッチ） | — |

`name` / `stem` / `suffix` / `parent`は **getter**（純粋な文字列導出）で、
括弧なしで読みます — `p.parent().name()`ではなく`p.parent.name`（括弧付き
の呼び出しも動きます）。下段のファイルシステム操作はI/Oを行いthrowし
うるためメソッドのままです。

2つの`Path`は内部パス文字列で比較します: `==`（`String`相手も可）と
`<` / `<=` / `>` / `>=`。よって`Path`配列はソート可能（`paths.sorted()`）で、
`min` / `max` / `Set`でも使えます。非パスとの順序比較は`TypeError`（意味のある
答えがないため）、一方`==`の非パス相手は単に`false`です。

`FS.*`ヘルパと`File.open` / `File.with`は、パスを取る所ならどこでも
`Path`を受けます（パス引数の型は`String | Path`）。よって`Path`は
`.str()`なしでそのまま流し込めます:

```culebra
# doctest: skip
let cfg = Path.new("/etc") / "app.conf"
let text = FS.read(cfg)                 # FS.read(String | Path)
for line in File.open(cfg).lines() { }  # File.open(String | Path)
```

こうしてopt-inするのは*パスを取る*標準ライブラリ関数だけです。それ
以外では`Path`と`String`は**別の型**のまま — ふつうの`fn(x: String)`
は`Path`を受け付けないので型境界は意味を保ちます。`String`専用APIに
生文字列を渡すには`p.str()`（または`"{p}"`）を使ってください。

---

## 4. `File`

`File`はストリーミングI/O用の**状態を持つハンドル**で、`FS`の
一発の全読み書き（`FS.read` / `FS.write`）と対になります。`File.open`
またはスコープ付きの`File.with`で開きます。I/Oは常にバイナリ
（テキストモードの改行変換なし）で、`String`はbyte stringなので
任意の内容が往復します。

ハンドル（以下`f`）は4つのメソッド群を実装します — **Reader**（`read` /
`lines` / `chunks`）、**Writer**（`write` / `flush`）、**Seekable**
（`seek` / `tell`）、**Closeable**（`close`）。どれが有効かはopen
モードに依ります。

### 開く

#### `File.open(path: String, mode: String = "r") -> File`

`path`を開く。`mode`は`"r"`（読み）/ `"w"`（切り詰め+書き）/
`"a"`（追記）。それ以外は`ValueError`、開けなければ`IOError`。

#### `File.with(path: String, mode: String = "r", fn: Function) -> Any`

`path`を開き`fn(handle)`を呼び、あらゆる脱出経路（正常・`return`・
例外）でハンドルを閉じます。`fn`の戻り値を返します。`open` +
`defer { close }`のnative版で、ハンドルの寿命が1ブロックに収まる
時に最も明快です。

```culebra
# doctest: skip
let head = File.with('big.log', 'r', fn (f) { f.read(256) })
```

### 資源安全 — 閉じる3つの方法

| パターン | 使う場面 |
|---|---|
| `File.with(p, m, fn (f) { … })` | ハンドルの寿命が1ブロックに収まる |
| `let f = File.open(p, m); defer { f.close() }` | 寿命が広い／他のロジックと混ざる |
| `for line in File.open(p).lines() { … }` | ストリーミング。iteratorがループ脱出（`break`含む）で閉じる |

明示的に閉じられなかったハンドルはGCバックストップが閉じますが、
それに頼らず上記3つのどれかを使ってください。

### Reader メソッド

#### `f.read() -> String` / `f.read(n: Long) -> String`

現在位置からのストリーミング読み: `read()`は残り全部、`read(n)`
は最大`n`バイト（EOFでは少なくなる）。ハンドル不要の一発全読みは
`FS.read(path)`を使います。

#### `f.lines() -> Iterator<String>`

行を反復、各行は末尾改行を剥がします（`\n` / `\r\n` / `\r`全対応）。
iteratorがハンドルを所有し、ループ終了・breakで閉じます。

```culebra
# doctest: skip
for line in File.open('access.log').lines() {
  if line.contains('ERROR') { inspect(line) }
}
```

#### `f.chunks(n: Long) -> Iterator<String>`

最大`n`バイトの固定長チャンクを反復（最後は短いことあり）。
`lines()`と同じclose-on-exit契約。

### Writer メソッド

#### `f.write(data: String) -> Nil`

現在位置に`data`を書き込み（生バイト、改行変換なし）。読み取り
専用ハンドルでは`IOError`。

#### `f.flush() -> Nil`

バッファした書き込みをOSにフラッシュ。

### Seekable メソッド

#### `f.seek(offset: Long, whence: String = "set") -> Nil`

カーソル移動。`whence`は`"set"`（先頭から）/ `"cur"`（相対）/
`"end"`（末尾から；負の`offset`を使う）。

#### `f.tell() -> Long`

現在のバイトオフセット。

### Closeable

#### `f.close() -> Nil`

ハンドルを閉じ、書き込みをフラッシュ。冪等 — 二重closeはno-op。
閉じたハンドルへの操作は`IOError`。

---

## 5. `Time`

Wall-clock + monotonic時刻、ISO 8601入出力、カレンダー算術。
モジュールが提供する2つのクラス — `Instant`（時点）と
`Duration`（時間幅）— 内部表現はUnix epoch起点の`i64`
ナノ秒（範囲 ±292年、完全ナノ秒精度）。

タイムゾーンは **UTC + localのみ**（`Asia/Tokyo`等の名前付き
ゾーンは将来対応）。各methodはkw-only `utc:`フラグを取り、
`iso`は`utc: true`がデフォルト（Z付きISO 8601がinteropの
wire formのため）、それ以外は`utc: false`（local）デフォルト。

### 取得

#### `Time.now() -> Instant`

現在のwall-clock時刻。NTPや手動時計調整の影響を受けるので、
経過計測には`Time.monotonic`を使うこと。

#### `Time.monotonic() -> Float`

最初の呼出（プロセス起動）からの経過秒数（sub-秒精度）。厳密に
非減少、壁時計変更の影響を受けない。ベンチやtimeoutの基本ツー
ル。

```culebra
# doctest: skip
let t0 = Time.monotonic()
do_work()
inspect("elapsed: {Time.monotonic() - t0} s")
```

#### `Time.sleep(secs: Float) -> Nil`

現在スレッドを`secs`秒以上ブロック。負値や0はno-op。

### `Instant` コンストラクタ

#### `Time.from_iso(s: String) -> Instant`

ISO 8601タイムスタンプをparse。受け付けるvariant:

- `2026-05-20T15:30:00Z`
- `2026-05-20T15:30:00.123Z`
- `2026-05-20T15:30:00.000123456Z`（ns精度の入力）
- `2026-05-20T15:30:00+09:00`
- `2026-05-20T15:30:00-0900`
- `2026-05-20`（日付のみ — UTC 0:00として扱う）
- `2026-05-20T15:30`（秒省略）

不正な入力は`ValueError`をthrow。

#### `Time.from_unix(secs: Long|Float) -> Instant`

Unix epoch秒から構築（Floatならsub-秒精度）。

#### `Time.from_parts(p: Object, utc: false) -> Instant`

parts dictからtimestampを組み立て — `Instant.parts`の逆操作。
認識キー: `year`、`month`、`day`、`hour`、`minute`、`second`、
`nanosecond`（デフォルト: `month=1`、`day=1`、その他0）。それ
以外のキーは無視。

#### `Time.parse(s: String, fmt: String) -> Instant`

非ISO入力向けの厳格strftime parse。formatはPOSIX `strptime`
準拠。`s`が`fmt`に一致しなければ`ValueError`をthrow。結果は
local timeとして解釈。

```culebra
Time.parse("2026/05/20 15:30:00", "%Y/%m/%d %H:%M:%S")
```

### `Instant` method

#### `t.iso(utc: true) -> String`

ISO 8601形式、完全ナノ秒精度（小数部が0の場合は省略）。デフォ
ルトUTC（`...Z`）、`utc: false`でlocal time + `±HH:MM` offset。

#### `t.format(fmt: String, utc: false) -> String`

strftime formatで整形。デフォルトはlocal time。

```culebra
# doctest: skip
t.format("%Y-%m-%d %H:%M:%S")             # local
t.format("%Y%m%d", utc: true)             # 20260520
```

#### `t.parts(utc: false) -> Object`

`{year, month, day, hour, minute, second, nanosecond, weekday,
dayofyear}`に分解。`weekday`はISO 8601起点（`0=Mon`、`6=Sun`）、
`dayofyear`は1-based（`1..366`）。

```culebra
let p = Time.now().parts()
if p.hour >= 9 && p.hour < 17 { inspect("business hours") }
```

#### `t.weekday(utc: false) -> Long`

weekday単体（0=Mon..6=Sun）。`parts()`のObject allocationを
避けたい場合用。

#### `t.add(years=0, months=0, days=0, hours=0, minutes=0, seconds=0, utc: false) -> Instant`

カレンダー算術。`years` / `months`は **月末clamp** セマンティ
クス: `2026-01-31 + 1ヶ月 → 2026-02-28`、
`2024-01-31 + 1ヶ月 → 2024-02-29`（うるう年）。日以下の場は単純
加算。

```culebra
let next_month   = Time.now().add(months: 1)
let next_quarter = Time.now().add(months: 3)
let next_year    = Time.now().add(years: 1)
```

#### `t.start_of(unit: String, utc: false) -> Instant`

カレンダー単位の先頭に丸める。`unit` ∈ `"year"` / `"month"` /
`"day"` / `"hour"` / `"minute"`。それ以外は`ValueError`。

```culebra
# doctest: skip
let day_bucket  = t.start_of("day")
let hour_bucket = t.start_of("hour")
```

#### `t.unix() -> Float`、`t.unix_nanos() -> Long`

Unix epochをFloat秒（現在時点で ~400ns精度）またはLong ns
（loss-less）で取得。

### `Duration` コンストラクタ

```culebra
# doctest: skip
Time.seconds(n)        # n 秒
Time.milliseconds(n)
Time.minutes(n)
Time.hours(n)
Time.days(n)
```

`n`はLongでもFloatでも可 — 小数単位はナノ秒に丸める。

### `Duration` method

#### `d.seconds() / .milliseconds() / .minutes() / .hours() / .days() -> Float`

指定単位での値（小数単位をround-tripするため常にFloat）。

#### `d.abs() -> Duration`

絶対値（負のdurationを正に）。

### 演算子オーバーロード

```culebra
# doctest: skip
let t = Time.now()
let one_hour = Time.hours(1)

t + one_hour            # Instant + Duration → Instant
t - one_hour            # Instant - Duration → Instant
t1 - t2                 # Instant - Instant → Duration

Time.minutes(1) + Time.seconds(30)   # → Duration (90s)
one_hour * 2                          # → Duration
one_hour / 2                          # → Duration
-one_hour                             # → Duration

a < b, a <= b, a == b                 # 両クラスで自然な順序
```

どちらもナノ秒を包むクラスですが、互換ではありません。意味を持たない
組み合わせ（`t1 + t2`、`t < one_hour`、どちらかが素の数値）は、黙って
計算せずに受け取った型を明示した`TypeError`になります。等値比較だけは
例外で、他の型との`==`はエラーではなく`false`です。

---

## 6. `Random`

乱数生成。プロセスごとに単一のMersenne-Twister-64エンジンを
持ち、インタプリタとJITで共有しています。`Random.seed(n)`は
エンジンをリセットし、以降の呼び出しを1回のプログラム実行内で
再現可能にします。`seed`を呼ばなければ`std::random_device`で
初期化されます。

### `Random.seed(n: Long) -> Nil`

PRNGを再シード。同じ`n` → 同じ系列。

```culebra
Random.seed(42)
```

### `Random.int(lo: Long, hi: Long) -> Long`

半開区間`[lo, hi)`からの一様整数。`hi > lo`必須、
違反すると`type error`。

```culebra
Random.seed(0)
inspect(Random.int(0, 10))        # 0..9
```

### `Random.uniform(lo: Float, hi: Float) -> Float`

半開区間`[lo, hi)`からの一様実数。`Long`引数も受け付け、
`Float`に昇格します。

### `Random.gauss(mu: Float, sigma: Float) -> Float`

平均`mu`、標準偏差`sigma`のガウス分布から1サンプル。
`Long`引数は`Float`に昇格します。

```culebra
Random.gauss(0.0, 1.0)         # 標準正規
```

### `Random.shuffle(a: Array) -> Nil`

Fisher–Yatesによるインプレース置換。`nil`を返し、引数は破壊的に
並び替えられます。

### `Random.weighted_choice(pop: Array, weights: Array) -> Any`

対応する`weights`に比例する確率で`pop`から1要素を取り出します。
`weights`はすべて数値かつ`pop`と同じ長さである必要があります。
空または長さ不一致は`type error`。重み`0`は選ばれません。

```culebra
Random.weighted_choice(['hit', 'miss'], [1, 9])   # ~10% 'hit'
```

---

## 7. `Sys`

プロセスレベルの情報。

### `Sys.argv -> Array`

スクリプトにコマンドラインで渡された`String`引数の配列。最初の
非フラグ引数がスクリプトパスで、**それより後ろ**がすべて`argv`
として取り込まれます。culebra自身のフラグ
（`--jit`・`--debug`など）はスクリプトパスより前に置く必要があり
ます。末尾引数が無い場合やREPL実行時は空配列です。

```culebra
# doctest: skip
# $ culebra run.cul hello world
inspect(Sys.argv)        # ['hello', 'world']
# $ culebra --jit run.cul hello   →  ['hello']   (--jit は culebra 用)
```

単独の`--`は任意のescape hatchです。フラグ解析を打ち切るので、
ダッシュで始まるファイル名でも次の引数をスクリプトにできます
（例: `culebra -- -weird.cul`）。通常は不要です。

`culebra build`で作ったバイナリも同じ形で（プログラム名を同様に除いて）
実行時の引数を返します。値はプロセス全体で共有されるので、
[isolate](#12-isolate) やworkerスレッド上のHTTPハンドラでも
メインスレッドと同じ配列が読めます。

### `Sys.exit(code: Long) -> Nil`

指定の終了コードでプロセスを即座に終了します。呼び出しは戻らず、
保留中の`defer`文も**実行されません**。

```culebra
# doctest: skip
if error_occurred { Sys.exit(1) }
```

### `Sys.env(name: String) -> String`

環境変数`name`の値を返します。未設定の場合は`''`（空文字列）。
未設定と空文字列設定を区別したい場合は`!v.empty()`を使用。

```culebra
# doctest: skip
inspect(Sys.env('HOME'))          # '/Users/alice'
inspect(Sys.env('NOT_A_VAR'))     # ''
```

### `Sys.set_env(name: String, value: String) -> Nil`

環境変数`name`を`value`に設定します（既存の値は上書き）。変更はこのプロセス
（`Sys.env`経由）と、以降に起動する子プロセス（例: `Proc.run`）から見えます。
失敗時（不正な変数名など）は`IOError`を送出します。

```culebra
Sys.set_env('CULEBRA_MODE', 'fast')
inspect(Sys.env('CULEBRA_MODE'))  # => 'fast'
```

### `Sys.getcwd() -> String`

現在の作業ディレクトリの絶対パスを返します。パス中のシンボリックリンクは
解決されます。ディレクトリを特定できない場合（プロセスの足元でディレクトリが
削除された等）は`IOError`を送出します。

```culebra
# doctest: skip
inspect(Sys.getcwd())             # '/Users/alice/project'
```

### `Sys.chdir(path: String) -> Nil`

プロセスの作業ディレクトリを`path`に変更します。パスが存在しない、または
ディレクトリでない場合は`IOError`を送出します。

```culebra
# doctest: skip
Sys.chdir('/tmp')
inspect(Sys.getcwd())             # '/tmp'（または解決後のパス）
```

### `Sys.executable -> String`

実行中のculebraバイナリの絶対パス。`culebra`が`PATH`にあることに頼らず、
インタプリタのワーカーコピーを起動するのに使う — 例
`Proc.run([Sys.executable, "worker.cul"], ...)`。（AOTビルドされたプログラムでは
その単体バイナリ自身のパスになる。）

```culebra
# doctest: skip
inspect(Sys.executable)           # '/usr/local/bin/culebra'
```

### `Sys.script -> String?`

実行中スクリプトの絶対パス（`__file__`相当）。カレントディレクトリに頼らず、
スクリプトの隣にあるファイルを解決するのに使う：
`FS.join(FS.dirname(Sys.script), "data.txt")`。実行時にソースファイルが存在しない
場合 — REPL・パイプした`stdin`・AOTビルドされたバイナリ（`.cul`を持たない。その
場合は`Sys.executable`を使う）— では`nil`。

```culebra
# doctest: skip
inspect(Sys.script)               # '/Users/alice/project/build.cul'
```

### `Sys.time() -> Float`

**単調時計**の経過秒数。原点はプロセス内での最初の呼び出し。壁時計の
補正で巻き戻ることがないので、コード区間の計測にはこちらを使います
（日時が必要なら`Time.now()`、§5）。`Time.monotonic()`（§5）も同じ
単調時計を読みますが、原点はそれぞれの関数の初回呼び出しなので、
2つを混ぜず同じ関数の2点の差を取ってください。

```culebra
let t0 = Sys.time()
let sum = range(1000).reduce(0, |a, x| a + x)
inspect(Sys.time() - t0 >= 0.0)   # => true
```

### `GC` — ヒープ情報の取得

`GC.stat()`はフルコレクションを実行し、その直後の生きたヒープを表す
`Object`を返す:

| キー | 型 | 意味 |
|---|---|---|
| `live_objects` | `Long` | 到達可能なヒープオブジェクト数 |
| `rc_objects` | `Long` | 到達可能な *参照カウント* オブジェクト数（traced-onlyなString/StringViewを除く） |
| `heap_bytes` | `Long` | それらが占めるバイト数 |

先にコレクションを走らせるので、数値はsweep待ちの循環残渣ではなく
*到達可能な* 状態を表す。呼び出し自体が結果`Object`を確保するため、
連続して読むと小さな定数分だけ差が出る — 絶対値ではなく対象コード前後の
差分（delta）を測ること。

```culebra
# doctest: skip
let base = GC.stat().live_objects
build_some_structure()
inspect(GC.stat().live_objects - base)   # 構造が保持しているオブジェクト数
```

これはリーク回帰テストの土台になる（`tests/test_gc_no_leak.cul`参照）:
多数の反復をまたいでdeltaが有界に留まることをassertする。メモリは
それ以外は自動管理 — メモリモデルと確定的`drop`は言語仕様を参照。

---

## 8. `Tensor`

N次元数値テンソル。lazy計算グラフを構築し、`Tensor.eval(...)`で
BLAS / vDSP経由のカーネルを起動して値を確定します。dtypeは
`Float32`（デフォルト）と`Float64`、形状はvariadicか`[m, n]`
配列で指定。`transpose` / `slice` / `reshape`はzero-copy view。

```culebra
let A = Tensor.from([[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]])  # [2, 3]
let B = Tensor.from([[1.0, 0.0], [0.0, 1.0], [1.0, 1.0]]) # [3, 2]
let C = A.dot(B) + 1.0                # lazy: グラフを作るだけ
Tensor.eval(C)                        # ここで BLAS GEMM が走る
inspect(C.shape())                    # => [2, 2]
inspect(C.to_array())                 # => [[5.0, 6.0], [11.0, 12.0]]
```

### 構築（名前空間関数）

#### `Tensor.zeros(...) -> Tensor` / `Tensor.ones(...)` / `Tensor.randn(...)`

形状をvariadic（`Tensor.zeros(3, 4)`）またはArray
（`Tensor.zeros([3, 4])`）で受け取ります。dtypeは文字列タグを
**第一引数**に置くJulia流。dtypeは`"f32"`のみです（float64は
GPUバックエンドに高速パスがないため）：

```culebra
let a   = Tensor.zeros(3, 4)              # F32 default
let a32 = Tensor.zeros("f32", 3, 4)       # 明示
let dims = [3, 4]
let b   = Tensor.zeros(dims)              # 計算済み形状
let r   = Tensor.randn(2, 3)              # 標準正規
```

#### `Tensor.from(arr: Array) -> Tensor`

ネストされたCulebra配列をTensorに変換します。1D（`[1.0, 2.0]`）
または2D（`[[1.0, 2.0], [3.0, 4.0]]`）を受け、F32で格納：

```culebra
let v = Tensor.from([1.0, 2.0, 3.0, 4.0])      # [4]
let m = Tensor.from([[1.0, 2.0], [3.0, 4.0]])  # [2, 2]
```

#### `Tensor.concat(parts: Array) -> Tensor`

Tensorを軸0（行）方向に積み重ね、1つのmaterialized Tensorに
します。すべてのpartはdtypeが一致し、軸0より後ろの次元も一致
している必要があります。結果の行数は各partの行数の合計です。
微分可能 — 勾配は各partの行範囲に切り分けて戻されます。

```culebra
let a = Tensor.from([[1.0, 2.0], [3.0, 4.0]])  # [2, 2]
let b = Tensor.from([[5.0, 6.0]])              # [1, 2]
let c = Tensor.concat([a, b])                  # [3, 2]
```

#### `Tensor.from_csv(path: String) -> Tensor`

CSVファイルを直接contiguousなTensorに読み込みます。常に
**rank-2** を返す — 単列CSVは`[N, 1]`（biasベクトル形式）。
nested Arrayを経由しないので、`Tensor.from(load_2d(path))`
パターンより3-5x速い（MNIST規模で実測）：

```culebra
# doctest: skip
let W1 = Tensor.from_csv("W1.csv")    # [30, 784]
let b1 = Tensor.from_csv("b1.csv")    # [30, 1]
let X  = Tensor.from_csv("X.csv")     # [N, 784]
```

#### `Tensor.eval(t1, t2, ...) -> Nil`

可変長のTensorを受け、依存グラフをtopological順に評価します。
共有部分式は一度だけ計算されます。学習ループのmini-batch境界で
**必ず一度呼ぶ**（呼ばないとグラフが累積してメモリが膨張）。

```culebra
# doctest: skip
W2 -= d2.dot(a1.transpose()) * lr
b2 -= d2.sum(1).reshape([N_OUT, 1]) * lr
W1 -= d1.dot(xb.transpose()) * lr
b1 -= d1.sum(1).reshape([N_HID, 1]) * lr
Tensor.eval(W1, b1, W2, b2)              # 4 つを 1 パスで評価
```

### 活性化関数

Tensorのインスタンスメソッドです。ユーザのクラスが独自に`relu` /
`sigmoid` / `softmax`を定義していても（microgptの`Value.relu()`
など）、メソッド解決はクラスメソッドをビルトインより優先するため衝突
しません。

```culebra
# doctest: skip
let h = z.sigmoid()        # 1/(1+exp(-z)) elementwise
let r = x.relu()           # max(0, x)
let p = logits.softmax()   # 最終軸で online stable
let l = p.log()            # 自然対数、elementwise
```

### Tensor のメソッド

形状・線形代数・reductionはメソッド構文：

| メソッド | 戻り値 | 説明 |
|---|---|---|
| `.shape() -> Array` | Array of Long | 形状をArrayで返す |
| `.dot(other: Tensor) -> Tensor` | lazy | 行列積。両辺rank-2 |
| `.linear_sigmoid(x, b) -> Tensor` | lazy | 融合`sigmoid(self @ x + b)` |
| `.pow(exp) -> Tensor` | lazy | elementwise冪、expはTensorまたはscalar |
| `.transpose() -> Tensor` | view | 全軸逆順（rank-2で行列転置） |
| `.slice(start, end) -> Tensor` | view | 軸0を`[start, end)`で切り出し |
| `.reshape(dims: Array) -> Tensor` | view | 連続入力のみ。新形状 |
| `.sum() -> Float` | scalar | 全要素和（暗黙eval） |
| `.sum(axis: Long) -> Tensor` | lazy | 軸を1つ畳む |
| `.mean() / .mean(axis)` | Float / Tensor | 同様 |
| `.max() / .max(axis)` | Float / Tensor | 同様 |
| `.argmax(axis: Long) -> Tensor` | lazy | 軸を畳んでインデックスをFloatで格納 |
| `.to_array() -> Array` | eager | Culebra Arrayへ変換（暗黙eval） |
| `.item() -> Float` | eager | 唯一の要素をFloatとして取り出す。要素数が1でない（任意rank）場合は例外 |

`.item()`はスカラーの取り出し口で、`.to_array()`（形状を持つデータ用）と対をなす。
lossなど単一要素の結果をreshapeせず読むのに使う。`loss.item()`は
`to_float(loss.to_array()[0])`の置き換え。

### 自動微分（reverse-mode）

Tensorプリミティブはネイティブなreverse-mode自動微分エンジンを
持ちます。forwardグラフがそのままtapeを兼ね、`.backward()`が
C++ 側でそれを辿ります。スクリプト側のラッパは不要 — 値を計算する
op自身がvector-Jacobian productを知っています。tapeが記録される
のは`requires_grad`な葉に繋がるopのみ。forward-onlyな処理
（推論、あるいはbackwardを自前で書く学習ループ）はtapeを一切
記録しないので、下層のテンソルライブラリと同等のコストで済みます。

| メソッド | 戻り値 | 説明 |
|---|---|---|
| `.requires_grad() -> Tensor` | self | 葉に勾配累積を要求。チェーン可 |
| `.backward() -> Nil` | — | `dL/dself = 1`を起点に全葉へ伝播 |
| `.grad() -> Tensor` | Tensor | 累積勾配（`backward`前はzeros） |
| `.zero_grad() -> Nil` | — | 次ステップ前に勾配をクリア |
| `.detach() -> Tensor` | Tensor | グラフも勾配も持たないmaterializedコピー |

`requires_grad`はforwardに伝播します — 勾配追跡する入力を持つop
の出力も勾配を追跡します。微分可能なopは`+ - * /`、`.pow()`（底に
ついて）、`.dot()`、軸`.sum()` / `.mean()`、`.relu()`、`.sigmoid()`、
`.softmax()`、`.log()`、`.transpose()`、`.reshape()`、`.slice()`、
`Tensor.concat()`。勾配は自動でun-broadcastされるので、バッチ越しに
加えたbiasは元の形状に和を取って戻ります。

```culebra
let w = Tensor.from([[2.0, 0.0], [0.0, 3.0]]).requires_grad()
let x = Tensor.from([[1.0], [1.0]]).requires_grad()
let y = w.dot(x)              # [2, 1]
let loss = (y * y).sum(0).sum(0)
loss.backward()
Tensor.eval(w.grad(), x.grad())
let gw = w.grad().to_array()  # dL/dw
```

典型的な学習ステップは、勾配をゼロ化 → forward → `.backward()` →
optimizer更新のため`.grad()`を読む → 新しい重みを`.detach()`して
次ステップをクリーンな葉から始める、という流れです。
`benchmarks/microgpt/microgpt_tensor.cul`のtransformerが完全な実例
（embedding、KVキャッシュ付きattention、RMSNorm、MLP、交差エントロピー、
Adam）で、すべてこれらのメソッドだけで構築されています。

`.backward()`はforwardバッファから勾配を読むためlossの
`Tensor.eval`を伴います。`.grad()`は他と同じTensorを返すので、
`.to_array()`の前に`Tensor.eval`でmaterializeしてください。

`Tensor.no_grad(fn) -> Any`は勾配追跡を抑制して`fn`を実行します。
内部の演算はautogradグラフを作らず（テープも`requires_grad`伝播も
発生しない）、`fn`の戻り値をそのまま返します。推論など、逆伝播しない
forwardに使います。

```culebra
# doctest: skip
let logits = Tensor.no_grad(fn () { model_forward(x) })
```

### 演算子オーバーロード

`+ - * /`はブロードキャストelementwise（numpy / silarray規則）。
スカラーとの混在も自動：

```culebra
let M = Tensor.ones(3, 4)
let v = Tensor.ones(4)            # → [3, 4] にブロードキャスト
let r = Tensor.ones(3, 1)         # → [3, 4] にブロードキャスト
Tensor.eval(M + v, M + r, M + 1.0, M * 2.0)
```

`@`演算子は未実装（`.dot()`を使う）。

### 複合代入 (`+=` `-=` `*=` `/=` `**=`) と in-place 書き込み

複合代入は左辺のTensorバッファに直接書き戻します（新規Tensorを
確保しない）。条件は「LHSが自前バッファを所有していて、形が右辺の
broadcast結果と一致する」こと — view・未評価グラフノード・形状不一致
の場合は通常経路（新規Tensor）に自動でフォールバックします。

```culebra
# doctest: skip
mut W = Tensor.randn('f32', 1024, 256)
let alias = W
W -= grad * lr     # W のバッファを直接書き換え
Tensor.eval(alias) # alias の to_array() でも更新後の値が見える
```

SGD形式の重み更新で`W = W - grad * lr`と書くと毎ステップWサイズ
分の確保が起きます。`-=`ならその確保が消えるので、巨大重みでループが
回るほど差が広がります（実測で5000ステップ・1024×256 f32のloopで
plain `=` 5.5s → `-=` 3.6s）。

サポートop: `+= -= *= /= **=`（`%=`と`@=`は対象外 — `%`は
Tensorで意味づけしておらず、`@`は出力形状が変わるためin-place不可）。

### dtype / 形状の制約

- dtypeはF32のみ（float64はGPUバックエンドに高速パスがない。
  `.item()` / `.to_array()`などスカラー出口は`Float`を返す）
- `.dot()`はrank-2のみ。3D+ batched matmulは将来検討
- `.reshape()`は連続入力のみ（transpose後reshapeはmaterializeが必要 —
  今は明示的に`Tensor.from((...).to_array())`を経由）
- `.softmax()`も連続入力のみ

### バックエンドとデバイス選択

評価はvendoredな`cpp-tensorlib`エンジン（`vendor/cpp-tensorlib`）に
委譲されます。lazy graph・カーネル融合・デバイスバックエンドはすべて
そちら側の責務です:

- **CPU** — ベクトル化カーネル（AVX2 / NEON）。macOSではBLAS形状の
  カーネルにAccelerateを使用
- **GPU** — macOSはMetal、Linux / WindowsはCUDA

デバイスはプロセスグローバル（interp / JIT / AOTで共有）で、実行時に
切り替えられます:

| 関数 | 効果 |
| --- | --- |
| `Tensor.use_cpu() -> Nil` | すべての演算をCPUで評価 |
| `Tensor.use_gpu() -> Nil` | GPUバックエンドで評価 |
| `Tensor.use_auto() -> Nil` | 演算ごとに問題サイズで選択（デフォルト） |
| `Tensor.gpu_available() -> Bool` | GPUバックエンドがビルドに含まれ到達可能か |
| `Tensor.device() -> String` | 現在の選択: `'cpu'` / `'gpu'` / `'auto'` |

```culebra
inspect(type_of(Tensor.gpu_available()))    # => 'Bool'
inspect(Tensor.device())                    # => 'auto'
```

`device()`が返すのは選択であって、個々の演算がどこで走ったかではありま
せん。`'auto'`では演算ごとに決まりますし、GPUに送った演算も到達できな
ければCPUで実行されます。

`use_auto`がデフォルトなのは、小さいテンソルがカーネル起動コストに
負けるからです。小さい演算はCPUに留め、移送コストに見合う大きさの
演算だけをGPUに送ります。`use_cpu()` / `use_gpu()`を呼べば — 最初の
テンソルを作る前でも — 以降の演算はそのデバイスに固定されます。

GPUが無いビルドで`use_gpu()`を呼んでもthrowせずCPU経路に
フォールバックするので、プログラムは可搬なままです。選択が結果を
左右する場合は`gpu_available()`を確認してください。格納はどのデバイス
でもF32のまま（上のdtype制約参照）。

Metalはビルド時に何も要りません。CUDAは`nvcc`が見つかったときに
組み込まれます（`CULEBRA_TENSOR_CUDA=AUTO`がデフォルト。`ON`は`nvcc`
が無ければconfigureエラー、`OFF`はバックエンドを外す）。CUDAドライバ
自体は実行時にロードされるので、CUDAを有効にしたバイナリはGPUの無い
マシンでもそのまま動き、`gpu_available()`が`false`を返すだけです。

---

## 9. `JSON`

Culebraの値とJSONテキストの相互変換。両バックエンドで同じAPI
を提供します。

### `JSON.stringify(v, indent=0, sort_keys=false, lines=false) -> String`

`v`をJSON文字列にシリアライズします。

* `indent > 0`でそのスペース数でインデントしpretty-printします
  （カンマの後に改行、`":"`の代わりに`": "`）。`indent <= 0`は
  コンパクト出力。
* `sort_keys=true`で`Object`のキーを挿入順ではなく辞書順で
  出力します。diff / ハッシュ向けの決定論的出力に有用。
* `lines=true`で **JSON Lines** を出力します。`Array` / `Tuple` /
  `Set`の各要素をそれぞれ独立した行としてcompact形式で出力し、
  末尾`\n`付き。空コレクションは空文字列を返します。`indent > 0`
  との併用や、Array/Tuple/Set以外への指定はどちらも`TypeError`。

旧APIとの互換: 位置引数2番目を`indent`として受けます。
`JSON.stringify(v, 2)`は`JSON.stringify(v, indent: 2)`と等価。

サポート対象:

| Culebra            | JSON                              |
|--------------------|-----------------------------------|
| `Nil`              | `null`                            |
| `Bool`             | `true` / `false`                  |
| `Long`, `Float`    | 数値（非有限Floatは`ValueError`を投げる） |
| `String`           | クォート文字列、`\n`/`\t`/`\r`/`\"`/`\\`/`\u00xx`エスケープ |
| `Array`            | JSON配列                          |
| `Tuple`            | JSON配列（`Array`と同じ形）         |
| `Set`              | JSON配列、メンバーは挿入順            |
| `Object`（Stringキーのみ）| JSONオブジェクト、キーは挿入順       |

`Function`, `Tensor`、および非Stringキーを持つObjectは
シリアライズ不可で`TypeError`を投げます。

### `JSON.parse(s, lines=false, number_mode='auto', jsonc=false) -> Any`

JSON文字列をCulebraの値に変換します。

* `number_mode='auto'`（既定）: 小数点や指数を含まない数値は`Long`、
  それ以外は`Float`。
* `number_mode='float'`: すべての数値を`Float`に。生産者側が
  数値型を統一している場合のround-trip安全性向上に。
* `lines=true`: 入力を`\n`で分割し、空でない各行を独立したJSON
  値として解析、`Array`を返します。
* `jsonc=true`: **JSONC** として解析します。`//`行コメント・`/* */`
  ブロックコメント・オブジェクト/配列の末尾カンマを許容するため、
  既存の設定ファイル（`tsconfig.json`、VSCodeの`settings.json`等）を
  事前に除去せず読めます。既定は厳格なJSONで、コメントや末尾カンマは
  `ValueError`で拒否します。

不正な入力には`ValueError`が投げられ、構造化Errorの
`e.line` / `e.col`（共に1-based、エラー位置の文字）にJSON内部の
位置が乗ります:

```culebra
let r = try { JSON.parse('{"a": ,}'); nil } catch e { e }
inspect(r.message)           # => 'JSON.parse: expected value'
inspect("{r.line}:{r.col}")  # => '1:7'
```

例:

```culebra
let v = {name: 'alice', age: 30, tags: ['admin', 'staff']}
# 既定はコンパクト。`sort_keys` はキーを辞書順に並べる。
inspect(JSON.stringify(v))                  # => '{"name":"alice","age":30,"tags":["admin","staff"]}'
inspect(JSON.stringify(v, sort_keys: true)) # => '{"age":30,"name":"alice","tags":["admin","staff"]}'
let back = JSON.parse(JSON.stringify(v))
inspect(back.name)                          # => 'alice'
let arr = JSON.parse("1\n2\n3\n", lines: true)
inspect(arr)                                # => [1, 2, 3]
let cfg = JSON.parse('{
  // コメントと末尾カンマを許容
  "port": 8080,
}', jsonc: true)
inspect(cfg.port)                           # => 8080
```

`indent`は整形出力、`lines`はJSON Linesを出します。どちらも複数行に
なります:

```culebra
let v = {name: 'alice', age: 30, tags: ['admin', 'staff']}
inspect(JSON.stringify(v, indent: 2))
inspect(JSON.stringify([1, 2, 3], lines: true))
# => |
# '{
#   "name": "alice",
#   "age": 30,
#   "tags": [
#     "admin",
#     "staff"
#   ]
# }'
# '1
# 2
# 3
# '
```

JITメモ: ビルトインの`JSON.{stringify, parse}`は他の名前空間
メソッドと同じ正準呼び出しリゾルバを経由するため、すべての呼び出し
形がインタプリタと同一に振る舞います。位置引数の束縛
（`JSON.stringify(v, 2)`は`indent`、`JSON.parse(s, true)`は
`lines`）、キーワード引数、リテラル`**{...}`と動的`**variable`
splatの両方。第一級の値として使った場合も束縛は同じです
（`let f = JSON.stringify; f(v, indent: 2)`）。

---

## 10. `Args`

宣言的なCLI引数パーサ。specはculebra `Object`でpositional / option /
subcommandを列挙し、`Args.parse`はparse結果を`Object`で返す。
`--help`指定でhelpをstdoutに出して`Sys.exit(0)`、パースエラー時は
stderrにerror表示 + `Sys.exit(2)`。プログラム制御したい場合は
`Args.try_parse`を使うと`{kind: "ArgParseError", message}`または
`{kind: "ArgParseHelp", help}`をthrowする。

### `Args.parse(argv: Array<String>, spec: Object) -> Object`

`argv` (通常`Sys.argv`) を`spec`に従ってparse。`--help` / `-h`でhelp
をstdout出力 + `Sys.exit(0)`、パースエラーでstderr出力 + `Sys.exit(2)`。

### `Args.try_parse(argv, spec) -> Object`

同じエンジンだがexitせず例外をthrow。テストや独自UX構築用。

### `Args.help(spec: Object) -> String`

parse / exitせずにhelp文字列だけ取得。広めのメッセージに埋め込みたい
時など。

### Spec 形式

各引数は次のフィールドを持つ`Object`:

| field | 型 | デフォルト | 意味 |
|---|---|---|---|
| `name` | `String` | (必須) | parse結果のkey |
| `type` | `String` | `"String"` | `"String"` / `"Long"` / `"Float"` / `"Bool"` |
| `short` | `String` | (なし) | 短縮形 (`"v"` → `-v`)。指定するとoption扱い。 |
| `default` | `Any` | (なし) | 省略時の値。指定するとoptional。 |
| `doc` | `String` | `""` | help用説明文 |
| `repeated` | `Bool` | `false` | 複数指定可、`Array`で集約 |

`type: "Bool"`の場合は値を取らない **flag** (`--verbose` / `-v`)。それ以外
の型は次のトークンを値として消費する (`--count 5` / `--count=5`)。

`short`も`default`も無い引数は **positional** 扱い。spec順にマッチし、
positionalは全て必須。`default`を付けた引数はpositionalではなく
**option** になり、long形 (`--encoding utf-8`) で渡す。`short`はそれに
1文字形 (`-l`) を足す。つまり省略可能な *positional* は表現できない —
省略可能にしたい引数は`default`を持たせて`--name value`の形で渡す。

### 例

```culebra
# doctest: skip
let spec = {
  name: "wc-lite",
  doc:  "count lines and words",
  args: [
    {name: "input",   type: "String", doc: "input file"},
    {name: "lines",   short: "l", type: "Bool", default: false, doc: "count lines"},
    {name: "words",   short: "w", type: "Bool", default: false, doc: "count words"},
    {name: "encoding",            type: "String", default: "utf-8"}
  ]
}

let args = Args.parse(Sys.argv, spec)
inspect(args.input)
if args.lines { inspect("lines: ...") }
if args.words { inspect("words: ...") }
inspect("encoding: {args.encoding}")
```

```
$ ./wc-lite -l file.txt
lines: ...
encoding: utf-8

$ ./wc-lite --help
wc-lite - count lines and words

Usage: wc-lite [options] <input>

Arguments:
  input    input file

Options:
  -l, --lines        count lines
  -w, --words        count words
      --encoding
  -h, --help         show this help and exit
```

### サブコマンド

`spec.subcommands`は`Array<Object>`で各要素がsub-spec (top-levelと
同形)。指定すると最初のpositionalトークンがsubcommand名として扱われ、
parse結果の`subcommand`フィールドに名前が入り、残りの引数はselected
subcommandのspecに従ってparseされる:

```culebra
# doctest: skip
let spec = {
  name: "git-lite",
  subcommands: [
    {name: "add",    args: [{name: "files",   type: "String", repeated: true}]},
    {name: "commit", args: [{name: "message", short: "m", type: "String"}]}
  ]
}

match Args.parse(Sys.argv, spec).subcommand {
  "add"    => stage_files(args.files),
  "commit" => commit_with_message(args.message)
}
```

### エラーハンドリング

`Args.parse`はエラー時にexitする。`Args.try_parse`はthrow:

```culebra
let r = try { Args.try_parse(["--bogus"], spec) } catch e { e }
# r == {kind: "ArgParseError", message: "unknown option '--bogus'"}
```

throw値の`kind`は次のいずれか:

| `kind` | 意味 | 付随フィールド |
|---|---|---|
| `ArgParseError` | parse失敗（不明オプション、型不一致、必須欠落、etc.） | `message` |
| `ArgParseHelp` | `--help` / `-h`指定 | `help`（help文字列） |

---

## 11. `Proc`

外部コマンドを同期（blocking）実行し、その出力を取得します。コマンドは
`Array<String>`で、`cmd[0]`が実行ファイル（PATH解決）、残りが引数です。
シェルを介さないのでクォートやインジェクションの心配がありません
（`["git", "commit", "-m", msg]`は`msg`をそのまま渡します）。

### `Proc.run(cmd: Array<String>, cwd=nil, env=nil, stdin="", check=false, timeout=0, share=nil) -> Object`

`cmd`を完了まで実行し、結果Objectを返します:

| フィールド | 型 | 意味 |
|---|---|---|
| `code` | `Long` | 正常終了時の終了コード。シグナル死した場合は`-1` |
| `stdout` | `String` | コマンドがstdoutに書いた全内容（一括取得） |
| `stderr` | `String` | stderrに書いた全内容 |
| `ok` | `Bool` | `code == 0`かつ`signal == nil`のとき`true` |
| `signal` | `String?` | シグナル死した場合のシグナル名（`"SIGTERM"`等）、それ以外は`nil` |
| `error` | `String?` | 起動失敗時のメッセージ。コマンドが実際に走った場合は常に`nil`。これを設定するのは`Proc.all`（allSettledのエラー表現）のみで、`Proc.run`では起動失敗はthrowされるため常に`nil`。 |
| `timed_out` | `Bool` | `timeout`超過でkillされた場合`true`、それ以外`false`。 |

キーワード引数:

- `cwd: String` — 子プロセスの作業ディレクトリ（既定: 親を継承）。
- `env: Object` — 環境変数。親の環境にマージされるので`PATH`等は維持されます
  （既定: そのまま継承）。値は`String`であること。
- `stdin: String` — 子プロセスの標準入力に書き込むバイト列。書き込み後にクローズ
  されます（既定: 空）。
- `check: Bool` — `true`のとき、非0終了・シグナル死・timeoutで`{ok: false}`を
  返す代わりに`ProcessError`をthrowします（既定: `false`）。
- `timeout: Long` — ミリ秒。これを超えて走るとコマンドはkillされ（`SIGTERM` →
  短い猶予の後`SIGKILL`）、結果は`ok: false` / `timed_out: true`になります
  （既定: `0` = 無制限）。**直接の子だけをkill**し、その子が生んだ孫はkillしません。
  stdout/stderrを早期に閉じて走り続けるプロセスには
  timeoutが届かないことがあります。

**非0終了**や**シグナル死**はエラーではなく通常の結果です — `ok` / `code` /
`signal`で分岐してください。**起動失敗**（実行ファイルが存在しない等）や
`check: true`での失敗のみが`ProcessError`をthrowします。非Array・非String
要素・空コマンドは`TypeError` / `ValueError`をthrowします。

```culebra
# doctest: skip
let r = Proc.run(["git", "rev-parse", "--abbrev-ref", "HEAD"])
if r.ok {
  IO.inspect("on branch " + r.stdout.trim())
} else {
  IO.print(r.stderr)
}

# 標準入力を渡して変換結果を読む。
let up = Proc.run(["tr", "a-z", "A-Z"], stdin: "hello\n")
assert_eq(up.stdout, "HELLO\n")

# ディレクトリと環境変数を指定し、失敗時に throw。
Proc.run(["make", "install"], cwd: "/src/app", env: {PREFIX: "/usr/local"}, check: true)
```

出力は全量バッファされるため、巨大な出力はそのぶんメモリを使います。stdoutと
stderrは並行して読み出すので、両方を埋めるコマンドでもデッドロックしません。

`share: {名前: buf}`は1つ以上の`SharedBuffer.shared(...)` bufferを子プロセス
へ渡す（子は`SharedBuffer.receive(name, Class)`で再アタッチする）。子はculebra
プロセスである必要があり、通常は`[Sys.executable, "worker.cul"]`。詳細は
[SharedBuffer › プロセス間での共有](#プロセス間での共有zero-copy)。

### `Proc.all(commands: Array<Array<String>>, limit: Long = <CPU数>, timeout: Long = 0, fail_fast: Bool = false, retries: Long = 0, share: Object? = nil) -> Array<Object>`

複数コマンドを並列実行し、結果Objectを入力順で返します。各コマンドは
`Array<String>`（`Proc.run`の第1引数と同形）。同時実行数は最大`limit`（既定 =
オンラインCPU数。絞るには小さい値、広げるには大きい値を渡す）。`timeout`（ms、
`0` = 無し）は各コマンドにその起動時刻から適用され、発火時は結果に
`timed_out: true`を立てます。

既定は **allSettled** です。1個の失敗が他を巻き込みません。走って非0終了した
コマンドは`{ok: false, code: N, error: nil}`、そもそも起動できなかった（実行
ファイルが無い等）コマンドは`{ok: false, error: "<メッセージ>"}`で、どちらも
throwしません。空リストは`[]`を返します。

**`fail_fast: true`** の場合は最初の失敗（非0終了・シグナル・timeout・起動失敗）
で残りの実行中コマンドを`SIGKILL`し、該当コマンドを示す`ProcessError`をthrow
します（既定の`Promise.allSettled`に対する`Promise.all`形）。全コマンド成功時は
通常どおり結果配列を返します。

**`retries`** は失敗したコマンドをその回数だけ再実行し、最終試行の結果を採用します。
再実行は空きが出た`limit`プールに割り込みます。`fail_fast`と併用した場合、コマンドが
失敗とみなされるのはretriesを使い切った後だけです。

**`share: {name: buf}`** は`SharedBuffer.shared(...)`バッファを**全**子に渡す
（各子が`SharedBuffer.receive`で再アタッチ）。`Proc.run`の`share:`と同じで、
ワーカープールが共有結果バッファに書ける（競合するセルは`buf.with_lock`）。

```culebra
# doctest: skip
let results = Proc.all([
  ["git", "fetch", "origin"],
  ["npm", "test"],
  ["cargo", "build"],
], limit: 2)
for r in results {
  if !r.ok { IO.print(r.error ?? r.stderr) }
}
```

### `Proc.race(commands: Array<Array<String>>, share: Object? = nil) -> Object`

全コマンドを起動し、**最初に完了した1個**の結果Objectを返し、残りに`SIGKILL`
を送ってreapします。冗長なプロバイダの競争や「最速のミラーが勝ち」に有用。空
リストは`ValueError`をthrowします。`share: {name: buf}`は`Proc.run`/`Proc.all`
と同様に共有バッファを子に渡します。

```culebra
# doctest: skip
let fastest = Proc.race([
  ["curl", "-s", "https://mirror-a.example/file"],
  ["curl", "-s", "https://mirror-b.example/file"],
])
IO.print(fastest.stdout)
```

### `Proc.spawn(cmd: Array<String>, cwd=nil, env=nil, stdin="", share=nil) -> handle`

コマンドを起動し、完了を待たずに即座に**ライブハンドル**を返します。ハンドルは
3つのメソッドを持ちます:

| メソッド | 戻り値 | 意味 |
|---|---|---|
| `h.wait()` | 結果Object | 子の終了まで待ち、出力をdrainする（ブロッキング） |
| `h.poll()` | 結果Objectまたは`nil` | 終了していれば結果、まだなら`nil`（非ブロッキング） |
| `h.kill(sig = 15)` | `nil` | シグナル送出（既定`SIGTERM`）。次の`wait`/`poll`がreap |

`wait()` / `poll()`は冪等で、子をreapした後はどちらも同じキャッシュ済み結果
Object（通常の`{code, stdout, stderr, ok, signal, error, timed_out}`）を返します。
起動失敗は`Proc.run`と同様`ProcessError`をthrowします。一度もwaitされずに
捨てられたハンドルはGCがreapし（子を`SIGKILL`）、ゾンビとして残りません — ただし
明示的に`wait()` / `kill()`する方が明快です。他のverbと同様、シグナルは直接の子
にのみ送られます（孫には届きません）。

```culebra
# doctest: skip
let server = Proc.spawn(["python", "-m", "http.server", "8000"])
# ... サーバに対して作業 ...
server.kill()                 # SIGTERM
let r = server.wait()
IO.inspect("server exited via " + (r.signal ?? to_string(r.code)))

# ブロックせずに完了をポーリング。
let job = Proc.spawn(["make", "-j4"])
while job.poll() == nil {
  IO.print(".")               # ...他の作業...
}
```

`stdin`はspawn時に一度だけ渡されてクローズされます。逐次streaming I/Oと
パイプライン（`a | b`）は将来追加予定です。

---

## 12. `Isolate`

クロージャを専用OSスレッド上で（独立したGCヒープを持たせて）実行し、真の
CPU並列を得ます。isolate間で可変メモリは共有されません。値は境界を越える際に
**コピー**されるため、2つのisolateが同じオブジェクトで競合することは決して
ありません。[§11 `Proc`](#11-proc)（プロセス並列）のスレッド版に当たります。

> `Isolate.spawn`・`Channel`・`Parallel`はいずれもインタプリタと`--jit`の
> 両方で動作します（クロージャは共有コード参照 — インタプリタはAST、JITは
> コンパイル済み`fn_ptr` — とコピーした捕獲で越境し、子の自前ヒープで実行）。

### `Isolate.spawn(fn, *args) -> handle`

`fn`を別スレッドで実行し、即座にライブハンドルを返します。位置引数`args`は
`fn`に渡されます。

```culebra
# doctest: skip
let h = Isolate.spawn(|| 1 + 2)
h.join()                       # => 3

let h2 = Isolate.spawn(|n| n * n, 7)
h2.join()                      # => 49
```

ハンドルのメソッド:

| メソッド | 戻り値 | 意味 |
|---|---|---|
| `h.join()` | クロージャの戻り値 | isolateの完了を待ち、（コピーされた）結果を返す |
| `h.poll()` | 結果または`nil` | 完了済みなら結果、未完了なら`nil`（ノンブロッキング） |

クロージャが例外を投げた場合、その例外は`join()`を呼んだスレッド上で
（kindを保ったまま）再送出されます。`join()`されずにdropされたハンドルは
GCがjoinするため、スレッドが取り残されることはありません。

### Sendable: 境界を越えられる値

クロージャ・その引数・戻り値は **Sendable** でなければなりません。違反は
`spawn`の時点で`SendError`を投げます（黙ってコピーはしません）:

| Sendable | Sendableでない |
|---|---|
| 数値・`String`・`Bool`・`nil` | ネイティブハンドル（`Proc` / `File` / isolateハンドル） |
| Sendable値からなる`Array` / `Object` / `Set` / `Tuple` | `Tensor`（将来はバッファ経由で共有） |
| `enum` / data-classインスタンス | `mut`変数を捕獲したクロージャ |
| Sendable値のみを捕獲したクロージャ | 自己参照する値（循環参照） |
| 自由関数（`fn name(...)`、参照で捕獲） | |

捕獲はコピーされるため、捕獲したコレクションを変更するクロージャは**自分の
コピー**を変更します。親側の値はそのままです:

```culebra
# doctest: skip
let xs = [1, 2, 3]
let h = Isolate.spawn(fn () { xs.push(99); xs.size() })
h.join()                       # => 4   (isolate 側のコピー)
xs                             # => [1, 2, 3]   (親は不変)
```

`mut`の捕獲は黙ってスナップショットを取らず拒否します — 値は引数として
渡してください:

```culebra
# doctest: skip
mut total = 0
Isolate.spawn(|| total)        # SendError: mutable 変数 'total' を捕獲
Isolate.spawn(|t| t, total)    # ok — 値渡し
```

### 並列度の上限

同時に生きているisolateには上限があります（既定はマシンのコア数、環境変数
`CULEBRA_ISOLATE_LIMIT`で上書き可）。上限を超えるspawnは新しいスレッドを
起こさず**現在のスレッド上で同期実行**されます — これにより再帰的並列が数千
スレッドに爆発しません。`join()`は同じ結果を返し、変わるのはタイミングだけです。

```culebra
# doctest: skip
# 並列 map: 仕事を isolate に分配して集約。
let parts = [[1, 2, 3], [4, 5, 6], [7, 8, 9]]
mut handles = []
for p in parts { handles.push(Isolate.spawn(|| p.reduce(0, |a, b| a + b))) }
mut total = 0
for h in handles { total += h.join() }
total                          # => 45
```

### キャンセル

isolateは協調的にキャンセル可能です。`join()`せずにハンドルをdropする（または
GCが回収する）とisolateに停止が伝わり、次の文境界またはchannelのブロッキング
境界で巻き戻ります。暴走中・待機中のisolateがプログラムをハングさせることは
ありません。

### Channel — `Channel.new(cap = 1) -> (tx, rx)`

channelはisolate間で値を渡す有界・ブロッキングのキューです。**(tx, rx)** の組を
返します。`tx.send(v)`で投入、`rx`で取り出し。channelの
endpointはSendable規則の唯一の例外で、（参照で）共有されます — クロージャが
`tx`/`rx`を捕獲してspawnしたisolateに持ち込めます。値そのものはコピーで渡り、
共有されるのはchannelだけです。

```culebra
# doctest: skip
let (tx, rx) = Channel.new(10)        # 有界・容量 10
let prod = Isolate.spawn(fn () {
  for line in source() { tx.send(parse(line)) }
  tx.drop()                            # この送信端を解放
})
tx.drop()                              # 親側の送信端も解放
for record in rx { process(record) }   # 全 tx が drop されると終了
prod.join()
```

| メソッド | 対象 | 意味 |
|---|---|---|
| `tx.send(v)` | tx | `v`を投入（バッファ満杯ならブロック）。closedなら`ChannelError` |
| `tx.clone()` / `rx.clone()` | 両方 | 同一channelの別endpoint（multi-producer / multi-consumer） |
| `tx.drop()` / `rx.drop()` | 両方 | このendpointを解放 |
| `rx.recv()` | rx | 1値をブロッキング取得。closedかつ空なら`nil` |
| `for v in rx { ... }` | rx | closedまでdrain（綺麗なend-of-streamの形） |

**auto-closeがデッドロック安全網です。** アクティブな送信端を数え、**最後の`tx`
がdropされた**時（producer isolateが正常／例外終了）にchannelがcloseし、
`for v in rx`が終了します。producerがクラッシュしてもconsumerはハングせず、
原因はproducerを`join()`してsurfaceします。multi-producerの罠に注意:
channelが閉じるには全ての`tx`（親の元のtx含む）がdropされる必要があります —
保持しないtxはdropしてください。

**`Channel.new(0)`はrendezvous channel**（容量0）: `send`は受信者が値を
受け取るまで返りません — バッファ無しの同期ハンドオフ。backpressure
（producerがconsumerを追い越せない）に有用。単一isolate内ではdeadlock
（渡す相手がいない）のでisolate間で使います。容量は`0以上`。

#### `Channel.fan_in(sources: [rx]) -> rx`

複数のreceiverを1つに束ねます。返る`rx`は、readyなsourceから順に値を
返し（全sourceを同時に待つ — Goの`select` / core.asyncの`merge`と同じ
イベント駆動、pollではない）、**全** sourceがcloseしたら終了します。これに
より各producerが`tx.clone()`で1本を共有する代わりに**自分専用のchannel**を
持てるので、multi-producerの罠（clone-drop忘れでconsumerがhang）を回避でき
ます — 各channelの`tx`は1個だけ、1:1でdrop。

```culebra
# doctest: skip
mut handles = []
mut sources = []
for w in workers {
  let (tx, rx) = Channel.new()
  handles.push(Isolate.spawn(fn () { produce(w, tx) }))  # producer の tx は終了時 auto-drop
  tx.drop()                                               # 親自身の tx（1:1、自明）
  sources.push(rx)
}
for v in Channel.fan_in(sources) { consume(v) }           # 1 本のストリーム、全 producer
for h in handles { h.join() }                             # handle を保持・エラー回収
```

mergeは渡したreceiverを**引き取り**ます — 元のrxを直接読まず、束ねた`rx`
経由でのみ読んでください（元を読むとmergeと競合）。source間の順序は保たれ
ません（mergeゆえ）。空リストは即closedな`rx`、source 1個はパススルー。

#### `Channel.fan_in(items, fn) -> rx`

オールインワン形：各itemにproducer isolateを起動し`fn(item, tx)`を実行、
出力をmergeします。`fn`は自分の`tx`にsendし、fan_inがchannel作成・親
txのdrop・producer handleの所有をすべて引き受けるので、consumerは **txも
dropもhandleも一切書きません**。`fn`と各itemはSendable必須。

```culebra
# doctest: skip
let merged = Channel.fan_in(workers, fn (w, tx) {
  for x in produce(w) { tx.send(x) }
})
for v in merged { consume(v) }
merged.join()        # producer を join、最初のエラーを再送出
```

`merged.join()`（stream終了後）はproducerをjoinし最初のエラーを再送出。
呼ばなければproducerのエラーは握り潰し（`Isolate.spawn` handleをjoinしない
のと同じ）。producerは専用スレッドで実行されます。

### Parallel — `Parallel.map` / `each` / `map_settled` / `race`

高レベル形。配列の各要素に関数をisolateプールで並列適用します（ハンドル管理
不要）。`fn`と各要素はSendableでなければなりません（`Isolate.spawn`と同じ規則）。

```culebra
# doctest: skip
Parallel.map([1, 2, 3, 4], |x| x * x)         # => [1, 4, 9, 16]  (入力順)
Parallel.map(urls, |u| fetch(u), limit: 8)    # 同時 isolate は最大 8
Parallel.each(jobs, |j| process(j))           # 副作用のみ、nil を返す
Parallel.map_settled(urls, |u| fetch(u))      # => [{ok, value, error}, ...]
Parallel.race(mirrors, |m| download(m))       # 最初の成功が勝つ
```

| 呼び出し | 戻り値 | 備考 |
|---|---|---|
| `Parallel.map(items, fn, limit = <コア数>)` | `Array` | 要素ごと1結果、**入力順**、fail-fast |
| `Parallel.each(items, fn, limit = <コア数>)` | `nil` | 副作用用、結果は集めない、fail-fast |
| `Parallel.map_settled(items, fn, limit = <コア数>)` | `Array` | 要素ごと1 `Result`、入力順、**fail-fastしない** |
| `Parallel.race(items, fn, limit = <コア数>)` | `Any` | **最初に成功**した要素、残りはキャンセル |

`limit`は同時に走るisolate数の上限（既定はコア数）。要素は1つの共有キューから
取り出すので、要素数ぶんでなく合計`limit`個のisolateです。

**`map` / `each`はfail-fast**: 最初に例外を投げた要素で残りを停止し、要素
index付きの`ParallelError`として再送出します（例: `Parallel.map: element[2]
failed: ...`）。

**`map_settled`はfail-fastしない**: 各要素は`Result` Object
`{ok: Bool, value: Any, error: String?}`（`Proc.all`と同じ形）を返すので、1つの
失敗で他の成果を失いません（`r.ok ? r.value : r.error`）。

**`race`は最初の成功**を返し、残りの要素をキャンセルします。*全要素*が例外を
投げたら`ParallelError`、空配列でも`ParallelError`（返す結果が無いため）。

**`on_progress:`で進捗報告。** どのメソッドも`on_progress: |done, total|`
callbackを受け取れます。`fn`と違い **Sendableではありません** — 呼び出し
スレッド上で実行されるので、捕獲した状態（カウンタや進捗バー等）を自由に読み
書きできます。要素が完了するたびに「完了数・総数」で呼ばれ、callbackが例外を
投げると実行はキャンセルされます。

```culebra
# doctest: skip
Parallel.map(urls, |u| fetch(u),
             on_progress: |done, total| IO.print("\r" + done.to_string() + "/" + total.to_string()))
```

(`map_reduce`は予定。)

### Signal — `Signal.notify` / `Signal.reset`

既定ではCtrl+Cは協調的な`Interrupted`をthrowします（言語ガイドの
*割り込み* 節を参照）。長時間動くサービスでは逆に、シグナルを一箇所で受けて
自分の都合でshutdownしたいことが多いです。`Signal.notify(tx)`はCtrl+Cを
*throw* から *配信* に切り替えます — 各押下が実行中コードを中断する代わりに
チャネルの`tx`に`"SIGINT"`を送るので、プログラムは`rx.recv()`（または
`for sig in rx`）でブロックして自前のgraceful shutdownを駆動できます。Goの
`signal.Notify`モデルです。

```culebra
# doctest: skip
let (tx, rx) = Channel.new(1)
Signal.notify(tx)              # Ctrl+C は throw でなくチャネルへ
serve_in_background()
rx.recv()                      # 最初の Ctrl+C までブロック
inspect("shutting down…")
drain_and_close()
```

| 呼び出し | 備考 |
|---|---|
| `Signal.notify(tx)` | Ctrl+Cをこのチャネルの`tx`へ流す（非ブロッキング送信 — バッファ満杯なら超過分はドロップ、Goと同じ）。有効中はthrowを抑制。バッファ付き（`Channel.new(1)`）を使う。 |
| `Signal.reset()` | 既定の`Interrupted` throw動作に戻し、`notify`が保持していたチャネルを解放。 |

`notify`は自分用の送信端をチャネルに保持するので、こちらの`tx.drop()`後も
開いたままになります（`reset()`で解放）。notify有効中は強制終了へのエスカ
レーションはありません（自分でシグナルを扱うと宣言したため。後続のCtrl+Cで
中断させたいなら`reset()`を呼ぶ）。配信はバックグラウンドのポーラ経由なので、
押下は数十ミリ秒以内に観測されます。

### SharedBuffer — zero-copy で共有する固定レイアウトデータ

`SharedBuffer`は固定レイアウトのレコード列を保持し、複数のisolateが
**コピーせず**読み書きできる。isolateモデルで唯一mutableメモリを共
有する場所であり、レコードを固定スカラフィールド（参照やポインタを含
まない）に限定することで安全性を保つ。

レコード型は`@packable`を付けた通常のクラスで、各フィールドは型注釈
と任意のデフォルトを持つ:

```culebra
# doctest: skip
@packable class Vec2 {
  x: Float32 = 0.0
  y: Float32 = 0.0
}
```

`@packable`はバイトレイアウト（C ABI自然アライメント）を確定する。
各フィールドは固定スカラ — `Float32`, `Float64`/`Float`, `Int8`,
`Int16`, `Int32`, `Int64`/`Long`, `Byte`, `Bool` — でなければならず、
非スカラフィールドはロード時に`SyntaxError`。デフォルト省略時は型の
ゼロ値（`0` / `0.0` / `false`）。

#### `SharedBuffer.new(count, Class) -> buffer`

`Class`のレイアウトで`count`個のゼロ初期化レコードを確保する。
`buffer.size`（`.count` / `.len`も同じ）が要素数を返す。バイトはこのプロセス
のヒープに置かれる — isolate（スレッド）間では共有できるが、プロセス間では
共有できない。

#### `SharedBuffer.file(path, count, Class) -> buffer`

同じbufferを、メモリマップしたファイル（POSIX `mmap(MAP_SHARED)`、Windows
`CreateFileMapping`）で裏打ちする。書き込み
はファイルのページに届く — **永続**（ファイルはプロセスより長生き）で、同じ
`path`をマップした別プロセスからも見える。ハンドルに`flush()`（dirtyページを
ディスクへ）が付く。ファイルは普通のファイルなので`FS.remove(path)`
で削除する。`path`をRAM上の場所（Linuxなら`/dev/shm/...`など）に向ければ、
ディスク永続なしの共有メモリになる。

```culebra
# doctest: skip
@packable class Cell { v: Int64 = 0 }
let buf = SharedBuffer.file("/tmp/grid.bin", 100, Cell)
buf[0].v = 42
buf.flush()                   # ディスクへ永続化
```

#### `SharedBuffer.shared(count, Class) -> buffer`

同じbufferを、**匿名の**共有メモリ（名前のないfd — Linuxは`memfd`、macOSは
即unlinkしたPOSIX shmオブジェクト、Windowsはpagefile-backedの
`CreateFileMapping`）で裏打ちする。ディスクにも
触れない。全ハンドルがdropされるとカーネルが解放する。用途は、`Proc.run` /
`Proc.spawn`の`share:`で**子プロセス**へ渡すこと（下記
[プロセス間での共有](#プロセス間での共有zero-copy)）。

#### `buffer[i] -> view`

添字アクセスは要素`i`の **view** を返す（負の添字は末尾から数える）。
`view.field`の読み書きはbacking bytesに直接届く — レコードごとのオブ
ジェクトは生成されない:

```culebra
@packable class Vec2 {
  x: Float32 = 0.0
  y: Float32 = 0.0
}
let buf = SharedBuffer.new(3, Vec2)
inspect(buf.size)                # => 3
buf[0].x = 1.5                # その場でバイトを書く
let v = buf[0]                # 保持した view は同じ要素を指す
v.y = 2.5
inspect([buf[0].x, buf[0].y])    # => [1.5, 2.5]
```

要素まるごとの代入（`buf[i] = ...`）は`TypeError` — レコードは単独の値
形を持たないので、フィールドを個別に設定する。未知のフィールドは
`AttributeError`、範囲外の添字は`IndexError`。

#### isolate 間での共有（zero copy）

bufferはisolate境界を **参照で**越える — 子は同じバイトを読み書きす
る。他のすべての値（境界でコピーされる）と異なり、SharedBufferは共有
される（channelと同じ例外）。これにより、ワーカーがdisjointな要素を
更新する典型的なデータ並列パターンがアロケーションなしで書ける:

```culebra
# doctest: skip
@packable class Cell { v: Int64 = 0 }
let cells = SharedBuffer.new(8, Cell)

Parallel.each([0, 1, 2, 3, 4, 5, 6, 7], fn (i) { cells[i].v = i * i })

# cells は 0, 1, 4, 9, 16, 25, 36, 49 を保持
```

**disjointな**要素を書くワーカーは同期不要。同じ要素を複数のisolate
が同時に書くのはデータ競合であり、作業を分割して（上記のように）各要素
のwriterを1つにするか、共有更新を [`with_lock`](#bufferwith_lockfn-で同期する)
で守るのは呼び出し側の責任。

#### プロセス間での共有（zero copy）

`SharedBuffer.shared(...)`のbufferは、別のisolateだけでなく**子プロセス**へ
も渡せる。親は`Proc.run`（または`Proc.spawn`）の`share:`キーワード（`名前 ->
buffer`のObject）で渡し、子はその名前で`SharedBuffer.receive(name, Class)`に
よって再アタッチする。両プロセスが同じ物理ページをマップする — コピーや
シリアライズなしで書き込みが見える。

```culebra
# doctest: skip
# --- parent.cul ---
@packable class Cell { v: Int64 = 0 }
let grid = SharedBuffer.shared(4, Cell)
grid[0].v = 100
Proc.run([Sys.executable, "worker.cul"], share: {grid: grid})
inspect(grid[0].v)               # 子の書き込みをここで読み戻す
grid.drop()
```

```culebra
# doctest: skip
# --- worker.cul ---
@packable class Cell { v: Int64 = 0 }
let grid = SharedBuffer.receive("grid", Cell)
for i in 0..grid.count { grid[i].v = grid[i].v + (i + 1) * 10 }
grid.drop()
```

`receive`は名前と`@packable`型だけを取る — 要素数は親由来（なので
`grid.count`が一致する）。子は同じ`@packable`クラスを宣言し、`receive`は
レコードサイズの一致を確認して、レイアウト不一致・未知の名前・
`SharedBuffer.shared(...)`でないbufferのときは`ValueError`を投げる（ヒープと
ファイルのbufferはこの方法では渡せない — ファイルbufferは`path`を開き直して
共有する）。`Sys.executable`は実行中のculebraバイナリのパスで、インタプリタの
ワーカーコピーを起動するのに使う。

`Proc.run`は子の終了までブロックするので、戻った時点で子の書き込みは完了して
いる。並行な子は`Proc.spawn`してそれぞれ`wait()`する。isolateと同様、各子が
**disjointな**要素を持つようにして書き込みが競合しないようにする。

#### `buffer.with_lock(fn)` で同期する

disjointな書き込みは同期不要。2つの書き手が本当に**同じ**データに触れざるを
得ないとき — カウンタや、一貫性を保ちたい複数フィールドの更新など — に
`with_lock`がescape hatchになる。callbackをbufferのロックを保持したまま
実行し、callbackの戻り値を返す。ロックは例外送出を含むあらゆる脱出経路で
解放される。

```culebra
# doctest: skip
@packable class Counter { n: Int64 = 0 }
let tally = SharedBuffer.new(1, Counter)

Parallel.each(iota(0, 8), fn (w) {
  for _ in 0..1000 {
    tally.with_lock(fn () { tally[0].n = tally[0].n + 1 })
  }
})
inspect(tally[0].n)              # ちょうど 8000 — lost update なし
```

同じ呼び出しが**プロセス間**でも効く。`.shared` / `.file`のbufferは
プロセス共有ロックを持つので、`share:`で渡された子（あるいは同じファイル
`path`を再オープンした別プロセス）どうしが排他される。callbackは短く保つ
こと — すべてのholderを直列化する。ロックは**再入不可**で、callbackの中から
同じbufferの`with_lock`を再度呼ぶとデッドロックする。引数が関数でなければ
`TypeError`、drop済みのbufferは`ValueError`。

`.file`のbufferはこのロック用に先頭へ小さな固定ヘッダを確保するので、その
バイト列は素のレコード配列ではなくculebraのコンテナ形式になる — 外部ツールが
ファイルを直接読む場合は注意。

#### 可変個数フィールド: `FixedArray<T, N>`

`@packable`フィールドには`FixedArray<T, N>`を使える — スカラ`T`を
**容量** `N`個まで保持する固定容量のインラインコレクションで、**個数**は
実行時に可変。完全インライン展開（`[len][T × N]`、ポインタなし）なので、
可変個数のデータも共有レコードに載る（VARCHAR(N) / 固定長配列の手法）。

```culebra
# doctest: skip
@packable class Body {
  mass: Float32 = 0.0
  trail: FixedArray<Float32, 8>   # 最大 8 点、初期は空
}

let bodies = SharedBuffer.new(100, Body)
let b = bodies[0]
b.trail.push(1.5)
b.trail.push(2.5)
b.trail.size()        # => 2   (capacity() => 8)
b.trail[0]            # => 1.5
b.trail[1] = 9.0
for p in b.trail { ... }
```

viewは`.size()` / `.capacity()` / `.push(v)` / `.get(i)` / `.set(i, v)` /
`arr[i]`（読み書き）/ `for x in arr`をサポート。容量超過の`push`と範囲外
の添字は`IndexError`。要素型は固定スカラに限る。フィールドまるごとの代入
（`record.field = ...`）は`TypeError` — view経由で変更する。viewはレコード
のバイトをその場で読み書きするので、bufferとともにisolate間で共有される。

#### テキストフィールド: `FixedString<N>`

`@packable`フィールドには`FixedString<N>`を使える — 最大`N`バイトの
UTF-8文字列を保持する固定容量インライン文字列（`[len][byte × N]`、ポインタ
なし）。`FixedArray`と違い、**まるごと`String`値として**読み書きする
（VARCHAR(N) の手法）:

```culebra
# doctest: skip
@packable class Row {
  id: Int32
  name: FixedString<16>
}

let rows = SharedBuffer.new(100, Row)
rows[0].name = "alice"     # まるごと書き込み（≤ 16 バイト）
rows[0].name               # => "alice"   （本物の String）
rows[0].name.upper()       # => "ALICE"   （String の全メソッドが効く）
rows[1].name               # => ""        （ゼロ値は空文字列）
```

`N`は**バイト**容量。`N`バイトを超える文字列は`CapacityError`、String以外
の代入は`TypeError`。読み出しは格納バイトの新しい`String`コピーを返すので、
bufferとともにisolate間で共有される（子isolateの書き込みが親の読み出しに
見える）。

#### ハッシュコレクション: `FixedSet<T, N>` / `FixedMap<K, V, N>`

`@packable`フィールドには`FixedSet<T, N>`（最大`N`個のスカラ値）や
`FixedMap<K, V, N>`（最大`N`組のスカラkey→value）も使える。どちらも完全
インライン展開（`[count][states][entries]`、ポインタなし）のopen-addressing
ハッシュテーブルで、view経由でその場変更する:

```culebra
# doctest: skip
@packable class Bag {
  tags:   FixedSet<Int32, 16>
  counts: FixedMap<Int32, Int32, 16>
}

let b = SharedBuffer.new(100, Bag)
let s = b[0].tags
s.add(5); s.add(7); s.add(5)   # 重複は no-op
s.size()                       # => 2   (capacity() => 16)
s.contains(7)                  # => true
s.remove(7)                    # => true（無ければ false）
for x in s { ... }

let m = b[0].counts
m.set(42, 1)
m.get(42)                      # => 1   （無ければ nil）
m.contains(42)                 # => true
m.set(42, 2)                   # 上書き。size() は不変
m.remove(42)                   # => true
m.keys()                       # => [Int32, ...]
for k, v in m { ... }          # (key, value) タプルを yield
```

容量超過の`add` / `set`は`CapacityError`。キー/値型は固定スカラに限り、等価は
スカラのバイト比較（`FixedSet<Float32>`は`0.0`と`-0.0`を別物とみなす）。
フィールドまるごとの代入は`TypeError` — view経由で変更する。バイトはレコード内
にあるので、bufferとともにisolate間で共有される。

#### Optional フィールド: `T?`

`@packable`フィールドにはoptionalスカラ`T?`も使える — 値または`nil`を持つ
スロットで、`[present:byte][T]`のレイアウト。まるごと値として読み書きし、present
バイトが0なら`nil`、そうでなければスカラ:

```culebra
# doctest: skip
@packable class Node {
  id:     Int32
  parent: Int32?      # 疎な「親なし」スロット
}

let n = SharedBuffer.new(100, Node)
n[0].parent            # => nil   （ゼロ値）
n[0].parent = 5
n[0].parent            # => 5
n[0].parent = nil      # クリア
n[0].parent ?? -1      # => -1
```

`0`は実値で`nil`とは別。packableなのはスカラoptionalのみ（`T`は固定スカラ）。
疎構造（id→optionalスロットの配列）が主用途で、tagged payloadはpackable enumと
組み合わせる。

#### タグ付き共用体: `@packable enum`

`@packable` enumは固定のタグ付き共用体`[tag:i32][payload]`: 各variantのスカラ
payloadが最大variantに合わせた1領域を共有し、tagがどのvariantがliveかを選ぶ。
`@packable`クラスのフィールドにそのenum型を使える。component種別・メッセージ型など、
共有レコード内の判別付きpayloadに使う:

```culebra
# doctest: skip
@packable enum Shape {
  Circle(Float32),
  Rect(Float32, Float32),
  Point
}

@packable class Obj {
  id:    Int32
  shape: Shape
}

let objs = SharedBuffer.new(100, Obj)
objs[0].shape = Shape.Rect(2.0, 3.0)   # variant 値を書く
match objs[0].shape {                  # 読み戻して match
  Rect(w, h) => w * h,
  Circle(r)  => 3.14 * r * r,
  Point      => 0.0
}
```

variant payloadは全て固定スカラに限る（非スカラpayloadはvariant位置で
`SyntaxError`）。そのenumのインスタンスでない値の書き込みは`TypeError`。読み出しは
バイトからvariantインスタンスを再構築する（enum namespace不要）ので、あるisolateが
書いた値を別のisolateが共有buffer越しに読める。

#### 生バイト: `Bytes<N>`

`@packable`フィールドには`Bytes<N>`も使える — 長さprefixなしの**ちょうど** `N`
バイトをインライン保持し、まるごとbyte `String`として読み書きする。ハッシュ・UUID・
固定バイナリblob用:

```culebra
# doctest: skip
@packable class Entry {
  id:     Int32
  digest: Bytes<32>      # 例: SHA-256
}

let e = SharedBuffer.new(100, Entry)
e[0].digest = some_32_byte_string
e[0].digest                       # => 32 バイト（バイナリ安全）
```

書き込む`String`は**ちょうど** `N`バイトでなければならない（違えば`ValueError`）。
String以外は`TypeError`。バイトはバイナリ安全（埋め込みNULも保持）。`FixedString<N>`
（長さprefix付きの可変長テキスト）と違い、`Bytes<N>`は固定長blob。

#### ネストレコード: `@packable` クラスのフィールド

`@packable`クラスのフィールドに別の`@packable`クラスを使える — そのレコードが
インラインで格納され、`outer.inner`がそのバイトへのviewを返すので、ネストフィールド
への代入はその場に書き込まれる:

```culebra
# doctest: skip
@packable class Point { x: Float32  y: Float32 }
@packable class Line  { id: Int32   start: Point  end: Point }

let lines = SharedBuffer.new(100, Line)
lines[0].start.x = 1.0        # インライン Point のバイトに書く
lines[0].start.y = 2.0
lines[0].start.x             # => 1.0
lines[0].end = lines[0].start # サブレコードまるごとコピー（memcpy）
```

ネストするクラスは、それを含むクラスより前に（`@packable`で）宣言する必要がある。
サブレコードまるごとの代入は**同じ**クラスの別レコードのバイトをコピーする（違えば
`TypeError`）。個別フィールドの設定はview経由（`outer.inner.field = v`）。

### Shared — 参照共有する immutable 値

`Shared.new(value)`は普通の値（オブジェクト・配列・タプル・セット・スカラの
任意のネスト）を**凍結**し、すべてのisolateがコピーなしで使える
**読み取り専用view** を返す。可変長の読み取り専用データ
（トークナイザ辞書、パース済み設定、検索インデックス）のためのレーン:
channelレーンはタスクごとにコピーし、`SharedBuffer`は固定レイアウトを
要求する。凍結ツリーは1つ、読み手は何個でも:

```culebra
# doctest: skip
let dict = Shared.new(JSON.parse(FS.read("vocab.json")))

let workers = [0, 1, 2, 3].map(|i| Isolate.spawn(fn () {
  dict["hello"]          # 全 isolate が同じ凍結ツリーを読む
}))
```

読みは普通のコレクションアクセスと同じ — `view.field`・`view[key]`・
`view[i]`・`view.size()`・`view.has(k)`・`view.keys()` / `view.values()`・
`for ... in view`（Object viewは`(key, value)`ペア、Array/Tuple/Set
viewは要素を返す）。スカラフィールドは読み手のheapに材料化され、
コンテナフィールドは別の共有viewとして返る（コピーなし）。ローカルの
作業コピーが要る時は`view.copy()`で普通の可変値へ深い材料化をする。

凍結はisolateへ値を送るのと同じwalkなので、Sendableなものは凍結
できる — 追加の拒否が2つ: **関数**（`Shared`の値はデータのみ）と
**nativeハンドル**（channel・buffer・別の`Shared` view）は`SendError`。
すべての書き込みは`ImmutableError`。更新は構造上copy-on-write
（新しい値を作って再度`Shared.new`し、新viewを配る）。`view.drop()`
は参照を解放（冪等）。ツリー本体はどこかの最後のviewがdropした時に
解放され、以降の読みは`ClosedError`。

| | レーン | 共有 | 読み手ごとのコピー |
|---|---|---|---|
| 固定レイアウトレコード | `SharedBuffer` | read **+ write** | なし |
| 可変長・読み取り専用 | `Shared` | read | なし |
| 任意・可変 | channel | コピー | あり |

---

## 13. Matchers

テスト用 / 実行時不変条件チェック用のアサーションmatcher。全10
個のmatcherが`import`不要のグローバル名としてすべての環境に
bindされています。失敗時は`{kind: "AssertionError", message: ...}`
形のculebra Objectをthrow — `try/catch`で捕捉可能。

`assert`キーワード / builtinは存在しません — 用途ごとに専用matcher
を使います。productionの不変条件には`if`/`throw`を直接書きます:

```culebra
# doctest: skip
if (!cond) {
  throw {kind: "AssertionError", message: "invariant violated"}
}
```

### 真偽 matcher

* **`assert_true(x: Bool) -> Nil`** — `x`がtruthyならpass。失敗
  時は`assert_true failed:\n  value: {x}`。`x`は`Bool` / `Long` /
  `Float`のみ — それ以外は`TypeError`。暗黙のtruthinessは無く、
  空文字列・空配列はfalsyではありません。
* **`assert_false(x: Bool) -> Nil`** — `assert_true`の逆。

### 比較 matcher

各比較matcherは **同名の演算子と同じdispatch** を行います —
`assert_eq(a, b)`は`a == b`と等価で、クラスインスタンスの
`__eq__` / `__lt__` / `__le__`が尊重されます。失敗messageは
`to_string`で両辺を表示 (ユーザ`__str__`を尊重)。

* **`assert_eq(a, b) -> Nil`** — `a == b`。
* **`assert_ne(a, b) -> Nil`** — `a != b`。
* **`assert_lt(a, b) -> Nil`** — `a < b`。
* **`assert_le(a, b) -> Nil`** — `a <= b`。
* **`assert_gt(a, b) -> Nil`** — `a > b`。
* **`assert_ge(a, b) -> Nil`** — `a >= b`。

```culebra
assert_eq(1 + 1, 2)                                # 成功時は無音

let r = try { assert_eq("foo", "bar"); nil } catch e { e }
inspect(r.kind)         # => 'AssertionError'
inspect(r.message)
# => |
# 'assert_eq failed:
#   left:  foo
#   right: bar'
```

### `assert_throws(kind: String, f: Function) -> Nil`

0引数`f()`を呼び、`kind`に一致する`kind`を持つエラーがthrow
されることを表明。組み込みエラー (`ZeroDivisionError`, `TypeError`
等) は`e.kind`を持ち、ユーザ`throw {kind: "X", ...}`も同じく
照合。`f`の引数数が0以外なら`ArityError`。

```culebra
assert_throws("ZeroDivisionError", fn() { let _ = 1 / 0 })
assert_throws("MyError", fn() {
  throw {kind: "MyError", message: "boom"}
})
```

### `assert_close(a: Float, b: Float, tol: Float) -> Nil`

`|a - b| <= tol`ならpass。`a` / `b` / `tol`のいずれかがNaNなら
**故意に失敗** (素朴な`diff > tol`だとNaNがsilently passする
ため)。浮動小数比較は`assert_eq`ではなくこちらを使う。

```culebra
assert_close(3.14, 3.1415, 0.01)
```

### 実装ノート

matcher一族はculebraソース (cppではなく) で定義されており、
lazy module機構で3 backend (interp / JIT / AOT) に共通でbind
されます。matcher内部の`==` / `<`等の演算子dispatchは各
backendが既に実装している演算子dispatchそのもので、matcher専用
のdrift防止ロジックは不要です。

---

## 14. `Regex`

線形時間・grapheme単位の正規表現（エンジン: vendor化した [cpp-regexlib](https://github.com/yhirose/cpp-regexlib)）。パターンは
Unicodeの **extended grapheme cluster** 単位でマッチし、コードポイント単位では
ありません — `.`は1つのユーザー知覚文字を消費します（`/./`が`🇯🇵`に1要素として
マッチ）。マッチは**線形時間**（Thompson NFA / Pike VM + lazy DFA fast path）で、
catastrophic backtrackingが原理的に起きないためbackreferenceはありません。
オフセットは**バイトオフセット**（Go流）で常にgrapheme境界上です。

`Regex`は **`Regex.compile`で一度コンパイルして再利用**します（コンパイル済み
プログラムが高コスト部分）。以降はメソッドで問い合わせます:

**パターンはシングルクォートのraw文字列で書きます**（`'\d+'`、`"\\d+"`ではなく）:
シングルクォートはエスケープ処理も`{...}`補間も行わないので`\d`や`{n}`がそのまま
通ります。アポストロフィを含むパターン（トークナイザの
`'s`/`'t`など）はバッククォートraw文字列`` `...` ``を使います（同じくrawで、
`'`・`"`・`{`も含められる）。フラグは`compile`に文字列で渡す
（`Regex.compile('hello', "i")`）か、パターン内にインライン: `(?i)`大文字小文字
無視、`(?m)`複数行、`(?s)` dotall。

定数パターンには [`re"..."`リテラル](language.ja.md#regex-リテラル)が
`Regex.compile(...)`の短縮形として使えます — `re'\d+'`や`re"hello"i`は
同じコンパイル済み`Regex`で、本体は常にraw、フラグは閉じクォートの直後。
リテラルは`${expr}`補間も使えます（Stringはエスケープ、`Regex`は合成
— 下記`Regex.interp`参照）。**全体**を実行時に組み立てるパターンは
`Regex.compile(...)`を直接使ってください。

パターンと対象文字列はどちらの文字列型（`String` / `StringView`）も受け付ける
ので、`String.split` / `.slice`が返す`StringView`をそのまま渡せます:
`Regex.compile('\d+').find_all(line.slice(0, 80))`。

| コンストラクタ / 静的 | 結果 |
| --- | --- |
| `Regex.compile(pat)` | `Regex` — コンパイル（再利用）。不正パターンは送出 |
| `Regex.compile(pat, flags)` | `Regex` — `flags`は`"i"` / `"m"` / `"s"`の文字列 |
| `Regex.escape(s)` | `String` — メタ文字を全てバックスラッシュエスケープし`s`をリテラル一致に |
| `Regex.interp(x)` | `String` — `re"...${x}..."`用の合成ヘルパ: `Regex` → `(?:src)`、それ以外 → エスケープしてリテラル一致 |

その場限りの利用には、下のnamespaceメソッドがパターンを直接受け取り`compile`を隠します
。1つのパターンを多数の入力に再利用するなら
`Regex.compile(pat)`を使いますが、エンジンがパターンでキャッシュするのでone-shot形に再コンパイルの
コストはありません。フラグはインライン（`(?i)` / `(?m)` / `(?s)`）で。

| one-shot | 等価 |
| --- | --- |
| `Regex.find(pat, s)` | `Regex.compile(pat).find(s)` — `Match`または`nil` |
| `Regex.match(pat, s)` | 先頭アンカーのマッチ |
| `Regex.find_all(pat, s)` | `[Match]` |
| `Regex.test(pat, s)` | `Bool` |
| `Regex.split(pat, s)` | `[String]` |
| `Regex.replace_all(pat, s, repl)` | `String` — テンプレートまたは`fn (Match) -> String`のrepl、全マッチ置換 |
| `Regex.replace_first(pat, s, repl)` | `String` — replは同じ、最左マッチのみ置換 |

```culebra
inspect(Regex.find('(\d+)', "ab12")[1])            # => '12'
inspect(Regex.test('(?i)hello', "HELLO"))          # => true
inspect(Regex.replace_all('[;；]', "a;b；c", "、"))  # => 'a、b、c'
# ミスは nil なので `?.` / `??` と合成できます:
inspect(Regex.find('x', "y")?.value ?? "none")     # => 'none'
```

| メソッド | 結果 |
| --- | --- |
| `re.test(s)` | `Bool` — `s`のどこかにマッチするか |
| `re.find(s)` | `Match`または`nil` — 最左マッチ |
| `re.match(s)` | `Match`または`nil` — 先頭anchoredマッチ |
| `re.find_all(s)` | `[Match]` — 全ての非重複マッチ |
| `re.find_all_str(s)` | `[String]` — マッチ文字列のみ（`Match`を作らない。match-denseで約12倍速） |
| `re.find_all_index(s)` | `[Int]` — flatなバイトspan `[s0, e0, s1, e1, …]`（位置のみ・確保は配列1個） |
| `re.count(s)` | `Int` — 非重複マッチ数（オブジェクト確保なし） |
| `re.find_iter(s)` | `Iterator<Match>` — 遅延。途中終了可（`.take(n)`） |
| `re.replace_all(s, repl)` | `String` — `repl`はテンプレート（`$1` / `$<name>` / `$$`）**または** `fn (Match) -> String`、全マッチ置換 |
| `re.replace_first(s, repl)` | `String` — `repl`の文法は同じ、最左マッチのみ置換。マッチなしなら`s`をそのまま返す |
| `re.split(s)` | `[String]` — マッチで`s`を分割 |

**bulk APIの選び方。** `find_all`はマッチごとに完全な`Match`
オブジェクト（テキスト・span・`groups`・`named`）を構築する。match-dense
な入力では、マッチングそのものより**このオブジェクト構築が支配的**になり、
エンジンの生スキャンの数十倍のコストになる。マッチごとのcaptureが不要なら
leanな変種を使う: 個数だけなら`count`、byte spanなら`find_all_index`、
マッチ文字列なら`find_all_str`、途中で止めるなら`find_iter`。
`groups`/`named`をマッチごとに実際に使うときだけ`find_all`を使う。

`Match`はデータオブジェクト（`nil`はマッチなし）:

| フィールド | 意味 |
| --- | --- |
| `m.value` | マッチ全体の文字列（`String`） |
| `m.start`, `m.end` | バイトオフセット |
| `m.groups` | `[Group \| nil]`; `groups[0]`はマッチ全体 |
| `m.named` | `{name: Group}` — 名前付きキャプチャ |

添字はキャプチャ専用アクセサです。`m[i]`は位置グループ`i`の文字列（`m[0]`は
マッチ全体、負数は配列同様にラップ）、`m["name"]`は名前付きグループの文字列を返します。
ミス（範囲外・未マッチの省略可能グループ・無い名前）はすべて`nil`なので`?? ""`と
合成できます。添字はキャプチャだけに届きレコードのフィールドには届かないため、マッチ全体は
`m.value`か`m[0]`（`m["value"]`ではない）。spanが要る時はdotフィールド
（`m.groups[i].start`）を使います。

`Group`は`.value` / `.start` / `.end`を持ちます。不正なパターンは`RegexError`を送出。

```culebra
let d = Regex.compile('\d+')
inspect(d.test("abc 123"))                                # => true
inspect(Regex.compile('\w+').find("  hello world").value) # => 'hello'
inspect(d.find("no digits"))                              # => nil
inspect(d.find_all("a1 b22 c333").size())                 # => 3
```

キャプチャは位置（`m[1]`）でも名前（`m["year"]`）でも取れます。`m[0]`は
マッチ全体、ミスは`nil`。`m.groups` / `m.named`の`Group`オブジェクトは
文字列に加えてspanも持ちます:

```culebra
let m = Regex.compile('(?<year>\d{4})-(\d{2})').find("2026-05")
inspect(m[1])                    # => '2026'
inspect(m["year"])               # => '2026'
inspect(m[0])                    # => '2026-05'
inspect(m[9] ?? "none")          # => 'none'
inspect(m.groups[1].value)       # => '2026'
inspect(m.named["year"].value)   # => '2026'
```

置換と分割 — 置換文字列は`$n`テンプレートか、`Match`を受け取る関数です。
`replace_all`は全マッチを置換し、`replace_first`は最左マッチだけを置換して
残りはそのまま残します（マッチが無ければ`s`をそのまま返すno-op）:

```culebra
let d = Regex.compile('\d+')
inspect(d.replace_all("a1 b22 c333", "#"))                        # => 'a# b# c#'
inspect(d.replace_first("a1 b22 c333", "#"))                      # => 'a# b22 c333'
inspect(Regex.compile('(\w+)@(\w+)').replace_all("x@y", '$2.$1')) # => 'y.x'
inspect(d.replace_all("a1 b22", fn (m) { "<{m.value}>" }))        # => 'a<1> b<22>'
inspect(Regex.compile('\s+').split("the quick  brown"))           # => ['the', 'quick', 'brown']
inspect(Regex.compile('hello', "i").test("HELLO world"))          # => true
```

`find_iter`は遅延なので走査を途中で止められます。
`for m in d.find_iter(s) { break }`はbreakしたマッチより先には進みません:

```culebra
let d = Regex.compile('\d+')
inspect(d.find_iter("1 2 3").take(2).collect().size())   # => 2
inspect(Regex.escape("a.b(c)"))                          # => 'a\.b\(c\)'
```

対応構文（literal / `.` / 文字クラス / `* + ? {n,m}` greedy・lazy / `|` /
キャプチャ・名前付きグループ / `\d \w \s \b` / lookahead / 可変長lookbehind /
`\p{…}` Unicodeプロパティ）とマッチモデル・資源上限は、vendor化したエンジン
[cpp-regexlib](https://github.com/yhirose/cpp-regexlib)（`vendor/cpp-regexlib`）に
記載しています。

---

## 15. `Http`

同期HTTP/HTTPSクライアント（エンジン: vendorの`cpp-httplib` + OpenSSLを
静的リンク）。各呼び出しはレスポンスが返るまで **blocking** で、async/awaitは
ありません。この名前空間は提供側も兼ねていて、後述の`Http.server()`が
ルーティング・静的ファイル・WebSocketを担い、`Http.sse` / `Http.ws`が
クライアントとして2つのストリーミングプロトコルを話します。
`https://` URLではTLSが自動で有効になり、サーバ証明書の検証には
システムの信頼ストア（macOSはkeychain、LinuxはプラットフォームのCAバンドル）
を使います。`gzip` / `deflate`のレスポンスは透過的に展開され、`body`は常に
デコード済みの内容です。

各メソッドは **レスポンスObject** を返し、例外を投げるのは *トランスポート* 失敗の
ときだけです:

| フィールド | 型 | 意味 |
|---|---|---|
| `status` | `Long` | HTTPステータスコード（`200`、`404` …） |
| `ok` | `Bool` | `status`が`[200, 300)`の範囲なら`true` |
| `reason` | `String` | ステータス文言（`"OK"`、`"Not Found"` …） |
| `body` | `String` | レスポンスボディ（生バイト列） |
| `headers` | `Object` | レスポンスヘッダ（名前→値、String→String） |
| `json()` | `Any` | `body`をJSONとしてパース（`JSON.parse(r.body)`の糖衣） |

**4xx/5xxは通常の結果**（`ok: false`）でありエラーではありません — `status` /
`ok`で分岐してください。**トランスポート失敗**（DNS・接続拒否・TLSハンドシェイク・
タイムアウト）は`HttpError`を投げます。スキーム/ホストの無い不正なURLも
`HttpError`、不正な`headers`値は`TypeError`を投げます。

| メソッド | 結果 |
| --- | --- |
| `Http.get(url, headers=nil, timeout=0, follow_redirects=true)` | レスポンスObject |
| `Http.delete(url, headers=nil, timeout=0, follow_redirects=true)` | レスポンスObject |
| `Http.head(url, headers=nil, timeout=0, follow_redirects=true)` | レスポンスObject |
| `Http.post(url, body="", content_type="text/plain", headers=nil, timeout=0, follow_redirects=true)` | レスポンスObject |
| `Http.put(url, body="", content_type="text/plain", headers=nil, timeout=0, follow_redirects=true)` | レスポンスObject |
| `Http.request(method, url, body="", content_type="text/plain", headers=nil, timeout=0, follow_redirects=true)` | レスポンスObject — 任意のメソッド（PATCH、OPTIONS …） |
| `Http.sse(url, on_event, headers=nil, timeout=0, follow_redirects=true)` | レスポンスObject — Server-Sent Eventsを`on_event`にストリーム（後述） |
| `Http.client(base_url, headers=nil, timeout=0, follow_redirects=true)` | 永続クライアントハンドル（ベースURL + デフォルトヘッダ + 接続再利用、後述） |
| `Http.server()` | HTTPサーバハンドル（ルート + `static` + `ws`を登録して`listen`、後述） |
| `Http.ws(url)` | WebSocketクライアント接続。ハンドル（`send`/`receive`/`for`/`close`/`is_open`）を返す（後述） |

キーワード引数（全メソッド共通）:

- `headers: Object` — リクエストヘッダ。値がすべて`String`の`Object`。
  非`String`値は`TypeError`（デフォルト: なし）。
- `params: Object` — クエリ文字列パラメータ。`String`値の`Object`で、URLに
  percent-encodeして付与（`?k=v&…`、URLに既存のクエリがあれば保持）。非`String`
  値は`TypeError`（デフォルト: なし）。
- `timeout: Long` — connect / read / writeの各フェーズのタイムアウト（**秒**）。
  `0`はライブラリのデフォルト（デフォルト: `0`）。
- `follow_redirects: Bool` — `3xx`の`Location`を追跡する（デフォルト: `true`）。
- `into: String | Function` — レスポンスボディをバッファせずsinkへストリーム
  する（下記ストリーミング参照。デフォルト: `nil`＝`body`にバッファ）。
- `json: Any`（`post` / `put` / `request`のみ）— 値をJSONにシリアライズし、
  `Content-Type: application/json`でボディとして送信。
- `form: Object`（`post` / `put` / `request`のみ）— `String`値の`Object`を
  `application/x-www-form-urlencoded`ボディとして送信（percent-encode）。
- `files: Object`（`post` / `put` / `request`のみ）— `multipart/form-data`ボディ
  （テキストフィールド＋ファイルpart、ストリーミング対応）を送信。下記Multipart参照。
  `body` / `json` / `form` / `files`は最大1つ（複数指定は`TypeError`）。
- `body: String | Function` / `content_type: String`（`post` / `put` /
  `request`のみ）— リクエストボディとその`Content-Type`（bodyが非空で、かつ
  `headers`で明示的な`Content-Type`が指定されていない場合のみ付与）。`String`
  は全体を送信、`Function`は **producer** としてchunkedストリーム（下記参照）。

```culebra
# doctest: skip
let r = Http.get("https://api.example.com/users", params: {page: "2"})
if r.ok {
  let users = r.json()                 # レスポンスボディを JSON としてパース
  IO.inspect(users.size().to_string())
} else {
  IO.inspect("request failed: {r.status}")
}

# ヘッダとタイムアウトを指定して JSON を POST（`json:` が serialize + Content-Type 設定）。
let resp = Http.post("https://api.example.com/users",
                     json: {name: "alice"},
                     headers: {Authorization: "Bearer " + token},
                     timeout: 30)
assert_true(resp.ok)

# トランスポート失敗は throw するが、404 は throw しない。
let missing = Http.get("https://api.example.com/nope")
assert_eq(missing.ok, false)        # 404 は通常の結果
assert_eq(missing.status, 404)
```

`get`/`post`等はボディ全体を単一の`String`（メモリに読み込み）で返すので、
JSON APIでは [`JSON.parse`](#9-json) と組み合わせます。

**ストリーミング（ダウンロード）— `into:`引数。** メモリに載らない大きな
レスポンスは、`into:`を渡してバッファせずsinkへストリームします。任意のメソッドで
使え、返る`body`は空になります（バイトはsinkへ流れる）。`into:`が受ける型:

* **`String`** — ファイルパス。ボディをそのファイルへ直接書き出します。
* **`Function`** — `|chunk|`クロージャ。到着した各chunkで呼ばれます。コールバックは
  呼び出しスレッド上で実行されるので捕捉した状態を自由に読み書きでき、throwすれば
  転送は中断されエラーが伝播します。

```culebra
# doctest: skip
Http.get("https://example.com/big.tar.gz", into: "big.tar.gz")   # → ファイル

mut bytes = 0
Http.get("https://example.com/big.csv", into: fn (chunk) { bytes += chunk.size() })

# 任意のメソッド。例: レスポンスがストリームで返る POST:
Http.post("https://example.com/query", body: q, into: fn (chunk) { handle(chunk) })
```

**ストリーミング（アップロード）。** 対称に、`body:`（`post` / `put` /
`request`）へ`Function`を渡すとリクエストボディをchunkedでストリームします
（大きなアップロードもメモリに全部載せない）。producerは繰り返し呼ばれ、次の
chunk `String`を返し、`nil`でストリーム終端を示します:

```culebra
# doctest: skip
let f = File.open("big.bin")
Http.post(url, body: fn () {
  let chunk = f.read(65536)
  !chunk.empty() ? chunk : nil            # nil で終端
}, content_type: "application/octet-stream")
```

producerは呼び出しスレッド上で実行され（捕捉状態をmutableに扱える）、throwすれば
アップロードは中断されエラーが伝播します。`String`/`nil`以外を返すと`TypeError`。

**Multipartアップロード — `files:`引数。** `files:`（`post` / `put` /
`request`）を渡すと`multipart/form-data`ボディを送信します（boundary付きの
`Content-Type`は自動設定）。`files:`は各エントリが1つのpartとなる`Object`で、
キーがフィールド名です。partの値は:

* **`String`** — 素のテキストフィールド。
* **`Object`** — ボディソースを正確に1つ持つ: `content:`（メモリ上の`String`）/
  `path:`（`String`のファイルパス、ディスクからストリーム）/ `stream:`（producer
  `Function`、ストリーム）。加えて任意の`filename:` / `content_type:`。`path:`の
  partは`filename`をファイルのベース名で既定化します。
* **`Array`** — 上記いずれかの配列。同一フィールド名で複数partを送る（例: 1つの
  `photos`フィールドに複数ファイル）。

`path:` / `stream:`のpartはchunkedでストリームされるので、大きなファイルや
生成に時間がかかるpartもメモリに全部載せません。`stream:` producerはストリーミング
`body:`と同じ規約（次のchunk `String`を返し、`nil`でpart終端）です。

```culebra
# doctest: skip
# テキストフィールド + メモリ上のファイル part
Http.post(url, files: {
  title: "My report",
  doc:   { content: "a,b,c\n1,2,3\n", filename: "data.csv", content_type: "text/csv" },
})

# 大きなファイルをディスクから直接ストリーム（全体をバッファしない）
Http.post(url, files: { clip: { path: "/movies/big.mp4", content_type: "video/mp4" } })

# 生成に時間がかかる part を producer からストリーム
mut row = 0
Http.post(url, files: {
  export: { filename: "rows.csv", content_type: "text/csv", stream: fn () {
    row += 1
    row <= 1000 ? "{row},{compute(row)}\n" : nil
  } },
})

# Array で同一フィールド名に複数 part
Http.post(url, files: {
  caption: "trip",
  photos:  [ { path: "./1.jpg" }, { path: "./2.jpg" } ],
})
```

partの値が`String` / `Object` / `Array`以外、`Object`が`content` / `path` /
`stream`を正確に1つ持たない、`content` / `path`が非`String`、`stream`が非
`Function`、はいずれも`TypeError`。開けない`path`は`IOError`です。

### `Http.sse(url, on_event, headers=nil, timeout=0, follow_redirects=true) -> Object`

[Server-Sent Events](https://developer.mozilla.org/docs/Web/API/Server-sent_events)
（`text/event-stream`）ストリームを開きます — 長寿命の`GET`で、イベントが届くたびに
`on_event`コールバックを1回ずつ呼びます。ストリーミングLLM/チャットAPIが使う
ワイヤ形式です。呼び出しはストリームが続く間blockingで、サーバが閉じた後に最終的な
レスポンスObjectを返します。

各イベントは3つのStringフィールドを持つObjectです:

| フィールド | 意味 |
|---------|---------|
| `event` | `event:`タイプ。サーバが送らない場合は`"message"` |
| `data`  | `data:`ペイロード。複数の`data:`行は`\n`で連結 |
| `id`    | 最後に見た`id:`フィールド。無ければ`""` |

```culebra
# doctest: skip
Http.sse("https://api.example/v1/stream", fn (e) {
  if e.data == "[DONE]" { return }
  let delta = JSON.parse(e.data)
  IO.print(delta.choices[0].delta.content)
})
```

`Accept`を自分で設定しない限り`Accept: text/event-stream`は自動で送られます。
コメント行（`: ...`）と`retry:`フィールドは無視されます。コールバックは呼び出し
スレッド上で実行され（捕捉状態をmutableに扱える）、returnでそのイベントの処理を
終え、throwするとストリームを中断してエラーを伝播します。トランスポート失敗は
`HttpError`です。

### `Http.client(base_url, headers=nil, timeout=0, follow_redirects=true) -> Object`

**1本のkeep-alive接続を再利用**し、**ベースURL** と**デフォルトヘッダ**を保持する
永続クライアント。同じAPIを何度も叩く時に、ホスト・認証ヘッダの繰り返しと毎回の
TLSハンドシェイクを避けられます。返り値は`get` / `post` / `put` / `delete` /
`head` / `request`（フリーの`Http.*`と同じkwarg。ただし第1引数は`base_url`に
連結されるパス）と`close`を持つハンドルです。

| 引数 | 意味 |
|---|---|
| `base_url` | スキーム + ホスト（+ 任意のパス接頭辞）。例: `https://api.example.com/v1` |
| `headers` | 各リクエストの下に敷くデフォルトヘッダ（キー単位でリクエスト側が優先） |
| `timeout` / `follow_redirects` | 接続レベルの既定（リクエストごとではない） |

```culebra
# doctest: skip
let api = Http.client("https://api.example.com/v1",
                      headers: {Authorization: "Bearer " + token},
                      timeout: 30)

let me   = api.get("/me").json()              # → GET https://api.example.com/v1/me
let user = api.get("/users/42").json()        # 同じ接続を再利用
api.post("/users", json: {name: "alice"})     # Authorization ヘッダが付く

api.get("/items", headers: {"Idempotency-Key": k})   # 既定の上にマージ
api.close()                                    # 接続を解放
```

リクエストメソッドの第1引数はパスです。先頭スラッシュor素の相対パスは`base_url`に
連結され（`/me` → `…/v1/me`）、スキーム付きの絶対URLは`base_url`を無視しますが
デフォルトヘッダは付きます。リクエストごとの`headers`はクライアント既定の上に
マージされます（同名キーは上書き）。`close()`で接続を解放します（スコープを抜けた
クライアントはGCも閉じるので明示`close`は任意）。`close`後のリクエストは
`HttpError`。ハンドルはnon-sendable（1接続・1スレッド）なので、ファンアウトする
ならisolateごとにクライアントを作ります。

**並列・レースリクエスト**はHTTP専用APIを使わず、汎用の [`Parallel`](#12-isolate)
コンビネータを`Http.get`に適用します — JSの`Promise.all`/`race`やElixirの
`Task.async_stream`と同じ形です:

```culebra
# doctest: skip
let urls = ["https://api.example/a", "https://api.example/b"]
Parallel.map(urls, |u| Http.get(u).body)        # all、入力順（fail-fast）
Parallel.map_settled(urls, |u| Http.get(u))     # allSettled: [{ok, value, error}, ...]
Parallel.race(urls, |u| Http.get(u))            # 最速成功が勝ち、残りはキャンセル
```

TLSは現在OpenSSLを静的リンクしていますが、将来BoringSSLへ切り替えてもビルド設定
のみの変更で、このAPIには影響しません（BoringSSLはホスト名検証がより厳格なので、
現在通るCNのみの証明書のサーバは拒否される可能性があります）。

### `Http.server() -> Object`

HTTPサーバです。`get`/`post`/`put`/`delete`/`patch`/`options`でルートを登録し、
`static`でファイルを配信し、`listen`で接続を受け付けます。各ハンドラは
`fn(req) -> response`クロージャです。

```culebra
# doctest: skip
let srv = Http.server()
srv.get("/", fn(req) { "Hello, world!" })
srv.get("/users/:id", fn(req) { "user " + req.params["id"] })
srv.post("/echo", fn(req) { req.body })
srv.get("/json", fn(req) {
  { status: 201, body: '{"ok":true}', content_type: "application/json",
    headers: {"X-Trace": req.headers["X-Request-Id"]} }
})
srv.static("/assets", "./public")
srv.listen(8080)                 # ブロックする。Ctrl+C で停止
```

| メソッド | 効果 |
| --- | --- |
| `get/post/put/delete/patch/options(pattern, handler)` | そのメソッドとルート`pattern`に`handler`（`fn(req)->response`）を登録。サーバを返す（チェーン可） |
| `static(mount, dir)` | URLプレフィックス`mount`で静的ファイルを配信。`dir`はStringパス（ディスク上のディレクトリをライブ配信）または`Embed.dir(...)`記述子（AOTではバイナリに焼き込み — [Embed](#embed) 参照） |
| `sink.write(chunk)` | （`stream:`クロージャ内）1チャンクを送出。クライアント切断時は`false`を返す |
| `bind(port, host="0.0.0.0") -> Long` | listenソケットを開き、実際に取れたポートを返す。`port=0`はOS任せのephemeral port。1回だけ、かつ配信開始後は不可 |
| `serve(workers=0)` | バインド済みソケットでacceptループを回す（中断まで呼び出しスレッドをブロック）。ハンドラはacceptループでなくworkerプールで動くので、遅いハンドラが新規接続の受付を止めない — ハンドラは **Sendable** 必須。`workers=0`（既定）はCPU連動のプールサイズ、正の数で固定 |
| `serve_async(workers=0)` | 同上を背後プールで行い即return。停止は`stop()` |
| `listen(port, host="0.0.0.0", workers=0)` | `bind` + `serve`を1回で。停止するまで返らない |
| `listen_async(port, host="0.0.0.0", workers=0) -> Long` | `bind` + `serve_async`。バインドしたポートを返す |
| `stop()` | 背後（`listen_async`）サーバを停止しスレッドをjoin（別スレッドからも呼べる） |
| `close()` | 配信を停止しサーバを解放（スコープを抜けたサーバはGCも閉じる） |

**リクエスト`req`** は`method`・`path`・`body`（String）と、`headers`・`query`
（解析済みクエリ文字列）・`params`（マッチしたルートのパスパラメータ。例: `:id` →
`req.params["id"]`）をStringのObjectとして持つオブジェクトです（Express流の
名前）。存在しないキーは`nil`を返します。

**ハンドラの戻り値**がレスポンスになります:

- `String` → `200`・`text/plain`・その文字列をbodyに;
- `Object` `{status?, body?, headers?, content_type?}` → 全制御（省略時は`200` /
  `""` / `text/plain`）。`headers`はStringのObject;
- `stream:`フィールドにFunctionを持つ`Object` → chunked（ストリーミング）応答（後述）;
- `nil` → `200`・空body。

ハンドラが例外を投げると、その文面をbodyとする`500`になります。未マッチの
ルートは`404`です。

**ストリーミング応答（SSE / chunked）。** body全体をバッファせず逐次送出するには、
`stream`フィールドが`fn(sink)`クロージャのObjectを返します。bodyはchunkedで
送られ、`status`・`content_type`・`headers`は通常どおり適用されます（`body`と
`stream`の併用は`TypeError`）。クロージャは`sink`ハンドルで呼ばれ、
`sink.write(chunk)`が1チャンクを送出し、クライアントが切断していれば`false`を
返します（長いループはそこで止められる）:

```culebra
# doctest: skip
srv.get("/events", fn(req) {
  { content_type: "text/event-stream",
    headers: {"Cache-Control": "no-cache"},
    stream: fn(sink) {
      for i in 0..10 { sink.write("data: " + i.to_string() + "\n\n") }
    } }
})
```

streamクロージャはworkerスレッド上で実行されるため、`workers: 1`では長寿命の
ストリームが単一スレッドを占有します（並行ストリームには`workers: N`）。ストリーム
途中の例外は接続を中断します（status行は送出済みなので`500`にはできません）。
途中で処理したい場合はbodyを`try`で囲みます。

**WebSocket — `srv.ws(pattern, fn(req, ws))`。** WebSocketルートを登録します。
ハンドラはその接続の間ずっと動く長寿命ループで、戻るまでworkerを1本占有します。
`ws`ハンドルでメッセージを読み書きします:

| メソッド | 効果 |
| --- | --- |
| `for msg in ws { … }` | 受信メッセージ（各`String`）を反復。peerのcloseで終了 |
| `ws.receive()` | 次の受信メッセージ（`String`）。peerがcloseすると`nil` |
| `ws.send(msg)` | テキストメッセージを送信。peer切断時は`false` |
| `ws.close()` | 接続を閉じる |
| `ws.is_open()` | 接続がまだ開いているか |

```culebra
# doctest: skip
srv.ws("/echo", fn(req, ws) { for msg in ws { ws.send(msg) } })
srv.ws("/chat", fn(req, ws) {
  while true {
    let m = ws.receive()
    if m == nil { break }              # peer が close
    ws.send(req.path + ": " + m)
  }
})
```

WebSocket接続はその寿命の間workerを占有するので、`workers: N`が同時WebSocket
接続数になります（`workers: 1`では1接続がサーバをブロック）。culebraから接続するには
[`Http.ws`](#httpwsurl---object) を使います。サーバはリクエスト/レスポンス・
ルートパラメータ・静的ファイル・ストリーミング・WebSocketをカバーします。

**並行性。** ハンドラは常にworkerスレッドのプールで動き（acceptループでは動かない）、
遅いハンドラが新規接続の受付を止めず、複数接続を並列に開くブラウザも直列化されません。
`workers`はプールサイズ:

- `workers: 0`（既定）— CPU連動のプール（最小4・最大8）。ブラウザはページ読み込みで
  複数接続を開き各keep-alive接続がworkerを短時間占有するため最小4、各workerは専用
  ランタイムを持つため上限あり。
- `workers: N` — N固定のプール。リクエストは**真の並列**で処理されます:
  あいだにグローバルなインタプリタロックは挟まりません。

各workerは専用ランタイムを持つため、ハンドラは **Sendable** 必須（可変変数や非Sendable
値をキャプチャ不可）。巨大なread-onlyデータは [`Shared.new`](#12-isolate) で共有（全
workerで1コピー）、workerごとの資源（DB接続）はハンドラ内で開く。非Sendableなハンドラ
は`listen`時に`SendError`（該当ルートを明示）。可変状態を持ちたいなら`Shared`/channel/
hubに寄せる（`Isolate`と同じ規則）。

```culebra
# doctest: skip
let model = Shared.new(load_weights())          # read-only 1 コピーを全 worker で共有
let srv = Http.server()
srv.post("/predict", fn(req) { infer(model, req.body) })
srv.listen(8080, workers: 8)                     # 8 ハンドラが並列実行
```

**バックグラウンドサーバ — `listen_async` + `stop`。** メインスレッドが別作業を
する間（GUIの背後など）配信するには`listen_async`を使います。背後スレッドで配信し
即return、`stop()`で停止（別スレッドからも呼べる）。サーバはハンドルを保持している
間だけ動くので、配信し続けたい間は`srv`を参照し続けてください。ハンドラは呼び出し
スレッド外で動くため **Sendable** 必須（`workers > 1`と同じ）。bind失敗は同期的に
`HttpError`で報告されます。サーバはsingle-useで、一度配信したら再起動は`HttpError`
です — もう一度配信するには新しい`Http.server()`を作ってください。

```culebra
# doctest: skip
let srv = Http.server()
srv.get("/health", fn(req) { "ok" })
srv.listen_async(8080, workers: 4)   # 即 return
# … 別作業をしつつ Http.get("http://127.0.0.1:8080/health") …
srv.stop()                           # 停止して背後スレッドを join
```

あるいは、ブロッキング`listen`をisolate内で動かし、そのisolateをdrop
（または`Ctrl+C`）してacceptループを止める方法もあります:

```culebra
# doctest: skip
let srv_iso = Isolate.spawn(fn() {
  let srv = Http.server()
  srv.get("/health", fn(req) { "ok" })
  srv.listen(8080, workers: 4)
})
# … メインスレッドから Http.get("http://127.0.0.1:8080/health") …
srv_iso.drop()                   # サーバに停止を通知して join
```

**起動完了とポート番号を知る — `bind` + `serve`。** ブロッキング`listen`は返らない
ので何も報告できません。「ソケットが開いた」ことも、`port 0`のときにOSが選んだ番号
も伝えられません。2つに分けると、その両方が判明している地点が生まれます。**`bind`
が返ること自体が起動完了の合図**です — ソケットはバックログ付きで開いているので、
その後に張られた接続は`serve`がacceptを始める前でもカーネルが受けています。

```culebra
# doctest: skip
let (tx, rx) = Channel.new(1)
let srv_iso = Isolate.spawn(fn() {
  let srv = Http.server()
  srv.get("/health", fn(req) { "ok" })
  tx.send(srv.bind(0))           # 0 = 空いているポート。番号を外へ
  tx.drop()
  srv.serve()                    # ここでブロック
})
tx.drop()                        # 親自身の sender コピー
let base = "http://127.0.0.1:" + rx.recv().to_string()
inspect(Http.get(base + "/health").body)     # => 'ok'
srv_iso.drop()
```

ここにチャネル固有の話は何もありません。ポートはただの`Long`なので、ログに出すのも
`Shared`に置くのもファイルに書くのも自由です。**ハンドラが自分のアドレスを知るには
この値を捕捉するのが唯一の手段**でもあります — サーバハンドルは非Sendableなので
`srv`を読むハンドラは弾かれますが、`Long`はそのままコピーされます。

```culebra
# doctest: skip
let srv = Http.server()
let port = srv.bind(0)
Log.info("http server listening", { port: port })
srv.get("/whoami", fn(req) { "http://127.0.0.1:" + port.to_string() })
srv.serve(workers: 4)
```

呼び出しスレッド上なら`listen_async`だけで足ります（バインドしたポートを返すので）。
分割が要るのは呼び出しがブロックする場面だけです。ルート登録は`bind`の前後どちらでも
構いません（必要なのは`serve`の時点）。未バインドでの`serve`、2度目の`bind`、
`bind`後の`listen`はいずれも捕捉可能な`HttpError`で、記録済みのルートは
そのまま残るのでやり直せます。bindに失敗したハンドルも未バインドのまま残るので、
別のポートを試せます。

### `Http.ws(url) -> Object`

WebSocketクライアントを`url`（`ws://host:port/path`）に接続し、サーバ側`ws`と
同じ形のハンドル（`send(msg)`・`receive()`（`String`、peerがcloseすると`nil`）・
`for msg in ws`・`close()`・`is_open()`）を返します。不正なURLや接続失敗は
`HttpError`です。

```culebra
# doctest: skip
let ws = Http.ws("ws://127.0.0.1:8080/echo")
ws.send("hello")
inspect(ws.receive())               # => エコーされたメッセージ
for msg in ws { handle(msg) }    # サーバが close するまでメッセージを drain
ws.close()
```

### Embed

`Embed.dir(name)`は`srv.static(mount, ...)`用のディレクトリ記述子を返し、
**バックエンドごとに**（コード変更なしで）解決されます:

- **ソース実行**（インタプリタ / JIT）: `name`のディスク上ディレクトリをライブ
  配信（エントリスクリプト相対で解決）。ファイルを編集してリロードすれば即反映
  ＝開発ループ。
- **`culebra build`**（AOT）: ビルド時にディレクトリを走査してバイト列をバイナリ
  に焼き込み、外部ファイル無しで配信。焼き込んだ内容はビルドが表示する
  （`embedded N file(s) (… bytes) from '…'`）。

```culebra
# doctest: skip
let srv = Http.server()
srv.static("/", Embed.dir("dist"))     # フロントエンド全体を1行で
srv.get("/api/ping", fn(req) { '{"ok":true}' })
srv.listen(8080)
```

`name`はAOTビルドが探して焼き込めるよう**文字列リテラル**であること（計算した
パスはソース実行では動くが焼き込まれない）。Content-Typeは拡張子から推論、
ディレクトリ（や`/`）へのリクエストはその`index.html`、ディレクトリに無いパスは
登録ルートにフォールスルー（APIルートが常に優先）。`Embed.dir`は`Http`非依存
＝任意の利用側が配信できるプレーンな記述子を返す。

`culebra build`は焼き込むアセットをculebraのヘッダに対してコンパイルするため、
ソースチェックアウトが必要。既定はそのバイナリをビルドしたときのパスで、
`$CULEBRA_HOME`があればそちらが優先される。どちらも無ければバイナリを作らず
エラーで停止する。

---

## 16. `Encoding`

テキストコーデックを**スキームごとのサブ名前空間**にまとめた名前空間
（`Encoding.html`、`Encoding.base64`、`Encoding.hex`、`Encoding.url`）。
コーデックのロジックはインタプリタとJIT/AOT両バックエンドで共有しており、
いずれもバイナリセーフ（埋め込みNULバイトも往復で保持）です。

### `Encoding.html`

| 関数 | 結果 |
| --- | --- |
| `Encoding.html.escape(s)` | `String` — HTMLで危険な5文字（`& < > " '`）をエンティティに置換 |
| `Encoding.html.unescape(s)` | `String` — エンティティ参照を元の文字へ戻す |

`escape`は最初に`&`を置換する（出力を再escapeしても安全）ため、
`&amp;` `&lt;` `&gt;` `&quot;` `&#39;`を出力します。

`unescape`は数値参照`&#DDD;`（10進）と`&#xHHH;` / `&#XHHH;`（16進、大小問わず）に
加え、常用の名前付き参照（typographic / Latin-1 / ギリシャ文字 / 数学記号 / 通貨の
よく使うもの。HTML5全 ~2200件ではない）を扱います。参照は`;`で終わる必要があり、
整形式かつ既知でない参照は**そのまま**残します（ブラウザ流の寛容さ）。単独の`&`や
未知のエンティティは素通しされます。

```culebra
inspect(Encoding.html.escape("a & b < c"))          # => 'a &amp; b &lt; c'
inspect(Encoding.html.escape("it's fine"))          # => 'it&#39;s fine'
inspect(Encoding.html.unescape("Tom &amp; Jerry"))  # => 'Tom & Jerry'
inspect(Encoding.html.unescape("caf&eacute; &mdash; x")) # => 'café — x'
inspect(Encoding.html.unescape("&#65;&#x42;"))      # => 'AB'
inspect(Encoding.html.unescape("&#12354;"))         # => 'あ'
inspect(Encoding.html.unescape("&unknownent;"))     # => '&unknownent;'
```

### `Encoding.base64`

| 関数 | 結果 |
| --- | --- |
| `Encoding.base64.encode(s)` | `String` — base64（RFC 4648標準アルファベット、`=`パディング） |
| `Encoding.base64.decode(s)` | `String` — デコード結果。不正入力は`ValueError` |

`encode`はバイナリセーフ（マルチバイトUTF-8を含む任意のバイト列）。`decode`は
入力中のASCII空白（行折り返しbase64）と`=`パディングを許容し、アルファベット外の
文字は`ValueError`。

```culebra
inspect(Encoding.base64.encode("user:pass"))   # => 'dXNlcjpwYXNz'
inspect(Encoding.base64.decode("dXNlcjpwYXNz")) # => 'user:pass'
```

```culebra
# doctest: skip
# 例: HTTP Basic 認証ヘッダ
let cred = Encoding.base64.encode(user + ":" + password)
let r = Http.get(url, headers: {Authorization: "Basic " + cred})
```

### `Encoding.hex`

| 関数 | 結果 |
| --- | --- |
| `Encoding.hex.encode(s)` | `String` — 小文字16進、1バイトにつき2桁 |
| `Encoding.hex.decode(s)` | `String` — デコード結果。不正入力は`ValueError` |

`encode`は常に小文字で出力します。`decode`は大小いずれの桁も受け付け、奇数長や
16進以外の文字は`ValueError`。

```culebra
inspect(Encoding.hex.encode("abc"))   # => '616263'
inspect(Encoding.hex.decode("616263")) # => 'abc'
inspect(Encoding.hex.decode("00FF").size()) # => 2
```

### `Encoding.url`

| 関数 | 結果 |
| --- | --- |
| `Encoding.url.encode(s)` | `String` — パーセントエンコード（RFC 3986） |
| `Encoding.url.decode(s)` | `String` — パーセントエスケープをデコード |

`encode`は非予約集合`A-Z a-z 0-9 - _ . ~`をそのまま残し、それ以外のバイトを
大文字16進の`%XX`にします（空白は`+`ではなく`%20`、マルチバイトUTF-8は
バイト単位でエンコード）。`decode`は寛容で、2桁の16進が続かない`%`は**そのまま**
残し、リテラルの`+`も`+`のまま（`encode`/`decode`がちょうど往復します）。

```culebra
inspect(Encoding.url.encode("a b&c"))   # => 'a%20b%26c'
inspect(Encoding.url.decode("a%20b%26c")) # => 'a b&c'
inspect(Encoding.url.encode("café"))    # => 'caf%C3%A9'
```

---

## 17. `Compress`

zlibを用いたgzip / deflate圧縮・展開。どの関数もバイナリセーフ（埋め込み
NULも往復で保持）です。

| 関数 | 結果 |
| --- | --- |
| `Compress.gzip(data: String) -> String` | gzip圧縮したバイト列（RFC 1952ラッパー）。標準の`gzip`ツールと相互運用可 |
| `Compress.gunzip(data: String) -> String` | 展開したバイト列。不正な入力は`ValueError` |
| `Compress.deflate(data: String, level: Long = -1) -> String` | zlib圧縮したバイト列（RFC 1950ラッパー）— `gzip`からgzip固有のヘッダを除いたもの |

`gunzip`はヘッダを自動判別するので、`gzip`と`deflate`の出力をどちらも同じ
1つの関数で展開します — 別に`inflate`はありません。切り詰められた入力や
認識できない入力は`ValueError`。

```culebra
let original = "the quick brown fox the quick brown fox the quick brown fox the quick brown fox"
let z = Compress.gzip(original)
inspect(z.size() < original.size())          # => true
inspect(Compress.gunzip(z) == original)      # => true
```

```culebra
# doctest: skip
# .gz ファイルを読み書き
let text = Compress.gunzip(FS.read("logs.gz"))
FS.write("out.gz", Compress.gzip(text))
```

`deflate`が`gzip`と違うのはラッパーだけです — ファイル名/mtimeヘッダも
CRC-32トレーラも無く、2 byteのヘッダとAdler-32チェックサムだけ。これは
PNGの`IDAT`チャンクが持つ形式そのもので、gzipの封筒が何の得にもならない
場面（メモリ上のblob、別のコンテナに埋め込む値）で選ぶとよい小さい方です。

```culebra
let text = "the quick brown fox the quick brown fox the quick brown fox"
let z2 = Compress.deflate(text)              # gzip ではなく zlib ラッパー
inspect(Compress.gunzip(z2) == text)         # 同じデコーダ
# => true
inspect(z2.size() < Compress.gzip(text).size())   # gzip ヘッダが無い分
# => true
```

`level`はzlib自身の規約に従います: `-1`（既定）はzlib内蔵のトレードオフ、
`0`は無圧縮で格納、`9`は最も時間をかけて最小の出力にします。`-1..9`の範囲
外の値はその呼び出しで`ValueError`。

```culebra
let text = "the quick brown fox the quick brown fox the quick brown fox"
inspect(Compress.deflate(text, level: 9).size() <=
        Compress.deflate(text, level: 0).size())   # => true
```

HTTPレスポンスは`Http`クライアントが透過的に展開します（`Accept-Encoding`を
送り、`Content-Encoding: gzip`を自動で展開）。したがって`Compress`は自分で扱う
データやファイル向けで、`Http`のボディには不要です。

---

## 18. `Hash`

メッセージダイジェストとHMAC。自前実装（OpenSSL非依存）で全バックエンド一致。
各関数は**小文字hex** のダイジェストを返します。入力はバイナリセーフ（埋め込み
NULも終端でなくメッセージの一部）。

| 関数 | 結果 |
| --- | --- |
| `Hash.sha256(data: String) -> String` | 64文字hexのSHA-256ダイジェスト |
| `Hash.sha1(data: String) -> String` | 40文字hexのSHA-1ダイジェスト |
| `Hash.sha512(data: String) -> String` | 128文字hexのSHA-512ダイジェスト |
| `Hash.md5(data: String) -> String` | 32文字hexのMD5ダイジェスト |
| `Hash.hmac_sha256(key: String, data: String) -> String` | 64文字hexのHMAC-SHA-256 |
| `Hash.hmac_sha1(key: String, data: String) -> String` | 40文字hexのHMAC-SHA-1 |
| `Hash.hmac_sha512(key: String, data: String) -> String` | 128文字hexのHMAC-SHA-512 |

```culebra
inspect(Hash.sha256("abc"))
# => 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'
inspect(Hash.md5("abc"))
# => '900150983cd24fb0d6963f7d28e17f72'
inspect(Hash.hmac_sha256("Jefe", "what do ya want for nothing?"))
# => '5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843'
```

生（hexでない）ダイジェストが必要なら`Encoding.hex.decode`で復号、別形式に
したいなら`Encoding.base64`と組み合わせます。MD5とSHA-1は既存システム
（チェックサム、レガシーAPI）との互換のために提供しています — 新規の
セキュリティ用途にはSHA-256 / SHA-512を使ってください。

---

## 19. `CSV`

カンマ区切り値（RFC 4180流）のparse / 生成。全backendでbyte一致。
フィールドはカンマ、レコードは改行で区切られ、カンマ・ダブルクォート・改行を
含むフィールドはダブルクォートで囲み、内部のクォートは`""`に二重化する。

| 関数 | 結果 |
| --- | --- |
| `CSV.parse(text, delimiter=",", header=false, types=nil) -> Array` | Stringフィールドの行、または（`header`指定で）ObjectのArray |
| `CSV.stringify(rows: Array, delimiter: String = ",") -> String` | CSVテキスト。各行はArray、各フィールドは`to_string`同様にレンダリング |

`delimiter:`オプション（1バイト。先頭バイトを使用）でフィールド区切りを選べる
— TSVなら`"\t"`。位置引数でもキーワードでも渡せる。

既定では`parse`は寛容（エラーなし）: 全フィールドは`String`で返り（数値推論は
しない）、空入力は0行、末尾改行は空行を足さない。LFとCRLFの両方がレコード区切り。
`stringify`は各フィールドを`to_string`と同じ変換でレンダリングするので数値や
`Bool`も自然に出力され、`stringify(parse(text))`は整形式入力をround-tripする。

```culebra
let rows = CSV.parse("name,age\nalice,30\nbob,25")
inspect(rows[1])                                         # => ['alice', '30']
inspect(CSV.stringify([["a,b", "c"], [1, 2]]) == "\"a,b\",c\n1,2")   # => true
inspect(CSV.parse("a\tb", delimiter: "\t")[0])           # => ['a', 'b']
```

**ヘッダモード — `header: true`.** 1行目を列名とし、以降の各行を（位置Arrayでなく）
その名前をキーにした`Object`にする：

```culebra
let rows = CSV.parse("name,age\nalice,30\nbob,25", header: true)
inspect(rows[0]["name"])                                 # => 'alice'
```

データ行が無いヘッダ（や空入力）は`[]`。ヘッダ名の重複や、ヘッダとフィールド数が
食い違うデータ行は`ValueError`。

**型付き列 — `types:`.** `types:`（ヘッダ名 → `"String"` / `"Long"` / `"Float"` /
`"Bool"`の`Object`）を渡すとその列を変換する。未指定の列は`String`のまま。変換は
**明示・推論しない** — 郵便番号やIDを`String`と宣言すれば元のテキストが正確に保たれる
（先頭ゼロや精度の喪失なし）。`types:`は`header: true`を要する。

```culebra
let rows = CSV.parse("name,age,active\nalice,30,true", header: true,
                     types: {age: "Long", active: "Bool"})
# age はもう本物の Long（String でない）ので算術が効く:
inspect(rows[0]["age"] + 1)                              # => 31
# 郵便番号は元テキストのまま — 数値推論なし:
let z = CSV.parse("zip\n01234", header: true, types: {zip: "String"})
inspect(z[0]["zip"])                                     # => '01234'
```

未知の型名・どの列も指さない`types`キー・変換不能なセル（`Long`への`"hello"`、
空セル等）は、レコード番号と列名付きの`ValueError`。Boolは厳密に`"true"` /
`"false"`のみ。変換は前後の空白をトリムしない。

制約検証（範囲・正規表現・許可集合・一意性）は**あえて組み込まない** — それは別の
汎用的な関心事。`types:`を使わない場合は`to_long` / `to_float`で都度変換する。

---

## 20. `Env`

dotenv形式の`.env`設定をparse / loadする。3 backendでバイト一致。

| 関数 | 結果 |
| --- | --- |
| `Env.parse(text: String) -> Object` | dotenvテキストを`String`値の`Object`にparse（副作用なし） |
| `Env.load(path: String = ".env", override: Bool = false) -> Object` | ファイルを読み、parseし、各エントリをプロセス環境に設定し、parseした`Object`を返す |

各エントリは1行`KEY=VALUE`:

- 空行と、先頭の非空白文字が`#`の行は無視;
- 先頭の`export `は除去（シェル流の`.env`をそのまま読める）;
- キーはtrimされ、`=`の無い行やキーが空の行はskip;
- ダブルクォート値（`"..."`）は`\n`・`\t`・`\r`・`\\`・`\"`のエスケープを解釈、
  シングルクォート値（`'...'`）はraw、無クォート値はtrimされ、インラインの
  ` # コメント`（空白に続く`#`）は除去（空白を伴わない`#`はリテラル）;
- 重複キーは最初の位置を保ち、最後の値が勝つ。

parseは寛容（不正な行はskip）。複数行の値は非対応。`Env.load`はファイルを開けない
場合`IOError`を送出。デフォルトでは既に環境にある変数を上書きしない（実環境が優先）。
`override: true`で既存値を置き換える。読み込んだ変数は`Sys.env`から見え、以降に
起動する子プロセス（例: `Proc.run`）に継承される。

```culebra
let cfg = Env.parse("# app config\nPORT=8080\nNAME=\"my app\"\nDEBUG=true")
inspect(cfg["PORT"])                 # => '8080'
inspect(cfg["NAME"])                 # => 'my app'
inspect(cfg["DEBUG"])                # => 'true'
```

```culebra
# doctest: skip
Env.load(".env")                  # ./.env があれば変数を設定
inspect(Sys.env("PORT"))
```

値を数値や真偽値として使うには、利用箇所で`to_long` / `to_float`で変換する
（全ての値は`String`）。

---

## 21. `UUID`

正準の小文字UUID（`8-4-4-4-12`ハイフン形式）を生成。2種類:

| 関数 | 結果 |
| --- | --- |
| `UUID.v4() -> String` | ランダムUUID（122ランダムビット） |
| `UUID.v7() -> String` | 時刻順UUID — 48-bit Unix-ミリ秒プレフィックス + ランダム。作成時刻でソートできる（DBキー向き） |

エントロピーは`Random.*`が使う共有PRNG由来なので`Random.seed`で再現可能、
かつ**暗号学的に安全ではない**（識別子向けで、トークンや秘密には不可）。v7は
ミリ秒単位の順序で、同一ミリ秒内に生成された2値は相互に順序づけられない
（monotonic counterは持たない）。

```culebra
inspect(UUID.v4().size())          # => 36
inspect(UUID.v4() != UUID.v4())    # => true
```

---

## 22. `Term`

テキストUI（TUI）を作るための端末制御 — 色・カーソル位置・代替画面・
端末サイズ・非ブロッキングなキー入力。色とエスケープのヘルパーは文字列を
返す純関数なので合成・テストが容易で、状態を持つ部分（rawモード・描画
ループ）は終了時に端末を必ず復帰するようラップされています。

### 色と属性

各関数は引数を対応するANSIコード（＋リセット）で包んで返すので、入れ子に
できます:

| 関数 | 結果 |
| --- | --- |
| `Term.fg(s, n) -> String` | 256色前景（`n`は0–255） |
| `Term.bg(s, n) -> String` | 256色背景 |
| `Term.rgb(s, r, g, b) -> String` | 24bitトゥルーカラー前景 |
| `Term.red(s)` / `green` / `yellow` / `blue` / `magenta` / `cyan` / `white` / `black` | 名前付き16色前景 |
| `Term.bold(s)` / `Term.dim(s)` / `Term.underline(s)` / `Term.reverse(s)` | 文字属性 |
| `Term.style(fg:, bg:, bold:, dim:, underline:, reverse:) -> String` | `Screen`セル用のSGRパラメータ文字列。`fg`/`bg`は256色インデックスか`(r,g,b)`タプル |

色は端末の**ケイパビリティレベル**（`0`なし / `1` 16 / `2` 256 / `3`
トゥルーカラー）に適応します。レベルは`isatty`・`NO_COLOR`（あれば無効）・
`FORCE_COLOR`・`COLORTERM`・`TERM`から検出。レベルを超える色は
ダウンサンプル（トゥルーカラー → 最近傍256 → 最近傍16）され、レベル0では
何も出さない（パイプ / `NO_COLOR`時はプレーン）。`Term.level()`で取得、
`Term.set_level(n)`で上書きできます。`fg`/`bg`/`rgb`/`bold`/… は直接表示用に
文字列を包み、`Term.style(...)`は色付きセル用に`screen.set` / `screen.put`
へ渡すスタイルを返します。

```culebra
inspect(Term.bold(Term.fg("alert", 196)))          # 太字・明るい赤の "alert"（表示用）
let st = Term.style(fg: (255, 128, 0), bold: true)   # Screen セル用
```

### エスケープ・サイズ・幅

| 関数 | 結果 |
| --- | --- |
| `Term.clear() -> String` | 画面クリア＋カーソルを原点へ |
| `Term.move(x, y) -> String` | カーソルを列`x`・行`y`へ（0始まり） |
| `Term.hide()` / `Term.show()` | カーソルの非表示 / 表示 |
| `Term.cols()` / `Term.rows() -> Long` | 端末サイズ（ttyでなければ80×24） |
| `Term.size() -> (Long, Long)` | `(cols, rows)` |
| `Term.width(s) -> Long` | 表示幅（全角 / 絵文字 = 2、結合 = 0） |
| `Term.flush()` | バッファ済み出力をフラッシュ |

### パイプ入力

`Term.attach_tty() -> Bool`はstdinを制御端末へ再接続する（現在のstdinが
パイプでもリダイレクトされたファイルでも上書きする）。raw modeとキー読み取り
（`Term.app`・`Term.poll`・`Term.read_key`）は本物のttyでしか機能しないため、
stdinに内容を流し込まれたスクリプト（`some-command | script.cul`）は先に
そのパイプを読み切ってから`Term.attach_tty()`を呼んで対話的なキー入力へ
切り替えられる —— `less`と同じパターン。制御端末が一切無い場合（完全に
非対話・CI下など）は`false`を返し、stdinはそのまま変更されない。

```culebra
# doctest: skip
let content = IO.stdin().read()   # 先にパイプを読み切る
if !Term.attach_tty() {
  println(content)                # 対話できる端末が無い
  Sys.exit(0)
}
Term.app(fn (screen) { ... })
```

### 入力イベント

入力は単一のイベントモデル: `Term.poll(timeout)` —— `Term.app`の
コールバック内では`screen.poll(timeout)`としても届きます —— が1つの
イベント **Object** を返し、`timeout`秒の間に何も来なければ`nil`を
返します。既に読み込んだエスケープ列を同じ形に変換するのが
`Term.parse(raw)`です。`kind`で種別を判別し、修飾子はboolです。

| `kind` | フィールド |
| --- | --- |
| `"key"` | `key`・`ctrl`・`shift`・`alt` |
| `"mouse"` | `event`・`button`・`x`・`y`・`ctrl`・`shift`・`alt` |
| `"resize"` | `cols`・`rows` |

**キー**の`key`は印字可能文字（`"q"`・`" "`）か名前:
`"up"` / `"down"` / `"left"` / `"right"`・`"enter"`・`"escape"`・`"tab"`・
`"backspace"`・`"insert"`・`"delete"`・`"home"`・`"end"`・`"pageup"`・
`"pagedown"`・`"f1"`…`"f12"`。修飾子は`ctrl` / `shift` / `alt`
（例Ctrl+Right → `{key: "right", ctrl: true}`、Ctrl+C →
`{key: "c", ctrl: true}`、Alt+x → `{key: "x", alt: true}`）。

**マウス**は`event`が`"press"` / `"release"` / `"drag"` / `"scroll"`、
`button`が`"left"` / `"middle"` / `"right"` / `"wheel_up"` / `"wheel_down"`、
`x` / `y`は0始まりのセル。

`Term.resized() -> Bool`は低レベルのリサイズフラグ（SIGWINCH後一度true）、
`poll`はそれを`"resize"`イベントにします。

マウスはオプトイン: `Term.app(..., mouse: true)`（または`Term.mouse_on()` /
`Term.mouse_off()`を自分でprint）で有効化。

```culebra
# doctest: skip
let ev = screen.poll(0.1)
if ev != nil {
  if ev.kind == "key" && ev.key == "q" { ... }
  else if ev.kind == "mouse" && ev.event == "press" { ... ev.x, ev.y ... }
}
```

### `Term.app` と `Screen`

`Term.app(fn (screen) { ... }, mouse: false)`はrawモードと代替画面に入り、
カーソルを隠し、リサイズを監視し（`mouse: true`でマウス報告も有効化）、
**終了時に端末を復帰**します（正常終了・例外・Ctrl+Cいずれも）—
`defer`による保証です。コールバックは`Screen`を
受け取ります:

| メソッド | 効果 |
| --- | --- |
| `screen.size()` / `cols()` / `rows()` | 端末の寸法 |
| `screen.clear()` | バックバッファを空フレーム（現在サイズ）にリセット |
| `screen.set(x, y, glyph, style = "")` | 1グラフェムを（任意の`Term.style`付きで）バックバッファに置く |
| `screen.put(x, y, s, style = "")` | `s`のグラフェムを`style`で連続セルに配置 |
| `screen.render() -> String` | 前フレームから画面を更新する最小エスケープ（フロントバッファも前進） |
| `screen.flush()` | `render()`を出力してフラッシュ |
| `screen.poll(timeout) -> Object?` | 最大`timeout`秒、入力イベント（キー / マウス / リサイズ）を待つ。無ければ`nil` |

`Screen`はセル（グリフ + 任意のスタイル）のダブルバッファです。`flush`は
前フレームから**変化したセルだけ**を、スタイル間の最小SGR遷移付きで出力
するので、ライブUIがちらつかず最小出力で更新されます（全角グリフは2
セル、リサイズは全再描画）。`clear` + `set` / `put`（色は`Term.style(...)`
を渡す）でフレームを組み立て`flush`、入力は`poll`（フレームごとのウェイト
も兼ねる）で読みます。

```culebra
Term.app(fn (s) {
  s.clear()
  s.put(2, 1, "hello")
  s.flush()
  s.poll(2.0)            # 最大 2 秒キー入力を待つ
})
```

---

## 23. `Log`

**標準エラー出力**へのレベル付き構造化ログ（プログラムのstdoutデータを汚さない
＝`myscript | jq`のようなパイプを壊さない）。

| 関数 | 結果 |
| --- | --- |
| `Log.debug(msg: String, fields: Object = {})` | `debug`でログ |
| `Log.info(msg, fields = {})` | `info`でログ |
| `Log.warn(msg, fields = {})` | `warn`でログ |
| `Log.error(msg, fields = {})` | `error`でログ |
| `Log.with(fields: Object) -> logger` | `fields`を全レコードに束縛するchild logger |
| `Log.set_level(level: String) -> Nil` | しきい値を設定（`"debug" < "info" < "warn" < "error"`） |
| `Log.set_format(format: String) -> Nil` | `"text"`（既定）または`"json"` |

各呼び出しはメッセージと省略可能な構造化フィールド`Object`を取る。しきい値以上の
レコードだけが出力される（既定`info`なので`debug`は落ちる）。しきい値とフォーマットは
`LOG_LEVEL` / `LOG_FORMAT`環境変数を既定値とし、`set_level` / `set_format`で上書き可能
（不明なレベル・フォーマットはraise）。タイムスタンプ（ISO 8601 UTC）は常に付与。

`text`は人間可読（stderrが端末ならlevelに色が付く）:

```
2026-06-24T21:30:01Z info request done method=GET status=200 ms=12.4
```

`json`は1行1オブジェクト（JSON Lines — `jq`やログシッパーへ）:

```json
{"time":"2026-06-24T21:30:01Z","level":"info","msg":"request done","method":"GET","status":200,"ms":12.4}
```

`Log.with(fields)`は **child logger** を返し、`fields`を全行に乗せる
（リクエスト/ジョブ単位の文脈を毎回でなく一度だけ束縛）。childは親のレベル/フォーマットを
共有し、ネスト可能。予約キー`time`・`level`・`msg`は同名フィールドより常に優先される。

```culebra
# doctest: skip
Log.info("server started")
Log.set_level("debug")
Log.debug("cache miss", {key: "user:42"})

let log = Log.with({request_id: id})   # 文脈を一度束縛
log.info("received")                    # ...以後の全行に付与
log.error("upstream failed", {status: 502})
```

値はtextでは`to_string`、jsonではJSON namespaceで直列化される。致命的状況は
`error`でログして`Sys.exit(1)`する（専用の`fatal`レベルは無い）。

---

## 24. `TOML`

[TOML](https://toml.io) 設定のparse / 生成。文法と直列化は値中立コアに置かれ、
インタプリタ・JIT・AOTがバイト単位で一致する。

| 関数 | 結果 |
| --- | --- |
| `TOML.parse(text: String) -> Object` | 文書を入れ子`Object`として返す |
| `TOML.stringify(v: Object, sort_keys: Bool = false) -> String` | TOMLテキスト。サブテーブルは`[section]`見出しに展開 |

`parse`はTOML v1.0の範囲を受け付ける: bare / quoted / dottedキー、
`[table]` / `[[array.of.tables]]`見出し、4種の文字列形式（basic・literalと
それぞれの複数行`"""` / `'''`）、整数（10進 / `0x` / `0o` / `0b`、`_`区切り可）、
浮動小数（`inf` / `nan`を含む）、真偽値、配列、インラインテーブル。値の対応:

| TOML | Culebra |
| --- | --- |
| table / インラインテーブル | `Object`（挿入順を保持） |
| 配列 | `Array` |
| テーブル配列`[[x]]` | `Array<Object>` |
| 文字列（4形式いずれも） | `String` |
| 整数 | `Long` |
| 浮動小数 | `Float` |
| 真偽値 | `Bool` |
| 日時 / 日付 / 時刻 | `String`（生のまま保持 — 専用の日時型は無い） |

日時は専用型ではなく生テキストとして返るため、往復すると通常の文字列として
再クォートされる。不正な入力は`ValueError`を投げ、`e.line` / `e.col`
（いずれも1始まり）が問題の文字を指す:

```culebra
let r = try { TOML.parse("x = "); nil } catch e { e }
inspect(r.message)            # => 'TOML.parse: expected value'
inspect("{r.line}:{r.col}")   # => '1:5'
```

`stringify`は`Object`（TOML文書は常にテーブル）を取り、まずスカラ / 配列 /
インライン値を出力し、続いてサブテーブルを`[section]`見出しへ、テーブル配列を
`[[…]]`ブロックへ展開する。これによりbareキーが必ず該当見出しより前に来る。
浮動小数は常に小数点を持つのでfloatとして読み戻せる。`sort_keys: true`は
キーをアルファベット順に走査し決定的出力にする。関数・テンソル・非Stringキーを
持つObjectは直列化できず、`stringify`は`TypeError`を投げる。

```culebra
let cfg = TOML.parse("""
title = "demo"
ports = [80, 443]

[server]
host = "localhost"
""")
inspect(cfg.title)            # => 'demo'
inspect(cfg.server.host)      # => 'localhost'
```

`stringify`は、裸のキーを、それを飲み込んでしまうヘッダより前に出します:

```culebra
inspect(TOML.stringify({a: 1, b: {c: 2}}))
# => |
# 'a = 1
#
# [b]
# c = 2
# '
```

---

## 25. `SQLite`

[SQLite](https://sqlite.org) による組み込みSQLデータベース（amalgamationを
vendor同梱・コンパイル済みで、システムライブラリ不要）。`SQLite.open`は
ステートフルな **Database** ハンドルを返す。高レベルの`execute` / `query` /
`transaction`が日常的なCRUDを、`prepare`が返す再利用可能な **Statement**
ハンドルがホットループを担う。どちらのハンドルもスコープを抜けると確定的に
クローズされるので、明示的な`close` / `finalize`は任意。

| 関数 | 結果 |
| --- | --- |
| `SQLite.open(path: String) -> Database` | DBを開く（無ければ作成）。`":memory:"`でインメモリ |
| `SQLite.version() -> String` | リンクされたSQLiteライブラリのバージョン（例`"3.53.2"`） |

### Database

| メソッド | 結果 |
| --- | --- |
| `db.execute(sql: String, params = nil) -> Long` | 文を1つ実行。影響行数を返す |
| `db.query(sql: String, params = nil) -> Array<Object>` | クエリを実行。各行は列名をキーとする`Object` |
| `db.prepare(sql: String) -> Statement` | 再利用可能な文をコンパイル |
| `db.transaction(fn: Function) -> Any` | `BEGIN` → `fn`実行 → `COMMIT`。throwされると`ROLLBACK`して再送出 |
| `db.close()` | 接続を閉じる（スコープ離脱時にも自動実行） |

### Statement

| メソッド | 結果 |
| --- | --- |
| `stmt.run(params = nil) -> Long` | 実行（INSERT/UPDATE/DELETE）。影響行数を返す |
| `stmt.query(params = nil) -> Array<Object>` | クエリを実行し行を収集 |
| `stmt.finalize()` | 文を解放（スコープ離脱時にも自動実行） |

### パラメータ

`params`はSQLのプレースホルダを束縛する。**Array** は位置プレースホルダ
`?`を左から順に、**Object** は名前付きプレースホルダ`:name`（`@name` /
`$name`も可）をキーで束縛する：

```culebra
# doctest: skip
db.execute("INSERT INTO users VALUES (?, ?)", [1, "Alice"])
db.query("SELECT * FROM users WHERE id = :id", {id: 1})
```

### 型マッピング

値は読み出し時は列の実行時型で、書き込み時はculebra値の型で対応づけられる：

| SQLite | Culebra |
| --- | --- |
| INTEGER | `Long` |
| REAL | `Float` |
| TEXT | `String` |
| BLOB | `String`（生バイト列） |
| NULL | `nil` |

書き込み側では`Bool`は`0` / `1`として束縛される。それ以外の型（`Array`・
`Object`・関数など）の束縛は`TypeError`。SQLエラーや制約違反はSQLite自身の
メッセージを載せた`SQLiteError`を送出する：

```culebra
let db = SQLite.open(":memory:")
db.execute("CREATE TABLE users (id INTEGER, name TEXT)")
db.execute("INSERT INTO users VALUES (?, ?)", [1, "Alice"])

let rows = db.query("SELECT * FROM users")
inspect(rows[0]["name"])      # => 'Alice'

# 再利用可能なプリペアド文
let ins = db.prepare("INSERT INTO users VALUES (?, ?)")
for u in [[2, "Bob"], [3, "Carol"]] { ins.run(u) }
ins.finalize()

# all-or-nothing
db.transaction(fn () {
  db.execute("UPDATE users SET name = 'Bob!' WHERE id = 2")
})

let r = try { db.query("SELECT * FROM missing"); nil } catch e { e }
inspect(r.kind)               # => 'SQLiteError'

db.close()
```

Database / Statementハンドルは生成したスレッド（isolate）に紐づく。`Sendable`
ではなく、`Isolate` / `Channel`境界を越えて渡せない。トランザクションはネスト
できない（必要なら`SAVEPOINT`を直接使う）。

---

## 26. `Canvas`

小さなゲームやピクセルグラフィックス向けのイミディエイトモード2Dフレーム
バッファ。フレームを描き、`present`で提示し、入力をポーリングして繰り返す。
色はpacked RGBA `Long`、バッファは任意サイズ（WASM-4流の160×160が典型）。
WASM PlaygroundではCanvasプログラムは **Canvasタブ**で動く — フレームは
`<canvas>`に表示され、キーボード / ポインタが入力になり、`tone`はWebAudioで
鳴る。ネイティブではmacOS・Linux・Windowsで（`Scene`と同じvendored静的
raylib + SDL3を使い）**実際のデスクトップウィンドウを開く**。Linuxでの
ビルドにはSDL3が挙げるビルド依存
（`vendor/SDL/docs/README-linux.md`）が必要で、探索するX11 / 音声のヘッダが
無いとSDL3のconfigureが失敗するので、入っていないマシンでは
`-DCULEBRA_ENABLE_CANVAS_WINDOW=OFF`でconfigureする。Windowsでは追加の
パッケージは不要 — SDL3のWin32バックエンドが要るヘッダはmingw-w64の
ツールチェーンに揃っている。できたバイナリはどこでも
動く: SDL3はX11/GL/音声（Windowsなら`opengl32.dll`など）を初回使用時に
読み込むので、ウィンドウビルドでもロード時のライブラリ依存は増えず、
ディスプレイの無いサーバでも問題なく起動する。
各`present`はフレームをアップロードし、最近傍で見やすい
サイズに整数倍拡大し、60fpsでvsyncまでブロックする。キーボードとマウスが
`Canvas.buttons` / `Canvas.mouse`になり、ウィンドウを閉じる（またはEsc）と`run`
ループが終わる。**ヘッドレスは宣言するもので、推測されるものではない**:
ウィンドウバックエンド無しのビルド、および`CULEBRA_CANVAS_HEADLESS`が
`0` / `off`以外に設定された実行では**ヘッドレス**: ピクセル / スプライト操作は同一に
動く（振る舞いはinterpreter / JIT / AOTで一致し`Canvas.get_pixel`で検証可能）が、
何も表示されず、入力は「ボタンなし」を返し、`tone`は無音。この環境変数が、
ディスプレイの無いサーバや（`just`の全レシピがexportするので）テストスイートが
ウィンドウ対応バイナリを走らせる方法。`-DCULEBRA_ENABLE_CANVAS_WINDOW=OFF`は
さらに踏み込んでraylib自体をビルドから外す。どちらも宣言していないウィンドウ
ビルドがウィンドウを開けない場合（ディスプレイ無し・使えるGL無し）は、黙って
ヘッドレスのふりをするのではなく、最初の`present`でこの環境変数を案内する
`RuntimeError`を送出する。ヘッドレスでなければネイティブ`tone`も
小さなソフトウェアAPU（下記音声参照）をraylibのオーディオスレッドで鳴らし、
音声デバイスは初回使用時に遅延で開く。

### 色

`Canvas.rgba(r, g, b, a = 255) -> Long`は0–255の4チャンネルを1つの`Long`
に詰める（バイト順`[r, g, b, a]`）。すべての描画呼び出しがこの色を取り、
**アルファは合成される**: 255は不透明に描き、0は何も描かず、その間は図形が
既存の内容の上にブレンドされる（整数source-over — 色チャンネルごとに
`(src*a + dst*(255-a) + 127) / 255`。3 backendで丸めが一致し、不透明な
バッファは不透明のまま）。`rgba(0, 0, 0, 128)`は半暗のオーバーレイで、
2回描けば2回暗くなる。例外は2つ: `clear`は全ピクセルを与えた値で
**置き換える**（フレームのリセットであって薄塗りではない）。`set_pixel`は
値をそのまま格納し`get_pixel`と対になる（透過を書き込むのはこの2つの
役目）。

`Canvas.rgb_to_hsv(r, g, b) -> Tuple`と`Canvas.hsv_to_rgb(h, s, v) -> Tuple`
は2つの色モデルを変換する — RGB側は`rgba`と同じ0–255チャンネル、HSV
側は色相/彩度/明度それぞれ`0.0..1.0`。パレットを**派生させる**場面は
HSVの出番で、彩度を上げる・明暗ペアを近づける・色相をずらす、といった操作
はどれもHSVでは1行の式になるが、RGBにはそれに相当する単純な式がない。
`Canvas.hsv(h, s, v, a = 255) -> Long`は結果をそのまま詰める — `rgba`と
対になる形。

```culebra
inspect(Canvas.rgb_to_hsv(255, 0, 0))     # => (0.0, 1.0, 1.0)
inspect(Canvas.hsv_to_rgb(0.0, 1.0, 1.0)) # => (255, 0, 0)
inspect(Canvas.hsv(0.0, 1.0, 1.0) == Canvas.rgba(255, 0, 0))  # => true

# 基本色を HSV で 40% 彩度アップしてから詰める
let (h, s, v) = Canvas.rgb_to_hsv(180, 140, 200)
inspect(Canvas.hsv(h, Math.min(1.0, s * 1.4), v))  # => 4291327148
```

2つは0–255のグリッド上で厳密に往復する — `hsv_to_rgb`は各チャンネルを
丸めるので、`rgb_to_hsv`の結果をそのまま`hsv_to_rgb`に戻すとどの入力
でも元の`r, g, b`に一致する。見た目が近いだけの近似ではない。

### 描画

| 関数 | 効果 |
| --- | --- |
| `Canvas.init(w, h)` | フレームバッファを確保（またはリサイズ）する。`Canvas.run`が代わりに行う |
| `Canvas.clear(color)` | 描画先全体を`color`で置き換える（合成なし） |
| `Canvas.set_pixel(x, y, color)` | 1ピクセルをそのまま格納（範囲外は無視） |
| `Canvas.get_pixel(x, y) -> Long` | ピクセルを読む（範囲外は0）— ピクセル読み戻しの当たり判定用 |
| `Canvas.rect(x, y, w, h, color, fill = true)` | 矩形（クリップ） |
| `Canvas.line(x1, y1, x2, y2, color)` | 線分（両端点を含む） |
| `Canvas.circle(cx, cy, r, color, fill = true)` | `(cx, cy)`中心の円 |
| `Canvas.ellipse(cx, cy, rx, ry, color, fill = true)` | 軸ごとの半径を持つ楕円 |
| `Canvas.triangle(x1, y1, x2, y2, x3, y3, color, fill = true)` | 三角形 |
| `Canvas.polygon(points, color, fill = true)` | 平坦な頂点列からの多角形 |
| `Canvas.width()` / `Canvas.height() -> Long` | 現在の描画先の寸法 |
| `Canvas.to_png() -> String` | 現在の描画先のピクセルをPNGバイト列で返す |
| `Canvas.present()` | フレームを提示（下記ループ参照） |

フレームバッファとスプライトレジストリは1つのisolateのもので、最初に触れた
isolate（描画でも`width`/`get_pixel`のような読み取りでも）が持ち主になる。
2つ目のisolateは競合させる代わりに拒否され、空のキャンバス（ピクセルも
スプライトハンドルも無い）を見る。報告できる呼び出しは理由を返す — `init`と
`draw_to`/`to_png`は`RuntimeError`。他のisolateが一度も触れていなければ、
持ち主がworkerでも構わない。

すべての図形は`fill: false`で塗りの代わりに1ピクセル幅の輪郭を描く:
`rect`は同じ塗りの最外周リング、`circle` / `ellipse`はつながった縁、
`triangle` / `polygon`は`line`で描いた辺の閉じた連鎖（半開区間の塗りと
違い頂点を含む）。`line`は長軸を1ピクセルずつ歩き短軸座標を最近傍に
丸める。先に端点をソートするので`line(a, b)`と`line(b, a)`は同一の
ピクセルを描く。半径`r`の円は中心行で`2r + 1`ピクセルにわたり —
`(cx ± r, cy)`が円周上に乗る — 半径0は1ピクセル。負の半径は何も
描かない。

位置とサイズの引数はすべて`Long|Float`。`Float`は −∞ 方向に丸める（ピクセル
*n* が`[n, n+1)`を覆う）ので、隣接するスパンは隙間なく敷き詰まり、負の座標は
0列目に吸着せず範囲外のままになる。非有限値と範囲外の値は例外を投げず飽和し、
`Math.nan`は0になる。これにより、位置を浮動小数で計算するプログラム（投影、
スクロールオフセット）は1つずつ丸めずそのまま渡せる。色 / blitフラグ /
アルファ / スプライトハンドルは`Long`のまま。

`Canvas.polygon`の`points`は平坦な`[x0, y0, x1, y1, …]`の頂点列（3頂点
以上、他の座標と同じくLongかFloat）で、輪郭は自動で閉じる。末尾の半端な1個
は無視される。塗りはeven-odd規則なので、凹んだ輪郭は見たとおりに凹む。
`Canvas.triangle`は同じ塗りを3頂点で直接書く形で、慣習的な形状呼び出し
（raylib / SDL / GPUラスタライザはいずれも3頂点を取る）に合わせるとともに、
ホットパスで呼び出しごとに`Array`を作らずに済む。`Canvas.circle`は半径を
1つにした`ellipse`。どちらも各行の半幅を正確な整数平方根で求めるので、
どのbackendでも同一の曲線になる。

行とスパンは`rect`と同じ半開区間 — 行は「その行を縦スパンに含む辺」に属し、
塗るスパンは`[xl, xr)`。したがって辺を共有する図形は継ぎ目なく、二重描画もなく
敷き詰まる（矩形を対角線で2つの三角形に切ると、ラスタライズ結果は元の矩形に
正確に戻る）。補間は全て整数なので、どのbackendでも同一の形になる。座標は
±2³⁰ のガードバンドに飽和するので、どんな入力でも桁あふれしない。

### ウィンドウ

ウィンドウはデスクトップビルドが実際に開いたときにだけ存在します。ヘッドレスと
ブラウザには無く、下の入口はそこでは何もしません。`Canvas`はまずフレーム
バッファであり、ここだけがOSの持ち物です。

| 関数 | 効果 |
| --- | --- |
| `Canvas.title(name)` | ウィンドウ名を設定。ループが始まる前に呼ぶ |

ブラウザで何もしないのは未実装ではなく方針です。タブのタイトルはキャンバスを
載せているページのものであって、その上で動くプログラムのものではありません。

### スプライト

`Canvas.Sprite.new(pixels, w, h, palette = nil)`はスプライトを一度アップロード
し、毎フレーム安価にblitできるハンドルを返す。`pixels`は行優先の平坦配列:
packed RGBA `Long`、または`palette`を与えたときはそのパレットへのインデックス
（コンパクトなインデックスカラー画像）。完全透過のソースピクセルはスキップされ
（形マスク）、部分透過のピクセル — PNGのアンチエイリアス縁 — は既存の内容に
ブレンドされるので、スプライトは合成される。スプライトのピクセルは最後の参照が
消えたときに解放される。

`Canvas.Sprite.new(png: String)`は代わりにPNGのバイト列をデコードする。`String`
はバイト列なので、例えば画像ファイルの`FS.read`をそのまま渡せる。サイズは画像から
得るのでデータ以外に渡すものは無い。`Canvas.Sprite.from_png(data)`は呼び出し側で意図が読める名前を付けた同じ
もの。グレースケール / パレット（`tRNS`込み）/ トゥルーカラー / 16bit/chのいずれも
フレームバッファと同じpacked RGBAになる。デコードできない入力は
`ValueError: not a valid PNG image`。

| メソッド | 効果 |
| --- | --- |
| `sprite.draw(x, y, flip_x = false, flip_y = false, transpose = false)` | スプライト全体を`(x, y)`にblit。`transpose`はX/Y入替（対角反射 — flipと組み合わせれば90° 回転） |
| `sprite.draw_sub(x, y, sx, sy, sw, sh, flip_x = false, flip_y = false, transpose = false)` | サブ矩形をblit（スプライトシート用） |
| `sprite.draw_scaled(x, y, w, h, flip_x = false, flip_y = false, smooth = false, alpha = 255)` | `(x, y)`の`w`×`h`矩形に、収まるようリサンプルしてblit |
| `sprite.draw_sub_scaled(x, y, w, h, sx, sy, sw, sh, flip_x = false, flip_y = false, smooth = false, alpha = 255)` | 同じくサブ矩形から |
| `sprite.to_png() -> String` | スプライトのピクセルをPNGバイト列で返す（`from_png`の逆） |
| `sprite.width()` / `sprite.height()` | スプライト寸法 |

拡縮blitのサンプリングは最近傍なので、ドット絵を拡大しても輪郭は鮮明なまま。
`smooth`は縮小時にソースをボックス平均する（どちらの軸も縮小しないときは無視）。
拡縮版に`transpose`は無い。`alpha`（0–255）はblit全体をスケールする: 各
ピクセルは自身のソースアルファ × `alpha`で合成されるので、不透明なスプライトの
`alpha: 128`は半分ブレンド、その中の半透明の縁ピクセルは4分の1でブレンド
される。完全透過のソースピクセルは直接サンプルされてもボックス平均されても
スキップされ、寄与しない。転送先・転送元の矩形の辺が非正なら何も描かない。

### オフスクリーン描画

`Canvas.Sprite.blank(w, h, color = 0)`は空のスプライトを作り、
`Canvas.draw_to(sprite, fn () { ... })`はクロージャの間だけすべての描画
呼び出しをそこへ向ける — `clear`、図形、`text`、他スプライトの`draw`、
さらに`Canvas.width()` / `height()`と`get_pixel`も描画先に従うので、
中央寄せのコードはオフスクリーンでもそのまま動く。`present()`は常に
フレームバッファを表示する。前の描画先はどの脱出経路でも（throwでも）
復元され、`draw_to`は呼び出しスタックのようにネストする。

```culebra
# doctest: skip
let bgd = Canvas.Sprite.blank(320, 240)
Canvas.draw_to(bgd, fn () {          # 背景は一度だけ描く…
  Canvas.clear(sky)
  for i in 0..50 { Canvas.circle(Random.below(320), Random.below(240), 2, star) }
})
Canvas.run(320, 240, fn () {
  bgd.draw(0, 0)                     # …毎フレームは blit 1 回
  true
})
```

`ValueError`になるのは2つ: スプライトを自分自身に描くこと（blitが自分の
書き込みを読んでしまう）と、いま描画先になっているスプライトを解放すること。
どちらも他のCanvasエラーと同じくbackend対称。

### 画像の保存

`sprite.to_png()`と`Canvas.to_png()`はPNGバイト列を返すので、書き出しは
その結果を`FS.write`するだけ、読み戻しは既存の`from_png`です。
`Canvas.to_png()`は`width` / `height` / `get_pixel`と同じく **現在の描画先**
に追随します — `draw_to`の中なら描画中のスプライト、外ならframebuffer。

```culebra
# doctest: skip
Canvas.init(320, 240)
Canvas.clear(Canvas.rgba(24, 24, 32))
Canvas.circle(160, 120, 40, Canvas.rgba(240, 180, 90))
FS.write("shot.png", Canvas.to_png())          # スクリーンショット

let tile = Canvas.Sprite.blank(16, 16)         # オフスクリーンで描いてもよい
Canvas.draw_to(tile, fn () { Canvas.clear(Canvas.rgba(80, 200, 120)) })
FS.write("tile.png", tile.to_png())
```

出力は8bit truecolour + アルファ、`IDAT`は1個で、各行はスコアが最小に
なるフィルタで符号化されます — 平坦でディザのかかったピクセルアートなら
専用エンコーダに近いところまで縮みます。ピクセルが1つもない画像
（`Canvas.init(0, 0)`）と、解放済みのスプライトハンドルはどちらも
`ValueError`です。

### テキスト

`Canvas.text(s, x, y, color, scale = 1)`は内蔵8×8ビットマップフォント
（WASM-4ランタイムのフォント、印字可能ASCII 32–126の大文字・小文字を収録、
範囲外の文字はスキップ）で`s`を描く。フォントの各ピクセルは`scale`×`scale`
のブロックになり、送りも1文字`8 * scale` pxで追随する — `scale: 2`が
タイトル画面サイズ。非正のscaleは何も描かない。
`Canvas.text_width(s, scale = 1) -> Long`はピクセル幅を返す（中央寄せ /
右寄せ用）。

### 入力

入力は毎フレームのポーリング（イベントキューでなく現在の状態を反映する）。

| 関数 | 結果 |
| --- | --- |
| `Canvas.buttons() -> Long` | 押下中ボタンのビットマスク |
| `Canvas.mouse() -> Object` | フレームバッファ座標の`{x, y, buttons}` |
| `Canvas.key(name) -> Bool` | 名前で指したキーが今押されているか |
| `Canvas.key_queue() -> Array` | このフレームのキー押下（名前）を引き取る |
| `Canvas.typed() -> String` | ユーザーが打った文字を引き取る |

ボタンビットは定数`Canvas.LEFT` / `RIGHT` / `UP` / `DOWN`（矢印キー、および
第2のセットとしてWASD）と
`Canvas.A` / `B`（アクションキー — `A`はSpace/Z、`B`はX）。エッジ検出には
`Canvas.Input.new()`が前フレームを覚える:

| メソッド | 結果 |
| --- | --- |
| `input.update()` | このフレームのボタンをサンプル（毎フレーム1回） |
| `input.down(btn) -> Bool` | 今押されている |
| `input.pressed(btn) -> Bool` | **このフレーム**に押された（フラップのトリガ） |

6ボタンの外側は`Canvas.key`が任意のキーを名前で報告する — **`Term.read_key`
と同じ語彙**なので、キー処理コードは2つのnamespace間をそのまま移動できる:
印字可能文字（`"a"`、`" "`、`"-"`）か特殊キー名（`"up"` / `"down"` / `"left"` /
`"right"`、`"enter"`、`"escape"`、`"tab"`、`"backspace"`、`"insert"`、
`"delete"`、`"home"`、`"end"`、`"pageup"`、`"pagedown"`、`"f1"`…`"f12"`）。
`" "`の読みやすい別名として`"space"`も受け付ける。未知の名前は単に
押されていない扱い。英字はShiftに関係なく物理キーを指す（`"a"`が両方の
ケースを覆う）。

`Canvas.key_queue()`は前回呼び出し以降に押されたキーを返し（エッジイベント —
押しっぱなしでリピートさせないバインディング用）、`Canvas.typed()`は打たれた
文字（Shift / 配列 / IME適用済み）を返す — 名前入力画面が読むのはこちら。
どちらも破壊的に引き取り、上限256件（古い方から溢れる）なので、フレームごとに
1箇所で呼んで結果を配ること。ネイティブでは最初の`typed()`呼び出しが
プラットフォームのテキスト入力を有効化する — キーだけをポーリングする
プログラムがIMEポップアップを見ることはない。ヘッドレスでは何も押されず
キューは空。

### 音声

`Canvas.tone(freq, dur, vol = 100, wave = 0, end_freq = nil, attack = 0,
decay = 0, release = 0, peak = nil, duty = 2)`はWASM-4流の小さなAPUで音を
鳴らす。最も簡単な形`Canvas.tone(freq, dur)`は`freq` Hzの`dur`フレーム
（~60fps）の音。オプション引数でエンベロープ全体を扱える: 音程は`freq → end_freq`
にスライドし、ADSRエンベロープ（`attack`/`decay`/`release`はフレーム数、`dur`
はサステイン長）が音量を0 → `peak` → サステインの`vol`（0–100）→ 0と整形する。
`wave`はチャンネルを選ぶ — `Canvas.PULSE` / `PULSE2`（`duty`サイクル付き:
`Canvas.DUTY_EIGHTH` / `DUTY_QUARTER` / `DUTY_HALF` / `DUTY_THREE_QUARTER`）、
`Canvas.TRIANGLE`、`Canvas.NOISE`、またはculebra拡張の`Canvas.SAWTOOTH`。各
チャンネルはモノフォニック（新しい音が前の音を止める）。合成波形は生で、4
チャンネルが同時に鳴りうるので、`tone`は0–100をフルスケールよりかなり低く
ミックスする — ファイル音源（`Sound`・`music`）はすでにミックス済みなので
`vol = 100`はファイル自身のレベルで鳴る。

ブラウザでは`tone`はWebAudioで鳴る（チャンネルごとのオシレータ、pulseのduty
サイクル用`PeriodicWave`、フィルタ済みノイズバッファ）。ネイティブではraylibの
オーディオスレッドで鳴らす小さなソフトウェアシンセ（帯域制限していない素朴な
オシレータ、noiseはフィルタ済みPRNG）で、初回使用時に遅延初期化するので
`tone`を一度も呼ばないプログラムは音声デバイスを開かない。ヘッドレスなネイティブ
では無音、音声デバイスの無いマシンでも無音のまま（デバイスを開かず、クラッシュも
しない）。ネイティブ音声とWebAudioは**似た音になるよう調整した別実装**であって
サンプル単位で同一ではない — ピクセル操作と違い`tone`はbackend間でビット完全一致
である必要はない。

### 効果音

`Canvas.Sound.new(data)`はワンショットのサンプルをバイト列 — WAV / MP3 /
Ogg Vorbis、`Sprite.from_png`と同じバイト列渡しの流儀 — からデコードし、
呼び出しごとに再生する。`tone`の合成に対する録音サンプル側の対応物
（スイープする矩形波でなくファイルの爆発音）。

| メソッド | 効果 |
| --- | --- |
| `sound.play(vol = 100)` | 先頭から再生（再生中なら先頭からやり直し） |
| `sound.stop()` | 停止 |
| `sound.playing() -> Bool` | まだ鳴っているか |

各`Sound`は1ボイス — 再生中の`play`はやり直しになる。下にいるホストの
サンプラーと同じ挙動 — で、デコード済みサンプルは最後の参照とともに解放される。
3形式のどれでもないバイト列はどのbackendでも
`ValueError: not a valid WAV, MP3 or Ogg audio stream`を投げる。検査を通った
のにデコードできないストリームは無音のまま。ヘッドレス（および音声デバイスの
無いマシン）ではすべてno-opで`playing()`はfalse — `music`と同じ。

### 音楽

`Canvas.music(data, loop = true, vol = 100, start = 0.0)`はMP3 / Ogg Vorbis
ファイルをそのバイト列（`String`、例えば`FS.read`の結果 — `Sprite.from_png`と
同じバイト列渡しの流儀）から再生する。スロットはpygameの`mixer.music`と同じく
**1つだけ**: 新しいファイルを再生すると前のものは置き換わり、ハンドルは
スクリプトに渡らない。`vol`は0–100で`100`がファイル自身のレベル、`start`は
ファイル内の秒位置。MP3でもOggでもないバイト列はどのbackendでも
`ValueError: not a valid MP3 or Ogg audio stream`を投げ、この検査を通ったのに
デコードに失敗したストリームは無音のままになる。

| 関数 | 効果 |
| --- | --- |
| `Canvas.music(data, loop = true, vol = 100, start = 0.0)` | デコードして再生（再生中のファイルは置換） |
| `Canvas.music_stop()` | 停止してアンロード |
| `Canvas.music_pause()` / `Canvas.music_resume()` | 一時停止 / 停止位置から再開 |
| `Canvas.music_volume(vol)` | 音量変更（0–100） |
| `Canvas.music_seek(seconds)` | 位置ジャンプ |
| `Canvas.music_playing() -> Bool` | いま鳴っているか |

何もロードされていないときの各操作はno-opで`music_playing()`は`false` —
`tone`の「clampして投げない」流儀に合わせ、フォーマット検査だけが唯一の
エラー。ネイティブではストリームを逐次デコードし、そのバッファは`present()`
から補充されるので、**音楽はフレームをpresentしている間だけ進む** — present
しなくなったプログラムは音楽も一緒に止まる。ヘッドレス（および音声デバイスの
無いマシン）ではすべてno-op。ブラウザではファイルのデコードはWebAudioが行い、
Ogg対応はブラウザ依存（Safariは歴史的にMP3のみ）である点に注意。ネイティブは
両フォーマットとも常に再生できる。

### ゲームループ

`Canvas.run(w, h, tick, frames = 600)`は`w`×`h`のフレームバッファを用意し、
`tick()`を毎フレーム呼んで各回のあとにpresentする。`tick`が`false`を返すと
停止（例: プレイヤーが終了）。対話的なPlaygroundビルドでは`present()`が
ブラウザの次のアニメーションフレームを待つので、ループは自らペーシングし協調的に
譲る。それ以外の場合は`frames`後に停止するので、誰も終われない実行が無限に回り
続けることはない。無制限に回り続けるには両方の条件が要る: フレームが実際に画面へ
届いていること（ウィンドウバックエンド無しのビルド、`CULEBRA_CANVAS_HEADLESS`、
ディスプレイの無いマシンはいずれも該当しない）と、実行が対話的であること（パイプ
されたネイティブ実行と非JSPIブラウザは該当しない）。端末からのヘッドレス実行は
後者しか満たさない — 何も表示せず、入力も受けず、閉じるボタンも無い — ので、他の
自動実行と同じように上限が掛かる。

```culebra
# doctest: skip
let red = Canvas.rgba(220, 60, 60)
mut x = 0
Canvas.run(160, 160, fn () {
  Canvas.clear(Canvas.rgba(20, 24, 40))
  Canvas.rect(x, 76, 8, 8, red)
  x = (x + 2) % 160
  true
})
```

---

## 27. `Scene`

手続きジオメトリから組み立てる3D用のretained-modeレンダラ。ノードの
シーングラフ — プリミティブ（box / sphere / cylinder / plane）と手組みメッシュ
— を並べ、マテリアルとトランスフォームを与え、カメラを置いて描画する。
ライティングは物理ベース（metallic / roughnessマテリアル、2カスケード影付き
の指向性sun、sky / fog、SSAA・アンビエントオクルージョン・bloom・被写界深度の
post stack）なので、出力はフラットシェーディングのプリミティブ以上になる。

`Scene`は **ゲームエンジンではない**。物理・当たり判定なし、モデル / テクスチャ
のimportなし（ジオメトリは手続き生成か頂点単位の組み立て、テクスチャはプロセス
内生成）、スケルタルアニメーションなし、マウス入力なし。狙いは*組み立てる*
3D — 可視化、手続き的シーン、チェイスカメラ付きの車両 / フライトデモ — であって、
アセット駆動のゲームではない。サーキットのメッシュ、チェイスカメラ、
ゲームパッド操作といったレーシングデモの形が、設計の基準になっている。

`Scene`は **opt-inで現状macOS限定**。デフォルトビルドには入らない。
`-DCULEBRA_ENABLE_SCENE=ON`で有効化すると、vendoredな静的SDL3 + raylib
バックエンドをビルドする。Linux / Windowsとブラウザ向けのウィンドウ
バックエンドはまだ無いので、`Canvas`と違い`Scene`プログラムはヘッドレスでも
Playgroundでも動かない。

### View とフレームループ

`Scene.View.new(w, h, title)`はウィンドウを開く。開けない場合
（使えるディスプレイ / GLが無い）は`RuntimeError`を送出する — `Canvas`と
違いfallbackすべきヘッドレスモードは無い（`View`の観測可能な動作はすべて
GPUを必要とするため）。位置とサイズは`Float`
（ワールド単位）、色は`0–255`の3または4チャンネル整数で、範囲外の
チャンネルは端に丸められる。1フレームは、
2Dオーバーレイ付きの3Dパス（`render_3d()` → オーバーレイ描画 → `present()`）
か、純2D（`begin2d()` → 描画 → `present()`）のいずれか。

| メソッド | 効果 |
| --- | --- |
| `view.target_fps(fps)` | フレームレート上限 |
| `view.closing() -> Bool` | ウィンドウ閉じ要求（trueまでループ） |
| `view.dt() -> Float` | 前フレームからの秒数 |
| `view.width()` / `view.height() -> Float` | ウィンドウ寸法 |
| `view.camera(px,py,pz, tx,ty,tz, ux,uy,uz, fov)` | 視点位置・注視点・upベクトル・垂直FOV |
| `view.render_3d()` | シーングラフを描画し、2Dオーバーレイ用にフレームを開く |
| `view.begin2d()` | 純2Dフレームを開く（3Dパスなし） |
| `view.present()` | フレームを確定して提示 |

### シーングラフ

`view.add_node()`は空ノードを追加。`add_*`ヘルパーはジオメトリを追加して新しい
ノードを返す。ノードは入れ子（`node.add_node()`、`node.add_box()` …）にでき、
トランスフォーム系メソッドはfluentなので、部分木を1式で組める。永続的な
ジオメトリは一度組んで、毎フレーム動かす。

| メソッド | 効果 |
| --- | --- |
| `view.add_box(w, h, d)` / `add_sphere(r)` / `add_cylinder(r, h)` / `add_plane(w, d)` | プリミティブノードを追加（`node`からも同じ形状を子として追加できる） |
| `view.add_mesh()` / `node.add_mesh()` | 空のカスタムメッシュを追加（下記） |
| `node.move(x, y, z)` | 位置を設定 |
| `node.yaw(a)` / `pitch(a)` / `roll(a)` | 1軸まわりの回転（ラジアン） |
| `node.spin(x, y, z, a)` / `euler(x, y, z)` | 軸角 / オイラー回転 |
| `node.scale(s)` / `scale3(x, y, z)` | 一様 / 軸別スケール |
| `node.tint(r, g, b)` | ノード単位の色 |
| `node.material(id)` | マテリアルを割り当て（下記） |
| `node.hide()` / `show()` / `name(n)` | 可視性 / ラベル |
| `node.x()` / `y()` / `z() -> Float` | 位置を読み戻す |

カスタムメッシュは頂点と三角形から組み、最後に確定する: `m.vertex(x, y, z, nx,
ny, nz)`（または`vertex_uv(…, u, v)`）が頂点、`m.tri(a, b, c)`が頂点インデックス
での三角形、`m.build()`がアップロード。（raylibは16bitインデックスバッファ
なので1メッシュは65535頂点が上限。超過は`build()`が拒否する。）アップロード済み
メッシュはアップロードしたviewのもので、`view.drop()`後も残したnodeは変換としては
使えるが、後続のviewでは何も描かない。

### マテリアル・ライティング・テクスチャ

マテリアルはview上で作り、idで参照する:

| メソッド | 結果 |
| --- | --- |
| `view.material(r, g, b) -> id` | フラット色マテリアル |
| `view.material_pbr(r, g, b, metallic, roughness) -> id` | PBRマテリアル（`metallic` / `roughness`は0–1） |
| `view.material_tex(tex, r, g, b) -> id` / `material_tex_pbr(tex, r, g, b, metallic, roughness) -> id` | テクスチャ付きマテリアル |

テクスチャはプロセス内生成（画像ファイルローダは無い）:
`view.checker(px, checks, r1,g1,b1, r2,g2,b2) -> tex`は市松、
`view.grain(px, r, g, b, amt) -> tex`はノイズ、`view.canvas(w, h) -> tex`は
2D呼び出し（`rect` / `text` …）で塗るrender-to-textureを開き、
`view.canvas_end()`で閉じる — デモがリバリーや看板を描くのに使っている方法。
canvasは同時に1枚だけ: 閉じる前の2枚目の`canvas()`は拒否され、開いたまま
フレームを開くと先に閉じられる。テクスチャはUVの向きに関わらずメッシュ上で
正しい向きになる。

ライティングはview上で設定する:

| メソッド | 効果 |
| --- | --- |
| `view.background(r, g, b)` | クリア色 |
| `view.sky(tr,tg,tb, br,bg,bb)` | 天頂 → 地平のグラデーション（反射環境も兼ねる） |
| `view.sun(dx,dy,dz, intensity, r,g,b)` | 指向性ライト（2カスケード影）。`(0, 0, 0)`は方向を指さないので拒否される |
| `view.ambient(intensity, r, g, b)` | フィルライト |
| `view.fog(start, end, r, g, b)` | 距離フォグ |
| `view.screenshot(path)` | 現フレームをPNGに保存 |

### 2D オーバーレイ

`render_3d()`（または`begin2d()`）の後、これらが上に描かれる（HUD用）:

| メソッド | 効果 |
| --- | --- |
| `view.text(s, x, y, size, r, g, b)` | テキスト描画 |
| `view.rect(x, y, w, h, r, g, b)` | 塗り矩形 |
| `view.circle(x, y, radius, r, g, b)` | 塗り円 |
| `view.line(x0, y0, x1, y1, thick, r, g, b)` | 線 |
| `view.alpha(a)` | 以降のオーバーレイ描画の不透明度（0–255） |

### 入力

入力は毎フレームviewからポーリングする。キーボードのキーやゲームパッドの
軸 / ボタンは生の整数コード（raylibキーコード、SDLゲームコントローラの
インデックス）で、名前付き定数は無い:

| メソッド | 結果 |
| --- | --- |
| `view.held(key) -> Bool` | キーが押下中（例: `262`–`265` = 矢印、`32` = space） |
| `view.pressed(key) -> Bool` | このフレームで押された |
| `view.pad_available() -> Bool` | ゲームパッド接続あり |
| `view.pad_axis(n) -> Float` | 軸の値（スティック、トリガー） |
| `view.pad_button(n) -> Bool` / `pad_pressed(n) -> Bool` | ボタン押下中 / 今押された |
| `view.rumble(left, right, sec)` | ハプティクス（SonyパッドとXInput。Xbox × macOSは無音） |
| `view.pad_name() -> String` / `view.gamepad_mappings(db)` | パッド識別 / SDLマッピングDB読込 |

### 音声

`Scene.Sound.new(path)`はワンショット効果音、`Scene.Music.new(path)`は
ストリーム再生トラック。どちらもファイルパスを取り、`volume(v)`・`pitch(p)`・
`pan(p)`を持つ。`Sound`は`play` / `stop` / `playing`、`Music`は`pause` /
`resume` / `looping(on)`を加え、バッファを供給し続けるため毎フレーム
`update()`を呼ぶ必要がある。

### 最小のシーン

```culebra
# doctest: skip
let view = Scene.View.new(960, 540, "spinner")
view.target_fps(60)
view.background(30, 34, 42)
view.sun(0.5, -0.8, -0.3, 1.2, 255, 245, 230)
view.ambient(0.4, 180, 200, 220)

let gold = view.material_pbr(230, 180, 60, 0.9, 0.3)
let box = view.add_box(2.0, 2.0, 2.0).material(gold)

mut a = 0.0
while !view.closing() {
  a += view.dt()
  box.yaw(a)
  view.camera(4.0, 3.0, 5.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 55.0)
  view.render_3d()
  view.text("culebra scene", 20.0, 20.0, 28, 235, 235, 240)
  view.present()
}
view.drop()
```

---

## 28. `Net`

生のTCP / UDPソケットと名前解決 — [§15 Http](#15-http) の下位レイヤ。
ブロッキング + ソケット単位のタイムアウトで、`Http`と同じ「asyncではなく
スレッド」モデル。メインスレッドを止めたくないサーバは
[`Isolate`](#12-isolate) の中で動かす。

転送の失敗（接続拒否・名前解決失敗・切断・タイムアウト）は`NetError`を
送出する。メッセージはOSの文言をそのまま使うが、タイムアウトだけは常に
`timed out`。ブロック中の`read` / `accept` / `recv_from`は割り込み可能で、
Ctrl+C一回で`Interrupted`になる（ハングしない）。

ソケットハンドルは **Sendableではない** — ソケットはそれを開いたスレッドに
属する。別スレッドで待ち受けるなら、そのアイソレートの中で開くこと。

### `Net.connect(host: String, port: Long, timeout: Long = 0) -> Socket`

`host:port`へ接続する。`timeout`はミリ秒（`0` = 無制限）で、接続の待ち時間を
制限し、そのままこのソケットの読み書きタイムアウトになる。

```culebra
# doctest: skip
let s = Net.connect("example.com", 80, timeout: 5000)
s.write("GET / HTTP/1.0\r\nHost: example.com\r\n\r\n")
s.shutdown_write()                  # リクエスト完了をサーバに伝える
inspect(s.read())                      # サーバが閉じるまで読む
s.close()
```

**`Socket`ハンドル** — [§4 File](#4-file) と同じreader/writerの形なので、
読むだけのコードは両方で動く:

| メソッド | 効果 |
| --- | --- |
| `read(n = nil)` | 最大`n`バイト（ソケットでは短い読み取りが正常）。`nil`は相手が閉じるまで読む。EOFでは`""` |
| `read_line()` | 1行（行末は除去）。ストリームが終わったら`nil` |
| `read_exact(n)` | ちょうど`n`バイト。相手が途中で閉じたら短い読み取りではなく`NetError` |
| `lines()` | 行イテレータ。相手が閉じると終了する。ファイルの`f.lines()`と違いソケットは閉じない（通常この後に返信するため） |
| `write(data)` | `data`を全バイト書く |
| `shutdown_write()` | ハーフクローズ: 返信を読みながら相手にEOFを伝える |
| `local_addr()` / `peer_addr()` | 自分側 / 相手側の`{host, port}` |
| `set_timeout(ms)` | 読み書きのタイムアウト（ms）。`0`は無制限 |
| `set_nodelay(on = true)` | Nagleアルゴリズムを無効化（小さい書き込みを即送信） |
| `is_open()` / `close()` | 生存確認 / クローズ（冪等。スコープを抜けたハンドルはGCも閉じる） |

行の終端は`\n`のみで、末尾の`\r`は除去される — CRLFプロトコルがそのまま
読める。（ファイルの`f.lines()`は単独の`\r`でも区切るが、ソケットでは不可能: CRLFが
2つのパケットに分かれて届いたときにブロックしうる1バイト先読みが必要になる。）

### `Net.listen(port: Long, host: String = "0.0.0.0", backlog: Long = 0) -> Listener`

bindしてlistenする。`port: 0`はOSに空きポートを選ばせ、`listener.port`で
読み戻せる — テストで固定ポートの衝突を避ける確実な方法。

```culebra
# doctest: skip
let server = Net.listen(7000)
println("listening on " + server.port.to_string())
for conn in server {                    # ループで accept
  conn.write("hello " + conn.peer_addr().host + "\n")
  conn.close()
}
```

| メンバ | 効果 |
| --- | --- |
| `port` / `host` | 実際にbindしたアドレス（エフェメラルポートもここに出る） |
| `accept()` | 接続が来るまでブロックし、`Socket`を返す |
| `for conn in listener` | ループでacceptする。止めるときは本体で`break` |
| `serve(handler, workers = 0)` | ループでacceptし、`handler(conn)`をワーカープールで実行する。中断されるまでブロック |
| `set_timeout(ms)` | `accept`が待つ上限（超えたら送出）。acceptしたソケットもこれを引き継ぐ |
| `is_open()` / `close()` | `Socket`と同じ |

`accept`と`for conn in listener`は逐次処理で、ハンドリングもacceptした
スレッドで走る — 遅い接続が次の接続をブロックする。

### `listener.serve(handler, workers = 0)` — 並行版

`serve`はacceptを続けながら、ハンドラをワーカースレッドのプールで実行する。
遅い接続がacceptループを止めることはない:

```culebra
# doctest: skip
let server = Net.listen(7000)
server.serve(fn(conn) {
  for line in conn.lines() { conn.write(line.upper() + "\n") }
}, workers: 8)                      # Ctrl+C までブロック
```

- `workers: 0`（既定）はCPU数に応じたプール（最小4、最大8）。正の数を渡せば
  固定できる。各ワーカーが自分のランタイムを持つのでグローバルロックはなく、
  **真に並列**に処理される。
- 各ワーカーがハンドラを自分のヒープに再構築するため、ハンドラは **Sendable**
  でなければならない（可変変数や非Sendableな値をキャプチャできない）。非
  Sendableなハンドラは最初の接続時ではなく`serve`自身が`SendError`を送出
  する。読み取り専用データの共有は [`Shared.new`](#12-isolate)、接続ごとの
  リソースはハンドラの中で開く。
- ハンドラがreturnすると接続は閉じられる。ハンドラが送出した場合は **その
  接続だけ** が閉じられ、ワーカーもサーバも動き続ける（`Http.server`と違い、
  `500`に変換できるレスポンスが存在しないため）。
- `serve`はブロックする。Ctrl+C（またはこれを動かしているアイソレートのdrop）
  でacceptループが止まり、実行中のハンドラを待ってから`Interrupted`を送出
  する。メインスレッドで別の作業を続けたいなら
  [`Isolate`](#12-isolate) の中で動かす。

HTTPを話すなら [`Http.server`](#httpserver---object) を使う — 同じワーカー
モデルの上に、ルーティング・静的ファイル・ストリーミング・WebSocketを備える。

### `Net.udp(port: Long = 0, host: String = "0.0.0.0") -> UdpSocket`

`host:port`にbindしたデータグラムソケットを開く（`port: 0` = エフェメラル）。

```culebra
# doctest: skip
let sock = Net.udp(9000)
sock.set_timeout(2000)
let msg = sock.recv_from()              # {data, host, port}
sock.send_to("ack", msg.host, msg.port)
```

| メンバ | 効果 |
| --- | --- |
| `port` / `host` | bindしたアドレス |
| `send_to(data, host, port)` | データグラムを1つ送る（全部送るか送らないかで、部分書き込みはない） |
| `recv_from(max = 65536)` | データグラムを1つ`{data, host, port}`で受け取る。UDPの仕様どおり、`max`を超えた分は切り捨てられる |
| `set_broadcast(on = true)` | ブロードキャストアドレスへの送信を許可 |
| `set_timeout(ms)` / `is_open()` / `close()` | `Socket`と同じ |

UDPにはEOFも接続もない。空のデータグラムはデータであり、相手がいなくなっても
エラーではなく単に無音になる。

### `Net.resolve(host: String) -> Array<String>`

`host`が解決する数値アドレスを、リゾルバの順序で重複を除いて返す。数値
アドレスはそれ自身に解決する。解決できない名前は`NetError`。

```culebra
# doctest: skip
inspect(Net.resolve("localhost"))       # => ["127.0.0.1", "::1"]
```

### Playground では使えない

ブラウザに生のソケットはないので、WebAssemblyビルドでは`Net`のすべての
呼び出しが`NetError: networking is not available in this build`を送出する。

---

## 29. `Desktop` / `Webview`

Web技術で書くデスクトップGUI: ローカルHTTPサーバがUIを配信し、
**ネイティブWebView** ウィンドウがそれを表示し、全体が1バイナリに
なります。`Webview`はOS自身のエンジン（macOSはWKWebView、Linuxは
WebKitGTK、WindowsはWebView2）をラップするので、ブラウザは同梱しません。
どちらの名前空間もデフォルトビルドに含まれます（`-DCULEBRA_ENABLE_WEBVIEW=OFF`
で無効化。エンジンのヘッダが無ければ自動で無効化 — LinuxならGTK4 /
WebKitGTKのdevパッケージ、Windowsなら`WebView2.h`）。
`culebra build`は実際に参照しているプログラムに対してのみ
WebViewフレームワークをリンクします。

これらのヘッダが要るのはビルド時だけです。バイナリはウィンドウ生成時に
エンジンをロードします（Windowsはvendoredヘッダ自身のローダー、Linuxは
`dlopen`）。したがってエンジンの無い機械でも起動し、ウィンドウを開かない
プログラムはそこで問題なく動きます。エンジンが無いままウィンドウを要求すると
`webview: failed to create window`になります。

### `Desktop.run(config: Object) -> Nil`

1呼び出しのfacade: サーバを起動し、その上にウィンドウを開き、ウィンドウが
閉じるまでブロックし、閉じたらサーバを止めます。

| キー | デフォルト | 意味 |
| --- | --- | --- |
| `title` | `'culebra'` | ウィンドウタイトル |
| `size` | ウィンドウ既定値 | `[width, height]`（ピクセル） |
| `assets` | — | `/`に配信する静的ルート。通常は`Embed.dir('dist')` — devではディスクから読み、`culebra build`ではバイナリに焼き込まれる |
| `routes` | — | `fn (srv) { ... }`。アプリ自身のルートを`Http`サーバ（§15）に登録する |
| `port` | `8731`、だめならOS任せ | サーバがbindするloopbackポート。未指定: まず`8731`を試し、使用中なら（別のculebraデスクトップアプリなど）OSが選ぶ空きポートに切り替える — ポートが変わるとページのorigin、つまり`localStorage`も変わる点に注意。明示指定: そのポートにbindするか失敗する |
| `workers` | `4` | サーバのワーカースレッド数 |

`POST /__quit`ルートが自動登録されるので、ページ側からアプリを閉じられます
（`fetch('/__quit', {method: 'POST'})`）。culebra側からは`Desktop.quit()`。

```culebra
# doctest: skip
Desktop.run({
  title: 'culebra desktop',
  size: [720, 560],
  assets: Embed.dir('dist'),
  routes: fn (srv) {
    srv.get('/api/hello', fn (req) {
      { content_type: 'application/json',
        body: JSON.stringify({ message: 'hello' }) }
    })
  }
})
```

### `Webview.Window` — 生のバインディング

サーバが要らない場合（インラインHTML、あるいはリモートURL）はこちらを
直接使います。

| メソッド | 効果 |
| --- | --- |
| `Webview.Window.new()` | ウィンドウを生成 |
| `w.set_title(title)` | タイトル設定 |
| `w.set_size(width, height)` | サイズ変更 |
| `w.set_html(html)` | HTML文字列を読み込む |
| `w.navigate(url)` | URLを読み込む |
| `w.run()` | ネイティブイベントループを実行（終了までブロック） |
| `w.terminate()` | このウィンドウの`run()`を終了 |
| `Webview.Window.quit()` | いま`run()`中のウィンドウを終了 — 別スレッド（例: HTTPハンドラ）から呼べる |
| `Webview.Window.is_running()` | ウィンドウがイベントループを回している間`true`。`quit()`の前提ではない（早く届いたquitは保持される）が、別スレッドがループの起動を待てる |

```culebra
# doctest: skip
let w = Webview.Window.new()
w.set_title('hello')
w.set_size(480, 320)
w.set_html('<h1>hi from culebra</h1>')
w.run()
```

**ウィンドウが見えるようになるタイミング。** macOSでは、ページが最初のフレームを
画面に出すまでウィンドウを透明にしてあります。空の白い矩形が一瞬見えてから中身が
出るのではなく、最初から中身が見えます。フレームを報告しないページ（JavaScript
無効、完了しないナビゲーション）は1.5秒後に空のまま表示します — 見えないままより
はましだからです。WindowsとLinuxは`set_size`の時点でウィンドウを表示するので、
空のフレームは依然として一瞬見えます。

---

## 30. 設計上の注記

### 名前空間ファースト、グローバルは CLI のエイリアス

ライブラリ自体はmatcher一族以外の**グローバル名を追加しません**。
それ以外の関数は`Math`, `IO`, `Random`, `Sys`のいずれかに属します。
これにより`culebra::environment()`はホストアプリケーションに埋め込
むスクリプトエンジンとして、意図しないグローバルを持ち込まない形に
なります。

ただしCLIスクリプトで`inspect` / `print` / `println`は頻出する
ため、毎回`IO.inspect`と書くのは摩擦が大きい。CLIバイナリ
（`src/main.cc`）は環境構築直後にこれらをグローバルとしてインストール
します。指す関数値は`IO`配下と同一なので重複はありません。V8が
同様のアプローチを採っており、エンジン自体は`print`を提供せず、
`d8`シェルが後付けで導入しています。

### 名前空間はファーストクラス値

すべてのstdlib名前空間（`Math`, `IO`, `FS`, `Random`, `Sys`,
`Tensor`, `JSON`）は`Object`です。変数に束縛したり、関数引数
として渡したり、コレクションに格納でき、そのバインディング経由
のメソッド呼出は直接呼出と同じ意味論を保ちます:

```culebra
let io = IO
io.inspect("hello")              # IO.inspect("hello") と同じ

fn run_with(ns, x) { ns.inspect(x) }
run_with(IO, "via parameter")
```

両backendがこれを保証します。JIT/AOTのスローパスはruntime
ディスパッチャ（`stdlib_jit.h::kNsMethods`）を経由し、構文的
ファストパス（`IO.inspect(x)`直接呼出）は従来のinline IR生成を
保ちます。

### 自由関数 vs メソッド

自由関数（名前空間内）は、無から値を構築する場合（`iota`,
`IO.input`）、複数の型に等しく適用される場合（`type_of`,
`to_string`）に使います。特定の型に関する操作はメソッド構文を
用いますが、その設計方針は言語仕様 §18（String/Array/Object
メソッド）を参照してください。

### エラー送出 vs `nil` 戻り値

回復不能な型エラー（`to_long('abc')`、存在しないファイルへの
`FS.read(...)`など）は例外送出を優先し、「見つかるかどうか」の
述語はセンチネルを返す方針です（`IO.input()`はEOFで`''`）。
`try`/`catch`なしでホットパスを簡潔に保つためです。

---

## 31. 未収録（将来検討）

### 重量級データ構造

`Queue` / `Deque` / 優先度ヒープはありません。`Set`と`Tuple`は
言語組込みです（[`docs/language.ja.md`](language.ja.md) 参照）。それ以外は
`Array`と`Object`で代用してください。

### OS 拡張

ファイル監視はありません。生ソケットのTLSもありません（`Net`は平文で、
TLSは [§15 Http](#15-http) が自前で持ちます）。必要なら
[§11 Proc](#11-proc) でサブプロセスに委譲してください。

---

関連: 言語仕様は [`docs/language.ja.md`](language.ja.md) にあります。
