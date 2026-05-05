Culebra チュートリアル
========================

Culebra を触ったことがない人向けの 5 分クイックスタートです。
詳細な仕様は [`language.md`](language.ja.md)、標準ライブラリは
[`stdlib.md`](stdlib.ja.md) を参照してください。

準備
----

```bash
just build
echo "puts('hello')" > hello.cul
./build/culebra hello.cul          # → 'hello'
./build/culebra --jit hello.cul    # JIT 版も同じ出力
```

1. 値と宣言
------------

Culebra には 8 種類の型があります: `Nil`, `Bool`, `Long`, `Float`,
`String`, `Array`, `Object`, `Function`。

```culebra
x = 10             # bare 代入（イミュータブル束縛、外側があれば再代入）
let y = 20         # let: 新規イミュータブル束縛
mut z = 30         # mut: 新規ミュータブル束縛
z = z + 1          # mut なら再代入可
```

変数名は見つけた順に外側スコープまで探して再代入します。見つからな
ければ現スコープに新規作成します。

2. 関数とクロージャ
--------------------

関数は第一級の値です。定義は `fn (params) { body }`、または短縮形は
`|x| expr`:

```culebra
add = fn (a, b) { a + b }
puts(add(2, 3))                    # 5

# 型注釈はオプション
add_typed = fn (a: Long, b: Long) -> Long { a + b }

# 再帰は self で
fib = fn (x) {
  if x < 2 { x } else { self(x - 2) + self(x - 1) }
}
puts(fib(10))                      # 55
```

**クロージャ**は外側の変数を捕捉します:

```culebra
make_counter = fn () {
  mut n = 0
  fn () { n = n + 1; n }   # bare `n = ...` は外側の n を再代入
}
c = make_counter()
puts(c())                  # 1
puts(c())                  # 2
```

3. 制御フロー
--------------

`if` (および §5 の `match`) は式で、選ばれた分岐の値を持ちます。
`while` と `for` は文で、値は `nil` です。

```culebra
sign = if x > 0 { 1 } else if x < 0 { -1 } else { 0 }

mut i = 0
while i < 5 { puts(i); i = i + 1 }

for c in 'abc'   { puts(c) }    # 文字列を 1 文字ずつ走査（UTF-8 スカラ単位）
for n in 0..10   { puts(n) }    # exclusive range
for n in 0..=10  { puts(n) }    # inclusive range
for k in obj     { puts("{k}={obj[k]}") }   # Object のキーを昇順走査
```

`break` / `continue` は `while` と `for` の中で使えます。

4. 配列・Object・イテレータ連鎖
--------------------------------

```culebra
arr = [1, 2, 3]
puts(arr.size())                      # 3
puts(arr.sum())                       # 6   (product / min / max も同様)

# メソッドチェーンで合成。ラムダは |x| 形式が便利
puts([1, 2, 3, 4].map(|x| x * x)
                 .filter(|x| x > 4)
                 .reduce(0, |a, b| a + b))  # 25

obj = {name: 'alice', mut age: 30}    # `mut` を付けたプロパティのみ再代入可
puts(obj.name)                        # 'alice'
puts(obj.keys())                      # ['age', 'name']
obj.age = 31                          # `mut` プロパティの再代入
```

`Math.range(N)` は遅延イテレータを返し、`.map`, `.filter`, `.reduce`,
`.for_each` 等と組み合わせると中間配列を作りません。JIT は多くの
パターンを単純なカウンタループへ融合します — 詳細は
[`language.md` §17](language.ja.md)。

5. 文字列補間とパターンマッチ
------------------------------

```culebra
name = 'Culebra'
puts("hello {name}!")                 # 'hello Culebra!'

describe = fn (v) {
  match v {
    0                  => 'zero',
    1 | 2 | 3          => 'small',
    n: Long if n > 100 => "big ({n})",
    s: String          => "str ({s})",
    [head, ...tail]    => "head={head}, rest={tail.size()}",
    {name, age}        => "{name}, {age}",
    _                  => 'other'
  }
}
puts(describe(2))                     # 'small'
puts(describe([1, 2, 3, 4]))          # 'head=1, rest=3'
puts(describe({name: 'bob', age: 25})) # 'bob, 25'
```

6. `class` 糖衣
----------------

`class` はコンストラクタとメソッドを宣言します。`this.x = ...` で作る
フィールドはデフォルトで mutable。インスタンスは `class:` タグを持ち
`obj.class` や `match` から参照できます。

```culebra
class Car {
  new(mpr)  { this.miles = 0; this.mpr = mpr }
  run(n)    { this.miles = this.miles + this.mpr * n }
  total()   { "走行距離: {this.miles} miles." }
}
car = Car.new(5)
car.run(1); car.run(2)
puts(car.total())                     # '走行距離: 15 miles.'
puts(car.class)                       # 'Car'
```

メソッドには演算子オーバーロード用の dunder (`__add__`, `__mul__`,
`__pow__`, `__matmul__`, ...) や、well-known な `drop` (RAII フック)
も書けます:

```culebra
class V {
  new(x)        { this.x = x }
  __add__(o)    { V.new(this.x + o.x) }
  drop()        { puts("releasing {this.x}") }
}
puts((V.new(2) + V.new(3)).x)         # 5
# V がコレクトされるたび 'releasing ...' が呼ばれる
```

外側の `mut` を捕捉するクロージャもオブジェクトとして使えます
(`class` キーワードなし)。両スタイルの比較は
[`language.md` §10](language.ja.md) を参照。

7. エラーハンドリング
----------------------

`throw` で送出、`try ... catch` で捕捉します。送出する値は任意の
Culebra 値で、文字列でも Object でも構いません。

```culebra
parse_pos = fn (s) {
  let n = to_long(s)
  if n <= 0 { throw "expected positive, got {s}" }
  n
}

result = try { parse_pos('-3') } catch err { "fallback ({err})" }
puts(result)                          # 'fallback (expected positive, got -3)'
```

`defer { cleanup() }` で、正常終了・`return`・`throw` のどの経路でも
必ず走るクリーンアップを登録できます。

8. 標準ライブラリ
------------------

コア言語の組み込み関数は裸で使えます（`assert`, `to_long`,
`to_string`, `type_of`）。それ以外は `Math`, `IO`, `Sys`, `Random`,
`String` 等の名前空間配下です:

```culebra
puts(Math.abs(-7))              # 7
puts(Math.iota(5))              # [0, 1, 2, 3, 4]

name = IO.input()               # 標準入力から 1 行
IO.write('out.txt', 'hello')    # ファイル書き出し

# $ culebra script.cul -- alice bob
puts(Sys.argv)                  # ['alice', 'bob']
```

`puts` と `print` は CLI が提供するエイリアスで `IO.puts` /
`IO.print` と同じ関数を指します。`culebra` バイナリでスクリプト
を実行する場合 `puts(x)` と `IO.puts(x)` は等価です。
`culebra::environment()` を直接使う埋め込み用途では `IO` 名前空間
のみが提供されます。

詳細は [`stdlib.ja.md`](stdlib.ja.md)。

> **サイドバー — UFCS。** 任意の自由関数 `f(x, ...)` は `x.f(...)`
> としても呼べます。純粋に構文上の糖衣ですが、自分が所有していない
> 型に対するヘルパーをメソッドチェーンに自然に混ぜられます。

> **サイドバー — シャドウ禁止。** 外側関数で捕捉された変数を
> `let` / `mut` / パラメータ / `match` パターンで**シャドウすると**
> コンパイルエラーになります。"新しいローカルを作ったつもりが
> 外側を書き換えるつもりだった" バグを未然に防ぎます。詳細は
> [`language.ja.md` §6](language.ja.md)。

次の一歩
---------

- サンプル集: [`samples/`](../samples/)（`class.cul`, `closure.cul`,
  `match.cul`, `types.cul`, `microgpt/` など）
- 言語仕様の全体像: [`language.ja.md`](language.ja.md)
- 標準ライブラリのリファレンス: [`stdlib.ja.md`](stdlib.ja.md)
- インタラクティブに試す: `./build/culebra --shell`
