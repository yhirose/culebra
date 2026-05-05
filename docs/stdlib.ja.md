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
3. `Sys`
4. 設計上の注記
5. 未収録（将来検討）

---

1. `Math`
---------

整数演算のユーティリティ群。Culebra には `Float` 型と `**` 演算子が
導入されていますが（言語仕様 §4 / §7）、ライブラリレベルの浮動小数点
ヘルパ（`sqrt`, `log`, `exp`, `sin`, `cos`, `random`）は引き続き
保留です。下の §5 を参照。

### `Math.abs(x: Long) -> Long`

`x` の絶対値。

```culebra
puts(Math.abs(-7))    # 7
```

### `Math.min(a: Long, b: Long) -> Long`

`a` と `b` のうち小さい方。

### `Math.max(a: Long, b: Long) -> Long`

`a` と `b` のうち大きい方。

### `Math.pow(base: Long, exp: Long) -> Long`

整数累乗。繰り返し二乗法で `base ** exp` を計算します。
`Math.pow(x, 0)` は `x` に関わらず `1`（`0` を含む）。

**例外**: `exp < 0` のとき `type error at L:C.`。

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

---

3. `Sys`
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

4. 設計上の注記
----------------

### 名前空間ファースト、グローバルは CLI のエイリアス

ライブラリ自体は**グローバル名を一切追加しません**。すべての関数
は `Math`, `IO`, `Sys` のいずれかに属します。これにより
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

5. 未収録（将来検討）
----------------------

### 浮動小数点演算（`Math.sqrt`, `Math.sin`, `Math.cos`, `Math.log`, `Math.exp`, `Math.random`）

`Float` 型は導入されており、冪乗は `**` 演算子で行えますが、
ライブラリレベルのヘルパ（`sqrt`、三角関数、対数、乱数）はまだ
配線されていません。Phase 2 で追加予定。当面は平方根なら
`x ** 0.5` などで代替できます。

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
