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

Culebra には 7 種類の型があります: `Nil`, `Bool`, `Long`, `String`,
`Array`, `Object`, `Function`。

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

関数は第一級の値です。定義は `fn (params) { body }`:

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

3. 配列と Object
-----------------

```culebra
arr = [1, 2, 3]
puts(arr.size())                      # 3
puts(arr.map(fn (x) { x * x }))       # [1, 4, 9]
puts(arr.filter(fn (x) { x % 2 == 1 })) # [1, 3]
puts(arr.reduce(0, fn (acc, x) { acc + x })) # 6

obj = {name: 'alice', age: 30}
puts(obj.name)                        # 'alice'
puts(obj.keys())                      # ['age', 'name']
obj.age = 31                          # プロパティ再代入
```

4. 文字列補間とパターンマッチ
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

5. クロージャによるオブジェクト（Culebra の核イディオム）
----------------------------------------------------------

クラス構文はありませんが、**クロージャで state を捕捉する Object リテラル**
でオブジェクト指向スタイルが書けます:

```culebra
Car = {
  new: fn (miles_per_run) {
    mut total_miles = 0
    {
      run:   fn (times) { total_miles = total_miles + miles_per_run * times },
      total: fn ()      { "走行距離: {total_miles} miles." }
    }
  }
}

car = Car.new(5)
car.run(1)
car.run(2)
puts(car.total())                     # '走行距離: 15 miles.'
```

`run` メソッド内の裸の `total_miles = ...` が、外側 `new` の `mut
total_miles` を再代入します。これで private state を持つオブジェクト
が表現できます。

6. シャドウ禁止で身を守る
---------------------------

外側関数でキャプチャされた変数を `let` / `mut` / パラメータ / `match`
パターンで**シャドウすると**コンパイルエラーになります:

```culebra
make_bumper = fn () {
  mut count = 0
  bump = fn () {
    mut count = 10    # エラー: 外側変数 'count' をシャドウできません
  }
}
```

これは "新しいローカルを作ったつもりが外側を書き換えるつもりだった"
という典型的バグを未然に防ぎます。グローバル変数や同関数内のブロック
スコープでのシャドウは許可されます。詳細は
[`language.ja.md` §6](language.ja.md) の "シャドウ禁止" を参照。

次の一歩
---------

- サンプル集: [`samples/`](../samples/)（`class.cul`, `closure.cul`,
  `match.cul`, `types.cul` など）
- 言語仕様の全体像: [`language.ja.md`](language.ja.md)
- 標準ライブラリのリファレンス: [`stdlib.ja.md`](stdlib.ja.md)
- インタラクティブに試す: `./build/culebra --shell`
