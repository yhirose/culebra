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
`assert_throws` 等) は [§10 Matchers](#10-matchers) で扱います。
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
2. [`IO`](#2-io) — 出力・標準入力・ファイル I/O
3. [`FS`](#3-fs) — パス操作・ファイル/ディレクトリ問い合わせ・更新
4. [`Time`](#4-time) — `Instant` / `Duration` クラス、ISO 8601、カレンダー算術、ナノ秒精度
5. [`Random`](#5-random) — シード可能な PRNG（uniform / gauss / shuffle / weighted_choice）
6. [`Sys`](#6-sys) — argv / exit / env
7. [`Tensor`](#7-tensor) — N 次元数値テンソル、BLAS 対応 lazy graph
8. [`JSON`](#8-json) — stringify / parse の相互変換
9. [`Args`](#9-args) — 宣言的な CLI 引数パーサ (positional / option / subcommand / `--help`)
10. [Matchers](#10-matchers) — `assert_true` / `assert_eq` / `assert_throws` / `assert_close` 一族
11. [`Regex`](#11-regex) — 線形時間・grapheme 単位の正規表現
12. [設計上の注記](#12-設計上の注記)
13. [未収録（将来検討）](#13-未収録将来検討)

**目的別索引**

| やりたいこと | 参照先 |
|---|---|
| 定数（π、e、inf、nan） | [§1 Math 定数](#math-pi) |
| スカラー演算（abs / min / max / log / exp / sqrt / floor / ceil / round） | [§1 Math](#1-math) |
| 標準出力 | `IO.puts`（改行 + クォート付き） / `IO.print`（生） |
| ファイル読込 | `IO.read`（失敗時 throw） |
| パス操作（join / basename / dirname / stem / extension） | [§3 FS](#3-fs) |
| ディレクトリ列挙・作成・削除 | `FS.list_dir`、`FS.mkdir`、`FS.remove` |
| `Instant` / `Duration` クラス、ISO 8601、カレンダー算術 | [§4 Time](#4-time) |
| 乱数 | `Random.int`、`.uniform`、`.gauss`、`.shuffle`、`.weighted_choice` |
| CLI 引数解析 | [§9 Args](#9-args) |
| プロセス情報 | `Sys.argv`、`Sys.exit`、`Sys.env` |
| 行列・テンソル演算（BLAS 対応） | [§6 Tensor](#6-tensor) |
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
`clamp`）。整数列ファクトリ `range` / `iota` は言語コアグローバルで、
[言語仕様 §18](language.ja.md#18-コア組み込み関数) を参照。

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

出力、標準入力、ファイル入出力。

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

### `IO.read(path: String) -> String`

`path` のファイル全体を `String` として読み込みます。

**エラー**（実行時 `type error` として送出。`try`/`catch` でユーザ
捕捉はできない）: ファイルが存在しない、読込権限がない、ディレクトリ
を指している。ファイル不在が例外的でない場合は事前に
`IO.exists(path)` でチェックしてください。

```culebra
# doctest: skip
contents = IO.read('data.txt')
```

### `IO.write(path: String, content: String) -> Nil`

`content` を `path` のファイルに書き込みます（作成または上書き）。

**エラー**（実行時 `type error`）: 親ディレクトリが存在しない、
書込権限がない、書込が失敗した（ディスクフル等）。既存ファイルは
警告なしで上書きします。

```culebra
# doctest: skip
IO.write('out.txt', 'hello\n')
```

### `IO.exists(path: String) -> Bool`

`path` にエントリ（ファイル／ディレクトリ／シンボリックリンクを
区別しない）があるかを返します。空文字列や不正なパスは `false`。
`try`/`catch` 無しで「取得前に有無を確認」パターンに使えます。

```culebra
# doctest: skip
if !IO.exists('data.txt') {
  IO.write('data.txt', 'hello')
}
```

`FS.exists` は同じ判定を `FS` 名前空間下で提供するもので、新規
コードではそちらを推奨します。`IO.exists` は後方互換用に残して
あります。

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

### 問い合わせ

#### `FS.exists(path: String) -> Bool`

`path` に何かが存在するか。ファイル／ディレクトリ／シンボリック
リンクの区別なし。`IO.exists` と同じセマンティクス。

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

#### `FS.remove(path: String) -> Nil`

ファイル、または空ディレクトリを削除。対象が存在しない／削除
できない／非空ディレクトリの場合は `IOError` を throw。再帰削除
は提供しません — 必要なら明示的に列挙して削除してください。

---

## 4. `Time`

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

## 5. `Random`

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

## 6. `Sys`

プロセスレベルの情報。

### `Sys.argv -> Array`

スクリプトにコマンドラインで渡された `String` 引数の配列。単独の
`--` より後ろが取り込まれ、`culebra` 実行ファイルとスクリプトパス
自体は含みません。`--` ブロックが無い場合や REPL 実行時は空配列
です。

```culebra
# $ culebra run.cul -- hello world
puts(Sys.argv)        # ['hello', 'world']
```

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

---

## 7. `Tensor`

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

### 活性化関数（名前空間関数）

メソッド形式（`.relu()` 等）にしないのは、ユーザのクラスメソッドで
`relu` を定義する慣習（microgpt の `Value.relu()` など）と衝突する
ため。

```culebra
# doctest: skip
let h = Tensor.sigmoid(z)        # 1/(1+exp(-z)) elementwise
let r = Tensor.relu(x)           # max(0, x)
let p = Tensor.softmax(logits)   # 最終軸で online stable
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

## 8. `JSON`

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

JIT メモ: ビルトインの `JSON.{stringify, parse}` は両バックエンドで
kwargs を直接受け付け、リテラル `**{key: val, ...}` splat と動的
`**variable` splat の両方が動作します。リテラル splat は
コンパイル時にフラット化されオーバーヘッドゼロ、動的 splat は
built-in 専用 runtime adapter が Object のキーを実行時に列挙
（インタプリタと同じアルゴリズム）。

---

## 9. `Args`

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

## 10. Matchers

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

## 11. `Regex`

線形時間・grapheme 単位の正規表現（エンジン: `include/regexlib.h`）。パターンは
Unicode の **extended grapheme cluster** 単位でマッチし、コードポイント単位では
ありません — `.` は1つのユーザー知覚文字を消費します（`/./` が `🇯🇵` に1要素として
マッチ）。マッチは**線形時間**（Thompson NFA / Pike VM + lazy DFA fast path）で、
catastrophic backtracking が原理的に起きないため backreference はありません。
オフセットは**バイトオフセット**（Go 流）で常に grapheme 境界上です。

`Regex` は **`Regex.compile` で一度コンパイルして再利用**します（コンパイル済み
プログラムが高コスト部分）。以降はメソッドで問い合わせます:

**パターンはシングルクォートの raw 文字列で書きます**（`'\d+'`、`"\\d+"` ではなく）:
シングルクォートはエスケープ処理も `{...}` 補間も行わないので `\d` や `{n}` がそのまま
通ります（Python の `r"..."` と同じ）。フラグは `compile` に文字列で渡す
（`Regex.compile('hello', "i")`）か、パターン内にインライン: `(?i)` 大文字小文字
無視、`(?m)` 複数行、`(?s)` dotall。

| コンストラクタ | 結果 |
| --- | --- |
| `Regex.compile(pat)` | `Regex` — コンパイル（再利用）。不正パターンは送出 |
| `Regex.compile(pat, flags)` | `Regex` — `flags` は `"i"` / `"m"` / `"s"` の文字列 |

| メソッド | 結果 |
| --- | --- |
| `re.test(s)` | `Bool` — `s` のどこかにマッチするか |
| `re.find(s)` | `Match` または `nil` — 最左マッチ |
| `re.match(s)` | `Match` または `nil` — 先頭 anchored マッチ |
| `re.find_all(s)` | `[Match]` — 全ての非重複マッチ |
| `re.replace_all(s, repl)` | `String` — `repl` 内で `$1` / `$<name>` / `$$` |
| `re.split(s)` | `[String]` — マッチで `s` を分割 |

`Match` はデータオブジェクト（`nil` はマッチなし）:

| フィールド | 意味 |
| --- | --- |
| `m.value` | マッチ全体の文字列（`String`） |
| `m.start`, `m.end` | バイトオフセット |
| `m.groups` | `[Group \| nil]`; `groups[0]` はマッチ全体 |
| `m.named` | `{name: Group}` — 名前付きキャプチャ |

`Group` は `.value` / `.start` / `.end` を持ちます。不正なパターンは `RegexError` を送出。

```culebra
let d = Regex.compile('\d+')
d.test("abc 123")                                // => true
Regex.compile('\w+').find("  hello world").value // => "hello"
d.find("no digits")                              // => nil
d.find_all("a1 b22 c333").size()                 // => 3

let m = Regex.compile('(\d{4})-(\d{2})').find("2026-05")
m.groups[1].value                                // => "2026"

d.replace_all("a1 b22 c333", "#")                // => "a# b# c#"
Regex.compile('\s+').split("the quick  brown")   // => ["the", "quick", "brown"]
Regex.compile('hello', "i").test("HELLO world")  // => true（フラグ引数）
d.find("xyz")?.value ?? "none"                   // ?. / ?? と合成可
```

対応構文（literal / `.` / 文字クラス / `* + ? {n,m}` greedy・lazy / `|` /
キャプチャ・名前付きグループ / `\d \w \s \b` / lookahead / 可変長 lookbehind /
`\p{…}` Unicode プロパティ）とマッチモデル・資源上限は `docs/regexlib.md`（および
`docs/regexlib.ja.md`）に記載しています。

---

## 12. 設計上の注記

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
`IO.read(...)` など）は例外送出を優先し、「見つかるかどうか」の
述語はセンチネルを返す方針です（`IO.input()` は EOF で `''`）。
`try`/`catch` なしでホットパスを簡潔に保つためです。

---

## 13. 未収録（将来検討）

### 三角関数

`Math.sin` / `cos` / `tan` / `atan2` は未実装です。乱数生成と主要な
超越関数（`log`, `exp`, `sqrt`）は揃っているので、三角関数は具体的
なユースケースが出てきた時点で追加します。

### 日時

将来対応。必要なら `IO.read` / `IO.write` 経由でヘルパープロセスを
呼ぶ形で代用できます。

### `Array`/`Object` 以外のコレクション

`Set`, `Queue`, `Tuple` などはありません。当面は `Array` と
`Object` で代用してください。

---

関連: 言語仕様は [`docs/language.ja.md`](language.ja.md) にあります。
