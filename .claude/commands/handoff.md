---
description: 現在の作業状態から「次セッションの開始メッセージ」を生成し、クリップボードへ入れる
---

別の新規セッションが今の作業を引き継いで**次のサイクル**を進めるための「開始メッセージ」を作り、クリップボードに入れてください。

## 手順

1. **現状把握**（コマンドで確認、推測しない）:
   - 作業中の worktree/ブランチ・直近コミット・未コミット変更（`git -C <worktree> log --oneline -3` / `status --short` / `branch --show-current`）
   - 生きている他 worktree（`git worktree list`）
   - 直近のテスト/ゲート状況（このセッションで何が green か）
   - 関連する memory トピック（今サイクルで読んだ/更新したもの）

2. **開始メッセージを作成**（次セッションが *最初のメッセージとしてそのまま貼れる* 自己完結プロンプト、日本語）。必ず含める:
   - **次サイクルのゴール**: `$ARGUMENTS` があればそれを反映。無ければ現状の memory「次サイクル worklist / 次の手順」から最上位の1〜数項目を具体化。
   - **まず読む memory**: `[[topic_name]]` 形式で列挙し「着手前に Read すること」と明記（今サイクルの主トピック＋関連 feedback/project）。
   - **worktree/ブランチ方針**: 続きを同一ブランチでやるか新 worktree を切るか。着手の最初が `git worktree add`、submodule init、master 直コミット禁止、段階テスト（[[feedback_multi_session_coordination]] [[feedback_build_loop]] [[feedback_test_strategy_staged]]）等、該当ルールを1〜2行で。
   - **進め方**: 着手前に「最小スコープ（具体例）」を提示して確認を取る、日本語で応答、等このプロジェクトの流儀。
   - **現在地の要約**: 直近コミット hash・何が done で何が残っているか（次セッションが差分から把握できるよう）。
   - 冗長にしない。次セッションが5秒で状況を掴め、すぐ着手判断できる密度に。

3. **クリップボードへコピー**: 生成した本文を一時ファイル（`$TMPDIR` 配下）に Write してから、OS に応じたコマンドで入れる。**クリップボードアクセスはサンドボックス外なので Bash は `dangerouslyDisableSandbox: true` で実行。**
   - macOS（`uname -s` = `Darwin`）: `pbcopy < <file>`
   - **WSL2**（`/proc/version` に `microsoft` を含む）: `iconv -f UTF-8 -t UTF-16LE <file> | clip.exe`。
     **`clip.exe` に UTF-8 を直接流すと日本語が化ける** — ANSI コードページとして解釈されるため。
     UTF-16LE に変換すれば通る。BOM は付けないこと（付けると貼り付けた本文の先頭に不可視文字が残る）。
   - その他の Linux: `xclip -selection clipboard < <file>`、無ければ `wl-copy < <file>`
   どのコマンドも無い/失敗する環境では、コピーは諦めてチャット表示のみで済ませる。
   コピー後は `powershell.exe -NoProfile -Command "[Console]::OutputEncoding=[Text.Encoding]::UTF8; Get-Clipboard"`（WSL2）
   等で読み戻し、日本語が化けていないことを確認する。

4. **チャットにも本文を表示**し、「クリップボードにコピー済み」と一言添える。次セッションはこれを貼るだけで開始できる。

$ARGUMENTS
