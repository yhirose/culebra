# D: linux-asan（ASan+UBSan、slab passthrough で smoke・vm/jit・gc-stress）

- 規模：大（CI を待つ）。計画 → 独立レビュー2回（v1、v2）→ user 確定 → 実装 → ci/ 2本（ジョブだけ、修正を戻した木）→ 着地 f142abbf。
- 見積もりと実績：実装 1h の見込みに対し、CI の往復が4回（GCC+UBSan の constexpr で落ちる、test_size_too_large が ASan の operator new と合わない、gc-stress の初計測、着地候補）。冷えた ccache で linux-asan は 70 分、温まって 35 分。
- 効き目：41644eda の修正を戻した木で value_api_smoke が heap-use-after-free を報告（合格条件は報告の文字列。赤だけでは修正前のテスト自身が落ちるので区別できない）。
- 組み替えのきっかけ：
  - レビューの指摘（mold が ASan のリンクフラグを上書き、行の指定、ctest の出力、smoke を先に、スタック、JIT キャッシュの鍵）はどれも当たっていた。
  - GCC で -fsanitize=null を付けると `if constexpr (ptr != nullptr)` が定数にならない。macOS の clang ではビルドが通るので、手元では見えなかった。
  - ASan の throwing operator new は allocator_may_return_null でも投げない。bad_alloc に頼るテストは外すしかない（CULEBRA_SWEEP_SKIP）。
- 副産物：手元の ASan バイナリで、非 String キーの表を引く最中の鍵の eq による UAF を確認（別件、memory に記録）。
- スキルへの気づき：ASan のように「別のコンパイラでだけ通らない」軸は、最初の ci/ で必ず1往復かかる。計画の見積もりに CI の往復を1回足しておく。
