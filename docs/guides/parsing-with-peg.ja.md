# PEGで解析する

`Peg`名前空間の使い方を、順を追って紹介します。正規表現では手が届かない
ところで文法を書くところから始めて、最後は自分用の小さな言語と、それを
動かすインタプリタまで作ります。

このガイドに出てくるコードブロックは、すべて実際に動きます。doctestとして
両方のエンジンで実行されているので、書きっぱなしのものは1つもありません。

APIのリファレンスは[`stdlib.ja.md` §34](../stdlib.ja.md#34-peg)にあります。
あちらは「各呼び出しが何をするか」を引くための文書で、こちらは
「どういうときに使うか」「文法をどう組み立てるか」のための文書です。

## PEGとは

PEG（parsing expression grammar）は、構文解析器を得るために書く文法
です。かつてYACCに渡していた仕事だと思ってください。実務で効いてくる
違いは3つあります。字句解析器を別に用意しなくてよいこと（PEGは
トークンと同じように文字も記述できるので、字句解析も規則として書けます）。
ビルド手順が要らないこと（`Peg.compile`が実行時に文法を読み込むので、
文法はプログラムの中のただの文字列です）。そして選択に**順序がある**
こと — `a / b`は`a`が一致した時点で`a`に決めるので、shift/reduce
conflictが報告されて解決を迫られる、ということが起きません。つまりPEGは
作りからして曖昧さを持たず、書いた順に上から読めます。なお、PEGの有名な
制約が1つ、ここでは当てはまりません。左再帰です。PEGは本来これをまったく
扱えないのですが、エンジンの
[cpp-peglib](https://github.com/yhirose/cpp-peglib)（culebra自身の
フロントエンドが載っているのと同じパーサ）が、左再帰を扱えるように拡張
された実装の1つだからです。

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
let p = Peg.compile(g)

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
  let n = p.parse(line)
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
let p = Peg.compile(g)

let lines = 'Map<String, List<Pair<Int, String>>>
Result<Vec<u8>, Error>
i32'

for src in lines.lines() {
  let n = p.parse(src)
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
let p = Peg.compile(g)

let lines = 'See [the [inner] guide](https://x/a(b).md) now.
Also [plain](https://y/z) and [a] alone.'

for line in lines.lines() {
  for n in p.parse(line).nodes {
    if n.name == 'Link' {
      println("text={n.nodes[0].token}  url={n.nodes[1].token}")
    }
  }
}
# => |
# text=the [inner] guide  url=https://x/a(b).md
# text=plain  url=https://y/z
```

`[^\]]*`は最初の`]`で止まるので、1つめのリンクはテキストが`the [inner`に
なり、URLは空になってしまいます。`[^)]*`のほうもURLを`a(b`で切って
しまいます。どちらも同じ間違いです。文字クラスは数を数えられません。
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

inspect(eval(Peg.parse(calc, '1 + 2 * 3')))    # => 7
inspect(eval(Peg.parse(calc, '(1 + 2) * 3')))  # => 9
inspect(eval(Peg.parse(calc, '1 - 2 - 3')))    # => -4
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

答えだけが欲しいときは、`actions`を使うと入力を直接解釈できます。規則名
から関数へのマップを渡す形で、各関数はその規則の畳み込みを受け取り、その
規則が親に渡す値を返します。

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

fn apply(sv) {
  if sv.values.size() < 3 {
    return sv.values[0]
  }
  match sv.values[1] {
    '+' => sv.values[0] + sv.values[2],
    '-' => sv.values[0] - sv.values[2],
    '*' => sv.values[0] * sv.values[2],
    _ => sv.values[0] / sv.values[2],
  }
}

let actions = {
  Number: |sv| to_long(sv.token),
  AddOp: |sv| sv.token,
  MulOp: |sv| sv.token,
  Expr: apply,
  Term: apply,
}

inspect(Peg.parse(calc, '1 + 2 * 3 - 4', actions: actions))  # => 3
inspect(Peg.parse(calc, '8 / 4 / 2', actions: actions))      # => 1
```

使い分けの目安はこうです。入力を調べたい、変換したい、何度も歩きたい
ときは木を使ってください。解析すること自体が計算になっていて、木は
作って捨てるだけになるときは、actionsのほうが素直です。

## 3. 小さな言語

ここまでのやり方は、そのまま本物のインタプリタまで届きます。

これから作る言語は、整数の変数、算術、比較、`if`/`else`、`while`、
引数つきの関数、そして再帰を持ちます。プログラムと呼べるものが書ける
程度には揃っています。文法と評価器をあわせて130行ほどです。

長めですが、ここまでに出てきた道具しか使っていません。

```culebra
let grammar = `
  Program  <- Stmt+                              { no_ast_opt }
  Stmt     <- Fn / Return / If / While / Assign / ExprStmt
  Fn       <- 'fn' Ident '(' Params ')' Block    { no_ast_opt }
  Params   <- (Ident (',' Ident)*)?              { no_ast_opt }
  Block    <- '{' Stmt* '}'                      { no_ast_opt }
  Return   <- 'return' Expr ';'                  { no_ast_opt }
  If       <- 'if' Expr Block Else?              { no_ast_opt }
  Else     <- 'else' Block
  While    <- 'while' Expr Block                 { no_ast_opt }
  Assign   <- Ident '=' Expr ';'                 { no_ast_opt }
  ExprStmt <- Expr ';'                           { no_ast_opt }

  Expr     <- Cmp
  Cmp      <- Add CmpOp Add / Add
  Add      <- Add AddOp Mul / Mul
  Mul      <- Mul MulOp Unary / Unary
  Unary    <- Neg / Primary
  Neg      <- '-' Unary                          { no_ast_opt }
  Primary  <- Call / Number / Ident / '(' Expr ')'
  Call     <- Ident '(' Args ')'                 { no_ast_opt }
  Args     <- (Expr (',' Expr)*)?                { no_ast_opt }

  CmpOp    <- < '==' / '!=' / '<=' / '>=' / '<' / '>' >
  AddOp    <- < '+' / '-' >
  MulOp    <- < '*' / '/' / '%' >
  Number   <- < [0-9]+ >
  Ident    <- < !Keyword [a-z_] [a-zA-Z0-9_]* >
  Keyword  <- ('fn' / 'return' / 'if' / 'else' / 'while') ![a-zA-Z0-9_]

  %whitespace <- ([ \t\r\n] / '#' (!'\n' .)*)*
`
let lang = Peg.compile(grammar)

fn lookup(scopes, name) {
  mut i = scopes.size() - 1
  while i >= 0 {
    if scopes[i].has(name) {
      return scopes[i][name]
    }
    i -= 1
  }
  throw "undefined variable '{name}'"
}

fn store(scopes, name, v) {
  mut i = scopes.size() - 1
  while i >= 0 {
    if scopes[i].has(name) {
      scopes[i][name] = v
      return nil
    }
    i -= 1
  }
  scopes[scopes.size() - 1][name] = v
}

fn binop(op, a, b) {
  match op {
    '+' => a + b,
    '-' => a - b,
    '*' => a * b,
    '/' => a / b,
    '%' => a % b,
    '==' => a == b,
    '!=' => a != b,
    '<' => a < b,
    '<=' => a <= b,
    '>' => a > b,
    _ => a >= b,
  }
}

fn run(src) {
  let tree = lang.parse(src, 'program')
  mut fns = {}
  mut out = []

  fn eval(n, scopes) {
    match n {
      {name: 'Number', token} => to_long(token),
      {name: 'Ident', token} => lookup(scopes, token),
      {name: 'Neg', nodes: [x]} => -eval(x, scopes),
      {name: 'Call', nodes: [f, args]} => call(f.token, args.nodes.map(|arg| eval(arg, scopes))),
      {nodes: [a, op, b]} => binop(op.token, eval(a, scopes), eval(b, scopes)),
    }
  }

  fn call(name, args) {
    if name == 'print' {
      out.push(args[0])
      return nil
    }
    if !fns.has(name) {
      throw "undefined function '{name}'"
    }
    let f = fns[name]
    mut frame = {}
    mut i = 0
    while i < f.params.size() {
      frame[f.params[i]] = args[i]
      i += 1
    }
    match exec_block(f.body, [frame]) {
      {ret} => ret,
      _ => nil,
    }
  }

  fn exec_block(block, scopes) {
    for st in block.nodes {
      let r = exec(st, scopes)
      if r != nil {
        return r
      }
    }
    nil
  }

  fn exec(n, scopes) {
    match n {
      {name: 'Fn', nodes: [name, params, body]} => {
        fns[name.token] = {params: params.nodes.map(|prm| prm.token), body: body}
        nil
      },
      {name: 'Assign', nodes: [name, e]} => {
        store(scopes, name.token, eval(e, scopes))
        nil
      },
      {name: 'Return', nodes: [e]} => {ret: eval(e, scopes)},
      {name: 'If', nodes} => {
        if eval(nodes[0], scopes) {
          exec_block(nodes[1], scopes)
        } else if nodes.size() > 2 {
          exec_block(nodes[2], scopes)
        } else {
          nil
        }
      },
      {name: 'While', nodes: [cond, body]} => {
        mut r = nil
        while r == nil && eval(cond, scopes) {
          r = exec_block(body, scopes)
        }
        r
      },
      {name: 'ExprStmt', nodes: [e]} => {
        eval(e, scopes)
        nil
      },
    }
  }

  exec_block(tree, [{}])
  out
}

inspect(run('
  # 素朴な再帰と、それを使うループ
  fn fib(n) {
    if n < 2 { return n; }
    return fib(n - 1) + fib(n - 2);
  }

  i = 0;
  while i < 10 {
    print(fib(i));
    i = i + 1;
  }
'))
# => [0, 1, 1, 2, 3, 5, 8, 13, 21, 34]
```

この中には、言語の文法を書くたびに出てくる判断が4つ入っています。名前を
付けておくと、次に自分で書くときに思い出しやすいはずです。

**キーワードは識別子から除外する必要があります。** `Ident <- < !Keyword
[a-z_] [a-zA-Z0-9_]* >`の先読みがないと、`Primary <- ... / Ident`が
`else`を変数名として受け付けてしまいます。止まるべき解析が、間違った形の
まま成功してしまうわけです。`Keyword`の末尾を`![a-zA-Z0-9_]`にしてあるのは、
`iffy`のような名前を普通の識別子のままにしておくためです。

**規則の名前そのものが情報を持つところに`no_ast_opt`を置きます。** 子が
1つだけのノードは、その子で置き換えられます。これはほとんどの場所で
ありがたい挙動で、木を帳簿ではなく構造の話に保ってくれます。

困るのは、規則そのものが情報になっている場合です。文が1つだけの`Block`も
`Block`のままでいてほしいですし、`Return`が返す式に潰れてしまっては困り
ます。子を位置で読む親を持つ規則も同じで、畳まれると位置がずれてしまい
ます。

**文が返すのは値ではなく合図です。** `exec`は「続行」なら`nil`を、
「`return`が起きた」ならオブジェクトを返し、`exec_block`は最初の非`nil`で
止まります。ここでの非局所脱出はこれだけです。`break`のある言語なら、
同じ経路に別の形を1つ足すことになります。

**スコープは配列にして、後ろから探します。** `store`は既存の束縛が
見つかればそこへ代入し、見つからなければいちばん内側のスコープに作ります。
関数呼び出しは、呼び出し側の内側に入れ子にするのではなく、新しいフレームを
積みます。再帰が動くのはそのおかげです。

ここから拡張していく場合、作業のほとんどは文法の側になります。文字列を
足すなら`Str`規則と`eval`の分岐が1つずつ、`else if`の連鎖は
`Else <- 'else' (If / Block)`にすれば自然に出てきます。第一級関数にする
には、`call`が名前ではなく値を受け取るようにします。

## 4. 落とし穴

最後に、先に知っておくと半日を節約できるものを並べておきます。

**左再帰はここでは書けますが、これは拡張です。** `Expr <- Expr AddOp
Term / Term`は問題なく動きますし、別の規則を経由した間接的な左再帰も
通ります。木も、望みどおり左に入れ子になります（§2）。ただし、これは
本来のPEGではないことを覚えておいてください。もともとのPEGでは、何も
消費しないまま自分に戻る規則は無限にループしますし、多くの実装はいまも
そのままです。他所へ持っていく文法なら、`Expr <- Term (AddOp Term)*`の
形に書き直す必要があるかもしれません。

**順序付き選択は「どちらか」ではありません。** `'a' / 'ab'`は`ab`に決して
一致しません。長い選択肢と長いリテラルを先に置いてください。

**繰り返しは、自分の中に戻って再試行しません。** `[a-z]* 'x'`は`abx`で
失敗します。`*`がすでに`x`まで取ってしまっているからです。先読みで意図を
書きましょう。`(!'x' [a-z])* 'x'`とすれば通ります。

**歩く側を疑う前に、AST最適化が何をしたかを見てください。**
`Peg.str(node)`は`peglint --ast`と同じ形で木を表示します。どのノードが
畳まれたかを頭の中で考えるより、たいてい速く済みます。

```culebra
let g = `
  Wrap  <- Inner
  Inner <- < [0-9]+ >
`

print(Peg.str(Peg.parse(g, '1')))
# => |
# - Inner (1)
```

**測っていないなら、packratは切らないでください。** 選択肢が接頭辞を
共有する文法（`A <- B '+' A / B`。最初に書く文法は、たいていこの形に
なります）は、その接頭辞を選択肢の数だけ解析し直します。メモ化がないと
指数時間です。リファレンスの実測では、10段の入れ子がメモ化ありで0.04ms、
なしで2.0秒でした。

**1度コンパイルして、何度も解析しましょう。** 高いのは`Peg.compile`の
ほうです。一発形の`Peg.parse(grammar, text)`もスレッドごとに読み込み済みの
文法をキャッシュするので罠ではありませんが、多数の入力を回すループでは、
文法を外に出したほうが読みやすくなります。

## この先

* [`stdlib.ja.md` §34](../stdlib.ja.md#34-peg) — 全呼び出し、ノードの
  フィールド、エラーと深さの上限。
* [cpp-peglibの構文リファレンス](https://github.com/yhirose/cpp-peglib#syntax)
  — このガイドで使わなかった部分も含めた、記法の全体。
* [Practical PEG parsing](https://berthub.eu/articles/posts/practical-peg-parsing/)
  by Bert Hubert — 同じ主張をC++から見たものです。Prometheusのメトリクスを
  解析する実物が載っています。文法を使う価値があるかどうかの判定として、
  彼が出している課題は公平だと思います。手書きの版を書いてみて、時間を
  計ってみてください。
