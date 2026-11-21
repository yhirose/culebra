# --jit が翻訳の後のヒープを OS に返す

- 規模: 小。直列、担当なし。/simplify の4人だけ並列。flight-break #99（#96 の測り方）から。
- 見込みと実績: 見込みは「GC.release_memory() を足し、測る側が呼ぶ」。docs、テスト、`just check`、doctest まで済んだあと、/simplify の高度の指摘で方針を替えた。翻訳が終わる所（`JIT::exec` の lookup の直後）で `malloc_trim(0)` を1回呼ぶだけにして、公開 API は足さなかった。dev のビルドは3回（submodule が無くて1回失敗）。
- 計画で効いたこと: 先に `mallinfo2` と `malloc_trim` を LD_PRELOAD で当て、差が glibc の返していない空き（--jit で 764 MB）だと数字で確かめてから直した。
- 組み替えのきっかけ: 測るときに消す形は、原因（翻訳の分）より1段浅い。exec で返せば、どの --jit の実行も、`top` も、手で走らせた計測も正しくなる。プログラム自身が解放した分（ゲームで 50〜130 MB）は残るが、予算の判断には足りる。
- 数字: ゲームの1視点の VmRSS が 1049 MB → 612 MB（VM は 495 MB）。`malloc_trim` は小さなプログラムで 0.03 ms、ゲームで 29 ms。
- 守備範囲の外で回したもの: なし。テストは足していない（tests/*.cul は VM と JIT の両方で回り、RSS の上限は機械によって揺れる）。見張りは flight-break の Linux の定点撮影のメモリ予算が持つ。
