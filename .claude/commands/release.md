---
description: culebra の新バージョンをリリースする（版数 bump → リリースノート生成 → tag/push → バイナリのビルド完了を待って公開）
---

**ユーザーが明示的に `/release` を実行したときだけ動かす手順。** タグを打って push し
GitHub Release を公開する不可逆な操作を含むので、自発的に開始しない。

引数: `--patch` / `--minor` / `--major` のいずれか 1 つ（`$ARGUMENTS`）。

## 0. 引数の確認

`$ARGUMENTS` から bump 種別を取る。指定なし・複数指定・不正な値なら**中断して**ユーザーに
どれかを確認する（勝手に patch と推測しない）。

## 1. 事前チェック（main tree で）

1. `git -C ~/Projects/culebra status --short` が空であること。

   **例外は submodule のポインタずれだけ**（` M vendor/...` だけが並ぶ状態）。これは誰かの
   作業ではなく、他セッションが vendor を更新したコミットに master を進めた後
   `git submodule update` が走っていないだけなので、`git submodule update --init --recursive`
   で master が記録している状態に同期してよい（`git -C vendor/<m> status --porcelain` が
   空＝中に未コミット変更が無いことだけ先に確認する）。v0.1.0 のリリースでは 2 回詰まった。

   それ以外の変更があれば中断してユーザーに確認する（自分が作っていない変更は触らない）。
2. `git -C ~/Projects/culebra worktree list` を見て、他セッションが作業中かを把握する。
3. `git -C ~/Projects/culebra fetch origin master` して、ローカル `master` と `origin/master`
   が一致しているか確認する。乖離があれば中断してユーザーに確認する。
4. **タグを打つ対象（master HEAD）の CI が green か**を確認する:

       gh run list --workflow=ci.yml --branch master --limit 1 \
         --json headSha,status,conclusion,url

   - `headSha` が master HEAD と一致しない → その commit の CI がまだ無い。push 済みか確認し、
     必要なら `gh run watch` で待つ。
   - `status` が `completed` でない → `gh run watch <databaseId> --exit-status` で待つ。
   - `conclusion` が `success` でない → **中断**してユーザーに報告する。リリースしない。

## 2. 出すバージョンを決める

現在値は `include/culebra.h` の 1 行から読む:

    sed -n 's/^#define CULEBRA_VERSION "\([^"]*\)"/\1/p' include/culebra.h

**タグがまだ 1 つも無い場合**（`git tag -l` が空 = 初回リリース）は bump しない。ヘッダにある
版数がそのまま最初のリリースになる（例: ヘッダが `0.1.0` なら `v0.1.0` を出す）。この場合
Step 5 の版数書き換えは不要なので飛ばし、bump 種別の引数は無視した旨をユーザーに伝える。

**2 回目以降**は semver で bump する（`--patch` なら Z、`--minor` なら Y を上げて Z=0、
`--major` なら X を上げて Y=Z=0）。

どちらの場合も、**出す版数を `0.1.0 → 0.1.1`（または「初回なので 0.1.0 のまま」）の形で
ユーザーに提示して確認を取ってから**次へ進む。

## 3. リリースノートを書く

前回タグ: `git describe --tags --abbrev=0`（無ければ初回）。

- **初回**（タグがまだ 1 つも無い）: 全履歴の要約は無意味なので commit log は使わず、README の
  Features と主要な機能（3 backend、stdlib、ツール群）から「culebra とは何か」のハイライトを
  書く。
- **2 回目以降**: `git log <prev-tag>..HEAD --oneline` をサブシステム別にまとめる。

**英語**で書き、`$TMPDIR` のファイルに保存する（後で `--notes-file` に渡す）。書式:

    ## Highlights
    - ...

    ## Changes
    ### Language / interpreter
    ### JIT / AOT
    ### Standard library
    ### Tooling

    Full diff: https://github.com/yhirose/culebra/compare/vPREV...vX.Y.Z

## 4〜6 は版数を変える場合のみ

**初回リリース（Step 2 で bump しないと決めた場合）は Step 4〜6 を飛ばす。** 変更するファイルが
無いので worktree もコミットも着地も不要 — そのまま Step 7（タグ付け）へ進む。

### 4. 専用 worktree を切る

CLAUDE.md の worktree 必須ルールに従う（リリースも例外ではない）:

    git worktree add ~/Projects/culebra-release-vX.Y.Z -b release-vX.Y.Z
    git -C ~/Projects/culebra-release-vX.Y.Z submodule update --init --recursive

`EnterWorktree` に `path` でその worktree を渡し、以後のパスは全部その絶対パスで書く。

### 5. 版数を更新してコミット

1. `include/culebra.h` の `#define CULEBRA_VERSION "X.Y.Z"` を新バージョンに書き換える
   （**ここが単一の真実源** — CLI も Playground もランディングページもこの 1 行を読む）。
2. `just dev` でビルドし、`./build-dev/culebra --version` が `culebra X.Y.Z (vm+jit)` を
   返すことを確認する。
3. `just site-build` を実行して `site/playground/index.html` の版数表記を更新する。
   **これは省略できない** — `just check-site-version`（CI gate）が culebra.h と突き合わせるので、
   飛ばすと版数 bump のコミットで CI が落ちる。emsdk（`~/Projects/emsdk`）が無い環境なら
   そもそもリリースを進められないので、中断してユーザーに報告する。

   **罠**: 新しい worktree には wasm の stamp ファイルが無い（コミット対象外）ので、
   `site-build` は毎回 wasm を作り直す。その結果 `culebra-{basic,full}.{js,wasm}` に
   約 10 MB の付随的な差分が出て、さらに master 側で site コピーが古くなっていた例
   （`examples/` の一部）も一緒に書き換わることがある。**`site-build` の結果から
   コミットするのは `site/playground/index.html` だけ**にして、それ以外は
   `git checkout --` で戻す。
4. `just sync-site-version` を実行して `site/index.html`（ランディングページ）の版数を
   更新する。ナビの `<span class="ver">` がここでしか更新されない — このページには
   ビルド工程が無いので `site-build` は触らない。**これも省略できない**（同じ
   `check-site-version` が両ページを見る）。emsdk は不要なので、直前の `site-build` が
   通った環境なら必ず通る。
5. `git commit -m "Bump version to X.Y.Z"`（英語・trailer なし）。
   このコミットに入るのは `include/culebra.h` / `site/playground/index.html` /
   `site/index.html` の 3 ファイル。

`just test-dev` はここでは回さない — Step 6 の `just land` が rebase 後に実行する。

### 6. master へ着地させる

手で rebase / merge しない。既存の着地機構を使う（`/ff-merge` と同じ）:

1. `just land release-vX.Y.Z` を実行する（**`dangerouslyDisableSandbox: true` 必須**）。
   内部で rebase → `just test-dev` → ff-only merge を machine-wide ロック下で 1 プロセス実行し、
   他セッションが先に着地しても自動リトライする。数分かかる。
   - `land: rebase conflict ...` で失敗 → 中断してユーザーに報告（手動で衝突解消が必要）。
   - リトライ上限で失敗 → 中断してユーザーに報告。
2. `ExitWorktree` を `action: "keep"` で呼んで main tree に戻り、worktree を手動で削除する
   （**削除も `dangerouslyDisableSandbox: true` 必須** — `.git/worktrees/` の削除は sandbox が
   常に拒否する）。

## 7. タグを打って push する

main tree で:

    git tag -a vX.Y.Z -m "culebra vX.Y.Z"
    git push origin master
    git push origin vX.Y.Z

タグ push が `release.yml` を起動する。

## 8. ビルドを待ってから、Release notes を載せる

**ここで `gh release create` を呼んではいけない。** draft を作るのは `release.yml` の
`prepare` job だけ、と決めてある。`gh release create` に「無ければ作る」形はなく、
`gh release view || gh release create` も atomic ではないので、両側が作ろうとすると同じタグに
draft が 2 つできる（v0.1.0 で実際に発生し、片方を消したらもう片方の `tag_name` が
`untagged-<id>` に化けて、`gh release upload <tag>` が対象を見失う寸前だった）。

まずビルドの完了を待つ:

    gh run list --workflow=release.yml --limit 1 --json databaseId,url
    gh run watch <databaseId> --exit-status

macOS / Linux x64 / Windows x64 の 3 本を並列でビルドし、それぞれ
`culebra-<os>-<arch>.{tar.gz,zip}` + `.sha256` を、最後に `SHA256SUMS` を添付する
（アーカイブ名に版数は入らない — README の `releases/latest/download/...` を固定リンクに
保つため。中のディレクトリ名と `culebra --version` が版を名乗る）。

完走したら release は確実に存在するので、notes を**設定する**:

    gh release edit vX.Y.Z --notes-file <file>

念のため `gh release view vX.Y.Z --json tagName,assets` で、タグ名が `vX.Y.Z` のままで
アセットが 7 個（3 アーカイブ + 3 `.sha256` + `SHA256SUMS`）あることを確認する。

**失敗したら draft のまま停止してユーザーに報告する。** 自動でリトライしたり release を消したり
しない。ログを見て原因を伝え、直したうえで
`gh workflow run release.yml -f tag=vX.Y.Z`（同じタグで assets だけ作り直せる）を提案する。

## 9. 公開して締める

    gh release edit vX.Y.Z --draft=false
    gh release view vX.Y.Z --web

公開したら**そのタグで `toolchain-smoke` を回す**:

    gh workflow run toolchain-smoke.yml -f tag=vX.Y.Z

`culebra toolchain install` がリリースの kit を実際に取ってきてリンクできるかを、
ダウンロード版バイナリ自身に確かめさせる唯一の場所。draft の asset は匿名で
取得できないので `release.yml` の中では検証できず、公開後にしか走らせられない
（ci.yml が見ているのは `--from` 以降＝ダイジェスト・展開・配置・全 axis・uninstall で、
fetch だけがここに残る）。落ちたら kit を差し替えて `gh release upload --clobber`。

最後にユーザーへ確認する（**勝手にやらない**）:

- README の "Pre-1.0. No version tags, CHANGELOG, or package registry yet" の文言、および
  CLAUDE.md「## PR・リリース」節の「culebra は公開前」の記述を更新するか（初回リリース後は
  事実と食い違う）。

## リリースは「CI が通した構成」しか出さない

v0.1.0 の 1 回目のビルドは Windows のリンクで落ちた。`release.yml` の Windows job だけが
`CULEBRA_LTO=ON` で、ci.yml の Windows job は 4 つとも OFF — つまり**この組み合わせは
どこでも一度もリンクされたことがなかった**。UCRT64 の gcc は自身の prebuilt `libstdc++.a` と
LTO リンクできず、`duplicate section ... has different size` で ld が拒否する。

`/simplify` はこれを事前に指摘していた。「CI 未検証だから OFF が安全側」という指摘に対し、
`check_ipo_supported` が自動フォールバックするはずだから壊れる証拠ではない、と退けたのが誤り
だった（`check_ipo_supported` はコンパイラがフラグを受け付けるかしか見ない）。

**判断基準**: リリースビルドの設定が ci.yml と食い違っていたら、その差分は「CI が一度も
通していない経路」だと読む。差分に理由があるなら、リリース前に `ci/<topic>` ブランチで
1 回通しておく（このリポジトリは `ci/**` の push で全マトリクスが走る）。リリースのタグ push
を初回実行にしない。

## sandbox について（実測済み 2026-07-30）

**`git push` と `gh` は両方とも `dangerouslyDisableSandbox: true` が要る。** 最初から付けて
呼ぶこと（sandbox のまま実行すると下記のエラーで失敗する）:

- `git push` → `Connection closed by UNKNOWN port 65535` / `Could not read from remote
  repository`（SSH が遮断される）
- `gh` 全般 → `tls: failed to verify certificate: x509: OSStatus -26276`（`github.com` /
  `api.github.com` は network allowlist にあるが、証明書検証が通らない）

`just land` と `git worktree remove` も同様に sandbox 無効化が要る（CLAUDE.md 記載）。
つまりこの手順で sandbox のまま動くのは編集とローカルビルド／テストだけ。
