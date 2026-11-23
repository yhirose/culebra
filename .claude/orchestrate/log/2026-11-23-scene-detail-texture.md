# Sceneの細部のテクスチャ mat.detail（2026-11-23）

- 規模: 小〜中。flight-break#129（近くの水面のさざ波）。C++は`culebra_rt_scene.cc`の1ファイル（devは1回約3分。変更前のバイナリを作るためと、壊した版の確認とで計4回）。
- 作業: `mat.detail(tex, us, vs, strength = 1.0, masked = false)`。メッシュ自身のUVで繰り返し（`uv()`では動かない）、テクスチャの色に灰色128を中心に足す（unlitにも効くよう`base`の前で混ぜる）。`masked`のときは元のテクスチャのαが重ねる強さになり、被覆ではなくなる。テクスチャの枠はOCCLUSION（法線マップのROUGHNESSと同じやり方）。重ねる所の決め方（元のα）とUVの基準（メッシュのUV）はuserに選択肢を出して決めた。
- 確認: scene_api_test.shに`detail`（繰り返しの倍率、陸は元の色、水の平均、uv()との独立、遠くの平均、maskedで被覆にならない、無いときはαが被覆、nilで外れる、litでも効く）。壊した3か所（細部のUVを`uv()`後から取る、maskedでも被覆にαを使う、強さにαを掛けない）で、対応する確認が落ちることを確かめた。追加は両エンジンで約0.7秒（scene_api_test.sh全体の約50秒に対して）。
- 速さ: 3000枚の板を別々の材質で描くフレームを、変更前後のdevバイナリで交互に8回ずつ。中央値37.9→38.2ms（差0.8%、同じバイナリどうしの揺れ約3msの中）。`just build`では測っていない。
- 後処理: just check、doctest、check-generated（gate_budgetのdocsのブロック+2）、gen-quick-guide、scene_api_test.sh（llvmpipeで全件）。/simplifyの採用: 上下反転の式を`Texture::upright`に1つ、細部のuniformを1つのifで、テストの市松を`view.checker`で。見送り: `masked`を元のテクスチャ側の設定に移す（userが選んだAPIを変える）、uniformの詰め合わせ（今の流儀は設定ごとに1つ）、白のテクスチャを束縛しない最適化（法線マップと一緒でなければやらない、測って差が無い）。
- この環境（WSLg）では、変更前のバイナリでも`scene_api_test.sh`の`bias`が落ちる（前回の記録と同じ）。llvmpipeでは通る。
