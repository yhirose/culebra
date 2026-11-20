# 2026-11-20 Random の種を子に引き継ぐ（中）

- 作業: docs は「プロセスに1つの共有エンジン、Random.seed で以後すべて再現」と書くが、実装は Runtime ごとで、main の seed が isolate に届かなかった。user は「ユーザーの益の大きい方」を選び、子（isolate・Parallel の要素・サーバーのワーカー）の種を親のエンジンから引く形にした。エンジンは初回に使うときに作り、ワーカーのコンストラクタは種が必須。Audio のノイズの SplitMix64 と1つにまとめた（出力はバイト単位で同一）。
- 見込みと実績: 実装・docs・テストで約1.5時間。ci/ の CI が2回（1回目は gate_budget の更新漏れで build-ubuntu だけが赤）。
- 組み替え: 説明の段階では「Parallel のプールは再現しない」と言ったが、要素ごとに種を作れば再現するので、そうした（報告で明示）。
- 後処理の穴: tests/isolate にファイルを足したのに `tools/checks/gate_budget.txt` を増やさず、CI の build-ubuntu で初めて落ちた。memory には同じ教訓（feedback_gate_budget_with_new_tests）があったのに引かなかった。一覧の §2 のテストの行に `check_gate_budget.sh` を足した。
- /simplify: 3担当。採ったのは、Audio の noise_stream との SplitMix64 の重複、assert_ne、古い「shared engine」の説明4か所、spawn_source のテストの追加、落ち得なかった確認（seed 8 と 7 の比較が main の引きまで含んでいた）の修正。
- 気づき: セッションの入れ替わりで CI の見張りが止まった。入れ替わった後は、走らせていた見張りを必ず確かめ直す。
