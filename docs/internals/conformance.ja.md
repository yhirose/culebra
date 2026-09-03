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
| `tests/test_tensor_ops.cul` | stdlib §8 (`Tensor`) |
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
`docs/quick-guide.md`に生成される署名索引とPEG grammarから
抜き出したkeyword集合を読み、それぞれの名前を`tests/*.cul`・
`tests/*.sh`・`just doctest`が走らせるdoctestブロックと突き合わせます。
呼び出し元がどこにも無い文書化済みの名前は`tools/checks/api_untested.txt`
に記載されていない限り検査に落ち、記載済みの名前が呼び出し元を
得た場合も落ちるので、このファイルも減る方向にしか動きません。

どちらも`check-generated`の一部として走るため、`just test-dev`と
CIの両方がこれを回します。
