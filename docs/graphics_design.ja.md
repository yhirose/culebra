# `Graphics` — レンダラ非依存グラフィックス facade（設計メモ）

実 SUZUKA（`~/Projects/racing`、Swift/SceneKit+SpriteKit、16k 行）の描画表面を抽出し、
culebra 側 IF を固定したままバックエンド（raylib / Filament 等）を差し替え可能にするための契約。

> 改訂履歴: 初版＝SceneKit からの素朴抽出。第2版（本版）＝設計レビュー会議の指摘を反映
> （保持土台化 / マテリアル PBR-intent 化 / drop / 座標系固定 / capability / 動的メッシュ）。
> バックエンド選定で不採用にした Qt は [`_history.ja.md`](_history.ja.md) を参照。

> 状態（2026-06-28）: この facade は**実装・出荷済み**（master `1712c59`、raylib を culebra
> core に opt-in 統合する `CULEBRA_ENABLE_GRAPHICS`、AOT usage-gated、`culebra build suzuka.cul`
> で単体バイナリ）。本書は**設計の根拠の記録**であり、以下の具体シグネチャは実装前のスケッチで
> 出荷 API そのものではない。`wrap.h` が kwargs デフォルト/タプル引数を取れないため、出荷 API は
> **fluent setter + 平坦スカラ + 色 0..255** に確定した（例: `make_material(color, metallic:, …)`
> でなく `view.material_tex_pbr(tex, r,g,b, metallic, roughness)` / `add_box(w,h,d).material(m).move(x,y,z)`
> / `view.camera(ex,ey,ez, tx,ty,tz, …)`） — 正の API は `examples/graphics/suzuka.cul` を参照。
> 切替機構も、バックエンドごとに `culebra wrap` 拡張で別バイナリを作る案から **core への opt-in
> ビルド**へ変更した。設計の根拠自体（renderer 非依存 / 保持土台 / PBR-intent / 座標系固定 /
> core⇔optional の線引き / backend 射程）は有効。

## 要件

1. **3D と 2D の両方**を扱える。
2. **culebra 側 IF を極力変えずに別ライブラリへ切り替え可能**にする。

## 対象バックエンドの射程（重要）

差し替え互換には2階層ある:
1. **描画 API の互換**（add_box, set_camera, text…）— facade で吸収できる。
2. **フレーム駆動モデルの互換**（誰がメインループを握るか）— facade では吸収しきれない。

→ **当面の射程は「ゲームがループを握る」型のバックエンドに限定**: **raylib / Filament(+GLFW) / bgfx / sokol**。
いずれも `while !view.should_close() { ... }` でゲームが毎フレーム描画を呼ぶモデルで、上記1・2とも互換。


## 核心の設計判断

- **3D も 2D も「保持（retained）を土台、即時（immediate）はその上の糖衣」**。
  - 当初「3D=保持 / 2D=即時」と非対称に分けたが、即時 2D 決め打ちは保持型バックエンドの足を引っ張り、
    HUD のフォント再描画コストも毎フレーム発生する。土台を保持で統一し、即時ヘルパは保持ノードの薄い糖衣にする。
  - 即時モードの raylib はバインディング内部にノード列を持ち `render()` で replay（薄いアダプタ）。
  - SUZUKA の HUD（SpriteKit）も元は保持なので、保持土台が自然。
- **切り替え = 同一 `Graphics.*` 名前空間を各バックエンドの `culebra wrap` バインディングが実装する**。
  ゲームは `Graphics` だけに依存し、切り替えはビルド対象バイナリ（`culebra-ray` / `culebra-filament`）を変えるだけ。
  未使用バックエンドは usage-gate（`aot_uses_any_name`、master `860c466`）で非リンク＝コストゼロ。
- **作り込みの度合い**: 「差し替え可能な抽象」は実装1個からは正しく書けない。**契約（皮）は今かぶせるが、
  実装（下）は raylib 1 枚で素直に作る**。契約の穴は **2 個目（Filament）を載せて初めて検証される**。
  完璧主義で投機的な抽象境界を引かない（YAGNI）。
- マテリアルは **PBR-intent**（実装モデル名でなく意図を書く。後述）。
- 色は **RGBA（r,g,b,a; 0.0..1.0）**。アルファ必須。
- 入力は **held（`key_down`）と edge（`key_pressed`）の2系統**。
- **座標系を契約で1つに固定**: 右手系・Y-up・前方 -Z・角度ラジアン・回転 ZYX オイラー。
  各バインディングが内部変換する（固定しないと「差し替えたら鏡像世界」事故）。

## 抽出した契約

エントリは1オブジェクト `Graphics.View`（window + 3D scene + camera + 2D + input + frame を保持。名前は暫定）。
3D/2D オブジェクトは `Graphics.Node` / `Graphics.Node2D` ハンドルを返す。

### 3D（保持モード）

```
# プリミティブ（糖衣。本体は add_mesh）
add_box(w, h, d) -> Node                  # 角ばった箱。面取りは add_mesh で
add_sphere(r, segments) -> Node
add_cylinder(r, h, radial_segments) -> Node
add_plane(w, h) -> Node
# カスタム三角形メッシュ（トラック路面・車体 loft の本命）
add_mesh(vertices, indices, normals?, uvs?) -> Node    # 法線/UV はオプショナル
node.update_mesh(vertices, indices)       # 動的メッシュ（タイヤスモーク/デバッグ線/プール）

# マテリアル（PBR-intent — 実装モデルでなく意図）
make_material(base_color,
              metallic? = 0.0, roughness? = 0.8,
              emissive? = none, unlit? = false,
              texture? = none, double_sided? = false,
              writes_depth? = true, uv_scale? = 1.0) -> Material
node.set_material(mat)
material.set_base_color(c) / set_texture(tex)    # ランタイム差し替え（信号灯）

# ノード/変換/寿命
make_node() -> Node                       # 空コンテナ
node.set_position(x, y, z)
node.set_euler(x, y, z)                    # ラジアン, ZYX
node.set_rotation(ax, ay, az, angle)      # 軸-角（サスアーム）
node.set_scale(s) / set_scale3(x, y, z)
node.add_child(node)
node.set_name(s) / view.find(name) -> Node
node.set_casts_shadow(bool)
node.set_hidden(bool)
node.drop()                               # 確定 drop で GPU リソース解放（リタイア/ピット）

# カメラ（rig 計算は culebra 側＝レンダラ非依存。最終ポーズだけ渡す）
set_camera(eye_x,eye_y,eye_z, tgt_x,tgt_y,tgt_z, up_x,up_y,up_z, fov)

# 環境
set_background(texture)
set_fog(start, end, color, density_exp)
add_ambient_light(intensity, color)
add_directional_light(dir_x,dir_y,dir_z, intensity, color, casts_shadow) -> Node
```

### 2D オーバーレイ（保持土台 + 即時糖衣・スクリーン座標）

```
# 保持ノード（土台）— 位置/テキスト/可視を更新、毎フレーム作り直さない
add_text(s, x, y, size, color, align) -> Node2D   # align: left|center|right
add_rect(x, y, w, h, color, corner_radius?) -> Node2D
add_sprite(texture, x, y, w, h) -> Node2D
node2d.set_position(x, y) / set_text(s) / set_color(c) / set_alpha(a) / set_hidden(b) / drop()

# 即時糖衣（毎フレーム呼ぶ簡易版。保持ノードの薄いラッパ）
text(s, x, y, size, color, align)
rect(x, y, w, h, color, corner_radius?) / rect_stroke(...)
circle(x, y, r, color) / circle_stroke(x, y, r, color, line_width)
line(x0, y0, x1, y1, color, width) / polyline(points, color, width)
polygon(points, color)                    # 警告フラグの三角形
sprite(texture, x, y, w, h, alpha)
push_scale(s) / pop_scale()               # HUD 全体の UI スケール
```

### テクスチャ（CPU 生成）

SUZUKA のテクスチャは全て Core Graphics でコード描画（外部画像なし）。

```
texture_from_pixels(rgba_bytes, w, h) -> Texture
```

### フレーム / 入力 / ライフサイクル / capability

```
should_close() -> Bool
begin_frame(); render_3d(); ... 2D draw calls ...; end_frame()
dt() -> Float
key_down(key) -> Bool       # held
key_pressed(key) -> Bool    # 押下の瞬間（edge）
screenshot(path)
width() -> Long / height() -> Long
capabilities() -> { d2: Bool, pbr: Bool, shadow: Bool, bloom: Bool, ... }   # 後述
drop()                      # ~View で確定 teardown
```

## core と optional capability の線引き

「最も狭いバックエンドが天井」になるのを避けるため、**core は共通項**に限定し、高品質効果は
**optional capability**（未対応バックエンドは no-op）に分離。**未対応はサイレント no-op にせず、
`capabilities()` で問い合わせ可能にし、起動時に1回 warning**（「このバックエンドは 2D 未対応」等）。
サイレントに壊れるのが最悪。

| 区分 | 内容 |
|---|---|
| **core**（全バックエンド必須） | primitive / `add_mesh` / `update_mesh` / material(PBR-intent) / node(変換・寿命) / camera / 2D（保持+即時）/ fog / ambient+directional light / texture |
| **optional capability** | bloom・SSAO・DoF・exposure/saturation/contrast、影の細部（shadowMapSize 等）、面別マテリアル（box6面/cylinder3スロット） |

optional は `view.set_bloom(x)` 等で公開し、未対応なら no-op（`capabilities()` で判別可能）。

## 契約に入れない（呼び出し側 or 移植時に逃がす）

- **subdivisionLevel**（車体 loft の滑らか化）= アセット問題。まず loft のリング密度を上げて回避、後で
  culebra で Catmull-Clark 実装に上げる。**どちらでも `add_mesh` 契約は不変**。
- **面取り箱 chamfer** → `add_box` は角ばったまま。丸めたければ `add_mesh`。
- **Blinn-Phong の lighting model 名** → 契約に焼かない。PBR-intent に一度だけ翻訳（金属=metallic高/roughness低）。
- **面別マテリアル** → core は単一 `set_material`。複数スロットは optional、必要箇所だけノード分割。

原則: **プリミティブ4種は糖衣、`add_mesh` が本体、optional は no-op 許容**。

## バックエンド対応表（射程内のみ）

| 機能 | raylib | Filament | bgfx / sokol |
|---|---|---|---|
| ゲーム駆動ループ | ✓ | ✓（+GLFW） | ✓ |
| 3D core（mesh/primitive/node/camera） | ✓（即時→保持アダプタ） | ✓（ネイティブ保持） | ✓（要実装） |
| 2D オーバーレイ | ✓（**現状唯一の完動**） | ⬜（未解決ブロッカー、要 2D View） | 要実装 |
| 高品質（PBR/IBL/影/post-fx） | △（自前 shader で到達可） | ✓（SceneKit 級） | △ |
| CPU テクスチャ | ✓ | ✓ | ✓ |

→ 契約は射程内で共通。違いは「実装の成熟度」だけで、Filament の 2D ブロッカーは Filament バックエンドの
実装問題に局所化され、IF は不変（抽象が誠実）。`capabilities()` で利用側が対応状況を知れる。

## レンダラ非依存で culebra へ移植する塊（Graphics に依存しない）

SUZUKA の 16k 行の大半は描画と無関係。Graphics 契約が固まれば並行移植可能:

- **CameraRig**（233 行）= スムージング/プリセット/ドリー/オービットの純数学。最終 eye/target/fov を
  計算して `set_camera` に渡すだけ。
- **HUD のレイアウト/更新ロジック**（626 行）= 2D 保持ノード + 毎フレーム更新に書き換え。
- 物理 / AI / トラックデータ / レース進行 / 予選 = 完全にレンダラ非依存。

## 数量感（性能設計の参考）

- 車 ≈ 62 ノード/台（サス円柱 24 本が支配）→ 32 台で ≈ 2000 ノード。インスタンシング/簡略化候補。
- トラック ≈ 300–400 ノード（多くが原点固定の大型静的メッシュ。構築は一度きり、ホットパスは車の transform 更新）。
- raylib 実測（既存 PoC）: 27 台 0.234 ms/frame（予算 1.4%）。game-logic は非ボトルネック。

## 切り替え機構の詳細

- 各バックエンド = `culebra wrap` 拡張。バインディングが**同一の `Graphics` 名前空間・同一メソッド名**を登録。
- ゲームの `Graphics.View.new(...)` は、実行中バイナリに組み込まれた方に解決される。
- 正規化（即時↔保持、命名、座標/色/キーコード）は **C++ バインディング側**に置き、ゲーム側は不変。

## 次のステップ

1. 本契約を `wrap.h` の宣言として書き起こす（まず raylib バインディングを `Graphics` 形に。既に完動なので最短実証）。
   — **完璧主義をしない。raylib 1 枚で素直に作る = 2 個目を載せる準備運動**。
2. 契約越しに `racer3d` 相当が動くことを確認（interp/JIT 対称）。
3. CameraRig / 簡易 HUD を culebra へ移植し「契約だけでゲームが書ける」を実証。
4. **2 個目（Filament 3D）を載せて契約の穴を検証** → 必要なら再改訂。2D はブロッカー解消後。
5. SceneKit 固有（subdivision / chamfer）の翻訳方針を移植時に確定。

## 参照

- 移植元: `~/Projects/racing/Sources/SuzukaGP/`（CarBuilder/TrackBuilder/CameraRig/HUD/GameView/Palette）
- 既存 facade: `raylib-poc:examples/raylib/raylib_binding.cpp`（`Ray.Window`）、
  `filament-lib:examples/filament/filament_binding.cpp`（`Filament.Viewer`）
- usage-gate: master `860c466`（`include/runtime/aot_scan.h` の `aot_uses_any_name`）
