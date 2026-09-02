# PEGで解析する

`Peg`名前空間の使い方。正規表現では届かないところで文法を書く方法、
自分用の小さな言語を設計する方法、そのインタプリタを書く方法を扱う。
このガイドのコードブロックはすべて実際に動く。doctestとして両方の
エンジンで実行される。

APIリファレンスは[`stdlib.ja.md` §34](../stdlib.ja.md#34-peg)。あちらは
各呼び出しが何をするかを書いた文書で、こちらは**どういうときに使うか**と
**文法をどう組むか**の文書である。

## なぜ文法を書くのか

Bert Hubertが[Practical PEG parsing](https://berthub.eu/articles/posts/practical-peg-parsing/)
でうまく言っている。簡単なことは簡単に、難しいことも可能に、というのが
PEGの立ち位置だ。正規表現は簡単な側では優秀だが、難しい側には答えを
持っていない。難しい側とは構造が出てくるところ、つまり入れ子、
エスケープ、「このトークン、ただしあのトークンの中にあるものは除く」で
ある。そこで手書きのスキャナを書きはじめて、半日が消える。

PEGはその中間にある。小さな仕事なら正規表現と同じくらいの手間で書け、
正規表現と違って、仕事が育っても壊れない。効いているのは3つの性質だ。

* **規則を組み合わせられる。** 規則は他の規則を参照でき、自分自身も
  参照できる。入れ子が扱えるのはこれのおかげで、正規表現に根本的に
  欠けているのもこれである。
* **選択に順序がある。** `a / b`はまず`a`を試し、失敗したときだけ`b`を
  試す。解決すべき曖昧さが存在しない。曖昧さがないということは、文法は
  書いたとおりの意味を、書いた順序どおりに持つということだ。
* **木が手に入る。** culebraでは木は素の`Object`なので、`match`が
  そのまま分解できる。あいだにvisitor APIを挟まない。

エンジンは[cpp-peglib](https://github.com/yhirose/cpp-peglib)で、culebra
自身のフロントエンドが載っているのと同じパーサである。

## 1. 正規表現にはできない検索と置換

正規表現がそもそも解けないか、解けても誰も保守したくない形になる仕事を
並べる。解き方の形はどれも同じだ。**関心のない部分も含めてテキスト全体を
文法で記述し**、そのあと断片を歩く。

### 1.1 文字列とコメントの中は避けて名前を変える

いちばんよくある置換。正規表現には、その一致が文字列リテラルの中にある
かどうかを知る手段がないので、`s/total/sum/g`はデータもコメントも壊す。
文法は知っている。文字列とコメントがそれぞれ独立した規則で、順序付き
選択がそれを先に試すからだ。

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

これを成立させているのは`Other <- < . >`である。この規則があるおかげで
入力の全バイトがどれかの断片に属し、トークンを繋ぎ直せば、変えた箇所を
除いて入力がそのまま再現される。関心のある部分だけを記述した文法だと、
隙間を運ぶ仕組みを別に用意することになる。

### 1.2 入れ子 — 正規表現には数えられない

入れ子になるブロックコメントは、正規表現の限界を示す定番の例だ。
`/\*.*?\*/`は最初の`*/`で止まり、`/\*.*\*/`は最後の`*/`まで飲み込む。
どちらも正しくないし、工夫しても直らない。数を数えることが正規言語の
外側にあるからだ。自分自身を参照する規則には、その制限がない。

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

`Comment <- '/*' ( Comment / !'*/' . )* '*/'`は、定義をそのまま読み下せる。
開き記号、そのあとに「入れ子のコメント1つ」か「閉じ記号を始めない文字」の
どちらかが任意個、最後に閉じ記号。

### 1.3 エスケープ

正規表現が脆くなるもう1つの場所。`"([^"]*)"`は`"a\"b"`で壊れるし、
直したものは書けても読めない。文法にすると、口で言うとおりの順序で
並んだ2つの選択肢になる。バックスラッシュなら次の1文字を連れていく、
そうでなければ閉じ引用符でない任意の文字を取る。

```culebra
let g = `
  Str  <- '"' < Char* > '"'
  Char <- '\\' . / !'"' .
`
println(Peg.compile(g).parse('"a\"b\\c"').token)  # => a\"b\\c
```

この2つの並び順が肝心である。逆にすると`!'"' .`が単独でバックスラッシュ
を消費し、次の文字が普通の文字として読まれ、エスケープされた引用符で
文字列が途中で終わってしまう。Hubertが「順序付き選択は形式ではなく
意味を持つ」と書いているのは、まさにこのことだ。

### 1.4 規則を組み合わせる — 文字列を知っている括弧の釣り合い

「規則を組み合わせられる」が何を買うかの例。呼び出しから引数リストを
取り出すには括弧の釣り合いが要る。この時点で正規表現には無理だが、
実際の入力には文字列リテラルの中の括弧があり、そちらは数えてはいけない。
その追加は、1つの規則に選択肢を1つ足すだけで済む。

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

`"x)y"`の中の`)`は何も閉じていない。`Str`が先に試され、リテラル全体を
1手で消費するからである。

### 1.5 構造を見た書き換え

ここまでを組み合わせると、テキスト置換ではない書き換えができる。次の例は
`f(a, b)`を`a.f(b)`に変える。引数は入れ子の呼び出しでもよく、文字列の中に
カンマや括弧が入っていてもよい。

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

書き換え**られなかった**ものに注目してほしい。文字列の中の`f(1, 2)`である。
この条件を正規表現で書いてみるとよい。

木を組み直すのではなく元のテキストを編集したいときは、各ノードが
`position`と`length`を持っているので、`src.slice(n.position, n.position +
n.length)`がそのノードの原文になる。その前後を繋げばよい。

## 2. 四則計算 — 優先順位と、評価の2通り

四則計算は、本格的な文法設計が要る最小の仕事である。優先順位を規則の形
から出さなければならないからだ。定石は、優先順位1段につき規則を1つ作り、
結合の弱いほうが強いほうを参照する形にすることだ。

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

`Expr <- Term (AddOp Term)*`が作るのは平らな並び、つまり
`[Term, op, Term, op, Term]`である。評価器が2つ飛ばしで歩いて左から
畳んでいるのはそのためだ。この結合方向は自分で選んでいる。規則を
`Term AddOp Expr`と書けば右に入れ子になり、`1 - 2 - 3`は`-4`ではなく
`2`になる。

`%whitespace`は、文法を読める形に保つ唯一の糖衣構文である。これがないと、
どの規則にも空白の入りうる位置を書き並べることになる。

### 同じ文法を、木を作らずに

答えだけが欲しいときは、`actions`が入力を直接解釈する。規則名から関数への
マップで、各関数はその規則の畳み込みを受け取り、その規則が親に渡す値を
返す。

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

入力を調べたい、変換したい、複数回歩きたいときは木を使う。解析すること
自体が計算であって、木は作って捨てるだけになるときはactionsを使う。

## 3. 小さなDSLを設計する

DSLが割に合うのは、代わりに置くものが条件分岐の増えていく設定ファイルに
なるときだ。ここでは、レコードの一覧に対する絞り込み言語を作る。検索ボックス
の裏側にあるようなものである。

設計は、書きたい文を先に並べるところから始める。

    status = open and priority >= 2
    priority = 1 or status = open

そこから優先順位を読み取る。`or`がいちばん弱く、次が`and`、いちばん強い
のが比較1つ。上の四則計算と同じ梯子だ。

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

この文法から真似する価値のある点が2つある。

**演算子の並び順が効いている。** `Op <- < '>=' / '<=' / '!=' / '=' / '>'
/ '<' >`は2文字の演算子を先に並べている。順序付き選択は最初に一致した
選択肢を採るので、`'>'`を`'>='`より前に置くと、入力`>=`は`>`に一致し、
残った`=`で解析が失敗する。目安はこうだ。**長いリテラルを、その接頭辞に
なるものより前に置く。**

**解析するときに入力に名前を付ける。** `q.parse(text, 'filter')`はその
名前をエラーに入れるので、利用者の打ち間違いが、素のオフセットではなく
診断として返ってくる。

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

ここまでのやり方は、そのまま本物のインタプリタまで届く。以下の言語は
整数の変数、算術、比較、`if`/`else`、`while`、引数つきの関数、そして再帰を
持つ。プログラムと呼べるものが書ける程度には揃っている。文法と評価器を
あわせて130行ほどだ。

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

この中の4つの判断は、言語の文法を書くたびに出てくるので名前を付けて
おきたい。

**キーワードを識別子から除外する必要がある。** `Ident <- < !Keyword
[a-z_] [a-zA-Z0-9_]* >`の先読みがないと、`Primary <- ... / Ident`が
`else`を変数名として受け付けてしまい、止まるべき解析が間違った形で
成功する。`Keyword`の末尾が`![a-zA-Z0-9_]`なのは、`iffy`を普通の識別子の
ままにしておくためだ。

**規則の名前そのものが情報を持つ場所に`no_ast_opt`を置く。** 子が1つ
だけのノードはその子で置き換えられる。これはほとんどの場所で望ましい
挙動で、木を帳簿ではなく構造の話に保ってくれる。望ましくないのは、
規則そのものが情報であるときだ。文が1つだけの`Block`は`Block`のまま
でなければならないし、`Return`が返す式に潰れてはいけない。子を位置で
読む親を持つ規則も同じで、畳まれると位置がずれる。

**文が返すのは値ではなく合図である。** `exec`は「続行」なら`nil`を、
「`return`が起きた」ならオブジェクトを返し、`exec_block`は最初の非`nil`
で止まる。ここでの非局所脱出はこれだけだ。`break`のある言語なら、同じ
経路に別の形を1つ足すことになる。

**スコープは配列で、後ろから探す。** `store`は既存の束縛が見つかれば
そこへ代入し、なければいちばん内側のスコープに作る。関数呼び出しは
呼び出し側の内側に入れ子にするのではなく新しいフレームを積む。再帰が
動くのはそのためだ。

拡張はほとんど文法の仕事になる。文字列は`Str`規則と`eval`の分岐1つ、
`else if`の連鎖は`Else <- 'else' (If / Block)`にすれば自然に出てくる。
第一級関数にするには、`call`が名前ではなく値を受け取るようにする。

## 5. 落とし穴

**左再帰は書けない。** `Expr <- Expr '+' Term`は停止しない。何も消費
しないうちに`Expr`をもう一度試すからだ。代わりに繰り返しで書き
（`Expr <- Term ('+' Term)*`）、§2のように自分で畳む。yacc系の文法から
移ってくると、この習慣だけは通用しない。

**順序付き選択は「どちらか」ではない。** `'a' / 'ab'`は`ab`に決して
一致しない。長い選択肢と長いリテラルを先に置くこと。

**繰り返しは自分の中に戻って再試行しない。** `[a-z]* 'x'`は`abx`で失敗
する。`*`がすでに`x`を取ってしまっているからだ。先読みで意図を書く。
`(!'x' [a-z])* 'x'`。

**歩く側を疑う前に、AST最適化が何をしたか見る。** `Peg.str(node)`は
`peglint --ast`と同じ形で木を出力する。どのノードが畳まれたかを頭の中で
考えるより速い。

```culebra
let g = `
  Wrap  <- Inner
  Inner <- < [0-9]+ >
`
print(Peg.str(Peg.parse(g, '1')))
# => |
# - Inner (1)
```

**測っていないならpackratは切らない。** 選択肢が接頭辞を共有する文法
（`A <- B '+' A / B`、最初に書く文法はたいていこの形になる）は、その
接頭辞を選択肢の数だけ解析し直し、メモ化がないと指数時間になる。
リファレンスの実測で、10段の入れ子がメモ化ありで0.04ms、なしで2.0秒。

**1度コンパイルして、何度も解析する。** 高いのは`Peg.compile`のほうだ。
一発形の`Peg.parse(grammar, text)`はスレッドごとに読み込み済みの文法を
キャッシュするので罠ではないが、多数の入力を回すループは、文法を外に
出したほうが読みやすい。

## この先

* [`stdlib.ja.md` §34](../stdlib.ja.md#34-peg) — 全呼び出し、ノードの
  フィールド、エラーと深さの上限。
* [cpp-peglibの構文リファレンス](https://github.com/yhirose/cpp-peglib#syntax)
  — このガイドで使わなかった部分も含めた記法の全体。
* [Practical PEG parsing](https://berthub.eu/articles/posts/practical-peg-parsing/)
  by Bert Hubert — 同じ主張をC++から見たもの。Prometheusのメトリクスを
  解析する実物が載っている。文法を使う価値があるかどうかの判定として、
  彼の出している課題は公平だ。手書きの版を書いてみて、時間を計ってみると
  よい。
