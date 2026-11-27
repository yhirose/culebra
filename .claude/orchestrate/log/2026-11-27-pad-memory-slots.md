# JITの片付けパッドがslotをメモリから読む形の計測と不採用（2026-11-27）

- 規模: 大として計画（段0の計測の試作 → 段1 → 段2）。段0で止める線に当たり、記録だけで終えた。
- 見積もりと実績: 段0は3〜4時間の見積もりに対し、試作と判定で約2時間。その後の原因の分析に約1.5時間。
- 結果: 同期のstoreをどう減らしても今の形より重い。本物の焼き込み（`culebra_preamble_cc Kauai`、1部分、命令数）で、今の形335G、bindingだけhome 430G（+28%）。正しさ（`just check`、vm/jit対称385件）は保てていた。詳細は`docs/_history.md` 18。
- 組み替え:
  - 計画の前提「パッドの費用の主因は入口の多いphi」が誤り。運ぶ情報の量はphiでもstoreでもほぼ同じで、phiの方が数倍安い。
  - 前回の上限計測（45.6→23.3秒）とSLPが7.4秒という途中の所見は、Homebrewの`opt -O2`がSLPを回すための誤差を含んでいた。culebraの`PassBuilder`（`PipelineTuningOptions()`の既定）はSLPを回さない。本物での上限は−27.5%。
- 守備範囲の外で回したもの: なし（試作は計測専用で、着地させていない）。
- userの指摘: 途中で英語の報告が続き、日本語に戻すよう2回言われた。
- 気づき:
  - 焼き込みの費用は、本物の道具で測る。`culebra_preamble_cc <Name> -o <1つ>`はモジュール丸ごとを本物のパイプラインで通し、`/usr/bin/time -l`のinstructions retiredはマシンの負荷にほとんど動かされない（別セッションのground.culが685% CPUの中でも、同じ形で334.9G / 335.1G）。
  - `CULEBRA_JIT_TIME_PASSES`は焼き込みの道具ではIRのパイプラインの表だけを出す。
  - devにはruntime archiveがないので`culebra build --emit-llvm`は使えない。`--jit -O0 --emit-llvm`で同じ最適化前IRが取れる。
  - サンドボックスの中のsubmoduleの初期化は`.vscode`の書き込みで止まり、`vendor/`に`m`が残る。最初から外で回す（一覧§1の通り）。
