Culebra ガイド
================

Rust 風シンタックスを持つ小さな動的型付けスクリプト言語。 ツリー
ウォーキング型インタプリタと LLVM ORC JIT の 2 バックエンドが 1 つの
AST を共有します。 このガイドは "hello" から C++ ホストへの埋め込み
までを案内します。 厳密な文法は [`language.ja.md`](language.ja.md)、
API リファレンスは [`stdlib.ja.md`](stdlib.ja.md)、 実装の内部詳細は
[`internals.md`](internals.md) (英語のみ) を参照してください。

> **doctest 規約。** 本ガイドの各 ` ```culebra ` ブロックは実行可能
> な例です。 行末の `# => <value>` は標準出力の期待値、`# !! <pattern>`
> は `throw` の期待値。 ブロック先頭の `# doctest: skip` は説明用
> （多くは *Planned* 機能）。 ブロック間は独立スコープです。

> **Status ラベル。** ラベル無しの見出しは現時点の実装を記述します。
> 出現するラベル: **Draft** (実装中、API 変更あり)、**Planned**
> (採用決定、未実装)、**Deprecated** (将来削除予定)。 採用せずと
> 決定した機能は [`internals.md` §13](internals.md) に集約。

目次
----

- **第 I 部 — 言語コア**
  1. [Hello & セットアップ](#1-hello--セットアップ)
  2. [値・束縛・制御フロー](#2-値束縛制御フロー)
  3. [関数とクロージャ](#3-関数とクロージャ)
  4. [文字列](#4-文字列)
  5. [イテレータ](#5-イテレータ)
  6. [パターンマッチ](#6-パターンマッチ)
  7. [エラー処理と RAII](#7-エラー処理と-raii)
- **第 II 部 — 抽象化の道具**
  8. [クラス](#8-クラス)
  9. [演算子オーバーロード](#9-演算子オーバーロード)
  10. [UFCS とマルチメソッド](#10-ufcs-とマルチメソッド)
  11. [デコレータ](#11-デコレータ)
  12. [モジュール](#12-モジュール)
- **第 III 部 — 型とライブラリ**
  13. [型システム](#13-型システム)
  14. [標準ライブラリ巡り](#14-標準ライブラリ巡り)
  15. [Tensor プリミティブ](#15-tensor-プリミティブ)
- **第 IV 部 — 検証とデプロイ**
  16. [テスト (`culebra test`)](#16-テスト-culebra-test)
  17. [リント (`culebra lint`)](#17-リント-culebra-lint)
  18. [フォーマット (`culebra fmt`)](#18-フォーマット-culebra-fmt)
  19. [AOT バイナリビルド](#19-aot-バイナリビルド)
  20. [埋め込み概観](#20-埋め込み概観)

0. 設計哲学
-----------

ここを 1 度読めば、以降の章は前提知識として扱えます。

- **2 バックエンド、1 AST。** ツリーウォーキング型インタプリタと
  LLVM ORC JIT が同じ AST を共有。 インタプリタは LLVM 非依存
  (~1 MB バイナリ、埋め込み向き)、JIT は `-O2` で同じプログラムを
  実行。 両方を維持 — どちらも捨てません。
- **8 つの組み込み型。** `Nil` / `Bool` / `Long` / `Float` / `String` /
  `Array` / `Object` / `Function`。 クラス・モジュール・エラーなど
  はすべて `Object` 上に構築。
- **Rust 風の表面構文。** `let` / `mut` / `fn` / `match` / ブロッ
  クは式。 クロージャは第一級、エラーは値、隠れたグローバル無し。
- **UFCS、パイプライン不採用。** 任意の自由関数 `f(x, ...)` を
  `x.f(...)` として呼べる。 パイプライン演算子は検討の上不採用 (詳細
  は [`internals.md` §13](internals.md))。
- **暗黙 import、明示 `import` 文無し。** トップレベル識別子への
  bare な参照がモジュールビルド内でファイル境界を越える (Ch.12)。
  明示 `import` は検討の上不採用。
- **async/await 無し。** I/O はブロッキング設計、並行はスレッドで。
  HTTP 等のネットワークスタックはブロッキング、典型的なスケール
  上限は数千接続。
- **batteries-included、ティア制。** コア stdlib
  (Math/IO/Sys/Random/String/FS/Time/Args) は出荷済み。 Tier 1
  (Regex/HTTP/Hash+Encoding) が次の優先 — Ch.14 参照。
- **1.0 前。** ソース・API は変わる可能性。 リリース機構 (バージョン
  タグ、CHANGELOG、Homebrew formula 等) は 1.0 後。

---

第 I 部 — 言語コア
==================

1. Hello & セットアップ
-----------------------

インタプリタ (LLVM 17+ があれば JIT も) をビルド:

```bash
just build              # JIT 付き
just build-no-jit       # インタプリタのみ、~1 MB
just dev                # LTO 無しの高速ビルド → build-dev/ (内側ループ用)
just test-dev           # build-dev/ で interp==JIT を素早く確認 (各編集ごと)
just test               # 全 backend + embed スモークテスト (並列; JOBS=1 で逐次化)
./build/culebra --shell # REPL (--jit で JIT REPL)
```

Culebra ソースの拡張子は `.cul`。 `culebra` バイナリで実行:

```bash
echo "puts('hello, culebra!')" > hello.cul
./build/culebra hello.cul            # インタプリタ
./build/culebra --jit hello.cul      # JIT (出力は同じ)
./build/culebra --jit-faststart hello.cul # JIT・起動が速い
./build/culebra --help                    # 全オプション・コマンド一覧
```

3 バックエンドとも観測可能な出力は同一 (interp↔JIT の差分コーパス全件で
検証済み)。`--jit-faststart` は JIT の最適化バックエンドを FastISel に切り替え、
**JIT warmup (起動・コード生成時間) をほぼ半減**する代わりに steady-state を
少し犠牲にする — 純スクリプトの hot loop で約 7%、重い処理が C++/BLAS ランタイム
側 (例: `Tensor`) にある場合は ~0%。短命スクリプトや BLAS 律速の実行に向く。
既定の `--jit` (`-O2`) は steady-state スループットが最良。

コメントは `#` (行) または `/* ... */` (ブロック)。 文は `;` で
区切る (省略時は改行)。 行末 `;` は通常は省略。

```culebra
# this is a comment
puts('hello')          # => 'hello'
```

`puts` は値を *inspect* 形式で出力するため、文字列は引用符付き
(`'hello'`) で、参照型はそのリテラル表記で表示される。引用符の付かない
生のテキストが必要なときは `print` を使う (末尾の改行も付かない) — 第11章参照。

2. 値・束縛・制御フロー
-----------------------

### 2.1 8 つの型

```culebra
puts(type_of(nil))            # => 'Nil'
puts(type_of(true))           # => 'Bool'
puts(type_of(42))             # => 'Long'
puts(type_of(3.14))           # => 'Float'
puts(type_of('hi'))           # => 'String'
puts(type_of([1, 2]))         # => 'Array'
puts(type_of({a: 1}))         # => 'Object'
puts(type_of(fn () { 1 }))    # => 'Function'
```

### 2.2 束縛: bare / `let` / `mut`

```culebra
x = 10              # bare: 新規不変束縛、または外側を再代入
let y = 20          # let: 新規不変束縛 (外側のシャドウは不可)
mut z = 30          # mut: 新規可変束縛
z = z + 1           # mut は再代入可能
z += 1              # 複合 (`-= *= /= %= **= @=` も同様)
puts(z)             # => 32
```

bare 代入は外側スコープへ向かって検索し、最も近い同名束縛を再代入。
見つからなければ現スコープに新規束縛。 クロージャベースのオブジェクト
パターン (Ch.8) はこの挙動で成立しています。

> **注意:** 束縛はデフォルトで不変なので、裸の `x = 1` の後に同一
> スコープで `x = 2` と書くとエラーになります (`immutable variable
> 'x'`)。再代入したい変数には `mut` を付けます (`mut x = 1; x = 2`)。
> `let` は任意で、(元から不変の) 意図を明示し、かつ外側束縛のシャドウ
> を禁止するマーカーです。

### 2.3 シャドウ禁止

外側関数で捕捉された変数を `let` / `mut` / パラメータ / `match`
パターンで**シャドウすると**コンパイルエラーになります。 同関数内
の block-local な rebinding は許容。

```culebra
make = fn () {
  mut n = 0
  fn () {
    # let n = 1   # エラー: 捕捉された `n` をシャドウしてしまう
    n = n + 1     # bare 代入で捕捉 `n` を更新 — OK
    n
  }
}
c = make()
puts(c())                     # => 1
puts(c())                     # => 2
```

### 2.4 制御フロー

`if` と `match` (Ch.6) は式 — 選ばれた枝の値を返す。 `while` と
`for` は文 (値は `nil`)。

```culebra
x = 7
sign = if x > 0 { 1 } else if x < 0 { -1 } else { 0 }
puts(sign)                    # => 1

mut i = 0
while i < 3 { puts(i); i = i + 1 }
# => |
# 0
# 1
# 2

for n in 0..3   { puts(n) }   # 排他レンジ
# => |
# 0
# 1
# 2

for n in 0..=2  { puts(n) }   # 包含レンジ
# => |
# 0
# 1
# 2
```

`break` / `continue` は `while` / `for` 内で動作。

### Why シャドウ規則を軸ごとに分けるか

クロージャベースのオブジェクトパターンでは、捕捉された状態が
そのままオブジェクトの状態。 silent shadow はオブジェクトを壊す。
一方、関数内 block での rebinding は `let a = transform(a)` のよう
な日常パターン。 多くの言語は 1 つのポリシーを全軸に適用するが、
Culebra は各軸が違う目的を持つので分けて扱う。

3. 関数とクロージャ
-------------------

### 3.1 `fn` と `|x|`

```culebra
add = fn (a, b) { a + b }
puts(add(2, 3))               # => 5

# 型注釈はオプション; 詳細は Ch.13
add_typed = fn (a: Long, b: Long) -> Long { a + b }
puts(add_typed(2, 3))         # => 5

# |x| expr は fn (x) { expr } の糖衣
square = |x| x * x
puts(square(7))               # => 49

# 再帰には `self` (関数自身への参照)
fib = fn (x) {
  if x < 2 { x } else { self(x - 2) + self(x - 1) }
}
puts(fib(10))                 # => 55
```

### 3.2 クロージャ

内側関数は外側束縛を参照で捕捉する。 `mut` を付けると書き換え可能。

```culebra
make_counter = fn () {
  mut n = 0
  fn () { n = n + 1; n }   # bare `n = ...` で捕捉 `n` を更新
}
c = make_counter()
puts(c())                     # => 1
puts(c())                     # => 2
puts(c())                     # => 3
```

### 3.3 キーワード引数と `**splat`

パラメータにはデフォルト値を宣言可能。 最後の positional の後の
`*` は以降を keyword-only にする。 `**rest` は未指定 keyword を
Object にまとめる。

```culebra
greet = fn (name, *, greeting = 'hi', **opts) {
  prefix = if opts.has('formal') && opts.formal { 'Mr./Ms. ' } else { '' }
  "{greeting}, {prefix}{name}"
}
puts(greet('alice'))                       # => 'hi, alice'
puts(greet('alice', greeting: 'hello'))    # => 'hello, alice'
puts(greet('bob', formal: true))           # => 'hi, Mr./Ms. bob'

# `**` で Object をキーワード引数として splat
opts = {greeting: 'yo', formal: false}
puts(greet('carol', **opts))               # => 'yo, carol'
```

### Why keyword-only

`*` マーカーは呼び出し側にオプション名を書かせるので、長いパラメー
タリストが読みやすくなり、再配置・拡張もコール側を壊さない。 free
な positional rest (`*args`) は意図的に不採用 — Array リテラルが
その役割を果たす。

4. 文字列
---------

### 4.1 補間と連結

```culebra
name = 'Culebra'
puts("hello, {name}!")                   # => 'hello, Culebra!'
puts("two plus three is {2 + 3}")        # => 'two plus three is 5'
puts('a' + 'b' + 'c')                    # => 'abc'
```

### 4.2 反復とインデックス

文字列は Unicode スカラ単位で反復 (1 コードポイント = 1 ステップ)。
インデックスは UTF-8 上のバイトオフセット — 範囲外はエラー。

```culebra
for c in 'café' { puts(c) }
# => |
# 'c'
# 'a'
# 'f'
# 'é'

puts('café'.size())            # => 5
```

`size()` は UTF-8 表現上のバイト数を返す (`é` は 2 バイトなので `'café'`
は 5)。一方、上の `for` ループは Unicode スカラ単位で 1 ステップずつ進む
(4 ステップ)。

### 4.3 よく使うメソッド

```culebra
puts('hello world'.split(' '))        # => ['hello', 'world']
puts('  hi  '.trim())                 # => 'hi'
puts('abc'.upper())                   # => 'ABC'
puts('foo'.starts_with('fo'))         # => true
puts(['a', 'b', 'c'].join('-'))       # => 'a-b-c'
```

完全な一覧は [`stdlib.ja.md` §String](stdlib.ja.md)。

### 4.4 `StringView`、`StringLike`、graphemes() lazy

> **Status: Planned.** 未実装。 設計議論は
> [`internals.md` §6](internals.md) (英語) 参照。 これらが入ると、
> ユーザコードが `String` でも借用でも copy 無しで受け取れ、
> grapheme cluster を lazy に走査できるようになる。

```culebra
# doctest: skip
# パラメータ型 StringLike は String と StringView 両方を受ける:
print_first_grapheme = fn (s: StringLike) {
  for g in s.graphemes() { puts(g); break }
}
print_first_grapheme('🇯🇵 hello')   # planned: => 🇯🇵
```

### Why Go 流のバイトインデックス

Swift / Python 3 は bytes vs scalar の区別を不透明な `Character` /
`str` インデックスで隠す。 ソケット・ファイル I/O と相互運用する
までは便利だが、その時点で破綻する。 Go はバイトオフセットを露出
させ、`rune` 反復をその上に置く。 Culebra は同じモデル — I/O 意味
論が予測可能で、スカラ反復は欲しい時に、(将来) lazy grapheme 反復
は表示用に使える。

5. イテレータ
-------------

### 5.1 `range` (lazy) と `iota` (eager)

```culebra
# range は何も構築しない; for ループが lazy に消費
for i in range(3) { puts(i) }
# => |
# 0
# 1
# 2

# iota は Array を割り当てる
puts(iota(3))                 # => [0, 1, 2]
puts(iota(2, 5))              # => [2, 3, 4]
```

### 5.2 遅延チェイン

`.iter()` で Array を lazy iterator に。 チェインは consumer
(`collect` / `reduce` / `find` 等) で止まり、中間 Array は作らない。

```culebra
result = range(1000)
  .filter(|x| x % 2 == 0)
  .map(|x| x * 3)
  .take(5)
  .collect()
puts(result)                  # => [0, 6, 12, 18, 24]

total = range(1, 11).reduce(0, |a, x| a + x)
puts(total)                   # => 55

puts([1, 2, 3, 4].iter().any(|x| x > 3))      # => true
puts([10, 20, 30].iter().find(|x| x > 15))    # => 20
```

### 5.3 `enumerate` / `zip` / `flat_map` / `skip` / `take_while`

```culebra
for i, v in ['fizz', 'buzz', 'bang'].enumerate() {
  puts("{i}: {v}")
}
# => |
# '0: fizz'
# '1: buzz'
# '2: bang'

for p in [1, 2, 3].iter().zip(['a', 'b', 'c']) {
  puts("{p.first} / {p.second}")
}
# => |
# '1 / a'
# '2 / b'
# '3 / c'

flat = [[1, 2], [3], [4, 5, 6]].iter().flat_map(|xs| xs).collect()
puts(flat)                    # => [1, 2, 3, 4, 5, 6]

head = range(100).skip(10).take_while(|x| x < 15).collect()
puts(head)                    # => [10, 11, 12, 13, 14]
```

### 5.4 ユーザー定義イテレータ

3 メソッドを実装: `iter()` (慣習でイテレータ自身を返す)、
`has_next()` (`Bool` を返す)、`next()` (次の要素を返す)。

```culebra
countdown = fn (start) {
  mut i = start
  {
    iter:     fn () { this },
    has_next: fn () { i > 0 },
    next:     fn () { v = i; i = i - 1; v }
  }
}

for v in countdown(3) { puts(v) }
# => |
# 3
# 2
# 1
```

### 5.5 ジェネレータ (`yield`)

> **Status: Planned.** `yield` ベースのジェネレータ構文はロードマップ
> 上。 現状は手書きイテレータ (5.4) か `range` / `iter()` 合成
> (5.1–5.3) で代替。

### Why パイプライン `|>` を採用しないか

`x.f(...)` は同じ読み方ができる上、ユーザ型上の自由関数の解決経路
(Ch.10) も兼ねる。 `|>` を加えるとイディオム空間が分裂するだけで
機能的な得は無い。

6. パターンマッチ
-----------------

### 6.1 基本

```culebra
describe = fn (x) {
  match x {
    0                  => 'zero',
    1 | 2 | 3          => 'small',
    n: Long if n > 100 => "big ({n})",
    n: Long            => "int ({n})",
    s: String          => "str ({s})",
    true               => 'TRUE',
    false              => 'FALSE',
    nil                => 'NIL',
    _                  => 'other'
  }
}
puts(describe(0))             # => 'zero'
puts(describe(2))             # => 'small'
puts(describe(999))           # => 'big (999)'
puts(describe('hi'))          # => 'str (hi)'
puts(describe([1]))           # => 'other'
```

### 6.2 式として

`match` は値を生む — 計算式の中で使える。

```culebra
classify = fn (n: Long) -> Long {
  match n {
    n if n < 0 => -1,
    0          => 0,
    _          => 1
  }
}
puts(classify(-5))            # => -1
puts(classify(0))             # => 0
puts(classify(7))             # => 1
```

### 6.3 分解

```culebra
shape = fn (a) {
  match a {
    []              => 'empty',
    [x]             => "one ({x})",
    [x, y]          => "two ({x},{y})",
    [head, ...tail] => "head={head}, rest={tail.size()}",
  }
}
puts(shape([]))               # => 'empty'
puts(shape([10, 20]))         # => 'two (10,20)'
puts(shape([1, 2, 3, 4]))     # => 'head=1, rest=3'

first_name = fn (people) {
  match people {
    [{name}, ..._] => name,
    _              => 'none'
  }
}
puts(first_name([{name: 'x'}, {name: 'y'}]))     # => 'x'
puts(first_name([]))                              # => 'none'
```

### 6.4 再帰

```culebra
is_even = fn (n) {
  match n {
    0 => true,
    1 => false,
    _ => self(n - 2)
  }
}
puts(is_even(10))             # => true
puts(is_even(7))              # => false
```

### Why exhaustiveness check 無し

静的型システム無しで Object の shape を網羅性検査するには、節約
以上のランタイムコストがかかる。 `_` 節 (またはガード付き最終
パターン) で意図を明示する方針。 型システムが育てば (Ch.13)、
Union の網羅性検査が現実的になる。

7. エラー処理と RAII
--------------------

### 7.1 `throw` / `try` / `catch`

throw される値は任意の Culebra 値 — String、Object、何でも可。

```culebra
validate = fn (x) {
  if x < 0 { throw "negative: {x}" }
  x
}

try {
  puts(validate(42))          # => 42
  puts(validate(-1))          # throws、次の行は到達せず
  puts('unreached')
} catch e {
  puts("caught: {e}")         # => 'caught: negative: -1'
}
```

### 7.2 `try` を式として

```culebra
validate = fn (x) {
  if x < 0 { throw "negative: {x}" }
  x
}
safe = fn (x) {
  try { validate(x) } catch _ { 0 }
}
puts(safe(7))                 # => 7
puts(safe(-99))               # => 0
```

### 7.3 `defer`

`defer { ... }` は囲むブロックの**全 exit パス** (通常終了 /
`return` / `throw`) で LIFO に実行されるクリーンアップ登録。
保護対象を `{ }` で囲むことで、本体が throw してもクリーンアップが
発火する。

```culebra
demo = fn (fail) {
  {
    defer { puts('cleanup A') }
    defer { puts('cleanup B') }
    if fail { throw 'failed' }
    puts('work done')
  }
}

demo(false)
# => |
# 'work done'
# 'cleanup B'
# 'cleanup A'
```

### 7.4 `drop` による RAII

オブジェクトが no-arg Function 型の `drop` プロパティを持つと、
最後の参照が消えた時点でランタイムが自動で呼ぶ。 `drop` はファク
トリ関数で組み立て、ブロックスコープに束縛するのが定石 — スコープ
離脱で確実に refcount が 0 になる。

```culebra
make_resource = fn (id) {
  { drop: fn () { puts("R{id} released") } }
}

puts('enter')
{
  r = make_resource('X')
}
puts('exit')
# => |
# 'enter'
# 'RX released'
# 'exit'
```

`drop` はカスケードする — 外側オブジェクトが解放されると、内側に
持つ参照 (drop 付き) が連鎖して解放される。

### 7.5 Scope guard パターン

```culebra
make_guard = fn () {
  mut fns = []
  {
    add: fn (f) { fns.push(f) },
    run: fn () {
      mut i = fns.size() - 1
      while i >= 0 { fns[i](); i = i - 1 }
      fns = []
    }
  }
}

process = fn (items) {
  {
    g = make_guard()
    defer { g.run() }

    items.for_each(fn (item) {
      g.add(fn () { puts("close {item}") })
      puts("open {item}")
    })
  }
}

process(['a', 'b'])
# => |
# 'open a'
# 'open b'
# 'close b'
# 'close a'
```

### Why throw 値は任意

`throw "msg"` で十分なケースがほとんど (スクリプト)。 ライブラリ
ではクラス化したエラー (Ch.8) で十分。 階層は最初から要らない。
catch 節は thrower が使った形をパターンマッチで受ければよい (Ch.6)。

---

第 II 部 — 抽象化の道具
=======================

8. クラス
---------

### 8.1 構文

`class` はコンストラクタ (`new`) とメソッドを宣言する。 `this.x =
...` で設定したフィールドはデフォルトで可変。 インスタンスは可読な
`class:` タグを持つ。

```culebra
class Car {
  new(mpr)  { this.miles = 0; this.mpr = mpr }
  run(n)    { this.miles = this.miles + this.mpr * n }
  total()   { "走行距離: {this.miles} miles" }
}

car = Car.new(5)
car.run(1); car.run(2)
puts(car.total())             # => '走行距離: 15 miles'
puts(car.class)               # => 'Car'
```

### 8.2 クロージャベースの別解

`class` は糖衣 — 同じカプセル化はファクトリが Object リテラルを
返す形でも書ける。 状態は捕捉ローカル (真にプライベート)。 両方
とも第一級。

```culebra
Car2 = {
  new: fn (mpr) {
    mut miles = 0
    {
      run:   fn (n) { miles = miles + mpr * n },
      total: fn () { "走行距離: {miles} miles" }
    }
  }
}

car = Car2.new(5)
car.run(1); car.run(2)
puts(car.total())             # => '走行距離: 15 miles'
```

`class:` タグと shape マッチが欲しいなら `class`、private 状態の
方が重要ならクロージャ形式を選ぶ。

### 8.3 Static method

> **Status: Planned.** 現状、ファクトリはクラスの外の自由関数として
> 書く:
>
> ```culebra
> class Shape { new(name) { this.name = name } }
> make_circle = fn (r) { Shape.new("circle r={r}") }
> puts(make_circle(3).name)         # => circle r=3
> ```
>
> static method が入れば `Shape.make_circle(3)` と書ける。

### Why `class` とクロージャ両方サポートか

クロージャ as オブジェクトが先に存在し、使い捨てカプセル化 (使い切り
イテレータ、scope guard 等) では今も正解。 `class` 形式は、オブ
ジェクトが遠くまで運ばれてアイデンティティが必要 (`class:` タグ、
`match` やデバッグ出力で使う) になる時に意味を持つ。

9. 演算子オーバーロード
-----------------------

### 9.1 特殊メソッド

| 演算子   | メソッド       | 典型用途             |
|----------|---------------|---------------------|
| `+`      | `__add__`     | 数値 / ベクトル     |
| `-`      | `__sub__`     | 数値 / ベクトル     |
| `*`      | `__mul__`     | 数値 / スカラ       |
| `/`      | `__div__`     | 数値                |
| `%`      | `__mod__`     | 数値                |
| `**`     | `__pow__`     | 数値                |
| `@`      | `__matmul__`  | 行列積              |
| 単項 -   | `__neg__`     | 符号反転            |
| `==`     | `__eq__`      | 等価                |
| `<`      | `__lt__`      | 順序 (`<=` 等は導出) |
| `()`     | `__call__`    | callable インスタンス |
| `[i]`    | `__index__`   | インデックス        |

逆側メソッド (`__radd__` 等) はサポートしない — オーバーロードは
その演算を所有する型に置く。

### 9.2 例: 2 次元ベクトル

```culebra
class Vec2 {
  new(x, y)   { this.x = x; this.y = y }
  __add__(o)  { Vec2.new(this.x + o.x, this.y + o.y) }
  __sub__(o)  { Vec2.new(this.x - o.x, this.y - o.y) }
  __mul__(k)  { Vec2.new(this.x * k, this.y * k) }
  __neg__()   { Vec2.new(-this.x, -this.y) }
  __eq__(o)   { this.x == o.x && this.y == o.y }
  show()      { "({this.x}, {this.y})" }
}

a = Vec2.new(1, 2)
b = Vec2.new(3, 4)
puts((a + b).show())          # => '(4, 6)'
puts((b - a).show())          # => '(2, 2)'
puts((a * 3).show())          # => '(3, 6)'
puts((-a).show())             # => '(-1, -2)'
puts(a == Vec2.new(1, 2))     # => true
```

### 9.3 `__call__` で callable インスタンス

> **Status: Planned.** インスタンスを直接呼び出す形 (`add5(10)`) は
> まだ未対応。 当面はクラスに名前付きメソッドを持たせてそれを呼ぶ
> (`add5.apply(10)`)。

```culebra
# doctest: skip
class Adder {
  new(n)        { this.n = n }
  __call__(x)   { x + this.n }
}

add5 = Adder.new(5)
puts(add5(10))                # => 15
puts(add5(99))                # => 104
```

10. UFCS とマルチメソッド
-------------------------

### 10.1 UFCS 解決順

`x.name(args)` の解決:

1. `x` にプロパティ/メソッド `name` があればそれを使う
2. 無ければスコープ内の自由関数 `name` を `name(x, args)` として呼ぶ
3. それも無ければプロパティアクセスは `nil`、 nil に対する call は
   エラー

```culebra
double = fn (x) { x * 2 }
puts(42.double())                                  # => 84
puts('hello world'.split(' ').size())              # => 2

# 既存メソッドが常に優先 — Array の組み込み `reverse` は
# ユーザの `reverse` で上書きされない
reverse = fn (x) { puts('user reverse NOT called') }
mut a = [1, 2, 3]
a.reverse()
puts(a)                                            # => [3, 2, 1]
```

### 10.2 マルチメソッド (自由関数の多重ディスパッチ)

同名で別型の関数を複数定義。 ランタイムが引数の宣言型に対する最具
体マッチを選ぶ。

```culebra
class Circle { new(r) { this.r = r } }
class Square { new(s) { this.s = s } }

fn area(c: Circle) { 3.14159 * c.r * c.r }
fn area(s: Square) { s.s * s.s }
fn area(n: Long)   { n }                     # 数値のフォールバック

puts(area(Circle.new(2)))                    # => 12.56636
puts(area(Square.new(3)))                    # => 9
puts(area(10))                               # => 10
```

ディスパッチは positional / kwargs / `**splat` 全部カバー。 インス
タンスメソッドはまだマルチメソッド非対応 (10.3 参照)。

### 10.3 ディスパッチ拡張

> **Status: Planned.** 4 つすべて採用決定、未実装。 現状は
> `match v.class` で明示分岐、または関数名を分けて回避。
>
> - **class 継承ディスパッチ** — パラメータ `x: Shape` が `Shape`
>   のサブクラスを受け、最近接マッチが選ばれる。
> - **Union 注釈ディスパッチ** — `fn f(x: Long | String)` が引数
>   の実行時型で選ばれる。
> - **メソッドマルチメソッド** — `class.method` が自由関数と同じ
>   解決に参加。
> - **ディスパッチ IC** — call-site ごとの inline cache。

### Why 自由関数から先か

自由関数のマルチメソッドは UFCS や import された namespace と無
理なく合成できる (暗黙のサブタイピングが入らない)。 メソッドマル
チメソッドは own-class vs UFCS vs free の優先順序を決める必要が
あり、推測より実ワークロードで決めたい。

11. デコレータ
--------------

### 11.1 `@deco`

`fn` (または `class`) の前に置く `@deco` は、`deco(original)` の
結果を元の名前に束縛する。

```culebra
log = fn (f) {
  fn (x) {
    puts("calling with {x}")
    f(x)
  }
}

@log
fn double(x) { x * 2 }

puts(double(7))
# => |
# 'calling with 7'
# 14
```

### 11.2 ファクトリとスタック

```culebra
prefix = fn (tag) {
  fn (f) {
    fn () {
      puts("[{tag}]")
      f()
    }
  }
}

@prefix('A')
@prefix('B')
fn greet() { puts('hi') }

greet()
# => |
# '[A]'
# '[B]'
# 'hi'
```

外側デコレータが内側の結果をラップする — ここでは `@prefix('A')`
が `@prefix('B')` でラップ済みの関数を更にラップ。 上から下に読む
と実行順と一致。

### 11.3 Memoize の実例

```culebra
memoize = fn (f) {
  mut cache = {}
  fn (x) {
    k = to_string(x)
    if !cache.has(k) { cache[k] = f(x) }
    cache[k]
  }
}

@memoize
fn slow_square(x) { x * x }

puts(slow_square(7))          # => 49
puts(slow_square(7))          # => 49
```

### 11.4 `fn.params` introspection

> **Status: Planned.** 関数の宣言パラメータ名と型を露出する仕組み。
> `@autograd` / `@trace` のような signature を知る必要のあるデコ
> レータが書けるようになる。

### Why デコレータとマルチメソッドは併存しないか

デコレートされた関数は単一値 (ラップされたクロージャ) として束縛
される — これは「同名の `fn` が複数共存する」というマルチメソッド
の形と相容れない。 名前ごとにどちらか一方を選ぶ。

12. モジュール
--------------

### 12.1 暗黙 import

Culebra の「モジュールビルド」は 1 つのエントリファイルから始まり、
そこからトップレベル識別子を辿って参照される全ての兄弟 `.cul` を
取り込む。

```culebra
# lib.cul
greet = fn (name) { "hello, {name}" }
PI    = 3.14159
```

```culebra
# doctest: skip
# main.cul — lib.cul と同じディレクトリ
puts(greet('world'))          # => hello, world
puts(PI)                      # => 3.14159
```

`culebra main.cul` を実行すると、未解決の名前 `greet` を辿って
`lib.cul` を自動発見する。 `import` 文は無い。

### 12.2 エントリ環境の隔離

エントリファイル (`main.cul` 上) で導入された束縛は import された
ファイルからは見えない。 import されたファイル内で導入された束縛は
ビルドから到達可能などこからでも見える。 Go の直観 (パッケージ内
全ファイルが「パッケージ」) に揃え、エントリだけは最上位 main 扱い。

### 12.3 循環

ファイル間の循環参照はモジュールビルド時に検出され、ファイル/行付き
で拒絶される。 共通の第三ファイルで分解。

### Why 暗黙か

1500 行のプログラムで 30 個のヘルパファイルを取り込むケースを想像
する。 個別 `import` を要求する得は無い — 未解決識別子からツールが
同じグラフを導けるから。 著作ループが速く・単純になる代わりに、
tree-shaking もちゃんと効く (リーチャブルなトップレベルがツールで
分かるため、Ch.17)。

リゾルバ設計と循環検出アルゴリズムの詳細は [`internals.md` §10](internals.md) (英語)。

---

第 III 部 — 型とライブラリ
==========================

13. 型システム
--------------

### 13.1 現状: オプショナル注釈 + `Any`

注釈は 3 つの境界での**ランタイム**チェック: 変数代入、関数パラ
メータ渡し、関数戻り値。

```culebra
let x: Long = 10
puts(x)                       # => 10

add = fn (a: Long, b: Long) -> Long { a + b }
puts(add(3, 4))               # => 7

# Any は全部受ける
identity = fn (x: Any) -> Any { x }
puts(identity(42))            # => 42
puts(identity('hi'))          # => 'hi'

# 一部だけ型注釈、残りは動的
describe = fn (v, label: String) -> String { "{label}: {v}" }
puts(describe([1, 2], 'array'))     # => 'array: [1, 2]'
```

`type_of` (Ch.2.1) が組み込み型のランタイム introspection。
`match` 節の `n: ClassName` (Ch.6) はそのクラスのインスタンスに
マッチ。

### 13.2 Union / Optional / Tuple

> **Status: Planned.** `Long | String`、 `T?` (= `T | Nil`)、
> `(Long, String)` — 採用決定、未実装。

```culebra
# doctest: skip
let id: Long | String = 42
let maybe: Long?      = nil
let pair: (Long, String) = (1, 'one')
```

### 13.3 Trait / Protocol

> **Status: Planned.** Structural と nominal の両方が射程内。
> trade-off (とどちらを先に入れるか) は議論中。
> [`internals.md` §13](internals.md) (英語) 参照。

### 13.4 Generic

> **Status: Planned.** 1.0 前に必須。 大きなライブラリが育った後に
> generic を入れるのは高コスト (Java 5 期の教訓)。 表面積が小さい
> うちに着地させる方針。

### TypeScript から意図的に取り入れない要素

Conditional types、mapped types、template literal types、ユーティリ
ティ型群 (`Pick` / `Omit` 等) は、小さな動的言語には複雑すぎる。
目標は Rust/Swift の表現力であって、TS の表現力ではない。

14. 標準ライブラリ巡り
----------------------

CLI ドライバは `puts` / `print` を `IO.puts` / `IO.print` のエイ
リアスとして提供する。 自前で環境を組み立てる埋め込み用途では
namespace は見えるが bare alias は無い。

### 14.1 コア組み込み

```culebra
puts(to_long('42'))           # => 42
puts(to_long('  -7 '))        # => -7
puts(to_string(42))           # => '42'
puts(to_string([1, 2]))       # => '[1, 2]'
puts(type_of(42))             # => 'Long'
puts(iota(3))                 # => [0, 1, 2]
puts(iota(2, 5))              # => [2, 3, 4]
assert_eq(1 + 1, 2)           # 成功時は無音、失敗で throw
```

### 14.2 `Math`

```culebra
puts(Math.abs(-7))            # => 7
puts(Math.min(3, 5))          # => 3
puts(Math.max(3, 5))          # => 5
puts(Math.pow(2, 10))         # => 1024
puts(Math.sign(-42))          # => -1
puts(Math.clamp(15, 0, 10))   # => 10
```

### 14.3 `IO`

```culebra
print('Hello, '); print('world!'); print("\n")   # => Hello, world!
# IO.input()                 # 標準入力から 1 行
# FS.write('out.txt', 'hi')  # ファイル書き込み
# FS.read('in.txt')          # ファイル読み込み
```

### 14.4 `Sys` / `Random` / `String` / `FS` / `Time` / `Args`

簡単に紹介。 完全リファレンスは [`stdlib.ja.md`](stdlib.ja.md)。

```culebra
puts(Sys.argv)                # => []
# Sys.env('HOME')             # プロセス環境
# Sys.exit(0)                 # 終了

puts(Random.int(0, 100) >= 0)          # => true

# String / FS / Time / Args namespace — stdlib.ja.md を参照
```

### 14.5 `Regex`

> **Status: Planned (Tier 1).** 線形時間マッチ (NFA ベース、
> catastrophic backtracking 無し)、grapheme cluster 単位がデフォルト。
> `/u` フラグ不要 — `.` や文字クラスは常に grapheme レベルで動作。

```culebra
# doctest: skip
re = Regex.compile("\\d+")
m = re.match('order #42')
puts(m.text)                  # planned: => 42
```

### 14.6 `Hash` + `Encoding`

> **Status: 実装済み。** `Hash.sha256` / `sha1` / `sha512` / `md5` と
> `Hash.hmac_sha256` / `hmac_sha1` / `hmac_sha512` は hex ダイジェストを返す
> （stdlib §18 参照）。 `Encoding.base64` / `Encoding.hex` / `Encoding.url` /
> `Encoding.html`（`.encode`/`.decode`）と `Compress.gzip` / `Compress.gunzip`
> も利用可能（stdlib §16–17 参照）。 JSON は top-level の `JSON` ネームスペース
> （`JSON.parse` / `JSON.stringify`）— `Encoding.json` は無い。

### 14.7 `HTTP`

> **Status: Planned (Tier 1).** Blocking、SSE / WebSocket 含む、
> TLS は statically link した BoringSSL。 `async` / `await` 無し
> — 並行はスレッドで。 スケール上限は数千接続。

```culebra
# doctest: skip
res = HTTP.get('https://example.com')
puts(res.status)              # planned: => 200

# サーバ
HTTP.serve('127.0.0.1', 8080, fn (req) {
  HTTP.response(200, 'hello')
})
```

### 14.8 さらに計画中

> **Status: Planned (Tier 2/3).** `Compression` (gzip)、 `Crypto`、
> `Process` (子プロセス)、 `Sockets` (raw TCP/UDP)。 順序未確定、
> demand-driven で。

### Why "batteries-included、ティア制"

CLI ツールや小さなサーバを書くのにパッケージマネージャを引っ張り
出さなくて済むようにしたい。 Tier 1 (Regex / HTTP / Hash+Encoding)
が日常スクリプトの大半をカバー。 Tier 2/3 は具体ユーザがクリ
ティカルパスに押し上げたら着手。

15. Tensor プリミティブ
-----------------------

### 15.1 構築と算術

`Tensor` は組み込みの n 次元配列で、BLAS にルーティングされる
(macOS は Apple Accelerate、Linux は OpenBLAS)。 要素型は現状
`Float` (F64)。

```culebra
a = Tensor.from([1.0, 2.0, 3.0])
b = Tensor.from([10.0, 20.0, 30.0])
puts((a + b).to_array())      # => [11.0, 22.0, 33.0]
puts((a * 2.0).to_array())    # => [2.0, 4.0, 6.0]
puts(a.sum())                 # => 6.0
```

### 15.2 形状と matmul

```culebra
m = Tensor.from([[1.0, 2.0], [3.0, 4.0]])
puts(m.shape())               # => [2, 2]

# matmul (`dot`) は遅延グラフを作る; `Tensor.eval` で BLAS カーネルが走る。
c = m.dot(m)
Tensor.eval(c)
puts(c.to_array())            # => [[7.0, 10.0], [15.0, 22.0]]
```

### 15.3 ブロードキャスト

```culebra
row = Tensor.from([1.0, 2.0, 3.0])
col = Tensor.from([[10.0], [20.0]])
puts((row + col).to_array())  # => [[11.0, 12.0, 13.0], [21.0, 22.0, 23.0]]
```

### 15.4 GPU プリミティブ

> **Status: Planned.** CUDA / Metal Shading Language 用のバックエ
> ンドを別の `Matrix` (または `GTensor`) として用意する計画 —
> `Tensor` 自体は CPU 専用のまま。

### Why BLAS にルーティングするか

行列重視のコード (MLP 推論、microgpt) を手書きの O(n³) ループで
出荷したら NumPy より桁違いに遅かった。 BLAS にすると、このコード
ベースが実際に学習する MNIST サイズで PyTorch CPU の ~1.2× 以内に
収まる。 ベンチ詳細は
[`benchmarks/mnist/README.md`](../benchmarks/mnist/README.md) と
[`benchmarks/microgpt/README.md`](../benchmarks/microgpt/README.md)。

F32 / F64 のトレード、アロケータ選定、lazy shape の議論は
[`internals.md` §8](internals.md) (英語)。

---

第 IV 部 — 検証とデプロイ
=========================

16. テスト (`culebra test`)
---------------------------

> `test()` / `@test` / `@parametrize` と matcher 群、 引数で DI 解決
> される fixture (decorator 不要、 env 内の任意の fn) が実装済で、
> `culebra test [path]` で動きます。

### 16.1 doctest 規約 (確定)

本ガイド、 `language.ja.md`、 `stdlib.ja.md` の各 ` ```culebra `
ブロックは以下の規約に従う:

- `# => <value>` — 期待 stdout (1 行)
- `# => |` + 続く `# <line>` 行 — 複数行期待 stdout
- `# !! <pattern>` — 期待 `throw` (部分一致)
- `# doctest: <directive>` (ブロック先頭) — 制御:
  - `skip` — 説明用、実行しない (主に *Planned* 機能)
  - `compile-only` — 構文チェックのみ
  - `interp-only` / `jit-only` / `aot-only` — backend 限定

ブロック間は独立、`setup` / `teardown` は無し。

実行は `culebra test --doc <path>` (または `just doctest`)。各ブロックを
抽出し、新しいインタプリタで実行してマーカーと出力を照合する。現状
honor されるのは `skip` のみ。`compile-only` と backend 限定ディレクティブ
は予約済み (該当ブロックは当面そのまま実行される)。

### 16.2 テストの書き方

3 つの書き方があります — `test()` 呼出形と `@test` デコレータ形は
等価。 `@parametrize` は cases ごとに 1 テストを登録します。

```culebra
# doctest: skip
# tests/test_string.cul

# 呼び出し形
test("interpolation embeds Long", fn() {
  let x = 42
  assert_eq("hi {x}", "hi 42")
})

# デコレータ形 — 関数名がテスト名になる
@test
fn interpolation_embeds_float() {
  let pi = 3.14
  assert_eq("π = {pi}", "π = 3.14")
}

# Parametrize — case ごとに 1 テスト、`<fn>[i]` という名前
@parametrize([(1, 2, 3), (2, 3, 5), (10, 20, 30)])
fn adds_correctly(a, b, want) {
  assert_eq(a + b, want)
}
```

**`describe` ネストは採用しない**。 グルーピングはディレクトリ
(`tests/strings/`) とテスト名の `/` 区切り
(`"Array/push: appends element"`) で表現します。

**DI による fixture**。 test の positional 引数は名前で env から
resolve されます — env 内の任意の fn が fixture として使えます
(decorator 不要)。 fixture が fixture を引数で取ることも可能です。

```culebra
# doctest: skip
fn db()       { { users: [], next_id: 1 } }
fn user(db)   { db.users.push({ id: 1, name: "alice" }); db.users[0] }

@test
fn user_has_name(user) {
  assert_eq(user.name, "alice")
}
```

1 つの test 内では、 fixture は **1 回だけ評価** されます — 直接 +
推移的に複数回 mention されても同じ instance を共有します。 test
間では fresh。

**class `drop` での cleanup**。 teardown が必要なリソースは class に
ラップして `drop` メソッドを置きます (§7.4)。 ランタイムの ref count
管理が test 終わりに per-test cache の release を契機に発火します。

```culebra
# doctest: skip
class TestDB {
  new()    { this.conn = Database.connect("memory") }
  drop()   { this.conn.close() }
  users()  { this.conn.users }
}

fn db() { TestDB.new() }

@test
fn user_count(db) {
  db.users().create("alice")
  assert_eq(db.users().count(), 1)
  # test 末で db が drop → conn.close()
}
```

fixture 関数本体内の `defer` は fixture fn が return した瞬間に発火
してしまう (test 本体実行前) ので、 cleanup には class `drop` を使い
ます。

複数の test ファイルで共有したい長寿命 state (例: 1 回ロードした
model) は module top-level に置き、 各 test ファイルで import します。
モジュールシステムが binding を cache するので、 `import` は常に同一
インスタンスを返します。

**matchers**。 アサーションは matcher 一族を使います — `assert`
キーワード / builtin は存在しません。 matcher は **3 backend の
global** として bind されており (`puts` / `Math` と同じ立場)、
`culebra script.cul`、 `culebra --jit script.cul`、 `culebra build`、
`culebra test` のいずれでも同じく動きます:

```culebra
# doctest: skip
assert_true(x)                          # x が truthy
assert_false(x)                         # x が falsy
assert_eq(arr.len(), 3)                 # == ; 失敗時に両辺を表示
assert_ne(status, "error")              # !=
assert_lt(elapsed, 1.0)                 # <
assert_le(count, max)                   # <=
assert_gt(score, 0)                     # >
assert_ge(items.len(), 1)               # >=
assert_throws("TypeError", fn() { let _ = 1 + 'b' })
assert_close(0.1 + 0.2, 0.3, 1e-9)      # |a - b| <= tol
```

- `assert_eq` / `assert_ne` / `assert_lt` / `assert_le` / `assert_gt` /
  `assert_ge` は `==` / `<` / `<=` 演算子と同じ `__eq__` / `__lt__` /
  `__le__` dispatch を行います — クラスインスタンスでも `assert_eq(p1,
  p2)` と式 `p1 == p2` は一致します。
- `assert_throws(kind, fn)` は 0 引数の `fn()` を呼んで throw を検査。
  組み込みエラーは `kind`、 ユーザの `throw { kind: ..., message: ... }`
  は `.kind` プロパティを比較。
- `assert_close(a, b, tol)` は `|a - b| <= tol` を検査。 NaN は失敗扱い
  (素朴な `>` 検査だと発散計算が silently pass してしまうため)。

**production の不変条件**。 テストスイート外で `if (!cond) throw {...}`
を書くときは `if`/`throw` を直接書きます (Go 流儀、
[language.ja.md §15](language.ja.md) 参照)。 production build で
disable する別の `assert` キーワードは存在しません。

### 16.3 実行

`culebra test [path]` がテストファイルを discover します。 このサブコマンド
経由で起動した場合のみ、 `test` / `@test` / `@parametrize` が
**ambient global** として注入されます — `import` 不要。 matcher 一族は
3 backend で常時 global なので追加注入は不要です。 これは script 実行
モード下でだけ `puts` / `print` が ambient で、 `culebra::environment()`
には注入されない設計と同じ流儀 ([stdlib.ja.md §11](stdlib.ja.md) 参照)。

```sh
culebra test                       # 現在ディレクトリから探索・実行
culebra test tests/strings/        # サブツリー指定
culebra test --filter "Array/push" # テスト名部分一致
culebra test --reporter json       # NDJSON 出力 (1 行 1 JSON)
culebra test --bail                # 最初の failure で停止
culebra test --bail 3              # 3 個失敗で停止
culebra test --list                # 実行せず discovery のみ
```

Discovery: 指定されたパスがファイルならそれを使用。 ディレクトリなら
`test_*.cul` 一致を再帰的に walk。 終了コードは全 pass で `0`、何か
fail で `1`。

**reporter**。 default は人間向け。 `--reporter json` で NDJSON
(1 行 1 JSON object) — agent loop / CI 連携向け:

```
{"event":"test_pass","name":"adds_correctly","source":"tests/test_math.cul",
 "stdout":""}
{"event":"test_fail","name":"divides_correctly","kind":"AssertionError",
 "message":"assert_eq failed:\n  left:  3\n  right: 4","line":12,"col":3,
 "source":"tests/test_math.cul",
 "snippet":" 10  @test\n 11  fn divides_correctly() {\n 12>   assert_eq(6/2, 4)\n 13  }\n",
 "stdout":""}
{"event":"file_error","source":"tests/test_bad.cul","kind":"SyntaxError",
 "message":"..."}
{"event":"test_list","name":"divides_correctly","source":"tests/test_math.cul"}
{"event":"list_end","count":42}
{"event":"run_end","passed":42,"failed":1,"errored_files":0,"bailed":false}
```

JSON モードでは test 内の `puts(...)` は event の `stdout` フィールド
に capture され NDJSON ストリームに interleave しません。 失敗 event
には `snippet` が付き、 失敗行を `>` で marker、 前後 2 行の文脈と共に
含まれます — consumer が file 再読込なしで該当コードを表示できます。

`just test` 経由の従来 `tests/*.cul` スイート (matcher 使用、`test()`
呼出なし) は各ファイルを `./build/culebra <f>` / `--jit <f>` /
`culebra build <f>` で直接実行します。 matcher は language-level
global なので `culebra test` を介さずに 3 backend で同じファイルが
回ります。

### 16.4 今後の拡張

- **明示 `import { test } from "std/test"`** — `culebra test` 経由でない
  コード (embedded test helper 等) で使うため
- **`culebra test --doc docs/`** — 16.1 の規約に従って markdown から
  ` ```culebra ` ブロックを抽出・実行
- **`--backend interp|jit|aot`** — 現状 runner は interp のみ。
  backend 選択は今後
- **並列実行** — 現状逐次。 JIT/AOT が入った時の parallel default は
  optional

17. リント (`culebra lint`)
---------------------------

`culebra lint <file.cul>...` はプログラムを**実行せずに**静的な問題を
報告し、CI でゲートできるよう非ゼロ終了する (0 = クリーン、1 = 警告のみ、
2 = エラー)。全 backend が load 段で走らせている静的解析を再利用する
(報告されるエラーは実行時に abort するものと厳密に同じ) 上に、助言的な
警告を追加する。

```bash
culebra lint app.cul
# app.cul:12:7: warning: unused variable 'tmp'
# app.cul:20:3: error: undefined variable 'reuslt'
```

現状の報告対象:

- **エラー** — 健全で必ず失敗する集合: ループ外の `break`/`continue`、
  関数外の `return`、不正なパラメータ/代入形、重複パラメータ/メンバ、
  shadowing、どこにも束縛されない名前の読み (未定義変数の sound subset)。
  これらは元々あらゆる実行を abort させる。`lint` は最初の1件で止まらず
  まとめて表示するだけ。
- **警告** — 現状は**未使用ローカル変数**: 関数内の `let`/`mut` 束縛で
  一度も読まれないもの。先頭アンダースコア (`_x`、または素の sink `_`)
  は意図的な未使用の印で、決して報告しない。パラメータとトップレベル
  束縛は報告しない (未使用パラメータは設計上よくあり、トップレベル名は
  export される可能性がある)。

予定: 未使用 import、到達不能コード、エディタ/LSP 連携用の
`--format json`、インライン `# lint: ignore` 抑制。

18. フォーマット (`culebra fmt`)
--------------------------------

`culebra fmt [files...]` はソースを唯一の正準スタイルに整形する: 演算子
周りの空白正規化、2スペースインデント、ブレースブロックの複数行化、行幅を
超えた引数リスト/コレクションリテラルの折り返し。`gofmt` と同じく opinionated
かつ zero-config (スタイル設定フラグ無し)。

```bash
culebra fmt app.cul          # 整形結果を stdout に出力
culebra fmt -i app.cul       # ファイルをその場で書き換え
culebra fmt -i .             # カレント以下の .cul を全整形
culebra fmt --check app.cul  # 未整形なら exit 1 (CI ゲート)
culebra fmt -l src/*.cul     # 変更が必要なファイル名を列挙
cat app.cul | culebra fmt -  # stdin -> stdout (エディタの保存時整形)
```

ディレクトリ引数は再帰的に `.cul` を走査するので、`culebra fmt -i .` で
プロジェクト全体を整形、`culebra fmt --check .` で CI ゲートにできる。

コメントは保持される: 行頭コメントは導く文の上に、行末コメントは同じ行に
残り、文と文の間の空行は1つ保持する (空行が連続する場合は1つに圧縮)。
match / cond の腕、class / trait / enum のメンバ、分配パターン、パラメータ
リストも正規化され、長い二項式やメソッドチェーンは行幅で折り返す。

仕組み: ソースを構文木にパースして再出力し、その結果を**再パースして元と
照合**する。整形がプログラムの意味を変えてしまう、またはコメントを脱落・重複
させてしまう場合は整形を拒否し、ファイルを書き換えずに残す (コード破壊を絶対に
避ける)。整形は冪等で、2回かけても1回と同じ結果になる。

### エディタ統合

stdin 形式 (`culebra fmt -`) が保存時整形のフック。Vim/Neovim は同梱
`ftplugin` が `formatprg=culebra\ fmt\ -` を設定するので `gq` で整形できる
(保存時整形は `misc/vim/cul_ftplugin.vim` のコメントアウトした `BufWritePre`
autocmd 参照)。「外部コマンドで整形」「保存時に実行」の仕組みを持つエディタ
なら同様にバッファを `culebra fmt -` に通せる。

19. AOT バイナリビルド
----------------------

`culebra build` は `.cul` ソースを ahead-of-time で自己完結バイナ
リにコンパイルする。 ランタイムに LLVM 不要。 tree-shaking で使われ
ないランタイムヘルパを落とす。 Tensor を使わないプログラムでは
Accelerate / BLAS フレームワーク依存も外せる。

```bash
./build/culebra build my-program.cul -o ./out
./out                                     # standalone、~350 KB on macOS
otool -L ./out                            # Accelerate も LLVM も無し
```

### 19.1 クロスコンパイル

```bash
./build/culebra build my-program.cul \
  --target=x86_64-unknown-linux-gnu \
  --sysroot=$LINUX_SYSROOT \
  --rt-lib=$PWD/build-linux-x86_64/libculebra_rt.a \
  -o ./out-linux
```

ランタイムアーカイブのビルド、sysroot の用意、クロスコンパイル全
ワークフローの詳細は [`binary_build.ja.md`](binary_build.ja.md)。

### Why tree-shaking が効くか

`puts` だけ使う "hello world" は FFT も HTTP ランタイムも要らない。
エントリファイルから call graph を辿ることで、参照されていないラン
タイムヘルパ (~200 個) を落とせる。 `Tensor` 参照が無ければ no-BLAS
archive に差し替わるので、数 MB が数百 KB になる。

20. 埋め込み概観
----------------

Culebra は header-friendly な C++23 ライブラリ。 最小埋め込み例:

```cpp
#include <culebra.h>

int main() {
  auto env  = culebra::environment();
  auto value = culebra::eval(env, R"(
    add = fn (a, b) { a + b }
    add(40, 2)
  )");
  std::cout << culebra::to_string(value) << "\n";   // 42
}
```

環境構築は埋め込み側。 `IO` は提供されるが、CLI 専用 alias の
`puts` / `print` はホスト側で用意する設計。 エラーは
`culebra::Error` 例外として throw され、元の値と行/列情報を持つ。

環境カスタマイズ、値変換、JIT ホスト、AOT-archive 埋め込み経路
(`libculebra_rt.a`) の詳細は [`embedding.ja.md`](embedding.ja.md)。

---

次の一歩
--------

- 厳密な文法と評価規則: [`language.ja.md`](language.ja.md)
- API リファレンス: [`stdlib.ja.md`](stdlib.ja.md)
- 実装の内部詳細: [`internals.md`](internals.md) (英語のみ)
- 大きめの実例: [`benchmarks/microgpt/`](../benchmarks/microgpt/)
- インタラクティブな REPL: `./build/culebra --shell`
