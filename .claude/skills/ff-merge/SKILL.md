---
name: ff-merge
description: culebra の worktree での作業完了後、rebase + test-dev + ff-only merge で local master へ着地し、worktree を片付ける
disable-model-invocation: true
---

culebra (`~/Projects/culebra`) のトピック worktree での作業が完了し、全て commit 済みのときに、
local master へ着地して worktree を片付ける。`misc/land.sh`（`just land`）が rebase → `just
test-dev` → ff-only merge を1プロセス・machine-wide ロック下で実行し、その途中で他セッションが
先に着地して master が動いても自動でリトライする（詳細は `misc/land.sh` 冒頭のコメント）。

## 前提確認

1. 現在の worktree が culebra のトピック worktree であること（`git rev-parse --show-toplevel`
   が `~/Projects/culebra` 本体ではない、かつ `git branch --show-current` が `master` でない）。
   main tree 上や master ブランチ上であれば中断し、ユーザーに確認する。
2. `git status --short` が空であること（未 commit の変更がない）。空でなければ中断し、まず
   commit するようユーザーに確認する。

## 着地

3. `BRANCH=$(git branch --show-current)` を取得。
4. `just land "$BRANCH"` を実行する（`dangerouslyDisableSandbox: true` 必須 — build/test-dev
   が ccache tmp 書き込みや `nice` 権限でサンドボックスに阻まれる）。数分かかることがある
   （rebase + `just dev` の rebuild + `test-dev` ~1分強、他セッションが着地中ならそのロック待ち
   も加わる）。
   - **rebase 衝突で失敗**した場合（`land: rebase conflict -- resolve manually ...` というメッセージ）:
     中断してユーザーに報告する。`git -C <worktree> rebase master` を手動で衝突解消してから、この
     スキルを再実行する。
   - **リトライ上限（5回）で失敗**した場合: 他セッションの着地が異常に頻発している可能性がある。
     ユーザーに報告し、少し待ってから再実行を提案する。
   - **成功**した場合: `land: OK -- master is now <hash>` が出力される。次のクリーンアップへ進む。

## クリーンアップ

5. `ExitWorktree` を `action: "keep"` で呼び、セッションの cwd を main tree に戻す
   （この worktree は `git worktree add` で手動作成したものなので、`ExitWorktree` の
   スコープ外 — `action: "remove"` は効かない。必ず `keep` で戻ってから手動で消す）。
6. 手動でクリーンアップする（`dangerouslyDisableSandbox: true` 必須 — submodule 入り worktree の
   `.git/worktrees/<dir>` 削除がサンドボックスで拒否されるため）:
   ```
   git worktree remove --force <worktree-path>
   git branch -d <branch>
   ```
7. `git worktree list` で worktree エントリが消えたことを確認し、最終的な master の HEAD
   （手順4で出力された hash）をユーザーに報告する。

$ARGUMENTS
