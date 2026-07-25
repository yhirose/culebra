# Phase 0 — 律速 spike の測定記録

racer 移植の既定値（解像度 / drawDistance / totalCars / backend）を決めるための計測。2026-07-25 実施。

**結論**:

- native **JIT は余裕**（1024×768 / dd=300 で 0.58 ms/frame）
- native **interp は dd=100〜200 なら 50fps 前後**、dd=300 で 30fps ぎりぎり
- **Playground（wasm interp）は忠実設定では届かない**。200台 + dd=300 で 4.9fps。
  30fps を取るには **totalCars ~20-50 かつ drawDistance ~50** まで落とす必要がある
- 律速は**充填でも Canvas prim でもなく interp のロジック実行**。充填は wasm でも 1.5 Gpx/s 出る
- 原作の AI 早期 return は片側しか見ておらず、**update が 1 周かけて 2.7 倍に増える**（周回ごとにリセット）

## ファイル

| ファイル | 何を測るか |
|---|---|
| `logic.cul` | `update()`（200台 + 20セグメント先読み）と `render()` の投影ループ。描画呼び出しは全部外し、算術だけ |
| `fill.cul` | 既存 Canvas prim の充填レート（`clear` / `rect` / `blit`）と、道路帯 200×4 span の実測 |
| `callcost.cul` | Canvas 1 呼び出しの内訳（prim 本体 vs preamble ラッパ vs ピクセル数） |
| `ops.cul` / `ops2.cul` / `ops3.cul` | interp の素の単価較正（ループ機構・文・プロパティ・呼び出し） |

`logic.cul` は 3 backend で `sink` が一致するので、計算内容が同一であることも確認済み。

## 測定環境と注意

macOS / Apple Silicon、`just dev` ビルド（Release -O3、LTO off）。
Playground は `site/playground` の既存 wasm（2026-07-25 01:05 ビルド、`println` 改名前なので
`puts` に置換）を localhost で serve し Chrome で計測。full（JSPI）ビルド。

**このマシンは他セッションと共有している。** 計測中に load average が 50 を超え、同じ設定が
3〜5 倍ぶれた。`logic.cul` は best-of-5（`REPS`）に切り替えて競合に耐えるようにしてあるが、
それでも残ノイズはあるので、表の数字は**桁と比**を読むこと。

## 1. スイープ（native interp、640×480）

**A. update ms/frame — プレイヤーのトラック上の位置別、dd=300**

| AI 窓 | 周回 0% | 25% | 50% | 75% | 95% | 先読み回数/f (95%) |
|---|---|---|---|---|---|---|
| v4 のまま | 13.0 | 19.9 | 28.4 | 35.4 | 52.9〜79.4 | 3722 |
| 前方dd + 後方100 | — | 19.9 | 24.5 | 15.0 | 16.2 | 312 |
| 前方dd のみ | 13.8 | 15.9 | 16.9 | 15.0 | 14.6 | 243 |

原作の早期 return は `carSegment.index - playerSegment.index > drawDistance` で**符号付き**。
プレイヤーがスタート地点にいるとほぼ全車が「前方遠く」でスキップされ、ゴール手前では
1 台もスキップされない。結果 **update が 1 周かけて 13ms → 40〜50ms に単調増加し、
周回のたびにリセットする**（JS では JIT が効くので誰も気づいていない）。
周回距離で窓にすると 15ms 前後で平坦になる。

**B. render ms/frame — drawDistance 別（プレイヤー 50%）**

| dd | 300 | 200 | 100 | 50 |
|---|---|---|---|---|
| render | 19.5 | 12.2 | 5.5 | 2.7 |

ほぼ dd に線形。解像度には**ほぼ非依存**（投影は解像度非依存で、描画は別勘定）。

**C. update ms/frame — totalCars 別（dd=300、プレイヤー 75%）**

| totalCars | 200 | 100 | 50 | 20 |
|---|---|---|---|---|
| v4 のまま | 52.9 | 16.3 | 6.5 | 2.3 |
| 窓あり | 13.8 | 6.2 | 3.1 | 1.2 |

窓を入れると**台数に完全線形**（≈ 0.069 ms/台）。先読みを消しても 1 台 69µs 残るのは
`update_cars` の 1 台あたりの雑務（`find_segment` ×2、`increase`、`percent_remaining`、
セグメント間の移し替え）が本体だから。**update の床は先読みでなく台数で決まる。**

## 2. backend 間の比（同一設定、640×480 / dd=300 / 200台 / プレイヤー 0%）

| | native JIT | native interp | wasm interp |
|---|---|---|---|
| update | 0.17 | 12.0 | 66.7 |
| render | 0.41 | 19.4 | 138.5 |
| 合計 | **0.58** | 31.4 | **205.2** |

wasm/native interp の比は update ≈ 5.6、render ≈ 7.1。
JSPI(full) vs basic の差は 14%（205 vs 180ms）— JSPI は律速ではない。

この比で外挿すると wasm interp は **update ≈ 0.39 ms/台**、**render ≈ 0.41 ms/dd10**。
30fps（33ms）に収めるには 20〜50 台 + dd 50〜75 あたりまで落とす必要がある。

## 3. 充填レート（`fill.cul`）— 問題ではない

| 項目 | native | wasm |
|---|---|---|
| `rect` 全画面 | 1.1 Gpx/s | 1.5 Gpx/s |
| `blit` 1:1 | 0.6 Gpx/s | 0.4 Gpx/s |

デイサイクル遷移中の背景 3 層 ×2 ＝ 480×360 で ~1.2M px/frame は **wasm でも 3ms 程度**。
hold:transition 比（3:4）を切り詰める必要はない。

## 4. 呼び出し単価 — ここが効く

道路帯テスト（200 帯 × 4 span = 800 呼び出し）は native 4.9ms / wasm 5.5ms で、
**解像度を 4.5 倍にしても時間が変わらない** ＝ ピクセルでなく呼び出し回数が支配項。

`callcost.cul`（native interp）:

```
empty while loop                    349 ns/iter
user fn call (5 args)              2158 ns
_Canvas.rect (prim, 1px)           3186 ns
Canvas.rect (wrapper, 1px)         5681 ns   ← preamble ラッパが +2.5us
_Canvas.rect (prim, 320x2 = 640px) 3684 ns   ← 640px 塗っても +0.5us
```

- **Canvas prim 1 回 ≈ 3µs、preamble ラッパ経由だと ≈ 5.7µs**。ホットループでは
  `_Canvas.*` を直接叩くだけで描画コストが約 1.8 倍速くなる
- **`Canvas.trapezoid`（要望 F）が効く理由は「速いから」ではなく「呼び出し回数を減らすから」**

## 5. interp の素の単価（`ops*.cul`、native interp）

```
empty for  (0..N)   588 ns/iter     ← ループ機構だけでこれ
empty while         349 ns/iter
1 文追加ごと        +305 ns
class field read     +60 ns         ← プロパティ参照自体は安い
user fn call       +1100 ns
Math.floor         +1700 ns
```

- **`for x in 0..N` は `while` より 1 反復あたり ~240ns 高い**（range が iterator プロトコル経由）
- 関数呼び出しが 1 文の 3.6 倍。ホットループでは inline 化が効く
- これらは racer 固有でなく **interp 全体の天井**。Playground の他の例にも同じ制約がかかる

## 6. 移植時の実装注意（この spike で踏んだもの）

- **投影は割る前に cull する。** `cameraDepth / p.camera.z` は、プレイヤーがセグメント境界に
  ちょうど乗ると `z == 0` になる。JS は Infinity を返して次行の `p1.camera.z <= cameraDepth`
  で捨てるが、culebra は `ZeroDivisionError` を投げてゲームが落ちる
- `Canvas.init` はまだ preamble の公開 API に無い（Phase 2 G）。spike は `_Canvas.init` を直呼び
