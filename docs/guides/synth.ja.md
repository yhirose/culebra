# Synthで音を書く

音を式として書く名前空間`Synth`のガイドです。正弦波1つから始めて、時間の形、音色、弦、エコー、ステレオの部屋、音符、鳴らしている最中の変更を、1つずつ足していきます。例はどれも、書いたとおりに動くプログラムです。完全なリファレンスは[`stdlib.ja.md` §46](../stdlib.ja.md#46-synth)、これらの音を使える音楽言語は[Kauaiで音を作る](kauai-sounds.ja.md)にあります。

## 音は式

`Synth.sine(440.0)`は440Hzの正弦波です。これは*部品*（part）で、音声そのものは持たない値です。部品は`+`、`-`、`*`で組み合わせられ、`render`が式を計算してサンプルを返します。

```culebra
let {sine, render} = Synth
let tone = sine(440.0) * 0.2
let samples = render(tone, 0.5)
println(samples.size())  # => 22050
println(samples.max() <= 0.2)  # => true
```

`let {sine, render} = Synth`で名前をスコープに入れるので、`Synth.`なしで書けます。`let {*} = Synth`なら全部入ります。以下の例ではこれを使います。

部品は作った後で変わらないので、式は取っておいて何度も使えます。同じ部品を2回使っても1つの部品です。`let a = sine(440.0)`のとき、`a + a`は1つの波を自分に足したもので、`sine(440.0) + sine(440.0)`は2つの波です。

## 聞く

`play`は式をデバイスで鳴らします。波は自分では終わらないので、`secs`を渡します。

```culebra
# doctest: skip (it plays on the device)
let {*} = Synth
play(sine(440.0) * 0.2, secs: 1.0)
```

`play`は音が終わるまで待ちます。`wait: false`なら、すぐ戻り、プログラムが他のことをする間も音は鳴り続けます。

聞く代わりに眺めるなら、`render`がサンプルを返し、`Audio.Sound.new(samples).data()`がWAVになるので、`FS.write`で保存できます。

[Synth Studio](https://yhirose.github.io/culebra/playground/studio.html)は、プログラムの最後の式を鳴らし、その波形、スペクトル、倍音を描きます。音を変えたときの違いが、聞くだけでなく見えます。ブラウザで動き、[`examples/synth-studio/`](../../examples/synth-studio/README.md)は同じページをネイティブのウィンドウで開くものです。

## 時間の形: エンベロープ

一定の大きさの正弦波はテスト音です。ベルや撥弦を作るのは音量の動き方で、それをエンベロープで書きます。`perc(attack, release)`は、打たれた音のように、ピークまで上がって0へ戻ります。

```culebra
let {*} = Synth
let blip = sine(880.0) * perc(0.005, 0.3, peak: 0.3)
println(render(blip).size())  # => 13450
```

音は、出力が0のまま戻らなくなったところで自然に終わります。エンベロープとの積は、エンベロープが終わるところで終わるので、`seconds`なしの`render`もそこで止まります。波だけでは終わらないので、前の例では`secs`が要りました。

`env(levels, times)`は一般形で、`levels`を`times`の長さの区間でつなぎ、最後のレベルを保ちます。`curve`で区間を曲げられ、`"exp"`は一定の比で動きます（音高の聞こえ方に合います）。

```culebra
let {*} = Synth
let swell = env([0.0, 0.3, 0.1, 0.0], [0.2, 0.3, 0.5])
let drop = xline(2000.0, 100.0, 0.5)  # a pitch falling 4.3 octaves
println(swell.at(0.2))  # => 0.3
println(drop.duration())  # => 0.5
let kick = sine(drop) * perc(0.001, 0.4, peak: 0.8)
println(render(kick).size() > 17000)  # => true
```

数を取る所にはエンベロープも渡せて、その値の最初からの予定表になります。上のキックは、音高が落ちていく正弦波です。

`adsr(attack, decay, sustain, release)`は押さえ続ける音のためのものです。ピークまで上がり、サステインのレベルまで下がり、離されるまでそこにいます。`e.hold(secs)`は、始まりから`secs`秒で離します。

```culebra
let {*} = Synth
let held = sine(440.0) * adsr(0.02, 0.1, 0.6, 0.3).hold(0.5) * 0.3
println(render(held).size())  # => 35280
```

## 信号は値

数を取る所に部品を渡すと、その信号そのものが値になります。つまり部品で別の部品の値を動かせて、よく使うエフェクトはこれで書けます。

**トレモロ**は音量を動かします。-1から1の正弦波を、`0.5 + 0.5 * sine(5.0)`か`range`で0から1の音量にします。

```culebra
let {*} = Synth
let tremolo = sine(330.0) * sine(5.0).range(0.3, 1.0) * 0.3
println(render(tremolo, 1.0).size())  # => 44100
```

**ビブラート**は音高を動かします。`hz`は音番号をHzにし（69が440Hz、半音は1オクターブの12分の1）、信号を渡すと信号を返すので、`sine(5.0) * 0.3`は0.3半音のゆれになります。

```culebra
let {*} = Synth
let vibrato = sine(hz(69.0 + sine(5.0) * 0.3)) * 0.3
println(render(vibrato, 1.0).size())  # => 44100
```

**FM**は、波の位相を別の波で動かします。2つの周波数の比が、結果が倍音になるかを決めます。616 / 440は1.4で、部分音が倍音の間に入ります。深さを音より早く減らすと、消えていくにつれて純音に近づくベルになります。

```culebra
let {*} = Synth
let depth = perc(0.001, 1.2, peak: 3.0)
let bell = sine(440.0, phase: sine(616.0) * depth) * perc(0.002, 2.5, peak: 0.3)
println(render(bell).size() > 100000)  # => true
```

## 音色: 波とフィルタ

`sine`、`saw`、`square`、`triangle`が4つの波で、`pulse(freq, width)`は幅を持つ矩形波、`noise()`はホワイトノイズです。のこぎり波にはすべての倍音があり、正弦波には1つしかないので、フィルタが働く材料はのこぎり波のほうに多くあります。

`lowpass`、`highpass`、`bandpass`は部品のメソッドで、`cutoff`と`q`を取ります。`q`が高いとカットオフで鳴ります。カットオフもほかの値と同じなので、エンベロープで掃引できます。

```culebra
let {*} = Synth
let cutoff = env([100.0, 5000.0, 100.0], [2.0, 2.0], "exp")
let sweep = saw(55.0).lowpass(cutoff, q: 6.0) * 0.2
println(render(sweep, 4.0).size())  # => 176400
```

数セントずらした2本ののこぎり波はうなりを作り、1本より広く聞こえます。部品を渡す所に配列を渡すと、要素ごとに1チャンネルになり、`mix`がそのチャンネルを足して1つの部品に戻します。

```culebra
let {*} = Synth
let pad = saw([110.0, 110.6]).mix().lowpass(700.0 + sine(0.4) * 400.0, q: 2.0) * 0.2
println(render(pad, 1.0).size())  # => 44100
```

## 弦

`pluck`は、入力で弾かれる弦です。短いノイズのバーストが、1周期ぶんの長さのディレイの中で鳴り続けます。`ring`は消えるまでの秒数です。`|>`で書くと、バーストが受け手になります。

```culebra
let {*} = Synth
let string = noise() * perc(0.001, 0.01) |> pluck(hz(57.0), ring: 1.5)
println(render(string).size() > 66000)  # => true
```

弦を3本、順に弾くと和音になります。`delay(time)`は`time`秒前の部品なので、各弦を時間の中に置けます。

```culebra
let {*} = Synth
let string = fn (note, at) {
  noise() * perc(0.001, 0.01) |> pluck(hz(note), ring: 2.0).delay(at)
}
let chord = [string(57.0, 0.0), string(60.0, 0.15), string(64.0, 0.3)].mix() * 0.5
println(render(chord).size() > 88000)  # => true
```

## エコーはループ

`feedback(|x| ...)`でループを書きます。`x`は式の答えを、ディレイを通して聞いたものなので、`x.delay(0.33) * 0.5`は3分の1秒前の音を半分の大きさにしたものです。

```culebra
let {*} = Synth
let blip = sine(880.0) * perc(0.001, 0.08, peak: 0.4)
let echo = feedback(|x| blip + x.delay(0.33) * 0.5)
println(render(echo, 2.0).size())  # => 88200
```

`x`から答えまでにディレイを通らない道があると`ValueError`です。音が同じ瞬間の自分に依存してしまうからです。ループは1サンプルずつ計算するので、ループなしの同じ部品の数倍の費用がかかります。

`Synth`に名前のないフィルタもループで書けます。`y.delay(0.0)`は1サンプル前の答えなので、1次のローパス`y = 0.05 x + 0.95 y[-1]`は次のとおりです。

```culebra
let {*} = Synth
let x = saw(220.0) * 0.8
let smoothed = feedback(|y| x * 0.05 + y.delay(0.0) * 0.95)
println(render(smoothed, 0.1).size())  # => 4410
```

## ステレオと部屋

部品を渡す所に2要素の配列を渡すとステレオで、最初が左、2番目が右です。`p.pan(pos)`は1チャンネルを、-1（左）から1（右）の間に等パワーで置きます。`channels: 2`の`render`は`[left, right]`を返します。

```culebra
let {*} = Synth
let wide = [saw(110.0).pan(-0.5), saw(110.77).pan(0.5)].mix() * 0.1
let sides = render(wide, 0.5, channels: 2)
println((sides.size(), sides[0].size()))  # => (2, 22050)
```

`reverb(mix, room, damp)`は部品を部屋に入れ、ステレオにします。`mix`は音のうち部屋の分の割合、`room`は響きの長さ、`damp`は高域が消える早さです。次は4音の和音で、各音を左右に少しずらした2本ののこぎり波にし、ゆっくり動くフィルタと広い部屋を通します。

```culebra
let {*} = Synth
let voice = fn (note) {
  let f = hz(note)
  [saw(f).pan(-0.7), saw(f * 1.004).pan(0.7)].mix()
}
let chord = [48.0, 55.0, 60.0, 63.0].map(voice).mix()
let cutoff = 400.0 + (sine(0.25) + 1.0) * 1200.0
let swell = adsr(0.8, 0.5, 0.7, 1.5).hold(4.0)
let wide = chord.lowpass(cutoff, q: 1.5) * swell * 0.08 |> reverb(mix: 0.35, room: 0.8)
println(render(wide, channels: 2)[0].size() > 200000)  # => true
```

## 音符

`Voices(count, builder)`は音符を鳴らします。`builder(freq)`は1つの声の式を返し、`freq`は音符ごとに設定されるHzの信号で、`count`個の声それぞれについて1回ずつ呼ばれます。ゲート付きのエンベロープ（`adsr`）は音符の始まりで始まり、音符の終わりで離されます。

```culebra
let {*} = Synth
let lead = Voices(4, |f| sine(f) * adsr(0.01, 0.1, 0.6, 0.2) * 0.2)
lead.sequence([60, 64, 67, nil, 72], step: 0.25, len: 0.2)
println(lead.ends())  # => 1.4
println(render(lead, 1.4).size())  # => 61740
```

`sequence`は音符を`step`秒おきに、それぞれ`len`の長さで置き、`nil`は休符です。`note_on(note, at)`と`note_off(note, at)`は1音ずつ置きます。音符は空いている声へ行き、空きがなければ最も長く鳴っている声へ行って、その声は先に5msでフェードアウトします。

声のグラフは音符ごとではなく1回だけ作られるので、状態を持つものは音符から音符へ引き継がれます。`freq.lag(0.1)`は前の音符から今の音符へ滑り、ポルタメントになります。

## 鳴らしている最中に変える

`control(name, init)`は、外から設定できる値です。`wait: false`の`play`はハンドルを返し、`h.set(name, v)`でコントロールを動かします。

```culebra
let {*} = Synth
let h = sine(control("freq", 440.0)) * perc(0.01, 1.0) * 0.3 |> play(wait: false)
println(h.ends())  # => 1.01
h.set("freq", 660.0, at: 0.5)  # from half a second in
h.set("freq", 330.0, secs: 0.2)  # now, over a fifth of a second
h.stop()
```

`h.set`は、`secs`で直線の滑らかな変化、`at`で音自身の時計での時刻を取ります。`lo`と`hi`を渡したコントロールは、その間に収まります。

`Proxy`は、鳴らしている最中に中身の音を入れ替えられる箱で、ほかの式から信号として読めます。`box.set(expr)`で新しい音を入れると、古い音は`fade`秒でフェードアウトし、新しい音は同じ間にフェードインします。箱を読む音は、グラフを作り直さずに入れ替わりをすぐ聞くので、箱は音をライブで編集する手段になります。

```culebra
let {*} = Synth
let lfo = Proxy()
lfo.set(sine(5.0))
let box = Proxy(fade: 0.05)
box.set(saw(110.0 + lfo * 3.0).lowpass(800.0) * 0.2)
box.play()
lfo.set(sine(7.0))  # the vibrato quickens where box reads it
box.set(saw([110.0, 110.77]).mix().lowpass(1200.0) * 0.1)  # crossfades
box.stop()
lfo.stop()
```

箱を読む音には自分の終わりがなく、鳴らしているときにだけ聞こえます。その`render`は`ValueError`です。

## MIDIコントローラーで弾く

キーボードやウインドコントローラーで弾く音は、入力が読むメッセージ（[`MIDI`](../stdlib.ja.md#41-midi)）で動かします。ノートオンとオフは`Voices`へ、ベンド、コントローラー（CC）、アフタータッチは`control`へ、`play`が返したハンドルで設定します。1回の`set`で全部の声が動きます。

```culebra
let {*} = Synth
let bend = control("bend", 1.0)  # a ratio to each note's freq
let vibrato = sine(5.5) * control("wheel", 0.0).lag(0.05) * 0.01
let pitch = bend * (1.0 + vibrato)  # made once, shared by every voice
let keys = Voices(8, |f| saw(f * pitch) * adsr(0.01, 0.2, 0.7, 0.3) * velocity() * 0.1)
let h = play(keys, wait: false)
println(h.ends())  # => nil
h.stop()
```

```culebra
# doctest: skip (it needs a MIDI device)
let input = MIDI.Input.new(MIDI.inputs()[0].id)
for m in input.messages() {
  cond {
    m.kind == "note_on" => keys.note_on(m.note, velocity: m.velocity),
    m.kind == "note_off" => keys.note_off(m.note),
    m.kind == "bend" => h.set("bend", 2.0 ** (m.bend * 2.0 / 12.0)),  # a range of 2 semitones
    m.kind == "cc" && m.cc == 1 => h.set("wheel", input.control(1)),
    _ => {},
  }
}
```

コントローラーの値は、幅の128分の1ずつ段になって動き、音量やカットオフでは段ごとの跳びがクリックやジッパーのような音に聞こえます。`control`の後の`.lag(t)`は、段ごとの跳びを約`t`秒かけて滑らかにします。0.02から0.05なら、演奏に遅れずについていけて、段も聞こえません。14ビットで送るコントローラーは段が細かく、`input.control(cc)`はその精度で読みます。メッセージの`value`は7ビットです。

ウインドコントローラーは1度に1音を吹き、息（多くはCC 2）が音の大きさです。声は1つで足ります。音高は滑るcontrolにし、息をそのままエンベロープにします。

```culebra
let {*} = Synth
let breath = control("breath", 0.0).lag(0.02)
let freq = hz(control("note", 60.0)).lag(0.05)
let h = saw(freq).lowpass(300.0 + breath * 4000.0) * breath * 0.3 |> play(wait: false)
println(h.ends())  # => nil
h.stop()
```

```culebra
# doctest: skip (it needs a MIDI device)
for m in input.messages() {
  cond {
    m.kind == "note_on" => h.set("note", m.note),
    m.kind == "cc" && m.cc == 2 => h.set("breath", input.control(2)),
    _ => {},
  }
}
```

ノートオンで音高だけを動かしてノートオフには何もしなければ、レガートになり（[`MIDI`](../stdlib.ja.md#41-midi)にあるとおり、ウインドコントローラーは次の音のノートオンを先に送ります）、音を止めるのは息だけになります。

## Kauaiの曲の音

Kauaiの曲は、各声が鳴らす音を名前で指し、プログラムはその音を供給できます。`Synth`はその音を書く手段です。外部の楽器は、音高に対して`Audio.Sound`を返す関数で、`seconds`なしの`render`は、消えるまでの長さの音を作ります。そのため、音符を何秒保つかは楽器ではなく曲が決めます。

```culebra
let {*} = Synth
let plucked = {
  note: |pitch, params| Audio.Sound(render(noise() * perc(0.001, 0.01) |> pluck(hz(pitch), ring: params.ring))),
  knobs: {ring: {default: 0.5, min: 0.1, max: 2.0}},
}
println(render(noise() * perc(0.001, 0.01) |> pluck(220.0, ring: 0.5)).size())  # => 22535
```

曲からの使い方は[Kauaiで音を作る](kauai-sounds.ja.md)にあります。

## 次に読むもの

- [`stdlib.ja.md` §46](../stdlib.ja.md#46-synth)に、すべての関数とメソッドの範囲とエラー、音の終わりの求め方、`Voices`と`Proxy`の規則があります。
- `culebra docs -g 'Synth'`で、同じ節をバイナリから表示できます。
- Studioのサンプルプログラム（`site/playground/studio-samples/`）は、読んで変えるための短い音です。FMのベル、撥弦、エコー、広い和音があります。
