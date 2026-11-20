# 2026-11-20 Linux の CI の赤（cpp-httplib の WebSocket の時間切れ）（大）

- 作業: ff5b39f2 から Linux の ci-light と linux-assert が examples/fm-synth の test_serve で赤。Docker gcc:14 の 60 行の C++ で再現し、cpp-httplib の WebSocket がフレームの途中や断片の間の時間切れで接続を閉じることを根と特定。cpp-httplib 9458c27（再開できる FrameReader、ws は既定で TCP_NODELAY、ヘッダーと中身の先頭を1回の write）を上流に push し、Culebra afa91a7e で submodule を更新して着地。master の CI は全ジョブ緑。
- 見込みと実績: 再現までは約1時間（macOS の loopback では出ないので Docker へ）。直す段は user の判断を2回挟んで数時間。cpp-httplib の全テスト（no-TLS 937、OpenSSL 1157）と Linux の WebSocket テストを回してから push。CI は約1時間。
- 組み替え: (1) 初版は「始まったメッセージは読み切るまで待つ」形だったが、レビューで wss に TCP_NODELAY が効かない・期限のないクライアントが途中で止まった相手に固まる、が出て、user が「途中でも Timeout、続きから読む」構造を選んだ。(2) 「1回の write で Linux の数十 ms の遅れが消える」と言ったのが計測で誤り（遅れはメッセージの間の Nagle）。user に訂正して TCP_NODELAY を既定にする案を選び直してもらった。
- 同じ着地で: fm-synth の Store が書き込み途中の空の settings.json を読む競合（負荷で約30回に1回）を、一時ファイルに書いて rename する形で直した。
- 後処理: 新しいテストは修正前のヘッダーで落ちることを1件ずつ確認。online の badssl テストが1回だけ落ち、再実行で緑（無関係）。
- 気づき: 比較用のヘッダーをソースの隣に置くと `#include "httplib.h"` が -I より先にそれを拾い、修正前後が同じヘッダーになる（版ごとのディレクトリに分ける）。性能や遅延の主張は、推奨として出す前に安い計測で裏を取る（今回はそれを怠って user に選び直させた）。
- 別件として記録: test_engine.cul の実機での時々の失敗（Proxy.stop の負の時刻）、Random の docs と実装（Runtime ごと）の食い違い。
