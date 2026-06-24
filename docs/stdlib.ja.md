# Culebra 標準ライブラリ

本書は Culebra の**組み込みライブラリの API リファレンス**です。
ランタイムユーティリティをまとめた名前空間オブジェクト
（`Math`, `IO`, `Sys`, `FS`, `Time`, `Args`, `Random`, `String`）
を対象とします。 ここに記載のものは `import` 文なしで利用できます。

実例つきの導入とイディオムは [`guide.ja.md` §14](guide.ja.md#14-標準ライブラリ巡り)、
ライブラリの実装詳細と設計理由は [`internals.md`](internals.md) (英語) を
参照してください。

言語レベルの組み込み関数（`to_long`, `to_float`, `to_string`,
`type_of`, `range`, `iota`）は [言語仕様 §18](language.ja.md)
を参照してください。 matcher 一族 (`assert_true` / `assert_eq` /
`assert_throws` 等) は [§13 Matchers](#13-matchers) で扱います。
組み込み型（`String`, `Array`, `Object`）のメソッドは
[言語仕様 §17](language.ja.md) に規定されています。

CLI（`src/main.cc`）はこれに加え、`puts` と `print` を
`IO.puts` / `IO.print` のエイリアスとしてグローバルに配置します
（[言語仕様 §20](language.ja.md) 参照）。`culebra::environment()`
を直接呼び出す埋め込み用途では、これらのエイリアスは導入されず
名前空間はクリーンなままです。

本書で用いる記法:

* 型注釈は[言語仕様 §14](language.ja.md) に従います。`Any` は任意の
  値を示します。
* 例外条項は `type error at L:C.` 等の実行時エラーを示します。
  [言語仕様 §15](language.ja.md) を参照。

## 目次

1. [`Math`](#1-math) — 数値ユーティリティ・定数・整数列
2. [`IO`](#2-io) — 出力・標準入力（ファイル I/O は `FS`）
3. [`FS`](#3-fs) — パス操作・ファイル/ディレクトリ問い合わせ・更新
4. [`File`](#4-file) — ストリーミング読み書き/seek の状態付きハンドル
5. [`Time`](#5-time) — `Instant` / `Duration` クラス、ISO 8601、カレンダー算術、ナノ秒精度
6. [`Random`](#6-random) — シード可能な PRNG（uniform / gauss / shuffle / weighted_choice）
7. [`Sys`](#7-sys) — argv / exit / env / executable。`GC` ヒープ情報の取得も同節
8. [`Tensor`](#8-tensor) — N 次元数値テンソル、BLAS 対応 lazy graph
9. [`JSON`](#9-json) — stringify / parse の相互変換
10. [`Args`](#10-args) — 宣言的な CLI 引数パーサ (positional / option / subcommand / `--help`)
11. [`Proc`](#11-proc) — 外部コマンドを同期実行し stdout/stderr/終了コードを取得
12. [`Isolate`](#12-isolate) — クロージャを別スレッド（独立ヒープ）で実行、値は境界でコピー。`Channel` / `Parallel` / `Signal`（Ctrl+C をチャネルへ振り向け）/ `SharedBuffer`（zero-copy 共有する固定レイアウトデータ）/ `Shared`（参照共有する immutable 値）も同節
13. [Matchers](#13-matchers) — `assert_true` / `assert_eq` / `assert_throws` / `assert_close` 一族
14. [`Regex`](#14-regex) — 線形時間・grapheme 単位の正規表現
15. [`Http`](#15-http) — 同期 HTTP/HTTPS クライアント（get/post/put/delete/head/request）
16. [`Encoding`](#16-encoding) — スキーム別のテキストコーデック（`Encoding.html`、`Encoding.base64`、`Encoding.hex`、`Encoding.url`）
17. [`Compress`](#17-compress) — データ・ファイルの gzip 圧縮/展開
18. [`Hash`](#18-hash) — SHA-256/SHA-1/SHA-512/MD5 ダイジェストと HMAC（hex 出力）
19. [`CSV`](#19-csv) — RFC 4180 流の CSV を parse / stringify
20. [`UUID`](#20-uuid) — v4（ランダム）/ v7（時刻順）UUID 生成
21. [`Term`](#21-term) — TUI 向けの端末の色・カーソル制御・サイズ・キー/マウス入力
22. [設計上の注記](#22-設計上の注記)
23. [未収録（将来検討）](#23-未収録将来検討)

**目的別索引**

| やりたいこと | 参照先 |
|---|---|
| 定数（π、e、inf、nan） | [§1 Math 定数](#math-pi) |
| スカラー演算（abs / min / max / log / exp / sqrt / floor / ceil / round） | [§1 Math](#1-math) |
| 三角関数（sin / cos / tan / asin / acos / atan / atan2、ラジアン） | [§1 Math](#1-math) |
| 標準出力 | `IO.puts`（改行 + クォート付き） / `IO.print`（生） |
| ファイル全体を読む | `FS.read`（失敗時 throw） |
| ファイルをストリーム（行 / チャンク / seek） | [§4 File](#4-file) — `File.open` / `File.with` |
| パス操作（join / basename / dirname / stem / extension） | [§3 FS](#3-fs) |
| stat / walk / glob / copy / rename / symlink | [§3 FS](#3-fs) |
| ディレクトリ列挙・作成・削除 | `FS.list_dir`、`FS.mkdir`、`FS.remove` |
| `Instant` / `Duration` クラス、ISO 8601、カレンダー算術 | [§5 Time](#5-time) |
| 乱数 | `Random.int`、`.uniform`、`.gauss`、`.shuffle`、`.weighted_choice` |
| CLI 引数解析 | [§10 Args](#10-args) |
| プロセス情報 | `Sys.argv`、`Sys.exit`、`Sys.env`、`Sys.executable` |
| 外部コマンド実行 | [§11 Proc](#11-proc) — `Proc.run(["git", "status"])` |
| HTTP/HTTPS API を呼ぶ | [§15 Http](#15-http) — `Http.get("https://api.example/x")` |
| HTML エンティティの escape / unescape | [§16 Encoding](#16-encoding) — `Encoding.html.unescape("a &amp; b")` |
| base64 / hex / url のエンコード・デコード | [§16 Encoding](#16-encoding) — `Encoding.base64.encode(s)` |
| データ・ファイルの gzip / gunzip | [§17 Compress](#17-compress) — `Compress.gzip(s)` / `Compress.gunzip(z)` |
| ハッシュ / チェックサム / HMAC | [§18 Hash](#18-hash) — `Hash.sha256(s)` / `Hash.hmac_sha256(key, s)` |
| CSV のパース / 生成 | [§19 CSV](#19-csv) — `CSV.parse(text)` / `CSV.stringify(rows)` |
| UUID の生成 | [§20 UUID](#20-uuid) — `UUID.v4()` / `UUID.v7()` |
| 別スレッドで処理を実行（CPU 並列） | [§12 Isolate](#12-isolate) — `Isolate.spawn(\|\| fib(40))` |
| 固定レイアウトデータをスレッド/プロセス間で共有（zero copy） | [§12 SharedBuffer](#sharedbuffer--zero-copy-で共有する固定レイアウトデータ) — `SharedBuffer.new(n, Vec2)` / `.file` / `.shared` |
| 可変長の read-only データをスレッド間で共有（コピーなし） | [§12 Shared](#shared--参照共有する-immutable-値) — `Shared.new(value)` |
| Ctrl+C / SIGINT を綺麗に扱う | [§12 Signal](#signal--signalnotify--signalreset) — `Signal.notify(tx)` / `Signal.reset()` |
| ヒープ情報・リークチェック | [§7 GC](#gc--ヒープ情報の取得) — `GC.stat()` → `{live_objects, heap_bytes}` |
| 行列・テンソル演算（BLAS 対応） | [§8 Tensor](#8-tensor) |
| String / Array / Object のメソッド | [言語仕様 §17](language.ja.md) |
| 整数列（`range`, `iota`） | [言語仕様 §18](language.ja.md) |
| 変換（`to_long`、`to_float`、`to_string`、`type_of`） | [言語仕様 §18](language.ja.md) |

---

## 1. `Math`

数値ユーティリティ群。整数専用ルーチン（`pow`・`sign`・`clamp`）
は `Long` 入力を保ち、浮動小数点ルーチン（`log` ほか）は `Long` /
`Float` のいずれかを受け取ります。`Long` と `Float` の相互作用は
言語仕様 §4 / §7 を参照。

このセクションのサブグループ: **定数**（`Math.pi`、`Math.e`、
`Math.inf`、`Math.nan`） — **スカラー演算**（`abs`、`min`、`max`、
`log`、`exp`、`sqrt`、`floor`、`ceil`、`round`、`pow`、`sign`、
`clamp`） — **三角関数**（`sin`、`cos`、`tan`、`asin`、`acos`、
`atan`、`atan2`、ラジアン）。整数列ファクトリ `range` / `iota` は
言語コアグローバルで、[言語仕様 §18](language.ja.md#18-コア組み込み関数) を参照。

### 定数

`Math.pi` / `Math.e` / `Math.inf` / `Math.nan` は `Float` の
プロパティです。`--jit` でもコンパイル時定数として展開されます。

<a id="math-pi"></a>
#### `Math.pi`

`π` ≈ `3.141592653589793`。

#### `Math.e`

ネイピア数 ≈ `2.718281828459045`。

#### `Math.inf`

正の無限大（`Math.inf > 1e308 == true`）。負の場合は `-Math.inf`。

#### `Math.nan`

quiet NaN。`Math.nan == Math.nan` は IEEE-754 通り `false`。

```culebra
puts(Math.pi)              # 3.141592653589793
puts(Math.e)               # 2.718281828459045
puts(Math.inf > 1e308)     # true
puts(Math.nan == Math.nan) # false
```

### スカラー演算

### `Math.abs(x: Long|Float) -> Long|Float`

絶対値。`Long` 入力なら `Long`、`Float` 入力なら `Float` を返します。

```culebra
puts(Math.abs(-7))     # 7
puts(Math.abs(-7.5))   # 7.5
```

### `Math.min(a, b, ...) -> Long|Float`、`Math.max(a, b, ...) -> Long|Float`

2 つ以上の数値引数から最小 / 最大を取ります。全て `Long` なら `Long`、
1 つでも `Float` が含まれれば結果は `Float`。引数 1 個以下、または
数値以外が混じれば `type error`。

```culebra
puts(Math.min(3, 1, 4, 1, 5))   # 1
puts(Math.max(1.5, 2, 0.5))     # 2.0
```

### `Math.log(x: Long|Float) -> Float`

自然対数。`Math.log(0)` は `-inf`、負の値は `nan` を返します。
整数値を返す場合でも戻り値は常に `Float`。

### `Math.exp(x: Long|Float) -> Float`

`e` の `x` 乗。

### `Math.sqrt(x: Long|Float) -> Float`

主平方根。`Math.sqrt(-1.0)` は `nan`。

### `Math.sin(x) -> Float`、`Math.cos(x) -> Float`、`Math.tan(x) -> Float`

三角関数。`x` は**ラジアン**（`Long` または `Float`）。

```culebra
puts(Math.sin(Math.pi / 2))   # => 1.0
puts(Math.cos(0))             # => 1.0
```

### `Math.asin(x) -> Float`、`Math.acos(x) -> Float`、`Math.atan(x) -> Float`、`Math.atan2(y, x) -> Float`

逆三角関数。戻り値はラジアン。`asin` / `acos` は `x` が `[-1, 1]`
範囲（外は `nan`）。`Math.atan2(y, x)` は `y / x` の象限を考慮した
逆正接。

```culebra
puts(Math.atan2(1.0, 1.0))    # => 0.7853981633974483
# (= pi/4)
```

### `Math.floor(x: Long|Float) -> Long`、`Math.ceil(...) -> Long`、`Math.round(...) -> Long`

整数への丸め。`Long` 入力はそのまま返します。`Math.floor` は
`-∞` 方向、`Math.ceil` は `+∞` 方向、`Math.round` は
**偶数丸め（bankers' rounding）** — Python の `round()` と同じ挙動。

```culebra
puts(Math.floor(-1.5))   # -2
puts(Math.ceil(-1.5))    # -1
puts(Math.round(2.5))    # 2      (偶数側へ丸める)
puts(Math.round(3.5))    # 4
```

### `Math.pow(base: Long, exp: Long) -> Long`

整数累乗。繰り返し二乗法で `base ** exp` を計算します。
`Math.pow(x, 0)` は `x` に関わらず `1`（`0` を含む）。

**例外**: `exp < 0` のとき `type error at L:C.`。

後方互換のため残してあります。**基本は `**` 演算子を使ってください**
（`Float`・負指数も扱えます。言語仕様 §7）。

```culebra
puts(Math.pow(2, 10))    # 1024
puts(Math.pow(7, 0))     # 1
puts(Math.pow(-3, 3))    # -27
```

### `Math.sign(x: Long) -> Long`

負数で `-1`、ゼロで `0`、正数で `1` を返します。

```culebra
puts(Math.sign(-5))      # -1
puts(Math.sign(0))       # 0
puts(Math.sign(42))      # 1
```

### `Math.clamp(x: Long, lo: Long, hi: Long) -> Long`

`x` を閉区間 `[lo, hi]` に収めます。`lo > hi` の場合はエラーに
ならず `hi` を返します。

```culebra
puts(Math.clamp(5, 0, 10))   # 5
puts(Math.clamp(-5, 0, 10))  # 0
puts(Math.clamp(15, 0, 10))  # 10
```

---

## 2. `IO`

出力と標準入力。ファイルの読み書きは `FS`（`FS.read` / `FS.write` /
`FS.exists`）にあります。

### `IO.puts(x: Any) -> Nil`

`x` を改行付きで標準出力に書き出します。参照型は
`Array.str_array()` / `Object.str_object()` と同じ書式で整形され、
文字列は**シングルクォートで囲んで**出力されます。

```culebra
IO.puts('hi')       # → 'hi'
IO.puts(42)         # → 42
IO.puts([1, 'a'])   # → [1, 'a']
```

### `IO.print(x: Any) -> Nil`

`x` を**末尾改行なし**で標準出力へ書き出します。書式は `to_string`
と同じで、文字列は**引用符なし**で出力されます。複数回の書き込みで
1 行を組み立てたい場合に便利です。

```culebra
IO.print('Hello, ')
IO.print('world!')
IO.puts('')         # → Hello, world!
```

### `IO.input() -> String`

標準入力から 1 行読み取ります。末尾の改行は除かれます。EOF のとき
`''`（空文字列）を返します。

```culebra
# doctest: skip
puts('name?')
name = IO.input()
puts("Hello, {name}")
```

### `IO.read_all() -> String`

標準入力を EOF まで全て読み取ります。`FS.read("/dev/stdin")`（POSIX 専用＝
Windows に `/dev/stdin` は無い）の移植可能な代替。即 EOF なら空文字列。

```culebra
# doctest: skip
let src = if IO.stdin_is_terminal() { read_clipboard() } else { IO.read_all() }
```

### `IO.eputs(x: Any) -> Nil` / `IO.eprint(x: Any) -> Nil`

標準エラーへ書き出します（`puts` / `print` の双子）。`eputs` は文字列を
クォートし改行を付けます（`puts` 同様）、`eprint` は生の表示形（`print` 同様）。
stdout に混ぜたくない診断出力に使います。

### `IO.stdin_is_terminal() -> Bool` / `IO.stdout_is_terminal() -> Bool` / `IO.stderr_is_terminal() -> Bool`

指定した標準ストリームが端末に接続されているか（POSIX `isatty`）を返し
ます。対話性に応じた分岐に使えます。stdin ならプロンプト表示かパイプ
読み取りか、stdout / stderr なら色付けかプレーン出力か。Rust の
`io::stdin().is_terminal()` / Node の `process.stdin.isTTY` 相当。スト
リームがファイルやパイプにリダイレクトされている場合は `false` を返し
ます。

```culebra
# doctest: skip
let src = if IO.stdin_is_terminal() { read_clipboard() } else { FS.read("/dev/stdin") }
if IO.stdout_is_terminal() { puts(colorize(msg)) } else { puts(msg) }
```

`IO` は標準ストリームとコンソールの名前空間です。ファイルの読み書きは
`FS`（`FS.read` / `FS.write` / `FS.exists`）にあります。

---

## 3. `FS`

ファイルシステムのパス操作とディレクトリ操作。実装は
`std::filesystem`。変更系の呼び出しは失敗時に構造化された
`IOError`（`{kind: "IOError", message, line, col}`）を throw し、
埋め込み側でソース位置に紐づけてエラー処理できます。

### パス操作

#### `FS.join(parts...: String) -> String`

プラットフォームの区切り文字でパス要素を結合します。引数 0 個は
`""` を返します。`std::filesystem::path::operator/=` と同じく、
途中要素の末尾区切り文字は尊重されます。

```culebra
puts(FS.join('a', 'b', 'c.txt'))      # => 'a/b/c.txt'
puts(FS.join('/usr', 'local', 'bin')) # => '/usr/local/bin'
puts(FS.join())                       # => ''
```

#### `FS.basename(path: String) -> String`

最終要素（ファイル名＋拡張子）。末尾区切り文字のみのパスは `""`。

```culebra
puts(FS.basename('a/b/c.txt'))  # => 'c.txt'
puts(FS.basename('/'))          # => ''
```

#### `FS.dirname(path: String) -> String`

親パス。親が無い場合は `""`（`'c.txt' -> ''`）。

#### `FS.extension(path: String) -> String`

拡張子（先頭ドット込み）。無ければ `""`。ドットファイル
（`.hidden`）は拡張子なしとして扱います — `std::filesystem` 仕様
通り。

```culebra
puts(FS.extension('a/b/c.txt'))  # => '.txt'
puts(FS.extension('.hidden'))    # => ''
```

#### `FS.stem(path: String) -> String`

拡張子を除いた basename。

```culebra
puts(FS.stem('a/b/c.txt'))  # => 'c'
```

### ファイル全体の読み書き

#### `FS.read(path: String) -> String`

`path` のファイル全体を `String` として読み込みます（open + read +
close を1呼び出しで）。常にバイナリ: 戻り値は任意の内容を往復できる
byte string。逐次／ストリーミング読みは `File` ハンドルを使います。
ファイルが存在しない・読込不可・ディレクトリの場合 `IOError` を throw。

```culebra
# doctest: skip
contents = FS.read('data.txt')
```

#### `FS.write(path: String, content: String) -> Nil`

`content` を `path` に書き込みます（作成または上書き）。バイナリ、
改行変換なし。親ディレクトリが無い・書込不可の場合 `IOError` を throw。

```culebra
# doctest: skip
FS.write('out.txt', 'hello\n')
```

### 問い合わせ

#### `FS.exists(path: String) -> Bool`

`path` に何かが存在するか。ファイル／ディレクトリ／シンボリック
リンクの区別なし。空文字列や不正なパスは `false`。

#### `FS.is_file(path: String) -> Bool`

`path` が通常ファイルなら true。シンボリックリンクは follow。

#### `FS.is_dir(path: String) -> Bool`

`path` がディレクトリなら true。シンボリックリンクは follow。

#### `FS.size(path: String) -> Long`

ファイルサイズ（バイト）。`path` が存在しない／通常ファイルで
ない場合は `IOError` を throw。

### ディレクトリ変更系

#### `FS.list_dir(path: String) -> Array<String>`

`path` の直下エントリをファイル名（プレフィックスなし、`.` /
`..` 除く）の配列で返します。順序はファイルシステム任せなので
必要なら明示的にソートしてください。`path` がディレクトリでなけ
れば `IOError` を throw。

```culebra
# doctest: skip
let names = FS.list_dir('/tmp/build')
assert_true(names.contains('out.o'))
```

#### `FS.mkdir(path: String) -> Nil`

ディレクトリを作成。途中の親ディレクトリも含めて作成
（`mkdir -p` セマンティクス）。既存なら no-op。パスがファイル
として存在する／作成に失敗した場合は `IOError` を throw。

#### `FS.remove(path: String, recursive: Bool = false) -> Nil`

ファイルまたは空ディレクトリを削除（既定）。非空ディレクトリは
`IOError`。`recursive: true` でディレクトリツリーを削除（`rm -rf`）。
対象が存在しない／削除できない場合は `IOError`。

```culebra
# doctest: skip
FS.remove('/tmp/build/out.o')
FS.remove('/tmp/build', recursive: true)
```

#### `FS.rename(src: String, dst: String) -> Nil`

`src` を `dst` にリネーム／移動（同一ファイルシステム内は atomic）。
失敗時 `IOError`。

#### `FS.copy(src: String, dst: String, recursive: Bool = false) -> Nil`

ファイルをコピー（`dst` 既存なら上書き）。`recursive: true` で
ディレクトリツリーをコピー。失敗時 `IOError`。

### stat / メタデータ

#### `FS.stat(path: String) -> Object`

`{size, is_dir, is_file, is_symlink, mtime}` を返す。`size` はバイト
（非通常ファイルは 0）、`mtime` は Unix epoch 秒、`is_symlink` は
リンク自体を、他フィールドはリンク先を見ます。存在しなければ
`IOError`。

### 再帰探索

#### `FS.walk(path: String) -> Array<String>`

`path` 配下の全パスを再帰・深さ優先で。各要素はフルパス。
ディレクトリでなければ `IOError`。

#### `FS.glob(pattern: String) -> Array<String>`

glob `pattern` にマッチするパス（ソート済み）。セグメント単位で
`*` / `?` / `[...]`、`**` で再帰下降。シェル流 glob で `Regex` とは
別物です。

```culebra
# doctest: skip
let sources = FS.glob('src/**/*.cul')
```

### パス解決

#### `FS.abspath(path: String) -> String`

`path` の絶対・正規化形（カレントディレクトリ基準）。シンボリック
リンクは解決しません。

#### `FS.realpath(path: String) -> String`

シンボリックリンクを解決した正準パス（`weakly_canonical`、末尾の
存在しない要素は保持）。

#### `FS.normpath(path: String) -> String`

ファイルシステムに触れず字句的に正規化（`.` / `..` / 重複区切りを
畳む）。

#### `FS.is_abs(path: String) -> Bool`

`path` が絶対パスか。

### シンボリックリンク

#### `FS.symlink(target: String, link: String) -> Nil`

`target` を指すシンボリックリンクを `link` に作成。

#### `FS.readlink(path: String) -> String`

シンボリックリンクの参照先を読む。`path` がリンクでなければ
`IOError`。

#### `FS.is_symlink(path: String) -> Bool`

`path` がシンボリックリンク自体か（参照先でなく）。

---

## 4. `File`

`File` はストリーミング I/O 用の**状態を持つハンドル**で、`FS` の
一発の全読み書き（`FS.read` / `FS.write`）と対になります。`File.open`
または スコープ付きの `File.with` で開きます。I/O は常にバイナリ
（テキストモードの改行変換なし）で、`String` は byte string なので
任意の内容が往復します。

ハンドルは4つのメソッド群を実装します — **Reader**（`read` /
`lines` / `chunks`）、**Writer**（`write` / `flush`）、**Seekable**
（`seek` / `tell`）、**Closeable**（`close`）。どれが有効かは open
モードに依ります。

### 開く

#### `File.open(path: String, mode: String = "r") -> File`

`path` を開く。`mode` は `"r"`（読み）/ `"w"`（切り詰め+書き）/
`"a"`（追記）。それ以外は `ValueError`、開けなければ `IOError`。

#### `File.with(path: String, mode: String = "r", fn: Function) -> Any`

`path` を開き `fn(handle)` を呼び、あらゆる脱出経路（正常・`return`・
例外）でハンドルを閉じます。`fn` の戻り値を返します。`open` +
`defer { close }` の native 版で、ハンドルの寿命が1ブロックに収まる
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
| `for line in File.open(p).lines() { … }` | ストリーミング。iterator がループ脱出（`break` 含む）で閉じる |

明示的に閉じられなかったハンドルは GC バックストップが閉じますが、
それに頼らず上記3つのどれかを使ってください。

### Reader メソッド

#### `File.read() -> String` / `File.read(n: Long) -> String`

現在位置からのストリーミング読み: `read()` は残り全部、`read(n)`
は最大 `n` バイト（EOF では少なくなる）。ハンドル不要の一発全読みは
`FS.read(path)` を使います。

#### `File.lines() -> Iterator<String>`

行を反復、各行は末尾改行を剥がします（`\n` / `\r\n` / `\r` 全対応）。
iterator がハンドルを所有し、ループ終了・break で閉じます。

```culebra
# doctest: skip
for line in File.open('access.log').lines() {
  if line.contains('ERROR') { puts(line) }
}
```

#### `File.chunks(n: Long) -> Iterator<String>`

最大 `n` バイトの固定長チャンクを反復（最後は短いことあり）。
`lines()` と同じ close-on-exit 契約。

### Writer メソッド

#### `File.write(data: String) -> Nil`

現在位置に `data` を書き込み（生バイト、改行変換なし）。読み取り
専用ハンドルでは `IOError`。

#### `File.flush() -> Nil`

バッファした書き込みを OS にフラッシュ。

### Seekable メソッド

#### `File.seek(offset: Long, whence: String = "set") -> Nil`

カーソル移動。`whence` は `"set"`（先頭から）/ `"cur"`（相対）/
`"end"`（末尾から；負の `offset` を使う）。

#### `File.tell() -> Long`

現在のバイトオフセット。

### Closeable

#### `File.close() -> Nil`

ハンドルを閉じ、書き込みをフラッシュ。冪等 — 二重 close は no-op。
閉じたハンドルへの操作は `IOError`。

---

## 5. `Time`

Wall-clock + monotonic 時刻、ISO 8601 入出力、カレンダー算術。
モジュールが提供する 2 つのクラス — `Instant`（時点）と
`Duration`（時間幅）— 内部表現は Unix epoch 起点の `i64`
ナノ秒（範囲 ±292 年、完全ナノ秒精度）。

タイムゾーンは **UTC + local のみ**（`Asia/Tokyo` 等の名前付き
ゾーンは将来対応）。各 method は kw-only `utc:` フラグを取り、
`iso` は `utc: true` がデフォルト（Z 付き ISO 8601 が interop の
wire form のため）、それ以外は `utc: false`（local）デフォルト。

### 取得

#### `Time.now() -> Instant`

現在の wall-clock 時刻。NTP や手動時計調整の影響を受けるので、
経過計測には `Time.monotonic` を使うこと。

#### `Time.monotonic() -> Float`

最初の呼出（プロセス起動）からの経過秒数（sub-秒精度）。厳密に
非減少、壁時計変更の影響を受けない。ベンチや timeout の基本ツー
ル。

```culebra
# doctest: skip
let t0 = Time.monotonic()
do_work()
puts("elapsed: {Time.monotonic() - t0} s")
```

#### `Time.sleep(secs: Float) -> Nil`

現在スレッドを `secs` 秒以上ブロック。負値や 0 は no-op。

### `Instant` コンストラクタ

#### `Time.from_iso(s: String) -> Instant`

ISO 8601 タイムスタンプを parse。受け付ける variant:

- `2026-05-20T15:30:00Z`
- `2026-05-20T15:30:00.123Z`
- `2026-05-20T15:30:00.000123456Z`（ns 精度の入力）
- `2026-05-20T15:30:00+09:00`
- `2026-05-20T15:30:00-0900`
- `2026-05-20`（日付のみ — UTC 0:00 として扱う）
- `2026-05-20T15:30`（秒省略）

不正な入力は `ValueError` を throw。

#### `Time.from_unix(secs: Long|Float) -> Instant`

Unix epoch 秒から構築（Float なら sub-秒精度）。

#### `Time.from_parts(p: Object, utc: false) -> Instant`

parts dict から timestamp を組み立て — `Instant.parts` の逆操作。
認識キー: `year`、`month`、`day`、`hour`、`minute`、`second`、
`nanosecond`（デフォルト: `month=1`、`day=1`、その他 0）。それ
以外のキーは無視。

#### `Time.parse(s: String, fmt: String) -> Instant`

非 ISO 入力向けの厳格 strftime parse。format は POSIX `strptime`
準拠。`s` が `fmt` に一致しなければ `ValueError` を throw。結果は
local time として解釈。

```culebra
Time.parse("2026/05/20 15:30:00", "%Y/%m/%d %H:%M:%S")
```

### `Instant` method

#### `t.iso(utc: true) -> String`

ISO 8601 形式、完全ナノ秒精度（小数部が 0 の場合は省略）。デフォ
ルト UTC（`...Z`）、`utc: false` で local time + `±HH:MM` offset。

#### `t.format(fmt: String, utc: false) -> String`

strftime format で整形。デフォルトは local time。

```culebra
# doctest: skip
t.format("%Y-%m-%d %H:%M:%S")             # local
t.format("%Y%m%d", utc: true)             # 20260520
```

#### `t.parts(utc: false) -> Object`

`{year, month, day, hour, minute, second, nanosecond, weekday,
dayofyear}` に分解。`weekday` は ISO 8601 起点（`0=Mon`、`6=Sun`）、
`dayofyear` は 1-based（`1..366`）。

```culebra
let p = Time.now().parts()
if p.hour >= 9 && p.hour < 17 { puts("business hours") }
```

#### `t.weekday(utc: false) -> Long`

weekday 単体（0=Mon..6=Sun）。`parts()` の Object allocation を
避けたい場合用。

#### `t.add(years=0, months=0, days=0, hours=0, minutes=0, seconds=0, utc: false) -> Instant`

カレンダー算術。`years` / `months` は **月末 clamp** セマンティ
クス: `2026-01-31 + 1 ヶ月 → 2026-02-28`、
`2024-01-31 + 1 ヶ月 → 2024-02-29`（うるう年）。日以下の場は単純
加算。

```culebra
let next_month   = Time.now().add(months: 1)
let next_quarter = Time.now().add(months: 3)
let next_year    = Time.now().add(years: 1)
```

#### `t.start_of(unit: String, utc: false) -> Instant`

カレンダー単位の先頭に丸める。`unit` ∈ `"year"` / `"month"` /
`"day"` / `"hour"` / `"minute"`。それ以外は `ValueError`。

```culebra
# doctest: skip
let day_bucket  = t.start_of("day")
let hour_bucket = t.start_of("hour")
```

#### `t.unix() -> Float`、`t.unix_nanos() -> Long`

Unix epoch を Float 秒（現在時点で ~400ns 精度）または Long ns
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

`n` は Long でも Float でも可 — 小数単位はナノ秒に丸める。

### `Duration` method

#### `d.seconds() / .milliseconds() / .minutes() / .hours() / .days() -> Float`

指定単位での値（小数単位を round-trip するため常に Float）。

#### `d.abs() -> Duration`

絶対値（負の duration を正に）。

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

---

## 6. `Random`

乱数生成。プロセスごとに単一の Mersenne-Twister-64 エンジンを
持ち、インタプリタと JIT で共有しています。`Random.seed(n)` は
エンジンをリセットし、以降の呼び出しを 1 回のプログラム実行内で
再現可能にします。`seed` を呼ばなければ `std::random_device` で
初期化されます。

### `Random.seed(n: Long) -> Nil`

PRNG を再シード。同じ `n` → 同じ系列。

```culebra
Random.seed(42)
```

### `Random.int(lo: Long, hi: Long) -> Long`

半開区間 `[lo, hi)` からの一様整数。`hi > lo` 必須、
違反すると `type error`。

```culebra
Random.seed(0)
puts(Random.int(0, 10))        # 0..9
```

### `Random.uniform(lo: Float, hi: Float) -> Float`

半開区間 `[lo, hi)` からの一様実数。`Long` 引数も受け付け、
`Float` に昇格します。

### `Random.gauss(mu: Float, sigma: Float) -> Float`

平均 `mu`、標準偏差 `sigma` のガウス分布から 1 サンプル。
`Long` 引数は `Float` に昇格します。

```culebra
Random.gauss(0.0, 1.0)         # 標準正規
```

### `Random.shuffle(a: Array) -> Nil`

Fisher–Yates によるインプレース置換。`nil` を返し、引数は破壊的に
並び替えられます。

### `Random.weighted_choice(pop: Array, weights: Array) -> Any`

対応する `weights` に比例する確率で `pop` から 1 要素を取り出します。
`weights` はすべて数値かつ `pop` と同じ長さである必要があります。
空または長さ不一致は `type error`。重み `0` は選ばれません。

```culebra
Random.weighted_choice(['hit', 'miss'], [1, 9])   # ~10% 'hit'
```

---

## 7. `Sys`

プロセスレベルの情報。

### `Sys.argv -> Array`

スクリプトにコマンドラインで渡された `String` 引数の配列。最初の
非フラグ引数がスクリプトパスで、**それより後ろ**がすべて `argv`
として取り込まれます（Python / Node の慣習）。culebra 自身のフラグ
（`--jit`・`--debug` など）はスクリプトパスより前に置く必要があり
ます。末尾引数が無い場合や REPL 実行時は空配列です。

```culebra
# $ culebra run.cul hello world
puts(Sys.argv)        # ['hello', 'world']
# $ culebra --jit run.cul hello   →  ['hello']   (--jit は culebra 用)
```

単独の `--` は任意の escape hatch です。フラグ解析を打ち切るので、
ダッシュで始まるファイル名でも次の引数をスクリプトにできます
（例: `culebra -- -weird.cul`）。通常は不要です。

### `Sys.exit(code: Long) -> Nil`

指定の終了コードでプロセスを即座に終了します。呼び出しは戻らず、
保留中の `defer` 文も**実行されません**。

```culebra
# doctest: skip
if error_occurred { Sys.exit(1) }
```

### `Sys.env(name: String) -> String`

環境変数 `name` の値を返します。未設定の場合は `''`（空文字列）。
未設定と空文字列設定を区別したい場合は `.size() > 0` を使用。

```culebra
puts(Sys.env('HOME'))          # '/Users/alice'
puts(Sys.env('NOT_A_VAR'))     # ''
```

### `Sys.executable -> String`

実行中の culebra バイナリの絶対パス。`culebra` が `PATH` にあることに頼らず、
インタプリタのワーカーコピーを起動するのに使う — 例
`Proc.run([Sys.executable, "worker.cul"], ...)`。（AOT ビルドされたプログラムでは
その単体バイナリ自身のパスになる。）

```culebra
# doctest: skip
puts(Sys.executable)           # '/usr/local/bin/culebra'
```

### `GC` — ヒープ情報の取得

`GC.stat()` はフルコレクションを実行し、その直後の生きたヒープを表す
`Object` を返す:

| キー | 型 | 意味 |
|---|---|---|
| `live_objects` | `Long` | 到達可能なヒープオブジェクト数 |
| `heap_bytes` | `Long` | それらが占めるバイト数 |

先にコレクションを走らせるので、数値は sweep 待ちの循環残渣ではなく
*到達可能な* 状態を表す。呼び出し自体が結果 `Object` を確保するため、
連続して読むと小さな定数分だけ差が出る — 絶対値ではなく対象コード前後の
差分（delta）を測ること。

```culebra
# doctest: skip
let base = GC.stat().live_objects
build_some_structure()
puts(GC.stat().live_objects - base)   # 構造が保持しているオブジェクト数
```

これはリーク回帰テストの土台になる（`tests/test_gc_no_leak.cul` 参照）:
多数の反復をまたいで delta が有界に留まることを assert する。メモリは
それ以外は自動管理 — メモリモデルと確定的 `drop` は言語仕様を参照。

---

## 8. `Tensor`

N 次元数値テンソル。lazy 計算グラフを構築し、`Tensor.eval(...)` で
BLAS / vDSP 経由のカーネルを起動して値を確定します。dtype は
`Float32`（デフォルト）と `Float64`、形状は variadic か `[m, n]`
配列で指定。`transpose` / `slice` / `reshape` は zero-copy view。

```culebra
let A = Tensor.from([[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]])  # [2, 3]
let B = Tensor.randn(3, 2)
let C = A.dot(B) + 1.0                # lazy: グラフを作るだけ
Tensor.eval(C)                        # ここで BLAS GEMM が走る
puts(C.shape())                       # [2, 2]
puts(C.to_array())                    # [[..., ...], [..., ...]]
```

### 構築（名前空間関数）

#### `Tensor.zeros(...) -> Tensor` / `Tensor.ones(...)` / `Tensor.randn(...)`

形状を variadic（`Tensor.zeros(3, 4)`）または Array
（`Tensor.zeros([3, 4])`）で受け取ります。dtype は `"f32"` か
`"f64"` の文字列を**第一引数**に置く Julia 流：

```culebra
let a   = Tensor.zeros(3, 4)              # F32 default
let a64 = Tensor.zeros("f64", 3, 4)       # 明示
let dims = [3, 4]
let b   = Tensor.zeros(dims)              # 計算済み形状
let r   = Tensor.randn(2, 3)              # 標準正規
```

#### `Tensor.from(arr: Array) -> Tensor`

ネストされた Culebra 配列を Tensor に変換します。1D（`[1.0, 2.0]`）
または 2D（`[[1.0, 2.0], [3.0, 4.0]]`）を受け、F32 で格納：

```culebra
let v = Tensor.from([1.0, 2.0, 3.0, 4.0])      # [4]
let m = Tensor.from([[1.0, 2.0], [3.0, 4.0]])  # [2, 2]
```

#### `Tensor.concat(parts: Array) -> Tensor`

Tensor を軸 0（行）方向に積み重ね、1 つの materialized Tensor に
します。すべての part は dtype が一致し、軸 0 より後ろの次元も一致
している必要があります。結果の行数は各 part の行数の合計です。
微分可能 — 勾配は各 part の行範囲に切り分けて戻されます。

```culebra
let a = Tensor.from([[1.0, 2.0], [3.0, 4.0]])  # [2, 2]
let b = Tensor.from([[5.0, 6.0]])              # [1, 2]
let c = Tensor.concat([a, b])                  # [3, 2]
```

#### `Tensor.from_csv(path: String) -> Tensor`

CSV ファイルを直接 contiguous な Tensor に読み込みます。常に
**rank-2** を返す — 単列 CSV は `[N, 1]`（bias ベクトル形式）。
nested Array を経由しないので、`Tensor.from(load_2d(path))`
パターンより 3-5x 速い（MNIST 規模で実測）：

```culebra
# doctest: skip
let W1 = Tensor.from_csv("W1.csv")    # [30, 784]
let b1 = Tensor.from_csv("b1.csv")    # [30, 1]
let X  = Tensor.from_csv("X.csv")     # [N, 784]
```

#### `Tensor.eval(t1, t2, ...) -> Nil`

可変長の Tensor を受け、依存グラフを topological 順に評価します。
共有部分式は一度だけ計算されます。学習ループの mini-batch 境界で
**必ず一度呼ぶ**（呼ばないとグラフが累積してメモリが膨張）。

```culebra
# doctest: skip
W2 = W2 - d2.dot(a1.transpose()) * lr
b2 = b2 - d2.sum(1).reshape([N_OUT, 1]) * lr
W1 = W1 - d1.dot(xb.transpose()) * lr
b1 = b1 - d1.sum(1).reshape([N_HID, 1]) * lr
Tensor.eval(W1, b1, W2, b2)              # 4 つを 1 パスで評価
```

### 活性化関数

Tensor のインスタンスメソッドです。ユーザのクラスが独自に `relu` /
`sigmoid` / `softmax` を定義していても（microgpt の `Value.relu()`
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

形状・線形代数・reduction はメソッド構文：

| メソッド | 戻り値 | 説明 |
|---|---|---|
| `.shape() -> Array` | Array of Long | 形状を Array で返す |
| `.dot(other: Tensor) -> Tensor` | lazy | 行列積。両辺 rank-2 |
| `.linear_sigmoid(x, b) -> Tensor` | lazy | 融合 `sigmoid(self @ x + b)` |
| `.pow(exp) -> Tensor` | lazy | elementwise 冪、exp は Tensor または scalar |
| `.transpose() -> Tensor` | view | 全軸逆順（rank-2 で行列転置） |
| `.slice(start, end) -> Tensor` | view | 軸 0 を `[start, end)` で切り出し |
| `.reshape(dims: Array) -> Tensor` | view | 連続入力のみ。新形状 |
| `.sum() -> Float` | scalar | 全要素和（暗黙 eval） |
| `.sum(axis: Long) -> Tensor` | lazy | 軸を 1 つ畳む |
| `.mean() / .mean(axis)` | Float / Tensor | 同様 |
| `.max() / .max(axis)` | Float / Tensor | 同様 |
| `.argmax(axis: Long) -> Tensor` | lazy | 軸を畳んでインデックスを Float で格納 |
| `.to_array() -> Array` | eager | Culebra Array へ変換（暗黙 eval） |
| `.item() -> Float` | eager | 唯一の要素を Float として取り出す。要素数が 1 でない（任意 rank）場合は例外 |

`.item()` はスカラーの取り出し口で、`.to_array()`（形状を持つデータ用）と対をなす。
loss など単一要素の結果を reshape せず読むのに使う。`loss.item()` は
`to_float(loss.to_array()[0])` の置き換え。

### 自動微分（reverse-mode）

Tensor プリミティブはネイティブな reverse-mode 自動微分エンジンを
持ちます。各 lazy op が記録する forward グラフがそのまま tape を
兼ね、`.backward()` が C++ 側でそれを辿ります。スクリプト側の
ラッパは不要 — 値を計算する op 自身が vector-Jacobian product を
知っています。

| メソッド | 戻り値 | 説明 |
|---|---|---|
| `.requires_grad() -> Tensor` | self | 葉に勾配累積を要求。チェーン可 |
| `.backward() -> Nil` | — | `dL/dself = 1` を起点に全葉へ伝播 |
| `.grad() -> Tensor` | Tensor | 累積勾配（`backward` 前は zeros） |
| `.zero_grad() -> Nil` | — | 次ステップ前に勾配をクリア |
| `.detach() -> Tensor` | Tensor | グラフも勾配も持たない materialized コピー |

`requires_grad` は forward に伝播します — 勾配追跡する入力を持つ op
の出力も勾配を追跡します。微分可能な op は `+ - * /`、`.pow()`（底に
ついて）、`.dot()`、軸 `.sum()` / `.mean()`、`.relu()`、`.sigmoid()`、
`.softmax()`、`.log()`、`.transpose()`、`.reshape()`、`.slice()`、
`Tensor.concat()`。勾配は自動で un-broadcast されるので、バッチ越しに
加えた bias は元の形状に和を取って戻ります。

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
optimizer 更新のため `.grad()` を読む → 新しい重みを `.detach()` して
次ステップをクリーンな葉から始める、という流れです。
`benchmarks/microgpt/microgpt_native.cul` の transformer が完全な実例
（embedding、KV キャッシュ付き attention、RMSNorm、MLP、交差エントロピー、
Adam）で、すべてこれらのメソッドだけで構築されています。

`.backward()` は forward バッファから勾配を読むため loss の
`Tensor.eval` を伴います。`.grad()` は他と同じ Tensor を返すので、
`.to_array()` の前に `Tensor.eval` で materialize してください。

`Tensor.no_grad(fn) -> Any` は勾配追跡を抑制して `fn` を実行します。
内部の演算は autograd グラフを作らず（テープも `requires_grad` 伝播も
発生しない）、`fn` の戻り値をそのまま返します。推論など、逆伝播しない
forward に使います。

```culebra
# doctest: skip
let logits = Tensor.no_grad(fn () { model_forward(x) })
```

### 演算子オーバーロード

`+ - * /` はブロードキャスト elementwise（numpy / silarray 規則）。
スカラーとの混在も自動：

```culebra
let M = Tensor.ones(3, 4)
let v = Tensor.ones(4)            # → [3, 4] にブロードキャスト
let r = Tensor.ones(3, 1)         # → [3, 4] にブロードキャスト
Tensor.eval(M + v, M + r, M + 1.0, M * 2.0)
```

`@` 演算子は未実装（`.dot()` を使う）。

### 複合代入 (`+=` `-=` `*=` `/=` `**=`) と in-place 書き込み

複合代入は左辺の Tensor バッファに直接書き戻します（新規 Tensor を
確保しない）。条件は「LHS が自前バッファを所有していて、形が右辺の
broadcast 結果と一致する」こと — view・未評価グラフノード・形状不一致
の場合は通常経路（新規 Tensor）に自動でフォールバックします。

```culebra
# doctest: skip
mut W = Tensor.randn('f32', 1024, 256)
let alias = W
W -= grad * lr     # W のバッファを直接書き換え
Tensor.eval(alias) # alias の to_array() でも更新後の値が見える
```

SGD 形式の重み更新で `W = W - grad * lr` と書くと毎ステップ W サイズ
分の確保が起きます。`-=` ならその確保が消えるので、巨大重みでループが
回るほど差が広がります（実測で 5000 ステップ・1024×256 f32 の loop で
plain `=` 5.5s → `-=` 3.6s）。

サポート op: `+= -= *= /= **=`（`%=` と `@=` は対象外 — `%` は
Tensor で意味づけしておらず、`@` は出力形状が変わるため in-place 不可）。

### dtype / 形状の制約

- dtype は F32 / F64 のみ。binop / dot は同 dtype 必須（暗黙昇格なし）
- `.dot()` は rank-2 のみ。3D+ batched matmul は将来検討
- `.reshape()` は連続入力のみ（transpose 後 reshape は materialize が必要 —
  今は明示的に `Tensor.from((...).to_array())` を経由）
- `.softmax()` も連続入力のみ

### バックエンド

Phase 1 では **CPU** のみ。

- macOS: Accelerate framework（`cblas_sgemm/dgemm`、scalar fallback で sigmoid）
- Linux: OpenBLAS（`find_package(BLAS)`）

将来 Metal / CUDA を `tensor_backend.h` の dispatch table 経由で
追加予定。API は変わらず、`use_cpu()` / `use_metal()` /
`use_cuda()` のグローバル切替で動作先を選ぶ silarray 流。

---

## 9. `JSON`

Culebra の値と JSON テキストの相互変換。両バックエンドで同じ API
を提供します。

### `JSON.stringify(v, indent=0, sort_keys=false, lines=false) -> String`

`v` を JSON 文字列にシリアライズします。

* `indent > 0` でそのスペース数でインデントし pretty-print します
  （カンマの後に改行、`":"` の代わりに `": "`）。`indent <= 0` は
  コンパクト出力。
* `sort_keys=true` で `Object` のキーを挿入順ではなく辞書順で
  出力します。diff / ハッシュ向けの決定論的出力に有用。
* `lines=true` で **JSON Lines** を出力します。`Array` / `Tuple` /
  `Set` の各要素をそれぞれ独立した行として compact 形式で出力し、
  末尾 `\n` 付き。空コレクションは空文字列を返します。`indent > 0`
  との併用や、Array/Tuple/Set 以外への指定はどちらも `TypeError`。

旧 API との互換: 位置引数 2 番目 を `indent` として受けます。
`JSON.stringify(v, 2)` は `JSON.stringify(v, indent: 2)` と等価。

サポート対象:

| Culebra            | JSON                              |
|--------------------|-----------------------------------|
| `Nil`              | `null`                            |
| `Bool`             | `true` / `false`                  |
| `Long`, `Float`    | 数値（非有限 Float は `ValueError` を投げる） |
| `String`           | クォート文字列、`\n`/`\t`/`\r`/`\"`/`\\`/`\u00xx` エスケープ |
| `Array`            | JSON 配列                          |
| `Tuple`            | JSON 配列（`Array` と同じ形）         |
| `Set`              | JSON 配列、メンバーは挿入順            |
| `Object`（String キーのみ）| JSON オブジェクト、キーは挿入順       |

`Function`, `Tensor`、および非 String キーを持つ Object は
シリアライズ不可で `TypeError` を投げます。

### `JSON.parse(s, lines=false, number_mode='auto') -> Any`

JSON 文字列を Culebra の値に変換します。

* `number_mode='auto'`（既定）: 小数点や指数を含まない数値は `Long`、
  それ以外は `Float`。
* `number_mode='float'`: すべての数値を `Float` に。生産者側が
  数値型を統一している場合の round-trip 安全性向上に。
* `lines=true`: 入力を `\n` で分割し、空でない各行を独立した JSON
  値として解析、`Array` を返します。

不正な入力には `ValueError` が投げられ、構造化 Error の
`e.line` / `e.col`（共に 1-based、エラー位置の文字）に JSON 内部の
位置が乗ります:

```culebra
let r = try { JSON.parse('{"a": ,}'); nil } catch e { e }
puts(r.message)           # JSON.parse: expected value at 1:7.
puts("{r.line}:{r.col}")  # 1:7
```

例:

```culebra
let v = {name: 'alice', age: 30, tags: ['admin', 'staff']}
puts(JSON.stringify(v))                              # コンパクト
puts(JSON.stringify(v, indent: 2))                   # pretty
puts(JSON.stringify(v, sort_keys: true))             # 辞書順
puts(JSON.stringify([1, 2, 3], lines: true))         # JSONL
let back = JSON.parse(JSON.stringify(v))
puts(back.name)                                      # alice
let arr = JSON.parse("1\n2\n3\n", lines: true)
puts(arr)                                            # [1, 2, 3]
```

JIT メモ: ビルトインの `JSON.{stringify, parse}` は他の名前空間
メソッドと同じ正準呼び出しリゾルバを経由するため、すべての呼び出し
形がインタプリタと同一に振る舞います。位置引数の束縛
（`JSON.stringify(v, 2)` は `indent`、`JSON.parse(s, true)` は
`lines`）、キーワード引数、リテラル `**{...}` と動的 `**variable`
splat の両方。第一級の値として使った場合も束縛は同じです
（`let f = JSON.stringify; f(v, indent: 2)`）。

---

## 10. `Args`

宣言的な CLI 引数パーサ。spec は culebra `Object` で positional / option /
subcommand を列挙し、`Args.parse` は parse 結果を `Object` で返す。
`--help` 指定で help を stdout に出して `Sys.exit(0)`、パースエラー時は
stderr に error 表示 + `Sys.exit(2)`。プログラム制御したい場合は
`Args.try_parse` を使うと `{kind: "ArgParseError", message}` または
`{kind: "ArgParseHelp", help}` を throw する。

### `Args.parse(argv: Array<String>, spec: Object) -> Object`

`argv` (通常 `Sys.argv`) を `spec` に従って parse。`--help` / `-h` で help
を stdout 出力 + `Sys.exit(0)`、パースエラーで stderr 出力 + `Sys.exit(2)`。

### `Args.try_parse(argv, spec) -> Object`

同じエンジンだが exit せず例外を throw。テストや独自 UX 構築用。

### `Args.help(spec: Object) -> String`

parse / exit せずに help 文字列だけ取得。広めのメッセージに埋め込みたい
時など。

### Spec 形式

各引数は次のフィールドを持つ `Object`:

| field | 型 | デフォルト | 意味 |
|---|---|---|---|
| `name` | `String` | (必須) | parse 結果の key |
| `type` | `String` | `"String"` | `"String"` / `"Long"` / `"Float"` / `"Bool"` |
| `short` | `String` | (なし) | 短縮形 (`"v"` → `-v`)。指定すると option 扱い。 |
| `default` | `Any` | (なし) | 省略時の値。指定すると optional。 |
| `doc` | `String` | `""` | help 用説明文 |
| `repeated` | `Bool` | `false` | 複数指定可、`Array` で集約 |

`type: "Bool"` の場合は値を取らない **flag** (`--verbose` / `-v`)。それ以外
の型は次のトークンを値として消費する (`--count 5` / `--count=5`)。

`short` も `default` も無い引数は **positional** 扱い。spec 順にマッチし、
`default` 付きの positional は optional。

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
puts(args.input)
if args.lines { puts("lines: ...") }
if args.words { puts("words: ...") }
puts("encoding: {args.encoding}")
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

`spec.subcommands` は `Array<Object>` で各要素が sub-spec (top-level と
同形)。指定すると最初の positional トークンが subcommand 名として扱われ、
parse 結果の `subcommand` フィールドに名前が入り、残りの引数は selected
subcommand の spec に従って parse される:

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

`Args.parse` はエラー時に exit する。`Args.try_parse` は throw:

```culebra
let r = try { Args.try_parse(["--bogus"], spec) } catch e { e }
# r == {kind: "ArgParseError", message: "unknown option '--bogus'"}
```

throw 値の `kind` は次のいずれか:

| `kind` | 意味 | 付随フィールド |
|---|---|---|
| `ArgParseError` | parse 失敗（不明オプション、型不一致、必須欠落、etc.） | `message` |
| `ArgParseHelp` | `--help` / `-h` 指定 | `help`（help 文字列） |

---

## 11. `Proc`

外部コマンドを同期（blocking）実行し、その出力を取得します。コマンドは
`Array<String>` で、`cmd[0]` が実行ファイル（PATH 解決）、残りが引数です。
シェルを介さないのでクォートやインジェクションの心配がありません
（`["git", "commit", "-m", msg]` は `msg` をそのまま渡します）。

### `Proc.run(cmd: Array<String>, cwd=nil, env=nil, stdin="", check=false, timeout=0, share=nil) -> Object`

`cmd` を完了まで実行し、結果 Object を返します:

| フィールド | 型 | 意味 |
|---|---|---|
| `code` | `Long` | 正常終了時の終了コード。シグナル死した場合は `-1` |
| `stdout` | `String` | コマンドが stdout に書いた全内容（一括取得） |
| `stderr` | `String` | stderr に書いた全内容 |
| `ok` | `Bool` | `code == 0` かつ `signal == nil` のとき `true` |
| `signal` | `String?` | シグナル死した場合のシグナル名（`"SIGTERM"` 等）、それ以外は `nil` |
| `error` | `String?` | 起動失敗時のメッセージ。コマンドが実際に走った場合は常に `nil`。これを設定するのは `Proc.all`（allSettled のエラー表現）のみで、`Proc.run` では起動失敗は throw されるため常に `nil`。 |
| `timed_out` | `Bool` | `timeout` 超過で kill された場合 `true`、それ以外 `false`。 |

キーワード引数:

- `cwd: String` — 子プロセスの作業ディレクトリ（既定: 親を継承）。
- `env: Object` — 環境変数。親の環境にマージされるので `PATH` 等は維持されます
  （既定: そのまま継承）。値は `String` であること。
- `stdin: String` — 子プロセスの標準入力に書き込むバイト列。書き込み後にクローズ
  されます（既定: 空）。
- `check: Bool` — `true` のとき、非 0 終了・シグナル死・timeout で `{ok: false}` を
  返す代わりに `ProcessError` を throw します（既定: `false`）。
- `timeout: Long` — ミリ秒。これを超えて走るとコマンドは kill され（`SIGTERM` →
  短い猶予の後 `SIGKILL`）、結果は `ok: false` / `timed_out: true` になります
  （既定: `0` = 無制限）。**直接の子だけを kill**し、その子が生んだ孫は kill しません
  （Python/Node 既定と同じ）。stdout/stderr を早期に閉じて走り続けるプロセスには
  timeout が届かないことがあります。

**非 0 終了**や**シグナル死**はエラーではなく通常の結果です — `ok` / `code` /
`signal` で分岐してください。**起動失敗**（実行ファイルが存在しない等）や
`check: true` での失敗のみが `ProcessError` を throw します。非 Array・非 String
要素・空コマンドは `TypeError` / `ValueError` を throw します。

```culebra
# doctest: skip
let r = Proc.run(["git", "rev-parse", "--abbrev-ref", "HEAD"])
if r.ok {
  IO.puts("on branch " + r.stdout.trim())
} else {
  IO.print(r.stderr)
}

# 標準入力を渡して変換結果を読む。
let up = Proc.run(["tr", "a-z", "A-Z"], stdin: "hello\n")
assert_eq(up.stdout, "HELLO\n")

# ディレクトリと環境変数を指定し、失敗時に throw。
Proc.run(["make", "install"], cwd: "/src/app", env: {PREFIX: "/usr/local"}, check: true)
```

出力は全量バッファされるため、巨大な出力はそのぶんメモリを使います。stdout と
stderr は並行して読み出すので、両方を埋めるコマンドでもデッドロックしません。

`share: {名前: buf}` は 1 つ以上の `SharedBuffer.shared(...)` buffer を子プロセス
へ渡す（子は `SharedBuffer.receive(name, Class)` で再アタッチする）。子は culebra
プロセスである必要があり、通常は `[Sys.executable, "worker.cul"]`。詳細は
[SharedBuffer › プロセス間での共有](#プロセス間での共有zero-copy)。

### `Proc.all(commands: Array<Array<String>>, limit: Long = <CPU数>, timeout: Long = 0, fail_fast: Bool = false, retries: Long = 0, share: Object? = nil) -> Array<Object>`

複数コマンドを並列実行し、結果 Object を入力順で返します。各コマンドは
`Array<String>`（`Proc.run` の第1引数と同形）。同時実行数は最大 `limit`（既定 =
オンライン CPU 数。絞るには小さい値、広げるには大きい値を渡す）。`timeout`（ms、
`0` = 無し）は各コマンドにその起動時刻から適用され、発火時は結果に
`timed_out: true` を立てます。

既定は **allSettled** です。1個の失敗が他を巻き込みません。走って非 0 終了した
コマンドは `{ok: false, code: N, error: nil}`、そもそも起動できなかった（実行
ファイルが無い等）コマンドは `{ok: false, error: "<メッセージ>"}` で、どちらも
throw しません。空リストは `[]` を返します。

**`fail_fast: true`** の場合は最初の失敗（非 0 終了・シグナル・timeout・起動失敗）
で残りの実行中コマンドを `SIGKILL` し、該当コマンドを示す `ProcessError` を throw
します（既定の `Promise.allSettled` に対する `Promise.all` 形）。全コマンド成功時は
通常どおり結果配列を返します。

**`retries`** は失敗したコマンドをその回数だけ再実行し、最終試行の結果を採用します。
再実行は空きが出た `limit` プールに割り込みます。`fail_fast` と併用した場合、コマンドが
失敗とみなされるのは retries を使い切った後だけです。

**`share: {name: buf}`** は `SharedBuffer.shared(...)` バッファを**全**子に渡す
（各子が `SharedBuffer.receive` で再アタッチ）。`Proc.run` の `share:` と同じで、
ワーカープールが共有結果バッファに書ける（競合するセルは `buf.with_lock`）。

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

全コマンドを起動し、**最初に完了した1個**の結果 Object を返し、残りに `SIGKILL`
を送って reap します。冗長なプロバイダの競争や「最速のミラーが勝ち」に有用。空
リストは `ValueError` を throw します。`share: {name: buf}` は `Proc.run`/`Proc.all`
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
3 つのメソッドを持ちます:

| メソッド | 戻り値 | 意味 |
|---|---|---|
| `h.wait()` | 結果 Object | 子の終了まで待ち、出力を drain する（ブロッキング） |
| `h.poll()` | 結果 Object または `nil` | 終了していれば結果、まだなら `nil`（非ブロッキング） |
| `h.kill(sig = 15)` | `nil` | シグナル送出（既定 `SIGTERM`）。次の `wait`/`poll` が reap |

`wait()` / `poll()` は冪等で、子を reap した後はどちらも同じキャッシュ済み結果
Object（通常の `{code, stdout, stderr, ok, signal, error, timed_out}`）を返します。
起動失敗は `Proc.run` と同様 `ProcessError` を throw します。一度も wait されずに
捨てられたハンドルは GC が reap し（子を `SIGKILL`）、ゾンビとして残りません — ただし
明示的に `wait()` / `kill()` する方が明快です。他の verb と同様、シグナルは直接の子
にのみ送られます（孫には届きません）。

```culebra
# doctest: skip
let server = Proc.spawn(["python", "-m", "http.server", "8000"])
# ... サーバに対して作業 ...
server.kill()                 # SIGTERM
let r = server.wait()
IO.puts("server exited via " + (r.signal ?? to_string(r.code)))

# ブロックせずに完了をポーリング。
let job = Proc.spawn(["make", "-j4"])
while job.poll() == nil {
  IO.print(".")               # ...他の作業...
}
```

`stdin` は spawn 時に一度だけ渡されてクローズされます。逐次 streaming I/O と
パイプライン（`a | b`）は将来追加予定です。

---

## 12. `Isolate`

クロージャを専用 OS スレッド上で（独立した GC ヒープを持たせて）実行し、真の
CPU 並列を得ます。isolate 間で可変メモリは共有されません。値は境界を越える際に
**コピー**されるため、2 つの isolate が同じオブジェクトで競合することは決して
ありません。[§11 `Proc`](#11-proc)（プロセス並列）のスレッド版に当たります。

> `Isolate.spawn`・`Channel`・`Parallel` はいずれもインタプリタと `--jit` の
> 両方で動作します（クロージャは共有コード参照 — インタプリタは AST、JIT は
> コンパイル済み `fn_ptr` — とコピーした捕獲で越境し、子の自前ヒープで実行）。

### `Isolate.spawn(fn, *args) -> handle`

`fn` を別スレッドで実行し、即座にライブハンドルを返します。位置引数 `args` は
`fn` に渡されます。

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
| `h.join()` | クロージャの戻り値 | isolate の完了を待ち、（コピーされた）結果を返す |
| `h.poll()` | 結果または `nil` | 完了済みなら結果、未完了なら `nil`（ノンブロッキング） |

クロージャが例外を投げた場合、その例外は `join()` を呼んだスレッド上で
（kind を保ったまま）再送出されます。`join()` されずに drop されたハンドルは
GC が join するため、スレッドが取り残されることはありません。

### Sendable: 境界を越えられる値

クロージャ・その引数・戻り値は **Sendable** でなければなりません。違反は
`spawn` の時点で `SendError` を投げます（黙ってコピーはしません）:

| Sendable | Sendable でない |
|---|---|
| 数値・`String`・`Bool`・`nil` | ネイティブハンドル（`Proc` / `File` / isolate ハンドル） |
| Sendable 値からなる `Array` / `Object` / `Set` / `Tuple` | `Tensor`（将来はバッファ経由で共有） |
| `enum` / data-class インスタンス | `mut` 変数を捕獲したクロージャ |
| Sendable 値のみを捕獲したクロージャ | 自己参照する値（循環参照） |
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

`mut` の捕獲は黙ってスナップショットを取らず拒否します — 値は引数として
渡してください:

```culebra
# doctest: skip
mut total = 0
Isolate.spawn(|| total)        # SendError: mutable 変数 'total' を捕獲
Isolate.spawn(|t| t, total)    # ok — 値渡し
```

### 並列度の上限

同時に生きている isolate には上限があります（既定はマシンのコア数、環境変数
`CULEBRA_ISOLATE_LIMIT` で上書き可）。上限を超える spawn は新しいスレッドを
起こさず**現在のスレッド上で同期実行**されます — これにより再帰的並列が数千
スレッドに爆発しません。`join()` は同じ結果を返し、変わるのはタイミングだけです。

```culebra
# doctest: skip
# 並列 map: 仕事を isolate に分配して集約。
let parts = [[1, 2, 3], [4, 5, 6], [7, 8, 9]]
mut handles = []
for p in parts { handles.push(Isolate.spawn(|| p.reduce(0, |a, b| a + b))) }
mut total = 0
for h in handles { total = total + h.join() }
total                          # => 45
```

### キャンセル

isolate は協調的にキャンセル可能です。`join()` せずにハンドルを drop する（または
GC が回収する）と isolate に停止が伝わり、次の文境界または channel のブロッキング
境界で巻き戻ります。暴走中・待機中の isolate がプログラムをハングさせることは
ありません。

### Channel — `Channel.new(cap = 1) -> (tx, rx)`

channel は isolate 間で値を渡す有界・ブロッキングのキューです。**(tx, rx)** の組を
返します（Rust `mpsc` 流）。`tx.send(v)` で投入、`rx` で取り出し。channel の
endpoint は Sendable 規則の唯一の例外で、（参照で）共有されます — クロージャが
`tx`/`rx` を捕獲して spawn した isolate に持ち込めます。値そのものはコピーで渡り、
共有されるのは channel だけです。

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
| `tx.send(v)` | tx | `v` を投入（バッファ満杯ならブロック）。closed なら `ChannelError` |
| `tx.clone()` / `rx.clone()` | 両方 | 同一 channel の別 endpoint（multi-producer / multi-consumer） |
| `tx.drop()` / `rx.drop()` | 両方 | この endpoint を解放 |
| `rx.recv()` | rx | 1 値をブロッキング取得。closed かつ空なら `nil` |
| `for v in rx { ... }` | rx | closed まで drain（綺麗な end-of-stream の形） |

**auto-close がデッドロック安全網です。** アクティブな送信端を数え、**最後の `tx`
が drop された**時（producer isolate が正常／例外終了）に channel が close し、
`for v in rx` が終了します。producer がクラッシュしても consumer はハングせず、
原因は producer を `join()` して surface します。multi-producer の罠に注意:
channel が閉じるには全ての `tx`（親の元の tx 含む）が drop される必要があります —
保持しない tx は drop してください。

**`Channel.new(0)` は rendezvous channel**（容量 0）: `send` は受信者が値を
受け取るまで返りません — バッファ無しの同期ハンドオフ。backpressure
（producer が consumer を追い越せない）に有用。単一 isolate 内では deadlock
（渡す相手がいない）ので isolate 間で使います。容量は `0 以上`。

#### `Channel.fan_in(sources: [rx]) -> rx`

複数の receiver を 1 つに束ねます。返る `rx` は、ready な source から順に値を
返し（全 source を同時に待つ — Go の `select` / core.async の `merge` と同じ
イベント駆動、poll ではない）、**全** source が close したら終了します。これに
より各 producer が `tx.clone()` で 1 本を共有する代わりに**自分専用の channel**を
持てるので、multi-producer の罠（clone-drop 忘れで consumer が hang）を回避でき
ます — 各 channel の `tx` は 1 個だけ、1:1 で drop。

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

merge は渡した receiver を**引き取り**ます — 元の rx を直接読まず、束ねた `rx`
経由でのみ読んでください（元を読むと merge と競合）。source 間の順序は保たれ
ません（merge ゆえ）。空リストは即 closed な `rx`、source 1 個はパススルー。

#### `Channel.fan_in(items, fn) -> rx`

オールインワン形：各 item に producer isolate を起動し `fn(item, tx)` を実行、
出力を merge します。`fn` は自分の `tx` に send し、fan_in が channel 作成・親
tx の drop・producer handle の所有をすべて引き受けるので、consumer は **tx も
drop も handle も一切書きません**。`fn` と各 item は Sendable 必須。

```culebra
# doctest: skip
let merged = Channel.fan_in(workers, fn (w, tx) {
  for x in produce(w) { tx.send(x) }
})
for v in merged { consume(v) }
merged.join()        # producer を join、最初のエラーを再送出
```

`merged.join()`（stream 終了後）は producer を join し最初のエラーを再送出。
呼ばなければ producer のエラーは握り潰し（`Isolate.spawn` handle を join しない
のと同じ）。producer は専用スレッドで実行されます。

### Parallel — `Parallel.map` / `each` / `map_settled` / `race`

高レベル形。配列の各要素に関数を isolate プールで並列適用します（ハンドル管理
不要）。`fn` と各要素は Sendable でなければなりません（`Isolate.spawn` と同じ規則）。

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
| `Parallel.map(items, fn, limit = <コア数>)` | `Array` | 要素ごと 1 結果、**入力順**、fail-fast |
| `Parallel.each(items, fn, limit = <コア数>)` | `nil` | 副作用用、結果は集めない、fail-fast |
| `Parallel.map_settled(items, fn, limit = <コア数>)` | `Array` | 要素ごと 1 `Result`、入力順、**fail-fast しない** |
| `Parallel.race(items, fn, limit = <コア数>)` | `Any` | **最初に成功**した要素、残りはキャンセル |

`limit` は同時に走る isolate 数の上限（既定はコア数）。要素は 1 つの共有キューから
取り出すので、要素数ぶんでなく合計 `limit` 個の isolate です。

**`map` / `each` は fail-fast**: 最初に例外を投げた要素で残りを停止し、要素
index 付きの `ParallelError` として再送出します（例: `Parallel.map: element[2]
failed: ...`）。

**`map_settled` は fail-fast しない**: 各要素は `Result` Object
`{ok: Bool, value: Any, error: String?}`（`Proc.all` と同じ形）を返すので、1 つの
失敗で他の成果を失いません（`r.ok ? r.value : r.error`）。

**`race` は最初の成功**を返し、残りの要素をキャンセルします。*全要素*が例外を
投げたら `ParallelError`、空配列でも `ParallelError`（返す結果が無いため）。

**`on_progress:` で進捗報告。** どのメソッドも `on_progress: |done, total|`
callback を受け取れます。`fn` と違い **Sendable ではありません** — 呼び出し
スレッド上で実行されるので、捕獲した状態（カウンタや進捗バー等）を自由に読み
書きできます。要素が完了するたびに「完了数・総数」で呼ばれ、callback が例外を
投げると実行はキャンセルされます。

```culebra
# doctest: skip
Parallel.map(urls, |u| fetch(u),
             on_progress: |done, total| IO.print("\r" + done.to_string() + "/" + total.to_string()))
```

(`map_reduce` は予定。)

### Signal — `Signal.notify` / `Signal.reset`

既定では Ctrl+C は協調的な `Interrupted` を throw します（言語ガイドの
*割り込み* 節を参照）。長時間動くサービスでは逆に、シグナルを一箇所で受けて
自分の都合で shutdown したいことが多いです。`Signal.notify(tx)` は Ctrl+C を
*throw* から *配信* に切り替えます — 各押下が実行中コードを中断する代わりに
チャネルの `tx` に `"SIGINT"` を送るので、プログラムは `rx.recv()`（または
`for sig in rx`）でブロックして自前の graceful shutdown を駆動できます。Go の
`signal.Notify` モデルです。

```culebra
# doctest: skip
let (tx, rx) = Channel.new(1)
Signal.notify(tx)              # Ctrl+C は throw でなくチャネルへ
serve_in_background()
rx.recv()                      # 最初の Ctrl+C までブロック
puts("shutting down…")
drain_and_close()
```

| 呼び出し | 備考 |
|---|---|
| `Signal.notify(tx)` | Ctrl+C をこのチャネルの `tx` へ流す（非ブロッキング送信 — バッファ満杯なら超過分はドロップ、Go と同じ）。有効中は throw を抑制。バッファ付き（`Channel.new(1)`）を使う。 |
| `Signal.reset()` | 既定の `Interrupted` throw 動作に戻し、`notify` が保持していたチャネルを解放。 |

`notify` は自分用の送信端をチャネルに保持するので、こちらの `tx.drop()` 後も
開いたままになります（`reset()` で解放）。notify 有効中は強制終了へのエスカ
レーションはありません（自分でシグナルを扱うと宣言したため。後続の Ctrl+C で
中断させたいなら `reset()` を呼ぶ）。配信はバックグラウンドのポーラ経由なので、
押下は数十ミリ秒以内に観測されます。

### SharedBuffer — zero-copy で共有する固定レイアウトデータ

`SharedBuffer` は固定レイアウトのレコード列を保持し、複数の isolate が
**コピーせず**読み書きできる。isolate モデルで唯一 mutable メモリを共
有する場所であり、レコードを固定スカラフィールド（参照やポインタを含
まない）に限定することで安全性を保つ。

レコード型は `@packable` を付けた通常のクラスで、各フィールドは型注釈
と任意のデフォルトを持つ:

```culebra
# doctest: skip
@packable class Vec2 {
  x: Float32 = 0.0
  y: Float32 = 0.0
}
```

`@packable` はバイトレイアウト（C ABI 自然アライメント）を確定する。
各フィールドは固定スカラ — `Float32`, `Float64`/`Float`, `Int8`,
`Int16`, `Int32`, `Int64`/`Long`, `Byte`, `Bool` — でなければならず、
非スカラフィールドはロード時に `SyntaxError`。デフォルト省略時は型の
ゼロ値（`0` / `0.0` / `false`）。

#### `SharedBuffer.new(count, Class) -> buffer`

`Class` のレイアウトで `count` 個のゼロ初期化レコードを確保する。
`buffer.size`（`.count` / `.len` も同じ）が要素数を返す。バイトはこのプロセス
のヒープに置かれる — isolate（スレッド）間では共有できるが、プロセス間では
共有できない。

#### `SharedBuffer.file(path, count, Class) -> buffer`

同じ buffer を、メモリマップしたファイル（`MAP_SHARED`）で裏打ちする。書き込み
はファイルのページに届く — **永続**（ファイルはプロセスより長生き）で、同じ
`path` をマップした別プロセスからも見える。ハンドルに `flush()`（dirty ページを
`msync` でディスクへ）が付く。ファイルは普通のファイルなので `FS.remove(path)`
で削除する。`path` を RAM 上の場所（Linux なら `/dev/shm/...` など）に向ければ、
ディスク永続なしの共有メモリになる。

```culebra
# doctest: skip
@packable class Cell { v: Int64 = 0 }
let buf = SharedBuffer.file("/tmp/grid.bin", 100, Cell)
buf[0].v = 42
buf.flush()                   # ディスクへ永続化
```

#### `SharedBuffer.shared(count, Class) -> buffer`

同じ buffer を、**匿名の**共有メモリ（名前のない fd — Linux は `memfd`、macOS は
即 unlink した POSIX shm オブジェクト）で裏打ちする。名前を持たず、ディスクにも
触れない。全ハンドルが drop されるとカーネルが解放する。用途は、`Proc.run` /
`Proc.spawn` の `share:` で**子プロセス**へ渡すこと（下記
[プロセス間での共有](#プロセス間での共有zero-copy)）。

#### `buffer[i] -> view`

添字アクセスは要素 `i` の **view** を返す（負の添字は末尾から数える）。
`view.field` の読み書きは backing bytes に直接届く — レコードごとのオブ
ジェクトは生成されない:

```culebra
@packable class Vec2 {
  x: Float32 = 0.0
  y: Float32 = 0.0
}
let buf = SharedBuffer.new(3, Vec2)
puts(buf.size)                # 3
buf[0].x = 1.5                # その場でバイトを書く
let v = buf[0]                # 保持した view は同じ要素を指す
v.y = 2.5
puts([buf[0].x, buf[0].y])    # [1.5, 2.5]
```

要素まるごとの代入（`buf[i] = ...`）は `TypeError` — レコードは単独の値
形を持たないので、フィールドを個別に設定する。未知のフィールドは
`AttributeError`、範囲外の添字は `IndexError`。

#### isolate 間での共有（zero copy）

buffer は isolate 境界を **参照で**越える — 子は同じバイトを読み書きす
る。他のすべての値（境界でコピーされる）と異なり、SharedBuffer は共有
される（channel と同じ例外）。これにより、ワーカーが disjoint な要素を
更新する典型的なデータ並列パターンがアロケーションなしで書ける:

```culebra
# doctest: skip
@packable class Cell { v: Int64 = 0 }
let cells = SharedBuffer.new(8, Cell)

Parallel.each([0, 1, 2, 3, 4, 5, 6, 7], fn (i) { cells[i].v = i * i })

# cells は 0, 1, 4, 9, 16, 25, 36, 49 を保持
```

**disjoint な**要素を書くワーカーは同期不要。同じ要素を複数の isolate
が同時に書くのはデータ競合であり、作業を分割して（上記のように）各要素
の writer を 1 つにするか、共有更新を [`with_lock`](#bufferwith_lockfn-で同期する)
で守るのは呼び出し側の責任。

#### プロセス間での共有（zero copy）

`SharedBuffer.shared(...)` の buffer は、別の isolate だけでなく**子プロセス**へ
も渡せる。親は `Proc.run`（または `Proc.spawn`）の `share:` キーワード（`名前 ->
buffer` の Object）で渡し、子はその名前で `SharedBuffer.receive(name, Class)` に
よって再アタッチする。両プロセスが同じ物理ページをマップする — コピーや
シリアライズなしで書き込みが見える。

```culebra
# doctest: skip
# --- parent.cul ---
@packable class Cell { v: Int64 = 0 }
let grid = SharedBuffer.shared(4, Cell)
grid[0].v = 100
Proc.run([Sys.executable, "worker.cul"], share: {grid: grid})
puts(grid[0].v)               # 子の書き込みをここで読み戻す
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

`receive` は名前と `@packable` 型だけを取る — 要素数は親由来（なので
`grid.count` が一致する）。子は同じ `@packable` クラスを宣言し、`receive` は
レコードサイズの一致を確認して、レイアウト不一致・未知の名前・
`SharedBuffer.shared(...)` でない buffer のときは `ValueError` を投げる（ヒープと
ファイルの buffer はこの方法では渡せない — ファイル buffer は `path` を開き直して
共有する）。`Sys.executable` は実行中の culebra バイナリのパスで、インタプリタの
ワーカーコピーを起動するのに使う。

`Proc.run` は子の終了までブロックするので、戻った時点で子の書き込みは完了して
いる。並行な子は `Proc.spawn` してそれぞれ `wait()` する。isolate と同様、各子が
**disjoint な**要素を持つようにして書き込みが競合しないようにする。

#### `buffer.with_lock(fn)` で同期する

disjoint な書き込みは同期不要。2 つの書き手が本当に**同じ**データに触れざるを
得ないとき — カウンタや、一貫性を保ちたい複数フィールドの更新など — に
`with_lock` が escape hatch になる。callback を buffer のロックを保持したまま
実行し、callback の戻り値を返す。ロックは例外送出を含むあらゆる脱出経路で
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
puts(tally[0].n)              # ちょうど 8000 — lost update なし
```

同じ呼び出しが**プロセス間**でも効く。`.shared` / `.file` の buffer は
プロセス共有ロックを持つので、`share:` で渡された子（あるいは同じファイル
`path` を再オープンした別プロセス）どうしが排他される。callback は短く保つ
こと — すべての holder を直列化する。ロックは**再入不可**で、callback の中から
同じ buffer の `with_lock` を再度呼ぶとデッドロックする。引数が関数でなければ
`TypeError`、drop 済みの buffer は `ValueError`。

`.file` の buffer はこのロック用に先頭へ小さな固定ヘッダを確保するので、その
バイト列は素のレコード配列ではなく culebra のコンテナ形式になる — 外部ツールが
ファイルを直接読む場合は注意。

#### 可変個数フィールド: `FixedArray<T, N>`

`@packable` フィールドには `FixedArray<T, N>` を使える — スカラ `T` を
**容量** `N` 個まで保持する固定容量のインラインコレクションで、**個数**は
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

view は `.size()` / `.capacity()` / `.push(v)` / `.get(i)` / `.set(i, v)` /
`arr[i]`（読み書き）/ `for x in arr` をサポート。容量超過の `push` と範囲外
の添字は `IndexError`。要素型は固定スカラに限る。フィールドまるごとの代入
（`record.field = ...`）は `TypeError` — view 経由で変更する。view はレコード
のバイトをその場で読み書きするので、buffer とともに isolate 間で共有される。

#### テキストフィールド: `FixedString<N>`

`@packable` フィールドには `FixedString<N>` を使える — 最大 `N` バイトの
UTF-8 文字列を保持する固定容量インライン文字列（`[len][byte × N]`、ポインタ
なし）。`FixedArray` と違い、**まるごと `String` 値として**読み書きする
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

`N` は**バイト**容量。`N` バイトを超える文字列は `CapacityError`、String 以外
の代入は `TypeError`。読み出しは格納バイトの新しい `String` コピーを返すので、
buffer とともに isolate 間で共有される（子 isolate の書き込みが親の読み出しに
見える）。

#### ハッシュコレクション: `FixedSet<T, N>` / `FixedMap<K, V, N>`

`@packable` フィールドには `FixedSet<T, N>`（最大 `N` 個のスカラ値）や
`FixedMap<K, V, N>`（最大 `N` 組のスカラ key→value）も使える。どちらも完全
インライン展開（`[count][states][entries]`、ポインタなし）の open-addressing
ハッシュテーブルで、view 経由でその場変更する:

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

容量超過の `add` / `set` は `CapacityError`。キー/値型は固定スカラに限り、等価は
スカラのバイト比較（`FixedSet<Float32>` は `0.0` と `-0.0` を別物とみなす）。
フィールドまるごとの代入は `TypeError` — view 経由で変更する。バイトはレコード内
にあるので、buffer とともに isolate 間で共有される。

#### Optional フィールド: `T?`

`@packable` フィールドには optional スカラ `T?` も使える — 値または `nil` を持つ
スロットで、`[present:byte][T]` のレイアウト。まるごと値として読み書きし、present
バイトが 0 なら `nil`、そうでなければスカラ:

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

`0` は実値で `nil` とは別。packable なのはスカラ optional のみ（`T` は固定スカラ）。
疎構造（id→optional スロットの配列）が主用途で、tagged payload は packable enum と
組み合わせる。

#### タグ付き共用体: `@packable enum`

`@packable` enum は固定のタグ付き共用体 `[tag:i32][payload]`: 各 variant のスカラ
payload が最大 variant に合わせた1領域を共有し、tag がどの variant が live かを選ぶ。
`@packable` クラスのフィールドにその enum 型を使える。component 種別・メッセージ型など、
共有レコード内の判別付き payload に使う:

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

variant payload は全て固定スカラに限る（非スカラ payload は variant 位置で
`SyntaxError`）。その enum のインスタンスでない値の書き込みは `TypeError`。読み出しは
バイトから variant インスタンスを再構築する（enum namespace 不要）ので、ある isolate が
書いた値を別の isolate が共有 buffer 越しに読める。

#### 生バイト: `Bytes<N>`

`@packable` フィールドには `Bytes<N>` も使える — 長さ prefix なしの**ちょうど** `N`
バイトをインライン保持し、まるごと byte `String` として読み書きする。ハッシュ・UUID・
固定バイナリ blob 用:

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

書き込む `String` は**ちょうど** `N` バイトでなければならない（違えば `ValueError`）。
String 以外は `TypeError`。バイトはバイナリ安全（埋め込み NUL も保持）。`FixedString<N>`
（長さ prefix 付きの可変長テキスト）と違い、`Bytes<N>` は固定長 blob。

#### ネストレコード: `@packable` クラスのフィールド

`@packable` クラスのフィールドに別の `@packable` クラスを使える — そのレコードが
インラインで格納され、`outer.inner` がそのバイトへの view を返すので、ネストフィールド
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

ネストするクラスは、それを含むクラスより前に（`@packable` で）宣言する必要がある。
サブレコードまるごとの代入は**同じ**クラスの別レコードのバイトをコピーする（違えば
`TypeError`）。個別フィールドの設定は view 経由（`outer.inner.field = v`）。

### Shared — 参照共有する immutable 値

`Shared.new(value)` は普通の値（オブジェクト・配列・タプル・セット・スカラの
任意のネスト）を**凍結**し、すべての isolate がコピーなしで使える
**読み取り専用 view** を返す。可変長の読み取り専用データ
（トークナイザ辞書、パース済み設定、検索インデックス）のためのレーン:
channel レーンはタスクごとにコピーし、`SharedBuffer` は固定レイアウトを
要求する。凍結ツリーは 1 つ、読み手は何個でも:

```culebra
# doctest: skip
let dict = Shared.new(JSON.parse(FS.read("vocab.json")))

let workers = [0, 1, 2, 3].map(|i| Isolate.spawn(fn () {
  dict["hello"]          # 全 isolate が同じ凍結ツリーを読む
}))
```

読みは普通のコレクションアクセスと同じ — `view.field`・`view[key]`・
`view[i]`・`view.size()`・`view.has(k)`・`view.keys()` / `view.values()`・
`for ... in view`（Object view は `(key, value)` ペア、Array/Tuple/Set
view は要素を返す）。スカラフィールドは読み手の heap に材料化され、
コンテナフィールドは別の共有 view として返る（コピーなし）。ローカルの
作業コピーが要る時は `view.copy()` で普通の可変値へ深い材料化をする。

凍結は isolate へ値を送るのと同じ walk なので、Sendable なものは凍結
できる — 追加の拒否が 2 つ: **関数**（`Shared` の値はデータのみ）と
**native ハンドル**（channel・buffer・別の `Shared` view）は `SendError`。
すべての書き込みは `ImmutableError`。更新は構造上 copy-on-write
（新しい値を作って再度 `Shared.new` し、新 view を配る）。`view.drop()`
は参照を解放（冪等）。ツリー本体はどこかの最後の view が drop した時に
解放され、以降の読みは `ClosedError`。

| | レーン | 共有 | 読み手ごとのコピー |
|---|---|---|---|
| 固定レイアウトレコード | `SharedBuffer` | read **+ write** | なし |
| 可変長・読み取り専用 | `Shared` | read | なし |
| 任意・可変 | channel | コピー | あり |

---

## 13. Matchers

テスト用 / 実行時不変条件チェック用のアサーション matcher。 全 10
個の matcher が `import` 不要のグローバル名としてすべての環境に
bind されています。 失敗時は `{kind: "AssertionError", message: ...}`
形の culebra Object を throw — `try/catch` で捕捉可能。

`assert` キーワード / builtin は存在しません — 用途ごとに専用 matcher
を使います。 production の不変条件には `if`/`throw` を直接書きます:

```culebra
# doctest: skip
if (!cond) {
  throw {kind: "AssertionError", message: "invariant violated"}
}
```

### 真偽 matcher

* **`assert_true(x: Bool) -> Nil`** — `x` が truthy なら pass。 失敗
  時は `assert_true failed:\n  value: {x}`。 `x` は `Bool` / `Long` /
  `Float` のみ — それ以外は `TypeError`。 culebra に Python 流の
  truthiness はありません (空文字列・空配列は falsy ではない)。
* **`assert_false(x: Bool) -> Nil`** — `assert_true` の逆。

### 比較 matcher

各比較 matcher は **同名の演算子と同じ dispatch** を行います —
`assert_eq(a, b)` は `a == b` と等価で、 クラスインスタンスの
`__eq__` / `__lt__` / `__le__` が尊重されます。 失敗 message は
`to_string` で両辺を表示 (ユーザ `__str__` を尊重)。

* **`assert_eq(a, b) -> Nil`** — `a == b`。
* **`assert_ne(a, b) -> Nil`** — `a != b`。
* **`assert_lt(a, b) -> Nil`** — `a < b`。
* **`assert_le(a, b) -> Nil`** — `a <= b`。
* **`assert_gt(a, b) -> Nil`** — `a > b`。
* **`assert_ge(a, b) -> Nil`** — `a >= b`。

```culebra
assert_eq(1 + 1, 2)                                # 成功時は無音

let r = try { assert_eq("foo", "bar"); nil } catch e { e }
puts(r.kind)         # => 'AssertionError'
puts(r.message)
# => |
# 'assert_eq failed:
#   left:  foo
#   right: bar'
```

### `assert_throws(kind: String, f: Function) -> Nil`

0 引数 `f()` を呼び、 `kind` に一致する `kind` を持つエラーが throw
されることを表明。 組み込みエラー (`ZeroDivisionError`, `TypeError`
等) は `e.kind` を持ち、 ユーザ `throw {kind: "X", ...}` も同じく
照合。 `f` の引数数が 0 以外なら `ArityError`。

```culebra
assert_throws("ZeroDivisionError", fn() { let _ = 1 / 0 })
assert_throws("MyError", fn() {
  throw {kind: "MyError", message: "boom"}
})
```

### `assert_close(a: Float, b: Float, tol: Float) -> Nil`

`|a - b| <= tol` なら pass。 `a` / `b` / `tol` のいずれかが NaN なら
**故意に失敗** (素朴な `diff > tol` だと NaN が silently pass する
ため)。 浮動小数比較は `assert_eq` ではなくこちらを使う。

```culebra
assert_close(3.14, 3.1415, 0.01)
```

### 実装ノート

matcher 一族は culebra ソース (cpp ではなく) で定義されており、
lazy module 機構で 3 backend (interp / JIT / AOT) に共通で bind
されます。 matcher 内部の `==` / `<` 等の演算子 dispatch は各
backend が既に実装している演算子 dispatch そのもので、 matcher 専用
の drift 防止ロジックは不要です。

---

## 14. `Regex`

線形時間・grapheme 単位の正規表現（エンジン: vendor 化した [cpp-regexlib](https://github.com/yhirose/cpp-regexlib)）。パターンは
Unicode の **extended grapheme cluster** 単位でマッチし、コードポイント単位では
ありません — `.` は1つのユーザー知覚文字を消費します（`/./` が `🇯🇵` に1要素として
マッチ）。マッチは**線形時間**（Thompson NFA / Pike VM + lazy DFA fast path）で、
catastrophic backtracking が原理的に起きないため backreference はありません。
オフセットは**バイトオフセット**（Go 流）で常に grapheme 境界上です。

`Regex` は **`Regex.compile` で一度コンパイルして再利用**します（コンパイル済み
プログラムが高コスト部分）。以降はメソッドで問い合わせます:

**パターンはシングルクォートの raw 文字列で書きます**（`'\d+'`、`"\\d+"` ではなく）:
シングルクォートはエスケープ処理も `{...}` 補間も行わないので `\d` や `{n}` がそのまま
通ります（Python の `r"..."` と同じ）。アポストロフィを含むパターン（トークナイザの
`'s`/`'t` など）はバッククォート raw 文字列 `` `...` `` を使います（同じく raw で、
`'`・`"`・`{` も含められる）。フラグは `compile` に文字列で渡す
（`Regex.compile('hello', "i")`）か、パターン内にインライン: `(?i)` 大文字小文字
無視、`(?m)` 複数行、`(?s)` dotall。

定数パターンには [`re"..."` リテラル](language.ja.md#regex-リテラル)が
`Regex.compile(...)` の短縮形として使えます — `re'\d+'` や `re"hello"i` は
同じコンパイル済み `Regex` で、本体は常に raw、フラグは閉じクォートの直後。
リテラルは `${expr}` 補間も使えます（String はエスケープ、`Regex` は合成
— 下記 `Regex.interp` 参照）。**全体**を実行時に組み立てるパターンは
`Regex.compile(...)` を直接使ってください。

パターンと対象文字列はどちらの文字列型（`String` / `StringView`）も受け付ける
ので、`String.split` / `.slice` が返す `StringView` をそのまま渡せます:
`Regex.compile('\d+').find_all(line.slice(0, 80))`。

| コンストラクタ / 静的 | 結果 |
| --- | --- |
| `Regex.compile(pat)` | `Regex` — コンパイル（再利用）。不正パターンは送出 |
| `Regex.compile(pat, flags)` | `Regex` — `flags` は `"i"` / `"m"` / `"s"` の文字列 |
| `Regex.escape(s)` | `String` — メタ文字を全てバックスラッシュエスケープし `s` をリテラル一致に |
| `Regex.interp(x)` | `String` — `re"...${x}..."` 用の合成ヘルパ: `Regex` → `(?:src)`、それ以外 → エスケープしてリテラル一致 |

その場限りの利用には、下の namespace メソッドがパターンを直接受け取り `compile` を隠します
（Python `re.search` / `re.sub` と同様）。1 つのパターンを多数の入力に再利用するなら
`Regex.compile(pat)` を使いますが、エンジンがパターンでキャッシュするので one-shot 形に再コンパイルの
コストはありません。フラグはインライン（`(?i)` / `(?m)` / `(?s)`）で。

| one-shot | 等価 |
| --- | --- |
| `Regex.find(pat, s)` | `Regex.compile(pat).find(s)` — `Match` または `nil` |
| `Regex.match(pat, s)` | 先頭アンカーのマッチ |
| `Regex.find_all(pat, s)` | `[Match]` |
| `Regex.test(pat, s)` | `Bool` |
| `Regex.split(pat, s)` | `[String]` |
| `Regex.replace_all(pat, s, repl)` | `String` — テンプレート または `fn (Match) -> String` の repl |

| メソッド | 結果 |
| --- | --- |
| `re.test(s)` | `Bool` — `s` のどこかにマッチするか |
| `re.find(s)` | `Match` または `nil` — 最左マッチ |
| `re.match(s)` | `Match` または `nil` — 先頭 anchored マッチ |
| `re.find_all(s)` | `[Match]` — 全ての非重複マッチ |
| `re.find_all_str(s)` | `[String]` — マッチ文字列のみ（`Match` を作らない。match-dense で約12倍速） |
| `re.find_all_index(s)` | `[Int]` — flat なバイト span `[s0, e0, s1, e1, …]`（位置のみ・確保は配列1個） |
| `re.count(s)` | `Int` — 非重複マッチ数（オブジェクト確保なし） |
| `re.find_iter(s)` | `Iterator<Match>` — 遅延。途中終了可（`.take(n)`） |
| `re.replace_all(s, repl)` | `String` — `repl` はテンプレート（`$1` / `$<name>` / `$$`）**または** `fn (Match) -> String` |
| `re.split(s)` | `[String]` — マッチで `s` を分割 |

**bulk API の選び方。** `find_all` はマッチごとに完全な `Match`
オブジェクト（テキスト・span・`groups`・`named`）を構築する。match-dense
な入力では、マッチングそのものより**このオブジェクト構築が支配的**になり、
エンジンの生スキャンの数十倍のコストになる。マッチごとの capture が不要なら
lean な変種を使う: 個数だけなら `count`、byte span なら `find_all_index`、
マッチ文字列なら `find_all_str`、途中で止めるなら `find_iter`。
`groups`/`named` をマッチごとに実際に使うときだけ `find_all` を使う。

`Match` はデータオブジェクト（`nil` はマッチなし）:

| フィールド | 意味 |
| --- | --- |
| `m.value` | マッチ全体の文字列（`String`） |
| `m.start`, `m.end` | バイトオフセット |
| `m.groups` | `[Group \| nil]`; `groups[0]` はマッチ全体 |
| `m.named` | `{name: Group}` — 名前付きキャプチャ |

添字はキャプチャ専用アクセサです。`m[i]` は位置グループ `i` の文字列（`m[0]` は
マッチ全体、負数は配列同様にラップ）、`m["name"]` は名前付きグループの文字列を返します。
ミス（範囲外・未マッチの省略可能グループ・無い名前）はすべて `nil` なので `?? ""` と
合成できます。添字はキャプチャだけに届きレコードのフィールドには届かないため、マッチ全体は
`m.value` か `m[0]`（`m["value"]` ではない）。span が要る時は dot フィールド
（`m.groups[i].start`）を使います。

`Group` は `.value` / `.start` / `.end` を持ちます。不正なパターンは `RegexError` を送出。

```culebra
let d = Regex.compile('\d+')
d.test("abc 123")                                // => true
Regex.compile('\w+').find("  hello world").value // => "hello"
d.find("no digits")                              // => nil
d.find_all("a1 b22 c333").size()                 // => 3

let m = Regex.compile('(?<year>\d{4})-(\d{2})').find("2026-05")
m[1]                                             // => "2026"（位置キャプチャ）
m["year"]                                        // => "2026"（名前付きキャプチャ）
m[0]                                             // => "2026-05"（マッチ全体）
m[9] ?? "none"                                   // => "none"（ミス -> nil）
m.groups[1].value                                // => "2026"（Group オブジェクト、span 用）
m.named["year"].value                            // => "2026"（(?<name>...) で名前付き）

d.replace_all("a1 b22 c333", "#")                // => "a# b# c#"
Regex.compile('(\w+)@(\w+)').replace_all("x@y", '$2.$1') // => "y.x"
d.replace_all("a1 b22", fn (m) { "<{m.value}>" })// => "a<1> b<22>"（コールバック）
Regex.compile('\s+').split("the quick  brown")   // => ["the", "quick", "brown"]
Regex.compile('hello', "i").test("HELLO world")  // => true（フラグ引数）
d.find("xyz")?.value ?? "none"                   // ?. / ?? と合成可

for m in d.find_iter("a1 b22") { break }         // 遅延。いつでも途中終了可
d.find_iter("1 2 3").take(2).collect().size()    // => 2（全走査しない）
Regex.escape("a.b(c)")                           // => `a\.b\(c\)`（リテラル一致）
```

対応構文（literal / `.` / 文字クラス / `* + ? {n,m}` greedy・lazy / `|` /
キャプチャ・名前付きグループ / `\d \w \s \b` / lookahead / 可変長 lookbehind /
`\p{…}` Unicode プロパティ）とマッチモデル・資源上限は、vendor 化したエンジン
[cpp-regexlib](https://github.com/yhirose/cpp-regexlib)（`vendor/cpp-regexlib`）に
記載しています。

---

## 15. `Http`

同期 HTTP/HTTPS クライアント（エンジン: vendor の `cpp-httplib` + OpenSSL を
静的リンク）。各呼び出しはレスポンスが返るまで **blocking** で、async/await は
ありません。`https://` URL では TLS が自動で有効になり、サーバ証明書の検証には
システムの信頼ストア（macOS は keychain、Linux はプラットフォームの CA バンドル）
を使います。`gzip` / `deflate` のレスポンスは透過的に展開され、`body` は常に
デコード済みの内容です。

各メソッドは **レスポンス Object** を返し、例外を投げるのは *トランスポート* 失敗の
ときだけです:

| フィールド | 型 | 意味 |
|---|---|---|
| `status` | `Long` | HTTP ステータスコード（`200`、`404` …） |
| `ok` | `Bool` | `status` が `[200, 300)` の範囲なら `true` |
| `reason` | `String` | ステータス文言（`"OK"`、`"Not Found"` …） |
| `body` | `String` | レスポンスボディ（生バイト列） |
| `headers` | `Object` | レスポンスヘッダ（名前→値、String→String） |
| `json()` | `Any` | `body` を JSON としてパース（`JSON.parse(r.body)` の糖衣） |

**4xx/5xx は通常の結果**（`ok: false`）でありエラーではありません — `status` /
`ok` で分岐してください。**トランスポート失敗**（DNS・接続拒否・TLS ハンドシェイク・
タイムアウト）は `HttpError` を投げます。スキーム/ホストの無い不正な URL も
`HttpError`、不正な `headers` 値は `TypeError` を投げます。

| メソッド | 結果 |
| --- | --- |
| `Http.get(url, headers=nil, timeout=0, follow_redirects=true)` | レスポンス Object |
| `Http.delete(url, headers=nil, timeout=0, follow_redirects=true)` | レスポンス Object |
| `Http.head(url, headers=nil, timeout=0, follow_redirects=true)` | レスポンス Object |
| `Http.post(url, body="", content_type="text/plain", headers=nil, timeout=0, follow_redirects=true)` | レスポンス Object |
| `Http.put(url, body="", content_type="text/plain", headers=nil, timeout=0, follow_redirects=true)` | レスポンス Object |
| `Http.request(method, url, body="", content_type="text/plain", headers=nil, timeout=0, follow_redirects=true)` | レスポンス Object — 任意のメソッド（PATCH、OPTIONS …） |
| `Http.sse(url, on_event, headers=nil, timeout=0, follow_redirects=true)` | レスポンス Object — Server-Sent Events を `on_event` にストリーム（後述） |

キーワード引数（全メソッド共通）:

- `headers: Object` — リクエストヘッダ。値がすべて `String` の `Object`。
  非 `String` 値は `TypeError`（デフォルト: なし）。
- `params: Object` — クエリ文字列パラメータ。`String` 値の `Object` で、URL に
  percent-encode して付与（`?k=v&…`、URL に既存のクエリがあれば保持）。非 `String`
  値は `TypeError`（デフォルト: なし）。
- `timeout: Long` — connect / read / write の各フェーズのタイムアウト（**秒**）。
  `0` はライブラリのデフォルト（デフォルト: `0`）。
- `follow_redirects: Bool` — `3xx` の `Location` を追跡する（デフォルト: `true`）。
- `into: String | Function` — レスポンスボディをバッファせず sink へストリーム
  する（下記ストリーミング参照。デフォルト: `nil` ＝ `body` にバッファ）。
- `json: Any`（`post` / `put` / `request` のみ）— 値を JSON にシリアライズし、
  `Content-Type: application/json` でボディとして送信。
- `form: Object`（`post` / `put` / `request` のみ）— `String` 値の `Object` を
  `application/x-www-form-urlencoded` ボディとして送信（percent-encode）。`body` /
  `json` / `form` は最大1つ（複数指定は `TypeError`）。
- `body: String | Function` / `content_type: String`（`post` / `put` /
  `request` のみ）— リクエストボディとその `Content-Type`（body が非空で、かつ
  `headers` で明示的な `Content-Type` が指定されていない場合のみ付与）。`String`
  は全体を送信、`Function` は **producer** として chunked ストリーム（下記参照）。

```culebra
# doctest: skip
let r = Http.get("https://api.example.com/users", params: {page: "2"})
if r.ok {
  let users = r.json()                 # レスポンスボディを JSON としてパース
  IO.puts(users.size().to_string())
} else {
  IO.puts("request failed: {r.status}")
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

`get`/`post` 等はボディ全体を単一の `String`（メモリに読み込み）で返すので、
JSON API では [`JSON.parse`](#9-json) と組み合わせます。

**ストリーミング（ダウンロード）— `into:` 引数。** メモリに載らない大きな
レスポンスは、`into:` を渡してバッファせず sink へストリームします。任意のメソッドで
使え、返る `body` は空になります（バイトは sink へ流れる）。`into:` が受ける型:

* **`String`** — ファイルパス。ボディをそのファイルへ直接書き出します。
* **`Function`** — `|chunk|` クロージャ。到着した各 chunk で呼ばれます。コールバックは
  呼び出しスレッド上で実行されるので捕捉した状態を自由に読み書きでき、throw すれば
  転送は中断されエラーが伝播します。

```culebra
# doctest: skip
Http.get("https://example.com/big.tar.gz", into: "big.tar.gz")   # → ファイル

mut bytes = 0
Http.get("https://example.com/big.csv", into: fn (chunk) { bytes = bytes + chunk.size() })

# 任意のメソッド。例: レスポンスがストリームで返る POST:
Http.post("https://example.com/query", body: q, into: fn (chunk) { handle(chunk) })
```

**ストリーミング（アップロード）。** 対称に、`body:`（`post` / `put` /
`request`）へ `Function` を渡すとリクエストボディを chunked でストリームします
（大きなアップロードもメモリに全部載せない）。producer は繰り返し呼ばれ、次の
chunk `String` を返し、`nil` でストリーム終端を示します:

```culebra
# doctest: skip
let f = File.open("big.bin")
Http.post(url, body: fn () {
  let chunk = f.read(65536)
  chunk.size() > 0 ? chunk : nil          # nil で終端
}, content_type: "application/octet-stream")
```

producer は呼び出しスレッド上で実行され（捕捉状態を mutable に扱える）、throw すれば
アップロードは中断されエラーが伝播します。`String`/`nil` 以外を返すと `TypeError`。

### `Http.sse(url, on_event, headers=nil, timeout=0, follow_redirects=true) -> Object`

[Server-Sent Events](https://developer.mozilla.org/docs/Web/API/Server-sent_events)
（`text/event-stream`）ストリームを開きます — 長寿命の `GET` で、イベントが届くたびに
`on_event` コールバックを 1 回ずつ呼びます。ストリーミング LLM/チャット API が使う
ワイヤ形式です。呼び出しはストリームが続く間 blocking で、サーバが閉じた後に最終的な
レスポンス Object を返します。

各イベントは 3 つの String フィールドを持つ Object です:

| フィールド | 意味 |
|---------|---------|
| `event` | `event:` タイプ。サーバが送らない場合は `"message"` |
| `data`  | `data:` ペイロード。複数の `data:` 行は `\n` で連結 |
| `id`    | 最後に見た `id:` フィールド。無ければ `""` |

```culebra
# doctest: skip
Http.sse("https://api.example/v1/stream", fn (e) {
  if e.data == "[DONE]" { return }
  let delta = JSON.parse(e.data)
  IO.print(delta.choices[0].delta.content)
})
```

`Accept` を自分で設定しない限り `Accept: text/event-stream` は自動で送られます。
コメント行（`: ...`）と `retry:` フィールドは無視されます。コールバックは呼び出し
スレッド上で実行され（捕捉状態を mutable に扱える）、return でそのイベントの処理を
終え、throw するとストリームを中断してエラーを伝播します。トランスポート失敗は
`HttpError` です。

**並列・レースリクエスト**は HTTP 専用 API を使わず、汎用の [`Parallel`](#12-isolate)
コンビネータを `Http.get` に適用します — JS の `Promise.all`/`race` や Elixir の
`Task.async_stream` と同じ形です:

```culebra
# doctest: skip
let urls = ["https://api.example/a", "https://api.example/b"]
Parallel.map(urls, |u| Http.get(u).body)        # all、入力順（fail-fast）
Parallel.map_settled(urls, |u| Http.get(u))     # allSettled: [{ok, value, error}, ...]
Parallel.race(urls, |u| Http.get(u))            # 最速成功が勝ち、残りはキャンセル
```

TLS は現在 OpenSSL を静的リンクしていますが、将来 BoringSSL へ切り替えてもビルド設定
のみの変更で、この API には影響しません（BoringSSL はホスト名検証がより厳格なので、
現在通る CN のみの証明書のサーバは拒否される可能性があります）。

---

## 16. `Encoding`

テキストコーデックを**スキームごとのサブ名前空間**にまとめた名前空間
（`Encoding.html`、`Encoding.base64`、`Encoding.hex`、`Encoding.url`）。
コーデックのロジックはインタプリタと JIT/AOT 両バックエンドで共有しており、
いずれもバイナリセーフ（埋め込み NUL バイトも往復で保持）です。

### `Encoding.html`

| 関数 | 結果 |
| --- | --- |
| `Encoding.html.escape(s)` | `String` — HTML で危険な 5 文字（`& < > " '`）をエンティティに置換 |
| `Encoding.html.unescape(s)` | `String` — エンティティ参照を元の文字へ戻す |

`escape` は最初に `&` を置換する（出力を再 escape しても安全）ため、
`&amp;` `&lt;` `&gt;` `&quot;` `&#39;` を出力します。

`unescape` は数値参照 `&#DDD;`（10 進）と `&#xHHH;` / `&#XHHH;`（16 進、大小問わず）に
加え、常用の名前付き参照（typographic / Latin-1 / ギリシャ文字 / 数学記号 / 通貨の
よく使うもの。HTML5 全 ~2200 件ではない）を扱います。参照は `;` で終わる必要があり、
整形式かつ既知でない参照は**そのまま**残します（ブラウザ流の寛容さ）。単独の `&` や
未知のエンティティは素通しされます。

```culebra
puts(Encoding.html.escape("a & b < c"))          # => 'a &amp; b &lt; c'
puts(Encoding.html.escape("it's fine"))          # => 'it&#39;s fine'
puts(Encoding.html.unescape("Tom &amp; Jerry"))  # => 'Tom & Jerry'
puts(Encoding.html.unescape("caf&eacute; &mdash; x")) # => 'café — x'
puts(Encoding.html.unescape("&#65;&#x42;"))      # => 'AB'
puts(Encoding.html.unescape("&#12354;"))         # => 'あ'
puts(Encoding.html.unescape("&unknownent;"))     # => '&unknownent;'
```

### `Encoding.base64`

| 関数 | 結果 |
| --- | --- |
| `Encoding.base64.encode(s)` | `String` — base64（RFC 4648 標準アルファベット、`=` パディング） |
| `Encoding.base64.decode(s)` | `String` — デコード結果。不正入力は `ValueError` |

`encode` はバイナリセーフ（マルチバイト UTF-8 を含む任意のバイト列）。`decode` は
入力中の ASCII 空白（行折り返し base64）と `=` パディングを許容し、アルファベット外の
文字は `ValueError`。

```culebra
puts(Encoding.base64.encode("user:pass"))   # => 'dXNlcjpwYXNz'
puts(Encoding.base64.decode("dXNlcjpwYXNz")) # => 'user:pass'
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
| `Encoding.hex.encode(s)` | `String` — 小文字 16 進、1 バイトにつき 2 桁 |
| `Encoding.hex.decode(s)` | `String` — デコード結果。不正入力は `ValueError` |

`encode` は常に小文字で出力します。`decode` は大小いずれの桁も受け付け、奇数長や
16 進以外の文字は `ValueError`。

```culebra
puts(Encoding.hex.encode("abc"))   # => '616263'
puts(Encoding.hex.decode("616263")) # => 'abc'
puts(Encoding.hex.decode("00FF").size()) # => 2
```

### `Encoding.url`

| 関数 | 結果 |
| --- | --- |
| `Encoding.url.encode(s)` | `String` — パーセントエンコード（RFC 3986） |
| `Encoding.url.decode(s)` | `String` — パーセントエスケープをデコード |

`encode` は非予約集合 `A-Z a-z 0-9 - _ . ~` をそのまま残し、それ以外のバイトを
大文字 16 進の `%XX` にします（空白は `+` ではなく `%20`、マルチバイト UTF-8 は
バイト単位でエンコード）。`decode` は寛容で、2 桁の 16 進が続かない `%` は**そのまま**
残し、リテラルの `+` も `+` のまま（`encode`/`decode` がちょうど往復します）。

```culebra
puts(Encoding.url.encode("a b&c"))   # => 'a%20b%26c'
puts(Encoding.url.decode("a%20b%26c")) # => 'a b&c'
puts(Encoding.url.encode("café"))    # => 'caf%C3%A9'
```

---

## 17. `Compress`

zlib を用いた gzip 圧縮・展開。どちらの関数もバイナリセーフ（埋め込み NUL も
往復で保持）で、標準の `gzip` ツールと相互運用できます。

| 関数 | 結果 |
| --- | --- |
| `Compress.gzip(data: String) -> String` | gzip 圧縮したバイト列（RFC 1952 ラッパー） |
| `Compress.gunzip(data: String) -> String` | 展開したバイト列。不正な入力は `ValueError` |

`gunzip` はヘッダを自動判別するので、gzip と zlib（`deflate`）の両方を展開します。
切り詰められた入力や gzip でない入力は `ValueError`。

```culebra
let original = "the quick brown fox the quick brown fox the quick brown fox the quick brown fox"
let z = Compress.gzip(original)
puts(z.size() < original.size())          # => true
puts(Compress.gunzip(z) == original)      # => true
```

```culebra
# doctest: skip
# .gz ファイルを読み書き
let text = Compress.gunzip(FS.read("logs.gz"))
FS.write("out.gz", Compress.gzip(text))
```

HTTP レスポンスは `Http` クライアントが透過的に展開します（`Accept-Encoding` を
送り、`Content-Encoding: gzip` を自動で展開）。したがって `Compress` は自分で扱う
データやファイル向けで、`Http` のボディには不要です。

---

## 18. `Hash`

メッセージダイジェストと HMAC。自前実装（OpenSSL 非依存）で全バックエンド一致。
各関数は**小文字 hex** のダイジェストを返します。入力はバイナリセーフ（埋め込み
NUL も終端でなくメッセージの一部）。

| 関数 | 結果 |
| --- | --- |
| `Hash.sha256(data: String) -> String` | 64 文字 hex の SHA-256 ダイジェスト |
| `Hash.sha1(data: String) -> String` | 40 文字 hex の SHA-1 ダイジェスト |
| `Hash.sha512(data: String) -> String` | 128 文字 hex の SHA-512 ダイジェスト |
| `Hash.md5(data: String) -> String` | 32 文字 hex の MD5 ダイジェスト |
| `Hash.hmac_sha256(key: String, data: String) -> String` | 64 文字 hex の HMAC-SHA-256 |
| `Hash.hmac_sha1(key: String, data: String) -> String` | 40 文字 hex の HMAC-SHA-1 |
| `Hash.hmac_sha512(key: String, data: String) -> String` | 128 文字 hex の HMAC-SHA-512 |

```culebra
puts(Hash.sha256("abc"))
# => 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'
puts(Hash.md5("abc"))
# => '900150983cd24fb0d6963f7d28e17f72'
puts(Hash.hmac_sha256("Jefe", "what do ya want for nothing?"))
# => '5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843'
```

生（hex でない）ダイジェストが必要なら `Encoding.hex.decode` で復号、別形式に
したいなら `Encoding.base64` と組み合わせます。MD5 と SHA-1 は既存システム
（チェックサム、レガシー API）との互換のために提供しています — 新規の
セキュリティ用途には SHA-256 / SHA-512 を使ってください。

---

## 19. `CSV`

カンマ区切り値（RFC 4180 流）の parse / 生成。全 backend で byte 一致。
フィールドはカンマ、レコードは改行で区切られ、カンマ・ダブルクォート・改行を
含むフィールドはダブルクォートで囲み、内部のクォートは `""` に二重化する。

| 関数 | 結果 |
| --- | --- |
| `CSV.parse(text: String, delimiter: String = ",") -> Array<Array<String>>` | String フィールドの行 |
| `CSV.stringify(rows: Array, delimiter: String = ",") -> String` | CSV テキスト。各行は Array、各フィールドは `to_string` 同様にレンダリング |

`delimiter:` オプション（1 バイト。先頭バイトを使用）でフィールド区切りを選べる
— TSV なら `"\t"`。位置引数でもキーワードでも渡せる。

`parse` は寛容（エラーなし）: 全フィールドは `String` で返り（数値推論はしない）、
空入力は 0 行、末尾改行は空行を足さない。LF と CRLF の両方がレコード区切り。
`stringify` は各フィールドを `to_string` と同じ変換でレンダリングするので数値や
`Bool` も自然に出力され、`stringify(parse(text))` は整形式入力を round-trip する。

```culebra
let rows = CSV.parse("name,age\nalice,30\nbob,25")
puts(rows[1])                                         # => ['alice', '30']
puts(CSV.stringify([["a,b", "c"], [1, 2]]) == "\"a,b\",c\n1,2")   # => true
puts(CSV.parse("a\tb", delimiter: "\t")[0])           # => ['a', 'b']
```

数値列は parse 後に `to_long` / `to_float` で map して変換する。ヘッダ行があれば
単に `rows[0]` で、専用のヘッダモードは無い。

---

## 20. `UUID`

正準の小文字 UUID（`8-4-4-4-12` ハイフン形式）を生成。2 種類:

| 関数 | 結果 |
| --- | --- |
| `UUID.v4() -> String` | ランダム UUID（122 ランダムビット） |
| `UUID.v7() -> String` | 時刻順 UUID — 48-bit Unix-ミリ秒プレフィックス + ランダム。作成時刻でソートできる（DB キー向き） |

エントロピーは `Random.*` が使う共有 PRNG 由来なので `Random.seed` で再現可能、
かつ**暗号学的に安全ではない**（識別子向けで、トークンや秘密には不可）。v7 は
ミリ秒単位の順序で、同一ミリ秒内に生成された 2 値は相互に順序づけられない
（monotonic counter は持たない）。

```culebra
puts(UUID.v4().size())          # => 36
puts(UUID.v4() != UUID.v4())    # => true
```

---

## 21. `Term`

テキスト UI（TUI）を作るための端末制御 — 色・カーソル位置・代替画面・
端末サイズ・非ブロッキングなキー入力。色とエスケープのヘルパーは文字列を
返す純関数なので合成・テストが容易で、状態を持つ部分（raw モード・描画
ループ）は終了時に端末を必ず復帰するようラップされています。

### 色と属性

各関数は引数を対応する ANSI コード（＋リセット）で包んで返すので、入れ子に
できます:

| 関数 | 結果 |
| --- | --- |
| `Term.fg(s, n) -> String` | 256 色前景（`n` は 0–255） |
| `Term.bg(s, n) -> String` | 256 色背景 |
| `Term.rgb(s, r, g, b) -> String` | 24bit トゥルーカラー前景 |
| `Term.red(s)` / `green` / `yellow` / `blue` / `magenta` / `cyan` / `white` / `black` | 名前付き 16 色前景 |
| `Term.bold(s)` / `Term.dim(s)` / `Term.underline(s)` / `Term.reverse(s)` | 文字属性 |
| `Term.style(fg:, bg:, bold:, dim:, underline:, reverse:) -> String` | `Screen` セル用の SGR パラメータ文字列。`fg`/`bg` は 256 色インデックスか `(r,g,b)` タプル |

色は端末の**ケイパビリティレベル**（`0` なし / `1` 16 / `2` 256 / `3`
トゥルーカラー）に適応します。レベルは `isatty`・`NO_COLOR`（あれば無効）・
`FORCE_COLOR`・`COLORTERM`・`TERM` から検出。レベルを超える色は
ダウンサンプル（トゥルーカラー → 最近傍 256 → 最近傍 16）され、レベル 0 では
何も出さない（パイプ / `NO_COLOR` 時はプレーン）。`Term.level()` で取得、
`Term.set_level(n)` で上書きできます。`fg`/`bg`/`rgb`/`bold`/… は直接表示用に
文字列を包み、`Term.style(...)` は色付きセル用に `screen.set` / `screen.put`
へ渡すスタイルを返します。

```culebra
puts(Term.bold(Term.fg("alert", 196)))          # 太字・明るい赤の "alert"（表示用）
let st = Term.style(fg: (255, 128, 0), bold: true)   # Screen セル用
```

### エスケープ・サイズ・幅

| 関数 | 結果 |
| --- | --- |
| `Term.clear() -> String` | 画面クリア＋カーソルを原点へ |
| `Term.move(x, y) -> String` | カーソルを列 `x`・行 `y` へ（0 始まり） |
| `Term.hide()` / `Term.show()` | カーソルの非表示 / 表示 |
| `Term.cols()` / `Term.rows() -> Long` | 端末サイズ（tty でなければ 80×24） |
| `Term.size() -> (Long, Long)` | `(cols, rows)` |
| `Term.width(s) -> Long` | 表示幅（全角 / 絵文字 = 2、結合 = 0） |
| `Term.flush()` | バッファ済み出力をフラッシュ |

### 入力イベント

入力は単一のイベントモデル: `Screen.poll(timeout)`（と `Term.parse(raw)`）は
1 つのイベント **Object**、または入力なしで `nil` を返します。`kind` で種別を
判別し、修飾子は bool です。

| `kind` | フィールド |
| --- | --- |
| `"key"` | `key`・`ctrl`・`shift`・`alt` |
| `"mouse"` | `event`・`button`・`x`・`y`・`ctrl`・`shift`・`alt` |
| `"resize"` | `cols`・`rows` |

**キー**の `key` は印字可能文字（`"q"`・`" "`）か名前:
`"up"` / `"down"` / `"left"` / `"right"`・`"enter"`・`"escape"`・`"tab"`・
`"backspace"`・`"insert"`・`"delete"`・`"home"`・`"end"`・`"pageup"`・
`"pagedown"`・`"f1"`…`"f12"`。修飾子は `ctrl` / `shift` / `alt`
（例 Ctrl+Right → `{key: "right", ctrl: true}`、Ctrl+C →
`{key: "c", ctrl: true}`、Alt+x → `{key: "x", alt: true}`）。

**マウス**は `event` が `"press"` / `"release"` / `"drag"` / `"scroll"`、
`button` が `"left"` / `"middle"` / `"right"` / `"wheel_up"` / `"wheel_down"`、
`x` / `y` は 0 始まりのセル。

`Term.resized() -> Bool` は低レベルのリサイズフラグ（SIGWINCH 後一度 true）、
`poll` はそれを `"resize"` イベントにします。

マウスはオプトイン: `Term.app(..., mouse: true)`（または `Term.mouse_on()` /
`Term.mouse_off()` を自分で print）で有効化。

```culebra
# doctest: skip
let ev = screen.poll(0.1)
if ev != nil {
  if ev.kind == "key" && ev.key == "q" { ... }
  else if ev.kind == "mouse" && ev.event == "press" { ... ev.x, ev.y ... }
}
```

### `Term.app` と `Screen`

`Term.app(fn (screen) { ... }, mouse: false)` は raw モードと代替画面に入り、
カーソルを隠し、リサイズを監視し（`mouse: true` でマウス報告も有効化）、
**終了時に端末を復帰**します（正常終了・例外・Ctrl+C いずれも）—
`defer` による保証です。コールバックは `Screen` を
受け取ります:

| メソッド | 効果 |
| --- | --- |
| `screen.size()` / `cols()` / `rows()` | 端末の寸法 |
| `screen.clear()` | バックバッファを空フレーム（現在サイズ）にリセット |
| `screen.set(x, y, glyph, style = "")` | 1 グラフェムを（任意の `Term.style` 付きで）バックバッファに置く |
| `screen.put(x, y, s, style = "")` | `s` のグラフェムを `style` で連続セルに配置 |
| `screen.render() -> String` | 前フレームから画面を更新する最小エスケープ（フロントバッファも前進） |
| `screen.flush()` | `render()` を出力してフラッシュ |
| `screen.poll(timeout) -> Object?` | 最大 `timeout` 秒、入力イベント（キー / マウス / リサイズ）を待つ。無ければ `nil` |

`Screen` はセル（グリフ + 任意のスタイル）のダブルバッファです。`flush` は
前フレームから**変化したセルだけ**を、スタイル間の最小 SGR 遷移付きで出力
するので、ライブ UI がちらつかず最小出力で更新されます（全角グリフは 2
セル、リサイズは全再描画）。`clear` + `set` / `put`（色は `Term.style(...)`
を渡す）でフレームを組み立て `flush`、入力は `poll`（フレームごとのウェイト
も兼ねる）で読みます。

```culebra
Term.app(fn (s) {
  s.clear()
  s.put(2, 1, "hello")
  s.flush()
  s.poll(2.0)            # 最大 2 秒キー入力を待つ
})
```

完全なプログラムは `examples/donut.cul`（入力なしの描画ループ）と
`examples/froggy.cul`（キー操作のゲーム）を参照。

---

## 22. 設計上の注記

### 名前空間ファースト、グローバルは CLI のエイリアス

ライブラリ自体は matcher 一族以外の**グローバル名を追加しません**。
それ以外の関数は `Math`, `IO`, `Random`, `Sys` のいずれかに属します。
これにより `culebra::environment()` はホストアプリケーションに埋め込
むスクリプトエンジンとして、意図しないグローバルを持ち込まない形に
なります。

ただし CLI スクリプトで `puts` / `print` は頻出するため、毎回
`IO.puts` と書くのは摩擦が大きい。CLI バイナリ（`src/main.cc`）
は環境構築直後にこれらをグローバルとしてインストールします。
指す関数値は `IO` 配下と同一なので重複はありません。V8 が同様の
アプローチを採っており、エンジン自体は `print` を提供せず、`d8`
シェルが後付けで導入しています。

### 名前空間はファーストクラス値

すべての stdlib 名前空間（`Math`, `IO`, `FS`, `Random`, `Sys`,
`Tensor`, `JSON`）は `Object` です。変数に束縛したり、関数引数
として渡したり、コレクションに格納でき、そのバインディング経由
のメソッド呼出は直接呼出と同じ意味論を保ちます:

```culebra
let io = IO
io.puts("hello")              # IO.puts("hello") と同じ

fn run_with(ns, x) { ns.puts(x) }
run_with(IO, "via parameter")
```

両 backend がこれを保証します。JIT/AOT のスローパスは runtime
ディスパッチャ（`stdlib_jit.h::kNsMethods`）を経由し、構文的
ファストパス（`IO.puts(x)` 直接呼出）は従来の inline IR 生成を
保ちます。

### 自由関数 vs メソッド

自由関数（名前空間内）は、無から値を構築する場合（`iota`,
`IO.input`）、複数の型に等しく適用される場合（`type_of`,
`to_string`）に使います。特定の型に関する操作はメソッド構文を
用いますが、その設計方針は言語仕様 §17（String/Array/Object
メソッド）を参照してください。

### エラー送出 vs `nil` 戻り値

回復不能な型エラー（`to_long('abc')`、存在しないファイルへの
`FS.read(...)` など）は例外送出を優先し、「見つかるかどうか」の
述語はセンチネルを返す方針です（`IO.input()` は EOF で `''`）。
`try`/`catch` なしでホットパスを簡潔に保つためです。

---

## 23. 未収録（将来検討）

### 重量級データ構造

`Queue` / `Deque` / 優先度ヒープはありません。`Set` と `Tuple` は
言語組込みです（[`docs/language.ja.md`](language.ja.md) 参照）。それ以外は
`Array` と `Object` で代用してください。

### ネットワーク / OS 拡張

生 TCP/UDP ソケット・DNS リゾルバ・SQLite・ファイル監視はありません。
必要なら [§11 Proc](#11-proc) でサブプロセスに委譲してください。

---

関連: 言語仕様は [`docs/language.ja.md`](language.ja.md) にあります。
