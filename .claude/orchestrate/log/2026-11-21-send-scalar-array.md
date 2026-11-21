# flight-break#95: 配列をisolateへ送ると要素1つにつき約900バイト

- 規模: 小〜中。直列、担当なし。30行を超えるので、着手前に見積もりを出して user が方針を選んだ（スカラーの配列を詰める。SendNode を細くする案は見送り）。
- 直し方: 要素がすべて Nil/Bool/Long/Float の Array と Tuple を、SendNode の `scalars`（種類と64ビット、16バイト）に詰めて送る。400万要素で最大RSS 3.67 GB → 224 MB（VM）/ 241 MB（JIT）、1.56 s → 0.10 s。
- 落ちた検査: just check の test_deep_values。詰めた経路が深さの数え（ValueWalkFrame）より前で抜けていて、一番内側の配列が1段に数えられなかった。数えを分岐の前へ。新しいテストは振る舞いを保つ確認なので、修正前でも通る。
- /simplify（1本で4観点）: タグとスカラーの対応が3か所にあったのを `_jit_scalar_of` / `_jit_from_scalar` にまとめた。SendNode 自体を細くする（クロージャ用の欄を別に持つ）案は、文字列の配列や Object の値にも効くが、今回の範囲の外として残した。
