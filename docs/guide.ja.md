Culebra ガイド
================

Rust 風シンタックスを持つ小さな動的型付けスクリプト言語。 ツリー
ウォーキング型インタプリタと LLVM ORC JIT の 2 バックエンドが 1 つの
AST を共有します。 このガイドは "hello" から C++ ホストへの埋め込み
までを案内します。 厳密な文法は [`language.ja.md`](language.ja.md)、
API リファレンスは [`stdlib.ja.md`](stdlib.ja.md)、 実装の内部詳細は
[`internals.ja.md`](internals.ja.md) を参照してください。

> **doctest 規約。** 本ガイドの各 ` ```culebra ` ブロックは実行可能
> な例です。 行末の `# => <value>` は標準出力の期待値、`# !! <pattern>`
> は `throw` の期待値。 ブロック先頭の `# doctest: skip` は説明用
> （複数ファイル・ネットワークアクセス・`culebra test` ランナーが
> 必要な場合が多い）。 ブロック間は独立スコープです。 規約とディレ
> クティブ一覧の全体は第16.1節。

> **Status ラベル。** ラベル無しの見出しは現時点の実装を記述します。
> 出現するラベル: **Draft** (実装中、API 変更あり)、**Planned**
> (採用決定、未実装)、**Deprecated** (将来削除予定)。 採用せずと
> 決定した機能は [`record.ja.md`](record.ja.md) に集約。

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
  8. [代数的エフェクト](#8-代数的エフェクト)
- **第 II 部 — 抽象化の道具**
  9. [クラス](#9-クラス)
  10. [演算子オーバーロード](#10-演算子オーバーロード)
  11. [UFCS とマルチメソッド](#11-ufcs-とマルチメソッド)
  12. [デコレータ](#12-デコレータ)
  13. [モジュール](#13-モジュール)
- **第 III 部 — 型とライブラリ**
  14. [型システム](#14-型システム)
  15. [標準ライブラリ巡り](#15-標準ライブラリ巡り)
  16. [Tensor プリミティブ](#16-tensor-プリミティブ)
- **第 IV 部 — 検証とデプロイ**
  17. [テスト (`culebra test`)](#17-テスト-culebra-test)
  18. [リント (`culebra lint`)](#18-リント-culebra-lint)
  19. [フォーマット (`culebra fmt`)](#19-フォーマット-culebra-fmt)
  20. [AOT バイナリビルド](#20-aot-バイナリビルド)
  21. [埋め込み概観](#21-埋め込み概観)

## 0. 設計哲学

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
  は [`record.ja.md`](record.ja.md))。
- **暗黙 import、明示 `import` 文無し。** トップレベル識別子への
  bare な参照がモジュールビルド内でファイル境界を越える (Ch.13)。
  明示 `import` は検討の上不採用。
- **async/await 無し。** I/O はブロッキング設計、並行はスレッドで。
  HTTP 等のネットワークスタックはブロッキング、典型的なスケール
  上限は数千接続。
- **batteries-included、ティア制。** コア stdlib
  (Math/IO/Sys/Random/String/FS/Time/Args) と Tier 1
  (Regex/Http/Hash+Encoding) はどちらも出荷済み。 Tier 2/3
  (Crypto、Sockets) は需要次第 — Ch.15 参照。
- **1.0 前。** ソース・API は変わる可能性。 リリース機構 (バージョン
  タグ、CHANGELOG、Homebrew formula 等) は 1.0 後。

---

第 I 部 — 言語コア
==================

## 1. Hello & セットアップ

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
検証済み)。`--jit-faststart` は IR と機械語の両方の最適化を省き、
**JIT warmup (起動・コード生成時間) を約 40 分の 1**にする代わりに steady-state を
少し犠牲にする — 純スクリプトの hot loop で約 12%、重い処理が C++/BLAS ランタイム
側 (例: `Tensor`) にある場合は ~0%。`-O0` を含意し、別の `-O` を併記するとエラー。
短命スクリプトや BLAS 律速の実行に向く。
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

## 2. 値・束縛・制御フロー

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
パターン (Ch.9) はこの挙動で成立しています。

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

クロージャベースのオブジェクトパターン (Ch.9) では、捕捉された状態が
そのままオブジェクトの状態なので、silent shadow はオブジェクトを壊す。
一方、関数内 block での rebinding は日常パターンなので許容する。詳細
なルールと設計根拠は [language.ja.md §6](language.ja.md)。

## 3. 関数とクロージャ

### 3.1 `fn` と `|x|`

```culebra
add = fn (a, b) { a + b }
puts(add(2, 3))               # => 5

# 型注釈はオプション; 詳細は Ch.14
add_typed = fn (a: Long, b: Long) -> Long { a + b }
puts(add_typed(2, 3))         # => 5

# |x| expr は fn (x) { expr } の糖衣
square = |x| x * x
puts(square(7))               # => 49

# 再帰には `fn` (関数自身への参照)
fib = fn (x) {
  if x < 2 { x } else { fn(x - 2) + fn(x - 1) }
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

`*` マーカーは呼び出し側にオプション名を書かせるので、長いパラメー
タリストが読みやすくなり、再配置・拡張もコール側を壊さない。 free
な positional rest (`*args`) は意図的に不採用 — Array リテラルが
その役割を果たす。パラメータ・デフォルト値・splat の完全な仕様は
[language.ja.md §11](language.ja.md)。

## 4. 文字列

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

完全な一覧は [language.ja.md §18.1](language.ja.md)。

### 4.4 `StringView`、`StringLike`、graphemes() lazy

`.slice()` / `.split()` / `.view()` は `StringView` を返す — 元の
文字列のバイト列への zero-copy な借用で、元の束縛がスコープを抜けて
も共有オーナーが生存させ続ける。 `StringLike` 型のパラメータは
`String` と `StringView` の両方を受け付けるので、読むだけのヘルパー
が copy を強制しない。

```culebra
print_first_grapheme = fn (s: StringLike) {
  for g in s.graphemes() { puts(g); break }
}
print_first_grapheme('café')          # => 'c'

puts(type_of('hello'.slice(1, 4)))    # => 'StringView'
puts('hello'.slice(1, 4))             # => 'ell'
```

`.graphemes()` は Unicode の *extended grapheme cluster* を lazy に
走査する — ZWJ で連結された複数コードポイントの絵文字ファミリーで
あっても、1 ステップ = ユーザが知覚する 1 文字になる:

```culebra
puts('a👨‍👩‍👧b'.graphemes().collect().size())    # => 3
puts('café'.graphemes().collect().size())        # => 4
```

`StringView`/grapheme の完全な API は [language.ja.md §18.1](language.ja.md)。
設計議論は [`internals.ja.md` §6](internals.ja.md) 参照。

### Why Go 流のバイトインデックス

Swift / Python 3 は bytes vs scalar の区別を不透明な `Character` /
`str` インデックスで隠す。 ソケット・ファイル I/O と相互運用する
までは便利だが、その時点で破綻する。 Go はバイトオフセットを露出
させ、`rune` 反復をその上に置く。 Culebra は同じモデルに、表示用の
lazy grapheme 反復 (上記) を足したもの。

## 5. イテレータ

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
`has_next()` (`Bool` を返す)、`next()` (次の要素を返す)。 lazy チェ
インの早期終了保証を含む完全なプロトコルは
[language.ja.md §18.5](language.ja.md)。

```culebra
countdown = fn (start) {
  mut i = start
  {
    iter:     fn () { self },
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

`yield` を含む `fn` 本体はジェネレータになる — 呼び出しても本体は
実行されず、イテレータ (5.4 の `iter`/`has_next`/`next` プロトコル)
が返るので、`for` や lazy チェインのメソッドがそのまま使える。
`yield from` は他の iterable へ委譲する。

```culebra
fn countdown(start) {
  mut i = start
  while i > 0 { yield i; i = i - 1 }
}
for v in countdown(3) { puts(v) }
# => |
# 3
# 2
# 1

fn chunk(arr, n) {
  mut buf = []
  for v in arr {
    buf.push(v)
    if buf.size() >= n { yield buf; buf = [] }
  }
  if buf.size() > 0 { yield buf }
}
puts(chunk([1, 2, 3, 4, 5], 2).collect())    # => [[1, 2], [3, 4], [5]]
```

## 6. パターンマッチ

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
    _ => fn(n - 2)
  }
}
puts(is_even(10))             # => true
puts(is_even(7))              # => false
```

### Why exhaustiveness check 無し

静的型システム無しで Object の shape を網羅性検査するには、節約
以上のランタイムコストがかかる。 `_` 節 (またはガード付き最終
パターン) で意図を明示する方針。 詳細と Union 型の例外は
[language.ja.md §13](language.ja.md)。

## 7. エラー処理と RAII

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
発火する。 exit パスと順序の完全なルールは
[language.ja.md §15](language.ja.md)。

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
持つ参照 (drop 付き) が連鎖して解放される。 完全なメモリモデル
(RC + サイクル収集) は [language.ja.md §17](language.ja.md)。

### 7.5 Scope guard パターン

自前で `defer` を置けないコード (例: 呼び出し元のスコープでの
クリーンアップを望むコールバック) からクリーンアップを登録したい
場合は、クリーンアップ用クロージャのリストを持つ小さなヘルパー
オブジェクトを用意し、呼び出し側の 1 つの `defer` から LIFO で実行
すればよい。 完全な実装例は [language.ja.md §15](language.ja.md)。

### Why throw 値は任意

`throw "msg"` で十分なケースがほとんど (スクリプト)。 ライブラリ
ではクラス化したエラー (Ch.9) で十分。 階層は最初から要らない。
catch 節は thrower が使った形をパターンマッチで受ければよい (Ch.6)。

---

## 8. 代数的エフェクト

*エフェクト*は、意味を呼び出し側が決める操作をコードから呼べるようにします。
操作を `perform` し、コールスタック上位の `handle` ブロックが、それが何をするか、
そして `perform` したコードを *再開 (resume)* するかを選びます。ジェネレータ・
例外・依存性注入・バックトラック探索を 1 つの機構でカバーします。

### 8.1 `perform` と `handle`

操作は `effect fn`（本体なし）で宣言し、`perform` し、`handle` ブロックに `with`
clause を与えます。clause は操作の引数と `resume` 継続を受け取ります:

```culebra
effect fn ask()

let answer = handle {
  let n = perform ask()
  n * 2
} with ask(resume) {
  resume(21)
}
puts(answer)   # => 42
```

`resume(21)` はハンドルされた本体を `perform ask()` の地点から値 `21` で再開する
ので、`n` は `21` となり本体は `42` を返します。

### 8.2 ハンドラは状態を紡ぐ

ハンドラは `perform` のたびに走るので、自身が持つ状態に対して操作を解釈できます
— ここでは `get` が読み `put` が書くセルです:

```culebra
effect fn get()
effect fn put(v)

effect fn counter() {
  perform put(perform get() + 1)
  perform put(perform get() + 1)
  perform get()
}

mut cell = 0
let n = handle { counter() } with get(k) { k(cell) }
                             with put(v, k) { cell = v; k(nil) }
puts(n)   # => 2
```

どの関数からでも `perform` できます。plain 関数の `perform` は現在のコール
スタックに設置されたハンドラへディスパッチされ、`effect fn` マーカーが必要なのは
ハンドラが複数回 resume する、または非末尾で resume する場合（継続のキャプチャが
要る場合）だけです。1 つの `handle` は操作ごとに `with` clause を持てます。

### 8.3 複数回の再開

継続は multi-shot です — ハンドラは `resume` を何回でも呼べ、各呼び出しは
perform 側の残りを独立に再実行します。両方の再開を返せば両方の選択肢を探索します:

```culebra
effect fn choose(a, b)

let both = handle {
  let x = perform choose(1, 2)
  x * 10
} with choose(a, b, k) {
  [k(a), k(b)]
}
puts(both)   # => [10, 20]
```

### 8.4 再開しないハンドラ

`resume` を呼ばない clause は残りの計算を破棄します — まさに例外です。ハンドラの
ない `perform` は `EffectError` を送出するので、エフェクトは回復可能で型付きの
失敗としても使えます:

```culebra
effect fn raise(msg)

effect fn safeDiv(a, b) {
  if b == 0 { perform raise("div by zero") }
  a / b
}

puts(handle { safeDiv(10, 2) } with raise(m, k) { -1 })   # => 5
puts(handle { safeDiv(10, 0) } with raise(m, k) { -1 })   # => -1
```

正常完了値を写すには `with return(v) { … }` を加えます。エフェクトフルな本体の
中に `handle` を書けば、外側の計算をキャプチャしてそこから再開できます。完全な
リファレンスと制約は [language.ja.md §16](language.ja.md) を参照。

動く実例が
[`examples/effects/queen.cul`](../examples/effects/queen.cul) にあります。
バックトラッキングを一言も書かない N-クイーン探索で、`search` は
`perform choose(...)` で列を尋ね `perform reject()` で失敗を告げるだけ。その
意味は外側の `handle` が与えるので、同じ本体が全解の列挙にも、最初の解での
短絡にも、配置数のカウントにもなります。

---

第 II 部 — 抽象化の道具
=======================

## 9. クラス

### 9.1 構文

`class` はコンストラクタ (`new`) とメソッドを宣言する。 `self.x =
...` で設定したフィールドはデフォルトで可変。 インスタンスは可読な
`class:` タグを持つ。

```culebra
class Car {
  new(mpr)  { self.miles = 0; self.mpr = mpr }
  run(n)    { self.miles = self.miles + self.mpr * n }
  total()   { "走行距離: {self.miles} miles" }
}

car = Car.new(5)
car.run(1); car.run(2)
puts(car.total())             # => '走行距離: 15 miles'
puts(car.class)               # => 'Car'
```

クラスそのものを呼び出すのは `.new` のショートハンドです。`Car(5)` は
`Car.new(5)` とまったく同じで、キーワード引数もそのまま渡せます。読みやすい
方を使ってください。クラスはコンストラクタと同じように callable です。

```culebra
class Point { new(x, y) { self.x = x; self.y = y } }
p = Point(3, 4)               # Point.new(3, 4) と同じ
puts("{p.x},{p.y}")           # => '3,4'
```

### 9.2 クロージャベースの別解

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

### 9.3 Static method とフィールド

メソッドやフィールドに `static` を付けるとクラス自身に載る (インス
タンス不要) — ファクトリやクラスレベルの定数を置く自然な場所。

```culebra
class Circle {
  new(r)          { self.r = r }
  static PI       = 3.14
  static unit()   { Circle.new(1) }
  area()          { self.r * self.r * Circle.PI }
}
puts(Circle.unit().area())    # => 3.14
puts(Circle.PI)               # => 3.14
```

static フィールドはクラス宣言時に一度だけ eager に評価される。

### Why `class` とクロージャ両方サポートか

クロージャ as オブジェクトが先に存在し、使い捨てカプセル化 (使い切り
イテレータ、scope guard 等) では今も正解。 `class` 形式は、オブ
ジェクトが遠くまで運ばれてアイデンティティが必要 (`class:` タグ、
`match` やデバッグ出力で使う) になる時に意味を持つ。

## 10. 演算子オーバーロード

### 10.1 特殊メソッド

算術・比較・インデックス・call の各演算子は dunder メソッド
(`__add__` / `__eq__` / `__lt__` / `__index__` / `__call__` 等) に
対応し、クラスがそれを定義すればその演算に参加できる。 逆側メソッド
(`__radd__` 等) はサポートしない — オーバーロードはその演算を所有
する型に置く。 完全なメソッド表とディスパッチ規則は
[language.ja.md §10](language.ja.md) (演算子オーバーロード)。

### 10.2 例: 2 次元ベクトル

```culebra
class Vec2 {
  new(x, y)   { self.x = x; self.y = y }
  __add__(o)  { Vec2.new(self.x + o.x, self.y + o.y) }
  __sub__(o)  { Vec2.new(self.x - o.x, self.y - o.y) }
  __mul__(k)  { Vec2.new(self.x * k, self.y * k) }
  __neg__()   { Vec2.new(-self.x, -self.y) }
  __eq__(o)   { self.x == o.x && self.y == o.y }
  show()      { "({self.x}, {self.y})" }
}

a = Vec2.new(1, 2)
b = Vec2.new(3, 4)
puts((a + b).show())          # => '(4, 6)'
puts((b - a).show())          # => '(2, 2)'
puts((a * 3).show())          # => '(3, 6)'
puts((-a).show())             # => '(-1, -2)'
puts(a == Vec2.new(1, 2))     # => true
```

### 10.3 `__call__` で callable インスタンス

クラスに `__call__` を定義すると、そのインスタンスを直接呼び出せる。

```culebra
class Adder {
  new(n)        { self.n = n }
  __call__(x)   { x + self.n }
}

add5 = Adder.new(5)
puts(add5(10))                # => 15
puts(add5(99))                # => 104
```

## 11. UFCS とマルチメソッド

### 11.1 UFCS 解決順

`x.name(args)` では、既存のプロパティ/メソッド `name` が常に優先
され、無ければスコープ内の自由関数 `name` が `name(x, args)` として
呼ばれる。 完全な解決順序 (`DOT` + 呼び出しリストの要件を含む) は
[language.ja.md §10](language.ja.md) (Methods and UFCS)。

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

### 11.2 マルチメソッド (自由関数の多重ディスパッチ)

同名で別型の関数を複数定義。 ランタイムが引数の宣言型に対する最具
体マッチを選ぶ。

```culebra
class Circle { new(r) { self.r = r } }
class Square { new(s) { self.s = s } }

fn area(c: Circle) { 3.14159 * c.r * c.r }
fn area(s: Square) { s.s * s.s }
fn area(n: Long)   { n }                     # 数値のフォールバック

puts(area(Circle.new(2)))                    # => 12.56636
puts(area(Square.new(3)))                    # => 9
puts(area(10))                               # => 10
```

ディスパッチは positional / kwargs / `**splat` 全部カバー、Union
で注釈したパラメータ (`x: Long | String`、Ch.14.2) もここに参加する。
完全なディスパッチ/優先度規則は [language.ja.md §20](language.ja.md)。

インスタンスメソッドも同じ方式でディスパッチする — クラスは同名で
パラメータ型の異なるメソッドを複数宣言できる:

```culebra
class Calc {
  new() {}
  go(x: Long)   { "long" }
  go(x: String) { "string" }
}
c = Calc.new()
puts(c.go(1))                  # => 'long'
puts(c.go('a'))                # => 'string'
```

### 11.3 ディスパッチ拡張

> **Status: Planned.** hot なディスパッチ経路向けの call-site 単位
> inline cache がロードマップ上 — 現状は毎回オーバーロード集合を
> 再解決する。 class ベース (nominal) の継承は検討の上不採用 — 型
> ファミリー全体にわたる多態は trait ディスパッチ (Ch.14.3) 側で担
> う。UFCS と無理なく合成でき、サブタイピングの物語を増やさずに済む
> ため。

### Why 自由関数から先か

自由関数のマルチメソッドは UFCS や import された namespace と無
理なく合成できる (暗黙のサブタイピングが入らない)。 メソッドマル
チメソッドは own-class vs UFCS vs free の優先順序を決める必要が
あり、推測より実ワークロードで決めたい。

## 12. デコレータ

### 12.1 `@deco`

`fn` (または `class`) の前に置く `@deco` は、`deco(original)` の
結果を元の名前に束縛する。 マルチメソッドとの相互作用を含む完全な
仕様は [language.ja.md §21](language.ja.md)。

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

### 12.2 ファクトリとスタック

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

外側デコレータが内側の結果をラップする。 上から下に読むと実行順と
一致。

### 12.3 Memoize の実例

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

### 12.4 `fn.params` introspection

`Function` 値は宣言時のシグネチャを露出する。 `@autograd` / `@trace`
のような signature を知る必要のあるデコレータはこれを使って書ける。

```culebra
add_typed = fn (a: Long, b: Long) -> Long { a + b }
puts(add_typed.params.map(|p| p.name))    # => ['a', 'b']
puts(add_typed.return_type)               # => 'Long'
```

デコレートされた関数は単一値 (ラップされたクロージャ) として束縛
される — これは「同名の `fn` が複数共存する」というマルチメソッド
の形と相容れない。名前ごとにどちらか一方を選ぶ (完全な規則は
[language.ja.md §21](language.ja.md))。

## 13. モジュール

### 13.1 暗黙 import

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

### 13.2 エントリ環境の隔離と循環

エントリファイルで導入された束縛は import されたファイルからは見え
ない。 import されたファイル内で導入された束縛はビルドから到達可能
などこからでも見える — Go の直観 (パッケージ内全ファイルが「パッ
ケージ」) に揃え、エントリだけは最上位 main 扱い。 ファイル間の循環
参照はモジュールビルド時に検出され、ファイル/行付きで拒絶される
(共通の第三ファイルで分解)。 完全な解決規則と循環検出は
[language.ja.md §24](language.ja.md)。

### Why 暗黙か

1500 行のプログラムで 30 個のヘルパファイルを取り込むケースを想像
する。 個別 `import` を要求する得は無い — 未解決識別子からツールが
同じグラフを導けるから。 著作ループが速く・単純になる代わりに、
tree-shaking もちゃんと効く (リーチャブルなトップレベルがツールで
分かるため、Ch.18)。

リゾルバ設計と循環検出アルゴリズムの詳細は [`internals.ja.md` §11](internals.ja.md)。

---

第 III 部 — 型とライブラリ
==========================

## 14. 型システム

### 14.1 現状: オプショナル注釈 + `Any`

注釈は 3 つの境界での**ランタイム**チェック: 変数代入、関数パラ
メータ渡し、関数戻り値。 静的な narrowing は無い。 完全な注釈仕様は
[language.ja.md §14](language.ja.md)。

```culebra
add = fn (a: Long, b: Long) -> Long { a + b }
puts(add(3, 4))               # => 7

# Any は全部受ける。型注釈と動的パラメータは混在できる
identity = fn (x: Any) -> Any { x }
describe = fn (v, label: String) -> String { "{label}: {v}" }
puts(identity(42))                  # => 42
puts(describe([1, 2], 'array'))     # => 'array: [1, 2]'
```

`type_of` (Ch.2.1) が組み込み型のランタイム introspection。
`match` 節の `n: ClassName` (Ch.6) はそのクラスのインスタンスに
マッチ。

### 14.2 Union / Optional / Tuple

`Long | String` はどちらの型も受け付け、`T?` は `T | Nil` の糖衣、
`(Long, String)` は固定長・不変・要素ごと等価な `Tuple`。 完全な
仕様は [language.ja.md §14](language.ja.md) (Union types /
Optional types) と [language.ja.md §10](language.ja.md) (Tuples)。

```culebra
show = fn (x: Long | String) -> String { to_string(x) }
puts(show(1))                  # => '1'
puts(show('hi'))               # => 'hi'

pair = (1, 'one')
puts(type_of(pair))            # => 'Tuple'
puts(pair == (1, 'one'))       # => true
```

### 14.3 Trait / Protocol

`trait` は必須メソッド集合を宣言する。 それらに (名前・アリティが)
マッチするメソッドを持つクラスなら、明示 `impl` 無しで conform し
(structural conformance)、必須メソッドを欠くクラスは黙って通らず
ディスパッチが失敗する (`DispatchError`)。 trait はデフォルト実装
メソッドも持てて `@derive` で導出できる。 完全な仕様は
[language.ja.md §14](language.ja.md) (Traits and protocols)。
nominal (class) 継承がこの structural モデルのために不採用になった
経緯は [`record.ja.md`](record.ja.md) 参照 (Ch.11.3)。

```culebra
trait Greeter { hello() -> String }

class Bob {
  new(name)  { self.name = name }
  hello()    { "hi, {self.name}" }
}

greet = fn (x: Greeter) -> String { x.hello() }
puts(greet(Bob.new('Alice')))   # => 'hi, Alice'
```

### 14.4 Generic

`Array<Long>` のような注釈は要素型をドキュメント化し、マルチメソッ
ドの specificity (Ch.11.2) にも使われる。 要素チェック自体は no-op
— Rust/Swift の generic を精神的に踏襲するが、アクセスの度に要素を
ランタイムチェックするコストは払わない。 bound 制約と generic クラ
ス宣言は [language.ja.md §14](language.ja.md)。

```culebra
first = fn (xs: Array<Long>) -> Long { xs[0] }
puts(first([1, 2, 3]))         # => 1
```

### TypeScript から意図的に取り入れない要素

Conditional types、mapped types、template literal types、ユーティリ
ティ型群 (`Pick` / `Omit` 等) は、小さな動的言語には複雑すぎる。
目標は Rust/Swift の表現力であって、TS の表現力ではない。

## 15. 標準ライブラリ巡り

CLI ドライバは `puts` / `print` を `IO.puts` / `IO.print` のエイ
リアスとして提供する。 自前で環境を組み立てる埋め込み用途では
namespace は見えるが bare alias は無い。

### 15.1 コア組み込み

```culebra
puts(to_long('42'))           # => 42
puts(to_string([1, 2]))       # => '[1, 2]'
puts(iota(2, 5))              # => [2, 3, 4]
assert_eq(1 + 1, 2)           # 成功時は無音、失敗で throw
```

完全な一覧は [language.ja.md §19](language.ja.md)。

### 15.2 `Math`

```culebra
puts(Math.abs(-7))            # => 7
puts(Math.min(3, 5))          # => 3
puts(Math.max(3, 5))          # => 5
puts(Math.pow(2, 10))         # => 1024
puts(Math.sign(-42))          # => -1
puts(Math.clamp(15, 0, 10))   # => 10
```

完全リファレンス: [`stdlib.ja.md` §1](stdlib.ja.md)。

### 15.3 `IO`

```culebra
print('Hello, '); print('world!'); print("\n")   # => Hello, world!
# IO.input()                 # 標準入力から 1 行
# FS.write('out.txt', 'hi')  # ファイル書き込み
# FS.read('in.txt')          # ファイル読み込み
```

完全リファレンス: [`stdlib.ja.md` §2](stdlib.ja.md)。

### 15.4 `Sys` / `Random` / `FS` / `Time` / `Args`

```culebra
puts(Sys.argv)                # => []
# Sys.env('HOME')             # プロセス環境
# Sys.exit(0)                 # 終了

puts(Random.int(0, 100) >= 0)          # => true

# Path — パスを持ち回る FS の流暢なラッパ:
#   let cfg = Path.new('/etc') / 'app.conf'   # `/` で結合
#   cfg.parent().name(); cfg.read()           # プロパティ + FS 操作
# FS.* や File.open も Path を直接受けます。
```

完全リファレンス: `Sys` [`stdlib.ja.md` §7](stdlib.ja.md)、`Random` §6、
`FS`/`Path` §3、`Time` §5、`Args` §11。

### 15.5 `Regex`

線形時間マッチ (NFA ベース、catastrophic backtracking 無し)、
grapheme cluster 単位がデフォルト。 `/u` フラグ不要。

```culebra
re = Regex.compile('\d+')
puts(re.test('order #42'))    # => true

# `re"..."` リテラルは Regex.compile(pattern, flags) の糖衣。
# body は raw なので `\d` はそのまま通る。
puts(re'\d+'.test('abc 123')) # => true
```

完全な API: [`stdlib.ja.md` §15](stdlib.ja.md)。

### 15.6 `Hash` / `Encoding` / `Compress`

`Hash.sha256`/`sha1`/`sha512`/`md5` とその `hmac_*` 系は hex ダイ
ジェストを返す。 `Encoding.base64`/`hex`/`url`/`html` はそれぞれ
`.encode`/`.decode` を持つ。 `Compress.gzip`/`gunzip` がデータ系
namespace を締めくくる。 JSON は独立の top-level `JSON.parse`/
`JSON.stringify` — `Encoding.json` は無い。 完全リファレンス:
[`stdlib.ja.md`](stdlib.ja.md) §17 (Encoding)、§18 (Compress)、
§19 (Hash)。

### 15.7 `Http`

Blocking なクライアントとサーバ、SSE / WebSocket 含む、TLS は
statically link した BoringSSL。 `async` / `await` 無し — 並行は
スレッドで、スケール上限は数千接続。

```culebra
# doctest: skip
res = Http.get('https://example.com')
puts(res.status)              # => 200

# サーバ
srv = Http.server()
srv.get('/', fn (req) { 'hello' })
srv.listen('127.0.0.1', 8080)
```

streaming・ルーティング・クライアントセッション API:
[`stdlib.ja.md` §16](stdlib.ja.md)。

### 15.8 さらに計画中

> **Status: Planned (Tier 2/3).** `Crypto` (`Hash` を超える非対称鍵/TLS
> プリミティブ) と `Sockets` (raw TCP/UDP)。 順序未確定、Ch.0 のティア
> 方針どおり demand-driven で。

## 16. Tensor プリミティブ

### 16.1 構築・matmul・ブロードキャスト

`Tensor` は組み込みの n 次元配列で、BLAS にルーティングされる
(macOS は Apple Accelerate、Linux は OpenBLAS)。 格納は F32 で、
スカラー結果は `Float` として返る。 matmul (`dot`) は遅延グラフを
作り、`Tensor.eval` が単一の BLAS カーネルとして実行する。要素ごと
の演算は NumPy 同様にブロードキャストする。 完全な API (shape・
reduction・autograd) は [`stdlib.ja.md` §9](stdlib.ja.md)。

```culebra
a = Tensor.from([1.0, 2.0, 3.0])
b = Tensor.from([10.0, 20.0, 30.0])
puts((a + b).to_array())      # => [11.0, 22.0, 33.0]
puts(a.sum())                 # => 6.0

m = Tensor.from([[1.0, 2.0], [3.0, 4.0]])
c = m.dot(m)
Tensor.eval(c)
puts(c.to_array())            # => [[7.0, 10.0], [15.0, 22.0]]
```

### 16.2 GPU プリミティブ

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

dtype の根拠、アロケータ選定、lazy shape の議論は
[`internals.ja.md` §9](internals.ja.md)。

---

第 IV 部 — 検証とデプロイ
=========================

## 17. テスト (`culebra test`)

> `test()` / `@test` / `@parametrize` と matcher 群、 引数で DI 解決
> される fixture (decorator 不要、 env 内の任意の fn) が実装済で、
> `culebra test [path]` で動きます。

### 17.1 doctest 規約 (確定)

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

### 17.2 テストの書き方

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
  new()    { self.conn = Database.connect("memory") }
  drop()   { self.conn.close() }
  users()  { self.conn.users }
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
してしまう (test 本体実行前) ので、 cleanup には class `drop` を
使います。 複数の test ファイルで共有したい長寿命 state (例: 1 回
ロードした model) は module top-level に置きます — モジュールシス
テム (Ch.13) が binding を cache するためです。

**matchers**。 アサーションは matcher 一族を使います — `assert`
キーワード / builtin は存在しません。 matcher は **3 backend の
global** として bind されており (`puts` / `Math` と同じ立場)、
`culebra script.cul`、 `culebra --jit script.cul`、 `culebra build`、
`culebra test` のいずれでも同じく動きます:

```culebra
# doctest: skip
assert_eq(arr.len(), 3)                 # == ; 失敗時に両辺を表示
assert_throws("TypeError", fn() { let _ = 1 + 'b' })
assert_close(0.1 + 0.2, 0.3, 1e-9)      # |a - b| <= tol
```

完全な matcher 一覧 (`assert_true`/`false`/`ne`/`lt`/`le`/`gt`/`ge`
と `__eq__`/`__lt__` dispatch の規則) は
[`stdlib.ja.md` §14](stdlib.ja.md)。

**production の不変条件**。 テストスイート外で `if (!cond) throw {...}`
を書くときは `if`/`throw` を直接書きます (Go 流儀、
[language.ja.md §15](language.ja.md) 参照)。 production build で
disable する別の `assert` キーワードは存在しません。

### 17.3 実行

`culebra test [path]` がテストファイルを discover します。 このサブコマンド
経由で起動した場合のみ、 `test` / `@test` / `@parametrize` が
**ambient global** として注入されます — `import` 不要。 matcher 一族は
3 backend で常時 global なので追加注入は不要です。 これは script 実行
モード下でだけ `puts` / `print` が ambient で、 `culebra::environment()`
には注入されない設計と同じ流儀 ([stdlib.ja.md §12](stdlib.ja.md) 参照)。

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
{"event":"test_pass","name":"adds_correctly","source":"tests/test_math.cul","stdout":""}
{"event":"test_fail","name":"divides_correctly","kind":"AssertionError",
 "message":"assert_eq failed:\n  left:  3\n  right: 4","line":12,"col":3,"stdout":""}
{"event":"run_end","passed":42,"failed":1,"errored_files":0,"bailed":false}
```

JSON モードでは test 内の `puts(...)` は event の `stdout` フィールド
に capture され NDJSON ストリームに interleave しません。 失敗 event
には `snippet` が付き、 失敗行を `>` で marker した文脈が含まれます。

`just test` 経由の従来 `tests/*.cul` スイート (matcher 使用、`test()`
呼出なし) は各ファイルを `./build/culebra <f>` / `--jit <f>` /
`culebra build <f>` で直接実行します。 matcher は language-level
global なので `culebra test` を介さずに 3 backend で同じファイルが
回ります。

### 17.4 今後の拡張

- **明示 `import { test } from "std/test"`** — `culebra test` 経由でない
  コード (embedded test helper 等) で使うため
- **`culebra test --doc docs/`** — 16.1 の規約に従って markdown から
  ` ```culebra ` ブロックを抽出・実行
- **`--backend interp|jit|aot`** — 現状 runner は interp のみ。
  backend 選択は今後
- **並列実行** — 現状逐次。 JIT/AOT が入った時の parallel default は
  optional

## 18. リント (`culebra lint`)

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
- **警告** — 実行を止めない助言的な指摘:
  - **未使用ローカル変数** — 関数内の `let`/`mut` 束縛で一度も読まれない
    もの。
  - **未使用トップレベル束縛** — トップレベルの `let`/`mut` で、モジュール
    が一度も読まず re-export もしないもの。関数/クラス/enum/trait 宣言は
    モジュールの export 面なので報告しない。
  - **未使用 import** — `import` した名前をモジュールが一度も使わないもの。
  - **到達不能コード** — 同じブロック内で `return`/`throw`/`break`/
    `continue` の後に置かれ、決して実行されない文。

  先頭アンダースコア (`_x`、または素の sink `_`) は意図的な未使用の印で、
  決して報告しない。**パラメータは報告しない**: Culebra では未使用
  パラメータはほぼ意図的 — 多重ディスパッチの節やメソッド署名が arity を
  固定し、高階コールバック (ルートハンドラ `fn(req)` や `|i| 4.0`) は宣言
  必須だが使わない引数を持つ — ため、検査してもノイズにしかならない。

予定: エディタ/LSP 連携用の `--format json`、インライン
`# lint: ignore` 抑制。

## 19. フォーマット (`culebra fmt`)

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

stdin 形式 (`culebra fmt -`) が整形フック。各統合はバッファ全体を整形し、
exit 0 の時だけ結果を適用する (parse/安全網エラー時はバッファ不変)。

- **VSCode** — 同梱拡張 (`misc/vscode/`) が document formatting provider
  を登録するので、`.cul` で **Format Document** と `editor.formatOnSave`
  がそのまま動く (`build-vsix.sh` / `install.sh` で再ビルド・再インストール)。
- **Zed** — `settings.json` の `"languages": { "Culebra": { ... } }` に
  `"formatter": { "external": { "command": "culebra", "arguments": ["fmt", "-"] } }`
  を追加。
- **Vim/Neovim** — 同梱 `ftplugin` が `:CulebraFmt` コマンドを提供 (全体
  整形・カーソル保持・エラー時不変)。保存時整形は
  `let g:culebra_fmt_autosave = 1`。`gq` / `'formatprg'` には**あえて紐付
  けない** (パース不能な部分範囲を空出力で置換してしまうため)。
- 他のエディタも「保存時整形」機構があれば同様にバッファを
  `culebra fmt -` に通せる。

## 20. AOT バイナリビルド

`culebra build` は `.cul` ソースを ahead-of-time で自己完結バイナ
リにコンパイルする。 ランタイムに LLVM 不要。 tree-shaking で使われ
ないランタイムヘルパを落とす。 Tensor を使わないプログラムでは
Accelerate / BLAS フレームワーク依存も外せる。

```bash
./build/culebra build my-program.cul -o ./out
./out                                     # standalone、~350 KB on macOS
otool -L ./out                            # Accelerate も LLVM も無し
```

### 20.1 クロスコンパイル

```bash
./build/culebra build my-program.cul \
  --target=x86_64-unknown-linux-gnu \
  --sysroot=$LINUX_SYSROOT \
  --rt-lib=$PWD/build-linux-x86_64/libculebra_rt.a \
  -o ./out-linux
```

ランタイムアーカイブのビルド、sysroot の用意、クロスコンパイル全
ワークフローの詳細は [`deployment.ja.md`](deployment.ja.md)。

### Why tree-shaking が効くか

`puts` だけ使う "hello world" は FFT も HTTP ランタイムも要らない。
エントリファイルから call graph を辿ることで、参照されていないラン
タイムヘルパ (~200 個) を落とせる。 `Tensor` 参照が無ければ no-BLAS
archive に差し替わるので、数 MB が数百 KB になる。

## 21. 埋め込み概観

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
(`libculebra_rt.a`) の詳細は [`deployment.ja.md`](deployment.ja.md)。

---

次の一歩
--------

- 厳密な文法と評価規則: [`language.ja.md`](language.ja.md)
- API リファレンス: [`stdlib.ja.md`](stdlib.ja.md)
- 実装の内部詳細: [`internals.ja.md`](internals.ja.md)
- バイナリビルド・埋め込み・ラッピング: [`deployment.ja.md`](deployment.ja.md)
- 大きめの実例: [`benchmarks/microgpt/`](../benchmarks/microgpt/)
- インタラクティブな REPL: `./build/culebra --shell`
