# Dynamic perform — plain fn から perform を書けるようにする

Branch: `feat/algebraic-effects`（tip `160a018`）で作業。master には戻さない。

## ゴール

**plain fn と effect fn を双方向に自由に呼び合えるようにする。**
① plain fn の中から `perform` を書ける ② plain fn から effect fn を呼べる
③ effect fn から plain fn（perform 込み）を呼べる。現状は performing する関数と
中継関数を全部 `effect fn` にする必要があり、DI/state/mock 用途で実用性がない
（marker のコストだけ払って追跡の恩恵がない）。

到達点：`effect fn` マーカーは **full-control（multi-shot / 非末尾 resume）を使う
ときだけ**の意味ある宣言に縮む。tail-resumptive/abort な DI・state・mock は
全部 plain fn で書け、`.map(plain_fn)` 越しでも perform が透過する。

## 設計方針：Koka 流 fun/ctl 分類ハイブリッド

handler clause を parse 時に3分類し、継続キャプチャ不要なものは動的ディスパッチ
に落とす。

1. **tail-resumptive**（`resume(v)` が clause 末尾に exactly-1）＝実用の9割
   → CPS を通さず、perform を「動的スコープ handler スタックから clause を引いて
   普通の関数呼び出し」に lower。native コールスタックがそのまま継続。
2. **abort**（`resume` を呼ばない）→ clause 実行結果を捕捉不能な panic 値で unwind、
   `handle` driver が受ける（ユーザーの try/catch を素通りさせる設計が要る）。
3. **full-control**（multi-shot / 非末尾 resume）→ 従来 CPS 経路のまま。
   plain fn からの perform が full-control handler に当たったら対称ランタイムエラー。

## Prior art / 位置づけ

先行研究・実装と突き合わせた結果、本計画の骨格は主流の実践と一致する。以下は
「なぜこの選択か」を後から再議論しないための位置づけ（web 調査で裏取り済み）。

- **3分類（tail / abort / full-control）= Effekt と同型**。Effekt は
  「tail-resumptive handler と abortive handler はどちらもヒープ確保を伴わない
  （継続を reify しない）」と明言。tail と abort を安い経路、full-control だけを
  継続 reify が要る高い経路に分けるのは標準的な切り方。
- **tail-resumptive = ただの関数呼び出し = Koka の evidence-passing 最適化**。
  「末尾 resume なら継続をキャプチャせず通常の関数呼び出しとして実行できる」は
  Koka が狙う中核最適化。本計画の「恒等 resume を差す」は
  "the operation computes its result and returns it directly" の一実装。
- **multi-shot を effect fn 宣言に閉じ込める = 健全性の常識と合致**。
  「multi-shot は可変参照があると標準的な変換・最適化を不健全にする（OCaml 5 が
  one-shot なのはこのため）」。full-control だけ明示 `effect fn` を要求し他を対称
  エラーにするのはこの慎重さと一致。

意識的に主流から分岐している点（トレードオフとして承知の上で採る）：

- **dispatch は O(n) の動的スタック探索（`_find`）。evidence passing は非ゴール**。
  Koka は handler を隠し引数で持ち回り O(1) ディスパッチにするが、これは型駆動
  コンパイルの大工事で、culebra の source-transform + 3 backend 対称 +
  leak-freedom の優先順位（性能より対称性・安全性）と相性が悪い。OCaml / Eff と
  同じ動的探索を承知で採る。
- **静的な effect 型を持たない → 未処理 perform は実行時 EffectError（OCaml 5 型）**。
  Koka/Eff/Effekt は row-polymorphic effect type で未処理をコンパイル時に拒否する
  が、culebra は effect row を持たないので実行時エラーが唯一の手段。
  [[project_error_detection_strategy]]（soundness 取り・completeness 諦め）と整合。
  **含意**：`plain fn から full-control handler へ perform` は Koka なら型で静的に
  弾ける誤りを実行時に落とす。「handler が偶然 multi-shot だった時だけ落ちる」
  ケースがありうる弱さは承知の上。

参照：Generalized Evidence Passing for Effect Handlers (Xie & Leijen, ICFP 2021)、
Effekt (effekt-lang.org)、OCaml 5 Effect Handlers manual、Retrofitting Effect
Handlers onto OCaml (Sivaramakrishnan)、Algebraic Handler Lookup in Koka/Eff/OCaml/Unison。

## 2軸モデル（実コードと照合済み・確定）

perform の lower と clause 分類は**別軸**。

```
【perform lowering 軸 — 静的・fn の色ごと】
  effect fn 内 → SUSPEND state machine（捕捉可能な継続）… 全 clause 種を扱える
  plain  fn 内 → __Eff._perform_direct(op, args)      … tail + abort のみ扱える

【clause 分類軸 — handle ごと・lower_handle で確定】frame に tag を載せる
  tail         → 恒等 resume、perform 地点へ普通に return
  abort        → unwind
  full-control → _perform_direct 経由(=plain fn)なら【対称ランタイムエラー】
                  state machine 経由(=effect fn)なら本物の継続で動作
```

**重要な発見**：handler clause の adapter は tail/full-control で**作り分け不要**。
adapter の形は両者で同一 `fn(_eh_args, _eh_resume){…}`。違うのは渡す `_eh_resume`
の値だけ：
- full-control（現状）: `resume = fn(v){ _drive(clone, v, ret) }`（本物の継続）
- tail-direct（新）: `resume = |v| v`（恒等＝native スタックが継続）

恒等 resume の注入点は **`_perform_direct` の1箇所だけ**。SUSPEND 経路では tail clause
でも本物の resume が必須（恒等を渡すと `_drive` が computation の残りを破棄する）。

## 呼び出し方向マトリクス（新設計後）

| 呼ぶ側 → 呼ばれる側 | 可否 | 理由 |
|---|---|---|
| plain → plain（perform 込み） | ✅ | direct perform が動的スコープ `_handlers` に届く（ゴール） |
| effect → plain（tail/abort perform） | ✅ | plain fn が `_step` 内で走り `_perform_direct` が `_find` |
| effect → effect | ✅ | 現状の DELEGATE 機構そのまま |
| plain → effect（tail/abort handler） | ✅ | **呼び出し地点ミニドライバ**（下記）で状態機械を回す |
| plain → effect（full-control handler） | ❌ | plain fn の native フレームはキャプチャ不能 → 対称エラー |

**呼び出し地点ミニドライバ（plain → effect の lower）**：effect fn の状態機械は
自分のフレームを自前でキャプチャしている。plain フレーム側でトランポリンを回し、
SUSPEND が来るたび `_find` で clause を引き、tail なら恒等 resume で値を差し戻して
継続、abort なら unwind。full-control clause に当たった SUSPEND だけ対称エラー
（handle と effect fn の間の plain native フレームは reify できないため）。

鍵となる物理制約：native コールスタックは継続をキャプチャできない。したがって
full-control だけは handle から effect fn チェーンで届く必要がある（OCaml 5 は
runtime fiber でこれを解くが、source-transform の culebra では原理的制約）。
tail/abort（実用の9割）は両方向とも自由に混在できる。

## 実コードで確認済みのシグネチャ・実装場所

| 要素 | 実際の形 | 場所 |
|---|---|---|
| `_find` | `_find(op, line)` → frame から adapter を返す（無ければ EffectError） | src/preambles/effects.cul:28 |
| `_drive` | `_drive(stack, rv, ret)` トランポリン | src/preambles/effects.cul:40 |
| adapter | `fn(_eh_args, _eh_resume) { let vp = _eh_args[i]; …; let resume = _eh_resume; <body> }` | include/effects_transform.h:1540 |
| adapter 呼び | `_drive` 内で `h(comp._eff_args, resume)`, `resume = fn(v){ _drive(clone, v, ret) }` | src/preambles/effects.cul:65-66 |
| perform emission | `cps_emit_suspension` が SUSPEND state 生成（effect fn computation 内でのみ） | include/effects_transform.h:879 |
| plain fn perform 拒否 | generic walk が PERFORM 到達＝未消費 → SyntaxError throw | include/effects_transform.h:158-166 |
| fn の色 | `collect_effect_fn_names` → `is_effect_call` | include/effects_transform.h:91,210 |
| clause 分類の挿入点 | `lower_handle` per-clause ループ、`resume_name`（1533）を使い body を slice 前に walk | include/effects_transform.h:1509-1542 |

## spike 手順（安い実証を先に、本実装はその後）

### spike 1：tail-resumptive direct perform の手 lower（半日以下）

`_perform_direct` を preamble に足し、DI の1例を手 lower して interp/JIT 両方で動かす。

貫通ターゲット：
```culebra
effect fn ask()
fn greet() {
  let name = perform ask()   # 手 lower: let name = __Eff._perform_direct("ask", [])
  "hi " + name
}
handle { greet() } with ask(resume) { resume("yuji") }   # → "hi yuji"
```

合格条件：
1. DI 例が interp/JIT で一致
2. `_sflag` 保存/復元込みで「effect fn → plain fn → perform」の混在チェーンが動く
3. tail clause body 内の bare effect-fn 呼びが両 backend で EffectError
4. no-handler 時の `e.line` が perform 元行（direct 経路でも）
5. **plain → effect（呼び出し地点ミニドライバの手 lower）**：plain fn から
   effect fn（body 内で perform する）を呼び、tail handler 下で interp/JIT 一致

### spike 2：abort 経路の unwind 最小実証

abort（resume を呼ばない）clause の非局所 unwind を、既存の Interrupted/panic 機構が
流用できるか最小実証。

**確認済み（2026-07-18）**：両 backend とも「未知の C++ 例外型は user catch を素通り
する」構造が既存。
- interp: `eval_try` は `Value`/`CulebraError`/`std::runtime_error` のみ捕捉、
  それ以外は `catch (...)` で **defer 実行後に再 throw**（interpreter.h:10243）。
- JIT/AOT: landingpad catch-all → `culebra_runtime_try_translate` で分類、
  carrier 無し（foreign）は flag 0 のまま **rethrow で unwinding 続行**
  （`try.notours`、jit.h:13006-13009）。scope cleanup は cleanup pad が担うので
  defer/drop は素通し経路でも走る。
→ したがって abort シグナルは「新規 C++ struct（`Value`/`runtime_error` 非派生）」で
実現可能。必要な native 追加は2点のみ：
1. throw 側: `__eff_abort(v)` builtin — `EffAbortSignal{Value}` を throw
2. catch 側: `_handle`/`_run_direct` が使う native ヘルパ — culebra thunk を呼び
   `EffAbortSignal` だけ捕捉して `{aborted, val}` 相当を返す
（native fn 追加のアンカーは [[project_add_stdlib_namespace]] の 7 アンカー + JIT slow path）

### spike 3（本実装、spike 1+2 が両方 green なら）

1. 文法：plain fn で perform 許可（`transform()` line 158-166 の throw を、effect fn
   でない文脈では `__Eff._perform_direct(op, args)` への lower に置換）。
   plain fn 内の effect-fn 呼び出しは `__Eff._run_direct(<state machine>)`
   （呼び出し地点ミニドライバ）に lower（bare-call EffectError を置換）
2. clause 分類：`lower_handle` に read-only walk を追加（`captures_outer` 同型）。frame を
   `op: adapter` → `op: {tag, fn: adapter}` に拡張。`_find`/`_perform_direct`/`_drive`
   が tag で分岐
3. transform 変更：plain fn perform には ANF/CPS 不要（suspension が無いただの関数呼び。
   `perform op(a+b, f(x))` → `__Eff._perform_direct("op", [a+b, f(x)])` に直接書換）
4. difftest Dim36 拡張 + docs §16（en+ja）更新 + memory 同期

## 実装時の要注意点（レビューで抽出）

- **`_sflag` 漏れ（実バグ候補）**：effect fn → plain fn → `_perform_direct` は `_step` 窓
  （`_sflag[0]==true`）内で走る。このまま adapter を呼ぶと adapter body 内の effectful
  effect-fn 呼びが DELEGATE 文脈と誤認され coloring-leak fix（`6d28b34`）が逆戻り。
  **fix**：`_perform_direct` は adapter 呼び出し前に `_sflag[0]` を保存して false にし、
  呼び出し後に復元する。
- **tail 判定は意味論的に必須（最適化でない）**：`resume(v) + 1` のような非 tail 1回でも
  direct 版は `v+1` を perform 地点に返し真の意味論と結果が変わる。分類は保守側に倒す：
  - `resume` の呼び出し以外の出現（alias `let r = resume`、引数渡し、クロージャ捕捉）→ full
  - 同名 shadowing（`let resume=…`、内側 fn の param）→ walk を境界で stop
    （`captures_outer` が CLASS_DECL で stop するのと同じ罠。雑にやると誤 tail 化＝silent wrong）
  - 全経路の末尾で exactly-1（if の両 arm 末尾は OK、loop 内は full 扱いが安全）
  - **バイアスは明確に full 側へ**：full に誤判定はエラー（安全）、tail に誤判定は silent wrong
- **handler clause body 内の perform も自動で解決**：reparse_expr は synth 全体（frame
  adapter 含む）を sub-EffectsLowerer に通す（include/effects_transform.h:1619-1621）ので、
  clause body 内の PERFORM が新設計で自動的に `_perform_direct` 化される。outer handler
  への re-perform（handler 合成の常用パターン）が追加実装ゼロで手に入る。
- **full-control への direct perform エラーの provenance**：`_perform_direct` にも
  `_eff_line` 相当を渡し、`e.line`＝perform 元行に。エラー文言は actionable に
  （「full-control handler への perform は effect fn の中でしか使えない、fn を effect fn にせよ」）。

## ビルド/テスト

- 内側ループ：`CCACHE_DIR=$TMPDIR just dev`
- 段階ゲート：`just test-dev` → `tools/difftest/run.sh ./build-dev/culebra`(interp==jit)
  → `just test aot` → `tools/difftest/leak.sh ./build-dev/culebra`(no new / baseline 10)
- ゲートの Bash は `dangerouslyDisableSandbox: true`

## 現在地

- done: 設計確定・実コード照合・呼び出し方向マトリクス確定・レビュー・先行研究照合完了
- 未着手: spike 1。まだ1行も書いていない
