Culebra 標準ライブラリ
======================

本書は Culebra 実行系に組み込まれている**グローバル関数**と**入出力**
を規定します。ここに記載のものは `import` 文なしで利用できます。

組み込み型（`String`, `Array`, `Object`）のメソッドは言語自身の一部で、
[言語仕様 §17](language.ja.md) を参照してください。

本書で用いる記法:

* 型注釈は[言語仕様 §14](language.ja.md) に従います。`Any` は任意の
  値を示します。
* 例外条項は `type error at L:C.` 等の実行時エラーを示します。
  [言語仕様 §15](language.ja.md) を参照。

目次
----

1. グローバル関数
2. 入出力
3. 設計上の注記
4. 未収録（将来検討）

---

1. グローバル関数
------------------

### `puts(x: Any) -> Nil`

`x` を改行付きで標準出力に書き出します。参照型は
`Array.str_array()` / `Object.str_object()` と同じ書式で整形され、
文字列は**シングルクォートで囲んで**出力されます。

```culebra
puts('hi')       # → 'hi'
puts(42)         # → 42
puts([1, 'a'])   # → [1, 'a']
```

### `print(x: Any) -> Nil`

`x` を**末尾改行なし**で標準出力へ書き出します。書式は `to_string`
と同じで、文字列は**引用符なし**で出力されます。複数回の書き込みで
1 行を組み立てたい場合に便利です。

```culebra
print('Hello, ')
print('world!')
puts('')         # → Hello, world!
```

### `assert(cond: Bool) -> Nil`

`cond` を評価し、偽なら `assert failed at L:C.` で中断します。位置は
`assert` 呼出のソース位置です。

```culebra
assert(1 + 1 == 2)
```

**例外**: 偽のとき `assert failed at L:C.`、`cond` が `Bool` でも
`Long` でもないとき `type error at L:C.`。

### `abs(x: Long) -> Long`

`x` の絶対値。

```culebra
puts(abs(-7))    # 7
```

### `min(a: Long, b: Long) -> Long`

`a` と `b` のうち小さい方。

### `max(a: Long, b: Long) -> Long`

`a` と `b` のうち大きい方。

### `range(n: Long) -> Array` / `range(start: Long, end: Long) -> Array`

整数の新しい `Array` を生成します。

* `range(n)` は `[0, 1, ..., n-1]` を返します。`n <= 0` なら空配列。
* `range(start, end)` は `[start, start+1, ..., end-1]` を返します。
  `start >= end` なら空配列。

```culebra
puts(range(3))         # [0, 1, 2]
puts(range(2, 5))      # [2, 3, 4]
puts(range(5, 2))      # []
```

### `to_long(s: String) -> Long`

`s` を 10 進の符号付き整数としてパース。前後の空白は許容、それ以外
は失敗します。

**例外**: パースできない場合 `type error at L:C.`。

```culebra
puts(to_long('42'))    # 42
puts(to_long('-7'))    # -7
```

### `to_string(v: Any) -> String`

`v` を表示形式に変換します（補間時と同じフォーマット。文字列は
引用符なしで渡る）。

```culebra
puts(to_string(42))         # '42'
puts(to_string([1, 2]))     # '[1, 2]'
puts(to_string('hi'))       # 'hi'
```

### `type_of(v: Any) -> String`

`v` の実行時の型名を返します。
`'Nil'`, `'Bool'`, `'Long'`, `'String'`, `'Array'`, `'Object'`,
`'Function'` のいずれか。

```culebra
puts(type_of(42))          # 'Long'
puts(type_of('hi'))        # 'String'
puts(type_of([1, 2]))      # 'Array'
```

---

2. 入出力
----------

### `input() -> String`

標準入力から 1 行読み取ります。末尾の改行は除かれます。EOF のとき
`''`（空文字列）を返します。

```culebra
puts('name?')
name = input()
puts("Hello, {name}")
```

### `read_file(path: String) -> String`

`path` のファイル全体を `String` として読み込みます。

**例外**: 開けないとき `type error at L:C.`。

```culebra
contents = read_file('data.txt')
```

### `write_file(path: String, content: String) -> Nil`

`content` を `path` のファイルに書き込みます（作成または上書き）。

**例外**: 書き込み用に開けないとき `type error at L:C.`。

```culebra
write_file('out.txt', 'hello\n')
```

---

3. 設計上の注記
----------------

### 自由関数 vs メソッド

自由関数は、無から値を構築する場合（`range`, `input`）、複数の型に
等しく適用される場合（`type_of`, `to_string`）、および修飾なしの
名前の方が読みやすい場合（`abs(x)`, `min(a, b)`）に使います。
特定の型に関する操作はメソッド構文を用いますが、その設計方針は
言語仕様 §17（String/Array/Object メソッド）を参照してください。

### エラー送出 vs `nil` 戻り値

回復不能な型エラー（`to_long('abc')`、存在しないファイルへの
`read_file(...)` など）は例外送出を優先し、「見つかるかどうか」の
述語はセンチネルを返す方針です（`input()` は EOF で `''`）。
`try`/`catch` なしでホットパスを簡潔に保つためです。

---

4. 未収録（将来検討）
----------------------

### 浮動小数点演算（`sqrt`, `sin`, `cos`, `log`, `pow`, `random`）

Culebra には `Float` 型がまだありません。整数のみの数学関数は
意味が不自然になります（`sqrt` は整数平方根になるなど）。本格的な
数学ライブラリは、数値型体系を導入する将来フェーズに委ねます。

### 正規表現

将来対応。ビルトインの正規表現エンジン、またはベンダ依存のライブラリ
が必要です。

### 日時、乱数、OS インスペクション

将来対応。必要なら `read_file`/`write_file` 経由でヘルパープロセスを
呼ぶ形で代用できます。

### `Array`/`Object` 以外のコレクション

`Set`, `Queue`, `Tuple` などはありません。当面は `Array` と
`Object` で代用してください。

---

関連: 言語仕様は [`docs/language.ja.md`](language.ja.md)にあります。
