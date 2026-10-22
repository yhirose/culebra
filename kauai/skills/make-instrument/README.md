# make-instrument

A skill for Claude Code: making an `external` instrument for Kauai from
royalty-free recordings, with Culebra alone, by measuring, checking, and
comparing versions blind with the person listening. `SKILL.md` is what
Claude reads; `references/` holds the detail it reads when it needs it.

In this repository the skill is linked from `.claude/skills/make-instrument`,
so Claude Code finds it in any session started here. To use it elsewhere,
link or copy this folder into the project's `.claude/skills/` (or your own
`~/.claude/skills/`):

```sh
ln -s /path/to/culebra/kauai/skills/make-instrument ~/.claude/skills/make-instrument
```

It expects a checkout of this repository: the tools it uses are
`kauai/tools/check.cul`, `kauai/tools/listen.cul` and
`kauai/tools/make_sounds.cul`, and a new instrument goes in
`kauai/sounds/<name>/`, its work in `kauai/sounds/<name>/work/`.
