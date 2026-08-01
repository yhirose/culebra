## Culebra

ソースファイルの拡張子は `.cul`。manifest もパッケージマネージャも無く、
標準ライブラリは `import` なしで全て見えている。

```bash
culebra prog.cul          # 実行
culebra --jit prog.cul    # 同じ出力を LLVM JIT で
culebra test              # cwd 以下の test_*.cul を実行
culebra fmt -i .          # その場で整形（スタイル設定は無い）
culebra lint .            # 静的検査
```

**API は推測せず引く。** リファレンス一式はバイナリの中にあるので、
実行しているビルドと必ず一致する:

```bash
culebra docs -g 'Math.wrap'          # 一致したセクションを表示
culebra docs -g '<name>' >/dev/null  # 一致が無ければ exit 1
```

終了コードは grep と同じで、`0` は何か表示した、`1` は一致なし。
`-g` が見つけられない署名は存在しない。

**culebra を書く前に `culebra docs llm` を読む。** プロンプトに収まる
1 ファイルで、構文・標準ライブラリの全署名・他言語から持ち込めない習慣の
一覧が入っている。その一覧がどういう内容かの一例:

```culebra
# !! TypeError
'ab' * 3
```

**書いたものは実行する。** 未定義の名前はプログラム開始前に弾かれるが、
この検査が見るのは名前だけでメンバーは見ない。`Math.abss(1)` や
`xs.len()` は `culebra lint` を通り、その行が走った時に失敗する。
存在しないプロパティはエラーではなく `nil` になる。
