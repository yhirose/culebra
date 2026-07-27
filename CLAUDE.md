# CLAUDE.md — culebra

culebra は個人の趣味プロジェクト（未公開のプログラミング言語処理系。interp + JIT + AOT の3 backend）。

## 言語・コミュニケーション

- 会話・計画・進捗報告・エラー説明はすべて**日本語**。
- コード内コメントは既存スタイルに合わせて**英語**のまま。
- **コミットメッセージは英語**。GitHub PR の title/body・issue・PR コメントなど公開コンテンツも英語。
- コミットメッセージに `Co-Authored-By: Claude ...` 等の trailer や Generated-with バナーは**絶対に追加しない**（amend/squash でも同様）。

## 作業フロー: worktree 必須（例外なし）

**あらゆる作業（コード変更・プロファイル計測・ベンチ・ビルドを含む）は着手前に専用 worktree+branch で開始する。** main tree (`~/Projects/culebra`) は ff マージ点専用とし、そこで編集・commit・build しない。

1. `git worktree add ~/Projects/culebra-<topic> -b <topic>`
2. `git -C ~/Projects/culebra-<topic> submodule update --init --recursive`（必須。未初期化だと build が死ぬ）
3. 以後そのセッションの全パスは worktree の絶対パスで統一する
4. マージ = **rebase → 再テスト → ff-only**。textual に無衝突でも意味的に正しいとは限らないので rebase 後は必ず再テストする。ただし**再テストは既定で `just test-dev`（~80s）**であって全ゲートではない（下の「どこまで回すか」）。master は他セッションで頻繁に動くので、rebase のたびに全ゲートを回すとマシンが占有され全員が止まる
5. 完了後 `git worktree remove --force` + `git branch -d`

複数セッションが同じ repo で並行作業しうる。作業開始時は `git log --oneline master` と `git worktree list` を確認し、`git status` に自分が作っていない未コミット変更がある場合はそれを触らずユーザーに確認する。

## ビルド

- inner loop（修正→実行→修正）は **`just dev`**（LTO off、`-O1`、AOT archive スキップ、`main.cc` 単体 rebuild）。ヘッダを実質変更した場合の再ビルドは約 1 分半。
- ccache は既定の `~/.cache/ccache` をそのまま使う（`CCACHE_DIR` を設定しない）。justfile が `CCACHE_BASEDIR` を worktree root に export するので、同じ commit なら別 worktree の初回ビルドがキャッシュに当たる（実測 90s → 32s）。**絶対パスを焼き込む define を `main.cc` 側に足さないこと** — worktree 間共有が壊れる（`src/source_dir.cc` に隔離してある）。
- **`just build`** はコミット前の最終確認、または AOT runtime archive 自体を触った変更のときのみ。**性能計測は必ず `just build`（`-O3` + LTO）で**。`build-dev/` は `-O1` なので数字が出ない。
- このマシンは 20 スレッド / 15 GB。`just build-gate` は `-j20` でピーク約 11 GB 使うので、**別 worktree セッションと build を同時に走らせるとスワップする** — これは `misc/one_at_a_time.sh` のロックで直列化済み（下記「並走時のマシン占有」）。ロックを外して並走させるなら片方を `CULEBRA_BUILD_JOBS=8` 程度に絞る。
- `build`/`dev`/`build-gate`/`build-no-jit` の make、および `_run-tests`（`test`/`test-dev` 共通）の culebra 実行・ctest はデフォルトで `nice -n 10` 経由。複数 worktree セッション並走時の CPU 専有で通常の Mac 操作が詰まる問題への対処（単独実行時は速度低下なし、競合時のみ譲る）。`CULEBRA_NICE=0` で無効化可。

## テスト（速い順に段階的に）

1. 単発確認: `./build-dev/culebra <file>.cul`（+ `--jit`）
2. 両 backend 対称確認: **`just test-dev`**（~80s、no-LTO）— 通常はここまで
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
| docs | `just doctest` |
| push 後 | **CI の両 OS を確認**（toolchain 差異はここでしか出ない） |

**CI の穴（ローカルでしか塞げない 2 点）**:
- CI は全ジョブで `CULEBRA_CANVAS_WINDOW_DEFAULT=OFF`。**raylib window backend を一度もビルドしていない**
- macOS CI は `CULEBRA_TEST_SKIP_HEAVY=1`。**macOS の AOT と difftest は走らない**（Ubuntu で代替。ただし
  「macOS だけで壊れる AOT リンク」は CI では出ない — 実例あり）

### 並走時のマシン占有

`just build` / `just build-gate` / `just test` は **machine-wide ロックで直列化**される
（`misc/one_at_a_time.sh`）。2 本目は "waiting: another culebra build/gate holds this machine…" と
出して待つ。重いレーンの並走は実測 2.4〜2.8 倍遅くなるので、待つほうが速く終わる。
`just dev` / `just test-dev` は**ロックしない**（他人のゲート中でも即動く）。`CULEBRA_GATE_LOCK=0` で解除可。

## 最重要要件

- **interp/JIT/AOT の完全対称化。** 同じプログラムは backend を問わず同じ結果・同じエラーを返す。①振る舞い ②エラーメッセージ（kind+文面+位置） ③検査/throw のタイミングと順序 — 3次元すべて一致させる。既知の差は niche でも直す（放置しない）。
- **JIT のメモリ管理はリークが構造的に起こり得ない形にする。** RAII/ownership 流。場当たり的な leak fix は禁止。
- 修正は手戻りがあってもきれい・エレガントに。他言語の確立した実装（V8/Ruby/Go 等）を参照して正しい抽象を選ぶ。その場しのぎのハックにしない。

## PR・リリース

- PR は **user から明示的に頼まれない限り作成しない**。commit・local master へのマージまでは依頼通り進めてよい。
- culebra は公開前。version tag・CHANGELOG・GitHub Release 等のリリース系成果物は提案しない（docs 更新は release cadence と独立に進めて OK）。

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

## その他の運用ルール

- 動作確認用の一時ファイルは Bash heredoc でなく Write ツールで書く。
- ツール呼び出しは1つずつ逐次実行。依存のある処理を並列バッチにしない。
- 設計判断で `/meeting` は使わない（キャラ演じ分けの多視点は実質同一モデルの焼き直しで低価値）。申し送り+最新コード+他言語実装を見て直接判断する。
- `/code-review` の指摘は全件即 fix しない。「実害確認済み」「理論的ドリフト」「誤検出」に仕分けし、failing test か実際に踏まれるコード経路がないものは保留してよい。
- 強い設計推奨を出す前に、安い実証（spike・最小再現・小さい計測）で裏取りする。
