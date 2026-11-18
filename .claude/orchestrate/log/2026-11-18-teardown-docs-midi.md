# ワーカーの後始末の slab、docs-cpp の bash 3.2、midi_test、test_proc の時計

- 規模：小が3つ（どれも ci/ 1本、Search の件の CI 待ちの間に並べた）。着地 9bcbed38（後始末）、f175dccd（midi_test と test_proc）、8fdff545（docs-cpp）。
- 後始末：~Runtime の周回で、trait 既定の表の解放が走らせた drop が名前空間の表を生き返らせ、同じ周が slab を消したあとに解放していた。周回を GC のヒープより上と下の2段に。`just build-assert` は dev と同じ程度の重さで手元で再現できた（修正前の assert ビルドで両レーン abort）。
- docs-cpp：macOS の bash 3.2 は `<({ ... })` の中を「,」でブレース展開する（コメントの中でも）。include の確認が何も読まずに通っていた。作り手を関数にし、0件なら失敗にした。最初はバッククォートが原因と書いていたのを、/simplify のレビューが正した。
- midi_test：手元で負荷をかけて20回回しても再現しなかったので、推測で直さず、次に起きたときに原因が分かる診断を足した。
- test_proc：絶対時間の閾値を、パイプのある子との比較に。退行版（tick だけで待つ）で落ち、全コアに負荷をかけても通ることを確かめた。
- 組み替えのきっかけ：docs-cpp-noise の worktree で submodule を初期化しておらず、着地の生成物のビルドで止まった。テストやスクリプトだけの作業でも、着地する worktree では submodule を初期化しておく。
