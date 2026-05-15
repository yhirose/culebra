# Culebra 標準ライブラリ

本書は Culebra の**組み込みライブラリ**を規定します。ランタイム
ユーティリティをまとめた名前空間オブジェクト（`Math`, `IO`, `Sys`）
を対象とします。ここに記載のものは `import` 文なしで利用できます。

言語レベルの組み込み関数（`assert`, `to_long`, `to_float`,
`to_string`, `type_of`, `range`, `iota`）は [言語仕様 §18](language.ja.md)
を参照してください。組み込み型（`String`, `Array`, `Object`）の
メソッドは [言語仕様 §17](language.ja.md) に規定されています。

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
3. [`Random`](#3-random) — シード可能な PRNG（uniform / gauss / shuffle / weighted_choice）
4. [`Sys`](#4-sys) — argv / exit / env
5. [`Tensor`](#5-tensor) — N 次元数値テンソル、BLAS 対応 lazy graph
6. [`JSON`](#6-json) — stringify / parse の相互変換
7. [設計上の注記](#7-設計上の注記)
8. [未収録（将来検討）](#8-未収録将来検討)

**目的別索引**

| やりたいこと | 参照先 |
|---|---|
| 定数（π、e、inf、nan） | [§1 Math 定数](#math-pi) |
| スカラー演算（abs / min / max / log / exp / sqrt / floor / ceil / round） | [§1 Math](#1-math) |
| 標準出力 | `IO.puts`（改行 + クォート付き） / `IO.print`（生） |
| ファイル読込 | `IO.read`（失敗時 throw） |
| 乱数 | `Random.int`、`.uniform`、`.gauss`、`.shuffle`、`.weighted_choice` |
| プロセス情報 | `Sys.argv`、`Sys.exit`、`Sys.env` |
| 行列・テンソル演算（BLAS 対応） | [§5 Tensor](#5-tensor) |
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
contents = IO.read('data.txt')
```

### `IO.write(path: String, content: String) -> Nil`

`content` を `path` のファイルに書き込みます（作成または上書き）。

**エラー**（実行時 `type error`）: 親ディレクトリが存在しない、
書込権限がない、書込が失敗した（ディスクフル等）。既存ファイルは
警告なしで上書きします。

```culebra
IO.write('out.txt', 'hello\n')
```

### `IO.exists(path: String) -> Bool`

`path` にエントリ（ファイル／ディレクトリ／シンボリックリンクを
区別しない）があるかを返します。空文字列や不正なパスは `false`。
`try`/`catch` 無しで「取得前に有無を確認」パターンに使えます。

```culebra
if !IO.exists('data.txt') {
  IO.write('data.txt', 'hello')
}
```

---

## 3. `Random`

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

## 4. `Sys`

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

## 5. `Tensor`

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
let W1 = Tensor.from_csv("W1.csv")    # [30, 784]
let b1 = Tensor.from_csv("b1.csv")    # [30, 1]
let X  = Tensor.from_csv("X.csv")     # [N, 784]
```

#### `Tensor.eval(t1, t2, ...) -> Nil`

可変長の Tensor を受け、依存グラフを topological 順に評価します。
共有部分式は一度だけ計算されます。学習ループの mini-batch 境界で
**必ず一度呼ぶ**（呼ばないとグラフが累積してメモリが膨張）。

```culebra
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

## 6. `JSON`

Culebra の値と JSON テキストの相互変換。両バックエンドで同じ API
を提供します。

### `JSON.stringify(v: Any) -> String` / `JSON.stringify(v: Any, indent: Long) -> String`

`v` を JSON 文字列にシリアライズします。引数 1 つだとコンパクト出力
（空白なし）、正の `indent` を渡すとそのスペース数でインデントし
pretty-print します（カンマの後に改行、`":"` の代わりに `": "`）。
`indent <= 0` はコンパクトと等価です。

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

### `JSON.parse(s: String) -> Any`

JSON 文字列を Culebra の値に変換します。小数点や指数表記を含まない
数値は `Long`、それ以外は `Float` として読み込まれます。Object の
キーは入力順に保持されます。

不正な入力には短いメッセージ付きで `ValueError` が投げられます
（`JSON.parse: unterminated string`, `expected ':'` など）。

```culebra
let v = {name: 'alice', age: 30, tags: ['admin', 'staff']}
puts(JSON.stringify(v))              # コンパクト: {"name":"alice",...}
puts(JSON.stringify(v, 2))           # pretty, 2 スペースインデント
let back = JSON.parse(JSON.stringify(v))
puts(back.name)                      # alice
```

---

## 7. 設計上の注記

### 名前空間ファースト、グローバルは CLI のエイリアス

ライブラリ自体は**グローバル名を一切追加しません**。すべての関数
は `Math`, `IO`, `Random`, `Sys` のいずれかに属します。これにより
`culebra::environment()` はホストアプリケーションに埋め込むスクリプト
エンジンとして、意図しないグローバルを持ち込まない形になります。

ただし CLI スクリプトで `puts` / `print` は頻出するため、毎回
`IO.puts` と書くのは摩擦が大きい。CLI バイナリ（`src/main.cc`）
は環境構築直後にこれらをグローバルとしてインストールします。
指す関数値は `IO` 配下と同一なので重複はありません。V8 が同様の
アプローチを採っており、エンジン自体は `print` を提供せず、`d8`
シェルが後付けで導入しています。

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

## 8. 未収録（将来検討）

### 三角関数

`Math.sin` / `cos` / `tan` / `atan2` は未実装です。乱数生成と主要な
超越関数（`log`, `exp`, `sqrt`）は揃っているので、三角関数は具体的
なユースケースが出てきた時点で追加します。

### 正規表現

将来対応。ビルトインの正規表現エンジン、またはベンダ依存のライブラリ
が必要です。

### 日時

将来対応。必要なら `IO.read` / `IO.write` 経由でヘルパープロセスを
呼ぶ形で代用できます。

### `Array`/`Object` 以外のコレクション

`Set`, `Queue`, `Tuple` などはありません。当面は `Array` と
`Object` で代用してください。

---

関連: 言語仕様は [`docs/language.ja.md`](language.ja.md) にあります。
