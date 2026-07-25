# Phase 0 — 律速 spike の測定記録

racer 移植の既定値（解像度 / drawDistance / totalCars / backend）を決めるための計測。
2026-07-25 実施。第1ラウンド = `logic.cul` ほか、第2ラウンド = `opt.cul`。

**結論**:

- 忠実設定（200台 / dd=300 / 640×480）のロジックは **native でも Playground でも約 12 ms/frame**。
  **設定を分ける必要はない**
- 律速は充填でも Canvas prim でもなく **interp のロジック実行**。その中の最大の単一要因は
  **`return` / `break` / `continue` が C++ 例外であること**（早期 return 1 回 ≈ 22 µs）
- **wasm interp は native interp とほぼ同速**（比 0.8〜1.1）
- native **JIT は余裕**（1024×768 / dd=300 で 0.58 ms/frame）
- 原作の AI 早期 return は片側しか見ておらず、**update が 1 周かけて 2.7 倍に増える**（周回ごとにリセット）

## 第1ラウンドの訂正

以下は第1ラウンドの記録が誤っていた点。第2ラウンドで訂正した。

| 誤 | 正 |
|---|---|
| wasm interp は native の 5.6〜7.1 倍遅い | **ほぼ同速**（0.8〜1.1 倍） |
| Playground は忠実設定で 4.9fps（205 ms/frame） | 同じロジック（L0）で **29.7〜32.9 ms** |
| 測定は `just dev` = Release -O3 | `just dev` は **-O1**（`CULEBRA_DEV_O1`）。ただし -O3 との差はこのワークロードでは小さい |

第1ラウンドの wasm 測定がなぜ 6 倍ずれたのかは特定できていない。第2ラウンドは
**同一ファイルを native と wasm で走らせ、`sink`（計算結果の集計値）が完全一致することを確認**
した上での比較なので、そちらを正とする。

## ファイル

| ファイル | 何を測るか |
|---|---|
| `opt.cul` | **第2ラウンド**。`logic.cul` と同じ世界・同じ算術のまま、update/render を段階的に書き換えて各段のコストを分離する。section D が `return` のコストを切り出した場所 |
| `logic.cul` | 第1ラウンド。`update()`（200台 + 20セグメント先読み）と `render()` の投影ループ。描画呼び出しは全部外し、算術だけ |
| `fill.cul` | 既存 Canvas prim の充填レート（`clear` / `rect` / `blit`）と、道路帯 200×4 span の実測 |
| `callcost.cul` | Canvas 1 呼び出しの内訳（prim 本体 vs preamble ラッパ vs ピクセル数） |
| `ops.cul` / `ops2.cul` / `ops3.cul` | interp の素の単価較正（ループ機構・文・プロパティ・呼び出し） |

`logic.cul` / `opt.cul` はどちらも 3 backend で `sink` が一致するので、計算内容が同一であることも確認済み。

## 測定環境と注意

macOS / Apple Silicon。native は `just build`（-O3 + LTO）と `just dev`（-O1）の両方で測り、
**このワークロードでは両者の差は小さい**（tree-walking interp なので最適化レベルが効きにくい）。
Playground は `site/playground` の既存 wasm（2026-07-25 01:05 ビルド、`println` 改名前なので
`puts` に置換）を localhost で serve し Chrome で計測。full（JSPI）ビルド。

**このマシンは他セッションと共有している。** 計測中に load average が 50 を超え、同じ設定が
3〜5 倍ぶれた。`logic.cul` / `opt.cul` は best-of-N（`REPS`）にしてあるが、それでも残ノイズは
あるので、表の数字は**桁と比**を読むこと。計測前に `uptime` で load を確認する。

`opt.cul` を Playground で走らせるコピーの作り方（section A だけに絞り、`println` を戻す）:

```
perl -ne 'last if /^# B\. how the best level/; s/^let QUICK = false/let QUICK = true/;
          s/\bprintln\b/puts/g; print' \
  examples/games/racer/spike/opt.cul > site/playground/examples/greeting.cul
```

（`examples/greeting.cul` を差し替えて Run する。エディタへの巨大ペーストを避けるため。
macOS の `sed` は `\b` 非対応なので `perl` を使う。`http://[::]` は非 secure 扱いになり
WebGPU が落ちて basic 側が崩れるので、**必ず localhost で開く**。）

## 1. 最適化レベル（`opt.cul`、第2ラウンド）

各レベルは下のレベルを含む。

- **L0** = `logic.cul` の書き方 + AI 窓（窓は第1ラウンドで採用確定）
- **L1** + **car の segment cursor**: 車の位置を track 絶対の `z` でなく `(segment index, セグメント内 offset)`
  で持つ。1 フレームで進むのは高々 1 セグメントなので加算 1 回と繰り上がり判定で済み、
  `find_segment` ×2（各 `Math.floor` + 剰余 + 呼び出し）と `increase` / `percent_remaining` が消える
- **L2** + **render の inline 化**: `project()` の展開、`for`→`while`、セグメント index の剰余を加算+ラップ判定に、
  ループ不変なカメラ項の外出し、fog の事前テーブル化、そして**画面座標を float のまま扱う**
  （`Math.round` ×6/segment と `Math.floor` ×2/sprite を描画 prim 側に移す前提）
- **L3** + **制御フロー例外の排除**: `return` / `break` / `continue` を `done` フラグで置き換える

**A. ms/frame（640×480、dd=300、200台、AI 窓 behind=100、プレイヤー 50%）**

| level | native -O3 | wasm (2回) |
|---|---|---|
| L0 | 36.6 | 32.9 / 29.7 |
| L1 | 35.8 | 27.7 / 25.2 |
| L2 | 14.3 | 13.8 / 12.6 |
| **L3** | **12.1** | **12.9 / 12.4** |

`sink` は全レベルで native と wasm が完全一致（`5062850` / `5074692` / `7286037` / `7286037`）。

内訳（native -O3、プレイヤー位置スイープの平均的な値）:

| level | update | render |
|---|---|---|
| L0 | 15.7 | 20.4 |
| L1 | 8.0 | 20.1 |
| L2 | 8.1 | 6.2 |
| L3 | 3.1 | 5.7 |

**L0→L1 と L2→L3 は純粋な書き換え**のつもりだったが、`sink` は L0 と L1 で一致しない
（`5062850` vs `5074692`）。L1 は overlap 判定を「距離 vs 半幅の和」に変形しており、境界ちょうどの
ケースで浮動小数の丸めが変わる。native と wasm では同じ値になるので backend 対称性は保たれている。
L2 と L3 は完全一致。

**B. drawDistance 別（L3、プレイヤー 50%、native -O3）**

| dd | 300 | 200 | 150 | 100 | 50 |
|---|---|---|---|---|---|
| update | 3.25 | 3.37 | 3.34 | 2.89 | 2.66 |
| render | 5.86 | 4.03 | 3.02 | 1.88 | 0.99 |
| **合計** | **9.11** | 7.40 | 6.36 | 4.77 | 3.65 |

render はほぼ dd に線形。解像度には**ほぼ非依存**（投影は解像度非依存で、描画は別勘定）。

**C. totalCars 別（L3、dd=300、プレイヤー 50%、native -O3）**

| totalCars | 200 | 100 | 50 | 20 |
|---|---|---|---|---|
| update | 3.44 | 1.69 | 0.79 | 0.26 |

台数に完全線形（≈ 0.017 ms/台。L0 では 0.069 ms/台だった）。

## 2. `return` / `break` / `continue` は C++ 例外

`interpreter.h` の `throw ReturnValue{...}` / `BreakSignal` / `ContinueSignal`。
関数の早期脱出もループの脱出も、毎回スタック unwind を伴う。

`opt.cul` の section D は、update の per-car の本体から要素を 1 つずつ落として値段を測る
（結果は意図的に壊れる。コストの切り分け専用）。native -O3、dd=300、プレイヤー 50%、200台:

| AI 呼び出しを何に差し替えたか | update ms |
|---|---|
| 本体まるごと無し（ループと z 更新だけ） | 0.75 |
| 空関数（呼び出しコストだけ） | 2.27 |
| 窓判定のみ、**`return` あり** | 6.71 |
| 窓判定のみ、**`return` を `if/else` に**（グローバル読みは残す） | **2.54** |
| 窓判定のみ、`return` あり・グローバル読み無し（全部ローカル引数） | 5.94 |
| フル（実際の先読みも含む） | 7.54 |

- 早期 return 190 回/frame で **+4.2 ms** ＝ **1 回あたり約 22 µs**。1 文 305 ns の 70 倍以上
- **グローバル変数（トップレベル `mut`）の読み書きは無罪**。ローカル引数に変えても速くならない
- render 側も同じ: dd=300 のうち **87 セグメントが丘クリップで捨てられる**ので、v4 のとおり
  `continue` すると毎フレーム 87 回の unwind が乗る

**例外コストは native のほうが高い。** L2→L3 の効き方が backend で逆になる:

| | L2 | L3 | 差 |
|---|---|---|---|
| native | 14.3 | 12.1 | −2.2 |
| wasm | 12.6 | 12.4 | −0.1 |

`-fwasm-exceptions` は macOS arm64 の Itanium ABI unwind より安い。

## 3. AI 窓（第1ラウンド、`logic.cul`）

**update ms/frame — プレイヤーのトラック上の位置別、dd=300、native -O1**

| AI 窓 | 周回 0% | 25% | 50% | 75% | 95% | 先読み回数/f (95%) |
|---|---|---|---|---|---|---|
| v4 のまま | 13.0 | 19.9 | 28.4 | 35.4 | 52.9〜79.4 | 3722 |
| 前方dd + 後方100 | — | 19.9 | 24.5 | 15.0 | 16.2 | 312 |
| 前方dd のみ | 13.8 | 15.9 | 16.9 | 15.0 | 14.6 | 243 |

原作の早期 return は `carSegment.index - playerSegment.index > drawDistance` で**符号付き**。
プレイヤーがスタート地点にいるとほぼ全車が「前方遠く」でスキップされ、ゴール手前では
1 台もスキップされない。結果 **update が 1 周かけて 13ms → 40〜50ms に単調増加し、
周回のたびにリセットする**（JS では JIT が効くので誰も気づいていない）。
周回距離で窓にすると 15ms 前後で平坦になる。**採用**。

## 4. 充填レート（`fill.cul`）— 問題ではない

| 項目 | native | wasm |
|---|---|---|
| `rect` 全画面 | 1.1 Gpx/s | 1.5 Gpx/s |
| `blit` 1:1 | 0.6 Gpx/s | 0.4 Gpx/s |

デイサイクル遷移中の背景 3 層 ×2 ＝ 480×360 で ~1.2M px/frame は **wasm でも 3ms 程度**。
hold:transition 比（3:4）を切り詰める必要はない。

## 5. 呼び出し単価（`callcost.cul`）

道路帯テスト（200 帯 × 4 span = 800 呼び出し）は native 4.9ms / wasm 5.5ms で、
**解像度を 4.5 倍にしても時間が変わらない** ＝ ピクセルでなく呼び出し回数が支配項。

```
empty while loop                    349 ns/iter
user fn call (5 args)              2158 ns
_Canvas.rect (prim, 1px)           3186 ns
Canvas.rect (wrapper, 1px)         5681 ns   ← preamble ラッパが +2.5us
_Canvas.rect (prim, 320x2 = 640px) 3684 ns   ← 640px 塗っても +0.5us
```

- **Canvas prim 1 回 ≈ 3µs、preamble ラッパ経由だと ≈ 5.7µs**。ホットループでは
  `_Canvas.*` を直接叩くだけで描画コストが約 1.8 倍速くなる
- `Canvas.trapezoid`（要望 F）が効くとすれば「速いから」ではなく「呼び出し回数を減らすから」

## 6. interp の素の単価（`ops*.cul`、native interp）

```
empty for  (0..N)   588 ns/iter     ← ループ機構だけでこれ
empty while         349 ns/iter
1 文追加ごと        +305 ns
class field read     +60 ns         ← プロパティ参照自体は安い
user fn call       +1100 ns
Math.floor         +1700 ns
early return     +22000 ns          ← 第2ラウンドで判明（§2）
```

- **`for x in 0..N` は `while` より 1 反復あたり ~240ns 高い**（range が iterator プロトコル経由）
- これらは racer 固有でなく **interp 全体の天井**。Playground の他の例にも同じ制約がかかる

## 7. 移植時の実装注意（この spike で踏んだもの）

- **投影は割る前に cull する。** `cameraDepth / p.camera.z` は、プレイヤーがセグメント境界に
  ちょうど乗ると `z == 0` になる。JS は Infinity を返して次行の `p1.camera.z <= cameraDepth`
  で捨てるが、culebra は `ZeroDivisionError` を投げてゲームが落ちる
- `Canvas.init` はまだ preamble の公開 API に無い（Phase 2 G）。spike は `_Canvas.init` を直呼び
- **ホットパスでは `return` / `break` / `continue` を使わない**（§2）。`done` フラグ + `while !done && …`
  で書く。これは racer に限らず culebra のホットループ全般に効く
