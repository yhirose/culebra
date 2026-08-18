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
| 生成difftest corpus | 17,262ケースをinterp / `--jit` / `--vm`で。差異0、**skip 0**（`tools/difftest/vm_skip_ceiling.txt`はratchetで現在0） |
| curated `tools/bench/vm_cases/` | 177ケース。`--vm`と`--vm-llvm`をそれぞれinterpと差分比較し、同じスイープを`CULEBRA_GC_STRESS=1`下でもう一度 |
| `tests/*.cul`の対称性とisolateスイート | 4レーン: interp、`--jit`、`--vm`、`--vm-llvm` |
| `just doctest` | ドキュメント465ブロックを3エンジン全部で（`just doctest LANE=interp\|vm\|jit\|all`） |
| `dap_test`（ctest） | デバッグアダプタのシナリオを両デバッグエンジンで |

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
`culebra --vm-llvm --emit-llvm f.cul | opt -passes=verify`が
lowering作業の常設チェックであり、codegenを変えてはならない
リファクタの常設チェックは`-O0 --emit-llvm`のIR差分である。

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

### 12.5 Phase 3に残っている借り

- **載せ替え以降まだ測っていない性能パリティ。** `--jit`は別の
  フロントエンドを通ってLLVMに届くようになった。§10の数字はもう
  一方のものである。
- `--vm-llvm`は今日では`--jit`と同じエンジンであり、レーンというより
  重複したフラグである。退役させるなら、これを名指ししているゲートを
  全部張り替えることになる。
- このフェーズのものではないがVM側にある既知の欠陥が1つ: 並行負荷の
  下で`tests/isolate/test_lazy_ns_parse_race_jit.cul`が`--vm`でのみ
  segfaultする — 10プロセス幅で40回中7回、interp・`--jit`・`--vm-llvm`は
  それぞれ40回中0回である。
