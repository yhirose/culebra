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

- **Line breakpoints** and the `debugger` statement
- **Stepping**: continue, step over (`next`), step in, step out; stop on entry
- **Call stack** (current frame) with source location
- **Variables**: in-scope locals at the stop point
- **Program output** in the debug console

Not yet supported (use breakpoints + inspection): editing variables
(`setVariable`), watch/`evaluate` expressions, and conditional breakpoints.

## VSCode

VSCode needs a tiny extension to register the `culebra` debug type (it's pure
registration — all the logic lives in `culebra dap`, so the same adapter then
works in every editor). Publishing isn't required: the repo ships a template
under `misc/vscode/` with an installer.

1. Install the extension:

   ```sh
   misc/vscode/install.sh
   ```

   This copies `misc/vscode/package.json` into
   `~/.vscode/extensions/culebra-debug/` and, if `culebra` is on your `PATH`,
   bakes in its absolute path. (To do it by hand instead, copy that folder
   yourself; edit `program` to an absolute path if `culebra` isn't on `PATH`.)
2. Reload VSCode (Command Palette → **Developer: Reload Window**).
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

> **Iterating on the extension itself?** Instead of copying it into
> `~/.vscode/extensions`, open the extension folder in VSCode and press
> <kbd>F5</kbd> with an `extensionHost` launch config — that opens a separate
> *Extension Development Host* window with the extension loaded, where you debug
> your `.cul` project. This avoids reinstalling on every change; for just
> *using* the debugger, the `~/.vscode/extensions` install above is simpler.

## Neovim (nvim-dap)

No extension needed — just configure [nvim-dap](https://github.com/mfussenegger/nvim-dap):

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
-- `.cul` files must have filetype `culebra` for the config above to apply:
vim.filetype.add({ extension = { cul = "culebra" } })
```

Toggle a breakpoint with `:lua require('dap').toggle_breakpoint()` and start
with `:lua require('dap').continue()`. Add
[nvim-dap-ui](https://github.com/rcarriga/nvim-dap-ui) for a variables/stack
panel.

## Vim (vimspector)

No extension needed — install [vimspector](https://github.com/puremourning/vimspector)
and add `.vimspector.json` to the project root:

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
Optional syntax highlighting: run `misc/vim/install-vim-syntax.sh`.

## Zed

Zed has a built-in DAP client. Point its debug configuration at `culebra dap`
with a `launch` request whose `program` is the file to debug:

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

> Zed's debugger is newer than VSCode/nvim-dap and its config schema is still
> evolving — the keys above may differ in your version. The essentials are the
> same everywhere: launch `culebra dap`, `request: launch`, and a `program`
> pointing at the file.

## Notes

- The program to debug and its arguments come from the `launch` request's
  `program` field, not the `culebra dap` command line.
- Breakpoints are matched by canonical (symlink-resolved) path, so a breakpoint
  set on a file under a symlinked directory still binds.
- A `debugger` statement in source forces a stop regardless of breakpoints —
  handy for a one-off pause without configuring anything.
