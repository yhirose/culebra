# JIT 所有権 — リークを構造的に不可能にする

Status: **Design / north star — かつ、2026-07 時点で shipped 済み。** §4 の各
レイヤはすべて実装され、ゲート化されている (§4.8 unwind-temp window、§4.9
block-pinned raws + 型付き `compile_*` seam、rc-discipline ラチェット群)。本書は
JIT がヒープ値の寿命を*どのように*管理するかの authoritative な記述であり、この
領域における今後すべての作業の standing rule である。本書は
[`jit_gc_design.ja.md`](jit_gc_design.ja.md) (トレーシング backstop と
object/heap モデルを扱う) の所有権 counterpart である。

> **翻訳ノート。** 本書は英語原典 `jit_ownership.md` の日本語ミラーである。
> 原典が source of truth であり、本ファイルは常に原典と同期させること。

---

## 0. ルール (交渉不可)

**リークと double-free は構造的に不可能にしなければならない — RAII (C++) と
所有権/`Drop` (Rust) がそうするように、構築によって防ぐのであって、ケースごとに
パッチを当てるのではない。**

- **場当たり的な leak fix を禁じる。** リークする codegen サイトを 1 つずつ狩る
  のは明示的に戦略*ではない*。これまで shipped した個別 fix はいずれも実質的な
  改善をもたらしたが、根底のハザード (次の未 audit なパス) はそのまま残した。
  manual-RC 設計は「リークの巣」であり、我々は卵ではなく巣を取り除く。
- **賢さより優雅さと単純さ。** 散在する特別ケースより、codegen 全体が値を通す
  1 つの小さく正しい抽象を優先する。
- **例外なし。** ヒープ値を生成または消費する新しい codegen は所有権レイヤを
  使う。新しいコードパスにおける裸の `emit_value_retain` / `emit_value_release`
  は設計上の匂いであり、レビューで正当化するか、より良くは refactor して消すべき
  である。

なぜ構造的投資に値するか: このリーク class は、fast reclaim パス (決定的 RC drop、
最終的には `gc_refs` / precise GC) が load-bearing になるのを妨げている最後の
要素である。leak-free な RC こそが、conservative backstop の過剰保持 (scalar-
microgpt の減速の支配的要因、`jit_gc_design.ja.md` §1 と
[[project_jit_gc_rewrite]] 参照) を退役させる前提条件である。

---

## 1. 問題の範囲

Culebra には 2 つの実行 backend があり、**この問題を持つのは JIT だけ**である:

- **ツリーインタプリタ** — 値は `shared_ptr`。refcounting は自動かつ正確で、
  リークと double-free は構築上起こり得ない。これは*意味論*が問題ないことの
  証明であり、JIT にはその機構が欠けているだけである。
- **JIT (LLVM) / AOT** — codegen はあらゆる所有権境界 (call 引数、method
  receiver、演算子オペランド、ループ本体の値、scope 退出、例外エッジ、…) で
  `retain` / `release` 呼び出しを手で emit する。これをあらゆる制御フローパス —
  通常の fall-through、`break`、`continue`、早期 `return`、例外 unwinding — で
  正確に正しく行うことこそ、あらゆるリークとあらゆる double-free の出所である。

目標は、JIT に `shared_ptr` なしでインタプリタの保証に匹敵する、*自動かつ正確*な
所有権規律を与えることである。

規律がカバーしなければならないヒープ値種別 (tagged `i64`、`data` = `ptrtoint`):
`Object`、`Array`/`Tuple`、`Func` (closure)、`Cell`、`Set`、`Tensor`、および
inline-length の `String`。スカラー (`Long`/`Float`/`Bool`/`Nil`) は決して
refcount されない。あらゆる op は tag-aware であり、それらに対しては no-op となる。

---

## 2. 設計を形作らねばならない唯一の洞察: 所有権 ≠ rooting

今日の JIT における refcount は暗黙のうちに**2 つの独立した仕事**をこなしており、
これらを混同することこそ、素朴な「冗長な retain を削除する」fix をクラッシュ
させる原因である:

1. **所有権 / release** — オブジェクトが*いつ*解放されるかを決める
   (release-to-zero、これは決定的 `drop` も発火させる)。
2. **Rooting / liveness** — GC collect を跨いで*何が生存するか*を決める。

どちらの仕事も重要だが、それらは**異なるレイヤ**によって保証されており、
「冗長な retain を削除する」fix は正しい方に照らして判断されねばならない。

**Rooting は既に、いかなる単一の retain とも独立に、2 つの shipping された
collector によって提供されている** (2026-07-02 検証、以下参照):

- **conservative** collector は*マシンスタック全体*をスキャンする — あらゆる
  JIT フレーム*および*あらゆる C++ ランタイムヘルパフレーム — に加えて
  setjmp-flush された callee-saved レジスタ (`jit_gc.h scan_roots`)。ヘルパの
  実行中のローカル (cell、upstream、half-built な closure) はそのスタック上に
  あるため、construction-time の collect はその refcount に関わらずそれらを
  root する。
- **gc_refs** collector (endgame) は refcount から root を seed する: refcount が
  in-heap 参照数を超えるオブジェクトはすべて root であり、marking はそこから
  推移的である。ネイティブフレームの `+1` *は*外部参照であるから、実行中の
  temporary は構築上 root となる — ただし RC 会計が正しい限りで、そしてそれこそ
  本書の所有権規律が確立するものである。gc_refs 下での rooting soundness ≡
  RC accuracy。

rooting のギャップが実在するのは、*保留中*の precise-shadow-stack 経路
(`jit_gc_design.ja.md` §12 Phase 2 wall) だけである: shadow stack は
ネイティブヘルパフレームやレジスタ内にまだある値を見られない。もしその経路が
いつか復活すれば、実行中の pinning 問題もそれと共に復活する。shipping 中の
ものはそれに依存しない。

**Task #4 の罠、再診断:** lazy-combinator リーク ([[project_jit_gc_rewrite]]
Task #4) に対する revert された 2 つの試みは、当初 rooting ギャップのせいだと
された。両 revert パッチを `CULEBRA_GC_NEVER=1` (*あらゆる* collect を無効化する
診断) で再実行しても、collector が全く走っていない状態で `test_iter.cul` を
8/8 でクラッシュさせる。したがって両クラッシュは**純粋な RC over-release**
(まだ参照が保持されている間の release-to-zero) であって、GC sweep ではない。
判別子は今や恒久的ツールである: `CULEBRA_GC_NEVER=1` で生き残るクラッシュは
所有権バグ、消えるものは rooting バグである。

**設計上の帰結:** *冗長に見える* retain が実は load-bearing かもしれない —
ただし*別の call site での所有権*のため (`+1` を渡さない caller、無条件に
release する consumer) であって、shipping された collector 下での rooting の
ためでは決してない。したがって、それを削除する前提条件は accounting 証明 —
そのオブジェクトの economy 全体の refcount trace とフルゲート — であって、
新しい rooting 機構ではない。

---

## 3. 先行技術と、それぞれが寄与するもの

| System | Mechanism | What we take | Why it doesn't fully fit |
|---|---|---|---|
| **Rust** | affine ownership + borrow checker、scope 末尾での `Drop` | 値の規律としての *move-or-drop*。borrow は決して free しない | codegen に borrow checker なし。cycle はなお backstop が必要 (`Rc` と同じ) |
| **C++ RAII** | destructor が cleanup を emit し、ホストの制御フローに従う | *実装の乗り物*: dtor が `release` IR を emit する C++ ハンドル、codegen 自身の early-return/例外を跨いで正しい | straight-line temporary しか自然にはカバーしない |
| **Swift ARC** | コンパイラが uniform convention から retain/release を*挿入*し、次いで peephole が冗長ペアを相殺する | **手で配置するな — convention から導出し、次いで最適化せよ** | ObjC-interop の convention は我々に必要な以上に重い |
| **Perceus (Koka)** | drop 特殊化 + in-place reuse 付きの、コンパイラ挿入の*precise* な RC | 最も近い一致: precise、決定的 drop、reuse。cycle collector と組む | functional IR 向けに設計。我々は dynamic AST を lower する |
| **Nim ORC** | RC + cycle collector (Bacon-Rajan) | 「RC + cycle backstop」を shipping な組み合わせ (我々が選んだモデル) として validate する | trial-deletion の cycle コスト。我々は既に mark-sweep を持つ |
| **Lobster** | compile-time lifetime 解析が*dynamic-ish* な言語でほとんどの RC op を elide する | dynamic な言語が RC を静的に elide できる証明 | 我々が持たない whole-program type inference に依存する |
| **MLIR / Swift SIL ownership** | 所有権が*IR 内に*ある。verifier + 専用パスが cleanup を挿入/check する | end state: comment による convention ではなく、check される IR プロパティとしての所有権 | 完全な ownership SSA は大きな build である |
| **Regions / arenas** (MLKit, Zig) | scope ごとに bump-allocate し、scope 末尾で region を free する | per-object RC ゼロで scope-bounded な temporary に一致する | escape/共有される値はなお RC が必要 |

この分野が収束し — かつ Culebra に fit する — 統合は、**Swift/Perceus 流:
コンパイラが uniform convention から RC 配置を所有し、プログラマは
retain/release を決して手書きしない**である。Culebra はこれを LLVM IR の上の
C++ RAII ハンドルで実現し、既に持っている scope 機構を再利用する。

---

## 4. Culebra の設計

レイヤ化された規律。各レイヤはリークする方法を 1 つずつ取り除き、合わせて
リークには*型/RAII 不変条件の破壊*が必要となる — それは C++ codegen における
コンパイルエラーであって、静かなランタイムリークではない。

### 4.1 uniform な所有権 convention (契約)

Single-source で文書化され、今日真である (Phase-0 fresh-source 所有権プローブ、
[[project_jit_gc_rewrite]] により検証):

- **`compile(expr)` は `+1`-owned な値を返す。** あらゆる式ノード。
- **Parameters / receivers は borrow (`+0`)。** callee フレームは*caller*が提供
  する 1 つの参照を消費する (caller は call の前に retain する)。フレームを持た
  ないネイティブ callee はそれを明示的に消費する。`this`/args はこれに従う。
- **Slots は所有する。** scope slot に格納された値はその slot が所有する。
- **Cells は store 時に retain しない。** `cell_new(tag,data)` は raw で格納し、
  *caller*が渡す参照を所有していなければならず、cell-release は格納された値を
  release する。(これこそ lazy-combinator の「internal retain」が load-bearing
  である正確な理由である — それが cell の owned 参照を供給する。)

convention が仕様である。以下すべてはそれを*強制する*機構である。

### 4.2 `Owned` — 一時的な `+1` 値のための唯一のハンドル

C++ RAII ハンドル (既に `jit.h` にあり、演算子で pilot 済み —
[[project_jit_gc_rewrite]] "B (Owned RAII) pilot" 参照):

- `Owned { JIT*, llvm::Value*, bool consumed }` — non-copyable, move-only。
- `.borrow()` — 消費**せず**に tag/data を読む (borrow 引数向け)。
- `.consume()` — `+1` を先へ渡す (slot、call、return へ)。spent とマークする。
  double-consume は **codegen-time の `assert`/abort** — バグが double-free では
  なく build failure になる。
- `.drop()` — `.consume()` の explicit-release の双子: release を**今**、現在の
  挿入点で emit し、spent とマークする。単に dead で渡されない値、かつその
  release が後続 IR (emit する `make_*`、branch arm、out-param load) より前に
  landing しなければならず、scope-exit dtor ではそれを遅すぎる位置に置いてしまう
  値のためのもの。デフォルト構築された (空の) `Owned` は no-op に drop するため、
  optional 引数を綺麗にモデル化する。
- destructor — まだ owned なら `emit_value_release` を emit する (RAII の `Drop`)。
- move-assign は先に古い値を release する。

`.drop()`/dtor は builder の*現在*点で release するため、`Owned` は
**straight-line temporary のみ**をカバーする。emit されたループの中で寿命が
1 ループ*反復*である値は straight-line ではない。そのような release は裸のまま
にする (あるいは `Owned` をループ本体領域にスコープする) — §6 参照。

`Owned` としてのみ保持される straight-line temporary は **C++ 型システムにより
leak- かつ double-free-proof** である: それらは codegen が取るどのパス
(codegen 自身の早期 return や例外を含む) 上でも、ちょうど 1 回消費されるか、
ちょうど 1 回 release されるかのいずれかである。`compile_power` の `**1` fast
path はその正準な実演である: `base` は一方の arm で `consume()` され、他方では
drop され、control-flow-correct な release が自動的に導かれる。

### 4.3 escape する値はちょうど 1 つの scope slot が所有する

現在の straight-line 領域より長生きしなければならない値 (名前に束縛、captured、
ループ/branch を跨いで保持される) は**scope slot** (`make_stack_slot` +
`define_var`) に `.consume()` される。既存の scope-unwind 機構が次いでそれを
**あらゆる退出パス**で release する:

- 通常の fall-through / block 末尾 → `pop_scope`
- `break` / `continue` → ループの cleanup エッジ
- 早期 `return` → `release_all_scopes_for_exit`
- 例外 → fn-level / lexical / match-arm の landingpad

これは「escape」のための*1 つ*の機構であり、各サイトでの bespoke な release では
なく、どこでも再利用される。**1 つの scope 内での同名 rebinding は、上書きの前に
先の slot の owned `+1` を release する** (`define_var` → `release_slot_value`):
LIFO teardown リストは 1 つの名前について最初の slot だけを記録するため、静かな
上書きは以前の束縛を孤児にしてしまう。正準なトリガは、同名パラメータによって
shadow された暗黙の `self`/`this` slot (recursion / receiver のため無条件に束縛
される) であった — これは lint パスも今や pre-eval で reject する
(`self`/`this` は予約パラメータ名)。したがって 2 つの fix は belt-and-suspenders
である: reject がソースを止め、release が*任意の* rebinding を構築上 leak-free に
保つ。同じ slot 機構は**branch-spanning オペランド**も anonymous region scope
として運ぶ: 3 項以上の比較連鎖 (`a < b < c`) は short-circuit ブロックを跨いで
lower されるため、各オペランドは連鎖の周りに push された scope の owned
`cmpchain.N` slot に消費される (`compile_match` の subject パターン) —
entry-nil-initialised な slot は、共有された merge が取られたパスが materialise
したものを正確に release することを意味し、領域の cleanup pad は throw エッジを
カバーする。比較に入る 2 つの slot はその call についてだけ blank にされる:
比較ヘルパはそのオペランドの throw エッジを所有する (§4.7) ため、領域もそれらを
release すると double-free になる。(論理演算子はこれを一切必要としない: strict
truthiness は `to_bool` によって release されるオペランドを持つヒープ条件を
reject し、`??` はヒープ候補を決して上書きしない — nil だけが置換される唯一の
ものである。) ループは反復ごとに fresh な scope を得る (既にそう) ため、
per-iteration temporary は毎回 drop される。`dispose` を運ぶ
イテレータ/ジェネレータは同じ scope 機構でそれを register するため早期退出が
それを走らせる — ここでのギャップは現行の for-in raw-alloca イテレータ
([[project_jit_gc_rewrite]] Task #2) であり、これは*欠落した* scope 登録、
まさにこのレイヤが構造的に閉じる種類の穴である。

**機構ノート — 2 つの退出ファミリは異なるコードで cleanup され、fix は両方を
カバーしなければならない** (container-literal 例外安全性の revert された最初の
試みで学んだ):

- **Compile-time-emitted な退出** (通常の fall-through、`break`/`continue`、
  早期 `return`) は `release_scope_slots` / `release_all_scopes_for_exit` を
  走らせ、これはその時点で既知の scope について release を*静的に emit* する。
  fresh な `make_stack_slot` + `define_var` はここで無償にカバーされる。
- **Runtime exception unwind** はそれらの compile-time emitter を fall-through
  しない — cleanup landingpad に landing する。歴史的にその landingpad は
  `defer_run_to(mark)` だけを走らせていたため、throw は `release_scope_slots` と
  owned-region drop を丸ごとスキップした: scope の owned ローカルはリークし、
  その `drop()` は `throw` で決して発火しなかった (fall-through / `return` のみ)。
  **これは今や閉じられている** — 下記「Generalized scope cleanup」参照。

このレイヤへの帰結: construction 領域を例外安全にするには、実行中の値の release が
scope slot ではなく例外エッジに乗らなければならない (scope slot は
normal/return 退出のみをカバーする — `make_stack_slot` のみの「fix」は throw パス
で静かにリークする。検証済み: slot を置いても `[mkheap(), boom(), mkheap()]` は
まだ ~2/loop リークした)。

**実装された機構 — per-region cleanup landingpad、can-throw で gate**
(container literal 向けに shipped。`compile_array` / `_object` / `_tuple` / `_set`):

- 空の `cleanupBB` を作り、element/key/value コンパイルの周りだけで
  `current_lpad_` に設定する。throw し得る sub-expression は unwind エッジが
  `cleanupBB` を狙う `invoke` にコンパイルされ、throw しないもの (単純な
  identifier / literal) は普通の call にコンパイルされてエッジを追加しない。
- construction 後、`finish_construction_cleanup`: `cleanupBB` に前任者が無い
  (何も throw し得なかった) なら**消去する** — happy-path オーバーヘッドゼロ、
  これが `[this, o]` / `[1.0, 1.0]` が何も払わない理由である。さもなくば
  「partial な値を release し、外側の landingpad に re-raise する」で埋める。
- partial な値は **entry alloca** (`make_build_guard`) を通じて cleanup に渡され、
  landingpad で load される — invoke エッジを跨いで live な call-result SSA として
  ではない (その形は SDAG-O0 の register coalescer をクラッシュさせる)。
  `make_pending_guard` / `clear_pending_guard` は、risky な call を跨いでまだ
  消費されていない実行中の temporary (value より前のヒープオブジェクト key、
  extend/merge より前の spread source) をカバーする。各 guard はその window の
  外では nil であり、nil の release は no-op である。
- landingpad は関数が personality を持つことを要求する (さもなくば leaf function
  は持たない — null-personality codegen クラッシュ)。cleanup が実際に emit される
  ときに idempotent に設定する。

これは release を **defer stack** に register するより好まれる: defer stack は
invoke すべき*closure*を格納し (literal ごとに重量級)、通常退出でも走る
(cancel が必要) が、cleanup landingpad は例外パス専用で、不要なとき何もない
dead-code-eliminate になる。同じ landingpad の形は for-in プロトコルループにも
使われる。その raw-alloca イテレータの早期 `return` もカバーされる
(compile_return が `iter_cleanup_stack_` を walk し、同じ dispose+release を
innermost first で emit する)。

**Generalized scope cleanup — `finish_scope_cleanup` (shipped)。** 同じ
per-region で pred_empty-gated な landingpad が今や*あらゆる* scope 様領域を
backing するため、`throw` は fall-through がするのと正確に同じく owned slot を
release し `drop()` を発火する。例外エッジ上では領域の通常退出をミラーする —
領域の defer を走らせ (`defer_run_to`)、次いで `release_scope_slots` (非-escape な
各リソースについて refcount-0 経由で `drop` を発火) — 次いで囲む landingpad に
re-raise する。これは scope がまだ live な top-of-stack である間に走らねばならない
(その slot alloca は entry-zero-initialised なので、束縛が代入される前の throw は
nil を release する)。配線先: lexical scope、`while` / `for` 本体、`match` arm
**および match subject scope**、`try` 本体 (catch ハンドラが走る前に release
される)、**`for` 文自身の iterable scope** (Set 分岐の `set_to_array` temp も
保持する — この配線の前は for-in を unwind して抜ける throw が iterable の slot
参照を strand させた: `drop` なし、かつフレームが再入されない限り実際の
per-call リーク。なぜなら再実行の宣言 store だけが以前の strand を release する
から)、そして function フレーム — fn-level pad は今や本体が throw し得るとき
(fn-level `defer` があるときだけでなく) 常に emit される。これは owned ローカルが
*フレーム* scope に住む関数をカバーする。なぜなら本体 `BLOCK` はそのフレームに
コンパイルされ、ネストした `LEXICAL_SCOPE` にではないから。escape 済み/循環 drop
の残りはフレームレベルで一度だけ解決される
(`release_all_scopes_for_exit` の `owned_scope_exit` がフレームマーク全体に渡る)、
scope ごとではないため、各 cold cleanup block は最小に保たれる。通常退出の
scope teardown は `current_lpad_` を null に保つため、その `owned_scope_exit` は
普通の call であり、cleanup pad の前任者に決してならない (それは DCE を無に帰す)。
順序はインタプリタの unwind に一致する (リソース release より前の defer) ため、
`defer`/`drop` は throw パス上で同一に interleave する。コスト: may-throw call を
持つ関数は今や普通の `call` ではなく `invoke` (unwind エッジ) を emit する、
小さな一度きりの compile-time 増加。per-step 実行は変わらない。

### 4.4 Borrow は release できない

borrow された値は `.borrow()` (または別個の `Borrowed` view) を通じてのみ扱われる。
borrow と release の両方を行う API は存在しないため、「borrow されたオペランドを
release する」(double-free の出所) は表現不可能になる。

### 4.5 Rooting は GC レイヤの仕事であり、refcount の仕事ではない

§2 の通り: collect を跨ぐ liveness は既に collector 自身が保証している —
conservative scan はあらゆるネイティブフレームをカバーし、gc_refs は外部
refcount を持つものを root する。実行中のヘルパ temporary のための
scoped-pin / shadow-stack 機構は不要である (そしてこの目的のために追加しては
ならない。shadow-stack codegen は dead オーバーヘッドとして一度既に除去された)。
rooting が独立に保証されているため、`Owned`/slot 所有権は純粋に release
タイミングについてのものであり — refcount を最小化するための安全条件は
RC 会計の正確さであって、変更ごとに refcount trace、`CULEBRA_GC_NEVER` 判別、
フルゲートで証明可能である。

### 4.6 Cycle は count ではなく sweep する

参照 cycle はいかなる RC 規律の範囲外である (Rust も同じ `Rc`-cycle ギャップを
持つ)。mark-sweep backstop (または `gc_refs`) が cycle と残余を reclaim する。
所有権が single-source かつ正しければ、backstop は **non-load-bearing** になる
(per-step ではなく稀)、これがパフォーマンス上の眼目のすべてである。

### 4.7 ヘルパ所有権契約 — the callee-cleans table

may-throw ランタイム call を跨いで live な、codegen が所有する各 `+1` は、この
閉じた契約集合から引かれる、unwind エッジ上のちょうど 1 つの cleaner を持つ。
この表は (今のところ人間可読な) 入力であり、GAP4-ENFORCE 会計パスがそれに
照らしてチェックする: `invoke` を跨いで `+1` を保持する emitter は、どの行が
それをカバーするか名指せなければならない。

| Contract | Mechanism | Who uses it |
|---|---|---|
| **Caller-cleans (codegen)** | `ThrowGuard` — RAII として package された per-region cleanup pad。pred-empty DCE が happy path を無償に保つ | builtin-method receiver とオペランド引数。`compile_call` の callee closure。final-assignment switch を跨ぐ assignment lvalue。UFCS free-function callee。sort_by の `reverse:` coercion window (callback の `+1` は sorter が entered されるまで owner を持たない) |
| **Callee-cleans-on-direct-throw** | ヘルパ内の `JitUnwindRelease` — scope が unwind する場合に限り release する。user dispatch が declined した後にのみ arm される | operator entry (arith / ordering / `==` coercion / `to_bool` / neg / matmul)。`object_get_any` (`own_receiver` 下の receiver、加えて refcount された key)。`culebra_runtime_prop_get` (`own_receiver` 下)。`_culebra_expect_callback` (reject throw、全 tag)。`compile_function_call_raw` の not-a-function エッジ (call site ごとに flag-gated: `own_this_on_error` / `own_args_on_error`)。`dispatch_arr_iter` の receiver-resolution エラー block (HOF runtime のため既に consume() された値を release する。runtime はそのエッジで決して走らない) |
| **Callee-consumes-on-every-exit** | entry で宣言された `JitOwnedVal` / `JitMethodSelf` / `JitMethodArgs` — normal return と unwind の両方で release する | ネイティブ method endpoint (sendable な channel/isolate/shared-val/buffer method、FixedArray method、`@wrap` foreign thunk)。`@packable` store。HOF ヘルパ accumulator (`out`/`acc`/計算された sort key)。**HOF callback 自体** (eager driver 向けの `JitHofCallback`、lazy factory の capture cell) |
| **Invoker-cleans** | `_culebra_invoke_method*` 内の `JitUnwindRelease` — invoker がオペランドを retain し、callee フレームが NORMAL return でそれらを release し、invoker の guard は callee が throw する場合に限りそれらを release する | あらゆる user-dispatch window (`__op__` / `eq` / `hash` / `cmp` / `__index__` / getter body)。系: derived-method thunk (`_jit_derived_thunk_consume`) は normal path でのみ release する — throw でも release すると、invoker が unwind した時点で double-free する |
| **Transfer** | 入ってくる `+1` を結果として返す (または capture cell / slot に渡す) | iter-self method。lazy combinator の cell への `_culebra_capture_callback`。`emit_point_index` の uniformly-`+1` な結果 |

表から導かれる gate ルール:

- フラグ `own_receiver` (helper ABI) と `own_this_on_error` /
  `own_args_on_error` (emitter パラメータ) は同じ宣言 — 「この call site は値を
  所有する。ヘルパはその direct-error エッジ上の唯一の releaser である」— を
  2 つのレイヤで表現したものである。owned な値を渡された状態で direct error を
  raise し得る新しいヘルパは、新しい cleanup の形を発明するのではなく、同じ
  gate を取らねばならない。
- 2 つの契約が 1 つのエッジ上で決して overlap してはならない: 同じ値上の 2 つの
  cleaner は double-free する (ASan で 3 回確認済み: 演算子オペランドは user
  dispatch を跨いで codegen-side で guard された。pre-unification な HOF blanket
  guard。callback が callee-consumes になった後の callable adapter の throw-path
  release)。ヘルパの相互排他は常に「user dispatch が declined した」— これこそ
  `JitUnwindRelease` が dispatch 試行の後に arm され、その周りでは決して arm
  されない理由である — であり、consume された値については「owner が entered した」
  — これこそ consumed な値を跨ぐ codegen guard が consuming call の前に close
  しなければならない理由である。
- 既知の順序敏感な箇所が 1 つ: `wrap.h` の `jit_check_args` は binder-throw で
  自ら `self` を release するため、thunk の `JitMethodSelf` はチェックの**後**に
  宣言される — 先に宣言するとそのエッジを double-free してしまう。

### 4.8 自動 unwind-temp window (GAP4-ENFORCE)

§4.2 の `Owned` ハンドルは C++ destructor を通じて release するが、ランタイム
LLVM レベルの throw はそれを走らせられない。歴史的にこれは、**あらゆる**
「may-throw call を跨いで live な codegen が所有する `+1`」が、誰かが手で guard を
置かない限りリークすることを意味した: rhs がコンパイルされる間の binop の lhs
(`mk() + boom()`)、後続引数がコンパイルされる間の引数 (`g(mk(), boom())`)、
key がコンパイルされる間の index receiver (`mk()[boom()]`)、その引数がコンパイル
される間の method receiver。leak-fuzzer corpus はこれらの形を決して綴らなかった
ため、C①–⑨ はそれらなしで shipped した。six-shape プローブがそれらすべてが
リークしていることを確認した。

サイトを 1 つずつ guard する (§0 がそれを禁じる) 代わりに、所有権レイヤは今や
**構築によって** unwind エッジを所有する:

- **Registry.** ヒープ可能な値を保持するあらゆる live な `Owned` は JIT に
  register される (定数スカラーはスキップ)。登録は純粋な codegen bookkeeping
  である。consume/drop/move がそれを維持する。registry、slot pool、coverage
  stack は LLVM 関数ごと (`CompilerStateSaver` がそれらを swap する)。
- **Window.** `emit_call`、may-throw call がその unwind エッジを得る単一の点は、
  あらゆる live で uncovered な `Owned` を per-function pool slot (entry alloca、
  nil-initialised) に、ちょうどその 1 つの invoke の間だけ spill する: 前に
  store し、continuation で nil-clear する。coverage は構築上完全である —
  unwind し得る唯一のランタイムイベントは `emit_call` が emit する call である。
  live な temp を持たない call は何も払わない。
- **Release.** あらゆる scope-family の cleanup pad (`finish_scope_cleanup`、
  fn-level pad) は pool を最初に release する — 領域の defer より前に、
  インタプリタに一致させて。インタプリタでは throw する式の temporary は、
  囲むブロックの defer に先立って eval フレームが unwind するにつれ死ぬ —
  次いで各 slot を nil-clear するため、同じ chain 上の外側 pad は no-op になる。
  window の外ではあらゆる slot が nil なので、cold-path コストは一握りの no-op
  release である。
- **Coverage (`UnwindCovered`).** §4.7 契約が既に別の unwind-edge releaser を
  名指しているところ — 演算子ヘルパの direct-error release、user
  `__op__`/getter dispatch を跨ぐ invoker guard、`ThrowGuard` 自身の pad — では、
  値はその領域について covered と宣言され、window はそれをスキップする。
  それも spill すると double-free する (§4.7 overlap trap、ASan 確認済み)。
  `ThrowGuard` は自身の値を自動的に covered と宣言する。手書きの宣言は契約 call
  site にちょうど座る (`emit_binop_dispatch`、`emit_comparison_i1`、
  `value_to_bool`、neg/matmul/pow、`own_receiver` 下の `prop_get`、getter
  dispatch、well-known-property チェック) ため、§4.7 表は今やコード内に
  machine-visible な footprint を持つ。
- **Timeline discipline.** `consume()` は「ここから先の emit されたコードが
  `+1` を所有する」の codegen-time マーカーであるため、それはその値を実行時に
  消費するコードを emit する**前**に呼ばれねばならない — 後ではない。遅く消費
  した 2 つの emission hook (`try_fuse_iter_map_collect`、ufcs-builtin hook) は、
  emit されたコードが既に release した値を window が spill させた (ASan 確認済み
  の teardown UAF)。両者は今や up front で consume し、declined (no-IR) hook 上で
  re-own する。
- **Kill switch.** `CULEBRA_JIT_NO_UNWIND_TEMPS=1` は window off でコンパイル
  する — ASan レポートが window double-free か既存の over-release かを判定する
  ための判別子 (CULEBRA_GC_NEVER の兄弟)。

このレイヤは container literal 内の手書き pending-guard store (spread source、
value を待つヒープオブジェクト key) を subsume した — それらの `Owned` ハンドルは
今や window-covered であるため、手書き store は消え、`make_pending_guard` は slot
プリミティブとして生き残る (`make_build_guard` 用と window 自身の pool slot 用)。
`tools/difftest/leak_abort.sh` Case 4 で pin される (six probe 形は teardown
audit 下で静かなままでなければならない)。

### 4.9 Block-pinned raws — 裸の `+1` は basic block を跨がない (raw-across-BB ENFORCE)

§4.2–§4.8 は `+1` が**ハンドル内にある間**それを保護する。最後の escape hatch は
それがハンドルを離れる瞬間だった: `consume()` は裸の `llvm::Value*` を返し、
codegen はそれを basic-block 境界 (branch、phi merge) を跨いで運べた。そこでは
どのレイヤもそれを追跡しない — 1 つの CFG エッジ上の見逃された release が静かな
リークだった。最近のあらゆる product リーク (comparison chain、UFCS-kwargs
receiver、compound `obj[k] op=` key、`slice(start, end)` の第一引数、fn/return
/try epilogue) はこの class だった。本節はそれを閉じる。

- **不変条件。** 裸の `+1` は、それが consume された basic block 内でのみ使える。
  これは *sound かつ complete* である。なぜなら `emit_call` はあらゆる may-throw
  call を、現在のブロックを終端する `invoke` に変える (§4.8) から: 同じブロック
  ⟹ unwind エッジなし、かつ値が裸である間 branch が走らなかった。
- **`Pinned`.** `consume()` は pin block を記録する `Pinned` トークンを返す
  (定数と null sentinel は免除 — strand すべき `+1` がない)。その
  `llvm::Value*` への変換は builder がまだそのブロックに座っているか確認する。
  違反は `rc_pin_violation` を呼ぶ — **あらゆる build mode** で、codegen ソース
  位置と pin→use ブロック名付きの loud abort。difftest corpus (5641 ケース) は
  ほぼあらゆる構文をコンパイルするため、違反パターンはそれが*コンパイル*される
  最初の時に catch される — ランタイムリーク repro は不要。
- **`OwnedPhi`.** チェック済みの merge 構文: jit.h 内のあらゆる `%Value` phi は
  それを通じて構築される (ratchet: hand-built `CreatePHI(valueType_` = 0)。各
  incoming は**その arm ブロック内で**宣言される — `add_incoming(Owned&&)` は
  その場で consume する。raw な incoming は定数、現在のブロックで生成されたもの、
  または builder が座る normal dest を持つ invoke でなければならない — そして
  `finish(mergeBB)` は記録された各 arm の terminator がまだ merge を狙っているか
  検証する (`add_incoming` の後に紛れ込んだ `emit_call` は arm をその `call.cont`
  へ re-terminate していたはず)。同一の phi IR を emit する。安全性は codegen-time
  bookkeeping である。
- **`consume_unchecked()`.** 正当化された escape hatch、各サイトが 1 行の根拠を
  持ち ratchet でカウントされる: mutually-exclusive な dispatch arm への batch
  handoff (各 arm がそのランタイムパス上の唯一の releaser — `consume_all`
  パターン)、その `+1` が scope slot に所有される crossing (§4.3)、そして
  `declare_local` への prologue transfer。
- **`compile_*` return seam は型付けされた (2026-07-12 に closed)。** あらゆる
  `compile_*` ヘルパ — `compile()` の dispatch の背後にあるノードコンパイラ、
  call ファミリ全体、extension compile hook (`ExtensionHooks`)、およびそれらの
  背後の stdlib 実装 — は `Owned` を返す。decline するヘルパは空のハンドルを
  返す。`compile()` の古い `own(compiled)` re-owning 境界は消えた: switch 結果
  *が* `Owned` であるため、裸の `+1` は決して `compile_*` の C++ return を
  跨がない。return 型は今や所有権契約をエンコードする — `Owned` = `+1` が
  transfer する、raw `llvm::Value*` = borrowed/scalar、これこそ 2 つの
  borrowed-contract emitter がファミリから rename された理由である
  (`emit_property_get` は `+0` を返す、`emit_comparison_i1` は `i1` を返す。
  `emit_interp_fragment` は C-string ポインタを返す)。Ratchet:
  `llvm::Value*`-returning な `compile_*` = 0、jit.h + stdlib_jit.h 内で。残る
  唯一の raw 形は明示的に型付けされた `llvm::Value* x = ….consume();` 代入
  (代入時に変換する — これも 0 に ratchet)。

Phase-2 flip は既知バグを超えた real-strand ファミリを表面化し fix した:
scope teardown / defer run / 第二引数のコンパイルを跨いで裸で保持された値
(fn & return epilogue、try/catch と match-arm 結果、lvalue postfix chain、
`obj.get/get_or_put` key、compound-assign key、`slice` bounds、`take_while` の
callback、destructure rval)。各々は今や `Owned` に乗るため、§4.8 window が、
かつて strand したエッジ上でそれを release する。

---

## 5. リークを不可能にする不変条件

これらが成り立てば、リークには C++ 型/RAII 不変条件の破壊 (build-time failure) が
必要であり、ランタイムの偶発事故ではない:

1. あらゆる `+1` transient 値は `Owned` に保持される (または即座に consume
   される)。
2. `Owned` はちょうど 1 回 consume される**か**ちょうど 1 回 drop される —
   両方でも、どちらでもなくてもない (move-only + dtor + double-consume assert に
   より強制)。
3. あらゆる escape する値はちょうど 1 つの scope slot に consume される。scope
   unwind はあらゆる退出パス (normal/break/continue/return/throw) で各 slot を
   release する。
4. borrow された値は決して release されない (それを許す API がない)。
5. Rooting は所有権 refcount と独立に GC レイヤが提供する。
6. Cycle + 残余は backstop が reclaim する。backstop は steady-state メモリに
   ついて依存されない。
7. may-throw call を跨いで live なあらゆる `Owned` `+1` は unwind エッジ上に
   ちょうど 1 つの releaser を持つ: デフォルトでは自動 unwind-temp window
   (§4.8)、またはその call site が `UnwindCovered` で宣言する §4.7 契約 —
   両方ではない。
8. 裸の `+1` はそれが consume された basic block 内にのみ存在する (`Pinned`、
   §4.9)。あらゆる `%Value` phi は `OwnedPhi` を通じて構築される。意図的な
   crossing は per-edge releaser を宣言した `consume_unchecked` サイトであり、
   tools/check_rc_discipline.sh で ratchet される。
9. どの `compile_*` ヘルパ (core または extension hook) も裸の `llvm::Value*` を
   返さない — `+1` は `Owned` 内でのみ compile-layer の C++ return を跨ぐ
   (§4.9、0 に ratchet)。raw-returning な `compile_*` シグネチャは reopened な
   seam であって、スタイルの選択ではない。

系: 正しい codegen パスは裸の、手書きの `retain`/`release` を含まない。既存の
裸 call は migration debt であって、パターンではない。

---

## 6. 状態と移行パス

- **Done:** `Owned` ハンドルは存在し、6 スライスで zero-behavior-change
  (byte-identical IR、または leak-fix-only な IR デルタ) が証明されている。
  GC/leak/difftest ゲートは green
  ([[project_jit_gc_rewrite]]):
  - **operators** (binary/comparison/unary) — pilot。
  - **call arguments** — あらゆる ARG_LIST producer (positional、static-kwargs
    resolver、runtime-kwargs bucket) がその `+1` を `Owned` として保持する。
    `consume_all(vector<Owned>&&)` は raw call emitter への単一 handoff であり、
    その raw は mutually-exclusive な dispatch 分岐 (method/UFCS、call/`__call__`)
    から参照され得る — ランタイムパスごとに 1 consume。
  - **method receiver** — `compile_method_call` は `Owned receiver` を取る。
    consuming な子は明示的な `.consume()` handoff を得る。borrowing な arm
    (明示 `drop`、builtin method) はハンドル dtor に依存し、ufcs-builtin hook
    契約は明示的である (non-null 結果 = hook が `+1` を消費した)。
  - **postfix chain** — `compile_call` / `compile_call_with_builtins` は chain
    値 (callee → property view → index result → call result) を 1 つの `Owned`
    ハンドルを通じて回す: call dispatch は borrow し、fresh 結果の move-assign が
    前のリンクを release する。INDEX は `emit_index_step` に consume する。
    property-get の `swap_owned` arm はハンドルを consumed とマークする
    (ランタイムプリミティブが receiver の `+1` を view のそれと交換する)。
    `try_fuse_iter_map_collect` は hook 契約に従う (AST 形だけで decline、hit は
    `+1` を消費した)。
  - **container-literal elements** — array/tuple/set/object literal は各 element、
    spread source、key、value を `Owned` として保持する。slot-absorbing な
    ランタイム call は明示的な `.consume()` 点であり、spread source はハンドルを
    scope するため dtor が手書き release のあった場所に landing する。
    exception-path pending/build-guard 機構は手つかず (Owned レイヤは通常パス
    のみをカバーする)。このスライスの receiver-map パスは real SIGSEGV を
    catch した: `[](n, heapDefault)` は 1 つの `+1` の unretained なエイリアスで
    n slot を埋めた (fix 済み: `array_resize` は slot ごとに retain する。
    default は borrow される)。
  - **decorator callees** — 3 つの application site (fn/class/enum 宣言) は
    コンパイルされた decorator を `Owned` として保持する。raw call はそれを
    borrow し dtor がそれを release する (let-bound な closure decorator は
    application ごとに 1 つの callee 参照をリークしていた)。ufcs-builtin hook の
    throw arm は同じパスで audit された: 各 arm は今や raise の前に
    receiver/arg `+1` を消費する (そこでは release-before-throw は安全である —
    `value_to_long` のエラーパスは決して `data` を deref しない)。
  - **iter-protocol dispose/iterable** — プロトコルの見かけ上のバランスは
    補償ペアだった: proto 分岐は iterable の slot 参照を孤児にし、一方 dispose
    call はイテレータを double-consume した (frame consume + skipBB release)、
    そして両者はちょうど self-returning イテレータ (`iter()` = `this`、すなわち
    ジェネレータ) についてのみ相殺した。distinct なイテレータではペアが壊れた:
    iterable の `drop` は JIT 下で決して発火せず、skipBB release は既に解放された
    イテレータに landing した — 単なるリークではなく scale での real SIGSEGV。
    両側を同時に、1 つの変更として fix: dispose call はイテレータをフレームに
    渡す前に retain し (iter()-call convention)、proto 分岐は iterable の `+1` を
    ループの scope slot に保つため pop_scope がそれをちょうど 1 回 release する、
    range/keys 分岐のように。Drop/dispose タイミングは両イテレータ形について
    interp-symmetric である (`tests/test_iter_dispose_ownership.cul` および
    battery パターン `forin_proto_iterable` / `forin_generator` で pin される —
    前者は pre-fix バイナリをクラッシュさせる)。
  - **lazy-combinator receiver** — `dispatch_arr_iter` の obj arm は receiver を
    retain したが、どの arm もそれを consume しない (lazy factory は内部で自分の
    `+1` を作る。terminal driver は値を pull するだけ。receiver の `Owned`
    ハンドルは既に一度 release する)。それは `.iter().filter/flat_map/find/any`
    上で ~9 objects/loop リークした。それを除去する (borrow、eager Array arm と
    uniform) と、2 つ目の pre-existing バグを unmask した:
    `_iter_flat_map_fast_fn` が pull した値を、`_culebra_invoke1` が既にそれを
    消費した*後で* release した — ヒープ element 上の identity callback
    `fn(xs){xs}` が inner array を double-free する (integer がそれを隠した:
    Long の release は no-op。receiver リークがヒープを alive に保つことで更に
    隠した)。これはまさに §2 の教訓である — rooting ギャップではなく、他所の
    over-release を隠していた冗長に見える retain (`CULEBRA_GC_NEVER` 診断が
    あらゆる collect 無効化でクラッシュが RC over-release であることを証明した)。
    先の Task #4 の両試みは flat_map fix を欠いたためここでクラッシュした。
    battery パターン `iter_filter_lazy` / `iter_flat_map` / `iter_find_driver` /
    `iter_any_driver` で pin される。
  receiver フローの mapping はさらに 5 つの real な receiver `+1` リークを表面化
  した (auto-`parameters()`、Set `.add`/`.remove` fast dispatch、fused
  `map+collect`、イテレータ上の inlined-lambda `for_each`/`reduce`) — fix され、
  leak-battery パターンで pin された。決定的 drop RC と mark-sweep backstop は
  shipped 済み (`jit_gc_design.ja.md`)。
- **flip 自体は完了: `compile()` は `Owned` を返す。** あらゆる producer は今や
  その `+1` を move-only ハンドルを通じて渡すため、*新しい* call site はいずれも
  明示的な所有権の選択 — consume、borrow、または drop — をせねばならず、二重の
  handoff は codegen time で assert する。変換は構築上 behaviour-preserving で
  あった: 以前変換済みのサイト (`own(compile(…))`) は今やハンドルを直接取り、
  残るあらゆる legacy サイトは call で consume する (`compile(…).consume()` —
  まさに古い raw `+1` 契約)、-O2/-O0 で byte-identical IR とフルゲートで検証済み。
  `.consume()` call は変換すべきものの grep 可能な棚卸しである。
- **through-line 作業 (audit された残余まで完了):** それらの `.consume()`
  クラスタを real な `Owned` フロー (dtor release、borrow) にクラスタごとに変換
  し、各変換で手書き retain/release を削除する — §5 の不変条件が codebase 全域で
  成り立つまで。**これまでに完了** (スライス A1–A6、すべて `--emit-llvm` 0-diff
  で検証済み — `jit.h` 内の手書き `emit_value_release` は 81→45): あらゆる
  `compile_builtin_method` HOF callback (map/filter/for_each/reduce/find/
  any-all/flat_map、および lazy-iterator factory take_while/chain/zip と array
  sorter sort_by/sorted_by — A5)、あらゆる builtin-method オペランド引数
  (tensor pow/reshape/dot/linear_sigmoid、set union/intersect/diff/sym_diff/
  subset/superset、string join/index_of/contains/tr/trim/split/split_iter/
  starts_with/ends_with) が今やその値を `Owned` として保持する。さらに 2 つの
  straight-line サイトが同じ道を辿った (A6): defer thunk の closure
  (`own(emit_closure_build(...))`) と array rest-destructure の sliced tail
  (two-arm consume-or-drop — sink `..._` arm は drop し、named arm はその `+1` を
  `declare_local` に渡す)。2 つの形が再帰する: release が `return <value>` の
  直前に座っていたところではハンドルは単に scope から落ちる (裸の
  `return <value>` は IR を emit しないため dtor が同じ点に landing する)。
  release の後に IR が続く (a `make_*`、branch arm、out-param load) ところでは
  `Owned::drop()` を使う (今 release、spent とマーク) か C++ block scope を使う。
  **`Owned::drop()` (§4.2) はまさにこのために追加された。**
- **straight-line flip は完了。残るあらゆる裸 release は文書化された carve-out で
  ある。** A1–A6 の後、`jit.h` に残った ~45 の `emit_value_release` はサイト
  ごとに audit され、各々は `Owned` ハンドルがカバーする*意図でない* class
  (§4.2) に落ちる。どれも clean な straight-line `compile()`-temp discard では
  ない — それらはすべて変換済みである。carve-out class:
  - **loop-body / inline-loop emitter** (`emit_inlined_*`、`*_inline_loop`、
    for-in/iter body と dispose/iterable cleanup、build-guard cleanup ループ):
    per-*iteration* な `+1` で、その scope-exit dtor は builder の現在点で発火する
    — emit されたループ内の誤った場所。
  - **slot / scope プリミティブ** (`store_slot_raw` が古い値を release する、
    scope-slot release ヘルパ): `Owned` レイヤの下のインフラ。
  - **threaded chain 値** (`emit_index_step` の receiver、postfix lvalue
    temporary): 値は chain を下って渡される関数*パラメータ*であって、
    straight-line temporary ではない。所有権は threaded であって scoped では
    ない。
  - **branch-spanning consume-or-release** (point arm が消費するが slice arm が
    release する non-literal index `key`。in-place-Tensor rebind 分岐を跨ぐ
    compound-assign `cur`/`rval`/`lval`): 一方の arm が consume し、別の arm が
    release する — 単一 dtor は両方をモデル化できない。
  - **conditional release** (defer body 結果、block が terminator を持たない
    ときだけ解放される。scope-exit dtor は terminated/unreachable なパスに
    release を emit してしまう)。
  - **statement-sequencing** (`compile_statements` / standalone block): 結果は
    loop-accumulate され、次の文がコンパイルされる*前*に解放される — 素朴な
    move-assign 書き換えは drop をそれより後にずらし、インタプリタが観測する
    タイミングを変える。(変換済み: ループは今や各 `compile` の前に明示的な
    `drop()` を持つ `Owned` に乗り、これは手書きのものが占めていた同じ命令に
    release を landing させる。)
  - **shared-receiver dispatch children** (`compile_user_method_over_builtin`
    / set-mutate / method-or-ufcs): receiver/callee raw は runtime-exclusive な
    arm を跨いで共有される。パスごとに 1 回消費され、scoped ではない。
  プローブ sweep はこれら全体で**通常パスのリークなし**を発見した (statement
  discard / `cond` / guard / interpolation / `??` は flat。`if`/`while`/logical
  condition は Bool/Long/Float のみを受け入れるため、ヒープ `+1` は通常パスで
  それらに到達できない — それらの*throw* エッジだけがヒープ参照を運び、
  per-region cleanup pad の仕事である、§4.3)。28 の `emit_value_retain` は dual
  集合 (slot store、pattern binding、`this` handoff) — 正当な所有権*生成*で
  あって debt ではない。これらの裸 call は裸のまま残り、
  `tools/check_rc_discipline.sh` ratchet でカウントされ (population は縮小のみ
  可能)、CI leak battery で guard される ([[project_gc_safety_phase_plan]])。
- **Throw-path cleanup — 全面的に shipped (leak-fuzzer C①–C⑨、2026-07)。**
  leak-fuzzer (loud teardown audit `CULEBRA_GC_LEAK_ABORT` 下で実行) は、corpus の
  *throw する*ケースが growth gate で決して測定されていなかった (その `_p` warmup
  が throw を catch してスキップする) ことを示したため、「cycle-only」は
  non-throwing サブセットについてのみ成り立っていた。throw エッジは 2 つの
  相補的機構によって閉じられ、実行中の `+1` を誰が所有するかによってサイトごとに
  選択される:
  - **Codegen がそれを所有 → `ThrowGuard`** (RAII ハンドルとして package された
    §4.3 cleanup pad): `compile_call` の callee closure (callee フレームは
    `__cls__` を borrow するため throw がそれを strand した)、builtin-method
    receiver とオペランド引数 (`[1,2,3].enumerate("x")`)、final-assignment
    switch を跨ぐ assignment lvalue (ImmutableError 上の `o.a = v`)、
    tensor/set/string オペランド引数、UFCS free-function callee、および HOF
    callback (下記 tag-gated ノート参照)。
  - **ヘルパがそれを直接 throw → callee-cleans-on-direct-throw。** *direct* な
    エラーを raise する — user dispatch が走らなかった — あらゆるランタイム
    ヘルパは、raise の前に渡された owned オペランドを release する。これは user
    `__op__`/method dispatch (その callee フレーム + invoker の unwind guard が
    既に throw を cleanup する。そこで guard すると double-free、ASan 確認済み) と
    相互排他であるため、各エッジはちょうど 1 つの releaser を持つ。shipped
    したもの: 演算子ヘルパ (`_arith_guard_numeric`、`num_matmul`/`num_neg`、
    `to_bool`、および sort comparator が borrow 契約を保つよう
    `_value_<name>_borrow`/public split 経由の ordering op — C①)、index/slice
    (`own_receiver` gate 下の `object_get_any` — C③、C⑤ で shared-val arm に
    拡張)、not-a-function エラーパス (`compile_function_call_raw`、
    unresolved-builtin パスが既にそのエッジで receiver を解放する — blanket
    release は double-free する — ため call site ごとに `own_this_on_error`/
    `own_args_on_error` で gate)、`==`/`!=` の non-Bool coercion (C⑤)、
    property-read cold path (`own_receiver` 下の `culebra_runtime_prop_get` —
    C⑧)、および HOF callback-reject arm (C⑦)。
  - **restate に値する 2 つの微妙な契約。** (1) HOF callback は
    **callee-consumes、uniformly** である: codegen は callback の `+1` を HOF
    runtime に `consume()` し、runtime はあらゆる退出でそれを所有する —
    eager driver 向けの `JitHofCallback`、lazy factory 向けの capture cell、
    validation throw 向けの reject guard。残る唯一の codegen-side cleanup は、
    ヘルパが走らないエッジ向けである: receiver-resolution エラー
    (`dispatch_arr_iter` のエラー block が consumed な値を release する) と
    sort_by の `reverse:` coercion window (scoped な `ThrowGuard`)。これは
    以前の tag-gated split (Function は codegen-side で guard、adapter は
    helper-side で解放) を置き換えた。その補償的な adapter-side release は
    lazily-captured なインスタンスをその capture cell の下から解放した —
    mid-iteration な `__call__` throw 上の teardown use-after-free。(2)
    `emit_point_index` は uniformly `+1` を返す (C⑨) — array パスは borrow した
    slot を retain し、shared-val memoized sub-view は `object_get_any` で retain
    される — ため、両 INDEX promotion サイトは単に receiver を release する
    (古い borrowed-promotion `swap_owned` は object パスを double-count した)。
  - **ネイティブメソッドは helper-side RAII を使う** (`JitMethodSelf`/
    `JitMethodArgs`): ネイティブ callee (channel send、`with_lock`、@wrap
    foreign thunk) は*あらゆる*退出でその `+1` self/args を消費する。なぜなら
    caller は unwind 上でネイティブメソッドのオペランドを cleanup しない (C⑤/C⑨)。
  - suite 全体の GAP5 gate (`tools/difftest/leak_abort_suite.sh`、standing な
    `just test` phase) が上記すべてを pin する: throw パスを含むあらゆる corpus
    ケース上の新しい inflated-RC リークは、オブジェクトの birth site 付きで
    fail する。
- **`compile_*` return seam は closed (2026-07-12、§4.9)。** jit.h 内の全 64 の
  `compile_*` ヘルパ、extension compile hook、および stdlib 実装は `Owned` を
  返す。`compile()` の `own(compiled)` 境界は削除された。call site の migration
  は残る raw batch carrier (class/enum/multifn decl method + static + dispatcher
  vector、decorator roll、for-in プロトコルイテレータ) を `Owned`/`consume()`
  handoff に変換し、absorbing な registry/slab/slot を各 consume で名指した。
  3 つの crossing が証明により raw のまま残る、根拠コメント付きの
  `consume_unchecked` サイトとして (2 つの `call.phi` late-merge arm — sibling
  arm を跨いで保持された `Owned` は non-dominating な SSA 値を §4.8 window に
  spill してしまう — と、その alloca slot が所有する for-protocol イテレータ)。
  ratchet 上限はまさにこれらのために 11→14 に動いた。§4.9 pin は飛行中の 1 つの
  変換 slip (property-get block を跨いで再利用されたイテレータ raw) を difftest
  compile time で catch した。2 つの ratchet が seam を pin する:
  raw-returning な `compile_*` = 0 と typed consume-assignment = 0。
- **順に unblock:** (1) rooting/ownership split (§2/§4.5) は evidence により
  closed — 両 shipping collector は実行中の temporary を独立に root するため、
  冗長な参照の除去は rooting 機構ではなく accounting 証明を必要とする →
  (2) 所有権 flip を完了 → (3) RC が leak-free → (4) `gc_refs` が conservative
  backstop の過剰保持を退役できる → scalar speedup。

あらゆる phase は hard gate を保つ: interp/JIT symmetry、difftest corpus、
`CULEBRA_GC_STRESS`、ASan/UBSan、**およびフルの `just test` gate** (leak fix は
フルゲートを走らせねばならない — 孤立したプローブ/ASan は pass し得る一方で
蓄積された corpus はクラッシュする。かつ `bin ... | tail` はクラッシュ exit
code を mask する — `>/dev/null 2>&1; echo $?` で確認せよ)。

---

## 7. Culebra 固有の制約

- **Tagged `i64` 値、ポインタではない。** Root 列挙は tag を読んで `data` が
  ヒープポインタかを決める。value-ABI 書き換えなし (statepoint はこの理由で
  reject された — `jit_gc_design.ja.md` §12)。
- **2 backend は symmetric に保たれねばならない。** いかなる所有権変更も
  JIT-internal であり、インタプリタに対する観測可能な振る舞い、メッセージ、
  または検査のタイミング/順序を変えてはならない (standing な symmetry 要件、
  [[feedback_check_jit_interp_symmetry]])。
- **動的 dispatch。** Method/演算子ターゲットはランタイムで解決されるため、
  所有権 convention は callee の静的型によってではなく call 境界で動的に強制
  される。
- **Elegance bar。** マクロ/ヘルパは抑制的に用いる ([[feedback_macros_sparingly]])。
  抽象は周囲の codegen のように読めるべきで、framework を bolt on するのでは
  ない。
