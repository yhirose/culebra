# JIT GC 書き直し — 設計仕様（保守的・世代別・非移動）

Status: **Shipped (2026-05-30).** JIT GC は手動参照カウントに、保守的・非移動
の mark-sweep バックストップを加えたものである。

本ドキュメントは正準たる原典 `jit_gc_design.md` の日本語ミラーであり、常に
同期を保つこと。RC・GC・確定的 `drop` が今日実際にどう振る舞うかの正準記述は
[`gc_model.ja.md`](gc_model.ja.md) であり、両者が食い違う場合は
`gc_model.ja.md` が勝つ。

本ドキュメントは権威ある仕様書であり、**安全性とコードの単純さを最優先**し
（性能は後回し）、フェーズごとに実装可能な形で書かれている。

---

## 1. 動機

現在の JIT は **手動参照カウント** を行っている。codegen があらゆる所有権
境界（代入・呼び出し引数・メソッドレシーバ・演算子オペランド・スコープ
退出）で `emit_value_retain` / `emit_value_release` を発行する。これがバグ
の一群の温床である:

- **リーク**: release が欠けているとき（メソッドレシーバ、演算子
  オペランド、ループ本体の値、…）。
- **二重解放**: 値が既に消費済みの箇所に release が追加されたとき（所有権
  規約が呼び出し境界をまたいで *不均一* である — 演算子オペランドでさえ
  不整合であることが判明した）。

手動 RC はまた **参照サイクル** を回収できず、トレーシングのバックストップ
なしでは、release の取りこぼしやサイクルは **永続** する — プロセスの生存
期間ずっと残る（microgpt JIT は ~5 GB まで膨らんだ）。バックストップは
これらを、最悪でも遅延回収へと変えるものである。

**インタプリタにはこれらの問題が一切ない**。`shared_ptr`（C++ デストラクタ
による *自動かつ厳密* な参照カウント）を使うからである。interp が安全性の
基準線だ。JIT は `shared_ptr` のアトミック参照カウントのオーバーヘッドを
避けるために厳密 RC を捨てて手発行の RC に移ったが、それを (a) 厳格に強制
された所有権規約でも (b) トレーシングのバックストップでも補わなかった。

**確立されたランタイムとの比較**（commit 履歴 / memory のサーベイ参照）:

- CPython/PHP: RC + **バックアップのトレーシング** コレクタ → リークは
  自己修復する。
- Java/Go/V8/Lua: **トレーシング** GC、手動 RC なし。ルートはスタック
  マップ/safepoint あるいは保守的スキャンで得る。
- Swift: コンパイラ挿入の RC を **OSSA** で検証（+0/+1 型付け）。
- Ruby/Boehm: **保守的スタックスキャン** → C 拡張のローカルは自動 root 化
  され、手動 rooting は一切不要。

> **所有権の規律は別文書で規定される**:
> [`jit_ownership.ja.md`](jit_ownership.ja.md)。すなわち、リークはサイトごと
> にパッチするのではなく **構造的に不可能** にせよ（RAII / Rust 流）という
> 恒久ルールと、そこへ至る層状設計（均一規約 → `Owned` RAII ハンドル →
> スコープスロットへの escape → GC 所有の rooting → サイクルバックストップ）
> である。本ドキュメント（GC 仕様）はトレーシング/バックストップとヒープ
> モデルを担い、あちらは *誰が・いつ release するか* を担う。両者を合わせて
> 読むこと。

## 2. 決定

> **決定を改訂 2026-05-30 — 純トレーシングではなく CPython 流ハイブリッド。**
> 下記の元の純トレーシング決定は *破棄* された。実装（Phase 1 の commit
> 1–2）と設計レビューにより、元案が見落としていた強い制約が浮上した:
> **`drop` はスコープ退出時に発火する確定的ファイナライザである**（interp は
> `shared_ptr` 経由でこれを発火し、`test_drop` が順序を assert し、かつ
> クロスバックエンド対称性ルールが JIT にも一致を要求する）。**共有所有権の
> 下での確定的ファイナライズは論理的に完全な参照カウントを要求する** —
> 非 drop コンテナ内に保持された drop 持ちオブジェクトが確定的に `drop` を
> 発火できるのは、そのコンテナの死も確定的なときだけであり、それは *あらゆる*
> オブジェクトが参照カウントされることへと再帰する。「drop オブジェクトだけ
> RC する」はコンテナ保持のオブジェクトに対して非確定的なので成立しない。
>
> したがって: **手動 RC を維持し**（メモリ + 即時再利用 + 確定的 `drop` を
> 担う）、**健全・保守的・非移動の完全 mark-sweep コレクタを *バックストップ*
> として追加する** — RC が回収できない残渣、すなわち参照 **サイクル** と
> **release の取りこぼし** で漏れたオブジェクト（いずれも root から到達不能
> になるので、完全 mark-sweep は陳腐な refcount によらず解放する）を回収する。
> これが CPython / PHP モデルである（§1 の最初のサーベイ項目）。
>
> **価値提案:** RC が一次メモリマネージャのままで、保守的 mark-sweep
> バックストップは RC が回収できない残渣だけを回収する。これにより RC バグは
> *永続リーク* から *わずかに遅延した回収* へと格下げされ、microgpt ~5 GB の
> 一群が解消する。
>
> **§7 は達成ではなく訂正される:** 「手動 RC を全削除」は確定的 `drop` と
> 論理的に両立しない。retain/release の機構は残る。変わるのはコレクタである。
>
> **レイアウト（plan A）:** `int64_t refcount` は struct オフセット 0 に
> 留まる（既存の retain/release IR は無傷）。コレクタのオブジェクト毎メタ
> データ（mark ビット・type タグ・サイズ）は **registry**（address→metadata
> マップ）に置き、オブジェクト内ヘッダには置かない — 直感に反するが、92 の
> emit サイトを末尾フィールドに書き換える代わりに元の retain/release IR を
> そのまま復活させるので、*より小さい* ロールバックである。後の perf フェーズ
> では mark ビットを末尾のオブジェクト内フィールドに移す **可能性がある**
> （オフセット 0 は `refcount` のままなので IR は依然無傷）。
>
> **ファイナライズ不変条件（確定的 drop 作業に合わせて更新）:**
> **`drop` はオブジェクト毎に厳密に 1 回、オブジェクト毎の `dropped` フラグで
> 重複排除された 4 経路の union から発火する** — (a) RC の release-to-zero、
> (b) 明示的な `obj.drop()`、(c) スコープ退出時の owned-region 解決
> （`owned_scope_exit` の試行削除）、(d) GC バックストップの **pre-sweep
> finalize パス**（`_jit_gc_finalize_dead`、PEP-442 流: dead set の `drop` を
> 構造がまだ無傷のうちに走らせ、その後 sweep が再発火せずにメモリを回収する
> — interp のミラー: clear カスケード前の `_owned_gc_backstop`）。したがって
> **サイクルメンバも `drop` を発火する**（(c) のスコープ退出、または (d) の
> 回収時に）。*sweep 自体* は依然として `drop` を決して発火しない — finalize
> は別個の pre-sweep パスである。本ドキュメントの以前の版は「サイクルメンバは
> drop を発火しない」と述べていたが、それは確定的 drop フェーズより前のもの
> であり、Shipped コードに対しては誤りだった（現在のチョークポイントは
> `jit_ownership.ja.md` と jit.h の `_culebra_call_drop_if_present` を参照）。
>
> 以下の各節（オブジェクトモデル・registry ヒープ・保守的 root スキャン・
> mark-sweep・安全装置）は依然として当てはまる — それらは **バックストップ
> コレクタ** を記述している。「the collector」は「唯一のメモリマネージャ」
> ではなく「バックストップ」と読むこと。Phasing（§13）と「手動 RC 削除」
> （§7）が本改訂で書き換わる部分である。

--- *元の（破棄された）決定が以下に続く* ---

JIT の手動 RC を **保守的・世代別・非移動の mark-sweep トレーシング GC**
（Ruby MRI / Boehm モデル）で置き換える。

根拠（安全性 + 単純さを優先）:

- **どこでも手動 acquire/release なし。** 生成コードと C++ ランタイム関数は
  値をマシンスタック上に保持するだけで、コレクタはスタック + レジスタを
  スキャンして root を見つける。これは上記のバグの一群全体を — C++ builtin
  内の一時値（レシーバ/オペランドのリーク）も含めて — タダで消し去る。
  ちょうど Ruby C 拡張が手動 rooting を必要としないのと同じである。
- **最小リスクの統合。** JIT は既に値をスタックスロットに保持している。
  機構を追加するのではなく、retain/release の発行を主に *削除* する
  （shadow stack なし、statepoint なし）。
- **安全側の失敗方向。** ポインタの誤認識は over-retain（64bit 上では有界で
  稀なリーク）にとどまる — メモリを破壊することは決してない。root の
  *取りこぼし* が use-after-free になる精密 rooting とは対照的である。

**非ゴール（現時点）:** 移動 / コンパクション / コピー nursery。これらは
精密 root を必要とする。それらの移行先は **LLVM Statepoints
(`gc.statepoint`)** — §12 参照。最初の書き直しでは意図的に非移動の free-list
ヒープを受け入れる。

**スコープ:** JIT ランタイムのみ。インタプリタは `shared_ptr` モデルを維持
（既に安全）。クロスバックエンドの共有オブジェクト（例: Tensor）は既存の
ハンドル境界で動き続ける。§11 参照。

## 3. オブジェクトモデル

GC は参照カウントされる 6 つのヒープ struct を管理する: `JitObject`,
`JitArray`, `JitCell`, `JitClosure`, `JitSet`, `JitTensor`。

各々に **GC ヘッダ** を付ける（現在の `int64_t refcount` 先頭フィールドを
置き換える — refcount は完全に削除される）:

```
struct GcHeader {
  uint8_t  mark;       // mark bit (+ colour for future incremental)
  uint8_t  type_tag;   // GC_TAG_* — drives enumerate_children & dtor
  uint8_t  generation; // 0 = young, 1 = old (Phase 2)
  // padding; total kept at 8 bytes so existing IR GEP offsets shift
  // uniformly. (Codegen field offsets are regenerated, not hand-kept.)
};
```

**可変長バッファは GC 割り当てではなく C++ 所有のまま。**
`JitArray::items`、`JitObject::slots`（`std::vector`）、`AnyKeyMap`、
`key_order`、closure `captures` などは、それぞれの struct が所有し続け、
sweep 時に struct の **C++ デストラクタ** によって解放される（RAII）。GC は
固定サイズの struct だけを追跡し、その中身（子の `JitValue`）はマーク時に
`enumerate_children`（既に存在し、そのまま再利用される）経由で到達する。

これにより GC は単純（均一な固定ノード）に保たれ、sweep のクリーンアップが
自動になる（`~JitArray()` が `items` を、`~JitObject()` が vector とサイド
カーを解放する — 手書きの解体不要）。

## 4. ヒープと割り当て

**単純さ優先: registry ヒープから始め、最適化は後。** アロケータは
*`gc_alloc` / `is_gc_object` の実装詳細* であって GC アルゴリズムの一部では
ない — だから極めて単純に始められ、mark/sweep/root に触れずに最適化できる。

- **Phase 0/1 — registry ヒープ。** `gc_alloc` = `malloc(header+payload)` +
  オブジェクトポインタを registry（`address → {type_tag, size}`）に記録。
  `is_gc_object(p)`（§5）= registry が `p` をオブジェクト開始として含む。
  自明に正しく、page/bitmap/size-class なし。最小のコードで root・マーク・
  sweep・安全ツールを検証できる。（sweep は registry 経由で解放。）
- **Phase 2a — flat-array registry（DONE）。** 最初の registry は
  `std::unordered_map<void*, GcHeader>` を使い、そのオブジェクト毎ノードの
  malloc/free が alloc-churn オーバーヘッド全体として計測された（object/array
  の churn で +12–21%、現実的なワークロードは影響なし）。これを `GcRegistry`
  — `{key, header}` をインラインに格納するオープンアドレッシングの flat hash
  map（エントリ毎割り当てなし、tombstone 再利用）— に置き換え、オーバー
  ヘッドを回収した（churn 1.21×→1.02×）。変更したのは
  `adopt`/`forget`/`find`/iteration だけで、mark/sweep/root は不変。plan A の
  下でもランタイムは依然 struct を `new` する（RC がメモリを所有）。registry
  はサイドテーブルである。
- **Phase 2b+ — size-class / region アロケータ（将来、計測された場合のみ）。**
  後に割り当てスループットがより必要だと示されたら、非移動の size 分離 /
  Immix 流の bump-region アロケータ（Go / JSC モデル）が次の一手 — これも
  `gc_alloc`/`is_gc_object`/iteration を差し替えるだけである。
  **移動 / コピーはこの経路に意図的に載せない**（§12 参照）。

`gc_alloc(size, type_tag)` は今日の `new JitObject()` などを置き換える
ランタイムエントリで、暗黙に C++ デストラクタを呼ぶことは決してない。割り当て
が GC のトリガ地点である。

## 5. root 探索（保守的）

回収時、コレクタは、ポインタとして読んだときに GC page 内の有効なオブジェクト
開始に着地する任意のマシンワードを **候補 root** として扱う（page 割り当て
bitmap + size-class アラインメントで検証）。候補ワードの発生源:

1. すべての mutator スレッドの **マシンスタック**。回収地点の SP から
   スレッドの記録済みスタック base まで。これは JIT 生成フレームと C++
   ランタイムフレームの両方をカバーする — だから任意の C++ builtin ローカル
   に保持された `JitValue` が自動で root 化される。
2. **callee-saved + 全レジスタ**。回収エントリでスタックへフラッシュされ
   （register-flush ルーチン / `setjmp` 流の spill 経由）、スタックの一部
   としてスキャンされる。
3. **明示的なグローバル root**。GC に登録される: namespace オブジェクト
   テーブル、モジュールキャッシュ、REPL グローバル、例外キャリア、defer
   スタック — スキャン対象スタックの外で `JitValue` を保持するもの全て。
   （Ruby の `rb_gc_register_address` の類似物。）

**正しさの論拠（なぜ最適化された JIT コードが安全か）:** GC を起こしうる
呼び出しをまたいで生きている GC ポインタは、プラットフォームの呼び出し規約に
より、呼び出し前にスタックへ spill されるか callee-saved レジスタに保持されて
いなければならない — caller-saved レジスタは保存されない。GC が走る瞬間
（呼び出し連鎖の奥深く `gc_alloc` 内）には、そうした値はすべてマシンスタック
上かフラッシュされた callee-saved レジスタ内にあり、(1)+(2) で見つかる。
これがまさに Boehm/Ruby の保守的 GC が最適化コンパイラの下で健全である理由
である。

**表現に関する注記。** `JitValue` は `{int8 tag, int64 data}` である。
スキャナは tag を参照せず、各 8 バイトワードをテストするので、ヒープ `data`
ポインタは tag によらず見つかる。非ポインタスカラー（ビットがたまたまヒープ
アドレスと一致する `Long`）は *偽* root になりうる — 有界な over-retention で
許容範囲。`data` は常にオブジェクトの base を指す（内部ポインタなし）ので
検証が単純化される。

## 6. Collect: mark & sweep

**Mark:** root 集合から推移的にトレースする。各生存オブジェクトについて
mark ビットを立て、その子（既存の `enumerate_children(ptr, type_tag, out)`
で得る）を mark スタックにプッシュする。固定点まで反復する。保守的 root は
pin される（非移動なので更新するものは何もない）。

**Sweep:** 全 page を走査。マークされていない各オブジェクトについて、その
C++ デストラクタを走らせ（`~JitObject` など — `items`/`slots`/サイドカーを
RAII で解放）、スロットを free list に戻す。全 mark ビットをクリアする。

実 root からの完全 mark-sweep は **どんな陳腐な帳簿によらず到達不能な
オブジェクトを回収する** — これが現行コレクタに欠けている性質であり、今日
リークが永続する理由である。

## 6a. ファイナライズ（`drop`）— 確定的、RC が所有（CPython モデル）

**決定（2026-05-30; §2 の改訂バナー参照）。** `drop` は **スコープ退出時に
発火する確定的** ファイナライザのままである — interp は `shared_ptr` 経由で
発火し、JIT は一致しなければならない（クロスバックエンド対称性ルール;
`test_drop` が親→子の順序を assert する）。共有所有権の下での確定的ファイナ
ライズは **完全な参照カウントを要求する**（非 drop コンテナ内に保持された
drop 持ちオブジェクトが確定的に `drop` を発火するのは、コンテナの死も確定的
なときだけ — それは全オブジェクトへと再帰する）。したがって:

- **RC がメモリ + `drop` を所有。** retain/release は残る。release-to-zero が
  `drop` を（1 回）発火し、C++ 解体（`items`/`slots`/サイドカー/`impl` を
  解放）を走らせ、オブジェクトを de-register する — まさに現行の振る舞い。
  `drop` のタイミングは interp の `shared_ptr` のタイミングである。
- **コレクタはバックストップであってメモリの所有者ではない。** RC が回収
  できないもの、すなわち **サイクル** と **release 取りこぼし** で漏れた
  オブジェクトだけを回収する。これらは root から到達不能なので、完全
  mark-sweep は陳腐な refcount によらず解放する。
- **sweep 自体は `drop` を決して発火しない — が pre-sweep finalize パスは
  発火する。** `_jit_gc_finalize_dead`（PEP-442 流、Python の post-PEP-442
  における「サイクルメンバへの `__del__`」ルールを反映）は回収毎に 1 回、
  無傷の dead set を走査してすべての死んだ Object に対し `drop` を発火する。
  その後 sweep が再発火せずにメモリを回収する。interp のミラー: clear
  カスケード前の `_owned_gc_backstop`。（以前の版はサイクルメンバの `drop`
  はスキップされると述べていたが、それは確定的 drop フェーズより前のもの。）
- **ファイナライズ不変条件:** `drop` はオブジェクト毎に **厳密に 1 回**、
  オブジェクト毎の `dropped` フラグで重複排除された 4 経路の union
  （release-to-zero / 明示 / スコープ退出解決 / バックストップ finalize）から
  発火する。sweep 経路は *メモリのみ* を解放する。RC 回収されたオブジェクトは
  解放時に de-register されるので、二重ファイナライズも二重解放経路も存在
  しない。

retain/release の機構は維持される（メモリと確定的 `drop` を所有）。保守的
バックストップは RC が回収できない残渣だけを回収する。

## 7. RC + バックストップの役割分担

- **RC がメモリを所有。** `emit_value_retain` / `emit_value_release` /
  `culebra_runtime_value_*` / cell の retain-release / スコープ退出の release /
  `array_push` の +1 steal はすべて残る — RC が一次マネージャである。
- **バックストップが残渣を回収。** 保守的完全 mark-sweep（`jit_gc.h`）が RC
  の回収できないもの — サイクルと release 取りこぼしのリーク — を解放する。
  割り当ては各オブジェクトをコレクタに登録し、コレクタは周期的 + 終了時に
  走って RC 到達不能な残渣を回収する。
- **Sweep の解体** は既存の型毎解体（`_culebra_value_release_impl` の本体）を
  再利用してバッファを解放する。ただし **`drop` 呼び出しを除き**、**子の再帰
  release も除く**（子はそれ自身の sweep / RC で回収される）。そしてオブジェクト
  を解放 + de-register する。
- `release_scope_slots`、ループ本体の release、メソッドレシーバの release など
  はすべて **残る** — これらは正しい RC 帳簿である。

書き直しが買う単純化は、RC の削除ではなく **健全なコレクタ**（サイクル +
release 取りこぼし残渣が自己修復する）である。

## 8. 世代別レイヤ（Phase 2 — 性能であって正しさではない）

非世代別の完全 mark-sweep（Phase 0–1）はそれ単体で *正しく安全* である。
世代別はベースが堅固になった後にのみ追加されるスループット最適化である。

- **`generation` バイトによる young / old。** 割り当ては young で、N 回の回収を
  生き延びたオブジェクトは old に昇格する（依然非移動 — ただのフラグ、コピー
  なし）。
- **Minor collect** は young + **remembered set** をトレースし、major は
  すべてをトレースする。
- **Write barrier（minor collect の健全性に必要）。** ヒープ `JitValue` を
  old オブジェクトのフィールドに格納するたびに、その old オブジェクトを
  **card table / remembered set** に記録する。これにより minor collect が
  old→young エッジを見落として生きた young オブジェクトを解放することがない。
  これが現行の世代別コレクタに欠けている健全性装置である。codegen が
  オブジェクトフィールド格納時に barrier を発行する（安価な card mark）。これは
  値毎 RC ではなく *構造的* なので、間違いやすい種類の手動帳簿ではない。

## 9. 安全装置（サーベイに従い並行して構築）

- **GC stress モード**（`CULEBRA_GC_STRESS=1`）: *あらゆる* 割り当てで
  collect する。rooting/marking のバグを flaky にではなく確定的に露出させる
  （SpiderMonkey の `gcZeal` モデル）。CI でスイートをこの下で走らせる。
- **`GC.stat()`**（実装済み）: `live_objects` / `heap_bytes` のイントロ
  スペクション — GC が追跡する生存オブジェクトを数えるよう転用。
- **リーク回帰テスト**（`tests/test_gc_no_leak.cul`、interp では既に green）が
  JIT の受け入れゲートになる。
- 解放スロットを poison パターンで **debug fill** し、使用時に assert。
- **Heap verify** パス（debug）: 全オブジェクトを走査し、すべての子ポインタが
  有効なヒープオブジェクトであることをチェック — marking/enumeration バグを
  捕える。

## 10. C++ 実装における RAII

GC 自体は RAII で書かれ、手動 acquire/release は決してしない:

- オブジェクト解体は struct の **デストラクタ**（sweep はそれを呼ぶだけ）。
- Stop-the-world / safepoint の協調は scope guard を使う
  （`StopTheWorld stw;` がデストラクタで mutator を再開）。
- 内部 GC データ構造は標準コンテナ / スマートポインタを使う。
- グローバル root 登録は、生存期間がスコープ化される所ではスコープ化された
  registrar を使う。

## 11. クロスバックエンドとスレッド

- **Interp** は `shared_ptr` RC を維持する。その正確な RC ゆえに残渣は参照
  *サイクル* だけで、これは `InterpGC` — 精密な CPython 流サイクルコレクタ
  （gc_refs 減算 + BFS + clear）— によって回収される。Array の ValVec と
  キャプチャされた Environment を追跡するので、JIT バックストップが回収する
  closure↔environment サイクルはインタプリタでも回収される — 2 バックエンドは
  振る舞い上対称のままである。（Object-/Tuple-/Set- を root とするサイクルは
  interp 側でまだ追跡されていない。保守的 JIT バックストップは全てをカバー
  する。）interp/JIT 境界をまたぐオブジェクト（Tensor `impl` は
  `shared_ptr<TensorImpl>`）は既存のハンドルを保つ: JIT `JitTensor` struct は
  GC 管理され、その `impl` shared_ptr は sweep 時に `~JitTensor` が release
  する。移動がないので、C++ に渡された raw ポインタは有効なままである。
  - **Rooting: 精密コレクタが精密なのは、すべての生存値が登録済み root から
    到達可能な場合に限る。** `InterpGC` は C++ スタックを **スキャンしない**
    ので、生きているがスタックだけにある値の 2 クラスは明示的に root 化しない
    と、プログラム途中で sweep されてしまう（キャプチャ変数に対する偽の
    `NameError` として表面化する use-after-free）:
    1. **アクティブな env チェーン。** 回収は **文境界** でのみ走る（`bump()`
       が `pending_` フラグを立て、`collect()` は実際には STATEMENT ディス
       パッチと `invoke_user_function` のエントリから発火する）。その安全点で
       生きている env は、現在の文の `env` と、C++ 呼び出しスタック上にまだ
       ある各呼び出し元の `env` である。各呼び出しは呼び出し元 env を
       `frame_roots_` にプッシュし（RAII `FrameRootGuard`）、`collect()` は
       現在の env の完全な `outer` チェーンと各 `frame_roots_` エントリを
       たどって mark set を seed する。これがないと、*追跡されない* スコープ
       env に直接束縛された値（例: 自前の closure を定義しない `while` 本体の
       `mut loss`）はいかなる追跡ノードからも到達不能で、そのキャプチャ
       def_env が sweep されてしまう。
    2. **実行中の C++ スタック一時値。** 実際の collect を `bump()` から次の
       文の安全点へ遅延させることは、C++ 返り値として輸送中にのみ保持される
       出来立ての自己キャプチャ closure も修正する — 文境界ではそれは既に
       root 化された env に格納済みである。
- **Isolate / スレッド**（並行ロードマップ）: 各スレッドは自分のスタック base
  を登録する。回収は safepoint（割り当て地点 + ループ back-edge）で isolate の
  スレッド全体に stop-the-world で行う。isolate 毎ヒープにより、ほとんどの
  回収はスレッドローカルである。保守的スキャンはスレッド毎 shadow stack を
  必要としない — 各スレッドのスタック境界だけでよい。

## 12. 精密 / 移動（Option C）— 条件付きの将来、既定経路ではない

> **決定（2026-05-30）: 移動 / コピー GC は culebra では意図的に追求しない。**
> ここには条件付きの将来オプションとしてのみ残し、ロードマップではない。

**なぜ移動が不適合か。** 移動コレクタには、culebra が今日満たせない 2 つの
強い前提条件がある: (1) **精密 rooting** — オブジェクトが移動したらすべての
root を更新しなければならず、*保守的* root（maybe-ポインタ）は更新できない
ので、移動 ⟹ 精密である。(2) **raw オブジェクトポインタが GC 非協調コードへ
escape しないこと** — culebra は Tensor とインタプリタ相互運用のために raw
ポインタを C++ に渡し（§3 はまさにそれらを有効に保つためにヒープを非移動に
保つ）、tagged な `JitValue {tag, i64 data}` 表現はポインタを整数に詰め込む
が、LLVM Statepoints はそれを追跡できない。これは CPython、Lua、Ruby（既定）、
Go が非移動のままである理由と同じである。移動を可能にするために値 ABI +
相互運用境界を作り直すことは、GC 自体を矮小化してしまう。

**スループットのレバーはいずれにせよ非移動である。** 移動が *唯一* 買う
勝ちはコンパクション（断片化対策）+ 回収コスト ∝ 生存者数 — どちらも culebra
にとって今は二次的である。速い *割り当て* は size-class / Immix bump-region
アロケータ（§4 Phase 2b、Go/JSC モデル）で非移動でも達成できる。だから
スループットが目標のときでさえ、次の一手はより良い非移動アロケータであって
移動ではない。

**再検討のトリガ（そのときだけ再開）。** (1) 断片化が、非移動アロケータでは
対処できない実コストであると計測で示される、または (2) raw ポインタ相互運用
がハンドル/pinning の背後に再設計される。再開するなら: root を保守的スキャン
から **LLVM Statepoints**（`gc.statepoint` / `gc.relocate`）へ移行する。
オブジェクトモデル（§3）、marking（§6）、世代別構造（§8）、安全装置（§9）は
**引き継がれる** よう設計されている。変わるのは root 探索（§5）とヒープの
移動だけである — 今ヒープを非移動に保つことが、それを第 2 の書き直しではなく
局所的な変更にする。

**精密 vs 速度に関する注記。** 移動を *伴わない* 精密 rooting は精密さだけを
買う（保守的コレクタの有界な last-batch 保持 — self-closure リークゲート参照
— を厳密にする）のであって速度ではない。その精密さ単独では Statepoints の
コストを正当化しない。移動が採用される場合にのみ随伴する。

## 13. Phasing

> **Shipped (2026-05-30).** Phase 0–1 + 2a が master に着地した。保守的
> バックストップが、維持された RC と並んで JIT のコレクタである。Phase 2b /
> 世代別 / §12 は将来、計測ゲート付きで残る。

- **Phase 0 — 足場（割り当てのみ; 怖い部分を隔離して検証）。** 保守的 GC の
  真に危険な 2 つの部分は (1) *スキャンがすべての生存 root を見つけるか?*
  （取りこぼし → Phase 1 で use-after-free）と (2) *ポインタ検証は正しいか?*
  である。Phase 0 は両方を **一切解放せずに** 構築・検証する — だから
  スキャナを走らせて既知の生存オブジェクトに対し *assert* でき、偽 root 率を
  計測でき、その間プログラムは動き続ける（割り当てのみのリークはゲートには
  問題ない）。スキャナが信頼できて初めて、Phase 1 がそれに基づいて sweep する。

  JIT に依存しない **自己完結モジュール**（`include/jit_gc.h`）として構築し、
  まず単体で unit テストした:
  - `GcHeader`（8 バイト、`refcount` フィールドを置き換える; sweep の型毎
    dtor ディスパッチ用に `type_tag` を、加えて `mark`/`generation` を持つ）。
  - **Registry ヒープ**（§4）: `gc_alloc` = malloc + register; `is_gc_object`
    = registry ルックアップ。+ unit テスト（live=true、
    interior/random/freed=false）。
  - **保守的スキャナ** `gc_scan_roots`: `setjmp` で callee-saved レジスタを
    スタックへフラッシュ、スタック base を捕捉（`pthread_get_stackaddr_np`）、
    `[sp, base)` をワードアラインで走査、各ワードを `is_gc_object` でテスト;
    加えて明示的グローバル root registry。+ ローカルに保持された既知オブジェクト
    が root として見つかることのテスト。
  - `GC.stat()` を新ヒープに再ポイント; `CULEBRA_GC_STRESS` が各割り当てで
    scan+heap-verify を走らせる; poison-fill インフラ; heap-verify
    （`enumerate_children` の子がすべて `is_gc_object` を通る）。
  - 割り当てのみでコンパイルし、in-tree 結線が着地する前に単体で検証。

  Phase 0 はユーザ可視のものを何も出荷しない; Phase 1 のリスクを下げる。
- **Phase 1 — 保守的完全 mark-sweep バックストップ。DONE (2026-05-30)。**
  保守的 root スキャン + 明示的コンテナ root（module/namespace テーブル、
  trait-default + multimethod closure、REPL グローバル、defer スタック、例外
  キャリア; キャッシュされた namespace オブジェクトは pin される）+ mark
  （`enumerate_children`）+ sweep（型毎バッファ解体、`drop` なし、子の再帰
  release なし）。手動 RC は **維持**（§2 改訂が §7 の「全 RC 削除」を訂正）。
  コレクタは閾値（適応的 `max(100k, live*2)`）で、`GC.stat()` で、終了時に
  バックストップとして走り、`CULEBRA_GC_STRESS=1` は各割り当てで collect する。
  達成: 3 つのリークゲートが JIT で通り、フルスイートが interp/JIT 対称
  （self-closure ゲートは JIT のみ green — interp はサイクルを断ち切れない）、
  スイートは GC stress 下でクラッシュなし（root set が完全）、OFF は不変、
  microgpt JIT RSS はフラット。
- **Phase 2 — 世代別。** `generation` バイト、昇格、write barrier + card
  table、minor/major。受け入れ: ベンチマークセットでスループットが現状 ≥ に
  回復し、stress 下で依然 green。
- **Phase 3（将来）— Statepoints による精密/移動（§12）。** 計測が正当化する
  場合のみ。

## 14. リスクと未解決の問い

- **レジスタ/スタックスキャンの可搬性**（まず ARM64 macOS）: 正しいレジスタ
  フラッシュ、スレッド毎スタック base 捕捉、`gc_alloc` をまたぐレジスタのみの
  生存性で値を失わないこと。GC stress で検証する。
- 実ワークロードでの **偽 root over-retention 率** — 計測せよ。64bit では
  無視できると予想。
- free-list（非移動）割り当てのスループット vs 将来のコピー nursery — 今は
  受け入れる。Phase 2 世代別がほとんどを回復する。
- **唯一の safepoint としての `gc_alloc`** はシングルスレッドでは問題ない。
  マルチスレッドは back-edge safepoint も必要（並行フェーズ）。
- **LLVM が GC ポインタを、*推移的に* 割り当てる非 `gc_alloc` 呼び出しを
  またいで caller-saved レジスタだけに保持** — 呼び出し規約の論拠（§5）で
  カバーされるが、inline を有効にして stress 下で検証すること。

## 15. 不変条件（常に保持されねばならない）

1. **（§2 改訂により訂正。）** 手動 RC は維持: `emit_value_retain` /
   `emit_value_release` は codegen に残り、メモリ + 確定的 `drop` を所有する。
   コレクタはバックストップであって RC の置き換えではない。レイアウト plan A:
   struct オフセット 0 は `int64_t refcount`; コレクタのオブジェクト毎メタ
   データ（mark/tag/size）はオブジェクトではなくヒープの address→metadata
   registry に置く。
1a. **GC サブステートは値保持サブステートより長生きする。** `~Runtime` は
   サブステートを **逆スロット順** で破棄するので、GC ヒープ（`kSlotInterpGc`、
   `kSlotJitGc` — 最下位スロット）が最後に解体される。順方向だと GC を先に
   解放し、その後に JitValue を release する後発サブステートのデストラクタ
   （module / namespace テーブル、テスト registry、defer スタック）が解放済み
   ヒープへ `forget()` を呼ぶ — use-after-free になる。各スロットは削除後に
   null 化されるので、解体途中でサブステートを解決してしまう release は空の
   ものを復活させる。
2. **健全性（安全性の保証）:** 回収は root や別の生存オブジェクトから到達可能
   なオブジェクトを **決して** 解放しない。これは無条件に保持されねばならない。
3. **完全性はベストエフォートであり厳密ではない。** *保守的* な回収は、陳腐な
   スタック/レジスタワードがそれへのポインタに見えるとき、到達不能オブジェクト
   を over-retain しうる。これは安全（64bit 上では有界で稀なリーク）で想定内で
   ある。帰結: コードとテストは「到達可能なものは生き残る」を仮定してよいが、
   厳密な回収集合は決して仮定しない。厳密な mark+sweep 振る舞いを必要とする
   テストのために、確定的な `collect_precise(roots)` エントリ（明示 root、
   スタックスキャンなし）が存在する。
4. Sweep は各死んだオブジェクトの C++ デストラクタをちょうど 1 回走らせる。
5. （Phase 2）すべての old→young ヒープ格納は、次の minor collect の前に write
   barrier で記録される。
6. スイートは `CULEBRA_GC_STRESS=1` の下で green である。

### 単体で検証済み（Phase 0、`include/jit_gc.h` + テスト）

- `is_object` ポインタ検証（live / interior / random / freed）。
- 保守的 root スキャンは **-O0…-O3** でスタック + グローバル root を見つける。
  root 探索は `__builtin_frame_address(0)`（フレームポインタ）ではなく
  **スタックポインタ** から始めなければならない — ローカル/spill は FP の下に
  あり、見落とされてしまう（修正するまで -O2 で失敗していた）。
- Mark-sweep: `collect_precise` は厳密な到達不能集合を回収する; 保守的
  `collect` は健全（到達可能なものはすべて生き残る）。
