# culebraの資源と道具の一覧

SKILL.mdの原則と手順をculebraに当てはめるための事実。今のmasterでできることだけを書く。道具が変わったら、その変更と同じコミットでここも直す。ビルドとテストの時間・メモリはCLAUDE.mdの「ビルド」とゲート表（justfileの`gate_rows`の最後の欄。行どうしの比較に使う）が正で、ここには写さない。

## 1. 直列の資源

| 資源 | culebraでの容量と調停 |
|---|---|
| マシンのコンパイル（16 GB、20スレッド） | **同時に1本**。main.ccの翻訳単位だけで6.9 GB、全ツリーで11 GBを使い、2本重ねるとスワップに入る。ロック（`misc/one_at_a_time.sh`）が1本ずつにするのは`build`・`build-gate`・`test`などだけで、それも同じ`TMPDIR`の中だけ（サンドボックスの中は`/tmp/claude-501`、外は`/var/folders/…`）。`just dev`と、その前にdevを回す`check`・`phase`・`doctest`・`test-dev`・`land`はロックの外。だから、この1本は自分で守る |
| CI（GitHub Actions、Freeプラン） | 同時20ジョブ、macOSは5。1 runがmacOSを4本使う |
| 着地 | `just land`（`/ff-merge`）が専用のロック（`.git`の下の`culebra-land.lock`）で1本ずつ |

**中心ヘッダ**：`include/`の下で、`main.cc`か`preamble_cc.cc`から辿れるもの（`include/stdlib/bindings.h`経由も含む）。触るとdevが約4.5分の直列の作り直しになる（preamble_cc.cc → 焼き込み → main.cc）。

**サンドボックス**：内と外を切り替えると、ソースを変えていなくてもdevが全部作り直す（約5分。原因は調査中）。原因が直るまで、ビルドとテストは全部サンドボックスの外で回す（`just land`と同じ側）。原因が直るまでは、CLAUDE.mdの「Claude Codeのsandboxについて」（中でも通る、という記述）よりこちらを優先する。`gh`（中ではTLSで必ず失敗する）、`ps`、submoduleの初期化も外で。一時ファイルは`$TMPDIR`かscratchpadに。

## 2. 検証の層のコマンドと守備範囲（担当には、この中から選んで「Checks for this subtask」に書く）

| 層 | culebraでのやり方 | 守備範囲（何を読み、何を捕まえるか） | 目安 |
|---|---|---|---|
| 単発の実行 | `./build-dev/culebra <file>.cul`（`--jit`も） | その1ファイルの振る舞い。手元のmacOSだけ | 数分 |
| 修正前に落ちることの確認 | 修正前のバイナリ（§3）で新しいテストを回す。作り直しは要らない | 新しいテストが本当に効くか | 数分 |
| 使う側の洗い出し | `git grep -n '<変えた名前や項目>' -- src include examples docs tests tools`。見つかった場所を確かめるゲートの行のうち、着地のゲートが回さない行（ゲート表の段が`test`だけのもの。例：`examples/languages`を全部回す行）は、`just phase <語>`でci/の前に回す | 変えた不変条件の写しやまね。ゲートの行がない場所は洗い出しだけが頼り | 数分＋該当する行 |
| 変更のたびの検査 | `just check` | sourceとIRのラチェット、`tests/*.cul`全体のassert（executor）、examples | 約20秒＋devのビルド |
| 触った領域 | `just phase <語>`（語は、ゲート表の関数名かラベルに含まれる行を全部選ぶ。複数の語は「または」。狙った行だけに当たる語を選び、`>>> [Ns] <label>`の行で実際に回った行を確かめる） | 選んだゲートの行の範囲 | ゲート表の数字＋devのビルド |
| docs | enとjaの両方を書き、`just doctest` | `docs/`のコードブロック（dev用のバイナリで2レーン）。C++の埋め込み例は読まない（CIのdoctest-ubuntuとdocs-cpp-macosだけ） | 数分 |
| ゲートの数の上限 | テストやdocsのコードブロックを足したら`just check-gate-budget`を回し、`tools/checks/gate_budget.txt`を理由と一緒に直し、足したテストの時間を測る | ゲートが掃く数（`gate_budget.txt`） | 数分 |
| 直接回すテスト | シェルで動くテスト（`tests/*_test.sh`）は`bash tests/<name>_test.sh build-dev/culebra`。`check`と`phase`では回らない | そのスクリプト | 数分 |
| レビュー | `/simplify` | 差分のコードの質（重複、命名、効率）。動作は確かめない。テストとdocsだけの変更には回さない | 5〜8分 |
| 全体のスイープ | ゲート表の行を使う。全テストのvm/jit対称は`just phase run_diff_vm_jit`（被覆の部分集合の行と全体の行の両方が回る）。手書きのループで回さない | 全`tests/*.cul`のレーン間の一致 | 10分以上 |
| 性能 | `just build`（`-O3`＋LTO）でだけ測る。`build-dev/`は`-O1` | ホットパスの時間とピークのメモリ | 約7分＋測定 |
| userの感覚での確認 | 音の変わる変更は、変更前後のWAVをuserに聞き比べてもらう | 音、見た目 | userしだい |
| 着地 | `just land`（rebase → `just test-dev` → 早送り） | 生成物のずれ、JIT対称の部分集合、codegen、ctestのCLIの半分、isolate。rebaseで混ざった他人の変更との組み合わせ | 約2〜7分 |
| CI（ci/かmasterへのpush） | CLAUDE.mdの「どこまで回すか」 | 手元では回らないもの：Ubuntuの`just test`（difftest、AOT、leak系、wrap）、assertを有効にしたビルド、Windows、各OSのリリース構成、C++の埋め込み例。**`.claude/`は読まない** | 約35分 |

## 3. 共有の準備
- 修正前のリリースビルド：同じSHAのものがあれば使い回す。なければ専用のworktree（`.claude/worktrees/baseline`）でサンドボックスの外から`just build`し、`<worktree>/build/culebra`を渡す。

## 4. 待ち方の道具
- ロックの持ち主：`lsof "$TMPDIR/culebra-heavy-lane.lock"`（重いビルド。`TMPDIR`はサンドボックスの内と外で違う）、`lsof "$(git rev-parse --git-common-dir)/culebra-land.lock"`（着地）。待っている側も一覧に出る。
- プロセス：`ps -p <pid>`（サンドボックスの外）。
- CI：`gh run list`（サンドボックスの外）。待つときは`timeout 3600 gh run watch <id> --exit-status --compact -i 30`を、サンドボックスの外のバックグラウンドで。終了コード124は期限切れ。

## 5. CIと着地の決まり
- ci/ ブランチで全CIを通すのは、CLAUDE.mdの「どこまで回すか」の表とメモリの決まりに当たる変更だけ：ランタイムのメモリ管理と並行性、音声スレッド、runtime archiveに入るヘッダ、AOT、CMake、window backend。それ以外（`.cul`、テスト、スクリプト、docsだけの変更など）はci/ を通さず、着地してpushする。
- ci/ のpushのたびに、CIのccacheが10 GBの割り当てに保存され、masterの分を追い出す（冷えた状態ではbuild-ubuntuが81→506秒、macOSが319→870秒）。重ねて本数を減らす。
- 同じci/ブランチにpushし直すと、走っているrunは取り消される（`cancel-in-progress`）。runの途中で積み増さない。重ねてからpushする。
- **CIが読まないファイル**：`.claude/`の下（skills、commands、設定）。CIのワークフローは`paths-ignore`で、これだけを変えたpushでは起動しない。計画と見積もりにCIの待ちを入れない。
- 同時に流すrunは2本まで（3本目からはmacOSの枠で待つ）。pushの前に`gh run list`を見る。
- pushはメモリの決まり（着地ごと、または検証済みの束ごとにpushしてCIを見る）に従う。pushの前に`git log origin/master..master`で、自分の分以外がないかを見る。

## 6. 担当に渡す決まり（brief-templateの「Project rules」に貼る）

```text
Project rules (culebra):
- Read CLAUDE.md first. Work in your own worktree with absolute paths under it.
- Setup: run `git submodule update --init --recursive` outside the sandbox, and confirm `git log --oneline -1` equals master <SHA>.
- Builds and tests: plain `just` recipes only. Never wrap them in `misc/one_at_a_time.sh` or `lockf` yourself. Run every build and test outside the sandbox (the same side as `just land`); switching sides forces a full rebuild.
- Expected times: a dev build after a header change takes about 4.5 min, a release build about 7 min. If something takes more than 1.5× that, find out what it waits on before retrying: `lsof "$TMPDIR/culebra-heavy-lane.lock"`, and `ps` outside the sandbox.
- `just phase <word>` runs every gate row whose function or label contains the word; pick one that hits only the rows you want, and read the `>>> [Ns] <label>` lines to see what ran. Never hand-write a sweep loop.
- Run only the checks listed under "Checks for this subtask"; the coordinator runs the rest.
- Bash: keep commands simple. The harness's worktree guard rejects complex shell around git (variables, loops and pipes mixed with git).
- Commits: English, no Claude-derived trailers. Do not push, do not land, do not touch master.
```

## 7. 振り返りの記録のありか（SKILL.md §7）
- 自分の会話：`~/.claude/projects/-Users-yuji-Projects-culebra/<session id>.jsonl`。担当の記録は、その下の`subagents/`と、作業中の`tasks/<id>.output`。各行の`timestamp`で、道具の呼び出しと通知の時刻が取れる。大きいので、丸ごと読まずにスクリプトで抜き出す。
- CI：`gh run list --json createdAt,updatedAt`、ジョブごとは`gh run view <id> --json jobs`（待ちと実行の時間が分かる）。
- ビルドのログ：担当がscratchpadに残した`*.log`。

## 8. 見積もりの表（ほかに置き場所のない数字）
測り直したら、日付と条件を付けて行を足す。古い行は残す。

### CI
| 項目 | 値 | 測定日・条件 |
|---|---|---|
| 1 run | 中央値 約35分（25〜44分） | 2026-11-08、成功した直近20回の作成から更新まで |
| ジョブ数 | 25（masterは`release-diff`を足して26）、合計317ジョブ分 | 2026-11-08。Linux 17、macOS 4、Windows 5 |
| クリティカルパス | test-macos 37〜39分 | 2026-11-08 |
| 同時実行の上限 | 20ジョブ、macOS 5 | GitHub Docs。3本重ねたとき、3本目のmacOSジョブは11〜19分待った（2026-11-08） |
| 安定性 | 直近100回：成功70、失敗10、取り消し18 | 2026-11-08。失敗の原因はばらばら |

### 着地
| 項目 | 値 | 測定日・条件 |
|---|---|---|
| `just land` | 約2〜7分 | rebase、devの作り直し（ヘッダを触った変更なら約4.5分、触っていなければ短い）、test-dev約2分（CLAUDE.mdは約125秒）。2026-11-08 |

### レビューとトークン
| 作業 | 時間 | トークン | 測定日 |
|---|---|---|---|
| /simplify（3人並列） | 5〜8分 | 約25万 | 2026-11-08 |
| 担当1人（中くらいの修正、テスト、docs込み） | 考える・書く20〜30分 | 出力26万〜37万 | 2026-11-08 |
| 独立したレビュー1本（計画や設計） | 4〜11分 | 12万〜26万 | 2026-11-08 |

### 環境の摩擦
| 項目 | 値 | 対処 |
|---|---|---|
| worktreeの安全装置による拒否 | 担当1人あたり4〜14回 | Bashは単純な形で打つ（変数、ループ、パイプをgitと混ぜない） |
| サンドボックス | `gh`、`ps`、submoduleの一部のファイル、既定の`mktemp`が失敗する | §1のとおり外で回す |
| 自動判定の拒否 | `kill`は拒否された | 自分の処理は`TaskStop`で止める。待ちに期限を付け、止める必要が出ないようにする |
