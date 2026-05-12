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
