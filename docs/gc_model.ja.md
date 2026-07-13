# Culebra メモリモデル — 検証済みの現状と、構造的 leak-freedom への道

Status: **Canonical.** 正準たる原典は英語版 `gc_model.md` であり、本ファイル
はそのミラーとして常に同期を保つこと。RC・GC・確定的 `drop` が今日実際に
どう振る舞うかについての唯一権威ある記述であり、独立した二重コード監査
（2 名のレビュアーが相互チェックし、すべての主張をコードに根拠づけた）
によって生み出された — かつて `jit_gc_design.ja.md` にあった stale な不変条件
が、それ以前の推論を汚染していたことが判明した後のことである。本ドキュメント
と他ドキュメントが食い違う場合は **本ドキュメントが勝つ**。他方を直せ。

本ドキュメントが果たす 2 つのゴール（優先順）:

> **G1 — 確定的 `drop`。** `drop` メソッドを持つオブジェクトは、予測可能な
> 地点・予測可能な順序で、どのバックエンドでも同一に、ちょうど 1 回だけ
> それを発火する。
>
> **G2 — 構造的 leak-freedom。** 到達不能なオブジェクトが無期限に生き残る
> ことはなく、いかなるリークも黙って覆い隠されることはない — サイトごとの
> 警戒によってではなく、構成によって。

§1–§4 は今日真であることを記述する。§5 はギャップリストである: 今日の
振る舞いが G1/G2 に届いていないあらゆる箇所を、要求事項として述べる。

---

## 1. 二層モデル

Culebra のメモリ管理は両バックendともに **RC 主体 + トレーシング backstop**
である:

- **RC がメモリと `drop` タイミングを所有する。** インタプリタ: 値は
  `shared_ptr`（自動・厳密）。JIT: 値はタグ付き `i64`。RC の配置は
  `jit_ownership.ja.md` §4 の層状 ownership 規律（`Owned` ハンドル、
  scope スロット、unwind-temp ウィンドウ、block-pinned raw）が所有する。
  残る裸の retain/release サイトは監査済みの carve-out であり、
  `check_rc_discipline.sh` ラチェットが数える。
  release-to-zero は速やかに解放し `drop` を発火する — これが `drop` を
  確定的にする（G1）。
- **トレーシングは RC が回収できないものを回収する**: 参照サイクル、および
  RC 配置バグでリークしたあらゆるもの。JIT: レジストリヒープ（`jit_gc.h`）
  上の保守的 mark-sweep、マシンスタック全体の root スキャン、refcount
  非依存。インタプリタ: `InterpGC`、*追跡対象のノード種のみ* に対する精密な
  CPython 流 gc_refs コレクタ（§5-GAP2 参照）。

両層がなぜ必要か（そしてどちらも引退不可か）:

- RC 単独では決して leak-free にならない: サイクルは互いのカウントを永遠に
  正に保ち、手配置バグはリークする。（Swift — コンパイラ完璧な ARC、
  サイクルコレクタなし — は retain サイクルを日常的にリークする。これが
  「体系的 RC で十分」への反例である。）
- トレーシング単独は G1 を破壊する: トレーサは collect 時にしか死を発見しない
  ので、`drop` は遅く・順序なく・非確定的に発火する（Go/Java の finalizer
  モデル）。共有所有下での確定的 finalization には完全な参照カウントが要る。

したがってアーキテクチャは「一時的な松葉杖付き RC」ではない — **トレーサは
恒久的で荷重を担うコンポーネント** であり、設計上の問いは、それが RC バグを
*黙って覆い隠す* のをいかに止めるか（G2、§5-GAP5）のみである。

## 2. 確定的 `drop` — 四経路 union

`drop` は 4 つの経路から発火し、**オブジェクトごとの `dropped` フラグにより
exactly-once に dedup される**（JIT: `JitObject::dropped`; インタプリタ:
`OrderedSymbolMap::dropped`）。不変条件は呼び出しサイトではなくフラグである。

| path | trigger | mechanism |
|---|---|---|
| (a) release-to-zero | 最後の参照が release された | JIT `_culebra_value_release_impl` OBJECT ケースは *まず* drop を発火し、その後 proto/slots/sidecar を解体する。インタプリタ: prop-map `shared_ptr` deleter → `_destroy_prop_map` → `_call_drop_if_present`。 |
| (b) explicit `obj.drop()` | ユーザ呼び出し | `culebra_runtime_explicit_drop` / インタプリタ eval; オブジェクトは生存し続ける（フラグは二度目の発火を防ぐだけ）。 |
| (c) scope exit | owned 領域の解決 | 両バックエンドとも、scope に登録された drop 保持オブジェクトに対する **局所化 Bacon-Rajan 試行削除** を走らせる（`owned_scope_exit` / `_owned_resolve_ambiguous`）: 外部から到達不能 → 今発火する（サイクルメンバーを含む）; 外部から到達可能 → 生存し、親 scope へ compact される。予算 `kNodeBudget = 4096`; オーバーフロー → 全員生存（安全方向: 遅くはあれ、決して早くない）。 |
| (d) GC backstop finalize | collect が死集合を発見 | **PEP-442 流 pre-sweep パス** — JIT `_jit_gc_finalize_dead`、インタプリタ `_owned_gc_backstop`: 構造が無傷なうちに死集合の `drop` を発火し、その後 sweep/clear が `_drop_suppressed` 下で *メモリのみ* を回収するので、何も再発火しない。**サイクルメンバーは `drop` を発火する。** |

順序契約: scope 内では LIFO（新しいもの優先）; defer は各 exit 経路でスロット
release より前に走る; RC カスケードでは親が子より先。インタプリタの 5 つの
scope-exit 判定（`kOwnedSurvive/Leave/Cascade/Release/Fire`）は、ひとえに
JIT の drop 順序を再演するために存在する — 非循環な候補は意図的に本物の
`shared_ptr` カスケードに委ねられる。

荷重を担う微妙な点（これらを「単純化」して消し去るな）:

- `_culebra_call_drop_if_present` は **本体の実行中 refcount を `1<<40` に
  ピンし、その後エントリ時のカウントを復元する**。この巨大なピンは無制限の
  再入 release を吸収する（drop 本体が自分のサイクルに対し `this.me = nil`
  を行うケース）; *復元*（ゼロではなく）が必要なのは、経路 (b)/(c)/(d) は
  生きた参照を伴って到達するからである。
- scope-exit の生存者は **どの drop が発火するより前に** 再 push される —
  drop 本体はより高い id の新エントリを登録し得るので、それらの後ろに生存者を
  追記すると id 順のスタックが壊れる。
- `culebra_runtime_throw` は意図的にペイロードを retain **しない**: carrier が
  thrower の `+1` を引き継ぐ。そこに retain を再追加すると固定リークが再導入
  される（catch されたペイロードが不死化、drop が抑制される）。
- Multifn ディスパッチャ: RC 経路はテーブル保持の body 参照を release する
  （`release_bodies=true`）; sweep 経路はそうしてはならない（body は自身の
  sweep エントリを持つ）— この非対称は二重解放ガードであり、テーブルの
  detach は body release より前でなければならない（発火した drop が同名を
  再宣言し得る）。
- トップレベル束縛はプログラム終了時に意図的に drop を決して発火しない
  （JIT 抑制フラグ ≙ インタプリタの決して解体されないグローバル env）。
  構成上対称。

## 3. 何が refcount され、何がされないか

refcount される（一律に構造体オフセット 0 の refcount）: `Func`（closure）、
`Array`、`Tuple`、`Object`、`Tensor`、`Set`、および内部 capture の `Cell`。

**refcount されない: `String`/`StringView`** — JIT の文字列は `malloc` され、
**設計上プロセス寿命の間リークする**。retain/release は文字列に対しては
静かな no-op であり、これは文字列の所有権ミスがどんな RC 監査にも不可視に
なることも意味する。Nil/Bool/Long/Float は即値である。

## 4. Rooting — 2 バックエンドは異なるやり方をする

- **JIT（出荷デフォルト）:** *マシンスタック全体* の保守的スキャン
  （`setjmp` レジスタフラッシュ、SP→スタック底の走査）、refcount 非依存。
  すべての C++ helper フレームの飛行中の裸 `JitValue` は、その refcount が
  正しかろうがなかろうが root される。`gc_refs` モード（`CULEBRA_GC_REFS=1`、
  既定 off）は CPython アルゴリズム — refcount からヒープ内エッジを引く、
  **スタックスキャンなし**; その soundness ≡ RC 会計の正確性。
  `collect_refs_diag` は「保守的には死だが gc_refs では保持」なオブジェクトを
  *inflated-RC*（確実な RC 配置リーク）と *transitively-held*（サイクル）に
  分類する — 出来合いの false-positive ゼロな RC-leak 検出器である。
- **インタプリタ:** **C++ スタックスキャンは一切なし。** 安全性は *スケジュー
  リング* による: collection は文の境界でのみ走り、root は現在の env チェーン
  + `FrameRootGuard` エントリから手で走査される。将来もし式の途中で collect
  するショートカットを入れれば、飛行中の一時値を掃いてしまうだろう。

帰結: 「rooting は単一の retain とは独立に提供される」（`jit_ownership.ja.md`
§2 の主張）は **JIT 限定** である。そして `gc_refs` 下では、rooting は RC 会計
と同程度にしか sound でない — 保守的スキャンは `gc_refs` が root できるより
厳密に多く（借用された飛行中の helper 一時値を）root する。これが、保守的
スキャンを *balance* 証明の強さだけで `gc_refs` に置き換えられない理由である
（balance ≠ lifetime; 早すぎるが balance の取れた release は、保守的スキャン
が黙って吸収する UAF ウィンドウを `gc_refs` 下では生む）。

## 5. ギャップ — G1/G2 への要求事項

各ギャップは次の形で述べる: 今日壊れていること → それが課す要求事項。
これらは二重監査（2026-07）時点で既知の *完全な* 集合である。

### GAP1 — `break`/`continue` skip owned-slot release (JIT). **CLOSED (2026-07).**
かつて: `compile_break`/`compile_continue` は反復の defer を走らせてジャンプ
していた; 放棄された反復 scope に対して `release_scope_slots` を **走らせな
かった**。反復ごとの owned スロットは backstop へリークし — そして stale な
スロット alloca がオブジェクトを保守的に root し続けたため、`break` で抜ける
ループ本体に束縛された drop 保持オブジェクトは **`drop` を決して発火しなかった**
（遅くではなく — 決して、良くてもフレーム終了まで）。インタプリタは同じ
スロットを自動的に release する（C++ `BreakSignal` 経路を通じた `shared_ptr`
巻き戻し）ので、これは **drop 意味論のバックエンド非対称** であり、リークより
厳密に悪かった。
**Fix:** 各 `LoopBlocks` はループ本体 scope のインデックスを `scopes_` に記録
する; `compile_break`/`compile_continue` は今 `emit_loop_scope_exit` を呼び、
最内 scope からその本体 scope まで（含む）の全開放中 scope の owned スロットを
release し、本体 scope のマークで owned 領域を解決する — fall-through が使うのと
同じ `release_scope_slots` + `emit_owned_scope_exit` 機構である（`scopes_` は
無傷のまま残されるので、今や死んだ fall-through 経路上でループ自身の
`finish_and_pop_scope` はなお走る）。break のターゲットはループ末尾で既に
ループの外側 scope（for-in の iterable）を release するので、ここで閉じられる
のは反復ごとの scope のみである。検証: interp/JIT の drop タイミング対称性
（`tests/test_drop_loop_control.cul`）、leak-fuzz ゲート（新規リークなし）、
difftest コーパス、フルゲート。注意: leak-fuzzer の反復ごと成長オラクルは
このクラスを **捕捉しない**（一発の `break` は反復ごとの成長ではない;
`continue` のスロットは次の反復のストアで上書きされる）— これは drop-*タイミング*
バグであり、drop テストでのみ捕捉される。

### GAP2 — Interp cycle blind spot: pure Object↔Object / Tuple / Set cycles. **CLOSED (2026-07).**
かつて: `InterpGC` は Array の backing vector と closure Environment のみを
追跡していたので、完全に Object プロパティマップ（あるいは Tuple/Set
コンテナ）を経由するサイクルは不可視だった — メモリは決して回収されず、
それが誕生 scope を脱出していれば `drop` も決して発火しなかった。
**Fix:** Object の prop マップ、Tuple の要素、Set のメンバーは今や追跡される
サイクルノードである（`GcKind` {Env,Vec,Set,Map}; Map/Set エントリは自身の
sidecar weak_ptr を運び、collect 全体を通じて `live` にロックされるので、
兄弟の clear がまだ sweep されていない sidecar を sweep の途中で解放できない）。
コンテナのエッジ列挙は `gc_for_each_container_backing` / `gc_for_each_child`
に単一源化された（両コレクタでかつて欠けていた Set の `index`-key エッジが
今は数えられる）。ノード変更が偽の root に変えてしまった 2 件の hidden-capture
の過少カウントを修正した: 合成 `drop`-`this` ビューはエッジを出さず
（`is_synthetic`）、`_wrap_method_with_this` はレシーバを、eval closure と
コレクタフックの両方が共有する 1 つの共有 Value に保持する（2 つ目のコピーは
レシーバを root したままにしていた）。検証: `tests/test_interp_cycle_gc.cul`、
コーパス全体に対する GC_STRESS（over-collection/UAF 検出器）、difftest、
leak-fuzz、フルゲート。言語仕様の「サイクルは危険」carve-out（§16 / §24 +
ja ミラー）は削除された。**残り（フォローアップ）:** `_owned_resolve_ambiguous`
を同じ共有列挙に通し、まだ別個のコンテナ走査が再び drift できないようにする
（現在は Set の `index` を過少カウントしている — 保守的なピンであり、バグでは
ない）。

### GAP3 — Runtime helper interiors leak on the C++ throw path (JIT). **G2 違反 — P0 クラス。**
Iterator/HOF helper（`iter_collect` の `out`、`iter_reduce` の `acc`、
`_iter_filter_fast_fn`/`_iter_take_while_fast_fn` の保留要素、`array_map` の
半分組み立てられた `out`、…）は、ユーザコードに再入し throw し得る呼び出しを
跨いで裸の `+1` `JitValue` を保持する。throw 時それらは RAII なしに C++ スタック
上で drop され — backstop によってのみ回収され（覆い隠される、G2 違反）、
leak-fuzzer がそれらをピンする（47 エントリのベースラインの非循環な多数派）。
`JitOwnedVal`（move-only な owned ラッパ、dtor が巻き戻し時に release、
`consume()` で手渡す）は存在し、`iter_find` で実証済み。
**Requirement — FIX:** すべての helper 保持の owned `JitValue` は `JitOwnedVal`
（あるいは合う場合はその `JitMethodSelf`/`JitMethodArgs` ABI エイリアス）の
中で生きる。有界な作業: コールバック再入 helper 集合は ~24 関数。
**Requirement — ENFORCE（これがそれを *構造的* にする、単に fix したのでは
なく）:** **runtime helper 本体内の裸の owned retain** でビルドを失敗させる
静的ゲート — すなわち throw し得る呼び出しを跨いで、guard ではなく裸の
`JitValue` local に保持された `culebra_runtime_value_retain`（あるいは `+1` を
返す helper の結果）。これがなければ GAP3 は「現在の 24 個を fix した」+
カバレッジに縛られた回帰ネットである; これが *あれば*、裸の `+1` を手保持する
将来の 25 個目の helper は **コンパイルできない** ので、throw 経路リークは
二度と再隠蔽できない。これは codegen の double-consume assert の C++ 側類似物
である。
**Status — FIX landed（leak-fuzzer C①–C⑨、2026-07）:** 既知の helper 保持
リークは閉じられた（RAII guard + callee-cleans-on-direct-throw 規約、
`docs/jit_ownership.ja.md` §4.3/§6）、そしてスイート全体の GAP5 ゲートが
それらをピンする。
**Status — ENFORCE landed as a ratchet gate（2026-07）:**
`tools/check_rc_discipline.sh`、常設の `just test`/`test-dev` フェーズが、
runtime ファイルごと（stdlib_jit.h / sendable_jit.h）の裸 RC 呼び出しと
codegen 側の裸 emit サイトを数え、いずれかのカウントが増えたらビルドを
失敗させる。RAII 形式を使わず裸の `+1` を手保持する将来の helper は上限を
超える; サイトを変換すると上限が下がる（上限は縮むのみ）。これはサイトごとの
コンパイルエラーではなくラチェットである — 残る裸呼び出しは監査済みの
carve-out であり、各々スクリプトのコメントで正当化されている。

### GAP4 — Codegen-side exception edges: the dominant fuzzer leaks. **G2 違反。**
最大の実測リーク（整数要素での exception-through-iterator ~10 obj/iter —
すなわち要素ではなくチェーン一時値）は **caller 側** である: `.iter()`
チェーン / レシーバ / 飛行中の一時値は通常経路では release されるが throw
エッジで取り残される。それはまさに `Owned` の直線的 dtor がモデル化できない
carve-out の形状（スレッド化チェーン、ループ本体、branch を跨ぐ
consume-or-release）においてである。領域ごとの cleanup-landingpad 機構
（`finish_scope_cleanup`、build/pending guard）は出荷済みで正しい機構である
— これらのサイトはただそれに配線されていないだけだ。
**Requirement — FIX:** may-throw 領域を跨いで現在 `+1` を保持する全 carve-out
は cleanup pad に乗る。leak-fuzzer ベースラインをそのサイクルのみの残渣まで
追い込み、閉じた各クラスを battery パターンでピンする。
**Requirement — ENFORCE（構造的）:** **`Owned` ハンドルに対する codegen 時の
会計パス**: 生成された全 `Owned` は、emitter が取る *あらゆる* 制御フロー経路
（exception エッジを含む）で consume-or-drop されねばならない — さもなくば
codegen 時 abort（既存の直線的 double-consume assert の全経路一般化）。これは
却下された whole-IR ownership verifier では **ない**: それは codegen 自身の
`Owned` ハンドル（有限・emit 時可視）についてのみ推論し、ヒープエイリアスや
オブジェクト寿命についてではないので、あの verifier の致命的な反論を回避する
（helper 内部 → GAP3 の C++ RAII で別途カバー; balance ≠ lifetime →
discharge 会計は寿命の主張を一切しない）。本当に難しいのは、carve-out の
*形状*（スレッド化チェーン、共有レシーバのディスパッチ、branch を跨ぐ
consume-or-release）がまさに、単一の `Owned` では現在モデル化できないもので
ある点だ — GAP4 を構造的に閉じるとは、それらの形状に会計が見える
`Owned`-ファミリの抽象を与えることを意味する。ENFORCE がなければ、*新しい*
carve-out が黙って throw 経路リークを再導入する; それがあれば、会計されない
`+1` はビルド失敗である。
**Status — FIX landed（leak-fuzzer C②–C⑨、2026-07）:** 既知の caller 側
throw エッジリークは閉じられた（`ThrowGuard` cleanup pad + helper ごとの
callee-cleans 契約、`docs/jit_ownership.ja.md` §6）、leak-fuzz ベースラインと
abort-suite allowlist は共にサイクルのみになり、スイート全体の GAP5 ゲートが
throw 経路をピンする。
**Status — ENFORCE landed（2026-07-11）、assert ではなく *構成的* 機構として:**
自動 unwind-temp ウィンドウ（`docs/jit_ownership.ja.md` §4.8）。生きている
全 `Owned` `+1` は各 may-throw 呼び出しの周りで spill され、scope チェーンの
cleanup pad によって release されるので、throw する sub-compile を跨いで `+1` を
保持する emitter は *既定で正しい* — リークは書けず、これはそれを abort する
より強い。callee が既に unwind エッジを掃除する呼び出しサイトはそれを宣言する
（`UnwindCovered`、機械可視の §4.7 契約）、エッジごとにちょうど 1 つの releaser を
保つ。これが閉じた probe クラス（後続の throw するオペランドを跨いで保持された
binop lhs / call argument / index receiver / `==` lhs / method receiver）は、
サイクルのみのコーパスベースラインにもかかわらずあらゆる形状でリークして
いた — コーパスはそれらの形状を綴らなかった; `tools/difftest/leak_abort.sh`
の Case 4 が今それらをピンする。
**Status — the last escape hatch closed（2026-07-12、
`docs/jit_ownership.ja.md` §4.9）:** ウィンドウは *ハンドル内の* `+1` を守る;
`consume()` からの裸の `llvm::Value*` を basic block を跨いで運ぶことは、依然
どの層にも不可視だった。`consume()` は今 block-pinned トークンを返す
（ピン block の外で raw を使うと、どのビルドモードでも codegen が abort する）、
すべての `%Value` phi は検査付き `OwnedPhi` を通じて構築され、すべての
`compile_*` helper（core と extension フック）は裸の `+1` ではなく `Owned` を
返す。difftest コーパスがほぼすべての構文を *コンパイルする* ので、違反
パターンは実行時リーク再現なしにコンパイル時に捕捉される — カバレッジは
実行された経路からコンパイルされた経路へ移った。

### GAP5 — The backstop silently masks RC bugs. **G2 違反 — メタギャップ。**
今日、配置バグは「backstop が回収する、誰も知らない」として出荷される。
検出はカバレッジに縛られる（テスト + fuzzer）; 実行されない経路のリークは、
ユーザがメモリ成長を報告するまで不可視である — 受け入れがたいフィードバック
ループだ。構造的 fix は GAP3+GAP4（配置バグをソースで不可能にする）である;
*検出* の fix には既に出荷済みの機構がある: `collect_refs_diag` の inflated-RC
分類器は、helper 内部もカバーする false-positive ゼロな確実 RC-leak 検出器で
ある。
**Requirement:** debug/CI ビルドで、inflated-RC 分類はオブジェクトの誕生
サイトとともに abort する（loud、行動可能）。leak-fuzz ゲート
（`tools/difftest/leak.sh`、ベースライン駆動）はコーパス全体の回帰ネットとして
残り、そのベースラインはサイクルのみの残渣まで単調に縮む。プロダクションは
回収を続ける（クラッシュは DoS のトレードになる）、これは受け入れられる —
*なぜなら* GAP3/GAP4 が検出に頼るのではなくソースでバグクラスを除去するからだ。

**Status — machinery shipped（2026-07）。** `CULEBRA_GC_LEAK_ABORT=1` がそれを
起動する: JIT は各オブジェクトの割り当て backtrace を捕捉し（`Heap::birth_sites_`、
起動時のみ）、プログラム解体時に `Heap::maybe_audit_leaks` がヒープを分類し、
すべての inflated-RC オブジェクトの誕生サイトとともに abort する。元の計画が
過小評価していた 2 つの事実:
- **監査は 1 つの静止 safepoint で走る、collect ごとではない。** 式の途中では、
  新しく割り当てられたオブジェクトは正当に refcount 1 かつ内部エッジゼロを持ち
  （inflated に見える）、その唯一の参照は保守的スキャンがまだ spill していない
  レジスタに座っている — スナップショットでは真の orphan と区別できない。
  そこで監査は JIT 解体 collect（`run_modules` の `CollectGuard`）にフックされる。
  トップレベル本体が return した後、しかしモジュール/namespace の root がまだ
  配線されている間だ: すべての正当な新生児は root されたか release されている
  ので、そこで inflated-and-dead なオブジェクトは本物のリークである。それは
  `CULEBRA_GC_NEVER` に関わらず走るので、fuzzer の backstop-off モードもなお
  loud な解体チェックを得る。クリーンなコード（fn スコープおよびトップレベル
  スコープ）と良性の自己サイクル（inflated ではなく transitively-held に分類）で
  false-positive ゼロを検証済み; 本物の非循環リーク（`Shared.new` サブビュー）と
  throw 経路の内側スコープリークで正確な誕生サイトとともに発火する。
- **no-LTO（debug/CI）ツール。** 監査は保守的スキャンの *completeness* に乗る、
  これは設計上ベストエフォートである（stale なスタックワードが死んだ
  オブジェクトを live としてエイリアスし得る）。LTO の変更されたスタック
  レイアウトはそのギャップを十分に広げるので LTO release ビルドは過少報告する
  （no-LTO ビルドが捕捉するリークを見逃す）、したがって検出器は no-LTO の
  ゲート/dev ビルドを対象とする — まさに要求事項が名指す「debug/CI」だ。
  プロダクション（LTO）が静かなままなのは意図した DoS トレードであり、回帰では
  ない。
- **スイート全体で起動（C⑥、2026-07）。** 非サイクルのベースラインリークが
  閉じられたので、監査は今や difftest コーパス全体に対して常設の `just test`
  フェーズとして走る（`tools/difftest/leak_abort_suite.sh`、allowlist
  `leak_abort_allow.txt` — サイクルのみ、2 エントリ）。成長ゲートが計測できない
  throw 経路を含む、どのコーパスケースの新しい inflated-RC リークも、
  オブジェクトの誕生サイトとともにゲートを失敗させる。`leak-abort` スモーク
  テスト（`tools/difftest/leak_abort.sh`）はなお検出器自身を守り
  （fires-on-leak + false-positive ゼロ）、`just leak-abort FILE` はその下で
  任意のスクリプトを走らせて報告されたリークを局所化する。

### GAP7 — Residual `drop`-timing carve-outs. **G1、minor — ACCEPTED (2026-07-11).**
`drop` が *厳密に* は確定的でない 3 ケース、すべて **文書化された言語意味論
として受容** — Python/Swift の規範に一致し、各々タイミングのみを劣化させ、
リソースの実際の回収を決して劣化させない:
1. **`kNodeBudget = 4096` オーバーフロー** — 一度に >4096 個の drop 保持
   オブジェクトを解決する scope はオーバーフローを backstop に先送りする
   （遅く、決して早くない）。稀（1 scope 内の巨大なリソースグラフ）。
2. **トップレベル束縛はプログラム終了時に決して `drop` を発火しない**、設計上
   （JIT 抑制フラグ ≙ インタプリタの決して解体されないグローバル env）。
   `drop` を持つグローバルは終了時に finalize しない。
3. **Self-captured-env タイミング非対称** — インタプリタは自己捕捉する env を
   強制的にピンし、そのサイクルの `drop` を JIT より 1 collect 遅く発火する。
   drop *回数* は対称; *タイミング* は 1 collect ずれる。
**決定: 3 つすべてを受容** — 基準は「Python/Swift と同程度に強い確定的 drop」
であり、「carve-out ゼロ」ではない。理由:
- あらゆる主流の RC/GC 言語は同一の carve-out を出荷している: Python の
  モジュールレベル `__del__` はインタプリタ終了時に保証されず、循環 finalizer は
  PEP-442 前は到達不能だった; Go の `defer` は `os.Exit` では走らない;
  C++/C の静的 dtor と `atexit` の順序/実行は `_exit`/signal/`abort` 下で
  保証されない。これは受容された技術水準である。
- ユーザ可視の利害がある唯一の carve-out（#2）は **標準ライブラリのリソースに
  影響しない**: `File` は per-Runtime テーブル内の `std::fstream` なので、
  通常終了の解体（あるいは `Sys.exit`）はストリームのデストラクタを走らせ、
  `drop` が発火したか否かに関わらず flush/close する — フラッシュされていない
  トップレベルファイルを書き、通常終了と `Sys.exit` の両方の後に読み戻して、
  interp と JIT で検証済み。#2 が観測可能なのは、下位レベルの RAII backstop を
  持たない副作用（別れのパケット、リモートコミット）を持つ *ユーザ定義* `drop`
  で、AND そのオブジェクトがトップレベルで束縛されている場合のみ — そして
  Culebra は既にまさにそのケースのために `with` / `defer` / 明示的 `.drop()` を
  提供している、Python/Go が後から追加したのと同じエスケープハッチだ。
それらを閉じること（予算を上げる、終了時 drop パス、interp ピンの整合）は
可能だが追求しない: #1/#3 はほぼ何も得ず、#2 のトップレベル終了時パスは
interp/JIT 対称性要求（[[feedback_check_jit_interp_symmetry]]）を、どの出荷済み
言語もしない保証と引き換えにする。Culebra が Rust 級の確定的リソース管理を
明示的なセールスポイントとして位置づけるようになった場合にのみ再訪せよ。

### GAP6 — Stale docs poison reasoning. **Meta.**
この監査が存在するのは、PEP-442 finalize が出荷された後も長く
`jit_gc_design.ja.md` §0/§6a が「サイクルメンバーは drop を発火しない」と
なお述べていたからだ（2026-07 修正）。さらに 2 件の comment-vs-code drift が
見つかった: `release_scope_slots` の docblock は params が borrowed スロットだと
主張する（それらは owned である; borrowed なのは capture cell のみ）、そして
`release_all_scopes_for_exit` の「throw 経路はなおリークする」コメントは
`finish_scope_cleanup` より前のものだった（throw はカバー済み;
break/continue がなお真だった部分 — 今 GAP1 の fix で閉じられ、コメントは更新
済み）。
**Requirement:** 不変条件を述べるバナーは 1 つのドキュメント（本ドキュメント）
に住む; 他のドキュメントはここへリンクする。不変条件に触れる振る舞い変更は
同じコミットで本ドキュメントを更新する（[[feedback_sync_code_docs_memory]]）。

## 6. 「完了」とは何か — そして fix/enforce の線

**2 つのフィニッシュライン** があり、ギャップリストは異なる地点でそれらを
横切る。どちらが目標かを明示せよ。

### G1 — 確定的 `drop`
- **実用的（Python/Swift と同程度に強い）:** GAP1 + GAP2 が閉じた
  （break/continue と脱出したサイクルが両バックエンドで、あらゆる形状で drop を
  発火する）、四経路 union + `dropped` フラグが単一機構のまま残る（いかなる
  第 5 経路も `_culebra_call_drop_if_present` を通る）。GAP7 の 3 つの carve-out は
  文書化された意味論として受容。
- **carve-out ゼロ:** 加えて GAP7 を閉じる（予算、トップレベル終了時、interp
  ピンタイミング）。

### G2 — 構造的 leak-freedom + 黙った覆い隠しなし
G2 には 2 つの半分がある; それらは異なる地点で完了する:
- **「到達不能なオブジェクトは生き残らない」— 既に完了。** トレーサが到達性に
  よってこれを恒久的に保証する。ギャップなし。
- **「黙って覆い隠されるリークなし」— 2 段階:**
  - **実用的（非常に強い、カバレッジに縛られる）— 到達（2026-07）:**
    GAP3-FIX + GAP4-FIX（*既知の* throw 経路リークをすべて閉じた）+ GAP5
    （inflated-RC 検出器が debug/CI で loud、スイート全体で起動）+ leak-fuzz
    ゲートと abort-suite allowlist の両方がサイクルのみの残渣にある。
    結果: 既知のリークなし、新しいものは CI で捕捉、loud な誕生サイト abort。
  - **構造的（カバレッジ非依存、「隠れられない」）— 到達（2026-07）:**
    加えて GAP3-ENFORCE（ファイルごとの裸 RC ラチェットゲート）+
    GAP4-ENFORCE（§4.8 自動 unwind-temp ウィンドウ — throw 経路リークは
    abort ではなく既定で正しい）+ §4.9 block-pin（`consume()` が block-pinned
    トークンを返す; basic block を跨ぐ裸の `+1` は codegen を abort する;
    すべての `compile_*` helper が `Owned` を返すので、裸の `+1` が
    compile 層の C++ return を跨がない）。*新しい* リークは今や、レビューで
    ラチェット上限を上げるか、サイトごとの根拠を伴う `consume_unchecked`
    エスケープハッチを通じて書くかのいずれかを要する — 黙って現れることは
    できない。

**現在地（2026-07-12）:** 確定的 drop は Python/Swift 級（G1 実用的、GAP7
carve-out は意味論として受容）であり、G2 の両段階に到達している。残る
リーク面は: サイクル（トレーサの恒久的な仕事、2 エントリのサイクルのみの
残渣として allowlist 化）と、ラチェットが数える監査済み carve-out サイト
（各々が書かれた根拠を持つ）である。コーパスゲートは今や発見機構ではなく
安全ネットである — 新しいリークパターンは、それが最初に *コンパイルされた*
とき（§4.9）あるいは CI で最初に走った とき（GAP5）に捕捉される、ユーザが
メモリ成長を報告したときではなく。

明示的な非ゴール、今サイクルにデータとともに決定: トレーシングコレクタの
引退（G2 と矛盾 — トレーシング *こそ* 構造的保証である; そして `gc_refs` は
飛行中の helper 一時値について保守的スキャンより厳密に少なく root する）;
whole-function IR ownership verifier（敵対的にレビューされ却下: helper 内部に
盲目、balance ≠ lifetime — 注意: GAP4-ENFORCE は *より狭い* Owned ハンドル会計
であり、これではない）; Perceus reuse/elision（実測で死: slab は既に 98.7%
再利用、共有は genuine）。
