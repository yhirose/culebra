# 自分のコールバックから閉じる・再入する（Search・Http.client・Llama・wrap<T>）

- 規模：大（CI を待つ）。Search のフックの中の close の SIGSEGV から始め、担当の洗い出しで同じ仕組みが5か所（1つは誤検出）と分かった。計画 v1 → レビュー1回（1つの印では足りない、wrap<T> は取り出し口で数える）→ user 確定（2つの束、close はエラー、drop は §17 のとおり表示して残す）→ 束1 → /simplify → 束2 の計画 → レビュー1回 → user「おすすめで」→ 束2 → /simplify → ci/ → 着地 b24f59df。
- 構造：取り出し口（IdRegistry::use、wrap.h の jit_handle_self）が Held を返すので、数え忘れる呼び出し元を作れない。close/drop の判断はコア、文面と位置はバインディング。再入の規則は対象ごと（Search は add の最中だけ、Http はパスの要求の入れ子、Llama は n_slots 超え）。
- 組み替えのきっかけ：
  - 担当の洗い出しの「C++ splitter の UAF」は誤検出（searchlib は splitter の出力をバッファに溜めてからフックを呼ぶ）。読みの主張は再現で確かめてから採る。
  - Llama の close の書き換えで `return true;` を落とし、戻り値がごみ（UB）。dev-llama で正常な close が拒まれて気づいた。dev-llama は sandbox の外（llama.cpp の展開）。テスト用モデルは misc/aot_axes/fetch_llama_test_model.sh。
  - 生成物のゲート（gc-pin-door）が自分の `Pin`/`.pin()` を GC の pin と取り違えた。名前を Held に。
  - doctest-ubuntu で quick-guide.ja.md が古いと落ちた。stdlib.ja.md の表の行がバッククォートで始まるとシグネチャとして数えられる。`just check-quick-guide` は check-generated に入っていない。
  - user の指摘：ASan を待つ必要はない（決めた方針に反して待っていた）。
  - `gh run watch` を並べて API の回数制限を使い切った。見張りは1本で数分おき。
- 後処理：just check、just doctest、gc-hazard、check-generated、修正前のバイナリで確認ごとに落ちること（Search・Http・CodeGen・Llama）、ci/ の全ジョブ。
- スキルへの気づき：docs/stdlib*.md を触ったら ci/ の前に `just check-quick-guide` も回す。
