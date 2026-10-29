# PEGで解析する

`PEG`名前空間の使い方を、順を追って紹介します。正規表現では手が届かない
ところで文法を書くところから始めて、最後は自分用の小さな言語と、それを
動かすインタプリタまで作ります。

このガイドに出てくるコードブロックは、すべて実際に動きます。doctestとして
両方のエンジンで実行されているので、書きっぱなしのものは1つもありません。

APIのリファレンスは[`stdlib.ja.md` §35](../stdlib.ja.md#35-peg)にあります。
あちらは「各呼び出しが何をするか」を引くための文書で、こちらは
「どういうときに使うか」「文法をどう組み立てるか」のための文書です。

## PEGとは

PEG（parsing expression grammar）は、構文解析器を得るために書く文法
です。かつてYACCに渡していた仕事だと思ってください。実務で効いてくる違いは3つあります。1つめは、字句解析器を別に用意しなくてよいことです。PEGはトークンと同じように文字も記述できるので、字句解析も規則として書けます。2つめは、ビルド手順が要らないことです。`PEG.parser`が実行時に文法を読み込むので、文法はプログラムの中のただの文字列です。3つめは、選択に**順序がある**ことです。`a / b`は`a`が一致した時点で`a`に決めるので、shift/reduce conflictが報告されて解決を迫られる、ということが起きません。つまりPEGは作りからして曖昧さを持たず、書いた順に上から読めます。なお、PEGの有名な
制約が1つ、ここでは当てはまりません。左再帰です。PEGは本来これをまったく
扱えませんが、エンジンの
[cpp-peglib](https://github.com/yhirose/cpp-peglib)（culebra自身の
フロントエンドが載っているのと同じパーサ）は、左再帰を扱えるように拡張
された実装の1つなので、ここでは問題になりません。

## なぜ文法を書くのでしょうか

Bert Hubertが[Practical PEG parsing](https://berthub.eu/articles/posts/practical-peg-parsing/)
という記事で、PEGの立ち位置をうまく言い表しています。簡単なことは簡単に、
難しいことも可能に、というのがそれです。

正規表現は、簡単なことにはとても向いています。ただ、難しくなってくると
手が出せません。ここでいう難しさとは構造のことです。入れ子、エスケープ、
「このトークン、ただしあのトークンの中にあるものは除く」といったもの
です。そこで手書きのスキャナを書きはじめて、気がつくと半日たっている、
というのがよくある展開ではないでしょうか。

PEGはその中間にあります。小さな仕事なら正規表現と同じくらいの手間で
書けますし、正規表現と違って、仕事が育っても壊れません。効いているのは
次の3つの性質です。

* **規則どうしを組み合わせられます。** 規則は他の規則を参照できますし、
  自分自身を参照することもできます。入れ子が扱えるのはこのおかげで、
  正規表現に根本的に欠けているのもここです。
* **選択に順序があります。** `a / b`は、まず`a`を試して、失敗したときだけ
  `b`を試します。どちらを採るか迷う場面が生まれないので、文法は書いた
  とおりの意味を、書いた順序どおりに持ちます。
* **木が手に入ります。** culebraでは木が素の`Object`なので、`match`が
  そのまま分解できます。あいだにvisitor APIのようなものを挟みません。

エンジンには[cpp-peglib](https://github.com/yhirose/cpp-peglib)を使って
います。culebra自身のフロントエンドが載っているのと同じパーサです。

## 1. 行から取り出す — 正規表現の手が届かないところ

テキストを扱う仕事のいちばんよくある形は、行のループです。1行ずつ
パターンに当てて、その中身を取り出していきます。正規表現はまさにその
ための道具なのですが、パターンが**入れ子になった**とたんに、話が変わり
ます。書きにくくなるのではなく、正規言語で表せる範囲の外に出てしまう
のです。

ここでは3つ例を挙げます。どれも正規表現で一度はやったことのある仕事で、
どれも再帰する規則が1つだけ入っています。

### 1.1 構造化ペイロードを持つログ行

まず入力を見てください。1行ごとに、構造を持ったコンテキストが付いている
ログです。

```
2026-09-01T10:00:02 WARN retry ctx={user: {id: 7, tags: [a, b]}, path: /x}
2026-09-01T10:00:05 INFO ok ctx={path: /y}
```

ここから各行について、レベル、メッセージ、`ctx`の**トップレベルの**キー名、
そして入れ子の深さを取り出したい、とします。欲しい出力はこうです。

```
WARN retry keys=['user', 'path'] depth=3
INFO ok keys=['path'] depth=1
```

時刻・レベル・メッセージまでは正規表現の得意分野です。ですが`ctx=`の
あとの値は違います。オブジェクトと配列が入れ子になっているからです。

`PEG.parser`が文法からパーサを作り、`enable_ast(true)`が木の生成を有効にします。
`true`は、子が1つだけのノードを畳んだ木にする指定です（§3でもう一度
触れます）。パーサの`parse`は`(ok, tree)`を返します。パースできない行では
`ok`が`false`、木が`nil`になります。ここの行はどれもパースできるので、以下の例では
そのまま木を使います。

```culebra
let g = `
  Line  <- Time Level Msg 'ctx=' Obj
  Time  <- < [0-9:T-]+ >
  Level <- < [A-Z]+ >
  Msg   <- < [a-z_]+ >
  Obj   <- '{' Pair (',' Pair)* '}'    { no_ast_opt }
  Pair  <- Key ':' Value
  Key   <- < [a-z_]+ >
  Value <- Obj / Arr / Word
  Arr   <- '[' Value (',' Value)* ']'  { no_ast_opt }
  Word  <- < [^ ,}\]]+ >
  %whitespace <- [ ]*
`
let p = PEG.parser(g)
p.enable_ast(true)

fn depth(n) {
  match n {
    {name: 'Obj', nodes} => 1 + nodes.map(|kv| depth(kv.nodes[1])).max(),
    {name: 'Arr', nodes} => 1 + nodes.map(depth).max(),
    _ => 0,
  }
}

let log = '2026-09-01T10:00:02 WARN retry ctx={user: {id: 7, tags: [a, b]}, path: /x}
2026-09-01T10:00:05 INFO ok ctx={path: /y}'

for line in log.lines() {
  let (ok, n) = p.parse(line)
  let ctx = n.nodes[3]
  let keys = ctx.nodes.map(|kv| kv.nodes[0].token)
  println("{n.nodes[1].token} {n.nodes[2].token} keys={keys} depth={depth(ctx)}")
}
# => |
# WARN retry keys=['user', 'path'] depth=3
# INFO ok keys=['path'] depth=1
```

ここで正規表現がうまくいかない理由は、2つあります。どちらも知って
おく価値があります。

1つめは、**値がどこで終わるのかを決められない**ことです。
`ctx=\{(.*)\}`は貪欲なので、その行の最後の`}`まで走ってしまいます。
`ctx=\{(.*?)\}`は控えめなので、最初の`}`で止まります。1行目でそれに
あたるのは`{id: 7, tags: [a, b]}`を閉じている`}`なので、取り出した
ペイロードは途中で切れてしまいます。

2つめは、**そのキーがどの階層にあるのかを区別できない**ことです。
「トップレベルのキーだけ並べる」は、正規表現では書きようがありません。
深さという概念を持たないパターンにとって、`id`と`tags`は`user`や`path`と
まったく同じに見えるからです。文法のほうは、その概念を最初から持って
います。`depth`が6行で書けているのも同じ理由です。

### 1.2 型シグネチャ

入力は型名の一覧です。シンボルの一覧や、コンパイラの診断から出てくるような
ものだと思ってください。

```
Map<String, List<Pair<Int, String>>>
Result<Vec<u8>, Error>
i32
```

ここから、外側の型名と、その型引数を取り出します。引数はトップレベルの
カンマでだけ分けます。

```
Map <- ['String', 'List<Pair<Int, String>>']
Result <- ['Vec<u8>', 'Error']
i32 <- []
```

この「トップレベルのカンマでだけ分ける」が、入れ子の型に対していちばん
やりたいことであり、正規表現には決してできないことでもあります。

```culebra
let g = `
  Type <- Name ('<' Args '>')?  { no_ast_opt }
  Name <- < [A-Za-z_] [A-Za-z0-9_]* >
  Args <- Type (',' Type)*      { no_ast_opt }
  %whitespace <- [ ]*
`
let p = PEG.parser(g)
p.enable_ast(true)

let lines = 'Map<String, List<Pair<Int, String>>>
Result<Vec<u8>, Error>
i32'

for src in lines.lines() {
  let (ok, n) = p.parse(src)
  let args = n.nodes.size() > 1 ? n.nodes[1].nodes : []
  let texts = args.map(|a| src.slice(a.position, a.position + a.length))
  println("{n.nodes[0].token} <- {texts}")
}
# => |
# Map <- ['String', 'List<Pair<Int, String>>']
# Result <- ['Vec<u8>', 'Error']
# i32 <- []
```

`<(.+)>`で中身を取って`,`で分割すると、1行目は`String`、
`List<Pair<Int`、`String>>`の3つに割れてしまいます。正規表現にとっては、
どの深さのカンマも同じカンマだからです。`Type`が`Args`を通して自分自身を
参照している、それだけで直ります。文法は4行です。

最後の行にも注目してください。`i32`には型引数がありませんが、同じ文法が
そのまま扱えています。`<...>`のまとまりを省略可能にしてあるからです。
「そのパターンが無いこともある」に対応しようとすると、正規表現では
たいてい、選択肢を1つ増やしてキャプチャグループを足すことになります。

### 1.3 Markdownのリンク

入力はリンクを含む文章です。ここではリンクテキストにも、URLにも、括弧が
入っています。

```
See [the [inner] guide](https://x/a(b).md) now.
Also [plain](https://y/z) and [a] alone.
```

取り出したいのは、本物のリンクのテキストとURLです。リンクの形をしていない
`[a]`は拾いません。

```
text=the [inner] guide  url=https://x/a(b).md
text=plain  url=https://y/z
```

おなじみの`\[([^\]]*)\]\(([^)]*)\)`は、いま挙げたような括弧が出てくるまでは
ちゃんと動きます。そしてWikipediaのURLには、丸括弧がしょっちゅう入って
います。

```culebra
let g = `
  Doc   <- (Link / Other)*        { no_ast_opt }
  Link  <- '[' Text ']' '(' Url ')'
  Text  <- < Inner* >
  Inner <- '[' Inner* ']' / !']' .
  Url   <- < Part* >
  Part  <- '(' Part* ')' / !')' .
  Other <- < . >
`
let p = PEG.parser(g)
p.enable_ast(true)

let lines = 'See [the [inner] guide](https://x/a(b).md) now.
Also [plain](https://y/z) and [a] alone.'

for line in lines.lines() {
  let (ok, links) = p.parse(line)
  for n in links.nodes {
    if n.name == 'Link' {
      println("text={n.nodes[0].token}  url={n.nodes[1].token}")
    }
  }
}
# => |
# text=the [inner] guide  url=https://x/a(b).md
# text=plain  url=https://y/z
```

`[^\]]*`は最初の`]`で止まり、その後ろに`(`が続かないので、1つめのリンクは
そもそも見つかりません。また、テキストに括弧が無ければ、`[^)]*`のほうが
URLを`a(b`で切ってしまいます。どちらも同じ間違いです。文字クラスは数を数えられません。
そしてどちらも、自分自身を参照する規則1つで直ります。

この例には、行を扱うときのもう1つのコツも入っています。`Other <- < . >`が
あるおかげで、この文法は興味のある部分だけでなく**行全体**を書き表して
います。だから1行にリンクがいくつあってもよく、あいだに文章がどれだけ
あってもかまいません。2行目の`[a]`はリンクの形をしていないので、拾われ
ません。

## 2. 四則計算 — 優先順位と、評価の2通り

四則計算は、本格的な文法設計が必要になる最小の題材です。優先順位を、
規則の形そのものから出さなければならないからです。

定石は、優先順位1段につき規則を1つ作り、結合の弱いほうが強いほうを参照
する形にすることです。

文法の教科書どおりに書くと、この各規則は**左再帰**になります
（`Expr <- Expr AddOp Term / Term`）。PEGは本来これを扱えません。何も
消費しないまま自分自身に戻る規則は、そのまま無限に回ってしまうからです。
素のPEGの文法が左再帰を避けた形で書かれているのは、そのためです。ただ
cpp-peglibは、左再帰を扱えるように拡張された実装の1つで（CPythonのPEG
パーサも同じく拡張されています）、そのおかげでここでは教科書どおりの形が
そのまま書けます。

```culebra
let calc = `
  Expr   <- Expr AddOp Term / Term
  Term   <- Term MulOp Factor / Factor
  Factor <- Number / '(' Expr ')'
  AddOp  <- < '+' / '-' >
  MulOp  <- < '*' / '/' >
  Number <- < '-'? [0-9]+ >
  %whitespace <- [ \t\r\n]*
`

fn eval(n) {
  match n {
    {name: 'Number', token} => to_long(token),
    {nodes: [a, op, b]} => match op.token {
      '+' => eval(a) + eval(b),
      '-' => eval(a) - eval(b),
      '*' => eval(a) * eval(b),
      _ => eval(a) / eval(b),
    },
    _ => throw "unexpected {n.name}",
  }
}

let parser = PEG.parser(calc)
parser.enable_ast(true)
fn calculate(src) {
  let (ok, tree) = parser.parse(src)
  eval(tree)
}

inspect(calculate('1 + 2 * 3'))    # => 7
inspect(calculate('(1 + 2) * 3'))  # => 9
inspect(calculate('1 - 2 - 3'))    # => -4
```

`Expr <- Expr AddOp Term / Term`は、そのまま読める形をしています。式とは、
式と演算子と項が並んだものか、あるいは項1つである、と言っています。
返ってくる木は**左**に入れ子になるので、`1 - 2 - 3`は`2`ではなく`-4`に
なります。ノードはどれも子3つの組なので、評価器は分岐1つで済みます。

もう1つの書き方が`Expr <- Term (AddOp Term)*`です。左再帰を避ける代わりに、
`[Term, op, Term, op, Term]`という平らな並びが返ってくるので、自分で
畳みながら結合方向を決めることになります。平らな並びが欲しいときは
こちらを使ってください。そうでなければ、左再帰の形のほうが文法も評価器も
短く済みます。

`%whitespace`は、文法を読める形に保ってくれる唯一の糖衣構文です。これが
ないと、どの規則にも空白の入りうる位置を書き並べることになってしまいます。

### 同じ文法を、木を作らずに使う

答えだけが欲しいときは、各規則に*action*を付けると入力を直接解釈できます。
actionは関数で、その規則が集めた値を受け取って、その規則が親に渡す値を
返します。規則にactionが付いているパーサは木を作らず、`parse`は
`(ok, value)`を返します。

```culebra
let calc = `
  Expr   <- Expr AddOp Term / Term
  Term   <- Term MulOp Factor / Factor
  Factor <- Number / '(' Expr ')'
  AddOp  <- < '+' / '-' >
  MulOp  <- < '*' / '/' >
  Number <- < '-'? [0-9]+ >
  %whitespace <- [ \t\r\n]*
`

fn apply(vs) {
  if vs.size() < 3 {
    return vs[0]
  }
  match vs[1] {
    '+' => vs[0] + vs[2],
    '-' => vs[0] - vs[2],
    '*' => vs[0] * vs[2],
    _ => vs[0] / vs[2],
  }
}

let parser = PEG.parser(calc)
parser['Number'] = |vs| vs.token_to_number()
parser['AddOp'] = |vs| vs.token()
parser['MulOp'] = |vs| vs.token()
parser['Expr'] = apply
parser['Term'] = apply

inspect(parser.parse('1 + 2 * 3 - 4'))  # => (true, 3)
inspect(parser.parse('8 / 4 / 2'))      # => (true, 1)
```

使い分けの目安はこうです。入力を調べたい、変換したい、何度も走査したい
ときは木を使ってください。解析すること自体が計算になっていて、木は
作って捨てるだけになるときは、actionのほうが素直です。

## 3. 言語まるごと

これから動かすプログラムを、先に見てください。

```
def fib(x)
  x < 2 ? 1 : fib(x - 2) + fib(x - 1)

for n from 1 to 10
  puts(fib(n))
```

出力はこうなってほしい、とします。

```
[1, 2, 3, 5, 8, 13, 21, 34, 55, 89]
```

この言語は[fiblang](https://github.com/yhirose/fiblang)です。まさにこの
プログラムを書くためだけに存在していて、それ以外には何もできません。
一度に読み切れる大きさですし、実在の言語でもあります。この文法は
cpp-peglib自身のテストケースの1つです。

fiblangでは**すべてが式**です。`for`も式なので、プログラムに文の区切りが
なく、文法にも文の規則がありません。関数の引数は1つ、比較演算子は1つ、
算術演算子は2つ。これで言語の全部です。

下の文法はfiblangのもので、エラー回復用の2つの演算子（`↑`と`%recover`）
だけ外してあります。あれは別の機能なので、ここでは扱いません。

```culebra
let grammar = `
  START             <- STATEMENTS
  STATEMENTS        <- (DEFINITION / EXPRESSION)*   { no_ast_opt }
  DEFINITION        <- 'def' Identifier '(' Identifier ')' EXPRESSION
  EXPRESSION        <- TERNARY
  TERNARY           <- CONDITION ('?' EXPRESSION ':' EXPRESSION)?
  CONDITION         <- INFIX (ConditionOperator INFIX)?
  INFIX             <- CALL (InfixOperator CALL)*
  CALL              <- PRIMARY ('(' EXPRESSION ')')?
  PRIMARY           <- FOR / Identifier / '(' EXPRESSION ')' / Number
  FOR               <- 'for' Identifier 'from' Number 'to' Number EXPRESSION

  ConditionOperator <- < '<' >
  InfixOperator     <- < '+' / '-' >
  Identifier        <- !Keyword < [a-zA-Z][a-zA-Z0-9_]* >
  Number            <- < [0-9]+ >
  Keyword           <- 'def' / 'for' / 'from' / 'to'

  %whitespace       <- [ \t\r\n]*
  %word             <- [a-zA-Z]
`
let fiblang = PEG.parser(grammar)
fiblang.enable_ast(true)

fn run(src) {
  mut fns = {}
  mut out = []

  fn eval(n, env) {
    match n {
      {name: 'Number', token} => to_long(token),

      {name: 'Identifier', token} => env[token],

      {name: 'CONDITION', nodes: [a, op, b]} => eval(a, env) < eval(b, env),

      {name: 'TERNARY', nodes: [c, t, f]} => eval(c, env) ? eval(t, env) : eval(f, env),

      {name: 'INFIX', nodes} => {
        mut acc = eval(nodes[0], env)
        mut i = 1
        while i < nodes.size() {
          let r = eval(nodes[i + 1], env)
          acc = nodes[i].token == '+' ? acc + r : acc - r
          i += 2
        }
        acc
      },

      {name: 'CALL', nodes: [f, arg]} => {
        let v = eval(arg, env)
        if f.token == 'puts' {
          out.push(v)
          v
        } else {
          let d = fns[f.token]
          mut frame = {}
          frame[d.param] = v
          eval(d.body, frame)
        }
      },

      {name: 'FOR', nodes: [name, lo, hi, body]} => {
        for i in to_long(lo.token)..=to_long(hi.token) {
          mut inner = {...env}
          inner[name.token] = i
          eval(body, inner)
        }
        nil
      },

      {name: 'DEFINITION', nodes: [name, param, body]} => {
        fns[name.token] = {param: param.token, body: body}
        nil
      },

      {name: 'STATEMENTS', nodes} => {
        for c in nodes {
          eval(c, env)
        }
        nil
      },
    }
  }

  let (ok, program) = fiblang.parse(src, 'fib')
  eval(program, {})
  out
}

inspect(run('def fib(x)
  x < 2 ? 1 : fib(x - 2) + fib(x - 1)

for n from 1 to 10
  puts(fib(n))'))
# => [1, 2, 3, 5, 8, 13, 21, 34, 55, 89]
```

この中で名前を付けておきたいものが4つあります。どれも、言語の文法を書く
たびに出てくる話です。

**すべてを式にしていることが、この文法の小ささを支えています。** 文の
規則も、ブロックも、区切り記号も、`return`もありません。`for`も`def`も
ほかと同じ式ですし、関数の本体は式1つです。逆に言うと、式でない機能を
足すたびに、規則が1つと評価器の分岐が1つ増えます。

**キーワードは2か所で除外する必要があります。**
`Identifier <- !Keyword < ... >`は、`for`が変数名として読まれるのを
止めます。もう1つが`%word <- [a-zA-Z]`です。これは「文法の中の`'for'`の
ようなリテラルは、単語の切れ目で終わらなければならない」とパーサに
伝えるものです。これがないと、`format`という変数名が`for`と`mat`に
読まれてしまいます。

**AST最適化が刈り込みをしてくれるので、評価器には意味のある規則しか
届きません。** `enable_ast(true)`の`true`がその指定です。
`EXPRESSION <- TERNARY`は子が1つなので丸ごと消えます。
`TERNARY`が残るのは実際に`?`が一致したときだけ、`CONDITION`が残るのは
`<`が一致したときだけ、`CALL`が残るのは括弧があったときだけです。評価器の
分岐が規則ごとではなく**概念ごと**になっているのは、そのためです。
例外は`STATEMENTS`で、`{ no_ast_opt }`を付けてあります。文が1つだけの
プログラムでも、その1文ではなくリストとして届いてほしいからです。

**また平らな並びが出てきます。** `INFIX <- CALL (InfixOperator CALL)*`は
§2で見た繰り返しの形なので、`a - b - c`は子5つとして届き、評価器は§2で
`Expr <- Term (AddOp Term)*`について説明した畳み込みと同じやり方で、左から畳んでいます。もう一方の書き方
（`INFIX <- INFIX InfixOperator CALL / CALL`）にすれば、最初から入れ子で
届いて、分岐は子3つになります。両方が載っているので、並べて見比べて
みてください。

ここから育てるときも、作業はほとんど規則を足すことです。引数を2つに
するなら`DEFINITION`の中を`Identifier (',' Identifier)*`にして、`frame`を
リストで埋めます。演算子を増やすなら`InfixOperator`や`ConditionOperator`に
足したうえで、§2のやり方で優先順位の段を1つ用意します。分岐を書く前に
木の形を見たくなったら、`PEG.ast_to_s`が表示してくれます（§4）。

## 4. 落とし穴

最後に、先に知っておくと半日を節約できるものを並べておきます。

**左再帰はここでは書けますが、これは拡張です。** `Expr <- Expr AddOp
Term / Term`は問題なく動きますし、別の規則を経由した間接的な左再帰も
通ります。木も、望みどおり左に入れ子になります（§2）。ただし、これは
本来のPEGではないことを覚えておいてください。もともとのPEGでは、何も
消費しないまま自分に戻る規則は無限にループしますし、多くの実装はいまも
そのままです。他のライブラリへ持っていく文法なら、
`Expr <- Term (AddOp Term)*`の形に書き直す必要があるかもしれません。

**順序付き選択は「どちらか」ではありません。** `'a' / 'ab'`は`ab`に決して
一致しません。長い選択肢と長いリテラルを先に置いてください。

**繰り返しは、いったん取った分を戻してくれません。** `[a-z]* 'x'`は
`abx`で失敗します。`*`がすでに`x`まで取ってしまっているからです。
`(!'x' [a-z])* 'x'`のように、先読みで「どこで止まってほしいか」を
書いてください。

**ASTを走査するコードを疑う前に、ASTそのものを見てください。**
`PEG.ast_to_s(node)`が`peglint --ast`と同じ形でASTを表示します。AST最適化で
どのノードが畳まれたかを頭の中で考えるより、たいていこちらが速く済みます。

```culebra
let g = `
  Wrap  <- Inner
  Inner <- < [0-9]+ >
`

let parser = PEG.parser(g)
parser.enable_ast(true)
print(PEG.ast_to_s(parser.parse('1')[1]))
# => |
# - Inner (1)
```

**選択肢が接頭辞を共有する文法では、packratを有効にしてください。**
メモ化は`parser.enable_packrat_parsing()`を呼ぶまで無効です。
`A <- B '+' A / B`（最初に書く文法は、たいていこの形になります）は、
その接頭辞を選択肢の数だけ解析し直すので、メモ化がないと指数時間です。
リファレンスの実測では、10段の入れ子がメモ化ありで0.04ms、なしで2.0秒
でした。このガイドの文法はこの形をしていないので、どれも有効にしていません。

**パーサは1度作って、何度も解析しましょう。** 高いのは文法の読み込みの
ほうです。読み込み済みの文法はスレッドごとに保持されるので、同じ文法の
`PEG.parser`をもう一度呼んでも罠ではありませんが、多数の入力を回すループ
なら、パーサを外に出したほうが読みやすくなります。

## この先

* [`stdlib.ja.md` §35](../stdlib.ja.md#35-peg) — 全呼び出し、ノードの
  フィールド、ロガーと深さの上限。規則の判定に文脈が要る文法のための
  述語と`enter` / `leave`、cpp-peglibのAPIと違う点もここにあります。
* [cpp-peglibの構文リファレンス](https://github.com/yhirose/cpp-peglib#syntax)
  — このガイドで使わなかった部分も含めた、記法の全体。
* [Practical PEG parsing](https://berthub.eu/articles/posts/practical-peg-parsing/)
  by Bert Hubert — 同じ主張をC++から見たものです。Prometheusのメトリクスを
  解析する実物が載っています。文法を使う価値があるかどうかの判定として、
  彼が出している課題は公平だと思います。手書きの版を書いてみて、時間を
  計ってみてください。
