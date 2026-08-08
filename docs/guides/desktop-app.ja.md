# デスクトップアプリを作る

culebraの`Webview`・`Desktop`名前空間で小さなデスクトップアプリを作る手順。
ネイティブウィンドウ、素のHTML/CSS/JSで書いたUI、ローカルHTTPサーバの
向こう側にいるculebraバックエンド — これら全部が1つのバイナリとして
出荷される。[`examples/webview/`](../../examples/webview/)にある同じ
アプリを1ステップずつ組み立てる。先に完成コードを読みたければそちらの
ファイルを直接実行してもよい。

このガイドが扱うのは`Desktop`/`Webview`の**使い方**。APIリファレンスは
[`stdlib.ja.md` §29](../stdlib.ja.md#29-desktop--webview)、内部の仕組み
（loopback bridge、`Embed.dir`のdev/AOT切り替え、プラットフォームごとの
ビルド要件、Ubuntuのsandbox注意点の全文）は
[`examples/webview/README.md`](../../examples/webview/README.md)を参照。

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

生のバインディングは`Webview.Window`。作って、何か表示するものを与えて、
イベントループを回すだけ。

```culebra
# doctest: skip
let w = Webview.Window.new()
w.set_title("Hello")
w.set_size(640, 480)
w.set_html("<h1>It works</h1>")
w.run()
```

`run()`は呼び出したスレッドをウィンドウが閉じるまでブロックする — それが
GUIスレッドの仕事のすべて。`set_html`はHTML文字列リテラルをそのまま
受け取るので、このバージョンにはサーバも別ファイルもない。完全に動く版は
[`examples/webview/hello.cul`](../../examples/webview/hello.cul)を参照。
実際のアプリはHTML/CSS/JSをファイルから配信する — それが次に出てくる
`Desktop.run`。

### 2. 自前のフロントエンドを配信する

`Desktop.run`は`Webview.Window`の上に建てたfacade: ローカルHTTPサーバを
起動し、そこへウィンドウを向け、ウィンドウが閉じるまでブロックする。
`assets:`にディレクトリを渡せば`/`で配信される。

```culebra
# doctest: skip
Desktop.run({title: "My App", size: [
  720,
  560,
], assets: Embed.dir("dist")})
```

`Embed.dir("dist")`はコード変更なしに**backendごとに**解決先が変わる:
ソースから実行するとエントリスクリプトの隣にあるディレクトリを実ディスクから
生きたまま読む（`dist/index.html`を編集してウィンドウをreloadすれば
そのまま反映される）。`culebra build`はビルド時にそのディレクトリを
walkし、バイト列を実行ファイルへ焼き込む。後半の詳細は
[§6](#6-単一バイナリとして配布する)。

`dist/`は普通の静的サイトと同じレイアウトでよい。

```
dist/
  index.html
  style.css
  app.js
```

[`examples/webview/dist/`](../../examples/webview/dist/)に、このガイドが
組み立てる最終形の3ファイルがある。

### 3. APIルートを追加する

`routes:`クロージャは、ウィンドウが開く前にアプリ自身のエンドポイントを
（[§15](../stdlib.ja.md#15-http)が説明するのと同じ）`Http`サーバへ登録する。

```culebra
# doctest: skip
Desktop.run({assets: Embed.dir("dist"), routes: fn (srv) {
  srv.get("/api/hello", fn (req) {
    {content_type: "application/json", body: JSON.stringify({message: "Hello from culebra's local server"})}
  })
  srv.post("/api/echo", fn (req) {
    let input = JSON.parse(req.body)
    {content_type: "application/json", body: JSON.stringify({reply: "You said: " + input["text"]})}
  })
}})
```

これは[`examples/webview/desktop_app.cul`](../../examples/webview/desktop_app.cul)
と同じ形: 固定メッセージを返す`GET`と、ページが送ってきたものをそのまま
返す`POST`（実ファイルには3つ目のルートもある — 永続化された訪問回数
カウンタで、詳しくは下のレシピ
[「ワーカー間で安全に状態を持つ」](#ワーカー間で安全に状態を持つ)を参照）。
ハンドラの戻り値がそのままレスポンスになる規則は素の`Http.server()`と
同じ — `String`なら`200 text/plain`、それ以外は
`content_type`/`body`/`status`/`headers`を持つ`Object`で自由に制御する。

### 4. JavaScriptから呼び出す

ページはただのWebページなので、他のどんなHTTPバックエンドに対してと
同じように`fetch`でAPIへ届く。

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

これが[§3](#3-apiルートを追加する)の2つのルートを呼んでいる`dist/app.js`。
ネイティブなJS↔culebraブリッジは一切介在しない — ページは
`127.0.0.1:PORT`へ普通のHTTPで話しかけているだけで、どこかのリモート
サーバに対してと変わらない。つまり普段のWeb開発の作法もそのまま持ち込める
— dev tools、`fetch`、相対URL、ブラウザキャッシュ。

### 5. ページからウィンドウを閉じる

`Desktop.run`のアプリには`POST /__quit`が自動登録されるので、ページは
フレーム自身の閉じるボタンに頼らず、同じHTTPブリッジ経由でアプリを
終了できる。

```js
document.getElementById("quit").addEventListener("click", () => {
  fetch("/__quit", { method: "POST" });
});
```

ページ側が「閉じてよいタイミング」を自分で**決めたい**場合 — 例えば
確認ダイアログを出してから、といった場合 — ネイティブ側はフレームの
閉じるボタンを尊重する前に、ページ自身の`window`グローバルにある2つの
慣習的な名前のプロパティを探す。

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

`__culebra_close__()`はネイティブウィンドウに実際に閉じるよう伝える関数、
`__culebra_before_close__`はページがその呼び出しをゲートできるopt-inの
フックだ。`examples/webview/dist/app.js`には完全版があり、ページ内の
`confirmDialog`（`window.confirm()`ではなく、アプリ自身の見た目に合わせた
プレーンな`Promise`を返すヘルパー）も含む。正確な呼び出し順は
stdlibリファレンスの
[「ページの`window`オブジェクト」](../stdlib.ja.md#ページのwindowオブジェクト)
を参照。

### 6. 単一バイナリとして配布する

`culebra build`はculebraバックエンドと焼き込み済みの`dist/`アセットを
まとめて1つの実行ファイルへコンパイルする。

```sh
culebra build examples/webview/desktop_app.cul -o app
# culebra build: embedded 3 file(s) (...) from 'dist'
./app
```

バイナリの隣に`dist/`は要らない — どこへコピーしても動く。`culebra build`
はWebViewフレームワークのリンクも、プログラムが実際に`Webview`/`Desktop`
を参照しているかどうかでゲートするので、どちらも使わないプログラムは
何もリンクしない。

開発中は毎回ビルドせず、完成したアプリをそのまま実行すればよい。

```sh
culebra examples/webview/desktop_app.cul          # インタプリタ
culebra --jit examples/webview/desktop_app.cul    # 同じ出力、JIT経由
```

## パート2 — レシピ

### 固定ポートを選ぶ

デフォルトでは`Desktop.run`はまずポート`8731`を試し、それが塞がっていれば
OSが割り当てた空きポートへ落ちる — 2つのculebraデスクトップアプリを
並べて動かすには便利だが、ページのorigin（つまり`localStorage`）が
実行のたびに変わりうるということでもある。`port:`を渡せば固定でき、
かつ空いていなければ黙ってfallbackせず、はっきり失敗するようになる。

```culebra
# doctest: skip
Desktop.run({assets: Embed.dir("dist"), port: 5173, routes: fn (srv) {
  # ...
}})
```

### ワーカー間で安全に状態を持つ

`Desktop.run`のサーバはワーカープール（`workers:`、デフォルト`4`）で
リクエストを捌く — そして**各ワーカーは自分のランタイム、自分のヒープを
持つ**。これがルートハンドラがSendableでなければならない理由: 外側の
mutable変数やnon-Sendableなハンドル（`SQLite`のコネクションも含む）を
捕獲できない。これは[§15](../stdlib.ja.md#15-http)と
[§12](../stdlib.ja.md#12-isolate)が`Http.server()`と`Isolate.spawn()`一般
について説明しているのと同じ規則だ。1つの`db`ハンドルをハンドラ間で
共有しようとすると、リクエストが1本も走る前に`SendError`が飛ぶ。

直し方も同じ節が示す通り: リソースを捕獲せず、必要になったハンドラの
中で毎回開き直す。ファイルバックの小さなストアなら、リクエストごとに
開くコストは無視できる。

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

ルートの中でも同じ3行 — `SQLite.open`、作業、`close()`（ハンドラの末尾で
スコープを抜けるに任せてもよい）— が、上のin-memoryの部分を実ファイルに
置き換えるだけで成り立つ。`examples/webview/desktop_app.cul`はまさにこれを
永続化された訪問回数カウンタに適用していて、`/api/hello`・`/api/echo`と
並ぶもう1つのルートになっている。

```culebra
# doctest: skip
srv.post("/api/visit", fn (req) {
  let db = SQLite.open(VISITS_DB)
  db.execute("CREATE TABLE IF NOT EXISTS visits (id INTEGER PRIMARY KEY, n INTEGER)")
  let rows = db.query("SELECT n FROM visits WHERE id = 1")
  let n = if rows.size() == 0 { 0 } else { rows[0]["n"] } + 1
  db.execute("INSERT OR REPLACE INTO visits (id, n) VALUES (1, ?)", [n])
  db.close()
  {content_type: "application/json", body: JSON.stringify({visits: n})}
})
```

`dist/app.js`の`trackVisit()`が読み込み時にこれを呼び、結果を表示する —
`culebra examples/webview/desktop_app.cul`を実行してウィンドウを閉じ、
もう一度実行してみるとよい。カウントは続きから始まる。プロセスではなく
アプリを実行した場所の隣にある`visits.db`に住んでいるからだ。同時書き込みの
直列化はSQLite自身のファイルロックが引き受ける — ハンドルを捕獲しないこと
以外に、culebra側で気を配る調整は要らない。

### 1つのウィンドウで複数ページを扱う

`assets:`はディレクトリ全体を配信するので、HTMLファイルが複数あっても
静的サイトとまったく同じように振る舞う — culebra側のコードは要らず、
リンク1本でよい。[`examples/webview/dist/`](../../examples/webview/dist/)の
`dist/about.html`が2枚目のページで、`dist/index.html`は普通のやり方で
そこへ届く。

```html
<p class="nav"><a href="about.html">About this app</a></p>
```

それだけでよい — サーバはディレクトリ全体を配信しているので、
`GET /about.html`にはすでに答えられる。クリックを介さないプログラム的な
遷移は別の層の話: [§1](#1-ウィンドウを1行で)の生のウィンドウハンドルに
対する`Webview.Window.navigate(url)`であり、`Desktop.run`はこれを
`routes:`には公開していない。複数ウィンドウのAPIはまだない — 1回の
`Desktop.run`呼び出しはプロセスの生存期間中1つのウィンドウに対応する。
[examples/webview/README.mdの「Not here yet」](../../examples/webview/README.md#not-here-yet)
を参照。

### Ubuntuでウィンドウが開かない

Ubuntu 23.10以降で`webview: failed to create window`（あるいは
`bwrap` / `RTM_NEWADDR`のクラッシュ）は、WebKitGTK自身のsandboxが必要と
する非特権user namespaceへのAppArmor制限が原因 — WebKitGTKを埋め込む
アプリはすべて同じ症状に当たり、culebraも例外ではない。最新のUbuntu
（24.04.2以降）はすでに修正済みのプロファイルを同梱している。古い、または
削ぎ落とされたイメージでは、どちらかを行う。

```sh
sudo apt install --reinstall bubblewrap apparmor
```

または自分のバイナリ1つにだけnamespaceを許可する — 正確なprofileは
[examples/webview/README.mdのAppArmorの節](../../examples/webview/README.md#the-sandbox-on-ubuntu-family-systems-apparmor)
を参照。これはOSのポリシーの性質であってアプリの問題ではない — Debian、
Fedora、Arch、Ubuntu由来のpatchが入っていないkernelはどれもこの対応が
不要だ。

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
