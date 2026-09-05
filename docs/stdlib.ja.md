# Culebra 標準ライブラリ

本書はCulebraの**組み込みライブラリのAPIリファレンス**です。
ランタイムユーティリティをまとめた名前空間オブジェクト
（`Math`, `IO`, `Sys`, `FS`, `Time`, `Args`, `Random`, `String`）
を対象とします。ここに記載のものは`import`文なしで利用できます。

実例つきの導入とイディオムは [`handbook.ja.md` §14](handbook.ja.md#14-標準ライブラリ)
を参照してください。

言語レベルの組み込み関数（`to_long`, `to_float`, `to_string`,
`type_of`, `range`, `iota`, `repeat`）は [言語仕様 §19](language.ja.md)
を参照してください。matcher一族 (`assert_true` / `assert_eq` /
`assert_throws`等) は [§13 Matchers](#13-matchers) で扱います。
組み込み型（`String`, `Array`, `Object`）のメソッドは
[言語仕様 §18](language.ja.md) に規定されています。

これに加えて、`inspect`・`print`・`println`が`IO.inspect` / `IO.print` /
`IO.println`のエイリアスとしてグローバルに束縛されます（[言語仕様
§22](language.ja.md)参照）。これは無条件なので、埋め込みで動かす
プログラムからも、スクリプトと同じように見えます。

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
6. [`Random`](#6-random) — シード可能なPRNG（uniform / gauss / shuffle / choice / weighted_choice）
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
26. [`Canvas`](#26-canvas) — ゲーム向けイミディエイトモード2Dフレームバッファ（図形 / スプライト / オフスクリーン描画先 / テキスト / キー・マウス・ゲームパッド / ウィンドウ制御 / tone / 効果音 / music）
27. [`Scene`](#27-scene) — 手続きジオメトリ向けのretained-mode 3Dレンダラ
28. [`Net`](#28-net) — 生のTCP / UDPソケットと名前解決（`Http`の下位レイヤ）
29. [`Desktop` / `Webview`](#29-desktop--webview) — ネイティブWebViewのデスクトップアプリ: ローカルHTTPサーバ + ウィンドウを1呼び出しで
30. [`Vector2`](#30-vector2) — グラフィックス/ゲーム向けの最小限の2D floatベクトル（「Point」の代わりも兼ねる）
31. [`Vector3`](#31-vector3) — `Vector2`の3D版
32. [`Deque`](#32-deque) — 両端キュー、両端のpush/popが償却O(1)
33. [`PriorityQueue`](#33-priorityqueue) — 二分ヒープ、push/popがO(log n)
34. [`PEG`](#34-peg) — PEGパーサジェネレータ。文法を書くと構文木が返る
35. [`CodeGen`](#35-codegen) — 小さな言語のIRを手で組み立てて実行する
36. [`StateMachine`](#36-statemachine) — 入れ子にできる状態機械。テキストでも書ける
37. [`FST`](#37-fst) — 書き換えない辞書を圧縮して持つ。前方一致・補完・あいまい検索
38. [`Search`](#38-search) — 自分の文書を全文検索して順位をつける（実験的）
39. [設計上の注記](#39-設計上の注記)
40. [未収録（将来検討）](#40-未収録将来検討)

**目的別索引**

| やりたいこと | 参照先 |
|---|---|
| 定数（π、e、inf、nan） | [§1 Math 定数](#math-pi) |
| スカラー演算（abs / min / max / log / exp / sqrt / floor / ceil / round） | [§1 Math](#1-math) |
| 三角関数（sin / cos / tan / asin / acos / atan / atan2、ラジアン） | [§1 Math](#1-math) |
| 標準出力 | `IO.inspect`（改行 + クォート付き） / `IO.println`（改行、生） / `IO.print`（生、改行なし） |
| 呼び出しが印字した内容を読み戻す | [§2 IO](#2-io) — `IO.capture(fn)` |
| ファイル全体を読む | `FS.read`（失敗時throw） |
| ファイルをストリーム（行 / チャンク / seek） | [§4 File](#4-file) — `File.open` / `File.with` |
| パス操作（join / basename / dirname / stem / extension） | [§3 FS](#3-fs)；流暢な`Path`ラッパ: [§3 `Path`](#path--流暢なラッパ) |
| stat / walk / glob / copy / rename / symlink / chmod / chown | [§3 FS](#3-fs) |
| ディレクトリの変更監視 | [§3 FS](#3-fs) — `FS.watch` |
| ディレクトリ列挙・作成・削除 | `FS.list_dir`、`FS.mkdir`、`FS.remove` |
| `Instant` / `Duration`クラス、ISO 8601、カレンダー算術 | [§5 Time](#5-time) |
| 負になりうるインデックスを`0..n`に巻き戻す | [§1 Math](#1-math) — `Math.wrap(i, n)`（`%`は切り捨てなので負のまま） |
| 乱数 | `Random.int`、`.uniform`、`.gauss`、`.shuffle`、`.choice`、`.weighted_choice` |
| CLI引数解析 | [§10 Args](#10-args) |
| プロセス情報 | `Sys.argv`、`Sys.exit`、`Sys.env`、`Sys.set_env`、`Sys.getcwd`、`Sys.chdir`、`Sys.executable`、`Sys.script` |
| プロセスを越えて残すデータの保存先 | [§7 Sys](#7-sys) — `Sys.data_dir("myapp")`（プラットフォームごとのユーザーデータディレクトリ） |
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
| 固定レイアウトデータをスレッド/プロセス間で共有（zero copy） | [§12 SharedBuffer](#sharedbuffer--zero-copy-で共有する固定レイアウトデータ) — `SharedBuffer.new(n, FloatPair)` / `.file` / `.shared` |
| 可変長のread-onlyデータをスレッド間で共有（コピーなし） | [§12 Shared](#shared--参照共有する-immutable-値) — `Shared.new(value)` |
| Ctrl+C / SIGINTを綺麗に扱う | [§12 Signal](#signal--signalnotify--signalreset) — `Signal.notify(tx)` / `Signal.reset()` |
| デスクトップGUI（ネイティブWebView + ローカルサーバ） | [§29 Desktop](#29-desktop--webview) — `Desktop.run({title, assets, routes})` |
| ヒープ情報・リークチェック | [§7 GC](#gc--ヒープ情報の取得) — `GC.stat()` → `{live_objects, rc_objects, heap_bytes}` |
| 2D/3Dベクトル演算（dot、length、normalize、distance） | [§30 `Vector2`](#30-vector2) / [§31 `Vector3`](#31-vector3) |
| FIFOキュー、スライディングウィンドウ、前後両端のスタック | [§32 `Deque`](#32-deque) — `Deque.new()` — `push_back`/`pop_front` |
| 優先度スケジューリング、イベントシミュレーション、最短経路探索 | [§33 `PriorityQueue`](#33-priorityqueue) — `PriorityQueue.new()` — `push`/`pop` |
| 自前の言語・設定フォーマットをパースする | [§34 PEG](#34-peg) — PEG文法を書いて`PEG.parse(grammar, text)`。返る木は`match`で分解できる |
| 手順・通信手順・画面のモードを状態とイベントで表す | [§36 `StateMachine`](#36-statemachine) — `StateMachine.parse(text)`、または記述用の`Object` |
| 変わらない大きな語彙から前方一致で補完する | [§37 FST](#37-fst) — `FST.Set.new(FST.compile_set(words))` → `.predictive_search("hel")` |
| 綴りの直し・あいまい検索 | [§37 FST](#37-fst) — `.edit_distance_search(word, 1)`／`.suggest(word)` |
| たくさんの文書を語で検索して順位をつける | [§38 Search](#38-search) — `Search.Index.new()` → `.add(key, text)` → `.search("quick -dog")` |
| 行列・テンソル演算（BLAS対応） | [§8 Tensor](#8-tensor) |
| String / Array / Objectのメソッド | [言語仕様 §18](language.ja.md) |
| 整数列（`range`, `iota`） | [言語仕様 §19](language.ja.md) |
| `n`個の値で`Array`を埋める | [言語仕様 §19](language.ja.md) — `repeat(n, value)` |
| 変換（`to_long`、`to_float`、`to_string`、`type_of`） | [言語仕様 §19](language.ja.md) |

---

## 1. `Math`

数値ユーティリティ群。整数専用ルーチン（`pow`・`sign`・
`wrap`）は`Long`入力を保ち、`clamp`と浮動小数点ルーチン（`log`ほか）は
`Long` / `Float`のいずれかを受け取ります。`Long`と`Float`の相互作用は
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
inspect(Math.pi)               # => 3.141592653589793
inspect(Math.e)                # => 2.718281828459045
inspect(Math.inf > 1e308)      # => true
inspect(Math.nan == Math.nan)  # => false
```

### スカラー演算

### `Math.abs(x: Long|Float) -> Long|Float`

絶対値。`Long`入力なら`Long`、`Float`入力なら`Float`を返します。

```culebra
inspect(Math.abs(-7))    # => 7
inspect(Math.abs(-7.5))  # => 7.5
```

### `Math.min(a, b, ...) -> Long|Float`、`Math.max(a, b, ...) -> Long|Float`

2つ以上の数値引数から最小 / 最大を取ります。全て`Long`なら`Long`、
1つでも`Float`が含まれれば結果は`Float`。引数1個以下、または
数値以外が混じれば`type error`。

```culebra
inspect(Math.min(3, 1, 4, 1, 5))  # => 1
inspect(Math.max(1.5, 2, 0.5))    # => 2.0
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
inspect(Math.sin(Math.pi / 2))  # => 1.0
inspect(Math.cos(0))            # => 1.0
```

### `Math.asin(x) -> Float`、`Math.acos(x) -> Float`、`Math.atan(x) -> Float`、`Math.atan2(y, x) -> Float`

逆三角関数。戻り値はラジアン。`asin` / `acos`は`x`が`[-1, 1]`
範囲（外は`nan`）。`Math.atan2(y, x)`は`y / x`の象限を考慮した
逆正接。

```culebra
inspect(Math.atan2(1.0, 1.0))  # => 0.7853981633974483
# (= pi/4)
```

### `Math.floor(x: Long|Float) -> Long`、`Math.ceil(...) -> Long`、`Math.round(...) -> Long`

整数への丸め。`Long`入力はそのまま返します。`Math.floor`は
`-∞`方向、`Math.ceil`は`+∞`方向、`Math.round`は
**偶数丸め（bankers' rounding）**。

```culebra
inspect(Math.floor(-1.5))  # => -2
inspect(Math.ceil(-1.5))   # => -1
# ちょうど半分は偶数側へ丸めるので 2.5 も 3.5 も偶数になる:
inspect(Math.round(2.5))  # => 2
inspect(Math.round(3.5))  # => 4
```

### `Math.f32(x: Long|Float) -> Float`

最も近い`float`（IEEE binary32）に丸めて`Float`として返します。
単精度で書かれた計算（ゲームの乱数生成器、32bit値を格納するファイル形式
など）を`Float`のまま再現するためのものです。`float`の最大値をわずかに
超える値はその最大値に戻り、さらに外側は`±inf`、`nan`は`nan`のままです。

```culebra
inspect(Math.f32(0.1))       # => 0.10000000149011612
inspect(Math.f32(16777217))  # => 1.6777216e+07
```

### `Math.pow(base: Long, exp: Long) -> Long`

整数累乗。繰り返し二乗法で`base ** exp`を計算します。
`Math.pow(x, 0)`は`x`に関わらず`1`（`0`を含む）。

**例外**: `exp < 0`のとき`type error at L:C.`。

後方互換のため残してあります。**基本は`**`演算子を使ってください**
（`Float`・負指数も扱えます。言語仕様 §7）。

```culebra
inspect(Math.pow(2, 10))  # => 1024
inspect(Math.pow(7, 0))   # => 1
inspect(Math.pow(-3, 3))  # => -27
```

### `Math.sign(x: Long) -> Long`

負数で`-1`、ゼロで`0`、正数で`1`を返します。

```culebra
inspect(Math.sign(-5))  # => -1
inspect(Math.sign(0))   # => 0
inspect(Math.sign(42))  # => 1
```

### `Math.clamp(x: Long|Float, lo: Long|Float, hi: Long|Float) -> Long|Float`

`x`を閉区間`[lo, hi]`に収めます。`lo > hi`の場合はエラーに
ならず`hi`を返します。`x`・`lo`・`hi`が全て`Long`なら`Long`、
いずれかが`Float`なら`Float`を返します（`Math.min`/`Math.max`と
同じ昇格ルール）。

```culebra
inspect(Math.clamp(5, 0, 10))         # => 5
inspect(Math.clamp(-5, 0, 10))        # => 0
inspect(Math.clamp(15, 0, 10))        # => 10
inspect(Math.clamp(0.5, 0.0, 1.0))    # => 0.5
inspect(Math.clamp(-3.0, 0.0, 10.0))  # => 0.0
```

### `Math.wrap(x: Long, n: Long) -> Long`

`x`を幅`n`に巻き戻します。`%`では得られない **floor剰余** です
（`%`は切り捨てなので結果は`x`の符号を持つ。[言語仕様 §7](language.ja.md#算術)）。
`Math.wrap`の結果は`n`の符号を持つので、`n`が正なら必ず`[0, n)`
に入ります。巡回インデックスが欲しいのはこちらで、インデックス0の
1つ手前は負の添字ではなく末尾の要素になります。

```culebra
inspect(Math.wrap(3, 320))    # => 3
inspect(Math.wrap(-3, 320))   # => 317
inspect(-3 % 320)             # => -3
inspect(Math.wrap(320, 320))  # => 0
```

`x`が非負の範囲では両者は一致するので、`Math.wrap`が効くのは`x`が
負になりうるとき — スクロール量、折り返すタイル座標、逆向きに進めた
角度 — だけです。

```culebra
let frames = ['a', 'b', 'c']
let prev = fn (i) {
  frames[Math.wrap(i - 1, frames.size())]
}
inspect(prev(0))  # => 'c'
```

`n`が負なら全体が反転し、結果は`(n, 0]`に入ります。`n`が`0`の
場合は`x % 0`と同じく`divide by 0 error`を送出します。

---

## 2. `IO`

出力と標準入力。ファイルの読み書きは`FS`（`FS.read` / `FS.write` /
`FS.exists`）にあります。

### `IO.inspect(x: Any) -> Nil`

`x`を改行付きで標準出力に書き出します。参照型は
`Array`/`Object`の既定の`to_string`と同じ書式で整形され、
文字列は**シングルクォートで囲んで**出力されます。

```culebra
IO.inspect('hi')      # → 'hi'
IO.inspect(42)        # → 42
IO.inspect([1, 'a'])  # → [1, 'a']
```

### `IO.print(x: Any) -> Nil`

`x`を**末尾改行なし**で標準出力へ書き出します。書式は`to_string`
と同じで、文字列は**引用符なし**で出力されます。複数回の書き込みで
1行を組み立てたい場合に便利です。

```culebra
IO.print('Hello, ')
IO.print('world!')
IO.println('')  # → Hello, world!
```

### `IO.println(x: Any = '') -> Nil`

`x`を改行付きで標準出力に書き出します。書式は`to_string`と同じで、
文字列は**引用符なし**で出力されます — `inspect`の生表示版であり、
`print`の改行付き版です。`x`は`''`がデフォルトなので、`println()`単体は
空行だけを出力します。

```culebra
IO.println('hi')  # → hi
IO.println(42)    # → 42
IO.println()      # → (空行)
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
  if line.contains("error") {
    IO.println(line.upper())
  }
}

let src = if IO.stdin_is_terminal() {
  read_clipboard()
} else {
  IO.stdin().read()
}
```

### `IO.einspect(x: Any) -> Nil` / `IO.eprint(x: Any) -> Nil` / `IO.eprintln(x: Any) -> Nil`

標準エラーへ書き出します（`inspect` / `print` / `println`の双子）。
`einspect`は文字列をクォートし改行を付けます（`inspect`同様）、
`eprint`は末尾改行なしの生の表示形（`print`同様）、`eprintln`は
改行付きの生の表示形（`println`同様）。stdoutに混ぜたくない診断
出力に使います。

```culebra
# doctest: skip
IO.einspect("warning: retrying")  # → stderr
if !ok {
  IO.eprint("error: {msg}\n")
}
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
let src = if IO.stdin_is_terminal() {
  read_clipboard()
} else {
  FS.read("/dev/stdin")
}
if IO.stdout_is_terminal() {
  println(colorize(msg))
} else {
  println(msg)
}
```

### `IO.capture(f: Function) -> String`

`f`をstdoutをバッファに向けた状態で実行し、書き出された内容を返します。
`f`の戻り値は捨てられます（返す値ではなく印字する値が目的なので）。
「返さずに出力する」関数をテストで検証する手段です:

```culebra
let out = IO.capture(fn () {
  println('hello')
})
assert_eq(out, "hello\n")
```

リダイレクトは**呼び出したスレッドのもの**です。`f`の実行中に別スレッドが
印字したものはstdoutへ出ますし、2つのスレッドが同時にcaptureしても互いの
出力を奪いません。`f`がthrowしてもリダイレクトは元に戻り、throwはそのまま
伝播します。入れ子にもでき、内側のcaptureは端末ではなく外側のバッファに
戻します。`IO.eprint`系はstderrに書くので捕捉されません。

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

プラットフォームの区切り文字（POSIXでは`/`、Windowsでは`\` — 下の
`FS.sep()`参照）でパス要素を結合します。引数0個は`""`を返します。
`std::filesystem::path::operator/=`と同じく、途中要素の末尾区切り文字は
尊重されます。

```culebra
inspect(FS.join('a', 'b', 'c.txt'))       # => 'a/b/c.txt'
inspect(FS.join('/usr', 'local', 'bin'))  # => '/usr/local/bin'
inspect(FS.join())                        # => ''
```

上の例はPOSIXの区切り文字での表示。結合したパスをリテラルと比較する
プログラムは、`/`を決め打ちせず`FS.sep()`で組み立てるか、パスの
構成要素同士を比較すること。

#### `FS.sep() -> String`

プラットフォームのパス区切り文字 — `FS.join`と`Path`の`/`が要素間に
挿むもの。POSIXでは`/`、Windowsでは`\`。

```culebra
inspect(FS.join('a', 'b') == "a{FS.sep()}b")  # => true
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

#### `FS.temp_dir() -> String`

このマシンが一時ファイルを置くディレクトリ。POSIXでは`$TMPDIR`と
そのフォールバック、Windowsでは`GetTempPath`。末尾に区切り文字を
付けないので`FS.join`で二重にならない。プラットフォームが返した
パスがディレクトリでない場合は`IOError`をthrow。

```culebra
assert_true(FS.is_dir(FS.temp_dir()))
```

#### `FS.mkdtemp(prefix: String) -> String`

`FS.temp_dir()`の下に`prefix`で始まる名前の新しいディレクトリを
作成し、そのパスを返す。名前を決めるだけでなく実際に**作成**するので、
同時に走る2つのプログラムが同じディレクトリを受け取ることはない
（サフィックスは共有エンジンではなくOSから引くので、`Random.seed`で
同じ種を与えた2つの実行でも一致しない）。削除は呼び出し側の責任。

`prefix`はパスではなく名前。存在しない親ディレクトリを含む場合は
暗黙の`mkdir -p`ではなく`IOError`。

```culebra
let dir = FS.mkdtemp('build-')
assert_true(FS.is_dir(dir))
assert_eq(FS.dirname(dir), FS.temp_dir())
FS.remove(dir, recursive: true)
```

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

プラットフォームをまたいで通用するのは所有者のwriteビットだけです。Windows
は権限ビットではなく読み取り専用属性を1つ持つので、どのファイルも`0o666`か
`0o444`として読め、modeは往復しません（`FS.chmod(p, 0o755)`の後の
`FS.stat(p).mode`は`0o666`）。所有者writeを落とす（`0o444`）・再び立てる、
という操作だけがどこでも同じように振る舞います。

```culebra
# doctest: skip
FS.chmod('deploy.sh', 0o755)        # 実行可能にする
inspect(FS.stat('deploy.sh').mode)  # 493  (0o755)
```

#### `FS.chown(path: String, owner = nil, group = nil) -> Nil`

`path`の所有者・グループを変更する。`owner`/`group`はそれぞれ名前（`String`）・
数値id（`Long`）・`nil`（その項目は変更しない）を受ける。現在のidは
`FS.stat(path).uid` / `.gid`で読める。所有者の変更は通常rootが必要、グループは
所属グループへなら一般ユーザでも可。存在しないパス・不明な名前・権限失敗で
`IOError`、String/Long/Nil以外の引数は`TypeError`。

```culebra
# doctest: skip
FS.chown('app.log', group: 'staff')   # グループだけ名前で設定、所有者は維持
FS.chown('data', 'deploy', 'deploy')  # 両方を名前で（root）
```

### stat / メタデータ

#### `FS.stat(path: String) -> Object`

`{size, is_dir, is_file, is_symlink, mtime, mode, uid, gid}`を返す。`size`は
バイト（非通常ファイルは0）、`mtime`はUnix epoch秒、`mode`は権限ビットの整数
（8進と比較: `st.mode == 0o644`）、`uid`/`gid`は所有者・グループid、
`is_symlink`は
リンク自体を、他フィールドはリンク先を見ます。存在しなければ
`IOError`。

WindowsにはPOSIXの所有者モデルが無いので、`uid`と`gid`は代替値を持つのでは
なく**フィールドごと存在しません**。`st.uid`は`nil`になり、そのプラット
フォームが所有者idを持つかは`st.has('uid')`で問います。`mode`はどこにでも
ありますが、Windowsは権限ビットではなく読み取り専用属性を1つ持つだけなので
意味が薄くなります（下の`FS.chmod`を参照）。

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

### 変更の監視

#### `FS.watch(path: String, recursive: Bool = true, match: Array? = nil) -> WatchHandle`

ディレクトリの変更を監視します。ハンドルの反復は次の変更までブロック
し、`{path, kind}`を返します。`path`は絶対・正規化済み（シンボリック
リンク解決済みなので、macOSでは`/tmp`配下が`/private/tmp`配下として
報告される）、`kind`は`'created'` / `'modified'` / `'deleted'`。
`match`は指定拡張子で終わるパスだけを通します（先頭ドットは有無どちら
でも可 — `'cul'`と`'.cul'`は同じフィルタ）。判定はイベントをキューに積む
前に行うので、弾かれた変更はキューに入りません。
ディレクトリでなければ`IOError`、`match`が文字列配列でなければ
`TypeError`。

`recursive: false`は監視をそのディレクトリ直下の項目に絞ります。サブ
ディレクトリの*下*で起きた変更は報告されませんが、サブディレクトリ項目
そのものの変更は報告されることがあります — 中のファイルが書かれたとき
にタイムスタンプが動いたことをWindowsは報告し、inotifyとFSEventsは報告
しません。

バックエンドはmacOSがFSEvents、Linuxがinotify、Windowsが
`ReadDirectoryChangesW`。

```culebra
# doctest: skip
for e in FS.watch('src', match: ['.cul']) {
  println("{e.kind} {e.path}")
}
```

イベントは監視を開いた時点から積まれるので、最初の取り出しより前に起き
た変更も届きます。ハンドルはそのまま反復でき、`break`しても使えるまま
です。名前付きハンドルは再度反復できます:

```culebra
# doctest: skip
let w = FS.watch('src')
for e in w { break }        # ハンドルは開いたまま
for e in w { break }        # 前のループの続きから
w.close()
```

`close()`で監視を止めます。スコープを抜けたハンドルは自分で閉じるので、
匿名の`for e in FS.watch(p)`はループ終了時に停止します。監視は開いた
スレッドに属し、isolateへ送ることはできません。

意図的に約束していないことが3点あります。**kindはOSの粒度でのbest
effort**です。FSEventsは短い窓の中で同一パスへの複数の変更を1イベント
に畳み、inotifyは個別に報告するため、同じ編集が片方では1イベント、もう
片方では2イベントになりえます。**renameはペアになりません** — 旧パスの
`deleted`と新パスの`created`として別々に届きます。**重複排除も
debounceもせず**、キューは無制限です。木の変化より遅い消費者はイベント
を落とすのではなくメモリを消費します。

待機中の反復を終わらせられるのは`break`・Ctrl+C・実行中のisolateの
キャンセルだけです。タイムアウトやノンブロッキング取得はありません。

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
  let dst = src.parent / "content.js"  # vs FS.join(FS.dirname(src), …)
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
let text = FS.read(cfg)  # FS.read(String | Path)
for line in File.open(cfg).lines() {
}  # File.open(String | Path)
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
let head = File.with('big.log', 'r', fn (f) {
  f.read(256)
})
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
  if line.contains('ERROR') {
    inspect(line)
  }
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
t.format("%Y-%m-%d %H:%M:%S")  # local
t.format("%Y%m%d", utc: true)  # 20260520
```

#### `t.parts(utc: false) -> Object`

`{year, month, day, hour, minute, second, nanosecond, weekday,
dayofyear}`に分解。`weekday`はISO 8601起点（`0=Mon`、`6=Sun`）、
`dayofyear`は1-based（`1..366`）。

```culebra
let p = Time.now().parts()
if p.hour >= 9 && p.hour < 17 {
  inspect("business hours")
}
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
let next_month = Time.now().add(months: 1)
let next_quarter = Time.now().add(months: 3)
let next_year = Time.now().add(years: 1)
```

#### `t.start_of(unit: String, utc: false) -> Instant`

カレンダー単位の先頭に丸める。`unit` ∈ `"year"` / `"month"` /
`"day"` / `"hour"` / `"minute"`。それ以外は`ValueError`。

```culebra
# doctest: skip
let day_bucket = t.start_of("day")
let hour_bucket = t.start_of("hour")
```

#### `t.unix() -> Float`、`t.unix_nanos() -> Long`

Unix epochをFloat秒（現在時点で ~400ns精度）またはLong ns
（loss-less）で取得。

### `Duration` コンストラクタ

```culebra
# doctest: skip
Time.seconds(n)  # n 秒
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
持ち、VMとJITで共有しています。`Random.seed(n)`は
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
inspect(Random.int(0, 10))  # 0..9
```

### `Random.uniform(lo: Float, hi: Float) -> Float`

半開区間`[lo, hi)`からの一様実数。`Long`引数も受け付け、
`Float`に昇格します。

### `Random.gauss(mu: Float, sigma: Float) -> Float`

平均`mu`、標準偏差`sigma`のガウス分布から1サンプル。
`Long`引数は`Float`に昇格します。

```culebra
Random.gauss(0.0, 1.0)  # 標準正規
```

### `Random.shuffle(a: Array) -> Nil`

Fisher–Yatesによるインプレース置換。`nil`を返し、引数は破壊的に
並び替えられます。

### `Random.weighted_choice(pop: Array, weights: Array) -> Any`

対応する`weights`に比例する確率で`pop`から1要素を取り出します。
`weights`はすべて数値かつ`pop`と同じ長さである必要があります。
空または長さ不一致は`type error`。重み`0`は選ばれません。

```culebra
Random.weighted_choice(['hit', 'miss'], [1, 9])  # ~10% 'hit'
```

### `Random.choice(pop: Array) -> Any`

`pop`から一様な確率で1要素を取り出します。`pop`が空なら`type error`。

```culebra
Random.seed(0)
Random.choice(['rock', 'paper', 'scissors'])
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
inspect(Sys.argv)  # ['hello', 'world']
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
if error_occurred {
  Sys.exit(1)
}
```

### `Sys.env(name: String, fallback = '') -> Any`

環境変数`name`の値を返します。未設定の場合は`fallback`をそのまま返します。
既定値が`''`なので、1引数形式は従来どおり`String`を返します。

`fallback`は型を問わずそのまま返るため、`nil`を渡すことで「未設定」と
「空文字列が設定されている」を区別できます。1引数形式はどちらも`''`に
なります。

```culebra
# doctest: skip
inspect(Sys.env('HOME'))                # '/Users/alice'
inspect(Sys.env('NOT_A_VAR'))           # ''
inspect(Sys.env('PORT', '8080'))        # '8080'（PORT未設定時）
inspect(Sys.env('NOT_A_VAR', nil))      # nil — 未設定
inspect(Sys.env('SET_BUT_EMPTY', nil))  # '' — 設定済みで空文字列
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
inspect(Sys.getcwd())  # '/Users/alice/project'
```

### `Sys.chdir(path: String) -> Nil`

プロセスの作業ディレクトリを`path`に変更します。パスが存在しない、または
ディレクトリでない場合は`IOError`を送出します。

```culebra
# doctest: skip
Sys.chdir('/tmp')
inspect(Sys.getcwd())  # '/tmp'（または解決後のパス）
```

### `Sys.data_dir(app: String) -> String`

`app`がプロセスを越えて残すデータ（セーブファイル、ハイスコア、設定）を
置くユーザーごとのディレクトリを返します。規約はプラットフォームのものに
従います:

| プラットフォーム | ディレクトリ |
|---|---|
| macOS | `~/Library/Application Support/<app>` |
| Windows | `%APPDATA%\<app>`（未設定なら`%USERPROFILE%\AppData\Roaming\<app>`） |
| その他 | `$XDG_DATA_HOME/<app>`、未設定なら`~/.local/share/<app>` |

ディレクトリは**作成しません** — 最初に書き込むときに`FS.mkdir`が親ごと作ります。
`app`は単一のパスセグメントでなければならず、空文字列・`/`や`\`や`:`を含む
名前・`.`・`..`はいずれも`ValueError`になります。したがって戻り値がベース
ディレクトリの外を指すことはありません（Windowsのドライブレターは、付け足す
のではなくベースを置き換えてしまうため）。プラットフォームのベースディレクトリを
決定できない場合（`HOME`が無い、`APPDATA`も`USERPROFILE`も無い）は`IOError`を
送出します。

```culebra
# doctest: skip
let dir = Sys.data_dir('samegame')
FS.mkdir(dir)
FS.write(FS.join(dir, 'scores.json'), JSON.stringify(scores))
inspect(dir)  # '/Users/alice/Library/Application Support/samegame'
```

### `Sys.executable -> String`

実行中のculebraバイナリの絶対パス。`culebra`が`PATH`にあることに頼らず、
culebraのワーカーコピーを起動するのに使う — 例
`Proc.run([Sys.executable, "worker.cul"], ...)`。（AOTビルドされたプログラムでは
その単体バイナリ自身のパスになる。）

```culebra
# doctest: skip
inspect(Sys.executable)  # '/usr/local/bin/culebra'
```

### `Sys.script -> String?`

実行中スクリプトの絶対パス（`__file__`相当）。カレントディレクトリに頼らず、
スクリプトの隣にあるファイルを解決するのに使う：
`FS.join(FS.dirname(Sys.script), "data.txt")`。実行時にソースファイルが存在しない
場合 — REPL・パイプした`stdin`・AOTビルドされたバイナリ（`.cul`を持たない。その
場合は`Sys.executable`を使う）— では`nil`。

```culebra
# doctest: skip
inspect(Sys.script)  # '/Users/alice/project/build.cul'
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
inspect(Sys.time() - t0 >= 0.0)  # => true
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
inspect(GC.stat().live_objects - base)  # 構造が保持しているオブジェクト数
```

これはリーク回帰テストの土台になる: 多数の反復をまたいでdeltaが有界に
留まることをassertする。メモリはそれ以外は自動管理 — メモリモデルと
確定的`drop`は言語仕様を参照。

---

## 8. `Tensor`

N次元数値テンソル。lazy計算グラフを構築し、`Tensor.eval(...)`で
BLAS / vDSP経由のカーネルを起動して値を確定します。dtypeは
`Float32`（デフォルト）と`Float64`、形状はvariadicか`[m, n]`
配列で指定。`transpose` / `slice` / `reshape`はzero-copy view。

```culebra
let A = Tensor.from([[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]])    # [2, 3]
let B = Tensor.from([[1.0, 0.0], [0.0, 1.0], [1.0, 1.0]])  # [3, 2]
let C = A.dot(B) + 1.0                                     # lazy: グラフを作るだけ
Tensor.eval(C)                                             # ここで BLAS GEMM が走る
inspect(C.shape())                                         # => [2, 2]
inspect(C.to_array())                                      # => [[5.0, 6.0], [11.0, 12.0]]
```

### 構築（名前空間関数）

#### `Tensor.zeros(...) -> Tensor` / `Tensor.ones(...)` / `Tensor.randn(...)`

形状をvariadic（`Tensor.zeros(3, 4)`）またはArray
（`Tensor.zeros([3, 4])`）で受け取ります。dtypeは文字列タグを
**第一引数**に置くJulia流。dtypeは`"f32"`のみです（float64は
GPUバックエンドに高速パスがないため）：

```culebra
let a = Tensor.zeros(3, 4)           # F32 default
let a32 = Tensor.zeros("f32", 3, 4)  # 明示
let dims = [3, 4]
let b = Tensor.zeros(dims)  # 計算済み形状
let r = Tensor.randn(2, 3)  # 標準正規
```

#### `Tensor.from(arr: Array) -> Tensor`

ネストされたCulebra配列をTensorに変換します。1D（`[1.0, 2.0]`）
または2D（`[[1.0, 2.0], [3.0, 4.0]]`）を受け、F32で格納：

```culebra
let v = Tensor.from([1.0, 2.0, 3.0, 4.0])      # [4]
let m = Tensor.from([[1.0, 2.0], [3.0, 4.0]])  # [2, 2]
```

#### `Tensor.concat(parts: Array, axis: Long = 0) -> Tensor`

Tensorを`axis`方向（既定は軸0＝行）に積み重ね、1つのTensorに
します。すべてのpartはdtype・rank、`axis`以外の次元が一致して
いる必要があります。結果の`axis`方向の長さは各partの合計です。
微分可能 — 勾配は各partに対応する`axis`方向のwindowに切り分けて
戻されます。

```culebra
let a = Tensor.from([[1.0, 2.0], [3.0, 4.0]])  # [2, 2]
let b = Tensor.from([[5.0, 6.0]])              # [1, 2]
let c = Tensor.concat([a, b])                  # [3, 2]

let k1 = Tensor.randn(2, 4, 8)   # [B, T, D]
let k2 = Tensor.randn(2, 1, 8)   # 新しい1ポジション分
let kv = Tensor.concat([k1, k2], 1)  # [2, 5, 8] — KVキャッシュへの追記
```

#### `Tensor.where(cond: Tensor, a, b) -> Tensor`

要素ごとの選択で、3つのオペランド全てを（他のbinopと同じ規則で）
broadcastします: `cond != 0 ? a : b`。`a`/`b`はスカラーでもよく、
`cond`のdtypeに合わせてliftされます。masking（attention mask、
padding mask）の基礎部品です。微分可能 — `da = g*cond`、
`db = g*(1-cond)`。`cond`自体には勾配が流れません
（numpy/PyTorch自身の`where()`と同じ規約）。

```culebra
let mask = Tensor.from([[1.0, 0.0, 1.0]])   # [1, 3]、行方向にbroadcast
let scores = Tensor.randn(4, 3)
let masked = Tensor.where(mask, scores, -1.0e9)  # padding maskのパターン
```

#### `Tensor.index_add(indices: Tensor, values: Tensor, target_shape: Array) -> Tensor`

`.index_select()`の正確な双対: `t.index_select(indices)`のような形の
`values`を、`target_shape`の新規ゼロバッファへscatter-addします。
同じインデックスが重複すれば累積されます——embeddingテーブルの勾配は
これです。`values`について微分可能（入ってきた勾配に対する
`.index_select(indices)`）。`indices`には勾配が流れません。

```culebra
let idx = Tensor.from([1.0, 1.0, 0.0])       # 行1が2回ヒット: 累積される
let values = Tensor.from([[10.0, 20.0], [30.0, 40.0], [1.0, 2.0]])
let grad = Tensor.index_add(idx, values, [4, 2])  # [[1,2],[40,60],[0,0],[0,0]]
```

#### `Tensor.scatter_to_axis(indices: Tensor, values: Tensor, size: Long) -> Tensor`

新規の末尾軸（長さ`size`）へのone-hot scatter: `indices[...] == k`のとき
`out[..., k] = values[...]`、それ以外は0。`indices`/`values`は同じ形状
である必要があります。poolingレイヤー自身の手書きbackwardが`.fold()`と
組み合わせてpadded input勾配を再構築するネイティブプリミティブです
——`.max(axis)`/`.argmax(axis)`がwindowから1要素選ぶのに対し、これは
勾配を同じwindow形状へscatterして戻します。今のところforward-only
（ネイティブ`.backward()`は未対応）。

```culebra
let idx = Tensor.from([2.0, 0.0, 1.0])
let values = Tensor.from([5.0, 7.0, 9.0])
let out = Tensor.scatter_to_axis(idx, values, 4)
# [[0,0,5,0], [7,0,0,0], [0,9,0,0]]
```

#### `Tensor.from_csv(path: String) -> Tensor`

CSVファイルを直接contiguousなTensorに読み込みます。常に
**rank-2** を返す — 単列CSVは`[N, 1]`（biasベクトル形式）。
nested Arrayを経由しないので、`Tensor.from(load_2d(path))`
パターンより3-5x速い（MNIST規模で実測）：

```culebra
# doctest: skip
let W1 = Tensor.from_csv("W1.csv")  # [30, 784]
let b1 = Tensor.from_csv("b1.csv")  # [30, 1]
let X = Tensor.from_csv("X.csv")    # [N, 784]
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
Tensor.eval(W1, b1, W2, b2)  # 4 つを 1 パスで評価
```

### 活性化関数

Tensorのインスタンスメソッドです。ユーザのクラスが独自に`relu` /
`sigmoid` / `softmax`を定義していても（microgptの`Value.relu()`
など）、メソッド解決はクラスメソッドをビルトインより優先するため衝突
しません。

```culebra
# doctest: skip
let h = z.sigmoid()       # 1/(1+exp(-z)) elementwise
let r = x.relu()          # max(0, x)
let p = logits.softmax()  # 最終軸で online stable
let l = p.log()           # 自然対数、elementwise
let t = z.tanh()          # 双曲線正接
let s = z.sin()           # elementwise sin/cos
let c = z.cos()
let m = z.clamp(-1.0, 1.0)  # elementwise clip、[lo, hi]に収める
```

### Tensor のメソッド

形状・線形代数・reductionはメソッド構文：

| メソッド | 戻り値 | 説明 |
|---|---|---|
| `.shape() -> Array` | Array of Long | 形状をArrayで返す |
| `.dot(other: Tensor) -> Tensor` | lazy | 行列積。rank-1/2、またはbatched（rank>=3、先頭次元が完全一致） |
| `.linear_sigmoid(x, b) -> Tensor` | lazy | 融合`sigmoid(self @ x + b)` |
| `.pow(exp) -> Tensor` | lazy | elementwise冪、expはTensorまたはscalar |
| `.gt(other) / .lt(other) / .ge(other) / .le(other) / .eq(other) / .ne(other) -> Tensor` | lazy | elementwise比較、`1.0`/`0.0`のマスクを返す。otherはTensorまたはscalar |
| `.tanh() / .sin() / .cos() -> Tensor` | lazy | elementwiseな三角関数・双曲線正接 |
| `.clamp(lo, hi) -> Tensor` | lazy | `[lo, hi]`へのelementwise clip |
| `.rope(pos: Long, base) -> Tensor` | lazy | 回転位置埋め込み（half-split方式）、最後の軸に適用。selfは`[H, D]`または`[H, T, D]`、行rの位置は`pos + (r % T)` |
| `.transpose() -> Tensor` | view | 全軸逆順（rank-2で行列転置） |
| `.permute(axes: Array) -> Tensor` | view | 任意軸並べ替え。`axes[i]`が結果の軸`i`に対応する自分自身の軸を指定 |
| `.slice(start, end) -> Tensor` | view | 軸0を`[start, end)`で切り出し |
| `.narrow(params: Array) -> Tensor` | view | `[axis, start, end]`。`.slice()`を任意軸に一般化 |
| `.reshape(dims: Array) -> Tensor` | view | 連続入力のみ。新形状 |
| `.unfold(params: Array) -> Tensor` | view | `[axis, win, step]`。`axis`沿いのスライディングウィンドウ、末尾に長さ`win`の軸を追加 |
| `.pad(params: Array) -> Tensor` | new buffer | `[axis, before, after]`。`axis`沿いにゼロパディング |
| `.fold(params: Array) -> Tensor` | new buffer | `[axis, orig_size, step]`。`.unfold()`の逆——重なる窓をscatter-addで`axis`へ戻す |
| `.sum() -> Float` | scalar | 全要素和（暗黙eval） |
| `.sum(axis: Long?) -> Tensor` | lazy | 軸を1つ畳む。axisがnilなら軸指定なし＝スカラー形と同じ |
| `.mean() / .mean(axis)` | Float / Tensor | 同様 |
| `.max() / .max(axis)` | Float / Tensor | 同様 |
| `.argmax(axis: Long) -> Tensor` | lazy | 軸を畳んでインデックスをFloatで格納 |
| `.index_select(indices: Tensor) -> Tensor` | lazy | 軸0方向の行gather: `out[i] = self[indices[i]]`——embeddingテーブルのlookup |
| `.softmax_cross_entropy(targets: Tensor) -> Tensor` | lazy | `[N, C]`のlogitsの行ごとにsoftmax + cross-entropyを融合。行あたり1つのloss（`[N]`） |
| `.to_array() -> Array` | eager | Culebra Arrayへ変換（暗黙eval） |
| `.item() -> Float` | eager | 唯一の要素をFloatとして取り出す。要素数が1でない（任意rank）場合は例外 |

`.item()`はスカラーの取り出し口で、`.to_array()`（形状を持つデータ用）と対をなす。
lossなど単一要素の結果をreshapeせず読むのに使う。`loss.item()`は
`to_float(loss.to_array()[0])`の置き換え。

#### `.softmax_cross_entropy(targets: Tensor) -> Tensor`

クラス方向のsoftmaxと`targets`に対するcross-entropyを1つのopに融合したもの。
`self`は`[N, C]`のlogits、`targets`は行あたり1つのクラスid（`[N]`、
`.index_select()`のindicesと同じくFloat値。要素値の範囲検査をしない点も同じ）。
結果は行ごとのlossなので、どう畳むかは呼び手が決めます:

```culebra
# doctest: skip
let rows = logits.softmax_cross_entropy(targets)
let loss = rows.mean(0)                        # 素直なバッチ平均
let sft = (rows * mask).sum(0) * (1.0 / kept)  # 一部の行だけ採点する場合
```

自前で合成しても値は同じですが、勾配の経路が違います。合成した場合
`.backward()`はlog、clamp、積、softmaxを順に遡り、そのそれぞれが
`[N, C]`のフルパスです。融合すると、VJPは閉形`softmax(self) - onehot`の
1パスになります。`[512, 1000]`でbackwardが4.2 msから0.7 msになります
（forwardは4.0 ms）。

合成版に必要なclampはop内部にあります。確率が0にアンダーフローした
クラスは、無限大のlossではなく`-log(1e-15)`を寄与します。dezero自身の
融合`SoftmaxCrossEntropy`と同じく、backwardは閉形を取り、このclampは
微分しません。

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
`.softmax()`、`.log()`、`.tanh()`、`.sin()`、`.cos()`、`.clamp()`、
`.transpose()`、`.permute()`、`.reshape()`、`.slice()`、`.narrow()`、
`.rope()`、`.softmax_cross_entropy()`、`Tensor.concat()`、`Tensor.where()`、
`.index_select()` /
`Tensor.index_add()`（互いが相手のVJP）。勾配は自動でun-broadcastされる
ので、バッチ越しに加えたbiasは元の形状に和を取って戻ります。
`.permute()`のVJPは逆置換です。これによりattentionのhead分割
（`[B, C, H, D]`から`[B, H, C, D]`）が学習できます——`.transpose()`は
*全*軸を反転するので、rank 3以上では別の操作であって、これの短縮形では
ありません。置換はノード自身のスカラースロットに1軸4ビットで収めるため、
rank 16以上の`.permute()`を通した`.backward()`は、切り詰めた置換を
保存するのではなく例外を投げます。`.rope()`も何も保存しません。
回転行列の逆は自身の転置であり、head次元の後ろ半分の符号を反転して
から回して戻せばその転置になるので、backwardは順方向をもう一度
走らせるだけで済みます——使ったcos/sinの表を持ち回る必要がありません。
`.unfold()`、`.pad()`、`.fold()`、
`Tensor.scatter_to_axis()`は今のところforward-onlyです——これらを通した
`.backward()`は例外を投げます。im2col方式のconvやpoolingレイヤーなど
これらを使う学習ループは、今のところ自前でbackwardを書きます。
`.gt()` / `.lt()` / `.ge()` / `.le()` / `.eq()` / `.ne()`（および`.max()` /
`.argmax()`）は恒久的に微分不可能です——比較を通した`.backward()`は常に
例外を投げます（PyTorch/numpyと同じ）。0/1マスクは`*`で合成してください
（ReLU系のbackward gateが具体例）。Tanh/Sin/Cos/Clipのbackwardを
これらのopから自前で組み立てるやり方も、ネイティブVJPと共存する
選択の一つとして今も有効です——どちらの経路でも動きます。

```culebra
let w = Tensor.from([[2.0, 0.0], [0.0, 3.0]]).requires_grad()
let x = Tensor.from([[1.0], [1.0]]).requires_grad()
let y = w.dot(x)  # [2, 1]
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
let logits = Tensor.no_grad(fn () {
  model_forward(x)
})
```

### 演算子オーバーロード

`+ - * /`はブロードキャストelementwise（numpy / silarray規則）。
スカラーとの混在も自動：

```culebra
let M = Tensor.ones(3, 4)
let v = Tensor.ones(4)     # → [3, 4] にブロードキャスト
let r = Tensor.ones(3, 1)  # → [3, 4] にブロードキャスト
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
W -= grad * lr      # W のバッファを直接書き換え
Tensor.eval(alias)  # alias の to_array() でも更新後の値が見える
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
- `.dot()`はbatched matmul対応（rank 3以上）——両オペランドの先頭
  次元が一致していれば、それぞれの末尾2軸同士でmatmulが走る
  （GPU上でもbatchのsliceごとにdispatchされる）
- `.reshape()`・`.softmax()`とも非連続入力（`.transpose()`や
  `.permute()`の結果など）をそのまま受け付ける——明示的な
  materialize手順は不要。`.reshape()`は元が非連続なら内部で
  連続バッファへclone、`.softmax()`はstrided入力を直接読む

### バックエンドとデバイス選択

評価はvendoredな`cpp-tensorlib`エンジンに委譲されます。lazy graph・カーネル融合・デバイスバックエンドはすべて
そちら側の責務です:

- **CPU** — ベクトル化カーネル（AVX2 / NEON）。macOSではBLAS形状の
  カーネルにAccelerateを使用
- **GPU** — macOSはMetal、Linux / WindowsはCUDA

デバイスはプロセスグローバル（VM / JIT / AOTで共有）で、実行時に
切り替えられます:

| 関数 | 効果 |
| --- | --- |
| `Tensor.use_cpu() -> Nil` | すべての演算をCPUで評価 |
| `Tensor.use_gpu() -> Nil` | GPUバックエンドで評価 |
| `Tensor.use_auto() -> Nil` | 演算ごとに問題サイズで選択（デフォルト） |
| `Tensor.gpu_available() -> Bool` | GPUバックエンドがビルドに含まれ到達可能か |
| `Tensor.device() -> String` | 現在の選択: `'cpu'` / `'gpu'` / `'auto'` |

```culebra
inspect(type_of(Tensor.gpu_available()))  # => 'Bool'
inspect(Tensor.device())                  # => 'auto'
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
let r = try {
  JSON.parse('{"a": ,}')
  nil
} catch e {
  e
}
inspect(r.message)           # => 'JSON.parse: expected value'
inspect("{r.line}:{r.col}")  # => '1:7'
```

`\uXXXX`エスケープはUTF-8に復号される。BMP外の文字はJSONの規定どおり
UTF-16のサロゲートペアで書かれ、2つで1文字に合成される。片方だけでは
どの文字も指さないので`ValueError`。`stringify`は制御バイトを`\u00xx`で
出すので、自身の出力は常に読み戻せる。

```culebra
# Goのencoding/jsonは`<`・`>`・`&`をこの形でエスケープするため、Go製
# サービスの応答にはこれが含まれる。
inspect(JSON.parse('"\u003cb\u003e"'))        # => '<b>'
inspect(JSON.parse('"\uD83D\uDE00"'))         # => '😀'
```

ネストの深さには上限がある: コンテナのネストが1000段を超えると、Cスタックを
使い果たす代わりに`ValueError`（`nesting too deep (limit 1000)`）を投げる。
`stringify`も走査する値の木に同じ上限を適用する。

例:

```culebra
let v = {name: 'alice', age: 30, tags: ['admin', 'staff']}
# 既定はコンパクト。`sort_keys` はキーを辞書順に並べる。
inspect(JSON.stringify(v))                   # => '{"name":"alice","age":30,"tags":["admin","staff"]}'
inspect(JSON.stringify(v, sort_keys: true))  # => '{"age":30,"name":"alice","tags":["admin","staff"]}'
let back = JSON.parse(JSON.stringify(v))
inspect(back.name)  # => 'alice'
let arr = JSON.parse("1\n2\n3\n", lines: true)
inspect(arr)  # => [1, 2, 3]
let cfg = JSON.parse(
  '{
  // コメントと末尾カンマを許容
  "port": 8080,
}',
  jsonc: true,
)
inspect(cfg.port)  # => 8080
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
形がVMと同一に振る舞います。位置引数の束縛
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
| `positional` | `Bool` | (推論) | `true`でpositional、`false`でoptionに固定 |

`type: "Bool"`の場合は値を取らない **flag** (`--verbose` / `-v`)。それ以外
の型は次のトークンを値として消費する (`--count 5` / `--count=5`)。

`short`も`default`も無い引数は **positional** 扱い。また`default`を持つか
`repeated`か`Bool` flagのいずれかであれば **省略可能**、そうでなければ必須。
`positional`はこの2つの規則が食い違う場合にどちらかを明示する — 省略可能な
positionalは「省略時の値」を言うために`default`が要るが、それだけだと
optionと見なされてしまうため。

```culebra
# doctest: skip
{name: "file", positional: true, default: "-"}   # cat [<file>]
{name: "file", positional: true, default: nil}   # 同じ。省略時はnil
{name: "out", positional: false}                 # 必須の--out
```

positionalはspec順にマッチするが、省略可能なものは「後続の必須positionalが
必要とする数」より多くトークンが残っている時だけ受け取る。つまり
`[{name: "files", repeated: true}, {name: "dest"}]`は`a b d`を
`files == ["a", "b"]` / `dest == "d"`とparseする。

### 例

```culebra
# doctest: skip
let spec = {
  name: "wc-lite",
  doc: "count lines and words",
  args: [
    {name: "input", type: "String", doc: "input file"},
    {
      name: "lines",
      short: "l",
      type: "Bool",
      default: false,
      doc: "count lines",
    },
    {
      name: "words",
      short: "w",
      type: "Bool",
      default: false,
      doc: "count words",
    },
    {name: "encoding", type: "String", default: "utf-8"},
  ],
}

let args = Args.parse(Sys.argv, spec)
inspect(args.input)
if args.lines {
  inspect("lines: ...")
}
if args.words {
  inspect("words: ...")
}
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
    {name: "add", args: [{name: "files", type: "String", repeated: true}]},
    {name: "commit", args: [{name: "message", short: "m", type: "String"}]},
  ],
}

match Args.parse(Sys.argv, spec).subcommand {
  "add" => stage_files(args.files),
  "commit" => commit_with_message(args.message),
}
```

### エラーハンドリング

`Args.parse`はエラー時にexitする。`Args.try_parse`はthrow:

```culebra
let r = try {
  Args.try_parse(["--bogus"], spec)
} catch e {
  e
}
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

### `Proc.run(cmd: Array<String>, cwd=nil, env=nil, stdin="", check=false, timeout=0, share=nil, inherit_env=true) -> Object`

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
- `env: Object` — 子プロセスに設定する環境変数（既定: 継承する分以外は無し）。
  値は`String`であること。
- `inherit_env: Bool` — `env`が乗る土台が親自身の環境かどうか（既定: `true`
  なので`PATH`等は維持されます）。`false`にすると子は何も無い状態から始まり、
  `env`が与えたものだけを持ちます。親の環境にある秘密を、それを読む理由の無い
  子プロセスから遠ざける手段です:

  ```culebra
  # doctest: skip
  Proc.run(cmd, inherit_env: false,
           env: {PATH: Sys.env("PATH"), HOME: "/tmp"})
  ```

  2つは独立しています — `env`だけなら追加、`inherit_env: false`だけなら空、
  両方でちょうど1つの環境を組み立てます。`share`で渡したバッファはそれ自身が
  環境変数として運ばれるので、どちらの場合も子に届きます。
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
Proc.run(
  ["make", "install"],
  cwd: "/src/app",
  env: {PREFIX: "/usr/local"},
  check: true,
)
```

出力は全量バッファされるため、巨大な出力はそのぶんメモリを使います。stdoutと
stderrは並行して読み出すので、両方を埋めるコマンドでもデッドロックしません。

`share: {名前: buf}`は1つ以上の`SharedBuffer.shared(...)` bufferを子プロセス
へ渡す（子は`SharedBuffer.receive(name, Class)`で再アタッチする）。子はculebra
プロセスである必要があり、通常は`[Sys.executable, "worker.cul"]`。詳細は
[SharedBuffer › プロセス間での共有](#プロセス間での共有zero-copy)。

### `Proc.all(commands: Array<Array<String>>, limit: Long = <CPU数>, timeout: Long = 0, fail_fast: Bool = false, retries: Long = 0, share: Object? = nil, inherit_env: Bool = true) -> Array<Object>`

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
`inherit_env: false`は`Proc.run`と同様に、各子へ独自の環境を与えます。

```culebra
# doctest: skip
let results = Proc.all(
  [["git", "fetch", "origin"], ["npm", "test"], ["cargo", "build"]],
  limit: 2,
)
for r in results {
  if !r.ok {
    IO.print(r.error ?? r.stderr)
  }
}
```

### `Proc.race(commands: Array<Array<String>>, share: Object? = nil, inherit_env: Bool = true) -> Object`

全コマンドを起動し、**最初に完了した1個**の結果Objectを返し、残りに`SIGKILL`
を送ってreapします。冗長なプロバイダの競争や「最速のミラーが勝ち」に有用。空
リストは`ValueError`をthrowします。`share: {name: buf}`が共有バッファを子に渡し、
`inherit_env: false`が親の環境を子から遠ざけるのは`Proc.run`/`Proc.all`と同様です。

```culebra
# doctest: skip
let fastest = Proc.race([
  ["curl", "-s", "https://mirror-a.example/file"],
  ["curl", "-s", "https://mirror-b.example/file"],
])
IO.print(fastest.stdout)
```

### `Proc.spawn(cmd: Array<String>, cwd=nil, env=nil, stdin="", share=nil, inherit_env=true) -> handle`

コマンドを起動し、完了を待たずに即座に**ライブハンドル**を返します。ハンドルは
3つのメソッドを持ちます:

| メソッド | 戻り値 | 意味 |
|---|---|---|
| `h.wait()` | 結果Object | 子の終了まで待ち、出力をdrainする（ブロッキング） |
| `h.poll()` | 結果Objectまたは`nil` | 終了していれば結果、まだなら`nil`（非ブロッキング） |
| `h.kill(sig = 15)` | `nil` | シグナル送出（既定`SIGTERM`）。次の`wait`/`poll`がreap |

`wait()` / `poll()`は冪等で、子をreapした後はどちらも同じキャッシュ済み結果
Object（通常の`{code, stdout, stderr, ok, signal, error, timed_out}`）を返します。
起動失敗は`Proc.run`と同様`ProcessError`をthrowし、`cwd` / `env` / `stdin` /
`share` / `inherit_env`の意味も`Proc.run`と同じです。一度もwaitされずに
捨てられたハンドルはGCがreapし（子を`SIGKILL`）、ゾンビとして残りません — ただし
明示的に`wait()` / `kill()`する方が明快です。他のverbと同様、シグナルは直接の子
にのみ送られます（孫には届きません）。

```culebra
# doctest: skip
let server = Proc.spawn(["python", "-m", "http.server", "8000"])
# ... サーバに対して作業 ...
server.kill()  # SIGTERM
let r = server.wait()
IO.inspect("server exited via " + (r.signal ?? to_string(r.code)))

# ブロックせずに完了をポーリング。
let job = Proc.spawn(["make", "-j4"])
while job.poll() == nil {
  IO.print(".")  # ...他の作業...
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

> `Isolate.spawn`・`Channel`・`Parallel`はいずれもVMと`--jit`の
> 両方で動作します（クロージャは共有コード参照 — VMはバイトコード、JITは
> コンパイル済み`fn_ptr` — とコピーした捕獲で越境し、子の自前ヒープで実行）。

### `Isolate.spawn(fn, *args) -> handle`

`fn`を別スレッドで実行し、即座にライブハンドルを返します。位置引数`args`は
`fn`に渡されます。

```culebra
# doctest: skip
let h = Isolate.spawn(|| 1 + 2)
h.join()  # => 3

let h2 = Isolate.spawn(|n| n * n, 7)
h2.join()  # => 49
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
let h = Isolate.spawn(fn () {
  xs.push(99)
  xs.size()
})
h.join()  # => 4   (isolate 側のコピー)
xs        # => [1, 2, 3]   (親は不変)
```

`mut`の捕獲は黙ってスナップショットを取らず拒否します — 値は引数として
渡してください:

```culebra
# doctest: skip
mut total = 0
Isolate.spawn(|| total)      # SendError: mutable 変数 'total' を捕獲
Isolate.spawn(|t| t, total)  # ok — 値渡し
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
for p in parts {
  handles.push(Isolate.spawn(|| p.reduce(0, |a, b| a + b)))
}
mut total = 0
for h in handles {
  total += h.join()
}
total  # => 45
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
let (tx, rx) = Channel.new(10)  # 有界・容量 10
let prod = Isolate.spawn(fn () {
  for line in source() {
    tx.send(parse(line))
  }
  tx.drop()  # この送信端を解放
})
tx.drop()  # 親側の送信端も解放
for record in rx {
  process(record)
}  # 全 tx が drop されると終了
prod.join()
```

| メソッド | 対象 | 意味 |
|---|---|---|
| `tx.send(v)` | tx | `v`を投入（バッファ満杯ならブロック）。closedなら`ChannelError` |
| `tx.clone()` / `rx.clone()` | 両方 | 同一channelの別endpoint（multi-producer / multi-consumer） |
| `tx.drop()` / `rx.drop()` | 両方 | このendpointを解放 |
| `rx.recv()` | rx | 1値をブロッキング取得。closedかつ空なら`nil` |
| `for v in rx { ... }` | rx | closedまでdrain（綺麗なend-of-streamの形） |
| `rx.try_recv()` | rx | ブロックせず1値取得。`Value(v)` / `Empty` / `Closed`を返す |
| `rx.drain(max = nil)` | rx | 今キューにある分を最大`max`件までブロックせず取得（Array、空もあり） |

**auto-closeがデッドロック安全網です。** アクティブな送信端を数え、**最後の`tx`
がdropされた**時（producer isolateが正常／例外終了）にchannelがcloseし、
`for v in rx`が終了します。producerがクラッシュしてもconsumerはハングせず、
原因はproducerを`join()`してsurfaceします。multi-producerの罠に注意:
channelが閉じるには全ての`tx`（親の元のtx含む）がdropされる必要があります —
保持しないtxはdropしてください。

**cloneしたendpointは競合し、複製はしません。** 各値はちょうど1つのendpoint
（最初に`recv`が届いたもの）に渡るので、`rx`のcloneはストリームをconsumerに
分散させるのであって、各consumerにコピーを配るのではありません。1つの
consumerでは捌ききれない時に`rx`をcloneします: 共有キューに長時間実行の
consumerを複数並べれば、producerがバッファ満杯で止まらずに済み、単一
consumerなら直列化されるバーストも吸収できます。consumerごとにコピーが
欲しい場合は、consumerごとのchannelへ送ってください。

**`Channel.new(0)`はrendezvous channel**（容量0）: `send`は受信者が値を
受け取るまで返りません — バッファ無しの同期ハンドオフ。backpressure
（producerがconsumerを追い越せない）に有用。単一isolate内ではdeadlock
（渡す相手がいない）のでisolate間で使います。容量は`0以上`。

#### ブロックしない受信 — `rx.try_recv`と`rx.drain`

`recv()`と`for v in rx`はブロックします。channelだけが仕事のconsumerには
それが正解ですが、自分のスケジュールで進み続けなければならない呼び出し側 —
`Canvas.run`のフレームコールバック、TUIのキーループ、その他あらゆる時間駆動の
ループ — はそこで止まれません。ブロックしたフレームは落ちたフレームです。
`try_recv()`は常に即座に返ります。

戻り値は`enum`の形をした3つのvariantのいずれかです:

```culebra
# doctest: skip
enum ChannelResult { Value(Any), Empty, Closed }
```

`Value(v)`はキューにあった値を運び、`Empty`は**今は**何もない（channelは
openのままなので後でまた聞く）、`Closed`はもう二度と何も来ない（全`tx`が
無くなりキューも空）ことを意味します。

```culebra
# doctest: skip
match rx.try_recv() {
  Value(ev) => world.apply(ev),
  Empty()   => nil,        # このフレームは何もなし — 続行
  Closed()  => running = false,
}
```

`Empty()`と`Closed()`の`()`に注意してください。matchのアームでの裸の`Empty`は
識別子パターンで、**何にでも**マッチして束縛するため、それ以降のアームは
決して実行されません。マッチしたものに名前を付けたければ`x: Empty`も使えます。

`nil`を返さず3つのvariantにしたのは、ここでは`nil`が3つの意味を同時に
持たざるを得ないからです: `recv()`は既に本物の`nil`ペイロードとend-of-stream
の両方で`nil`を返しており、ノンブロッキング読み出しではそこに「まだ何もない」
が加わります。`while (v = rx.try_recv()) != nil`と書いたループは、本物の`nil`
が流れてきた最初の瞬間に早期終了してしまいます。variantなら各ケースに
答えられます。

`drain()`はキュー全体を一度に取ります。フレームが欲しいのは通常こちらです:

```culebra
# doctest: skip
for ev in rx.drain() { world.apply(ev) }
```

返るのはArrayで、何もなければ空、closedなchannelでも空なので、closeを
検知する必要があれば`try_recv()`と併用してください。返るのは**呼んだ瞬間に
キューにあった分**で、1ステップで取り出されます。これは見た目以上に重要です:
1件ずつpopするとスロットが空き、ブロック中のsenderが起きて、次のpopが見る前に
補充するため、容量4のchannelに対する`try_recv()`ループが数百件を返す例が
実測されています。`drain()`はこの暴走をしませんが、自分で書いた`try_recv()`
ループはします。

キュー自体が長くなりうる場合は、`max`でバッチをさらに縛れます:

```culebra
# doctest: skip
for ev in rx.drain(16) { world.apply(ev) }   # このフレームは最大16件
```

残りはキューに留まり次回に回ります。`max`は`Long`（件数）か制限なしの`nil`
で、負値は`ValueError`になります。

どちらのメソッドも`Channel.fan_in`のreceiverではまだ使えません。プレーンな
`rx`専用です。

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
  handles.push(Isolate.spawn(fn () {
    produce(w, tx)
  }))        # producer の tx は終了時 auto-drop
  tx.drop()  # 親自身の tx（1:1、自明）
  sources.push(rx)
}
for v in Channel.fan_in(sources) {
  consume(v)
}  # 1 本のストリーム、全 producer
for h in handles {
  h.join()
}  # handle を保持・エラー回収
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
  for x in produce(w) {
    tx.send(x)
  }
})
for v in merged {
  consume(v)
}
merged.join()  # producer を join、最初のエラーを再送出
```

`merged.join()`（stream終了後）はproducerをjoinし最初のエラーを再送出。
呼ばなければproducerのエラーは握り潰し（`Isolate.spawn` handleをjoinしない
のと同じ）。producerは専用スレッドで実行されます。

### Parallel — `Parallel.map` / `each` / `map_settled` / `race`

高レベル形。配列の各要素に関数をisolateプールで並列適用します（ハンドル管理
不要）。`fn`と各要素はSendableでなければなりません（`Isolate.spawn`と同じ規則）。

```culebra
# doctest: skip
Parallel.map([1, 2, 3, 4], |x| x * x)       # => [1, 4, 9, 16]  (入力順)
Parallel.map(urls, |u| fetch(u), limit: 8)  # 同時 isolate は最大 8
Parallel.each(jobs, |j| process(j))         # 副作用のみ、nil を返す
Parallel.map_settled(urls, |u| fetch(u))    # => [{ok, value, error}, ...]
Parallel.race(mirrors, |m| download(m))     # 最初の成功が勝つ
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
Parallel.map(
  urls,
  |u| fetch(u),
  on_progress: |done, total| IO.print("\r" + done.to_string() + "/" + total.to_string()),
)
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
Signal.notify(tx)  # Ctrl+C は throw でなくチャネルへ
serve_in_background()
rx.recv()  # 最初の Ctrl+C までブロック
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
@packable
class FloatPair {
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
@packable
class Cell {
  v: Int64 = 0
}
let buf = SharedBuffer.file("/tmp/grid.bin", 100, Cell)
buf[0].v = 42
buf.flush()  # ディスクへ永続化
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
@packable
class FloatPair {
  x: Float32 = 0.0
  y: Float32 = 0.0
}
let buf = SharedBuffer.new(3, FloatPair)
inspect(buf.size)  # => 3
buf[0].x = 1.5     # その場でバイトを書く
let v = buf[0]     # 保持した view は同じ要素を指す
v.y = 2.5
inspect([buf[0].x, buf[0].y])  # => [1.5, 2.5]
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
@packable
class Cell {
  v: Int64 = 0
}
let cells = SharedBuffer.new(8, Cell)

Parallel.each([0, 1, 2, 3, 4, 5, 6, 7], fn (i) {
  cells[i].v = i * i
})

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
@packable
class Cell {
  v: Int64 = 0
}
let grid = SharedBuffer.shared(4, Cell)
grid[0].v = 100
Proc.run([Sys.executable, "worker.cul"], share: {grid: grid})
inspect(grid[0].v)  # 子の書き込みをここで読み戻す
grid.drop()
```

```culebra
# doctest: skip
# --- worker.cul ---
@packable
class Cell {
  v: Int64 = 0
}
let grid = SharedBuffer.receive("grid", Cell)
for i in 0..grid.count {
  grid[i].v = grid[i].v + (i + 1) * 10
}
grid.drop()
```

`receive`は名前と`@packable`型だけを取る — 要素数は親由来（なので
`grid.count`が一致する）。子は同じ`@packable`クラスを宣言し、`receive`は
レコードサイズの一致を確認して、レイアウト不一致・未知の名前・
`SharedBuffer.shared(...)`でないbufferのときは`ValueError`を投げる（ヒープと
ファイルのbufferはこの方法では渡せない — ファイルbufferは`path`を開き直して
共有する）。`Sys.executable`は実行中のculebraバイナリのパスで、culebraの
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
@packable
class Counter {
  n: Int64 = 0
}
let tally = SharedBuffer.new(1, Counter)

Parallel.each(iota(0, 8), fn (w) {
  for _ in 0..1000 {
    tally.with_lock(fn () {
      tally[0].n = tally[0].n + 1
    })
  }
})
inspect(tally[0].n)  # ちょうど 8000 — lost update なし
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
@packable
class Row {
  id: Int32
  name: FixedString<16>
}

let rows = SharedBuffer.new(100, Row)
rows[0].name = "alice"  # まるごと書き込み（≤ 16 バイト）
rows[0].name            # => "alice"   （本物の String）
rows[0].name.upper()    # => "ALICE"   （String の全メソッドが効く）
rows[1].name            # => ""        （ゼロ値は空文字列）
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
@packable
class Node {
  id: Int32
  parent: Int32?  # 疎な「親なし」スロット
}

let n = SharedBuffer.new(100, Node)
n[0].parent  # => nil   （ゼロ値）
n[0].parent = 5
n[0].parent        # => 5
n[0].parent = nil  # クリア
n[0].parent ?? -1  # => -1
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
@packable
enum Shape {
  Circle(Float32),
  Rect(Float32, Float32),
  Point,
}

@packable
class Obj {
  id: Int32
  shape: Shape
}

let objs = SharedBuffer.new(100, Obj)
objs[0].shape = Shape.Rect(2.0, 3.0)  # variant 値を書く
match objs[0].shape {
  # 読み戻して match
  Rect(w, h) => w * h,
  Circle(r) => 3.14 * r * r,
  Point => 0.0,
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
@packable
class Entry {
  id: Int32
  digest: Bytes<32>  # 例: SHA-256
}

let e = SharedBuffer.new(100, Entry)
e[0].digest = some_32_byte_string
e[0].digest  # => 32 バイト（バイナリ安全）
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
@packable
class Point {
  x: Float32
  y: Float32
}
@packable
class Line {
  id: Int32
  start: Point
  end: Point
}

let lines = SharedBuffer.new(100, Line)
lines[0].start.x = 1.0  # インライン Point のバイトに書く
lines[0].start.y = 2.0
lines[0].start.x               # => 1.0
lines[0].end = lines[0].start  # サブレコードまるごとコピー（memcpy）
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
  dict["hello"]  # 全 isolate が同じ凍結ツリーを読む
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

捕捉されなかった失敗は`assert_*`呼び出しの位置を報告します:

```
AssertionError at 12:1: assert_eq failed:
  left:  foo
  right: bar
```

`assert_throws`以外のmatcherは末尾に省略可能な`label`を取り、失敗
メッセージがそれを繰り返します。効いてくるのは位置では2つの失敗を
区別できない場面 — 呼び出し元に代わってassertするヘルパーは、どの
失敗も自分自身の同じ1行として報告するからです:

```culebra
let check = fn (got, want, label) {
  assert_eq(got, want, label)
}
check(1, 1, "first")   # pass
# ここで失敗すれば: assert_eq failed: second
```

`assert`キーワード / builtinは存在しません — 用途ごとに専用matcher
を使います。productionの不変条件には`if`/`throw`を直接書きます:

```culebra
# doctest: skip
if !cond {
  throw {kind: "AssertionError", message: "invariant violated"}
}
```

### 真偽 matcher

* **`assert_true(x: Bool, label: String? = nil) -> Nil`** — `x`がtruthyならpass。失敗
  時は`assert_true failed:\n  value: {x}`。`x`は`Bool` / `Long` /
  `Float`のみ — それ以外は`TypeError`。暗黙のtruthinessは無く、
  空文字列・空配列はfalsyではありません。
* **`assert_false(x: Bool, label: String? = nil) -> Nil`** — `assert_true`の逆。

### 比較 matcher

各比較matcherは **同名の演算子と同じdispatch** を行います —
`assert_eq(a, b)`は`a == b`と等価で、クラスインスタンスの
`__eq__` / `__lt__` / `__le__`が尊重されます。失敗messageは
`to_string`で両辺を表示 (ユーザ`__str__`を尊重)。

* **`assert_eq(a, b, label: String? = nil) -> Nil`** — `a == b`。
* **`assert_ne(a, b, label: String? = nil) -> Nil`** — `a != b`。
* **`assert_lt(a, b, label: String? = nil) -> Nil`** — `a < b`。
* **`assert_le(a, b, label: String? = nil) -> Nil`** — `a <= b`。
* **`assert_gt(a, b, label: String? = nil) -> Nil`** — `a > b`。
* **`assert_ge(a, b, label: String? = nil) -> Nil`** — `a >= b`。

```culebra
assert_eq(1 + 1, 2)  # 成功時は無音

let r = try {
  assert_eq("foo", "bar")
  nil
} catch e {
  e
}
inspect(r.kind)  # => 'AssertionError'
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
assert_throws("ZeroDivisionError", fn () {
  let _ = 1 / 0
})
assert_throws("MyError", fn () {
  throw {kind: "MyError", message: "boom"}
})
```

### `assert_close(a: Float, b: Float, tol: Float, label: String? = nil) -> Nil`

`|a - b| <= tol`ならpass。`a` / `b` / `tol`のいずれかがNaNなら
**故意に失敗** (素朴な`diff > tol`だとNaNがsilently passする
ため)。浮動小数比較は`assert_eq`ではなくこちらを使う。

```culebra
assert_close(3.14, 3.1415, 0.01)
```

### 実装ノート

matcher一族はculebraソース (cppではなく) で定義されており、
lazy module機構で3 backend (VM / JIT / AOT) に共通でbind
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
inspect(Regex.find('(\d+)', "ab12")[1])                 # => '12'
inspect(Regex.test('(?i)hello', "HELLO"))               # => true
inspect(Regex.replace_all('[;；]', "a;b；c", "、"))  # => 'a、b、c'
# ミスは nil なので `?.` / `??` と合成できます:
inspect(Regex.find('x', "y")?.value ?? "none")  # => 'none'
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
inspect(d.test("abc 123"))                                 # => true
inspect(Regex.compile('\w+').find("  hello world").value)  # => 'hello'
inspect(d.find("no digits"))                               # => nil
inspect(d.find_all("a1 b22 c333").size())                  # => 3
```

キャプチャは位置（`m[1]`）でも名前（`m["year"]`）でも取れます。`m[0]`は
マッチ全体、ミスは`nil`。`m.groups` / `m.named`の`Group`オブジェクトは
文字列に加えてspanも持ちます:

```culebra
let m = Regex.compile('(?<year>\d{4})-(\d{2})').find("2026-05")
inspect(m[1])                   # => '2026'
inspect(m["year"])              # => '2026'
inspect(m[0])                   # => '2026-05'
inspect(m[9] ?? "none")         # => 'none'
inspect(m.groups[1].value)      # => '2026'
inspect(m.named["year"].value)  # => '2026'
```

置換と分割 — 置換文字列は`$n`テンプレートか、`Match`を受け取る関数です。
`replace_all`は全マッチを置換し、`replace_first`は最左マッチだけを置換して
残りはそのまま残します（マッチが無ければ`s`をそのまま返すno-op）:

```culebra
let d = Regex.compile('\d+')
inspect(d.replace_all("a1 b22 c333", "#"))                         # => 'a# b# c#'
inspect(d.replace_first("a1 b22 c333", "#"))                       # => 'a# b22 c333'
inspect(Regex.compile('(\w+)@(\w+)').replace_all("x@y", '$2.$1'))  # => 'y.x'
inspect(d.replace_all("a1 b22", fn (m) {
  "<{m.value}>"
}))                                                       # => 'a<1> b<22>'
inspect(Regex.compile('\s+').split("the quick  brown"))   # => ['the', 'quick', 'brown']
inspect(Regex.compile('hello', "i").test("HELLO world"))  # => true
```

`find_iter`は遅延なので走査を途中で止められます。
`for m in d.find_iter(s) { break }`はbreakしたマッチより先には進みません:

```culebra
let d = Regex.compile('\d+')
inspect(d.find_iter("1 2 3").take(2).collect().size())  # => 2
inspect(Regex.escape("a.b(c)"))                         # => 'a\.b\(c\)'
```

対応構文（literal / `.` / 文字クラス / `* + ? {n,m}` greedy・lazy / `|` /
キャプチャ・名前付きグループ / `\d \w \s \b` / lookahead / 可変長lookbehind /
`\p{…}` Unicodeプロパティ）とマッチモデル・資源上限は、vendor化したエンジン
[cpp-regexlib](https://github.com/yhirose/cpp-regexlib)に記載しています。

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
| `Http.sse(url, on_event, headers=nil, timeout=0, follow_redirects=true)` | レスポンスObject — `GET`を開いてServer-Sent Eventsを`on_event`にストリーム（後述） |
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
  let users = r.json()  # レスポンスボディを JSON としてパース
  IO.inspect(users.size().to_string())
} else {
  IO.inspect("request failed: {r.status}")
}

# ヘッダとタイムアウトを指定して JSON を POST（`json:` が serialize + Content-Type 設定）。
let resp = Http.post(
  "https://api.example.com/users",
  json: {name: "alice"},
  headers: {Authorization: "Bearer " + token},
  timeout: 30,
)
assert_true(resp.ok)

# トランスポート失敗は throw するが、404 は throw しない。
let missing = Http.get("https://api.example.com/nope")
assert_eq(missing.ok, false)  # 404 は通常の結果
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
Http.get("https://example.com/big.tar.gz", into: "big.tar.gz")  # → ファイル

mut bytes = 0
Http.get(
  "https://example.com/big.csv",
  into: fn (chunk) { bytes += chunk.size() },
)

# 任意のメソッド。例: レスポンスがストリームで返る POST:
Http.post(
  "https://example.com/query",
  body: q,
  into: fn (chunk) { handle(chunk) },
)
```

**ストリーミング（アップロード）。** 対称に、`body:`（`post` / `put` /
`request`）へ`Function`を渡すとリクエストボディをchunkedでストリームします
（大きなアップロードもメモリに全部載せない）。producerは繰り返し呼ばれ、次の
chunk `String`を返し、`nil`でストリーム終端を示します:

```culebra
# doctest: skip
let f = File.open("big.bin")
Http.post(
  url,
  body: fn () {
  let chunk = f.read(65536)
  !chunk.empty() ? chunk : nil  # nil で終端
},
  content_type: "application/octet-stream",
)
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
Http.post(
  url,
  files: {
  title: "My report",
  doc:   { content: "a,b,c\n1,2,3\n", filename: "data.csv", content_type: "text/csv" },
},
)

# 大きなファイルをディスクから直接ストリーム（全体をバッファしない）
Http.post(
  url,
  files: { clip: { path: "/movies/big.mp4", content_type: "video/mp4" } },
)

# 生成に時間がかかる part を producer からストリーム
mut row = 0
Http.post(
  url,
  files: {
  export: { filename: "rows.csv", content_type: "text/csv", stream: fn () {
    row += 1
    row <= 1000 ? "{row},{compute(row)}\n" : nil
  } },
},
)

# Array で同一フィールド名に複数 part
Http.post(
  url,
  files: {
  caption: "trip",
  photos:  [ { path: "./1.jpg" }, { path: "./2.jpg" } ],
},
)
```

partの値が`String` / `Object` / `Array`以外、`Object`が`content` / `path` /
`stream`を正確に1つ持たない、`content` / `path`が非`String`、`stream`が非
`Function`、はいずれも`TypeError`。開けない`path`は`IOError`です。

### `Http.sse(url, on_event, headers=nil, timeout=0, follow_redirects=true) -> Object`

[Server-Sent Events](https://developer.mozilla.org/docs/Web/API/Server-sent_events)
（`text/event-stream`）ストリームを開きます — 長寿命の`GET`で、イベントが届くたびに
`on_event`コールバックを1回ずつ呼びます。呼び出しはストリームが続く間blockingで、
サーバが閉じた後に最終的なレスポンスObjectを返します。

**開くのは`GET`だけです。** プロトコル自体がそうなっており（ブラウザの`EventSource`も
POSTできません）、ストリーミングLLM/チャットAPIは同じワイヤ形式をリクエストをJSON
bodyに載せた**`POST`**で要求するため、この関数では届きません。それらは上記の`into:`で
レスポンスbodyを受け取り、フレームを自分でデコードします（後述）。

各イベントは3つのStringフィールドを持つObjectです:

| フィールド | 意味 |
|---------|---------|
| `event` | `event:`タイプ。サーバが送らない場合は`"message"` |
| `data`  | `data:`ペイロード。複数の`data:`行は`\n`で連結 |
| `id`    | 最後に見た`id:`フィールド。無ければ`""` |

```culebra
# doctest: skip
Http.sse("https://api.example/v1/events", fn (e) {
  if e.event == "progress" {
    IO.println("{e.id}: {e.data}")
  }
})
```

回答をストリームで返す`POST`——LLM APIはすべてこれ——はもう一方の経路を使います。
`into:`がbodyをチャンク単位で渡すので、フレームの分割は自分で行います:
`field: value`の行、イベントの区切りは空行1つ、そしてチャンク境界はどこにでも
（行の途中にも、文字の途中にも）落ちます。

```culebra
# doctest: skip
Http.post(
  "https://api.example/v1/chat",
  json: {model: model, messages: messages, stream: true},
  into: fn (chunk) { decoder.feed(chunk) },
)
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
let api = Http.client(
  "https://api.example.com/v1",
  headers: {Authorization: "Bearer " + token},
  timeout: 30,
)

let me = api.get("/me").json()             # → GET https://api.example.com/v1/me
let user = api.get("/users/42").json()     # 同じ接続を再利用
api.post("/users", json: {name: "alice"})  # Authorization ヘッダが付く

api.get("/items", headers: {"Idempotency-Key": k})  # 既定の上にマージ
api.close()                                         # 接続を解放
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
Parallel.map(urls, |u| Http.get(u).body)     # all、入力順（fail-fast）
Parallel.map_settled(urls, |u| Http.get(u))  # allSettled: [{ok, value, error}, ...]
Parallel.race(urls, |u| Http.get(u))         # 最速成功が勝ち、残りはキャンセル
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
srv.get("/", fn (req) {
  "Hello, world!"
})
srv.get("/users/:id", fn (req) {
  "user " + req.params["id"]
})
srv.post("/echo", fn (req) {
  req.body
})
srv.get("/json", fn (req) {
  {
    status: 201,
    body: '{"ok":true}',
    content_type: "application/json",
    headers: {"X-Trace": req.headers["X-Request-Id"]},
  }
})
srv.static("/assets", "./public")
srv.listen(8080)  # ブロックする。Ctrl+C で停止
```

| メソッド | 効果 |
| --- | --- |
| `get/post/put/delete/patch/options(pattern, handler)` | そのメソッドとルート`pattern`に`handler`（`fn(req)->response`）を登録。サーバを返す（チェーン可） |
| `static(mount, dir)` | URLプレフィックス`mount`で静的ファイルを配信。`dir`はStringパス（ディスク上のディレクトリをライブ配信）または`Embed.dir(...)`ハンドル（AOTではバイナリに焼き込み — [Embed](#embed) 参照） |
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
srv.get("/events", fn (req) {
  {content_type: "text/event-stream", headers: {"Cache-Control": "no-cache"}, stream: fn (sink) {
    for i in 0..10 {
      sink.write("data: " + i.to_string() + "\n\n")
    }
  }}
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
| `ws.try_receive()` | ブロックせずに1件取る。`Message(String)`・`Empty`・`Closed`のいずれか（`set_timeout`が要る。下記参照） |
| `ws.send(msg)` | テキストメッセージを送信。peer切断時は`false` |
| `ws.set_timeout(ms)` | 読み取りが`Empty`を返すまでの待ち時間。`0`は無期限 |
| `ws.close()` | 接続を閉じる |
| `ws.is_open()` | 接続がまだ開いているか |

```culebra
# doctest: skip
srv.ws("/echo", fn (req, ws) {
  for msg in ws {
    ws.send(msg)
  }
})
srv.ws("/chat", fn (req, ws) {
  while true {
    let m = ws.receive()
    if m == nil {
      break
    }  # peer が close
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
let model = Shared.new(load_weights())  # read-only 1 コピーを全 worker で共有
let srv = Http.server()
srv.post("/predict", fn (req) {
  infer(model, req.body)
})
srv.listen(8080, workers: 8)  # 8 ハンドラが並列実行
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
srv.get("/health", fn (req) {
  "ok"
})
srv.listen_async(8080, workers: 4)  # 即 return
# … 別作業をしつつ Http.get("http://127.0.0.1:8080/health") …
srv.stop()  # 停止して背後スレッドを join
```

あるいは、ブロッキング`listen`をisolate内で動かし、そのisolateをdrop
（または`Ctrl+C`）してacceptループを止める方法もあります:

```culebra
# doctest: skip
let srv_iso = Isolate.spawn(fn () {
  let srv = Http.server()
  srv.get("/health", fn (req) {
    "ok"
  })
  srv.listen(8080, workers: 4)
})
# … メインスレッドから Http.get("http://127.0.0.1:8080/health") …
srv_iso.drop()  # サーバに停止を通知して join
```

**起動完了とポート番号を知る — `bind` + `serve`。** ブロッキング`listen`は返らない
ので何も報告できません。「ソケットが開いた」ことも、`port 0`のときにOSが選んだ番号
も伝えられません。2つに分けると、その両方が判明している地点が生まれます。**`bind`
が返ること自体が起動完了の合図**です — ソケットはバックログ付きで開いているので、
その後に張られた接続は`serve`がacceptを始める前でもカーネルが受けています。

```culebra
# doctest: skip
let (tx, rx) = Channel.new(1)
let srv_iso = Isolate.spawn(fn () {
  let srv = Http.server()
  srv.get("/health", fn (req) {
    "ok"
  })
  tx.send(srv.bind(0))  # 0 = 空いているポート。番号を外へ
  tx.drop()
  srv.serve()  # ここでブロック
})
tx.drop()  # 親自身の sender コピー
let base = "http://127.0.0.1:" + rx.recv().to_string()
inspect(Http.get(base + "/health").body)  # => 'ok'
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
Log.info("http server listening", {port: port})
srv.get("/whoami", fn (req) {
  "http://127.0.0.1:" + port.to_string()
})
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
`try_receive()`・`set_timeout(ms)`・`for msg in ws`・`close()`・`is_open()`）を
返します。不正なURLや接続失敗は`HttpError`です。

```culebra
# doctest: skip
let ws = Http.ws("ws://127.0.0.1:8080/echo")
ws.send("hello")
inspect(ws.receive())  # => エコーされたメッセージ
for msg in ws {
  handle(msg)
}  # サーバが close するまでメッセージを drain
ws.close()
```

#### 1本の接続で読みながら送る — `set_timeout`と`try_receive`

wsハンドルはそれを開いたスレッドのものです（Sendableではなく、背後のレジストリも
スレッドごと）。つまり接続の送受信は1箇所から動かすことになります。ところが
`receive()`と`for msg in ws`はメッセージが届くまで戻らないので、待っている間は
送信の番が回ってきません。

`set_timeout(ms)`でその待ち時間を区切り、結果を`try_receive()`が答えます。返る形は
[`Channel.try_recv`](#ブロックしない受信--rxtry_recvとrxdrain)と同じ3変種で、理由も
同じです。`nil`に「メッセージ」「まだ来ていない」「もう来ない」の3つを兼ねさせられ
ません。

```culebra
# doctest: skip
enum WsResult { Message(String), Empty, Closed }
```

```culebra
# doctest: skip
ws.set_timeout(50)  # ミリ秒。0は無期限
while running {
  match ws.try_receive() {
    Message(m) => world.apply(m),
    Empty()    => nil,          # 今回は何も来ていない。このまま送信へ
    Closed()   => running = false,
  }
  for out in outbox.drain() {
    ws.send(out)
  }
}
```

`Empty()`と`Closed()`の`()`は`Channel`のmatchと同じ理由で要ります。裸の`Empty`は
何にでも一致する識別子パターンです。

タイムアウトを設定しても`receive()`と`for msg in ws`の意味は変わりません。どちらも
**次のメッセージ**を待つもので、タイムアウトはメッセージが無いというだけで終端では
ないからです。ただし待ち直す隙にCtrl+Cが届くようになるので、中断できるようには
なります。

### Embed

`Embed.dir(name)`はアセットディレクトリのハンドルを返し、**バックエンドごとに**
（コード変更なしで）解決されます:

- **ソース実行**（VM / JIT）: `name`のディスク上ディレクトリをライブに
  読む（エントリスクリプト相対で解決）。ファイルを編集して実行し直せば即反映
  ＝開発ループ。
- **`culebra build`**（AOT）: ビルド時にディレクトリを走査してバイト列をバイナリ
  に焼き込み、外部ファイル無しで読む。焼き込んだ内容はビルドが表示する
  （`embedded N file(s) (… bytes) from '…'`）。

| メソッド | 返り値 | 備考 |
|---|---|---|
| `dir.read(path)` | `String` | ファイルのバイト列（バイナリセーフ）。無ければ`IOError` |
| `dir.exists(path)` | `Bool` | `path`がそのディレクトリのファイルか |

`path`はディレクトリ相対でスラッシュ区切り（`"sub/logo.png"`）。絶対パスや`..`を
含むパスは外に出るのではなく「見つからない」として扱われる。

```culebra
# doctest: skip
let art = Embed.dir("assets")
let sheet = Canvas.Sprite.from_png(art.read("sprites.png"))
if art.exists("music.ogg") {
  Canvas.music(art.read("music.ogg"))
}
```

同じハンドルでディレクトリ全体をHTTP配信できる:

```culebra
# doctest: skip
let srv = Http.server()
srv.static("/", Embed.dir("dist"))  # フロントエンド全体を1行で
srv.get("/api/ping", fn (req) {
  '{"ok":true}'
})
srv.listen(8080)
```

`name`はAOTビルドが探して焼き込めるよう**文字列リテラル**であること（計算した
パスはソース実行では動くが焼き込まれない）。`srv.static`経由ではContent-Typeは
拡張子から推論、ディレクトリ（や`/`）へのリクエストはその`index.html`、
ディレクトリに無いパスは登録ルートにフォールスルー（APIルートが常に優先）。
`Embed.dir`は`Http`非依存＝アセットを読むだけのプログラムにサーバは要らない。
ハンドルはSendableで、別のisolateに送ると同じディレクトリのハンドルとして
向こう側で再構築される。

`culebra build`は焼き込むアセットをculebraのヘッダに対してコンパイルするため、
ソースチェックアウトが必要。既定はそのバイナリをビルドしたときのパスで、
`$CULEBRA_HOME`があればそちらが優先される。どちらも無ければバイナリを作らず
エラーで停止する。

---

## 16. `Encoding`

テキストコーデックを**スキームごとのサブ名前空間**にまとめた名前空間
（`Encoding.html`、`Encoding.base64`、`Encoding.hex`、`Encoding.url`）。
コーデックのロジックはVMとJIT/AOT両バックエンドで共有しており、
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
inspect(Encoding.html.escape("a & b < c"))                # => 'a &amp; b &lt; c'
inspect(Encoding.html.escape("it's fine"))                # => 'it&#39;s fine'
inspect(Encoding.html.unescape("Tom &amp; Jerry"))        # => 'Tom & Jerry'
inspect(Encoding.html.unescape("caf&eacute; &mdash; x"))  # => 'café — x'
inspect(Encoding.html.unescape("&#65;&#x42;"))            # => 'AB'
inspect(Encoding.html.unescape("&#12354;"))               # => 'あ'
inspect(Encoding.html.unescape("&unknownent;"))           # => '&unknownent;'
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
inspect(Encoding.base64.encode("user:pass"))     # => 'dXNlcjpwYXNz'
inspect(Encoding.base64.decode("dXNlcjpwYXNz"))  # => 'user:pass'
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
inspect(Encoding.hex.encode("abc"))          # => '616263'
inspect(Encoding.hex.decode("616263"))       # => 'abc'
inspect(Encoding.hex.decode("00FF").size())  # => 2
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
inspect(Encoding.url.encode("a b&c"))      # => 'a%20b%26c'
inspect(Encoding.url.decode("a%20b%26c"))  # => 'a b&c'
inspect(Encoding.url.encode("café"))      # => 'caf%C3%A9'
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
inspect(z.size() < original.size())      # => true
inspect(Compress.gunzip(z) == original)  # => true
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
let z2 = Compress.deflate(text)       # gzip ではなく zlib ラッパー
inspect(Compress.gunzip(z2) == text)  # 同じデコーダ
# => true
inspect(z2.size() < Compress.gzip(text).size())  # gzip ヘッダが無い分
# => true
```

`level`はzlib自身の規約に従います: `-1`（既定）はzlib内蔵のトレードオフ、
`0`は無圧縮で格納、`9`は最も時間をかけて最小の出力にします。`-1..9`の範囲
外の値はその呼び出しで`ValueError`。

```culebra
let text = "the quick brown fox the quick brown fox the quick brown fox"
inspect(Compress.deflate(text, level: 9).size() <=
  Compress.deflate(text, level: 0).size())  # => true
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
inspect(rows[1])                                                    # => ['alice', '30']
inspect(CSV.stringify([["a,b", "c"], [1, 2]]) == "\"a,b\",c\n1,2")  # => true
inspect(CSV.parse("a\tb", delimiter: "\t")[0])                      # => ['a', 'b']
```

**ヘッダモード — `header: true`.** 1行目を列名とし、以降の各行を（位置Arrayでなく）
その名前をキーにした`Object`にする：

```culebra
let rows = CSV.parse("name,age\nalice,30\nbob,25", header: true)
inspect(rows[0]["name"])  # => 'alice'
```

データ行が無いヘッダ（や空入力）は`[]`。ヘッダ名の重複や、ヘッダとフィールド数が
食い違うデータ行は`ValueError`。

**型付き列 — `types:`.** `types:`（ヘッダ名 → `"String"` / `"Long"` / `"Float"` /
`"Bool"`の`Object`）を渡すとその列を変換する。未指定の列は`String`のまま。変換は
**明示・推論しない** — 郵便番号やIDを`String`と宣言すれば元のテキストが正確に保たれる
（先頭ゼロや精度の喪失なし）。`types:`は`header: true`を要する。

```culebra
let rows = CSV.parse(
  "name,age,active\nalice,30,true",
  header: true,
  types: {age: "Long", active: "Bool"},
)
# age はもう本物の Long（String でない）ので算術が効く:
inspect(rows[0]["age"] + 1)  # => 31
# 郵便番号は元テキストのまま — 数値推論なし:
let z = CSV.parse("zip\n01234", header: true, types: {zip: "String"})
inspect(z[0]["zip"])  # => '01234'
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
inspect(cfg["PORT"])   # => '8080'
inspect(cfg["NAME"])   # => 'my app'
inspect(cfg["DEBUG"])  # => 'true'
```

```culebra
# doctest: skip
Env.load(".env")  # ./.env があれば変数を設定
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
inspect(UUID.v4().size())        # => 36
inspect(UUID.v4() != UUID.v4())  # => true
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
inspect(Term.bold(Term.fg("alert", 196)))           # 太字・明るい赤の "alert"（表示用）
let st = Term.style(fg: (255, 128, 0), bold: true)  # Screen セル用
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
  s.poll(2.0)  # 最大 2 秒キー入力を待つ
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

let log = Log.with({request_id: id})  # 文脈を一度束縛
log.info("received")                  # ...以後の全行に付与
log.error("upstream failed", {status: 502})
```

値はtextでは`to_string`、jsonではJSON namespaceで直列化される。致命的状況は
`error`でログして`Sys.exit(1)`する（専用の`fatal`レベルは無い）。

---

## 24. `TOML`

[TOML](https://toml.io) 設定のparse / 生成。文法と直列化は値中立コアに置かれ、
VM・JIT・AOTがバイト単位で一致する。

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
再クォートされる。`\uXXXX` / `\UXXXXXXXX`エスケープはUnicodeスカラ値を
指さねばならず、サロゲート（`U+D800`〜`U+DFFF`）や`U+10FFFF`超のコードポイントは
parseエラーになる — culebra自身の文字列エスケープと同じ境界である。
不正な入力は`ValueError`を投げ、`e.line` / `e.col`
（いずれも1始まり）が問題の文字を指す:

```culebra
let r = try {
  TOML.parse("x = ")
  nil
} catch e {
  e
}
inspect(r.message)           # => 'TOML.parse: expected value'
inspect("{r.line}:{r.col}")  # => '1:5'
```

ネストの深さには上限がある: 木が1000段を超える文書 — ネストした配列 /
インラインテーブルでも、dottedキーでも — は、Cスタックを使い果たす代わりに
`ValueError`（`nesting too deep (limit 1000)`）を投げる。`stringify`も
走査する値の木に同じ上限を適用する。

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
inspect(cfg.title)        # => 'demo'
inspect(cfg.server.host)  # => 'localhost'
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
inspect(rows[0]["name"])  # => 'Alice'

# 再利用可能なプリペアド文
let ins = db.prepare("INSERT INTO users VALUES (?, ?)")
for u in [[2, "Bob"], [3, "Carol"]] {
  ins.run(u)
}
ins.finalize()

# all-or-nothing
db.transaction(fn () {
  db.execute("UPDATE users SET name = 'Bob!' WHERE id = 2")
})

let r = try {
  db.query("SELECT * FROM missing")
  nil
} catch e {
  e
}
inspect(r.kind)  # => 'SQLiteError'

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
ビルドにはSDL3が挙げるビルド依存が必要で、探索するX11 / 音声のヘッダが
無いとSDL3のconfigureが失敗するので、入っていないマシンでは
`-DCULEBRA_ENABLE_CANVAS_WINDOW=OFF`でconfigureする。Windowsでは追加の
パッケージは不要 — SDL3のWin32バックエンドが要るヘッダはmingw-w64の
ツールチェーンに揃っている。できたバイナリはどこでも
動く: SDL3はX11/GL/音声（Windowsなら`opengl32.dll`など）を初回使用時に
読み込むので、ウィンドウビルドでもロード時のライブラリ依存は増えず、
ディスプレイの無いサーバでも問題なく起動する。
各`present`はフレームをアップロードし、最近傍で見やすい
サイズに整数倍拡大し、60fpsでvsyncまでブロックする。キーボードとマウスが
`Canvas.buttons` / `Canvas.mouse`になり、ウィンドウを閉じる（クローズボタン、
またはスクリプト自身の`Canvas.quit()`）と`run`ループが終わる — Escはここでは
ただのキー（`Canvas.key`参照）であり、組み込みの終了操作ではない。
**ヘッドレスは宣言するもので、推測されるものではない**:
ウィンドウバックエンド無しのビルド、および`CULEBRA_CANVAS_HEADLESS`が
`0` / `off`以外に設定された実行では**ヘッドレス**: ピクセル / スプライト操作は同一に
動く（振る舞いはVM / JIT / AOTで一致し`Canvas.get_pixel`で検証可能）が、
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
inspect(Canvas.rgb_to_hsv(255, 0, 0))                         # => (0.0, 1.0, 1.0)
inspect(Canvas.hsv_to_rgb(0.0, 1.0, 1.0))                     # => (255, 0, 0)
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

`set_pixel`、`get_pixel`、`rect`、`line`、`circle`、`ellipse`、
`triangle`は、それぞれ`(x, y)`座標ペアの代わりに[`Vector2`](#30-vector2)
も受け付けます——`Canvas.line(Vector2.new(0, 0), Vector2.new(10, 10),
color)`は`Canvas.line(0, 0, 10, 10, color)`と等価です。`fill:`はどちら
の形でも位置引数・キーワード引数のいずれでも指定できます。`polygon`
に`Vector2`版はありません(`points`引数は平坦な`Array`で、宣言された
パラメータ型だけでは数値の`Array`と`Vector2`の`Array`を区別できない
ため)。

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
| `Canvas.toggle_fullscreen()` | フルスクリーンを切り替える |
| `Canvas.fullscreen() -> Bool` | 今フルスクリーンかどうか |
| `Canvas.resizable(enabled)` | OSウィンドウをドラッグで拡大縮小できるようにする |
| `Canvas.resized() -> Bool` | このフレームでウィンドウのピクセルサイズが変わったか |
| `Canvas.show_cursor()` / `Canvas.hide_cursor()` | OSマウスカーソルを表示 / 非表示にする |
| `Canvas.cursor_hidden() -> Bool` | 今カーソルが隠れているか |
| `Canvas.clipboard() -> String` | OSクリップボードを読む |
| `Canvas.set_clipboard(text)` | OSクリップボードに書く |
| `Canvas.quit()` | ウィンドウを閉じ、クローズボタンと同じ形で`run`のループを終える |
| `Canvas.can_quit() -> Bool` | 閉じられる実ウィンドウがあるか |

ブラウザで何もしないのは未実装ではなく方針です。タブのタイトルはキャンバスを
載せているページのものであって、その上で動くプログラムのものではありません
— 上の他の項目もすべて同じ理由でブラウザとヘッドレスではno-opです。そこには
影響を与えるべきOSウィンドウがそもそも無いからです。

`Canvas`のウィンドウは通常固定サイズです。`resizable`はOSウィンドウ自体を
ドラッグできるようにするだけで、フレームバッファ自身の論理解像度は追従しま
せん — `resized()`は、スクリプトが自分でそれに反応する（新しいサイズで
`init()`し直す、自前のUIをreflowする、あるいは無視する）ための、その1フレーム
だけ立つエッジです。`quit()`は、ウィンドウ自身のクローズボタンが`run`のループ
を止めるのと同じ合図を1フレーム遅れで送ります。`can_quit()`は閉じられる
ウィンドウがそもそもあるかを返すので、ゲームは「終了しますか？」という
プロンプトを出す意味があるかどうかを、実際に出す前に判断できます。

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
Canvas.draw_to(bgd, fn () {
  # 背景は一度だけ描く…
  Canvas.clear(sky)
  for i in 0..50 {
    Canvas.circle(Random.below(320), Random.below(240), 2, star)
  }
})
Canvas.run(320, 240, fn () {
  bgd.draw(0, 0)  # …毎フレームは blit 1 回
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
FS.write("shot.png", Canvas.to_png())  # スクリーンショット

let tile = Canvas.Sprite.blank(16, 16)  # オフスクリーンで描いてもよい
Canvas.draw_to(tile, fn () {
  Canvas.clear(Canvas.rgba(80, 200, 120))
})
FS.write("tile.png", tile.to_png())
```

出力は8bit truecolor + アルファ、`IDAT`は1個で、各行はスコアが最小に
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

### TTFフォント

`Canvas.Font(data)`はTTF/OTFのバイト列をパースし — フォントファイルの`FS.read`や、
`Embed.dir(...).read(...)`でAOTバイナリへ焼き込める（`Sprite.from_png`のアセットと
同じ要領） — ハンドルを返す。`Sprite`と違い`Font`はサイズを固定しない — サイズは
描画のたびに引数で渡すので、1つの`Font`がプログラム内で使う全サイズを兼ねる。
ラスタライズ済みグリフは内部で(font, codepoint, size)ごとにキャッシュされる。
stb_truetypeがパースできないものは`ValueError: not a valid TTF/OTF font`を送出
する。**信頼できるソースのフォントのみ読み込むこと** — 大半のTTF/OTFパーサ同様、
stb_truetypeは壊れたファイル内部のオフセットを範囲チェックしない。

| メソッド | 効果 |
| --- | --- |
| `font.draw(s, x, y, color, size)` | `s`を`(x, y)`（視覚上の左上）起点に`color`・`size` pxで描く |
| `font.draw_screen(s, x, y, color, size)` | 同じものを、フレームが実際に表示される解像度で描く |
| `font.text_width(s, size) -> Long` | `size`で`s`が占めるピクセル幅(中央寄せ/右寄せ用) |
| `font.ascent(size) -> Long` | `size`でのベースラインからの上端ピクセル数 |
| `font.advance(codepoint, size) -> Long` | `size`での1コードポイント分の送り幅 |

`draw`は`s`中の全Unicodeスカラー値を歩く(内蔵ビットマップフォントの`text`はASCII
限定の`.bytes()`ベースだが、こちらはフルUnicode)。各グリフはアンチエイリアス付き
で描かれ — 他の描画呼び出しと同じアルファブレンドを通るので、部分的にしか覆われて
いないピクセルは完全なon/offでなく`color`へ向かってブレンドする。フォントに無い
コードポイントはスキップされずフォント自身の`.notdef`グリフへフォールバックする。
kerningはない: 送り幅は各グリフ自身の幅の和。
hintingもない — これは欠落でなく意図的な選択で、hintingを入れるとグリフ形状が
hintingエンジン自体の丸め処理に依存してしまい、`Canvas.Font`はnative・ブラウザ・
headlessの全backendで同じ結果を目指す他の全Canvas primitiveと同じ立場を取って
いる。

```culebra
# doctest: skip
let font = Canvas.Font(FS.read("assets/roboto.ttf"))
Canvas.run(320, 240, fn () {
  Canvas.clear(Canvas.rgba(20, 20, 24))
  font.draw("Score: 42", 8, 8, Canvas.rgba(255, 255, 255), 16)
  true
})
```

#### ディスプレイ解像度でのテキスト描画

フレームはフレームバッファをnearest-neighborで拡大して表示される。スプライトや
ビットマップフォントにはこれが正しいが、グリフのアンチエイリアスされた縁は
ブロック状に拡大されてしまう。**`font.draw_screen`**は`draw`と同じ引数を同じ
座標系で取り、描かれる位置も`draw`と同じだが、フレームが実際に表示されるサイズで
ラスタライズし、フレームの中でなく上に合成する — ウィンドウサイズやPlaygroundの
ペイン幅がいくつでもシャープなまま。呼び出しは名前を変えるだけで切り替わり、
`text_width`・`ascent`・`advance`はどちらにも同じように使える。

画面レイヤーがフレームバッファと別であることから、2点が導かれる:

- **毎フレームクリアされる。** フレームバッファは`present()`をまたいで残るので
  描画を積み重ねられるが、画面テキストは毎フレーム描き直す必要がある(`Canvas.run`
  のtickなら元々そうしている)。これは必然で、グリフはアンチエイリアス付きなので
  フレームごとに自分自身の上へ合成し続けると縁がどんどん濃くなってしまう。
- **`Canvas.to_png()`には写らない。** `to_png`は描画ターゲットを読むもので、画面
  レイヤーはターゲットではない。フレームバッファへの描画をスプライトへ振り替える
  `Canvas.draw_to`の影響も受けない。保存する画像に入れたいテキストには`draw`を
  使うこと。

ネイティブウィンドウではウィンドウの公称サイズでなくディスプレイの実ピクセル密度で
ラスタライズする: 2x(Retina/HiDPI)の画面では640x380のウィンドウは実際には1280x760
ピクセルで、テキストはそのサイズで描かれる。Playgroundも同じで、ペインが表示されて
いる幅にブラウザのデバイスピクセル比を掛けたサイズで描く。拡大先の
ディスプレイが無い場合(headless、あるいはウィンドウを開けなかった場合)はスケールが
1になり、`draw_screen`は`draw`とまったく同じ位置に描く。

### 入力

入力は毎フレームのポーリング（イベントキューでなく現在の状態を反映する）。

| 関数 | 結果 |
| --- | --- |
| `Canvas.buttons() -> Long` | 押下中ボタンのビットマスク |
| `Canvas.mouse() -> Object` | フレームバッファ座標の`{x, y, buttons}` |
| `Canvas.key(name) -> Bool` | 名前で指したキーが今押されているか |
| `Canvas.key_queue() -> Array` | このフレームのキー押下（名前）を引き取る |
| `Canvas.typed() -> String` | ユーザーが打った文字を引き取る |
| `Canvas.wheel() -> Float` | 前フレームからのマウスホイール垂直方向の変化量 |

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

`wheel()`が正の値はユーザーから遠ざかる向き（上スクロール / ズームイン）
で、ブラウザの`-deltaY`の符号規則に合わせています。

### ゲームパッド

| 関数 | 結果 |
| --- | --- |
| `Canvas.pad_available(index = 0) -> Bool` | `index`にゲームパッドが接続されているか |
| `Canvas.pad_axis(axis, index = 0) -> Float` | 軸の値（スティック、トリガー） |
| `Canvas.pad_held(button, index = 0) -> Bool` | ボタンが今押されているか |
| `Canvas.pad_pressed(button, index = 0) -> Bool` | **このフレーム**に押されたか |
| `Canvas.pad_name(index = 0) -> String` | パッドが報告する名前 |
| `Canvas.pad_rumble(left, right, sec, index = 0)` | 両モーターを強さ0–1で`sec`秒振動させる |
| `Canvas.pad_mappings(db) -> Bool` | バンドル済みDBに無いパッド用の`SDL_GameControllerDB`追加行を読み込む |

`index`（0–3）はどのパッドかを選ぶ — 単一コントローラの一般的なケースなら0、
ローカルマルチプレイならそれ以外を使う。`pad_pressed`は`buttons()`に対する
`input.pressed`と同じ「このフレームだけのエッジ」だが、raylibがネイティブに
追跡しているため`update()`や前フレーム状態の管理は不要。ボタン・軸の番号は
raylib自身の`GamepadButton` / `GamepadAxis`の値で、スクリプトがその番号を
知らずに済むよう名前が付けられている:

| ボタン | 軸 |
| --- | --- |
| `Canvas.PAD_UP` / `RIGHT` / `DOWN` / `LEFT`（十字キー） | `Canvas.AXIS_LX` / `LY`（左スティック） |
| `Canvas.PAD_Y` / `B` / `A` / `X`（フェイスボタン、Xbox命名） | `Canvas.AXIS_RX` / `RY`（右スティック） |
| `Canvas.PAD_LB` / `LT` / `RB` / `RT`（肩ボタン / トリガー） | `Canvas.AXIS_LT` / `RT`（アナログトリガー） |
| `Canvas.PAD_SELECT` / `GUIDE` / `START` | |
| `Canvas.PAD_L3` / `R3`（スティック押し込み） | |

`pad_rumble`は触覚フィードバックの無いbackend/パッド（macOSのXboxパッドは
それを駆動するAPIが無い）では黙って何もしない。ゲームパッドの状態は
ブラウザとヘッドレスでは利用できず、`pad_available`は常に`false`。

### 衝突判定

軸並行の重なり・内包判定。ネイティブ側の実装を持たない純粋なculebraコード:

| 関数 | 結果 |
| --- | --- |
| `Canvas.rect_overlap(x1, y1, w1, h1, x2, y2, w2, h2) -> Bool` | 2つの矩形が重なるか |
| `Canvas.circle_overlap(x1, y1, r1, x2, y2, r2) -> Bool` | 2つの円が重なるか |
| `Canvas.point_in_rect(px, py, x, y, w, h) -> Bool` | 点が矩形の内側にあるか |
| `Canvas.point_in_circle(px, py, cx, cy, r) -> Bool` | 点が円の内側にあるか |

`rect`側の`(x, y, w, h)`引数は`Canvas.rect`自身の規約（左上座標とサイズ）
に合わせてあるので、スプライトの当たり判定ボックスと描画する矩形とで
同じ数値をそのまま使い回せます。

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
`tone`を一度も呼ばないプログラムは音声デバイスを開かない。どちらの実装でも
ノートはデバイスが埋めていたバッファの境界ではなくオーディオストリーム自身の
サンプルクロック上に置かれるので、デバイスが選んだバッファ長によらず
シーケンサの音符間隔は保たれる。ヘッドレスなネイティブ
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

`tick`のペースは目標フレームレート（既定60、ブラウザのループに合わせて
ある）にvsyncされているので、大半のゲームは以下の関数に触れる必要が
ありません:

| 関数 | 効果 |
| --- | --- |
| `Canvas.dt() -> Float` | 前回の`present()`からの経過秒数 |
| `Canvas.target_fps(n)` | フレームレートの上限を設定（0で無制限） |
| `Canvas.fps() -> Long` | 実測フレームレート |

`dt()`が意味を持つのは、vsyncされたtickが前提とする固定`1/60`の代わりに、
実際に経過した秒数でステップしたい`tick`だけです（このリポジトリ自身の
`lunar_lander.cul`の例がそうしています）。3つともネイティブ限定です —
ブラウザビルドはディスプレイ自身のアニメーションフレームに自らペーシングし、
ヘッドレスにはそもそもフレームレートが無いので、そこでは`dt()` / `fps()`は
`0`を返し、`target_fps`はno-opになります。

---

## 27. `Scene`

手続きジオメトリから組み立てる3D用のretained-modeレンダラ。ノードの
シーングラフ — プリミティブ（box / sphere / cylinder / plane）と手組みメッシュ
— を並べ、マテリアルとトランスフォームを与え、カメラを置いて描画する。
ライティングは物理ベース（metallic / roughnessマテリアル、2カスケード影付き
の指向性sun、sky / fog、SSAA・アンビエントオクルージョン・bloom・被写界深度の
post stack）なので、出力はフラットシェーディングのプリミティブ以上になる。

`Scene`は **ゲームエンジンではない**。物理・当たり判定なし、モデルのimportなし
（ジオメトリは手続き生成か頂点単位の組み立て。テクスチャはプロセス内で描くか
PNGのバイト列から読む）、スケルタルアニメーションなし。狙いは*組み立てる*
3D — 可視化、手続き的シーン、チェイスカメラ付きの車両 / フライトデモ — であって、
アセット駆動のゲームではない。サーキットのメッシュ、チェイスカメラ、
ゲームパッド操作といったレーシングデモの形が、設計の基準になっている。

`Scene`は`Canvas`のウィンドウバックエンドを持つビルドには必ず入る — macOS・
Linux・Windowsの既定で、リリースバイナリもこれを持つ — vendoredな静的SDL3 +
raylibを`Canvas`と共用し、`-DCULEBRA_ENABLE_SCENE=OFF`で外せる。`culebra build`
した`Scene`プログラムはraylibとSDL3を静的リンクし、macOS arm64で約5.1 MBになる
（`print`だけなら約0.4 MB）。フレームを
実際に描いて確認しているのはLinux（毎pushのCIがXvfb下で`tests/scene_api_test.sh`
を回し、全メソッドを両エンジンで呼ぶ）とmacOS（手動）で、Windowsビルドは
リンクまでは通るがまだウィンドウを開いたことがない。`View`にヘッドレスモードは
無いので、`Canvas`と違い`Scene`プログラムはヘッドレスでもPlaygroundでも動かない。
ウィンドウ無しで動くのはCPU側の画像ベイカー`Scene.Image`だけ。

### View とフレームループ

`Scene.View.new(w, h, title)`はウィンドウを開く。開けない場合
（使えるディスプレイ / GLが無い）は`RuntimeError`を送出する — `Canvas`と
違いfallbackすべきヘッドレスモードは無い（`View`の観測可能な動作はすべて
GPUを必要とするため）。位置とサイズは`Float`
（ワールド単位）、色は`0–255`の3または4チャンネル整数で、範囲外の
チャンネルは端に丸められる。1フレームは、
2Dオーバーレイ付きの3Dパス（`render_3d()` → オーバーレイ描画 → `present()`）
か、純2D（`begin_2d()` → 描画 → `present()`）のいずれか。

| メソッド | 効果 |
| --- | --- |
| `view.target_fps(fps)` | フレームレート上限 |
| `view.closing() -> Bool` | 閉じるボタンが押された、または`quit()`が呼ばれた（trueまでループ） |
| `view.quit()` | スクリプトからループを終える。以降`closing()`はtrue |
| `view.dt() -> Float` | 前フレームからの秒数 |
| `view.width()` / `view.height() -> Float` | ウィンドウ寸法 |
| `view.camera(px,py,pz, tx,ty,tz, ux,uy,uz, fov)` | 視点位置・注視点・upベクトル・垂直FOV |
| `view.render_3d()` | シーングラフを描画し、2Dオーバーレイ用にフレームを開く |
| `view.begin_2d()` | 純2Dフレームを開く（3Dパスなし） |
| `view.present()` | フレームを確定して提示 |
| `view.fullscreen(on)` / `view.is_fullscreen() -> Bool` | フルスクリーンの切替 / 現在フルスクリーンか |
| `view.resizable(on)` / `view.resized() -> Bool` | 端をドラッグしてのリサイズを許す / このフレームでサイズが変わったか |
| `view.size(w, h)` / `view.title(s)` | ウィンドウの寸法 / タイトルを設定 |
| `view.vsync(on)` | `present()`でディスプレイのリフレッシュを待つ |
| `view.cursor(on)` / `view.mouse_capture(on)` | ポインタの表示 / 非表示、ウィンドウに固定（マウスルック用） |
| `view.clipboard() -> String` / `view.set_clipboard(s)` | システムのクリップボード |
| `view.fps() -> Long` / `view.time() -> Float` | 実測フレームレート / ウィンドウを開いてからの秒数 |
| `view.supersample(n)` | ウィンドウの`n`倍で描いて縮小する（1–4。既定の2がアンチエイリアス） |
| `view.clip_planes(near, far)` | 3Dパスのクリップ面（メートル。既定2と8000。コックピットカメラは`near`を手前に） |

3Dのターゲットはウィンドウに追従する: リサイズやフルスクリーン切替が認識された
（その直後の`present()`）後のフレームは、古い解像度を引き伸ばすのでなく新しい
解像度で描かれる。

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
| `node.material(m)` | `Material`を割り当て（下記）。`nil`でtintだけに戻る |
| `node.order(n)` | 描画順: 小さいほど先（既定0。ワールド内のHUD板は大きな値に） |
| `node.opacity(a)` | 0–255。マテリアルの値に掛かる |
| `node.quat(x, y, z, w)` | 姿勢をクォータニオンで — 積分器が渡してくる形。オイラー角を置き換える（`spin`は上に重なる） |
| `node.billboard(on = true)` | 毎フレームカメラに正対する（ワールド内のスプライト、遠景の木のカード） |
| `node.hide()` / `show()` / `name(n)` | 可視性 / ラベル |
| `node.x()` / `y()` / `z() -> Float` | 位置を読み戻す |
| `node.world_x()` / `world_y()` / `world_z() -> Float` | 祖先すべての変換を掛けた後の位置 |
| `node.child_count() -> Long` / `node.child_at(i) -> Node` | 子ノード（範囲外は`RuntimeError`） |
| `node.find(name) -> Node` / `node.has(name) -> Bool` | 名前付きの子孫（無い名前の`find`は`RuntimeError`。`has`が判定） |
| `node.remove()` | 親から外す — ハンドルは有効なまま、再追加までどこにも描かれない |
| `node.vertex_count() -> Long` | カスタムメッシュの頂点数（push済みまたはアップロード済み）。上限の前に次のノードへ移るため |
| `node.cull_radius(r)` | カリングに使う境界球（0 = 形状から） |
| `node.culling(on)` | このノードをフラスタムカリングの対象から外す（境界が合わない形状向け） |
| `view.remove(node)` / `view.find(name) -> Node` / `view.has(name) -> Bool` | 同じことをシーン全体（ルート含む）に |
| `view.culling(on)` | 境界球が画面外のノードを飛ばす（既定on。絵は決して変わらない） |

カスタムメッシュは頂点と三角形から組み、最後に確定する: `m.vertex(x, y, z, nx,
ny, nz)`（または`vertex_uv(…, u, v)`）が頂点、`m.tri(a, b, c)`が頂点インデックス
での三角形、`m.build()`がアップロード。（raylibは16bitインデックスバッファ
なので1メッシュは65535頂点が上限。超過は`build()`が拒否する。）テクスチャ座標の
`(0, 0)`は2Dと同じく画像の左上。プリミティブは画像を正立で見せ、その上辺は`+y`側
（planeでは`-z`側、`+z`のカメラから見て奥の辺）に来る。アップロード済み
メッシュはアップロードしたviewのもので、`view.drop()`後も残したnodeは変換としては
使えるが、後続のviewでは何も描かない。

### マテリアル・ライティング・テクスチャ

マテリアルはviewが作るハンドルで、fluentなsetterで設定するので1式で書ける:

| メソッド | 結果 |
| --- | --- |
| `view.add_material() -> Material` | 新しいマテリアル: 白・マット・テクスチャなし |
| `mat.rgb(r, g, b) -> Material` | 基本色 |
| `mat.pbr(metallic, roughness) -> Material` | PBR応答。どちらも0–1（既定は0と0.85） |
| `mat.texture(tex) -> Material` | サンプルする`Texture`。`nil`で無し |
| `mat.uv(us, vs, uo = 0.0, vo = 0.0) -> Material` | テクスチャ座標の拡縮とオフセット（路面のタイル。`us`に`-1.0`で左右反転） |
| `mat.normal_map(tex, strength = 1.0) -> Material` | タンジェント空間の法線マップ（`Scene.Image.to_normal`が作る）。タンジェント枠はピクセルごとに導出するのでメッシュにタンジェントは不要。`nil`で外す |
| `mat.opacity(a) -> Material` | 0–255。255未満で透過面になる |
| `mat.cutout(threshold) -> Material` | 被覆（opacity × テクスチャalpha）がこれ未満のピクセルを捨てる（葉のカード）。0でoff |
| `mat.blend(name) -> Material` | 透過面が背後と混ざる方法: `"over"`（既定）、`"add"`（ランプ、光）、`"multiply"`、`"screen"` |
| `mat.emissive(r, g, b, k = 1.0) -> Material` | 面が発する光。ライティングの後に加算される（bloomに乗る） |
| `mat.unlit(on = true) -> Material` | 基本色をそのまま — 光も影も反射も無し（ミラー像、デカール） |
| `mat.double_sided(on = true) -> Material` | 両面を描く（旗、フェンス） |
| `mat.depth_write(on)` / `mat.depth_test(on) -> Material` | デプスバッファに書くか / それに隠されるか（既定は両方on） |
| `mat.casts_shadow(on) -> Material` | 影パスに参加するか（既定on。ブロブシャドウやミラー板はoff） |
| `mat.fog(on) -> Material` | 距離フォグが届くか（既定on。スカイドームはoff） |

```culebra
# doctest: skip
let gold = view.add_material().rgb(230, 180, 60).pbr(0.9, 0.3)
view.add_box(2.0, 2.0, 2.0).material(gold)
let glass = view.add_material().rgb(200, 220, 255).opacity(90).depth_write(false).casts_shadow(false)
let shadow = view.add_material().rgb(0, 0, 0).opacity(110).unlit().depth_write(false).casts_shadow(false)
```

面は、不透明度（node × material）が255未満か、blendが`"over"`以外なら透過面。
litパスは不透明な面を手前から奥へ先に描き、次に透過面を奥から手前へ描く。
`node.order()`はその両方より優先されるので、何より上に載る板（リアビューミラーの
枠と像、そのLED帯）は大きなorderを持てば距離に関わらず最後に描かれる。透過面は
色だけを混ぜ、フレームの深度は不透明パスが書いたまま残すので、アンビエント
オクルージョンと被写界深度はガラスを透かして背後の実体を見る — ブロブシャドウや
ウィンドスクリーンが望む読み方そのもの。透過面は影を落とさないが、cutoutは形が
実体なので落とす。

### ポストプロセス

`render_3d()`はフレームをpostパスに通す: 被写界深度、アンビエントオクルージョン、
bloom、トーンマップ、彩度、そして求めればvignetteとカラーグレーディング。それぞれに
つまみがあり、既定値はシェーダを調整したときの見た目そのもの。`0`でそのパスはoff。

| メソッド | 効果 |
| --- | --- |
| `view.post_process(on)` | パス全体（offなら、litフレームをそのまま。アンチエイリアスは残る） |
| `view.exposure(k)` | トーンマップの露出（既定1.35） |
| `view.saturation(k)` | 1でlitのまま（既定1.1） |
| `view.bloom(threshold, strength)` | `threshold`より明るい部分から滲む光（既定0.7、1.5） |
| `view.dof(strength, range)` | 画面中央の深度を基準にした被写界深度（既定0.85、3.5） |
| `view.ssao(strength, radius)` | アンビエントオクルージョン（既定0.45、3.0） |
| `view.vignette(k)` | 四隅を暗くする（既定0） |
| `view.lut(tex, amount = 1.0)` | 3D LUTでカラーグレーディング。`amount`で混ぜ、`nil`でoff |

LUTは`n`個のスライス（各`n`×`n`）を横に並べたストリップを持つ`Texture`で、`n`は
テクスチャの高さ（4096×64のストリップなら64スライス）: 青がスライスを選び、赤が
横に、緑が縦に走る。`Scene.Image`で組み、ミップマップとリピートをoffにして
アップロードする（`view.texture(img, false, false)`）。パスはセルをセルとして
サンプルする。恒等LUT — 各セルが自分自身の色 — はフレームを変えず、グレーディング
されたものはカラーツールが書き出すものそのまま。

### 第2カメラ

`view.render_to(tex, px,py,pz, tx,ty,tz, ux,uy,uz, fov)`は別のカメラからシーンを
`render_target`テクスチャに描く — `render_3d()`と同じlitパスで、post stackは無し。
リアビューミラーは、そのターゲットを貼った板に`uv(-1.0, 1.0, 1.0, 0.0)`で像を
左右反転したもの。毎フレーム`render_3d()`の前に呼ぶ: テクスチャをサンプルする板は
`render_3d()`の中で描かれるので、後に呼べば前のフレームの像が出る。影は直前の
`render_3d()`がメインカメラに合わせて張ったものをそのまま使う — 同じ道を振り返る
ミラーなら共有できる — ので、最初のフレームのミラーには影が無い。render targetは
canvasと同じく、メッシュ上でも`sprite`でも`uv()`越しでも正しい向きになる。

```culebra
# doctest: skip
let mirror = view.render_target(512, 256)
let mirror_mat = view.add_material().texture(mirror).unlit().uv(-1.0, 1.0, 1.0, 0.0)
view.add_box(1.2, 0.4, 0.02).move(0.0, 1.6, 2.0).material(mirror_mat).order(10000)
while !view.closing() {
  view.camera(0.0, 1.5, 6.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 55.0)
  view.render_to(mirror, 0.0, 1.5, 1.0, 0.0, 1.5, 20.0, 0.0, 1.0, 0.0, 120.0)
  view.render_3d()
  view.present()
}
```

テクスチャもハンドル。2つのハンドル型が契約そのもの: マテリアルの位置に
テクスチャ（やその逆）を渡せば呼び出し時点で`TypeError`、drop済みのハンドルは
`ClosedError`になる。

| メソッド | 結果 |
| --- | --- |
| `view.texture(img, mipmaps = true, repeat = true) -> Texture` | `Scene.Image`（下記）をアップロード。タイル用マテリアルならミップマップ＋リピート、スプライトやLUTなら両方off |
| `view.texture_png(bytes) -> Texture` | PNGのバイト列（`FS.read`や`Embed.dir`の資産）を直接テクスチャに |
| `view.checker(px, checks, r1,g1,b1, r2,g2,b2) -> Texture` | 市松 |
| `view.grain(px, r, g, b, amt) -> Texture` | 単色＋ノイズの粒 |
| `view.canvas(w, h) -> Texture` / `view.canvas_end()` | 2D呼び出しで塗るrender-to-textureを開く / 閉じる |
| `view.render_target(w, h) -> Texture` | 第2カメラからシーンを描き込むテクスチャ（下記） |
| `tex.width()` / `tex.height() -> Float` | ピクセル寸法 |
| `tex.filter(name) -> Texture` | サンプリング: `"point"`（ピクセルアート、LUTのセルそのまま）、`"bilinear"`、`"trilinear"` |
| `tex.wrap(name) -> Texture` | 端の外側: `"repeat"`、`"clamp"`、`"mirror"` |

canvasはGPUでテクスチャを描く方法（`canvas()`と`canvas_end()`の間の
`rect` / `text` …）で、レーシングデモがリバリーや看板を塗るやり方。同時に1枚だけ:
閉じる前の2枚目の`canvas()`は`RuntimeError`、開いたままフレームを開くと先に
閉じられる。テクスチャはUVの向きに関わらずメッシュ上で正しい向きになる。
テクスチャやマテリアルは作ったviewより長生きしてよいが、その後のviewでは
不活性なハンドルになる: 白としてサンプルされ、nodeのメッシュが何も描かないのと
同じ扱い。

### 画像

`Scene.Image`はCPU上の画像 — シーンを飾る手続きテクスチャ（リバリー、路面の粒、
ランプの光、カラーグレーディングのLUT）のベイカーであり、PNGのピクセルの入り口。
ウィンドウを必要としない: `View`が1つも無い段階で画像を組み、ピクセル単位で
読み戻し、書き出せる。`Scene`のうちディスプレイ無しでテストが回る唯一の部分で
ある理由がこれ。常にRGBA、座標はピクセル、色はそれぞれ自分のalpha（既定255）を持つ。

| メソッド | 結果 |
| --- | --- |
| `Scene.Image.new(w, h) -> Image` | 透明な画像 |
| `Scene.Image.from_png(bytes) -> Image` | PNGをデコード（PNGでなければ`RuntimeError`） |
| `img.width()` / `img.height() -> Float` | 寸法 |
| `img.get(x, y) -> Long` | 1ピクセル。`0xRRGGBBAA`に詰めた値 |
| `img.copy() -> Image` / `img.save_png(path) -> Bool` / `img.to_png() -> String` | コピー / PNGに書く / PNGのバイト列 |
| `img.fill(r, g, b, a = 255)` | 全ピクセル |
| `img.pixel(x, y, r, g, b, a = 255)` | 1ピクセル |
| `img.rect(x, y, w, h, r, g, b, a = 255)` / `img.rect_line(x, y, w, h, r, g, b, a = 255)` | 塗り / 枝線の矩形 |
| `img.circle(x, y, radius, r, g, b, a = 255)` / `img.circle_line(x, y, radius, r, g, b, a = 255)` | 塗り / 枝線の円 |
| `img.line(x0, y0, x1, y1, thick, r, g, b, a = 255)` | 線 |
| `img.triangle(x0, y0, x1, y1, x2, y2, r, g, b, a = 255)` | 塗り三角形 |
| `img.text(s, x, y, size, r, g, b, a = 255, font = nil, spacing = 0.0)` | `Font`でテキスト。組み込みフォント（`nil`）はウィンドウがあってはじめて存在する |
| `img.gradient(r1,g1,b1, r2,g2,b2, direction = 0)` | 画像全体の線形グラデーション。`direction`は度で、0が上 → 下、90が左 → 右 |
| `img.gradient_radial(density, r1,g1,b1, r2,g2,b2)` | 中心 → 外周。`density`（0–1）は内側の色が届く距離 |
| `img.noise(seed, scale, amount = 255)` | Perlinノイズを`amount`（0–255）で混ぜる。`scale`は模様の大きさ |
| `img.cellular(tile, amount = 255)` | `tile`ピクセルのWorleyセルを混ぜる（砂利、石） |
| `img.blit(src, x, y, r = 255, g = 255, b = 255, a = 255)` | 別の画像を色を掛けて重ねる |
| `img.blit_rot(src, x, y, rot = 0.0, scale = 1.0)` | 同じく、中心を`(x, y)`に置いて回転・拡縮 |
| `img.blur(radius)` / `img.tint(r, g, b)` / `img.invert()` / `img.grayscale()` / `img.brightness(k)` | 画像全体のパス |
| `img.flip_v()` / `img.flip_h()` / `img.rotate(degrees)` / `img.resize(w, h)` / `img.crop(x, y, w, h)` | 幾何 |
| `img.to_normal(strength = 1.0)` | 高さ（赤チャンネル）をタンジェント空間の法線マップに。+Yが上、端はラップするのでタイルの法線もタイルする |

描画呼び出しはfluentなので、テクスチャ1枚が1式になる:

```culebra
# doctest: skip
let asphalt = Scene.Image.new(256, 256).fill(60, 60, 64).noise(3, 4.0, 90).cellular(24, 40)
let road = view.add_material().texture(view.texture(asphalt)).pbr(0.0, 0.9)
```

`img.get`は厳密 — CPUラスタはどのプラットフォームでも同じバイト列 — なので、
描画フレームはゆるく比較するしかないところを、テストはピクセルを断言できる。

ライティングはview上で設定する:

| メソッド | 効果 |
| --- | --- |
| `view.background(r, g, b)` | クリア色 |
| `view.sky(tr,tg,tb, br,bg,bb)` | 天頂 → 地平のグラデーション（反射環境も兼ねる） |
| `view.sun(dx,dy,dz, intensity, r,g,b)` | 指向性ライト（2カスケード影）。`(0, 0, 0)`は方向を指さないので拒否される |
| `view.ambient(intensity, r, g, b)` | フィルライト |
| `view.fog(start, end, r, g, b)` | 距離フォグ |
| `view.screenshot(path)` | 描いている途中のフレームをPNGに保存 — `render_3d()` / `begin_2d()`の後、`present()`の前に呼ぶ。後では表示済みバッファは既に無く、1〜2フレーム古い絵になる |

### 2D オーバーレイ

`render_3d()`（または`begin_2d()`）の後、これらが上に描かれる（HUD用）。開いた
`canvas()`の中にも描け、それがテクスチャを描く方法。座標はウィンドウ座標、
角度は度、すべての呼び出しは共有の`alpha()`を使う。

| メソッド | 効果 |
| --- | --- |
| `view.alpha(a)` | 以降の2D描画の不透明度（0–255） |
| `view.text(s, x, y, size, r, g, b, font = nil, spacing = 0.0, rot = 0.0)` | `Font`で、`font`が`nil`なら組み込みビットマップフォントでテキスト描画。`rot`は`(x, y)`まわりの回転 |
| `view.text_width(s, size, font = nil, spacing = 0.0) -> Float` / `view.text_height(s, size, font = nil, spacing = 0.0) -> Float` | `text`が占める矩形（右寄せ・中央寄せ用） |
| `view.rect(x, y, w, h, r, g, b)` / `view.rect_line(x, y, w, h, thick, r, g, b)` | 塗り / 枝線の矩形 |
| `view.rect_round(x, y, w, h, roundness, r, g, b)` / `view.rect_round_line(x, y, w, h, roundness, thick, r, g, b)` | 丸角（`roundness`は0–1） |
| `view.rect_gradient(x, y, w, h, r1,g1,b1, r2,g2,b2, horizontal = false)` | 上 → 下（または左 → 右）のグラデーション |
| `view.circle(x, y, radius, r, g, b)` / `view.circle_line(x, y, radius, r, g, b)` | 塗り / 枝線の円 |
| `view.circle_gradient(x, y, radius, r1,g1,b1, r2,g2,b2)` | 中心 → 外周のグラデーション（ランプ） |
| `view.ring(x, y, r_in, r_out, a0, a1, r, g, b)` | 2つの半径の間の弧帯、`a0`から`a1`まで（ゲージ） |
| `view.line(x0, y0, x1, y1, thick, r, g, b)` | 線 |
| `view.triangle(x0, y0, x1, y1, x2, y2, r, g, b)` | 塗り三角形。巻き方向は問わない |
| `view.poly(x, y, sides, radius, rot, r, g, b)` | 正多角形 |
| `view.sprite(tex, x, y, w, h, rot = 0.0, ox = 0.0, oy = 0.0, r = 255, g = 255, b = 255)` | `Texture`を`(x, y)`の`w`×`h`の箱に、箱内の`(ox, oy)`まわりに回して、色を掛けて描く（白 = そのまま） |
| `view.sprite_rec(tex, sx, sy, sw, sh, x, y, w, h, rot = 0.0, ox = 0.0, oy = 0.0)` | テクスチャの部分矩形（アトラス） |
| `view.clip(x, y, w, h)` / `view.clip_end()` | 間の描画を矩形にクリップ |

パスはスカラpushで組む — `view.path_begin()`、頂点ごとに`view.path_to(x, y)`、
`view.path_close()`で始点に戻る — そして`view.path_fill(r, g, b)`（始点からの
ファン。凸形状向け）、`view.path_strip(r, g, b)`（左右ペアのリボン。ミニマップの
コース）、`view.path_stroke(thick, r, g, b)`（輪郭）、`view.path_spline(thick, r,
g, b)`（4点以上を通るCatmull-Rom曲線）のいずれかで描く。点は次の`path_begin()`
まで残るので、塗ってから輪郭を引ける。

### フォント

`view.font(path, size, chars = "") -> Font`はTTF/OTFを1つのピクセルサイズで
グリフアトラスに焼く。`view.font_bytes(data, size, chars = "") -> Font`は
ファイルのバイト列から同じことをする — `Embed.dir`の資産に使え、One Binaryの
ゲームがフォントを自分の中に持てる。`chars`は含めるグリフ（`""`は印字可能
ASCII）: 数字と数語のHUDはそれだけ列挙して小さなアトラスを得、日本語を使うなら
使う文字を列挙する。読めないフォントは`RuntimeError`で、組み込みフォントへ
黙って落ちて計測がずれることはない。`font.size() -> Float`と
`font.glyphs() -> Long`で作った内容を読み戻せる。テクスチャと同じく、フォントは
作ったviewのもの。

### 入力

入力は毎フレームviewからポーリングする。キーは名前で指す — **`Canvas.key`と
`Term.read_key`と同じ語彙**: 印字可能な1文字（`"a"`、`" "`、`"-"`）か特殊キー名
（`"up"` / `"down"` / `"left"` / `"right"`、`"enter"`、`"escape"`、`"tab"`、
`"backspace"`、`"insert"`、`"delete"`、`"home"`、`"end"`、`"pageup"`、
`"pagedown"`、`"f1"`…`"f12"`。`"space"`は`" "`の読みやすい別名）。未知の名前は
押されることのないキーになる。Escはここでは普通のキーで、ウィンドウを閉じる
手段ではない: ループは閉じるボタンか`view.quit()`で終わる。

ゲームパッドのボタンと軸も名前で指す。SDLのマッピングDBがどのパッドも
正規化するレイアウトに従い、フェイスボタンは`"a"` `"b"` `"x"` `"y"`
（Xboxの文字 — PlayStationパッドでは`"a"`が×）、十字キーは`"up"` `"down"`
`"left"` `"right"`、ショルダーは`"lb"` `"rb"`、トリガーは`"lt"` `"rt"`、
`"select"` `"guide"` `"start"`、スティック押し込みは`"l3"` `"r3"`。軸は`"lx"`
`"ly"` `"rx"` `"ry"` `"lt"` `"rt"`。`index`でパッド（0–3）を選び、既定は最初の1台。
マウスはウィンドウ座標で報告し、ボタンは`"left"`・`"right"`・`"middle"`。

| メソッド | 結果 |
| --- | --- |
| `view.key(name) -> Bool` | キーが押下中 |
| `view.key_pressed(name) -> Bool` / `key_released(name) -> Bool` | このフレームで押された / 離された |
| `view.pad_available(index = 0) -> Bool` | ゲームパッド接続あり |
| `view.pad_axis(name, index = 0) -> Float` | 軸の値（スティック、トリガー） |
| `view.pad(name, index = 0) -> Bool` / `pad_pressed(name, index = 0) -> Bool` | ボタン押下中 / 今押された |
| `view.rumble(left, right, sec, index = 0)` | ハプティクス（SonyパッドとXInput。Xbox × macOSは無音） |
| `view.pad_name(index = 0) -> String` / `view.gamepad_mappings(db)` | パッド識別 / SDLマッピングDB読込 |
| `view.mouse_x()` / `mouse_y() -> Float` | ポインタ位置 |
| `view.mouse_dx()` / `mouse_dy() -> Float` | 前フレームからの移動量（capture中も） |
| `view.mouse_wheel() -> Float` | このフレームのホイール量 |
| `view.mouse(button) -> Bool` / `mouse_pressed(button) -> Bool` | マウスボタン押下中 / 今押された |

### 音声

`Scene.Sound.new(path)`はワンショット効果音、`Scene.Music.new(path)`は
ストリーム再生トラック。どちらもファイルパスを取り、`volume(v)`・`pitch(p)`・
`pan(p)`を持つ。`Sound`は`play` / `stop` / `playing`、`Music`は`pause` /
`resume` / `looping(on)`を加え、バッファを供給し続けるため毎フレーム
`update()`を呼ぶ必要がある。

`Scene.Audio.new(rate, channels, buffer)`はスクリプト自身がサンプル単位で合成する
ストリーム — 回転数に追従するエンジン音、ブレーキノイズ、ビープ — 音声ファイルを
持たないゲームが音を作る方法。サンプルはメインスレッドで作ってブロック単位で
渡す。スクリプトの一部がオーディオスレッドで走ることは無い: そこでのGC停止は
可聴のドロップアウトになる。

| メソッド | 効果 |
| --- | --- |
| `Scene.Audio.new(rate, channels, buffer) -> Audio` | `rate` Hz、1または2チャンネル、`buffer`フレームずつ供給するストリーム（1024が妥当な既定。〜512未満は非対応） |
| `audio.ready() -> Bool` | オーディオデバイスの無いマシンではfalse。以下はすべて何もしなくなる |
| `audio.needed() -> Long` | 今受け取れるフレーム数: 1ブロック消費されたら`buffer`、両方満杯なら0 |
| `audio.push(s)` / `audio.push2(l, r)` | モノラル1サンプル / ステレオ1フレーム（−1..1）を保留ブロックへ |
| `audio.pending() -> Long` | pushしたがまだ渡していないフレーム数 |
| `audio.submit() -> Long` | 保留ブロックをストリームに渡す。書き込んだフレーム数 |
| `audio.dropped() -> Long` | 満杯のブロックを越えてpushされ捨てられたサンプル数（作りすぎ） |
| `audio.latency() -> Float` | `submit()`からスピーカーまでの秒数: 2ブロック分 |
| `audio.play()` / `stop()` / `pause()` / `resume()` / `playing() -> Bool` | 再生制御 |
| `audio.volume(v)` / `pitch(p)` / `pan(p)` | `Sound`と同じ |

```culebra
# doctest: skip
let engine = Scene.Audio.new(44100, 1, 1024)
engine.play()
mut phase = 0.0
while !view.closing() {
  while engine.needed() > 0 {
    for i in 0..engine.needed() {
      phase += (48.0 + 165.0 * rpm_frac) / 44100.0
      if phase >= 1.0 { phase -= 1.0 }
      engine.push((phase * 2.0 - 1.0) * (0.03 + 0.13 * rpm_frac))
    }
    engine.submit()
  }
  # … 描画 …
}
```

60 fpsなら44.1 kHzのストリームは1フレームに735フレームを求める。合成は算術で、
JITなら余裕。`needed()`がフレームをまたいで高いままなら、スクリプトが追いついて
いない — ストリームは1ブロックを繰り返すか無音にする。

### 最小のシーン

```culebra
# doctest: skip
let view = Scene.View.new(960, 540, "spinner")
view.target_fps(60)
view.background(30, 34, 42)
view.sun(0.5, -0.8, -0.3, 1.2, 255, 245, 230)
view.ambient(0.4, 180, 200, 220)

let gold = view.add_material().rgb(230, 180, 60).pbr(0.9, 0.3)
let box = view.add_box(2.0, 2.0, 2.0).material(gold)

mut a = 0.0
while !view.closing() {
  a += view.dt()
  box.yaw(a)
  view.camera(4.0, 3.0, 5.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 55.0)
  view.render_3d()
  view.text("culebra scene", 20.0, 20.0, 28, 235, 235, 240)
  view.present()
  if view.key_pressed("escape") { view.quit() }
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
s.shutdown_write()  # リクエスト完了をサーバに伝える
inspect(s.read())   # サーバが閉じるまで読む
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
for conn in server {
  # ループで accept
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
server.serve(fn (conn) {
  for line in conn.lines() {
    conn.write(line.upper() + "\n")
  }
}, workers: 8)  # Ctrl+C までブロック
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
let msg = sock.recv_from()  # {data, host, port}
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
inspect(Net.resolve("localhost"))  # => ["127.0.0.1", "::1"]
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

### 全体の構成

この機能には性質の異なる3つの要素が関わっていて、混同しやすいので
整理します。

| 要素 | 何であるか |
| --- | --- |
| `Desktop` | culebraの1呼び出しfacade — HTTPサーバを起動し、その上にウィンドウを開き、ウィンドウが閉じるまでブロックし、閉じたらサーバを止める |
| `Webview.Window` | 生のネイティブウィンドウ・バインディング — プラットフォームのWebViewエンジンに支えられた1個のOSウィンドウで、URLへのナビゲートや生HTMLの読み込みができる |
| `window` | 標準JSの**グローバルオブジェクト** — ネイティブウィンドウが表示しているWebページの内側に存在する。culebraのAPIではなく、ただのJavaScript |

`Desktop.run`は`Webview.Window`の上に組み立てられています。ローカルHTTP
サーバを起動し、そのサーバのloopback URL（`http://127.0.0.1:PORT`）へ
ウィンドウを生成してナビゲートします。そこから先、ネイティブウィンドウと
それが描画するページは、2つの独立した経路でやり取りします。

- **loopback HTTP。** ページ側のJSは`Desktop.run`が起動したサーバに対して
  `fetch('/__quit', {method: 'POST'})`を呼べます（このルートは自動登録
  済みです）。culebra側のコードなら`Desktop.quit()` /
  `Webview.Window.quit()`を直接呼べます。これは*サーバ側やページ側*から
  *ネイティブウィンドウ*に「閉じてよい」と伝える経路です。
- **ページの`window`上の2つのプロパティ。** ネイティブ側はフレーム自身の
  閉じるボタンを尊重する前に、ページの`window`が`__culebra_before_close__`
  を定義しているか確認します。定義されていれば、そのフック単独が閉じるか
  どうか・いつ閉じるかを決め、準備ができたら`window.__culebra_close__()`
  を呼びます。これは*ページ*から*ネイティブウィンドウ*へ「（確認ダイアログ
  などを経て）閉じても安全」と伝える経路です。詳細は後述の
  [ページの`window`オブジェクト](#ページのwindowオブジェクト)を参照してください。

要するに、`Desktop`が全体を統括し、`Webview.Window`はそれが操作する
ネイティブウィンドウであり、`window`はそのウィンドウがたまたま表示している
ページの内側にある、ごく普通のブラウザのグローバルオブジェクトです —
culebraはそれを生成も所有もせず、その上にある2つの決められた名前の
プロパティを読み書きするだけです。

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
Desktop.run({title: 'culebra desktop', size: [
  720,
  560,
], assets: Embed.dir('dist'), routes: fn (srv) {
  srv.get('/api/hello', fn (req) {
    {content_type: 'application/json', body: JSON.stringify({message: 'hello'})}
  })
}})
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

### ページの`window`オブジェクト

ネイティブウィンドウとそれが表示するページは、ページ自身の`window`
— culebraが定義・所有するものではなく、ただの標準JSグローバルオブジェクト
— 上にある、名前が決められた2つのプロパティを介して「閉じてよいか」を
やり取りします。ページ側で何かをimportする必要はなく、culebraはページが
そこに置いたものを読んで呼び出すだけです。

フレーム自身の閉じるボタンを押しても、ウィンドウはそのままでは閉じません。

1. 閉じる前に、ネイティブ側はページが`window.__culebra_before_close__`を
   定義しているか確認します。
2. 定義されていれば、そのフック単独が閉じるかどうか・いつ閉じるかを
   判断します（confirmダイアログや未保存チェックなど） — 準備ができたら
   `window.__culebra_close__()`を呼びます。これはページ内のquitボタンから
   直接呼んでもよい関数で、`Desktop.run`を使っているかどうかを問いません。
3. `__culebra_before_close__`を定義していないページは、これまで通り即座に
   閉じます。

```js
// ページ自身のJS — culebra側のコードではない
window.__culebra_before_close__ = () => {
  if (confirm('未保存の変更を破棄しますか？')) {
    window.__culebra_close__()
  }
  // __culebra_close__()を呼ばずに戻れば、閉じるのはキャンセルされる
}
```

---

## 30. `Vector2`

グラフィックス/ゲームコード向けの最小限の2D floatベクトル——言語組み込み
ではなく普通のculebraクラスです。要素は
常に`Float`: `new`は`Long | Float`を受けてcoerceするので`Vector2.new(1,
0)`は動きますが、`.x` / `.y`が`Long`になることはありません。

`Vector2`は「Point」の代わりも兼ねます——位置と方向を型で区別しない
設計で、これはCGAL / nalgebraの厳密な分離よりもUnity / Godot /
three.jsの流儀に合わせたもので、標準ライブラリ全体の「必要最小限の
型数で済ませる」という方針とも一致します。`SharedBuffer`の固定レイア
ウトレコード(§12)に使う`@packable class`パターンとは別物——あちらは
演算を持たない生バイトレイアウト記述子で、`Vector2`は汎用の数学型
です。

```culebra
let a = Vector2.new(3, 4)
inspect(a.length())             # => 5.0
inspect(a.normalized())         # => (0.6, 0.8)
inspect(a + Vector2.new(1, 1))  # => (4.0, 5.0)
```

| メンバー | 返り値 |
|---|---|
| `Vector2.new(x, y)` | `Vector2` |
| `a + b` / `a - b` | `Vector2`（成分ごと） |
| `a * k` / `k * a` | `Vector2`（スカラー；`k: Long \| Float`） |
| `-a` | `Vector2` |
| `a == b` | `Bool` — nominal判定: `Vector2`以外(同形の`Vector3`も含む)は常に`false`、例外は投げない |
| `a.hash()` | `Long` — 等しいvectorは同じhash値になるので、`Set`の要素やObjectのキーにできる |
| `a.dot(b)` | `Float` |
| `a.length()` / `a.length_squared()` | `Float` |
| `a.normalized()` | `Vector2`（単位長；ゼロベクトルは`ZeroDivisionError`、他の`Float / 0.0`と同じ） |
| `a.distance_to(b)` | `Float` |
| `a.distance_squared_to(b)` | `Float` — 平方根を省く。距離の比較や閾値判定用 |
| `"{a}"` / `to_string(a)` | `String` — `"(x, y)"` |

`cross()`は(`Vector2`・`Vector3`(§31)いずれにも)意図的に持たせて
いません: 2Dの外積はスカラー(perp dot)、3Dの外積はベクトルを返す
非対称な演算で、このリポジトリのどのexampleでも必要とされなかった
ためです。

[`§26 Canvas`](#26-canvas)の座標を取るメソッド(`set_pixel`、
`get_pixel`、`line`、`rect`、`circle`、`ellipse`、`triangle`)は、
それぞれ`(x, y)`座標ペアの代わりに`Vector2`も受け付けます。

## 31. `Vector3`

`Vector2`(§30)の3D版——同じ設計(Float固定、独立したPoint型を
作らない、nominalな`==`)、同じメンバー構成で、`x` / `y` / `z`
フィールドと`Vector3.new(x, y, z)`を持ちます。現時点で`Canvas`
(2D専用)や`Scene`(culebra側wrapper層を持たないネイティブクラスで、
`Canvas`のようにオーバーロードを追加する経路が無い)からは受け
付けられません。

```culebra
let a = Vector3.new(1, 2, 3)
inspect(a.length())                # => 3.7416573867739413
inspect(a + Vector3.new(1, 1, 1))  # => (2.0, 3.0, 4.0)
```

| メンバー | 返り値 |
|---|---|
| `Vector3.new(x, y, z)` | `Vector3` |
| `a + b` / `a - b` | `Vector3`（成分ごと） |
| `a * k` / `k * a` | `Vector3`（スカラー；`k: Long \| Float`） |
| `-a` | `Vector3` |
| `a == b` | `Bool` — nominal判定、`Vector2`と同様 |
| `a.hash()` | `Long` — `Vector2`と同様 |
| `a.dot(b)` | `Float` |
| `a.length()` / `a.length_squared()` | `Float` |
| `a.normalized()` | `Vector3`（単位長；ゼロベクトルは`ZeroDivisionError`） |
| `a.distance_to(b)` | `Float` |
| `a.distance_squared_to(b)` | `Float` — 平方根を省く。距離の比較や閾値判定用 |
| `"{a}"` / `to_string(a)` | `String` — `"(x, y, z)"` |

## 32. `Deque`

両端キュー——言語組み込みではなく普通のculebraクラスです。
成長するリングバッファ(配列 +
先頭インデックス + 要素数)で実装しているため、どちらの端での
`push`/`pop`も償却O(1)です。`Array`(言語仕様§18)は末尾でしか
伸縮しません(`push`/`pop`)。`Array`でFIFOキューを組もうとすると
`remove_at(0)`が必要になり、残り全要素をシフトするのでdequeue
1回がO(n)になります。前方が関わる場面——FIFOキュー(`push_back` +
`pop_front`)、スライディングウィンドウ、「どちら側か」を明示した
いスタック(`push_back` + `pop_back`、`Array.push`/`pop`と対応)
——では`Deque`を使ってください。

```culebra
let q = Deque.new()
q.push_back(1)
q.push_back(2)
q.push_front(0)
inspect(q.pop_front())  # => 0
inspect(q.pop_front())  # => 1
inspect(q.size())       # => 1
```

| メンバー | 返り値 |
|---|---|
| `Deque.new()` | `Deque` |
| `d.push_back(x)` | `Nil` |
| `d.push_front(x)` | `Nil` |
| `d.pop_back()` | `Any` — 取り除いた要素、空なら`nil` |
| `d.pop_front()` | `Any` — 取り除いた要素、空なら`nil` |
| `d.peek_back()` | `Any` — 空なら`nil`、取り除かない |
| `d.peek_front()` | `Any` — 空なら`nil`、取り除かない |
| `d.size()` | `Long` |
| `d.empty()` | `Bool` |
| `d.to_array()` | `Array` — スナップショット、前から後ろの順 |
| `d.iter()` | `Iterator` — `for x in d { ... }`が使える、前から後ろの順 |
| `"{d}"` / `to_string(d)` | `String` |

`Array.pop()`と同様、`pop_front`/`pop_back`/`peek_front`/
`peek_back`は空の`Deque`に対して例外を投げず`nil`を返します——
`Array.pop()`が既に受け入れているのと同じ曖昧さです(pushした
`nil`と空であることは戻り値だけからは区別できません)。

## 33. `PriorityQueue`

`Array`上の二分ヒープ——言語組み込みではなく普通のculebraクラスです。
`push`/`pop`はO(log n)で、
素朴な代替（`Array`をソート済みに保つ、あるいはpopのたびに最小値を
線形探索する）は1回あたりO(n log n)またはO(n)になります。

順序付けは`Array.sort`/`sort_by`(言語仕様§18)と同じ規約に従い
ます: `key:`が無ければ要素は`<`で比較され(クラスの`__lt__`が
尊重されます)、`reverse: true`でmax-heapに反転します。

```culebra
let pq = PriorityQueue.new()
pq.push(5)
pq.push(1)
pq.push(3)
inspect(pq.pop())   # => 1
inspect(pq.peek())  # => 3

let jobs = PriorityQueue.new(key: |j| j[0])
jobs.push((2, 'retry'))
jobs.push((1, 'ping'))
inspect(jobs.pop())  # => (1, 'ping')
```

| メンバー | 返り値 |
|---|---|
| `PriorityQueue.new(*, key: Function \| Nil = nil, reverse: Bool = false)` | `PriorityQueue` |
| `pq.push(x)` | `Nil` |
| `pq.pop()` | `Any` — 最小値(`reverse: true`なら最大値)を取り除いて返す、空なら`nil` |
| `pq.peek()` | `Any` — `pop()`と同じだが取り除かない、空なら`nil` |
| `pq.size()` | `Long` |
| `pq.empty()` | `Bool` |
| `"{pq}"` / `to_string(pq)` | `String` — `"PriorityQueue(size=n)"`(ヒープ配列そのものではない——要素の並びは優先度順ではないため) |

`Array.pop()`や`Deque`と同様、`pop`/`peek`は空のキューに対して
例外を投げず`nil`を返します。

## 34. `PEG`

パーサジェネレータ。**PEG**（parsing expression grammar）を書くと構文木が返る。
エンジンは同梱の[cpp-peglib](https://github.com/yhirose/cpp-peglib)で、culebra自身の
フロントエンドが載っているものと同じ。`peglint`が受理する文法はここでも同じ振る舞いをする。

文法は**一度コンパイルして使い回す**（`PEG.compile` — 文法のロードが重い部分）。
コンパイル済みの文法はいくつの入力にも適用できる:

```culebra
let calc = `
  Additive       <- Multiplicative '+' Additive / Multiplicative
  Multiplicative <- Primary '*' Multiplicative / Primary
  Primary        <- '(' Additive ')' / Number
  Number         <- < [0-9]+ >
  %whitespace    <- [ \t\r\n]*
`
let p = PEG.compile(calc)
inspect(p.parse("1 + 2").name)  # => 'Additive'
inspect(p.test("1 +"))          # => false
```

**文法はバッククォートのraw string（`` `...` ``）で書く。** エスケープ処理も
`{...}`補間も行わないので、`\t`・`[0-9]{2}`・`'`がそのまま文法に届く。シングル
クォートのraw stringでも書けるが、PEGは`'literal'`という引用で埋まるので通常は
バッククォートのほうが収まりがよい。

### 文法の記法

記法の全体は[cpp-peglib側](https://github.com/yhirose/cpp-peglib#syntax)にある。
よく使うのは次の部分:

| 形 | 意味 |
| --- | --- |
| `Name <- e` | 規則の定義 |
| `'text'` / `"text"` | リテラル |
| `[a-z]` | 文字クラス。`.`は任意の1文字 |
| `e1 e2` | 連接 — `e1`の次に`e2` |
| `e1 / e2` | 順序付き選択 — `e1`を試し、失敗したときだけ`e2` |
| `e*` `e+` `e?` | 繰り返し（貪欲、内部への後戻りなし） |
| `&e` `!e` | 先読み。消費せずに成功/失敗する |
| `< e >` | トークン境界。ノードがマッチしたテキストを保持する |
| `%whitespace <- e` | トークン間で読み飛ばすもの |
| `~e` | この要素を木から落とす |
| `{ no_ast_opt }` | 子が1つでもこの規則のノードを残す |
| `{ ast_name: Tag }` | 規則名の代わりに`Tag`としてノードを出す |

PEGは構成上曖昧にならない。`/`は順序付きなので最初にマッチした選択肢が勝ち、解決すべき
曖昧性が生じない。裏を返せば順序が意味を持つ — `'a' / 'ab'`は`ab`にマッチしない。

### 木

ノードはただの`Object`なので、専用APIなしで`match`が分解できる:

```culebra
let calc = `
  Additive       <- Multiplicative '+' Additive / Multiplicative
  Multiplicative <- Primary '*' Multiplicative / Primary
  Primary        <- '(' Additive ')' / Number
  Number         <- < [0-9]+ >
  %whitespace    <- [ \t\r\n]*
`
fn eval(n) {
  match n {
    {name: "Number", token} => to_long(token),
    {name: "Additive", nodes: [a, b]} => eval(a) + eval(b),
    {name: "Multiplicative", nodes: [a, b]} => eval(a) * eval(b),
    {nodes: [only]} => eval(only),
    _ => throw "unexpected {n.name}",
  }
}
inspect(eval(PEG.parse(calc, "(1 + 2) * 3")))  # => 9
```

| フィールド | 意味 |
| --- | --- |
| `name` | そのノードを作った規則名、または`ast_name`で上書きした名前 |
| `token` | トークンノードのマッチしたテキスト。枝ノードでは`""` |
| `nodes` | 子ノード。ソース順 |
| `line` / `column` | 入力中でのノード開始位置（1始まり） |
| `position` / `length` | バイトオフセットとバイト長。`s.slice(position, position + length)`がそのノードのテキスト全体 |
| `is_token` | `< ... >`のトークン境界から来たノードかどうか |
| `choice` | 順序付き選択のどの選択肢がマッチしたか（0始まり） |

`parent`リンクは持たせていない。ただのObjectの木に親ポインタを張ると参照サイクルになる。
下向きに辿り、必要なものは引数で持ち回る。

### 構築と走査

| コンストラクタ / static | 結果 |
| --- | --- |
| `PEG.compile(grammar)` | `PEG` — ロード（使い回される）。不正な文法は`PEGError` |
| `PEG.compile(grammar, start)` | 文法の最初の規則ではなく`start`から始める |
| `PEG.compile(grammar, start, optimize)` | `optimize`が`false`なら子1つのノードも残す |
| `PEG.compile(grammar, start, optimize, packrat)` | `packrat`が`false`ならメモ化を切る |
| `PEG.check(grammar)` | `Nil` — ロードして捨てる。不正な文法なら送出 |
| `PEG.check(grammar, start)` | 同じ。start規則つき |

| メソッド | 結果 |
| --- | --- |
| `p.parse(text)` | ルートノード。構文エラーは`PEGError` |
| `p.parse(text, path)` | 同じ。エラーメッセージに載る対象名を指定 |
| `p.parse(text, path, actions)` | 代わりに`actions`で`text`を直接解釈する — 下記 |
| `p.test(text)` | `Bool` — `text`がパースできるか。木は返らない |

| ワンショット | 等価な式 |
| --- | --- |
| `PEG.parse(grammar, text)` | `PEG.compile(grammar).parse(text)` |
| `PEG.parse(grammar, text, start, optimize, packrat, path, actions)` | 同じ。`compile`のオプション・対象名・セマンティックアクションつき |
| `PEG.test(grammar, text)` | `Bool` |
| `PEG.test(grammar, text, start, packrat)` | 同じ。`compile`のオプションつき |

ワンショット形は`compile`の手順を隠す。エンジンがロード済み文法をスレッドごとに
キャッシュするので再ロードのコストは払わない。1つの文法を多くの入力に使うなら
`PEG.compile(...)`と書いたほうが意図が読める。

| 木のヘルパー | 結果 |
| --- | --- |
| `PEG.walk(node)` | `Iterator` — そのノードと子孫を深さ優先・ソース順で |
| `PEG.find(node, name)` | 名前が`name`の最初のノード、なければ`nil` |
| `PEG.find_all(node, name)` | `[Node]` — 名前が`name`のノード全部 |
| `PEG.str(node)` | `String` — インデント付きのダンプ。`peglint --ast`と同じ形 |

```culebra
let g = `
  Doc  <- Item+
  Item <- < [a-z]+ > _
  _    <- [ ]*
`
let doc = PEG.parse(g, "ab cd")
inspect(PEG.find_all(doc, "Item").map(|n| n.token))  # => ['ab', 'cd']
inspect(PEG.find(doc, "Nothing"))                    # => nil
inspect(PEG.str(doc))
# => |
# '+ Doc
#   - Item (ab)
#   - Item (cd)
# '
```

### AST最適化

既定では子がちょうど1つのノードをその子で置き換える。木が文法の内部事情ではなく構造を
運ぶようになるからで、ほぼどの走査コードもこれを望む。制御は2通り:

* 規則ごとに文法側で — `{ no_ast_opt }`でその規則のノードを残し、`{ ast_name: Tag }`で
  改名して2つの生成規則を1つのタグに合流させる
* 文法全体をculebra側で — `PEG.compile(grammar, "", false)`で全ノードを残す

```culebra
let g = `
  Wrap  <- Inner
  Inner <- < [0-9]+ >
`
inspect(PEG.walk(PEG.parse(g, "1")).map(|n| n.name).collect())
# => ['Inner']
inspect(PEG.walk(PEG.parse(g, "1", "", false)).map(|n| n.name).collect())
# => ['Wrap', 'Inner']
```

### メモ化（`packrat`）

既定でon。そしてこの既定は効いている。PEGは後戻りするので、接頭辞を共有する選択肢
（`A <- B '+' A / B`という、最初に書く文法がたいてい持つ形）は同じ接頭辞を選択肢の数だけ
パースし直し、メモ化なしでは指数時間になる。上の電卓文法での実測では、10段のネストが
メモ化ありで0.04ms、なしで2.0s。

代償は入力長×規則数に比例するテーブル（入力1バイトあたり概ね1KB）。
`PEG.compile(grammar, "", true, false)`で切れるが、切る価値があるのは接頭辞を共有しない
書き方をした文法（左因子化済み、あるいは素朴な文法が再帰するところを`*`/`+`で書いたもの）を
テーブルが効いてくるほど大きな入力に適用する場合だけ。

### エラーと境界

不正な文法も、パースできない入力も`PEGError`を送出する。文法内・入力内の位置は
**メッセージの中**にあり、エラーの`line`/`col`には入らない。そちらは他と同じく
culebra側の呼び出し位置を指す:

```culebra
inspect(try {
  PEG.parse(`N <- < [0-9]+ >`, "x")
} catch e {
  e.kind
})  # => 'PEGError'
```

`path`は対象を名指す引数で、メッセージの汎用的な「PEG: 」プレフィクスの代わりに
その名前が入り、他のコンパイラの診断メッセージと同じ読み方になる。`compile`ではなく
`parse`側の引数なのは、1つの文法を多くのファイルに使うからで、cpp-peglib自身の
`parse_n()`が呼び出しごとにpathを取るのと同じ理由:

```culebra
let n = PEG.compile(`N <- < [0-9]+ >`)
inspect(try {
  n.parse("x", "input.txt")
} catch e {
  e.message
})  # => 'input.txt:1:1: syntax error, unexpected 'x', expecting <N>.'
inspect(try {
  n.parse("x")
} catch e {
  e.message
})  # => 'PEG: 1:1: syntax error, unexpected 'x', expecting <N>.'
```

不正な*文法*のエラーはpathに関わらず「PEG: grammar:」の形のまま
— pathが名指すのは対象のドキュメントで、文法テキストはそれではない。

敵対的な入力を致命的な失敗でなく捕捉可能なエラーに留めるため、境界が2つある:

* **規則の入場回数** — 4000回を超えて降りるパースは`PEGError`（`nesting too deep`）。
  そうしないと機械生成のネストがパーサ内部でCスタックを溢れさせる。culebraが自分の文法に
  かけているのと同じ限界値の同じガード。
* **木の深さ** — 1000段を超える木は`ValueError`（`nesting too deep (limit 1000)`）。
  `JSON`や`TOML`が自分の木に適用しているのと同じ
  [値ネストの上限](language.ja.md#値ネストの上限)。

文法はスレッドごとの状態なので、`PEG`の値がisolate境界を越えるときは文法テキストが渡って
向こう側でロードし直される。共有されるものは何もない。

### セマンティックアクション

`PEG.parse`/`p.parse`は`actions`も受け取る: 規則名から`Function`への写像で、木を
一切作らずに対象を直接解釈する。登録した各`Function`は引数を1つ受け取る
— その規則自身の還元結果である`sv` Object — そして親の`sv.values`に渡す値を返す。
C++版が呼ぶ名前で言えば
[semantic action](https://github.com/yhirose/cpp-peglib#semantic-actions)
（cpp-peglibの`parser["Rule"] = [](const SemanticValues &sv) { ... }`）にあたる。
`PEG`側は文法に据える可変状態ではなく`parse`に渡す値にした
— 同じ文法を異なるactionsで使い回せるように:

```culebra
let calc = `
  Additive       <- Multiplicative '+' Additive / Multiplicative
  Multiplicative <- Primary '*' Multiplicative / Primary
  Primary        <- '(' Additive ')' / Number
  Number         <- < [0-9]+ >
  %whitespace    <- [ \t\r\n]*
`
let actions = {
  Number: |sv| to_long(sv.token),
  Additive: |sv| sv.values.size() < 2 ? sv.values[0] : sv.values[0] + sv.values[1],
  Multiplicative: |sv| sv.values.size() < 2 ? sv.values[0] : sv.values[0] * sv.values[1],
}
inspect(PEG.parse(calc, "1 + 2 * 3", actions: actions))  # => 7
```

`sv`は木の`Node`とフィールド名を共有し、`nodes`だけ`values`に変わる:
`{name, token, values, line, column, position, length, choice}`。
`values[i]`は子`i`自身のactionが生成した値
— actionを登録していない規則にはcpp-peglib自身の既定が適用される: 最初の子の値、
子を持たない規則なら`nil`（actionの無いleafトークン規則は値そのものを持たない。
素のcpp-peglibと同じで、テキストが要るleafには自分でactionを書く）。
規則が返せるのはスカラーだけではない — actionの戻り値はただのculebra値なので、
1つの文法で評価器・整形器・独自の木構造のいずれも、各actionが何を返すか次第で駆動できる。

`actions`のキーが文法内のどの規則の名前でもない場合、パース開始前に検出される
— 未定義start規則と同じ`PEGError`で、typoが黙って無視されることはない:

```culebra
inspect(try {
  PEG.parse(`N <- < [0-9]+ >`, "1", actions: {Nope: |sv| sv})
} catch e {
  e.message
})  # => 'PEG: no such rule 'Nope''
```

actionが投げたものは何であれ — culebraの`throw`、`sv`の誤用による`TypeError`、
何でも — そのまま伝播する。action専用のcatchで回避する必要はない。
`actions`を渡すと`optimize`は無効（最適化する木自体が存在しない）。
機械生成のネストに対する規則入場の深度ガードは引き続き効くが、
木の深さの`ValueError`は効かない — それを課す対象の木構築パス自体が無いので。

## 35. `CodeGen`

閉じた中間表現(IR)と、それを実行するレジスタ方式のbytecodeコンパイラ・実行器 ——
[cpp-vmlib](https://github.com/yhirose/cpp-vmlib)。`PEG`がcpp-peglibを取り込むのと
同じ形でvendorしている。`PEG`が構文木を返すのに対して、`CodeGen`はその構文木で
何をするかを置く場所を提供する: `PEG.parse`が返した木を`match`で辿りながら、
小さな言語のIRを手で組み立てて実行する。

```culebra
let m = CodeGen.Module.new()
let forty_two = m.binary(op: 'add', lhs: m.literal(v: 40, line: 1, col: 1),
                         rhs: m.literal(v: 2, line: 1, col: 1), line: 1, col: 1)

let args = m.list_new()
m.list_push(args, forty_two)
let print_stmt = m.intrinsic(name: 'print', args_list: args, line: 1, col: 1)

let stmts = m.list_new()
m.list_push(stmts, print_stmt)
let body = m.block(stmts_list: stmts, line: 1, col: 1)

m.add_func(name: 'main', num_locals: 0, num_captures: 0, num_cells: 0, num_params: 0, body: body)
m.verify()
m.run()  # => 42
```

### IRノードはオブジェクトでなく`Long`

`Module`のbuilderメソッドはどれも、モジュール自身のノード表に対する素の`Long`
(index)を返す ——handleではない。小さな式木を組み立てて、その数値を繋いでいく:

```culebra
let m = CodeGen.Module.new()
let a = m.literal(v: 3, line: 1, col: 1)
let b = m.literal(v: 4, line: 1, col: 1)
let sum = m.binary(op: 'add', lhs: a, rhs: b, line: 1, col: 1)  # sumはLong
```

`Block`の文並び・`Call`のcapture転送・一部の`Object`メソッドは、culebraの普通の
呼び出しのような可変長引数を取れない —— そこで小さなステージング用listを介する:
`list_new()`がlist idを返し、`list_push(list, value)`がノードidを追加し、
そのlistは`block()`か`intrinsic()`に渡した瞬間に消費されて消える。渡した後に
同じlist idを使い回すのは未定義。

### プログラムを組み立てる

| 呼び出し | 組み立てるもの |
| --- | --- |
| `CodeGen.Module.new()` | 空のmodule |
| `m.literal(v:, line:, col:)` | 整数定数 |
| `m.bool_literal(v:, line:, col:)` / `m.double_literal(v:, line:, col:)` / `m.nil_literal(line:, col:)` / `m.str_literal(s:, line:, col:)` | 残りのスカラー定数 |
| `m.var_ref(kind:, index:, line:, col:)` | local/captureスロット`index`の読み |
| `m.unary(op:, operand:, line:, col:)` | `op`は`'neg'`/`'bitnot'`、または`wrapi8 wrapi16 wrapi32 wrapu8 wrapu16 wrapu32`のいずれか —— Longをその幅に切り詰め、符号拡張(`wrapi*`)またはゼロ拡張(`wrapu*`)して戻す。固定幅の`int`/`uint`(C#/Java/Goのサブセットなど)を下ろすフロントエンド向け。オペランドは既にLongでなければならず、これらはどれも`lt`/`le`/`gt`/`ge`/`div`/`mod`/`shr`が自前のwrapなしに前提とする正規化済みの形にスロットの値を保つ |
| `m.binary(op:, lhs:, rhs:, line:, col:)` | `op`は`add sub mul div mod eq ne lt le gt ge bitand bitor bitxor shl shr`のいずれか(ビット演算はLong専用。シフト量は下位6ビットでマスクされ、`shr`は算術シフト)、または`udiv umod ushr ult ule ugt uge`のいずれか —— `uint64`に必要な符号なし版。そのビットパターンは、幅の狭い符号なし値が正規化済みで既にそうであるのとは違い、符号付きLongの大小順に乗らないため |
| `m.assign(kind:, index:, value:, line:, col:)` | local/captureスロット`index`への書き |
| `m.make_if(cond:, then_branch:, line:, col:)` | `else`の無い`if` |
| `m.make_if_else(cond:, then_branch:, else_branch:, line:, col:)` | `if`/`else` |
| `m.make_switch(subject:, arms_list:, line:, col:)` / `m.make_switch_default(subject:, arms_list:, default_body:, line:, col:)` | `subject`を`arms_list`(key, body, key, body, …。各keyはLongかStringのリテラルノードで、全部同じ種類・重複なし —— どちらも`verify()`が検査する)で分岐する。`make_switch_default`は何も一致しなかったときの腕を足す。一致がなくdefaultも無ければ`nil`(`else`の無い`if`と同じ)。keyの種類とsubjectの型が食い違えば失敗する。腕の中の`break`は最内の`while`を捕まえたままで、switch自体は`break`を捕まえない(`if`と同じ) |
| `m.make_while(cond:, body:, line:, col:)` | ループ |
| `m.block(stmts_list:, line:, col:)` | 文の並び。ステージング用listを消費する |
| `m.call(func:, cmap:, line:, col:)` | 関数index `func`の呼び出し。captureはcapture-map `cmap`経由で転送 |
| `m.make_closure(func:, cmap:, line:, col:)` | 関数`func`のクロージャ**値** —— 保持・受け渡しでき、後から呼べる |
| `m.call_value(callee:, args_list:, line:, col:)` | `callee`の評価結果が何であれ呼ぶ。引数はステージング用listを消費 |
| `m.declare_native(name:)` | このモジュールが`name`というホスト関数を呼ぶことを宣言し、そのindexを答える。モジュールが持つのは名前だけで、何に解決されるかは実行ごとに`Program.run`の`natives:`が渡す。だから同じモジュールがその時々の実装に対して走り、1つも宣言しなければ何も要らない |
| `m.native_ref(index:, line:, col:)` | 宣言済みnative `index`を指す値。呼ぶときは通常の`call_value`。`declare_native`が配っていないindexは`verify()`が弾く |
| `m.intrinsic(name:, args_list:, line:, col:)` | `name`は`'print'`/`'len'`/`'tostr'`/`'typeof'`/`'toint'`/`'todouble'`/`'tofloat32'`(各引数1個)・`'readint'`(引数0個)・`'fmod'`/`'pow'`(引数2個)。`'printraw'`は改行なしの`'print'`。コンテナのprimitiveは`'arraypush'`/`'objecthas'`/`'objectremove'`(引数2個)と`'arraypop'`/`'objectkeys'`(引数1個)。空配列のpopは失敗、keysは挿入順。`'mapnew'`(引数0個)はmapを作る —— キーが名前でなく値そのものである2つ目のキー付きコンテナで、nil・bool・int・double・文字列は値で、それ以外は同一性で比較する。読み書きはobjectと同じ`index`/`set_index`、問い合わせも同じ`'len'`/`'objecthas'`/`'objectkeys'`/`'objectremove'`が答える。`'strslice'`/`'arrayslice'`(引数3個: 文字列または配列と、開始・終了index)は半開区間をコピーして取り出し、`'strbyte'`(引数2個)/`'strfrombyte'`(引数1個)は文字列から1バイト読む・1バイトの文字列を作る。コードポイントでなくバイトなので、独自のテキストモデルを持つfront end自身がデコードする。`'same'`(引数2個)は参照の同一性 —— 同じヒープオブジェクトか、スカラーなら同じタグと中身か —— を答える(`eq`はオブジェクト同士を拒む)。`'argcount'`(引数0個)は実行中の関数が呼ばれたときの実引数の個数(`set_lenient_arity`を参照)。`'genresume'`/`'genreturn'`/`'genthrow'`(引数2個)はgeneratorの活性化を駆動する(`set_generator`を参照)。`'enqueue'`(引数1個、引数0個のクロージャ)はそれを実行のジョブキューに積む —— エントリ関数が戻ったあとにFIFO順で消化され、各ジョブは次に移る前に最後まで走るので、ジョブ自身が積んだものは待っている全ジョブの後ろに並ぶ —— 値は`nil`。`'fnarity'`(引数1個)はクロージャの宣言上の仮引数の個数(関数以外は失敗)。`'collect'`(引数0個)はその場で完全なtracing collectionを走らせ、解放したオブジェクト数を返す(回収対象のdrop hookを新しいものから順に先に走らせる)。`'heapstats'`(引数0個)はランタイムのヒープの`{live_objects, heap_bytes}`を返す。`tostr`は整数値のdoubleを`4.0`でなく`4`と整形するので、表示規則が異なるフロントエンドは後処理する。`typeof`はタグを文字列(`'int'`・`'double'`・`'string'`…)で返す。`toint`はゼロ方向へ切り捨て、NaN・∞・範囲外は失敗。`fmod`はIEEE fmod(0除数は整数modと同じ失敗)。`pow`はdouble上。`tofloat32`はDouble(整数引数は`todouble`と同様まず幅拡張される)を最も近い`float`へ丸め、その値をちょうど保持するDoubleへ戻す —— `float`と`double`の両方を持つフロントエンドが自前では作れない丸め —— `float`自身の最大/最小値へ飽和し、実際のオーバーフロー境界を超えたときだけ+-infinityになる。NaNはそのまま通る |
| `m.array_lit(items_list:, line:, col:)` / `m.object_lit(kv_list:, line:, col:)` | 要素のステージング用listから配列を、key, value, key, value, …を持つlistからオブジェクトを組み立てる |
| `m.index(recv:, key:, line:, col:)` / `m.set_index(recv:, key:, value:, line:, col:)` | 読みと書き。`recv`の実体でディスパッチする: 配列はLongのindex(範囲外は失敗)、オブジェクトはStringのkey(無いkeyの読みは`nil`)、文字列は1バイトを文字列として読み出す(書きは拒否) |
| `m.field_get(recv:, slot:, name:, line:, col:)` / `m.field_set(recv:, slot:, name:, value:, line:, col:)` | フロントエンド自身が採番した`slot`にあるstructフィールド(localスロットの番号付けと同じ契約) —— `index`/`set_index`のkey比較ではなく直接読み書きするので、`recv`はどちらかがそこに届く前に、その通りのslot順で`object_lit`によって全フィールドが組み立て済みでなければならない。`name`はトラップメッセージ("field 'next' of …")のためだけに保持され、実行には一切使われない |
| `m.scope(first_local:, end_local:, body:, line:, col:)` | localスロット`[first_local, end_local)`を所有するレキシカル領域。どの経路で抜けても抜けた時点で解放され、中で登録されたdeferがLIFOで走る |
| `m.scope_release(first_local:, end_local:, body:, release_list:, line:, col:)` | 同じ領域を、解放順を明示して作る。`release_list`は`var_ref`ノード(範囲内の`'local'`か`'cell'`)の並びで、どの経路で抜けてもその順に解放される。フロントエンドは宣言の逆順を、捕獲されたスロットも含めて並べる —— そうすれば捕獲された束縛もフレームごとでなく自分の番で解放される。解放されたcellは作り直されるので、それを捕獲していたクロージャは古いcellをその値ごと持ち続ける |
| `m.make_return(value:, line:, col:)` | 実行中の関数から`value`を返す(裸の`return`は明示的な`nil_literal`で綴る) |
| `m.make_break(line:, col:, depth: 0)` / `m.make_continue(line:, col:, depth: 0)` | `while`を抜ける/再判定する。`depth`は外側のループをいくつ飛ばすかで、`0`(既定)が最も内側。自前でラベルを解決したfront endはdepthで答えればよく、IR側にラベル表は要らない。途中のスコープはどちらの場合も出る際に離れる。ループ本体の外や、実際に開いているより深いループを名指したものは`verify()`が弾く |
| `m.make_throw(value:, line:, col:)` | 任意の値を送出する |
| `m.make_try(caught_local:, body:, handler:, line:, col:)` | `body`を保護する。throw(またはexecutorのトラップ —— 0除算・型違いのオペランド)が運んだ値がlocalスロット`caught_local`に入り、`handler`で実行が再開する。トラップの値は`{message, line, col}`のオブジェクト。完了した側の子の値を返す |
| `m.make_defer(value:, line:, col:)` | 引数0個のcallableを、囲む`scope()`の脱出時に走るよう登録する —— fall-through・`break`・`continue`・`return`・throwのunwindのいずれでも走る。scopeの外のdeferは`verify()`が拒否する |
| `m.cell_fresh(cell:, line:, col:)` | フレームのcell 1個を新しいboxに置き換える —— 「ループの反復ごとの束縛」の正体。過去の反復で作られたクロージャは古いcellを保持し続ける |
| `m.make_yield(value:, line:, col:)` | 実行中のgenerator関数を中断し、再開した側に`value`を渡す。ノード自身の値は次の再開で送り込まれた値。generatorの外では`verify()`が拒む |
| `m.set_generator(func:)` | 関数`func`をgeneratorにする。呼び出すと本体を走らせる代わりに中断状態の活性化を包んで返す。`'genresume'`(活性化と送る値)は次のyieldまで走らせて`{value, done}`を答え、`'genreturn'`(活性化と値)は途中で閉じる —— 止まっていた本体の未実行のdeferを内側から順に走らせてから`{value, done: true}`を答える。`'genthrow'`(活性化と値)は止まっていたyieldにその値を、yield式が投げたかのように届ける —— 本体のハンドラが先に見て、投げが越えるdeferは走り、本体が捕まえなかったものはgeneratorをdoneにして呼び出しから出てくる。捕まえて再びyieldした本体は通常のresumeと同じく`{value, done: false}`を答える。まだ始まっていない・すでに終わったgeneratorには値の落ち先となるフレームがないので、呼び出しの場でそのまま投げられる |
| coroutineのintrinsic | coroutineは専用のノード形ではなく、6つのintrinsicで駆動する通常の関数。generator(上の`set_generator`)が1つのフレームを`yield`で止めるのに対し、coroutineは呼び出しスタック全体を止めるので、`'coroyield'`は何段深い呼び出しの中にあってもよい。`'corocreate'`(引数1個、関数値)が1つ包み、`'cororesume'`(引数2個: coroutineと送る値)はyieldか終了まで走らせて`{value, done}`を答える。`'coroyield'`(引数1個)は実行中のcoroutineを中断して値を外に渡し、次のresumeが送ってきた値を答える。`'coroclose'`(引数1個)は中断中のものを早期に終わらせ、止まっていたフレームのdeferを走らせる。`'corostatus'`(引数1個)は`'start'`/`'suspended'`/`'running'`/`'done'`を答え、`'corocurrent'`(引数0個)は実行中のコードが入っているcoroutine(最上位ならnil)を答える。すでに走っているもののresumeや、nativeのコールバックの中からのyield(止められるC++フレームが無い)は、おかしな動作をせず失敗する。`'enqueue'`はclosureと同じようにcoroutineも受け取り、これがjob queueをスケジューラにしている —— yieldしたcoroutineはparkされ、キューが後で戻ってくる |
| `m.set_lenient_arity(func:)` | `func`の呼び出しが引数の個数を問わなくなる。余分は捨て、届かなかった仮引数は`nil`で始まり、本体は実際に渡された個数を`'argcount'`で読める —— フロントエンドはそれで自前の「引数が足りない」診断を出すか既定値を埋める。指定しなければ個数の不一致は実行器のトラップ |
| `m.set_tail_calls(func:)` | `func`の中の末尾位置の呼び出し —— `make_return`のオペランド、またはblock・`if`・`switch`・scopeを通した本体の最後の値 —— が`func`自身のフレームを積まずに置き換える。呼び出しの連鎖として書いたループがどれだけ長くても1フレームで走り、深さ制限が問題にならなくなる。変わるのはフレームを出る**タイミング**だけ: 呼び先が走る前に出るので、ローカルのdropフックが呼び先の出力より先に来る(通常の呼び出しでは後)。`make_try`の本体の中や、deferまたは独自のrelease順を宣言したscopeを跨ぐ呼び出しは通常の呼び出しのまま —— どれもフレームがまだ在ることを必要とするため |
| `m.set_entry_frame_drops(on:)` | プログラム終了時に、入口関数自身の束縛のdrop hookを走らせるかどうか(既定は走らせる)。トップレベルのスコープをデストラクタなしで解放する言語のフロントエンドは切る。入口関数のdeferは変わらず走り、内側のスコープも通常どおりdropする |
| `m.list_new()` | ステージング用list。`stmts_list:`/`args_list:`に渡す |
| `m.list_push(list:, value:)` | ステージング用listにノードidを追加する |
| `m.add_func(name:, num_locals:, num_captures:, num_cells:, num_params:, body:)` | 関数。indexを返す(`funcs[0]`が`run()`の開始点) |
| `m.set_local_name(func:, index:, name:)` | localに名前を付ける(診断用のみ) |
| `m.set_capture_name(func:, index:, name:)` | captureに名前を付ける(診断用のみ) |
| `m.capture_map_new()` | ステージング用capture map |
| `m.capture_map_push(cmap:, kind:, index:)` | 転送する変数を1つ追加する |
| `m.add_capture_map(cmap:)` | 完成させる。結果を`call()`の`cmap:`に渡す |
| `m.verify()` | モジュールの構造を検査する。壊れていれば`IrError` |
| `m.run()` | verifyしてから実行する |
| `m.dump_ir()` | 木の可読なダンプ(デバッグ用) |
| `m.dump_bc()` | コンパイル済みbytecodeの可読なダンプ(デバッグ用) |

executorはdrop契約も履行する: `"\x01drop"`キーにクロージャを持つオブジェクトは、refcountが0になった瞬間・解放の前に、そのオブジェクト自身を引数としてクロージャが呼ばれる。throwするデストラクタはstderrに報告して飲み込む。デストラクタが引数をどこかへ保存したら復活扱いで解放はスキップされる。

`kind:`は`'local'`(その関数自身のスロット)、`'cell'`(同じくその関数自身の
スロットだが、この関数が行う呼び出しがcaptureできるようbox化したもの)、
`'capture'`(外側から借りたスロット)のいずれか。変数参照は実行時の名前解決
ではない —— 後述の「変数はcaptureであり静的リンクではない」を参照。

ステージング用listやcapture mapは、`block()`/`intrinsic()`/`call()`に渡した
瞬間に消費されて消える。渡した後に同じidを使い回すのは未定義。

### IRを読み返す

上のbuilderにはそれぞれ対応する読み出しがある: スクリプト自身の定数畳み込みや、
`dump_ir()`の文字列マッチではなくノードの形を検査するテストが、Compilerや
Dumperと同じ経路で木を辿れる。不正なnode/func/cmap id、あるいはtagやliteralの
種類を取り違えた読み出しは`IrError`を送出する —— `verify()`自身が報告するのと
同じ失敗の種類だ。

| 呼び出し | 読むもの |
| --- | --- |
| `m.num_nodes()` | これまでに作られたノード数 |
| `m.node_tag(node:)` | タグ名 —— `'literal'`・`'varref'`… `dump_ir()`が印字するのと同じ語彙 |
| `m.node_line(node:)` | そのノード自身のソース行 |
| `m.node_col(node:)` | そのノード自身のソース列 |
| `m.num_children(node:)` | `node`の子の数 |
| `m.child(node:, index:)` | `index`番目の子。ノードidで返る |
| `m.const_kind(node:)` | `literal`ノードが持つ5種のどれか —— `'nil'`/`'int'`/`'bool'`/`'double'`/`'str'` |
| `m.int_const(node:)` | int literalの値。他の種類では失敗する |
| `m.bool_const(node:)` | bool literalの値。他の種類では失敗する |
| `m.double_const(node:)` | double literalの値。他の種類では失敗する |
| `m.str_const(node:)` | str literalの値。他の種類では失敗する |
| `m.node_op(node:)` | `unary`/`binary`/`intrinsic`ノードの演算子名 —— `op:`/`name:`が受け取るのと同じ語彙。`varref`/`assign`は`var_kind()`へ誘導される |
| `m.var_kind(node:)` | `varref`か`assign`ノードの`'local'`/`'capture'`/`'cell'` |
| `m.var_index(node:)` | `varref`か`assign`ノードのスロットindex |
| `m.switch_subject(node:)` | `switch`ノードのsubject(ノードid) |
| `m.switch_arm_count(node:)` | `node`が持つ`(key, body)`の腕の数 |
| `m.switch_key(node:, index:)` / `m.switch_body(node:, index:)` | `index`番目の腕のkeyかbody(ノードid) |
| `m.switch_has_default(node:)` | `node`にdefaultの腕があるか |
| `m.switch_default_body(node:)` | そのdefaultの腕(ノードid)。無ければ失敗 |
| `m.field_slot(node:)` / `m.field_name(node:)` / `m.field_receiver(node:)` | `fieldget`か`fieldset`ノードについて、そのslot・診断専用の名前・受け手(ノードid) |
| `m.field_set_value(node:)` | `fieldset`ノードの値(ノードid) |
| `m.scope_first_local(node:)` | `scope`ノードが所有する最初のlocalスロット |
| `m.scope_end_local(node:)` | `scope`ノードが所有する最後のlocalスロットの1つ先 |
| `m.try_caught_local(node:)` | `try`ノードの捕捉値localスロット |
| `m.closure_func(node:)` | `makeclosure`ノードの関数index |
| `m.closure_cmap(node:)` | `makeclosure`ノードのcapture-map index |
| `m.cell_index(node:)` | `cellfresh`ノードのcell index |
| `m.num_funcs()` | 存在する関数の数 |
| `m.func_name(func:)` | その名前 |
| `m.func_num_locals(func:)` | そのlocalスロット数 |
| `m.func_num_captures(func:)` | そのcaptureスロット数 |
| `m.func_num_cells(func:)` | そのcellスロット数 |
| `m.func_num_params(func:)` | その仮引数の数 |
| `m.func_body(func:)` | その本体。ノードidで返る |
| `m.func_is_generator(func:)` | `set_generator`が呼ばれたかどうか |
| `m.func_lenient_arity(func:)` | `set_lenient_arity`が呼ばれたかどうか |
| `m.func_local_name(func:, index:)` | `set_local_name`がそのlocalに付けた名前 |
| `m.func_capture_name(func:, index:)` | `set_capture_name`がそのcaptureに付けた名前 |
| `m.num_capture_maps()` | 存在するcapture mapの数 |
| `m.num_capture_entries(cmap:)` | `cmap`が持つ要素の数 |
| `m.capture_kind(cmap:, index:)` | その要素の`kind:` |
| `m.capture_index(cmap:, index:)` | その要素のスロットindex |

`node_op`/`var_kind`/`capture_kind`/`const_kind`が返す文字列は、上のbuilderが
`op:`/`kind:`/`name:`として受け取るのと同じもの —— これらで読み戻した値は、
そのまま別のbuilder呼び出しに渡せる。

### 1度コンパイルして何度も走らせる

`m.compile()`は`m.run()`が行うことのうち実行そのものを除く全て —— `m`を
verifyし、`CodeGen.Program`を返す。だから同じコンパイル済みプログラムを
2度以上走らせたり、runの合間にheapを覗いたりできる —— どちらも`m.run()`
だけでは不可能だった。

| 呼び出し | すること |
| --- | --- |
| `m.compile()` | `m`をverifyする。`CodeGen.Program`を返す |
| `CodeGen.Runtime.new()` | プログラムが走れる空のheap |
| `p.run(rt: nil, max_call_depth: 10000, natives: nil)` | コンパイル済みプログラムを走らせる。`rt`は走らせる`CodeGen.Runtime`(nilなら使い捨てのヒープ)、`max_call_depth`はフレームスタックの上限。`natives`はモジュールの`declare_native`が名付けたものの実体を渡す —— 宣言済みの各名前を通常のculebraの`Function`に対応付けるObjectで、プログラムは他の値と同じように呼ぶ。モジュールが宣言した名前は全て表に無ければならない(無いものを名指しした`IrError`が、実行が始まる前に出る)。表にあってモジュールが宣言していない名前は単に使われないので、1つの表を複数のプログラムで使い回せる。境界を越える値はコピーされ、越えられるのは`nil`・`Bool`・`Long`・`Float`・`String`だけ: プログラムのヒープとculebra自身のヒープは独立に回収されるので、配列・オブジェクト・map・クロージャ・coroutineを渡すには2つのコレクタが合意する必要がある。それ以外は —— 引数でも戻り値でも —— 該当スロットを名指しした`TypeError`になり、プログラム自身の`make_try`で捕まえられる。引数の個数違い(関数自身のarityと突き合わせる)も、関数が投げたものも同じ: `nil`/`Bool`/`Long`/`Float`/`String`はそのまま届き、それ以外はculebraのエラーが持つ`{kind, message, line, col}`オブジェクトとして届く |
| `p.dump_bc()` | コンパイル済みプログラムに対する`m.dump_bc()`の出力 |
| `rt.live_objects()` | そのheapの現在のオブジェクト数 |
| `rt.heap_bytes()` | そのheapの現在のバイト数 |
| `rt.collect()` | その場で完全なcollectionを走らせる —— プログラムの中から`'collect'`intrinsicが走らせるのと同じものを外から走らせる。解放したオブジェクト数を返す |

```culebra
let m = CodeGen.Module.new()
let forty_two = m.binary(op: 'add', lhs: m.literal(v: 40, line: 1, col: 1),
                         rhs: m.literal(v: 2, line: 1, col: 1), line: 1, col: 1)
let args = m.list_new()
m.list_push(args, forty_two)
m.add_func(name: 'main', num_locals: 0, num_captures: 0, num_cells: 0, num_params: 0,
           body: m.intrinsic(name: 'print', args_list: args, line: 1, col: 1))
m.verify()

let p = m.compile()
let rt = CodeGen.Runtime.new()
p.run(rt: rt)  # => 42
p.run(rt: rt)  # => 42
inspect(rt.live_objects())  # => 0
```

**別の**コンパイル済みプログラムのオブジェクトをまだ保持している`Runtime`
では走らせられない: `Program`のクロージャは自分自身のchunk表を指すindexを
持つので、別プログラムのchunk表を読むと読み違える。`rt.collect()`(または
単に新しい`Runtime`を使うこと)で道が空く。

`entry_frame_drops`はコンパイル済み`Program`のプロパティであって`run()`の
引数ではない: `compile()`より前に`m.set_entry_frame_drops(on:)`を呼んで
決める —— `m.run()`の挙動を決めているのと同じフラグだ。

### 変数はcaptureであり静的リンクではない

変数参照は、実行中の関数自身のフレームのスロット(`kind: 'local'`)か、外側の
フレームから借りたスロット(`kind: 'capture'`)のどちらかを指す。教科書的な
静的スコープVMにある「レベル」という概念は無い: 静的リンクは変数を宣言した
フレームがまだスタック上に生きていることを前提にするが、これはクロージャで
破綻する。だからcapture転送という形にしておけば、クロージャが後から来ても
`var_ref`の意味を変えずに済む。

呼び出しごとの転送表は**呼び出し側**が持つものであって、呼ばれる関数側では
ない。自己再帰するプロシージャは、自己再帰の呼び出しでは自分のcaptureを
そのまま転送し、それを最初に呼ぶ側は自分のcellを転送する —— 同じ呼び出し先に
対して2種類の転送表があるのは、自己再帰の呼び出しが要求するものを関数単位の
表では表現できないからだ(この点の詳しい説明はcpp-vmlibのREADMEを参照)。

素の`'local'`はこの形で転送できない: それを持つフレームと運命を共にして
消えるが、組み立てている呼び出しはそのフレームより長生きするかもしれない
からだ。呼び出しがcaptureするlocalは、まず`'cell'`に昇格させる必要がある
——フレームとそれを捕まえたすべての呼び出しが共有するbox化スロットだ——
`capture_map_push`がそれを名指す前に。`verify()`は`kind: 'local'`の
capture map要素を、後で解放済みメモリを読みに行かせる代わりに拒否する。
どのスロットを昇格させるかはフロントエンド自身が解析すること——captureされる
変数の集合を歩き、そこに名前が出るスロットを昇格させる。昇格したスロットの
読み書きはcapture地点だけでなく全て`kind: 'cell'`に切り替わる。

### エラー・割り込み・再帰

0除算・未初期化の読み・型違いのオペランド・暴走する再帰 —— 実行時の失敗は
まず**組み立てたプログラムの内側で**捕捉できる: それを`body`に含む
`make_try`領域は、`{message, line, col}`のオブジェクトを受け取って`handler`で
再開する。どの`make_try`にも保護されない失敗(と`verify()`の構造エラー全て)
だけがculebra側へ届き、`kind: 'IrError'`の`CulebraError`として送出される:

```culebra
let m = CodeGen.Module.new()  # funcsが1つも無い -- verify()が検知する
inspect(try {
  m.run()
} catch e {
  e.kind
})  # => 'IrError'
```

`CodeGen`で組み立てたプログラムは、他の実行中のculebraコードと同じように
割り込み可能(Ctrl+C、isolate自身のcancel)であり、暴走する再帰呼び出しは
プロセス自身のスタックを溢れさせるのではなく`IrError`(`"recursion limit
exceeded"`)を送出する。

### 対象範囲

値は`nil`・真偽値・整数・浮動小数点・文字列・配列・オブジェクト・クロージャ・
generatorの活性化(`set_generator`を参照)。変数のcaptureはフロントエンドがIRを
組み立てる時点で一度だけ解決され、実行時には解決されない。`make_closure`で
組み立てたクロージャは第一級値で、変数に保持でき、`call_value`の引数として
渡せ、関数の結果として返せる。`CodeGen.Module`・`CodeGen.Program`・
`CodeGen.Runtime`のいずれもisolateの境界を越えられない
(`Isolate.spawn`/`Parallel.map`のworkerはそれぞれ自分自身のModuleを組み立てる)。

## 36. `StateMachine`

入れ子にできる状態機械。言語の組み込みではなく、素のculebraクラス。
状態は入れ子にでき、いま居る状態が
受け取らなかったイベントは親へ渡され、入れ子の外へ出る遷移はその親の
`exit`を通って出ていく。

作り方は2通りあるが、出来上がる機械は同じもの。`StateMachine.new`には
記述用の`Object`を渡すので、guardやactionはその場に普通の関数として書ける。
`StateMachine.parse`には下記のテキストを渡し、guardとactionは`guards:` /
`actions:`の表から名前で引く——§34のsemantic actionsと同じ「名前→関数」の表。

```culebra
let vending = `
  Vending {
    idle initial {
      coin: add_coin -> ready
    }
    ready {
      coin: add_coin -> ready
      select[enough]:  dispense -> idle
      select[!enough]: reject   -> ready
    }
  }
`
let m = StateMachine.parse(vending,
  guards: {enough: fn (ctx, ev) { ctx.balance >= 100 }},
  actions: {
    add_coin: fn (ctx, ev) { ctx.balance += ev.payload },
    dispense: fn (ctx, ev) { ctx.balance -= 100; ctx.log.push('dispensed') },
    reject: fn (ctx, ev) { ctx.log.push("only {ctx.balance}") },
  },
  context: {mut balance: 0, log: []})
m.fire('coin', 60)
inspect(m.state())      # => 'ready'
m.fire('select')
m.fire('coin', 60)
m.fire('select')
inspect(m.state())      # => 'idle'
inspect(m.context.log)  # => ['only 60', 'dispensed']
```

機械のテキストは**そのまま文字列**（バッククォートか単一引用符）で書く。
`"..."`だと波括弧が変数の埋め込みとして読まれてしまう。

### テキストの書き方

本文の1行は`event ['[' guard ']'] [':' action] ['->' target]`、状態の見出しは
`name [initial] [entry: action] [exit: action]`。本文側に予約語は無い——
`entry`・`exit`・`initial`が意味を持つのは見出しの位置だけなので、`entry`という
名前のイベントも書ける。`#`から行末までは注記。改行は普通の空白なので、
`a { x -> b }`と1行に詰めても同じ機械になる。

| 書き方 | 意味 |
| --- | --- |
| `name { ... }` | 状態。ブロックの中に状態を書けば入れ子になる |
| `name initial { ... }` | 親に入ったとき既定で入る状態 |
| `name entry: f exit: g { ... }` | その状態に入るとき`f`、出るとき`g`を呼ぶ |
| `e -> s` | `e`が来たら`s`へ移る |
| `e: f -> s` | 出る処理と入る処理の間で`f`を呼ぶ |
| `e: f` | 内部遷移。`f`だけ呼び、どの状態も出入りしない |
| `e -> 自分の名前` | 外部遷移。`exit`と`entry`が実際に走る |
| `e[g] -> s` | guard `g`が通ったときだけ |
| `e[!g] -> s` | 通らなかったときだけ |

状態名は1つの機械の中で重複しないので、`-> normal`のように経路を書かずに
名指しできる。同じイベントの行が複数あるときは書いた順に試し、guardが最初に
通った1本を採る。どれも通らなければ、そのイベントは親へ渡って探索が続く。

### 入れ子

遷移が出入りするのは**その遷移の範囲**より内側の状態だけ。範囲とは、遷移を
書いた状態と行き先の両方について、自分自身を含めない祖先のうち最も深いもの。
だから入れ子の中での兄弟どうしの移動では親は出入りせず、外へ出る移動では親の
`exit`が走る。入れ子の状態へ入るときは`initial`をたどって葉まで降り、途中の
`entry`を順に呼ぶ。

```culebra
let player = StateMachine.parse(`
  Player {
    stopped initial { play -> normal }
    playing entry: start_clock exit: stop_clock {
      stop -> stopped
      normal initial { seek -> seeking }
      seeking { done -> normal }
    }
  }
`, actions: {
  start_clock: fn (ctx, ev) { ctx.log.push('start') },
  stop_clock: fn (ctx, ev) { ctx.log.push('stop') },
}, context: {log: []})
inspect(player.state())                    # => 'stopped'
player.fire('play')                        # 初期状態をたどって葉まで降りる
inspect(player.state())                    # => 'normal'
inspect(player.in_state('playing'))        # => true
player.fire('seek')                        # `playing`の中に留まる
inspect(player.context.log)                # => ['start']
player.fire('stop')                        # `playing`に書いた遷移が効く
inspect((player.state(), player.context.log))  # => ('stopped', ['start', 'stop'])
```

### 記述用の`Object`

同じ機械を`Object`で書くと、状態名→`{initial, entry, exit, on, states}`。
`on`はイベント名から1件または`Array`への対応で、各件は
`{guard, negate, action, target}`。どのキーも省略できる。ここでのguardと
actionは関数そのものを書くのが普通だが、`String`を書けばテキスト版と同じく
`guards:` / `actions:`から引かれる。

```culebra
let bump = fn (ctx, ev) { ctx.n += 1 }
let m = StateMachine.new({
  off: {initial: true, on: {flip: {action: bump, target: 'on'}}},
  on: {on: {flip: {target: 'off'}}},
}, context: {mut n: 0})
m.fire('flip')
m.fire('flip')
inspect((m.state(), m.context.n))  # => ('off', 1)
```

### 一覧

| メンバ | 返り値 |
|---|---|
| `StateMachine.new(desc: Object, *, name: String = "", guards: Object = {}, actions: Object = {}, context = nil)` | `StateMachine` |
| `StateMachine.parse(text: String, *, guards: Object = {}, actions: Object = {}, context = nil, path: String = "")` | `StateMachine` — テキストから作る。`path`は`PEGError`に出す対象名 |
| `m.context` | 渡した`context:`そのもの。guardとactionが受け取る |
| `m.state()` | `String` — いま居る葉の状態 |
| `m.in_state(name: String)` | `Bool` — いま居る葉と、それを含むすべての状態について真 |
| `m.fire(event: String, payload = nil)` | `Bool` — 遷移したかどうか。どの状態も受け取らないイベントは無視され、誤りにはならない |
| `m.can_fire(event: String, payload = nil)` | `Bool` — `fire`が遷移するかどうか（guardは実際に呼ばれるので、副作用があってはいけない） |
| `m.reset()` | `Nil` — いまの状態から出て、初期状態に入り直す。contextは呼び手のものなので触らない |
| `"{m}"` / `to_string(m)` | `String` — `"StateMachine(Name, state=s)"` |

guardとactionは`fn (ctx, ev)`。`ctx`は渡した`context:`、`ev`は
`{name, payload}`という`Object`——後から項目が増えても、既にある関数が
受け取れなくなることはない。機械が初期状態に入る2つの場面——組み立てた
直後と`reset()`——にはきっかけとなるイベントが無いので、そこで走る`entry`
は`ev.name`が`nil`になる。状態名・guard名・action名は機械を組み立てる
時点で全て解決するので、綴り違いはそこで`StateMachineError`になる（その
イベントが実際に来るまで黙っている、ということがない）。初期状態が無い機械、
初期状態を持たない入れ子、初期状態が2つある兄弟も同じ時点で弾かれる。

扱わないもの: 並行領域と履歴状態（SCXMLの`<parallel>`と`<history>`）。
機械が同時に居る葉の状態は常に1つ。

## 37. `FST`

**書き換えない辞書を圧縮して持つ**ための名前空間。鍵の集合、または鍵から値への対応を、一度だけ小さなバイト列に組み上げ、あとは引くだけにする（エンジンは同梱の[cpp-fstlib](https://github.com/yhirose/cpp-fstlib)。有限状態トランスデューサ）。鍵どうしで前も後ろも共有するので大きな語彙がよく縮み、しかも展開せずそのまま引ける。引き換えに、組み上げたあとは足すことも消すこともできない。

鍵が多く、かつ「あるかどうか」以上のことを訊きたいときに向く。入力補完（`predictive_search`）、最長一致での切り出し（`longest_common_prefix_search`）、綴りの直し（`edit_distance_search`、`suggest`）。実行中に中身が変わる集合の在不在を見るだけなら、言語側の`Set`や`Object`のほうがよい。

組み上げと問い合わせが別の手順なのは、この構造が「一度組んで何度も引く」ためのものだから。多くの場合、引くのは別の実行、別のプロセスになる。

```culebra
let words = ["hello", "hell", "help", "world"]
let bytes = FST.compile_set(words)     # String。そのままファイルに書ける
let dict = FST.Set.new(bytes)          # 引くために開く
inspect(dict.contains("hell"))         # => true
inspect(dict.predictive_search("hel"))  # => ['hell', 'hello', 'help']
```

バイト列は普通の`String`で（`Compress`の出力と同じくバイト列をそのまま持てる）、専用のファイル API は要らない。

```culebra
# doctest: skip
FS.write("words.fst", FST.compile_set(words))
let dict = FST.Set.new(FS.read("words.fst"))
```

### 組み上げる

どれもバイト列を`String`で返す。`sorted: true`は「渡す鍵はもう並べ替えてある」という申告で、内部の並べ替えを省く。大きな整列済みの一覧では速くなる。申告が嘘だった場合は`FSTError`になる。

| 組み上げ | 結果 |
| --- | --- |
| `FST.compile_set(keys: [String], sorted: Bool = false)` | 鍵だけ → `FST.Set`で開く |
| `FST.compile_map(entries: Object, sorted: Bool = false)` | `{鍵: String}` → `FST.Map`で開く |
| `FST.compile_index_map(entries: Object, sorted: Bool = false)` | `{鍵: Long}` → `FST.IndexMap`で開く |
| `FST.compile_auto_index(keys: [String], sorted: Bool = false)` | 鍵だけ。値は並べ替えた順の位置 → `FST.IndexMap`で開く |

`compile_index_map`の値は`0..4294967295`の整数。外れた値はその場で`ValueError`になる。空の鍵、重なった鍵、`sorted: true`の申告に反する並びは、何番目かを添えて`FSTError`になる。

`compile_auto_index`は語彙に番号を振る小さな形。各鍵の値は「鍵を並べ替えたときの0始まりの位置」なので、別に配列を持たなくても辞書自身が番号表になる。

```culebra
let ranked = FST.IndexMap.new(FST.compile_auto_index(["cherry", "apple", "fig"]))
inspect(ranked.get("apple"))   # => 0
inspect(ranked.get("cherry"))  # => 1
inspect(ranked.get("fig"))     # => 2
```

### 引く

組み上げ方に応じて3つのクラスがある。どれもバイト列を受け取り、壊れていれば`FSTError`にする。**3種のうち別の種類として組まれたバイト列も弾く**ので、`Set`のバイト列を`Map`として開いてしまい、意味のない答えが返る、ということは起きない。

| 開き方 | 結果 |
| --- | --- |
| `FST.Set.new(bytes)` | `Set` — 鍵だけ |
| `FST.Map.new(bytes)` | `Map` — 鍵ごとに`String`を持つ |
| `FST.IndexMap.new(bytes)` | `IndexMap` — 鍵ごとに`Long`を持つ |

メソッドの顔ぶれは3つで共通で、違うのは結果が値を連れてくるかどうかだけ。`Set`は鍵と長さだけを返し、`Map`／`IndexMap`はそれぞれに`value`が付く。

| メソッド | `Set` | `Map`／`IndexMap` |
| --- | --- | --- |
| `contains(key)` | `Bool` | — |
| `get(key)` | — | 値。鍵が無ければ`nil` |
| `common_prefix_search(text)` | `[Long]` — `text`の先頭に一致した鍵それぞれの長さ | `[{length, value}]` |
| `longest_common_prefix_search(text)` | `Long`または`nil` | `{length, value}`または`nil` |
| `predictive_search(prefix)` | `[String]` — `prefix`で始まる鍵すべて | `[{key, value}]` |
| `edit_distance_search(word, max_edits, insert_cost = 1, delete_cost = 1, replace_cost = 1)` | `[String]` | `[{key, value}]` |
| `suggest(word)` | `[{ratio, key}]` | `[{ratio, key, value}]` |

長さは`Regex`の位置と同じく**バイト**単位。見つからなければ`nil`（`get`、`longest_common_prefix_search`）か空の配列なので、`??`や`?.`とそのままつながる。

`common_prefix_search`は「与えた文字列の先頭部分になっている鍵」をたどる。文章を辞書で切り出すときの「ここで何に一致するか」という問いがこれ。`predictive_search`は逆に「与えた文字列で始まる鍵」をたどる。こちらが入力補完の問い。

```culebra
let d = FST.Set.new(FST.compile_set(["hell", "hello", "help", "world"]))
inspect(d.common_prefix_search("helpless"))         # => [4]
inspect(d.longest_common_prefix_search("helpless"))  # => 4
inspect(d.longest_common_prefix_search("zebra"))     # => nil
inspect(d.predictive_search("hel"))  # => ['hell', 'hello', 'help']
```

`Map`は同じ形の答えに、しまってある値を添えて返す。

```culebra
let m = FST.Map.new(FST.compile_map({hello: "こんにちは", world: "世界"}))
inspect(m.get("hello"))    # => 'こんにちは'
inspect(m.get("missing"))  # => nil
inspect(m.predictive_search("w"))  # => [{key: 'world', value: '世界'}]
inspect(m.longest_common_prefix_search("worldly"))  # => {length: 5, value: '世界'}
```

### あいまいに引く

`edit_distance_search`は`max_edits`の範囲で届く鍵をすべて返す。3つの代価は直し方ごとの重みで、名前は**探す語の側から**付いている。`delete`は鍵にあって探す語に無いバイトを落とすこと、`insert`は探す語にあって鍵に無いバイトを足すこと、`replace`は1バイトを別のバイトに変えること。代価を上げると、その直し方で届く鍵が同じ予算から外れる。

```culebra
let d = FST.Set.new(FST.compile_set(["hell", "hello", "help"]))
inspect(d.edit_distance_search("helo", 1))  # => ['hell', 'hello', 'help']
# "helo"から"hello"へは、鍵側の2つめの"l"を落として届く:
inspect(d.edit_distance_search("helo", 1, 1, 2))  # => ['hell', 'help']
# 置き換えの代価を払うのは同じ長さの鍵だけ:
inspect(d.edit_distance_search("helo", 1, 1, 1, 2))  # => ['hello']
```

`suggest`は綴りを直すときの形。近さの順に並べて返すので、呼ぶ側は距離をあらかじめ決めずに上位いくつかを取れる。`ratio`はエンジンが出す`0..1`の近さ。

```culebra
let d = FST.Set.new(FST.compile_set(["their", "there", "third", "tier"]))
inspect(d.suggest("thier")[0].key)  # => 'their'
```

鍵は**バイト列として**比べる。大文字小文字も Unicode の正規化形も区別するし、編集距離が数えるのは文字ではなくバイトなので、複数バイトの文字を1文字書き換えると2つ以上の直しとして数えられる。そこが問題になる場面では、組み上げる前と引く前に`.lower()`や NFC への正規化をかけておく。

## 38. `Search`

**実験的。** 自分で決めた鍵のもとに文書を登録しておき、問い合わせ言語で検索して、
順位のついた結果を受け取る**全文検索**（エンジンは同梱の
[cpp-searchlib](https://github.com/yhirose/cpp-searchlib)。転置索引とBM25の
順位付け）。ここに出しているのはエンジンができることのごく一部で、今後増える。
リリースをまたいでAPIが変わりうるものとして扱ってほしい。

部分一致の走査では足りないとき——文書が多い、複数の語を組み合わせて絞りたい、
結果に順位が要る——に使う。1つの文字列から1つのパターンを探すなら`Regex`、
変わらない語彙への前方一致なら`FST`のほうが向いている。

```culebra
let idx = Search.Index.new()
idx.add("doc-1", "The quick brown fox jumps over the lazy dog.")
idx.add("doc-2", "A quick brown dog outpaces a lazy fox.")

for hit in idx.search("quick -dog") {
  inspect(hit.key)
}
```

索引は文書をメモリに持つ生きたハンドルで、ファイルと同じくスコープを抜けるときに
閉じられる。`close()`で明示的に閉じてもよい。

### 鍵は呼び出す側のもの

`add`には文書を呼ぶための鍵を渡し、`search`はその鍵をそのまま返す。鍵は
`String`で、`Long`は文字列化せずに拒否する——出てくる鍵が入れた鍵と同じもので
あるようにするため。すでにある鍵で`add`すると、その文書を置き換える。

```culebra
let idx = Search.Index.new()
idx.add("a", "first")
idx.add("a", "second")            # 置き換わる
inspect(idx.search("first"))      # => []
inspect(idx.search("second").size())  # => 1
```

`remove(key)`は文書を消す。知らない鍵なら何もしない。削除は論理削除で、領域は
索引を作り直すまで戻らない。

### 問い合わせ言語

| 書き方 | 意味 |
|---|---|
| `apple banana` | AND。すべての語を含む文書 |
| `apple \| banana` | OR |
| `apple -banana` | NOT。その語を含む文書を除く |
| `"apple tree"` | フレーズ。語が隣り合っていること |
| `apple ~ tree` | NEAR。4語の範囲内 |
| `app*` | 前方一致。`app`で始まる索引済みの語すべて |
| `a*e`、`*ana` | ワイルドカード。`*`は語のどこにでも置ける |
| `apple~2` | あいまい一致。2編集までの語 |
| `( ... )` | まとめる |

`NOT`には肯定の語が1つ以上要る。索引は全文書を列挙できないため。壊れた問い合わせは
何にも一致しないのではなく`SearchError`になる。

文書も問い合わせも同じ規則で語に切る。Unicodeの語境界
（[UAX #29](https://unicode.org/reports/tr29/)）で区切り、文字か数字を含む区間だけを
語として小文字に揃える。したがって`version 2.0`は`version`と`2.0`、`don't`や
`1,234.56`はそれぞれ1語で、語のあいだの空白と句読点は語にならず、大文字と小文字は
区別されない。空白で語を分ける文字体系は語ごとに切れる。分けない文字体系（漢字・
ひらがな・タイ文字など）は1文字ずつになり、カタカナの連なりだけは1語になる。これは
UAX #29自身が辞書に委ねると定めている箇所で、辞書なしの土台にあたる。`東京タワー`は
`東` `京` `タワー`として索引に入り、`東京`の問い合わせは`東`+`京`の句になるので
見つかる。`東京`を1語にするのは辞書に基づく splitter（下）の仕事。

### 解析器

索引には、文章を語に切る手順を自分で持たせられる。`add`のときと`search`のときで
同じものが走る。索引を作ったときと違う切り方で問い合わせると何も見つからないため、
2つを別々に設定することはできない。

```culebra
let stop_words = fn (t) {
  if ["the", "a", "of"].contains(t) { nil } else { t }
}

let idx = Search.Index.new(analyzer: { filters: [stop_words] })
idx.add("doc-1", "The quick brown fox")
inspect(idx.search("quick").size())  # => 1
inspect(idx.search("the"))           # => []
```

| 欄 | | |
|---|---|---|
| `splitter` | `fn (text: String) -> Array` | 文章を語に切る。要素は`{term, position, length}`で、語の文字列と、それが切り出されたバイト範囲。省くと上に書いた組み込みの切り方になる。`Search.segmenter`（下）や、C++で書いたsplitter（次の段落）も渡せる |
| `normalizer` | `fn (term: String) -> String` | 語ごとに、filtersより先に走る。省くと組み込みの小文字化 |
| `filters` | `fn (term: String) -> String \| Nil`のArray | normalizerのあとに順に適用する。置き換えた語を返すか、`nil`を返して捨てる |

出す語は、範囲が指すバイト列と一致していなくてよい。形態素解析器が原形を索引に入れ
つつ、ハイライトは書かれたとおりの文字列に当たる、という形にできるのはこのため。
範囲は重なってはならず、昇順でなければならない。

1つの語を複数に増やす filter は書けない。複数の出力は「どれか」を意味するので
問い合わせ側の仕事であり、語を**並び**に割るのは splitter の仕事だから。

**これらは語ごとに呼ばれる。** Culebraで書いた`normalizer`は全文書の全語に対して
走る。既定がネイティブなのはそのため。`splitter`は1文書につき1回、問い合わせの
1語につき1回。手軽な経路であって速い経路ではない——千語の文書は千回プログラムへ
戻ってくる。

C++で書いたsplitterはネイティブに走る。`ISplitter`を継承して`wrap<T>`で宣言した
クラスのハンドルは、そのまま`splitter`に渡せる。契約とハンドルの寿命は
[Searchにsplitterを挿す](deployment.ja.md#searchにsplitterを挿す)にある。

`Search.Index.load(path, analyzer)`も解析器を取る。保存したファイルは、どの解析器で
作られたかを一切記録しない。違うものを渡して開くと、誰にも気づけないまま壊れた結果が
返る。同じものを渡すこと。

### 日本語: モデルによる分かち書き

組み込みの切り方では漢字とひらがなは1文字ずつになる（上）。`Search.segmenter(model)`は
分かち書きのモデル（エンジンは cpp-searchlib に同梱の
[cpp-segmentlib](https://github.com/yhirose/cpp-segmentlib)）を読み込み、日本語の
文字が続く区間を語に切り、それ以外は組み込みの規則に任せる splitter を返す。
`iPhone 15を買った`は`iphone` `15` `を` `買っ` `た`、`東京タワー`は`東京` `タワー`として
索引に入る。解析器の`splitter`に渡して使う。ネイティブに走るので、文書ごとに
プログラムへ戻ることはない。

```culebra
# doctest: skip
let seg = Search.segmenter("ja-ud-gsd")
let idx = Search.Index.new(analyzer: { splitter: seg })
idx.add("doc-1", "私は東京タワーに行った")
inspect(idx.search("東京タワー").size())  # => 1
inspect(idx.search("京"))                # => []   東京 が1語になった
```

`model`は下の表にある名前か、cpp-segmentlibが読める形式のモデルファイルのパス。
読み込めないファイルは`SearchError`になる。segmenterは索引と同じくハンドルで、
スコープを抜けるか`close()`で解放する。索引は作られるときにsplitterを写し取るので、
あとでハンドルを閉じても索引には影響せず、そのハンドルで新しい索引を作れなくなるだけ。
モデルは上に書いた意味で解析器の一部——両側で同じものを使い、変えたら索引を作り直す。

| 名前 | モデル | 大きさ | ライセンス |
|---|---|---|---|
| `ja-ud-gsd` | cpp-segmentlibの参照モデルv0.1.1。UD Japanese-GSDで訓練 | 2.1 MB | CC BY-SA 4.0 |

名前は`Sys.data_dir("culebra")/models`に置かれた写しに解決される。初回はculebraが
端末で確認してから取得する。stdinかstderrが端末でないプログラム——スクリプト、CI、
パイプ——には聞かず、URLと写しを置くべきパスと、取得を避ける2つの方法（自分でその
パスに置く、パスを渡す）を添えた`SearchError`になる。ダウンロードはこのリリースに
固定されたダイジェストと照合してから書き、モデルのNOTICEを隣に置く。`CULEBRA_OFFLINE`が
`0`以外に設定されていれば何も取得しない。`Http`を名指さないAOTバイナリも取得できないが、
キャッシュは共有なので、同じマシンで`culebra`を一度走らせれば両方がそれを使う。

写しのファイル名にはバージョンが入る（`ja-ud-gsd-v0.1.1.mod`）ので、上流がモデルを
更新しても、索引を作ったときのモデルが差し替わることはない。語の境界が変われば索引は
作り直しで、いつ変えるかを決めるのはネットワークではなくculebraのリリースである。

### 言語は索引のもの

索引がどのモデルを使うか（使わないか）は解析器の設定で、つまり索引のもの。すべての
文書とすべてのクエリが同じものを通る。文書ごとの言語指定も、テキストからの推測もない。
クエリは数文字で文書は1ページなので、それぞれから推測すると両側で食い違う。`東京都庁`は
仮名に囲まれた文書の中では`東京` `都庁`に切れるが、クエリとして単独では推測の手がかりに
なる仮名がなく丸ごと1語のまま、何にも一致しない。文字体系の混在は言語ではなく文字体系で
扱う。モデルが日本語の文字が続く区間を引き受け、残りは組み込みの規則が切る——両側とも
同じように。別々のモデルが要る複数の言語のコーパスは、複数の索引にする。

### 検索結果

`search(query, limit = 10)`はObjectのArrayを、スコアの高い順に返す。

| 欄 | |
|---|---|
| `key` | 登録したときの鍵 |
| `score` | BM25の関連度。1回の結果の中では比べられるが、別の検索とは比べられない |
| `ranges` | 一致した語の位置。登録した文字列へのバイト単位の`{position, length}` |

`ranges`は該当箇所を目立たせるためのもの。

```culebra
let text = "A quick brown dog outpaces a lazy fox."
let idx = Search.Index.new()
idx.add("doc", text)
let r = idx.search("outpaces")[0].ranges[0]
inspect(text.slice(r.position, r.position + r.length))  # => 'outpaces'
```

### 保存と読み込み

```culebra
# doctest: skip
idx.save("notes.idx")
let reopened = Search.Index.load("notes.idx")
```

保存されるのは索引であって文書そのものではない。`search`が返すのは変わらず鍵と
バイト位置なので、それを解決するための元の文字列は呼び出す側が持っておく。ファイル
形式はエンジン自身のもので、このnamespaceが実験的なあいだはリリースをまたいで
安定しない。

### 一覧

| | |
|---|---|
| `Search.Index.new(analyzer: Object = nil) -> Index` | 空の索引 |
| `Search.Index.load(path: String, analyzer: Object = nil) -> Index` | 保存した索引を開く |
| `idx.add(key: String, text: String) -> Nil` | 文書を登録する。同じ鍵があれば置き換える |
| `idx.remove(key: String) -> Nil` | 文書を消す。知らない鍵は無視する |
| `idx.search(query: String, limit: Long = 10) -> Array` | 順位つきの結果 |
| `idx.save(path: String) -> Nil` | 索引を書き出す |
| `idx.close() -> Nil` | 解放する。何度呼んでもよい |
| `Search.segmenter(model: String) -> Segmenter` | `analyzer.splitter`に渡す、モデルによる日本語のsplitter。`model`は目録の名前かファイルのパス |
| `seg.close() -> Nil` | 解放する。それで作った索引はそのまま使える |

索引は`Sendable`ではない。結果が索引を参照しているので、作ったスレッドから出せない。
isolateごとに別の索引を持たせる。

## 39. 設計上の注記

### 名前空間ファースト、グローバルは出力の3つだけ

ライブラリが追加する**グローバル名は、matcher一族と出力用の3つだけ**
です。それ以外の関数は`Math`, `IO`, `Random`, `Sys`のいずれかに属し
ます。名前空間にまとめてあるおかげで、ホストアプリケーションに埋め込ん
でも、スクリプトのグローバルスコープに名前が大量に現れることはありま
せん。

例外が`inspect` / `print` / `println`です。毎回`IO.inspect`と書くのは
負担が大きいので、この3つはグローバルにも束縛されます。指す関数値は
`IO`配下と同一なので重複はありません。matcher一族と違ってサブコマンド
単位ではなく、どのレーンでも束縛されるので、埋め込みで動かすプログラム
とスクリプトとで`println`の意味が食い違うことはありません。V8は同じ線を
別の位置に引いていて、エンジン自体は`print`を提供せず、`d8`シェルが
後付けで導入しています。

### 名前空間はファーストクラス値

すべてのstdlib名前空間（`Math`, `IO`, `FS`, `Random`, `Sys`,
`Tensor`, `JSON`）は`Object`です。変数に束縛したり、関数引数
として渡したり、コレクションに格納でき、そのバインディング経由
のメソッド呼出は直接呼出と同じ意味論を保ちます:

```culebra
let io = IO
io.inspect("hello")  # IO.inspect("hello") と同じ

fn run_with(ns, x) {
  ns.inspect(x)
}
run_with(IO, "via parameter")
```

両backendがこれを保証します。JIT/AOTのスローパスはruntime
ディスパッチャ（`stdlib_rt.h::kNsRows_*`）を経由し、構文的
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

## 40. 未収録（将来検討）

### 重量級データ構造

`Set`と`Tuple`は言語組込みです（[`docs/language.ja.md`](language.ja.md)
参照）。`Deque`（§32）と`PriorityQueue`（§33）でキュー・ヒープの形は
カバーし、書き換えない辞書は`FST`（§37）が受け持ちます。ソート済み
map/treeはありません。順序が必要なら`Object`に`.sort()`/`.sorted()`
（言語仕様§18）を組み合わせてください。

### OS 拡張

生ソケットのTLSはありません（`Net`は平文で、TLSは [§15 Http](#15-http) が
自前で持ちます）。必要なら [§11 Proc](#11-proc) でサブプロセスに委譲して
ください。

---

関連: 言語仕様は [`docs/language.ja.md`](language.ja.md) にあります。
