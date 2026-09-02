# PEGで解析する

`Peg`名前空間の使い方を、順を追って紹介します。正規表現では手が届かない
ところで文法を書く方法、自分用の小さな言語を設計する方法、そしてその
インタプリタを書く方法までを扱います。

このガイドに出てくるコードブロックは、すべて実際に動きます。doctestとして
両方のエンジンで実行されているので、書きっぱなしのものは1つもありません。

APIのリファレンスは[`stdlib.ja.md` §34](../stdlib.ja.md#34-peg)にあります。
あちらは「各呼び出しが何をするか」を引くための文書で、こちらは
「どういうときに使うか」「文法をどう組み立てるか」のための文書です。

## なぜ文法を書くのでしょうか

Bert Hubertが[Practical PEG parsing](https://berthub.eu/articles/posts/practical-peg-parsing/)
という記事で、PEGの立ち位置をうまく言い表しています。簡単なことは簡単に、
難しいことも可能に、というのがそれです。

正規表現は、簡単な側ではとても優秀です。ただ、難しい側には答えを持って
いません。難しい側というのは構造が出てくるところ、つまり入れ子や、
エスケープや、「このトークン、ただしあのトークンの中にあるものは除く」
といった話です。そこで手書きのスキャナを書きはじめて、気がつくと半日
たっている、というのがよくある展開ではないでしょうか。

PEGはその中間にあります。小さな仕事なら正規表現と同じくらいの手間で
書けますし、正規表現と違って、仕事が育っても壊れません。効いているのは
次の3つの性質です。

* **規則どうしを組み合わせられます。** 規則は他の規則を参照できますし、
  自分自身を参照することもできます。入れ子が扱えるのはこのおかげで、
  正規表現に根本的に欠けているのもここです。
* **選択に順序があります。** `a / b`は、まず`a`を試して、失敗したときだけ
  `b`を試します。解決すべき曖昧さがそもそも生まれません。曖昧さがないと
  いうことは、文法が書いたとおりの意味を、書いた順序どおりに持つという
  ことです。
* **木が手に入ります。** culebraでは木が素の`Object`なので、`match`が
  そのまま分解できます。あいだにvisitor APIのようなものを挟みません。

エンジンには[cpp-peglib](https://github.com/yhirose/cpp-peglib)を使って
います。culebra自身のフロントエンドが載っているのと同じパーサです。

## 1. 正規表現にはできない検索と置換

まずは、正規表現ではそもそも解けないか、解けたとしても誰も保守したく
ない形になってしまう仕事から見ていきます。

解き方の形は、実はどれも同じです。**関心のない部分も含めてテキスト全体を
文法で書き**、そのあとで断片を順に見ていきます。ここを押さえておくと、
以下の5つが全部同じ話に見えてくるはずです。

### 1.1 文字列とコメントの中は避けて名前を変える

いちばんよくある置換から始めましょう。正規表現には、見つけた一致が
文字列リテラルの中にあるのかどうかを知る手段がありません。ですから
`s/total/sum/g`は、データもコメントも一緒に壊してしまいます。

一方、文法のほうは知っています。文字列とコメントがそれぞれ独立した規則に
なっていて、順序付き選択がそれを先に試すからです。

```culebra
let g = `
  Doc     <- Chunk*                        { no_ast_opt }
  Chunk   <- Str / Comment / Ident / Other
  Str     <- < '"' ( '\\' . / !'"' . )* '"' >
  Comment <- < '#' (!'\n' .)* >
  Ident   <- < [a-zA-Z_] [a-zA-Z0-9_]* >
  Other   <- < . >
`
fn rename(src, from, to) {
  mut out = ''
  for n in Peg.parse(g, src).nodes {
    out = out + (n.name == 'Ident' && n.token == from ? to : n.token)
  }
  out
}
let src = 'total = total + 1  # total, コメントの中
label = "total, 文字列の中"'
println(rename(src, 'total', 'sum'))
# => |
# sum = sum + 1  # total, コメントの中
# label = "total, 文字列の中"
```

これを成り立たせている立役者は、地味ですが`Other <- < . >`です。この規則が
あるおかげで、入力のすべてのバイトがどれかの断片に属します。だから
トークンを繋ぎ直すと、変えたところ以外は入力がそのまま戻ってきます。
関心のある部分だけを書いた文法だと、残りの隙間を運ぶ仕組みを別に用意する
ことになってしまいます。

### 1.2 入れ子 — 正規表現には数えられません

入れ子になるブロックコメントは、正規表現の限界を示す定番の例です。
`/\*.*?\*/`は最初の`*/`で止まってしまいますし、`/\*.*\*/`は最後の`*/`まで
飲み込んでしまいます。どちらも正しくありませんし、工夫を重ねても直り
ません。数を数えることが、そもそも正規言語の外側にあるからです。

自分自身を参照できる規則には、その制限がありません。

```culebra
let g = `
  Doc     <- Piece*                { no_ast_opt }
  Piece   <- Comment / Text
  Comment <- '/*' ( Comment / !'*/' . )* '*/'
  Text    <- < ( !'/*' . )+ >
`
fn strip(src) {
  mut out = ''
  for n in Peg.parse(g, src).nodes {
    if n.name == 'Text' {
      out = out + n.token
    }
  }
  out
}
println(strip('a /* one /* nested */ still a comment */ b'))  # => a  b
```

`Comment <- '/*' ( Comment / !'*/' . )* '*/'`という行は、定義をそのまま
読み下せます。開き記号があって、そのあとに「入れ子のコメント1つ」か
「閉じ記号を始めない文字」のどちらかが好きなだけ続いて、最後に閉じ記号、
というだけです。

### 1.3 エスケープ

正規表現が脆くなる、もう1つの場所です。`"([^"]*)"`は`"a\"b"`で壊れます。
直した版は、書けたとしても読み返せないものになりがちです。

文法にすると、口で説明するときと同じ順序で並んだ2つの選択肢になります。
バックスラッシュなら次の1文字を連れていく、そうでなければ閉じ引用符でない
任意の文字を取る、というだけです。

```culebra
let g = `
  Str  <- '"' < Char* > '"'
  Char <- '\\' . / !'"' .
`
println(Peg.compile(g).parse('"a\"b\\c"').token)  # => a\"b\\c
```

ここでは、この2つの並び順が肝心です。逆にすると`!'"' .`のほうが単独で
バックスラッシュを消費してしまい、次の文字が普通の文字として読まれて、
エスケープしたはずの引用符で文字列が途中で終わってしまいます。Hubertが
「順序付き選択は形式ではなく意味を持つ」と書いているのは、まさにこういう
ことです。

### 1.4 規則を組み合わせる — 文字列を知っている括弧の釣り合い

「規則どうしを組み合わせられる」ことが何を買ってくれるのか、という例を
見てみましょう。

呼び出しから引数リストを取り出すには、括弧の釣り合いを取る必要があります。
この時点で正規表現には無理なのですが、さらに実際の入力には、文字列
リテラルの中の括弧が出てきます。そちらは数に入れてはいけません。

この追加は、1つの規則に選択肢を1つ足すだけで済みます。

```culebra
let g = `
  Call     <- Name '(' Args ')'
  Name     <- < [a-zA-Z_] [a-zA-Z0-9_]* >
  Args     <- < Balanced >
  Balanced <- ( Str / '(' Balanced ')' / !')' . )*
  Str      <- '"' ( '\\' . / !'"' . )* '"'
`
let node = Peg.compile(g).parse('f(g(1, h(2)), "x)y")')
println(node.nodes[0].token)  # => f
println(node.nodes[1].token)  # => g(1, h(2)), "x)y"
```

`"x)y"`の中の`)`は、何も閉じていません。`Str`のほうが先に試されて、
リテラル全体を1手で消費してしまうからです。

### 1.5 構造を見た書き換え

ここまでを組み合わせると、単なるテキスト置換ではない書き換えができる
ようになります。次の例は`f(a, b)`を`a.f(b)`に変えます。引数が入れ子の
呼び出しでもかまいませんし、文字列の中にカンマや括弧が入っていても
大丈夫です。

```culebra
let g = `
  Doc    <- Piece*                          { no_ast_opt }
  Piece  <- Call / Str / Other
  Call   <- Name '(' Arg (',' Arg)* ')'
  Name   <- < [a-zA-Z_] [a-zA-Z0-9_]* >
  Arg    <- < ( Str / '(' Nested ')' / [^,()] )+ >
  Nested <- ( Str / '(' Nested ')' / [^()] )*
  Str    <- < '"' ( '\\' . / !'"' . )* '"' >
  Other  <- < . >
`
fn to_ufcs(src) {
  mut out = ''
  for n in Peg.parse(g, src).nodes {
    match n {
      {name: 'Call', nodes} => {
        let args = nodes.slice(1, nodes.size()).map(|x| x.token.trim())
        let rest = args.slice(1, args.size()).join(', ')
        out = out + "{args[0]}.{nodes[0].token}({rest})"
      },
      _ => out = out + n.token,
    }
  }
  out
}
println(to_ufcs('log(fmt("a,b", x), 2) and keep("f(1, 2)")'))
# => fmt("a,b", x).log(2) and "f(1, 2)".keep()
```

書き換え**られなかった**ほうにも目を向けてみてください。文字列の中の
`f(1, 2)`はそのまま残っています。この条件を正規表現で書こうとすると、
どうなるでしょうか。

なお、木を組み直すのではなく元のテキストを直接編集したい場合もあると
思います。そのときは、各ノードが`position`と`length`を持っているので、
`src.slice(n.position, n.position + n.length)`がそのノードの原文に
なります。その前後を繋げば大丈夫です。

## 2. 四則計算 — 優先順位と、評価の2通り

四則計算は、本格的な文法設計が必要になる最小の題材です。優先順位を、
規則の形そのものから出さなければならないからです。

定石は、優先順位1段につき規則を1つ作り、結合の弱いほうが強いほうを参照
する形にすることです。

```culebra
let calc = `
  Expr   <- Term (AddOp Term)*
  Term   <- Factor (MulOp Factor)*
  Factor <- Number / '(' Expr ')'
  AddOp  <- < '+' / '-' >
  MulOp  <- < '*' / '/' >
  Number <- < '-'? [0-9]+ >
  %whitespace <- [ \t\r\n]*
`
fn eval(n) {
  match n {
    {name: 'Number', token} => to_long(token),
    {nodes} => {
      mut acc = eval(nodes[0])
      mut i = 1
      while i < nodes.size() {
        let r = eval(nodes[i + 1])
        acc = match nodes[i].token {
          '+' => acc + r,
          '-' => acc - r,
          '*' => acc * r,
          _ => acc / r,
        }
        i += 2
      }
      acc
    },
  }
}
inspect(eval(Peg.parse(calc, '1 + 2 * 3')))    # => 7
inspect(eval(Peg.parse(calc, '(1 + 2) * 3')))  # => 9
```

`Expr <- Term (AddOp Term)*`が作るのは、`[Term, op, Term, op, Term]`と
いう平らな並びです。評価器が2つ飛ばしで歩いて左から畳んでいるのは、
そのためです。

この結合方向は、実は自分で選んでいます。規則を`Term AddOp Expr`と書けば
右に入れ子になり、`1 - 2 - 3`の答えが`-4`ではなく`2`になります。意図した
ほうを選んでください。

`%whitespace`は、文法を読める形に保ってくれる唯一の糖衣構文です。これが
ないと、どの規則にも空白の入りうる位置を書き並べることになってしまいます。

### 同じ文法を、木を作らずに使う

答えだけが欲しいときは、`actions`を使うと入力を直接解釈できます。規則名
から関数へのマップを渡す形で、各関数はその規則の畳み込みを受け取り、その
規則が親に渡す値を返します。

```culebra
let calc = `
  Expr   <- Term (AddOp Term)*
  Term   <- Factor (MulOp Factor)*
  Factor <- Number / '(' Expr ')'
  AddOp  <- < '+' / '-' >
  MulOp  <- < '*' / '/' >
  Number <- < '-'? [0-9]+ >
  %whitespace <- [ \t\r\n]*
`
fn fold(sv) {
  mut acc = sv.values[0]
  mut i = 1
  while i < sv.values.size() {
    let r = sv.values[i + 1]
    acc = match sv.values[i] {
      '+' => acc + r,
      '-' => acc - r,
      '*' => acc * r,
      _ => acc / r,
    }
    i += 2
  }
  acc
}
let actions = {
  Number: |sv| to_long(sv.token),
  AddOp: |sv| sv.token,
  MulOp: |sv| sv.token,
  Expr: fold,
  Term: fold,
}
inspect(Peg.parse(calc, '1 + 2 * 3 - 4', actions: actions))  # => 3
```

使い分けの目安はこうです。入力を調べたい、変換したい、何度も歩きたい
ときは木を使ってください。解析すること自体が計算になっていて、木は
作って捨てるだけになるときは、actionsのほうが素直です。

## 3. 小さなDSLを設計する

DSLが割に合うのは、代わりに置くものが「条件分岐の増えていく設定ファイル」
になってしまうときです。ここでは、レコードの一覧を絞り込む言語を作って
みます。検索ボックスの裏側にあるようなものだと思ってください。

設計は、書きたい文を先に並べるところから始めるのがおすすめです。

    status = open and priority >= 2
    priority = 1 or status = open

こう並べてみると、優先順位が読み取れます。`or`がいちばん弱く、次が`and`、
いちばん強いのが比較1つ。上の四則計算と同じ梯子です。

```culebra
let grammar = `
  Query  <- Or
  Or     <- And ('or' And)*
  And    <- Cmp ('and' Cmp)*
  Cmp    <- Field Op Value
  Field  <- < [a-z_]+ >
  Op     <- < '>=' / '<=' / '!=' / '=' / '>' / '<' >
  Value  <- Number / Word
  Number <- < '-'? [0-9]+ >
  Word   <- < [a-zA-Z_] [a-zA-Z0-9_]* >
  %whitespace <- [ \t]*
`
let q = Peg.compile(grammar)

fn compare(op, a, b) {
  match op {
    '=' => a == b,
    '!=' => a != b,
    '<' => a < b,
    '<=' => a <= b,
    '>' => a > b,
    _ => a >= b,
  }
}
fn eval(n, row) {
  match n {
    {name: 'Or', nodes} => nodes.any(|c| eval(c, row)),
    {name: 'And', nodes} => nodes.all(|c| eval(c, row)),
    {name: 'Cmp', nodes: [f, op, v]} => {
      if !row.has(f.token) {
        throw "no field named '{f.token}'"
      }
      compare(op.token, row[f.token], v.name == 'Number' ? to_long(v.token) : v.token)
    },
  }
}

let rows = [
  {title: 'write guide', status: 'open', priority: 3},
  {title: 'fix build', status: 'done', priority: 1},
  {title: 'read paper', status: 'open', priority: 1},
]
fn select(rows, text) {
  let tree = q.parse(text, 'filter')
  rows.filter(|r| eval(tree, r)).map(|r| r.title)
}
inspect(select(rows, 'status = open and priority >= 2'))  # => ['write guide']
inspect(select(rows, 'priority = 1 or status = open'))
# => ['write guide', 'fix build', 'read paper']
```

この文法には、真似する価値のある点が2つあります。

**演算子の並び順が効いています。** `Op <- < '>=' / '<=' / '!=' / '=' /
'>' / '<' >`では、2文字の演算子を先に並べています。順序付き選択は最初に
一致した選択肢を採るので、もし`'>'`を`'>='`より前に置いてしまうと、入力
`>=`は`>`のほうに一致してしまい、残った`=`のところで解析が失敗します。

目安として覚えておくとよいのは、**長いリテラルを、その接頭辞になるものより
前に置く**、という一言です。

**解析するときに、入力に名前を付けましょう。** `q.parse(text, 'filter')`と
書くと、その名前がエラーに入ります。利用者の打ち間違いが、素のオフセット
ではなく、読める診断として返ってくるようになります。

```culebra
let q = Peg.compile(`
  Query  <- Cmp
  Cmp    <- Field Op Value
  Field  <- < [a-z_]+ >
  Op     <- < '>=' / '<=' / '!=' / '=' / '>' / '<' >
  Value  <- < [a-zA-Z0-9_]+ >
  %whitespace <- [ \t]*
`)
inspect(try { q.parse('status =', 'filter') } catch e { e.message })
# => 'filter:1:9: syntax error, expecting <Value>.'
```

## 4. 小さな言語

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
  Cmp      <- Add (CmpOp Add)?
  Add      <- Mul (AddOp Mul)*
  Mul      <- Unary (MulOp Unary)*
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
      {name: 'Cmp', nodes: [a, op, b]} => binop(op.token, eval(a, scopes), eval(b, scopes)),
      {nodes} => fold(nodes, scopes),
    }
  }
  fn fold(nodes, scopes) {
    mut acc = eval(nodes[0], scopes)
    mut i = 1
    while i < nodes.size() {
      acc = binop(nodes[i].token, acc, eval(nodes[i + 1], scopes))
      i += 2
    }
    acc
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

## 5. 落とし穴

最後に、先に知っておくと半日を節約できるものを並べておきます。

**左再帰は書けません。** `Expr <- Expr '+' Term`は停止しません。何も消費
しないうちに`Expr`をもう一度試してしまうからです。代わりに繰り返しで書いて
（`Expr <- Term ('+' Term)*`）、§2のように自分で畳んでください。yacc系の
文法から移ってくると、この習慣だけは通用しません。

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
