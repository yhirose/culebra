# Kauai 言語仕様

> **Status: Planned.** 本書はKauaiの設計であり、Culebraにはまだ実装されて
> いません。[付録C](#付録-c-完全な曲)の曲は、試作の展開器で展開し、1音ずつ
> 確かめてあります。

Kauaiは、バンドが演奏するとおりに音楽を書くための言語です。コードと
メロディのリードシートを書き、バンドがそれをどう伴奏するかをgrooveとして
書きます。Kauaiのファイルは拡張子`.kau`のプレーンテキストで、Culebraで
書いたプログラムから`Audio.Kauai`で再生します（[10章](#10-culebra-から再生する)）。

名前はハワイの島Kauaʻiに由来します。Culebraがプエルトリコの島の名前である
のと同じです。コードやファイル名では`kauai`、`.kau`と書きます。

## 目次

1. [概要](#1-概要)
2. [最初の曲](#2-最初の曲)
3. [名前と語](#3-名前と語)
4. [ファイル](#4-ファイル)
5. [バンド](#5-バンド)
6. [グルーヴ](#6-グルーヴ)
7. [セクション](#7-セクション)
8. [曲](#8-曲)
9. [エラー](#9-エラー)
10. [Culebra から再生する](#10-culebra-から再生する)
11. [付録 A: コードの種類](#付録-a-コードの種類)
12. [付録 B: 文法](#付録-b-文法)
13. [付録 C: 完全な曲](#付録-c-完全な曲)
14. [付録 D: 持ち込めない書き癖](#付録-d-持ち込めない書き癖)

## 1. 概要

曲は4種類の定義でできています。

| 定義 | 持つもの |
|---|---|
| `band` | 楽器（voice）とドラムキット |
| `groove` | 1小節以上の伴奏。音符ではなく、コードに対する位置で書く |
| `section` | リードシート。1小節1行で、コードとメロディを書く。書き出したパートも置ける |
| `song` | テンポ、拍子、調と、sectionを演奏する順番 |

設計は次の原則に従います。

1. **1行は1小節。** sectionの各行は1小節で、コード、音符の順に書きます。
   例外は`%%`の1行だけで、コード譜の2小節の反復記号と同じく2小節です。
2. **ミュージシャンが書くとおりに書く。** コード記号、音価で書く音の長さ
   （`c4.`は付点4分音符）、楽譜で音符に付ける印は音符に（`.c8`は
   スタッカート）、強弱やテンポは変わるところに（`\cresc`、`\rit 104`）。
3. **同じことを二度書かない。** grooveはそれを弾くすべての小節で共有され、
   コードは次のコードまで続き、調やendingを変えた繰り返しはsongの1行です。
4. **直した影響はその場所にとどまる。** 1つの音を直せば、その音だけが
   変わります。オクターブは前の音ではなく決まった範囲から決まり、省略した
   長さはその小節の中でしか引き継がれません。
5. **誤りは書いた場所で止める。** [9章](#9-エラー)のエラーはすべて演奏の
   前に見つかり、その行を示します。
6. **展開は決定的。** 曲は毎回同じ音に展開されます。乱数で選ぶものは
   ありません。

## 2. 最初の曲

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

sectionの各行は1小節で、コード、音符の順に書きます。2行目はコードを書いて
いないのでCのままで、`F G`は小節を2つのコードで分けます。`c'`と`d'`は
範囲`C5..B5`の1オクターブ上の音です。grooveはすべての小節の下で鳴り、
ベースは各コードのルート（`1`）を弾いて伸ばし（`-`）、次に5度（`5`）を
弾きます。

## 3. 名前と語

**名前とキーワード。** 大文字で始まる語は、曲の中で定義した名前です
（`Duo`、`Tune`、`Verse`、`Walk`）。小文字の語は言語のキーワードです
（`band`、`voice`、`plays`、`groove`）。音楽家が大文字で書くものは、ここでも
大文字で書きます。コード記号（`Fmaj7`）、音の高さと調（`C4`、`Eb`）、音程
（`M3`、`P5`）です。どれも、それを書く場所にだけ現れます。

**行とブロック。** Kauaiは1行ずつ読まれます。`{`で終わる行がブロックを
開き、`}`だけの行が閉じます。語は空白で区切り、インデントに意味は
ありません。

**コメント。** 行頭の`#`、または空白の直後の`#`から行末までがコメントです。
語の中の`#`はシャープなので、`g#`、`F#m7`、`D#3`はコメントではありません。

**範囲などに書く音の高さ。** 定義の中で音の高さを示すとき（`E2..D#3`、
`from C4`）は、大文字の音名、省略可能な`#`か`b`、オクターブ番号で書きます。
`C4`が中央のド（MIDI 60）です。音符の行では、同じ文字を小文字で書くと音符で、
後ろの数字は長さです。`e4`は4分音符のEで、`E4`は音の高さです。

**コード記号。** ルート（大文字の音名と省略可能な`#`か`b`）、コード表
（[付録A](#付録-a-コードの種類)）にある種類、省略可能な`/`の後のベース音から
なります。例: `Fmaj7`、`Bm7b5`、`C/D`、`Gmaj7/A`。種類はコード譜で使う綴りでも
書けます（`CM7`、`C△7`、`C-7`、`Cø`、`C°7`、`C+`）。テンションは、コード譜と
同じく後ろの括弧にカンマ区切りで書きます（`C7(9,13)`、`Cm7(11)`、`C7(b9,#11)`、
[テンション](#テンション)）。`b`と`#`の代わりに`♭`と`♯`も書けます。大文字の前の`/`はベース音を
示し、数字の前の`/`は種類の一部です（`C6/9`）。ルートは音名の直後の`#`や`b`を
取るので、`Cb9`はCフラットのナインスです。`N.C.`はコードなしです。

## 4. ファイル

演奏するファイルは、`song`をちょうど1つ持ちます。`song`を持たないファイルは
ライブラリで、そのband、groove、sectionをほかのファイルから使うためのもの
です。

**`use 'file'`** は、別のファイルの定義を、その位置に書かれていたかのように
取り込みます。パスは引用符で囲み、`use`を書いたファイルからの相対パスです。
取り込まれるファイルは`song`を持てません。

**`voicings { ... }`** は、[付録A](#付録-a-コードの種類)の表にコードの種類を
追加します。1行に1つ、種類の名前とルートからの音程（半音）を書きます。
名前は`#`や`b`で始められません。ルートのシャープやフラットと読めてしまうため
です。表にすでにある種類や綴りも名前にできません。

```kauai
voicings {
  7b5     0 4 6 10
  9sus4   0 5 7 10 14
}
```

同じ種類で同じ名前の定義（2つのgroove、2つのsectionなど）があるとエラーです
（同じファイル内でも、ファイルをまたいでも）。grooveとsectionが同じ名前を
持つのは構いません。

## 5. バンド

```kauai
band Run {
  voice Lead    pulse     plays melody            duty 1/4  vol 15  env 1 2 4  gap 2
  voice Second  pulse2                            duty 1/8  vol 9   env 1 2 6  gap 2
  voice Bass    saw       plays roots in E2..D#3            vol 18  env 0 3 3  gap 3
  voice Arp     triangle  plays chords from C4              vol 7   env 1 4 6  gap 4

  Second echoes Lead      late 3/16  level 70%
  Second harmonizes Lead  under m3

  drum Kick   noise 120       len 1  vol 20  env 0 3 2
  drum Tom    noise 900->500  len 3  vol 12  env 0 0 4
}
```

### ボイス

`voice NAME SOURCE [plays ...] OPTIONS...`

音源は[`Audio.tone`](../stdlib.ja.md#tone)のチャンネル（`pulse`、`pulse2`、
`triangle`、`saw`）のどれか、またはプログラムが用意する楽器を表す
`host NAME`です（[10章](#10-culebra-から再生する)）。チャンネルは一度に1音
しか鳴らせず、bandの2つの楽器が同じチャンネルを共有することはできません。
ドラムは`noise`のチャンネルを使います。

**何を弾くか**で、その楽器の行の読み方が決まります。

| 書き方 | その楽器が弾くもの |
|---|---|
| `plays melody` | sectionのメロディ。bandの中でちょうど1つの楽器が弾く |
| `plays roots in LO..HI` | コードの度数で書くベースライン（[ベースの行](#ベースの行)）。`LO..HI`は各コードのベース音を置く1オクターブ |
| `plays chords from NOTE` | コードの中の位置（[和音の行](#和音の行)）。`NOTE`から上に積む。`plays chords rootless from NOTE`はルートを除く |
| `plays part NAME...` | sectionが書いたパート。1つ以上の名前を書ける（`plays part Upper Lower`、[パート](#パート)）。その小節ではgrooveの行の代わりに弾く。ほかの`plays`と併用できる |
| （`plays`なし） | echoやharmonyで作るパートだけ（[派生パート](#派生パート)） |

コードは、その種類の音程から積みます。最初の音は、その音名の`NOTE`以上で
最も低い音、以降の各音は、その音名の、直前の音より高い中で最も低い音です。
`plays chords from C4`では`Fmaj7`はF4 A4 C5 E5、
`plays chords rootless from E3`では`Abmaj9`はC4 Eb4 G4 Bb4になります。

| オプション | 意味 | 既定値 |
|---|---|---|
| `duty 1/8` `1/4` `1/2` `3/4` | パルス波のデューティ比 | `1/2` |
| `vol N` | `mf`のときの音量。`Audio.tone`と同じ`0`〜`100`。host楽器では楽器自身の音量に対する百分率（`vol 46%`） | （必須） |
| `env A D R` | アタック、ディケイ、リリース（1/60秒のtick） | `0 0 0` |
| `gap N` | 各音の終わりを切るtick数。音と音を区切って聞かせるため | `0` |
| `poly` | 複数の音を同時に鳴らせるhost楽器 | 一度に1音 |

host楽器は、各音の音を最後まで鳴らします。`env`、`gap`、音の長さは適用
されません。名前の後に`name=value`の形で自分用のパラメータを持てます
（`host Ep bright=0.8`）。Kauaiはこれを各音と一緒にホストに渡します。

### 派生パート

bandの中で、楽器の名前で始まり`echoes`か`harmonizes`が続く行は、その楽器に
別の楽器の音から作るパートを与えます。どちらを鳴らすかは、songがsection
ごとに選びます（[8章](#8-曲)）。

| 行 | 楽器`V`が弾くパート |
|---|---|
| `V echoes SOURCE late 3/16 level 70%` | SOURCEの音を、ある長さ（ここでは16分音符3つ分）遅れて、Vの`vol`に対する百分率で |
| `V harmonizes SOURCE under m3` | SOURCEの各音について、その下で鳴っているコードの音のうち（オクターブは問わない）、その音程以上下にある最も高い音を、同じ長さで |

音程は`m2 M2 m3 M3 P4 TT P5 m6 M6 m7 M7 P8`で書きます。echoはsectionの境界を
またいで元の音を読み、繰り返す曲では、曲の最初の音が曲の最後をechoします。

### ドラム

`drum NAME noise FREQ[->END] len N vol V [env A D R] [plays hits]`
`drum NAME host SOUND [name=value...] vol P% [plays hits]`

noiseのドラムは、ノイズのチャンネルで鳴る打音です。`FREQ`は高さ（Hz）で、
`END`を書くとそこまで滑らせ、`len`のtickだけ保ちます。チャンネルは一度に
1つのドラムしか鳴らせないので、2つのnoiseのドラムが同時に鳴ることは
できません。

hostのドラムは、プログラムが用意する音（[10章](#10-culebra-から再生する)）を、
その音自身の音量に対する百分率で、終わりまで鳴らします。hostのドラムは、
ドラムセットの各太鼓のように、ほかのhostのドラムともnoiseのドラムとも同時に
鳴らせます。

```kauai
band Combo {
  voice Keys  host Ep bright=0.8  plays chords rootless from E3  poly  vol 40%
  voice Bass  triangle            plays roots in E2..D#3            vol 30

  drum Kick   host Kick   vol 80%
  drum Ride   host Ride   vol 50%
  drum Hat    host Hat    vol 40%
}
```

`plays hits`を付けたドラムは、sectionのキメで鳴ります。付けていないドラムは、
キメの間は休みます（[キメとブレイク](#キメとブレイク)）。

## 6. グルーヴ

```kauai
groove Hook {
  Bass   1 - 8 -  1 8 - 1  - 8 1 -  5 - > -
  Arp    4 3  2 1  2 3  4 3
  Kick   x... ...x x... ....
  Hat    ..x. ..x. ..x. ..x.
  Snare  .... x... .... x...
  first {
    Kick   .... ...x x... ....
    Crash  x... .... .... ....
  }
  last {
    Snare  .... x... ...x xx..
    Floor  .... .... .... ..xx
  }
}
```

grooveは、伴奏する楽器やドラムごとの行でできていて、行頭にその名前を
書きます。行はグリッドで、1小節に`n`個のトークンを書くと、各トークンは
小節の`n`分の1になります（4/4で16個なら16分音符、8個なら8分音符）。そのため
grooveはどの拍子の小節にも合います。各トークンは、普通の音価、付点、3連符の
いずれかの長さでなければなりません（4/4で12個なら8分の3連符、15個はエラー）。`|`で次の小節に移り、2小節の行は2小節
ごとに繰り返します。grooveの小節は、sectionの最初の小節、またはそのgrooveを
指定した`groove`行から数え、各行は自分の小節を順に巡ります。そのため1小節の
行は、2小節の行と並べても毎小節鳴ります。どの行でも、`-`は直前の音を伸ばし、
`.`は無音です。grooveに行のない楽器は、そのgrooveの間は鳴りません。

### ベースの行

`plays roots`の楽器は、コードの度数を読みます。

| トークン | 音 |
|---|---|
| `1` | コードのベース音（`/`の後の音、なければルート）を`LO..HI`に置く |
| `3` `5` `7` `9` | コードが持つその度数（`Bm7b5`の`5`はF）を、`1`以上で最も低い高さに |
| `8` | `1`の1オクターブ上 |
| `>` / `<` | 次のコードの`1`の半音下 / 上。`>`は下から、`<`は上からそこへ導く |

各トークンは、その時刻に鳴っているコードを読みます。`C F`の小節では、前半の
トークンはC、後半はFを読みます。次のコードとは、曲が次に演奏する小節の最初の
コードです。ループしない曲の最後の小節には次のコードがないので、`>`と`<`は
そのコード自身の`1`を弾きます。コード表にその度数がないコードは、既定値の
4、7、10、14半音を使います。

### 和音の行

`plays chords`の楽器は、ボイシングの中の位置を下から数えて読みます。
`1 2 3 …`（最上音より大きい数は最上音）、または和音全体を同時に鳴らす`x`
（`poly`の楽器）です。

### ドラムの行

ドラムの行は、叩くところを`x`、叩かないところを`.`として、1ステップ1文字で
書きます。文字の間の空白は無視されます。

### 最初と最後の小節

`first { ... }`と`last { ... }`は、grooveが下で鳴る各sectionの最初と最後の
小節で、同じ名前の行を置き換える行を持ちます（`xN`の各回も数えます）。
sectionをクラッシュで始めたり、フィルで終えたりするのに使います。

## 7. セクション

```kauai
section Refrain {
  pickup  \pp (e g
  Cadd9         \tempo d'2.
  C/G           c2 e'4
  Cmaj7         b2.
  C6/G          a2.)
  G7 G7 C/E     \cresc (a b c \!
  ending 1 {
    C           c,2.~
    D7 D7 C/E   c,) (b c)
  }
  ending 2 {
    C           c,2.~
    D7 D7 C/E   c,) (b c)
  }
}
```

`section NAME [key KEY] { ... }`は、小節の行と次の行を持ちます。

| 行 | 意味 |
|---|---|
| `pickup NOTES` | sectionへ導く弱起の音。直前に演奏される小節の最後の拍に重なって鳴り、その小節はそこで休んでいなければならない（[弱起](#弱起)） |
| `groove NAME` | その後の小節のgroove |
| `meter N/D` | その後の小節の拍子（[拍子の変化](#拍子の変化)） |
| `melody in LO..HI` | このsectionの音の範囲（[オクターブ](#オクターブ)） |
| `NAME: NOTES` | 小節の下に書き、その小節でパート`NAME`が弾く音（[パート](#パート)） |
| `%` / `%%` | 直前の小節、または直前の2小節をもう一度（[反復記号](#反復記号)） |
| `hits: x8 r ...` | 小節の下に書き、その小節でバンドがgrooveの代わりに弾くリズム（[キメとブレイク](#キメとブレイク)） |
| `2nd: NOTES` | 小節の下に書き、2回目に演奏するときのその小節（[2回目以降](#2回目以降)） |
| `part NAME { ... }` | パート`NAME`を書き出したもの。1行1小節（[パート](#パート)） |
| `part NAME in LO..HI` | パート`NAME`の音の範囲 |
| `swing 8` / `straight` | その後の小節のスウィング（[スウィング](#スウィング)）。`straight`は書いたとおりに弾く |
| `ending N { ... }` | sectionを締めくくる小節。演奏する回ごとに違うものを使う。`ending N as written { ... }`はsongの`in`で移調されない |

`key KEY`は、sectionが書かれている調を示し、songの`in`はこの調から移調
します。既定はsongの調です。`in`なしで演奏するsectionは、songの調にかかわらず
書いたとおりに鳴ります。

### 小節の行

小節の行は、その小節のコード、音符の順に書きます。コードは、行頭で大文字で
始まる語、または`N.C.`です。コードは小節を等分するので、その個数は小節の拍数
の約数でなければなりません。コードの間の`/`は、コード譜と同じビート・スラッシュで、
直前のコードをもう1拍続けます。4/4の`C / / G7`は、Cが3拍、G7が1拍です。
スラッシュを使う小節は、拍ごとにコードか`/`を1つずつ書きます。`/`で始まる小節は、
鳴っているコードを続けます。コードを書かない小節は、直前の小節のコードのままです。
sectionとendingの最初の小節は、コードを書きます。`N.C.`の間は、ルートと和音を
弾く楽器が休み、ドラムは続けます。

音符のない、コードだけの小節では、メロディは休みます。コード譜のスラッシュだけの
小節と同じです。

```kauai
section Vamp {
  Dm7 / G7 /
  Dm7 / G7 /
  Cmaj7         e4 d c2
}
```

### 反復記号

`%`だけの行は、コード譜の反復記号と同じく、直前の小節をもう一度弾きます。
コード、音符、その下に書いた行（パート、キメ、`2nd:`の行）も含みます。`%%`は
直前の2小節をもう一度弾き、2小節になります。繰り返した小節は、grooveと拍子と
スウィングの上では自分の位置のものに従います。違う小節は書き出すので、`%`の下
には何も書けません。

```kauai
section Riff {
  Dm7           d8 f a c' r2
  G7            b4 g r2
  %%
  Cmaj7         e1
}
```

### 音符

| 書き方 | 意味 |
|---|---|
| `c` `d` `e` `f` `g` `a` `b` | 音符。常に小文字 |
| 後ろに`#` / `b` | シャープ / フラット（`f#`、`eb`。`bb`はBのフラット。`##` / `bb`で重嬰 / 重変: `f##`、`ebb`。Bの重変は`bbb`） |
| さらに後ろに`'` / `,` | 範囲の1オクターブ上 / 下（`e'`、`c,`） |
| `1 2 4 8 16 32` | 全音符、2分、4分、8分、16分、32分 |
| 音価の後の`.` | 付点（`..`で複付点）: `g4.` |
| （音価なし） | 同じ小節の直前の音と同じ長さ（付点も含む）。小節の最初の音なら1拍 |
| 末尾の`~` | 次の音へのタイ。次の音は同じ高さ |
| `r` | 休符。音符と同じく音価を付ける（`r4`、`r2.`） |
| `3[c8 d e]` | 連符。数字は中の音の数 |
| `2:3[c8 d]` | 比を書いた連符。3つ分の時間に2つ |
| `<c e g>4` | 和音。中の音を、1つの音符と同じように同時に鳴らす |
| `{d}c4` | 装飾音。導く音の前に書く |

音は名前のとおりの高さです。調はシャープやフラットを加えないので、`key D`でも
Fシャープは`f#`と書き、`f`はFナチュラルです。臨時記号はその音だけに付き、
小節の残りには及びません。`f# g f`はFシャープ、G、Fナチュラルです。

連符は、中に書いた長さを超えない範囲で最も長い普通の音価（全音符、2分、
4分…）に収まります。`3[c8 d e]`は4分音符1つ分に8分音符3つ、`3[c4 d e]`は
2分音符1つ分に4分音符3つ、`5[c16 d e f g]`は4分音符1つ分に16分音符5つです。
数字は`[`に付けて書くので、`\tempo 3[c8 d e]`は曲のテンポに戻ってから連符を
弾きます。

この規則で収まらない連符は、楽譜に印刷されるとおりに比`p:q`を付けて書きます。
中の音は書いた長さの`q/p`倍で鳴り、`q`個分の時間に`p`個が入ります。6/8拍子の
`2:3[c8 d]`は8分音符3つ分の時間に2つ、`4:6[c8 d e f]`は6つ分の時間に4つです。
比は音の数ではなく音価を数えるので、長さの違う音が混ざる連符にも使えます。
`3:2[c4 d8]`は、4分音符1つ分の時間に4分音符と8分音符が1つずつ入ります。
`3[c8 d e]`は`3:2[c8 d e]`を短く書いたものです。

和音は、高さを複数持つ1つの音符です。長さは`>`の後ろに、奏法の印は`<`の前に
書き（`.<c e g>8`）、臨時記号とオクターブの印は中の各音に付けます
（`<c e' g>`）。`>`の後ろの`~`は和音全体をタイでつなぎ、中の音の後ろの`~`は
その音だけをつなぎます。そのため、和音の一部だけを伸ばしてほかを変えることが
できます。`<g,~ b~ e g~>2.`の次に`<g, b d g>4`と書くと、G、B、Gは伸び、Dが
新しく鳴ります。同時に鳴る音は`poly`の楽器が弾く必要があります。メロディが
和音を含むとき、派生パートはその最高音をもとにします。

装飾音は、導く音の前に波かっこで書きます（`{d}c4`、`{e d}c4`）。1つにつき
64分音符1つ分を後の音の頭から借り、後の音は残りの長さで鳴ります。装飾音には
臨時記号とオクターブの印を付けられますが、長さや奏法の印は付けません。後の音は
和音でも構いませんが、休符やタイで伸びている音には付けられません。

### オクターブ

`'`も`,`も付かない音は、範囲の中のその音名の位置に置かれます。範囲はsectionの
`melody in`、なければsongのものを使います。範囲は、幹音から、その1オクターブ上
の半音下までの1オクターブです（`melody in F5..E6`、`melody in D4..C#5`）。
そのため各音名の幹音は範囲にちょうど1回現れ、シャープやフラットは五線と同じく
そこから音を動かします。`C4..B4`では、`cb`は`c`のすぐ下（B3）、`b#`は`b`の
すぐ上（C5）です。`'`1つで1オクターブ上、`,`1つで1オクターブ下に動きます。
音は直前の音に依存しないので、1つを直してもほかは変わりません。旋律の最も
低い音の近くから範囲を始めると、印が少なくて済みます。

### 音符に付ける印

印は音名の前に、音の長さは音名の後に書きます。

| 書き方 | 印 | 鳴らし方 |
|---|---|---|
| `.c8` | スタッカート | 長さの半分 |
| `!c4` | アクセント | 1段階強く |
| `-c4` | テヌート | 長さいっぱい |
| `^c4` | マルカート | 1段階強く、長さの4分の3 |
| `~<c e g>2` | アルペジオ | 和音を分散させる。下の音から64分音符ずつずらして鳴らし始め、一緒に終える |
| `(c8 d e f)` | スラー | 各音を次の音へ切れ目なくつなぐ。最後の音はつながない |
| `@c2` | フェルマータ | 伸ばす。その時間を2倍の長さで弾き、バンド全体も一緒に待つ |

印は重ねられます（`.!c8`はアクセント付きのスタッカート）。休符にはフェルマータ
だけを付けられます（`@r2`はバンド全体の休止）。タイで伸びている音（弾き直さない
音）には印を付けられません。フェルマータの音も、小節の長さには書いた長さで数え
ます。アルペジオの`~`は、楽譜が波線を書く位置、つまり`<`の直前に
書きます。音の後ろの`~`はタイなので、`~<g, g>2.~`は次へタイでつながる分散
和音です。タイで分散和音へ伸びてきた音は弾き直さず、分散は弾き直す音のうち
最も低い音から始まります。装飾音は印より前に書きます（`{d}.c8`）。

スラーは、曲が演奏する順に`(`から`)`まで続きます。小節やsectionをまたげるので、
弱起で始めて何小節も先で終えられます。スラーは一度に1本です。スラーが開いて
いる間の`(`、開いていないときの`)`、曲の終わりでまだ開いているスラーはエラー
です。endingはsectionの本体の続きなので、本体が開いたままにしたスラーは、
楽譜が最初のendingでだけ閉じて描いていても、各endingで閉じます。

```kauai
section Tag {
  C             (e4 d c2~
  ending 1 {
    G7          c1)
  }
  ending 2 {
    G7          c2) r
  }
}
```

### 指示

`\`で始まる語は指示で、その後の音から効きます。

| 書き方 | 意味 |
|---|---|
| `\pp` `\p` `\mp` `\mf` `\f` `\ff` | 強弱。楽器の`vol`の0.4、0.55、0.7、0.85、1.0、1.15倍。曲は`mf`で始まる |
| `\cresc` / `\dim` | 次の強弱の指示まで、時間に沿って均等に強く / 弱く |
| `\!` | `cresc`や`dim`をここで終える。始めたときより1段階強く / 弱くなる |
| `\tempo 114` | ここからこのテンポ。`\tempo 4.=76`はほかの音価（ここでは付点4分音符）で数える |
| `\rit 104` / `\accel 140` | 次のテンポの指示で着くように、均等に遅く / 速く |
| `\tempo` | 曲のテンポに戻る |

小節の行に書いた強弱は、バンド全体のものです。チャートの`mf`や`cresc.`を
演奏者全員が読むのと同じで、メロディ、パート、groove、ドラム、派生パートの
すべてが、その音量にそれぞれの`vol`を掛けた音量で鳴ります。そのため、Aメロで
全体を落とし、サビに向かって全体で盛り上げる、という書き方がそのままできます。
パートの行に書いた強弱は、そのパートだけの音量を、その位置から次に小節の行に
強弱が書かれるまで決めます。そこからは、パートもまたバンド全体の強弱に従います。
`cresc`や`dim`は、曲が次に演奏するsectionへ続けられます。

強弱の指示で終わる`cresc`は、その指示で終わるので`\!`は要りません。`p`から
`\cresc a b c \f d`と書くと、`a`、`b`、`c`と強くなり、`d`で`f`に着きます。
`\!`は始めたときから1段階の所で終えるので、`p`から`\cresc a b c \! \f d`と書くと
`mp`までしか上がらず、`d`で`f`に跳びます。

テンポの指示はバンド全体に効きます。`\rit N`と`\accel N`は、書いた位置から、
曲が次に演奏するテンポの指示（後のsectionにあっても構いません）まで均等に
テンポを変え、そこで`N`に着きます。そこからは、その指示のテンポになります。
後にテンポの指示がなければ、曲の終わりで`N`に着きます。

### パート

パートは、伴奏のために書き出した音の並びです。フィル、導入、対旋律、2重奏や
3重奏の各声部などがあたります。sectionにはパートの名前だけを書き、楽器の名前は
書きません。誰が弾くかは、メロディの`plays melody`と同じように、bandの
`plays part NAME`が決めます。パートが書かれた小節では、そのパートを弾く楽器は
grooveの行の代わりにパートの音を弾き、それ以外の小節ではいつもどおりgrooveを
弾きます。

パートは2つの書き方のどちらかで書きます。1つ目は、小節の下に、パートの名前と
コロンで始まる行を置き、その小節の音を書く形です。

```kauai
section Intro {
  N.C.          r2.
    Fill:       r4 e g
  G9            r2.
}
```

2つ目は、section（またはending）の中に`part NAME { ... }`のブロックを置き、
小節の順に1行ずつ書く形です。パートに音のない小節は`.`と書きます。

```kauai
section Theme {
  C             e4 g c'2
    Alto:       c2 e
  G7            d'4 c' g2
    Alto:       b2 d
  C             e2 c

  part Cello {
    c1
    g,1
    .
  }
}
```

2つの書き方の意味は同じです。1つ目は、スコアのように各小節のパートを上下に
まとめて見せるので、ときどき現れるパートに向いています。2つ目は、パート譜の
ように1つのパートを最初から最後まで続けて見せるので、曲の間ずっと続く声部に
向いています。1つのパートは、section（またはending）ごとにどちらか一方の書き方
で書き、ブロックの行数はその小節数とちょうど同じにします。

パートの音は、メロディと同じ書き方です。`'`も`,`も付かない音は、パートの範囲
（sectionかsongの`part NAME in LO..HI`、なければメロディの範囲）に置かれます。

### キメとブレイク

バンドは、grooveを離れて全員でリズムを打ったり（キメ）、演奏を止めて1小節を
1人に任せたり（ブレイク）します。小節の下の`hits:`行が、そのリズムです。
`x`はバンドが鳴らすところ、`r`は休むところで、音符と同じく音価、付点、タイ、
連符と、印の`!`と`.`を書けます。

その小節では、バンドはgrooveの代わりにキメを弾きます。

| 誰が | 各打で弾くもの |
|---|---|
| `plays roots`の楽器 | コードのベース音（ベースの行の`1`） |
| `plays chords`の楽器 | コード。和音の行の`x`と同じボイシングで、`poly`でない楽器ならその一番上の音 |
| `plays hits`のドラム | 1打 |
| そのほかのドラム | 何も弾かず、その小節は休む |

1打は音価のぶん鳴り（`x4`は4分音符）、`.`が付けばその半分、タイでつなげば
伸びます（`x8~ x8`）。各打は、その時点で鳴っているコードと、バンドの強弱に
従います。`!`を付けると1段階強くなります。メロディは自分の音符を弾き、
sectionがパートを書いているところではパートの楽器はそのパートを弾きます。
grooveの上で弾くときと同じです。

休符だけの`hits:`行がブレイクです。バンドはその小節の間は鳴らず、メロディや
パートだけが弾きます。

```kauai
section Kime {
  Dm7 G7          d'8 f' a' g' f'4 d'
  EbM7 F          !g'8. f'16 r8 !g' r !bb'4.
    hits:         !x8. x16 r8 !x r !x4.
  G7sus4          r4 d'8 e' f' g' a' b'
    hits:         !x4 r r2
  C               c'1
    hits:         r1
}
```

キメで鳴るドラムは、bandで指定します。

```kauai
band Fusion {
  voice Lead  saw                 plays melody                     vol 12
  voice Keys  host Ep  poly       plays chords from C4             vol 40%
  voice Bass  triangle            plays roots in E2..D#3           vol 30

  drum Kick   host Kick   vol 80%  plays hits
  drum Crash  host Crash  vol 60%  plays hits
  drum Hat    host Hat    vol 40%
}
```

`2nd hits:`は2回目のキメで、`2nd hits: .`と書けばその回はgrooveを弾きます
（[2回目以降](#2回目以降)）。ブレイク中のドラムのフィルは、キメではなく、
その小節だけのgrooveで書きます。

### 2回目以降

曲が2回以上演奏するsectionは、回によって1、2小節だけ違うことがあります。
小節の下の`2nd:`で始まる行は、曲がそのsectionを2回目に演奏するときのその小節
です。`3rd:`なら3回目、以下同様です。回数は曲全体で数えます。そのsectionを
演奏するsongの行が1つにつき1回、`xN`なら`N`回です。ループする曲は、最初の
1周だけを数えます。

```kauai
section Verse {
  C             e4 g c'2
  G7            d'4 c' g2
  2nd:          d'4 b g2
  F G           a4 f g d
  C             c1
  2nd:          C7  c2 r
  F             f1
  2nd:          Fm
}
```

この行には、その回のコード、音符、または両方を書きます。コードを書かなければ
元の小節のコードのまま、上の`2nd: Fm`のように音符を書かなければ、元の小節の
音符を新しいコードの上で弾きます。変わるのはその小節だけで、次の小節は、
コードを書いていなくても元のコードのままです。パートでは、小節の下の`2nd NAME:`が、その回にパート`NAME`が
弾く音です。パートのブロックでは、ブロックの行の下の`2nd:`の行が2回目のその行
になり、1小節とは数えません。元の小節にそのパートの音がなくても構わず、`.`と
書けばその回はパートを弾きません。同じように、`2nd hits:`はその回のその小節の
キメで、`2nd hits: .`はその回はキメなしです（[キメとブレイク](#キメとブレイク)）。

```kauai
section Theme {
  C             e4 g c'2
    Alto:       c2 e
    2nd Alto:   c2 g
  G7            d'4 c' g2

  part Cello {
    c1
    g,1
    2nd: b,1
  }
}
```

`Alto 2nd:`ではなく`2nd Alto:`と回数を先に書くのは、行の最初の語で、その行が
何の行かがわかるようにするためです。

### 弱起

`pickup`行は、sectionの最初の小節へ導く音を持ちます。音価のない最初の音は、
その小節の1拍です。音は、曲がそのsectionの前に演奏する小節の最後の拍に重なって
鳴り、その小節はそこで休んでいなければなりません。`x2`で演奏するsectionは、
2回目の弱起を自分の最後の小節に重ねます。

そのsectionが曲の最初なら、曲は弱起から始まります。最初の小節の前に、弱起の
長さだけの小節が置かれ、そこではバンドは弾きません。ループする曲では、弱起は
曲の最後の小節の最後の拍にもう一度重なって鳴るので、その小節はそこで休んで
いなければなりません。

```kauai
section Opening {
  pickup  g8 a
  C             c2 e4 d
  G7            b,1
}
```

### 拍子の変化

section（またはending）の中の`meter N/D`行は、その後の小節の拍子を、その
sectionの終わりまで変えます。ほかのsectionはsongの拍子のままです。endingは、
sectionの本体が終わったときの拍子で始まります。テンポは変わらず4分音符で数え、
音価のない小節の最初の音はその小節の拍子の1拍、grooveの行は各小節に合わせて
分割されます。

```kauai
section Turn {
  C             e4 g c'2
  meter 3/4
  F             a2.
  G             g2 d4
  meter 4/4
  C             c1
}
```

## 8. 曲

```kauai
song JeTeVeux {
  tempo 124
  meter 3/4
  key C
  band Salon
  groove Valse
  melody in D4..C#5

  Intro
  Refrain  ending 1
  Couplet
  Refrain  ending 2
  Couplet
  Refrain  ending 3
  Coda
}
```

| 行 | 意味 | 既定値 |
|---|---|---|
| `tempo N` | 1分あたりの4分音符の数。拍子によらない（小数も可）。`tempo 4.=60`はほかの音価（ここでは6/8拍子の譜面と同じく付点4分音符）で数える | （必須） |
| `meter N/D` | 1小節の拍数と、1拍の音価。sectionで変えられる（[拍子の変化](#拍子の変化)） | `4/4` |
| `key K` | 調。音名で書き、短調は`m`を付ける（`Eb`、`F#m`） | `C` |
| `band NAME` | 演奏するband | （必須） |
| `groove NAME` | grooveを指定せず、同じ名前のgrooveもないsectionが使うgroove | なし |
| `melody in LO..HI` | `'`も`,`も付かない音の範囲 | （範囲のないsectionがあれば必須） |
| `part NAME in LO..HI` | パート`NAME`の音の範囲 | メロディの範囲 |
| `swing 8` | 8分音符の組（`swing 16`なら16分音符）を長短で弾く。比を続けなければ2:1（`swing 8 3:2`）（[スウィング](#スウィング)） | 書いたとおり |
| `loop` | 最後まで行ったら最初から繰り返す | 1回だけ |
| `mark NAME` | プログラムから問い合わせられる位置（[10章](#10-culebra-から再生する)） | |

それ以外の行はsectionを演奏します。オプションは次のとおりで、順序は自由、
それぞれ1回までです（`Chorus with echo ending 2 in E`）。

| オプション | 意味 |
|---|---|
| `with echo` / `with harmony` | その下で鳴らす派生パート |
| `ending N` | 本体の後に演奏するending |
| `in K` | 調`K`で演奏する。sectionの調から、小さいほうの音程で動かす（上へは増4度まで） |
| `xN` | `N`回演奏する |

各小節は、sectionが`groove`で指定したgroove、なければsectionと同じ名前の
groove、なければsongの`groove`を使い、どれもなければ伴奏なしです。

### スウィング

音符はまっすぐ書き、スウィングは弾き方として指定します。コード譜がまっすぐの
8分音符の上に*Swing*と書くのと同じです。`swing 8`では、小節の頭から数えた
8分音符2つずつの組を長短で弾きます。1つ目を組の時間の3分の2で、2つ目を
残りの3分の1で弾くので、3連符の8分音符と同じになります。`swing 8 3:2`は
長いほうと短いほうの比を変え、`swing 16`は16分音符を組にします。

組の前半と後半の中の時刻は、それぞれ比例して動きます。そのため、`swing 8`の
下の16分音符も一緒に跳ね、音の順序が入れ替わることはありません。連符は、
始まりと終わりが鳴る位置の間に中の音を等間隔に並べるので、1拍の3連符は
書いたとおりに弾きます。スウィングはバンドが弾くものすべてに効きます。
メロディ、パート、grooveの行とドラムです。

songの`swing`はすべてのsectionに効きます。section（またはending）の中の
`swing`や`straight`の行は、その後の小節のスウィングを変えます。

```kauai
section Bridge {
  straight
  Dm7           d8 f a c' b a g f
  G7            e4 g2 r4
  swing 8 3:2
  Cmaj7         e8 g b d' c' b a g
}
```

## 9. エラー

曲は演奏の前に全体が検査されます。各エラーはファイルと行を示します。grooveの
中のエラーは、grooveの行と、それが鳴った小節を示します。

- 拍子の長さにならない小節、コードが拍を等分しない小節（4/4の`C F G`）。スラッシュを使っていて、拍ごとにコードか`/`が1つずつになっていない小節（4/4の`C / G7`）
- 解釈できない音符、音価、コード、指示。大文字で書いた音符はコードとして読まれ、そこでエラーになる。コード表にない種類（`Cdim9`）。[テンション](#テンション)の表にないテンション（`C7(10)`、`C7(b11)`）、同じテンションの重複
- シャープとフラットの両方が付いた音（`c#b`）、3つ以上のシャープやフラット
- 休符に付けたフェルマータ以外の印、タイで伸びている音に付けた印、違う音やタイから休符へのタイ、曲の終わりでまだつながっているタイ
- スラーが開いている間の`(`、開いていないときの`)`、曲の終わりでまだ開いているスラー
- 比のない連符で、数字と中の音の数が合わないもの。0より大きい2つの数になっていない比（`2:3`のように書く）
- 1音だけの和音、`poly`でない楽器で同時に鳴る音
- 小節の下とブロックの両方に書いたパート、行数が小節数と違うパートのブロック、どの小節よりも前にあるパートの行
- 1音だけのアルペジオ、休符やタイで伸びている音の前の装飾音、装飾音やアルペジオに対して短すぎる音
- 変える小節や行より前にある`2nd:`の行、コードも音符もない`2nd:`の行、曲がそのsectionを演奏しない回（2回演奏するsectionの`3rd:`）、`1st` `2nd` `3rd` `4th`…以外の書き方の回数
- `x`と`r`以外を含む、小節の長さにならない、どの小節よりも前にある、1つの小節の下に2回書いた`hits:`行。楽器（voice）に付けた`plays hits`
- コードのない、sectionやendingの最初の小節
- 同じsectionやendingの中に繰り返す小節がない`%`や`%%`、`%`の下に書いた行
- 直前の小節の音に重なる弱起、直前の小節より長い弱起。ループする曲の最初の弱起が、曲の最後の小節の音に重なるもの
- 進行中の`cresc`や`dim`がない`\!`、曲の終わりでまだ続いている`cresc`や`dim`、テンポを書いていない`\rit`や`\accel`（`\rit 104`のように書く）
- 音価でないもので数えたテンポ（`tempo 3=60`）
- 定義されていないgroove、楽器、ドラム、section、band、ending、派生パート、2回定義された名前
- トークンのない小節や、トークンが音価にならない小節（4/4で15個）があるgrooveの行
- 同時に鳴る2つのnoiseのドラム、同じチャンネルにある2つの楽器
- 音の名前がない、`len`や`env`を持つ、`vol`が百分率でない、hostのドラム
- メロディを弾く楽器がない、または2つ以上ある
- 1オクターブでない範囲、シャープやフラットの音から始まる範囲
- 1拍が音価でない`meter`（`3/5`）
- `N.C.`の前の`>`や`<`
- songの行で2回書いたオプション（`in D in E`）
- 取り込まれるファイルにある`song`、1つのファイルに2つ以上の`song`
- `#`や`b`で始まる、またはコード表にすでにある`voicings`の名前
- `8`と`16`以外への`swing`、「長い:短い」になっていない比（`3:2`のように書く）、組が小節を埋めないスウィング（3/8拍子の`swing 8`）

## 10. Culebra から再生する

```culebra
# doctest: skip (Kauai is not implemented yet; this is the planned API)
let song = Audio.Kauai.load("ballad.kau", voices: {
  ep: fn (pitch, params) { Audio.Sound.new(electric_piano(pitch, params.bright)) },
})
song.prepare()          # build one host note; answers how many remain
song.play()
song.volume(0.5)
song.reached("Drive")   # has the song passed `mark Drive`?
song.stop()
```

`Audio.Kauai.load(path)`は曲のファイルを読み、そのファイルが`use`で指定した
ファイルを、そこからの相対パスで読みます。`Audio.Kauai.new(text)`は曲を
文字列で受け取り、その中では`use`を使えません。どちらも曲を検査し、
[9章](#9-エラー)の最初のエラーを投げます。

runtimeは、すべての音を音声ストリーム自身のクロック上でスケジュールします。
そのため、長いフレームがあってもテンポは崩れず、フレームループのない
プログラムでも演奏できます。

host楽器は1音ずつ作ります。曲に必要な各音の高さについて、`voices:`の中で
楽器のhost名に対応する関数を、音の高さ（MIDIのノート番号。`60`が中央のド）と、
その楽器の`name=value`パラメータをまとめたオブジェクトで呼び、返された
`Audio.Sound`を鳴らします。hostのドラムは1度だけ作ります。`drums:`の中で
その音の名前に対応する関数（`drums: {kick: fn (params) { ... }}`）を、ドラムの
`name=value`パラメータで呼び、返された`Audio.Sound`を鳴らします。`voices:`と
`drums:`のキーは、host名を小文字にしたものです。`host Ep`は`ep:`、`host HiHat`は
`hihat:`です。`prepare()`は
音を1つ作って残りの数を返すので、ゲームは作業を複数のフレームに分散できます。
`play()`は残りを先に作ります。

## 付録 A: コードの種類

`plays chords`が積み、度数を読むもとになる表です。`voicings`ブロックで追加
できます（[4章](#4-ファイル)）。種類はどの綴りでも書けます。数字の入った括弧は
その前の種類に足すテンションで（[テンション](#テンション)）、表にある和音なら表と
同じになります（`C7(b9)`は`C7b9`）。それ以外の括弧は、外してから表を引きます
（`Cm(maj7)`は`Cmmaj7`）。

| 種類 | 書き方 | ほかの綴り | 音程 |
|---|---|---|---|
| メジャー | `C` | | 0 4 7 12 |
| マイナー | `Cm` | `C-` `Cmi` `Cmin` | 0 3 7 12 |
| ディミニッシュ | `Cdim` | `C°` `Co` | 0 3 6 12 |
| オーギュメント | `Caug` | `C+` | 0 4 8 12 |
| サスツー | `Csus2` | | 0 2 7 12 |
| サスフォー | `Csus4` | `Csus` | 0 5 7 12 |
| シックス | `C6` | | 0 4 7 9 |
| シックスナインス | `C6/9` | `C69` | 0 4 7 9 14 |
| マイナーシックス | `Cm6` | `C-6` | 0 3 7 9 |
| アドナインス | `Cadd9` | | 0 4 7 14 |
| マイナーアドナインス | `Cmadd9` | | 0 3 7 14 |
| ドミナントセブンス | `C7` | | 0 4 7 10 |
| メジャーセブンス | `Cmaj7` | `CM7` `Cma7` `CΔ` `CΔ7` `C△` `C△7` | 0 4 7 11 |
| マイナーセブンス | `Cm7` | `C-7` `Cmi7` `Cmin7` | 0 3 7 10 |
| マイナーメジャーセブンス | `Cmmaj7` | `Cm(maj7)` `CmM7` `CmΔ7` `C-Δ7` | 0 3 7 11 |
| ハーフディミニッシュ | `Cm7b5` | `Cø` `Cø7` `C-7b5` `Cm7-5` | 0 3 6 10 |
| ディミニッシュセブンス | `Cdim7` | `C°7` `Co7` | 0 3 6 9 |
| セブンスシャープファイブ | `C7#5` | `C+7` `Caug7` | 0 4 8 10 |
| セブンスサスフォー | `C7sus4` | `C7sus` | 0 5 7 10 |
| セブンスフラットナインス | `C7b9` | `C7(b9)` | 0 4 7 10 13 |
| セブンスシャープナインス | `C7#9` | `C7(#9)` | 0 4 7 10 15 |
| セブンスシャープイレブンス | `C7#11` | `C7(#11)` | 0 4 7 10 18 |
| オルタード | `C7alt` | | 0 4 10 15 20 |
| メジャーセブンスシャープイレブンス | `Cmaj7#11` | `Cmaj7(#11)` | 0 4 7 11 18 |
| ナインス | `C9` | | 0 4 7 10 14 |
| メジャーナインス | `Cmaj9` | `CM9` `Cma9` `CΔ9` `C△9` | 0 4 7 11 14 |
| マイナーナインス | `Cm9` | `C-9` | 0 3 7 10 14 |
| マイナーイレブンス | `Cm11` | `C-11` | 0 3 7 10 14 17 |
| サーティーンス | `C13` | | 0 4 7 10 14 21 |

### テンション

種類の後ろの括弧にカンマ区切りで書いたテンションは、コード譜と同じく、その
種類に音を足します。

| 書き方 | 足す音 | 半音 |
|---|---|---|
| `9` `b9` `#9` | 9度、そのフラットとシャープ | 14、13、15 |
| `11` `#11` | 11度、そのシャープ | 17、18 |
| `13` `b13` | 13度、そのフラット | 21、20 |
| `b5` `#5` | 5度のフラットとシャープ。5度と置き換わる | 6、8 |

足すのは書いた音だけです。`C7(13)`はC7に13度を足したもので、9度は含みません
（`C7(9,13)`と`C13`は両方を含みます）。表でルートをオクターブ上に重ねている3和音
では、テンションがそのオクターブと置き換わります。`C(9)`は`Cadd9`、`Cm(9)`は
`Cmadd9`と同じです。括弧の中では`-`と`+`をフラットと
シャープの意味で書けます（`C7(-9)`、`C7(+11)`）。括弧の外の`-`は、これまでどおり
マイナーです（`C-7`）。これらのどれでもないもの（`C7(10)`、`C7(b11)`）、同じテンションの
重複、`b`も`#`も付かない`5`はエラーです。

```kauai
section Drive {
  CM7(9)            e4 g b d'
  Am7(11)           c'2. b4
  Dm7(9) G7(9,13)   a4 f e d
  C6(9) C7(b9,#11)  e2 g4 bb
}
```

ベースの行が読む度数は、そのコード自身のものです。`3`は3〜5半音、`5`は
6〜8半音、`7`は9〜11半音、`9`は13〜15半音の音程です。

## 付録 B: 文法

PEGで書いた文法です。ファイルは1行ずつ読まれ、`EOL`が行の終わりで、行の中の
要素は空白で区切ります。文法が言語より多くを許す部分（小節の拍数、名前が
定義されているかなど）は、[9章](#9-エラー)が検査の内容を示します。

```
File       <- (Use / Voicings / Band / Groove / Section / Song / EOL)*
Use        <- 'use' Quoted EOL

Voicings   <- 'voicings' '{' EOL (!('#' / 'b') Quality Int+ EOL)* '}' EOL
Band       <- 'band' Name '{' EOL (Voice / Drum / Derived / EOL)* '}' EOL
Voice      <- 'voice' Name Source VoiceWord* EOL
Source     <- 'pulse' / 'pulse2' / 'triangle' / 'saw' / 'host' Name Param*
Param      <- Word '=' Number
VoiceWord  <- 'plays' Plays / 'duty' Ratio / 'vol' Level / 'env' Int Int Int
            / 'gap' Int / 'poly'
Plays      <- 'melody' / 'roots' 'in' Range / 'chords' 'rootless'? 'from' Pitch
            / 'part' Name+
Derived    <- Name 'echoes' Name 'late' Ratio 'level' Percent EOL
            / Name 'harmonizes' Name 'under' Interval EOL
Drum       <- 'drum' Name ('noise' Int ('->' Int)? 'len' Int 'vol' Int
              ('env' Int Int Int)? / 'host' Name Param* 'vol' Percent)
              ('plays' 'hits')? EOL

Groove     <- 'groove' Name '{' EOL (Row / Edge / EOL)* '}' EOL
Edge       <- ('first' / 'last') '{' EOL (Row / EOL)* '}' EOL
Row        <- Name Cells ('|' Cells)* EOL
Cells      <- (Degree / Place / 'x' / '-' / '.' / DrumCells)+

Section    <- 'section' Name ('key' Key)? '{' EOL SectionLine* '}' EOL
SectionLine <- 'pickup' Notes EOL / 'groove' Name EOL / 'melody' 'in' Range EOL
            / 'part' Name 'in' Range EOL / PartBlock / Ending / Again / PartLine
            / Hits / Repeat / Feel / Meter / Bar / EOL
Ending     <- 'ending' Int ('as' 'written')? '{' EOL
              (Bar / Repeat / Again / PartLine / Hits / PartBlock / 'groove' Name EOL / Feel / Meter / EOL)* '}' EOL
Feel       <- ('swing' Swing / 'straight') EOL
Meter      <- 'meter' Int '/' Int EOL
Bar        <- (Chord / '/')+ Notes? EOL / Notes EOL     # '/': a beat more of the chord
PartLine   <- Name ':' Notes EOL
PartBlock  <- 'part' Name '{' EOL
              ((Notes / '.') EOL / Ordinal ':' (Notes / '.') EOL / EOL)* '}' EOL
Again      <- Ordinal ':' (Chord+ Notes? / Notes) EOL / Ordinal Name ':' (Notes / '.') EOL
            / Ordinal 'hits' ':' (HitNotes / '.') EOL
Hits       <- 'hits' ':' HitNotes EOL
Repeat     <- ('%%' / '%') EOL                    # the bar, or the two bars, before it again
HitNotes   <- (Int (':' Int)? '[' Hit+ ']' / Hit)+
Hit        <- ('.' / '!')* ('x' / 'r') Value? '~'?
Ordinal    <- Int ('st' / 'nd' / 'rd' / 'th')

Notes      <- (Direction / Tuplet / Slurred)+
Tuplet     <- Int (':' Int)? '[' (Direction / Slurred)+ ']'
Slurred    <- '('? Note ')'?
Note       <- Grace? Mark* ('~'? '<' ChordNote ChordNote+ '>' / Pitchname Octave* / 'r')
              Value? '~'?
Grace      <- '{' (Pitchname Octave*)+ '}'
ChordNote  <- Pitchname Octave* '~'?
Mark       <- '.' / '!' / '-' / '^' / '@'
Pitchname  <- [a-g] ('##' / '#' / 'bb' / 'b')?
Octave     <- "'" / ','
Value      <- ('1' / '2' / '4' / '8' / '16' / '32') '.'*
Direction  <- '\\' ('pp' / 'p' / 'mp' / 'mf' / 'f' / 'ff' / 'cresc' / 'dim'
              / 'rit' Tempo / 'accel' Tempo / 'tempo' (Tempo !'[')? / '!')
Tempo      <- (Value '=')? Number                 # quarter notes a minute, or Value's

Song       <- 'song' Name '{' EOL SongLine* '}' EOL
SongLine   <- 'tempo' Tempo / 'meter' Int '/' Int / 'key' Key / 'band' Name
            / 'groove' Name / 'melody' 'in' Range / 'part' Name 'in' Range
            / 'loop' / 'mark' Name / 'swing' Swing
            / Play / EOL
Swing      <- ('8' / '16') (Int ':' Int)?
Play       <- Name PlayOption* EOL          # each option at most once
PlayOption <- 'with' ('echo' / 'harmony') / 'ending' Int / 'in' Key / 'x' Int

Chord      <- 'N.C.' / ChordRoot Quality ('/' ChordRoot)?
ChordRoot  <- [A-G] ('#' / 'b' / '♯' / '♭')?
Root       <- [A-G] ('#' / 'b')?
Quality    <- ([a-zA-Z0-9#+°øΔ△♭♯()-] / ',' / '/' [0-9])*  # a quality of the chord table, then tensions
Pitch      <- Root Int
Range      <- Pitch '..' Pitch             # from a natural note, one octave
Key        <- Root 'm'?

Degree     <- [13579] / '8' / '>' / '<'
Place      <- Int
DrumCells  <- ('x' / '.')+

Name       <- [A-Z] [A-Za-z0-9]*
Word       <- [a-z] [a-z0-9]*
Int        <- [0-9]+
Number     <- [0-9]+ ('.' [0-9]+)?
Ratio      <- Int '/' Int
Percent    <- Int '%'
Level      <- Int / Percent
Interval   <- 'm2' / 'M2' / 'm3' / 'M3' / 'P4' / 'TT' / 'P5' / 'm6' / 'M6'
            / 'm7' / 'M7' / 'P8'
Quoted     <- "'" [^']* "'"
```

## 付録 C: 完全な曲

1つのゲームのための3曲（レースの2つのラップとエンディングのテーマ）と、
エリック・サティの曲です。2つのラップは`use`でbandを共有します。

### run.kau

2つのラップで共有するバンドです。

```kauai
band Run {
  voice Lead    pulse     plays melody            duty 1/4  vol 15  env 1 2 4  gap 2
  voice Second  pulse2                            duty 1/8  vol 9   env 1 2 6  gap 2
  voice Bass    saw       plays roots in E2..D#3            vol 18  env 0 3 3  gap 3
  voice Arp     triangle  plays chords from C4              vol 7   env 1 4 6  gap 4

  Second echoes Lead      late 3/16  level 70%
  Second harmonizes Lead  under m3

  drum Kick   noise 120       len 1  vol 20  env 0 3 2
  drum Snare  noise 800       len 3  vol 13  env 0 2 2
  drum Hat    noise 3000      len 1  vol 5   env 0 0 1
  drum Open   noise 3000      len 3  vol 6   env 0 0 3
  drum Crash  noise 5000      len 4  vol 11  env 0 0 30
  drum Tom    noise 900->500  len 3  vol 12  env 0 0 4
  drum Floor  noise 500->250  len 3  vol 13  env 0 0 5
}
```

### cruise.kau

1周目の曲です。

```kauai
# Lap one of a racing game: ~129 bpm in C.

use 'run.kau'

song Cruise {
  tempo 129
  key C
  band Run
  melody in F5..E6
  loop

  Intro    with harmony
  Verse
  Climb    with echo
  Hook     with echo ending 1
  Hook     with harmony ending 2
}

groove Intro {
  Bass   1 - . 1  8 - 1 -  1 - . 1  8 - > -
  Arp    1 2  3 4  3 4  2 3
  Kick   x... .... x..x ....
  Hat    ..x. ..x. ..x. ..x.
  Snare  .... x... .... x...
  last {
    Kick   x... .... x... ....
    Hat    ..x. ..x. ..x. ....
    Snare  .... x... ...x x...
    Tom    .... .... .... .x..
    Floor  .... .... .... ..xx
  }
}

groove Verse {
  Bass   1 - . 1  8 - 1 -  . 8 - 1  5 - > -
  Arp    1 2  3 4  3 4  2 3
  Kick   x... ...x x... ....
  Hat    ..x. ..x. ..x. ....
  Snare  .... x... .... x...
  Open   .... .... .... ..x.
}

groove Climb {
  Bass   1 - 1 -  1 - 1 -  1 - 1 -  1 - > -
  Arp    1 2  3 4  1 2  3 4
  Kick   x... .... x... ....
  Hat    ..xx ..xx ..xx ..xx
  Snare  .... x... .... x...
  last {
    Kick   x... .... .... ....
    Hat    .... .... .... ....
    Snare  ..x. x.xx .... ....
    Tom    .... .... xxxx ....
    Floor  .... .... .... xxxx
  }
}

groove Hook {
  Bass   1 - 8 -  1 8 - 1  - 8 1 -  5 - > -
  Arp    4 3  2 1  2 3  4 3
  Kick   x... ...x x... ....
  Hat    ..x. ..x. ..x. ..x.
  Snare  .... x... .... x...
  Open   .... .... .... ...x
  first {
    Kick   .... ...x x... ....
    Crash  x... .... .... ....
  }
  last {
    Hat    ..x. ..x. ..x. ....
    Snare  .... x... ...x xx..
    Open   .... .... .... ....
    Floor  .... .... .... ..xx
  }
}

section Intro {
  Fmaj7       r8 a16 c e8. d c8 a4
  G6          r8 b16 d g'8. e d8 b d
  Em7         e2 d8 b g a
  Am7         b8. c4~ c16 e8 d c b
}

section Verse {
  Cmaj7       r4. e,16 g b8. a g8~
  Bm7b5 E7    g f8 e, d,4 g#8 b
  Am7         c4. b8 a4 e,8 g~
  Gm7 C7      g8 f bb8. a g8 e,4
  Fmaj7       f r8 a16 c e8. d c8~
  Fm6         c r8 ab g f d, f
  Em7 A7      e, g8 b c#8. e d8~
  Dm7 G7      d c8 a b4 g8 f
}

section Climb {
  Fmaj7       e,8 f a c4 e8 c a
  Em7         b g8 b4 d8 b g
  Dm7         a8 c d f'4 e8 d c
  G7sus4 G7   d4. c8 b4 r16 g a b
}

section Hook {
  Fmaj7       c8. a c8 e4 d8 c
  G7          d4. b4 g8 a b
  Em7         b8. g b8 d4 e8 d
  Am7         c2 r8 e d c
  Dm7         a8. f a8 c4 e8 f'
  G7          g' f'8 d b4 d8 f'
  ending 1 {
    Cmaj7       e2 d8 b g a~
    Gm7 C7      a r8 bb a g e, g
  }
  ending 2 {
    Cmaj7       e2 g'8 e d c~
    Gm7 C7      c r8 d c bb g e,
  }
}
```

### sprint.kau

2周目の曲です。2回目のサビは1回目を全音上げたもの（`in E`）で、最後の2小節は書いたとおりの音でDに戻ります。

```kauai
# Lap two of the racing game: 150 bpm in D, after the Japanese fusion bands of
# the 80s.

use 'run.kau'

song Sprint {
  tempo 150
  key D
  band Run
  melody in A5..G#6
  loop

  Riff     with harmony
  Verse
  Lift     with echo
  Chorus   with echo ending 1
  Chorus   with harmony ending 2 in E
}

groove Riff {
  Bass   1 - . 1  - . 1 -  . 1 - .  1 - 8 -
  Arp    1 2  3 4  4 3  2 1
  Kick   x... ...x x... ....
  Hat    ..xx ..x. ..xx ..x.
  Snare  .... x... .... x...
  Open   .... .... .... ...x
}

groove Verse {
  Bass   1 8 1 8  1 8 1 8  1 8 1 8  5 8 > 8
  Arp    1 3  2 4  1 3  2 4
  Kick   x... ...x ..x. ....
  Hat    .xxx .xx. xx.x .xxx
  Snare  .... x... .... x...
}

groove Lift {
  Bass   1 - 1 -  1 - 1 -  1 - 1 -  1 - > -
  Arp    1 2  3 4  1 2  3 4
  Kick   x... .... x... ....
  Hat    .xxx .xxx .xxx .xxx
  Snare  .... x... .... x...
  last {
    Kick   x... .... .... ....
    Hat    .xxx .xxx .... ....
    Snare  .... x... x.xx ....
    Tom    .... .... .... xx..
    Floor  .... .... .... ..xx
  }
}

groove Chorus {
  Bass   1 . 8 1  . 8 1 .  1 8 . 1  8 . > 8
  Arp    4 3  2 1  4 3  2 1
  Kick   x... ...x x... ....
  Hat    .x.x .x.. .x.x .x.x
  Open   ..x. ..x. ..x. ..x.
  Snare  .... x... .... x...
  first {
    Kick   .... ...x x... ....
    Hat    .xxx .xx. .xxx .x.x
    Open   .... .... .... ..x.
    Crash  x... .... .... ....
  }
  last {
    Hat    .x.x .x.. .x.. ....
    Open   ..x. ..x. .... ....
    Snare  .... x... ..xx ....
    Tom    .... .... .... xx..
    Floor  .... .... .... ..xx
  }
}

section Riff {
  Dmaj7       d8 r16 a8 r16 d8 e f# r16 a'8.
  C/D         g r8 e r16 d8 c8. a8
  Bbmaj7      bb8 r16 f,8 r16 bb8 c d r16 f8.
  A7sus4 A7   e8. d a8 c# e d16 c# b a
}

section Verse {
  Em9         f#,2 r8 g,16 a b8 d~
  F#m7        d8 e4 c# b8 a4~
  Gmaj7       a r8 f#,16 a c#8. b a8~
  A7sus4 A7   a8 e, g, a d8. c# r8
  Bm9         r8 f#,16 a c#4 b8 a4 f#,8~
  Gmaj7       f#, e,8 f#, a8. b d8
  C9          e d8 bb4 g,8 e, g,
  F#m7 B7     a8. c# e8 d#4 a8 f#,
}

section Lift {
  G/A         e,8 a b d4 e8 d b
  A/B         f#,8 b c# e4 f#8 e c#
  Bb/C        g,8 c d f4 g8 f d
  C/D D7      e4. d8 c a f#,16 g, a8
}

section Chorus {
  Gmaj7       f#2 e8 f# d f#~
  F#m7        f#8 e c#4 b8 c# e4
  Fmaj7       e2 d8 e c e~
  Em7 A7      e8 d b4 c#8 e g f#~
  Bm7         f#4. e8 d8. c# b8~
  E9          b g#,8 b d8. e f#8~
  ending 1 {
    Gmaj7/A     f#4. e8 d8. b a8~
    Cmaj7 D/E   a g,8 b d8. e f#8
  }
  ending 2 as written {
    Amaj7/B     g# a'8 g# f# e c# a~
    Bbmaj7 C    a d8 f a' g e c
  }
}
```

### ballad.kau

エンディングのテーマです。エレクトリックピアノはhost楽器（`Ep`）で和音を弾き、メロディも同じ楽器で、より明るく弾きます。

```kauai
# The racing game's ending theme: a slow ballad in Eb, after the late-night
# coast-road ballads of the 16-bit arcades.

band Trio {
  voice Piano  host Ep bright=0.8  plays chords rootless from E3  poly  vol 26%
  voice Sing   host Ep bright=1.3  plays melody                    poly  vol 46%
  voice Bass   triangle            plays roots in E2..D#3          vol 34  env 3 0 14  gap 3

  drum Brush  noise 4000  len 2  vol 4  env 0 0 5
  drum Ride   noise 6000  len 1  vol 3  env 0 0 10
}

song Theme {
  tempo 69
  key Eb
  band Trio
  melody in D5..C#6

  Intro
  mark Drive
  Verse
  Peak
}

groove Alone {
  Piano  x - - -  - - - -  - - 4 -  - - - - | x - - -  - - - -  x - - -  - - - -
  Bass   1 - - -  - - - -  - - - -  5 - - - | 1 - - -  - - - -  1 - - -  - - > -
  Ride   .... x... .... x...
}

groove Verse {
  Piano  x - - -  - - - -  - - x -  - - 4 -
  Bass   1 - - -  - - - -  - - 5 -  - - > -
  Brush  .... x... .... x...
}

groove Peak {
  Piano  x - - 4  - - 3 -  x - - 4  - - 3 -
  Bass   1 - - -  - - 5 -  1 - - -  8 - > -
  Brush  .... x... .... x...
  Ride   .... .... x... ..x.
}

groove Settle {
  Piano  x - - -  - - 4 -  - - 3 -  - - 2 -
  Bass   1 - - -  - - - -  - - - -  - - - -
}

groove Ring {
  Piano  x - - -  - - - -  - - - -  - - - -
  Bass   1 - - -  - - - -  - - - -  - - - -
}

section Intro {
  groove Alone
  Abmaj9      r4. bb16 g bb2~
  Gm7 C7      bb g8 f e2
  Fm9         f4. bb16 g eb2~
  Bb7sus4 Bb7 eb r8 eb16 f d4 f8 g
}

section Verse {
  Ebmaj9      bb2~ bb8 g4 f8
  Gm7 Cm9     g2 eb4 d8 eb
  Abmaj9      c2 bb4 g
  Fm9 Bb7sus4 ab4. g8 f4 eb
  Ebmaj9      d4. eb8 f4 g8 bb
  Dm7b5 G7    c4. ab8 b4. g8
  Cm9         g2~ g8 eb d c,
  Abm6 Db9    eb2 f4 eb
}

section Peak {
  Abmaj9      eb'4. c bb4
  Bb/Ab       d'2 c4 bb
  Gm7         bb4. d' c8 bb
  Cm9         g2~ g8 bb g eb
  Fm9         f2 ab4 g
  Bb7sus4 Bb7 eb4. f8 d4. r8
  groove Settle
  Abmaj9      eb2 g4 bb
  groove Ring
  Ebmaj9      g1
}
```

### jetveux.kau

*ジュ・トゥ・ヴー*（エリック・サティ、1897年）。歌のパートはOpenScore Liederの楽譜（CC0）のとおりで、スラー、強弱、クレッシェンドとディミヌエンドも楽譜のとおりです。前奏のピアノの右手はパート`Fill`で、和音とタイも楽譜のとおりです。コード記号はピアノのパートから読み取ったものです。`\rit 104`とそれに応える`\tempo`は、楽譜の*retenir*と*au refrain*を表したものです。

```kauai
# Je te veux (Erik Satie, 1897): the voice as written, from the OpenScore
# Lieder score (CC0); the chords are a reading of the piano part.

voicings {
  7b5     0 4 6 10
  9sus4   0 5 7 10 14
}

band Salon {
  voice Singer  pulse               plays melody                           duty 1/2  vol 22  env 2 2 8
  voice Bass    triangle            plays roots in E1..D#2                           vol 32  env 0 2 6
  voice Piano   host Ep bright=0.9  plays chords from E3  plays part Fill  poly      vol 30%
}

song JeTeVeux {
  tempo 124
  meter 3/4
  key C
  band Salon
  groove Valse
  melody in D4..C#5
  part Fill in A3..G#4

  Intro
  Refrain  ending 1
  Couplet
  Refrain  ending 2
  Couplet
  Refrain  ending 3
  Coda
}

groove Valse {
  Bass   1 - -
  Piano  . x x
}

section Intro {
  N.C.          r2.
    Fill:       r4 e g
  G9            r2.
    Fill:       <a d>2.
  C6/G          r2.
    Fill:       <a c e a'>2 <e, e>4
  G7            r2.
    Fill:       <g,~ b~ e g~>2.
                r r2
    Fill:       <g, b d g>4 e g
}

section Refrain {
  pickup  \pp (e g
  Cadd9         \tempo d'2.
  C/G           c2 e'4
  Cmaj7         b2.
  C6/G          a2.)
  Cmaj7         (b2.
  C6/G          a2 e4
  G7/D          b2.~
  G6            b) (d e
  G7/D          g2.~
  G6            g) (d e
  Dm6           a2.~
  G6            a) (d e
  G7/D          g2 f4~
  G7            f g2
  C             e2.
  Em7/B         d) (e g
  A7sus4        d'2.
  C/G           c2 e'4
  Cmaj7         b2.
  C6/G          a2) r4
  Cmaj7         (b2.
  C6/G          a2 g4
  Dm7           a2.
  Dm/F          d) (f g
  G7 G7 C/E     \cresc a b c \!
  Dm/F Em/G Dm7 d' e' f'
  C/E C/G Am    g' e' c
  F#m7b5        a c e'
  C/G C/G F/A   \dim g2 f4) \!
  G7/F          (e d2
  ending 1 {
    C             c,2.~
    D7 D7 C/E     c,) (b c)
  }
  ending 2 {
    C             c,2.~
    D7 D7 C/E     c,) (b c)
  }
  ending 3 {
    C             c,2.~
                  c,) r r
  }
}

section Couplet key G {
  melody in E4..D#5
  Am6           \p (e2.~
  D7            e) (e f#
  G             d,2.~
  D7            d,) \cresc (f# g \!
  D7/A          d2 d4~
  D7            d d2
  G6            \dim e'2. \!
  D7            d) (b c
  Am6           e2.~
  D7            e) (e f#
  G             d,2.~
  G/D           d,) (b g
  F#7           \cresc f#2 f#4~ \!
  F#7/C#        f# f#2
  Bm/D          \dim b2. \!
  Cmaj7         d) (b c
  Am6           e2.~
  D7            e) (e f#
  G             d,2.~
  D7            d,) \cresc (f# g \!
  D7/A          d2 d4~
  D7            d d2
  Gmaj7         \dim f#'2. \!
  D7            e') (d b
  Am6           e2.~
  D7            e) (e f#
  G             d,2.~
  D7            d,) (g f#
  Am7           e \cresc g c
  Ab7b5         \rit 104 d2 \! d4
  G G9sus4 G6   \dim g'2. \!
  G7            g) r2
}

section Coda {
  Em            \tempo 114 r2.
  Dm7/F         r2.
  G7/D          r2.
  C             r2.
                r2.
}
```

## 付録 D: 持ち込めない書き癖

ほかの楽譜の書き方で書き慣れたことのうち、Kauaiでは書き方や読み方が違うものです。
各行は書き癖と、Kauaiでの書き方です。多くは、そのまま書くとエラーにならずに
違う音が鳴ります。

| 書き癖 | Kauaiでは |
|---|---|
| 調号で音にシャープやフラットが付く | 調は臨時記号を付けない。`key D`でもFのシャープは`f#` |
| `c'`は決まった1つのオクターブ | `'`と`,`は、音を範囲（`melody in LO..HI`）から1オクターブ動かす |
| 音符の行の`C4`は中央のド | 音符は小文字で、後ろの数字は長さ。`c4`は4分音符。`C4`が音の高さになるのは範囲と`from`の後だけ |
| `bb`はBが2つにも読める | `bb`はBのフラット。Bの重変は`bbb` |
| 長さが次の小節に引き継がれる | 小節の最初の音で音価を書かなければ1拍 |
| 印を音の後ろに書く（`c4-.`） | 印は音の前に書く。`.c4` |
| 強弱を`p`、`\<`、`/p`と書く | 指示は`\`で始まる。`\p`、`\cresc`、`cresc`を終えるのは`\!` |
| 9度を足したコードを`C9`と書く | `C9`は短7度を含む。`Cadd9`と`C(9)`は含まない |
| `Cdim`でディミニッシュ・セブンス | `Cdim`は3和音。4和音は`Cdim7` |
| 6/8拍子で付点4分音符の`tempo 60` | `tempo`は4分音符で数える。付点4分音符なら`tempo 4.=60` |
| 16ステップのつもりで15や17個のドラムの行 | 小節のステップは音価の長さになる必要があり、数え間違いはエラー |
