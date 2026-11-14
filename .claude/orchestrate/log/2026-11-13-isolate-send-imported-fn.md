# 2026-11-13 import した関数の Isolate 送信（記録の確かめだけ）

- 規模: 小（確かめるだけ）。user の依頼の4件目は「未修正」と索引にあったが、memory 本体は 2026-11-09 に修正済み（27eb6f8d）と書いていた。
- 確かめたこと: `mut` を持つモジュールの関数を `m.double` と `let {double} = m` の両方で Isolate へ送り、master の VM と --jit で 42 と 8 が返った（scratchpad の mod.cul / main.cul）。
- 直したもの: MEMORY.md の索引の「未修正」を「修正済み 27eb6f8d」に（memory のみ、repo は変えない）。
- 気づき: 索引の一行が本体より古いと、user も依頼の時点で未修正と読む。memory を修正済みにしたら索引の行も同じ変更で直す。
