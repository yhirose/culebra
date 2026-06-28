# `Graphics` API スケッチ — 使い勝手を「書いて」検証する

目的: culebra 側 IF の**使いやすさを最優先**に設計する。そのために、まず SUZUKA の実シーン
（車・トラック・カメラ・HUD・メインループ）を `Graphics` で**実際に書き**、読みやすさを確かめてから
API シグネチャを逆算する。バックエンドは raylib 主軸（Filament は将来の選択肢）。

設計の駆動原則（SceneKit のライブラリ設計レビューより）:
- **最頻の操作を真の1行に**（SceneKit はアプリに `add()`/`mat()` ヘルパを書かせていた → 言語機能で消す）。
- **kwargs + デフォルト**で「生成して後から代入」を一発に。
- **フルーエント連鎖**（変換系メソッドはノード自身を返す）。
- **親が子を生む**（`parent.add_box(...)` = create+addChild を1手に）。
- **色/テクスチャは型で区別**（SceneKit の `.contents = Any` を避ける）。
- **確定 drop** で後始末順序の地雷を解消。

---

## 1. メインループ（全体の骨格）

```culebra
let view = Graphics.View.new(960, 600, "SUZUKA 1987")
view.target_fps(60)

build_environment(view)
let track = build_track(view)
let car   = build_car(view, Team.williams.color)

while !view.closing() {
  let dt = view.dt()

  # --- ゲームロジック（レンダラ非依存・別ファイル）が車の位置と姿勢を更新 ---
  car.update(view, dt)                      # CarPhysics 由来。Graphics に依存しない
  car.node.move(car.x, car.y, car.z).yaw(car.heading)
  car.lean.roll(car.lean_angle)             # コーナリングで車体が傾く

  # --- 追走カメラ（純数学。CameraRig 由来）---
  view.camera(eye: car.chase_eye(), look_at: car.chase_target(), fov: car.cam_fov)

  # --- 描画: 3D（保持シーンを一括）→ 2D オーバーレイ（即時）---
  view.render_3d()
  draw_hud(view, car)
  view.present()
}
view.drop()                                 # ~View が確定 teardown（順序はバインディングが保証）
```

読みどころ: フレームは `render_3d()` → 2D → `present()` の3手。3D は保持なので毎フレーム組み直さず、
動いたノードの `move/yaw` だけ更新する。

---

## 2. 車（合成的シーングラフ + プリミティブ + メッシュ）

SceneKit の `CarBuilder.add()`/`mat()` ヘルパが不要になることを確認する。

```culebra
fn build_car(view, team_color) {
  # root は yaw だけ、その子 lean が roll/pitch（SceneKit の良い合成パターンを踏襲）
  let car  = view.add_node().name("car")
  let lean = car.add_node().name("lean")

  # マテリアルは一度作って使い回す（PBR-intent: 色 + metallic/roughness）
  let dark = view.material(color: rgb(26, 26, 26), roughness: 0.6)
  let body = view.material(color: team_color,      metallic: 0.3, roughness: 0.4)
  let tire = view.material(color: rgb(14, 14, 14), roughness: 0.85)

  # 平らな床: 最頻の「色付き箱を置く」が本当に1行
  lean.add_box(1.32, 0.06, 2.75, material: dark).move(0, 0.09, 0.32)

  # ロフトした車体（カスタム三角形メッシュ。頂点は CarBuilder の profile 由来）
  lean.add_mesh(loft(profile_body(team)), material: body)

  # コックピット + ヘルメット（球は segments を kwarg で）
  lean.add_box(0.52, 0.08, 0.66, material: dark).move(0, 0.58, -0.42)
  lean.add_sphere(0.16, segments: 16, material: view.material(color: rgb(220, 30, 30)))
      .move(0, 0.66, -0.40)

  # 4 輪（円柱の軸を X に倒す。ループ + フルーエントで簡潔に）
  for sx in [-1.0, 1.0] {
    for sz in [-1.0, 1.0] {
      lean.add_cylinder(0.34, 0.30, segments: 32, material: tire)
          .move(sx * 0.75, 0.34, sz * 1.10)
          .spin(0, 0, 1, Math.pi / 2.0)        # 軸-角でホイールを立てる
    }
  }
  Car.new(car, lean)                            # ハンドルをゲーム側クラスに包む
}
```

**SceneKit との比較**（同じ「床1枚」）:
- SceneKit 生: マテリアル5行 + ノード生成/parent 4行 = 9行、しかも `add()`/`mat()` ヘルパ前提。
- `Graphics`: `lean.add_box(1.32, 0.06, 2.75, material: dark).move(0, 0.09, 0.32)` の **1行**。

---

## 3. トラック（カスタムメッシュ中心・静的）

```culebra
fn build_track(view) {
  let asphalt = view.material(color: rgb(60, 62, 66), roughness: 0.9)
  let grass   = view.material(color: rgb(44, 120, 58), roughness: 1.0)
  let white   = view.material(color: rgb(240, 240, 240), unlit: true)   # 遠景で溶けない白線

  # 地面ハイトフィールド・路面リボン・白線は全てワールド座標で焼いた add_mesh
  view.add_mesh(ground_field(),       material: grass).name("ground")
  view.add_mesh(course_ribbon(),      material: asphalt).name("asphalt")
  view.add_mesh(centerline_strip(),   material: white).name("lines")

  # スタート信号の 3 灯（名前で引いてランタイムにマテリアル差し替え）
  for i in 0..3 {
    view.add_cylinder(0.3, 0.1, segments: 16, material: lamp_dim(i))
        .move(grid_x(i), 8.0, grid_z()).name("lamp{i}")
  }
  Track.new()
}

# レース開始時: 赤→緑（material をハンドルで差し替え。stringly な find は補助に留める）
fn set_start_signal(view, state) {
  if state == "green" { view.find("lamp2").set_material(lamp_lit(2)) }
}
```

`add_mesh` は頂点群を受け取るだけなので、subdivision/chamfer の有無は呼び出し側の自由＝**契約は不変**。

---

## 4. 環境・ライト

```culebra
fn build_environment(view) {
  view.background(sky_texture())
  view.sun(dir: (0.5, -1.0, -0.6), intensity: 1.0, color: rgb(255, 250, 240), shadow: true)
  view.ambient(intensity: 0.4, color: rgb(180, 200, 220))
  view.fog(start: 200.0, end: 2000.0, color: rgb(120, 140, 160), density: 1.0)

  # optional capability（未対応バックエンドは no-op、capabilities() で判別可）
  view.bloom(0.5)
  view.ssao(true)
}
```

---

## 5. HUD（2D オーバーレイ・即時糖衣）

毎フレーム再計算する HUD は即時スタイルが素直に読める。位置を保持して更新したい要素（ミニマップ車点）は
保持ノードにもできる（後述）。

```culebra
fn draw_hud(view, car) {
  let w = view.width()
  let h = view.height()
  let white = rgb(245, 245, 245)

  # スピード / ギア / ラップ（左下〜左上）
  view.text("{car.kmh} KM/H", 30, 52,     size: 44, color: white)
  view.text("GEAR {car.gear}", 34, 22,    size: 20, color: rgb(200, 200, 200))
  view.text("LAP {car.lap}/{car.total}", 30, h - 36, size: 24, color: white)

  # タイミングタワー背景（半透明 rect）+ 行
  view.rect(20, h - 112, 262, car.tower_h, color: rgba(0, 0, 0, 150), radius: 4)
  for row in car.tower_rows {
    view.rect(20, row.y, 7, 17, color: row.team_color)               # チームチップ
    view.text(row.text, 36, row.y, size: 16, color: row.text_color, align: "left")
  }

  # ミニマップ（輪郭 polyline + 車の点 circle）
  view.polyline(car.map_outline, color: rgba(255, 255, 255, 217), width: 3.0)
  for dot in car.map_dots {
    view.circle(dot.x, dot.y, dot.r, color: dot.color)
  }

  # 一時メッセージ（中央・任意アルファ）
  if car.message != "" {
    view.text(car.message, w / 2, h * 0.68, size: 34, color: rgba(222, 200, 140, car.msg_alpha), align: "center")
  }
}
```

保持版が要るとき（フォント再ラスタライズを毎フレーム避けたい等）:

```culebra
# 構築時に一度
let speed = view.add_text("", 30, 52, size: 44, color: white)
# 毎フレーム更新だけ
speed.set("{car.kmh} KM/H")
```

---

## 逆算した API シグネチャ

上のコードが読みやすく書けることから、以下の表面を採用する。**kwargs はすべてデフォルト付き**。

### View（エントリ）
```
Graphics.View.new(w, h, title) -> View
view.target_fps(n)
view.closing() -> Bool
view.dt() -> Float
view.render_3d()                 # 背景クリア + 保持シーン描画
view.present()                   # 2D 確定 + スワップ
view.width() -> Long / height() -> Long
view.held(key) -> Bool           # 押しっぱなし
view.pressed(key) -> Bool        # 押した瞬間
view.screenshot(path)
view.find(name) -> Node          # 補助的なランタイム検索
view.drop()
```

### シーン構築（View / Node が子を生む）
```
view.add_node() / node.add_node() -> Node
view.add_box(w, h, d, material?) / node.add_box(...) -> Node
view.add_sphere(r, segments?, material?) -> Node
view.add_cylinder(r, h, segments?, material?) -> Node
view.add_plane(w, h, material?) -> Node
view.add_mesh(mesh, material?) -> Node          # mesh = Graphics.Mesh（頂点/索引/法線?/UV?）
node.update_mesh(mesh)                           # 動的メッシュ
```

### Node（フルーエント = self を返す）
```
node.move(x, y, z) -> Node
node.yaw(rad) / node.roll(rad) / node.pitch(rad) -> Node     # 単軸の糖衣
node.euler(x, y, z) -> Node                                  # ZYX 一括
node.spin(ax, ay, az, angle) -> Node                         # 軸-角
node.scale(s) / node.scale3(x, y, z) -> Node
node.material(m) / node.set_material(m) -> Node
node.name(s) -> Node
node.shadow(bool) -> Node
node.hide() / node.show() -> Node
node.add_child(node) -> Node
node.drop()
```

### マテリアル / 色 / メッシュ / テクスチャ
```
view.material(color:, metallic? = 0.0, roughness? = 0.8, emissive? = none,
              unlit? = false, texture? = none, double_sided? = false) -> Material
material.set_color(c) / set_texture(tex)
rgb(r, g, b) -> Color            # 0..255（ゲーム開発に馴染む）
rgba(r, g, b, a) -> Color        # a も 0..255
Graphics.Mesh.new(verts, indices, normals?, uvs?) -> Mesh   # verts/normals = [(x,y,z)...], uvs = [(u,v)...]
view.texture_from_pixels(rgba_bytes, w, h) -> Texture
```

### カメラ / 環境 / ライト
```
view.camera(eye:, look_at:, up? = (0,1,0), fov? = 55)          # eye/look_at = (x,y,z) タプル
view.background(texture)
view.fog(start:, end:, color:, density? = 1.0)
view.ambient(intensity:, color:)
view.sun(dir:, intensity:, color:, shadow? = false) -> Node
```

### 2D（即時糖衣 + 保持ノード）
```
view.text(s, x, y, size:, color:, align? = "left")
view.rect(x, y, w, h, color:, radius? = 0)
view.rect_stroke(x, y, w, h, color:, width:, radius? = 0)
view.circle(x, y, r, color:) / view.circle_stroke(x, y, r, color:, width:)
view.line(x0, y0, x1, y1, color:, width:)
view.polyline(points, color:, width:)        # points = [(x,y)...]
view.polygon(points, color:)
view.sprite(texture, x, y, w, h, alpha? = 255)
# 保持版
view.add_text(s, x, y, size:, color:, align?) -> Node2D
node2d.set(s) / move(x, y) / color(c) / alpha(a) / hide() / show() / drop()
```

### optional capability
```
view.bloom(strength) / view.ssao(bool) / view.fxaa(bool) / view.dof(...)
view.capabilities() -> { d2:, pbr:, shadow:, bloom:, ssao:, ... }
```

---

## このスケッチで検証できたこと

- 車体（CarBuilder 807 行の SceneKit）が、`Graphics` では **1 部品 = 1 行**で書ける。`add()`/`mat()` ヘルパ不要。
- 合成シーングラフ（root=yaw / lean=roll）はそのまま自然に表現でき、毎フレームは `move/yaw/roll` だけ。
- HUD は即時で素直、更新頻度が高い要素だけ保持ノードに逃がせる（二層の正しさが実例で確認できた）。
- kwargs + デフォルトで「色だけ」「サイズだけ」指定が効き、初心者も最小記述で書ける。

## probe 検証結果（2026-06-26・実証済み）

`examples/gfx/probe/`（最小バインディング + `.cul`）を `culebra wrap` で拡張バイナリ化し、
**interp と --jit で出力一致**を確認した。結果:

| 機構 | 判定 | 根拠 |
|---|---|---|
| **kwargs（名前付き・逆順）+ 位置引数** | ✅ 動く | `config(7,3)` == `config(a:7,b:3)` == `config(b:3,a:7)` == 703、両 backend 一致 |
| **親が子を生む → 使えるハンドル返却** | ✅ 動く | `view.add_box(2,3,4)` の handle で `sx/sy/sz` 取得 OK |
| **フルーエント連鎖（self 返却）** | ✅ 動く・**新機能不要** | `m.move(1,2,3).tint(42)` が**元の m を変更**（sx=1,col=42）。既存の `borrowed_method`（`T&` を借用ハンドルで返す）でそのまま実現。両 backend 一致 |
| **タプル引数** `eye:(x,y,z)` | ❌ 非対称 | `ValueAs<Value>` は interp のみ、JIT `jit_arg_get` が `Value` 非対応。→ **平坦スカラ引数に落とす** |
| **デフォルト引数**（ラップメソッド） | ❌ 未対応 | `typed_params` が default を設定しない（`Parameter.default_value` 機構自体はあるが wrap が使っていない）。→ **optional は fluent setter で表現** |

### 確定した2つの設計調整（スケッチからの差分）

1. **タプルでなく平坦スカラ**: `camera(ex,ey,ez, tx,ty,tz, ux,uy,uz, fov)`、メッシュ頂点は flat 配列
   `[x,y,z, x,y,z, ...]`（大量頂点で速度も有利）。
2. **optional kwargs でなく fluent setter**: デフォルト引数が使えないので、必須コア引数 + 任意は連鎖 setter。
   - `view.material(rgb(26,26,26)).roughness(0.6)`（色は必須、残りは連鎖）
   - `lean.add_box(1.32, 0.06, 2.75).material(dark).move(0, 0.09, 0.32)`
   - 「デフォルト」＝ setter を呼ばないこと。chaining が動くと実証済みなので、この形が最も一貫する。

→ いずれも**既存の wrap.h 機能だけで成立**し、使い勝手は維持される（むしろ一貫性が上がる）。
色は `rgb()` 0..255 コンストラクタを採用（内部 0..1 に正規化）。

### 任意の将来拡張（必須ではない）

- wrap メソッドに**デフォルト引数**を足す（`Parameter.default_value` は既存・binder も尊重するので、
  `typed_params` が default を載せれば可能。ただし JIT 側 kwarg 束縛の対称対応が要る）。これがあれば
  fluent setter でなく `add_box(w,h,d, material: dark)` も書けるが、現状は不要。

## 次のステップ

1. 上記2調整を反映して `Graphics` シグネチャを確定（DESIGN.md と統一）。
2. `raylib_binding.cpp` を `Graphics` 形で実装（`borrowed_method` で連鎖、平坦スカラ、fluent setter）。
3. スケッチの車・トラック・HUD を `.cul` 化して動かし、読みやすさと interp/JIT 対称を確認。
