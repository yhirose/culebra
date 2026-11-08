# culebraの資源と道具の一覧

SKILL.mdの原則と手順をculebraに当てはめるための事実。今のmasterでできることだけを書く。道具が変わったら、その変更と同じコミットでここも直す。ビルドとテストの時間・メモリはCLAUDE.mdの「ビルド」とゲート表（justfileの`gate_rows`の最後の欄。行どうしの比較に使う）が正で、ここには写さない。

## 1. 直列の資源

| 資源 | culebraでの容量と調停 |
|---|---|
| マシンのコンパイル（このマシンは16 GB、20スレッド。ほかのマシンでは最初の大きな仕事で測り直す） | **同時に1本**。main.ccの翻訳単位だけで6.9 GB、全ツリーで11 GBを使い、2本重ねるとスワップに入る。ロック（`misc/one_at_a_time.sh`）が1本ずつにするのは`build`・`build-gate`・`test`などだけで、それも同じ`TMPDIR`の中だけ（サンドボックスの中は`/tmp/claude-501`、外は`/var/folders/…`）。`just dev`と、その前にdevを回す`check`・`phase`・`doctest`・`test-dev`・`land`はロックの外。だから、この1本は自分で守る |
| CI（GitHub Actions、Freeプラン） | 同時20ジョブ、macOSは5。1 runがmacOSを4本使う |
| 着地 | `just land`（`/ff-merge`）が専用のロック（`.git`の下の`culebra-land.lock`）で1本ずつ |

**中心ヘッダ**：`include/`の133本のうち、`preamble_cc.cc`からも辿れる96本は、触るとdevが278秒の直列の作り直しになる（preamble_cc.cc → 焼き込み → main.cc）。`main.cc`だけが辿る30本は約111秒＋リンク、残りの7本はほぼ0（2026-11-08、ed17896eのdevの依存ファイルから）。C++を1か所直すたびのdevは、中央値3.9分、p90 5.0分（312回）なので、直しはまとめてからビルドする。

**サンドボックス**：内と外を切り替えると、ソースを変えていなくてもdevが全部作り直す（平均5.4分、記録の中で78回、1週あたり約130分。原因は調査中）。原因が直るまで、ビルドとテストは全部サンドボックスの外で回す（`just land`と同じ側）。`gh`（中ではTLSで必ず失敗する）、`ps`、`sysctl`、`nice`（中では`setpriority`が失敗し、codegenの比較の結果を壊す）、submoduleの初期化も外で。`diff -`やプロセス置換`<(…)`も中では失敗するので、比べる両側をファイルに書いてから比べる。一時ファイルは`$TMPDIR`かscratchpadに。

## 2. 後処理の特性表（SKILL.md原則3で選ぶ。担当にはこの中から選んで「Checks for this subtask」に書く）

| 後処理 | やり方 | 守備範囲（何を読み、何を捕まえるか） | 費用（時間／占める資源） | 確実さ |
|---|---|---|---|---|
| 単発の実行 | `./build-dev/culebra <file>.cul`（`--jit`も） | その1ファイルの振る舞い。手元のmacOSだけ | 数分／マシン（devのビルドが要る） | |
| テストの効き目 | 修正前のバイナリ（§3）で新しいテストを回す | 新しいテストが本当に効くか | 数分／なし（作り直し不要） | |
| 使う側の洗い出し | `git grep -n '<変えた名前や項目>' -- src include examples docs tests tools`。見つかった場所を確かめるゲートの行のうち、着地のゲートが回さない行（段が`test`だけ。例：`examples/languages`を全部回す行）は`just phase <語>`でci/の前に回す | 変えた不変条件の写しやまね。ゲートの行がない場所は、これだけが読む | 数分＋該当する行／マシン | |
| 変更のたびの検査 | `just check` | sourceとIRのラチェット、`tests/*.cul`全体のassert（executor）、examples | 約20秒＋devのビルド／マシン | |
| 触った領域 | `just phase <語>`（語を関数名かラベルに含む行を全部選ぶ。複数の語は「または」。狙った行だけに当たる語を選び、`>>> [Ns] <label>`で実際に回った行を確かめる） | 選んだゲートの行 | ゲート表の数字＋devのビルド／マシン | |
| 直接回すテスト | `bash tests/<name>_test.sh build-dev/culebra` | そのシェルのテスト（`check`と`phase`では回らない） | 数分／マシン | 負荷で落ちるものがある（§8） |
| 全体のスイープ | ゲート表の行（全テストのvm/jit対称は`just phase run_diff_vm_jit`）。手書きのループで回さない | 全`tests/*.cul`のレーン間の一致 | 10分以上／マシン | |
| docsの同期 | 機能の変更と同じ変更で、enとjaの両方とメモリを直し、`just doctest` | `docs/`のコードブロック（devのバイナリで2レーン）。C++の埋め込み例は読まない（CIだけ） | 数分／マシン | |
| ゲートの数の上限 | `just check-gate-budget`。足した分は`tools/checks/gate_budget.txt`を理由と一緒に直し、足したテストの時間を測る | ゲートが掃くテストとdocsのブロックの数 | 数分／なし | |
| コードのレビュー | `/simplify`（3人並列） | コードの差分の質（重複、命名、効率）。テストとdocsの差分、動作は読まない | 5〜8分／トークン約25万（大半が1回ごとの固定費） | まとめて回す：後続が積む境目と、着地の前の束に1回 |
| 設計や計画のレビュー | 独立した担当1人に、主張を一次資料で確かめさせる | 計画の誤り、見落とし | 4〜11分／トークン12万〜26万 | |
| 性能 | `just build`（`-O3`＋LTO）でだけ測る（`build-dev/`は`-O1`） | ホットパスの時間とピークのメモリ（ホットパスを触った変更だけが読む対象） | 約7分＋測定／マシン（全ツリーのビルド） | 負荷の高いときは揺れる |
| userの感覚での確認 | 音の変わる変更は、変更前後のWAVを聞き比べてもらう | 音、見た目 | 数分の手間、数時間の遅れ／user | |
| 着地のゲート | `just land`（rebase → スコープで決まる検査 → 早送り。スコープは`tools/checks/change_scopes.txt`） | 生成物のずれ、JIT対称の部分集合、codegen、ctestのCLIの半分、isolate、docsのdoctest、rebaseで混ざった他人の変更との組み合わせ。スコープnoneなら全体のgrep 2本だけ | 約2〜7分／着地のロック、マシン | 負荷で落ちるものがある（§8） |
| CI | ci/かmasterへのpush（決まりは§5）。スコープnoneなら起動しない、lightならOSごとの重いレーンを省く（ci.ymlの`scope`ジョブ） | 手元で回らないもの：Ubuntuの`just test`（difftest、AOT、leak系、wrap）、assertを有効にしたビルド、Windows、各OSのリリース構成、C++の埋め込み例。**`.claude/`は読まない** | 約35分／CIの枠（macOS 4/5）。手元のマシンは使わない | 直近100回で失敗10（原因はばらばら） |

### 変更の種類から引く（最小のビルドと、着地の前に回すもの）

| 変更 | 最小のビルド | 着地の前に回す | ci/ |
|---|---|---|---|
| `.claude/`、`CLAUDE.md`だけ（スコープnone） | なし | なし。`just land`が全体のgrep 2本だけを回してビルドしない。CIは起動しない | 不要 |
| `docs/`だけ（スコープlight） | なし（`docs_test`だけはdevで埋め込みの1TU） | 着地のゲートが回す（doctestを含む）。CIは重いレーンを省く | 不要 |
| `site/`（スコープlight） | なし | `just check-playground ./build-dev/culebra`（PagesはCIを待たずに公開される） | 不要 |
| `examples/`、`kauai/`の`.cul`（スコープlight） | なし（今のbuild-devで回す） | 該当のスイート。`examples/languages/`なら`just phase languages` | 不要 |
| `tests/*.cul`、`tests/*_test.sh` | なし | 単発の実行、`bash tests/<name>_test.sh build-dev/culebra`（pl0、mini_*、scene、wrapのスクリプトは着地で回らない） | 不要 |
| `src/preambles/*.cul` | dev（278秒。`just dev`がヘッダを作り直すので、`gen-preambles`を別に回さなくてよい） | `just check`、該当のphase | 不要 |
| 中心ヘッダ（`include/`の133本のうち96本） | dev 278秒 | 該当の後処理 | §5の種類に当たれば要る |
| main.ccだけが読むヘッダ（30本）、`src/main.cc` | dev約111秒＋リンク | 該当の後処理 | 同上 |
| runtime archiveだけのソース（`src/runtime/culebra_rt*.cc`、`include/aot/bootstrap.h`） | devでは一切コンパイルされない | `build-gate`か、CIに任せる | 要る |
| `CMakeLists.txt`、`vendor/` | 広い作り直し | 該当の後処理 | 要る |
| `justfile`、`tools/checks/`、`misc/` | なし | `just check-generated`と、変えたレシピやチェック自体 | 不要 |
| `.github/workflows/` | なし | `just check-generated` | CIでしか確かめられない（release.ymlはタグでしか走らない） |

## 3. 共有の準備
- 修正前のリリースビルド：同じSHAのものがあれば使い回す。なければ専用のworktree（`.claude/worktrees/baseline`）でサンドボックスの外から`just build`し、`<worktree>/build/culebra`を渡す。

## 4. 待ち方の道具
- ロックの持ち主：`lsof "$TMPDIR/culebra-heavy-lane.lock"`（重いビルド。`TMPDIR`はサンドボックスの内と外で違う）、`lsof "$(git rev-parse --git-common-dir)/culebra-land.lock"`（着地）。待っている側も一覧に出る。
- プロセス：`ps -p <pid>`（サンドボックスの外）。
- CI：`gh run list`（サンドボックスの外）。コミットで絞るときは`--commit <完全なSHA>`（短いSHAでは何も返らない）。待つときは`timeout 3600 gh run watch <id> --exit-status --compact -i 30`を、サンドボックスの外のバックグラウンドで。終了コード124は期限切れ。

## 5. CIと着地の決まり
- **着地（`/ff-merge`、`just land`）は要るときだけ**：後続の作業がその変更に依存する、ほかのセッションやuserが使う、束が仕上がった、溜めすぎて壊れたときに切り分けにくくなる手前（目安は束に2〜3の変更）。小さな修正のたびには着地しない。
- **ci/ブランチは要るときだけ**：CLAUDE.mdの「どこまで回すか」の表に当たる変更と、次の変更（ランタイムのメモリ管理と並行性、音声スレッド、runtime archiveに入るヘッダ、AOT、CMake、window backend）で、主ブランチが赤になると困るとき。それ以外（`.cul`、テスト、スクリプト、docsだけの変更など）はci/を通さず、着地してpushし、masterのCIの1回に任せる。
- ci/ のpushのたびに、CIのccacheが10 GBの割り当てに保存され、masterの分を追い出す（冷えた状態ではbuild-ubuntuが81→506秒、macOSが319→870秒）。重ねて本数を減らす。
- 同じci/ブランチにpushし直すと、走っているrunは取り消される（`cancel-in-progress`）。runの途中で積み増さない。重ねてからpushする。
- **CIが読まないファイル**：スコープnone（`.claude/`の下と`CLAUDE.md`）。これだけを変えたpushではCIは起動しない。計画と見積もりにCIの待ちを入れない。
- 同時に流すrunは2本まで（3本目からはmacOSの枠で待つ）。pushの前に`gh run list`を見る。
- **着地の前に回す静的な検査**：`just check`は`check-generated`も`_check-quick-guide`も回さないが、着地のゲート（test-dev）は回す。着地の失敗の約3分の1はこれだった（記録の約30回中10回）。着地の前に`just check-generated`を回し、docs、テスト、標準ライブラリ、文法を変えたら`just check-quick-guide`も回す。
- **変更のスコープ**（`tools/checks/change_scopes.txt`）で、着地とCIは回す範囲を自分で絞る。`just land`のままでよい。表を変えたら`just check-change-scopes`。
- **後片付け**：リモートのci/ブランチの削除は自動判定に拒否されるので、まとめてuserに頼む。worktreeは`ExitWorktree`の`remove`で消す（`rm -rf`は使わない）。担当のworktreeは、担当が終わるまで消せない。
- pushは着地のたびに行い、CIの読むファイルが変わっていればCIを見る。pushの前に`git log origin/master..master`で、自分の分以外がないかを見る。

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

## 7. 振り返りの記録のありか（SKILL.md §6）
- 自分の会話：`~/.claude/projects/<リポジトリのパスの/を-にした名前>/<session id>.jsonl`。担当の記録は、その下の`subagents/`と、作業中の`tasks/<id>.output`。各行の`timestamp`で、道具の呼び出しと通知の時刻が取れる。大きいので、丸ごと読まずにスクリプトで抜き出す。
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

### ビルド（このマシン。ほかのマシンでは測り直す）
| 項目 | 値 | 測定日・条件 |
|---|---|---|
| `just dev`（C++を直した後） | 中央値3.9分、p90 5.0分。30秒未満は約1割 | 2026-11-08、記録の312回 |
| `just build` | 中央値6.5分、p90 9.9分 | 2026-11-08、記録の36回 |
| 新しいworktreeの最初のdev | 中央値0.5分（ccacheが効く） | 2026-11-08、22回 |

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
| worktreeの安全装置による拒否 | 司令塔は1セッションの中央値で約12回（最大59回）、担当は1〜2回 | Bashは単純な形で打つ。ループやパイプが要るときは、スクリプトをファイルに書いて`bash <file> <args>`で回す |
| サンドボックス | `gh`、`ps`、submoduleの一部のファイル、既定の`mktemp`が失敗する | §1のとおり外で回す |
| GitHub APIの上限 | userの全セッションで共有。`gh run view`をループで回して403になった（2回） | 1つのrunに`gh run watch -i 30`を1本。403なら`gh api rate_limit`でリセットを待つ |
| 自動判定の拒否 | `kill`、`pkill`、リモートブランチの削除、scratchの`rm -rf`は拒否された | 自分の処理は`TaskStop`で止める。待ちに期限を付け、止める必要が出ないようにする |
| 負荷で時々落ちるテスト | JITのコンパイル時間ゲート、`signal_test`（メモリより）。`midi_test`の負荷落ちはbfa3cb4aで直した | 時間切れなら単独で回し直す（SKILL.md §4）。再発したら原因を調べる |
