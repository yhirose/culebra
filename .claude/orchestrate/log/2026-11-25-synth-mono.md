# Synth.Mono（#30の4、2026-11-25）

- 規模: 中（Synthのpreamble、examples/fm-synth、docs英日）。仕様の4点（`Synth.Mono`という別の名前、legatoではvelocityを最初の音のまま、優先は最後に押した音だけ、グライドは今までどおり`f.lag(t)`）をuserが決めた。`m.held()`は実装中にFM Synthの都合で足した（確認待ち）。
- 作ったもの: `SynthVoices`から声のグラフとイベントの部分を`SynthNoteTrack`に切り出し、`Voices`と`Mono`で共有した。`play`、`render`、`Proxy`は`SynthNotes`（`_track()`を持つもの）で受ける。`Mono`は押した鍵と離した鍵を時刻の順に持ち、それぞれに声の状態と、そこまでのイベントの数を付ける。順序が前後した鍵は、差し込んだ位置から後ろだけを導き直す。鳴っている最中なら、違いの出た最初のイベントから取り消して当て直す。FM SynthのEWIは`Mono`にして、`@fingers`と`wind_hz`をやめた。
- 確かめ方: `tests/test_synth_mono.cul`（legatoとやり直し、押さえた音へ戻る、velocity、ひとりでに終わった音、逆順に作っても同じ描画、重なるsequence、演奏中の差し込み、Proxy、エラーの文面）。executorとJITで緑。演奏中の差し込みで古いイベントの取り消しを外すと129行目で落ちるのを見た。`just doctest`（861件×2）と`just check`は緑（`test_fs_watch`はsandboxの中では落ちるので外で）。
- 後処理: /simplify（3人）。効いたのは、重なる`sequence`（`len > step`）でノートオンが前のノートオフより前に来るたびに全部を導き直していて、ノート数の2乗になるという指摘（devビルドで1600音が4.1秒）。案は`Mono`の`sequence`だけ時刻順に並べることだったが、根は「最初からやり直す」ことなので、鍵ごとに状態を持って差し込んだ位置から導き直す形にした（同じ条件で0.034秒）。そのために`mono_step`を状態を書き換えない関数にした。ほかに、ノートを離す処理を`Voices`と共通の`let_go`に、`held()`の説明を「時刻がいちばん遅いノートの後」に、`key`の二重の意味を`press`に。見送りは、エラー文面の所有格（`Voices`の既存の文面が変わるため）。
- 気づき: preambleの中では、外側の名前（`made`、`started`）と同じ名前の変数を作れず、焼き込みの段で止まる。新しいstdlibの型名は`parser.h`の`is_stdlib_type_name`に足す。
