# flight-break#107: ObjectのキーがNULで切れる

- 規模: 小〜中。直列、担当なし。見込み約1時間、実績約1.5時間（devのビルド5回、うち1回は/simplifyの指摘で止めて組み直し）。
- 原因: `_jit_key_name`がStringの鍵を`const char*`で返し、内部の鍵の経路が`strlen`で長さを取り直していた。JitObjectとShapeは元から長さ付き。
- 直し方: `_jit_key_name`を`std::optional<std::string_view>`（`JitKeyName`）にし、鍵を受ける内部の関数を`string_view`に。ABI（JITが呼ぶ`const char*`）は入口だけ残して内部へ渡す（remove、has、setを同じ形に分けた）。ネイティブの組み立て（JSON.parse、CSV、TOML、sqlite、regexの名前付きグループ、kwargsのrest、spread、ホストAPI）の`.c_str()`も外した。
- 組み替え: テストを通したら、反復（`for k, v in o`）だけが空になった。`JitKeySnapshot::value`がStringのdataを`const char*`のまま`string_view`にしていた（2つ目の同じ仕組み）。grepでは拾えない形なので、`JitObject`のメンバに`const char*`の`= delete`を一時的に足し、`-fsyntax-only`で呼び出しを全部出して分類した（46件、値由来はこの1件だけ）。速くて客観的な洗い出しで、ほかのリポジトリでも使える。
- /simplify（3並列）: 効率の担当が、ICの書き込みの速い経路（`set_fast`、`set_ic`）に毎回の`strlen`が戻ったことを、生成コードで示した。この経路の`strlen`は以前わざわざ外していたもの。4つの補助関数を鍵の型のテンプレートにして、`strlen`を投げる枝だけに戻した。品質の担当の指摘で、embedの`set`を`_jit_object_set`経由に揃えた。
- テストの効き目: `.cul`の13の確認を、修正前のバイナリで1つずつ別に出して、全部落ちることを確かめた。ホストAPIの確認（`value_api_smoke.cc`）は、修正前のsmoke_suiteを作り直す費用を避け、修正後に通ることだけを見た。
