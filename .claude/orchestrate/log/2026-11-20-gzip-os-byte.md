# 2026-11-20 Compress.gzipを機械によらず同じバイトに（小）

- 作業: flight-in-nyの地形メッシュがMacで作り直すと全ファイル変わる件を追い、gzipのヘッダのOSの欄（zlibの既定でLinux 3、macOS 19）だけの差と特定。`Compress.gzip`が`deflateSetHeader`でOS 255を書くようにした（GoとPythonと同じ）。9b700afdで着地しpush。`Compress.zip`は既に時刻とOSの欄が固定で、gzipを書くのは`compress.h`の1か所だけだった。
- 見込みと実績: 見込みは調べて直す30分、dev約5分、着地約5分。実績もほぼ同じ（devは中心ヘッダで約5分）。
- 後処理: テストの効き目（修正前のPATHのculebraで17行目が落ちる）、単発の実行（executorと`--jit`、標準の`gzip -t`で展開）、`just check`、`just doctest`、`check-generated`、`check-quick-guide`。/simplifyは3人で、採った2件（コメントのnil→null、ヘッダ設定の失敗の文面）。
- 気づき: `just land`は引数にブランチ名を取る（`just land worktree-<topic>`）。一覧の「`just land`（`/ff-merge`）」の書き方だと引数なしで打って1回失敗した。
