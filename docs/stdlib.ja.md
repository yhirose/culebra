Culebra 標準ライブラリ
======================

本書は Culebra の**組み込みライブラリ**を規定します。ランタイム
ユーティリティをまとめた名前空間オブジェクト（`Math`, `IO`, `Sys`）
を対象とします。ここに記載のものは `import` 文なしで利用できます。

言語レベルの組み込み関数（`assert`, `to_long`, `to_string`,
`type_of`）は [言語仕様 §18](language.ja.md) を参照してください。
組み込み型（`String`, `Array`, `Object`）のメソッドは
[言語仕様 §17](language.ja.md) に規定されています。

CLI（`src/main.cc`）はこれに加え、`puts` と `print` を
`IO.puts` / `IO.print` のエイリアスとしてグローバルに配置します
（[言語仕様 §19](language.ja.md) 参照）。`culebra::environment()`
を直接呼び出す埋め込み用途では、これらのエイリアスは導入されず
名前空間はクリーンなままです。

本書で用いる記法:

* 型注釈は[言語仕様 §14](language.ja.md) に従います。`Any` は任意の
  値を示します。
* 例外条項は `type error at L:C.` 等の実行時エラーを示します。
  [言語仕様 §15](language.ja.md) を参照。

目次
----

1. `Math`
2. `IO`
3. `Random`
4. `Sys`
5. 設計上の注記
6. 未収録（将来検討）

---

1. `Math`
---------

数値ユーティリティ群。整数専用ルーチン（`pow`・`sign`・`clamp`・
`iota`・`range`）は `Long` 入力を保ち、浮動小数点ルーチン（`log` ほか）
は `Long` / `Float` のいずれかを受け取ります。`Long` と `Float` の
相互作用は言語仕様 §4 / §7 を参照。

### 定数

`Math.pi` / `Math.e` / `Math.inf` / `Math.nan` は `Float` の
プロパティ（π、e、正の無限大、quiet NaN）です。`--jit` でも
コンパイル時定数として展開されます。

```culebra
puts(Math.pi)              # 3.141592653589793
puts(Math.e)               # 2.718281828459045
puts(Math.inf > 1e308)     # true
puts(Math.nan == Math.nan) # false
```

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

### `Math.iota(n: Long) -> Array` / `Math.iota(start: Long, end: Long) -> Array`

連続整数の新しい `Array` を生成します。名前は APL / C++
`std::iota` / Scheme SRFI-1 の慣例に倣い、「連続整数を配列として
実体化する」操作を表します。**遅延版**は `Math.range`（同じ引数で
イテレータを返す）で、`for`-in ループでの反復には `Math.range`、
`Array` 自体が必要な場合には `Math.iota` を使い分けます。

* `Math.iota(n)` は `[0, 1, ..., n-1]` を返します。`n <= 0` なら空配列。
* `Math.iota(start, end)` は `[start, start+1, ..., end-1]` を返します。
  `start >= end` なら空配列。

```culebra
puts(Math.iota(3))         # [0, 1, 2]
puts(Math.iota(2, 5))      # [2, 3, 4]
puts(Math.iota(5, 2))      # []
```

### `Math.range(n: Long) -> Iterator` / `Math.range(start: Long, end: Long) -> Iterator`

`Math.iota` の遅延版。同じ整数列を 1 要素ずつ yield するイテレータを
返します。`for`-in ループや（Phase 3 で入る予定の）イテレータ
メソッドチェーンと組み合わせて使い、範囲サイズに関わらず**定数の
追加メモリ**で反復します。空範囲の規約は `iota` と同じで、`n <= 0`
や `start >= end` は即座に完了するイテレータを返します。

```culebra
for i in Math.range(5)     { puts(i) }     # 0, 1, 2, 3, 4
for i in Math.range(2, 6)  { puts(i) }     # 2, 3, 4, 5

# 巨大範囲でも定数メモリ
for i in Math.range(1_000_000_000) {
  if i > 3 { break }
  puts(i)
}
```

**JIT**: `Math.range` は `--jit` でも JIT-native な iterator
オブジェクトを返し、for-in や lazy iterator メソッドがプロトコル
速度（1 ステップあたりクロージャ呼出 1 回）で駆動します。最大
スループットで要素を回したい場合は `Math.iota`（eager `Array`）を、
巨大な整数列を定数メモリでストリームしたい場合は `Math.range` を
使い分けてください。詳細は language.ja.md §17.5 参照。

---

2. `IO`
-------

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

**例外**: 開けないとき `type error at L:C.`。

```culebra
contents = IO.read('data.txt')
```

### `IO.write(path: String, content: String) -> Nil`

`content` を `path` のファイルに書き込みます（作成または上書き）。

**例外**: 書き込み用に開けないとき `type error at L:C.`。

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

3. `Random`
-----------

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

4. `Sys`
--------

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

5. 設計上の注記
----------------

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

自由関数（名前空間内）は、無から値を構築する場合（`Math.iota`,
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

6. 未収録（将来検討）
----------------------

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
