# PEGのフックを借りたまま呼ぶUAF（`_jit_keep_closure`で持つ）とDirの疑いの調査

- 規模：調べないと分からない→小。ハンドルの件で持ち越した2つの疑い（PEGのフック、Dirの`files()`のスナップショット）を再現から調べた。
- 結果：
  - PEG：本物。`_PEG.run`に自分のhooksを渡し、フックが途中で自分を消すとSIGSEGV、または解放後の場所に入った別の関数を呼ぶ。各フックを`_jit_keep_closure`でパースの間持つ形に直した（86cea0ed）。
  - Dir：不具合ではない。文字列は参照カウントを持たず、回収はネイティブのスタックを保守的に調べるので、C++のローカルが持つパスは生き残る。`GC.stat()`とGCストレスでも落ちない。
- 組み替えのきっかけ：
  - 最初は`JitAny`で持つ形で書き、rc-disciplineの上限を13→14に上げた。1本目のci/が`check-borrowed-closure-door`（`check-generated`の中で、`just check`の外）で落ち、そこに「呼び出しを越えて一定の間持つクロージャは`_jit_keep_closure`」という決まりが書かれていた。それに合わせて組み直し、上限は13のまま、キャストのカウントは17→15、gc-pin-doorにPEGの2関数を登録。
  - 共通化のレビューは`_jit_keep_closure`を「HTTPの中にあり、ピンが余分」として退けていた。ゲートの決まりを読んでいれば最初から選べた。C++を変えたら、`just check`だけでなく`check-generated`も手元で回す。
- userの指摘：ツールの合間の一言を英語で書き続け、「日本語で会話して」と2回言われた（メモリfeedback_language_japaneseに追記）。
- 後処理：just check、check-generated、gc-hazard、PEGのテスト4本の両レーン、修正前のビルドで5つの確認がそれぞれ落ちること、/simplify、ci/ 2本。
- テスト時間：test_peg_callbacks.culはexecutorで変わらず、`--jit`で約+0.5秒（高負荷のマシンで約2.9→3.4秒）。
