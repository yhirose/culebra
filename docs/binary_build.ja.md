# Culebra バイナリビルド

`culebra build` は `.cul` ソースを **LLVM AOT codegen + システム
`cc`** によって単体実行ファイルにコンパイルします。生成バイナ
リに LLVM ランタイムは含まれず、依存表面は `libc++` / `libSystem`
(macOS) または `libstdc++` / `libc` (Linux)、それと `Tensor` を
参照するときに限り `Accelerate` / BLAS が加わるだけです。

## 使い方

```sh
culebra build path/to/program.cul -o ./program
./program [args...]
```

デフォルトはホストプラットフォーム向け。

### オプション

| フラグ | 説明 |
|---|---|
| `-o <path>` | 出力実行ファイルのパス（必須） |
| `-O<level>` | 最適化レベル 0–3（デフォルト 2） |
| `--emit-llvm` | プログラムの LLVM IR も書き出す（デバッグ用） |
| `--target=<triple>` | 指定 LLVM triple 向けにクロスコンパイル |
| `--sysroot=<path>` | `cc` の `--sysroot=` にそのまま渡す |
| `--rt-lib=<path>` | ランタイムアーカイブのパスを上書き（cross-compile では必須） |

### 環境変数オーバーライド

| 変数 | 効果 |
|---|---|
| `CULEBRA_VERBOSE=1` | 中間オブジェクトのパスと完全なリンクコマンドを表示 |
| `TMPDIR` | 中間オブジェクトファイル置き場（デフォルト `/tmp`） |

### ランタイムアーカイブの配布

両方のランタイムアーカイブ（`libculebra_rt.a`、
`libculebra_rt_no_tensor.a`）は cpp-embedlib によって **`culebra`
ドライバに直接埋め込まれています**。ドライバは単体で完結する 1
バイナリで、サイドカーの `.a` ファイルを別途インストールする必要
はありません。`culebra build` の初回呼び出し時に必要なアーカイ
ブを `$HOME/.cache/culebra/<fingerprint>/lib*.a` に展開します。
2 回目以降はキャッシュを再利用します。fingerprint は埋め込みアーカイブのコンテンツハッシュなの
で、`culebra` を再ビルドすると自動的に旧版のキャッシュと分離さ
れます。

## Tensor-free バイナリ

`culebra build` は AST を走査して `Tensor` 識別子を一切参照して
いないことを確認すると、`libculebra_rt_no_tensor.a` をリンクに
選択します。これは tensor 入口点（`culebra_runtime_tensor_*`、
値解放／文字列化の `TAG_TENSOR` 分岐など）が abort-on-call スタ
ブに置き換えられた第二のランタイムアーカイブです。
`culebra_runtime_num_add` から `cblas_*` への静的到達経路が断ち
切られるため、`Accelerate` / BLAS 依存自体もバイナリから外せます。

`otool -L`（macOS）や `ldd`（Linux）で確認できます:

```sh
$ culebra build my-program.cul -o /tmp/my-program     # Tensor 未使用
$ otool -L /tmp/my-program
/tmp/my-program:
        /usr/lib/libc++.1.dylib
        /usr/lib/libSystem.B.dylib
```

Tensor を使うプログラムでは全部入りアーカイブとフレームワーク
両方がリンクされます:

```sh
$ otool -L /tmp/microgpt_tensor
/tmp/microgpt_tensor:
        /usr/lib/libc++.1.dylib
        /System/Library/Frameworks/Accelerate.framework/.../Accelerate
        /usr/lib/libSystem.B.dylib
```

## クロスコンパイル

`--target=<triple>` で LLVM ターゲットを指定。よく使う triple:

- `x86_64-unknown-linux-gnu`
- `aarch64-unknown-linux-gnu`
- `x86_64-apple-macosx`

クロスコンパイルにはユーザ側で以下を用意する必要があります:

1. **ターゲット用 sysroot**（ターゲットの C++ ヘッダ、`libc`、
   CRT ファイルを含むディレクトリ）。`--sysroot=<path>` で渡す。
2. **ターゲット向けにビルドしたランタイムアーカイブ**。
   `--rt-lib=<path>` で渡す。ホスト用 `libculebra_rt.a` は
   ホスト triple 向けにビルドされているので使えません。

ターゲット向けランタイムをビルドするには、CMake にターゲット
ツールチェーン（同じソースツリーを、ターゲット sysroot と `cc`
で設定）を向けてください。

### Phase E MVP 制約

- 各ターゲット向けランタイムは同梱されない。ユーザが自前 CMake /
  ツールチェーンで生成する
- `--target=<triple>` と `Tensor` の併用は reject される。ホストの
  BLAS リンクフラグはターゲットでは正しくないため。`Tensor` 参照
  を外すか、将来のフェーズを待つ

### 例: macOS ホストから Linux x86_64 向け

```sh
# 1. ターゲット向けランタイムをビルド（ターゲットごと 1 回）
#    Linux sysroot が $LINUX_SYSROOT、cross 用 cc が用意済みとする
cmake -B build-linux-x86_64 \
      -DCMAKE_C_COMPILER=clang \
      -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_C_FLAGS="--target=x86_64-unknown-linux-gnu --sysroot=$LINUX_SYSROOT" \
      -DCMAKE_CXX_FLAGS="--target=x86_64-unknown-linux-gnu --sysroot=$LINUX_SYSROOT" \
      -DCULEBRA_ENABLE_JIT=ON \
      -DCULEBRA_ENABLE_TENSOR=OFF
cmake --build build-linux-x86_64 --target culebra_rt_no_tensor

# 2. プログラムをクロスコンパイル
culebra build my-program.cul \
  --target=x86_64-unknown-linux-gnu \
  --sysroot=$LINUX_SYSROOT \
  --rt-lib=$PWD/build-linux-x86_64/libculebra_rt_no_tensor.a \
  -o ./my-program-linux

# 3. 確認（Linux ホスト上、またはエミュレータ経由）
file ./my-program-linux
# ELF 64-bit LSB executable, x86-64, ...
```
