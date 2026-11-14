# 2026-11-13 Kauai: 楽器のエラーに声部の名前、`ending` のカットオフ 0

- 規模: 小（src/preambles の .cul 2本、テスト、docs）。計画 → 独立レビュー1回 → 実装 → 着地。
- 見込みと実績: 見込み1時間、実績は約1時間半（preamble の焼き直しの dev が3回: 実装、変異、戻し）。
- 計画の誤り（レビューで直った）: `ending` は「Graph が先に断るので実害なし」と見立てたが、`Synth.Voices` はグラフを作る前に `ending` を呼ぶ。カットオフの包絡が 0 に届くと ZeroDivisionError、adsr のカットオフも同じ、-200 は誤った長さを返していた。直し方は、最低が 0 以下なら終わり無し（Math.inf）。カットオフの規則（0 より上）の判定は Graph の1か所に残し、Synth に写しを作らない。
- レビューで足したこと: Ctrl+C（Interrupted）はそのまま通す、Sound でないときの TypeError は try の外（接頭辞が2重にならない）、位置は `where`（`line N` か `path:N`）。
- 選んだこと: ランタイムのエラーは `{kind, message, line, col}` の形で見分ける。ランタイムの印（JitObject::is_error）は Culebra から見えないので、利用者がちょうどその形の Object を投げると取り違える。まれなので組み込みは足さず、コメントに書いた。
- テストの効き目: 新しい振る舞いの確認4件は master で落ちる。種類と位置を保つ確認と、利用者の投げた値を通す確認は master でも通るので、誤った直し方2つ（`__raise` で位置を捨てる、何でも包む）を1回のビルドに入れて落ちることを見た。Voices の確認は master で ZeroDivisionError。
- テスト時間: tests/test_kauai.cul の --jit が 8.6 → 9.3 s（足した確認が Kauai の曲を4つ作り、Kauai のコードの新しい経路を JIT にかける分。関数にまとめて 9.5 から縮めた）。VM は 0.30 s のまま。test_synth_voices は 6.9 → 7.2 s（--jit）。
