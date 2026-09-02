Culebraクイックガイド
==========================

正しいCulebraを書くために必要なものを1ファイルにまとめたもの:
構文、他言語から**持ち込むと外れる**習慣、そして標準ライブラリの全
シグネチャ。凝縮元のリファレンス
([`handbook.ja.md`](handbook.ja.md)、[`language.ja.md`](language.ja.md)、
[`stdlib.ja.md`](stdlib.ja.md)) は合計15万トークン規模で、これは
そのうちプロンプトに載る部分です。

以下の` ```culebra ` ブロックはすべて `culebra test --doc docs` が
実行するので、実装から乖離することはありません。行末の
`# => <値>` は検証済みの stdout、`# !! <パターン>` は検証済みの throw
です。§4 は `just gen-quick-guide` がリファレンスから生成します —
手で編集しないでください。

目次
----

1. [プログラムを実行する](#1-プログラムを実行する)
2. [構文](#2-構文)
3. [持ち込むと外れる習慣](#3-持ち込むと外れる習慣)
4. [シグネチャ索引](#4-シグネチャ索引)
5. [テンプレート](#5-テンプレート)

## 1. プログラムを実行する

```bash
culebra prog.cul                 # バイトコードVM (既定)
culebra --jit prog.cul           # LLVM ORC JIT — 出力は同一
culebra build prog.cul -o prog   # AOT、自己完結バイナリ
culebra test                     # cwd以下のtest_*.culを全実行
culebra fmt -i .                 # その場で整形 (スタイル指定は無し)
culebra lint .                   # 静的検査; 警告1 / エラー2でexit
culebra docs -g 'Math.wrap'      # リファレンスから署名を引く
```

ソースファイルの拡張子は `.cul`。プロジェクトファイルもマニフェストも
パッケージマネージャもありません。§4 の内容はすべて `import` なしで
スコープに入っています。

未定義の名前はプログラム実行**前**に弾かれるので、ライブラリ名の当て
推量は途中まで動くことなく即座に失敗します:

```culebra
# !! undefined variable 'puts'
puts('hi')
```

弾かれるのは名前であってメンバではありません。`Math.abss(1)` や
`xs.len()` は `lint` を素通りし、その行が実行されて初めて落ちます。
存在しないプロパティはエラーでなく `nil` です。書いたら実行すること
— lint を通しただけのプログラムは検査されていません。

リファレンス一式はバイナリの中にあり、実行中のビルドと常に一致します。
`culebra docs -g <パターン>` は一致したセクションを表示し、無ければ
exit 1 になるので、出力を読まずに API の実在を判定できます。パターン
は識別子か語句であって、質問文ではありません。

## 2. 構文

### 2.1 束縛

束縛は `mut` を付けない限り不変です。裸の `x = ...` は新規束縛を作るか、
最も近い外側の束縛を再代入します。

```culebra
x = 1       # 新規不変束縛、または外側を再代入
let y = 2   # 不変; 外側のシャドウは不可
mut z = 3   # 可変
z = 4       # bare再代入
z += 1      # -= *= /= %= **= @= も同様
inspect(z)  # => 5
```

引数も不変です。代入するとローカルコピーではなくエラーになります:

```culebra
bump = fn (n) {
  n += 1
  n
}
inspect(try {
  bump(1)
} catch e {
  e.kind
})  # => 'ImmutableError'
```

**キャプチャした外側**の変数をシャドウする束縛の導入はコンパイル時
エラーです。1 つの関数内でのブロックローカルな再束縛は問題ありません。

### 2.2 型

```culebra
inspect(type_of(nil))     # => 'Nil'
inspect(type_of(true))    # => 'Bool'
inspect(type_of(42))      # => 'Long'
inspect(type_of(3.14))    # => 'Float'
inspect(type_of('hi'))    # => 'String'
inspect(type_of([1, 2]))  # => 'Array'
inspect(type_of({a: 1}))  # => 'Object'
inspect(type_of(fn () {
  1
}))                                    # => 'Function'
inspect(type_of((1, 'a')))             # => 'Tuple'
inspect(type_of({1, 2}))               # => 'Set'
inspect(type_of('hello'.slice(1, 3)))  # => 'StringView'
```

12 番目が `Tensor` です (§4)。クラス・モジュール・エラーはいずれも
`Object` の上に構築されています。

### 2.3 制御フロー

`if` / `match` / `cond` は式、`while` / `for` は文です。

```culebra
n = 7
sign = if n > 0 {
  1
} else if n < 0 {
  -1
} else {
  0
}
inspect(sign)  # => 1

label = match n {
  0 => 'zero',
  k if k < 10 => 'small',
  _ => 'large',
}
inspect(label)  # => 'small'

grade = cond {
  # 主語のないmatch
  n >= 90 => 'A',
  n >= 5 => 'B',
  _ => 'C',
}
inspect(grade)                    # => 'B'
inspect(n > 5 ? 'big' : 'small')  # => 'big'
```

```culebra
for i in 0..3 {
  inspect(i)
}  # 半開; 0..=2は閉区間; `by k`で刻む
# => |
# 0
# 1
# 2

for k, v in {a: 1, b: 2} {
  inspect("{k}={v}")
}
# => |
# 'a=1'
# 'b=2'
```

`break` / `continue` は両方のループで使えます。`nobreak` ブロックは
ループが `break` されなかったときだけ走ります。`while` / `if` /
`match` は構文内に閉じた init 節を取れます:

```culebra
for v in [1, 3, 5] {
  if v % 2 == 0 {
    break
  }
} nobreak {
  inspect('all odd')  # => 'all odd'
}

while mut i = 0; i < 3 {
  i += 1
}
if let m = 6; m > 5 {
  inspect('big')
}  # => 'big'
```

### 2.4 関数

```culebra
add = fn (a, b) {
  a + b
}
inspect(add(2, 3))  # => 5

typed = fn (a: Long, b: Long) -> Long {
  a + b
}
inspect(typed(2, 3))  # => 5

square = |x| x * x  # lambdaのbodyは既定で単一式
inspect(square(7))  # => 49

inspect([1, 2, 3].map(|x| x * 2))  # => [2, 4, 6]
inspect([[1, 2]].map(fn ((a, b)) {
  a + b
}))  # => [3]
```

値としてリテラルを束縛する (`name = fn (...) { ... }`) か、宣言形式
`fn name(...) { ... }` を使うかのどちらか。その場で渡す callback は
`|x|`。波括弧も使え、`|x| { ... }` は `fn (x) { ... }` と同じく文を
受け付ける。

**名前を付けて呼ぶ関数**、特に再帰関数には宣言形式を使うこと。
[ジェネレータ](#26-イテレータ)になれる、[多重ディスパッチ](#29-クラスufcs多重ディスパッチtrait)
に参加できる、`fn.name` にソースレベルの名前が入るのはいずれも宣言形式
だけだが、それ以外の点では2つの形式は等価で、どちらの body でも裸の
`fn` はその関数自身を呼ぶ:

```culebra
fn fib(x) {
  if x < 2 {
    x
  } else {
    fn(x - 2) + fn(x - 1)
  }
}
inspect(fib(10))   # => 55
inspect(fib.name)  # => 'fib'
```

ブロックは最後の式に評価されるので `return` はほとんど不要です。`*`
以降はキーワード専用、`**rest` は未知のキーワードを、`*rest` は余った
位置引数を集めます:

```culebra
greet = fn (name, *, greeting = 'hi', **opts) {
  suffix = opts.has('loud') && opts.loud ? '!' : ''
  "{greeting}, {name}{suffix}"
}
inspect(greet('alice'))                     # => 'hi, alice'
inspect(greet('bob', greeting: 'yo'))       # => 'yo, bob'
inspect(greet('cy', loud: true))            # => 'hi, cy!'
inspect(greet('dee', **{greeting: 'hey'}))  # => 'hey, dee'
```

### 2.5 文字列

補間されるのは二重引用符だけ。単一引用符はリテラルです。

```culebra
name = 'Culebra'
inspect("hello, {name}")  # => 'hello, Culebra'
inspect('hello, {name}')  # => 'hello, {name}'
inspect('a' + 'b')        # => 'ab'
```

`size()` は UTF-8 のバイト数、`for` と `iter()` は Unicode スカラー
1 個ずつ、`graphemes()` は書記素クラスタ 1 個ずつ進みます。`s[i]`
演算子は**ありません** — バイトオフセットを取る `slice` を使います。

```culebra
inspect('café'.size())                        # => 5
inspect('café'.graphemes().collect().size())  # => 4
inspect('café'.slice(0, 1))                   # => 'c'
inspect('hello world'.split(' '))              # => ['hello', 'world']
inspect(['a', 'b'].join('-'))                  # => 'a-b'
```

`"""` はブロック文字列。`"..."` と同じく補間し、閉じ区切りのインデントを
取り除き、その直前の改行も落とす。閉じ `"""` は独立した行に置く。複数行の
文字列は `\n` を連結せずこれを使う。

```culebra
sql = """
    SELECT *
    FROM t
    """

inspect(sql.lines())  # => ['SELECT *', 'FROM t']
```

### 2.6 イテレータ

`range` は遅延、`iota` は確保します。`.iter()` は Array を遅延化し、
チェーンは最初の consumer で止まって中間 Array を作りません。

```culebra
inspect(iota(3))  # => [0, 1, 2]
inspect(range(1000).filter(|x| x % 2 == 0).map(|x| x * 3).take(4).collect())
# => [0, 6, 12, 18]
inspect(range(1, 11).reduce(0, |a, x| a + x))                  # => 55
inspect([1, 2, 3, 4].iter().zip(['a', 'b']).collect().size())  # => 2

for i, v in ['x', 'y'].enumerate() {
  inspect("{i}:{v}")
}
# => |
# '0:x'
# '1:y'
```

body に `yield` を含む `fn` はジェネレータになり、呼ぶとイテレータが
返ります。

```culebra
fn countdown(start) {
  mut i = start
  while i > 0 {
    yield i
    i -= 1
  }
}
inspect(countdown(3).collect())  # => [3, 2, 1]
```

`iter()` / `has_next()` / `next()` を持つオブジェクトなら何でも `for`
と全チェーンメソッドで使えます。

### 2.7 パターンマッチ

```culebra
describe = fn (v) {
  match v {
    0 => 'zero',
    1 | 2 | 3 => 'small',
    n: Long if n > 100 => "big ({n})",
    n: Long => "int ({n})",
    s: String => "str ({s})",
    [] => 'empty',
    [x] => "one ({x})",
    [head, ...tail] => "head={head} rest={tail.size()}",
    {name} => "named {name}",
    _ => 'other',
  }
}
inspect(describe(2))            # => 'small'
inspect(describe(999))          # => 'big (999)'
inspect(describe([1, 2, 3]))    # => 'head=1 rest=2'
inspect(describe({name: 'z'}))  # => 'named z'
```

網羅性検査はありません。`_` の腕を用意してください。

### 2.8 エラー・`defer`・`drop`

throw できる値に制限はなく、`try` は式です。

```culebra
check = fn (x) {
  if x < 0 {
    throw "negative: {x}"
  }
  x
}
inspect(try {
  check(-1)
} catch e {
  e
})  # => 'negative: -1'
inspect(try {
  check(7)
} catch _ {
  0
})  # => 7
```

組み込みエラーは `kind` を持つ Object です:

```culebra
inspect(try {
  1 / 0
} catch e {
  e.kind
})  # => 'ZeroDivisionError'
```

`defer` は囲むブロックのあらゆる脱出経路で LIFO 順に走ります。引数
なしの `drop` プロパティを持つ Object は、最後の参照が消えた時点で
それが呼ばれます。

```culebra
{
  defer {
    inspect('second')
  }
  defer {
    inspect('first')
  }
  inspect('body')
}
# => |
# 'body'
# 'first'
# 'second'
```

```culebra
{
  r = {drop: fn () {
    inspect('released')
  }}
  inspect('in scope')
}
inspect('after')
# => |
# 'in scope'
# 'released'
# 'after'
```

### 2.9 クラス・UFCS・多重ディスパッチ・trait

フィールドには `self` でアクセスします (`this` ではありません)。
クラス自体の呼び出しは `.new` の短縮形です。クラスも Object リテラルと
同じ規則で `drop()` を定義でき (2.8)、インスタンスへの最後の参照が
消えた時点でそれが呼ばれます。

```culebra
class Car {
  wheels = 4  # デフォルト付きの宣言フィールド
  new(mpr) {
    self.miles = 0
    self.mpr = mpr
  }
  drop() {}  # 最後の参照が消えた時点でリソースを後始末
  run(n) {
    self.miles += self.mpr * n
  }
  get far() {
    self.miles > 10
  }  # 計算プロパティ、括弧なしで呼ぶ
  static unit() {
    Car(1)
  }
}

c = Car(5)
c.run(3)
inspect(c.miles)   # => 15
inspect(c.far)     # => true
inspect(c.wheels)  # => 4
inspect(c.class)   # => 'Car'
```

演算子は dunder メソッド (`__add__`、`__eq__`、`__lt__`、`__index__`、
`__setindex__`、`__call__` 等) に対応します。逆側メソッド
(`__radd__`) はありません — その演算を所有する型の側にオーバーロード
を置いてください。

自由関数 `f(x, ...)` は `x.f(...)` として呼べますが、既存のプロパティ
やメソッドが常に優先されます:

```culebra
double = fn (x) {
  x * 2
}
inspect(42.double())  # => 84
```

同名の自由関数を複数定義すると、宣言された引数型でディスパッチします:

```culebra
class Circle {
  new(r) {
    self.r = r
  }
}
fn area(c: Circle) {
  3 * c.r * c.r
}
fn area(n: Long) {
  n
}
inspect(area(Circle(2)))  # => 12
inspect(area(10))         # => 10
```

`trait` は構造的です。メソッド名とアリティが一致するクラスは `impl`
宣言なしで適合します。trait メソッドはデフォルト実装の body を持て、
`@derive(Eq, Hash, Show, Comparable)` が定型の適合メソッドを生成します。

```culebra
trait Greeter {
  hello() -> String
}
class Bob {
  new(n) {
    self.n = n
  }
  hello() {
    "hi, {self.n}"
  }
}
greet = fn (g: Greeter) -> String {
  g.hello()
}
inspect(greet(Bob('Ann')))  # => 'hi, Ann'
```

`enum`はsum typeです。nullary variantはsingleton値、payload variantは
コンストラクタで、`match`で分解します。variantはそのまま`Eq`かつ
`Hashable`なので、Object / Setのkeyになります。

```culebra
enum Shape {
  Circle(Float),
  Rect(Float, Float),
  Origin,
}
area = fn (s) {
  match s {
    Circle(r) => 3 * r * r,
    Rect(w, h) => w * h,
    o: Origin => 0,
  }
}
inspect(area(Shape.Rect(2.0, 3.0)))          # => 6.0
mut seen = {}
seen[Shape.Origin] = 'origin'
inspect(seen[Shape.Origin])                  # => 'origin'
inspect({Shape.Origin, Shape.Origin}.size())  # => 1
```

### 2.10 エフェクト

`perform` は、意味を外側の `handle` が決める操作を発行します。継続は
multi-shot です。

```culebra
effect fn ask()

inspect(handle {
  perform ask() * 2
} with ask(resume) {
  resume(21)
})  # => 42
```

`resume` を呼ばない節は残りの計算を捨てます — これはまさに例外です。
`with return(v) { ... }` は正常完了時の値を写します。

### 2.11 モジュール

`export` / `import` はトップレベル専用なので、依存グラフは parse 時に
確定します。

```culebra
# doctest: skip
# lib.cul
let greet = fn (n) {
  "hello, {n}"
}
export {greet}
```

```culebra
# doctest: skip
# main.cul
import lib from './lib.cul'
inspect(lib.greet('world'))  # => 'hello, world'
```

パスは import 元ファイルからの相対で解決される単一引用符リテラルです。
各モジュールの評価は 1 回だけ。循環は `ImportError` になります。

### 2.12 オプショナル型注釈

注釈が検査されるのは代入・引数渡し・戻り値の 3 境界だけで、それ以外
では検査されません。`Long | String` は Union、`T?` は `T | Nil`、
`Array<Long>` は要素型を**文書化**しますが要素ごとの検査はしません。

```culebra
show = fn (x: Long | String) -> String {
  to_string(x)
}
inspect(show(1))     # => '1'
inspect(show('hi'))  # => 'hi'
```

## 3. 持ち込むと外れる習慣

各行は、他言語の習慣のまま書くと失敗するか、黙って別のものになる
ケースです。

| つい書くもの | Culebra では |
|---|---|
| `'text {x}'` の補間 | 補間されるのは `"..."` のみ。`'...'` はリテラル |
| `puts` / `console.log` / 改行付きの `print(x)` | `inspect(x)` はクォート付きデバッグ形式、`println(x)` は生+改行、`print(x)` は生 |
| `this` | `self` |
| `x \|> f()` | UFCS の `x.f()`。パイプライン演算子は無い |
| 1 要素の Set として `{3}` | `SyntaxError`。1 要素は `{3,}`、`{}` は空 Object |
| 集合の `a \| b` / `a & b` | メソッド: `a.union(b)` / `a.intersect(b)` / `a.diff(b)` |
| `{a: 1} + {b: 2}` | `TypeError`。Object のマージ演算子は無い |
| String への `s[0]` | `TypeError`。バイトオフセットを取る `s.slice(0, 1)` を使う |
| `-7 % 3 == 2` (Python) | `-1` — 符号は被除数に従う (C 流) |
| `-7 / 2 == -4` (Python) | `-3` — Long 除算はゼロ方向に切り捨て |
| `if [] { }` / `if '' { }` | `TypeError`。判定できるのは `Bool` / `Long` / `Float` のみ |
| `0 == false` | `false` — 型をまたぐ暗黙変換は無い |
| `.length` / `.count` | `.size()`。存在しないプロパティは `nil` なので `.length` は raise せず `nil` になる |
| `.append(x)` | `.push(x)` |
| `del a[i]` / `a.splice(i, 1)` | `a.remove_at(i)`。取り除いた要素を返す |
| `obj['missing']` | `KeyError`。`obj.missing` は `nil`、`obj.get('missing', dflt)` は fallback を取る |
| `'ab' * 3` | `TypeError`。文字列の繰り返し演算子は無い |
| `s.find(x)` / `s.indexOf(x)` | `s.index_of(x)` — バイトオフセット、無ければ`-1` |
| `s.ljust(10)` / `s.padStart(10)` / `s.zfill(5)` | 補間の書式指定: `"{s:<10}"`、`"{n:05}"` |
| 引数なしの`s.split()`（空白で分割） | `s.split_whitespace()`。`split`は必ず区切りを取る |
| 引数への代入 | 引数は不変 — `ImmutableError` |
| 同一スコープで `x = 1` を 2 回 | `ImmutableError`。`mut x = 1` と宣言する |
| `and` / `or` / `not` | `&&` / `\|\|` / `!` |
| `elif` | `else if` |
| コメントが `#` だけ、または `//` だけ | 両方使える。加えて `/* ... */` |
| `async` / `await` | 設計上存在しない — I/O は blocking。`Isolate` / `Parallel` を使う |
| パッケージマネージャ | §4 の内容はすべて `import` なしでスコープにある |

エラーにならず値が返るぶん見落としやすいものが 2 つ:

```culebra
# split が返すのは String ではなく StringView。安いが type_of は異なる
inspect(type_of('a,b'.split(',')[0]))  # => 'StringView'

# 存在しないプロパティは黙って nil
inspect([1, 2].length)  # => nil
inspect([1, 2].size())  # => 2
```

### 慣用形

上の表はエラーになるもの。ここに挙げるのは**動く**が、この言語の書き方では
ないもので、何も警告してくれない。右の列で書く。

他言語からの移植では、`if/elif/else`や`case`/`switch`の連鎖はここでは
`match`/`cond`に対応する。`if`/`else if`の連鎖としてそのまま持ち込むのは、
移植でもっとも起こりやすい取り違えである。

| 動くが | こう書く |
|---|---|
| 値としての `if c { a } else { b }` | `c ? a : b` |
| 値を返す `if` / `else if`の連鎖 | 1つの値を定数群と比較するなら`match`、互いに無関係な条件なら`cond` |
| `"a" + x + "b"`（値の前後にリテラル文字列を継ぎ足す） | `"a{x}b"` |
| `i = i + 1` | `i += 1` |
| `x.size() == 0` / `> 0` | `x.empty()` / `!x.empty()` |
| ループの外に `mut i = 0` を置いて `i += 1` | `for i, v in xs.enumerate()` |
| `mut out = []` + `for` + `out.push(f(x))` | `xs.map(f)`（`filter` も同様） |
| `mut t = {}` + `for` + `t[k] = v` | `xs.map(\|x\| (k(x), v(x))).to_object()` |
| `mut found = false` + `while !found` | `for x in xs { … break }` か `xs.find(p)` |
| `mut hit = false` + `for` + `if p(x) { hit = true; break }` | `xs.any(p)`（逆は `xs.all(p)`） |
| `"a\n" + "b\n"` | `"""` ブロック |
| `.map(fn (x) { expr })` | `.map(\|x\| expr)` |
| `range(0, n)` | `range(n)` |
| `iota(n).map(\|_\| v)` | `repeat(n, v)` |
| `for i in 0..xs.size() { xs[i] … }` | `for x in xs` |
| `mut i = start; while i < end { …; i += 1 }` | `for i in start..end { … }` |
| `{k1: v1, k2: obj.k2, k3: obj.k3}`（1つ変えるために全フィールドを手コピー） | `{...obj, k1: v1}` |
| `if cond { stmt }` — `else` なしの単文 | `stmt if cond`（否定は `stmt unless cond`） |
| `if x == nil { x = v }` / `if !d.has(k) { d[k] = v }` | `x ??= v` / `d[k] ??= v`（`obj.key` にも使える） |
| `"{a.b(c).d ?? e}"`（長く入れ子になった式をそのまま埋め込む） | `let x = a.b(c).d ?? e` としてから `"{x}"` |

`cond` は主語のない `match` なので、互いに無関係な条件の連鎖は
`cond { a > 1 => …, b < 2 => …, _ => … }` になる。完走したかどうかを
知りたいループはフラグでなく `nobreak` ブロックを使う。

## 4. シグネチャ索引

レシーバ名は慣例です: `s` は String、`a` は Array、`o` は Object、
`it` は Iterator、`re` はコンパイル済み Regex、`f` は開いた File。
レシーバが付いていないエントリ (**セットのメソッド** の `contains(x)`、
**Http** の `json()` 等) は、そのグループ自身の型のメソッドです。
**(experimental)**が付いたグループはビルド時のopt-inで、リリースバイナリ
には入っていないことがあります（[`stdlib.ja.md`](stdlib.ja.md)の該当節に
書いてあります）。

<!-- BEGIN GENERATED: signature index -->

**文字列メソッド** — s.size() -> Long; s.empty() -> Bool; s.presence() -> String | StringView | Nil; s.upper() -> String; s.lower() -> String; s.capitalize() -> String; s.title() -> String; s.normalize(form: StringLike = "NFC") -> String; s.eq_ignore_case(other: StringLike) -> Bool; s.reverse() -> String; s.repeat(n: Long) -> String; s.truncate(max: Long, ellipsis: StringLike = "...") -> String; s.trim() -> String; s.trim_start(chars: StringLike = "") -> String; s.trim_end(chars: StringLike = "") -> String; s.tr(from: StringLike, to: StringLike) -> String; s.split(sep: StringLike, limit: Long = 0) -> Array<StringView>; s.rsplit(sep: StringLike, limit: Long = 0) -> Array<StringView>; s.split_whitespace() -> Array<StringView>; s.split_once(sep: StringLike) -> Tuple | Nil; s.rsplit_once(sep: StringLike) -> Tuple | Nil; s.split_iter(sep: StringLike) -> Iterator<StringView>; s.lines() -> Array<StringView>; s.replace(pat: String | Regex, repl: String | Function) -> String; s.contains(sub: StringLike) -> Bool; s.count(sub: StringLike) -> Long; s.starts_with(prefix: StringLike) -> Bool; s.ends_with(suffix: StringLike) -> Bool; s.index_of(sub: StringLike, start: Long = 0) -> Long; s.last_index_of(sub: StringLike) -> Long; s.strip_prefix(prefix: StringLike) -> String; s.strip_suffix(suffix: StringLike) -> String; s.replace_first(pat: String | Regex, repl: String | Function) -> String; s.is_digit() -> Bool; s.is_alpha() -> Bool; s.is_alnum() -> Bool; s.is_space() -> Bool; s.is_ascii() -> Bool; s.slice(start: Long, end: Long) -> StringView; s.view() -> StringView; s.to_string() -> String; s.iter() -> Iterator<StringView>; s.code_points() -> Iterator<Long>; s.graphemes() -> Iterator<StringView>; s.bytes() -> Iterator<Long>; String.from_code_point(cp: Long) -> String; String.from_code_points(cps: Array) -> String; String.from_bytes(bytes: Array) -> String

**配列メソッド** — a.size() -> Long; a.empty() -> Bool; a.presence() -> Array | Nil; a.push(x: Any) -> Nil (破壊的); a.pop() -> Any (破壊的); a.extend(other: Array) -> Nil (破壊的); a.insert(i: Long, x: Any) -> Nil (破壊的); a.remove_at(i: Long) -> Any (破壊的); a.get(i: Long, fallback: Any) -> Any; a.slice(start: Long, end: Long) -> Array; a.join(sep: String) -> String; a.contains(v: Any) -> Bool; a.index_of(v: Any) -> Long; a.reverse() -> Nil (破壊的); a.map(f: Function) -> Array; a.filter(f: Function) -> Array; a.for_each(f: Function) -> Nil; a.reduce(init: Any, f: Function) -> Any; a.find(f: Function) -> Any; a.any(f: Function) -> Bool; a.all(f: Function) -> Bool; a.flat_map(f: Function) -> Array; a.sum() -> Long | Float; a.product() -> Long | Float; a.min() -> Any; a.max() -> Any; a.min_by(f: Function) -> Any; a.max_by(f: Function) -> Any; a.to_set() -> Set; a.to_object() -> Object; a.group_by(f: Function) -> Object; a.partition(p: Function) -> Tuple; a.unzip() -> Tuple; a.sort(reverse: Bool = false) -> Nil (破壊的); a.sorted(reverse: Bool = false) -> Array; a.sort_by(key: Function, reverse: Bool = false) -> Nil (破壊的); a.sorted_by(key: Function, reverse: Bool = false) -> Array

**オブジェクトメソッド** — o.size() -> Long; o.empty() -> Bool; o.presence() -> Object | Nil; o.keys() -> Array; o.values() -> Iterator; o.has(key: String) -> Bool; o.get(key, fallback) -> value; o.get_or_put(key, init) -> value (破壊的); o.remove(key: String) -> Nil (破壊的)

**セットのメソッド** — size(); empty() -> Bool; presence(); contains(x) -> Bool; union(b); intersect(b); diff(b); sym_diff(b); subset(b) -> Bool; superset(b) -> Bool; to_array(); iter(); add(x); remove(x)

**イテレータメソッド** — it.map(f); it.filter(p); it.take(n); it.skip(n); it.take_while(p); it.skip_while(p); it.step_by(n); it.distinct(); it.tap(f); it.scan(init, f); it.flatten(); it.chunk_by(f); it.chunks(n); it.windows(n); it.flat_map(f); it.chain(other); it.zip(other); it.enumerate(); it.collect(); it.join(sep); it.for_each(f); it.reduce(init, f); it.find(p); it.any(p); it.all(p); it.count(); it.first(); it.last(); it.nth(n); it.position(p); it.contains(v); it.sum(); it.product(); it.min(); it.max(); it.min_by(f); it.max_by(f); it.to_set(); it.to_object(); it.group_by(f); it.partition(p); it.unzip()

**コア組み込み関数** — to_long(v: Any, *, base: Long = 10) -> Long; to_float(v: Any) -> Float; to_string(v: Any) -> String; type_of(v: Any) -> String; range(n: Long, *, step: Long = 1) -> Iterator; range(start: Long, end: Long, *, step: Long = 1) -> Iterator; iota(n: Long) -> Array; iota(start: Long, end: Long) -> Array; repeat(n: Long, value: Any) -> Array; grid(x_range: Range, y_range: Range) -> Iterator

**Math** — Math.pi; Math.e; Math.inf; Math.nan; Math.abs(x: Long|Float) -> Long|Float; Math.min(a, b, ...) -> Long|Float; Math.max(a, b, ...) -> Long|Float; Math.log(x: Long|Float) -> Float; Math.exp(x: Long|Float) -> Float; Math.sqrt(x: Long|Float) -> Float; Math.sin(x) -> Float; Math.cos(x) -> Float; Math.tan(x) -> Float; Math.asin(x) -> Float; Math.acos(x) -> Float; Math.atan(x) -> Float; Math.atan2(y, x) -> Float; Math.floor(x: Long|Float) -> Long; Math.ceil(...) -> Long; Math.round(...) -> Long; Math.pow(base: Long, exp: Long) -> Long; Math.sign(x: Long) -> Long; Math.clamp(x: Long|Float, lo: Long|Float, hi: Long|Float) -> Long|Float; Math.wrap(x: Long, n: Long) -> Long

**IO** — IO.inspect(x: Any) -> Nil; IO.print(x: Any) -> Nil; IO.println(x: Any = '') -> Nil; IO.input() -> String; IO.stdin() -> reader; .read(); .read(n: Long); .lines(); IO.einspect(x: Any) -> Nil; IO.eprint(x: Any) -> Nil; IO.eprintln(x: Any) -> Nil; IO.stdin_is_terminal() -> Bool; IO.stdout_is_terminal() -> Bool; IO.stderr_is_terminal() -> Bool; IO.capture(f: Function) -> String

**FS** — FS.join(parts...: String) -> String; FS.sep() -> String; FS.basename(path: String) -> String; FS.dirname(path: String) -> String; FS.extension(path: String) -> String; FS.stem(path: String) -> String; FS.read(path: String) -> String; FS.write(path: String, content: String) -> Nil; FS.exists(path: String) -> Bool; FS.is_file(path: String) -> Bool; FS.is_dir(path: String) -> Bool; FS.size(path: String) -> Long; FS.list_dir(path: String) -> Array<String>; FS.mkdir(path: String) -> Nil; FS.temp_dir() -> String; FS.mkdtemp(prefix: String) -> String; FS.remove(path: String, recursive: Bool = false) -> Nil; FS.rename(src: String, dst: String) -> Nil; FS.copy(src: String, dst: String, recursive: Bool = false) -> Nil; FS.chmod(path: String, mode: Long) -> Nil; FS.chown(path: String, owner = nil, group = nil) -> Nil; FS.stat(path: String) -> Object; FS.walk(path: String) -> Array<String>; FS.glob(pattern: String) -> Array<String>; FS.watch(path: String, recursive: Bool = true, match: Array? = nil) -> WatchHandle; FS.abspath(path: String) -> String; FS.realpath(path: String) -> String; FS.normpath(path: String) -> String; FS.is_abs(path: String) -> Bool; FS.symlink(target: String, link: String) -> Nil; FS.readlink(path: String) -> String; FS.is_symlink(path: String) -> Bool; p.resolve(); p.mkdir(); p.remove(recursive=false); p.rename(dst); p.list(); p.glob(pattern); p.walk(); p.str()

**File** — File.open(path: String, mode: String = "r") -> File; File.with(path: String, mode: String = "r", fn: Function) -> Any; f.read() -> String; f.read(n: Long) -> String; f.lines() -> Iterator<String>; f.chunks(n: Long) -> Iterator<String>; f.write(data: String) -> Nil; f.flush() -> Nil; f.seek(offset: Long, whence: String = "set") -> Nil; f.tell() -> Long; f.close() -> Nil

**Time** — Time.now() -> Instant; Time.monotonic() -> Float; Time.sleep(secs: Float) -> Nil; Time.from_iso(s: String) -> Instant; Time.from_unix(secs: Long|Float) -> Instant; Time.from_parts(p: Object, utc: false) -> Instant; Time.parse(s: String, fmt: String) -> Instant; t.iso(utc: true) -> String; t.format(fmt: String, utc: false) -> String; t.parts(utc: false) -> Object; t.weekday(utc: false) -> Long; t.add(years=0, months=0, days=0, hours=0, minutes=0, seconds=0, utc: false) -> Instant; t.start_of(unit: String, utc: false) -> Instant; t.unix() -> Float; t.unix_nanos() -> Long; d.seconds() / .milliseconds() / .minutes() / .hours() / .days() -> Float; d.abs() -> Duration

**Random** — Random.seed(n: Long) -> Nil; Random.int(lo: Long, hi: Long) -> Long; Random.uniform(lo: Float, hi: Float) -> Float; Random.gauss(mu: Float, sigma: Float) -> Float; Random.shuffle(a: Array) -> Nil; Random.weighted_choice(pop: Array, weights: Array) -> Any; Random.choice(pop: Array) -> Any

**Sys** — Sys.exit(code: Long) -> Nil; Sys.env(name: String, fallback = '') -> Any; Sys.set_env(name: String, value: String) -> Nil; Sys.getcwd() -> String; Sys.chdir(path: String) -> Nil; Sys.data_dir(app: String) -> String; Sys.time() -> Float

**Tensor** — Tensor.zeros(...) -> Tensor; Tensor.ones(...); Tensor.randn(...); Tensor.from(arr: Array) -> Tensor; Tensor.concat(parts: Array, axis: Long = 0) -> Tensor; Tensor.where(cond: Tensor, a, b) -> Tensor; Tensor.index_add(indices: Tensor, values: Tensor, target_shape: Array) -> Tensor; Tensor.scatter_to_axis(indices: Tensor, values: Tensor, size: Long) -> Tensor; Tensor.from_csv(path: String) -> Tensor; Tensor.eval(t1, t2, ...) -> Nil; .shape() -> Array; .dot(other: Tensor) -> Tensor; .linear_sigmoid(x, b) -> Tensor; .pow(exp) -> Tensor; .gt(other) / .lt(other) / .ge(other) / .le(other) / .eq(other) / .ne(other) -> Tensor; .tanh() / .sin() / .cos() -> Tensor; .clamp(lo, hi) -> Tensor; .rope(pos: Long, base) -> Tensor; .transpose() -> Tensor; .permute(axes: Array) -> Tensor; .slice(start, end) -> Tensor; .narrow(params: Array) -> Tensor; .reshape(dims: Array) -> Tensor; .unfold(params: Array) -> Tensor; .pad(params: Array) -> Tensor; .fold(params: Array) -> Tensor; .sum() -> Float; .sum(axis: Long?) -> Tensor; .mean() / .mean(axis); .max() / .max(axis); .argmax(axis: Long) -> Tensor; .index_select(indices: Tensor) -> Tensor; .to_array() -> Array; .item() -> Float; .requires_grad() -> Tensor; .backward() -> Nil; .grad() -> Tensor; .zero_grad() -> Nil; .detach() -> Tensor; Tensor.use_cpu() -> Nil; Tensor.use_gpu() -> Nil; Tensor.use_auto() -> Nil; Tensor.gpu_available() -> Bool; Tensor.device() -> String

**JSON** — JSON.stringify(v, indent=0, sort_keys=false, lines=false) -> String; JSON.parse(s, lines=false, number_mode='auto', jsonc=false) -> Any

**Args** — Args.parse(argv: Array<String>, spec: Object) -> Object; Args.try_parse(argv, spec) -> Object; Args.help(spec: Object) -> String

**Proc** — Proc.run(cmd: Array<String>, cwd=nil, env=nil, stdin="", check=false, timeout=0, share=nil, inherit_env=true) -> Object; Proc.all(commands: Array<Array<String>>, limit: Long = <CPU数>, timeout: Long = 0, fail_fast: Bool = false, retries: Long = 0, share: Object? = nil, inherit_env: Bool = true) -> Array<Object>; Proc.race(commands: Array<Array<String>>, share: Object? = nil, inherit_env: Bool = true) -> Object; Proc.spawn(cmd: Array<String>, cwd=nil, env=nil, stdin="", share=nil, inherit_env=true) -> handle; h.wait(); h.poll(); h.kill(sig = 15)

**Isolate** — Isolate.spawn(fn, *args) -> handle; h.join(); h.poll(); Channel.new(cap = 1) -> (tx, rx); tx.send(v); rx.recv(); rx.try_recv(); rx.drain(max = nil); Channel.fan_in(sources: [rx]) -> rx; Channel.fan_in(items, fn) -> rx; Parallel.map(items, fn, limit = <コア数>); Parallel.each(items, fn, limit = <コア数>); Parallel.map_settled(items, fn, limit = <コア数>); Parallel.race(items, fn, limit = <コア数>); Signal.notify(tx); Signal.reset(); SharedBuffer.new(count, Class) -> buffer; SharedBuffer.file(path, count, Class) -> buffer; SharedBuffer.shared(count, Class) -> buffer; buffer.with_lock(fn)

**Matchers** — assert_true(x: Bool, label: String? = nil) -> Nil; assert_false(x: Bool, label: String? = nil) -> Nil; assert_eq(a, b, label: String? = nil) -> Nil; assert_ne(a, b, label: String? = nil) -> Nil; assert_lt(a, b, label: String? = nil) -> Nil; assert_le(a, b, label: String? = nil) -> Nil; assert_gt(a, b, label: String? = nil) -> Nil; assert_ge(a, b, label: String? = nil) -> Nil; assert_throws(kind: String, f: Function) -> Nil; assert_close(a: Float, b: Float, tol: Float, label: String? = nil) -> Nil

**Regex** — Regex.compile(pat) -> Regex; Regex.compile(pat, flags) -> Regex; Regex.escape(s) -> String; Regex.interp(x) -> String; Regex.find(pat, s); Regex.match(pat, s); Regex.find_all(pat, s); Regex.test(pat, s); Regex.split(pat, s); Regex.replace_all(pat, s, repl) -> String; Regex.replace_first(pat, s, repl) -> String; re.test(s) -> Bool; re.find(s); re.match(s); re.find_all(s) -> [Match]; re.find_all_str(s) -> [String]; re.find_all_index(s) -> [Int]; re.count(s) -> Int; re.find_iter(s) -> Iterator<Match>; re.replace_all(s, repl) -> String; re.replace_first(s, repl) -> String; re.split(s) -> [String]

**Http** — json(); Http.get(url, headers=nil, timeout=0, follow_redirects=true); Http.delete(url, headers=nil, timeout=0, follow_redirects=true); Http.head(url, headers=nil, timeout=0, follow_redirects=true); Http.post(url, body="", content_type="text/plain", headers=nil, timeout=0, follow_redirects=true); Http.put(url, body="", content_type="text/plain", headers=nil, timeout=0, follow_redirects=true); Http.request(method, url, body="", content_type="text/plain", headers=nil, timeout=0, follow_redirects=true); Http.sse(url, on_event, headers=nil, timeout=0, follow_redirects=true); Http.client(base_url, headers=nil, timeout=0, follow_redirects=true); Http.server(); Http.ws(url); Http.sse(url, on_event, headers=nil, timeout=0, follow_redirects=true) -> Object; Http.client(base_url, headers=nil, timeout=0, follow_redirects=true) -> Object; Http.server() -> Object; static(mount, dir); sink.write(chunk); bind(port, host="0.0.0.0") -> Long; serve(workers=0); serve_async(workers=0); listen(port, host="0.0.0.0", workers=0); listen_async(port, host="0.0.0.0", workers=0) -> Long; stop(); close(); ws.receive(); ws.send(msg); ws.close(); ws.is_open(); Http.ws(url) -> Object; dir.read(path); dir.exists(path)

**Encoding** — Encoding.html; Encoding.html.escape(s) -> String; Encoding.html.unescape(s) -> String; Encoding.base64; Encoding.base64.encode(s) -> String; Encoding.base64.decode(s) -> String; Encoding.hex; Encoding.hex.encode(s) -> String; Encoding.hex.decode(s) -> String; Encoding.url; Encoding.url.encode(s) -> String; Encoding.url.decode(s) -> String

**Compress** — Compress.gzip(data: String) -> String; Compress.gunzip(data: String) -> String; Compress.deflate(data: String, level: Long = -1) -> String

**Hash** — Hash.sha256(data: String) -> String; Hash.sha1(data: String) -> String; Hash.sha512(data: String) -> String; Hash.md5(data: String) -> String; Hash.hmac_sha256(key: String, data: String) -> String; Hash.hmac_sha1(key: String, data: String) -> String; Hash.hmac_sha512(key: String, data: String) -> String

**CSV** — CSV.parse(text, delimiter=",", header=false, types=nil) -> Array; CSV.stringify(rows: Array, delimiter: String = ",") -> String

**Env** — Env.parse(text: String) -> Object; Env.load(path: String = ".env", override: Bool = false) -> Object

**UUID** — UUID.v4() -> String; UUID.v7() -> String

**Term** — Term.fg(s, n) -> String; Term.bg(s, n) -> String; Term.rgb(s, r, g, b) -> String; Term.style(fg:, bg:, bold:, dim:, underline:, reverse:) -> String; Term.clear() -> String; Term.move(x, y) -> String; Term.size() -> (Long, Long); Term.width(s) -> Long; Term.flush(); screen.clear(); screen.set(x, y, glyph, style = ""); screen.put(x, y, s, style = ""); screen.render() -> String; screen.flush(); screen.poll(timeout) -> Object?

**Log** — Log.debug(msg: String, fields: Object = {}); Log.info(msg, fields = {}); Log.warn(msg, fields = {}); Log.error(msg, fields = {}); Log.with(fields: Object) -> logger; Log.set_level(level: String) -> Nil; Log.set_format(format: String) -> Nil

**TOML** — TOML.parse(text: String) -> Object; TOML.stringify(v: Object, sort_keys: Bool = false) -> String

**SQLite** — SQLite.open(path: String) -> Database; SQLite.version() -> String; db.execute(sql: String, params = nil) -> Long; db.query(sql: String, params = nil) -> Array<Object>; db.prepare(sql: String) -> Statement; db.transaction(fn: Function) -> Any; db.close(); stmt.run(params = nil) -> Long; stmt.query(params = nil) -> Array<Object>; stmt.finalize()

**Canvas** — Canvas.init(w, h); Canvas.clear(color); Canvas.set_pixel(x, y, color); Canvas.get_pixel(x, y) -> Long; Canvas.rect(x, y, w, h, color, fill = true); Canvas.line(x1, y1, x2, y2, color); Canvas.circle(cx, cy, r, color, fill = true); Canvas.ellipse(cx, cy, rx, ry, color, fill = true); Canvas.triangle(x1, y1, x2, y2, x3, y3, color, fill = true); Canvas.polygon(points, color, fill = true); Canvas.to_png() -> String; Canvas.present(); Canvas.title(name); Canvas.toggle_fullscreen(); Canvas.fullscreen() -> Bool; Canvas.resizable(enabled); Canvas.resized() -> Bool; Canvas.cursor_hidden() -> Bool; Canvas.clipboard() -> String; Canvas.set_clipboard(text); Canvas.quit(); Canvas.can_quit() -> Bool; sprite.draw(x, y, flip_x = false, flip_y = false, transpose = false); sprite.draw_sub(x, y, sx, sy, sw, sh, flip_x = false, flip_y = false, transpose = false); sprite.draw_scaled(x, y, w, h, flip_x = false, flip_y = false, smooth = false, alpha = 255); sprite.draw_sub_scaled(x, y, w, h, sx, sy, sw, sh, flip_x = false, flip_y = false, smooth = false, alpha = 255); sprite.to_png() -> String; font.draw(s, x, y, color, size); font.draw_screen(s, x, y, color, size); font.text_width(s, size) -> Long; font.ascent(size) -> Long; font.advance(codepoint, size) -> Long; Canvas.buttons() -> Long; Canvas.mouse() -> Object; Canvas.key(name) -> Bool; Canvas.key_queue() -> Array; Canvas.typed() -> String; Canvas.wheel() -> Float; input.update(); input.down(btn) -> Bool; input.pressed(btn) -> Bool; Canvas.pad_available(index = 0) -> Bool; Canvas.pad_axis(axis, index = 0) -> Float; Canvas.pad_held(button, index = 0) -> Bool; Canvas.pad_pressed(button, index = 0) -> Bool; Canvas.pad_name(index = 0) -> String; Canvas.pad_rumble(left, right, sec, index = 0); Canvas.pad_mappings(db) -> Bool; Canvas.rect_overlap(x1, y1, w1, h1, x2, y2, w2, h2) -> Bool; Canvas.circle_overlap(x1, y1, r1, x2, y2, r2) -> Bool; Canvas.point_in_rect(px, py, x, y, w, h) -> Bool; Canvas.point_in_circle(px, py, cx, cy, r) -> Bool; sound.play(vol = 100); sound.stop(); sound.playing() -> Bool; Canvas.music(data, loop = true, vol = 100, start = 0.0); Canvas.music_stop(); Canvas.music_volume(vol); Canvas.music_seek(seconds); Canvas.music_playing() -> Bool; Canvas.dt() -> Float; Canvas.target_fps(n); Canvas.fps() -> Long

**Scene (experimental)** — view.target_fps(fps); view.closing() -> Bool; view.dt() -> Float; view.camera(px,py,pz, tx,ty,tz, ux,uy,uz, fov); view.render_3d(); view.begin2d(); view.present(); node.move(x, y, z); node.tint(r, g, b); node.material(id); view.material(r, g, b) -> id; view.material_pbr(r, g, b, metallic, roughness) -> id; view.background(r, g, b); view.sky(tr,tg,tb, br,bg,bb); view.sun(dx,dy,dz, intensity, r,g,b); view.ambient(intensity, r, g, b); view.fog(start, end, r, g, b); view.screenshot(path); view.text(s, x, y, size, r, g, b); view.rect(x, y, w, h, r, g, b); view.circle(x, y, radius, r, g, b); view.line(x0, y0, x1, y1, thick, r, g, b); view.alpha(a); view.held(key) -> Bool; view.pressed(key) -> Bool; view.pad_available() -> Bool; view.pad_axis(n) -> Float; view.rumble(left, right, sec)

**Net** — Net.connect(host: String, port: Long, timeout: Long = 0) -> Socket; read(n = nil); read_line(); read_exact(n); lines(); write(data); shutdown_write(); set_timeout(ms); set_nodelay(on = true); Net.listen(port: Long, host: String = "0.0.0.0", backlog: Long = 0) -> Listener; accept(); serve(handler, workers = 0); listener.serve(handler, workers = 0); Net.udp(port: Long = 0, host: String = "0.0.0.0") -> UdpSocket; send_to(data, host, port); recv_from(max = 65536); set_broadcast(on = true); Net.resolve(host: String) -> Array<String>

**Desktop / Webview** — Desktop.run(config: Object) -> Nil; Webview.Window.new(); w.set_title(title); w.set_size(width, height); w.set_html(html); w.navigate(url); w.run(); w.terminate(); Webview.Window.quit(); Webview.Window.is_running()

**Vector2** — Vector2.new(x, y); a.hash() -> Long; a.dot(b); a.normalized(); a.distance_to(b); a.distance_squared_to(b) -> Float

**Vector3** — Vector3.new(x, y, z); a.hash() -> Long; a.dot(b); a.normalized(); a.distance_to(b); a.distance_squared_to(b) -> Float

**Deque** — Deque.new(); d.push_back(x); d.push_front(x); d.pop_back() -> Any; d.pop_front() -> Any; d.peek_back() -> Any; d.peek_front() -> Any; d.size(); d.empty(); d.to_array() -> Array; d.iter() -> Iterator

**PriorityQueue** — PriorityQueue.new(*, key: Function | Nil = nil, reverse: Bool = false); pq.push(x); pq.pop() -> Any; pq.peek() -> Any; pq.size(); pq.empty()

**Peg** — Peg.compile(grammar) -> Peg; Peg.compile(grammar, start); Peg.compile(grammar, start, optimize); Peg.compile(grammar, start, optimize, packrat); Peg.check(grammar) -> Nil; Peg.check(grammar, start); p.parse(text); p.parse(text, path); p.parse(text, path, actions); p.test(text) -> Bool; Peg.parse(grammar, text); Peg.parse(grammar, text, start, optimize, packrat, path, actions); Peg.test(grammar, text); Peg.test(grammar, text, start, packrat); Peg.walk(node) -> Iterator; Peg.find(node, name); Peg.find_all(node, name) -> [Node]; Peg.str(node) -> String

**CodeGen** — CodeGen.Module.new(); m.literal(v:, line:, col:); m.var_ref(kind:, index:, line:, col:); m.unary(op:, operand:, line:, col:); m.binary(op:, lhs:, rhs:, line:, col:); m.assign(kind:, index:, value:, line:, col:); m.make_if(cond:, then_branch:, line:, col:); m.make_if_else(cond:, then_branch:, else_branch:, line:, col:); m.make_while(cond:, body:, line:, col:); m.block(stmts_list:, line:, col:); m.call(func:, cmap:, line:, col:); m.make_closure(func:, cmap:, line:, col:); m.call_value(callee:, args_list:, line:, col:); m.intrinsic(name:, args_list:, line:, col:); m.scope(first_local:, end_local:, body:, line:, col:); m.make_return(value:, line:, col:); m.make_throw(value:, line:, col:); m.make_try(caught_local:, body:, handler:, line:, col:); m.make_defer(value:, line:, col:); m.cell_fresh(cell:, line:, col:); m.make_yield(value:, line:, col:); m.set_generator(func:); m.set_lenient_arity(func:); m.list_new(); m.list_push(list:, value:); m.add_func(name:, num_locals:, num_captures:, num_cells:, num_params:, body:); m.set_local_name(func:, index:, name:); m.set_capture_name(func:, index:, name:); m.capture_map_new(); m.capture_map_push(cmap:, kind:, index:); m.add_capture_map(cmap:); m.verify(); m.run(); m.dump_ir(); m.dump_bc()

<!-- END GENERATED -->

## 5. テンプレート

### CLI ツール

```culebra
# doctest: skip
spec = {
  name: 'greet',
  args: [
    {name: 'who', doc: 'who to greet'},
    {name: 'times', type: 'Long', default: 1, doc: 'repeat count'},
  ],
}
args = Args.parse(Sys.argv, spec)
for _ in range(args.times) {
  println("hello, {args.who}")
}
```

### HTTP リクエスト

```culebra
# doctest: skip
r = Http.get('https://example.com/api', headers: {Accept: 'application/json'})
if !r.ok {
  throw "HTTP {r.status}: {r.reason}"
}
println(r.json().title)
```

### ファイル処理

```culebra
# doctest: skip
counts = File.with('input.txt', 'r', fn (f) {
  mut n = {}
  for line in f.lines() {
    for w in line.trim().lower().split(' ') {
      k = w.to_string()
      n[k] = n.get(k, 0) + 1
    }
  }
  n
})
for k in counts.keys().sorted() {
  println("{k}\t{counts[k]}")
}
```

### テストファイル

```culebra
# doctest: skip
# test_math.cul
@test
fn adds() {
  assert_eq(1 + 2, 3)
}

@parametrize([(1, 2, 3), (10, 20, 30)])
fn adds_each(a, b, want) {
  assert_eq(a + b, want)
}
```

次に読むもの
------------

- 理由付きのチュートリアル: [`handbook.ja.md`](handbook.ja.md)
- 形式文法と評価規則: [`language.ja.md`](language.ja.md)
- 散文付きの完全なAPIリファレンス: [`stdlib.ja.md`](stdlib.ja.md)
- バイナリビルドと埋め込み: [`deployment.ja.md`](deployment.ja.md)
