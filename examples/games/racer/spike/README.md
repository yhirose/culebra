# Phase 0 — 律速 spike の測定記録

racer 移植の既定値（解像度 / drawDistance / backend）を決めるための計測。2026-07-25 実施。
**結論だけ先に**: native は JIT なら余裕、native interp は 33fps 止まり、**Playground（wasm interp）は
忠実設定で 4.6〜10.7fps** で届かない。律速は充填でも Canvas prim でもなく **interp のロジック実行**。

## ファイル

| ファイル | 何を測るか |
|---|---|
| `logic.cul` | `update()`（200台 + 20セグメント先読み）と `render()` の投影ループ。描画呼び出しは全部外し、算術だけ。解像度 × drawDistance のスイープ |
| `fill.cul` | 既存 Canvas prim の充填レート（`clear` / `rect` / `blit`）と、道路帯 200×4 span の実測 |
| `callcost.cul` | Canvas 1 呼び出しの内訳（prim 本体 vs preamble ラッパ vs ピクセル数） |
| `ops.cul` / `ops2.cul` / `ops3.cul` | interp の素の単価較正（ループ機構・文・プロパティ・呼び出し） |

`logic.cul` は 3 backend で `sink` が一致する（`63741749`）ので、計算内容が同一であることも確認済み。

## 測定環境

macOS / Apple Silicon、`just dev` ビルド（Release -O3、LTO off）。
Playground は `site/playground` の既存 wasm（2026-07-25 01:05 ビルド、`println` 改名前なので
`puts` に置換して実行）を localhost で serve し、Chrome 上で計測。full（JSPI）ビルド。

## 1. ロジック（`logic.cul`、ms/frame）

| 設定 | native JIT | native interp | wasm interp |
|---|---|---|---|
| 1024×768 / dd=300 | **0.58** | 30.3 | — |
| 640×480 / dd=300 | 0.58 | 31.4 | **205.2** |
| 640×480 / dd=200 | 0.43 | 24.2 | 138.0 |
| 640×480 / dd=100 | 0.30 | 17.6 | 97.3 |
| 480×360 / dd=100 | 0.30 | 17.8 | **93.5** |

- 解像度はロジック時間にほぼ影響しない（投影は解像度非依存）。効くのは **drawDistance だけ**
- update は解像度・dd にほぼ非依存（native interp 11〜12ms、wasm 51〜67ms）。
  原作の「視界外の車は先読みしない」早期 return は *前方* にしか効かず、後方の車（＝ほぼ全部）は
  20 セグメント先読みを毎フレーム回している
- JSPI(full) vs basic の差は約 14%（205 vs 180ms）。JSPI は律速ではない

## 2. 充填レート（`fill.cul`）— 問題ではない

| 項目 | native | wasm |
|---|---|---|
| `rect` 全画面 | 1.1 Gpx/s | 1.5 Gpx/s |
| `blit` 1:1 | 0.6 Gpx/s | 0.4 Gpx/s |

デイサイクル遷移中の背景 3 層 ×2 ＝ 480×360 で ~1.2M px/frame は **wasm でも 3ms 程度**。
hold:transition 比を切り詰める必要はない。

## 3. 呼び出し単価（`fill.cul` の道路帯 + `callcost.cul`）— ここが効く

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
- 30fps の予算（33ms）は prim 呼び出し ~5000 回相当。dd=300 × 1 セグメント 6〜7 呼び出し = 2000 回で
  すでに 12ms を使う → **`Canvas.trapezoid`（F）は「速いから」ではなく「呼び出し回数を減らすから」効く**

## 4. interp の素の単価（`ops*.cul`、native interp）

```
empty for  (0..N)   588 ns/iter     ← ループ機構だけでこれ
empty while         349 ns/iter
1 文追加ごと        +305 ns
class field read    +60 ns   (プロパティ参照自体は安い)
user fn call       +1100 ns
Math.floor         +1700 ns
```

- **`for x in 0..N` は `while` より 1 反復あたり ~240ns 高い**（range が iterator プロトコル経由）
- 関数呼び出しが 1 反復ぶんの 3 倍以上高い → ホットループでは inline 化が効く
- これらは racer 固有でなく **interp 全体の話**。Playground の他の例にも同じ天井がかかっている
