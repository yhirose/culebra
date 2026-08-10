# vm2gol-v2

A culebra port of sonota88's
[vm2gol-v2](https://github.com/sonota88/vm2gol-v2) (MIT) — a small
compiler pipeline built from scratch as a teaching exercise, written up in
two blog posts:
[Rubyで素朴な自作言語のコンパイラを作った](https://memo88.hatenablog.com/entry/2020/05/04/155425)
and [vm2gol v2 製作メモ](https://memo88.hatenablog.com/entry/2019/05/04/234516).
The four stages, the stack-machine instruction set, and the sample
Game-of-Life program (`gol.vg.txt`) all come from the original; what's
different is the implementation language.

`vg` (the toy language `gol.vg.txt` is written in) is unrelated to
Culebra — it is the tiny, C-like language this pipeline exists to compile.
Its own source is untouched: it is not Culebra code, so nothing about it
needed porting.

## The pipeline

Four independent stages, each a standalone `.cul` program that also
exports its logic for reuse:

| stage | file | reads | writes |
|---|---|---|---|
| tokenize + parse | `vgparser.cul` | `.vg.txt` source | AST as JSON |
| code generation | `vgcg.cul` | AST JSON | stack-machine assembly text |
| assemble | `vgasm.cul` | assembly text | executable JSON-lines (labels resolved to addresses) |
| run | `vgvm.cul` | executable JSON-lines | (executes it) |

Each can be run on its own, exactly like `ruby vgparser.rb file.vg.txt` in
the original:

```sh
culebra vgparser.cul gol.vg.txt > tmp/gol.vgt.json
culebra vgcg.cul     tmp/gol.vgt.json > tmp/gol.vga.txt
culebra vgasm.cul    tmp/gol.vga.txt > tmp/gol.vge.txt
culebra vgvm.cul     tmp/gol.vge.txt
```

`run.cul` chains all four in one process (mirroring `run.sh`) and writes
the same intermediate files to `tmp/` so each IR stage can still be
inspected on disk:

```sh
culebra run.cul              # compiles and runs gol.vg.txt
culebra run.cul other.vg.txt
```

`vgvm.cul` (and so `run.cul`) is interactive by default, same as
`ruby vgvm.rb`: it dumps registers/stack/vram every 10 steps and waits for
Enter before starting. Set `TEST=1` to run silently, or `STEP=1` to single-step
with a dump before every instruction.

`gol.vg.txt` plays Conway's Game of Life forever (`gen_limit = 0`) on a 5x5
wrapped grid, printing each generation — press Ctrl+C to stop.

## Tests

`test_vm2gol.cul` covers the tokenizer/parser on the same small cases as
upstream's `test/test_vgparser.rb`, an assembler unit test, and an
end-to-end check mirroring `test/test_gol.rb`: it rewrites `gol.vg.txt`'s
`gen_limit` to stop after N generations, runs the full pipeline, and
compares the VRAM grid against a known-good trace (1 and 20 generations).

```sh
culebra test examples/vm2gol-v2
```
