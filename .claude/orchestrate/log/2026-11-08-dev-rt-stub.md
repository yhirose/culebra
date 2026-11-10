# 2026-11-08 just dev が embedded_rt.cc を毎回再コンパイルする件

- 見込み: 15〜30分。後処理は `just dev` の2連続実行だけ。着地は full スコープなので test-dev。
- 実際: 約15分。ビルド2回で再現（2回目に embedded_rt.cc の再コンパイルと再リンク）。
- 原因: configure のたびに `file(WRITE)` が内容の同じスタブヘッダの更新時刻を進めていた。
- 修正: `file(CONFIGURE ... CONTENT)` に変えた（内容が同じなら書かない）。修正後は4回目の `just dev` が何もビルドしない。
- 見落とし: なし。
