# デバッグ

Culebra は [Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol/)
(DAP) サーバを同梱しています。これにより、DAP 対応エディタ — VSCode・Neovim
(nvim-dap)・Vim (vimspector)・Zed・Emacs (dap-mode)・Helix など — でブレークポイント・
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

- **行ブレークポイント** と `debugger` 文
- **ステップ**: continue・step over (`next`)・step in・step out・エントリ停止
- **コールスタック**（現在フレーム）+ ソース位置
- **変数**: 停止地点で参照中のローカル
- **watch / evaluate**: 停止フレームで式を評価（watch パネル・hover・デバッグ
  コンソール）— 例 `x + y`・`arr[0]`・関数呼び出し
- **変数の編集 (set variable)**: `mut` 変数の値を変更（`let`（不変）は拒否）。
  変更は実行中のプログラムに反映される
- **プログラム出力** をデバッグコンソールに

未対応: 条件付きブレークポイントと多フレームの名前付きコールスタック。

## VSCode

VSCode は `culebra` デバッグタイプを登録する小さな拡張が必要です（登録のみで、ロジックは
全て `culebra dap` 側＝同じアダプタが全エディタで動く）。公開は不要。リポジトリに雛形と
インストーラを `misc/vscode/` に同梱しています。

1. 拡張をインストール:

   ```sh
   misc/vscode/install.sh
   ```

   `misc/vscode/package.json` を `~/.vscode/extensions/culebra-debug/` にコピーし、
   `culebra` が `PATH` 上にあればその絶対パスを埋め込みます。（手作業でやるなら、その
   フォルダを自分でコピー。`culebra` が `PATH` に無ければ `program` を絶対パスに編集。）
2. VSCode を再読み込み（コマンドパレット → **Developer: Reload Window**）。
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

> **拡張自体を作り込む場合**は、`~/.vscode/extensions` にコピーする代わりに、拡張フォルダを
> VSCode で開き `extensionHost` の launch 構成で <kbd>F5</kbd> を押すと、拡張がロードされた
> 別ウィンドウ（*Extension Development Host*）が開き、そこで `.cul` をデバッグできます。
> 変更のたびに入れ直す必要が無くなります。単に*使いたいだけ*なら上の
> `~/.vscode/extensions` への配置の方が簡単です。

## Neovim (nvim-dap)

拡張は不要 — [nvim-dap](https://github.com/mfussenegger/nvim-dap) を設定するだけ:

```lua
local dap = require("dap")
dap.adapters.culebra = { type = "executable", command = "culebra", args = { "dap" } }
dap.configurations.culebra = {
  {
    type = "culebra",
    request = "launch",
    name = "Debug file",
    program = "${file}",
    cwd = "${workspaceFolder}",
    stopOnEntry = false,
  },
}
-- 上の設定を効かせるため `.cul` の filetype を `culebra` に:
vim.filetype.add({ extension = { cul = "culebra" } })
```

ブレークポイントは `:lua require('dap').toggle_breakpoint()`、開始は
`:lua require('dap').continue()`。変数/スタックパネルが欲しければ
[nvim-dap-ui](https://github.com/rcarriga/nvim-dap-ui) を追加。

## Vim (vimspector)

拡張は不要 — [vimspector](https://github.com/puremourning/vimspector) を導入し、
プロジェクトルートに `.vimspector.json`:

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
シンタックスハイライト（任意）: `misc/vim/install-vim-syntax.sh` を実行。

## Zed

Zed は DAP クライアントを内蔵しています。デバッグ設定で `culebra dap` を起動し、
`program` にデバッグ対象ファイルを与えた `launch` 構成を指定します:

```jsonc
[
  {
    "label": "Debug file",
    "adapter": "culebra",
    "request": "launch",
    "command": "culebra",
    "args": ["dap"],
    "program": "$ZED_FILE",
    "stopOnEntry": false
  }
]
```

> Zed のデバッガは VSCode/nvim-dap より新しく、設定スキーマは流動的です。上記のキーは
> バージョンで異なる可能性があります。本質はどこでも同じ: `culebra dap` を起動・
> `request: launch`・`program` にファイルを指定。

## 補足

- デバッグ対象ファイルとその引数は `culebra dap` のコマンドラインではなく `launch`
  要求の `program` フィールドで渡します。
- ブレークポイントは正準（シンボリックリンク解決済み）パスで照合するので、シンボリック
  リンク下のファイルに置いたブレークポイントも有効です。
- ソース中の `debugger` 文はブレークポイントに関係なく停止します（設定不要の一時停止に
  便利）。
