# Culebra の埋め込み

Culebra は header-only です。ヘッダを include し、JIT を使う場合は
LLVM をリンクすれば、C++ からインタプリタや JIT を駆動できます。

## 最小例

```cpp
#include <culebra.h>
#include <stdlib_interp.h>

int main() {
  auto env = culebra::environment({});  // stdlib をバインド

  std::vector<std::string> msgs;
  auto ast = culebra::parse("<inline>", "1 + 2", 5, msgs);

  culebra::Value val;
  culebra::interpret(ast, env, val, msgs, culebra::Debugger());
  // val.to_long() == 3
}
```

JIT を使う場合は `<stdlib_jit.h>` を追加し、起動時に
`culebra::install_jit_stdlib()` を 1 回呼んで、`interpret` の代わりに
`culebra::JIT::run(ast)` を呼びます。

## スレッディング

ランタイムは **Runtime ごと**に単一スレッドで動きます。複数の
ホストスレッドから Culebra を使うには、各スレッドが独立した処理
を持つ形にします:

```cpp
std::thread([&]{
  auto env = culebra::environment({});
  // ... メインスレッドとは独立 ...
}).detach();
```

スレッドごとの状態（GC・例外キャリア・PRNG・defer stack・shape
registry）は、初回アクセス時に遅延生成される thread-local の
default `Runtime` に格納されます。異なるスレッドの状態は完全に
分離されています。

PEG パーサも thread-local なので、別スレッドでの並列 `parse()` も
安全です。

## 同一スレッドで複数スクリプトを動かす

`culebra::Runtime` は VM コンテキスト 1 個分を所有します。プラグイン
分離、サンドボックス DSL、ドキュメント毎の状態など、同一スレッド
内で複数の VM が必要なときは Runtime を明示的に作ります:

```cpp
culebra::Runtime rt_a, rt_b;

{
  culebra::RuntimeScope scope(rt_a);
  // ... rt_a のコンテキストでコンパイル・実行 ...
}
{
  culebra::RuntimeScope scope(rt_b);
  // ... 別の状態、別の GC、別の例外キャリア ...
}
```

`RuntimeScope` は RAII で、デストラクタで以前のアクティブ Runtime
を復元します。後で `rt_a` に戻ったときは、出ていったときの状態が
そのまま残っています。

ある Runtime で作った Value を別の Runtime に渡してはいけません。
GC トラッカー、shape registry、例外キャリアは、その値を作った
Runtime に紐付いています。

## Runtime ごとの拡張フック

`JIT::install_extension()`（およびそのラッパー
`culebra::install_jit_stdlib()`）は、現在アクティブな Runtime を
ターゲットにします。これにより、Runtime ごとに異なるホスト API を
公開できます:

```cpp
culebra::Runtime trusted, sandbox;

{
  culebra::RuntimeScope s(trusted);
  culebra::install_jit_stdlib();      // フル stdlib（IO, Sys, Math, ...）
}
{
  culebra::RuntimeScope s(sandbox);
  install_jit_stdlib_restricted();    // 制限されたサブセット
}

// 各 Runtime は自分のフックセットに対して Math/IO/Sys を解決する。
```

`RuntimeScope` がアクティブで無い状態で `install_jit_stdlib()` を
呼ぶと、プロセス共通のデフォルトに書き込まれ、override を設定していない
すべての Runtime がそこにフォールバックします — これが従来の
単一 VM と マルチスレッド embedding のパスです。

## C++ からスクリプト関数を呼ぶ

スクリプト実行後、トップレベルの `fn` や `let f = fn ...` は
`Environment` に登録されています。`culebra::call` で C++ から呼べます:

```cpp
// スクリプト: fn update(x, y) { x + y * 2 }
auto v = culebra::call(env, "update",
                      {culebra::Value(1L), culebra::Value(2L)});
// v.to_long() == 5
```

`call` は位置引数を位置パラメータにバインドし、あふれた分を
`__ARGS__` にまとめ、トップレベルの `return` を戻り値として返します。
デフォルト値付きパラメータの解決はこのヘルパーでは行わないので、
位置引数はすべて明示的に渡してください。

## スクリプトエラーの扱い

スクリプト内で発生した失敗は `culebra::CulebraError`（`<shared.h>`
で定義、`std::runtime_error` のサブクラス）として送出されます。
スクリプト側 `try`/`catch` で見える構造化フィールドをそのまま保
持します:

```cpp
class CulebraError : public std::runtime_error {
public:
  std::string kind;   // 例: "TypeError"、"ArityError"、ユーザ throw した kind
  long line = 0, col = 0;
};
```

`culebra::interpret` / `culebra::call` / `culebra::JIT::run` など
ユーザコードを駆動する経路をくるむ形で catch します:

```cpp
try {
  culebra::call(env, "update", {culebra::Value(1L), culebra::Value("oops")});
} catch (const culebra::CulebraError& e) {
  std::println(stderr, "{}: {} at {}:{}",
               e.kind, e.what(), e.line, e.col);
} catch (const std::exception& e) {
  // culebra 由来でない例外（ホスト側のバグ、std 失敗など）
  std::println(stderr, "host: {}", e.what());
}
```

スクリプト側ではこれと同じ値が `catch e { ... }` の `e` として
バインドされ、`e.kind` / `e.message` / `e.line` / `e.col` プロ
パティでアクセスできます — ユーザ側仕様と標準 kind 一覧
（`TypeError`, `ArityError`, `IOError`, `ValueError`, `NameError`,
`IndexError`, `KeyError`, `AssertionError`, `InternalError`）は
[言語仕様 §15](language.ja.md) を参照。

スクリプト側 `throw expr` でユーザが投げた値は別の
`culebra::CulebraException` として届きます（生の `JitValue` を保
持）。通常は埋め込み側でこれを直接 catch せず、スクリプト側で
`try`/`catch` してから kind 付きの `CulebraError` として届けるの
が定石です。

## ホスト関数の定義

`culebra::define` で C++ の callable をスクリプトから見える関数
として登録します。引数の型と戻り値の型は callable のシグネチャ
から自動推論されます。

```cpp
culebra::define(env, "log", [](const std::string& msg) {
  std::cout << msg << "\n";
}, {"msg"});

culebra::define(env, "host_add",
                [](long a, long b) { return a + b; }, {"a", "b"});
```

サポートする引数・戻り値の型: `long`, `int`, `double`, `float`,
`bool`, `std::string`, `std::string_view`, `const std::string&`,
`culebra::Value`（透過）。推論された型はスクリプト側パラメータの
型注釈（`Long`, `Float`, `Bool`, `String`）にマップされるので、
誤った型の引数は callable に入る前に呼び出し側で弾かれます。

パラメータ名を省略すると `_arg0`, `_arg1`, ... になります —
スクリプトから `fn.parameters()` で内省される場合は明示的に
指定してください。

`FunctionValue` を直接組みたい場合（可変長、デフォルト値、env を
直接触りたい等）は raw 形式で:

```cpp
env->initialize("custom",
    culebra::Value(culebra::FunctionValue(
        {{"msg", false}},
        [](std::shared_ptr<culebra::Environment> env) {
          // 手書き: env から引数を取って Value を返す
          return culebra::Value();
        })),
    /*mut=*/false);
```

raw 形式の実例は `include/stdlib_interp.h` の Math.abs / IO.print /
Random.uniform 等に多数あります。

## AOT 用 runtime archive (`libculebra_rt.a`)

ヘッダオンリー埋め込みでは `culebra_runtime_*` ヘルパが各 TU に
`inline` 展開される — ひとつのバイナリなら OK だが、**`culebra build`
で生成した AOT バイナリも配布したい場合** (詳細は
[`binary_build.ja.md`](binary_build.ja.md))、LLVM 依存なしの同じヘル
パ群を **static archive** として持つ必要がある。CMake は
`-DCULEBRA_ENABLE_JIT=ON` で 2 種類の archive を出力する:

| Archive | 含むもの |
|---|---|
| `libculebra_rt.a` | 全 stdlib runtime ヘルパ (Math / IO / FS / Time / Random / Sys / Tensor / JSON) |
| `libculebra_rt_no_tensor.a` | Tensor エントリポイントを abort スタブ化 — BLAS / Accelerate の link を bin から落とせる |

`culebra build` は AST スキャンで `Tensor` namespace 参照があれば前
者、なければ後者を自動選択。fizzbuzz 規模のバイナリは 1.2 MB → 350 KB
に縮む。

### AOT 経路を組み込む embedder

自分の embedder からも `culebra::JIT::build_object` で AOT コンパイ
ルを駆動したい場合は、同じ archive を link して `culebra build` に
位置を伝える: `CULEBRA_RT_LIBPATH` を compile-time に渡す
(CMake は [`CMakeLists.txt`](../CMakeLists.txt) でこの define を自
動セットしている)。

通常の embedder は archive とは無関係 — ヘッダオンリー include がサ
ポート経路。archive は AOT subprocess が standalone バイナリを link
する時だけ必要。

### `CULEBRA_RT_DEFINE_RUNTIME`

`CULEBRA_RT_DEFINE_RUNTIME` マクロは、`CULEBRA_RT_INLINE` タグ付き
ヘルパを `inline` から `extern "C"` に切り替えて archive 側 TU が唯一
の owner になるようにする。ヘッダオンリー embedder は **絶対に
define してはいけない**。AOT archive の生成元 TU
(`src/runtime/culebra_rt.cc`) のみで define されるべき。

### クロスコンパイル

`culebra build --target=<triple>` 現状はホスト用 archive のみ生成済
み。target 向けには `--rt-lib=<path>` で対応する archive を別途指
定する (target ごとの auto-build は roadmap、`docs/binary_build.ja.md`
参照)。

## スモークテスト

リポジトリには契約を検証する小さなサンプルが 2 つ含まれます:

* [`tests/embedding/mt_smoke.cc`](../tests/embedding/mt_smoke.cc) —
  4 つのホストスレッドがそれぞれ try/catch 付きスクリプトを parse
  + interpret、加えて JIT パスでも 4 スレッド。合計 240 並行実行。
* [`tests/embedding/mi_smoke.cc`](../tests/embedding/mi_smoke.cc) —
  1 スレッド内で 2 つの Runtime を交互に切替え、独立した PRNG 状態
  と独立した JIT フックセットを検証。
* [`tests/embedding/define_smoke.cc`](../tests/embedding/define_smoke.cc)
  — `culebra::define` を経由してスクリプトと `culebra::call` 両方から
  C++ 関数を呼び、自動付与される型注釈の動作も確認。
