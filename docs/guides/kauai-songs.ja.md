# Kauaiで曲を書く

culebraに付属する音楽言語Kauaiで、短い曲を書く手順を紹介します。最初の和音から、
くり返し、強弱、テンポの変化を含む曲までを、コマンドラインまたはculebraのプログラムから
鳴らすところまで扱います。言語の完全な仕様は[言語仕様](../kauai/language.ja.md)にあります。

曲は拡張子`.kau`のただのテキストファイルです。次の4つの部分を書き、ファイル内の順序は
自由です。

| 部分 | 中身 |
|---|---|
| `band` | 演奏する声（ボイス） |
| `groove` | 和音に合わせて書く伴奏 |
| `section` | リードシート。1行が1小節で、その和音とメロディを書く |
| `song` | テンポ、拍子、調、そして演奏する順のセクション |

## 最初の曲

次の内容を`first.kau`として保存します。

```kauai
band Duo {
  voice Tune  pulse     plays melody            vol 20
  voice Bass  triangle  plays roots in E2..D#3  vol 30
}

song First {
  tempo 100
  band Duo
  groove Walk
  melody in C5..B5

  Verse x2
}

groove Walk {
  Bass  1 - 5 -
}

section Verse {
  C     e4 g c'2
        d'4 c' g2
  F G   a4 f g d
  C     c1
}
```

コマンドラインで鳴らします。

```sh
culebra play first.kau
```

読み方は次のとおりです。

- **セクションの1行が1小節です。** 和音が先に来て、その後に音符を書きます。2行目は
  和音を書いていないので、`C`のままです。`F G`は1小節に2つの和音を並べます。
- **音符は音名です。** `c`はC、`e4`は4分音符のEです。長さは`1`（全音符）、`2`（2分音符）、
  `4`（4分音符）、`8`（8分音符）などです。長さを省くと、同じ小節の前の音符と同じ長さになります。
- **アポストロフィは1オクターブ上**です。`c'`は、音域`C5..B5`の上のCを指します。
  カンマは1オクターブ下げます。
- **グルーブは伴奏です。** `1`は和音の根音、`5`は5度、`-`は直前の音を伸ばします。
  グルーブは曲のすべての小節で鳴ります。
- **`Verse x2`** は、そのセクションを2回演奏します。

## 記号、強弱、テンポ

記号は音符の前に置き、指示は語をバックスラッシュで始めて書きます。指示は、その次の音符から
効きます。

```culebra
let song = Audio.Kauai.new(`
band Duo {
  voice Tune  pulse     plays melody            vol 20
  voice Bass  triangle  plays roots in E2..D#3  vol 30
}

song Second {
  tempo 100
  band Duo
  groove Walk
  melody in C5..B5

  Verse
  Chorus
}

groove Walk {
  Bass  1 - 5 -
}

section Verse {
  C     .e4 .g c'2
        \cresc d'4 c' g2 \f
  F G   a4 f g d
  C     c1 \rit 80
}

section Chorus {
  C     \tempo 120 c2 !c'4 g
  F G   a4 f g d
  C     c1
}
`)
println(song.length())
```

- 音符の前の`.`は、スタッカート（長さの半分）にします。`!`はアクセントです。
- `\cresc`は、次の強弱記号（ここでは`\f`）まで次第に強くします。強弱記号は、
  `\pp`、`\p`、`\mp`、`\mf`、`\f`、`\ff`です。
- `\rit 80`は、次のテンポ指示まで、テンポを均一に遅くします。
- `\tempo 120`は、その音符からテンポを設定します。

## プログラムから鳴らす

プログラムは`Audio.Kauai`で曲を鳴らします。`Audio.Kauai.new`は曲の文字列を、
`Audio.Kauai.load`はファイルを受け取ります。鳴らす前に、曲の長さや、鳴る音の並びを
調べられます。

```culebra
let song = Audio.Kauai.new(`
band Duo {
  voice Tune  pulse     plays melody            vol 20
  voice Bass  triangle  plays roots in E2..D#3  vol 30
}

song First {
  tempo 100
  band Duo
  groove Walk
  melody in C5..B5

  Verse x2
}

groove Walk {
  Bass  1 - 5 -
}

section Verse {
  C     e4 g c'2
        d'4 c' g2
  F G   a4 f g d
  C     c1
}
`)
println(song.length())             # => 19.2
println(song.events()[0])          # => {at: 0.0, len: 0.6, by: 'Tune', pitch: 76, vol: 20.0}
```

`song.render()`は、音声デバイスを使わずに、曲をサンプルの配列として返します。プログラムは
これをファイルに保存できます。コマンドラインでは`culebra play --wav first.wav first.kau`が
同じことをします。

## 間違いがあったとき

Kauaiは曲全体を鳴らす前に検査します。エラーには、その箇所の行番号が付くので、すぐに
見つけられます。セクションの行の先頭の語が和音でも調でもないとき（たとえば`Q c1`の`Q`）は、
どの音も鳴る前に止まり、`not a chord or key: Q`のようなメッセージが出ます。

## 次に読むもの

- [言語仕様](../kauai/language.ja.md)は、すべての語を載せています。声とその設定、記号、
  指示、くり返し、パート、ドラムなどです。
- [Kauaiで音を作る](kauai-sounds.ja.md)は、音を作る声と、外部の楽器を扱います。
