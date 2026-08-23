# CLAUDE.md — culebra

culebra は個人の趣味プロジェクト（プログラミング言語処理系）。**repo は公開済み**（`github.com/yhirose/culebra`）で、**2026-08-22 に `v0.3.0` をリリース**した — 3 OS のバイナリを GitHub Release で配布し、docs サイトと WASM Playground を GitHub Pages で公開している。

**エンジンは bytecode VM が既定**（v0.3.0 以降）。`vm::Compiler` が bytecode に落とし、`vm::Exec` が実行する。`--jit` と `culebra build`（AOT）は**同じ bytecode**を LLVM IR に降ろす消費者で、フロントエンドは1つ。tree-walking interpreter は隠しフラグ `--tree` でのみ届き、Phase 4 の最終バッチ（B7）で削除する — 仕様と進捗は `docs/internals/vm.md` §13。

## 言語・コミュニケーション

- 会話・計画・進捗報告・エラー説明はすべて**日本語**。
- コード内コメントは既存スタイルに合わせて**英語**のまま。
- **コミットメッセージは英語**。GitHub PR の title/body・issue・PR コメントなど公開コンテンツも英語。
- コミットメッセージに `Co-Authored-By: Claude ...` 等の trailer や Generated-with バナーは**絶対に追加しない**（amend/squash でも同様）。
- 日本語 docs（`docs/*.ja.md` など）の半角スペース規則は `.claude/rules/japanese-docs-spacing.md` を参照（該当ファイルを触るときのみ自動ロード）。

## 作業フロー: worktree 必須（例外なし）

**あらゆる作業（コード変更・プロファイル計測・ベンチ・ビルドを含む）は着手前に専用 worktree+branch で開始する。** main tree (`~/Projects/culebra`) は ff マージ点専用とし、そこで編集・commit・build しない。

1. `git worktree add ~/Projects/culebra-<topic> -b <topic>`
2. `git -C ~/Projects/culebra-<topic> submodule update --init --recursive`（必須。未初期化だと build が死ぬ）
3. `EnterWorktree` に `path` でその worktree を渡し、セッションの cwd 自体を移す（`name` での新規作成は使わない — submodule init が入らず build が死ぬ）
4. **以後そのセッションの全パスは worktree の絶対パスで統一する。** `EnterWorktree` は cwd を移すだけで、Edit/Write に渡した絶対パスは worktree 内へ正規化されない（main tree を直接汚した実例あり）
5. マージ = **rebase → 再テスト → ff-only**。textual に無衝突でも意味的に正しいとは限らないので rebase 後は必ず再テストする。ただし**再テストは既定で `just test-dev`（~80s）**であって全ゲートではない（下の「どこまで回すか」）。master は他セッションで頻繁に動くので、rebase のたびに全ゲートを回すとマシンが占有され全員が止まる
6. 完了後 `git worktree remove --force` + `git branch -d`

**Step 5-6 は `/ff-merge` skill で自動化されている**（`disable-model-invocation` なのでユーザーが明示的に呼ぶ）。内部で `just land`（`misc/land.sh`）が rebase → `just test-dev` → ff-only merge を machine-wide ロック下で1プロセス実行し、着地レース（他セッションが先に master を進めた場合）は自動リトライする。全て commit 済みならこれを使う。上記 5-6 は手動でやる場合の内訳。

**Claude Code の sandbox について**: `git worktree add`／`submodule update`／`just dev`／`just test-dev` は `.claude/settings.local.json` の `sandbox.filesystem.allowWrite`（ccache tmp dir 追加済み）と `sandbox.network.allowLocalBinding` で sandbox 有効のまま通る。**`git worktree remove` だけは例外** — `.git/worktrees/` 配下の削除は sandbox 下で常に拒否される（`.git` メタデータ削除への意図的な保護とみられる）ので、この一手だけ `dangerouslyDisableSandbox` が要る。

複数セッションが同じ repo で並行作業しうる。作業開始時は `git log --oneline master` と `git worktree list` を確認し、`git status` に自分が作っていない未コミット変更がある場合はそれを触らずユーザーに確認する。

## ビルド

- inner loop（修正→実行→修正）は **`just dev`**（LTO off、`-O1`、AOT archive スキップ、`main.cc` 単体 rebuild）。ヘッダを実質変更した場合の再ビルドは約 1 分半。
- ccache は既定の `~/.cache/ccache` をそのまま使う（`CCACHE_DIR` を設定しない）。justfile が `CCACHE_BASEDIR` を worktree root に export するので、同じ commit なら別 worktree の初回ビルドがキャッシュに当たる（実測 90s → 32s）。**絶対パスを焼き込む define を `main.cc` 側に足さないこと** — worktree 間共有が壊れる（`src/source_dir.cc` に隔離してある）。
- **`just build`** はコミット前の最終確認、または AOT runtime archive 自体を触った変更のときのみ。**性能計測は必ず `just build`（`-O3` + LTO）で**。`build-dev/` は `-O1` なので数字が出ない。
- このマシンは 20 スレッド / 15 GB。`just build-gate` は `-j20` でピーク約 11 GB 使うので、**別 worktree セッションと build を同時に走らせるとスワップする** — これは `misc/one_at_a_time.sh` のロックで直列化済み（下記「並走時のマシン占有」）。ロックを外して並走させるなら片方を `CULEBRA_BUILD_JOBS=8` 程度に絞る。
- `build`/`dev`/`build-gate`/`build-no-jit` の make、および `_run-tests`（`test`/`test-dev` 共通）の culebra 実行・ctest はデフォルトで `nice -n 10` 経由。複数 worktree セッション並走時の CPU 専有で通常の Mac 操作が詰まる問題への対処（単独実行時は速度低下なし、競合時のみ譲る）。`CULEBRA_NICE=0` で無効化可。

## テスト（速い順に段階的に）

1. 単発確認: `./build-dev/culebra <file>.cul`（+ `--jit` / `--tree`）
2. 全レーン対称確認: **`just test-dev`**（~80s、no-LTO）— 通常はここまで。生成物ゲート `check-generated`（grammar sync / preamble / blob / site version）を前段で回すので、生成物のずれは着地前にここで落ちる
3. フルゲート **`just test`**（実測 450〜880s、うち 95% は difftest + leak 系 + AOT）
4. **docs を触ったら必ず `just doctest`**（`just test` には含まれない別ステップ）

### どこまで回すか（変更内容に比例させる）

**ローカル全ゲートは既定にしない。** Ubuntu CI が `just test` を skip なしで回すので、
difftest・AOT・leak 系・wrap はそこで必ず走る。ローカルで全ゲートを回す価値があるのは、
**CI が構造的に見られない部分に触ったとき**だけ:

| 変更 / 状況 | ローカルで回すもの |
|---|---|
| rebase 後の再検証（textual 無衝突） | `just test-dev` |
| 通常のコード変更 | `just test-dev` |
| **Canvas/Scene の window backend、AOT gating、runtime archive** | **`just test`**（+ CMakeLists/wrap なら `just test wrap`） |
| **`Runtime` の teardown、GC heap、slab allocator** | **`just test-assert`** — 全レーンが `Release`（`-DNDEBUG`）で木の assert は 1 度も走らない。これだけが `NDEBUG` なしでビルドして同じスイープを回す |
| docs | `just doctest` |
| push 後 | **CI の両 OS を確認**（toolchain 差異はここでしか出ない） |

**CI の穴（ローカルでしか塞げない点）**:
- macOS CI は `CULEBRA_TEST_SKIP_HEAVY=1`。**macOS の difftest は走らない**（Ubuntu で代替）。
  AOT は `macos-canvas-window` が1本だけリンクするので、「macOS だけで壊れる AOT リンク」
  （実例あり）は**そのジョブが Canvas 経路については見る**。それ以外の AOT はローカルだけ
- **実際にウィンドウが開くこと**の macOS 側。Linux は Xvfb 下で実窓生成まで見るが、
  macOS ランナーにディスプレイサーバが無いので raylib の window path に入るのはローカルだけ

`test` マトリクスは今も全ジョブ `CULEBRA_CANVAS_WINDOW_DEFAULT=OFF` だが、**`linux-canvas-window`
と `macos-canvas-window` が window ON をビルドする**（CMake 既定に任せる。Linux は SDL3 の
build deps を入れる）。**両ジョブとも LTO ON ＝ 配布バイナリと同じ構成**で、window ON ビルドでの
headless 動作と AOT の `culebra_rt_canvas` force-load を見る（Linux はさらに Xvfb 下の実窓生成と
DT_NEEDED 検査）。`release.yml` も同じ構成だが `v*` tag でしか走らない — **タグでしか到達しない
構成は、そこを変更したバッチが検証できない**（v0.1.0 と v0.3.0 のリリースが2回ともこれで落ちた）。

**`linux-assert` ジョブ**が Ubuntu で `just test-assert` を回す（`NDEBUG` なしビルド + 同じスイープ）。
assert が本当に compile-in されたかを binary 内の assert 文字列 grep で検証してから走るので、
`NDEBUG` が戻っても緑にならない。

### 並走時のマシン占有

`just build` / `just build-gate` / `just test` は **machine-wide ロックで直列化**される
（`misc/one_at_a_time.sh`）。2 本目は "waiting: another culebra build/gate holds this machine…" と
出して待つ。重いレーンの並走は実測 2.4〜2.8 倍遅くなるので、待つほうが速く終わる。
`just dev` / `just test-dev` は**ロックしない**（他人のゲート中でも即動く）。`CULEBRA_GATE_LOCK=0` で解除可。

## 最重要要件

- **全レーンの完全対称化。** 同じプログラムは executor（既定）/ `--jit` / AOT、そして `--tree` が生きている間はそれも含めて、同じ結果・同じエラーを返す。①振る舞い ②エラーメッセージ（kind+文面+位置） ③検査/throw のタイミングと順序 — 3次元すべて一致させる。既知の差は niche でも直す（放置しない）。**B7 で `--tree` が消えると独立第二実装によるオラクルを失う**ので、それまでに見つかる差は全部潰す（詳細は `docs/internals/vm.md` §13.5）。
- **JIT のメモリ管理はリークが構造的に起こり得ない形にする。** RAII/ownership 流。場当たり的な leak fix は禁止。
- 修正は手戻りがあってもきれい・エレガントに。他言語の確立した実装（V8/Ruby/Go 等）を参照して正しい抽象を選ぶ。その場しのぎのハックにしない。

## PR・リリース

- PR は **user から明示的に頼まれない限り作成しない**。commit・local master へのマージまでは依頼通り進めてよい。
- **リリースは `/release` skill を user が明示的に実行したときだけ。** tag を push して GitHub Release を公開する不可逆な操作なので、自発的に開始しないし提案もしない（docs 更新は release cadence と独立に進めて OK）。CHANGELOG は持たず、リリースノートは `/release` が都度書く。
- **版数の単一源は `include/culebra.h` の `CULEBRA_VERSION`。** site 側の表記とのずれは `just check-site-version` が検出する（`check-generated` 経由で `just test-dev` が回すので、ずれたままでは着地できない）。landing page は `just sync-site-version`、Playground は `just site-build` で追従させる。
- repo は公開済みなので、README・docs・site・commit メッセージ・issue/PR は**そのまま公開コンテンツ**として扱う（言語は上記のとおり英語）。

## コードスタイル

- コメントは既存コードの粒度に合わせる。長文の why 説明を書かない（1 行で要点、非自明な理由のみ）。
- マクロは本物のロジック共有（~5 行以上の重複）がある場合のみ。signature の繰り返し短縮目的では使わない。
- user-facing な名前（CLI フラグ等）は効果が一目でわかる語にする。曖昧なら改名する。
- `|...|` lambda の body は単一 expression のみ。block（複数 statement/中間 let）が必要なら `fn (x) {...}` を使う。
- パイプライン演算子 `|>` は追加しない（UFCS + メソッド連鎖で十分と判断済み）。

## docs・memory 同期

- 機能追加・変更は**同じサイクル内で** `docs/*.md`（en+ja 両方）とメモリ（該当 project/roadmap ファイル）を更新する。後回しにしない。
- README/docs で自画自賛的な形容詞（"powerful", "elegant" 等）を使わず、事実だけを列挙する。
- 特長の打ち出しで特定言語を物差しにした比較表現（"Python-concise" 等）を避け、性質を culebra 自身の言葉で書く。
- **`docs/essays/` は上記と別の規律。** 自画自賛的形容詞の禁止は同じだが、他言語への言及・比較はここでは歓迎する — 良いアイデアの参照元には敬意と感謝を込めて記してよい。culebra の設計判断と他言語の対比は優劣付けや批判ではなく、culebra を理解するための有益な情報として書く。位置づけは Stroustrup『C++の設計と進化』と同じ: 仕様書（`docs/*.md`）には書ききれない、言語デザイナーの気持ちや現在の仕様に至った経緯・原則を記す補足文書。
- **エッセイのファイル名は主題ベースにし、人名を入れない**（`concurrency.md`／`concurrency.ja.md` のように、他の docs と同じ単語ベースの命名）。特定のインタビュー・書籍・人物を起点に書いた文章でも、ファイル名はあくまで扱う主題で決める。

## その他の運用ルール

- 動作確認用の一時ファイルは Bash heredoc でなく Write ツールで書く。
- `/code-review` の指摘は全件即 fix しない。「実害確認済み」「理論的ドリフト」「誤検出」に仕分けし、failing test か実際に踏まれるコード経路がないものは保留してよい。
- 強い設計推奨を出す前に、安い実証（spike・最小再現・小さい計測）で裏取りする。

<!-- culebra:agent:begin -->
## Culebra

Source files are `.cul`. There is no manifest and no package manager;
the whole standard library is in scope without an `import`.

```bash
culebra prog.cul          # run (the bytecode VM's executor)
culebra --jit prog.cul    # the same output, that bytecode lowered to LLVM
culebra test              # run every test_*.cul below the cwd
culebra fmt -i .          # format in place (no style options)
culebra lint .            # static checks
```

**Look an API up instead of guessing it.** The whole reference is
inside the binary, so it always matches the build being run:

```bash
culebra docs -g 'Math.wrap'          # print the sections that match
culebra docs -g '<name>' >/dev/null  # exits 1 when nothing matches
```

Exit status is grep's: `0` printed something, `1` nothing matched. A
signature `-g` cannot find does not exist.

**Read `culebra docs quick-guide` before writing Culebra.** It is one
prompt-sized file: the syntax, every standard-library signature, and a
table of the habits from other languages that do not carry over. One
row of that table, for the kind of thing it covers:

```culebra
# !! TypeError
'ab' * 3
```

**Run what you write.** Undefined names are rejected before the program
starts, but that check covers names, not members: `Math.abss(1)` and
`xs.len()` survive `culebra lint` and fail when the line runs, and a
missing property is `nil` rather than an error.
<!-- culebra:agent:end -->
