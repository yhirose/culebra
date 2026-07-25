# パレット・デイサイクル 検討過程の記録

racer 移植の色調・4シーン構成を決めるまでに作った探索スクリプトと成果物。**実装の入力ではなく、
判断の記録**（何を試して何故その値になったか）。Phase 1 の `gen_assets.py` はここから直接 import せず、
`.claude/plans/racer-port.md` に転記済みの確定パレット表を唯一の真実源として実装する。

## 何が最終決定か

- **`video_gen.py`** — 最終形。確定した彩度/narrow/fog レシピ（`SAT_K=0.85`, `NARROW=0.30`,
  `FOG_FLOOR=0.35`）と4シーンの基準 RGB（`DAY` / `ASAGAKE` → 実装では asayake / `YUUGURE` / `TWILIGHT`）を
  そのまま持つ。`dither.gif` と `blend.gif` を生成したスクリプトで、**`blend.gif`（本格アルファブレンド）が
  採用案**。計画書 Phase 2 の合成式（整数のみ、`(src*a+dst*(255-a)+127)/255`）はこの GIF の判断を受けて設計した
- **`swatch6.py`** / `swatch6.png` — 4シーンの色相候補（I1〜I4）。I2=asayake、I1=yuugure、I3=twilight として採用
- **`swatch5.py`** / `swatch5.png` — 彩度+85%を固定した上で「淡い側が淡すぎる」問題への対処4案（E/F/G/H）。G を採用

## 途中経過（参考のみ、直接の採用値ではない）

- `swatch.py`, `swatch4.py` — 彩度を上げる前段の試作、4段階の彩度比較
- `gen_sample.py`, `gen_car2.py`, `cars_preview.png`, `car0-5.png` — 車のプロポーション試作
  （フェラーリ風の最終形は `video_gen.py` の `car_ferrari()`）
- `sample.png`, `mock.png`, `palm.png`, `car.png` — 各ラウンドの単発プレビュー画像

## 除外したファイル

原作 `javascript-racer` のスプライト（`images/sprites/car01.png` 等）を最近傍拡大しただけの
プロポーション比較画像（`ref_car01.png` 等）は、著作権が不明瞭な原作素材の複製にあたるため
**この worktree にも culebra リポジトリにも含めていない**。「原作アセットは同梱不可」の決定
（[[project_racer_port]] メモリ参照）どおり。
