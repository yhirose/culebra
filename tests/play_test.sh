#!/usr/bin/env bash
# `culebra play`'s process contract: what it prints, on which stream, and the
# status it leaves, for a song that reads, one that does not, and one whose
# `external` sounds it has or lacks. The device stays closed
# (CULEBRA_AUDIO=off), so nothing here sounds and the play path itself ends
# at "no audio device"; the .cul sweeps cover what a song plays
# (tests/test_audio_kauai*.cul).
# Usage: play_test.sh <path-to-culebra>
set -u
CULEBRA="${1:?usage: play_test.sh <culebra-binary>}"
export CULEBRA_AUDIO=off
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

check() {  # check <label> <expected> <actual>
  [[ "$3" == "$2" ]] || {
    echo "FAIL $1: expected [$2], got [$3]"; fail=1; }
}
# run <args...>: stdout in $out, stderr in $err, the status in $rc.
run() {
  out=$("$CULEBRA" play "$ENGINE" "$@" 2> "$TMP/err"); rc=$?
  err=$(cat "$TMP/err")
}
# seen <n> <text>: wait until a watched run's output holds <text> <n> times,
# 30 s at most (a --jit run compiles the program again each time).
seen() {
  for _ in $(seq 300); do
    (( $(grep -c -F -- "$2" "$TMP/watch.out") >= $1 )) && return 0
    sleep 0.1
  done
  echo "FAIL $ENGINE watch: no [$2] x$1 in [$(cat "$TMP/watch.out")]"; fail=1
  kill -INT "$pid" 2>/dev/null
}
# retempo <tempo>: the watched song at another tempo, in place at once, as an
# editor's save does (a rename, so a read never meets half a file).
retempo() {
  sed "s/^  tempo .*/  tempo $1/" "$song" > "$TMP/watch/next.tmp"
  mv "$TMP/watch/next.tmp" "$song"
}
# The player is a program, so it answers the same on every engine this build
# has.
ENGINES="--vm"
[[ "$("$CULEBRA" --version)" == *"vm+jit"* ]] && ENGINES="--vm --jit"

# Two bars at tempo 120: four seconds.
cat > "$TMP/duo.kau" <<'EOF'
band Duo {
  voice Tune  pulse     plays melody            vol 20
  voice Bass  triangle  plays roots in E2..D#3  vol 30
}
groove Walk {
  Bass  1 - 5 -
}
song First {
  tempo 120
  band Duo
  groove Walk
  melody in C5..B5
  Verse
}
section Verse {
  C     c4 e g e
  G     d4 g b g
}
EOF
# The same two bars twice, with a mark between: eight seconds.
awk '{ print } /^  Verse$/ { print "  mark Again"; print "  Verse" }' "$TMP/duo.kau" > "$TMP/marked.kau"
# A band with an `external` voice and an `external` drum.
cat > "$TMP/external.kau" <<'EOF'
band Duo {
  voice Tune  pulse     plays melody            vol 20
  voice Keys  external Ep bright=0.8  plays chords from C4  poly  vol 40
  drum Kick   external Kick  vol 64
}
groove Walk {
  Keys  x - x -
  Kick  x... x...
}
song First {
  tempo 120
  band Duo
  groove Walk
  melody in C5..B5
  Verse
}
section Verse {
  C     c4 e g e
  G     d4 g b g
}
EOF
cat > "$TMP/sounds.cul" <<'EOF'
let sound = || Audio.Sound(String.from_bytes([]))
let voices = {ep: |pitch, params| sound()}
let drums = {kick: |params| sound()}
export {voices, drums}
EOF
cat > "$TMP/voices_only.cul" <<'EOF'
let voices = {ep: |pitch, params| nil}
export {voices}
EOF
# An instrument that brings files of its own: a folder, its main.cul the
# module, which reads what lies beside it (here, before it makes a sound).
mkdir -p "$TMP/instrument"
printf 'tines\n' > "$TMP/instrument/kind.txt"
cat > "$TMP/instrument/main.cul" <<'EOF'
let kind = Dir.embedded('.').read('kind.txt')
let sound = || Audio.Sound(String.from_bytes([]))
let voices = {ep: |pitch, params| kind == "tines\n" ? sound() : nil}
let drums = {kick: |params| sound()}
export {voices, drums}
EOF

for ENGINE in $ENGINES; do

# --check reads the song and says how long it is, on stdout.
run --check "$TMP/duo.kau"
check "$ENGINE check: status" 0 "$rc"
check "$ENGINE check: stdout" "$TMP/duo.kau: 0:04 once through" "$out"
check "$ENGINE check: stderr" "" "$err"

# Without --check and without a device, it says so and plays nothing.
run "$TMP/duo.kau"
check "$ENGINE no device: status" 1 "$rc"
check "$ENGINE no device: stdout" "" "$out"
check "$ENGINE no device: stderr" "$TMP/duo.kau: no audio device, so nothing plays" "$err"

# --wav writes the song as it would sound, and needs no device: four seconds
# of 16-bit mono at 44100 a second, and a little more.
rm -f "$TMP/duo.wav"
run --wav "$TMP/duo.wav" "$TMP/duo.kau"
check "$ENGINE wav: status" 0 "$rc"
check "$ENGINE wav: stdout" "$TMP/duo.kau: 0:04 written to $TMP/duo.wav" "$out"
check "$ENGINE wav: stderr" "" "$err"
check "$ENGINE wav: format" "RIFF" "$(head -c 4 "$TMP/duo.wav")"
size=$(wc -c < "$TMP/duo.wav" | tr -d ' ')
(( size >= 44 + 2 * 4 * 44100 && size < 44 + 2 * 5 * 44100 )) || {
  echo "FAIL $ENGINE wav: size $size"; fail=1; }
run --wav="$TMP/duo2.wav" "$TMP/duo.kau"
check "$ENGINE wav=: same bytes" "$(od -An -tx1 "$TMP/duo.wav" | md5sum)" "$(od -An -tx1 "$TMP/duo2.wav" | md5sum)"

# --from and --to write a part, from a second before --from.
run --wav "$TMP/part.wav" --from Again --to 6 "$TMP/marked.kau"
check "$ENGINE part: status" 0 "$rc"
check "$ENGINE part: stdout" "$TMP/marked.kau: 0:03 written to $TMP/part.wav" "$out"
check "$ENGINE part: stderr" "" "$err"

# A song's mistake names its file and line, on stderr.
sed 's/tempo 120/tempo fast/' "$TMP/duo.kau" > "$TMP/bad.kau"
run --check "$TMP/bad.kau"
check "$ENGINE mistake: status" 1 "$rc"
check "$ENGINE mistake: stdout" "" "$out"
[[ "$err" == "KauaiError: "*"bad.kau:9:"* ]] || {
  echo "FAIL $ENGINE mistake: stderr [$err]"; fail=1; }

run --check "$TMP/absent.kau"
check "$ENGINE absent: status" 1 "$rc"
check "$ENGINE absent: stderr" "IOError: Audio.Kauai: no song file $TMP/absent.kau" "$err"

# An external sound nobody supplies: the song's error, then where sounds come
# from.
run --check "$TMP/external.kau"
check "$ENGINE external, no sounds: status" 1 "$rc"
[[ "$err" == "ValueError: Audio.Kauai: "*'plays `external '*$'\n'"culebra play: --sounds "* ]] || {
  echo "FAIL $ENGINE external, no sounds: stderr [$err]"; fail=1; }

run --check --sounds "$TMP/sounds.cul" "$TMP/external.kau"
check "$ENGINE external, sounds: status" 0 "$rc"
check "$ENGINE external, sounds: stdout" "$TMP/external.kau: 0:04 once through" "$out"
check "$ENGINE external, sounds: stderr" "" "$err"
run --check --sounds="$TMP/sounds.cul" "$TMP/external.kau"
check "$ENGINE external, --sounds=: status" 0 "$rc"
# A folder names its main.cul, which finds its own files from wherever the
# player was started.
run --check --sounds "$TMP/instrument" "$TMP/external.kau"
check "$ENGINE external, a folder: status" 0 "$rc"
check "$ENGINE external, a folder: stdout" "$TMP/external.kau: 0:04 once through" "$out"
check "$ENGINE external, a folder: stderr" "" "$err"

# A module that exports one of the two leaves the other empty, and the hint
# is not repeated to someone who did name a module.
run --check --sounds "$TMP/voices_only.cul" "$TMP/external.kau"
check "$ENGINE external, voices only: status" 1 "$rc"
check "$ENGINE external, voices only: stderr" \
  'ValueError: Audio.Kauai: drum Kick plays `external Kick`, and drums: has no `kick`' "$err"

# --watch reads the song again whenever a file in its folder changes, says
# which, and watches on through a mistake until Ctrl+C.
mkdir -p "$TMP/watch"
cp "$TMP/duo.kau" "$TMP/watch/duo.kau"
song="$TMP/watch/duo.kau"
"$CULEBRA" play "$ENGINE" --check --watch "$song" > "$TMP/watch.out" 2>&1 &
pid=$!
seen 1 "$song: 0:04 once through"
retempo 60
seen 1 "$song: 0:08 once through"
retempo fast
seen 1 "KauaiError: $song:9: "
retempo 120
seen 2 "$song: 0:04 once through"
kill -INT "$pid"
wait "$pid"; rc=$?
check "$ENGINE watch: status" 130 "$rc"
check "$ENGINE watch: changes named" 3 "$(grep -c -F "$song changed" "$TMP/watch.out")"
check "$ENGINE watch: last line" "interrupted" "$(tail -n 1 "$TMP/watch.out")"

done

# A mark's name, seconds, or minutes and seconds name the same point; a
# part that is not in the song is the song's mistake, status 1. What reads
# them is the same program on every engine, so one runs it.
ENGINE=--vm
run --wav "$TMP/part2.wav" --from=0:04 --to=6.0 "$TMP/marked.kau"
check "part, 0:04: same bytes" "$(od -An -tx1 "$TMP/part.wav" | md5sum)" "$(od -An -tx1 "$TMP/part2.wav" | md5sum)"
run --wav "$TMP/part.wav" --from Twice "$TMP/marked.kau"
check "part, no mark: status" 1 "$rc"
check "part, no mark: stderr" "culebra play: $TMP/marked.kau has no mark Twice (it has Again)" "$err"
run --wav "$TMP/part.wav" --from 8 "$TMP/marked.kau"
check "part, past the end: stderr" "culebra play: --from 8 is past the song's end (0:08)" "$err"
run --wav "$TMP/part.wav" --from 0:05 --to Again "$TMP/marked.kau"
check "part, backwards: stderr" "culebra play: --to Again is not after --from 0:05" "$err"

# The command line's own mistakes are status 2 and reach no engine.
run
check "no song: status" 2 "$rc"
[[ "$err" == "Usage: culebra play "* ]] || { echo "FAIL no song: stderr [$err]"; fail=1; }
run "$TMP/duo.kau" "$TMP/external.kau"
check "two songs: status" 2 "$rc"
run --loud "$TMP/duo.kau"
check "unknown option: status" 2 "$rc"
check "unknown option: stderr" "culebra play: unknown option '--loud'" "$err"
run "$TMP/duo.kau" --wav
check "wav, no file: status" 2 "$rc"
check "wav, no file: stderr" "culebra play: --wav needs a file to write" "$err"
run --wav "$TMP/x.wav" --check "$TMP/duo.kau"
check "wav and check: status" 2 "$rc"
check "wav and check: stderr" "culebra play: --wav writes the song and plays nothing, so not with --check" "$err"
run --to 2 "$TMP/duo.kau"
check "part, no wav: status" 2 "$rc"
check "part, no wav: stderr" "culebra play: --to chooses the part --wav writes, so it needs --wav" "$err"
run --wav "$TMP/x.wav" --from 1:5 "$TMP/duo.kau"
check "part, 1:5: status" 2 "$rc"
check "part, 1:5: stderr" "culebra play: --from takes seconds (83.5), minutes and seconds (1:23.5) or a mark's name, not '1:5'" "$err"
run --wav "$TMP/x.wav" "$TMP/duo.kau" --to
check "part, no point: stderr" "culebra play: --to takes seconds (83.5), minutes and seconds (1:23.5) or a mark's name" "$err"
run "$TMP/duo.kau" --sounds
check "sounds, no file: status" 2 "$rc"
check "sounds, no file: stderr" "culebra play: --sounds needs a file or a folder" "$err"
run --sounds "$TMP/absent.cul" "$TMP/duo.kau"
check "absent sounds: status" 2 "$rc"
check "absent sounds: stderr" "culebra play: no sounds file '$TMP/absent.cul'" "$err"
run --help
check "help: status" 0 "$rc"
[[ "$out" == "Usage: culebra play "* ]] || { echo "FAIL help: stdout [$out]"; fail=1; }

if [[ $fail -eq 0 ]]; then echo "play_test OK"; exit 0; fi
echo "play_test FAILED"; exit 1
