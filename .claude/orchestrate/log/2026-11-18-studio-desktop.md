# Synth Studio のデスクトップ版（着地 6b874aad）

- 規模：大（culebra 3 commit、cpp-httplib の PR #2682 とリリース v0.67.0 を挟む）。
- 仕様は user が選んだ：ページで鳴らす（EWI 音源は別プログラムで試作する前提）、画面は playground/studio.* を共有、`examples/synth-studio/`、追加機能なし。
- 計画のレビューで、POST を WebSocket に変えた（ルートは要求の取り消しを知らず、終わらないコードの Stop が効かない）。print をページへ届けるのは継ぎはぎか仕様追加になるので、README の制限にした。
- 外れた見積もり：AOT のバイナリで全サンプルが失敗した（末尾で `Encoding` を名指し、`--with Synth,Audio` に無い）。base64 を runner 側に移し、studio_test に「末尾は Synth と Audio だけ」の検査を足した。AOT でしか出ない種類は、安い静的な見張りで前に出す。
- user の窓の確認で、閉じるとビジーカーソル。原因は cpp-httplib の `Server::stop()` が WebSocket ハンドラの read を終わらせないこと（最新でも未修正）。user の判断で cpp-httplib に PR、リリース後に submodule を上げて着地。ウィンドウなしの再現（サーバーと ws クライアントを1プログラムに）が原因の切り分けに効いた。
- /simplify は3回（culebra の差分、cpp-httplib の PR 前、着地後に user の指示で）。3回目で、2回目の直し（error で ready を下ろす）がブラウザ版の worker を壊す退行を見つけた。直しが別の runner に効くかを、両方の経路で確かめる。
- user の指摘：「PRをpushする前にかならず /simplify」。メモリに残した。
- 着地のゲートで examples の試験の予算（gate_budget.txt）を忘れた。examples に test_*.cul を足したら予算も。
