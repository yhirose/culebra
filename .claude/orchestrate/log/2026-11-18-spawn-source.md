# Isolate.spawn_source（着地 90924f5a）

- 規模：大（5 commit、AOT の runtime archive、CI 待ち）。
- 見積もりと実績：ci/ の初回で赤5件（gc-pin-door、param-names、rt-archive-tls と linux-llama の多重定義、mi_smoke）。直した後、ci-gc だけが Linux の gc-stress `--jit` で SEGV。
- AOT の多重定義：別アーカイブをやめ、culebra_rt.cc を実行器ごと作り直した1オブジェクトをコアのオブジェクトの代わりにリンクした（user が長所と短所を見て選択。費用は実測で driver +1.47 MB、見立ての 18 MB は外れ）。
- ci-gc の SEGV：Mac では再現せず、Docker の Ubuntu（arm64）で毎回再現。`*args` を受ける名前空間メソッドの slab が、既定値の String の割り当てで rest Array を掃かれていた。Docker の VM 8 GB では Release の全ツリーが `-j1` でしか通らない（2 本でも cc1plus が OOM）。
- 組み替えのきっかけ：linux-asan が 90 分の上限で 2 回打ち切られ、約3時間待った。その間に master では ASan が誰も待たない asan.yml に移り、CLAUDE.md も「ASan を待たずに着地」に変わっていた。rebase で取り込んでいたのに読まなかった。user の指摘で気づいた。
- userの指摘：「最近のCLAUDE.mdのASanの変更をチェックしましたか？」「時間がかかりすぎ？」
- スキルへの気づき：長い待ちに入る前と rebase のたびに、取り込んだ決まり（CLAUDE.md、orchestrate、workflow、land.sh）の差分を読む。打ち切られたジョブは ccache を残さないので、再実行は冷えたビルドからになる（asan.yml の保存を直した）。
