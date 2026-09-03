# デプロイ: バイナリ・埋め込み・ラッピング

素の`culebra`実行以外でCulebraコードを動かす3つの方法: standalone
AOTバイナリ（`culebra build`）、C++ ホスト内へのVM/JIT埋め込み、自分の
C++ クラスをビルトインとして公開する拡張`culebra`バイナリ
（`culebra wrap`）。3つは1つのruntime archiveレイアウトを共有し、
[§4](#4-共有-runtime-archive-レイアウト) にまとめて記述し各章から参照する。

目次
----

1. [Standalone バイナリビルド（`culebra build`）](#1-standalone-バイナリビルドculebra-build)
2. [C++ ホストへの Culebra 埋め込み](#2-c-ホストへの-culebra-埋め込み)
3. [C++ ライブラリのラッピング（`culebra wrap`）](#3-c-ライブラリのラッピングculebra-wrap)
4. [共有 runtime archive レイアウト](#4-共有-runtime-archive-レイアウト)

開発用サブコマンド — `test`・`lint`・`fmt`・`dap` — は
[`tooling.ja.md`](tooling.ja.md) が扱います。

## 1. Standalone バイナリビルド（`culebra build`）

`culebra build`は`.cul`ソースを **LLVM AOT codegen + システム
`cc`** によって単体実行ファイルにコンパイルします。生成バイナリに
LLVMランタイムは含まれず、依存表面は`libc++` / `libSystem`
(macOS) または`libstdc++` / `libc` (Linux)、それと`Tensor`を
参照するときに限り`Accelerate` / BLASが加わるだけです。

```sh
culebra build path/to/program.cul -o ./program
./program [args...]
```

デフォルトはホストプラットフォーム向け。

### ホスト側に必要なもの

スクリプトの実行に`culebra`バイナリ以外は要りません。`--jit`も同じで、
コンパイルに使うLLVMはバイナリの中にあります。外へ出るのは
`culebra build`だけです — codegenはプロセス内で完結しますが、
リンクにはプラットフォーム側のオブジェクトファイル周りが要り、
一度も何もビルドしたことのないマシンにはそれがありません。`build`は
コンパイルを始める前にこれを確認し、端末上ならば足りないものを
「名前を挙げるだけ」でなくインストールするか尋ねます。

| ホスト | リンク段が必要とするもの | 入手方法 |
|---|---|---|
| macOS | Xcode Command Line Tools（`/usr/bin`の`cc`はその実体ではなくシムで、Mach-Oが`libSystem`をリンクする先のSDKスタブもこれが持ち込む） | `culebra toolchain install`（Appleのインストーラを起動）または`xcode-select --install` |
| Linux | `cc`・libstdc++・Cランタイムのスタートアップファイル | `sudo apt install g++`（Debian / Ubuntu）、`sudo dnf install gcc-c++`（Fedora） |
| Windows | mingwのCRTオブジェクトと静的ライブラリ — **コンパイラもリンカも不要** | `culebra toolchain install`（約8MB） |

他には何も要りません。AOTのリンクが必要とするアーカイブは、
サードパーティの静的ライブラリも含めて`culebra`バイナリ自身から
出てきます（[§4](#4-共有-runtime-archive-レイアウト)）。
システムからリンクされるライブラリとして残るのはzlibだけで、
`Http`・`Compress`・`to_png`を使うプログラムが`-lz`で引きます —
macOSとWindowsのkitには既にありますが、Linuxではリンカが見る
`libz.so`のシンボリックリンクがdevパッケージ側にあります
（`sudo apt install zlib1g-dev`、`sudo dnf install zlib-devel`）。

culebraが自分でインストールしない唯一のホストがLinuxです。そこでの
C++ツールチェーンはディストリビューションのパッケージマネージャの
持ち物でrootが要り、ユーザーの代わりに`sudo`を叩くのはコンパイラの
仕事ではありません。`culebra toolchain install`はそのディストリビュー
ション向けのコマンドを1つ表示して終わります。

### `culebra toolchain`

```sh
culebra toolchain status                  # 今あるものと、リンクできるかどうか
culebra toolchain install                 # 足りないものを入れる
culebra toolchain install --from <zip>    # リリース版でなく手元でビルドしたkitから
culebra toolchain uninstall               # culebraが入れたものを削除する
```

Windowsでは`install`が、このバイナリが名乗るバージョンのリリースに
添付されたkitを取得し、隣に公開されたダイジェストと突き合わせてから
展開します。展開先は

```
%LOCALAPPDATA%\culebra\toolchain\<version>\
```

権限昇格も`PATH`の書き換えもレジストリへの書き込みもありません。
`uninstall`はこのディレクトリの削除で、手で消しても同じことです。
kitがバージョンごとに分かれているのは、kitのlibstdc++がこのバイナリに
埋め込まれたランタイムアーカイブをコンパイルしたものと同一でなければ
ならないからです — `install`はマニフェストが別バージョンを名乗るkitを
拒否しますし、culebraを上げたら付属のkitを入れ直すことになります。

他のプラットフォームでの`install`は薄いものです。macOSではAppleの
Command Line Toolsインストーラを起動し（このプロセスが待てないGUIの
流れなので、起動したことを伝えてbuildの再実行を促します）、Linuxでは
パッケージコマンドを表示します。`uninstall`はどちらでも「culebraが
入れたものは無い」と報告します。

### Windowsにコンパイラが要らない理由

Windowsはツールチェーンを一切同梱していませんが、そこでAOTのリンクに
足りないのはリンカであってコンパイラではありません — `culebra build`は
オブジェクトをプロセス内で自分で吐きます。そこでリンカをバイナリの中に
置きました。JITのために既に載せているLLVMの上に、culebraがlldを
リンクしています。kitが供給するのはリンクのmingw側の半分だけ —
CRTオブジェクト、libstdc++ / libgcc、そしてWin32のインポートライブラリです。

そのLLVMを共有できることが安く済む理由で、代替案は推測でなく実測して
比べました。MSYS2の`ld.lld.exe`は5.7MBのプログラムですが147MBの
`libLLVM` DLLにリンクされているため、それを同梱するkitは圧縮後55MB。
同じ静的アーカイブから自前でビルドしたlldはDLLを必要としませんが、
隣のバイナリと何も共有できず圧縮後41MBでした。lldを`culebra`に
リンクする形はダウンロードを23MB増やし（lldのアーカイブ自体は4MBですが、
lldのCOFFドライバが全LLVMバックエンドを参照するため芋づる式に入る）、
kitは8MBに収まります。結果として`culebra build`を使う人の合計は
84MBでなく約60MB、その代わり一度も使わない人が23MBを負担します。

kitはリリース時に、バイナリの中のランタイムアーカイブをコンパイルしたのと
同じMSYS2 UCRT64ツリーから梱包されます（`misc/windows_toolchain/pack_windows_toolchain.sh`）。
両者のlibstdc++が一致することが、誰かが正しく突き合わせた結果ではなく
構成上の帰結になるのはこのためです。kitはそのツールチェーンのC++ドライバが
組み立てるリンカコマンドライン（`link-recipe.txt`）を記録しており、
culebraは自分のオブジェクトとfeature側のライブラリをその中間に差し込みます —
つまりculebraが行うリンクは、ドライバ抜きで、ドライバが行ったはずの
リンクそのものです。

他のMSYS2環境でなくUCRT64なのは、同じlibstdc++ ABI（UCRT64のclangはGCCの
libstdc++の上にあり、CLANG64のlibc++は別ABI）と同じCランタイムだからです。
GNU ldでなくlldなのは、PEをELFやMach-Oのリンカと同じようにdead-stripできるのが
lldだけだからです。GNU ldは全関数のunwind情報を残し、その情報が関数を残すので、
`--gc-sections`を付けてもランタイムアーカイブ全体が全プログラムに残ります。

WindowsでCulebra自体をソースからビルドするのは別の話で、そちらは完全な
MSYS2ツールチェーンを必要とします。[`CONTRIBUTING.md`](../CONTRIBUTING.md)が
扱います。

### オプション

| フラグ | 説明 |
|---|---|
| `-o <path>` | 出力実行ファイルのパス（必須） |
| `-O<level>` | 最適化レベル0–3（デフォルト2） |
| `--emit-llvm` | プログラムのLLVM IRも書き出す（デバッグ用） |
| `--keep-symbols` | デバッグ用にシンボルテーブルを出力に残す（詳細は後述の[シンボルの除去](#シンボルの除去)を参照） |
| `--target=<triple>` | 指定LLVM triple向けにクロスコンパイル |
| `--sysroot=<path>` | `cc`の`--sysroot=`にそのまま渡す |
| `--rt-lib=<path>` | ランタイムアーカイブのパスを上書き（cross-compileでは必須） |

### 環境変数オーバーライド

| 変数 | 効果 |
|---|---|
| `CULEBRA_VERBOSE=1` | 中間オブジェクトのパスと完全なリンクコマンドを表示 |
| `TMPDIR` | 中間オブジェクトファイル置き場（デフォルト`/tmp`） |

### Tensor-free / Http-free バイナリ

各機能軸は独立にforce-loadされる（仕組みは
[§4](#4-共有-runtime-archive-レイアウト) 参照）ので、`Tensor`も
`Http`も使わないプログラムはbaseのみをlinkする。OpenSSLを
落とすだけで約4.7 MB効く（非Httpバイナリ ~7.6 MBに対しHttp版は
~12.2 MB、OpenSSLは静的リンクのため）。同じゲーティングは、外部
ライブラリを引かないが自前のコードを抱える1つのサブシステムにも
効く: 正規表現エンジン（`Regex`、`re'...'`リテラルを含む、
cpp-regexlibの約320 KB）。外部依存が無くても弱/強分岐が要るのは、
エンジン内部の`__builtin_cpu_supports`によるランタイム分岐チェックが
コンパイラに、それをコンパイルする翻訳単位ごとの起動時CPUID
コンストラクタを生成させるためで、これを無条件にすると自前のコード
以外にもこのコンストラクタが全バイナリに入ってしまう。`Proc`の
fork/exec層、`Canvas.Sprite.from_png` / `Canvas.Font`の背後の
PNG/TTFデコーダ、そして`Peg`（cpp-peglib）も外部ライブラリを引かない
が、それぞれ専用のchokeは不要——自身のnamespaceのdispatch tableを
通じてのみ到達するプレーンなコードとしてコンパイルされるので、
名指ししないプログラムはリンクされない（仕組みは
[§4](#4-共有-runtime-archive-レイアウト) 参照）。ただし`Peg`は
このdead-strippingが全バイトには届かない唯一のnamespaceで、`culebra::
pegparser::compile()`自体はPegを名指ししないバイナリから消えている
ことを`--keep-symbols`ビルドの`nm -C --defined-only`で確認済みだが、
peglibの`Ope`クラス階層はvtableとtypeinfoを残し、それを
`--gc-sections`が使用の有無に関わらず保持する。加えて、`std::function`
に包んだローカルラムダのcomdatテンプレート実体化が、それを呼ぶはずの
関数が消えたあとも孤立して生き残る（リンカの既知の制限で、到達可能な
呼び出し経路ではない）。どちらも不活性で、Peg未使用バイナリで実行され
ることは無く、実測で固定約53 KB——2本目のarchiveをforce-loadして
避けるのではなく、これは受け入れる。

`otool -L`（macOS）や`ldd`（Linux）で確認できます:

```sh
$ culebra build my-program.cul -o /tmp/my-program     # Tensor 未使用
$ otool -L /tmp/my-program
/tmp/my-program:
        /usr/lib/libc++.1.dylib
        /usr/lib/libSystem.B.dylib
```

Tensorを使うプログラムでは全部入りアーカイブとフレームワーク
両方がリンクされます:

```sh
$ otool -L /tmp/microgpt_tensor
/tmp/microgpt_tensor:
        /usr/lib/libc++.1.dylib
        /System/Library/Frameworks/Accelerate.framework/.../Accelerate
        /usr/lib/libSystem.B.dylib
```

### シンボルの除去

出力バイナリは自分のシンボルテーブルを読まない（ランタイムは
名前でシンボルを解決しない）ので、ビルドは2段階でこれを取り除く。
まずリンクがローカルシンボルを破棄し（`-Wl,-x` — ld64・GNU ld・
lldのいずれも解釈する。埋め込みランタイムアーカイブには
`GCC_except_table*`、テンプレートや文字列の実体化が数千個ある）、
次にプラットフォームの`strip`ツールがリンカの残したグローバル
シンボルテーブルを除去する（リリースパッケージが`culebra`本体に
掛けているのと同じ処理）。この2つで`print("hello")`のバイナリは
0.47 MB → 0.38 MBになる。ローダが必要とする動的シンボルは
どちらの段階でも残る。デバッグ用に両方を飛ばすには`--keep-symbols`
を渡す。クロスコンパイル（`--target`）の出力はリンク段階で止まる
（ホストの`strip`は他形式のオブジェクトを読めない）。`PATH`に
`strip`が無い場合も同様にエラーにはならない。

### クロスコンパイル

`--target=<triple>`でLLVMターゲットを指定。よく使うtriple:

- `x86_64-unknown-linux-gnu`
- `aarch64-unknown-linux-gnu`
- `x86_64-apple-macosx`

クロスコンパイルにはユーザ側で以下を用意する必要があります:

1. **ターゲット用sysroot**（ターゲットのC++ ヘッダ、`libc`、
   CRTファイルを含むディレクトリ）。`--sysroot=<path>`で渡す。
2. **ターゲット向けにビルドしたランタイムアーカイブ**。
   `--rt-lib=<path>`で渡す。ホスト用`libculebra_rt.a`は
   ホストtriple向けにビルドされているので使えません。

ターゲット向けランタイムをビルドするには、CMakeにターゲット
ツールチェーン（同じソースツリーを、ターゲットsysrootと`cc`
で設定）を向けてください。

現状の制約: 各ターゲット向けランタイムは同梱されず、ユーザが自前
CMake / ツールチェーンで生成する必要がある（下の例を参照）。また
`--target=<triple>`と`Tensor`の併用はrejectされる — ホストの
BLASリンクフラグはターゲットでは正しくないため。`Tensor`参照を
外すか、将来のフェーズを待つこと。

#### 例: macOS ホストから Linux x86_64 向け

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

## 2. C++ ホストへの Culebra 埋め込み

Culebraはheader-onlyです。ヘッダをincludeし、JITを使う場合は
LLVMをリンクすれば、C++ からbytecode VMやJITを駆動できます。

### 最小例

```cpp
#include <culebra.h>
#include <vm/embed.h>

int main() {
  culebra::Runtime rt;
  culebra::RuntimeScope scope(rt);
  culebra::vm::Embed embed;   // stdlib install 済み・trait 登録済み

  culebra::vm::Value val;
  std::vector<std::string> msgs;
  embed.run_source("<inline>", "1 + 2", val, msgs);
  // val.to_long() == 3
}
```

`vm::Embed`はセッションです。各`run_source`はそれまでの実行が
作ったトップレベル束縛を見ることができ、ホストは実行後にそれを
読み戻したり（`embed.global("x")`）、呼び出したり（後述の
`embed.call`）できます。独立したエンジンインスタンスごとに1つの
`Embed`を使ってください。`run_source`はソースをコピーして保持
します（parseのASTはそのコピーをstring_viewで参照し、
参照するプログラムが生きている間セッションが所有します）。
ホスト側でparse済みの入力には低レベルの
`run(ast, source, ...)`を使い、ASTのトークンが参照するバッファを
渡してください。

CLIと同じようにプログラム全体（import解決・stdlib preambleの
splice込み）を動かすには、loaderで読み込んでmodule listを
渡します:

```cpp
culebra::ModuleLoader loader;
auto modules = loader.load_program(path, entry_source, msgs);
culebra::splice_stdlib_preamble(modules);
culebra::vm::Value val;
embed.run(modules, val, msgs);
```

LLVMレーンを使う場合は`<stdlib_rt.h>`を追加し、起動時に
`culebra::install_jit_stdlib()`を1回呼んで、
`culebra::JIT::run(ast)`を使います。

### ホストプログラムのビルド

インストール手順も、リンクすべきライブラリもありません。ホストは
culebraのチェックアウトに対して直接ビルドします。ヘッダがvendor配下の
ライブラリを参照するので、それらのディレクトリもinclude pathに入ります。

VMレーンにLLVMは要りません:

```sh
c++ -std=c++23 \
    -I culebra/include \
    -I culebra/vendor/cpp-peglib \
    -I culebra/vendor/cpp-vmlib \
    -I culebra/vendor/cpp-unicodelib \
    -I culebra/vendor/cpp-tensorlib/include \
    -I culebra/vendor/stb \
    -I culebra/vendor/cpp-regexlib \
    host.cpp -lz -o host
```

どれも省けません。`vendor/stb`が無いと`font_ttf.h`で、
`vendor/cpp-regexlib`が無いと`regex.h`でビルドが止まります —
stdlibがどちらも無条件に参照するためです。`-lz`はAOTのリンクが
必要とするzlibと同じもので（[§1](#ホスト側に必要なもの)）、
`Compress`とPNGライタが参照するため、スクリプトが使うかどうかに
関わらずホスト側でリンクします。

LLVMレーン（`JIT::run`・`JIT::build_object`）では、該当ヘッダを
有効にするdefineとLLVM本体が加わります:

```sh
c++ -std=c++23 -DCULEBRA_JIT_ENABLED \
    -I "$(llvm-config --includedir)" \
    ...上の-Iリスト... \
    host.cpp $(llvm-config --ldflags --libs --system-libs) -lz -o host
```

LLVMは20以降が要ります（`CMakeLists.txt`がculebra自身のビルドに課している
下限と同じです）。複数のLLVMが入ったマシンでは、素の`llvm-config`は
`PATH`の先頭にあるものであって、目的のバージョンとは限りません。
`llvm-config-20`のようにバージョン付きの名前か、culebraをビルドした
prefix配下のものを名指ししてください。

`-DCULEBRA_JIT_ENABLED`が無ければ`jit.h`と`vm_lowering.h`は
プリプロセスの結果が空になります。`culebra.h`はそのままコンパイルでき、
VMも動き、ホストのどこもLLVMを参照しません。culebra自身のno-JITビルドが
使っているのと同じスイッチです。

`Http`と`SQLite`は`CULEBRA_HTTP_ENABLED` / `CULEBRA_SQLITE_ENABLED`を
定義しない限り入りません（`culebra`のビルドでCMakeが設定するのと同じ
名前です）。定義するならそれぞれvendor配下のディレクトリ
（`vendor/cpp-httplib`・`vendor/sqlite`）をinclude pathに足し、
固有のリンク依存も付きます。定義しなければ、この2つの名前は
どちらのレーンにも存在しません。

スクリプトが`Sys.argv`として見る値はプロセス全体のホルダです。
起動時に1回入れてください:

```cpp
culebra::sys_argv() = {"--verbose", "input.txt"};
```

### スレッディング

ランタイムは **Runtimeごと**に単一スレッドで動きます。複数の
ホストスレッドからCulebraを使うには、各スレッドが独立した処理
を持つ形にします:

```cpp
std::thread([&]{
  culebra::Runtime rt;
  culebra::RuntimeScope scope(rt);
  culebra::vm::Embed embed;
  // ... メインスレッドとは独立 ...
}).detach();
```

スレッドごとの状態（GC・例外キャリア・PRNG・defer stack・shape
registry）は、初回アクセス時に遅延生成されるthread-localの
default `Runtime`に格納されます。異なるスレッドの状態は完全に
分離されています。

PEGパーサもthread-localなので、別スレッドでの並列`parse()`も
安全です。

### 同一スレッドで複数スクリプトを動かす

`culebra::Runtime`はVMコンテキスト1個分を所有します。プラグイン
分離、サンドボックスDSL、ドキュメント毎の状態など、同一スレッド
内で複数のVMが必要なときはRuntimeを明示的に作ります:

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

`RuntimeScope`はRAIIで、デストラクタで以前のアクティブRuntime
を復元します。後で`rt_a`に戻ったときは、出ていったときの状態が
そのまま残っています。

あるRuntimeで作ったValueを別のRuntimeに渡してはいけません。
GCトラッカー、shape registry、例外キャリアは、その値を作った
Runtimeに紐付いています。

### Runtime ごとの拡張フック

`culebra::install_extension()`（およびそのラッパー
`culebra::install_jit_stdlib()`）は、現在アクティブなRuntimeを
ターゲットにします。これにより、Runtimeごとに異なるホストAPIを
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

`RuntimeScope`がアクティブで無い状態で`install_jit_stdlib()`を
呼ぶと、プロセス共通のデフォルトに書き込まれ、overrideを設定していない
すべてのRuntimeがそこにフォールバックします — これが従来の
単一VMとマルチスレッドembeddingのパスです。

### C++ からスクリプト関数を呼ぶ

実行後、トップレベルの`fn`や`let f = fn ...`はセッションに
残っています。`Embed::call`でC++ から呼べます:

```cpp
// スクリプト: fn update(x, y) { x + y * 2 }
std::vector<culebra::vm::Value> args;
args.emplace_back(int64_t{1});
args.emplace_back(int64_t{2});
auto v = embed.call("update", std::move(args));
// v.to_long() == 5
```

`call`は位置引数を位置パラメータにバインドします。`vm::Value`の
引数は呼び出しが消費します（新しいvectorを渡す）。結果は
返されたハンドルが所有します。

### スクリプトエラーの扱い

スクリプト内で発生した失敗は`culebra::CulebraError`（`<shared.h>`
で定義、`std::runtime_error`のサブクラス）として送出されます。
スクリプト側`try`/`catch`で見える構造化フィールドをそのまま保
持します:

```cpp
class CulebraError : public std::runtime_error {
public:
  std::string kind;   // 例: "TypeError"、"ArityError"、ユーザ throw した kind
  long line = 0, col = 0;
};
```

`Embed::call` / `culebra::JIT::run`などユーザコードを駆動する
経路をくるむ形でcatchします（`Embed::run*`は例外でなく`msgs`で
報告します — CLIが印字するのと同じテキストです）:

```cpp
try {
  std::vector<culebra::vm::Value> args;
  args.emplace_back(int64_t{1});
  args.emplace_back(std::string_view("oops"));
  embed.call("update", std::move(args));
} catch (const culebra::CulebraError& e) {
  std::println(stderr, "{}: {} at {}:{}",
               e.kind, e.what(), e.line, e.col);
} catch (const std::exception& e) {
  // culebra 由来でない例外（ホスト側のバグ、std 失敗など）
  std::println(stderr, "host: {}", e.what());
}
```

スクリプト側ではこれと同じ値が`catch e { ... }`の`e`として
バインドされ、`e.kind` / `e.message` / `e.line` / `e.col`プロ
パティでアクセスできます — ユーザ側仕様と標準kind一覧
（`TypeError`, `ArityError`, `IOError`, `ValueError`, `NameError`,
`IndexError`, `KeyError`, `AssertionError`, `InternalError`）は
[言語仕様 §15](language.ja.md) を参照。

`Embed::call`に届いたスクリプト側`throw expr`はホスト向けに
変換されます: 投げられたオブジェクトの`kind`/`message`がそのまま
`CulebraError`のフィールドになる（それ以外は表示形で届く）ので、
「catch CulebraError」の契約はuser throwにも通用します。
（`Embed::run*`は例外でなく`msgs`で報告します — CLIが印字する
のと同じテキストです。）

### interruptはエラーではない

Ctrl+Cやisolateのcancelはスクリプトの失敗ではなく停止の要求なので、
**`CulebraError`ではありません**:

```cpp
class Interrupted {          // 基底なし。std::exception ですらない
public:
  const char* what() const noexcept;   // "interrupted" / "isolate cancelled"
};
```

上のどちらのハンドラもこれを捕まえません。意図的です — スクリプト
エラーを報告するために書かれたハンドラがこれを捕まえると、押された
Ctrl+Cが診断メッセージになって実行が続いてしまい、フラグはone-shot
なので何も止まらなくなります。interruptに答えるのはループをhostする
コンポーネントだけで、その表明が型を名指すことです:

```cpp
try {
  embed.call("update", std::move(args));
} catch (const culebra::Interrupted&) {
  // この実行を止める。失敗として報告しない
  return;
} catch (const culebra::CulebraError& e) {
  ...
}
```

Ctrl+Cで何も止めたくない埋め込み側は、単にハンドラを設置しなければ
よい（`culebra::install_sigint_handler()`はopt-inでCLI専用）。
スクリプト側の見え方は変わりません: `catch e`は今までどおり
`e.kind == "Interrupted"`のエラーオブジェクトをバインドして再開でき、
押下はそのthrow時に消費されるので、catchしたinterruptが二度発火する
ことはありません。

### ホスト関数の定義

`Embed::define`でC++ のcallableをスクリプトから見える関数として
登録します。引数の型と戻り値の型はcallableのシグネチャから
自動推論されます。

```cpp
embed.define("log", [](std::string msg) {
  std::cout << msg << "\n";
}, {"msg"});

embed.define("host_add",
             [](int64_t a, int64_t b) { return a + b; }, {"a", "b"});
```

サポートする引数・戻り値の型: `int64_t`, `long`, `long long`, `int`,
`double`, `float`, `bool`, `std::string`, `std::string_view`,
`culebra::vm::Value`（透過）。誤った型の引数はcallableに入る前に
呼び出し側でcatch可能な`TypeError`として弾かれ、引数の個数違いは
`ArityError`になります。バインドは位置引数のみ（ホスト関数は
キーワード引数を取りません）。メソッド・ハンドル・キーワード束縛
などのより豊かな表面が要る場合は`culebra wrap`（§3）でクラスを
宣言してください — AOT含む全レーンに効きます。

### 自分の embedder から AOT 経路を組み込む

通常のembedderに`libculebra_rt.a`は無関係 — ヘッダオンリー
includeがサポート経路で、スクリプトをin-processで走らせるだけなら
archiveは要らない。必要になるのは1ケースだけ: `culebra build`と
同じく`culebra::JIT::build_object`を駆動して **standaloneバイナリを
出力したい**とき（[§1](#1-standalone-バイナリビルドculebra-build)）。
archiveは`culebra_aot_bootstrap`と、生成オブジェクトが呼ぶ
ランタイムヘルパを供給する。レイアウトは
[§4](#4-共有-runtime-archive-レイアウト)。

**入手先。** `-DCULEBRA_ENABLE_JIT=ON`でconfigureしたCMakeビルド
は`libculebra_rt.a`（と機能別archive）をビルドディレクトリに出力する。
配布される`culebra`ドライバは同じarchiveを埋め込んで持っていて、
最初の`culebra build`で`$HOME/.cache/culebra/<fingerprint>/`に
materializeする — 自前のlink手順からそのパスを指してもよい。

**オブジェクトの生成。** プログラムは`ModuleLoader`経由で読み込み、
`build_object`に渡す**前に** stdlib preambleをspliceする。この
preambleが`println` / `inspect`を定義している。（`Stringer` / `Eq` /
`Comparable`の宣言はこれには含まれない — `build_object`が`JIT::run`と
同じく自前でprependする。）

```cpp
#include <culebra.h>
#include <frontend/module_loader.h>
#include <stdlib/preamble.h>
#include <stdlib/bindings.h>

int main() {
  std::string src = "1 + 2";                  // 実際のホストではファイルから読む
  std::vector<std::string> msgs;
  culebra::ModuleLoader loader;
  auto modules = loader.load_program("prog.cul", src, msgs);
  culebra::splice_stdlib_preamble(modules);   // 必須 — 下記参照

  culebra::install_jit_stdlib();
  return culebra::JIT::build_object(modules, "prog.o", /*opt_level=*/2);
}
```

> **`splice_stdlib_preamble`を飛ばすと黙って失敗する。** オブジェクト
> の生成もlinkも通り、バイナリはexit 0で終わる — ただし何も出力
> しない。`println`が何にも解決されなかったため。診断は一切出ない
> ので、AOTバイナリが静かに何もしないならこれを疑う。

**リンク。** 生成オブジェクトに必要なのはarchiveとdead-stripと
C++ ランタイムだけ（重い機能を使わないプログラムの場合）:

```bash
# macOS
cc prog.o libculebra_rt.a -Wl,-dead_strip -Wl,-x -lc++ -o prog

# Linux (オブジェクトは非 PIC なので PIE 既定のディストロでは -no-pie が要る)
cc prog.o libculebra_rt.a -Wl,--gc-sections -Wl,-x -no-pie -lstdc++ -lm -o prog

# どちらも: 残ったシンボルテーブルを落とす (`culebra build` の既定動作)
strip prog
```

機能のnamespace（`Tensor` / `Http` / `Compress` / `SQLite` / `Regex` /
`Canvas` / …、[§4](#4-共有-runtime-archive-レイアウト) の表）を
参照するプログラムは、その機能のarchiveを **force-load** する必要がある — Mach-Oなら
`-Wl,-force_load,<archive>`、ELFなら`-Wl,--whole-archive <archive>
-Wl,--no-whole-archive` — さらにその機能の外部ライブラリも要る。
単にappendしても効かない: base archiveの弱シンボルスタブが既に
シンボルを満たしてしまい、メンバがloadされない
（gatingは [§4](#4-共有-runtime-archive-レイアウト)）。

これらの規則を書き写すより、ドライバを一度
`CULEBRA_VERBOSE=1 culebra build prog.cul -o prog`で走らせるのが早い。
実際に使った`link:`コマンドラインが機能archive込みでそのまま
表示され、embedderに必要なのも同じ形。 (`--rt-lib=<path>`はCLI側
の上書き用オプションで、crossビルドしたarchiveなどを指すのに使う。)

`CULEBRA_RT_DEFINE_RUNTIME`マクロは、`CULEBRA_RT_INLINE`タグ付き
ヘルパを`inline`から`extern "C"`に切り替えてarchive側TUが唯一
のownerになるようにする。ヘッダオンリーembedderは **絶対に
defineしてはいけない**。AOT archiveの生成元TU
(`src/runtime/culebra_rt.cc`) のみでdefineされるべき。

### スモークテスト

リポジトリには契約を検証する小さなサンプルが含まれます:

* [`tests/embedding/mt_smoke.cc`](../tests/embedding/mt_smoke.cc) —
  4つのホストスレッドがそれぞれtry/catch付きスクリプトをparse +
  Embedセッションを実行、加えてJITパスでも4スレッド。合計240並行実行。
* [`tests/embedding/mi_smoke.cc`](../tests/embedding/mi_smoke.cc) —
  1スレッド内で2つのRuntime（それぞれ自分のEmbedを持つ）を
  交互に切替え、独立したPRNG状態
  と独立したJITフックセットを検証。
* [`tests/embedding/define_smoke.cc`](../tests/embedding/define_smoke.cc)
  — `Embed::define`を経由してスクリプトと`Embed::call`両方から
  C++ 関数を呼び、推論型による誤引数の拒否も確認。

## 3. C++ ライブラリのラッピング（`culebra wrap`）

`culebra wrap`は、あなたのC++ クラスをビルトインとして組み込んだ
**拡張culebraバイナリ**を作ります — ランタイムのforkもplugin
ABIも不要です。短い宣言TUを書くとC++ コンパイラがglueを実体化し
（pybind11流）、`--vm`・`--jit`・拡張バイナリの`culebra build`
が作るAOTバイナリのすべてで同一に動きます。

### 宣言する

手元のheader-onlyクラスに対して:

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

宣言TUを1つ書きます:

```cpp
// vec2_binding.cpp
#include <interop/wrap.h>
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
専用のthunkがコンパイルされます。引数名は省略可能で、エラーメッセージと
実行時のキーワード束縛に使われます。

宣言するのは構築（`ctor`）だけで、破棄は宣言しません — `.dtor`ビルダーは
ありません。ラップした型の`~T()`は、ハンドルが継承する確定drop機構
（下記参照）が自動的に呼び出すので、書くのはC++ のデストラクタだけです。

### ビルドする

```sh
culebra wrap vec2_binding.cpp -o ext-culebra
# ビルド済みライブラリにリンクする場合:
culebra wrap mylib_binding.cpp --link "-L/opt/mylib/lib -lmylib" -o ext-culebra
```

`culebra wrap`はculebraのソースツリー（このバイナリのビルド元、または
`$CULEBRA_HOME`）にあなたのTUを加えて再ビルドし、`~/.cache/culebra-wrap/`
にキャッシュします。ccacheがあれば実質「宣言のコンパイル + relink」で
済みます。`--lto`で最適化バイナリ（ビルドは遅くなります）。

### 使う — 全 backend で

```culebra
# doctest: skip
let v = Geo.Vec2.new(3.0, 4.0)
inspect(v.len())  # => 5
v.scale(2.0)
let u = v.unit()  # 値返し -> 新しい所有インスタンス
v.drop()          # ~Vec2 が「いま」走る（確定的）
v.len()           # !! ClosedError
```

ラップされたインスタンスは完全なlifetimeモデルを持つリソースです:
scope終端の確定drop（循環込み）、冪等な明示`drop()`、use-after-dropの
`ClosedError`。`ext-culebra build script.cul`のAOTバイナリにも
バインディングが載ります。

ラップしたC++ボディ（ctor・メソッド・staticのいずれも）が例外を投げても
プロセスの外には出ません。`std::exception`は`what()`を持つcatch可能な
`RuntimeError`になり、位置は他のエラーと同じく呼び出し箇所です。ボディが
`culebra::CulebraError`を投げた場合はkindとメッセージがそのまま通ります。

バインディングとラップ対象ライブラリ（`--link`）がAOTバイナリに載るのは、
スクリプトがラップ名前空間を参照したときだけです。`ext-culebra`でビルドしても
ラップクラスを一切使わないプログラムは、ラップ対象ライブラリを一切リンクせず、
素の`culebra build`と同じサイズになります。判定は保守的な識別子マッチ
（`Geo`等）なので、過剰リンクはあっても不足リンクはありません。これは
[§4](#4-共有-runtime-archive-レイアウト) で説明する`Tensor` / `Http`と
同じusage-gatingを`libculebra_rt_wrap.a`に適用したものです。

### マーシャリング

| C++ | Culebra |
|---|---|
| `long` / `int` | `Long` |
| `double` / `float` | `Float` |
| `bool` | `Bool` |
| `std::string` / `std::string_view` / `const char*` | `String` |
| 値返しの`T`・`std::unique_ptr<T>` | ラップ済み`T`の所有インスタンス |
| `std::shared_ptr<T>` | shareを1つ保持するインスタンス |

ラップ済みクラスの`T&` / `const T&`返しは`.method`では**コンパイル
エラー**です — 参照は所有形状ではありません。`.borrowed_method`で
宣言してください:

```cpp
.borrowed_method<&Box::inner>("inner")     // Counter& inner()
```

結果は**借用ハンドル**です: 所有しない（dropはno-op）、存在する間は
親を生かし、アクセスごとに検証されます — 親が明示dropされた、または
借用取得後に非constメソッドで変更された（各インスタンスはgeneration
カウンタを持ち、非const dispatchが自動でbump）場合、解放/再配置済み
メモリに触れる代わりに`ClosedError`になります:

```culebra
let b = __Foreign.Box.new(3)
let c = b.inner()
b.reset(9)  # 非 const -> generation bump
c.value()   # !! ClosedError
```

借用を無効化しないことが確実な非constメソッドはopt-outできます:
`.method<&T::touch>("touch", {}, culebra::wrap_policy::preserves_borrows)`。
const性やこのフラグの誤宣言は著者の契約違反（sol2/pybind11と同じ
建付け）ですが、その場合も「staleエラーの過剰/欠落」であってculebra
側のメモリ非安全には絶対になりません。

コンテナ（`std::vector`/`std::map`）とコールバックは未対応です。

このワークフローのend-to-end検証は`tests/wrap_test.sh`が行います。

## 4. 共有 runtime archive レイアウト

上記3つのワークフローはすべて、生成バイナリや埋め込みバイナリが
LLVM依存を持たずに済むようstatic **runtime archive** を出荷します。
CMakeは`-DCULEBRA_ENABLE_JIT=ON`で、base archive＋ 重い機能ごとに
1つ（2^Nの組合せでなくN+1）を出力します:

| Archive | 内容 |
|---|---|
| `libculebra_rt.a` | base — 全部入りだが各機能のchokeは**弱シンボルのスタブ**（ここから呼べるコードはBLAS・OpenSSL・zlib・sqlite3・正規表現エンジンに到達しない）。サブプロセス層、画像デコーダ（stb_image / stb_truetype）、cpp-peglibパーサジェネレータ（`Peg`）は外部ライブラリを引かないのでこのarchiveに直接コンパイルされ、chokeでなく下のnamespace-group単位のdead-strippingに委ねる——3つのうち`Peg`だけは未使用でも固定約53 KBのpeglib RTTI/vtableメタデータを残す（§1参照） |
| `libculebra_rt_tensor.a` | 強いtensor choke（BLAS / Accelerateを引く） |
| `libculebra_rt_http.a` | 強いhttp choke（OpenSSL + zlibを引く） |
| `libculebra_rt_compress.a` | 強いcompress choke（zlibを引く。`to_png`もこれに乗る） |
| `libculebra_rt_sqlite.a` | 強いsqlite choke＋sqlite3 amalgamation |
| `libculebra_rt_regex.a` | 強いregex choke（cpp-regexlibエンジン、約320 KB） |
| `libculebra_rt_foreign.a` | foreign objectのテストが書かれている`__Foreign` wrapフィクスチャ（静的な`wrap<T>`レジストラなので専用archiveが要る） |
| `libculebra_rt_canvas.a` | raylibのwindowバックエンド（windowビルドのみ。baseはheadlessスタブを持つ） |
| `libculebra_rt_scene.a` | Sceneのwrap registrar（raylibを引く。baseには一切入っていない） |
| `libculebra_rt_webview.a` | Webviewのwrap registrar（OSのWebViewフレームワーク。baseには一切入っていない） |
| `libculebra_rt_wrap.a` | `culebra wrap`のバインディング |

`culebra build`（および拡張された`ext-culebra build`バイナリ）は
常にbaseをlinkし、ソースASTがそのnamespace（`Tensor` / `Http` /
`Compress` / `SQLite` / `Regex` / `Canvas` / `Scene` /
`Webview`、あるいはラップされたnamespace）を参照する時だけ機能
archiveを **force-load** し、同じ条件でその外部ライブラリ（BLAS /
OpenSSL / zlib / …）を付けます。強いchokeがbaseの弱スタブを上書きする
ので、これらの機能をどれも使わないプログラムはどれもlinkしません。

標準ライブラリ自身のdispatch表も、namespace単位で同じようにlinkされ
ます。各namespaceの行（`Math.abs`や`Isolate.spawn`が実行時に解決する
adapter）とその正準シグネチャは、baseアーカイブが定義はするが自分では
参照しない*group*を成し、プログラムのオブジェクトがgroupの一覧（全stdlib
namespace分。ソースが名指ししないものはnullエントリ）を運びます。名指し
されたgroupの行だけがdead-strippingを生き残り、それらだけが到達していた
ものも一緒に落ちます。走査はプログラムとspliceされたpreambleに対する字句
単位なので、`let m = Math; m.abs(x)`は`Math`を名指ししたと数え、lazy
モジュール内の`_Canvas`は`Canvas`がそのモジュールを引き込んだ時点で数え
ます。isolate間の値転送コード（値のシリアライズ、channelのendpoint、
`Shared`のview）も同じ仕組みに乗ります: 汎用のプロパティ／添字読み取り
経路は`Shared` viewの読み手にviewのコンストラクタが設定するフック経由で
届き、シンボルでは参照しないので、そのコードを参照するのは`Shared` /
`Isolate` / `Parallel` / `Net`のadapterだけになり、それらのgroupと一緒に
落ちます。ランタイム自身のメッセージは`std::format`ではなくculebraの小さな
フォーマッタ（`include/base/format.h`）で組み立てます。`std::format`の実装は
1つの引数visitorが整数・浮動小数・文字列のformatterをまとめて名指しするので
リンク単位が1ブロックになり、到達する呼び出しが1つあるだけでhelloが15%
太るためです。補間の書式指定（`"{x:.2f}"`）を書いたプログラムは今も
`std::format`をリンクします — その書式こそ`std::format`のミニ言語だからです。
機能軸と合わせて、これが`print("hello")`のバイナリを0.4 MB未満
に保っています。走査が見落としたnamespaceは黙って`nil`に読めたりはせず、
到達した時点でそのnamespace名を含む`InternalError`になります。

これらのアーカイブはcpp-embedlibによって **`culebra`ドライバに
直接埋め込まれています** — ドライバは単体で完結する1バイナリで、
サイドカーの`.a`ファイルを別途インストールする必要はありません。
deflate圧縮して格納しており、ドライバ内で33.8 MBのところ6.9 MB
です。`culebra build`の初回呼び出し時に必要なアーカイブを
`$HOME/.cache/culebra/<fingerprint>/lib*.a`へ展開し、2回目以降は
キャッシュを再利用します。fingerprintは埋め込みアーカイブのコンテ
ンツハッシュなので、`culebra`を再ビルドすると自動的に旧版のキャッ
シュと分離されます。

機能軸が**丸ごと**リンクするサードパーティ製の静的ライブラリも、
同じ置き場に自分の名前で同居します: `Http`のOpenSSL（`libssl.a` /
`libcrypto.a`）、`Canvas`のウィンドウバックエンドと`Scene`が使う
vendored SDL3 / raylib（`libSDL3.a` / `libraylib.a`）。これらは
ランタイムアーカイブの一部ではありません — `-l`で渡すと参照された
分しか引かれず、それでは目的を果たさないからです。かといってパスで
名指すこともできません: それぞれが置かれていたディレクトリは
ドライバをビルドしたマシンのもの（ディストリのディレクトリ、
Homebrewのprefix、MSYS2のツリー、CIランナーの依存キャッシュ）で、
リンクが走るのはユーザーのマシンだからです。そこで各機能軸のリンク
断片はこれらを名前で書き（`@libssl.a@`）、`culebra build`が`cc`へ
コマンドを渡す前に同じキャッシュへ解決します。素の`-l`のまま残すのは
どのマシンにもあるもの — `-lz`・`-lstdc++`・`-lpthread`・OSの
フレームワーク — だけです。断片にパスを焼き込むことはCMakeがconfigure
時に拒否し、`tools/checks/check_aot_link_portability.sh`が実際のリンク行を
読み返して1本も無いことを証明します。他のAOTテストはすべてビルド
ツリーの中で走るので、焼き込まれたパスがたまたま存在してしまうためです。

`culebra build --target=<triple>`は現状ホストアーカイブのみです —
埋め込み/キャッシュされたアーカイブはホスト向けにビルドされています。
クロスターゲットには`--rt-lib=<path>`で対応するarchiveを指定して
ください（ターゲットごとのauto-buildはroadmap。手動クロスビルドの
手順は [§1](#1-standalone-バイナリビルドculebra-build) 参照）。
