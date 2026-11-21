# ブラウザの音を AudioWorklet の Engine で（FM Synth をブラウザで、段階1）

- 規模: 大（直列、担当なし。/simplify の3人だけ並列）。着地 034a6e8d＋549e192a（ci/ で全ジョブ緑、そのまま早送り）。
- 見込み: W1（C++ の核）2〜3時間、W2（wasm とページ）3〜4時間、W3（docs）30分。実績はおおむね見込みどおり。dev の作り直しは5回（中心ヘッダ）。
- 範囲の変化: user が段階1の範囲を広げた（グラフだけの推奨に対し、tone と Kauai のスケジュールも Engine へ）。
- 守備範囲の外で回したもの: Playground の wasm を3回作り直した（`just site-build` 約10分。単一 TU の2本が重い）。
- /simplify の指摘で入れたもの: 部品の種類の表を1か所に（decode の範囲外読みを防ぐ）、NumberReader（size_t への未定義の変換）、graph_play をバスの番号で、worklet でグラフを1回だけ作る、JS の共通部分を1つのモジュールに（テストが本物の decoder を通るように）。
- 組み替えのきっかけ:
  - Node の試験で `Synth.play` の wait が戻らない（worklet の言葉が来るまで worker の時計が止まる）。Date.now() を両側の時計にし、reset に位置を載せて worklet が合わせる形にした。
  - user の確認で1回目は無音。page が worker の ready の前に audioPort を送り、worker の onmessage が無く捨てられていた。node の試験は page の配線（app.js）を通らないので捕まえられなかった。
  - 着地のゲートで `tests/test_proc.cul:333`（時間を測る確認）が1回落ち、単独で3回通って再実行で着地（負荷落ち）。
- スキルへの気づき: page と worker と worklet の配線（メッセージの順序）は、node の試験では見えない。ブラウザに触れる変更は、着地の前に user の耳か実ブラウザでの確認を1回置く（Chrome 拡張がつながっていれば自分で）。
