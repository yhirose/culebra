# SynthガイドにMIDIコントローラーで弾く節（#30の1、2026-11-25）

- 規模: 小（docsだけ）。#30の監査の順の1番目「CCを値に結ぶ方法を文書にする」。見込み約30分、実績も約30分。ビルドは新しいworktreeのdev 1回（ccacheが効いた）。
- 書いたこと: `docs/guides/synth.md`と`.ja.md`の「鳴らしている最中に変える」の後に節を1つ。キーボードは`Voices`＋ベンドとモジュレーションホイールの`control`、ウインドコントローラーは声1つで息をエンベロープに、音高を`hz(control)`の`lag`で。CCの段は`.lag(t)`で隠す。14ビットの息は`input.control(2)`で読む。
- 確かめ方: 書く前に2つの前提を手で確かめた（executorと`--jit`で同じ）。各声のbuilderの中で作った同じ名前の`control`は1つの値で、`h.set`1回で全部の声に届く（設定の前は同じサンプル、後で違う）。`hz`に信号の`control`を渡して音高が動く。`just doctest`は2レーンとも859件緑。docsのブロックが8つ増えたので`gate_budget.txt`のdocs.blocksを1256から1264に（足した例は合わせて約0.03秒）。`just check-generated`緑。
- 後処理: userの指示で/simplify（4人）。採ったのは、例の関係ない`lowpass`を外す、`bend * (1.0 + vibrato)`を声の外で1回作る、各声の`control`が1つの値であることをstdlib §46の`Voices`の段落にも書いてガイドは短く、レガートと14ビットの説明は§41へのリンクにしてホイールも`input.control(1)`で読む、の4件。見送りは、キーボードの例からホイールを外す案（CCを値に結ぶ例そのもの）と、stdlibの目次の行からガイドへリンクする案（目次はガイドへリンクしない書き方）。
- 気づき: 試しのスクリプトで`take`／`drop`を書いて落ちた。Arrayは`slice(start, end)`。CLAUDE.mdの「よくある名前ほど引く」のとおりだった。
