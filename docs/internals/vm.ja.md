共有バイトコードVM: 設計提案
=====================================

**Status: Phase 0〜2は完了して`master`にマージ済み、Phase 3はbranch
`vm-phase3`で進行中。**
§1〜9はtree-walkingインタプリタをバイトコードVMに置き換え、値表現と
フロントエンドをJITと共有するための動機・目標アーキテクチャ・移行
計画を記録する。§7のPhase 0 spikeは出口の2問とも yes で通過し
（[§10](#10-phase-0-spikeの結果)）、Phase 1は同じbranch上で第3の
backendを建て（§10の追記）、Phase 2は§7が定める同等性バーまで到達
した（[§11](#11-phase-2-完全同等性の結果)）。Phase 3はコンパイル型の
入口2つ — `--jit`と`culebra build` — をバイトコードの上へ移し、
残されたASTコードジェンを削除して§7の出口条件を満たした: ASTを読む
消費者は1つで、それはバイトコードコンパイラである
（[§12](#12-phase-3-jitをバイトコードの上に畳む)）。Phase 4 —
tree-walkerの退役 — は未実装である。
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
11. [Phase 2: 完全同等性の結果](#11-phase-2-完全同等性の結果)
12. [Phase 3: JITをバイトコードの上に畳む](#12-phase-3-jitをバイトコードの上に畳む)

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

- ~~**DAP/デバッガの意味論**~~（`dap.h`、873行、`Value`/`Environment`
  に深く結合）。step意味論とスコープ列挙をデバッグテーブル上で
  再実装する必要がある。定石のある機構だが実作業。Phase 2の同等性
  バーの一部。**Phase 2で決着**（§11.2）: `dap.h`はプロトコル層
  だけを残し、他は6問の`DebugEngine` interfaceに渡した。VMは束縛
  ごとのlive rangeとフレームスタックからそれに答える。予想外だった
  のはテーブルではなくスレッド境界のほう — §11.2を参照。
- ~~**stdlibの遅延解決。**~~ `Environment::initialize_lazy`はstdlib
  モジュールを初回の*名前検索*で解決する — 名前ベースアクセスを
  前提にした機構である。スロット解決には別のトリガが要る（おそら
  く: バイトコードコンパイル時、コンパイラがその名前を見た時点で
  解決）。**Phase 2でこの予想どおりに決着**: VMがpreamble自体を
  コンパイルするので、stdlib名はコンパイラがそれを見た時点で
  解決される。
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

追記 — Phase 1は同じbranchで開始し、spikeは破棄ではなく昇格した。
解析パスは`jit.h`から`fn_analysis.h`へ移動（§7の第一歩。持ち上げ
前後で全コーパスの`-O0 --emit-llvm` IRがバイト単位で一致）、
`vm_spike.h`は`include/vm.h`へ成長した: namespaceは`culebra::vm`、
フラグは`--vm` / `--vm-dump` / `--vm-llvm`に改名、rejectは
`VmError`、正当性セットは`tools/bench/vm_cases/`。スライスには
expression coreと基本制御フローが加わった — Float / Bool / nil /
StringとArrayリテラル（ArrayがRetain/Releaseを実在化させ、上の
留保の1つ目に答える）、JITと同一のdispatch形状を持つ算術・比較
（loweringはAST経路と同じ`emit_arith_step` / `emit_comparison_i1` /
`value_to_bool` emitterを呼ぶ）、`&&` / `||` / `??`、比較チェーン、
式としての`if`/三項演算子、`while`、そしてコンパイラのfront end
としての`FnAnalysis`（shadow checkがVMレーンでも走る。free_varsが
非キャプチャsliceのゲート）。続けて関数が入った: 非キャプチャの
関数リテラルは各自のchunkにコンパイルされ、呼び出しはJitFn ABIを
通る — 関数値は本物の`JitClosure`（executor側はトランポリン+
descriptor cell、lowered module側はネイティブ関数）なので、
ArityError／"expected Function"／RecursionErrorの意味論は位置まで
含めてJITと同じランタイム機構から出る — `return`・`fn`再帰
ハンドル・余剰引数のdropつき。型不一致・ゼロ除算・非Bool条件・
引数不足・非callable・再帰上限のエラーkind/文面/位置まで含め、
拡張後のセットでも3レーンは完全一致する。closuresが関数の残り
半分を埋めた: キャプチャされるローカルは`JitCell`（JIT自身のcell
機構）へ昇格し、専用の6 op（`CellNew` / `CellGet` / `CellSet` /
`CellRelease` / `BindCapture` / `ImmutErr`）を通る — 共有可変状態
のRCも命令ストリームに明示のまま。`MakeClosure`はchunkのcapture
list（関数リテラルの生成サイトは常に1つ）からcapturesを充填し、
キャプチャされるループ変数はiterationごとに新しいcellを得る。
非`mut`束縛への代入は全レーンでinterpと同じ実行時ImmutableError
になり（実行されない代入は沈黙のまま）、前方参照キャプチャだけが
未対応のrejectとして残る。`throw`と`try`/`catch`がspikeのEH形式
留保に答えた: 形式は静的なtry region（`EhRegion`: pc範囲・handler
pc・caught slot）を持ち、オペランド契約はborrow双子helper
（`num_*_borrow`等 — dispatch本体は同一、throw時にオペランドを
releaseしない）へ切替わった。これでthrow後も全レジスタはframe
所有のままで、handler先頭のbytecode解放ラダーが唯一のreleaserに
なる（cell slotはpinされ、ラダーのRelease/CellRelease選択は静的）。
executorはdispatch loopをC++ catchで包んでhandlerに再入し、
loweringはregionをそのままlandingpadへ写像する — `emit_call`の
既存invoke変換と、JITの`compile_try`と同じcarrier分類
（`try_translate`）により、CulebraErrorは同じerror objectとして
実体化し、foreign例外は素通りする。`defer`がEHを締めくくった:
defer本体は既存の`MakeClosure`機構（capture込み）で0-arity chunk
になり、3つのop（`DeferMark` / `DeferPush` / `DeferRunTo`）がJIT
と同じグローバルLIFO deferスタックを駆動する — frame単位のmark
（`has_any_defer`、chunkの先頭命令）へは`return`・Haltエピローグ・
frameを突き抜けるthrowが走り、scope単位のmark（`scope_has_defer`）
はlexical scope・loop body・try/catch本体が持つ。tryはregion mark
を取り、regionを本体fall-throughのdefer実行より前で閉じる — try
本体の正常出口でthrowするdeferは自分のcatchに捕まらない（interp
の`run_deferred`配置と同一）。handlerは「defer実行→解放ラダー」で
開く。どのregionにも捕まらないthrowはframeの残deferを実行してから
抜ける（executorは`run_frame`のcatch-all、loweringはframe単位の
cleanup pad） — JITのframe cleanupラダーの可観測な部分集合で、
slot解放は引き続きGC backstopに落ちる。deferの移植は、interp/JIT
一致を最初から破っていたJITバグ2件 — try/catch本体のdeferがblock
出口でなく関数出口で発火・break/continueがネストしたlexical scope
の残deferを取り逃す — をあぶり出し、`fn_analysis.h`/`jit.h`で修正
済み。3レーン比較は全deferケースを覆う。

続いて`match`がリーフパターンsliceの範囲で入った: リテラル
（値より先にtagを検査するので数値の暗黙変換はない — `1`は`1.0`に
マッチしない）・`_`・束縛・プリミティブ型名上の型付き束縛
（union対応、generic引数は剥がす）・非束縛限定のorパターン・
guard。新opは`JumpIfTag`（パターンのtagゲート）1つで足りた:
ゲート通過後の値検査は既存の`Eq`を、guardは既存の`JumpIfFalse`を
再利用する — その`to_bool`変換（Boolはそのまま、Long/Floatは
数値的、それ以外はmatchノードの位置を持つTypeError）が既存2レーン
のguard意味論と正確に一致していた。コンパイルは各armを「テスト→
束縛→guard→本体」の順に並べる。テスト失敗のジャンプ先には生きた
束縛がなく、束縛の解放が要るのはguard失敗経路だけになる。arm本体
は自分のdefer scope（`scan_eh_defer`のMATCHケース）で、subjectは
文所有のtemp 1つがarm横断で保持する。orパターン内の束縛だけが
reject（マッチした選択肢によって束縛されたりされなかったりする
ため）。matchの移植はコンパイラの潜在バグを1件あぶり出した:
tempの消費がsweepリストから同じslot番号のエントリを*全部*消して
いて、armスコープのslot巻き戻しが「同じ番号を持つエントリが2つ
並ぶ」最初の構文だった — リストが内側の文のwatermarkより短くなり、
sweepのゼロ埋めresizeが生きたcell slotへの`Release r0`を生んで
segfaultした。tempは1エントリずつ忘れる形に修正。あわせて
`return`は、囲む文の飛行中temp（arm横断で保持中のmatch subject等）
をGC backstopに残さず解放するようになった。

次に`fn name`宣言がarityディスパッチのoverload付きで入った:
`MultifnReg`が各body chunkのclosureをJITと同じランタイム
multimethodレジストリ（`multifn_register_and_install`、arityのみ —
型文字列はnull）へ登録するので、同一スコープのoverloadは1つの
dispatcherに合流し、ネストスコープの宣言はスコープ毎のレジストリ
キーでshadowingになり、同一arityの再宣言はテーブルエントリを
置換し、DispatchErrorのkind/文面/位置は共有dispatcher thunkから
出る。再帰が動くのは、コンパイラが文リストの`fn name`をスコープ
入口で事前宣言する（未束縛sentinel入りの所有cell）から — リスト
前方で作られたclosureも本物のcellをcaptureでき（相互再帰）、宣言
文が走る前の読み出しはlazy束縛の読み出しガード`UnboundErr`が
interpと同じNameErrorにする。この機構を移植前にinterp/JITで
プローブする（deferサイクルの手順）と、今度はJIT側のmaster潜在
バグが出た: closureのcaptureがlazy前方参照cellを*実体化*し、
null-pointer読み出しガードの頼る「宣言未実行」信号を消すため、
宣言前の読み出しでinterpがNameErrorを出すところをJITはnilの
プレースホルダをユーザーコードに流していた — `fn name`だけでなく
素の`let`でも。まず`jit.h`側を修正（sentinel+値ガード。VMは今回
その設計をミラーする）、回帰は`tests/test_forward_ref.cul`。

続いてstdlibがruntime層経由で届いた — Phase 1の見取り図の
「stdlibの大部分はruntime束縛経由で届く（3つ目のstdlib移植は
しない）」そのままに。新opは`NsGet`の1つだけ:
`culebra_runtime_namespace_get`で裸のbuiltinグローバルを名前
解決する — JIT自身のslow path（`emit_builtin_var_get`）が返す
Runtime毎キャッシュ済みclosureで、loweringはそのemitterをそのまま
呼ぶ — 結果は普通の関数値なので、呼び出しは既存の汎用`Call` opが
そのまま受ける。これでnativeな`kBuiltinFns`グローバル全部
（`to_string` / `to_long` / `to_float` / `type_of` / `hash` /
`inspect` / `print` / `println` / `range` / `iota` / `grid`）に
名前毎のコードなしで到達する。プローブで、builtinの直呼びと
値経由の呼び出し（`let f = to_string; f()`）が結果もエラーも —
kind・文面・call site位置、型付きパラメータ検査まで — interpと
JITで既に完全一致することを確認済みだったから: binderの
trampolineは全部を`set_call_site`に帰着させ、それはVMの`Call` opが
既に発行している。同名の読み出しは等値（`f == to_string` —
Runtime毎に1つのキャッシュ済みclosure）、lexicalな束縛は今まで
どおりbuiltinをshadowし、`println(<1引数>)`は専用opのpeepholeを
保ちつつ、それ以外の形 — 裸の`println()`・arity違い・値としての
println — は汎用経路とruntime自身の診断に乗る。lazy source
モジュール（`Time`・`Regex`・`assert_*`族など）はreject継続:
これらはpreamble spliceが組み立てるもので、VMレーンは意図的に
spliceしない。作業の中で境界も1つ見えた: `FnAnalysis`のlocals
集合は関数粒度なので、同じ関数内のどこかで宣言もされている
builtin名（fnリテラルの後ろにblockスコープの`let to_string`が
ある等）は、fn側の読み出しが前方参照captureになる — sliceの他の
前方参照captureと同じくrejectで、JITはこれをuse siteで解決する。

Phase 1の最終項目 — 差分機構にレーンを足す — は、sliceが正直に
検査できる範囲に合わせて2つに分けて着地した。curated corpus
（`tools/bench/vm_cases/`: 3レーン照合と、全allocationでcollect
する同じスイープ）はゲートになった: `just test-dev`・`just test`・
CIのci-lightシャードが回し、corpusはslice内のプログラムしか
持たないので、そこでのVmErrorは出力不一致 — sliceの後退は手動
スクリプト頼みでなくゲートを赤にする。そして
`misc/run_all_backends.sh` — Windows CIジョブが呼ぶ共有の
単発スクリプト対称性ヘルパー — には`--vm`/`--vm-llvm`レーンが
生えた: 出力一致はpass、slice外reject（全rejectが共有する
`--vm: unsupported: ...`という単一の契約）はSKIPとして
表示して緑のまま、それ以外は失敗 — sliceが育つにつれテストごとに
VMレーンが自然に点灯する。生成difftest corpusは当面2レーンの
まま: チャンク先頭にprobe preambleをテキスト連結する構造で、
preambleはsliceの遥か外（クラス・メソッド呼び出し）にあるため
全チャンクがコンパイル時rejectになる。per-caseのskip機構は
Phase 2のカバレッジがあって初めて意味を持ち、それはPhase 2の
仕事である。

## 11. Phase 2: 完全同等性の結果

2026-08-10から2026-08-17まで、同じ`vm-spike` branch上で46バッチ、
1バッチにつき1つの構文ファミリーという単位で実施した。§7が定める
バーは「今日interp + JITで走る全ゲートが3エンジンで緑になり、
corpusが差異を見つけない」であり、これは達成された。本節はゲートの
現在地、バイトコードが何に育ったか、第3レーンが他の2つで何を
見つけたか、そしてsliceの外に何が残っているかを記録する。§10の
追記はPhase 1が残した時点でのsliceの記録としてそのまま保存して
あり、現在の境界は下の§11.4である。

### 11.1 ゲートの現在地

| ゲート | 3レーンの状況 |
|---|---|
| 生成difftest corpus | 17,262ケースをinterp / `--jit` / `--vm`で。差異0、**skip 0**（skipはratchetだったが、0に達したので§13.5で撤去） |
| curated `tools/bench/vm_cases/` | 177ケース。`--vm`と`--vm-llvm`をそれぞれinterpと差分比較し、同じスイープを`CULEBRA_GC_STRESS=1`下でもう一度 |
| `tests/*.cul`の対称性とisolateスイート | 4レーン: interp、`--jit`、`--vm`、`--vm-llvm` |
| `just doctest` | ドキュメント465ブロックを3エンジン全部で（`just doctest LANE=interp\|vm\|jit\|all`） |
| `dap_test`（ctest） | デバッグアダプタのシナリオを両デバッグエンジンで |

この表はPhase 2が残した時点のものである。ここにある`--vm-llvm`
レーンは今日では`--jit`レーンであり、フラグが消えた理由は§12.11にある。

tree-walkerが持っていた3つの面は、複製ではなく継ぎ目として
建て直した。doctestランナーは`BlockRunner` —
`(name, code) -> {ok, kind, message}` — を受け取り、ブロック抽出・
stdout捕捉・マーカー照合は自分で持つ。`dap.h`は
6問の`DebugEngine` interface（`include/debug_engine.h`）の上の
プロトコル層まで薄くなり、VM側の答えは`include/vm_debug.h`にある。
REPLはセッションセルを介してVM上で走る。ユーザーから見える面は
`culebra --vm`（ファイル無指定ならREPL）・`culebra test --doc --vm`・
`culebra dap --vm`で、`--vm`と`--vm-llvm`は隠しフラグのままなので
`language.ja.md`とCLIドキュメントは変更していない。

### 11.2 バイトコードが何に育ったか

opcodeは142個、`include/vm.h`は約14.9k行。面積の大半を担った構造
判断は3つで、Phase 3がこのフォーマットに触る前に知っておく価値が
あるのもこの3つである:

- **stdlibはランタイム層経由で、次にpreamble経由で到達した。**
  Phase 1の`NsGet`はネイティブなbuiltinグローバルを名前で全部
  拾った。Phase 2が足したのは後半で、VMがstdlib preamble自体を
  コンパイルする。これが`Path`・`Regex`・`Vector`・`assert_*`
  一族・effectsランタイムを動かした。§8のstdlib遅延解決の問いは
  そこで予想したとおりに解決した — 解決はコンパイラがその名前を
  見た時点で起きる。
- **built-inメソッドはコードでなく表である。** `(name, argc)`
  1組につき`BMethSpec` 1行 — 受け手のtagマスク、引数ごとの宣言型、
  省略時のdefault、id — が、executor・LLVM lowering・reject判断の
  3つを単一の場所から駆動し、3つのop（`MethGate` → `ChkParam` →
  `BMeth`）がそれを運ぶ。これを安全にしている不変条件は狭く、
  外しやすい: **specの受け手マスクは、その名前をそのarityで解決
  する受け手の集合と厳密に一致していなければならない**。マスクの
  外側は全部メソッドmissとして即答されるからである。`BParam`に
  種別を足すときはexecutor側の述語とlowering側のswitchの*両方*に
  caseが要る。片方を忘れると`default`に落ち、片方のレーンだけが
  黙って誤った型を受理する。
- **デバッガはデバッグテーブルだけを読む。** 束縛ごとのlive range
  （`Chunk::SlotDebug`。`push_binding`という単一入口から供給される
  ので、rangeが名前検索とドリフトしえない）、`run_frame`が積む
  フレームスタック、そして文境界の`DbgStmt` 1個。テーブルから
  自動的には出てこなかったのはスレッド境界のほうである: フレームの
  registerウィンドウはdebuggeeスレッドの機械スタックであり、GCは
  スレッドローカルなので、デバッガのクエリは全部*停止中のdebuggee
  スレッド上*でjob pumpを介して走る — DAPスレッドから問い合わせて
  いた旧実装は偶然成立していただけだった。`evaluate`はREPLの
  セッション機構を再利用しており、これが名前解決を2度実装せずに
  `setVariable`のImmutableErrorを正しくしている。

### 11.3 第3レーンが見つけたもの

§2は「第3の実装は、手で保守されてきた2つが抱えていた非対称を
表に出す」と予想した。実際そうなった — 新しい構文に届いたバッチの
ほぼ全部で。そして移植可能なのは、それを生んだ手順のほうである:
**ある構文をコンパイルする前に、その構文のプローブをinterpとJITで
走らせて2つを差分する。** 1バッチのプローブは1行プログラム50〜200
本で、食い違った箇所は`interpreter.h`/`jit.h`側の独立したコミット
として、VMの作業を始める*前*に着地させた。VMが動く的に向かって
書かれることが一度も無いようにするためである。代表的な発見:

- 文が1つだけの`for`本体は、インタプリタでGC safe pointに一度も
  到達しなかった。1文の本体はパーサが畳んでしまい、その文の
  ディスパッチが唯一のポーリング地点だったからである。
  `for i in 0..4M { let a = [i] }`が34 MBであるべきところ1,406 MB
  を保持し、本体が1文の関数呼び出しだと5,094 MBになった。
- 同じ畳み込みが、デバッガ側でも: 1行のlambda・1文の`try`本体・
  `catch`腕・`cond`/`match`の腕にはbreakpointを置けなかった。
- 使い切ったiteratorの`next()`は、インタプリタでは`StopIteration`
  をthrowしJITではnilを返した — 組み込みiteratorの10ソース全部で、
  しかも*インタプリタ内部でも*built-inとgeneratorの間で割れて
  いた。nil側に統一し、`language.ja.md` §18.5に明記した。
- built-inメソッドへのキーワード引数: JITは1つの形を除き`KWARG`
  ノードを位置引数の値としてコンパイルしていたので、
  `'ab'.truncate(max: 3)`が食い違った。built-inはキーワードを
  kw-onlyパラメータ名としてのみ受け付ける形にし、判定は述語1本に
  した。
- 引数リストの評価順: object的な受け手のmissで、JITは約25箇所で
  引数評価を飛ばしていた。インタプリタはリストを先に走り切る。
  引数より先に来るのはscalar受け手のエラーだけである。
- 名前解決: 宣言は文リスト全体にではなく、それが実行された時点から
  効く。6つの形が食い違い、うち1つは閉包が一切絡まないものだった。
- `Range`の`class`スロットがJITでは素の`static const char[]`を
  保持していたため、2つのrangeの構造的な`==`が存在しない長さ
  プレフィックスを読んで即死した。

VM自身のバグは1つの型だけ記録に値する。構造的だからである:
**検証していないLLVMモジュールはそれでも動いてしまう。**
`run_program`の`verifyFunction`呼び出しは書かれて以来ずっと戻り値を
捨てていた。streamを渡してthrowさせるようにした瞬間、loweringの
chunkをまたぐ`alloca`漏れが出た — 全ゲートが緑のまま見逃していた
ものである。
`culebra --jit --emit-llvm f.cul | opt -passes=verify`が
lowering作業の常設チェックであり、codegenを変えてはならない
リファクタの常設チェックは`-O0 --emit-llvm`のIR差分である。
（このレーンは§12.11が退役させるまで`--vm-llvm`という綴りだった。）

### 11.4 sliceが閉じた、そのために何が要ったか

文法は入った。204本の`tests/*.cul`は全部VMレーンでコンパイルされて
走る — モジュール単位のrejectはゼロ、ポイズンチャンクとして残った
関数リテラルもゼロである。（branchをmasterへrebaseした後は207本と
17,262ケース。masterが自分のテストとcorpusケースを連れてきた。）

そこに至るにはゲートを注意深く読む必要があった。緑のゲートが、
見た目どおりのことを言っていなかったからである。対称性ゲートは
ファイルのミスマッチを次の2つのどちらかでskip扱いにする: 実行が
`--vm: unsupported:`を出したか、**dumpのどこかにポイズンチャンクが
あるか**。後者が広すぎる網で、ひとつのlambdaの中のひとつのslice外
構文が、そのファイルの差異を全部 — その構文と何の関係も無い差異
まで — 免除してしまう。最後に着地した4構文（型注釈付きの変数宣言・
総称型パラメータ・`@`・複数モジュールのスクリプト）はそれぞれ
ポイズンの印を消し、その裏に座っていた差異を表に出した:

- ステップがTensorをin-placeで変形する複合代入は、VMでは
  ImmutableError、他の2エンジンでは変形だった — しかも不変
  *プロパティ*越しではJITが「変形してからthrow」という3者で最悪の
  形だった。
- 式の位置に置いた`return`/`break`/`continue`（`let y = return e`・
  `f(return e)`・`[1, break]`）は、VMでは値を作って落ちていくかの
  ようにコンパイルされていた。他の2エンジンはフレームを抜ける。
  VMの`compile_expr`には発散文の腕が1つも無かった。
- 総称型パラメータの戻り値位置（§11.3のリスト）も同じ掃除から出た。

ここから一般化できる教訓: **ファイル単位で効くskip述語はフィルタ
ではなくマスクである。** 免除するものを、丸ごと免除する。

このフェーズが表に出した差異は1件を除いて全部閉じた。その1件も
「修正した」のではなく「構造的に閉じる」ほうである。発散式をレシーバ
位置に置いた形（`(return x).size()`）は3エンジンともフレームを抜ける
— `eval()`は保留中の完了レコードを入口で見るが、postfixチェーンは
evalを経由せず手元の値にディスパッチするので、同じ検査をそこにも
置いた。関数内の`[self] = [5]`はどこでもImmutableError、トップ
レベルではどこでも宣言になった。ネイティブbuiltinは本当のシグネチャ
を報告する — JITのbinderが既に読んでいる正準パラメータリストから
導出し、VMのチャンクが確立した「クロージャで引く継ぎ目」で答える。
`Isolate.spawn`のハンドルはどのエンジンでもidを見せる。`Channel`の
エンドポイントと`FS.watch`のハンドルが元からそうしていたとおりに。

残る1件はJIT固有である: sibling な`if`の腕2つが同じ名前を裸代入し、
それを閉包が捕獲すると、interpと両VMレーンが正しく束縛するところで
JITだけImmutableErrorになる。捕獲なしの形を直した実行時sentinelは、
「cellスロットには決してセットしない」と自分のコードに書かれており、
そこにセットするとはセルの生成位置を動かすことである — それは捕獲
されたループ変数に反復ごとのfresh cellを与えている当のものだ。
Phase 3はこの問いに答える代わりに問い自体を消す: `jit.h`が
バイトコードを下ろすようになれば、JITはVMの判定 — 既に正しい —
を継承する。

Phase 3 — `jit.h`をASTでなくバイトコードを下ろす形に書き換える —
は本節が終わるところから始まった。その記録が§12である。

---

## 12. Phase 3: JITをバイトコードの上に畳む

2026-08-18にbranch `vm-phase3`で開始（Phase 2が着地した`master`
コミットから）。コンパイル型の入口2つ — `--jit`と`culebra build` —
はバイトコードを下ろすようになり（§12.2）、残されたASTコードジェンは
消えた（§12.4）: `include/jit.h`は16,446行から5,271行になり、ASTを
読む消費者は1つ — §7が求めたとおりである。このフェーズに残っている
借りは§12.5である。

### 12.1 レーンの終了コードは答えの半分である

このフェーズはまず変更でなくプローブから始めた: 207本の
`tests/*.cul`を`--jit`と`--vm-llvm`に通して差分する。結果は
MATCH 203・差異4・skip 0 — そしてこの4件は本来ニュースであっては
ならなかった。対称性ゲートは`just test-dev`のたびに同じ比較を
走らせているからである。

走らせてはいたが、VMレーンの終了ステータスを捨てていた。比較は
stdoutだけなので、何も出力せずに死んだレーンがインタプリタと
「一致」と読める: 捕捉されないthrow（rc 255）もsegfault（rc 139）も
stdoutは空で、それはassertが全部通ったファイルが出力するものと
まったく同じである。ステータスを捨てたのには理由があった —
slice外のrejectもrc≠0で出るので、掃引はそれをskipにしたい。正しい
形は「ステータスについてもskipの問いを立てる」であって「問いを
立てない」ではない: rc≠0はstdoutと同じミスマッチ枝に入り、そこで
`vm_skipped`が以前どおりskipかfailかを決める。レポートにはrcと
そのレーンのstderrを足した。これが無いとクラッシュは空の差分に
見える。

こうして緑だったファイルが3本あり、3本とも機構は1つだった。
`if`/`cond`は腕が宣言するものを持ち上げる — どちらの腕もそのための
スコープを開かないからである。コンパイラは持ち上げのたびにセルを
鋳造し、`Binding::shadowed`でセルを連鎖させていた。だから入れ子の
腕の中で宣言された名前は階層ごとに1セルを持ち、連続する2つの`if`が
両方宣言する名前はそれぞれ1セルを持った。JITが名前ごとに単一の
`VarSlot::runtime_decl`スロットを登録し、後からコンパイルされる腕に
それを見つけさせるのに対してである。連鎖はクラッシュもした: 入れ子の
持ち上げの`CellNew`は片方の腕の中にあるので、その名前を読む兄弟の腕は
連鎖を辿って自分の経路が一度も確保していないセルに入り、ゼロの
スロットを間接参照した — `tests/test_args.cul`は短オプションかつ
非Boolのときに必ずsegfaultしていた。`Args.try_parse`の、構造的に
同型な2つのオプション腕を通って。

修正は2重にJITに倣う。名前につきセル1つを全腕で共有し — そして
複数の腕が1つの束縛に書くようになった以上、可変性をコンパイラに
答えさせるのをやめる。どの宣言が走ったかはこの呼び出しの事実なので、
セルの隣のスロットに置く（`Binding::mut_slot`、JITの
`VarSlot::mut_alloca`）。宣言は自分の`mut`をそこに記録し、裸の書き込みは
検査の前に「そもそもどれかの宣言が着地したか」をセルに訊く。この
実行時ビットが無いと、最後にコンパイルされた腕が全腕を代表して
しまい、`if a { let n = 1 }` / `if !a { mut n = 2 }`がインタプリタの
拒否する書き込みを受理していた。3件目の差異は隣接していた: 宣言は
借用したcaptureを通して書いてはならない。captureも他と同じくセル
束縛として読めるので、`fn () { let sh = sh + 1 }`は外側の`sh`を
shadowするのでなく代入していた。両者を分けるのは、そのセルを所有して
いるかどうかである。

### 12.2 コンパイル型の入口2つがバイトコードを下ろす

`--jit`は`run_modules_via_llvm`に、`culebra build`は
`build_object_from_modules`になった。そのloweringはPhase 2以降
`--vm-llvm`としてcorpusが検査し続けてきたものなので、挙動面のリスクは
小さく、実際に挙動面は平穏だった: 17,262ケース、`interp == jit`、
初回から。

AST側の入口が持っていてloweringに無かったものが3つ:

- `--jit-faststart`とバックエンドのオブジェクトキャッシュ。どちらも
  「モジュールがどう作られたか」でなく「どう実行されるか」の性質なので、
  `run_program`が`fast_codegen`とモジュール名を取る形にし、
  `JIT::jit_module_name`が以前と同じ材料からキャッシュの鍵を作る。
- **コンパイラより長生きするパラメータメタデータ。** loweringは
  `&VmProgram::param_metas[i]`を登録していた — コンパイル中プロセスの
  ヒープを指すポインタである。JITなら正しく、オブジェクトファイルには
  ダングリングで、AOTが突きつけた唯一の実障壁がこれだった。AST経路は
  そのメタデータを「今ビルド中のモジュールのグローバル」として出す
  機構を既に持っていた（`emit_param_meta_global`）ので、loweringは
  それを使う。その過程でこの関数は最後のAST引数を失った（defaultが
  存在するかを訊いていただけだった）。lowering全体を同じ間違いが
  無いか監査した: 他のホスト値は全部ランタイムの抽出を通って
  モジュールに届いており、IRに焼き込まれたポインタはこの1件だけ
  だった。
- モジュールとプログラムの分割。3箇所で手書きされていた — main.ccの
  スクリプト経路・同じくdoctest経路・AOTドライバ。
  `Compiler::compile_modules`がローダのリストを取り、先頭に継がれた
  `<stdlib>`プロローグを1度だけ剥がす。

機械的な罠を1つ記録しておく: `IRBuilder::CreateGlobalString`は挿入
位置の基本ブロックからモジュールを取るので、loweringが始まる前には
呼べない。メタデータのグローバルはprepassではなく、それを最初に必要と
した`MakeClosure`の地点で作ってキャッシュしている。

### 12.3 消費者を動かすと、ゲートが測る対象が動く

載せ替えのコストはこれで全部であり、そのどれも挙動の比較には
現れなかった。`--jit`は複数のゲートが名指しするレーンであり、その
レーンがエンジンを替えた瞬間、それらのゲートは別の実装を測り始めた。
そしてバイトコード経路が人知れず間違えていたものが、公然の回帰に
なった。出てきた順に:

- **leak-fuzz**（corpusのRCリーク回帰。interpとJITレーンの比較）が
  新規11件を2クラスタで報告した。どちらも既存 — `--vm`が同じだけ
  漏らす — で、どちらも同じ形、すなわち「1箇所で宣言された規則が
  1つの経路で守られていない」である。明示的な`x.drop()`はレシーバを
  借用し、解放は文のsweepに任せる契約だが、UFCSディスパッチの中では
  候補の腕が同じレシーバを呼び出しに渡し、その`Take`がsweepリストから
  そのtempを*全腕について*外す。だからdrop腕が走った経路で`+1`が
  迷子になった（レシーバ10形）。そしてfor-inの分配束縛は反復ごとに
  走るのに、入れ子パターンの中間コンテナはループ全体で1度しか
  発火しないsweepに委ねられていて、最後の反復以外の全部で迷子に
  なった。途中で`leak_baseline.txt`が1件縮んだ: AST経路が漏らして
  いてバイトコード経路が漏らさない、入れ子fnの再帰ケースである。
- **leak-abort-suite**は「より厳しいゲート」ではなく別のゲートである:
  leak-fuzzはwarm-upがthrowしたケースを捨てるので、throw経路を構造的に
  見ない。こちらはさらに3クラスタを見つけた。2つはランタイムヘルパで、
  渡された値を正常出口では必ず解放する — `culebra_runtime_set_add_method`と
  `culebra_runtime_object_remove_any` — 以上それを所有しているのに、
  unwind辺だけ入れ忘れていた。引数のハッシュ計算こそがunhashableな
  引数の投げる場所で、それがまさに解放を飛ばしていた経路である。
  `JitUnwindRelease`はその規約のRAII形であり、いま両方を覆っている。
  3つ目が移植可能なほうである: **共有emitterは自分の後始末の前提を
  一緒に連れてくる。** `emit_for_open_protocol`はプロトコルを検査する間
  イテレータを`Owned`ハンドルに置いたままにする — コメントにそう
  書いてあり、拒否されたプロトコルが`+1`を空中に残したままthrowするのは
  そのためである — が、そのプールはLLVM関数の側に住んでいて、VMの
  cleanup padが立っているバイトコードのtemp表からは見えず、誰も
  drainしていなかった。いま各スコープのpadがAST経路のpadと同じように
  プールをdrainする。解放はスロットをnilで潰すので、外側のpadが
  もう一度drainしてもno-opである。
- **opt-inレーンの寸法で置かれた容量上限2つ。** rc-leak battery
  （`tools/analysis/gc_leak_check.sh`）が全40行で`error`を出した。これは
  リークではなく計測の失敗である: batteryのパターンファイルは382
  スロットのフレームにコンパイルされ、`kMaxSlots`は256だったので、
  `--jit`がAST経路のコンパイルできたプログラムを拒否していた。256を
  望んでいたものはフォーマットの中に何も無い — 命令のオペランドは
  `int32`であり、両エンジンともレジスタ窓をチャンク自身の`num_slots`
  から確保する。この数が縛るのは「executorの1フレームが要求しうる
  マシンスタックの量」であり、そこに迫りうるのはトップレベル —
  スロット数が束縛とtempの数で決まり、決して再帰しない — である。
  いまは8192で、超過は引き続ききれいな`VmError`である。入れ子スコープの
  深さ`kMaxOwnedDepth`も、姿を変えた同じ上限だった: 64、インタプリタは
  200を上限無しで受けるのに。64のままだった理由は`run_frame`が
  `int64_t marks[kMaxOwnedDepth]`という固定長を宣言していたこと —
  予算が許す最深の入れ子分を全フレームに課金する形である。それは
  Phase 2がレジスタ窓に対して既に直した罠で、配列1つ隣の同じ罠だった。
  チャンク自身の`owned_depths`から確保すれば、予算は1024にできて、
  実際にそこまで入れ子になったフレームだけがその分を払う。
- この作業自体が入れたリーク。batteryが再び走れるようになった瞬間に
  見えた: 期待89に対して生存5,086、1周につき1個。
  `emit_conditional_rebind`がsentinel判定のプローブのためにtempスコープを
  開いていたので、スコープを閉じるときにスロット番号が巻き戻り、代入式が
  返す読みが同じ番号を取って、プローブの`+1`を解放せずに上書きしていた。
  レジスタVMではtempスコープは寿命であると同時に番号付けである。
  `assign_shadowing`にある同型のプローブは既に囲みの文のtempsに住んで
  いて、これもそこに属する。
- **カウンタ型の`for i in a..b`が境界のLong検査をしていなかった。**
  高速経路はLongのカウンタを歩く。`compile_range`は各端点をコンパイル
  した直後に`ChkLong`する — 悪い境界のエラーを後続の境界の副作用より
  先に出す評価順のためである — のに、カウンタ経路だけがその検査の
  無い唯一のrangeサイトだった。だから`for i in 1.5..3`は、インタプリタが
  投げるところで反復ゼロのまま何も言わず、`for i in 1..3 by 0.5`は
  自分で発明したステップを歩いた。これは永久にループする —
  `jit_error_pos_test`が単に失敗するのでなくタイムアウトしていたのは
  これである。

一般形はこうなる: **緑のゲートはレーンに紐づいており、レーンとは
名前ではなく実装である。** 挙動の同等性が初回から成立したのは、corpusが
そのloweringを1フェーズ前から比較し続けていたからである。`--jit`を
名指しするものは、どれもそうではなかった。

### 12.4 コードジェンを削除する

`include/jit.h`は16,446行から5,271行になり、`stdlib_jit.h`は1,996行を
失い、断片ヘッダ3本（`jit_compile_assign.h`・`jit_compile_class.h`・
`jit_compile_fn.h`）は丸ごと消えた。

去るかどうかはオーバーロード単位・パラメータ単位で決まる: そのメンバが
「無しでは済まないAST」を取るなら去る。「`peg::Ast`に言及しているか」は
2重に誤った述語である。値ベースのヘルパのいくつかは、エラー位置のための
`const peg::Ast* at = nullptr`という末尾引数を持っている
（`emit_type_check`・`emit_object_set`・`compile_function_call_raw`）。
そしていくつかの名前は、AST版のオーバーロードと、loweringが今も呼ぶ
値版のオーバーロードを両方持っている。どちらの取り違えも生きたコードを
巻き込み、ビルドがそう言ってくれる — ただし言うのはフルリビルドの後
なので、述語は最初の切断より前に正しくしておく価値がある。

機械的な部分は一度失敗していた。ブレース計数である: このファイルの
文字列リテラルとコメントはブレースを含んでいるので、それを真に受ける
計数器はメンバの末尾を行き過ぎ、クラスの閉じブレースまで持っていく。
効くのはインデントのほうだった — clang-formatはトップレベル構造体の
全メンバを2スペースに置き、その本体を厳密に`  }`という行で閉じる —
これは正確で、しかも述語を精密化するたびにきれいなツリーから
やり直せるほど安い。

もっと小さな変更では表に出なかったものが3つ出た:

- **ASTコードジェンは`jit.h`の外まで伸びていた。** `stdlib_jit.h`の
  `JitExtension`が`Math.sin(x)`・`IO.print(x)`と裸のグローバルを
  ASTからコンパイルしており、`ExtensionHooks`の関数ポインタ8本を
  通って呼ばれていた。8本のうち6本がASTを取っていた。フック表は
  ASTを取らない2本（`declare_runtime`・`is_builtin_var`）だけになった。
- **削除が孤立させたものは、削除そのものより大きかった。** AST側の
  メンバを消した結果、それらだけが呼んでいたメンバ・構造体・フィールドが
  60個ほど残った: 閉包の構築・コンストラクタの生成・変数スロットの
  コンストラクタ群・safe navigationのヘルパ・もう誰も書かなくなった
  call-root位置。C++は「誰も呼ばないprivateメンバ」について何も言わない
  ので、見つける方法は反復である — 呼び出し箇所の無いメンバを走査して
  消し、また走査する。不動点までに4巡かかった。
- **公開の入口2つは戻す必要があった。** `JIT::run`と`JIT::build_object`は
  [`deployment.ja.md`](../deployment.ja.md)がembedderに呼べと書いている
  ものである。宣言は`jit.h`に、定義は`vm.h`にバイトコードのレーンの上で
  置いた。公開APIは変わらず、その下のエンジンだけが新しい。

移動して残ったヘルパが2つ。`ArgScan`と`scan_arg_list`はコードジェン
ではなくASTの*解析*であり、いまやバイトコードコンパイラのものである:
構造体と走査は`check_arg_list`の隣の`parser.h`へ、built-inのキーワード
述語はコンパイラ自身の中へ移した。

2つのASTウォーカーを比較していたratchetは、退役ではなく張り替えに
なった。`tools/check_dispatch_symmetry.sh`はinterpの`_eval_dispatch`の
caseラベルを`JIT::compile`のそれと差分していたが、いまはコンパイラの
2つのswitch（文の位置と式の位置）と差分する。allowlistは1タグ
`STRING`だけになった — インタプリタがこれを`is_token`のフォール
スルーで畳んでいるからである。

削除の検証はゲートではなくIRで行った: 207本の`tests/*.cul`全部に対する
`--jit -O0 --emit-llvm`の出力が、stderrも含めて前後で1バイトも
変わらない。1万4千行の削除を危険な変更でなく機械的な変更にしているのは
これである — ゲートが通ったというだけなら「テストは今も通る」以上の
ことは言っていない。

### 12.5 コードは速く、コンパイルは遅く

`--jit`は別のフロントエンドを通ってLLVMに届くようになったので、この
フェーズは計測を1つ借りていた。2つのバイナリはどちらもこのツリーの
`just build`（-O3 + LTO） — masterのASTコードジェンと、branchの
バイトコードloweringで、下のLLVMは同じ — hyperfine、10 runs、idleな
マシン:

| ベンチ | master（AST） | branch（バイトコード） | |
|---|---|---|---|
| `for_range.cul`（2500万反復・最小の本体） | 478.5 ms | **106.8 ms** | 4.48倍速い |
| `for_range_dense.cul`（400万・密な本体） | 343.4 ms | **149.9 ms** | 2.29倍速い |
| `fib.cul`（fib(28)・再帰） | 585.9 ms | 603.1 ms | 1.03倍遅い |
| `hello.cul`（起動＋コンパイル） | 41.3 ms | 55.3 ms | 1.34倍遅い |
| `tests/test_core.cul`（コンパイル支配） | 3.58 s | 5.32 s | 1.49倍遅い |

分かれ方はきれいである: **ループ主体のコードは数倍速くなり、あらゆる
プログラムがコンパイルに余分に払う。** 実行時の勝ちはカウンタ型の
ループ — loweringがオプティマイザの望む形を出す — であり、呼び出しが
支配的な`fib`が変わらないことは「下のLLVMは同じ」から予想される
とおりである。

コンパイル時の損は、1つの原因が2度計上されたものである。loweringは
ASTコードジェンより**約30%多いIR**を吐き（`test_core`が`-O0`で
329,785行に対し429,311行、`hello`が3,329に対し4,218）、その量を
codegenで1度、オプティマイザでもう1度払う（別の実行なので`-O2`の
行は上の表より少し上に出ている）:

| `test_core` | master | branch | |
|---|---|---|---|
| `-O0`（オプティマイザ無し） | 3.18 s | 4.22 s | +1.03 s |
| `-O2`（既定） | 3.69 s | 5.33 s | +1.64 s |
| → オプティマイザの取り分 | 0.51 s | 1.11 s | +0.60 s |
| `-O0 --jit-faststart` | 1.26 s | 1.70 s | +0.44 s |

比はどの最適化レベルでも — fast codegenの経路でも — 1.3〜1.4倍
近辺に留まる。これは量の問題の見え方である: 特定のパスが悪いのでは
ない。したがって後のサイクルで狙うべきは、バイトコードのop 1つあたり
loweringが吐くIRであって、バイトコードを作るフロントエンドではない
（その1秒はフロントエンドに消えているのではない）。

### 12.6 Phase 3に残っている借り

- ~~**残っているcleanup。**~~ 決着した。ただし修正によってではない。
  §12.5のIR量は解消し（§12.8）、`-O2`に残る1秒はoptimizerではなく
  機械語生成であり（§12.9）、そしてそれは命令数ではなくレジスタ圧で
  ある（§12.10）。バイトコードのレジスタファイルをSSAに昇格させる
  代償であり、それは同時にloweringしたコードを速くしている当の性質で
  ある。padの形を4通り実測し、今木にあるものが最良だった。
- ~~`--vm-llvm`は今日では`--jit`と同じエンジンであり、レーンというより
  重複したフラグである。~~ 退役させた（§12.11）。

### 12.7 共有したセルは共有したヒープである

このフェーズのものではなくVM側にあった欠陥が1つ: 並行負荷の下で
`tests/isolate/test_lazy_ns_parse_race_jit.cul`が`--vm`でのみsegfault
していた — 10プロセス幅で40回中7回、interp・`--jit`・`--vm-llvm`は
それぞれ40回中0回。このレーンの偏りが手がかりの全部だった。executorが
作るクロージャは capture 0 に載った descriptor 経由で自分のバイトコード
に辿り着くが、`Exec::prepare`はその descriptor セルをチャンクごとに
プログラム全体で1個だけ作っていた — 同じチャンクから作られる
クロージャは全部その1個を retain する。lowering した2レーンにこのセルは
無い。チャンクがそれぞれネイティブ関数なので、fn_ptr が答えの全部だからだ。

共有したセルは、規律の無いまま共有したヒープオブジェクトである。
`JitCell::refcount`はただの`int64_t`だ — ランタイムは構造として
シングルスレッドで、isolateごとに1つの`Runtime`があり、それぞれが自分の
slabと自分のGCヒープを持つ、という前提だからである。`Parallel.map`が
24人の子を起動し、その全員が`Regex`・`Path`・`Term`・`Time`・`Canvas`を
自分のスレッドで解決すると、それらのモジュール本体のクロージャが親の
セルを一斉に retain/release する。更新が失われて、参照が残ったまま
カウントが0に落ち、先に着いた子が親のメモリを自分のslabへ解放する。

ThreadSanitizerは1回目の実行で名指しした — 59件、どれも
`Exec::prepare`が確保したセル上で、retainする2箇所（`MakeClosure`と
lazy-namespace builderの再構築）から。ASanは何も言わなかった。以前の
parse台帳の競合と同じ分かれ方である。

直し方は、共有をやめることだ。descriptorは他のcaptureと同じく
クロージャ自身のセルに載せる — デシリアライザが最初からそう再構築して
いたのと同じ形である。`desc_cells`とそのpin、`release_descs`、run側の
ガードはまとめて消え、lazy-namespaceレジストリはセルではなく
descriptorの値を持つようになる（各Runtimeが自分で確保したセル越しに
モジュールを組み立てる）。差し引き27行減った。代償はクロージャ1個あたり
セル1個の確保 — slabのpopとGCへの登録 — で、本体がクロージャを作って
呼ぶだけのループで15%（300万回、0.41秒→0.47秒）、`tests/perf`では
測れる差にならない。あちらはクロージャを1度作ってループの中で呼ぶからだ。

TSanはそのテストで0件、isolateテスト20本全部でも0件になり、ストレス
再現は7/40から0/80になった。

### 12.8 IR量は全部cleanupだった

§12.5は「約30%多いIR」を、どこにあるとも言わずに宿題として残した。
`-O0`のモジュールを基本ブロックの系統ごとに割り振ると、1パスで答えが出る。
hello worldの2,763命令の内訳:

| ブロック系統 | branch | master（AST） |
|---|---|---|
| unwind cleanup | **1,045（37.8%）** | 291（15.5%） |
| それ以外 | 1,718 | 1,581 |
| landing pad | 38 | 13 |
| `_Unwind_Resume_or_Rethrow` | 39 | 8 |

cleanup以外のIRはAST経路の8%増に収まっていた。**回帰は全部padにあった**
（超過命令の86%）。

原因は仕事の量ではなく形である。throwは in-flight のテンポラリを置き去りに
するが、lowering はその集合を「throwしうる全サイト」で問い合わせてメモ化し、
**集合ごとに専用の landing pad を作って、そこに解放列の写しと自前の
re-raise 辺を丸ごと持たせていた**。AST codegen は同じ後始末を別の形で
払っていた — **スコープごとに1つの pad**、そこから「1スロット解放して
次へ」のブロックを繋いだ鎖を、必要な段（rung）から入る
（`fn.release.3 → fn.release.2 → fn.release.1 → fn.unwind`）。だから
あるスロットの解放コードは、何サイトがそれを置き去りにしても1つで済む。

鎖はもともとフレームの束縛用に存在していた — `CleanupPad`自身が
「多入口の領域（フレームのラダー）は、共有する下降チェーンの末尾に
builderを置いてから死ぬ」と書いている。テンポラリがそれを使って
いなかっただけである。今は使う: スコープごとに1本の鎖、テンポラリ1つに
1段で接頭辞を共有（1スコープの集合は共通のfloorを持つスタックなので、
他の集合の接頭辞になっている集合は自分の段を持たない）、足元に
re-raiseが1つ、入口は landing pad と分岐だけ。pad の索引は
「どのspanで訊かれたか」ではなく「何を解放するか」を鍵にしたので、
同じテンポラリを置き去りにする2つの文は1つの入口を共有する。

| `tests/test_core.cul` | master（AST） | branch 修正前 | branch 修正後 |
|---|---|---|---|
| `-O0` IR命令数 | 234,293 | 353,220 | **232,289** |
| `-O0` コンパイル | 3.06秒 | 4.05秒 | **2.99秒** |
| `-O2` コンパイル | 3.56秒 | 5.30秒 | **4.35秒** |

量は消えた。loweringは置き換えた codegen よりわずかに少ないIRを吐き、
`-O0`ではもうコンパイルが遅くない。残るのは`-O2`の行で、これは
§12.5が特定したのとは別の問題である — §12.9で分解する。

### 12.9 `-O2`の残りはoptimizerではない

`-O2`のコンパイル時間から`-O0`のそれを引いて差を「optimizerの取り分」と
呼ぶのは誤りで、§12.5はそのせいで違う半分を追いかけた。**どちらの実行も
機械語生成をしており、しかも別のIRから生成している** — `-O0`は吐いたままの
モジュールを、`-O2`は1/3の大きさになったモジュールをcodegenに渡す。差は
「optimizerの時間」から「入力が小さくなってcodegenが得した分」を引いたもので、
この2つは独立に動く。

分けて回せばよい。JITのパイプラインは`buildPerModuleDefaultPipeline(O2)`で、
これは`opt -passes='default<O2>'`が回すものと同じだから、各バイナリが吐く
`-O0`モジュールをculebraの外で`opt`→`llc`と通せる
（`tests/test_core.cul`、wall clock）:

| | master（AST） | branch（bytecode） | |
|---|---|---|---|
| `opt -O2` | 1.48秒 | 1.58秒 | +7% |
| `llc`（`-O0`のIRに） | 3.13秒 | 2.81秒 | branchが安い |
| `llc`（`-O2`のIRに） | 1.76秒 | **2.36秒** | **+34%** |
| `-O2`の opt + llc | 3.24秒 | 3.94秒 | 1.22倍 |

最後の行はculebra自身の3.56秒対4.35秒と同じ比であり、`-O0`の行も
culebra自身の3.06秒対2.99秒と一致する — branchは本当にそちらでは安い。
**optimizerは最初から問題ではなかった。7%高いだけである。機械語生成が
34%高い。**

理由: codegenは関数サイズに対して超線形で、2つのモジュールは同じ仕事を
違う分け方で持っている。

| `-O2`後 | master | branch |
|---|---|---|
| 命令数 | 95,925 | 102,699 |
| 関数数 | 199 | 148 |
| 関数サイズの Σn^1.9 | 1.393e8 | **1.838e8（1.32倍）** |

予測1.32倍に対して実測1.34倍。指数はこのコードベースのコンパイル時間で
既に確かめてある値なので、一致は偶然ではない — 同じ法則を反対側から
見ているだけである。

ここから2つのことが出てきて、向きは逆である。**branchの関数数が少ないのは
欠陥ではない** — AST codegenがコールバックの本体を**複製していた**のだ。
`xs.map(fn (x) { … }).filter(fn (x) { … }).sum()`の1行で、masterは各lambdaを
**4部ずつ**吐く（ディスパッチの腕ごとに1部）。loweringはそれぞれ1チャンクである
（この1行だけで15関数対9関数）。重複を消すのは正しく、だからこそbranchの
cleanup以外のIRは`-O2`後にmasterより9,379命令**小さい**。

残っているのはまたcleanupで、§12.8はそれを終わらせていなかった:

| `-O2`後 | master | branch |
|---|---|---|
| cleanupの命令数 | 8,826（9.2%） | **24,979（24.3%）** |

超過16,153命令は、branchが全体で大きい6,774命令より多い。これを取り除いた
分だけ関数を縮めるとΣn^1.9は0.73倍になり、`llc`は1.72秒 — masterの1.76秒に
なる。**残っている差はまるごとこのcleanupである。**

閉じるには pad を短くするのではなく**数を減らす**必要がある。§12.8で各padは
短くなったが、「置き去りにするテンポラリの集合ごとに1つ」のままで、masterは
「スコープごとに1つ」だった。masterがそうできたのは、in-flightのテンポラリを
**他が二度と使い回さない専用スロット**（`build.guard`）に置いていたからで、
それを1つのpadが浚えばスコープ内の全サイトを覆える。loweringのテンポラリは
バイトコードのレジスタであり、**レジスタは世代をまたいで使い回される** —
executor自身のunwindがそう書いている（テンポラリがスロットをnilにする理由を
「後の世代がcellに変えたインデックスに対しても範囲解放が安全になる」と説明して
いる）。だからスコープのテンポラリスロットの和集合を解放すると、後の世代の
**cell**を平の解放で解放してしまう。つまりサイトごとの集合は効いており、
畳むには「レジスタファイルの使い回し」ではなく「loweringが自分で持つunwind
スロット」が要る — 2つのVMレーンが共有するテンポラリ規律への変更で、§12.8より
大きい仕事である。

### 12.10 throw経路が食うのはレジスタであって命令数ではない

§12.9は予測で終わっている — cleanupの超過を消せば差は埋まる、その手段は
「スコープごとに1つのlanding pad」だ、と。両方を実際に作って計測した。
予測は外れており、**その外れ方**が、残っている1秒が何を買っているのかを
説明する。

まず§12.9が挙げた障害から。これは実は取り除ける。粗い解放が生きている
束縛に届かないためには「同じスコープの中で、あるレジスタがテンポラリで
あり束縛でもある」ことが決して起きなければよく、それはコンパイラが保証
できる: `Scope::temp_high`がそのスコープがテンポラリに使ったところまでを
覚え、`alloc_slot`がそれを跨ぐ。するとステップは自分のセグメントが持った
テンポラリの**和集合**を解放できる（throwが届かなかったレジスタはnil）。
サイトごとの集合は結局効いていなかった。代償はフレームあたり約1/6の
レジスタ増で、動く。

そしてこれは、ここで計測したどれよりも小さい最適化後モジュールを作る
——命令数89,984、masterの95,925より小さい——と同時に、`llc`に
**33.8秒**かける。スコープ内の全invokeが1ブロックにunwindするようになり、
mem2regは「述語705個のブロックが十数本のレジスタを読む」に対して
「レジスタごとに幅705のphi」で答える。phi総重量は72,519から640,872へ。

つまり量は指標ではない。何が指標なのかを決める計測はこれだ:
padとその辺はそのままに、**何も解放させない**。

| `tests/test_core.cul` | `-O0` IR | `-O2` IR | `opt` | `llc` | 合計 |
|---|---|---|---|---|---|
| master（AST codegen） | 234,293 | 95,925 | 1.72 | 1.66 | 3.38 |
| branch（現状） | 232,289 | 102,699 | 1.84 | 2.42 | **4.26** |
| branch（padが何も解放しない） | 202,134 | 81,935 | 1.43 | 1.60 | **3.03** |

EHの構造はタダである。**差はまるごと「padが何を読むか」だった。**

`llc -time-passes`が行き先を言う: Greedy Register Allocatorが0.43秒
（masterは0.17）、Register Coalescerが0.29秒（同0.07）— 差0.76秒のうち
0.47秒。命令選択は両者同じである。アセンブリにも直接出ていて、同じ175関数で
スタック参照が43,703個、padを空にすると19,790個になる。**landing padへ
生きたまま入る値はスピルするしかない** — unwinderはフレームのCFIに従って
callee-savedを戻すだけで、それ以外は保存しない — から、ステップがまだ必要と
するレジスタは、throwしうるサイトのすべてでメモリを経由して辺を渡る。

AST codegenのthrow経路がコンパイルに安く、こちらが安くない理由がこれである。
masterはフレームの多くをメモリに置いたままにする: `-O2`を生き延びるstoreが
6,051個、branchは2,726個。masterのスコープスロットはそもそもSSA値ではないので、
padで解放しても生存区間を作らない。loweringはバイトコードのレジスタファイルを
mem2regで昇格させる — それこそが、置き換えたcodegenよりコードを2〜4.5倍速く
している当のものである（§12.5）。**コンパイル時間の請求書と実行時間の勝ちは
同じ一つの事実である。**

padの形を4通り、同じファイルで:

| padの形 | `-O0` IR | `-O2` IR | phi最大幅 | `opt`+`llc` |
|---|---|---|---|---|
| 置き去り集合ごとに1つ・鎖を共有（§12.8） | 232,289 | 102,699 | 63 | **4.26** |
| スコープごとに1つ・和集合を解放 | 225,282 | **89,984** | 705 | 37.6 |
| 文ごとに1つ・鎖を共有 | 246,801 | 105,519 | 73 | 4.95 |
| 文ごとに1つ・直列 | 328,994 | 113,705 | 176 | 5.55 |

§12.8が辿り着いた形が最良で、2つの軸は互いに引っ張り合っている:
padをサイト間で共有すればモジュールは縮みphiは太る、分ければphiは細り
モジュールは膨らむ。もう1つ試して効かなかったもの: temp preludeが
scopeステップへre-raiseせず**直接分岐**する形（preludeごとにlanding padと
`_Unwind_Resume_or_Rethrow`が1つ減る。4.18秒対4.24秒 — SimplifyCFGが
既にこの往復を畳んでいた）。

これを閉じるのはpadの形ではない。cleanupステップが読むレジスタをSSAに
昇格させないことであり、それは実行時の勝ちを差し出してコンパイル時間を買う
取引である。`--jit`が一度コンパイルしてループを回す言語では、それは逆向きの
取引になる。Phase 3はここで止める: コンパイル1.2倍、実行2〜4.5倍。

### 12.11 `--vm-llvm`の退役

フラグがレーンであるのは、その後ろで何かが違う動き方をしている間
だけである。`--vm-llvm`は§12.2でそうでなくなった。外す前に両者を
差分してある: 5ファイルの`--emit-llvm`出力の差は1行だけで、
`test_class.cul`の`__finit_<16進>`という記号名 — これは`--jit`を
2回走らせても変わる。残っていたのはエンジンの性質ではなく起動の
性質2つである。`--vm-llvm`は`--jit-faststart`を受け付けず、モジュール名を
`"vm"`に固定していたのでオブジェクトキャッシュの鍵にならなかった。
そのキャッシュは`CULEBRA_JIT_CACHE`未設定で無効なので、どちらにせよ
どのゲートも使っていない。

その結果、ゲートが本当でないことを言っていた。
`check_alloca_discipline.sh`は1つのプローブを1つのemitterに2回通して
`alloca-discipline OK (jit + vm lowering: ...)`と出していた。
`check_eh_balance.sh`は同じcodegenに対して`jit`レーンと`vm`レーンを
報告していた。対称性スイープは207本の`tests/*.cul`を`--jit`に通し、
続けて`--vm-llvm`にもう一度通していた。**エンジンが1つしかないのに
2つを名指しするゲートは、1つを名指しするゲートより悪い** — 余分な
名前は、読み手が確かめる術を持たない部分そのものだからである。
フラグを退役させる理由はこれであって、短縮した時間は副産物である。

その時間も実在する。4レーン目を本来の姿 — 2度目の`--jit`パス — として
復元すると、スイープは42秒に対して84秒、`just test-dev`は186秒に
対して226秒になる。

2箇所は理由があってこのフラグを名指ししていたので、削らずに張り替えた。
`tools/bench/vm_cases/compare.sh`は独自の177ケースのコーパスを持ち、
そのレーンはloweringの実カバレッジだった — 今は`--jit`という綴りで、
コストは変わらない。isolateスイートは、削除がカバレッジの削除に
なる唯一の場所だった: `jit`レーンは18本の`*_jit.cul`だけを走らせ、
`--vm-llvm`レーンは20本全部を走らせていたので、2本が誰にも
コンパイルされないまま残るところだった。この2本は`--jit`で通る
（`Isolate.spawn`・`Channel`・`Parallel`はPhase 2以来対称である）ので、
`jit`レーンがディレクトリ全体を取る — これは`--vm-llvm`レーンが
ずっと証明し続けてきたことである。ehゲートでは`vm`レーンの小さな
プローブも張り替えて残した: ループの反復ごとに1回入るtry/catchは、
fullプローブが持っていない形である。

`--vm`と`--vm-dump`は残す。executorは2つ目のエンジンであり、
どちらのフラグにも`--jit`側に等価物が無い。

## 13. Phase 4: tree-walkerの退役

### 13.1 エンジンに名前を付ける

デフォルトエンジンには名前が無かった。`--jit`と`--vm`はそれぞれ
フィールドを立て、tree-walkerは「どちらも立てなかったときに走るもの」
— `vm == Off && !jit`という排除による定義で、どの呼び手にも綴れない —
だった。デフォルトが動かないうちはこれは何のコストでもない。Phase 4は
それを動かす。そしてどのエンジンが欲しいのか一度も言わなかった呼び手は、
黙ってそれと一緒に動く: tree-walkerを測っていたレーンがVMを測り始め、
どちらであっても同じOKを出す。

そこで最初のバッチは、欠けている名前を足し、その不在を騒がしくする。
`--tree`はtree-walkerを明示的に選ぶ。`--help`には出さない — この
repoの中の呼び手のために存在し、削除バッチが再び取り除くからである
— そして`CULEBRA_JIT_ENABLED`のガードの外でparseする。no-JITビルドは
名指しできるエンジンをちょうど1つしか持たないが、ゲートスクリプトは
同じコマンドラインを渡してくるからである。

呼び手を見つける作業はgrepではなかった。`CULEBRA_REQUIRE_EXPLICIT_ENGINE=1`
は暗黙の選択をabortに変え、フルゲートが落ちるという形でそれらを報告する。
検査は自分でデフォルトを選ぶサイトごとに置く。5つあって互いに無関係
だからである: スクリプト実行・REPL・`dap`・doctestランナー・unitテスト
ランナーが、それぞれ自分の`Interp`を持っている。`fmt`・`lint`・`docs`・
`--ast`・`--version`はユーザーコードを走らせないので、一度も訊かれない。

非ゼロ終了ではなくabortにした。非ゼロステータスを期待するレーンが
いくつもあるからである — leak-abort suiteはSIGABRTを自分の信号として
読み、CLIテスト群は`rc`をassertする — きれいな失敗exitは、それらの
レーンがまさにテストしたかった事象として読まれてしまう。

これで105 launches・24ファイルが出た: justfileのinterpレーン、difftestの
生成器とその3つのランナー、14本の`tests/*.sh`のうち12本、`dap_test.cc`が
アダプタを起こす`execlp`、`just`を経由せずバイナリを起動するCIステップ、
そしてシェルがひとつも無い場所が1つ — `tests/isolate/test_proc_share_jit.cul`
が`Proc.run`で子のculebraプロセスを起こし、子は親から変数を継承する。
生成器は独立した1行に値する: `tools/difftest/gen.cul`は他の全レーンが
比較される対象となるコーパスを書くプログラム（§2）なので、それをどの
エンジンが走らせるかは細部ではない。

そのうち2つはフルゲートでしか現れなかった。`just test-dev`は`lint_test`と
`dap_test`が素起動のままでも緑だった。ctestは`just test`にしか無いから
である — §12.3を裏返した話で、**ゲートが測っているものはその名前が
示唆するものではなく、速いレーンは速度のために選ばれた部分集合である**。

もう1つはCIにしか現れず、しかもisolateテストと同じ形をしていた。Windowsの
SharedBufferスモークは、自分のプログラムを`ci.yml`の中にheredocとして
書き、`Proc.spawn([exe, ...])`で子を2つ起こす。ワークフローファイルの中に
綴られたculebraプログラムは、`.cul`ファイルを舐める掃引にもシェルコマンドを
舐める掃引にも届かず、ローカルのどのレーンも走らせない。ratchetはそれを、
唯一可能な方法で見つけた — 走らせることによって。

この変数はいま、すべてのjustfile recipeと両方のワークフローでデフォルト
設定されており、そこがこのバッチの要点である。一度きりの掃引は腐る。
ratchetがあれば、バイナリを素起動する新しいrecipeは最初に走った瞬間に
abortする。`just`の外側では何も変わっていない: 素の`culebra prog.cul`は
今もtree-walkerを走らせ、デフォルトが動くまではそうであり続ける。

### 13.2 コーパスだけが到達するものを測る

tree-walkerを消すことは差分オラクルを失うことである: その後はexecutorと
LLVM loweringが同じコンパイラからバイトコードを受け取るので、コンパイラの
バグは — あるいはランタイムヘルパーのバグ、共有emitterのバグは —
両レーンに同じ誤答を出させ、コーパスは緑のままになる（§7）。このバッチが
答える問いは、コーパスが消えたとして、手書きのスイートがその共有面を
どれだけ支え続けられるのか、である。

`-DCULEBRA_COVERAGE=ON`がドライバを計装する: `-O0 -fno-inline`なので
共有ヘッダの`inline`ヘルパーはどれも数えられる本体を持ち、
`-fprofile-update=atomic`なのはisolateスイートが複数スレッドから
カウンタを回すからである。フラグは`culebra`ターゲットだけに付く —
測る対象の面はヘッダのみで、その翻訳単位に落ちる — そしてビルドは
`--gc-sections`をopt-outする。さもないと「誰も呼ばない関数のカウンタ
セクション」を捨てるのは自由であり、それこそが測ろうとしている母集団
だからである。

面はファイルではなく**demangle後の名前**で切る。`vm.h`はバイトコード
コンパイラ・executor・loweringを1ファイルに抱えており、共有fateなのは
最初の1つだけである。`culebra::vm::Compiler::`・`culebra::JIT::`・
`culebra_runtime_*` / `culebra::_jit_*`のヘルパーを数え、`vm::Exec::`と
`vm::Lowering::`は数えない。これはレポートを、`vm.h`へのあらゆる編集が
無効化してしまう行範囲マップからも救っている。

プロファイルは1つでなく2つ: 耐久スイートと、それに生成コーパスを足した
もの。耐久側はゲートの`ci-light`レーン — 生成コーパスを持たない最大の
レーン — に`tests/*_test.sh`のCLIスクリプト・doctestブロック・
`--jit-faststart`パスを足したもので、それぞれ`--vm`と`--jit`で走り、
`--tree`では一度も走らせない（tree-walkerはコンパイラもランタイムも
共有していない）。gcdaはビルドツリーでなく`GCOV_PREFIX`経由で書く。
libgcovのread-modify-writeは並行安全でないからで、shimがプロセスごとに
自分のprefixを与え、`gcov-tool merge`が畳む。

耐久側は直列に走り、それはデフォルトではなく発見である。`xargs -P 20`で
走らせると、直列版より**少なく**ランタイムを測った — live 663に対して605
— 計装された`-O0`のプロセスが20個競合すると、負荷に敏感なスイート
（Httpサーバ・net・isolate）が実行を落とし始めるからである。プロセス
起動が実際にwall-clockの大半なのは確かだが、それを買い戻すと測ろうと
している数字自体が動く。そして**負荷について動く測定は測定ではない**。
コーパス側は並列のままである: あのチャンク群は純粋な計算で、ポートを
bindせず子も起こさない。32分、その大半は`-O0`の`--jit`レーンである。

| | 関数 | | | 行 | | |
|---|---|---|---|---|---|---|
| | total | live | corpus-only | total | live | corpus-only |
| compiler | 265 | 238 | **0** | 3169 | 3034 | **0** |
| emitter | 186 | 163 | **0** | 2152 | 1996 | **0** |
| runtime | 923 | 830 | 2 | 6017 | 5406 | 24 |

**コンパイラと共有emitterには、コーパスだけが到達するものが何も無い。**
関数レベルのゼロだけならほとんど何も証明しない — 両方が実行する関数の
中にコーパスだけが通る分岐は隠れうる — それが行の列がある理由であり、
そちらも同意している。コーパスが足すものはすべてランタイムにある:
12関数にまたがる24行である。

### 13.3 コーパスが代役を務めていたテストを書く

12関数の24行、そしてtree-walkerの最後の仕事は、そのそれぞれが何をすべきかを
言うことだった。オラクルのうち期限が来るのはその部分である: エンジンが1つに
なれば、新しいテストの期待値はそのエンジンが印字するものになり、「テストが
実装と一致する」は証拠ではなくなる。tree-walkerがまだここにいるうちなら、
ケースをそれに対して書き、`--vm`と`--jit`でバイト単位に一致することを
確認してから凍結できる — 下のケースはすべてそうやって書かれた。

置き場所は自分たちのファイルではなく、そのふるまいを既に所有している
スイートである。`tests/test_shared.cul`は凍結されたviewの読み側の辺を得た:
Stringキーの不在、`Long`と`Bool`のキーで凍結されたObject、Stringで添字
されるArrayのview（`expected Long, got String`）、そして末尾を越える添字。
`tests/test_runtime_errors.cul`は、添字エラーが一度も描画する必要のなかった
2つのタグ名 — `got Function`と`got Set` — に加えて、`/`と単項`-`のガード、
配列の末尾を越える書き込みの`IndexError`を得た。`tests/test_tensor.cul`は
`Tensor.no_grad`（手書きのものが一度も呼んだことがなかった）と、2要素
tensorに対する`.item()`を得た。残りは1件ずつである: キーを1つも持たない
objectから非Stringキーを削除する（`tests/test_object_keys.cul`）、variant
コンストラクタへの余分な位置引数（`tests/test_enum.cul`）、Stringの
イテレータに対する`join`（`tests/test_iter_terminal.cul`）、ピクセルが
既に解放されたspriteへの`draw_to`（`tests/test_canvas_module.cul`）、
そして還元するものが何も無い`Math.max()`（`tests/test_math_kernels.cul`）—
最後のものはthrowにソース位置を供給するためだけに存在するlambdaに届く。

測り直すと、corpus-onlyの集合は空である: 3つの列すべてで、0関数・0行。
`tools/coverage/corpus_only_coverage.txt`がそれをratchetとして記録する。
leak-abort allowlistと同じ双方向の契約で — そこに載っていないcorpus-only
関数はレポートを落とし、載っている名前をスイートがその後到達したなら
ファイルを縮められるように報告する。両方向とも、ファイルをcommitする前に
発火させた: もはや資格を満たさない名前は「shrink」と印字し、耐久側が空の
プロファイルは1231件の新規エントリとexit 1を出す。**誰も落ちるところを
見たことのないratchetはコメントである。**

キーは両方の列で関数であって、最初の列だけではない。両方の集合が既に
入っている関数の中の、コーパスだけが通る新しい分岐は、corpus-onlyな
*関数*を増やさないがcorpus-onlyな*行*を増やす — そしてその行の持ち主も
載っていなければレポートは落ちる。行の側を`(file, line)`でキーにするのは
明白な代案で、§13.2が関数の側について述べた理由から誤りである: 行番号は
あらゆる編集で動き、**churnするratchetは人が消すratchetである**。

持っていないものはcadenceである。`just coverage`は35分の計装測定で、
`just test`の外・PRごとのCIの外にあるのが正しく、その結果としてこの
ファイルの状態は、時間を差し出す誰かによってしか発見されない。それを
暗黙にする代わりに、その瞬間をファイルの中で名指しした: B5がコーパスを
executor対loweringに張り替える前に測り直せ、なぜならその後は測定が別の
問いに答えており、このbaselineは比較可能でなくなるからである。週次の
スケジュールジョブが代案で、計装ビルドと4コアランナー上の直列掃引が
かかる。それは実費であり、ここでは払っていない。

リストが空であることは、スイートが走らせているすべてのふるまいを
pinしていることと同じではない — §13.2の但し書きは今も有効で、カバレッジは
assertと素の呼び出しを区別できない。それが決着させるのはもっと狭い話で
ある: コーパスがexecutor対loweringに張り替えられたとき、共有面のどの
領域も、生成された入力を唯一の読み手として残されてはいない。

### 13.4 no-LLVMビルドにエンジンを与える

tree-walkerを消すことは、あらゆるビルドからエンジンを1つ取り上げることで
ある。ほとんどのビルドは2つ目を持っているが、3つは持っていない。JITを
有効にするCMakeオプションは`OFF`が既定なので素の`cmake ..`がその1つを
作り、`build-no-jit`のCIレーンがもう1つ、3つ目はWASM Playgroundで、
LLVMを一切持たないemccでビルドされる。§13の計画ではその削除はB7で着地する
のだが、ガードが当時のままで着地していたら、この3つはエンジン1つから
ゼロになっていた。

障害は一度もVMではなかった。この作業の前に測ってある: `vm.h`の最初の
`llvm::`は11,000行目、`Lowering`の内側であり、その上のコンパイラと
executorはLLVMをどこにも名指ししていない。8つのランタイム断片のうち7つも
そうだった。立ちはだかっていたのはガードである。それらの断片はどれも
`#ifdef CULEBRA_JIT_ENABLED`で始まっていたので、LLVMの無いビルドには
`JitValue`も無く、ランタイムヘルパーも無く、`builtin_signatures.h`も
無かった — 「届かないエンジン」ではなく、**そのエンジンが書かれている
ところの値モデル**が無かったのである。

だからこのバッチはポートではなく分割である。`rt.h`が新しく、両方の
消費者が立つものを持つ: それらの断片が`jit.h`に開いてもらうことを当てに
していたinclude前置き、断片8本そのもの、そして下記の小さなフロントエンド
契約。`jit.h`はLLVMのincludeと`struct JIT`を保つ。`vm.h`はコンパイラと
executorを保ち、`vm_lowering.h`が新しくloweringと`run_modules_via_llvm`・
`build_object_from_modules`・それらの上に定義された3つの`JIT::`埋め込み
入口を持つ。`CULEBRA_JIT_ENABLED`はいま「LLVMがリンクされている」と読み、
それが守るのは`jit.h`・`vm_lowering.h`・`stdlib_jit.h`の1メンバ・AOT
bootstrapである — 最後のものはLLVM自体を必要としないが、`culebra build`が
リンク用のオブジェクトを吐ける場所にしか存在しない。

`struct JIT`から出なければならなかったものが4つある。バイトコード
コンパイラがそれを通して読んでいて、さもなければ持ちえないクラスを
必要としてしまうからである: `install_extension` / `current_hooks`つきの
`ExtensionHooks`、自由変数解析が参照する`is_builtin_var`述語、
`fn_introspection_name`述語、そして両エンジンがswitchする`ForKind`の
カーソルタグ。どれ1つとしてJITを必要としたことはない — フロントエンドと
stdlibがinstallしたものとの間のコンパイル時契約であって、当時たまたま
唯一のコンパイラだったクラスに置かれていただけである。いまは`rt.h`の
名前空間スコープにある。`install_extension`は公開された名前を持つ唯一の
もので（`docs/deployment.md`）、名前ごと移動した。

「LLVM-free」なランタイムのうち2箇所は本当にLLVMを必要とし、どちらも
ランタイムの話ではなくLLVMに何かを手渡す話である。`jit_mem.h`の末尾には
Win64のRTDyldメモリマネージャがあり、RTDyldがロードするオブジェクトごとに
`RtlAddFunctionTable`を呼ぶ — JITが今書いたばかりのフレームをthrowが
unwindできるようにするためである。その場でガードすれば十分に見えて、
そうではない: あの2クラスは`llvm::SectionMemoryManager`から**派生して
いる**のに、`rt.h`は`jit.h`がLLVMヘッダを開く**前**に読まれるので、
WindowsでJITを有効にした構成では未宣言の基底に対して解析される。Linuxの
どのレーンもそれを示せない — 示せたのは`windows-jit-build`だけである。
2クラスは唯一の呼び手であるORCレイヤの隣、`jit.h`へ移動し、そこと共有する
mingwのEH宣言も一緒に行った。移動したらガードが消えた。これはその場での
ガードが**間違った深さ**だったという証拠である。もう1つは
`JitExtension::declare_runtime`で、ビルド中のモジュールにランタイムの
シグネチャを宣言する — 9,000行のヘッダの1メンバが、そのヘッダで唯一の
`llvm::`行を持っていた。こちらはその場でガードした: このhookはnullable
だと文書化されており、省略されたdesignated initializerこそno-LLVMビルドが
欲しがるnullである。

**一般形: 「LLVMを使う」と「LLVMから派生する」は違う。呼び出しはリンク時に
解決できるが、継承は宣言順に従う。**

継ぎ目が1つ捻れたまま残っており、隠すより名指しする価値がある。`vm.h`は
executorが解決先とするstdlibのために`stdlib_jit.h`をincludeし、
`stdlib_jit.h`はその1メンバの定義のために`jit.h`をincludeする — つまり
JITを**持つ**ビルドでは、`vm.h`は推移的にLLVMに届く。どの翻訳単位も実費を
払わないし（JITビルドの`culebra.h`はどのみち`jit.h`を直接include
する）、「`vm.h`はLLVMを必要としない」という主張はincludeグラフでなく
コンパイルの通る構成によって担保されている。真っ直ぐにするには
`stdlib_jit.h`をここで`jit.h`を割ったのと同じ形で割る必要があり、それは
B7の近所である — `stdlib_jit.h`の残り半分はネイティブモジュール束縛で、
あのバッチがどのみち動かすものである。

3つのものが落ちてきた。`JIT::known_builtin_methods()`は`shared.h`の
`culebra::builtin_method_names()`への転送でしかなく、唯一の呼び手である
起動時のドリフト検査はいま単一源を直接読む — おかげでこの検査はLLVMのある
構成だけでなく全構成で走るようになった。`repl.h`は`jit.h`をincludeして
いたが、そこから何も使っていなかった。そして`vm::Exec`と`vm::Compiler`は、
まさに今出ていったメンバに届くためだけに`JIT`のfriendと宣言されていたので、
その2つのfriendshipも一緒に消えた。本当にJITのemitterを使う
`vm::Lowering`だけが残っている。

この移動にIR diffは要らなかった。証明可能に変更ではないからである。
`Lowering`からファイル末尾までは丸ごとバイト単位で`vm_lowering.h`へ行き、
移動した領域の中で唯一のテキスト編集は、いま外側の名前空間で同じ値に
解決される5つの名前から`JIT::`修飾子を落としたことである。スクリプトが
`HEAD`のファイルから新ファイルを再構成して差分を取る。これはIRを吐き
直すより多くを語り、数秒で済む。フルゲートはどのみちコーパスを通して
loweringを走らせた。

この構成まわりのゲートは、§13.1と同じ理由で形を変えた。`build-no-jit`は
リンクを証明していたのであって、**リンクはエンジンが消えたことに気づけない**:
このバッチが防ごうとしているまさにその失敗 — 2つあるべきところにエンジンが
1つのバイナリ — は、完璧にコンパイルされ完璧にリンクされる。`just test-no-jit`は
実物を走らせる: `vm_cases`コーパスを`--vm`と`--tree`の両方で、ゲート自身の
VMフェーズが使うのと同じ`compare.sh`に通し、加えてREPL、加えて`--version`
— これはいま、そのビルドが持っているもの（`interp+vm`、あるいは
`interp+vm+jit`）を名指しする。LLVMの無いビルドがインタプリタしか持たないと
暗示するのをやめたのである。CIジョブはビルドだけでなくそれを走らせる。
比較を書き直さず`compare.sh`を再利用したのは経済性だけの話ではない: あれは
既に各レーンの**終了コード**を比較に畳み込んでおり、手書きの
`diff <(a) <(b)`は両方を捨てる — §12.3の教訓、このrepoが一度払った代償は、
stdoutしか読まないとSEGVが「等しい」として通るということである。

3つのうち1つは半分しか終わっておらず、そう言うことがここで名指しする
理由である。`build-no-jit`レーンと素の`cmake ..`はいまexecutorを持ち、
`--vm`でその上を走る。Playgroundは持たない。`playground/wasm_main.cc`は
`interpreter.h`をincludeして`interpret_modules`を直接呼んでおり、そもそも
選ぶべき2つ目のエンジンを持ったことがないからである。このバッチがそこで
変えたのは、いまや**できるようになった**ことだけである: `vm.h`がもはや
LLVMを要求しないというのが、それができなかった唯一の理由だった。実際に
そうするのはデフォルトエンジンの切替であり、それはB6のものである。前提に
せずB6のリストに載せてある — このrepoの中で誰も手で動かさないビルドこそ、
B7の後にエンジンを失っていたと発見されるビルドだからである。

### 13.5 リリースが残していくオラクル

tree-walkerに残された最後の仕事は、第二の意見であることである。§13.2は
手書きのスイートが共有面のどれだけに到達しているかを測り、§13.3は
到達していなかった分のテストを書いた。しかしどちらも§7が名指しする穴を
塞がない: 削除の後、executorとLLVM loweringは1つのコンパイラから
バイトコードを受け取り、コンパイラのバグは両レーンに同じ誤答を出させる。
カバレッジが言うのは「そのコードが実行された」であって、「何かがその
結果を検査した」ではない。

そして、保守しなくてよい第二の実装が1つだけ存在する。既にビルド済みで、
もう変わりようがないからである: 前のリリースのバイナリである。それは
ビルドされた日に言語が表現できたすべてについて答えられ、3つのプラットフォーム
向けにダウンロードでき、この作業ツリーへのどんな編集もそれを動かさない。
**tree-walkerを消すことは差分オラクルを破壊するというより封印することで
ある** — 凍結された部分集合については、独立実装がリリースをまたいで
答え続ける。

`tools/difftest/release_diff.sh`がその検査である。run.shが使うのと同じ
生成コーパスを組み立て、baselineバイナリとこのビルドの下でそれを走らせ、
ふるまいの変わったケースをすべて報告する。

**両側とも、フラグ無しで自分のデフォルトエンジンの上を走る。** これは
このrepoの中で選択を暗黙のままにする唯一の場所であり、それらの実行に
ついてはratchetをunsetにするほど意図的である: ここでの問いは`culebra
prog.cul`と打った人にとって何が変わったかであって、デフォルトは見落としでは
なく主題だからである。そもそもbaselineは`--tree`より前のもので、他の
言い方はできない。そしてこれは、B6がデフォルトを反転させたとき、このゲートが
編集されないままtree-walkerとVMを比較するということでもある — あの切替が
最も欲しがる検査が、ただで手に入る。

byte diffではない。そしてその理由は、run.shがもう持っていないものである。
古いバイナリは、リリースが足したbuilt-inのすべてについて、こちらが`ok=`と
答えるところで`err=`と答える。しかも`ok=`のケースは、失敗する側が決して
到達しない行を印字しうるので、2つのファイルは「ケースとして揃わなくなる」
より遥かに前に「テキストとして揃わなく」なる。`_p`はどのバイナリの下でも
ケースごとにちょうど1レコードを吐くので、レコードは位置で辿り、それぞれが
自分の後に印字された出力を連れて歩く。

ケースはbaselineより後に生まれた構文で書かれていることもあり、そのケースは
単独では落ちない — それが座っているチャンクがparseに失敗し、その後ろの
レコードが全部一緒に消える。完走ガードが短いチャンクを捕まえ、そのチャンクを
1ケース1プロセスで、並列に走らせ直す。それでもレコードを出さないケースは
`<label> ::: unsupported`として吐かれる。2つの側はレコード単位で揃った
ままになり、レポートは古いバイナリが何を表現できなかったかを正確に言える。
同じマーカーがhead側に出たら、それは変化ではなく失敗である: そのバイナリは
コーパスを生成した当人なので、走らせられないケースは書くべきでなかった
ケースである。

この経路は、必要とするのがbaseline側だけであっても両側が一緒に取る。
エラーレコードはthrowの起きた行と列を運んでおり、それはチャンクプログラムの
中の位置だからである。片側だけを1ケース1プロセスで走らせ直すと、その側の
エラーレコードが全部番号を振り直され、**throwしたケースすべてに差異を
でっち上げる** — このゲートが読もうとしている母集団まるごとである。この
経路は他の方法では要求に応じて到達できない（コーパスにいま入っている構文
より前のリリースが要る）ので、`RELEASE_DIFF_FORCE_FALLBACK=1`が全チャンクで
それを取る: v0.2.0相手にそうやって走らせると、88のchunk-sideすべてが
1ケースずつ走り直し、レポートはチャンク版と同一の261と261になる。

**どの差異も`release_diff_allow.txt`で名指しされていなければならない。**
それがこのファイルをリリースノートの下書きにしている。契約は双方向で、
leak-abort allowlistと同じである: 載っていない変化はゲートを落とし、何にも
マッチしないパターンは、それを必要としたリリースが出た後にファイルを
縮められるように報告される。v0.2.0に対する最初の実行は17,262ケース中261件が
変わったと言い、それを書き起こしたものがいまこのファイルの中身である:

| 何が変わったか | ケース数 |
|---|---|
| Stringが15のメソッド名に答えるようになった — 13は新規、加えて`index_of`と`reverse`、そのレシーバマスクはArrayしか通していなかった | 150 |
| その15のどれかを呼ばずに読む: 0.2.0ではnil、いまは「値としては使えない」 | 15 |
| `repeat(n, value)`が新しいグローバル関数で、UFCSはグローバルをコーパスが掃引する全レシーバのメソッド位置に置く | 90 |
| ネイティブの宣言シグネチャがどのエンジンでも正典になったので、誤った呼び出しはメソッドごとの手書きの腕で引数を数えるのでなくパラメータを名指しする | 6 |

パターンはケースラベルに対するglobで、ワイルドカードは`*`と`?`だけ、
それ以外は — 角括弧を含めて — リテラルである。`fnmatch`が明白な選択肢で、
そして誤りである: ラベルはculebraソースの断片なので
`kw|[1, 2, 3].sorted(bad: 1)`は文字クラスとして読まれ、1ケースを名指しする
つもりで書いたエントリが黙って100件にマッチしてしまう。

比較器は自分自身のスモークテストを持っており、これは儀式ではない。この
ゲートはmasterへのpushでしか走らず、何も変わっていなければOKと印字するので、
比較をやめた比較器は「静かな1週間」とまったく同じに見える。
`release_diff_selftest.sh`は10個の合成ケースを与える — どれも比較器が
**落ちられ**なければならない道筋である — そして`release_diff.sh`は他の
何よりも先にそれを走らせる。だから緑のレポートは、単にOKと印字したものでは
なく、検査された比較である。

CIはmasterへのpushごとに、公開されているバイナリを相手に走らせる: tagを
指定しないので、リリースを出せばbaselineは自分で前に進む。pull requestでは
走らせない — 比較する相手は*リリース*であり、まだ着地していないブランチに
ついては何も言わないし、B6の切替が見張ってほしいのは着地したコミットだから
である。またこれは、このマシンでは走らせられない唯一のレーンでもある:
リリースバイナリはubuntu-latestでビルドされ、この箱にあるより新しいglibcを
要求する。スクリプトがbaselineを自分で取りに行かず引数として受け取るのは
それが理由で、上の測定はv0.2.0のtagをローカルでビルドして得たものである。

最後に、コーパス自身が立ち位置を変えた。run.shの主たる比較はいま`--vm`対
`--jit` — executor対lowering、tree-walkerより長生きする2つの消費者 —
であり、`--tree == --vm`が第3のレーンとして、そしてその間にいるコンパイラに
ついての答えとして、訊けるtree-walkerがいる限り残る。3レーンとも、いまは
バイト単位で比較する: VMレーンはかつてskipの上限を持ってレコード単位で
辿られていた。サポートするsliceの外のケースがVmErrorと答えたからで、その
sliceはPhase 2で閉じ、上限は0に達した。今日のコーパスにおけるVmErrorは
ふつうの差異であり、`vm_skip_ceiling.txt`は無くなった。

この一連の前に`just coverage`を回し直すと — §13.3が名指しした瞬間である —
新しい穴ではなく壊れた計器が出てきた。corpus-onlyの集合は6つの列すべてで
今も空だが、1,039回の耐久実行のうち176回が非ゼロ終了しており、B2が
まさにそのために足したガード（「早く死んだ耐久実行は、到達しなかった分
だけcorpus-onlyの集合を膨らませる」）は毎回発火していて、何も意味しなく
なっていた。176件はすべて丹念に作られたエラーケースである — `vm_cases`の
84件、leak-abortプローブの4件、それぞれ2レーン分 — 捕捉されないthrowで
終わり設計どおり255で抜けるもので、その終了コードはあるべき場所で検査
されている: `compare.sh`が3レーンを同じ1つに縛るところで、である。この
2つの掃引はいま255だけを受け入れるので、シグナルは今も数えられる。
**毎回発火する検出器は検出器ではない**。そしてこれは書かれた日から、
間違った母集団について本物の数字を報告し続けていた。

### 13.6 Playground がエンジンを選ぶ

§13.4はPlaygroundを「準備は整えたが動かさなかったビルド」として残した。
`playground/wasm_main.cc`はまだ`interpreter.h`をincludeして
`interpret_modules`を呼んでいた。`vm.h`がLLVMを要求しなくなるまで、そこには
選ぶべき第2のエンジンが無かったからである。切り替えそのものは`main.cc`の
`--vm`経路がするのと同じ3つの呼び出しを、同じ順で並べるだけだ —
executorが名前を解決する先のstdlibのための`install_jit_stdlib()`、遅延
ビルダーを宣言するモジュールのための`splice_stdlib_preamble()`、そして
`Compiler::compile_modules`と`Exec::run`。

それはコンパイルが通らなかった。そしてそのとき出た内容こそが、この節の
存在理由である。§13.4の主張は「LLVMの無いビルドにもエンジンがある」で、
`just test-no-jit`がそれを走らせて証明する — だがそのレーンはネイティブの
Linuxビルドだ。wasmは**別の**LLVMの無いビルドであり、壊れた3つはエンジン
ではなく**プラットフォーム**の話である。3つとも、あのバッチが到達可能に
したヘッダを、一度も読んだことのないツールチェーンに手渡した結果だった:

- `jit_gc.h`はWindows以外のあらゆるプラットフォームで`<execinfo.h>`を
  includeしていた。GAP5が出力する生成地点のbacktraceのためである。
  emscriptenのsysrootはpthreadは持つがこれは持たない。mingwが既にそう
  だったのと同じ形で穏やかに縮退する — 0を返す`backtrace`と、それでも発火して
  「(no birth site)」と言う監査。
- `Heap::stack_base`は`#error "unsupported platform"`で終わっていた。wasmは
  値スタックをlinear memoryの外に置き、そこは誰にもスキャンできない。
  `emscripten_stack_get_base()`が返すのはemscriptenがlinear memoryの**内側**
  に確保するuser stackのベースで、アドレスを取られたローカルが載るのは
  そこだ — 保守的スキャンが見つけられたはずのローカルは、元々それだけで
  ある。
- `static_assert(sizeof(JitParamMeta) == 12 * sizeof(int64_t))`はwasm32で
  落ちる。これはポインタを含む構造体を`int64_t`の個数と秤にかけるので、
  フィールド一覧についての主張であると同時に64bitターゲットについての
  主張でもある。そして捕まえるべき対象は`emit_param_meta_global`が吐く
  LLVM構造体とのドリフトで、それはLLVMがある場所にしか無い。いまは
  `CULEBRA_JIT_ENABLED`の下にある。

4つ目は切り替え自身が招いたもので、切り替え自身が解消した。§13.4は警告を
1件借りたまま締めていた: `_jit_shared_val_prop`と`_jit_shared_val_index`は
`jit_runtime.h`で宣言され`sendable_jit.h`で定義されるが、no-LLVMのinclude
連鎖はそこへ届かない。executorのレーンは`stdlib_jit.h`をincludeし、そちらは
届く。

つまりB4の記録は注釈ではなく**訂正**が要る。「Playgroundは切り替えられる
ようになった」は`vm.h`について真で、Playgroundについては偽だった —
同じバッチがそれをコンパイル不能にしており、誰もビルドしていなかったので
誰も気づかなかった。教訓は§13.4自身のもの（「リンクは主張ではない」）より
狭く、より鋭い: **`build-no-jit`とPlaygroundは2つの構成であり、ratchetは
そのうち1つしかコンパイルしない**。emsdkを持つCIランナーは無い。§13.4は
「手で動かすもののないビルドこそ、B7の後にエンジンを失っていたと発見される
ものだ」と言って終わっていた。実際にはそれは既に**コンパイル**を失って
おり、予告より1バッチ早かった。

久しぶりの再ビルドからもう1件落ちてきた。これはここまでの話とは無関係だ:
`emcc`はこのtranslation unitをCとしてリンクし、6.0.8は`operator new`と
libc++の内部を未定義のまま残す — 古いツールチェーンは入力の拡張子から
C++だと推論していた。ドライバは`em++`になった。

検証はdifftestのレーンを縮小したものだ — 遅延stdlibに届き、getterを持つ
クラスがあり、`fib(22)`があり、masterが足したStringメソッドがあり、そして
コレクタを走らせることだけが仕事の2万個のクロージャがある。最後のものが
上の2番目の項目を実際に踏む。その出力はwasmビルドとネイティブの`--vm`とで
バイト一致する。

ただし2回走らせると2回目がtrapする。これはこのバッチが出荷するところだった
不具合であり、wasm固有の話が1つも無いので書き残す価値がある。ページはRun
クリックのたびに同じインスタンスを使うので`Runtime`が1回の実行より長生き
する — そしてnamespaceのキャッシュはそこに住み、キャッシュされたnamespaceが
持つクロージャはそれを作った`VmProgram`を指す。そのプログラムを所有し破棄
するのは、作った当の呼び出しである。遅延モジュール（`Time`・`Canvas`・
`Regex`・matcher群…）を名指すプログラムは、次の回にぶら下がったdescriptorを
読む。tree-walkerはこれに晒されていなかった: 実行ごとに新しい`Environment`
を作り、遅延モジュールはそこに束縛されていたからだ。

修正はすでに木の別の場所に書かれていた。`doc_block_runner`はdoctestの
ブロックごとに専用の`Runtime`を与えており、そのコメントは同じ言葉で理由を
述べている — namespaceのキャッシュとクラス／オーバーロードのレジストリが
そこに住む、と。`run_culebra`はいまそうしている。`install_jit_stdlib()`だけは
意図的にその外に置いた。hookが1回分のコピーではなくプロセス全体の既定に
載るようにするためである。2つのエンジンでここが分かれた理由は、実行ごとの
teardownがこれまでタダだったことにある: インタプリタの状態は実行が解放する
`Environment`にぶら下がっていて、エンジンを移したことでその状態が、誰も
終わらせていないRuntimeへ移ったのだ。

見つかり方がもう半分である。1回だけの実行は通った — 2つのセッションで2度
通った — 1回だけ走らせるのがsmokeテストの自然な形だからだ。見つけるのに
要ったのは、同じプログラムを同じインスタンスで**2回目**に走らせることで、
それはページがやっていて、ハーネスがやっていなかったことである。

そこでこのバッチは、それをレーンにして終わる。`just check-playground`
（`tools/playground/smoke.mjs`）はcommit済みのwasmをnodeで読み込み、4つの
ケースを1つのインスタンスで2回ずつ走らせ、その出力をネイティブのexecutorで
走らせた同じプログラムに突き合わせる。emsdkを必要としない — そこが要点で、
検査対象はPagesが配る`site/playground/`の成果物であって、自分でビルドした
ものではない。だからCIで、pushのたびに、`release-diff`の隣で、同じく
ダウンロードしたバイナリの上で走る。着地前に両方向を発火させた: 実行ごとの
`Runtime`をコメントアウトすると`lazy_stdlib.cul run 2: wasm trapped: memory
access out of bounds`と報告して1で抜け、残る3ケースは2回とも通る — 全面的な
失敗ではなくバグの形そのものである。`full`は手が届かない（JSPIビルドは
`WebAssembly.Suspending`無しにはインスタンス化すらできない）が、2つのビルドは
同じtranslation unitなので、エンジンについての問いはどちらでも答えられる。

### 13.7 ユニットテストランナー

§13.1が見つけた5サイトのうち4つには既にVMレーンがあった。スクリプト実行・
REPL・`dap`・doctestランナーはPhase 2でそれぞれ手に入れている。5つ目には
無かった。`culebra test`のユニットランナーはC++のレジストリに`Value`を持ち、
インタプリタの`call`ヘルパー経由でそれを呼び、fixtureを`Environment`から
解決していた — そしてそう明言して`--vm`を拒否していた。ここを動かさずに
デフォルトを切り替えれば、ユーザ自身のスイートを走らせる唯一のサブコマンドが、
退役するエンジンの上に取り残される。

エンジンのseamは再び`debug_engine.h`のもの — エンジンごとに1実装のinterfaceで、
その上は全部共有。小さく収まった理由はseamの**下**に置いたものにある。
`test`と`parametrize`は`FunctionValue`を組む90行のC++だったが、いまは
`src/preambles/test_ambient.cul`という両エンジンが走らせる1つのculebraソースで、
そこが埋めるレジストリはただのculebra Arrayである。これにより、さもなければ
ランナーがエンジンに訊かねばならなかった部分がプログラム自身の側へ移る:
テストがfixtureを受け取るパラメータはあちらの`f.params.filter(...)`であり、
`@parametrize`のケースを引数リストへ展開するのもあちらだ。ホストに残るのは
プログラムを走らせる・グローバルを読む・ArrayやObjectを歩く・関数を呼ぶ —
9つのメソッドで、そのどれも2つの値モデルを出会わせない。値はホスト自身の
ストアへのインデックスとして渡り、使ったテストが終わるときにマークまで
解放される。fixtureの`drop`が正しいテストの捕捉出力へ流れ込むのはそのためだ。

VMホストは各ファイルを、REPLが1行をコンパイルするのと同じ形 — セッション
単位 — でコンパイルする。トップレベル束縛が、それを走らせたフレームではなく
`vm::ReplSession`のセルに載るようにするためである。これがそもそも成立する
理由のすべてだ: ランナーはファイルが**戻ったあとで**そこへ呼び戻すので、
登録済みのクロージャは呼べたままでなければならず、fixtureは引ける名前の
ままでなければならない。これは回避策ではなく再利用である: その能力には既に
名前があり、デバッガが既に2人目の消費者だった。無かったのは**住所**のほうで
— REPLが所有していた — なので、保持されるプログラムと、遅延namespaceの
builderが2度登録されるのを防ぐトークン集合と、ユニット実行そのものは、いま
`vm_session.h`にあり、REPLとテストホストがそれぞれ1つずつ持つ`vm::Session`が
抱える。プロセス共有のセッションを借りるのではなく所有することが、1プロセス
内の2回目のrunが1回目のレジストリを見ないことを保証する。これは削除された
C++レジストリが自分をクリアすることで与えていた保証である。

3つのバグが出た。最初の2つは、2つのエンジンの装いをまとった同じバグである:
**何が値を生かしているか**。インタプリタ側のambientは
`TEST_AMBIENT_MODULE_SOURCE`（`const char*`）からパースしていた — これは
呼び出しの終わりで死ぬ一時`std::string`を作るので、ASTのトークンはすべて
解放済みメモリへのviewになり、`test`は`undefined variable '`とそこに残って
いた何かでできた名前を報告した。VMホストのストアは、既に参照を渡してきた
呼び出しの結果をもう一度retainしていたので、fixtureの参照カウントが0に届かず
`drop`が走らなかった。どちらも§13.6がPlaygroundで出会った形である:
**エンジンを移すと値を所有するものが移り、前の所有者の保証は付いてこない**。

3つ目はコンパイラにあり、このバッチが原因ではないので独立して記す価値がある。

**セッションの名前を1つの関数の2つの分岐から参照するとVMがクラッシュした。**
セッション束縛のセルを保持するスロットは`ReplCell`命令が埋め、コンパイラは
命令をソース順に吐く — つまりその`ReplCell`は名前が**最初に言及された**場所、
すなわち`if`の腕の中に落ちうる。あとの言及は束縛を再利用し（それは正しい:
2つは同じ名前を意味する）、したがって同じスロットを、何も再発行せずに使う。
最初の腕を飛ばして2つ目の腕の`CellGet`が誰も埋めていないスロットを読むように
呼べばよい:

```culebra
let helper = fn () { 7 }
let pick = fn (n) {
  if n == 2 { return helper() }
  if n == 1 { return helper() + 1 }   # segfault: `helper`のスロットが空
  0
}
```

これはmasterの`culebra --vm`のREPLであり、このバッチのどの部分も関与しない —
そして到達がはるかに容易になろうとしていた。テストホストの下では、テスト
ファイルの**あらゆる**トップレベル束縛がセッション束縛だからだ。修正は
`ReplCell`を巻き上げようとするのではなく使用箇所ごとに再発行する: 束縛は
設計上スコープ全体のもので、スコープ全体ではないのは命令のほうであり、
再発行が冪等なのはセッションが所有する唯一のセルを返すからだ。回帰ケースは
`tests/repl_test.sh`にあり、修正前のバイナリをsegfaultさせ、修正後は通る。

`culebra test`はいま`--vm`で走る。デフォルトは`--tree`のままで、ゲートは
2つのセルフスイートと2つのreporterにわたって両者をバイト一致に縛る。
`--jit`は拒否のまま。REPLとデバッガが拒否するのと同じ理由である: テストの
本体はホットループではなく、その全部をコンパイルしても手に入るのは
レイテンシだけだ。

### 13.8 デフォルトを動かす

いまや全サイトにbytecode VMのレーンがあるので、切り替えそのものは1行だ。
`parse_command_line`がコマンドラインがエンジンを名指したかを記録し、名指して
いなければexecutorを設定する。以後の読み手はすべて、エンジンを名指した
`Options`を見ることになり、どのサイトも既定をもう一度綴らずに済む。
`culebra test`・`culebra dap`・REPLはそれぞれ自前のパーサを持つので、同じ2行を
それぞれに足した。注意が要ったのは**順序**である: ratchetの
`require_explicit_engine`はエンジン分岐の**前**に訊かねばならない。さもないと
既定の実行がVMレーンへ入って、この検査が存在する理由そのものより先に戻って
しまう。

§13が挙げたレイテンシの問いには、**何を測るかで完全に変わる**答えがある。
両方書き残す価値がある。遅延stdlibモジュールをいくつか名指してあとは何もしない
スクリプトでは、VMは**7ms遅い**（35→42ms）。startup profileが行き先を教えるが、
それはPhase 0のspikeが推測した場所ではない: preambleの**パース**は両レーンで
ほぼ同額で（tree-walkerも払っている。各モジュールが最初に使われるところで
`interpret_modules`の中に散らばっているだけだ）、executorの超過分はその
preambleを**bytecodeにコンパイルする**分である。何も名指さないスクリプトでは
両者は2%以内。

実プログラムでは符号が逆転する。`tests/test_object_keys.cul`はexecutorで
**42%速く**、`test_core.cul`はわずかに速く、残りは互角。これはPhase 3の
2〜4.5倍のループ高速化が、固定の7msをほぼ即座に取り返しているということであり、
preamble bytecodeのblob（`grammar_blob.h`の形。§13が緩和策として名指した）を
**作らない**理由でもある。それが買い戻すのは、何も仕事をしないスクリプトだけが
感じられるコストだからだ。

切り替えが1つ失ったのは**可視性**である。ゲートのどのレーンもエンジンを名指す
— B1のratchetが強制しているのがそれだ — ので、既定が動いたあと、そのどれ一つ
として動かなかった場合に気づけない。`tests/cli_input_test.sh`がいまそれを検査
する場所であり、木の中で唯一`CULEBRA_REQUIRE_EXPLICIT_ENGINE`**なし**で
バイナリを走らせねばならない場所だ（`release_diff.sh`と同じやり方で外す）。
観測手段はstartup profile: `interpret_modules`のマークはtree-walkerの経路にしか
無いので、その不在がexecutorの実行を告げる。終了コードも併せてassertする。
abortもまたマークを出さないので、さもなければ素通りしてしまうからだ。

### 13.9 コレクタがwasmローカルに出会う

v0.3.0公開の2日後、PlaygroundのRocci Birdが1万フレーム目あたりで
`RuntimeError: memory access out of bounds`で死んだ。クラッシュはCanvasを
含まない8行 — フレームごとの`map(spread).filter().size()`パイプライン — に
還元でき、原因は1行で言える: `CULEBRA_GC_NEVER=1`なら同じ実行が完走する。
§13.6のsmokeはこのすぐ隣に立っていて、見えなかった: 2万個のクロージャは
コレクタの出番より先にrefcountで死ぬ一方、壊れるのはコレクタの*sweep*、
それもmark段が一度も見つけなかった値に対するsweepだからだ。

mark段が見つけられなかったのは、wasmでは保守的スキャンの中心的な仮定が
偽だからである。`scan_roots`は`setjmp`でcallee-savedレジスタを吐き出させて
からマシンスタックを歩く。だがwasmローカル — 値が実際に住むレジスタファイル —
は線形メモリの外にあり、`setjmp`はそこへ何も書き出さない。§13.4は
`Heap::stack_base`に`__EMSCRIPTEN__`アームを生やしたとき、この事実の半分を
既に書き留めていた: 線形メモリ側のスタックが持つのは「アドレスを取られた
ローカル…保守的スキャンがそもそも見つけ得た唯一のローカル」だけである。
`run_frame`のフレーム窓はVLAなので*見える*。見えないのは、唯一の参照が
wasmローカルに座っている値すべて — `num_add`が作ったばかりでまだ格納して
いない文字列や、mapのクロージャが走っている間の`iter_collect`内の作りかけ
配列だ。そうしたヘルパ内のallocation siteで閾値collectが発火し、sweepが
そのオブジェクトを解放し、解放済みブロックがslabのfree-listを腐らせる —
trapが後になって`SlabAllocator::alloc`の中で、再利用が与えた別の顔で
現れるのはそのためだ。tree-walkerはこれに出会わなかった: あちらの文字列は
refcountされており（`GC.stat`の`live == rc`がそれを示す）、コレクタが
文字列の寿命を決めることがない。エンジン切替こそが、スキャナの支えられない
ビルドへtraced-only表現を持ち込んだ当のものだった。

修正は「違法だった場所に当て物をする」のでなく「コレクションが合法な場所を
定義する」。`kDeferToSafepoint`（jit_gc.h、`__EMSCRIPTEN__`でのみon）の下では
インラインのcollectは一切走らない: 閾値超過はpendingフラグを立てるだけで、
executorが命令境界で`safepoint_collect()`をポーリングする。そこでは全フレームの
全live値が、スキャンから見えるレジスタ窓に載っている。移設後も1つだけ危険が
残る — 2つのVMフレームの*間*で中断しているヘルパ（コールバックが走っている
最中のiterator op）は、唯一の参照を自分のローカルに握っているかもしれない。
これを覆う不変条件は: **そのようなフレームがスタック上にある間はコレクション
しない。** 強制はヘルパごとの監査でなく1つのチョークポイントで行う: ヘルパから
ユーザコードへの呼び出しはすべて`_jit_invoke`（jit_value.h）を通り、その
`SafepointUnsafeScope`が呼び出しの間ポーリングを遅延させる。callee・receiver・
引数を呼び出しの間ずっとレジスタ窓に置くと監査済みのdispatchアーム — `Call`、
`CallM`、builtin gateのユーザメソッド引き渡し — は`_jit_invoke_rooted`を使って
collectableのままでいる。これが`Canvas.run`のゲームループを毎tick収集可能に
保っているものだ。マーキングは構造的にfail-safeである: 監査されないまま増えた
新しい呼び出し経路は、次のeligible pollまでコレクションを遅らせるだけ —
メモリは伸びるが、誤って解放されるものはない — そして閾値1が「eligible poll
ごとに収集」を意味するようになった`CULEBRA_GC_STRESS`が、まさにこの
プロトコルを叩く。`tools/playground/cases/pipeline_churn.cul`が縮約
パイプラインを`check-playground`で走らせる（あそこの他の全ケースと同じく
インスタンスごとに2回）ので、この種のバグを一度見逃したレーンが、いまはCIで
これに引っかかる。ネイティブビルドではプロトコル全体が畳まれて以前の挙動に
戻る。明示的なcollect（`GC.stat`）もwasmでは同じゲートを通るので、ヘルパの
下ではsweepでなくdeferになる。

### 13.10 削除

7つのバッチを、§3から借りた1つの原則で並べた: 「両エンジンが生きている間に
しかできない検証」を、その条件を終わらせる削除より前に置く。埋め込みの
切替までは可逆で、`--tree`はそれを取り除くバッチの直前まで正しく答え続けていた。

計画のインベントリはtree-walkerがまだ担いでいた3つの荷を見落としており、
仕事の大半はファイルの削除でなくその発見だった。第一に、**署名が
インタプリタのものだった。** どのコンパイル済みレーンも「`Math.pow`の
パラメータは何か」に、interpのstdlib環境を実行時に丸ごと組み立てて
`FunctionValue`から読むことで答えていた — AOTアーカイブの`culebra::`
シンボル7,322個のうち1,601個がその機構だった。置き換えは生成表
（`canon_sigs.gen.h`）で、両者が共存する間に走って置き換え対象の環境との
1:1一致をassertするツールが emit した。生成器はエンジンと共に退役し、
以後この表が、署名の編集が手で保守する正典である。第二に、**コンパイラの
includeグラフが、自分が見送るはずのエンジンを通っていた**: `vm.h`は
推移的に`interpreter.h`へ届いていたので、B7-bがキャリアヘッダ（カーネル、
preamble機構、isolateコア、送信ツリー）を分割し、`vm.h`単独のTUがinterp
ヘッダ0でコンパイルできるまで切り、ratchetがその切断を保持した。第三に、
**オラクル**: vm_casesの基準線、leak-fuzzの参照レーン、コーパス生成器は
すべて`--tree`にpinされていた。それぞれを、tree-walkerがまだ引き渡す
goldenに副署できるうちに張り替えた（§13.5のrelease-diffが、それより
長生きする独立の第二意見である）。

埋め込みAPIは可逆バッチの最後に移した: `vm::Embed`（deployment.md §2）が
`environment()` / `interpret` / `call` / `define`を置き換え、smokeは切替を
はさんで契約を保った。そして不可逆の2バッチ: B7-eが5つのエンジンサイトと
フラグを削除し — 以後`--tree`は未知のオプションで、`--version`は`vm+jit`と
名乗る — B7-fが本体を削除した: `interpreter.h`、`stdlib_interp.h`の残り、
interpのREPLとデバッガ、`wrap.h`とisolate/sendable層のinterp半分、そして
比較相手を失ったdrift検査群である。

最後のバッチから2つ、書き残す価値のある細部がある。未定義変数lintの
builtin名集合はinterp環境から読まれていた。いまはコンパイラが名前解決に
使うのと同じ述語（`builtin_var_names` + ns表 + lazyグループ）から実体化
されるので、lintとエンジンが「裸の名前が何を意味するか」で食い違うことは
できない。そして削除後の最初のコンパイルはLLVMのヘッダの中で失敗した:
`termios.h`は`CR1`をマクロとして定義し、`term.h`はそれをずっと漏らして
いて、LLVMを守っていたのはinterp REPL経由で先に届くlinenoiseの`#undef`
ブロックだった — それはREPLと一緒に去った。`#undef`はいま、includeする
その場所に住んでいる。

請求書を、B7-fの前後で同じマシンで測った: AOTランタイムアーカイブは
半減（20.2 → 10.3 MB）、ドライバのバイナリは6 MB減（84.8 → 78.7 MB）、
アーカイブの定義済み`culebra::`シンボルは6,433から1,472へ — interp環境
機構の取り分はちょうどゼロで、インベントリの1,601シンボルという見積りが
削除の価値として予告していたとおりだった。

削除が変えなかったものが、このフェーズ全体の要点である:
`tools/difftest/release_diff.sh`はこのツリーを最後のリリースバイナリ —
両エンジンを持つ最後の版であるv0.3.1 — と比較し、削除バッチは
そのallowlistが空のまま着地した。
