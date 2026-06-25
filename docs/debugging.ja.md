# デバッグ

Culebra は [Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol/)
(DAP) サーバを同梱しています。これにより、DAP 対応エディタ — VSCode・Neovim
(nvim-dap)・Vim (vimspector)・Zed・Emacs (dap-mode)・Helix など — でブレークポイント・
ステップ実行・変数の確認をビジュアルに行えます。1 つのアダプタで全エディタに対応します。

```
culebra dap        # stdin/stdout で DAP を話す
```

手で実行することは稀で、通常はエディタが起動します（下記）。

## 仕組み

アダプタはプログラムを**インタプリタ**で実行し、その statement 単位のフックで
ブレークポイント・`debugger` 文・ステップ時に一時停止します。その間 DAP ループが
エディタの要求に応答し、再開します。デバッグはインタプリタベースなので、スクリプトは
素の `culebra`（`--jit` ではなく）で実行してください。JIT/AOT は機械語にコンパイルされ
ソースレベルデバッグできません。

プログラムの `stdout`/`stderr` はエディタのデバッグコンソールに `output` イベントとして
転送されるので、プロトコルと混ざりません。

### 対応している機能

- **行ブレークポイント** と `debugger` 文
- **ステップ**: continue・step over (`next`)・step in・step out・エントリ停止
- **コールスタック**（現在フレーム）+ ソース位置
- **変数**: 停止地点で参照中のローカル
- **プログラム出力** をデバッグコンソールに

未対応（ブレークポイント + 確認で代替）: 変数の編集 (`setVariable`)・watch / `evaluate`
式・条件付きブレークポイント。

## VSCode

`culebra` デバッグタイプを登録しアダプタを指す小さな拡張を追加します（拡張は登録のみ
で、ロジックは全て `culebra dap` 側＝同じアダプタが全エディタで動く）。`package.json`:

```jsonc
"contributes": {
  "debuggers": [{
    "type": "culebra",
    "label": "Culebra",
    "program": "culebra",
    "args": ["dap"]
  }]
}
```

プロジェクトの `.vscode/launch.json`:

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

<kbd>F5</kbd> で開いているファイルをデバッグ。

## Neovim (nvim-dap)

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
```

`.cul` の filetype を `culebra` に設定（autocommand 等）すると
`dap.configurations.culebra` が適用されます。

## Vim (vimspector)

プロジェクトルートの `.vimspector.json`:

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

## Zed

Zed のデバッガ（内蔵 DAP クライアント）はデバッグ設定でカスタムアダプタを取れます。
`culebra dap` を指し、`program` にデバッグ対象ファイルを与えた `launch` 要求を設定します。
（Zed のデバッガは VSCode/nvim-dap より新しく、設定スキーマは今後変わる可能性あり。）

## 補足

- デバッグ対象ファイルとその引数は `culebra dap` のコマンドラインではなく `launch`
  要求の `program` フィールドで渡します。
- ブレークポイントは正準（シンボリックリンク解決済み）パスで照合するので、シンボリック
  リンク下のファイルに置いたブレークポイントも有効です。
- ソース中の `debugger` 文はブレークポイントに関係なく停止します（一時的な停止に便利）。
