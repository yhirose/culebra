# 2026-11-08 Playground にサックスの例を足す

- 見込み: 40〜60分。実際: 約40分。後処理は wasm（Node）での実行、project_test、`just check`、`/simplify` 1人、`just land`（test-dev）。
- 内容: `examples/kauai/play_alto_sax.cul`（alto_sax の harbor_lights、準備中の進捗を表示）と `examples.json` の `uses`（エンジン5ファイル、パッケージ、録音データ約9MB、曲）。
- 確認: wasm で52音が作られ、無音・欠落なしでスケジュールが再生に渡った。Chrome での表示確認は user の目に任せる。
- /simplify の指摘: `drums: alto.drums` は nil を渡す死んだ引数（play_recipes.cul からの写し）。直した。コピー元の例の引数は、写すときに実在を確かめる。
- 見落とし: なし。
