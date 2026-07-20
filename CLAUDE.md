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
4. マージ = **rebase → 再 build/test → ff-only**。textual に無衝突でも意味的に正しいとは限らないので rebase 後は必ず再テスト
5. 完了後 `git worktree remove --force` + `git branch -d`

複数セッションが同じ repo で並行作業しうる。作業開始時は `git log --oneline master` と `git worktree list` を確認し、`git status` に自分が作っていない未コミット変更がある場合はそれを触らずユーザーに確認する。

## ビルド

- inner loop（修正→実行→修正）は **`just dev`**（LTO off、AOT archive スキップ、`main.cc` 単体 rebuild）。`CCACHE_DIR=$TMPDIR` を設定しておくこと（未設定だとサンドボックスで build が即失敗する）。
- **`just build`** はコミット前の最終確認、または AOT runtime archive 自体を触った変更のときのみ。

## テスト（速い順に段階的に）

1. 単発確認: `./build-dev/culebra <file>.cul`（+ `--jit`）
2. 両 backend 対称確認: **`just test-dev`**（~48s、no-LTO）— 通常はここまで
3. フルゲート **`just test`**: PR 直前 / AOT・build path に触った変更のみ。毎修正では回さない
4. **docs を触ったら必ず `just doctest`**（`just test` には含まれない別ステップ）
5. LTO/静的リンク/ABI 等 toolchain 差異が出うる変更は、ローカル green だけで安心せず **CI の両 OS（macOS clang / Ubuntu GCC）を確認**してから採否判断する

## 最重要要件

- **interp/JIT/AOT の完全対称化。** 同じプログラムは backend を問わず同じ結果・同じエラーを返す。①振る舞い ②エラーメッセージ（kind+文面+位置） ③検査/throw のタイミングと順序 — 3次元すべて一致させる。既知の差は niche でも直す（放置しない）。
- **JIT のメモリ管理はリークが構造的に起こり得ない形にする。** RAII/ownership 流。場当たり的な leak fix は禁止。設計書は `docs/jit_ownership.md`（GC は `docs/jit_gc_design.md`）。
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
