# conformance: specを何が固定しているか

`docs/language.ja.md`と`docs/stdlib.ja.md`は言語が何であるかを述べます。
本書はその裏側 — それらに対して何が走り、規則がどこでも実行されなく
なったときに何が落ちるか — です。spec本体でなくここに置いているのは、
バイナリだけを持つ読者にはこれらのファイルが1つも存在しないためです。

## 1. 章ごとのテストファイル

言語specの各セクションには対応するテストファイルが`tests/`に
あります。`just test`でVM/JIT差分・AOT差分・埋め込みC++
smokeを1度に回します。AOT差分のみなら`just test aot`。下表
は主要オーナを示しますが、複数セクションに跨がるものは
"(broad)" と表記します。

| テストファイル | 検証するspecセクション |
|---|---|
| `tests/test_core.cul` | §6, §7, §8, §9, §10, §11, §12, §15, §18, §19 (broad — 主要unit-testまとめ) |
| `tests/test_class.cul` | §10 (class構文、演算子オーバーロード、`__str__`、auto-reflection、static methods)、§11 |
| `tests/test_class_parameters.cul` | §10 (自動合成`parameters()`) |
| `tests/test_decorator.cul` | §21 |
| `tests/test_defer.cul` | §15 (`defer`、scope-guardパターン) |
| `tests/test_forward_ref.cul` | §6 (スコープ)、§11 (closure)、§20 |
| `tests/test_iter.cul` | §12 (`for ... in`)、§18 (iterator protocol、Stringメソッド)、§19 (`range`、`iota`) |
| `tests/test_iter_combinators.cul` | §18 (lazy combinator群、無限ソースのlaziness) |
| `tests/test_iter_combinators_group.cul` | §18 (`min_by`/`max_by`、`to_set`/`to_object`/`group_by`/`partition`) |
| `tests/test_iter_combinators_stream.cul` | §18 (`unzip`/`flatten`/`scan`/`distinct`/`tap`/`step_by`/`chunk_by`、無限ソースのlaziness) |
| `tests/test_iter_terminal.cul` | §18 (terminal iteratorメソッド、§18.5 protocol contract) |
| `tests/test_kwargs.cul` | §11 (キーワード引数、`**` splat)、§20 (kwargs in多重dispatch)、§7 (mixed callの評価順) |
| `tests/test_match_class.cul` | §13 (型パターン) |
| `tests/test_multidispatch.cul` | §20 |
| `tests/test_object_keys.cul` | §10 (非Stringキー) |
| `tests/test_runtime_errors.cul` | §15 (`throw`/`try`/`catch`、すべての`kind`のcatch可能性) |
| `tests/test_set.cul` | §10 (Set) |
| `tests/test_tuple.cul` | §10 (Tuple、destructuring) |
| `tests/test_ufcs.cul` | §10 (メソッド、UFCS)、§19 (`__ARGS__`) |
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

個々の節が述べる規則を何も実行していないのではないか、は
`tools/checks/check_spec_examples.sh`が、実行される
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

上のどの検査も「母集団×1件あたりの単価」で値段が決まり、エンジン検査が
乗れる2つのレーンの単価は近くない。`tests/`の279ファイルは executor で
17 CPU秒、`--jit`で610 CPU秒かかる。JITレーンが払っているのは実行では
なく LLVMだからで、重いファイル1本の内訳は IRパイプライン8.5秒・
バックエンド13.5秒・実行3ミリ秒である。つまりコーパス全体にある軸を
JITレーンで適用すると、それだけでここの他の全部と同じくらいの値段に
なる。だから「どこで走るか」はその検査が何であるかの一部である。

justfileの`gate_rows`がその配置の単一の宣言になっている。1フェーズ1行
で、何を必要とするか(checkout・バイナリ・ビルドツリー)、どのローカル
tierが走らせるか、どの CIシャードが走らせるか、実測コストを書く。
`just check`・`just test-dev`・`just test`・各 CIシャードはすべてこの表
へのフィルタなので、新しいフェーズは各所に貼り付けられるのではなく
自分で宣言することでゲートに入る。`gate_table_selftest`が、シャードを
1つも名乗らないフェーズ、バイナリだけのシャードに乗ったビルドツリー
依存フェーズ、そして何も選ばないレーンを拒否する。

| tier | 答える問い | 中身 |
|---|---|---|
| `just check` | この編集は既にツリーが主張していることを壊したか | source/IR ratchetと、8,539個の assertionを executor 1プロセスで |
| `just test-dev` | これを masterに入れてよいか(`just land`はこれだけを回す) | 上記＋op coverに対する JITレーン、`vm_cases`の凍結出力、`-O0`と faststartの codegen軸、`ctest`の CLI半分、言語フロントエンド、isolate |
| `just test` | これを pushしてよいか | 全コーパス×全軸、AOT含む |
| CI | 両 OSと、ラップトップから見えないもの | 全掃き、refcount/leakレーン、プラットフォーム・ウィンドウビルド |

着地ゲートが何を持つかは「安いから」ではなく「実際にすり抜けた場所」で
決めてある。CIの`ctest`エントリ(escape 2件: `jit_error_pos_test`と
`search_model_test`)と`-O0`/faststartの codegen軸(既定の`-O2`では見え
なかったバグ3件)は中に入れ、コーパスを GC設定や refcount設定で二度
掃き直す軸——検出力がコーパスではなく軸に属するもの——は`just test`と
CIで一度だけ走らせる。

### JITレーンの部分集合

`tools/checks/jit_shape_set.txt`が着地ゲートの`--jit`脚が掃く対象で、
executorが実装する全 bytecode opを JITが降ろすことを保つ最小のテスト
ファイル集合である。`tools/checks/codegen_sensitive.txt`が名指しする
ファイルを必須の種にし、ブランチが触ったテストファイルを足す。34
ファイルで151 opのうち147を覆い、610 CPU秒のうち71秒で済む。届かない
4つは理由付きでこの集合の中に記載してあり、その一覧は減る方向にしか
動かせない。`tools/checks/check_jit_shape_set.sh`がゲートを回すたびに
コンパイル済み bytecodeから被覆を再計算するので、新しい opは誰かが
テストで届かせるまでゲートを落とす。

この cover が持っていないのは opの**組み合わせ**——ループの中の
クロージャの中の unwindエッジ——である。それは codegenの種が手で運び、
生成コーパス(`tools/difftest`、両レーンで約17,000ケース)が構成的に覆う。
だから279ファイルの全掃きは`just test`と CIの`ci-light`で毎 push走る。

### ゲート自身のコスト

`tools/checks/gate_budget.txt`が掃く母集団——コーパス、isolateファイル、
`vm_cases`、ctestエントリ、docブロック、言語サンプル——を記録し、
`check-gate-budget`がその数を厳密に保つ。増えるのは普通のことで、
増やすコミットの中でここを更新する。コーパスは6週間で203本から279本に
増え、それは JIT掃きの610 CPU秒のうち174秒に相当するが、ゲートが遅く
感じられるまで誰もそれを言わなかった。秒数を ratchetにしないのは意図的
で、同じレーンが負荷のかかったマシンでは1.5〜2倍ぶれる。表のフェーズ別
コストはレーン終了時に実測と並べて表示するだけで、何も落とさない。
