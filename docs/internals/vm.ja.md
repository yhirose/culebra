共有バイトコードVM: 設計提案
=====================================

**Status: 提案段階（Phase 0 spikeは通過）。** §1〜9はtree-walking
インタプリタをバイトコードVMに置き換え、値表現とフロントエンドを
JITと共有するための動機・目標アーキテクチャ・移行計画を記録する。
§7のPhase 0 spikeは実施済みで、出口の2問とも yes —
結果は[§10](#10-phase-0-spikeの結果)。spikeの範囲外は依然として
未実装である。
[`language.ja.md`](../language.ja.md)にある観測可能な言語契約は影響を
受けない — これはエンジンの変更であって言語の変更ではない。本書と
`language.ja.md`が食い違う場合は`language.ja.md`が勝つ。

英語原本は[`vm.md`](vm.md)。

目次
----

1. [要約](#1-要約)
2. [動機: 1つの意味論、2つの実装](#2-動機-1つの意味論2つの実装)
3. [既に存在するもの](#3-既に存在するもの)
4. [目標アーキテクチャ](#4-目標アーキテクチャ)
5. [値表現](#5-値表現)
6. [メモリ管理](#6-メモリ管理)
7. [移行計画](#7-移行計画)
8. [リスクと未解決の問題](#8-リスクと未解決の問題)
9. [先行例](#9-先行例)
10. [Phase 0 spikeの結果](#10-phase-0-spikeの結果)

---

## 1. 要約

tree-walkingインタプリタを退役させる。代わりに置くのは、両backendが
共有するバイトコードコンパイラ、そのバイトコードを実行するVM、
同じバイトコードをコンパイルするLLVM loweringの3つである。

```text
             (today)                          (proposed)

   AST ──> interpreter.h  (walk #1)     AST ──> analysis ──> bytecode
   AST ──> jit.h          (walk #2)                           │
                                              ┌───────────────┤
                                              ▼               ▼
                                         VM executor    LLVM lowering
```

VMはJITの既存のruntime値モデル（`JitValue`、`JitCell`、`JitClosure`、
`Shape`、slab allocator、RC + cycle collector）をシステム全体の
唯一の値表現として採用する。インタプリタの`Value` / `Environment`
モデルはtree-walkerとともに退役する。

## 2. 動機: 1つの意味論、2つの実装

culebraの中核要件はbackend間の3次元対称性 — 振る舞い、エラーの
kind/文面/位置、検査のタイミングと順序 — である。今日この対称性は
手作業で維持されている。同じ意味決定が2箇所に書かれているからだ:

- 独立した2つのAST walker: `interpreter.h`（12.6k行）と`jit.h`
  （16.2k行 + `jit_compile_*.h`断片の1.7k行）が同じノードタグの上で
  それぞれdispatchしている。JIT側ヘッダにはinterpとの対称性・鏡映を
  引き合いに出すコメントが150以上あり、その一つ一つが人間がもう
  片方のbackendの挙動を再導出した場所である。
- 2つのstdlibバインディング: `Value`上の`stdlib_interp.h`（7.9k行）
  と、tag+data上の`stdlib_jit.h`（10.3k行）。呼び出し規約は既に
  単一ソース化済み（JITは`NsParamMeta`をinterpの正準paramリストから
  導出する）で、Mathカーネルも共有済み（`stdlib_math.h`）だが、
  他のすべてのnamespaceは意味論を2回実装したままである。
- isolate転送用の2つのシリアライザ: `sendable.h`（724行）と
  `sendable_jit.h`（1.8k行）は、異なる値表現から同じ`SendNode`木に
  降ろすためだけに両方存在する。

このコストは仮説ではない。Mathカーネルを統合しただけで、実際の
非対称が即座に露出した: インタプリタはLongの`min`/`max`引数を
`double`経由で比較していて2^53を超えると隣接値が潰れ、JITは正確な
64bit比較をしていた（`a7e7cd7`で修正）。それ以前には`trim_start`が
差分corpusの外に2ヶ月間留まり、生きたarity非対称を抱えていた
（`tools/check_difftest_coverage.sh`参照）。どちらも同じ種のバグで
ある: 表現ごとに複製された決定がドリフトした。

共有バイトコード層は、対称性の第3次元 — 検査のタイミングと順序 —
を規律から構造的性質に変える: 両backendが同じ命令列を消費するの
だから、検査が同じ順序で起きるのは、それが*同じ検査そのもの*だから
である。

副次的な動機が2つある:

- **`jit.h`が縮む。** バイトコードからLLVM IRへのloweringはASTから
  のloweringよりはるかに小さな段差である: JIT中に散らばる約200箇所
  の`nodes[i]`直接読みはバイトコードコンパイラに移り、一度だけ
  書かれる。（これはWadoコンパイラがWIR層で観測したことの再現で
  ある: 低レベルIRを前段に置くと、codegenは薄いemitループへ潰れて
  いく。）
- **インタプリタが速くなる。** tree-walkerはすべての変数読みを名前
  で解決する — アクセスごとにハッシュ検索、外れたら`Environment`の
  親チェーンを歩く。バイトコードはスロットをコンパイル時に解決
  する。値サイズも縮む（§5）。参考値として`tools/bench/fib.cul`
  （fib 28、`-O3`ビルド）: interp 3.9秒、JIT 0.56秒（コンパイル
  込み）。VMがこの7倍差を埋めることはない — 大半はネイティブコード
  の寄与であって表現の寄与ではない — が、tree-walker比1.5〜2.5倍が
  レジスタVMの通常到達域である。

## 3. 既に存在するもの

この提案が新たに発明するものは見かけより少ない。4つの部品のうち
3つは既にツリーの中にある:

**runtimeデータモデル。** JITのヒープオブジェクトは既にVMのヒープ
オブジェクトである: `JitCell`（refcount + value）はLua流のupvalue
箱、`JitClosure`（fn_ptr + `JitCell**` captures + arity）は古典的な
VMクロージャ、`Shape`はV8流のhidden class、slab allocator・RC規律・
cycle collectorは`docs/internals/memory.ja.md` §3〜6の現役ヒープで
ある。VMはこれらをそのまま採用する — 新しい表現は設計しない。

**フロントエンドの解析。** バイトコードコンパイラが必要とするのは
「どの変数がどのスロットにいるか」「各関数が何をキャプチャするか」
である。その解析は今日`jit.h`の中に存在する —
`collect_fn_locals`、`visit_for_frees`、`analyze_fn_common`、
`scan_eh_defer`、`FuncInfo` — が、JIT専用であり、tree-walkerは代わり
に動的な名前解決をしている。本提案の下でこの解析は共有バイトコード
コンパイラのフロントエンドに昇格し、両backendが消費する。

**オラクル。** インタプリタの書き直しが通常危険なのは、書き直しが
何をすべきかの権威が存在しないからである。culebraには既にある:
差分corpus（`tools/difftest`）、`just test-dev`のinterp-vs-JIT
スイープ、`misc/run_all_backends.sh`。移行中、tree-walkerは参照実装
としてツリーに残り、VMの各増分は既にCIで回っている機構でそれと
差分比較される。

存在しないもの: バイトコード形式、VM実行ループ、デバッグ情報
テーブル。それが実際の新規作業である。

## 4. 目標アーキテクチャ

```text
  .cul source
      │  parse (peglib grammar, unchanged)
      ▼
     AST
      │  AST-level transforms (generator/effects — see §8)
      ▼
  analysis        locals/slots, captures, EH regions   (lifted from jit.h)
      ▼
  bytecode        slot-resolved, RC-explicit, lowering-friendly
      │
      ├────────────► VM executor        (new; default engine from Phase 4)
      │
      └────────────► LLVM lowering      (jit.h, rewritten smaller in Phase 3)
```

設計上のコミットメント（spike（§7）が確認するまでは仮置き）:

- **スタックベースでなくレジスタベース。** レジスタはJITのSSA値と、
  解析が既に計算しているフレームレイアウトに直接対応する。Luaの
  経験則では、インタプリタループの式あたりdispatch回数も減る。
- **変数はスロット番号。** 名前→スロット解決はバイトコード
  コンパイラで一度だけ起きる。`Environment`チェーンとアクセスごと
  のハッシュ検索は消える。キャプチャされた変数は、今日JITクロー
  ジャがキャプチャするのと全く同じく`JitCell`に住む。
- **RC操作は命令列に明示的に載る。** バイトコードコンパイラが
  retain/releaseを、今日のJIT codegenと同じ所有権規律
  （`memory.ja.md` §4）の下で発行する。VMはそれを実行し、LLVM
  loweringはそれをコンパイルする。発行者は1つ、消費者は2つ —
  リークゲート（`leak.sh`、`CULEBRA_GC_STRESS`、`just test-assert`）
  は両方に適用される。
- **位置情報はバイトコードに随伴する。** 命令オフセット→line/col
  のサイドテーブルを持ち、エラー経路がそれを読む。エラー位置を
  手渡しでなく構造的に対称にするのはこれである。
- **デバッグテーブルは一級市民。** スロット→名前、オフセット→
  line/colのテーブルは常に生成する（小さいので）。REPL・DAP・
  デバッガがこれを消費する（§8）。
- **バイトコードは内部表現。** シリアライズ形式なし、バージョン
  保証なし、ディスクに書かない。コンパイラと2つのエンジンの間の
  インメモリ契約であり、どのコミットでも自由に変えてよい。

## 5. 値表現

統一表現はJITのものを無変更で使う:

| | interp `Value`（退役） | `JitValue`（採用） |
|---|---|---|
| 値 | 24 B（`Type` + `std::any`） | 16 B（`{i64 tag, i64 data}`） |
| String | `std::string`を`any`が箱詰め | slabヘッダ、確保1回 |
| Array | 96 Bペイロード + `shared_ptr<vector>` | `JitArray` ≤ 48 B |
| Function | 368 Bペイロード、箱詰め | `JitClosure` ≤ 48 B |
| Object | 名前→値マップ | `Shape` + 固定slots ≤ 128 B |
| 管理 | `shared_ptr`（atomic RC） | RC（非atomic）+ slab + cycle GC |

`Value`退役の下流効果:

- `sendable.h` / `sendable_jit.h`は1本のシリアライザに畳まれる。
- AOT runtime archiveがインタプリタを抱き込まなくなる:
  `culebra_rt.cc`が今日`stdlib_interp.h`をincludeしているのは、
  正準パラメータspecがinterpの`FunctionValue`として表現されている
  からに過ぎない。表現が1つなら正準specはただのデータになる。
- stdlibバインディングは一度だけ書かれる。`stdlib_math.h`の
  カーネルパターンは無変更で生き残る — 意味決定はカーネルが持ち
  続ける — が、backendごとの境界シムは不要になる。
- `JitValue`の癖は維持する。i64のtagも含む（ABI要件: `{i8, i64}`
  返りはCコンパイラとJITで強制変換のされ方が食い違う —
  `jit_value.h`のコメント参照）。VMは気にしないが、JITは今も気に
  する。

## 6. メモリ管理

`language.ja.md` §17の契約（決定的`drop`、RC主体、tracing backstop）
は不変。変わるのは*インタプリタ経路のRC操作を誰が書くか*である:
今日tree-walkerは`shared_ptr`により構造的に正しさを得ている。VM下
ではバイトコードコンパイラがretain/releaseを発行し、構造的保証は
一段上に移る — 検証されるのは200箇所の手書きevalサイトではなく、
*発行者*が一度、である。

これはJITが既にした取引と同じであり、緩和策も既に揃っている:
`memory.ja.md` §4の所有権規律、`just test-dev`のRC ratchet、リーク
スイープ、`CULEBRA_GC_STRESS`、`NDEBUG`なしのassertレーン。VMが
加える新しい義務は1つ: インタプリタループ自身が命令間で不変条件を
維持すること（レジスタファイルはGCルート集合である）。

## 7. 移行計画

順序の原則: **VMは第三のbackendとして入り、完全な同等性に到達する
まで何も再構築せず、何も削除しない。** 既存の2エンジンは新エンジン
が育つ間、無傷のまま出荷され続ける。JITとの深い共有は、バイト
コードがcorpus全体で証明された後にだけ始める。tree-walkerの削除は
最後であり、「手作業で維持される2つのwalkerを正直に保つためだけ」
に存在した検査機構も一緒に削除する。各フェーズ境界は通常の着地で
ある: どのチェックポイントでもツリーはビルドでき、全ゲートが緑で、
製品として完全に使える。

**Phase 0 — spike（数日で打ち切る。数週間かけない）。** 非自明な
構文を1つ選ぶ（rangeの`for`、または`match`）。そのバイトコードを
定義し、それ用のVMループを書き、それ用のLLVM loweringを書く。
`jit.h`の書き直し自体はPhase 3に先送りするが、バイトコード形式は
生まれた時点でlowering-friendlyでなければならない — インタプリタ
しか消費できない命令設計は後続の全フェーズを毒するので、それを
最安で確かめられる瞬間がここである。出口の問いは2つ、コードで
答える:

1. バイトコード→LLVMのloweringは、同じ構文の今日のAST→LLVM
   （`compile_for`: 320行、`compile_match`: 165行）より有意に小さく
   単純か。
2. VMはその構文のマイクロベンチマークでtree-walkerに1.5倍以上
   勝つか。

どちらかがnoなら、そこで止める: 2日分の成果物が証拠であり、本書は
2ヶ月の埋没作業の代わりに「棄却。理由は以下」の追記を得る。

**Phase 1 — 第三のbackend。** バイトコードコンパイラとVM executor
を増分で構築する（式コア → 制御フロー → クロージャ → EH/`defer`）。
既存2つの隣に新エンジンとして置く（`--vm`フラグ）。最初の具体的な
一歩は解析パス（`collect_fn_locals`、`visit_for_frees`、
`analyze_fn_common`、`scan_eh_defer`、`FuncInfo`）を`jit.h`から共有
ヘッダへ持ち上げること — これらがバイトコードコンパイラのフロント
エンドになり、`jit.h`は無変更のままそれを消費し続ける。フェーズを
通して2つのコミットメントを守る:

- **VMは初日からJITのruntime層に乗る**: `JitValue`値、
  `culebra_runtime_*`ヘルパー、`kNsMethods` dispatch、slab/RC/
  cycle-GC。したがって「完全な同等性」は第三のstdlibバインディング
  を意味しない — stdlibの大部分は、JITが既に呼んでいるruntime層を
  通って手に入る。
- **VMは同じAST層変換**（generator/effects）**を消費する。** 3つの
  backendが一致しなければならない間、意味論の再モデル化はしない。
  フレームベースのgeneratorは統一後の単純化（§8）であって、移行中
  の作業ではない。

差分機構にレーンが1本増える: corpusと`misc/run_all_backends.sh`が
3エンジンすべてを比較する。

**Phase 2 — 完全な同等性。** tree-walkerがすることのすべてをVMが
する: isolateとsendable転送、REPL、doctest runner、VMのデバッグ
テーブル上のDAPとデバッガ。出口基準: 今日interp + JITで回っている
すべてのゲートが3つで回って緑、かつcorpusが乖離を見つけないこと。
第三レーンが炙り出す潜在的なinterp-vs-JIT乖離は、出てきた時点で
直す — 事前の掃討キャンペーンは不要である。Mathの`min`/`max`の
事例（§2）が何を予期すべきかの見本になる。

**Phase 3 — フロントエンドの共有。** バイトコード形式がcorpus全体
で叩かれた後に、JITを再構築する: `jit.h`をASTでなくバイトコードの
loweringに書き直す。これをPhase 2の後まで遅らせることで、構築期間
中のバイトコード形式の変更は常に消費者1つに閉じていたことになる。
出口基準: AST消費者がちょうど1つ（バイトコードコンパイラ）だけ
残り、JITから約200箇所の`nodes[i]`読みが消えること。§2が約束した
`jit.h`の縮小を回収するのはここである。

**Phase 4 — tree-walkerの退役。** 慣らし期間の後に既定エンジンを
VMへ切り替え、`interpreter.h`・`stdlib_interp.h`・`Value`モデルと
そのシリアライザを削除し、「2つのwalkerが手作業で維持されていた
からこそ」存在した検査を刈る — eval_X/compile_Xのdispatch-symmetry
ratchet、二重シリアライザのテスト。差分corpusは**その中に入らない**:
VM vs LLVMへre-pointする。1つのバイトコードの2つの消費者はlowering
でまだ乖離しうるのであり、それを捕まえるのがcorpusだからである。
リークゲート・`CULEBRA_GC_STRESS`・assertレーンも無変更で生き残る。

## 8. リスクと未解決の問題

- **DAP/デバッガの意味論**（`dap.h`、873行、`Value`/`Environment`
  に深く結合）。step意味論とスコープ列挙をデバッグテーブル上で
  再実装する必要がある。定石のある機構だが実作業。Phase 2の同等性
  バーの一部。
- **stdlibの遅延解決。** `Environment::initialize_lazy`はstdlib
  モジュールを初回の*名前検索*で解決する — 名前ベースアクセスを
  前提にした機構である。スロット解決には別のトリガが要る（おそら
  く: バイトコードコンパイル時、コンパイラがその名前を見た時点で
  解決）。
- **generator/effects変換**（`generator_transform.h` 1.5k行、
  `effects_transform.h` 1.9k行）は3backend期間を通じてAST→ASTの
  まま残す — VMはその出力をコンパイルするので、3エンジンは構造的
  に一致する。フレーム中断としての書き直しはPhase 4後の単純化
  （CPythonとLuaがgenerator/コルーチンをこうモデル化している）で
  あり、その時点で独自のミニspikeを持つ。意図的に移行の一部に
  *しない*。
- **既定エンジンが変わる。** Phase 4以降、`culebra prog.cul`は新規
  コードを走らせる。緩和策は差分corpusであり、フェーズ順序は同等性
  が証明されるまで旧エンジンを残す — が、リスク窓は実在し、長い。
- **性能の下限。** VMは意味のある場面でtree-walkerに負けてはなら
  ない。インタプリタ起動に敏感な用途に注意（ミリ秒で終わるスクリ
  プト。tree-walkerが即開始するのに対し、バイトコードコンパイルは
  追加レイテンシである）。
- **移行中は2つ分のフロントエンド保守。** Phase 1〜3の間、ツリー
  には3backendが同居する。フェーズ構造が買っているのはこの隔離で
  あり、コストはCI時間と注意力である。

## 9. 先行例

インタプリタとJITの両方を走らせる成熟した動的言語実装は、例外なく
バイトコードを両者で共有している: CPython、Ruby（YARV + YJIT）、
Lua/LuaJIT、V8（Ignition + TurboFan）、SpiderMonkey。culebraの現在
の形 — 1つのASTを2つの完全な実装が独立に歩く — のほうが珍しく、
そのコストは本書の冒頭に書いた場所、すなわち手作業の対称性維持に
現れている。

本提案の最も近い祖先はWadoコンパイラのWIR層（codegenが18k行に育っ
た後に導入された、wasmの一段上のtree状IR）である: そこで観測された
効果 — codegenが薄いemitterに潰れ、新しいコンパイルフェーズの追加
が安くなる — は、§2が`jit.h`に対して予測する効果と同じものである。
本提案が採用する値モデルは[`memory.ja.md`](memory.ja.md) §3〜6に
文書化済みであり、そこにある設計系譜の注記（§7）はVMにもそのまま
当てはまる。

## 10. Phase 0 spikeの結果

2026-08-09、`vm-spike` branchで実施。対象は§7の指定どおりcounted
range-`for`。以下の数字はcleanup後のもの（`/simplify`パスがVMの
hot loopから証明可能に無駄なRC操作を数個取り除いた — cleanup前の
数字はbranch履歴を参照、その時点でも合格ラインは十分クリアして
いた）。作ったもの（隠しフラグ`--vm-spike` / `--vm-spike-llvm` /
`--vm-spike-dump`、`include/vm_spike.h`、計約700行）:

- レジスタベースのバイトコード — slot解決済み変数、命令列に明示的
  なRC操作、run-lengthの位置サイドテーブル、常時生成のslot名デバッ
  グテーブル。fusedな`ForPrep`/`ForLoop`ループ命令の意味論は
  `RangeBounds::done()/take()`（`range_bounds.h`）— interpのfast
  pathが読み、JITが手書きIRで写している、あの共有シーケンス
  オラクルである
- Longのみの言語スライス用バイトコードコンパイラ（`let`/`let mut`、
  可視な`let mut` slotへの再代入、`+ - *`と単項マイナス、識別子
  bindingのネストしたcounted `for`、`break`/`continue`、単一引数
  `println`。それ以外はコンパイル時にreject）
- JITのruntime値モデル上で動くswitch-dispatchのVM executor
  （レジスタはC++スタック配列 — conservativeなGCスキャンがそのまま
  rootに取る）
- 同じバイトコードのLLVM lowering。`JIT`のfriendとして既存の
  codegen contextとORC `exec`の骨組みを再利用する。
  `decode_range_layout`は共有フロントエンド持ち上げの第一号として
  `jit.h`から`parser.h`へ移した

3レーンは正当性セット（`tools/bench/vm_spike_cases/`）で完全一致:
包含/排他/step付き/空のrange、int64両端のoverflow、ネスト、
shadowing、break/continue、zero-stepエラーのkind・文面・位置まで。

**Q2 — VMはtree-walkerに1.5倍以上勝つか? Yes: 約34倍。**
branch先端の`just build`（-O3 + LTO）バイナリ、hyperfine、10 runs、
idle状態:

| bench（反復数） | interp | `--vm-spike` | `--jit`（コンパイル込み） | `--vm-spike-llvm` |
|---|---|---|---|---|
| `for_range.cul`（25M、最小body） | 9.15 s | **0.270 s（33.9倍）** | 0.462 s（19.8倍） | 0.056 s（163倍） |
| `for_range_dense.cul`（4M、密なbody） | 6.53 s | **0.192 s（34.0倍）** | 0.337 s（19.4倍） | 0.101 s（64倍） |

per-iterationではinterp約366 ns、VM約11 ns（最小body）。差の主因は
tree-walkerがcounted fast pathでも払い続けるper-iterationの
`make_scope` + 名前mapへの挿入である。副次的発見が3つ: この規模の
プログラムではVMはwall clockでJITにも勝つ（JITレーンはLLVM
コンパイルが支配的）。バイトコード→LLVMレーンは小さなchunk
モジュールだけをコンパイルする（preamble spliceなし）ため約56 ms
で起動する。1行スクリプトの起動はVMレーン約4.4 ms vs interp約
4.9 msで、§8の起動レイテンシ懸念はこの規模では現れなかった。
interp側のコストベースラインが高すぎるため1.5倍の合格ラインに
弁別力はほぼなく、Phase 1にとって意味のある数字は約11 ns/iteration
というdispatch下限のほうである。

**Q1 — バイトコード→LLVMはAST→LLVMより有意に小さいか? Yes、同一
構文で約2〜3倍小さく、重い機構ごと消える。** 同じツリー上での実測:

| 単位 | 行数 |
|---|---|
| AST→LLVM、`compile_for`全体 | 309 |
| AST→LLVM、counted fast path合計（fast pathブロック35 + `compile_for_counted_range` 72 + `emit_for_body_with_owned_binding` 95 + `for_break_target` 16） | 約218 |
| うちスライスの意味論に実際に効く分（pattern/defer腕を除く） | 約183 |
| バイトコード→LLVM、スライス全体（`lower_chunk`、全15命令） | 151 |
| バイトコード→LLVM、ループ相当分（ForPrep/ForLoop/Jump case + ブロック骨格） | 約82 |
| バイトコードコンパイラのFOR case（エンジン間共有） | 51 |

スライス全体のloweringがAST経路の`for`構文単体より小さい。比率より
も定性差のほうが大きい: loweringにはscope chainも`Owned`ハンドルも
`PosGuard`の手渡しもASTの再デコードもない — slotはただのalloca
（mem2regがSSA化）、位置はテーブル参照になり、スコープとRC配置は
一度だけ書かれるエンジン共有のバイトコードコンパイラへ移った。

両判定への公平のため、Phase 1向けに記録した留保:

- RC規律の検証は空虚に真 — スライスの値は全て`TAG_LONG`/`TAG_NIL`
  で、`Retain`/`Release`は実行時no-opである。spikeが証明したのは
  形式がRCを*運べる*ことであり、heap値に対する発行規律の正しさでは
  ない
- スライスにはthrow経路のcleanup義務がなく、loweringはlanding pad
  を一切emitしない。本物のPhase 1形式にはEH region情報が要り、
  `lower_chunk`のサイズ優位の一部はそこで消費される
- 端点/step式の評価順・評価回数は正当性セットでは検証できていない
  （スライスの式は副作用フリー）。Phase 1で効果つき端点で再検証
  する
- spikeのscope-stack slot allocatorは持ち上げ予定の解析パス（§3）
  のプレースホルダであり、Q1の判定はその置換を前提とする
