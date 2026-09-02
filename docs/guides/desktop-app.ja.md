# デスクトップアプリを作る

culebraの`Webview`・`Desktop`名前空間を使って、小さなデスクトップアプリを
作る手順を紹介します。ネイティブウィンドウ、素のHTML/CSS/JSで書いたUI、
そしてローカルHTTPサーバの向こう側にいるculebraバックエンド。これらが全部
まとめて1つのバイナリになります。

[`examples/webview/`](../../examples/webview/)にあるのと同じアプリを、
1ステップずつ組み立てていきます。先に完成形を読みたい方は、そちらの
ファイルを直接実行してみてください。

このガイドが扱うのは`Desktop`/`Webview`の**使い方**です。APIリファレンスは
[`stdlib.ja.md` §29](../stdlib.ja.md#29-desktop--webview)に、内部の仕組み
（loopback bridge、`Embed.dir`のdev/AOT切り替え、プラットフォームごとの
ビルド要件、Ubuntuのsandbox注意点の全文）は
[`examples/webview/README.md`](../../examples/webview/README.md)にあります。

目次
----

- パート1 — 通し実装
  1. [ウィンドウを1行で](#1-ウィンドウを1行で)
  2. [自前のフロントエンドを配信する](#2-自前のフロントエンドを配信する)
  3. [APIルートを追加する](#3-apiルートを追加する)
  4. [JavaScriptから呼び出す](#4-javascriptから呼び出す)
  5. [ページからウィンドウを閉じる](#5-ページからウィンドウを閉じる)
  6. [単一バイナリとして配布する](#6-単一バイナリとして配布する)
- パート2 — レシピ
  - [固定ポートを選ぶ](#固定ポートを選ぶ)
  - [ワーカー間で安全に状態を持つ](#ワーカー間で安全に状態を持つ)
  - [1つのウィンドウで複数ページを扱う](#1つのウィンドウで複数ページを扱う)
  - [Ubuntuでウィンドウが開かない](#ubuntuでウィンドウが開かない)
- [次に読むもの](#次に読むもの)

## パート1 — 通し実装

### 1. ウィンドウを1行で

生のバインディングは`Webview.Window`です。作って、表示するものを与えて、
イベントループを回すだけです。

```culebra
# doctest: skip
let w = Webview.Window.new()
w.set_title("Hello")
w.set_size(640, 480)
w.set_html("<h1>It works</h1>")
w.run()
```

`run()`は、呼び出したスレッドをウィンドウが閉じるまでブロックします。
GUIスレッドの仕事はこれで全部です。`set_html`はHTML文字列リテラルを
そのまま受け取るので、この版にはサーバも別ファイルも出てきません。動く
完成形は[`examples/webview/hello.cul`](../../examples/webview/hello.cul)に
あります。

実際のアプリでは、HTML/CSS/JSはファイルから配信することになります。それを
やってくれるのが、次に出てくる`Desktop.run`です。

### 2. 自前のフロントエンドを配信する

`Desktop.run`は`Webview.Window`の上に建てた窓口です。ローカルHTTPサーバを
起動し、そこへウィンドウを向けて、ウィンドウが閉じるまでブロックします。
`assets:`にディレクトリを渡すと、その中身が`/`で配信されます。

```culebra
# doctest: skip
Desktop.run({title: "My App", size: [720, 560], assets: Embed.dir("dist")})
```

`Embed.dir("dist")`は、コードを変えないまま、**バックエンドごとに**
解決先が変わります。ソースから実行したときは、エントリスクリプトの隣に
あるディスク上のディレクトリをそのまま読みます。`dist/index.html`を編集して
ウィンドウを再読み込みすれば、すぐ反映されます。`culebra build`のほうは
ビルド時にそのディレクトリを歩いて、バイト列を実行ファイルへ焼き込みます。
後者の詳しい話は[§6](#6-単一バイナリとして配布する)にあります。

`dist/`のレイアウトは、普通の静的サイトと同じで大丈夫です。

```
dist/
  index.html
  style.css
  app.js
```

[`examples/webview/dist/`](../../examples/webview/dist/)に、このガイドで
組み立てる最終形の3ファイルが置いてあります。

### 3. APIルートを追加する

`routes:`に渡すクロージャは、ウィンドウが開く前に、アプリ自身の
エンドポイントを`Http`サーバへ登録します（[§15](../stdlib.ja.md#15-http)が
説明しているものと同じサーバです）。

```culebra
# doctest: skip
Desktop.run({assets: Embed.dir("dist"), routes: fn (srv) {
  srv.get("/api/hello", fn (req) {
    {
      content_type: "application/json",
      body: JSON.stringify({message: "Hello from culebra's local server"}),
    }
  })
  srv.post("/api/echo", fn (req) {
    let input = JSON.parse(req.body)
    {
      content_type: "application/json",
      body: JSON.stringify({reply: "You said: " + input["text"]}),
    }
  })
}})
```

これは[`examples/webview/desktop_app.cul`](../../examples/webview/desktop_app.cul)
と同じ形です。固定メッセージを返す`GET`と、ページが送ってきたものを
そのまま返す`POST`の2本です。実ファイルにはもう1本、訪問回数を数えて
保存しておくルートもあります。そちらは下のレシピ
[「ワーカー間で安全に状態を持つ」](#ワーカー間で安全に状態を持つ)で扱います。

ハンドラの戻り値がそのままレスポンスになる規則は、素の`Http.server()`と
同じです。`String`を返せば`200 text/plain`になりますし、それ以外は
`content_type`/`body`/`status`/`headers`を持つ`Object`で自由に決められます。

### 4. JavaScriptから呼び出す

ページはただのWebページなので、他のHTTPバックエンドを相手にするときと
同じように、`fetch`でAPIへ届きます。

```js
async function load() {
  const r = await fetch("/api/hello");
  const d = await r.json();
  document.getElementById("msg").textContent = d.message;
}

async function send() {
  const text = document.getElementById("text").value;
  const r = await fetch("/api/echo", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ text: text })
  });
  const d = await r.json();
  document.getElementById("reply").textContent = d.reply;
}
```

これが、[§3](#3-apiルートを追加する)で作った2つのルートを呼んでいる
`dist/app.js`です。ネイティブなJS↔culebraブリッジのようなものは、
一切あいだに入っていません。ページは`127.0.0.1:PORT`へ普通のHTTPで
話しかけているだけで、どこかのリモートサーバを相手にするのと変わりません。

ですから、普段のWeb開発の作法もそのまま持ち込めます。dev tools、`fetch`、
相対URL、ブラウザキャッシュ。どれもいつもどおりに使えます。

### 5. ページからウィンドウを閉じる

`Desktop.run`で作ったアプリには`POST /__quit`が自動で登録されます。ですから
ページは、ウィンドウ枠の閉じるボタンに頼らなくても、同じHTTPの経路を通って
アプリを終了させられます。

```js
document.getElementById("quit").addEventListener("click", () => {
  fetch("/__quit", { method: "POST" });
});
```

「閉じてよいタイミング」をページ側で**決めたい**こともあります。たとえば
確認ダイアログを先に出したい場合です。そういうときのために、ネイティブ側は
閉じるボタンの操作をそのまま通す前に、ページの`window`グローバルにある
決まった名前のプロパティを2つ探しにいきます。

```js
document.getElementById("quit").addEventListener("click", requestQuit);

// 定義されていれば、ネイティブランタイムは即座に閉じる代わりにこれを呼ぶ。
// 未定義ならフレームの閉じるボタンはそのままウィンドウを閉じる。
window.__culebra_before_close__ = requestQuit;

async function requestQuit() {
  const ok = await confirmDialog("Would you like to quit?");
  if (!ok) return;
  window.__culebra_close__();
}
```

`__culebra_close__()`は、ネイティブウィンドウに「実際に閉じてよい」と
伝える関数です。`__culebra_before_close__`のほうは、その呼び出しの手前に
ページが割り込むためのフックで、定義するかどうかは任意です。

`examples/webview/dist/app.js`に完全版が入っています。ページ内の
`confirmDialog`も一緒です（`window.confirm()`ではなく、アプリ自身の見た目に
合わせた、`Promise`を返すだけの小さなヘルパーです）。正確な呼び出し順は
stdlibリファレンスの
[「ページの`window`オブジェクト」](../stdlib.ja.md#ページのwindowオブジェクト)
にあります。

### 6. 単一バイナリとして配布する

`culebra build`は、culebraバックエンドと焼き込んだ`dist/`のアセットを
まとめて、1つの実行ファイルにコンパイルします。

```sh
culebra build examples/webview/desktop_app.cul -o app
# culebra build: embedded 3 file(s) (...) from 'dist'
./app
```

できたバイナリの隣に`dist/`を置く必要はありません。どこへコピーしても
動きます。`culebra build`はWebViewフレームワークをリンクするかどうかも、
プログラムが実際に`Webview`/`Desktop`を参照しているかどうかで決めるので、
どちらも使わないプログラムには何もリンクされません。

開発中は毎回ビルドしなくてかまいません。完成したアプリをそのまま実行
できます。

```sh
culebra examples/webview/desktop_app.cul          # バイトコードVM (既定)
culebra --jit examples/webview/desktop_app.cul    # 同じ出力、JIT経由
```

## パート2 — レシピ

### 固定ポートを選ぶ

既定では、`Desktop.run`はまずポート`8731`を試して、塞がっていればOSが
割り当てた空きポートに移ります。culebraのデスクトップアプリを2つ並べて
動かすときには便利な挙動です。

ただしこれは、ページのorigin、つまり`localStorage`の置き場所が、実行の
たびに変わりうるということでもあります。`port:`を渡すと固定できますし、
そのポートが空いていなければ、黙って別のポートに移らずにはっきり失敗して
くれるようになります。

```culebra
# doctest: skip
Desktop.run({
  assets: Embed.dir("dist"),
  port: 5173,
  routes: fn (srv) {
    # ...
  },
})
```

### ワーカー間で安全に状態を持つ

`Desktop.run`のサーバは、ワーカープール（`workers:`、既定は`4`）で
リクエストを捌きます。そして**ワーカーはそれぞれ自分のランタイムと自分の
ヒープを持ちます**。

ルートハンドラがSendableでなければならないのは、これが理由です。外側の
可変変数や、Sendableでないハンドル（`SQLite`の接続も含みます）は捕獲でき
ません。[§15](../stdlib.ja.md#15-http)と
[§12](../stdlib.ja.md#12-isolate)が`Http.server()`や`Isolate.spawn()`に
ついて説明しているのと同じ規則です。1つの`db`ハンドルをハンドラ間で
共有しようとすると、リクエストが1本も走らないうちに`SendError`が飛びます。

直し方も同じで、リソースを捕獲せずに、必要になったハンドラの中で毎回
開き直します。ファイルに置いた小さなストアなら、リクエストごとに開く
コストは気にしなくて大丈夫です。

```culebra
let db = SQLite.open(":memory:")
db.execute("CREATE TABLE notes (id INTEGER PRIMARY KEY, text TEXT)")
db.execute("INSERT INTO notes (text) VALUES (?)", ["buy milk"])
db.execute("INSERT INTO notes (text) VALUES (?)", ["walk the dog"])

let rows = db.query("SELECT id, text FROM notes ORDER BY id")
inspect(rows[0]["text"])  # => 'buy milk'
inspect(rows.size())      # => 2

db.execute("DELETE FROM notes WHERE id = ?", [rows[0]["id"]])
inspect(db.query("SELECT text FROM notes")[0]["text"])  # => 'walk the dog'
db.close()
```

ルートの中でも、やることは同じ3行です。`SQLite.open`して、作業して、
`close()`します。最後のものは、ハンドラの末尾でスコープを抜けるのに任せても
かまいません。上のメモリ上のデータベースを実ファイルに置き換えるだけで
成り立ちます。`examples/webview/desktop_app.cul`はまさにこれを訪問回数の
カウンタに使っていて、`/api/hello`・`/api/echo`と並ぶ3本目のルートに
なっています。

```culebra
# doctest: skip
srv.post("/api/visit", fn (req) {
  let db = SQLite.open(VISITS_DB)
  db.execute("CREATE TABLE IF NOT EXISTS visits (id INTEGER PRIMARY KEY, n INTEGER)")
  let rows = db.query("SELECT n FROM visits WHERE id = 1")
  let n = if rows.size() == 0 {
    0
  } else {
    rows[0]["n"]
  } + 1
  db.execute("INSERT OR REPLACE INTO visits (id, n) VALUES (1, ?)", [n])
  db.close()
  {content_type: "application/json", body: JSON.stringify({visits: n})}
})
```

`dist/app.js`の`trackVisit()`が読み込み時にこれを呼んで、結果を表示します。
`culebra examples/webview/desktop_app.cul`を実行してウィンドウを閉じ、もう
一度実行してみてください。カウントは続きから始まります。数はプロセスでは
なく、アプリを実行した場所の隣にある`visits.db`に入っているからです。

同時に書き込もうとしたときの順番待ちは、SQLite自身のファイルロックが
引き受けてくれます。ハンドルを捕獲しないこと以外に、culebra側で気を配る
ことはありません。

### 1つのウィンドウで複数ページを扱う

`assets:`はディレクトリ全体を配信するので、HTMLファイルが複数あっても、
静的サイトとまったく同じように振る舞います。culebra側のコードは要りません。
リンクを1本書くだけです。[`examples/webview/dist/`](../../examples/webview/dist/)の
`dist/about.html`が2枚目のページで、`dist/index.html`からは普通のやり方で
そこへ行けます。

```html
<p class="nav"><a href="about.html">About this app</a></p>
```

これだけで済みます。サーバはディレクトリ全体を配信しているので、
`GET /about.html`にはもう答えられる状態です。

クリックを介さずにプログラムから遷移させたい場合は、別の層の話になります。
[§1](#1-ウィンドウを1行で)で出てきた生のウィンドウハンドルに対する
`Webview.Window.navigate(url)`がそれで、`Desktop.run`はこれを`routes:`には
公開していません。複数ウィンドウのAPIもまだありません。1回の
`Desktop.run`呼び出しは、プロセスが生きているあいだ1つのウィンドウに
対応します。
[examples/webview/README.mdの「Not here yet」](../../examples/webview/README.md#not-here-yet)
に一覧があります。

### Ubuntuでウィンドウが開かない

Ubuntu 23.10以降で`webview: failed to create window`（あるいは
`bwrap`や`RTM_NEWADDR`まわりのクラッシュ）が出ることがあります。原因は
culebraではなく、WebKitGTK自身のsandboxが必要とする非特権user namespaceに、
AppArmorの制限がかかっていることです。WebKitGTKを埋め込むアプリは、どれも
同じ症状に当たります。

新しめのUbuntu（24.04.2以降）は、修正済みのプロファイルを最初から持って
います。古いイメージや、削ぎ落とされたイメージの場合は、次のどちらかを
してください。

```sh
sudo apt install --reinstall bubblewrap apparmor
```

あるいは、自分のバイナリ1つにだけnamespaceを許可する方法もあります。
正確なプロファイルは
[examples/webview/README.mdのAppArmorの節](../../examples/webview/README.md#the-sandbox-on-ubuntu-family-systems-apparmor)
にあります。

これはOSのポリシーの性質であって、アプリ側の問題ではありません。Debian、
Fedora、Arch、それにUbuntu由来のパッチが入っていないカーネルでは、どれも
この対応は要りません。

## 次に読むもの

- [`stdlib.ja.md` §29](../stdlib.ja.md#29-desktop--webview) — `Desktop`/`Webview`
  APIの完全なリファレンス。
- [`examples/webview/README.md`](../../examples/webview/README.md) —
  loopback bridge・単一バイナリ化・プラットフォームごとのビルド要件が
  内部でどう動くか、既知のWebView側の癖。
- [`deployment.ja.md` §1](../deployment.ja.md#1-standalone-バイナリビルドculebra-build) —
  `culebra build`のフラグ、クロスコンパイル、バイナリが何をリンクするかを絞る方法。
- [`stdlib.ja.md` §15](../stdlib.ja.md#15-http) — `Http`サーバの完全なAPI
  （`routes:`の中身は素の`Http.server()`そのもの）。
