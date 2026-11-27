# just installの遅さの調査と、焼き込みの分割並列（2026-11-27）

- 規模: 調査 → 大（計画、計画のレビュー1本、実装、/simplify、ci/ 2回、着地）。
- 調査: ccacheが全部当たってもinstallは約2分20秒。内訳は焼き直し約46秒（最後尾はKauaiの1本）、runtime archiveと埋め込み約16秒、ThinLTOのリンク約73秒。
- 組み替え1: 最初に推した2つのキャッシュ（焼き込みの内容、ThinLTO）は、直近32日でloweringかKauaiが24日、main.ccの入力が28日変わっていて当たりにくいと分かり、推薦を取り下げた。推す前に当たる頻度を測るべきだった。
- 組み替え2: 計画のレビューで、compile_partsのTargetMachineがホストCPU向け（焼き込みは汎用CPU）というブロッカーと、改名の順番・公開の範囲・macOSのAOTリンクがCIで見られていない点が実装前に見つかった。見込みは3〜3.5時間 → 約4時間。
- 結果: Kauai 46→13秒、devの焼き込み全体16秒、最大RSS 1.62→1.50 GB、最適化後IRの差264/737,000命令、描画の速さとWAVは同じ。f10b85bc。
- 組み替え3: ci/の1回目で、build-ubuntuとlinux-assertがone_at_a_time_testで赤（masterも同じく赤。ba3f265bのロックの包みが`sh -c`で、dashがexport -fの関数を捨てる）。そのせいでUbuntuの重いジョブが全部飛ばされた。ubuntu:24.04のDockerで再現して直し（c84b48d2）、ci/の2回目で全ジョブ緑。
- 守備範囲の外で回したもの: なし。flight-breakのlook.culの翻訳とbench.culの1ステップは、分け方の規則を--jitと共有したための確認。
- 気づき:
  - Kauaiのソースを`culebra build -O0 --emit-llvm`に通すと最適化前IRが取れ、Homebrewのopt/llcで焼き込みの時間を再現できる。模擬（llvm-extract）は定数の名前と整列がずれるので、IRの比較は実装で書き出したもので行った。
  - sandboxの外で`gh release download`すると、TMPDIRが別の場所になる。
  - 音を出すテストは`CULEBRA_AUDIO=off`で回さないと、手元のデバイスで落ちる。
