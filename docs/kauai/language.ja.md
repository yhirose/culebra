# Kauai 言語仕様

> **Status: Draft.** KauaiはCulebraの`Audio.Kauai`として実装済みです。
> ただし、リリースまでに言語やAPIが変わる可能性があります。[付録C](#付録-c-完全な曲)の
> 曲は、元のゲームや楽譜と1音ずつ照合してあります。

Kauaiは、バンドの演奏をそのまま書き表すための言語です。コードとメロディは
リードシートとして書き、バンドがそれにどう伴奏を付けるかはgrooveとして
書きます。Kauaiのファイルは拡張子`.kau`のプレーンテキストで、Culebraの
プログラムから`Audio.Kauai`で再生できます（[10章](#10-culebra-から再生する)）。

名前はハワイのカウアイ島（Kauaʻi）から取りました。Culebraという名前が
プエルトリコの島から来ているのにならっています。コードやファイル名では
`kauai`、`.kau`と書きます。

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

曲は4種類の定義で組み立てます。

| 定義 | 書く内容 |
|---|---|
| `band` | 楽器（voice）とドラムキット |
| `groove` | 1小節以上の伴奏。音符ではなく、コードに対する位置で書く |
| `section` | リードシート。1小節を1行に、コードとメロディを書く。書き譜のパートも置ける |
| `song` | テンポ、拍子、調と、sectionを演奏する順番 |

設計の原則は次のとおりです。

1. **1行が1小節。** sectionの各行は1小節で、コード、音符の順に書きます。
   例外は`%%`の行だけで、コード譜の2小節反復記号と同じく2小節ぶんになります。
2. **ミュージシャンの書き方で書く。** コードはコード記号で、音の長さは音価で
   書きます（`c4.`は付点4分音符）。楽譜で音符に付ける記号は音符に付け
   （`.c8`はスタッカート）、強弱やテンポの指示は変わる場所に書きます
   （`\cresc`、`\rit 104`）。
3. **同じことを二度書かない。** grooveは、それを使うすべての小節で共有します。
   コードは次のコードが来るまで続き、調やendingを変えた繰り返しもsongに
   1行書くだけで済みます。
4. **直したところだけが変わる。** 1つの音を直しても、変わるのはその音だけです。
   オクターブは、直前の音ではなく、あらかじめ指定した範囲をもとに決まります。
   長さの省略が引き継がれるのも、同じ小節の中だけです。
5. **誤りは書いた場所で知らせる。** [9章](#9-エラー)のエラーはすべて演奏前に
   見つかり、どの行かが示されます。
6. **展開は決定的。** 同じ曲は毎回同じ音に展開されます。乱数で何かを選ぶ
   ことはありません。

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

sectionの各行が1小節で、コード、音符の順に書きます。2行目はコードを
書いていないので、Cのままです。`F G`は、1小節を2つのコードで分けます。
`c'`と`d'`は、範囲`C5..B5`から1オクターブ上の音です。grooveはすべての小節で
伴奏として鳴り、ベースは各コードのルート（`1`）を弾いて伸ばし（`-`）、
続けて5度（`5`）を弾きます。

## 3. 名前と語

**名前とキーワード。** 大文字で始まる語は、曲の中で定義した名前です
（`Duo`、`Tune`、`Verse`、`Walk`）。小文字の語は言語のキーワードです
（`band`、`voice`、`plays`、`groove`）。音楽家がふだん大文字で書くものは、
Kauaiでも大文字で書きます。コード記号（`Fmaj7`）、音の高さと調（`C4`、`Eb`）、
音程（`M3`、`P5`）がそうです。これらは、それぞれを書くべき位置にしか
現れないので、名前と取り違えることはありません。

**行とブロック。** Kauaiは1行ずつ読み取ります。`{`で終わる行でブロックが
始まり、`}`だけの行で終わります。語は空白で区切り、インデントには意味が
ありません。

**コメント。** 行頭、または空白の直後にある`#`から行末までがコメントです。
語の途中の`#`はシャープなので、`g#`、`F#m7`、`D#3`はコメントになりません。

**範囲などで使う音の高さ。** 定義の中で音の高さを指定するとき（`E2..D#3`、
`from C4`）は、大文字の音名に、必要なら`#`か`b`を付け、オクターブ番号を
続けます。`C4`が中央のド（MIDI 60）です。音符の行では、同じ文字を小文字で
書くと音符になり、後ろの数字は長さを表します。つまり`e4`は4分音符のEで、
`E4`は音の高さです。

**コード記号。** ルート（大文字の音名に、必要なら`#`か`b`）、コード表
（[付録A](#付録-a-コードの種類)）にある種類、必要なら`/`とベース音、の順に
書きます。例: `Fmaj7`、`Bm7b5`、`C/D`、`Gmaj7/A`。種類はコード譜でよく使う
表記でも書けます（`CM7`、`C△7`、`C-7`、`Cø`、`C°7`、`C+`）。テンションは、
コード譜と同じく後ろの括弧にカンマ区切りで書きます（`C7(9,13)`、`Cm7(11)`、
`C7(b9,#11)`、[テンション](#テンション)）。`b`と`#`の代わりに`♭`と`♯`も
使えます。大文字の前の`/`はベース音を、数字の前の`/`は種類の一部を表します
（`C6/9`）。音名の直後の`#`や`b`はルートの一部になるので、`Cb9`は
Cフラットのナインスです。`N.C.`はコードなし（ノーコード）を表します。

## 4. ファイル

演奏するファイルには、`song`をちょうど1つ書きます。`song`のないファイルは
ライブラリで、中のband、groove、sectionをほかのファイルから使うためのもの
です。

**`use 'file'`** は、別のファイルの定義を、その位置に書いたのと同じように
取り込みます。パスは引用符で囲み、`use`を書いたファイルからの相対パスで
指定します。取り込むファイルには`song`を書けません。

**`voicings { ... }`** は、[付録A](#付録-a-コードの種類)の表にコードの種類を
追加します。1行に1つずつ、種類の名前と、ルートからの音程（半音数）を並べます。
名前を`#`や`b`で始めることはできません。ルートのシャープやフラットと区別が
つかなくなるからです。表にすでにある種類や表記と同じ名前も使えません。

```kauai
voicings {
  7b5     0 4 6 10
  9sus4   0 5 7 10 14
}
```

同じ種類の定義に同じ名前を付けるとエラーです（grooveが2つ、sectionが2つ
など）。同じファイルの中でも、ファイルをまたいでも同じです。grooveとsectionの
ように、種類が違えば同じ名前でも構いません。

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

音源には、[`Audio.tone`](../stdlib.ja.md#tone)のチャンネル（`pulse`、
`pulse2`、`triangle`、`saw`）のどれかか、プログラム側で用意する楽器を表す
`host NAME`を指定します（[10章](#10-culebra-から再生する)）。チャンネルは
一度に1音しか鳴らせないので、1つのbandの中で2つの楽器が同じチャンネルを
使うことはできません。ドラムは`noise`チャンネルを使います。

楽器が**何を弾くか**によって、その楽器の行の読み方が変わります。

| 書き方 | その楽器が弾くもの |
|---|---|
| `plays melody` | sectionのメロディ。bandの中でちょうど1つの楽器に指定する |
| `plays roots in LO..HI` | コードの度数で書いたベースライン（[ベースの行](#ベースの行)）。`LO..HI`は、各コードのベース音を置く1オクターブの範囲 |
| `plays chords from NOTE` | コードの構成音を、下から何番目かで指定したもの（[和音の行](#和音の行)）。和音は`NOTE`から上に積む。`plays chords rootless from NOTE`はルートを省く |
| `plays part NAME...` | sectionに書いたパート。名前は複数書ける（`plays part Upper Lower`、[パート](#パート)）。パートのある小節では、grooveの行の代わりにパートを弾く。ほかの`plays`と併用できる |
| （`plays`なし） | echoやharmonyで作るパートだけを弾く（[派生パート](#派生パート)） |

和音は、コードの種類が持つ音程に従って積み上げます。最初の音は、`NOTE`
以上でその音名を持つ最も低い音です。2つ目からは、直前の音より高い中で、
その音名を持つ最も低い音を選びます。たとえば`plays chords from C4`なら
`Fmaj7`はF4 A4 C5 E5に、`plays chords rootless from E3`なら`Abmaj9`は
C4 Eb4 G4 Bb4になります。

| オプション | 意味 | 既定値 |
|---|---|---|
| `duty 1/8` `1/4` `1/2` `3/4` | パルス波のデューティ比 | `1/2` |
| `vol N` | `mf`のときの音量。`Audio.tone`と同じ`0`〜`100`で指定する。host楽器では、楽器自身の音量に対する百分率（`vol 46%`） | （必須） |
| `env A D R` | アタック、ディケイ、リリース（単位は1/60秒のtick） | `0 0 0` |
| `gap N` | 各音の終わりを何tick切り詰めるか。音と音の間にすき間を作って、区切って聞かせる | `0` |
| `poly` | 複数の音を同時に鳴らせる（host楽器のみ） | 一度に1音 |

host楽器は、音ごとに用意された音声を最後まで鳴らします。`env`、`gap`、
音符の長さは効きません。名前の後ろに`name=value`の形で独自のパラメータを
付けられます（`host Ep bright=0.8`）。このパラメータは、音ごとにKauaiから
ホストへ渡されます。

### 派生パート

bandの中の、楽器名で始まり`echoes`か`harmonizes`が続く行は、別の楽器の音を
もとにしたパートをその楽器に割り当てます。どちらを鳴らすかは、songの中で
sectionごとに選びます（[8章](#8-曲)）。

| 行 | 楽器`V`が弾くパート |
|---|---|
| `V echoes SOURCE late 3/16 level 70%` | SOURCEの音を、指定した長さ（ここでは16分音符3つぶん）だけ遅らせ、Vの`vol`に対する百分率の音量で弾く |
| `V harmonizes SOURCE under m3` | SOURCEの各音に対し、そのとき鳴っているコードの構成音（オクターブは問わない）のうち、指定した音程以上低い中で最も高い音を、同じ長さで弾く |

音程は`m2 M2 m3 M3 P4 TT P5 m6 M6 m7 M7 P8`で書きます。echoはsectionの境界を
またいで元の音を追います。ループする曲では、曲の頭のechoが曲の終わりの音を
鳴らします。

### ドラム

`drum NAME noise FREQ[->END] len N vol V [env A D R] [plays hits]`
`drum NAME host SOUND [name=value...] vol P% [plays hits]`

noiseのドラムは、ノイズチャンネルで鳴らす打音です。`FREQ`は高さ（Hz）で、
`END`を書くとそこまで音程を滑らせ、`len`のtick数だけ鳴らします。チャンネルは
一度に1つのドラムしか鳴らせないので、noiseのドラム2つを同時に鳴らすことは
できません。

hostのドラムは、プログラム側で用意した音声（[10章](#10-culebra-から再生する)）を
最後まで鳴らします。音量は、その音声自身の音量に対する百分率で指定します。
hostのドラムは、ドラムセットの太鼓やシンバルと同じように、ほかのhostのドラムとも
noiseのドラムとも同時に鳴らせます。

```kauai
band Combo {
  voice Keys  host Ep bright=0.8  plays chords rootless from E3  poly  vol 40%
  voice Bass  triangle            plays roots in E2..D#3            vol 30

  drum Kick   host Kick   vol 80%
  drum Ride   host Ride   vol 50%
  drum Hat    host Hat    vol 40%
}
```

`plays hits`を付けたドラムは、sectionのキメに加わって鳴ります。付けていない
ドラムは、キメの間は休みます（[キメとブレイク](#キメとブレイク)）。

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

grooveには、伴奏する楽器やドラムごとに1行ずつ書き、行頭にその名前を置きます。
各行はグリッドになっていて、1小節に`n`個のトークンを並べると、1つのトークンが
小節の`n`分の1の長さになります（4/4で16個なら16分音符、8個なら8分音符）。
このため、grooveはどの拍子の小節にも当てはめられます。ただし、1トークンの長さは
通常の音価、付点音符、3連符のどれかにならなければいけません（4/4で12個なら
8分音符の3連符になり、15個はエラーです）。`|`で次の小節に進み、2小節の行は
2小節ごとに繰り返します。grooveの小節は、sectionの最初の小節か、そのgrooveを
指定した`groove`行から数え始め、各行はそれぞれ自分の小節を順に繰り返します。
そのため、1小節の行を2小節の行と並べても、1小節の行は毎小節鳴ります。どの行でも、
`-`は直前の音を伸ばし、`.`は無音を表します。grooveに行がない楽器は、そのgrooveの
間は鳴りません。

### ベースの行

`plays roots`の楽器の行には、コードの度数を書きます。

| トークン | 鳴る音 |
|---|---|
| `1` | コードのベース音（`/`の後の音、なければルート）。`LO..HI`の範囲に置く |
| `3` `5` `7` `9` | そのコードでのその度数の音（`Bm7b5`の`5`はF）。`1`以上で最も低い高さに置く |
| `8` | `1`の1オクターブ上 |
| `>` / `<` | 次のコードの`1`の半音下 / 半音上。`>`は下から、`<`は上から次のコードへつなぐ |

各トークンは、そのタイミングで鳴っているコードに従います。`C F`の小節なら、
前半のトークンはC、後半のトークンはFの音になります。「次のコード」とは、
次に演奏される小節の最初のコードです。ループしない曲の最後の小節には次の
コードがないので、`>`と`<`はそのコード自身の`1`を弾きます。その度数を持たない
コードでは、既定値の4、7、10、14半音を使います。

### 和音の行

`plays chords`の楽器の行には、ボイシングの中の位置を下から数えて書きます。
`1 2 3 …`で1音ずつ（最上音より大きい数は最上音）、`x`で和音全体を同時に
鳴らします（`x`は`poly`の楽器で使います）。

### ドラムの行

ドラムの行は、叩くところを`x`、叩かないところを`.`として、1ステップを1文字で
書きます。文字の間の空白は無視します。

### 最初と最後の小節

`first { ... }`と`last { ... }`には、grooveを使う各sectionの最初と最後の小節で
差し替える行を書きます。差し替えるのは同じ名前の行です（`xN`で繰り返す場合は、
その各回が対象です）。sectionをクラッシュで始めたり、フィルで締めくくったり
するのに使います。

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

`section NAME [key KEY] { ... }`の中には、小節の行と、次の表の行を書きます。

| 行 | 意味 |
|---|---|
| `pickup NOTES` | sectionへ入る弱起の音。直前に演奏される小節の最後の拍に重ねて鳴らすので、その小節のその部分は休符でなければならない（[弱起](#弱起)） |
| `groove NAME` | これ以降の小節のgroove |
| `meter N/D` | これ以降の小節の拍子（[拍子の変化](#拍子の変化)） |
| `melody in LO..HI` | このsectionの音の範囲（[オクターブ](#オクターブ)） |
| `NAME: NOTES` | 小節の下に書く。その小節でパート`NAME`が弾く音（[パート](#パート)） |
| `%` / `%%` | 直前の1小節、または直前の2小節を繰り返す（[反復記号](#反復記号)） |
| `hits: x8 r ...` | 小節の下に書く。その小節でバンドがgrooveの代わりに弾くリズム（[キメとブレイク](#キメとブレイク)） |
| `2nd: NOTES` | 小節の下に書く。2回目に演奏するときのその小節（[2回目以降](#2回目以降)） |
| `part NAME { ... }` | パート`NAME`の書き譜。1行が1小節（[パート](#パート)） |
| `part NAME in LO..HI` | パート`NAME`の音の範囲 |
| `swing 8` / `straight` | これ以降の小節のスウィング（[スウィング](#スウィング)）。`straight`は書いたとおりのリズムで弾く |
| `ending N { ... }` | sectionを締めくくる小節。演奏する回ごとに別のものを使える。`ending N as written { ... }`はsongの`in`で移調しない |

`key KEY`は、そのsectionが何調で書かれているかを示します。songの`in`は、
この調を基準に移調します。省略するとsongの調になります。`in`を付けずに
演奏するsectionは、songの調に関係なく書いたとおりの音で鳴ります。

### 小節の行

小節の行には、その小節のコード、音符の順に書きます。行頭にある大文字で
始まる語と`N.C.`がコードです。コードは小節を等分するので、コードの数は
小節の拍数を割り切れる数でなければなりません。コードの間に置く`/`は
コード譜のビート・スラッシュと同じで、直前のコードをもう1拍続けます。
4/4の`C / / G7`なら、Cが3拍、G7が1拍です。スラッシュを使う小節では、1拍ごとに
コードか`/`を1つずつ書きます。`/`で始まる小節は、鳴っているコードをそのまま
続けます。コードを書かない小節は、直前の小節のコードを引き継ぎます。ただし、
sectionとendingの最初の小節には必ずコードを書きます。`N.C.`の間は、ルートや
和音を弾く楽器が休み、ドラムだけが演奏を続けます。

音符がなくコードだけの小節では、メロディは休みます。コード譜で、スラッシュ
だけが並ぶ小節と同じ扱いです。

```kauai
section Vamp {
  Dm7 / G7 /
  Dm7 / G7 /
  Cmaj7         e4 d c2
}
```

### 反復記号

`%`だけの行は、コード譜の反復記号と同じく、直前の小節をもう一度弾きます。
繰り返すのは、コードと音符だけでなく、その下に書いた行（パート、キメ、
`2nd:`の行）も含みます。`%%`は直前の2小節をもう一度弾くので、2小節ぶんに
なります。grooveや拍子、スウィングは、繰り返した先の位置のものが使われます。
内容が少しでも違う小節は書き出す決まりなので、`%`の下には何も書けません。

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
| `c` `d` `e` `f` `g` `a` `b` | 音符。必ず小文字で書く |
| 後ろに`#` / `b` | シャープ / フラット（`f#`、`eb`。`bb`はBのフラット。`##` / `bb`でダブルシャープ / ダブルフラット: `f##`、`ebb`。Bのダブルフラットは`bbb`） |
| さらに後ろに`'` / `,` | 範囲から1オクターブ上 / 下（`e'`、`c,`） |
| `1 2 4 8 16 32` | 全音符、2分、4分、8分、16分、32分音符 |
| 音価の後ろの`.` | 付点（`..`で複付点）: `g4.` |
| （音価なし） | 同じ小節の直前の音と同じ長さ（付点も含む）。小節の最初の音なら1拍 |
| 末尾の`~` | 次の音へのタイ。次の音は同じ高さでなければならない |
| `r` | 休符。音符と同じく音価を付ける（`r4`、`r2.`） |
| `3[c8 d e]` | 連符。数字は中に入る音の数 |
| `2:3[c8 d]` | 比を指定した連符。3つぶんの時間に2つ入れる |
| `<c e g>4` | 和音。中の音を、1つの音符として同時に鳴らす |
| `{d}c4` | 装飾音。つなぐ先の音の前に書く |

音の高さは、書いた音名のとおりです。調がシャープやフラットを自動で付けることは
ないので、`key D`でもFシャープは`f#`と書き、`f`はFナチュラルになります。
臨時記号はその音にだけ効き、同じ小節の後の音には及びません。`f# g f`は
Fシャープ、G、Fナチュラルです。

連符は、中に書いた音の合計を超えない範囲で、最も長い通常の音価（全音符、
2分、4分…）の長さに収めます。`3[c8 d e]`は4分音符1つぶんに8分音符3つ、
`3[c4 d e]`は2分音符1つぶんに4分音符3つ、`5[c16 d e f g]`は4分音符1つぶんに
16分音符5つです。数字は`[`の直前に書くので、`\tempo 3[c8 d e]`は「曲のテンポに
戻してから連符を弾く」と読みます。

この規則に当てはまらない連符は、楽譜の表記と同じように比`p:q`を付けて書きます。
中の音は書いた長さの`q/p`倍で鳴り、`q`個ぶんの時間に`p`個が入ります。6/8拍子で
`2:3[c8 d]`と書けば8分音符3つぶんの時間に2つ、`4:6[c8 d e f]`なら6つぶんの
時間に4つです。比は音の個数ではなく音価で数えるので、長さの違う音が混ざった
連符にも使えます。`3:2[c4 d8]`は、4分音符1つぶんの時間に4分音符と8分音符を
1つずつ入れます。`3[c8 d e]`は`3:2[c8 d e]`の省略形です。

和音は、複数の高さを持つ1つの音符として扱います。長さは`>`の後ろに、奏法の
記号は`<`の前に書き（`.<c e g>8`）、臨時記号とオクターブの記号は中の音ごとに
付けます（`<c e' g>`）。`>`の後ろの`~`は和音全体をタイでつなぎ、中の音の後ろの
`~`はその音だけをつなぎます。これを使うと、和音の一部だけを伸ばしたまま、
残りの音を変えられます。`<g,~ b~ e g~>2.`の次に`<g, b d g>4`と書くと、G、B、
Gは伸びたままで、Dだけが新しく鳴ります。同時に鳴る音を弾くには`poly`の楽器が
必要です。メロディに和音が含まれる場合、派生パートはその一番上の音をもとに
作ります。

装飾音は、つなぐ先の音の前に波かっこで書きます（`{d}c4`、`{e d}c4`）。装飾音
1つにつき64分音符1つぶんの時間を後ろの音の頭から借りるので、後ろの音はその
ぶん短く鳴ります。装飾音には臨時記号とオクターブの記号を付けられますが、長さや
奏法の記号は付けられません。装飾音の後ろの音は和音でも構いませんが、休符や、
タイで伸ばしている音に装飾音は付けられません。

### オクターブ

`'`も`,`も付かない音は、範囲の中で、その音名の位置に置かれます。範囲には
sectionの`melody in`を、なければsongのものを使います。範囲は、幹音から始まり、
その1オクターブ上の半音下で終わる1オクターブです（`melody in F5..E6`、
`melody in D4..C#5`）。そのため、どの音名の幹音も範囲にちょうど1回ずつ現れ、
シャープやフラットは、五線譜と同じくそこから音をずらします。たとえば`C4..B4`
では、`cb`は`c`のすぐ下（B3）、`b#`は`b`のすぐ上（C5）です。`'`を1つ付けると
1オクターブ上、`,`を1つ付けると1オクターブ下に移ります。音の高さは直前の音に
左右されないので、1つの音を直してもほかの音は変わりません。範囲をメロディの
最低音の近くから始めると、記号が少なくて済みます。

### 音符に付ける印

記号は音名の前に、音の長さは音名の後ろに書きます。

| 書き方 | 記号 | 鳴らし方 |
|---|---|---|
| `.c8` | スタッカート | 長さを半分にする |
| `!c4` | アクセント | 1段階強く |
| `-c4` | テヌート | 長さいっぱいに |
| `^c4` | マルカート | 1段階強く、長さを4分の3にする |
| `~<c e g>2` | アルペジオ | 和音をばらして弾く。下の音から64分音符ずつずらして弾き始め、全部の音を同時に終える |
| `(c8 d e f)` | スラー | 各音を次の音へ切れ目なくつなぐ。最後の音はつながない |
| `@c2` | フェルマータ | その音の時間を2倍に伸ばす。バンド全体も一緒に待つ |

記号は重ねて付けられます（`.!c8`はアクセント付きのスタッカート）。休符に
付けられるのはフェルマータだけです（`@r2`でバンド全体の休止になります）。タイで
伸ばしている音（弾き直さない音）には、記号を付けられません。フェルマータを
付けた音も、小節の長さを数えるときは書いた長さのままで数えます。アルペジオの
`~`は、楽譜で波線を書く位置、つまり`<`の直前に書きます。音の後ろの`~`はタイ
なので、`~<g, g>2.~`は、次の音へタイでつながるアルペジオです。前の音からタイで
つながってきた音は弾き直さず、アルペジオは弾き直す音の中で一番低い音から
始まります。装飾音は記号より前に書きます（`{d}.c8`）。

スラーは、演奏される順に`(`から`)`まで続きます。小節やsectionをまたげるので、
弱起で始めて何小節も先で終えることもできます。一度に掛けられるスラーは1本だけ
です。スラーの途中にある`(`、スラーの外にある`)`、曲が終わっても閉じていない
スラーはエラーになります。endingはsection本体の続きとして扱うので、本体で
開いたままのスラーは、楽譜では最初のendingでしか閉じていなくても、Kauaiでは
各endingで閉じます。

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

`\`で始まる語は指示で、その次の音から効きます。

| 書き方 | 意味 |
|---|---|
| `\pp` `\p` `\mp` `\mf` `\f` `\ff` | 強弱。それぞれ楽器の`vol`の0.4、0.55、0.7、0.85、1.0、1.15倍。曲は`mf`で始まる |
| `\cresc` / `\dim` | 次の強弱の指示まで、時間に比例して一定の割合で強く / 弱くする |
| `\!` | `cresc`や`dim`をここで終える。始めた時点より1段階強い / 弱い音量で止まる |
| `\tempo 114` | ここからこのテンポにする。`\tempo 4.=76`のように、4分音符以外の音価（この例では付点4分音符）で数えることもできる |
| `\rit 104` / `\accel 140` | 次のテンポの指示の位置でこのテンポになるよう、一定の割合で遅く / 速くする |
| `\tempo` | 曲のテンポに戻す |

小節の行に書いた強弱は、バンド全体に効きます。コード譜の`mf`や`cresc.`を
演奏者全員が見るのと同じです。メロディ、パート、groove、ドラム、派生パートは
すべて、その強弱にそれぞれの`vol`を掛けた音量で鳴ります。そのため、「Aメロでは
全体を抑え、サビに向けてバンド全体で盛り上げる」といった書き方がそのまま
できます。一方、パートの行に書いた強弱は、そのパートの音量だけを変えます。
効くのは、次に小節の行で強弱が指定されるまでで、そこから先はパートもまた
バンド全体の強弱に従います。`cresc`や`dim`は、次に演奏されるsectionまで
続けられます。

`cresc`の後に強弱の指示があれば、`cresc`はそこで終わるので`\!`は要りません。
`p`から`\cresc a b c \f d`と書くと、`a`、`b`、`c`と強くなっていき、`d`で`f`に
達します。`\!`は始めた時点から1段階の音量で止めるので、`p`から
`\cresc a b c \! \f d`と書くと、`mp`までしか上がらず、`d`で`f`に跳びます。

テンポの指示は、バンド全体に効きます。`\rit N`と`\accel N`は、書いた位置から
次に演奏されるテンポの指示まで（後のsectionにあっても構いません）、一定の割合で
テンポを変え、その位置で`N`に達します。そこから先は、その指示のテンポに
なります。後ろにテンポの指示がなければ、曲の最後で`N`に達します。

### パート

パートとは、伴奏用に書き出した音の並びです。フィル、イントロのフレーズ、
対旋律、2重奏や3重奏の各声部などがこれにあたります。sectionにはパートの名前
だけを書き、楽器名は書きません。どの楽器が弾くかは、メロディを`plays melody`で
決めるのと同じように、bandの`plays part NAME`で決めます。パートが書かれている
小節では、そのパートを担当する楽器はgrooveの行の代わりにパートの音を弾きます。
それ以外の小節では、いつもどおりgrooveを弾きます。

パートの書き方は2通りあります。1つ目は、小節の下に、パート名とコロンで始まる
行を置いて、その小節の音を書く方法です。

```kauai
section Intro {
  N.C.          r2.
    Fill:       r4 e g
  G9            r2.
}
```

2つ目は、section（またはending）の中に`part NAME { ... }`のブロックを置き、
小節の順に1行ずつ書く方法です。パートが弾かない小節は`.`と書きます。

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

どちらの書き方でも意味は同じです。1つ目はスコアのように、各小節のパートを
縦に並べて見せるので、ところどころにだけ出てくるパートに向いています。2つ目は
パート譜のように、1つのパートを頭から終わりまで続けて見せるので、曲の間ずっと
鳴り続ける声部に向いています。1つのパートは、section（またはending）ごとに
どちらか一方の書き方で書きます。ブロックの行数は、小節数とちょうど同じにします。

パートの音の書き方は、メロディと同じです。`'`も`,`も付かない音は、パートの
範囲（sectionかsongの`part NAME in LO..HI`、なければメロディの範囲）に
置かれます。

### キメとブレイク

バンドは、grooveから離れて全員で同じリズムを打ったり（キメ）、演奏を止めて
1小節を誰か1人に任せたり（ブレイク）します。そのリズムは、小節の下の`hits:`行に
書きます。`x`はバンドが音を出すところ、`r`は休むところです。音符と同じように、
音価、付点、タイ、連符と、記号の`!`と`.`を使えます。

`hits:`を書いた小節では、バンドはgrooveの代わりにキメを演奏します。

| 楽器 | 各打で弾くもの |
|---|---|
| `plays roots`の楽器 | コードのベース音（ベースの行の`1`） |
| `plays chords`の楽器 | コード。和音の行の`x`と同じボイシングで弾く。`poly`でない楽器はその一番上の音 |
| `plays hits`のドラム | 1打 |
| そのほかのドラム | 何も叩かず、その小節は休む |

1打の長さは音価のとおりで（`x4`なら4分音符）、`.`を付けると半分になり、
タイでつなぐと伸びます（`x8~ x8`）。各打は、そのタイミングで鳴っているコードと、
バンド全体の強弱に従います。`!`を付けると1段階強くなります。メロディは
いつもどおり自分の音符を弾きます。sectionにパートが書いてあれば、パートの楽器も
そのパートを弾きます。grooveの上で弾くときと同じです。

休符だけの`hits:`行はブレイクになります。バンドはその小節の間は音を出さず、
メロディやパートだけが演奏を続けます。

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

キメに加わるドラムは、bandの中で指定します。

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

`2nd hits:`には2回目のキメを書きます。`2nd hits: .`と書けば、その回は
grooveを弾きます（[2回目以降](#2回目以降)）。ブレイク中のドラムのフィルは、
キメではなく、その小節専用のgrooveとして書きます。

### 2回目以降

2回以上演奏するsectionでは、回によって1〜2小節だけ内容が違うことがあります。
小節の下に`2nd:`で始まる行を書くと、そのsectionを2回目に演奏するときの、その
小節の内容になります。`3rd:`なら3回目で、以降も同様です。回数は曲全体を通して
数えます。そのsectionを演奏するsongの行1つにつき1回、`xN`なら`N`回と数えます。
ループする曲では、最初の1周だけを数えます。

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

この行には、その回のコード、音符、またはその両方を書きます。コードを
書かなければ元の小節のコードのままです。上の`2nd: Fm`のように音符を書かなければ、
元の小節の音符を新しいコードの上で弾きます。変わるのはその小節だけです。次の
小節は、コードを書いていなくても、元のコードのまま演奏されます。パートの場合は、
小節の下に`2nd NAME:`と書くと、その回にパート`NAME`が弾く音になります。パートの
ブロックでは、ブロックの行の下に書いた`2nd:`の行が、2回目のその行になります
（これ自体は1小節と数えません）。元の小節にそのパートの音がなくても構いません。
`.`と書けば、その回はパートを弾きません。同じように、`2nd hits:`はその回の
その小節のキメを表し、`2nd hits: .`はその回はキメなしを表します
（[キメとブレイク](#キメとブレイク)）。

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

`Alto 2nd:`ではなく`2nd Alto:`と回数を先に書くのは、行の最初の語を見れば、
それが何の行かわかるようにするためです。

### 弱起

`pickup`行には、sectionの最初の小節へ入る前の音を書きます。音価を書かない
最初の音は、その小節の1拍ぶんの長さになります。弱起の音は、そのsectionの
直前に演奏される小節の最後の拍に重ねて鳴らすので、その小節のその部分は
休符でなければなりません。`x2`で演奏するsectionでは、2回目の弱起を自分自身の
最後の小節に重ねます。

そのsectionが曲の最初にある場合は、曲が弱起から始まります。最初の小節の前に、
弱起の長さだけの小節が置かれ、そこではバンドは演奏しません。ループする曲では、
弱起が曲の最後の小節の最後の拍にもう一度重なるので、その小節のその部分も
休符でなければなりません。

```kauai
section Opening {
  pickup  g8 a
  C             c2 e4 d
  G7            b,1
}
```

### 拍子の変化

section（またはending）の中に`meter N/D`行を書くと、それ以降、そのsectionの
終わりまで拍子が変わります。ほかのsectionはsongの拍子のままです。endingは、
section本体が終わった時点の拍子で始まります。拍子が変わっても、テンポは
4分音符で数えます。小節の最初の音に音価を書かなければ、その小節の拍子での
1拍になります。grooveの行は、小節ごとにその拍子に合わせて分割されます。

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
| `tempo N` | 1分あたりの4分音符の数。拍子に関係なく4分音符で数える（小数も可）。`tempo 4.=60`のように別の音価で数えることもできる（この例は6/8拍子の譜面と同じく付点4分音符） | （必須） |
| `meter N/D` | 1小節の拍数と、1拍にあたる音価。sectionの中で変えられる（[拍子の変化](#拍子の変化)） | `4/4` |
| `key K` | 調。音名で書き、短調には`m`を付ける（`Eb`、`F#m`） | `C` |
| `band NAME` | 演奏するband | （必須） |
| `groove NAME` | grooveを指定しておらず、同じ名前のgrooveもないsectionで使うgroove | なし |
| `melody in LO..HI` | `'`も`,`も付かない音を置く範囲 | （範囲を指定しないsectionがあれば必須） |
| `part NAME in LO..HI` | パート`NAME`の音の範囲 | メロディの範囲 |
| `swing 8` | 8分音符2つの組（`swing 16`なら16分音符）を長・短で弾く。比を書かなければ2:1（`swing 8 3:2`）（[スウィング](#スウィング)） | 書いたとおり |
| `loop` | 最後まで演奏したら頭から繰り返す | 1回だけ |
| `mark NAME` | プログラムから到達を確かめられる位置（[10章](#10-culebra-から再生する)） | |

それ以外の行は、sectionを演奏する行です。次のオプションを、順序を問わず、
それぞれ1回まで付けられます（`Chorus with echo ending 2 in E`）。

| オプション | 意味 |
|---|---|
| `with echo` / `with harmony` | 一緒に鳴らす派生パート |
| `ending N` | 本体の後に演奏するending |
| `in K` | 調`K`で演奏する。sectionの調から、近いほうへ移調する（上方向は増4度まで） |
| `xN` | `N`回演奏する |

各小節で使うgrooveは、sectionが`groove`で指定したもの、なければsectionと同じ
名前のもの、それもなければsongの`groove`の順に決まります。どれもなければ
伴奏なしです。

### スウィング

音符はイーブンのリズムで書き、スウィングは演奏のしかたとして指定します。
コード譜で、イーブンの8分音符の上に*Swing*と書くのと同じ考え方です。
`swing 8`では、小節の頭から8分音符を2つずつ組にして、長・短で弾きます。
1つ目を組の時間の3分の2、2つ目を残りの3分の1で弾くので、3連符の8分音符と
同じリズムになります。`swing 8 3:2`のように比を書くと長短の比率を変えられ、
`swing 16`では16分音符を組にします。

組の前半と後半の中にある音の位置は、それぞれ比例して伸び縮みします。そのため、
`swing 8`の中の16分音符も一緒に跳ね、音の順番が入れ替わることはありません。
連符は、最初と最後の音が鳴る位置の間に中の音を等間隔に並べるので、1拍の
3連符は書いたとおりのリズムで弾かれます。スウィングは、メロディ、パート、
grooveの行やドラムなど、バンドが演奏するものすべてに効きます。

songに書いた`swing`は、すべてのsectionに効きます。section（またはending）の
中に`swing`や`straight`の行を書くと、それ以降の小節のスウィングが変わります。

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

曲は、演奏の前に全体が検査されます。エラーには、ファイル名と行番号が
示されます。grooveの中のエラーでは、grooveの行と、それが鳴った小節の両方が
示されます。

- 音符の長さの合計が拍子と合わない小節。コードの数で拍を等分できない小節（4/4の`C F G`）。スラッシュを使っているのに、1拍ごとにコードか`/`が1つずつ並んでいない小節（4/4の`C / G7`）
- 解釈できない音符、音価、コード、指示。大文字で書いた音符はコードとして読まれ、そこでエラーになる。コード表にない種類（`Cdim9`）。[テンション](#テンション)の表にないテンション（`C7(10)`、`C7(b11)`）や、同じテンションを2回書いたもの
- シャープとフラットの両方が付いた音（`c#b`）。シャープやフラットが3つ以上付いた音
- 休符に付けたフェルマータ以外の記号。タイで伸ばしている音に付けた記号。高さの違う音や休符へのタイ。曲が終わってもつながったままのタイ
- スラーの途中にある`(`、スラーの外にある`)`、曲が終わっても閉じていないスラー
- 比を書かない連符で、数字と中の音の数が合わないもの。0より大きい2つの数になっていない比（`2:3`のように書く）
- 1音だけの和音。`poly`でない楽器で、複数の音が同時に鳴るもの
- 小節の下とブロックの両方に書いたパート。行数が小節数と合わないパートのブロック。最初の小節より前にあるパートの行
- 1音だけのアルペジオ。休符やタイで伸ばしている音の前の装飾音。装飾音やアルペジオのずれに対して短すぎる音
- 変更の対象になる小節や行より前にある`2nd:`の行。コードも音符もない`2nd:`の行。そのsectionが演奏されない回（2回しか演奏しないsectionの`3rd:`）。`1st` `2nd` `3rd` `4th`…以外の書き方をした回数
- `x`と`r`以外を含む、長さが小節と合わない、最初の小節より前にある、または1つの小節の下に2回書いた`hits:`行。楽器（voice）に付けた`plays hits`
- コードのない、sectionやendingの最初の小節
- 同じsectionやendingの中に、繰り返す元の小節がない`%`や`%%`。`%`の下に書いた行
- 直前の小節の音と重なる弱起。直前の小節より長い弱起。ループする曲の冒頭で、曲の最後の小節の音と重なる弱起
- `cresc`や`dim`の途中でない場所の`\!`。曲が終わっても続いている`cresc`や`dim`。テンポを書いていない`\rit`や`\accel`（`\rit 104`のように書く）
- 音価でないもので数えたテンポ（`tempo 3=60`）
- 定義されていないgroove、楽器、ドラム、section、band、ending、派生パート。2回定義された名前
- トークンのない小節や、トークンの長さが音価にならない小節（4/4で15個）を含むgrooveの行
- 同時に鳴るnoiseのドラム2つ。同じチャンネルを使う1つのbandの楽器2つ
- 音声の名前がない、`len`や`env`が付いている、または`vol`が百分率でないhostのドラム
- メロディを弾く楽器がない、または2つ以上ある
- 1オクターブでない範囲。シャープやフラットの付いた音から始まる範囲
- 1拍が音価にならない`meter`（`3/5`）
- `N.C.`の前にある`>`や`<`
- songの行で2回書いたオプション（`in D in E`）
- 取り込まれるファイルにある`song`。1つのファイルにある2つ以上の`song`
- `#`や`b`で始まる、またはコード表にすでにある`voicings`の名前
- `8`と`16`以外を指定した`swing`。「長:短」になっていない比（`3:2`のように書く）。組が小節をちょうど埋めないスウィング（3/8拍子の`swing 8`）

## 10. Culebra から再生する

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
println(song.length())     # => 19.2
println(song.events()[0])  # => {at: 0.0, len: 0.6, by: 'Tune', pitch: 76, vol: 20.0}
song.play()
```

`Audio.Kauai.new(text)`は曲を文字列で受け取ります。この場合、`use`でほかの
ファイルを取り込むことはできません。`Audio.Kauai.load(path)`は曲のファイルを
読み込み、そのファイルが`use`で指定したファイルも、そこからの相対パスで
読み込みます。`dir:`を渡すと、ファイルシステムの代わりにディレクトリのハンドル
（`Embed.dir`、または`exists(name)`と`read(name)`を持つオブジェクト）から
読み込むので、1つのバイナリにビルドしたプログラムにも曲を同梱できます。
どちらも曲を検査し、見つかった最初のエラー（[9章](#9-エラー)）を`KauaiError`
として投げます。メッセージにはファイル名と行番号が付きます（`ballad.kau:12: ...`、
文字列から作った曲なら`line 12: ...`）。

| メソッド | 動作 |
|---|---|
| `play()` | `prepare()`でまだ作っていないhostの音声を作ってから、頭から演奏する（演奏中なら頭から演奏し直す） |
| `stop()` | 演奏を止め、鳴っている音も消す |
| `volume(v)` | 曲全体の音量（`0.0`〜`1.0`）。次の音から反映される |
| `playing()` | 演奏中かどうか。`play()`から`stop()`まで、`loop`しない曲なら曲の終わりまでが演奏中 |
| `reached(NAME)` | `mark NAME`の位置まで演奏が進んだかどうか |
| `prepare()` | hostの音声を1つ作り、残りの数を返す |
| `length()` | 1回通して演奏したときの秒数 |
| `events()` | 演奏する音を時間順に並べたもの。各要素は`{at, len, by, pitch, vol}`で、`at`と`len`は秒、`by`は楽器かドラムの名前、`pitch`はMIDIのノート番号（ドラムは`nil`）、`vol`はその音の強弱での楽器の`vol`（hostの音声では、音声自身の音量に対する割合） |

runtimeは、すべての音を音声ストリーム自身のクロックに合わせて予約します。
そのため、1フレームの処理が長引いてもテンポは崩れず、フレームループを持たない
プログラムでも演奏できます。また、曲は自分で経過時間を管理しています。
`playing()`と`reached()`は、音声デバイスで実際に鳴っているかどうかに関係なく、
`play()`からの経過時間をもとに答えます。

```culebra
# doctest: skip (reads ballad.kau, and electric_piano is the program's own)
let song = Audio.Kauai.load("ballad.kau", voices: {
  ep: |pitch, params| Audio.Sound(electric_piano(pitch, params.bright)),
})
song.prepare()          # build one host sound; answers how many remain
song.play()
song.reached("Drive")   # has the song passed `mark Drive`?
```

host楽器の音声は、音の高さごとに1つずつ作ります。曲で使う高さのそれぞれに
ついて、`voices:`に渡した関数のうち、その楽器のhost名に対応するものを呼び出し
ます。引数は、音の高さ（MIDIのノート番号。`60`が中央のド）と、その楽器の
`name=value`パラメータをまとめたオブジェクトです。関数が返した`Audio.Sound`を
その高さの音として鳴らします。hostのドラムの音声は1回だけ作ります。`drums:`に
渡した関数のうち、その音声の名前に対応するもの（`drums: {kick: fn (params) { ... }}`）
を、ドラムの`name=value`パラメータを引数にして呼び出し、返された`Audio.Sound`を
鳴らします。`voices:`と`drums:`のキーは、host名を小文字にしたものです
（`host Ep`なら`ep:`、`host HiHat`なら`hihat:`）。対応する関数がないhostの音声が
あると、曲を読み込んだ時点で`ValueError`になります。`prepare()`は音声を1つ作って
残りの数を返すので、ゲームでは音声の準備を複数のフレームに分けられます。
`play()`は、残っている音声をすべて作ってから演奏を始めます。

## 付録 A: コードの種類

`plays chords`で和音を積むときや、ベースの行で度数を読むときに使う表です。
`voicings`ブロックで種類を追加できます（[4章](#4-ファイル)）。種類は、表に
あるどの表記で書いても構いません。数字の入った括弧は、その前の種類に加える
テンションです（[テンション](#テンション)）。結果が表にある和音と同じなら、
その和音として扱います（`C7(b9)`は`C7b9`）。それ以外の括弧は、外してから表を
引きます（`Cm(maj7)`は`Cmmaj7`）。

| 種類 | 書き方 | ほかの表記 | 音程 |
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

種類の後ろの括弧にカンマ区切りで書いたテンションは、コード譜と同じように、
その種類に音を加えます。

| 書き方 | 加える音 | 半音 |
|---|---|---|
| `9` `b9` `#9` | 9度と、そのフラット、シャープ | 14、13、15 |
| `11` `#11` | 11度と、そのシャープ | 17、18 |
| `13` `b13` | 13度と、そのフラット | 21、20 |
| `b5` `#5` | 5度のフラット、シャープ。元の5度と置き換える | 6、8 |

加わるのは書いた音だけです。`C7(13)`はC7に13度を加えたもので、9度は
含みません（`C7(9,13)`と`C13`は両方を含みます）。表でルートの1オクターブ上を
重ねている3和音では、テンションがそのオクターブ上の音と置き換わります。
`C(9)`は`Cadd9`と、`Cm(9)`は`Cmadd9`と同じです。括弧の中では、`-`と`+`を
フラットとシャープの意味で使えます（`C7(-9)`、`C7(+11)`）。括弧の外の`-`は、
これまでどおりマイナーの意味です（`C-7`）。表にないテンション（`C7(10)`、
`C7(b11)`）、同じテンションの重複、`b`も`#`も付かない`5`はエラーです。

```kauai
section Drive {
  CM7(9)            e4 g b d'
  Am7(11)           c'2. b4
  Dm7(9) G7(9,13)   a4 f e d
  C6(9) C7(b9,#11)  e2 g4 bb
}
```

ベースの行の度数は、そのコード自身が持つ音から選びます。`3`は3〜5半音、
`5`は6〜8半音、`7`は9〜11半音、`9`は13〜15半音上にある構成音です。

## 付録 B: 文法

PEGで書いた文法です。ファイルは1行ずつ読み、`EOL`は行の終わりを、行の中の
要素の区切りは空白を表します。この文法は、実際の言語より広い範囲を受け付けます。
文法だけでは弾けない部分（小節の拍数や、名前が定義済みかどうかなど）の検査は、
[9章](#9-エラー)に挙げています。

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

あるゲームのために書いた3曲（レースの1周目と2周目、エンディングのテーマ）と、
エリック・サティの曲を載せます。1周目と2周目の曲は、`use`でbandを共有しています。

### run.kau

1周目と2周目の曲で共有するbandです。

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

2周目の曲です。2回目のサビは1回目を全音上に移調したもの（`in E`）で、最後の2小節は移調せず、書いたとおりの音でDに戻ります。

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

エンディングのテーマです。エレクトリックピアノはhost楽器（`Ep`）で、和音を弾きます。メロディも同じエレクトリックピアノで、音色を明るくして弾きます。

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

*ジュ・トゥ・ヴー*（エリック・サティ、1897年）。歌のパートは、スラー、強弱、クレッシェンド、ディミヌエンドまで含めて、OpenScore Liederの楽譜（CC0）のとおりです。前奏のピアノの右手はパート`Fill`として書き、和音やタイも楽譜どおりにしてあります。コード記号は、ピアノのパートから読み取って付けました。`\rit 104`と、それを受ける`\tempo`は、楽譜の*retenir*（テンポを落として）と*au refrain*（リフレインのテンポで）を表しています。

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

ほかの記譜法で身についた書き方のうち、Kauaiでは書き方や意味が違うものを
まとめます。左がよくある書き癖、右がKauaiでの書き方です。多くは、癖のまま
書いてもエラーにならず、意図と違う音が鳴ってしまいます。

| 書き癖 | Kauaiでは |
|---|---|
| 調号によって音にシャープやフラットが付く | 調は臨時記号を付けない。`key D`でもFシャープは`f#`と書く |
| `c'`は決まった1つのオクターブを指す | `'`と`,`は、範囲（`melody in LO..HI`）から1オクターブずらす |
| 音符の行の`C4`は中央のド | 音符は小文字で書き、後ろの数字は長さ。`c4`は4分音符。`C4`が音の高さを表すのは、範囲と`from`の後ろだけ |
| `bb`はBが2つとも読める | `bb`はBのフラット。Bのダブルフラットは`bbb` |
| 長さの省略が次の小節にも引き継がれる | 小節の最初の音で音価を省略すると1拍になる |
| 記号を音の後ろに書く（`c4-.`） | 記号は音の前に書く。`.c4` |
| 強弱を`p`、`\<`、`/p`のように書く | 指示は`\`で始める。`\p`、`\cresc`と書き、`cresc`は`\!`で終える |
| 9度を加えたコードを`C9`と書く | `C9`は短7度を含む。含まないのは`Cadd9`と`C(9)` |
| `Cdim`でディミニッシュ・セブンスを表す | `Cdim`は3和音。4和音は`Cdim7` |
| 6/8拍子で、付点4分音符を数えて`tempo 60` | `tempo`は4分音符で数える。付点4分音符で数えるなら`tempo 4.=60` |
| 16ステップのつもりで、15や17個並べたドラムの行 | 1ステップの長さが音価にならなければいけないので、数え間違いはエラーになる |
