# Regex stdlib API — 設計 (culebra 統合)

> **Status: Draft (2026-05-30).** vendoring した
> [cpp-regexlib](https://github.com/yhirose/cpp-regexlib) エンジン
> (`vendor/cpp-regexlib`) を culebra の stdlib namespace として公開する
> ための公開 API。エンジン内部とマッチングモデルはそのリポジトリ側の
> ドキュメントにある。本書は言語レベルの契約。
>
> 英語原本は [`regex_stdlib_api.md`](regex_stdlib_api.md)。両者は常に同期を保つこと。

## API の形

コンパイル済みの `Regex` は `Regex.compile` で一度だけ構築して再利用する
(高コストなのはコンパイル済みプログラムの部分)。これはネイティブの
`_Regex` プリミティブを包む culebra ソースのクラス — `_Time` / `Time` の
分割方式。

```culebra
# doctest: skip
let re = Regex.compile("\\d+")          // compile (reused); bad pattern -> error
let re = Regex.compile("\\w+", "i")     // flags: a string of "i" "m" "s"

re.test(s)            -> Bool           // does it match anywhere
re.find(s)            -> Match | nil    // leftmost match anywhere
re.match(s)           -> Match | nil    // anchored at start (prefix)
re.find_all(s)        -> [Match]        // all non-overlapping matches ([] if none)
re.replace_all(s, r)  -> String         // r: "$1" / "$<name>" / "$$"
re.split(s)           -> [String]       // split the subject on matches
```

**フラグ**は `compile` に文字列 (`"i"`/`"m"`/`"s"`) として渡し、インライン
`(?…)` グループとしてパターンに畳み込む。パターン中のインライン `(?i)` も
同様に効く。

**JIT 注記**: メソッド名 `find` / `split` / `match` は builtin メソッド名と
衝突する。以前はクラスインスタンス上でこれらを呼ぶと JIT が builtin へ
誤ディスパッチしていたが、修正済み (クラスインスタンス自身のメソッドが
JIT でも同名 builtin をシャドウするようになり、インタプリタと一致)。
これによりオブジェクト形式が 3 backend すべてで動く。

**パターンはシングルクォートの raw 文字列で書く** — `"\\d{4}"` ではなく
`'\d{4}'`。culebra のシングルクォート文字列はエスケープ処理も `{...}`
補間も行わないため、正規表現メタ文字 (`\d`, `\w`) と `{n}` 量化子が
そのまま透過する (Python の `r"..."` イディオム)。ダブルクォートの
パターンだと `\\d` が必要で、`{4}` が文字列補間に食われる。置換テンプレート
(`'$2.$1'`) も同様にシングルクォートが最もきれい。

`Match` はプレーンオブジェクト (マッチしないとき nil)。オフセットは対象
文字列への**バイトオフセット** (Go 流、culebra の String モデルと一致) で、
常に grapheme クラスタ境界に落ちる。

```culebra
# doctest: skip
m.value          -> String             // the whole-match text
m.start, m.end   -> Int                // byte offsets
m.groups         -> [Group | nil]      // groups[0] is the whole match
m.named          -> {name: Group}      // named captures, by name
// Group: g.value -> String, g.start / g.end -> Int
// subscript = captures accessor (string directly):
m[1]             -> String | nil       // positional group; m[0] = whole match
m["year"]        -> String | nil       // named group
// miss (out of range / unmatched / unknown name) -> nil; composes with ?? ""
// negative i wraps like an array; m["value"] is nil (use m.value / m[0])
// spans still via the Group objects: m.groups[1].start, m.named["year"].end
```

`Match` はプレーンな**データオブジェクト** (フィールドのみ) なので、
interp/JIT の値境界を変わらず越える — ネイティブプリミティブが直接構築する。
subscript のルーティングはオブジェクト上の O(1) の `is_match` フラグ
(interp `ObjectValue` / JIT `JitObject`) で、`str()` からは不可視なので
データ形状には一切触れない。

## 設計判断 (言語横断の統合)

- **no-match は nil** — Rust `Option`、Kotlin/Swift Optional、Python `None`。
  culebra の `?.` / `??` と合成: `Regex.find(p, s)?.value ?? ""`。
- **`find` / `find_all`** — Rust `find`/`find_iter`、Kotlin `find`/`findAll`
  (現代的な命名で `search` → `find`、`str` → `value`)。
- **find/captures の分割なし** — Rust は性能のため両者を分けるが、regexlib は
  速い tier を自動選択する (capture のないパターン → 純 DFA tier 1、capture
  ありのパターン → tier 2 / Pike) ため、利用者は分割を意識しない。
- **`split` を同梱** — Kotlin/Go/Ruby/JS/Python では標準。C++ エンジン API には
  無いので stdlib 層で追加 (find_all + マッチ間の slice)。
- **遅延でなく Array** — `find_all`/`split` は書き味のため配列を返す。遅延の
  `find_iter` (generator) は必要なワークロードが出たら後から足せる。
- **Match はクラスでなくデータ** — フィールドのみなので interp/JIT の値境界を
  変わらず越える (ネイティブプリミティブが直接構築する)。

## 実装 (3 backend)

`_Time` / `Time` の分割: ステートレスなネイティブプリミティブ + culebra
ソースのクラスラッパー。

- **`_Regex` プリミティブ** (新しい `Value` 型なし): `check`/`test`/`find`/`match`/
  `find_all`/`replace_all`/`split`。それぞれ `(pattern, subject, …)` を取る。
  パターン→`regexlib::Regex` キャッシュで再利用する。`check` は先行して検証
  する (不正なパターンは `Regex.compile` 時点で raise)。
  - **interp**: `stdlib_interp.h` の `make_regex_primitives_namespace()`。
    `regex_match_value` が Match オブジェクトを構築、no-match は `Value()` (nil)。
  - **JIT / AOT**: slow-path の `kNsMethods` アダプタのみ (`stdlib_jit.h` の
    `_ns_regex_*` + テーブルと `is_builtin_var` の `_Regex`) — `GC.stat` と同様、
    fast-path 分岐 / ランタイムヘルパ / `declare_runtime` なし。アダプタが
    `JitObject`/`JitArray` の結果を構築する。
- **`Regex` クラスラッパー** (`REGEX_MODULE_SOURCE`、遅延ロード): パターンを保持し、
  各メソッドを `_Regex` に委譲。`compile(pat, flags?)` はフラグをインライン
  `(?…)` グループに畳み込む。
- **バイトオフセット**: `MatchResult.begin/end` は既にバイトオフセットなので
  `m.start`/`m.end` へそのままコピー。
- **JIT の前提条件 (着地済み)**: `find`/`split`/`match` は builtin メソッド名と
  衝突する。JIT はクラスインスタンス自身のメソッドが同名 builtin をシャドウ
  するようになった (`compile_method_call` の一般修正、インタプリタと一致) ため、
  これらのメソッドは全 backend でラッパーへディスパッチする。

## 検証

test-first → interp → JIT → AOT で三者一致
(`feedback_check_jit_interp_symmetry`)。`tests/test_regex.cul` が happy path +
error path を、`tests/test_regex_extras.cul` が `escape` / `replace_all(fn)` /
`find_iter` (遅延、early exit、空マッチ、grapheme) をカバー。
docs: `docs/stdlib.ja.md` + `.md`。

## Extras (実装済み)

- **`Regex.escape(s)`** — メタ文字をバックスラッシュでクォートする。モジュール
  ラッパー内で純 culebra 実装 (メタ文字集合にバッククォート raw 文字列を使用)。
- **`replace_all(s, fn)`** — `Function` の `repl` を `Match` ごとに呼び、その
  戻り値をマッチ間に差し込む (`String` の `repl` はネイティブのテンプレート経路を
  維持)。純 culebra (`type_of` ディスパッチ + `slice`)。
- **`find_iter(s)`** — 遅延の `Iterator<Match>`。クラスメソッドは generator に
  なれない (CPS 変換はトップレベル `fn` しか書き換えない) ため、ネイティブの
  `_Regex.find_from(pat, s, pos) -> {m, nxt}` を駆動するトップレベル
  `fn _regex_find_iter` に委譲する。ネイティブは絶対オフセットと grapheme 的に
  正しい再開バイト (空マッチの 1 grapheme 先) を返すため、反復は常に前進する。
  ステップごとに接尾辞を再検索する — early exit には十分。全部を実体化するなら
  `find_all` を使う。(フィールド名は `next` でなく `nxt`。Iterator プロトコルが
  `next` を予約しているため。)
- **`find_all_str(s) -> [String]`** と **`count(s) -> Int`** — `Match` オブジェクトを
  スキップする軽量な一括 API。マッチ密なワークロード (256 KB の対象への
  `find_all` を `sample`) をプロファイルすると、ボトルネックはエンジン**ではなく**
  マッチごとの Object 構造 (Match obj + groups 配列 + group0 obj + named obj —
  各 ~6 確保、capture のないパターンでは大半が冗長) で、エンジンのマッチングは
  些細。`find_all_str` (テキストのみ) と `count` (オブジェクトなし) はこの
  ワークロードで `find_all` より**約 12× 速い**。`find_all_index(s) -> [Int]` は
  平坦なバイトスパン `[s0,e0,s1,e1,…]` を返す。Long は Array にインラインなので
  結果全体が 1 確保 (≈ `count` の速度、~13×)。`find_all` はオフセット*と*
  group/named capture の両方が要るときだけ使う。(`value` はコピーした `String`。
  StringView を値に持つ結果は試して撤回した — [`record.ja.md`](record.ja.md) を参照。)

## Open / deferred

- エンジンレベルの高速化 (SIMD リテラル prefilter、優先度 DFA) — 大きなテキストに
  *疎な*マッチのときだけ効く。その形状のワークロードが出るまで延期。
