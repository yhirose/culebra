# conformance: specを何が固定しているか

`docs/language.ja.md`と`docs/stdlib.ja.md`は言語が何であるかを述べます。
本書はその裏側 — それらに対して何が走り、規則がどこでも実行されなく
なったときに何が落ちるか — です。spec本体でなくここに置いているのは、
バイナリだけを持つ読者にはこれらのファイルが1つも存在しないためです。

## 1. 章ごとのテストファイル

言語specの各セクションには対応するテストファイルが`tests/`に
あります。`just test`でVM/JIT差分・AOT差分・埋め込みC++
smokeを1度に回します。AOT差分のみなら`just test aot`。下表は主要な担当テストを示しますが、複数セクションに跨がるものは"(broad)"と表記します。

| テストファイル | 検証するspecセクション |
|---|---|
| `tests/test_core.cul` | §6, §7, §8, §9, §10, §11, §12, §15, §18, §19 (broad — 主要unit-testまとめ) |
| `tests/test_class.cul` | §10 (class構文、演算子オーバーロード、`__str__`、auto-reflection、static methods)、§11 |
| `tests/test_class_parameters.cul` | §10 (自動合成`parameters()`) |
| `tests/test_decorator.cul` | §21 |
| `tests/test_defer.cul` | §15 (`defer`、scope-guardパターン) |
| `tests/test_forward_ref.cul` | §6 (スコープ)、§11 (closure)、§20 |
| `tests/test_iter.cul` | §12 (`for ... in`)、§18 (iterator protocol、Stringメソッド)、§19 (`range`、`iota`) |
| `tests/test_iter_combinators.cul` | §18 (lazy combinator各種、無限ソースのlaziness) |
| `tests/test_iter_combinators_group.cul` | §18 (`min_by`/`max_by`、`to_set`/`to_object`/`group_by`/`partition`) |
| `tests/test_iter_combinators_stream.cul` | §18 (`unzip`/`flatten`/`scan`/`distinct`/`tap`/`step_by`/`chunk_by`、無限ソースのlaziness) |
| `tests/test_iter_terminal.cul` | §18 (terminal iteratorメソッド、§18.5 protocol contract) |
| `tests/test_kwargs.cul` | §11 (キーワード引数、`**` splat)、§20 (kwargs in多重dispatch)、§7 (mixed callの評価順) |
| `tests/test_match_class.cul` | §13 (型パターン) |
| `tests/test_type_pattern.cul` | §13 (パターンとしての裸の型名、宣言するパターンはすべての名前を束縛) |
| `tests/test_multidispatch.cul` | §20 |
| `tests/test_object_keys.cul` | §10 (非Stringキー) |
| `tests/test_runtime_errors.cul` | §15 (`throw`/`try`/`catch`、すべての`kind`のcatch可能性) |
| `tests/test_catch_pattern.cul` | §15 (`catch`節: パターン、ガード、順序、どの節も受けない値) |
| `tests/test_error_kind_types.cul` | §15 (型名としてのkind: 節、アーム、型注釈、ディスパッチ) |
| `tests/test_set.cul` | §10 (Set) |
| `tests/test_tuple.cul` | §10 (Tuple、destructuring) |
| `tests/test_ufcs.cul` | §10 (メソッド、UFCS)、§19 (`__ARGS__`) |
| `tests/test_ufcs_first_param.cul` | §10 (UFCS: 第一引数が決める) |
| `tests/test_args.cul` | stdlib §10 (`Args`) |
| `tests/test_fs.cul` | stdlib §3 (`FS`) |
| `tests/test_json.cul` | stdlib §9 (`JSON`) |
| `tests/test_tensor.cul` | stdlib §8 (`Tensor`) |
| `tests/test_tensor_nn.cul` | stdlib §8 (`Tensor`) |
| `tests/test_tensor_nn_xent.cul` | stdlib §8 (`Tensor`) |
| `tests/test_tensor_nn_ops.cul` | stdlib §8 (`Tensor`) |
| `tests/test_tensor_nn_graph.cul` | stdlib §8 (`Tensor`) |
| `tests/test_tensor_ops.cul` | stdlib §8 (`Tensor`) |
| `tests/test_tensor_ops_rope.cul` | stdlib §8 (`Tensor`) |
| `tests/test_time.cul` | stdlib §5 (`Time`) |
| `tests/test_import.cul` | §24 (モジュール) — `tests/test_import_helpers/*.cul`が依存先 |

`tests/`配下のすべてのテストファイルは両backendで同一stdout
を出すことが要求されます — `just test`がそれを強制します。
backend固有の対話的機能（REPL stateを駆動するデバッガhook等）
は`.cul`スクリプトではなく`tests/embedding/`のC++ smoke
テストで検証します。

モジュールの章を受け持つのは1ファイルとその補助モジュールです。

`tests/test_import.cul`が両backendで正常系（基本的なimport、
関数とクラスの混在export、複数の`export`文、importの連鎖）を
検査します。補助モジュールは`tests/test_import_helpers/`配下に
あります。エラー系（循環import、トップレベル以外での使用、重複
export）は同じファイル内のインライン`try { ... } catch { ... }`で
カバーし、失敗するソース自体も別の補助モジュールとして置いています。

## 2. ratchet

上の表は手で維持するもので、章を指しています。より細かい問い2つは
ratchetが持ちます。

「個々の節が述べる規則を、何も実行していないのではないか」という問いは、`tools/checks/check_spec_examples.sh`が受け持ちます。このスクリプトは、`docs/language.md`の節のうち、実行される
` ```culebra `ブロック（`# doctest: skip`が付いていないもの。
残りは`just doctest`が両エンジンで走らせます）を持たない節を数え、
`tools/checks/spec_unpinned_sections.txt`と突き合わせます。例を失った節も、
最初から持たない新しい節も検査に落ちます。逆に、リストにある節が
例を得た場合も落ちるので、このファイルは減る方向にしか動きません。

stdlibリファレンスが文書化する個々の`Ns.fn`やgrammar keywordに
実際の呼び出し元があるか、は`tools/checks/check_api_coverage.sh`が
`tools/checks/api_surface.txt` — quick-guideの索引と同じ生成パスが
書き出す、リファレンスが文書化している署名の全体 — とPEG grammarから
抜き出したkeyword集合を読み、それぞれの名前を`tests/*.cul`・
`tests/*.sh`・`just doctest`が走らせるdoctestブロックと突き合わせます。
呼び出し元がどこにも無い文書化済みの名前は`tools/checks/api_untested.txt`
に記載されていない限り検査に落ち、記載済みの名前が呼び出し元を
得た場合も落ちるので、このファイルも減る方向にしか動きません。
母集団に`docs/quick-guide.md`の索引を使っていないのは意図的です。
あちらは凝縮パックが本文に並べる名前空間しか載せないので、そこから
読んでいたら、ある名前空間を「名前だけ」に移した瞬間にこの検査の
母集団が半分に落ちていました。

どちらも`check-generated`の一部として走るため、`just test-dev`と
CIの両方がこれを回します。

## 3. tier: どの検査をどこで走らせるか

上のどの検査も、費用は「母集団×1件あたりの単価」で決まる。エンジンの検査を載せられるレーンは2つあるが、その単価は大きく違う。`tests/`のファイル（327本。
`gate_budget.txt`に記録）は、executorよりも`--jit`のほうがはるかに
高くつく。JITレーンが払っているのは実行ではなくLLVMだからで、
重いファイル1本はIRパイプラインとバックエンドにそれぞれ数秒、実行に
数ミリ秒かかる。つまりコーパス全体に及ぶ軸をJITレーンに適用すると、それだけでここにある他の検査をすべて合わせたのと同じくらいの費用がかかる。だから「どこで走らせるか」は、その検査の性格の一部である。

justfileの`gate_rows`が、この配置を1か所で宣言している。1フェーズが1行で、必要なもの(checkout・バイナリ・ビルドツリー)、走らせるローカルのtier、走らせるCIシャード、実測コストを書く。
`just check`・`just test-dev`・`just test`・各CIシャードはすべてこの表
へのフィルタなので、新しいフェーズは各所に貼り付けられるのではなく
自分で宣言することで検査に入る。`gate_table_selftest`が、シャードを
1つも名乗らないフェーズ、バイナリだけのシャードに乗ったビルドツリー
依存フェーズ、そして何も選ばないレーンを拒否する。

| tier | 答える問い | 中身 |
|---|---|---|
| `just check` | この編集は既にツリーが主張していることを壊したか | source/IR ratchetと、すべてのassertionをexecutorで(20ファイルごとに1プロセス) |
| `just test-dev` | これをmasterに入れてよいか(`just land`はこれだけを回す) | 上記＋op coverに対するJITレーン、`-O0`とfaststartのcodegen軸、`ctest`のCLI半分、言語フロントエンド、isolate |
| `just test` | これをpushしてよいか | 全コーパス×全軸、AOT含む |
| CI | 両OSと、ラップトップから見えないもの | 全掃き、refcount/leakレーン、プラットフォーム・ウィンドウビルド |

着地前の検査が何を持つかは「安いから」ではなく「実際にすり抜けた場所」で
決めてある。CIの`ctest`エントリ(escape 2件: `jit_error_pos_test`と
`search_model_test`)と`-O0`/faststartのcodegen軸(既定の`-O2`では見え
なかったバグ3件)は中に入れ、コーパスをGC設定やrefcount設定で二度
掃き直す軸——検出力がコーパスではなく軸に属するもの——は`just test`と
CIで一度だけ走らせる。

### JITレーンの部分集合

`tools/checks/jit_shape_set.txt`が着地前の検査の`--jit`脚が掃く対象で、
executorが実装する全bytecode opをJITが降ろすことを保つ最小のテスト
ファイル集合である。`tools/checks/codegen_sensitive.txt`が名指しする
ファイルを必須の種にし、ブランチが触ったテストファイルを足す。31
ファイルで155 opのうち152を覆う。届かない
3つは理由付きでこの集合の中に記載してあり、その一覧は減る方向にしか
動かせない。`tools/checks/check_jit_shape_set.sh`が検査を回すたびに
コンパイル済みbytecodeから被覆を再計算するので、新しいopは誰かが
テストで届かせるまで検査を失敗させる。

このcoverが持っていないのはopの**組み合わせ**——ループの中の
クロージャの中のunwindエッジ——である。それはcodegenの種が手で運び、
生成コーパス(`tools/difftest`、両レーンで約21,000ケース)が構成的に覆う。
だから全掃きは`just test`とCIの`ci-gc`で毎push走る。

CIのmacOSジョブも全掃きの代わりにこのcoverを掃く。全掃きは3コアで
5分かかり、そのほとんどがLLVMである。したがって全掃きが走るのは
x86-64のLinuxだけで、arm64のバックエンドとcover外のファイルの両方が
揃って初めて出る不具合は、pushではなくMacで`just test`を回したときに
見つかる。

### 検査自身のコスト

`tools/checks/gate_budget.txt`が掃く母集団——コーパス、isolateファイル、
generatorのプローブ、`vm_cases`、ctestエントリ、docブロック、
言語サンプル——を記録し、
`check-gate-budget`がその数を厳密に保つ。増えるのは普通のことで、
増やすコミットの中でここを更新するので、遅くなった検査は時間の
行き先を示す。秒数をratchetにしないのは意図的
で、同じレーンが負荷のかかったマシンでは1.5〜2倍ぶれる。表のフェーズ別
コストはレーン終了時に実測と並べて表示するだけで、何も落とさない。

1つのレーンは1つのファイルを1度だけコンパイルする。コレクタの設定を
変えてコーパスをもう一度`--jit`に通すフェーズ——gc-stressの2本の脚と
`vm_cases`の3モード——は、それをコンパイルした掃きと1つのJIT object
cacheを共有する(justfileの`jit_cache`)。
繰り返されるのは実行であってLLVMではない。コレクタの設定が変えるのは
コードの走り方であって、コードそのものではないからである。gc-stressの
CPUの87%はこのコンパイルだった。vm/jitの全掃きとgc-stressが同じCI
シャードにあるのはこのためである。

このcacheは実行をまたいで残す——ユーザーのcacheディレクトリの下の
`culebra/gate-jit`で、すべてのworktreeが共有する——ので、後の実行は
前の実行がコンパイルしたところから始まる。objectは最適化後のモジュールと
バックエンドでキーづけられている([vm.ja.md](vm.ja.md) §7)ので、どちらも
変えない再ビルドはそれを見つける。飛ぶのはバックエンドで、コーパスの
コンパイルの53%にあたる。IRパイプラインはキーを得るために毎回走る。
`CULEBRA_GATE_JIT_CACHE`で別のディレクトリを指定でき、`off`ならその実行
だけのディレクトリになる。検査が書き込めないディレクトリは1行で
知らせたうえで、同じくその実行だけのものに切り替える。cacheを残すと、次の2つが決まる。1つ目は、vm/jit掃きのファイル別コンパイル予算がコンパイル全体を測るのは、そのモジュールを初めて見たときだけになることである。2つ目は、objectが作った実行より長く残るので、やり直しても説明のつかないJITの失敗は、原因を決めつける前に`off`でもう一度回すことである。

### 免除は、免除だと書かせる

走らない検査は落ちない検査である。このツリーが被覆を失った経路のうち2つは、
誰も見ていない免除だった。

**`# doctest: skip`。** skipされたブロックは一度も実行されないので、文書に
書かれた形が動かなくなっても文書には残り続ける。skipが正当と認められるのは次の2通りだけである。そのブロックがそもそも走れないか、なぜここで走らせないかが書いてあるか（`# doctest: skip — ウィンドウを開く`）である。`check_doctest_skips.sh`は、理由のないskipをexecutorで走らせる。きれいに走ったブロックは検査を失敗させるので、skipを外すか、読者が知りたい理由を書く。
ウィンドウ・サーバ・監視・stdin・ネットワーク・外部プログラム・マシン固有の
パスが要るブロックは理由を持ち、単に走る例示にskipは付かない。

skipにはあるマシンで成立して別のマシンで成立しないものがあり、これがこのレーンを
全プラットフォームで走らせる理由でもある——あるマシンで走れない例が、別のマシンでは走る。
`Net.listen(7000)`とそのacceptループはランナーではハングし、このラップトップでは
失敗した。`Proc.spawn(["python", ...])`は`python`がPATHにある環境では走り、
`python3`しかない環境では失敗する。**手元のマシンで通るskipは、正当なskipとは限らない。**

**`examples/`。** `examples/`配下のスイート（47本。`gate_budget.txt`に記録）は
全レーンで走る。`run_examples_sweep`がこれをexecutor1プロセスで回し、
`tests/*.cul`と同じoptional-namespaceの規則も適用する。
