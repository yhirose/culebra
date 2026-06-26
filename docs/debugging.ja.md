# デバッグ

Culebra は [Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol/)
(DAP) サーバを同梱しています。これにより、DAP 対応エディタ — VSCode・Vim
(vimspector)・Zed・Emacs (dap-mode)・Helix など — でブレークポイント・
ステップ実行・変数の確認をビジュアルに行えます。1 つのアダプタで全エディタに対応します。

```
culebra dap        # stdin/stdout で DAP を話す
```

手で実行することは稀で、通常はエディタが起動します（下記のエディタ別セットアップ参照）。

## 前提

- **`culebra` を実行できること** — `PATH` を通すか、下記の各設定で絶対パスを指定。
  （動作確認: `echo | culebra dap` が DAP 入力待ちでハングすれば OK、`Ctrl+C` で抜ける。
  エラーになる場合はまずパスを直す。）
- **デバッグはインタプリタで動く** — `--jit` を付けない。アダプタはプログラムを
  インタプリタモードで実行する。JIT/AOT は機械語にコンパイルされソースレベルデバッグ
  できない。

## 仕組み

アダプタはプログラムを**インタプリタ**で実行し、その statement 単位のフックで
ブレークポイント・`debugger` 文・ステップ時に一時停止します。その間 DAP ループが
エディタの要求に応答し、再開します。

プログラムの `stdout`/`stderr` はエディタのデバッグコンソールに `output` イベントとして
転送されるので、プロトコルと混ざりません。

### 対応している機能

- **行ブレークポイント**・**条件付きブレークポイント**（式が真のときだけ停止、
  例 `i == 3`）・`debugger` 文
- **ステップ**: continue・step over (`next`)・step in・step out・エントリ停止
- **コールスタック**: 名前付きの多フレームスタック（`inner` ← `outer` ←
  `main`）+ 各フレームのソース位置
- **変数**: 選択したフレームで参照中のローカル — コールスタックでフレームを
  選ぶとそのフレームのローカルを参照できる
- **watch / evaluate**: 選択フレームで式を評価（watch パネル・hover・デバッグ
  コンソール）— 例 `x + y`・`arr[0]`・関数呼び出し
- **変数の編集 (set variable)**: 選択フレームの `mut` 変数の値を変更（`let`
  （不変）は拒否）。変更は実行中のプログラムに反映される
- **プログラム出力** をデバッグコンソールに

## VSCode

VSCode は `.cul` のハイライトと `culebra` デバッグタイプ登録のための小さな拡張が必要です
（デバッグは登録のみで、ロジックは全て `culebra dap` 側＝同じアダプタが全エディタで動く）。
公開は不要。リポジトリに雛形とインストーラを `misc/vscode/` に同梱しています。

1. 拡張をインストール:

   ```sh
   misc/vscode/install.sh
   ```

   拡張を `.vsix` にパッケージし（`build-vsix.sh`、npm 不要）、`code --install-extension`
   でインストールします＝VS Code が公式にサポートする方法。（`~/.vscode/extensions` に
   フォルダを直接コピーする方法は**非サポート**で、認識されないことが多い。）`culebra` が
   `PATH` 上にあればデバッグアダプタ設定に絶対パスを埋め込みます。`code-insiders`/`cursor`/
   `codium` でも動作し、いずれの CLI も無ければ Extensions ビューから `.vsix` を入れる手順を
   案内します。シンタックスハイライトは `.cul` を開くだけで有効（以降の手順はデバッグ用のみ）。
   文法のキーワード一覧は `just sync-grammar` がパーサから生成（Vim 構文ファイルと同一ソース）
   するため言語からドリフトしません。
2. VSCode を**完全終了**（<kbd>Cmd</kbd>+<kbd>Q</kbd>）して再起動 — 入れたての拡張は
   ウィンドウ再読み込みだけでは拾われないことがあります。
3. プロジェクトに `.vscode/launch.json`:

   ```jsonc
   {
     "version": "0.2.0",
     "configurations": [{
       "type": "culebra",
       "request": "launch",
       "name": "Debug current file",
       "program": "${file}",
       "cwd": "${workspaceFolder}",
       "stopOnEntry": false
     }]
   }
   ```
4. `.cul` を開き、ガター（行番号の左）をクリックでブレークポイント → <kbd>F5</kbd>。

> **拡張自体を作り込む場合**は、変更のたびに `.vsix` を入れ直す代わりに、`misc/vscode/` を
> VSCode で開き `extensionHost` の launch 構成で <kbd>F5</kbd> を押すと、拡張がライブロード
> された別ウィンドウ（*Extension Development Host*）が開き、そこで `.cul` をデバッグできます。
> 単に*使いたいだけ*なら上の `install.sh` の方が簡単です。

## Vim (vimspector)

拡張は不要 — [vimspector](https://github.com/puremourning/vimspector) を導入し、
プロジェクトルートに `.vimspector.json`（vimspector はプロジェクト単位）:

```json
{
  "configurations": {
    "Debug file": {
      "adapter": { "command": ["culebra", "dap"] },
      "configuration": {
        "request": "launch",
        "program": "${file}",
        "stopOnEntry": false
      }
    }
  }
}
```

ブレークポイントは `<F9>`、開始は `<F5>`（vimspector のデフォルト）。
シンタックスハイライトは `misc/vim/install-vim-syntax.sh` を実行してください。

## Zed

Zed は DAP クライアントを内蔵（デバッグに拡張不要）。次でセットアップ:

```sh
misc/zed/install.sh            # このプロジェクトの .zed/debug.json を書く
misc/zed/install.sh --global   # またはユーザ全体の ~/.config/zed/debug.json
```

`launch` シナリオが `culebra dap` を指します（`culebra` が `PATH` 上なら絶対パス
を埋め込み）。`.cul` を開いてブレークポイントを置き、デバッグパネルから
"Debug current Culebra file" を開始してください。生成される設定:

```jsonc
[
  {
    "label": "Debug current Culebra file",
    "adapter": "culebra",
    "request": "launch",
    "command": "culebra",
    "args": ["dap"],
    "program": "$ZED_FILE",
    "stopOnEntry": false
  }
]
```

> Zed のデバッガは VSCode より新しく、設定スキーマは流動的です。上記のキーは
> バージョンで異なる可能性があります。本質はどこでも同じ: `culebra dap` を起動・
> `request: launch`・`program` にファイルを指定。Zed のシンタックスハイライトは別途
> tree-sitter 拡張が必要（ここでは未提供）で、デバッグはそれ無しで動きます。

## 補足

- デバッグ対象ファイルとその引数は `culebra dap` のコマンドラインではなく `launch`
  要求の `program` フィールドで渡します。
- ブレークポイントは正準（シンボリックリンク解決済み）パスで照合するので、シンボリック
  リンク下のファイルに置いたブレークポイントも有効です。
- ソース中の `debugger` 文はブレークポイントに関係なく停止します（設定不要の一時停止に
  便利）。
