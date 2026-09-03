Culebraガイド
================

動的型付けスクリプト言語。束縛は既定で不変、ブロックは値に
評価され、パターンマッチが言語の中核にあります。バイトコード
VMとLLVM ORC JITの2バックエンドが1つのパーサ・AST・バイトコード
コンパイラを共有します。このガイドは "hello" からC++ ホストへの埋め込み
までを案内します。厳密な文法は [`language.ja.md`](language.ja.md)、
APIリファレンスは [`stdlib.ja.md`](stdlib.ja.md) を参照してください。

> **doctest規約。** 本ガイドの各` ```culebra `ブロックは実行可能
> な例です。行末の`# => <value>`は標準出力の期待値、`# !! <pattern>`
> は`throw`の期待値。ブロック先頭の`# doctest: skip`は説明用
> （複数ファイル・ネットワークアクセス・`culebra test`ランナーが
> 必要な場合が多い）。ブロック間は独立スコープです。規約とディレ
> クティブ一覧の全体は [`tooling.ja.md` §1](tooling.ja.md#doctest)。

> **Statusラベル。** ラベル無しの見出しは現時点の実装を記述します。
> 出現するラベル: **Draft** (実装中、API変更あり)、**Planned**
> (採用決定、未実装)、**Deprecated** (将来削除予定)。

目次
----

- **第I部 — 言語コア**
  1. [導入](#1-導入)
  2. [値・束縛・制御フロー](#2-値束縛制御フロー)
  3. [関数とクロージャ](#3-関数とクロージャ)
  4. [文字列](#4-文字列)
  5. [イテレータ](#5-イテレータ)
  6. [パターンマッチ](#6-パターンマッチ)
  7. [エラー処理と RAII](#7-エラー処理と-raii)
  8. [代数的エフェクト](#8-代数的エフェクト)
- **第II部 — 抽象化の道具**
  9. [クラス](#9-クラス)
  10. [UFCS とマルチメソッド](#10-ufcs-とマルチメソッド)
  11. [デコレータ](#11-デコレータ)
  12. [モジュール](#12-モジュール)
- **第III部 — 型とライブラリ**
  13. [型システム](#13-型システム)
  14. [標準ライブラリ](#14-標準ライブラリ)
- **第IV部 — 検証とデプロイ**
  15. [ツール (`test`, `lint`, `fmt`, デバッグ)](#15-ツール-test-lint-fmt-デバッグ)
  16. [AOT バイナリビルド](#16-aot-バイナリビルド)
  17. [埋め込み概観](#17-埋め込み概観)

## 0. 設計哲学

ここを1度読めば、以降の章は前提知識として扱えます。

- **2バックエンド、1コンパイラ。** バイトコードVMとLLVM ORC JITが
  同じパーサ・AST・バイトコードコンパイラを共有。VMはLLVM非依存
  (ドライバ ~23 MB。LLVMを含めると ~82 MB)、JITは`-O2`で同じ
  プログラムを実行。両方を維持 — どちらも捨てません。
- **日常的に使う8つの型。** `Nil` / `Bool` / `Long` / `Float` /
  `String` / `Array` / `Object` / `Function`、加えて用途特化の4つ
  (`StringView` / `Tuple` / `Set` / `Tensor`)。クラス・モジュール・
  エラーなどはすべて`Object`上に構築。
- **既定で不変、`mut`で可変にする。** 束縛はそう書かない限り再代入
  できず、ブロックは最後の式に評価される。だから
  `let x = if c { a } else { b }`が普通のコードになる。クロージャは
  第一級、エラーは値、隠れたグローバル無し。
- **UFCS、パイプライン不採用。** 任意の自由関数`f(x, ...)`を
  `x.f(...)`として呼べる。パイプライン演算子は検討の上不採用。
- **明示的で静的なモジュール。** ファイルは`export { ... }`で束縛
  を公開し、利用側は`import name from './path.cul'`で束縛する。
  どちらもトップレベル専用なので依存グラフはパース時に確定し、
  それがAOTバンドルとtree-shakingを可能にしている (Ch.12)。
- **async/await無し。** I/Oはブロッキング設計、並行はスレッドで。
  HTTP等のネットワークスタックはブロッキング、典型的なスケール
  上限は数千接続。
- **batteries-included、ティア制。** コア
  (Math/IO/FS/File/Sys/Random/String/Time/Args) とTier 1
  (Regex/Http/Hash/Encoding/Compress/JSON/CSV/TOML/SQLite/UUID/
  Peg/Log/Term/Canvas) はどちらも出荷済み。Tier 2/3 (Crypto、Sockets)
  は需要次第 — Ch.14参照。
- **1.0前。** ソース・APIは変わる可能性。tag付きリリースには3プラット
  フォームのビルド済みバイナリが付く (§1.1)。CHANGELOGは無く、各
  リリースのノートが記録。

---

第I部 — 言語コア
==================

## 1. 導入

### 1.1 リリースバイナリのインストール

以下のリンクは常に最新リリースを指す。アーカイブの中身は実行ファイル1本と
ライセンスだけで、インストーラも追加で置くものもない。例外は
サブコマンド1つ: `culebra build`はホストのC++コンパイラでリンクするので、
一度も何もビルドしたことのないマシンにはそれが無い
（[`deployment.ja.md` §1](deployment.ja.md#ホスト側に必要なもの)）。

| プラットフォーム | ダウンロード |
|---|---|
| macOS (Apple Silicon) | [culebra-macos-arm64.tar.gz](https://github.com/yhirose/culebra/releases/latest/download/culebra-macos-arm64.tar.gz) |
| Linux (x86-64) | [culebra-linux-x64.tar.gz](https://github.com/yhirose/culebra/releases/latest/download/culebra-linux-x64.tar.gz) |
| Windows (x86-64) | [culebra-windows-x64.zip](https://github.com/yhirose/culebra/releases/latest/download/culebra-windows-x64.zip) |

コマンドラインで展開すると、macOSがFinder経由の展開物に付ける隔離フラグも
避けられる (バイナリは未署名):

```bash
curl -fsSL https://github.com/yhirose/culebra/releases/latest/download/culebra-macos-arm64.tar.gz | tar xz
sudo mv culebra-*/culebra /usr/local/bin/
culebra --version
```

macOSはパッケージマネージャからも入れられる:

```bash
brew install yhirose/culebra/culebra
```

WindowsとLinux向けのパッケージはまだ無いので、上のダウンロードを使う。

チェックサムと各リリースのノートは
[releasesページ](https://github.com/yhirose/culebra/releases)にある。

バイナリを入手したら、`culebra init`でエディタとこのプロジェクトで動く
コーディングエージェントの設定を整える。このマシンにあるVSCode・Vim・
Neovimのうち見つかったものにシンタックスハイライトとデバッグアダプタを
導入し、`AGENTS.md`（既に`CLAUDE.md`か`.github/copilot-instructions.md`
があればそちら）にコーディングエージェント向けの指示を追加する。
プロジェクトディレクトリで実行し、何度でも再実行して構わない:

```bash
culebra init
```

### 1.2 ソースからビルド

masterを追う場合とCulebra自体を開発する場合にだけ必要。`just build`は
バイトコードVMを、LLVM 20+があればJITも生成する:

```bash
just build              # JIT付き
just build-no-jit       # LLVM 無し: bytecode VMのみ、~23 MB
just dev                # LTO無し -O1の高速ビルド → build-dev/ (内側ループ用)
just test-dev           # build-dev/ でVM==JITを素早く確認 (各編集ごと)
just test               # 全backend + embedスモークテスト (並列; JOBS=1で逐次化)
just test wrap          # `culebra wrap`の端から端まで (`just test`には含まれない)
just test-assert        # 同じスイープをNDEBUGなしでビルドしassertを実行させる
just install            # Releaseバイナリを /usr/local/binへ (`just install ~/.local`でユーザーinstall)
```

`just install`を実行するまでバイナリは`./build/culebra` (または
`./build-dev/culebra`)にあるので、以降のコマンドは`culebra`ではなく
そのパスで呼ぶ。

### 1.3 Hello, Culebra

Culebraソースの拡張子は`.cul`。`culebra`バイナリで実行:

```bash
echo "inspect('hello, culebra!')" > hello.cul
culebra hello.cul                    # バイトコードVM (既定)
culebra --jit hello.cul              # JIT (出力は同じ)
culebra --jit-faststart hello.cul    # JIT・起動が速い
culebra --shell                      # REPL (常にVM)
culebra --help                       # 全オプション・コマンド一覧
```

3バックエンドとも観測可能な出力は同一(VM↔JITの差分コーパス全件で
検証済み)。`--jit-faststart`はIRと機械語の両方の最適化を省き、
**JIT warmup(起動・コード生成時間)を3〜5分の1**にする — 330行のプログラムで
1.6秒が0.3秒。`--jit`実行がコンパイルするのはプログラム自身のコードなので
(stdlib preambleはビルド時にバイナリへコンパイル済み)、この差はプログラムが
大きいほど開く。`-O0`を含意し、別の`-O`を併記するとエラー。

steady-stateの代償は、ホットな処理のうち最適化器が手を出せる割合で決まる:
C++/BLASランタイム側(例: `Tensor`)にあるなら~0%、呼び出しの多いスクリプトコードで
30%程度、スカラー算術のタイトループでは数倍。つまり**大きな**プログラムで
ホットな処理がランタイム側にある場合に効く。短命スクリプトならVMのほうが
どのJITレーンより速く起動し、スクリプトレベルの算術なら既定の`--jit`(`-O2`)が
そのまま最速。

コメントは`#` / `//` (行) または`/* ... */` (ブロック)。文は`;`で
区切る (省略時は改行)。行末`;`は通常は省略。

```culebra
# this is a comment
inspect('hello')  # => 'hello'
```

`inspect`は値をクォート付きのinspect形式で出力するため、文字列は引用符付き
(`'hello'`) で、参照型はそのリテラル表記で表示される。引用符の付かない
生のテキストに改行を付けたいときは`println`、改行も不要なら`print`を使う
— 第14章参照。

## 2. 値・束縛・制御フロー

### 2.1 日常的に使う 8 つの型

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
}))  # => 'Function'
```

残り4つは必要になったときに出てきます: `StringView` (Ch.4.4)、
`Tuple`と`Set` (Ch.13.2)、`Tensor` (Ch.14.2)。全体の表は
[language.ja.md §4](language.ja.md)。

### 2.2 束縛: bare / `let` / `mut`

```culebra
x = 10      # bare: 新規不変束縛、または外側を再代入
let y = 20  # let: 新規不変束縛 (外側のシャドウは不可)
mut z = 30  # mut: 新規可変束縛
z = 31      # mutは再代入可能
z += 1      # 複合 (`-= *= /= %= **= @=`も同様)
inspect(z)  # => 32
```

bare代入は外側スコープへ向かって検索し、最も近い同名束縛を再代入。
見つからなければ現スコープに新規束縛。クロージャベースのオブジェクト
パターン (Ch.9) はこの挙動で成立しています。

> **注意:** 束縛はデフォルトで不変なので、裸の`x = 1`の後に同一
> スコープで`x = 2`と書くとエラーになります (`immutable variable
> 'x'`)。再代入したい変数には`mut`を付けます (`mut x = 1; x = 2`)。
> `let`は任意で、(元から不変の) 意図を明示し、かつ外側束縛のシャドウ
> を禁止するマーカーです。

### 2.3 シャドウ禁止

外側関数で捕捉された変数を`let` / `mut` / パラメータ / `match`
パターンで**シャドウすると**コンパイルエラーになります。同関数内
のblock-localなrebindingは許容。

```culebra
make = fn () {
  mut n = 0
  fn () {
    # let n = 1   # エラー: 捕捉された`n`をシャドウしてしまう
    n += 1  # bare代入で捕捉`n`を更新 — OK
    n
  }
}
c = make()
inspect(c())  # => 1
inspect(c())  # => 2
```

### 2.4 制御フロー

`if`と`match` (Ch.6) は式 — 選ばれた枝の値を返す。`while`と
`for`は文 (値は`nil`)。

```culebra
x = 7
sign = if x > 0 {
  1
} else if x < 0 {
  -1
} else {
  0
}
inspect(sign)  # => 1

size = match x {
  # matchも式 (Ch.6)
  0 => 'zero',
  n if n < 10 => 'small',
  _ => 'large',
}
inspect(size)  # => 'small'

mut i = 0
while i < 3 {
  inspect(i)
  i += 1
}
# => |
# 0
# 1
# 2

for n in 0..3 {
  inspect(n)
}  # 排他レンジ
# => |
# 0
# 1
# 2

for n in 0..=2 {
  inspect(n)
}  # 包含レンジ
# => |
# 0
# 1
# 2

for n in 0..10 by 3 {
  inspect(n)
}  # ステップ付きレンジ
# => |
# 0
# 3
# 6
# 9

for k, v in {a: 1, b: 2} {
  inspect("{k}={v}")
}  # Objectはkey, valueを返す
# => |
# 'a=1'
# 'b=2'
```

`break`はループを抜け、`continue`は次の反復へ飛ぶ。どちらも
`while` / `for`内で動作。

```culebra
for n in 0..10 {
  if n % 2 == 1 {
    continue
  }  # 奇数は飛ばす
  if n > 4 {
    break
  }  # 4を超えたら止める
  inspect(n)
}
# => |
# 0
# 2
# 4
```

### 2.5 `nobreak` / init 節 / `cond` / `? :`

ループには`nobreak`ブロックを付けられます。`break`せずに完走した
ときだけ走ります。キーワードが検査する条件そのものを名乗っているので、
いつ走るのかを説明するコメントは要りません:

```culebra
mut found = nil
for n in [1, 3, 5] {
  if n % 2 == 0 {
    found = n
    break
  }
} nobreak {
  inspect('偶数なし')  # => '偶数なし'
}
```

`while` / `if` / `match`は **init節** — `;`で区切る、その構文
だけにスコープする束縛 — を取れるので、ループ変数が外側スコープに
漏れません:

```culebra
while mut i = 0; i < 3 {
  i += 1
}
if let n = 6; n > 5 {
  inspect('大きい')
}  # => '大きい'
```

多分岐には`cond` (主語のない`match`)、2分岐には三項`? :`:

```culebra
grade = fn (n) {
  cond {
    n >= 90 => 'A',
    n >= 80 => 'B',
    _ => 'C',
  }
}
inspect(grade(85))                       # => 'B'
inspect(grade(50) == 'C' ? 'ok' : 'no')  # => 'ok'
```

クロージャベースのオブジェクトパターン (Ch.9) では、捕捉された状態が
そのままオブジェクトの状態なので、silent shadowはオブジェクトを壊す。
一方、関数内blockでのrebindingは日常パターンなので許容する。詳細
なルールと設計根拠は [language.ja.md §6](language.ja.md)。

## 3. 関数とクロージャ

### 3.1 `fn` と `|x|`

```culebra
add = fn (a, b) {
  a + b
}
inspect(add(2, 3))  # => 5

# 型注釈はオプション; 詳細は Ch.13
add_typed = fn (a: Long, b: Long) -> Long {
  a + b
}
inspect(add_typed(2, 3))  # => 5

# ラムダは式ボディまたは波括弧のブロックボディを取れる
square = |x| x * x
inspect(square(7))  # => 49

scale = |x, y| {
  let factor = 10
  x * y * factor
}
inspect(scale(2, 3))  # => 60

# 再帰には `fn` (関数自身への参照)
fib = fn (x) {
  if x < 2 {
    x
  } else {
    fn(x - 2) + fn(x - 1)
  }
}
inspect(fib(10))  # => 55
```

### 3.2 クロージャ

内側関数は外側束縛を参照で捕捉する。`mut`を付けると書き換え可能。

```culebra
make_counter = fn () {
  mut n = 0
  fn () {
    n += 1
    n
  }  # bare代入で捕捉`n`を更新
}
c = make_counter()
inspect(c())  # => 1
inspect(c())  # => 2
inspect(c())  # => 3
```

### 3.3 キーワード引数と `**splat`

パラメータにはデフォルト値を宣言可能。最後のpositionalの後の
`*`は以降をkeyword-onlyにする。`**rest`は未指定keywordを
Objectにまとめる。

```culebra
greet = fn (name, *, greeting = 'hi', **opts) {
  prefix = if opts.has('formal') && opts.formal {
    'Mr./Ms. '
  } else {
    ''
  }
  "{greeting}, {prefix}{name}"
}
inspect(greet('alice'))                     # => 'hi, alice'
inspect(greet('alice', greeting: 'hello'))  # => 'hello, alice'
inspect(greet('bob', formal: true))         # => 'hi, Mr./Ms. bob'

# `**` で Object をキーワード引数として splat
opts = {greeting: 'yo', formal: false}
inspect(greet('carol', **opts))  # => 'yo, carol'
```

`*`マーカーは呼び出し側にオプション名を書かせるので、長いパラメー
タリストが読みやすくなり、再配置・拡張もコール側を壊さない。末尾の
`*rest`パラメータはその位置引数版で、余った位置引数をArrayに
集めます:

```culebra
sum_all = fn (first, *rest) {
  mut t = first
  for v in rest {
    t += v
  }
  t
}
inspect(sum_all(1, 2, 3, 4))  # => 10
```

パラメータ・デフォルト値・splatの完全な仕様は
[language.ja.md §11](language.ja.md)。

## 4. 文字列

### 4.1 補間と連結

```culebra
name = 'Culebra'
inspect("hello, {name}!")             # => 'hello, Culebra!'
inspect("two plus three is {2 + 3}")  # => 'two plus three is 5'
inspect('a' + 'b' + 'c')              # => 'abc'
```

### 4.2 反復とスライス

文字列はUnicodeスカラ単位で反復 (1コードポイント = 1ステップ)。
`s[i]`という添字は**ありません** — Stringは添字可能なコンテナでは
なく、`'café'[0]`は`TypeError`です。部分文字列は`slice`で取り、
そのオフセットはUTF-8上のバイトオフセットです (§4.4)。

```culebra
for c in 'café' {
  inspect(c)
}
# => |
# 'c'
# 'a'
# 'f'
# 'é'

inspect('café'.size())  # => 5
```

`size()`はUTF-8表現上のバイト数を返す (`é`は2バイトなので`'café'`
は5)。一方、上の`for`ループはUnicodeスカラ単位で1ステップずつ進む
(4ステップ)。

### 4.3 よく使うメソッド

```culebra
inspect('hello world'.split(' '))   # => ['hello', 'world']
inspect('  hi  '.trim())            # => 'hi'
inspect('abc'.upper())              # => 'ABC'
inspect('foo'.starts_with('fo'))    # => true
inspect(['a', 'b', 'c'].join('-'))  # => 'a-b-c'
```

完全な一覧は [language.ja.md §18.1](language.ja.md)。

### 4.4 `StringView`、`StringLike`、graphemes() lazy

`.slice()` / `.split()` / `.view()`は`StringView`を返す — 元の
文字列のバイト列へのzero-copyな借用で、元の束縛がスコープを抜けて
も共有オーナーが生存させ続ける。`StringLike`型のパラメータは
`String`と`StringView`の両方を受け付けるので、読むだけのヘルパー
がcopyを強制しない。

```culebra
print_first_grapheme = fn (s: StringLike) {
  for g in s.graphemes() {
    inspect(g)
    break
  }
}
print_first_grapheme('café')  # => 'c'

inspect(type_of('hello'.slice(1, 4)))  # => 'StringView'
inspect('hello'.slice(1, 4))           # => 'ell'
```

`.graphemes()`はUnicodeの *extended grapheme cluster* をlazyに
走査する — ZWJで連結された複数コードポイントの絵文字ファミリーで
あっても、1ステップ = ユーザが知覚する1文字になる:

```culebra
inspect('a👨‍👩‍👧b'.graphemes().collect().size())  # => 3
inspect('café'.graphemes().collect().size())                 # => 4
```

`StringView`/graphemeの完全なAPIは [language.ja.md §18.1](language.ja.md)。

### なぜバイトインデックスなのか

bytesとscalarの区別を不透明な文字インデックスで隠す設計は、その
文字列がソケットやファイルに出会うまではよく読める。そこでは長さも
オフセットもバイト数なので、境界のたびに2つのモデルを突き合わせる
ことになる。Culebraはバイトオフセットをそのまま見せ、他のビューを
その隣に置く: スカラー単位の処理には明示的なコードポイント反復、
表示には上記のlazy grapheme反復。

## 5. イテレータ

### 5.1 `range` (lazy) と `iota` (eager)

```culebra
# range は何も構築しない; for ループが lazy に消費
for i in range(3) {
  inspect(i)
}
# => |
# 0
# 1
# 2

# iota は Array を割り当てる
inspect(iota(3))     # => [0, 1, 2]
inspect(iota(2, 5))  # => [2, 3, 4]
```

### 5.2 遅延チェイン

`.iter()`でArrayをlazy iteratorに。チェインはconsumer
(`collect` / `reduce` / `find`等) で止まり、中間Arrayは作らない。

```culebra
result = range(1000).filter(|x| x % 2 == 0).map(|x| x * 3).take(5).collect()
inspect(result)  # => [0, 6, 12, 18, 24]

total = range(1, 11).reduce(0, |a, x| a + x)
inspect(total)  # => 55

inspect([1, 2, 3, 4].iter().any(|x| x > 3))    # => true
inspect([10, 20, 30].iter().find(|x| x > 15))  # => 20
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
inspect(flat)  # => [1, 2, 3, 4, 5, 6]

head = range(100).skip(10).take_while(|x| x < 15).collect()
inspect(head)  # => [10, 11, 12, 13, 14]

# chunks: 固定長のグループ (最後だけ短くなりうる)
inspect([1, 2, 3, 4, 5].iter().chunks(2).collect())
# => [[1, 2], [3, 4], [5]]

# windows: 1 要素ずつずらすスライディングビュー
inspect([1, 2, 3, 4].iter().windows(2).collect())
# => [[1, 2], [2, 3], [3, 4]]
```

### 5.4 ユーザー定義イテレータ

3メソッドを実装: `iter()` (慣習でイテレータ自身を返す)、
`has_next()` (`Bool`を返す)、`next()` (次の要素を返す)。lazyチェ
インの早期終了保証を含む完全なプロトコルは
[language.ja.md §18.5](language.ja.md)。

```culebra
countdown = fn (start) {
  mut i = start
  {
    iter: fn () {
      self
    },
    has_next: fn () {
      i > 0
    },
    next: fn () {
      v = i
      i -= 1
      v
    },
  }
}

for v in countdown(3) {
  inspect(v)
}
# => |
# 3
# 2
# 1
```

### 5.5 ジェネレータ (`yield`)

`yield`を含む`fn`本体はジェネレータになる — 呼び出しても本体は
実行されず、イテレータ (5.4の`iter`/`has_next`/`next`プロトコル)
が返るので、`for`やlazyチェインのメソッドがそのまま使える。
`yield from`は他のiterableへ委譲する。

```culebra
fn countdown(start) {
  mut i = start
  while i > 0 {
    yield i
    i -= 1
  }
}
for v in countdown(3) {
  inspect(v)
}
# => |
# 3
# 2
# 1

fn chunk(arr, n) {
  mut buf = []
  for v in arr {
    buf.push(v)
    if buf.size() >= n {
      yield buf
      buf = []
    }
  }
  if !buf.empty() {
    yield buf
  }
}
inspect(chunk([1, 2, 3, 4, 5], 2).collect())  # => [[1, 2], [3, 4], [5]]
```

## 6. パターンマッチ

### 6.1 基本

```culebra
describe = fn (x) {
  match x {
    0 => 'zero',
    1 | 2 | 3 => 'small',
    n: Long if n > 100 => "big ({n})",
    n: Long => "int ({n})",
    s: String => "str ({s})",
    true => 'TRUE',
    false => 'FALSE',
    nil => 'NIL',
    _ => 'other',
  }
}
inspect(describe(0))     # => 'zero'
inspect(describe(2))     # => 'small'
inspect(describe(999))   # => 'big (999)'
inspect(describe('hi'))  # => 'str (hi)'
inspect(describe([1]))   # => 'other'
```

### 6.2 式として

`match`は値を生む — 計算式の中で使える。

```culebra
classify = fn (n: Long) -> Long {
  match n {
    n if n < 0 => -1,
    0 => 0,
    _ => 1,
  }
}
inspect(classify(-5))  # => -1
inspect(classify(0))   # => 0
inspect(classify(7))   # => 1
```

### 6.3 分解

```culebra
shape = fn (a) {
  match a {
    [] => 'empty',
    [x] => "one ({x})",
    [x, y] => "two ({x},{y})",
    [head, ...tail] => "head={head}, rest={tail.size()}",
  }
}
inspect(shape([]))            # => 'empty'
inspect(shape([10, 20]))      # => 'two (10,20)'
inspect(shape([1, 2, 3, 4]))  # => 'head=1, rest=3'

first_name = fn (people) {
  match people {
    [{name}, ..._] => name,
    _ => 'none',
  }
}
inspect(first_name([{name: 'x'}, {name: 'y'}]))  # => 'x'
inspect(first_name([]))                          # => 'none'
```

### 6.4 再帰

```culebra
is_even = fn (n) {
  match n {
    0 => true,
    1 => false,
    _ => fn(n - 2),
  }
}
inspect(is_even(10))  # => true
inspect(is_even(7))   # => false
```

### Why exhaustiveness check 無し

静的型システム無しでObjectのshapeを網羅性検査するには、節約
以上のランタイムコストがかかる。`_`節 (またはガード付き最終
パターン) で意図を明示する方針。詳細とUnion型の例外は
[language.ja.md §13](language.ja.md)。

## 7. エラー処理と RAII

### 7.1 `throw` / `try` / `catch`

throwされる値は任意のCulebra値 — String、Object、何でも可。

```culebra
validate = fn (x) {
  if x < 0 {
    throw "negative: {x}"
  }
  x
}

try {
  inspect(validate(42))  # => 42
  inspect(validate(-1))  # throws、次の行は到達せず
  inspect('unreached')
} catch e {
  inspect("caught: {e}")  # => 'caught: negative: -1'
}
```

### 7.2 `try` を式として

```culebra
validate = fn (x) {
  if x < 0 {
    throw "negative: {x}"
  }
  x
}
safe = fn (x) {
  try {
    validate(x)
  } catch _ {
    0
  }
}
inspect(safe(7))    # => 7
inspect(safe(-99))  # => 0
```

### 7.3 `defer`

`defer { ... }`は囲むブロックの**全exitパス** (通常終了 /
`return` / `throw`) でLIFOに実行されるクリーンアップ登録。ブロッ
ク直下・関数本体直下・トップレベル直下のいずれでも、どのバックエンド
でも同じように発火する。関数の残りが動く前に後始末したいときだけ、
内側の`{ }`に入れる。exitパスと順序の完全なルールは
[language.ja.md §15](language.ja.md)。

```culebra
demo = fn (fail) {
  {
    defer {
      inspect('cleanup A')
    }
    defer {
      inspect('cleanup B')
    }
    if fail {
      throw 'failed'
    }
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

オブジェクトがno-arg Function型の`drop`プロパティを持つと、
最後の参照が消えた時点でランタイムが自動で呼ぶ。`drop`はファク
トリ関数で組み立て、ブロックスコープに束縛するのが定石 — スコープ
離脱で確実にrefcountが0になる。

```culebra
make_resource = fn (id) {
  {drop: fn () {
    inspect("R{id} released")
  }}
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

`drop`はカスケードする — 外側オブジェクトが解放されると、内側に
持つ参照 (drop付き) が連鎖して解放される。完全なメモリモデル
(RC + サイクル収集) は [language.ja.md §17](language.ja.md)。

### 7.5 Scope guard パターン

自前で`defer`を置けないコード (例: 呼び出し元のスコープでの
クリーンアップを望むコールバック) からクリーンアップを登録したい
場合は、クリーンアップ用クロージャのリストを持つ小さなヘルパー
オブジェクトを用意し、呼び出し側の1つの`defer`からLIFOで実行
すればよい。完全な実装例は [language.ja.md §15](language.ja.md)。

### Why throw 値は任意

`throw "msg"`で十分なケースがほとんど (スクリプト)。ライブラリ
ではクラス化したエラー (Ch.9) で十分。階層は最初から要らない。
catch節はthrowerが使った形をパターンマッチで受ければよい (Ch.6)。

---

## 8. 代数的エフェクト

*エフェクト*は、意味を呼び出し側が決める操作をコードから呼べるようにします。
操作を`perform`し、コールスタック上位の`handle`ブロックが、それが何をするか、
そして`perform`したコードを *再開 (resume)* するかを選びます。ジェネレータ・
例外・依存性注入・バックトラック探索を1つの機構でカバーします。

### 8.1 `perform` と `handle`

操作は`effect fn`（本体なし）で宣言し、`perform`し、`handle`ブロックに`with`
clauseを与えます。clauseは操作の引数と`resume`継続を受け取ります:

```culebra
effect fn ask()

let answer = handle {
  let n = perform ask()
  n * 2
} with ask(resume) {
  resume(21)
}
inspect(answer)  # => 42
```

`resume(21)`はハンドルされた本体を`perform ask()`の地点から値`21`で再開する
ので、`n`は`21`となり本体は`42`を返します。

### 8.2 ハンドラは状態を紡ぐ

ハンドラは`perform`のたびに走るので、自身が持つ状態に対して操作を解釈できます
— ここでは`get`が読み`put`が書くセルです:

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
inspect(n)  # => 2
```

どの関数からでも`perform`できます。plain関数の`perform`は現在のコール
スタックに設置されたハンドラへディスパッチされ、`effect fn`マーカーが必要なのは
ハンドラが複数回resumeする、または非末尾でresumeする場合（継続のキャプチャが
要る場合）だけです。1つの`handle`は操作ごとに`with` clauseを持てます。

### 8.3 複数回の再開

継続はmulti-shotです — ハンドラは`resume`を何回でも呼べ、各呼び出しは
perform側の残りを独立に再実行します。両方の再開を返せば両方の選択肢を探索します:

```culebra
effect fn choose(a, b)

let both = handle {
  let x = perform choose(1, 2)
  x * 10
} with choose(a, b, k) {
  [k(a), k(b)]
}
inspect(both)  # => [10, 20]
```

### 8.4 再開しないハンドラ

`resume`を呼ばないclauseは残りの計算を破棄します — まさに例外です。ハンドラの
ない`perform`は`EffectError`を送出するので、エフェクトは回復可能で型付きの
失敗としても使えます:

```culebra
effect fn raise(msg)

effect fn safeDiv(a, b) {
  if b == 0 { perform raise("div by zero") }
  a / b
}

inspect(handle { safeDiv(10, 2) } with raise(m, k) { -1 })  # => 5
inspect(handle { safeDiv(10, 0) } with raise(m, k) { -1 })  # => -1
```

正常完了値を写すには`with return(v) { … }`を加えます。これが走るのは*正常*
完了のときだけで、中断するclauseは通りません:

```culebra
effect fn ask()

let out = handle {
  let n = perform ask()
  n + 1
} with ask(k) { k(41) }
  with return(v) { "final={v}" }
inspect(out)  # => 'final=42'
```

エフェクトフルな本体の中に`handle`を書けば、外側の計算をキャプチャしてそこ
から再開できます。完全なリファレンスと制約は
[language.ja.md §16](language.ja.md) を参照。

効果が一番はっきり出るのは探索です。列を`perform choose(...)`で尋ね、
行き止まりを`perform reject()`で告げるN-クイーン探索は、バックトラッキング
を一言も書きません。その意味は外側の`handle`が与えるので、同じ本体が全解の
列挙にも、最初の解での短絡にも、配置数のカウントにもなります。

---

第II部 — 抽象化の道具
=======================

## 9. クラス

### 9.1 構文

`class`はコンストラクタ (`new`) とメソッドを宣言する。`self.x =
...`で設定したフィールドはデフォルトで可変。インスタンスは可読な
`class:`タグを持つ。

```culebra
class Car {
  new(mpr) {
    self.miles = 0
    self.mpr = mpr
  }
  run(n) {
    self.miles += self.mpr * n
  }
  total() {
    "走行距離: {self.miles} miles"
  }
}

car = Car.new(5)
car.run(1)
car.run(2)
inspect(car.total())  # => '走行距離: 15 miles'
inspect(car.class)    # => 'Car'
```

クラスそのものを呼び出すのは`.new`のショートハンドです。`Car(5)`は
`Car.new(5)`とまったく同じで、キーワード引数もそのまま渡せます。読みやすい
方を使ってください。クラスはコンストラクタと同じようにcallableです。

```culebra
class Point {
  new(x, y) {
    self.x = x
    self.y = y
  }
}
p = Point(3, 4)         # Point.new(3, 4) と同じ
inspect("{p.x},{p.y}")  # => '3,4'
```

フィールドはclass本体でデフォルト値つきに**宣言**することもできる。
各インスタンスが自分のコピーを持ち、`new`の実行前に実体化されるので、
コンストラクタが触らない経路でもフィールドは既知の値で存在する。`get`
メソッドは計算プロパティで、括弧なしで呼ぶ:

```culebra
class Temp {
  celsius = 0.0
  scale = 'C'
  new(c) {
    self.celsius = c
  }
  get fahrenheit() {
    self.celsius * 9.0 / 5.0 + 32.0
  }
}

t = Temp.new(100.0)
inspect(t.fahrenheit)  # => 212.0
inspect(t.scale)       # => 'C'
```

### 9.2 クロージャベースの別解

`class`は糖衣 — 同じカプセル化はファクトリがObjectリテラルを
返す形でも書ける。状態は捕捉ローカル (真にプライベート)。両方
とも第一級。

```culebra
Car2 = {new: fn (mpr) {
  mut miles = 0
  {
    run: fn (n) {
      miles += mpr * n
    },
    total: fn () {
      "走行距離: {miles} miles"
    },
  }
}}

car = Car2.new(5)
car.run(1)
car.run(2)
inspect(car.total())  # => '走行距離: 15 miles'
```

`class:`タグとshapeマッチが欲しいなら`class`、private状態の
方が重要ならクロージャ形式を選ぶ。

### 9.3 Static method とフィールド

メソッドやフィールドに`static`を付けるとクラス自身に載る (インス
タンス不要) — ファクトリやクラスレベルの定数を置く自然な場所。

```culebra
class Circle {
  new(r) {
    self.r = r
  }
  static PI = 3.14
  static unit() {
    Circle.new(1)
  }
  area() {
    self.r * self.r * Circle.PI
  }
}
inspect(Circle.unit().area())  # => 3.14
inspect(Circle.PI)             # => 3.14
```

staticフィールドはクラス宣言時に一度だけeagerに評価される。

### 9.4 演算子オーバーロード

算術・比較・インデックス・callの各演算子はdunderメソッド
(`__add__` / `__eq__` / `__lt__` / `__index__` / `__call__`等) に
対応し、クラスがそれを定義すればその演算に参加できる。逆側メソッド
(`__radd__`等) はサポートしない — オーバーロードはその演算を所有
する型に置く。完全なメソッド表とディスパッチ規則は
[language.ja.md §10](language.ja.md) (演算子オーバーロード)。

#### 例: 2 次元ベクトル

```culebra
class Vec2 {
  new(x, y) {
    self.x = x
    self.y = y
  }
  __add__(o) {
    Vec2.new(self.x + o.x, self.y + o.y)
  }
  __sub__(o) {
    Vec2.new(self.x - o.x, self.y - o.y)
  }
  __mul__(k) {
    Vec2.new(self.x * k, self.y * k)
  }
  __neg__() {
    Vec2.new(-self.x, -self.y)
  }
  __eq__(o) {
    self.x == o.x && self.y == o.y
  }
  show() {
    "({self.x}, {self.y})"
  }
}

a = Vec2.new(1, 2)
b = Vec2.new(3, 4)
inspect((a + b).show())       # => '(4, 6)'
inspect((b - a).show())       # => '(2, 2)'
inspect((a * 3).show())       # => '(3, 6)'
inspect((-a).show())          # => '(-1, -2)'
inspect(a == Vec2.new(1, 2))  # => true
```

#### 添字アクセス

`__index__(key)`と`__setindex__(key, value)`を定義すると`obj[k]` /
`obj[k] = v`がクラス側へ委譲され、ラッパ型が組み込みと同じ書き味で
添字アクセスできる。発火するのは、そのキーを直接のプロパティとして
持たない場合だけ。

```culebra
class Grid {
  new() {
    self.d = [10, 20, 30]
  }
  __index__(i) {
    self.d[i]
  }
  __setindex__(i, v) {
    self.d[i] = v
  }
}

g = Grid.new()
g[1] = 99
inspect(g[0])  # => 10
inspect(g[1])  # => 99
```

### 9.5 `__call__` で callable インスタンス

クラスに`__call__`を定義すると、そのインスタンスを直接呼び出せる。

```culebra
class Adder {
  new(n) {
    self.n = n
  }
  __call__(x) {
    x + self.n
  }
}

add5 = Adder.new(5)
inspect(add5(10))  # => 15
inspect(add5(99))  # => 104
```

### 9.6 `drop` によるRAIIクリーンアップ

`drop`は普通のメソッドと同じ形で書けるwell-knownメソッド — 無引数
`drop()`を定義したクラスは、インスタンスへの最後の参照が消えた時点で
それが自動的に呼ばれる。Objectリテラルの`drop` (§7.4) と同じ
auto-drop機構を通る — `class`本体はその`drop`プロパティになる
クロージャの置き場所に過ぎない。

```culebra
class Resource {
  new(id) {
    self.id = id
  }
  drop() {
    inspect("R{self.id} released")
  }
}

inspect('enter')
{
  r = Resource.new('X')
}
inspect('exit')
# => |
# 'enter'
# 'RX released'
# 'exit'
```

### Why `class` とクロージャ両方サポートか

クロージャasオブジェクトが先に存在し、使い捨てカプセル化 (使い切り
イテレータ、scope guard等) では今も正解。`class`形式は、オブ
ジェクトが遠くまで運ばれてアイデンティティが必要 (`class:`タグ、
`match`やデバッグ出力で使う) になる時に意味を持つ。

## 10. UFCS とマルチメソッド

### 10.1 UFCS 解決順

`x.name(args)`では、既存のプロパティ/メソッド`name`が常に優先
され、無ければスコープ内の自由関数`name`が`name(x, args)`として
呼ばれる。完全な解決順序 (`DOT` + 呼び出しリストの要件を含む) は
[language.ja.md §10](language.ja.md) (Methods and UFCS)。

```culebra
double = fn (x) {
  x * 2
}
inspect(42.double())                      # => 84
inspect('hello world'.split(' ').size())  # => 2

# 既存メソッドが常に優先 — Array の組み込み `reverse` は
# ユーザの `reverse` で上書きされない
reverse = fn (x) {
  inspect('user reverse NOT called')
}
mut a = [1, 2, 3]
a.reverse()
inspect(a)  # => [3, 2, 1]
```

### 10.2 マルチメソッド (自由関数の多重ディスパッチ)

同名で別型の関数を複数定義。ランタイムが引数の宣言型に対する最具
体マッチを選ぶ。

```culebra
class Circle {
  new(r) {
    self.r = r
  }
}
class Square {
  new(s) {
    self.s = s
  }
}

fn area(c: Circle) {
  3.14159 * c.r * c.r
}
fn area(s: Square) {
  s.s * s.s
}
fn area(n: Long) {
  n
}  # 数値のフォールバック

inspect(area(Circle.new(2)))  # => 12.56636
inspect(area(Square.new(3)))  # => 9
inspect(area(10))             # => 10
```

ディスパッチはpositional / kwargs / `**splat`全部カバー、Union
で注釈したパラメータ (`x: Long | String`、Ch.13.2) もここに参加する。
完全なディスパッチ/優先度規則は [language.ja.md §20](language.ja.md)。

インスタンスメソッドも同じ方式でディスパッチする — クラスは同名で
パラメータ型の異なるメソッドを複数宣言できる:

```culebra
class Calc {
  new() {}
  go(x: Long) {
    "long"
  }
  go(x: String) {
    "string"
  }
}
c = Calc.new()
inspect(c.go(1))    # => 'long'
inspect(c.go('a'))  # => 'string'
```

### 10.3 ディスパッチ拡張

> **Status: Planned.** hotなディスパッチ経路向けのcall-site単位
> inline cacheがロードマップ上 — 現状は毎回オーバーロード集合を
> 再解決する。classベース (nominal) の継承は検討の上不採用 — 型
> ファミリー全体にわたる多態はtraitディスパッチ (Ch.13.3) 側で担
> う。UFCSと無理なく合成でき、サブタイピングの物語を増やさずに済む
> ため。

### Why 自由関数から先か

自由関数のマルチメソッドはUFCSやimportされたnamespaceと無
理なく合成できる (暗黙のサブタイピングが入らない)。メソッドマル
チメソッドはown-class vs UFCS vs freeの優先順序を決める必要が
あり、推測より実ワークロードで決めたい。

## 11. デコレータ

### 11.1 `@deco`

`fn` (または`class`) の前に置く`@deco`は、`deco(original)`の
結果を元の名前に束縛する。マルチメソッドとの相互作用を含む完全な
仕様は [language.ja.md §21](language.ja.md)。

```culebra
log = fn (f) {
  fn (x) {
    inspect("calling with {x}")
    f(x)
  }
}

@log
fn double(x) {
  x * 2
}

inspect(double(7))
# => |
# 'calling with 7'
# 14
```

### 11.2 ファクトリとスタック

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
fn greet() {
  inspect('hi')
}

greet()
# => |
# '[A]'
# '[B]'
# 'hi'
```

外側デコレータが内側の結果をラップする。上から下に読むと実行順と
一致。

### 11.3 Memoize の実例

```culebra
memoize = fn (f) {
  mut cache = {}
  fn (x) {
    k = to_string(x)
    if !cache.has(k) {
      cache[k] = f(x)
    }
    cache[k]
  }
}

@memoize
fn slow_square(x) {
  x * x
}

inspect(slow_square(7))  # => 49
inspect(slow_square(7))  # => 49
```

### 11.4 `fn.params` introspection

`Function`値は宣言時のシグネチャを読み出せる。`@autograd` / `@trace`
のようなsignatureを知る必要のあるデコレータはこれを使って書ける。

```culebra
add_typed = fn (a: Long, b: Long) -> Long {
  a + b
}
inspect(add_typed.params.map(|p| p.name))  # => ['a', 'b']
inspect(add_typed.return_type)             # => 'Long'
```

デコレートされた関数は単一値 (ラップされたクロージャ) として束縛
される — これは「同名の`fn`が複数共存する」というマルチメソッド
の形と相容れない。名前ごとにどちらか一方を選ぶ (完全な規則は
[language.ja.md §21](language.ja.md))。

## 12. モジュール

### 12.1 `export` と `import`

モジュールは公開するものを`export`で列挙し、利用側は`import`で
モジュール全体を1つの名前に束縛する。

```culebra
# doctest: skip
# lib.cul
let greet = fn (name) {
  "hello, {name}"
}
let PI = 3.14159
let helper = fn () {
  'internal'
}  # exportしない

export {greet, PI}
```

```culebra
# doctest: skip
# main.cul — lib.cul と同じディレクトリ
import lib from './lib.cul'

inspect(lib.greet('world'))  # => 'hello, world'
inspect(lib.PI)              # => 3.14159
inspect(lib.helper)          # => nil — export Objectに載っていない
```

パスはシングルクォートのリテラルで、importする側のファイルの
ディレクトリを基準に解決される。1ファイル内の複数の`export`は
マージされるので、ヘルパを宣言 → export → さらに宣言、と書ける。

### 12.2 トップレベル限定・1 度だけ評価

`import`と`export`はトップレベル文としてのみ書ける — 関数内や
`if`の枝に書くと`SyntaxError`。これによりローダはパース時に依存
グラフ全体を確定でき、AOTバンドラとtree-shakerがそれに依存して
いる (Ch.16)。

各モジュールはプログラム中で1度だけ、依存順に、それぞれ独自の
スコープで評価される。トップレベル束縛はexport Object以外は非公開。
循環import (AがBを、BがAを) は循環を示す`ImportError`で
拒絶される。完全な解決規則・キャッシュ・エラーは
[language.ja.md §24](language.ja.md)。

### Why 明示か

明示的な`import`行があれば、そのファイルが何に依存しているかは
—読者にとってもツールにとっても— そこだけ見れば分かる。`culebra
lint`の未使用import警告 (と`--fix`、Ch.15) が曖昧さなく出せるのも、
AOTビルドが推測なしにバンドルできるのも同じ理由。

---

第III部 — 型とライブラリ
==========================

## 13. 型システム

### 13.1 現状: オプショナル注釈 + `Any`

注釈は3つの境界での**ランタイム**チェック: 変数代入、関数パラ
メータ渡し、関数戻り値。静的なnarrowingは無い。完全な注釈仕様は
[language.ja.md §14](language.ja.md)。

```culebra
add = fn (a: Long, b: Long) -> Long {
  a + b
}
inspect(add(3, 4))  # => 7

# Any は全部受ける。型注釈と動的パラメータは混在できる
identity = fn (x: Any) -> Any {
  x
}
describe = fn (v, label: String) -> String {
  "{label}: {v}"
}
inspect(identity(42))               # => 42
inspect(describe([1, 2], 'array'))  # => 'array: [1, 2]'
```

`type_of` (Ch.2.1) が組み込み型のランタイムintrospection。
`match`節の`n: ClassName` (Ch.6) はそのクラスのインスタンスに
マッチ。

### 13.2 Union / Optional / Tuple / Set

`Long | String`はどちらの型も受け付け、`T?`は`T | Nil`の糖衣、
`(Long, String)`は固定長・不変・要素ごと等価な`Tuple`。完全な
仕様は [language.ja.md §14](language.ja.md) (Union types /
Optional types) と [language.ja.md §10](language.ja.md) (Tuples /
Sets)。

```culebra
show = fn (x: Long | String) -> String {
  to_string(x)
}
inspect(show(1))     # => '1'
inspect(show('hi'))  # => 'hi'

pair = (1, 'one')
inspect(type_of(pair))       # => 'Tuple'
inspect(pair == (1, 'one'))  # => true
```

`Set`は挿入順を保つ、ハッシュ可能な値の重複なしコレクション。
リテラルは2要素以上 — または末尾カンマ — が必要で、空Objectの
`{}`や`{key: value}`の短縮形と衝突しないようになっている。
重複は構築時に潰れ、等価比較は順序を無視する。

```culebra
s = {1, 2, 3, 3}
inspect(s.size())                # => 3
inspect(s.contains(2))           # => true
inspect({1, 2, 3} == {3, 2, 1})  # => true
inspect({42,})                   # => {42}
```

集合演算は演算子でなくメソッド (`|`はラムダ引数に取られている):

```culebra
inspect({1, 2}.union({2, 3}))            # => {1, 2, 3}
inspect({1, 2, 3}.intersect({2, 3, 4}))  # => {2, 3}
inspect({1, 2, 3}.diff({3,}))            # => {1, 2}
```

### 13.3 Trait / Protocol

`trait`は必須メソッド集合を宣言する。それらに (名前・アリティが)
マッチするメソッドを持つクラスなら、明示`impl`無しでconformし
(structural conformance)、必須メソッドを欠くクラスは黙って通らず
ディスパッチが失敗する (`DispatchError`)。traitはデフォルト実装
メソッドも持てて`@derive`で導出できる。nominal (class) 継承は
検討の上、このstructuralモデルを採って不採用とした (Ch.10.3)。
完全な仕様は [language.ja.md §14](language.ja.md) (Traits and
protocols)。

```culebra
trait Greeter {
  hello() -> String
}

class Bob {
  new(name) {
    self.name = name
  }
  hello() {
    "hi, {self.name}"
  }
}

greet = fn (x: Greeter) -> String {
  x.hello()
}
inspect(greet(Bob.new('Alice')))  # => 'hi, Alice'
```

本体を持つtraitメソッドは**デフォルト実装**。conformしたクラスは
それを継承し、同名をクラス側で宣言すれば上書きになる。

```culebra
trait Counter {
  current() -> Long
  next() -> Long {
    self.current() + 1
  }  # デフォルト実装
}

class Zero {
  new() {}
  current() {
    0
  }
}
inspect(Zero.new().next())  # => 1
```

`@derive(...)`はデータクラスが手書きすることになるconformance
メソッドを生成する — `Eq` → `eq`、`Hash` → `hash`、`Show` → `to_s`、
`Comparable` → `cmp`。クラス自身が宣言したメソッドは上書きされない
ので、大半をderiveして1つだけ手書き、ができる。

```culebra
@derive(Eq, Hash, Show)
class Point {
  new(x, y) {
    self.x = x
    self.y = y
  }
}

inspect(Point.new(1, 2).eq(Point.new(1, 2)))  # => true
inspect(Point.new(1, 2).to_s())               # => 'Point(1, 2)'
```

`enum`のvariantは何も導出せずに`Eq`かつ`Hashable`: variantはそのまま
Object / Setのkeyになる（[language.md §14](language.md)「Sum type」）。

### 13.4 Generic

`Array<Long>`のような注釈は要素型をドキュメント化し、マルチメソッ
ドのspecificity (Ch.10.2) にも使われる。要素チェック自体はno-op。
型引数は読み手とディスパッチのために書くもので、これを強制すると
呼び出しの度にコレクションを走査することになる — この注釈はそのため
のものではない。bound制約とgenericクラス宣言は
[language.ja.md §14](language.ja.md)。

```culebra
first = fn (xs: Array<Long>) -> Long {
  xs[0]
}
inspect(first([1, 2, 3]))  # => 1
```

### 注釈がどこで止まるか

型から型を計算する仕組み — 型に対する条件分岐、ある形から別の形への
変換、文字列パターンから組み立てる型 — は意図的に持たない。どれも
学習・実装・デバッグの対象がもう一つ増えることを意味する一方、ここで
の注釈にはもっと狭い仕事しかない: 境界を越える値が違っていたら捕まえ
る、それだけである。型レベルのプログラムで表現することは、ランタイム
の検査でもっと直接に書ける。

## 14. 標準ライブラリ

以下の名前空間はすべて`import`なしで使えます。CLIドライバはさらに
`inspect` / `print` / `println`を`IO.inspect` / `IO.print` /
`IO.println`の別名として入れます。自前で環境を組む埋め込み側には
名前空間だけが見え、この裸の別名は入りません。

```culebra
inspect(to_long('42'))              # => 42
inspect(Math.clamp(15, 0, 10))      # => 10
inspect(re'\d+'.test('order #42'))  # => true
inspect(JSON.stringify({a: 1}))     # => '{"a":1}'
```

### 14.1 何が入っているか

| 領域 | 名前空間 |
|---|---|
| 数値・テキスト | `Math`、`Regex` |
| ファイル・プロセス・環境 | `FS`、`File`、`Path`、`Proc`、`Sys`、`Env` |
| データ形式 | `JSON`、`CSV`、`TOML`、`Encoding`、`Compress`、`Hash`、`UUID`、`Peg`（自前の文法） |
| ネットワーク | `Http`、`Net` |
| 並行 | `Isolate`、`Channel`、`Parallel`、`Shared`、`SharedBuffer` |
| ストレージ | `SQLite` |
| 時刻・CLI・ログ | `Time`、`Args`、`Log` |
| 端末・グラフィクス | `Term`、`Canvas`、`Scene`（experimental）、`Desktop` |

いずれも [`stdlib.ja.md`](stdlib.ja.md) が記述します。目次が名前空間を
順に並べ、その直下の「用途から探す」表が、やりたいこと — ハッシュを
取る、設定ファイルを読む、サブプロセスを走らせる — をそれを行う節に
対応付けます。意図的にまだ入れていないものも、同じ文書の末尾に記録して
あります。

名前空間に属さない組み込み — `to_long`・`to_string`・`type_of`・
`range`・`iota`・マッチャ群 — の仕様は
[language.ja.md §19](language.ja.md#19-コア組み込み関数) にあります。

### 14.2 `Tensor`

`Tensor`は名前空間ではなく言語に組み込まれたn次元配列で、vendoredな
`cpp-tensorlib`エンジン (ベクトル化CPUカーネル、macOSはMetal、
Linux/WindowsはCUDA) が実行します。格納はF32で、スカラー結果は
`Float`として返ります。要素ごとの演算はNumPyと同じくブロードキャスト
し、`dot`は遅延グラフを組んで`Tensor.eval`が1つの融合カーネルとして
走らせます。

```culebra
a = Tensor.from([1.0, 2.0, 3.0])
inspect((a + a).to_array())  # => [2.0, 4.0, 6.0]

m = Tensor.from([[1.0, 2.0], [3.0, 4.0]])
c = m.dot(m)
Tensor.eval(c)
inspect(c.to_array())  # => [[7.0, 10.0], [15.0, 22.0]]
```

同じ型がGPUでも動きます — GPU専用の型はありません。
`Tensor.use_gpu()` / `use_cpu()` / `use_auto()`がバックエンドをプロセス
全体で切り替え、既定の`use_auto`は問題サイズごとに選びます (小さい
テンソルはカーネル起動のオーバーヘッドに負けるため)。形状・リダクション・
autograd・デバイスの詳細は [`stdlib.ja.md` §8](stdlib.ja.md#8-tensor)
にあります。

## 15. ツール (`test`, `lint`, `fmt`, デバッグ)

`culebra`バイナリはツールチェーンそのものでもあります。テストランナー・
リンタ・フォーマッタ・デバッグアダプタは同じ実行ファイルのサブコマンドなので、
追加でインストールするものはありません。この章は概観です。フラグ単位の
リファレンスは [`tooling.ja.md`](tooling.ja.md) にあります。

### 15.1 テスト

テストファイルは`test_*.cul`という名前の`.cul`ファイルです。その中では
`test()`・`@test`デコレータ・`@parametrize`が環境に備わっており
（import不要）、表明にはマッチャ群を使います:

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
テスト終了時に`drop`が呼ばれます (Ch.7.4)。同じランナーはこのガイドの
例も実行します — `culebra test --doc docs` — 本書のすべての
` ```culebra `ブロックが期待出力を持っているのはそのためです。

### 15.2 lint とフォーマット

`culebra lint`は、プログラムを実行せずに静的解析で分かることを報告します。
実行すればどのみち中断されるエラーに加えて、助言的な警告（未使用の変数・
import、到達不能コード）も出します。終了コードはcleanが0、警告のみが1、
エラーが2なのでCIのゲートに使えます。

```bash
culebra lint .          # カレントディレクトリ配下の .cul を再帰的に
culebra lint --fix .    # 加えて未使用 import 行を削除する
```

`culebra fmt`は設定不要のフォーマッタです（スタイルフラグはありません）。
パース → 再出力 → **再パースして比較**してから書き出すので、意味を変えたり
コメントを落としたりするフォーマットは適用せず拒否します。

```bash
culebra fmt -i .        # プロジェクトをその場で整形
culebra fmt --check .   # 未整形があれば exit 1（CI ゲート）
```

エディタはstdin形式 (`culebra fmt -`) でフォーマッタを呼びます。同梱の
VSCode・Zed・Vim統合はすでにこれに繋いであります。

### 15.3 デバッグ

`culebra dap`はDebug Adapter Protocolをstdioで話します。ブレークポイント、
ステップ実行、コールスタック、ウォッチ式、実行中の`mut`変数の書き換えが、
DAPに対応した任意のエディタで動きます。アダプタを起動するのはエディタ側で、
手で走らせることはまずありません。デバッグはVMで動くので`--jit`
は付けないでください。

ソース中に裸の`debugger`文を置けば、設定なしでもその場所で必ず停止します。
エディタ別のセットアップ（VSCode・Vim・Zed）は
[`tooling.ja.md` §4](tooling.ja.md#4-デバッグ-culebra-dap) にあります。

## 16. AOT バイナリビルド

`culebra build`は`.cul`ソースをahead-of-timeで自己完結バイナ
リにコンパイルする。ランタイムにLLVM不要。tree-shakingで使われ
ないランタイムヘルパを落とす。Tensorを使わないプログラムでは
tensorエンジンが必要とするAccelerate / Metalフレームワーク依存も
外せる。

```bash
./build/culebra build my-program.cul -o ./out
./out                                     # standalone、~6 MB on macOS
otool -L ./out                            # Accelerate も Metal も LLVM も無し
```

codegenはプロセス内で完結する。Windowsではリンクもそうで、バイナリが
lldを内蔵し、必要なmingwライブラリは`culebra toolchain install`が取得する。
それ以外のプラットフォームではリンク段がホストのC++コンパイラを起動する —
macOSはXcode Command Line Tools、Linuxは`cc`。`build`は開始前に確認し、
端末上なら足りないものをインストールするか尋ねる。プラットフォーム別の
一覧は[`deployment.ja.md` §1](deployment.ja.md#ホスト側に必要なもの)。

### 16.1 クロスコンパイル

```bash
./build/culebra build my-program.cul \
  --target=x86_64-unknown-linux-gnu \
  --sysroot=$LINUX_SYSROOT \
  --rt-lib=$PWD/build-linux-x86_64/libculebra_rt.a \
  -o ./out-linux
```

ランタイムアーカイブのビルド、sysrootの用意、クロスコンパイル全
ワークフローの詳細は [`deployment.ja.md`](deployment.ja.md)。

### Why tree-shaking が効くか

`inspect`だけ使う "hello world" はtensorもHTTPランタイムも要らない。
エントリファイルからcall graphを辿ることで、参照されていないラン
タイムヘルパ (~450個) を落とせる。`Tensor`参照が無ければtensor抜きの
archiveに差し替わるので、全feature archiveを抱えると ~12.5 MBのところ
~6 MBに収まる。

## 17. 埋め込み概観

Culebraはヘッダだけで使えるC++23ライブラリ。最小の埋め込み例:

```cpp
#include <culebra.h>
#include <vm/embed.h>

int main() {
  culebra::Runtime rt;
  culebra::RuntimeScope scope(rt);
  culebra::vm::Embed embed;   // 標準ライブラリとtraitを登録済み

  culebra::vm::Value val;
  std::vector<std::string> msgs;
  embed.run_source("<inline>", R"(
    add = fn (a, b) { a + b }
    add(40, 2)
  )", val, msgs);
  std::cout << val.to_long() << "\n";   // 42
}
```

`vm::Embed`はセッション。`run_source`は前の実行が作った束縛を引き継ぎ、
ホスト側は`embed.global`で読み、`embed.call`で呼べる。標準ライブラリは
登録済みで、`inspect` / `print` / `println`のグローバルも入る（スクリプト
から見えるのと同じ名前）。スクリプト側の`throw`は`culebra::CulebraError`
例外になり、投げられた値のkind・メッセージ・位置を持つ。

環境カスタマイズ、値変換、JITホスト、AOT-archive埋め込み経路
(`libculebra_rt.a`) の詳細は [`deployment.ja.md`](deployment.ja.md)。

---

次の一歩
--------

- 厳密な文法と評価規則: [`language.ja.md`](language.ja.md)
- APIリファレンス: [`stdlib.ja.md`](stdlib.ja.md)
- バイナリビルド・埋め込み・ラッピング: [`deployment.ja.md`](deployment.ja.md)
- 大きめの実例: [`benchmarks/microgpt/`](../benchmarks/microgpt/)
- インタラクティブなREPL: `./build/culebra --shell`
