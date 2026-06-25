# Debugging

Culebra ships a [Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol/)
(DAP) server, so you can set breakpoints, step, and inspect variables visually
in any DAP-capable editor — VSCode, Neovim (nvim-dap), Vim (vimspector), Zed,
Emacs (dap-mode), Helix, and others. One adapter serves every editor.

```
culebra dap        # speak DAP over stdin/stdout
```

You rarely run this by hand; your editor launches it (see below).

## How it works

The adapter runs your program in the **interpreter** and uses its per-statement
hook to pause on a breakpoint, a `debugger` statement, or a step, while the DAP
loop answers the editor's requests and resumes execution. Because debugging is
interpreter-backed, run scripts with plain `culebra` (not `--jit`); the JIT/AOT
backends compile to machine code and aren't source-debuggable.

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

Add a small extension that registers the `culebra` debug type and points it at
the adapter (the extension is pure registration — all logic lives in
`culebra dap`, so the same adapter works in every editor). Its `package.json`
contributes:

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

Then a project `.vscode/launch.json`:

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

Press <kbd>F5</kbd> to debug the open file.

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

Set the filetype for `.cul` files to `culebra` (e.g. via an autocommand) so
`dap.configurations.culebra` applies.

## Vim (vimspector)

`.vimspector.json` in the project root:

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

Zed's debugger (built-in DAP client) takes a custom adapter in its debug
configuration; point it at `culebra dap` with a `launch` request whose
`program` is the file to debug. (Zed's debugger is newer than VSCode/nvim-dap,
so the exact config schema may evolve.)

## Notes

- The program to debug and its arguments come from the `launch` request's
  `program` field, not the `culebra dap` command line.
- Breakpoints are matched by canonical (symlink-resolved) path, so a breakpoint
  set on a file under a symlinked directory still binds.
- A `debugger` statement in source forces a stop regardless of breakpoints —
  handy for a one-off pause.
