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
| `--keep-symbols` | 出力にローカルシンボルを残す。既定ではリンク時に破棄し（`-Wl,-x`）、約 30% 小さくなる。デバッグ時に使用 |
| `--target=<triple>` | 指定 LLVM triple 向けにクロスコンパイル |
| `--sysroot=<path>` | `cc` の `--sysroot=` にそのまま渡す |
| `--rt-lib=<path>` | ランタイムアーカイブのパスを上書き（cross-compile では必須） |

### 環境変数オーバーライド

| 変数 | 効果 |
|---|---|
| `CULEBRA_VERBOSE=1` | 中間オブジェクトのパスと完全なリンクコマンドを表示 |
| `TMPDIR` | 中間オブジェクトファイル置き場（デフォルト `/tmp`） |

### ランタイムアーカイブの配布

ランタイムアーカイブ群——base `libculebra_rt.a` ＋ 重い機能ごとの
小さなアーカイブ（`libculebra_rt_tensor.a` / `libculebra_rt_http.a` /
`libculebra_rt_compress.a`、`culebra wrap` 用 `libculebra_rt_wrap.a`）
——は cpp-embedlib によって **`culebra` ドライバに直接埋め込まれて
います**。ドライバは単体で完結する 1 バイナリで、サイドカーの `.a`
ファイルを別途インストールする必要はありません。`culebra build` の
初回呼び出し時に必要なアーカイブを
`$HOME/.cache/culebra/<fingerprint>/lib*.a` に展開し、2 回目以降は
キャッシュを再利用します。fingerprint は埋め込みアーカイブのコンテ
ンツハッシュなので、`culebra` を再ビルドすると自動的に旧版のキャッ
シュと分離されます。

base アーカイブは tensor / http / compress の choke を**弱シンボルの
スタブ**として持つので、単体では BLAS・OpenSSL・zlib を一切参照しま
せん。`culebra build` は常に base を link し、ソースがその機能を使う
時だけ機能アーカイブを **force-load**（強い choke が base の弱スタブ
を上書き）します。2^N の組合せでなく N+1 アーカイブです。

## Tensor-free バイナリ

`culebra build` は AST を走査し、`Tensor` 識別子を参照する時だけ
`libculebra_rt_tensor.a`（強い tensor choke＝`culebra_runtime_tensor_*`、
`TAG_TENSOR` 分岐）を force-load し、`Accelerate` / BLAS を付けます。
`Tensor` が無ければ base の弱 tensor スタブが
`culebra_runtime_num_add` から `cblas_*` への静的到達経路を断つので、
BLAS シンボルを一切参照せず依存が外れます。

## Http-free バイナリ

同じことが `Http` にも独立に当てはまります。AST に `Http` 識別子が
ある時だけ `libculebra_rt_http.a`（強い http choke、`httplib.h` を
include）を force-load し、OpenSSL + zlib を付けます。runtime の http
ヘルパは in-process JIT のため `__attribute__((used))` が付き
`-dead_strip` / `--gc-sections` を貫通しますが、この別アーカイブに
入っているので、非 Http プログラムはそれを force-load せず OpenSSL/
zlib シンボルを参照しません。こちらが効果は大きく、非 Http バイナリ
は ~5 MB、Http 版は ~9.5 MB です（OpenSSL は静的リンク）。2 軸は独立
なので、Tensor も Http も使わないプログラムは base のみを link し
BLAS も OpenSSL も避けます。

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

## シンボルの除去

埋め込みランタイムアーカイブには、配布実行ファイルでは無用な
ローカルシンボル（`GCC_except_table*`、テンプレートや文字列の
実体化など）が数千個含まれる。リンクは既定でこれらを破棄し
（`-Wl,-x` — ld64・GNU ld・lld のいずれも解釈する）、ローダが必要
とするグローバル／動的シンボルは保持したままバイナリを約 30% 縮める
（例: Term/IO プログラムが ~7.6 MB → ~5.3 MB）。デバッグ用に残したい
場合は `--keep-symbols` を渡す。

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
      -DCULEBRA_ENABLE_JIT=ON
# base アーカイブは元々 Tensor-free（弱スタブ）で、cross は Tensor 非対応
# （上の制限参照）なので base をビルドする。
cmake --build build-linux-x86_64 --target culebra_rt

# 2. プログラムをクロスコンパイル
culebra build my-program.cul \
  --target=x86_64-unknown-linux-gnu \
  --sysroot=$LINUX_SYSROOT \
  --rt-lib=$PWD/build-linux-x86_64/libculebra_rt.a \
  -o ./my-program-linux

# 3. 確認（Linux ホスト上、またはエミュレータ経由）
file ./my-program-linux
# ELF 64-bit LSB executable, x86-64, ...
```
