# Kauaiで音を作る

Kauaiの曲が鳴らす音についての案内です。内蔵の4つの声、音の形の整え方、そして
プログラムやフォルダが`external`の楽器を提供する方法を扱います。最初は、ほかに何も
要らない内蔵の音から始め、最後に録音から作る楽器の入り口を示します。言語の完全な仕様は
[言語仕様](../kauai/language.ja.md)にあり、曲の書き方は[Kauaiで曲を書く](kauai-songs.ja.md)にあります。

## 内蔵の声

バンドは声に名前を付けます。それぞれの声は、内蔵の4種類から音源を1つ選び、
その後に音を整えるオプションを書きます。

```kauai
band Run {
  voice Lead    pulse     plays melody            duty 1/4  vol 15  env 1 2 4  gap 2
  voice Second  pulse2                            duty 1/8  vol 9   env 1 2 6  gap 2
  voice Bass    saw       plays roots in E2..D#3            vol 18  env 0 3 3  gap 3
  voice Arp     triangle  plays roots in C3..B3            vol 7   env 1 4 6  gap 4
}
```

| 音源 | 音 |
|---|---|
| `pulse` | パルス波。形は`duty`で決まります |
| `pulse2` | 2つ目のパルス波。`pulse`の声の横でハーモニーに使います |
| `triangle` | 三角波 |
| `saw` | のこぎり波 |

声には、次のオプションを付けられます。`vol`は必須です。

| オプション | 意味 | 既定値 |
|---|---|---|
| `duty 1/8`、`1/4`、`1/2`、`3/4` | パルス波のデューティ比 | `1/2` |
| `vol N` | `mf`での音量。`0`から`100`まで | （必須） |
| `env A D R` | アタック、ディケイ、リリース。単位は1/60秒 | `0 0 0` |
| `gap N` | 各音の末尾を切る長さ。音を離して聞こえさせます | `0` |
| `poly` | 一度に複数の音を鳴らす声のため。音色の声にも外部の声にも付けられる | 一度に1音 |

1つの音源は一度に1つの音しか鳴らせず、バンドの2つの声が同じ音源を共有することはできません。
`plays melody`と書いた声はメロディを、`plays roots`は各和音の下のベースを、
`plays chords from NOTE`は和音そのものを、その音から上へ並べて鳴らします。`plays`を
書かない声は、エコーまたはハーモナイズしたもの（派生パート）だけを鳴らします。

プログラムからバンドを鳴らして、音を確かめられます。

```culebra
let song = Audio.Kauai.new(`
band Run {
  voice Lead    pulse     plays melody            duty 1/4  vol 15  env 1 2 4  gap 2
  voice Second  pulse2                            duty 1/8  vol 9   env 1 2 6  gap 2
  voice Bass    saw       plays roots in E2..D#3            vol 18  env 0 3 3  gap 3
  voice Arp     triangle  plays roots in C3..B3            vol 7   env 1 4 6  gap 4
}

song Sound {
  tempo 100
  band Run
  groove Pad
  melody in C5..B5

  Line
}

groove Pad {
  Bass  1 - 5 -
  Arp   5 - 1 -
}

section Line {
  C  e4 g c'2
  F  a4 c' f2
}
`)
println(song.length())             # => 4.8
println(song.events()[0])          # => {at: 0.0, len: 0.6, by: 'Lead', pitch: 76, vol: 15.0}
```

`Second`には`plays`がないので、派生パートが与えたものだけを鳴らします。たとえば`Lead`の下に
ハーモニーを付ける`harmonizes`の行です（[派生パート](../kauai/language.ja.md#派生パート)）。

## 音の形を整える

音を形づくるのは、主に次の4つのオプションです。

- **`env`** は、音のアタック、ディケイ、リリースを決めます。単位は60分の1秒です。
  既定の`0 0 0`は、何の整形もしません。
- **`gap`** は各音の末尾を切ります。そのため、行の音は別々の音として聞こえます。
  既定の`0`は、何も切りません。
- **`duty`** はパルス波のデューティ比を決めます。ほかの音源には効きません。
- **`vol`** は音量です。強弱記号（`\pp`から`\ff`まで。言語仕様を参照）は、ここから音量を変えます。

指示は、その音符から、バンド全体または1つのパートの強弱を変えます。小節線の上の強弱は
バンド全体のもので、パートの行の中に書くと、そのパートだけに効きます。

## 外部の声

`voice`は、音源をプログラムから受け取れます。`external NAME`は、プログラムが用意する楽器を指します。
プログラムは、名前ごとに関数を`voices`の下で曲に渡します。関数は、音の高さと楽器のパラメータを
受け取り、その音の音を返します。

```culebra
let song = Audio.Kauai.new(`
band Lead {
  voice Ep  external Ep bright=0.5  plays melody  vol 40
}

song Test {
  tempo 120
  band Lead
  melody in C4..B4

  Line
}

section Line {
  C  c4 e g c4
}
`, {ep: |pitch, params| Audio.Sound([0.0, 0.5, 0.0, -0.5], rate: 44100)})
println(song.length())             # => 2.0
println(song.events()[0])          # => {at: 0.0, len: 0.5, by: 'Ep', pitch: 60, vol: 40.0}
```

ここでの関数は仮のものです。4つのサンプルを返しているだけで、実際の楽器なら
`pitch`の音を`params`に従って合成します。パラメータは、バンドの行（`external Ep bright=0.5`）から
来ます。楽譜は、音符の途中で`\bright=1.3`のような指示を書くことで、それを変えられます。

外部の声は、各音の音を最後まで鳴らします。そのため、`env`、`gap`、`duty`は受け付けません。
音符の長さは、それを聞く楽器にだけ効きます。`vol`は、ふつうどおり音量を決めます。

### コマンドラインで外部の音を使う

`culebra play`は、`external`の声の楽器を持っていません。`voices`を書き出したモジュールを
渡すと、その声で曲を鳴らします。

```culebra
# doctest: skip (a module for --sounds; electric_piano is the program's own)
let voices = {ep: |pitch, params| Audio.Sound(electric_piano(pitch, params.bright))}
export {voices}
```

```sh
culebra play --sounds sounds.cul song.kau
```

モジュールが作らない外部の音を曲が指定していると、何も鳴る前に拒否されます。すぐに間違いが分かります。

## モデルから音を作る

外部の楽器は、音のモデルにもできます。culebraで`Audio.partials`を使い、倍音の列と
包絡線から、倍音の音を作れます。ホルンのように、音の演奏の仕方に応じて鳴る楽器は、
オブジェクトとして渡します。`phrase`が関数で、フレーズの音符をまとめて受け取ります。
`hears`は、各音符のどの情報を受け取るかを指定します（`len`、`level`、`next`など。
[言語仕様](../kauai/language.ja.md#10-culebra-から再生する)を参照）。

次の関数は、音符の長さの間だけ鳴るオルガンのような音を作ります。

```culebra
fn freq_of(pitch) { 440.0 * Math.exp((pitch - 69) * Math.log(2.0) / 12) }

let organ = |notes, params| {
  let seconds = notes[0].len + 0.2
  Audio.Sound(Audio.partials(
    seconds,
    freq: [(0.0, freq_of(notes[0].pitch))],
    spectrum: [(0.0, [1.0, 0.5, 0.25])],
    gain: [(0.0, 0.0), (0.02, 0.5), (notes[0].len, 0.5), (seconds, 0.0)],
  ))
}

let song = Audio.Kauai.new(`
band Pipe {
  voice Organ  external Organ  plays melody  vol 60
}

song Test {
  tempo 100
  band Pipe
  melody in C5..B5

  Line
}

section Line {
  C  c4 e g c4
}
`, {organ: {phrase: |notes, params| organ(notes, params), hears: ["len"]}})
println(song.length())             # => 2.4
```

モデルを作るときに使うものを、必要になる順に挙げます。

- **`freq`、`gain`、`spectrum`** は包絡線です。音の始まりからの秒数と値の組`(at, value)`の列です。
  `spectrum`は`(at, amps)`の列で、k番目の振幅は、基音のk倍の倍音に対応します。
- **`Audio.partials(seconds, ...)`** は、最初に長さを受け取ります。サンプリング周波数の0.45倍を
  超える倍音は、自動的に除かれます。
- **`Math.pow`は整数しか受け取らない**ので、小数の累乗は、Floatを含む`x ** y`で書きます。
- **`range(n).map(...)`は遅延評価される**ので、`.collect()`で配列にしてから渡します。
- フレーズの中で変わる音（ポルタメントやビブラートなど）は、包絡線の点として書きます。たとえば、
  10ミリ秒ごとに1点を置きます。

[標準ライブラリの資料](../stdlib.ja.md)に、`Audio`のすべての関数があります。
`Audio.filter`、`Audio.envelope`、`Audio.mix`で音をさらに整えられ、`Audio.Graph`で部品から
組み立てられます。

## ハーネスを使って録音から楽器を作る

実際の演奏者の録音から楽器を作れば、モデルではなく、その演奏者に近い音になります。各録音の音を測り、
曲の音符は、音の高さが近い録音の音を借りて鳴らします。鳴らすのはエンジンです。手間は、録音を選ぶことと、
数値を決めることにあります。culebraのリポジトリにあるKauaiのハーネスは、この作業を自動で進めます。

ハーネスの流れは次のとおりで、それぞれを1つの役割を持つエージェントが行います。

1. **設計**：録音に必要な範囲を決めます。音域、半音ごとの音の数、1オクターブごとの長い音、強い音・中くらいの音・
   弱い音、曲で使うスラーです。探す前に決めて、探す目標にします。後で、カバーの道具で確かめます。
2. **探す**：設計を満たす録音を見つけます。各出典のライセンスは、再利用を認めている必要があります。
   クレジットの表記と、使い方を知らせることを求めるコレクションは、そのクレジットをパッケージの
   READMEに書けば使えます。
3. **曲を書く**：楽器を調整し、評価するための曲を書きます。
4. **測る**：測る道具で録音を測ります。10ミリ秒ごとの音の高さ、音量、倍音、息と、録音に含まれる音符を読みます。
5. **作る**：測った結果から、楽器のデータを作ります（`make_data`）。また、楽器の設定（`data/instrument.json`の
   `play`）を、目標に合わせて当てはめます。
6. **判断する**：距離で判断します。比べる道具が、調整用の曲の演奏を目標と比べ、一度に1つだけ変えて、
   距離が下がらなくなるまで繰り返します。
7. **懐疑する**：評価用の曲は、調整には使いません。よい数字が、ただの小細工で得られていないかを確かめます。
8. **聴く**：数字では、つなぎが自然かどうかは分かりません。聴き比べの道具は、2つの版を、同じ音量で、
   1と2の伏せた名前で鳴らします。聴く人は、好みの版を残します。

手順5と6の当てはめには、目標が要ります。目標とは、同じ曲を、その楽器の演奏者が演奏した録音のことです。
そのような目標がない録音版の楽器も、データと設定を手で決めれば作れます。ただし、数字で調整はできず、
判断はすべて聴く段階が担うことになります。そのときは、パッケージのREADMEにそう書いてください。

このようにして作ったパッケージは、1つのフォルダとして置きます。
中身は、データを共有エンジンに渡す`main.cul`、`data/`フォルダ、そして使った出典とそのライセンスをすべて書いた
READMEです。
