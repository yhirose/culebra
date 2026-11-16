# 鍵の eq が引いている表を変える UAF（JitKeyTable）と get(k, d) の drop UAF

- 規模：大（CI を待つ）。計画 v1 → レビュー1回 → v2 → user 確定（意味論は「引き直す」）→ 実装 → /simplify → コードレビュー1回 → ci/ 2本 → 着地 59f7c743（ci/ と同じコミットなので master の CI は回らない）。
- 構造：表の std コンテナの等価からプログラムのコードを外し（同一性だけ）、eq は表の外で訊いて版数で引き直す（CPython の lookdict）。生の find/insert は private 継承で外から呼べない。不変条件を型の1か所で保証する形。
- 性能（`just build`、交互5回の中央値）：Long 鍵 ±0、Object 鍵 +1.5〜6%、Object `==` VM +2.7% / JIT +7.7%。初版は JIT +8〜15% で、find が entry* を返す形と等価の同一性の近道で詰めた。
- 組み替えのきっかけ：
  - 計画レビューで get(k, d) の別の UAF が見つかり、同じ束の別コミットにした（user 確定）。ヒープの配列に +1 を積む案は保守的 GC から見えないので版数の設計に変えた。
  - コードレビューで、格納中にハッシュが変わった鍵があると Object の `==` が true になる穴。引けた数を突き合わせて直した。
  - 修正前に落ちるかの確認は MallocScribble ありだと逆に通る形がある（跡地の再利用で隠れる）。既定の設定でも確かめ、assert ごとにブロックを切り離して回した。
  - 1本目の ci/ は gate_budget.txt の件数（372→373）の更新漏れで build-ubuntu が落ち、後続の test-ubuntu などが走らなかった。CI の往復1回（約80分）。
- 後処理：just check、修正前のバイナリでの assert ごとの効き目、just doctest、ci/ の全ジョブ（docs-cpp-macos は2本目でランナー不足の cancelled、1本目は緑）。
- スキルへの気づき：ci/ に出す前に `just check-generated`（約10秒、sandbox の外で。gate_budget を含む）を回す。just check には入っておらず、just test-dev（着地）より先に ci/ へ出すと CI で初めて見つかり、build-ubuntu の後段がまとめて走らない。
