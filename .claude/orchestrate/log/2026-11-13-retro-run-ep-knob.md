# 2026-11-13 retro-run: Ep が bright を宣言する

- 規模: 小（examples/games/retro-run と Playground の写し）。user が Playground の画面（`KauaiError: ballad.kau:7: Ep takes no knobs, not bright`）を貼ったのが起点。
- 原因: edf405bf（楽器がつまみを宣言する、関数の楽器はつまみを取らない）で sax_tunes.cul は追従したが、retro-run.cul の `ep` は関数のまま残った。ゲーム本体はどのゲートでも走らないので素通りした。
- 直し方: Ep を ep.cul に出して `bright` を宣言（default 1.0、0〜2）。test_songs.cul がゲームの3曲をゲームの voices で読む（0.1 秒、examples sweep が拾う）。旧形式（関数）に戻すと同じ KauaiError で落ちることを確認。ゲームは VM・--jit ともヘッドレスで起動し、修正前は起動直後に落ちることも確認。
- 後処理: `just check` 緑（34s、予算23s超はマシン負荷）。Playground の写しは copy-frontend.sh で揃え、check_site_playground_sync.sh で確認。
