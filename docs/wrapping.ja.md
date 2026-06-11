# 自分の C++ クラスをラップする

`culebra wrap` は、あなたの C++ クラスをビルトインとして組み込んだ
**拡張 culebra バイナリ**を作ります — インタプリタの fork も plugin
ABI も不要です。短い宣言 TU を書くと C++ コンパイラが glue を実体化し
（pybind11 流）、インタプリタ・`--jit`・拡張バイナリの `culebra build`
が作る AOT バイナリのすべてで同一に動きます。

## 1. 宣言する

手元の header-only クラスに対して:

```cpp
// vec2.hpp — culebra を一切知らない素の C++
namespace demo {
class Vec2 {
 public:
  Vec2(double x, double y);
  double len() const;
  void scale(double k);
  std::string show() const;
  Vec2 unit() const;          // 値返し
  static long dims();
};
}
```

宣言 TU を 1 つ書きます:

```cpp
// vec2_binding.cpp
#include <wrap.h>
#include "vec2.hpp"

namespace {
const bool registered = [] {
  culebra::wrap<demo::Vec2>("Geo", "Vec2")
      .ctor<double, double>({"x", "y"})
      .method<&demo::Vec2::len>("len")
      .method<&demo::Vec2::scale>("scale", {"k"})
      .method<&demo::Vec2::show>("show")
      .method<&demo::Vec2::unit>("unit")
      .static_method<&demo::Vec2::dims>("dims");
  return true;
}();
}
```

メンバ関数は*テンプレート*引数（`method<&T::m>`）なので、メソッドごとに
専用の thunk がコンパイルされます。引数名は省略可能で、エラーメッセージと
インタプリタのキーワード束縛に使われます。

## 2. ビルドする

```sh
culebra wrap vec2_binding.cpp -o ext-culebra
# ビルド済みライブラリにリンクする場合:
culebra wrap mylib_binding.cpp --link "-L/opt/mylib/lib -lmylib" -o ext-culebra
```

`culebra wrap` は culebra のソースツリー（このバイナリのビルド元、または
`$CULEBRA_HOME`）にあなたの TU を加えて再ビルドし、`~/.cache/culebra-wrap/`
にキャッシュします。ccache があれば実質「宣言のコンパイル + relink」で
済みます。`--lto` で最適化バイナリ（ビルドは遅くなります）。

## 3. 使う — 全 backend で

```culebra
# doctest: skip
let v = Geo.Vec2.new(3.0, 4.0)
puts(v.len())          # => 5
v.scale(2.0)
let u = v.unit()       # 値返し -> 新しい所有インスタンス
v.drop()               # ~Vec2 が「いま」走る（確定的）
v.len()                # !! ClosedError
```

ラップされたインスタンスは完全な lifetime モデルを持つリソースです:
scope 終端の確定 drop（循環込み）、冪等な明示 `drop()`、use-after-drop の
`ClosedError`。`ext-culebra build script.cul` の AOT バイナリにも
バインディングが載ります。

## マーシャリング

| C++ | Culebra |
|---|---|
| `long` / `int` | `Long` |
| `double` / `float` | `Float` |
| `bool` | `Bool` |
| `std::string` / `std::string_view` / `const char*` | `String` |
| 値返しの `T`・`std::unique_ptr<T>` | ラップ済み `T` の所有インスタンス |
| `std::shared_ptr<T>` | share を 1 つ保持するインスタンス |

ラップ済みクラスの `T&` / `const T&` 返しは**コンパイルエラー**です —
参照には所有形状がまだありません（borrowing は後のフェーズ）。コンテナ
（`std::vector`/`std::map`）とコールバックも未対応です。

動く完全版は `examples/wrap/`、パイプラインの end-to-end 検証は
`tests/wrap_test.sh` を参照してください。
