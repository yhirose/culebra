# 2026-11-15 Synth Studio（Playground の2ページ目）

- 規模: 小（担当なし、1本の worktree で直列）。user の決定: ネイティブ（Desktop）版ではなくブラウザ版、Playground とは別のページ、倍音の棒グラフは保留して右ペインは AnalyserNode のスコープとスペクトル、選択は「選択の最初の行より前の行＋選択」を関数で包む。サンプルと共有リンクは最初の版に入れる。
- 見込みと実績: 見込み 3.5 時間（wasm の再ビルドは未計測で 20〜30 分と推定）。wasm の再ビルドは実測 3 分（basic と full の2本）。/simplify は1回で、取り込んだのは不具合4件（decode 中の Stop と重ねた Play、成功した出力の位置の書き換え、空行で始まる文書の全選択、列がバイト数）と小さな改善9件。
- 守備範囲の外で回したもの: なし。ビルドは要らず（既存の build/culebra）、check-playground と check-generated だけ。ブラウザの確認は Chrome 拡張がつながらず、ヘッドレスの Chrome を DevTools プロトコルで動かした（サンドボックスの外が要る）。
- 組み替えのきっかけ: (1) Desktop 版の計画のレビューで、別ランタイムの worker、eval がないこと、AOT では動かないことが分かり、user に案 C（ブラウザ）を示した。(2) Playground は実行の終わりに Sound を止めるので、プログラムが WAV を書き、worker の `collect` でページへ返す形にした（後で倍音のデータを渡すのにも使える）。(3) wasm が `gc: a teardown never reached forget()` の assert で落ち、本体の不具合を疑ったが、node のハーネスが worker の `self.__soundsPlaying` を用意していなかったのが原因だった（drop の中の EM_JS が JS の例外を投げた）。最小の再現を前のコミットの wasm でも回して、切り分けに 20 分ほど。
- 気づき: wasm の不具合を疑ったら、まず worker が実行の前に用意するもの（worker.js の run の前半）とハーネスの差を見る。テストで DOM の選択を作ってもヘッドレスでは CodeMirror に届かず、全体が鳴った（ダブルクリックで人と同じ経路にすると届いた）。
