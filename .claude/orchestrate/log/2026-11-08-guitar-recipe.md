# 2026-11-08 guitar recipe pluck knob

- 見込み: 作業30〜45分。後処理は曲の準備、kauaiのテスト、recipe.md。ci/は使わない。
- 実際: ほぼ見込み通り。原因は単純で、3つのギターのレシピに`pluck`つまみが無く、曲の`pluck=`が拒否されていた。
- 修正: `pluck`つまみ（既定0）で励振をくし形にした。既定値の音は、18音の前後ダイジェストでサンプル単位に一致。
- 見落とし: 最初の着地が check-generated で落ちた。`site/playground`が`kauai/recipes`の写しを持つため。`playground/copy-frontend.sh site/playground`でemsdkなしに写せる。`kauai/recipes`を触る変更は、着地前にこの手順が要る（プロファイルの逆引きに足す）。
