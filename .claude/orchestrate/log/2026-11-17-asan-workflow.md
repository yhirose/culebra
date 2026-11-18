# linux-asan を ci.yml から asan.yml へ（何も待たない独立のワークフロー）

- 規模：小〜中（CI の設定だけ、ci/ 1本）。user の問い（重いか、なぜ遅いか、ボトルネックか、要るか）→ 冷えた状態の内訳を実測 → 計画 v2 → user 確定 → 実装 → ci/asan-workflow 全ジョブ緑 → 着地 1f3d069d（push は別セッションの master の push に乗った。初回 f1abbabe の asan.yml は緑）。
- 判断：ASan は ci/ の結果を待つ時間の最長を決めていたが、捕まえるのは解放後の使用という遅れて分かっても困らない種類。master の push（範囲が full）と手動起動だけにし、`just land` が master の最新の結果が赤なら警告する。消さずに置き場所を変えた。
- 構造：change scope の判定を composite action（.github/actions/change-scope）にまとめ、ci.yml と asan.yml が同じものを使う。check_change_scopes.sh は両方の paths-ignore を表と突き合わせる（asan.yml をずらすと落ちることを確認）。
- 組み替えのきっかけ：`git checkout -- <file>` でステージ済みの古い版に戻り、まだステージしていない編集を失った（やり直し）。workflow_dispatch は既定ブランチにファイルが無いと起動できないので、ci/ では asan.yml 自体を試せない。
- 残り：test_proc.cul:327 の時計に頼る assert（別件）。
