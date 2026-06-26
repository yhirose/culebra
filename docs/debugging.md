# Debugging

Culebra ships a [Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol/)
(DAP) server, so you can set breakpoints, step, and inspect variables visually
in any DAP-capable editor — VSCode, Neovim (nvim-dap), Vim (vimspector), Zed,
Emacs (dap-mode), Helix, and others. One adapter serves every editor.

```
culebra dap        # speak DAP over stdin/stdout
```

You rarely run this by hand; your editor launches it (see the per-editor setup
below).

## Prerequisites

- **`culebra` must be runnable** — either on your `PATH` or referenced by an
  absolute path in the editor config below. (Quick check: `echo | culebra dap`
  should hang waiting for DAP input; `Ctrl+C` to quit. If it errors, fix the
  path first.)
- **Debugging runs in the interpreter** — don't pass `--jit`. The adapter runs
  your program in interpreter mode; the JIT/AOT backends compile to machine code
  and aren't source-debuggable.

## How it works

The adapter runs your program in the **interpreter** and uses its per-statement
hook to pause on a breakpoint, a `debugger` statement, or a step, while the DAP
loop answers the editor's requests and resumes execution.

Your program's `stdout`/`stderr` is forwarded to the editor's debug console as
`output` events, so it never collides with the protocol.

### Supported

- **Line breakpoints**, **conditional breakpoints** (break only when an
  expression is truthy, e.g. `i == 3`), and the `debugger` statement
- **Stepping**: continue, step over (`next`), step in, step out; stop on entry
- **Call stack**: a named multi-frame stack (`inner` ← `outer` ← `main`), each
  frame with its source location
- **Variables**: in-scope locals of the selected frame — pick a frame in the
  call stack to inspect its own locals
- **Watch / evaluate**: evaluate an expression in the selected frame (watch
  panel, hover, debug console) — e.g. `x + y`, `arr[0]`, a function call
- **Set variable**: edit a `mut` variable's value in the selected frame (an
  immutable `let` binding is rejected); the change takes effect in the running
  program
- **Program output** in the debug console

## VSCode

VSCode needs a tiny extension to highlight `.cul` files and register the
`culebra` debug type (debugging is pure registration — all the logic lives in
`culebra dap`, so the same adapter then works in every editor). Publishing
isn't required: the repo ships a template under `misc/vscode/` with an
installer.

1. Install the extension:

   ```sh
   misc/vscode/install.sh
   ```

   This packages the extension as a `.vsix` (via `build-vsix.sh`, no npm needed)
   and installs it with `code --install-extension` — the method VS Code
   supports. (Copying the folder into `~/.vscode/extensions` directly is *not*
   supported and is often not detected.) If `culebra` is on your `PATH`, its
   absolute path is baked into the debug adapter config. The script also works
   with `code-insiders` / `cursor` / `codium` if that's your CLI; if none is on
   `PATH`, it tells you how to install the `.vsix` from the Extensions view
   instead. Syntax highlighting then applies to any `.cul` file with no further
   setup; the steps below are only needed for debugging. The grammar's keyword
   list is generated from the parser by `just sync-grammar` (the same source as
   the Vim syntax file), so it never drifts from the language.
2. Fully quit VSCode (<kbd>Cmd</kbd>+<kbd>Q</kbd>) and reopen — a freshly
   installed extension may not be picked up by a window reload alone.
3. In your project, add `.vscode/launch.json`:

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
4. Open a `.cul` file, click in the gutter to set a breakpoint, and press
   <kbd>F5</kbd>.

> **Iterating on the extension itself?** Instead of reinstalling the `.vsix`
> on every change, open `misc/vscode/` in VSCode and press <kbd>F5</kbd> with an
> `extensionHost` launch config — that opens a separate *Extension Development
> Host* window with the extension loaded live, where you debug your `.cul`
> project. For just *using* the extension, `install.sh` above is simpler.

## Neovim (nvim-dap)

No extension needed. The quickest setup:

```sh
misc/vim/install.sh
```

This installs syntax highlighting (filetype `cul`) and writes a sourceable
nvim-dap config to `~/.config/nvim/lua/culebra_dap.lua` (the `culebra` path is
baked in when on `PATH`). Enable it by adding one line to your `init.lua`:

```lua
require("culebra_dap")
```

Toggle a breakpoint with `:lua require('dap').toggle_breakpoint()` and start
with `:lua require('dap').continue()`. Add
[nvim-dap-ui](https://github.com/rcarriga/nvim-dap-ui) for a variables/stack
panel.

To wire it by hand instead, the equivalent config is:

```lua
local dap = require("dap")
dap.adapters.culebra = { type = "executable", command = "culebra", args = { "dap" } }
-- Keyed on the `cul` filetype (the syntax install sets that for *.cul):
dap.configurations.cul = {
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

## Vim (vimspector)

No extension needed — install [vimspector](https://github.com/puremourning/vimspector)
and add `.vimspector.json` to the project root (vimspector configs are
per-project). `misc/vim/install.sh` prints this snippet with your `culebra`
path filled in:

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

Set a breakpoint with `<F9>` and start with `<F5>` (vimspector defaults).
For syntax highlighting only, `misc/vim/install-vim-syntax.sh` is enough.

## Zed

Zed has a built-in DAP client (no extension needed for debugging). Set it up
with:

```sh
misc/zed/install.sh            # writes .zed/debug.json for the current project
misc/zed/install.sh --global   # or the user-level ~/.config/zed/debug.json
```

This points a `launch` scenario at `culebra dap` (the `culebra` path is baked
in when on `PATH`). Then open a `.cul` file, set a breakpoint, and start the
"Debug current Culebra file" scenario from the debug panel. The generated
config is:

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

> Zed's debugger is newer than VSCode/nvim-dap and its config schema is still
> evolving — the keys above may differ in your version. The essentials are the
> same everywhere: launch `culebra dap`, `request: launch`, and a `program`
> pointing at the file. Syntax highlighting in Zed needs a separate tree-sitter
> extension (not provided here); debugging works without it.

## Notes

- The program to debug and its arguments come from the `launch` request's
  `program` field, not the `culebra dap` command line.
- Breakpoints are matched by canonical (symlink-resolved) path, so a breakpoint
  set on a file under a symlinked directory still binds.
- A `debugger` statement in source forces a stop regardless of breakpoints —
  handy for a one-off pause without configuring anything.
