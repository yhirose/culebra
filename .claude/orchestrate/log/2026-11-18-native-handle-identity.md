# ネイティブのハンドルの身元を欄から C++ へ（JitIdHandle / JitProc / JitXmlEvents / JitWrapped）

- 規模：大（CI を待つ）。始まりは「欄型のハンドルの `_id` を remove できる」という小さな記録。再現 → 案A（形を凍らせる）を推して user が選ぶ → 計画レビューで閉じない穴（同名の利用者クラス、Match の SIGSEGV）→ 担当の洗い出しで同じ根の穴がさらに3つ（stmt の `__parent__`、受け手なしの呼び出し、Error/Range）→ user が「根本的で構造的なのは？」と聞き直し、状態を C++ に移す案に決定 → 洗い出し → 計画 v2 → レビュー1回 → 実装 → /simplify → ci/ 2本 → 着地 a4343fec。
- 構造：IDを1つ持つ13種は JitIdHandle（静的な spec のアドレスが種類、spec ごとの meta、`_jit_id_handle_in` が唯一の判定）。Proc・XML・wrap<T> は専用の JitHandle。wrap だけの例外の印と `__nonsendable__` は不要になって消えた。展開・`**`・JSON/TOML は user 決定で TypeError。
- 組み替えのきっかけ：
  - 私が最初に推した案は根を残す最小案だった。B6・鍵の eq に続く同じ根の3つ目で、メモリの安全性に届くのに「まれで小さい」の決まりを当てはめた。user の聞き直しで構造的な案へ（feedback-recommend-structural-fix として記録）。
  - 洗い出しの担当と計画のレビューが、それぞれ私の見落とし（Match、同名クラス、stmt、受け手なし）を見つけた。同じ根の2つ目が出た時点で全部を洗うべきだった。
  - 1本目の ci/ は tests/isolate/test_channel.cul（JSON の `{}` を固定）で落ちた。tests/isolate は just check の対象外。ハンドルの振る舞いを変えたら tests/*/ も grep する。
- 後処理：just check、doctest、gc-hazard、check-generated、quick-guide、修正前のビルドで確認ごとに落ちること、/simplify（3本）、ci/ 2本。
- テストできなかったもの：捕まえられなかった Error の行の表示（AOT では Sys.executable が使えない）。手元の再現で確認。
- 続き（628b17a1）：/simplify を着地後にもう1回（a4343fec 全体）。user 決定2つ：ハンドルへの `remove` も TypeError（Listener の host/port が本当に読み取り専用に）、表示名は docs の型名（`[Database]`、`[Statement]` など、`IO.stdin() -> Stdin`）。ほかは「1つか複数の spec か TypeError」の一本化、`_jit_long_slot`、Isolate の「an Isolate」、古いコメント7か所、ja docs の訳語。ci/ 1本で緑。
