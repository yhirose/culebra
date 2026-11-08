# 担当への指示のひな形

担当（サブエージェント）はプロジェクトの指示ファイル（CLAUDE.mdなど）を読むが、司令塔のメモリと会話は見ない。必要な文脈は全部ここに書く。全員同じひな形から作り、`<…>`だけを埋める。

- 「Project rules」には、プロジェクトの一覧（例：`project-culebra.md`）の「担当に渡す決まり」をそのまま貼る。
- 「Checks for this subtask」には、SKILL.md §2の「後処理を選ぶ」で担当ごとに置いたものだけを書く。書いていない検証は、担当にさせない。

```text
<Purpose in 1–2 sentences, and why it is needed>

Task: <what to do; its scope, and what is out of scope>
Known facts: <files, functions, reproduction steps>
Baseline binary (pre-fix build of master <SHA>): <path, or "not needed">

<Project rules: paste the project's block here>

Checks for this subtask (do these and nothing heavier; the coordinator runs the rest after integration):
<pick from the following and delete the rest>
- none: write the code and tests only, without building. Say in the report what you could not check.
- single runs of the new or changed tests; show that each new test fails on the baseline binary.
- for a nondeterministic test: run it N times under <the load that triggers it> before and after the fix, and report the failure counts.
- if you change an invariant other code relies on: find every consumer, including copies and emulations in examples and docs, and update them in the same change.
- the project's checks for the area you touched: <pick from the project's verification table, e.g. project-culebra.md §2>.
- the review skill (e.g. `simplify`) on your diff.
- <heavy checks, e.g. a release-build measurement of time and peak memory, only if listed here>

How to work:
- Plan before implementing (approach, affected paths, side effects, how to check for regressions), and review the plan critically.
- Every background wait must end on its own: rely on the harness completion notification, or use a command with a timeout. No unbounded `until`/`while` loops, and no grepping of transcripts. If something takes more than 1.5× its expected time, find out what it is waiting on before you retry.
- If a build-time constant is the only way to show a test fails before the fix, change it temporarily in your local build and discard that change. Don't add a runtime knob without asking.
- If you find a different bug, record it with a reproduction and report it. Don't fix it here unless it blocks you. If it is a second case of the same mechanism, report that too: it may need every site fixed together.
- If a design decision that belongs to the user comes up, stop that part, report the options and your recommendation, and continue with whatever doesn't depend on it.
- Bash: keep commands simple.

Report (English):
- what changed and why;
- the commit SHAs;
- the checks you ran and their results, and the checks you did not run;
- measurements, with their conditions;
- anything not verified;
- other bugs you found.
```
