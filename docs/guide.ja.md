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
> クティブ一覧の全体は [`tooling.ja.md` §1](tooling.ja.md#doctest)。

> **Status ラベル。** ラベル無しの見出しは現時点の実装を記述します。
> 出現するラベル: **Draft** (実装中、API 変更あり)、**Planned**
> (採用決定、未実装)、**Deprecated** (将来削除予定)。

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
  17. [ツール (`test`, `lint`, `fmt`, デバッグ)](#17-ツール-test-lint-fmt-デバッグ)
  18. [AOT バイナリビルド](#18-aot-バイナリビルド)
  19. [埋め込み概観](#19-埋め込み概観)

## 0. 設計哲学

ここを 1 度読めば、以降の章は前提知識として扱えます。

- **2 バックエンド、1 AST。** ツリーウォーキング型インタプリタと
  LLVM ORC JIT が同じ AST を共有。 インタプリタは LLVM 非依存
  (~1 MB バイナリ、埋め込み向き)、JIT は `-O2` で同じプログラムを
  実行。 両方を維持 — どちらも捨てません。
- **日常的に使う 8 つの型。** `Nil` / `Bool` / `Long` / `Float` /
  `String` / `Array` / `Object` / `Function`、加えて用途特化の 4 つ
  (`StringView` / `Tuple` / `Set` / `Tensor`)。 クラス・モジュール・
  エラーなどはすべて `Object` 上に構築。
- **Rust 風の表面構文。** `let` / `mut` / `fn` / `match` / ブロッ
  クは式。 クロージャは第一級、エラーは値、隠れたグローバル無し。
- **UFCS、パイプライン不採用。** 任意の自由関数 `f(x, ...)` を
  `x.f(...)` として呼べる。 パイプライン演算子は検討の上不採用。
- **明示的で静的なモジュール。** ファイルは `export { ... }` で束縛
  を公開し、利用側は `import name from './path.cul'` で束縛する。
  どちらもトップレベル専用なので依存グラフはパース時に確定し、
  それが AOT バンドルと tree-shaking を可能にしている (Ch.13)。
- **async/await 無し。** I/O はブロッキング設計、並行はスレッドで。
  HTTP 等のネットワークスタックはブロッキング、典型的なスケール
  上限は数千接続。
- **batteries-included、ティア制。** コア
  (Math/IO/FS/File/Sys/Random/String/Time/Args) と Tier 1
  (Regex/Http/Hash/Encoding/Compress/JSON/CSV/TOML/SQLite/UUID/
  Log/Term/Canvas) はどちらも出荷済み。 Tier 2/3 (Crypto、Sockets)
  は需要次第 — Ch.15 参照。
- **1.0 前。** ソース・API は変わる可能性。 リリース機構 (バージョン
  タグ、CHANGELOG、Homebrew formula 等) は 1.0 後。

---

第 I 部 — 言語コア
==================

## 1. Hello & セットアップ

インタプリタ (LLVM 20+ があれば JIT も) をビルド:

```bash
just build              # JIT 付き
just build-no-jit       # インタプリタのみ、~1 MB
just dev                # LTO 無し -O1 の高速ビルド → build-dev/ (内側ループ用)
just test-dev           # build-dev/ で interp==JIT を素早く確認 (各編集ごと)
just test               # 全 backend + embed スモークテスト (並列; JOBS=1 で逐次化)
just test wrap          # `culebra wrap` の端から端まで (`just test` には含まれない)
./build/culebra --shell # REPL (--jit で JIT REPL)
```

Culebra ソースの拡張子は `.cul`。 `culebra` バイナリで実行:

```bash
echo "inspect('hello, culebra!')" > hello.cul
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
inspect('hello')          # => 'hello'
```

`inspect` は値をクォート付きの inspect 形式で出力するため、文字列は引用符付き
(`'hello'`) で、参照型はそのリテラル表記で表示される。引用符の付かない
生のテキストに改行を付けたいときは `println`、改行も不要なら `print` を使う
— 第15章参照。

## 2. 値・束縛・制御フロー

### 2.1 日常的に使う 8 つの型

```culebra
inspect(type_of(nil))            # => 'Nil'
inspect(type_of(true))           # => 'Bool'
inspect(type_of(42))             # => 'Long'
inspect(type_of(3.14))           # => 'Float'
inspect(type_of('hi'))           # => 'String'
inspect(type_of([1, 2]))         # => 'Array'
inspect(type_of({a: 1}))         # => 'Object'
inspect(type_of(fn () { 1 }))    # => 'Function'
```

残り 4 つは必要になったときに出てきます: `StringView` (Ch.4.4)、
`Tuple` と `Set` (Ch.14.2)、`Tensor` (Ch.16)。 全体の表は
[language.ja.md §4](language.ja.md)。

### 2.2 束縛: bare / `let` / `mut`

```culebra
x = 10              # bare: 新規不変束縛、または外側を再代入
let y = 20          # let: 新規不変束縛 (外側のシャドウは不可)
mut z = 30          # mut: 新規可変束縛
z = z + 1           # mut は再代入可能
z += 1              # 複合 (`-= *= /= %= **= @=` も同様)
inspect(z)             # => 32
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
inspect(c())                     # => 1
inspect(c())                     # => 2
```

### 2.4 制御フロー

`if` と `match` (Ch.6) は式 — 選ばれた枝の値を返す。 `while` と
`for` は文 (値は `nil`)。

```culebra
x = 7
sign = if x > 0 { 1 } else if x < 0 { -1 } else { 0 }
inspect(sign)                    # => 1

mut i = 0
while i < 3 { inspect(i); i = i + 1 }
# => |
# 0
# 1
# 2

for n in 0..3   { inspect(n) }   # 排他レンジ
# => |
# 0
# 1
# 2

for n in 0..=2  { inspect(n) }   # 包含レンジ
# => |
# 0
# 1
# 2

for n in 0..10 by 3 { inspect(n) }   # ステップ付きレンジ
# => |
# 0
# 3
# 6
# 9

for k, v in {a: 1, b: 2} { inspect("{k}={v}") }   # Object は key, value を返す
# => |
# 'a=1'
# 'b=2'
```

`break` / `continue` は `while` / `for` 内で動作。

### 2.5 `nobreak` / init 節 / `cond` / `? :`

ループには `nobreak` ブロックを付けられます。 `break` せずに完走した
ときだけ走る — Python の `for ... else` を紛らわしくない名前にした
もの:

```culebra
mut found = nil
for n in [1, 3, 5] {
  if n % 2 == 0 { found = n; break }
} nobreak {
  inspect('偶数なし')            # => '偶数なし'
}
```

`while` / `if` / `match` は **init 節** — `;` で区切る、その構文
だけにスコープする束縛 — を取れるので、ループ変数が外側スコープに
漏れません:

```culebra
while mut i = 0; i < 3 { i = i + 1 }
if let n = 6; n > 5 { inspect('大きい') }     # => '大きい'
```

多分岐には `cond` (主語のない `match`)、2 分岐には三項 `? :`:

```culebra
grade = fn (n) {
  cond {
    n >= 90 => 'A',
    n >= 80 => 'B',
    _       => 'C'
  }
}
inspect(grade(85))               # => 'B'
inspect(grade(50) == 'C' ? 'ok' : 'no')   # => 'ok'
```

クロージャベースのオブジェクトパターン (Ch.9) では、捕捉された状態が
そのままオブジェクトの状態なので、silent shadow はオブジェクトを壊す。
一方、関数内 block での rebinding は日常パターンなので許容する。詳細
なルールと設計根拠は [language.ja.md §6](language.ja.md)。

## 3. 関数とクロージャ

### 3.1 `fn` と `|x|`

```culebra
add = fn (a, b) { a + b }
inspect(add(2, 3))               # => 5

# 型注釈はオプション; 詳細は Ch.14
add_typed = fn (a: Long, b: Long) -> Long { a + b }
inspect(add_typed(2, 3))         # => 5

# |x| expr は fn (x) { expr } の糖衣
square = |x| x * x
inspect(square(7))               # => 49

# 再帰には `fn` (関数自身への参照)
fib = fn (x) {
  if x < 2 { x } else { fn(x - 2) + fn(x - 1) }
}
inspect(fib(10))                 # => 55
```

### 3.2 クロージャ

内側関数は外側束縛を参照で捕捉する。 `mut` を付けると書き換え可能。

```culebra
make_counter = fn () {
  mut n = 0
  fn () { n = n + 1; n }   # bare `n = ...` で捕捉 `n` を更新
}
c = make_counter()
inspect(c())                     # => 1
inspect(c())                     # => 2
inspect(c())                     # => 3
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
inspect(greet('alice'))                       # => 'hi, alice'
inspect(greet('alice', greeting: 'hello'))    # => 'hello, alice'
inspect(greet('bob', formal: true))           # => 'hi, Mr./Ms. bob'

# `**` で Object をキーワード引数として splat
opts = {greeting: 'yo', formal: false}
inspect(greet('carol', **opts))               # => 'yo, carol'
```

`*` マーカーは呼び出し側にオプション名を書かせるので、長いパラメー
タリストが読みやすくなり、再配置・拡張もコール側を壊さない。 末尾の
`*rest` パラメータはその位置引数版で、余った位置引数を Array に
集めます:

```culebra
sum_all = fn (first, *rest) {
  mut t = first
  for v in rest { t = t + v }
  t
}
inspect(sum_all(1, 2, 3, 4))                  # => 10
```

パラメータ・デフォルト値・splat の完全な仕様は
[language.ja.md §11](language.ja.md)。

## 4. 文字列

### 4.1 補間と連結

```culebra
name = 'Culebra'
inspect("hello, {name}!")                   # => 'hello, Culebra!'
inspect("two plus three is {2 + 3}")        # => 'two plus three is 5'
inspect('a' + 'b' + 'c')                    # => 'abc'
```

### 4.2 反復とインデックス

文字列は Unicode スカラ単位で反復 (1 コードポイント = 1 ステップ)。
インデックスは UTF-8 上のバイトオフセット — 範囲外はエラー。

```culebra
for c in 'café' { inspect(c) }
# => |
# 'c'
# 'a'
# 'f'
# 'é'

inspect('café'.size())            # => 5
```

`size()` は UTF-8 表現上のバイト数を返す (`é` は 2 バイトなので `'café'`
は 5)。一方、上の `for` ループは Unicode スカラ単位で 1 ステップずつ進む
(4 ステップ)。

### 4.3 よく使うメソッド

```culebra
inspect('hello world'.split(' '))        # => ['hello', 'world']
inspect('  hi  '.trim())                 # => 'hi'
inspect('abc'.upper())                   # => 'ABC'
inspect('foo'.starts_with('fo'))         # => true
inspect(['a', 'b', 'c'].join('-'))       # => 'a-b-c'
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
  for g in s.graphemes() { inspect(g); break }
}
print_first_grapheme('café')          # => 'c'

inspect(type_of('hello'.slice(1, 4)))    # => 'StringView'
inspect('hello'.slice(1, 4))             # => 'ell'
```

`.graphemes()` は Unicode の *extended grapheme cluster* を lazy に
走査する — ZWJ で連結された複数コードポイントの絵文字ファミリーで
あっても、1 ステップ = ユーザが知覚する 1 文字になる:

```culebra
inspect('a👨‍👩‍👧b'.graphemes().collect().size())    # => 3
inspect('café'.graphemes().collect().size())        # => 4
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
for i in range(3) { inspect(i) }
# => |
# 0
# 1
# 2

# iota は Array を割り当てる
inspect(iota(3))                 # => [0, 1, 2]
inspect(iota(2, 5))              # => [2, 3, 4]
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
inspect(result)                  # => [0, 6, 12, 18, 24]

total = range(1, 11).reduce(0, |a, x| a + x)
inspect(total)                   # => 55

inspect([1, 2, 3, 4].iter().any(|x| x > 3))      # => true
inspect([10, 20, 30].iter().find(|x| x > 15))    # => 20
```

### 5.3 `enumerate` / `zip` / `flat_map` / `skip` / `take_while`

```culebra
for i, v in ['fizz', 'buzz', 'bang'].enumerate() {
  inspect("{i}: {v}")
}
# => |
# '0: fizz'
# '1: buzz'
# '2: bang'

for p in [1, 2, 3].iter().zip(['a', 'b', 'c']) {
  inspect("{p.first} / {p.second}")
}
# => |
# '1 / a'
# '2 / b'
# '3 / c'

flat = [[1, 2], [3], [4, 5, 6]].iter().flat_map(|xs| xs).collect()
inspect(flat)                    # => [1, 2, 3, 4, 5, 6]

head = range(100).skip(10).take_while(|x| x < 15).collect()
inspect(head)                    # => [10, 11, 12, 13, 14]

# chunks: 固定長のグループ (最後だけ短くなりうる)
inspect([1, 2, 3, 4, 5].iter().chunks(2).collect())
# => [[1, 2], [3, 4], [5]]

# windows: 1 要素ずつずらすスライディングビュー
inspect([1, 2, 3, 4].iter().windows(2).collect())
# => [[1, 2], [2, 3], [3, 4]]
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

for v in countdown(3) { inspect(v) }
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
for v in countdown(3) { inspect(v) }
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
inspect(chunk([1, 2, 3, 4, 5], 2).collect())    # => [[1, 2], [3, 4], [5]]
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
inspect(describe(0))             # => 'zero'
inspect(describe(2))             # => 'small'
inspect(describe(999))           # => 'big (999)'
inspect(describe('hi'))          # => 'str (hi)'
inspect(describe([1]))           # => 'other'
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
inspect(classify(-5))            # => -1
inspect(classify(0))             # => 0
inspect(classify(7))             # => 1
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
inspect(shape([]))               # => 'empty'
inspect(shape([10, 20]))         # => 'two (10,20)'
inspect(shape([1, 2, 3, 4]))     # => 'head=1, rest=3'

first_name = fn (people) {
  match people {
    [{name}, ..._] => name,
    _              => 'none'
  }
}
inspect(first_name([{name: 'x'}, {name: 'y'}]))     # => 'x'
inspect(first_name([]))                              # => 'none'
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
inspect(is_even(10))             # => true
inspect(is_even(7))              # => false
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
  inspect(validate(42))          # => 42
  inspect(validate(-1))          # throws、次の行は到達せず
  inspect('unreached')
} catch e {
  inspect("caught: {e}")         # => 'caught: negative: -1'
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
inspect(safe(7))                 # => 7
inspect(safe(-99))               # => 0
```

### 7.3 `defer`

`defer { ... }` は囲むブロックの**全 exit パス** (通常終了 /
`return` / `throw`) で LIFO に実行されるクリーンアップ登録。 ブロッ
ク直下・関数本体直下・トップレベル直下のいずれでも、どのバックエンド
でも同じように発火する。 関数の残りが動く前に後始末したいときだけ、
内側の `{ }` に入れる。 exit パスと順序の完全なルールは
[language.ja.md §15](language.ja.md)。

```culebra
demo = fn (fail) {
  {
    defer { inspect('cleanup A') }
    defer { inspect('cleanup B') }
    if fail { throw 'failed' }
    inspect('work done')
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
  { drop: fn () { inspect("R{id} released") } }
}

inspect('enter')
{
  r = make_resource('X')
}
inspect('exit')
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
inspect(answer)   # => 42
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
inspect(n)   # => 2
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
inspect(both)   # => [10, 20]
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

inspect(handle { safeDiv(10, 2) } with raise(m, k) { -1 })   # => 5
inspect(handle { safeDiv(10, 0) } with raise(m, k) { -1 })   # => -1
```

正常完了値を写すには `with return(v) { … }` を加えます。エフェクトフルな本体の
中に `handle` を書けば、外側の計算をキャプチャしてそこから再開できます。完全な
リファレンスと制約は [language.ja.md §16](language.ja.md) を参照。

効果が一番はっきり出るのは探索です。列を `perform choose(...)` で尋ね、
行き止まりを `perform reject()` で告げる N-クイーン探索は、バックトラッキング
を一言も書きません。その意味は外側の `handle` が与えるので、同じ本体が全解の
列挙にも、最初の解での短絡にも、配置数のカウントにもなります。

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
inspect(car.total())             # => '走行距離: 15 miles'
inspect(car.class)               # => 'Car'
```

クラスそのものを呼び出すのは `.new` のショートハンドです。`Car(5)` は
`Car.new(5)` とまったく同じで、キーワード引数もそのまま渡せます。読みやすい
方を使ってください。クラスはコンストラクタと同じように callable です。

```culebra
class Point { new(x, y) { self.x = x; self.y = y } }
p = Point(3, 4)               # Point.new(3, 4) と同じ
inspect("{p.x},{p.y}")           # => '3,4'
```

フィールドは class 本体でデフォルト値つきに**宣言**することもできる。
各インスタンスが自分のコピーを持ち、`new` の実行前に実体化されるので、
コンストラクタが触らない経路でもフィールドは既知の値で存在する。 `get`
メソッドは計算プロパティで、括弧なしで呼ぶ:

```culebra
class Temp {
  celsius = 0.0
  scale   = 'C'
  new(c) { self.celsius = c }
  get fahrenheit() { self.celsius * 9.0 / 5.0 + 32.0 }
}

t = Temp.new(100.0)
inspect(t.fahrenheit)            # => 212.0
inspect(t.scale)                 # => 'C'
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
inspect(car.total())             # => '走行距離: 15 miles'
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
inspect(Circle.unit().area())    # => 3.14
inspect(Circle.PI)               # => 3.14
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
inspect((a + b).show())          # => '(4, 6)'
inspect((b - a).show())          # => '(2, 2)'
inspect((a * 3).show())          # => '(3, 6)'
inspect((-a).show())             # => '(-1, -2)'
inspect(a == Vec2.new(1, 2))     # => true
```

### 10.3 `__call__` で callable インスタンス

クラスに `__call__` を定義すると、そのインスタンスを直接呼び出せる。

```culebra
class Adder {
  new(n)        { self.n = n }
  __call__(x)   { x + self.n }
}

add5 = Adder.new(5)
inspect(add5(10))                # => 15
inspect(add5(99))                # => 104
```

## 11. UFCS とマルチメソッド

### 11.1 UFCS 解決順

`x.name(args)` では、既存のプロパティ/メソッド `name` が常に優先
され、無ければスコープ内の自由関数 `name` が `name(x, args)` として
呼ばれる。 完全な解決順序 (`DOT` + 呼び出しリストの要件を含む) は
[language.ja.md §10](language.ja.md) (Methods and UFCS)。

```culebra
double = fn (x) { x * 2 }
inspect(42.double())                                  # => 84
inspect('hello world'.split(' ').size())              # => 2

# 既存メソッドが常に優先 — Array の組み込み `reverse` は
# ユーザの `reverse` で上書きされない
reverse = fn (x) { inspect('user reverse NOT called') }
mut a = [1, 2, 3]
a.reverse()
inspect(a)                                            # => [3, 2, 1]
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

inspect(area(Circle.new(2)))                    # => 12.56636
inspect(area(Square.new(3)))                    # => 9
inspect(area(10))                               # => 10
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
inspect(c.go(1))                  # => 'long'
inspect(c.go('a'))                # => 'string'
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
    inspect("calling with {x}")
    f(x)
  }
}

@log
fn double(x) { x * 2 }

inspect(double(7))
# => |
# 'calling with 7'
# 14
```

### 12.2 ファクトリとスタック

```culebra
prefix = fn (tag) {
  fn (f) {
    fn () {
      inspect("[{tag}]")
      f()
    }
  }
}

@prefix('A')
@prefix('B')
fn greet() { inspect('hi') }

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

inspect(slow_square(7))          # => 49
inspect(slow_square(7))          # => 49
```

### 12.4 `fn.params` introspection

`Function` 値は宣言時のシグネチャを露出する。 `@autograd` / `@trace`
のような signature を知る必要のあるデコレータはこれを使って書ける。

```culebra
add_typed = fn (a: Long, b: Long) -> Long { a + b }
inspect(add_typed.params.map(|p| p.name))    # => ['a', 'b']
inspect(add_typed.return_type)               # => 'Long'
```

デコレートされた関数は単一値 (ラップされたクロージャ) として束縛
される — これは「同名の `fn` が複数共存する」というマルチメソッド
の形と相容れない。名前ごとにどちらか一方を選ぶ (完全な規則は
[language.ja.md §21](language.ja.md))。

## 13. モジュール

### 13.1 `export` と `import`

モジュールは公開するものを `export` で列挙し、利用側は `import` で
モジュール全体を 1 つの名前に束縛する。

```culebra
# doctest: skip
# lib.cul
let greet = fn (name) { "hello, {name}" }
let PI    = 3.14159
let helper = fn () { 'internal' }   # export しない

export { greet, PI }
```

```culebra
# doctest: skip
# main.cul — lib.cul と同じディレクトリ
import lib from './lib.cul'

inspect(lib.greet('world'))      # => 'hello, world'
inspect(lib.PI)                  # => 3.14159
inspect(lib.helper)              # => nil — export Object に載っていない
```

パスはシングルクォートのリテラルで、import する側のファイルの
ディレクトリを基準に解決される。 1 ファイル内の複数の `export` は
マージされるので、ヘルパを宣言 → export → さらに宣言、と書ける。

### 13.2 トップレベル限定・1 度だけ評価

`import` と `export` はトップレベル文としてのみ書ける — 関数内や
`if` の枝に書くと `SyntaxError`。 これによりローダはパース時に依存
グラフ全体を確定でき、AOT バンドラと tree-shaker がそれに依存して
いる (Ch.18)。

各モジュールはプログラム中で 1 度だけ、依存順に、それぞれ独自の
スコープで評価される。 トップレベル束縛は export Object 以外は非公開。
循環 import (A が B を、B が A を) は循環を示す `ImportError` で
拒絶される。 完全な解決規則・キャッシュ・エラーは
[language.ja.md §24](language.ja.md)。

### Why 明示か

明示的な `import` 行があれば、そのファイルが何に依存しているかは
—読者にとってもツールにとっても— そこだけ見れば分かる。 `culebra
lint` の未使用 import 警告 (と `--fix`、Ch.17) が曖昧さなく出せるのも、
AOT ビルドが推測なしにバンドルできるのも同じ理由。

ローダ設計と循環検出アルゴリズムの詳細は [`internals.ja.md` §10](internals.ja.md)。

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
inspect(add(3, 4))               # => 7

# Any は全部受ける。型注釈と動的パラメータは混在できる
identity = fn (x: Any) -> Any { x }
describe = fn (v, label: String) -> String { "{label}: {v}" }
inspect(identity(42))                  # => 42
inspect(describe([1, 2], 'array'))     # => 'array: [1, 2]'
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
inspect(show(1))                  # => '1'
inspect(show('hi'))               # => 'hi'

pair = (1, 'one')
inspect(type_of(pair))            # => 'Tuple'
inspect(pair == (1, 'one'))       # => true
```

### 14.3 Trait / Protocol

`trait` は必須メソッド集合を宣言する。 それらに (名前・アリティが)
マッチするメソッドを持つクラスなら、明示 `impl` 無しで conform し
(structural conformance)、必須メソッドを欠くクラスは黙って通らず
ディスパッチが失敗する (`DispatchError`)。 trait はデフォルト実装
メソッドも持てて `@derive` で導出できる。 nominal (class) 継承は
検討の上、この structural モデルを採って不採用とした (Ch.11.3)。
完全な仕様は [language.ja.md §14](language.ja.md) (Traits and
protocols)。

```culebra
trait Greeter { hello() -> String }

class Bob {
  new(name)  { self.name = name }
  hello()    { "hi, {self.name}" }
}

greet = fn (x: Greeter) -> String { x.hello() }
inspect(greet(Bob.new('Alice')))   # => 'hi, Alice'
```

### 14.4 Generic

`Array<Long>` のような注釈は要素型をドキュメント化し、マルチメソッ
ドの specificity (Ch.11.2) にも使われる。 要素チェック自体は no-op
— Rust/Swift の generic を精神的に踏襲するが、アクセスの度に要素を
ランタイムチェックするコストは払わない。 bound 制約と generic クラ
ス宣言は [language.ja.md §14](language.ja.md)。

```culebra
first = fn (xs: Array<Long>) -> Long { xs[0] }
inspect(first([1, 2, 3]))         # => 1
```

### TypeScript から意図的に取り入れない要素

Conditional types、mapped types、template literal types、ユーティリ
ティ型群 (`Pick` / `Omit` 等) は、小さな動的言語には複雑すぎる。
目標は Rust/Swift の表現力であって、TS の表現力ではない。

## 15. 標準ライブラリ巡り

CLI ドライバは `inspect` / `print` / `println` を `IO.inspect` /
`IO.print` / `IO.println` のエイリアスとして提供する。 自前で環境を組み
立てる埋め込み用途では namespace は見えるが bare alias は無い。

### 15.1 コア組み込み

```culebra
inspect(to_long('42'))           # => 42
inspect(to_string([1, 2]))       # => '[1, 2]'
inspect(iota(2, 5))              # => [2, 3, 4]
assert_eq(1 + 1, 2)           # 成功時は無音、失敗で throw
```

完全な一覧は [language.ja.md §19](language.ja.md)。

### 15.2 `Math`

```culebra
inspect(Math.abs(-7))            # => 7
inspect(Math.min(3, 5))          # => 3
inspect(Math.max(3, 5))          # => 5
inspect(Math.pow(2, 10))         # => 1024
inspect(Math.sign(-42))          # => -1
inspect(Math.clamp(15, 0, 10))   # => 10
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
inspect(Sys.argv)                # => []
# Sys.env('HOME')             # プロセス環境
# Sys.exit(0)                 # 終了

inspect(Random.int(0, 100) >= 0)          # => true

# Path — パスを持ち回る FS の流暢なラッパ:
#   let cfg = Path.new('/etc') / 'app.conf'   # `/` で結合
#   cfg.parent().name(); cfg.read()           # プロパティ + FS 操作
# FS.* や File.open も Path を直接受けます。
```

完全リファレンス: `Sys` [`stdlib.ja.md` §7](stdlib.ja.md)、`Random` §6、
`FS`/`Path` §3、`Time` §5、`Args` §10。

### 15.5 `Regex`

線形時間マッチ (NFA ベース、catastrophic backtracking 無し)、
grapheme cluster 単位がデフォルト。 `/u` フラグ不要。

```culebra
re = Regex.compile('\d+')
inspect(re.test('order #42'))    # => true

# `re"..."` リテラルは Regex.compile(pattern, flags) の糖衣。
# body は raw なので `\d` はそのまま通る。
inspect(re'\d+'.test('abc 123')) # => true
```

完全な API: [`stdlib.ja.md` §14](stdlib.ja.md)。

### 15.6 `Hash` / `Encoding` / `Compress`

`Hash.sha256`/`sha1`/`sha512`/`md5` とその `hmac_*` 系は hex ダイ
ジェストを返す。 `Encoding.base64`/`hex`/`url`/`html` はそれぞれ
`.encode`/`.decode` を持つ。 `Compress.gzip`/`gunzip` がデータ系
namespace を締めくくる。 JSON は独立の top-level `JSON.parse`/
`JSON.stringify` — `Encoding.json` は無い。 完全リファレンス:
[`stdlib.ja.md`](stdlib.ja.md) §16 (Encoding)、§17 (Compress)、
§18 (Hash)。

### 15.7 `Http`

Blocking なクライアントとサーバ、SSE / WebSocket 含む、TLS は
statically link した OpenSSL。 `async` / `await` 無し — 並行は
スレッドで、スケール上限は数千接続。

```culebra
# doctest: skip
res = Http.get('https://example.com')
inspect(res.status)              # => 200

# サーバ
srv = Http.server()
srv.get('/', fn (req) { 'hello' })
srv.listen('127.0.0.1', 8080)
```

streaming・ルーティング・クライアントセッション API:
[`stdlib.ja.md` §15](stdlib.ja.md)。

### 15.8 ライブラリの残り

上で触れていないもの（すべて [`stdlib.ja.md`](stdlib.ja.md) に記載）:
`JSON` (§9)、`Proc` (§11 — 外部コマンド実行)、`Isolate` / `Channel` /
`Parallel` / `Shared` (§12 — スレッドとメッセージパッシング)、
`CSV` (§19)、`Env` (§20 — dotenv)、`UUID` (§21)、`Term` (§22 — TUI)、
`Log` (§23)、`TOML` (§24)、`SQLite` (§25)、`Canvas` (§26 — 2D ゲーム)、
`Scene` (§27 — 3D、opt-in)、`Desktop` / `Webview` (§28 — ネイティブ
WebView のデスクトップアプリを 1 呼び出しで)。

### 15.9 まだ計画中

> **Status: Planned (Tier 2/3).** `Crypto` (`Hash` を超える非対称鍵/TLS
> プリミティブ) と `Sockets` (raw TCP/UDP)。 順序未確定、Ch.0 のティア
> 方針どおり demand-driven で。

## 16. Tensor プリミティブ

### 16.1 構築・matmul・ブロードキャスト

`Tensor` は組み込みの n 次元配列で、vendored な `cpp-tensorlib`
エンジン (ベクトル化 CPU カーネル、macOS は Metal、Linux/Windows は
CUDA) が実行する。 格納は F32 で、スカラー結果は `Float` として返る。
matmul (`dot`) は遅延グラフを作り、`Tensor.eval` が融合された単一
カーネルとして実行する。要素ごとの演算は NumPy 同様にブロードキャスト
する。 完全な API (shape・reduction・autograd・デバイス選択) は
[`stdlib.ja.md` §8](stdlib.ja.md)。

```culebra
a = Tensor.from([1.0, 2.0, 3.0])
b = Tensor.from([10.0, 20.0, 30.0])
inspect((a + b).to_array())      # => [11.0, 22.0, 33.0]
inspect(a.sum())                 # => 6.0

m = Tensor.from([[1.0, 2.0], [3.0, 4.0]])
c = m.dot(m)
Tensor.eval(c)
inspect(c.to_array())            # => [[7.0, 10.0], [15.0, 22.0]]
```

### 16.2 デバイスの選択

GPU でも同じ `Tensor` 型を使う — GPU 専用の型は無い。 デバイス選択は
プロセスグローバルで、実行時に切り替えられる:

```culebra
inspect(type_of(Tensor.gpu_available()))   # => 'Bool'
# Tensor.use_gpu()    # GPU バックエンド (Metal / CUDA) を強制
# Tensor.use_cpu()    # CPU バックエンドを強制
# Tensor.use_auto()   # 問題サイズごとに選択 (デフォルト)
```

`Tensor.gpu_available()` は GPU バックエンドがビルドに含まれ、かつ
到達可能かを返す。 含まれない場合 `use_auto` は CPU にフォールバック
する。 GPU が勝つかは形状次第 (小さいテンソルはカーネル起動コストに
負ける) なので、デフォルトの `use_auto` が無難。

### Why 専用エンジンなのか

行列重視のコード (MLP 推論、microgpt) を手書きの O(n³) ループで
出荷したら NumPy より桁違いに遅かった。 チューニング済みカーネルに
通すと、このコードベースが実際に学習する MNIST サイズで PyTorch CPU
の ~1.2× 以内に収まる。 ベンチ詳細は
[`benchmarks/mnist/README.md`](../benchmarks/mnist/README.md) と
[`benchmarks/microgpt/README.md`](../benchmarks/microgpt/README.md)。

dtype の根拠、アロケータ選定、lazy shape の議論は
[`internals.ja.md` §8](internals.ja.md)。

---

第 IV 部 — 検証とデプロイ
=========================

## 17. ツール (`test`, `lint`, `fmt`, デバッグ)

`culebra` バイナリはツールチェーンそのものでもあります。テストランナー・
リンタ・フォーマッタ・デバッグアダプタは同じ実行ファイルのサブコマンドなので、
追加でインストールするものはありません。この章は概観です。フラグ単位の
リファレンスは [`tooling.ja.md`](tooling.ja.md) にあります。

### 17.1 テスト

テストファイルは `test_*.cul` という名前の `.cul` ファイルです。その中では
`test()`・`@test` デコレータ・`@parametrize` が環境に備わっており
（import 不要）、表明にはマッチャ群を使います:

```culebra
# doctest: skip
# tests/test_math.cul
@test
fn adds_correctly() {
  assert_eq(1 + 2, 3)
}

@parametrize([(1, 2, 3), (10, 20, 30)])
fn adds_each(a, b, want) {
  assert_eq(a + b, want)
}
```

```sh
culebra test                        # カレントディレクトリから探索して実行
culebra test --filter "Array/push"  # 名前で絞って実行
```

テストの引数は周囲の環境から名前で解決されるので、スコープにある任意の
関数がフィクスチャになります。クラスインスタンスを返すフィクスチャは
テスト終了時に `drop` が呼ばれます (Ch.7.4)。同じランナーはこのガイドの
例も実行します — `culebra test --doc docs` — 本書のすべての
` ```culebra ` ブロックが期待出力を持っているのはそのためです。

### 17.2 lint とフォーマット

`culebra lint` は、プログラムを実行せずに静的解析で分かることを報告します。
実行すればどのみち中断されるエラーに加えて、助言的な警告（未使用の変数・
import、到達不能コード）も出します。終了コードは clean が 0、警告のみが 1、
エラーが 2 なので CI のゲートに使えます。

```bash
culebra lint .          # カレントディレクトリ配下の .cul を再帰的に
culebra lint --fix .    # 加えて未使用 import 行を削除する
```

`culebra fmt` は設定不要のフォーマッタです（スタイルフラグはありません）。
パース → 再出力 → **再パースして比較**してから書き出すので、意味を変えたり
コメントを落としたりするフォーマットは適用せず拒否します。

```bash
culebra fmt -i .        # プロジェクトをその場で整形
culebra fmt --check .   # 未整形があれば exit 1（CI ゲート）
```

エディタは stdin 形式 (`culebra fmt -`) でフォーマッタを呼びます。同梱の
VSCode・Zed・Vim 統合はすでにこれに繋いであります。

### 17.3 デバッグ

`culebra dap` は Debug Adapter Protocol を stdio で話します。ブレークポイント、
ステップ実行、コールスタック、ウォッチ式、実行中の `mut` 変数の書き換えが、
DAP に対応した任意のエディタで動きます。アダプタを起動するのはエディタ側で、
手で走らせることはまずありません。デバッグはインタプリタで動くので `--jit`
は付けないでください。

ソース中に裸の `debugger` 文を置けば、設定なしでもその場所で必ず停止します。
エディタ別のセットアップ（VSCode・Vim・Zed）は
[`tooling.ja.md` §4](tooling.ja.md#4-デバッグ-culebra-dap) にあります。

## 18. AOT バイナリビルド

`culebra build` は `.cul` ソースを ahead-of-time で自己完結バイナ
リにコンパイルする。 ランタイムに LLVM 不要。 tree-shaking で使われ
ないランタイムヘルパを落とす。 Tensor を使わないプログラムでは
tensor エンジンが必要とする Accelerate / Metal フレームワーク依存も
外せる。

```bash
./build/culebra build my-program.cul -o ./out
./out                                     # standalone、~350 KB on macOS
otool -L ./out                            # Accelerate も Metal も LLVM も無し
```

### 18.1 クロスコンパイル

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

`inspect` だけ使う "hello world" は tensor も HTTP ランタイムも要らない。
エントリファイルから call graph を辿ることで、参照されていないラン
タイムヘルパ (~200 個) を落とせる。 `Tensor` 参照が無ければ tensor 抜きの
archive に差し替わるので、数 MB が数百 KB になる。

## 19. 埋め込み概観

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
`inspect` / `print` はホスト側で用意する設計。 エラーは
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
