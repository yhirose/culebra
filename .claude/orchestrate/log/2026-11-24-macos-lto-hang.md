# test-macos（Xcode 26）のLTOリンクが終わらない件と、#131のsegfault（2026-11-24）

- 規模: 「調べないと分からない」から小へ。申し送り（67fcb0cf＝#131以降、test-macosがリンクで60分打ち切り）を受けて調査。手元のMacがXcode 26.2（Apple clang 17.0.0）で、CIと同じ系統だったので再現できた。
- 診断の道具: `sample <ld>`でThinLTOのワーカーがInstCombine（`freezeOtherUses`と沈め）で回り続けているのを見て、`-Wl,-mllvm,-opt-bisect-limit=-1`で最後にパスを受けた関数（`JIT::arith_versions`）を取った。Apple のlibLTOには`-debug-pass-manager`が無い。そこから40行の切り出し（`-flto=thin -dynamiclib -undefined dynamic_lookup`でLLVMのライブラリ無しにリンクできる）で、書き方を変えて0秒／止まるを数秒ずつ比べた。上流のclang 23とApple のfull LTOでは止まらない。LLVMの未解決の課題219102（freezeの沈めとfreezeOtherUsesの往復）とスタックが一致。
- 見つけたもう1件: レビューの途中で、#131の`arith_versions`が死んだguard（`return`の後の算術の並び）で、joinと共有のlanding padまで消してsegfaultすることが分かった（4096ブロック超の関数に死んだ並びを1つ置くだけで再現）。
- 直し方: cutの前に`EliminateUnreachableBlocks`、Floatの写しは「先頭を閉じたplainの走査が届かないブロック」として1回の走査で取る。cutの数え方が「作ったまま」から「到達できるブロック」に変わる点はuserに選んでもらった（test_diverging_exprのトップレベルだけが最適化の側へ）。
- 後処理: 独立レビュー1回（subagent、提案の正しさと後段への影響。数え方の変化を指摘）、修正前にfixtureがsegfaultすることの確認、jit_big_function_test、just check、doctest、手元の`just build`でXcode 26のリンク72秒（rebase後82秒）、/simplify（bashの`$?`がコマンド置換で0に戻る不具合など3件）、test-dev。
- 組み替え: 着地後のpushが、origin/masterが別の場所からの7件（cutを越えた関数の再最適化）で先に進んでいて拒まれた。手元のmasterには終了した別セッションの未pushの2件（Linux arm64のDocker）があった。自分の1件だけをorigin/masterに載せ直して検証し直し、pushした。arm64の2件はuserの判断で保留（手元のmasterでorigin/masterの後ろに載せ直した）。
- userの指摘: 「根本的で構造的でエレガントな方法は？」「これは本当に正しいかレビューして」。最初の案（入口から2回歩く）は死んだguardを見落としていて、レビューで崩れた。書き方を変えて止まりを避ける案を、コードの正しさを詰める前に出したのが早すぎた。
- 気づき: 「コンパイラのバグ」と結論した後も、その関数自体の正しさは別に疑う。止まりの回避だけを見て選んだ案は、同じ関数の別の穴を見落とした。
