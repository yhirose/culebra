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

宣言するのは構築（`ctor`）だけで、破棄は宣言しません — `.dtor` ビルダーは
ありません。ラップした型の `~T()` は、ハンドルが継承する確定 drop 機構
（§3 参照）が自動的に呼び出すので、書くのは C++ のデストラクタだけです。

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

バインディングとラップ対象ライブラリ（`--link`）が AOT バイナリに載るのは、
スクリプトがラップ名前空間を参照したときだけです。`ext-culebra` でビルドしても
ラップクラスを一切使わないプログラムは、ラップ対象ライブラリを一切リンクせず、
素の `culebra build` と同じサイズになります。判定は保守的な識別子マッチ
（`Geo` 等）なので、過剰リンクはあっても不足リンクはありません。これは
`culebra build` が `Http` を使うプログラムにだけ OpenSSL をリンクするのと
同じ仕組みです。

## マーシャリング

| C++ | Culebra |
|---|---|
| `long` / `int` | `Long` |
| `double` / `float` | `Float` |
| `bool` | `Bool` |
| `std::string` / `std::string_view` / `const char*` | `String` |
| 値返しの `T`・`std::unique_ptr<T>` | ラップ済み `T` の所有インスタンス |
| `std::shared_ptr<T>` | share を 1 つ保持するインスタンス |

ラップ済みクラスの `T&` / `const T&` 返しは `.method` では**コンパイル
エラー**です — 参照は所有形状ではありません。`.borrowed_method` で
宣言してください:

```cpp
.borrowed_method<&Box::inner>("inner")     // Counter& inner()
```

結果は**借用ハンドル**です: 所有しない（drop は no-op）、存在する間は
親を生かし、アクセスごとに検証されます — 親が明示 drop された、または
借用取得後に非 const メソッドで変更された（各インスタンスは generation
カウンタを持ち、非 const dispatch が自動で bump）場合、解放/再配置済み
メモリに触れる代わりに `ClosedError` になります:

```culebra
let b = __Foreign.Box.new(3)
let c = b.inner()
b.reset(9)             # 非 const -> generation bump
c.value()              # !! ClosedError
```

借用を無効化しないことが確実な非 const メソッドは opt-out できます:
`.method<&T::touch>("touch", {}, culebra::wrap_policy::preserves_borrows)`。
const 性やこのフラグの誤宣言は著者の契約違反（sol2/pybind11 と同じ
建付け）ですが、その場合も「stale エラーの過剰/欠落」であって culebra
側のメモリ非安全には絶対になりません。

コンテナ（`std::vector`/`std::map`）とコールバックは未対応です。

動く完全版は `examples/wrap/`、パイプラインの end-to-end 検証は
`tests/wrap_test.sh` を参照してください。
