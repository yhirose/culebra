#!/usr/bin/env bash
# kauai/tools/play.cul's process contract: what it prints, on which stream,
# and the status it leaves, for a song that reads, one that does not, one
# whose `external` sounds it has or lacks, and one it watches. The device
# stays closed (CULEBRA_AUDIO=off), so nothing here sounds and the play path
# itself ends at "no audio device"; the .cul sweeps cover what a song plays
# (tests/test_kauai*.cul).
# Usage: kauai_play_test.sh <path-to-culebra>
set -u
CULEBRA="${1:?usage: kauai_play_test.sh <culebra-binary>}"
TOOL="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../kauai/tools/play.cul"
export CULEBRA_AUDIO=off
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

check() {  # check <label> <expected> <actual>
  [[ "$3" == "$2" ]] || {
    echo "FAIL $1: expected [$2], got [$3]"; fail=1; }
}
# run <args...>: stdout in $out, stderr in $err, the status in $rc. A run that
# is not about the instruments kauai bundles names a light module of its own
# (sounds.cul): the bundled ones take most of a second to load.
run() {
  out=$("$CULEBRA" --vm "$TOOL" "$@" 2> "$TMP/err"); rc=$?
  err=$(cat "$TMP/err")
}
# seen <n> <text>: wait until a watched run's output holds <text> <n> times,
# 30 s at most.
seen() {
  for _ in $(seq 300); do
    (( $(grep -c -F -- "$2" "$TMP/watch.out") >= $1 )) && return 0
    sleep 0.1
  done
  echo "FAIL watch: no [$2] x$1 in [$(cat "$TMP/watch.out")]"; fail=1
  kill -INT "$pid" 2>/dev/null
}
# retempo <tempo>: the watched song at another tempo, in place at once, as an
# editor's save does (a rename, so a read never meets half a file).
retempo() {
  sed "s/^  tempo .*/  tempo $1/" "$song" > "$TMP/watch/next.tmp"
  mv "$TMP/watch/next.tmp" "$song"
}

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
  voice Keys  Ep bright=0.8           plays chords from C4  poly  vol 40
  drum Kick   Kick           vol 64
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
let voices = {ep: {note: |pitch, params| sound(), knobs: {bright: {default: 0.5, min: 0.0, max: 2.0}}}}
let drums = {kick: |n, params| sound()}
export {voices, drums}
EOF
cat > "$TMP/voices_only.cul" <<'EOF'
let voices = {ep: {note: |pitch, params| nil, knobs: {bright: {default: 0.5, min: 0.0, max: 2.0}}}}
export {voices}
EOF
# An instrument that brings files of its own: a folder, its main.cul the
# module, which reads what lies beside it (here, before it makes a sound).
mkdir -p "$TMP/instrument/data"
printf 'tines\n' > "$TMP/instrument/kind.txt"
printf '{"bright": 0}\n' > "$TMP/instrument/data/style.json"
cat > "$TMP/instrument/main.cul" <<'EOF'
let kind = Dir.embedded('.').read('kind.txt')
let sound = || Audio.Sound(String.from_bytes([]))
let voices = {ep: {note: |pitch, params| kind == "tines\n" ? sound() : nil, knobs: {bright: {default: 0.5, min: 0.0, max: 2.0}}}}
let drums = {kick: |n, params| sound()}
export {voices, drums}
EOF


# --check reads the song and says how long it is, on stdout.
run --check "$TMP/duo.kau"
check "check: status" 0 "$rc"
check "check: stdout" "$TMP/duo.kau: 0:04 once through" "$out"
check "check: stderr" "" "$err"

# Without --check and without a device, it says so and plays nothing.
run --sounds "$TMP/sounds.cul" "$TMP/duo.kau"
check "no device: status" 1 "$rc"
check "no device: stdout" "" "$out"
check "no device: stderr" "$TMP/duo.kau: no audio device, so nothing plays" "$err"

# --wav writes the song as it would sound, and needs no device: four seconds
# of 16-bit mono at 44100 a second, and a little more.
rm -f "$TMP/duo.wav"
run --sounds "$TMP/sounds.cul" --wav "$TMP/duo.wav" "$TMP/duo.kau"
check "wav: status" 0 "$rc"
check "wav: stdout" "$TMP/duo.kau: 0:04 written to $TMP/duo.wav" "$out"
check "wav: stderr" "" "$err"
check "wav: format" "RIFF" "$(head -c 4 "$TMP/duo.wav")"
size=$(wc -c < "$TMP/duo.wav" | tr -d ' ')
(( size >= 44 + 2 * 4 * 44100 && size < 44 + 2 * 5 * 44100 )) || {
  echo "FAIL wav: size $size"; fail=1; }
run --sounds "$TMP/sounds.cul" --wav="$TMP/duo2.wav" "$TMP/duo.kau"
check "wav=: same bytes" "$(od -An -tx1 "$TMP/duo.wav" | md5sum)" "$(od -An -tx1 "$TMP/duo2.wav" | md5sum)"

# --from and --to write a part, from a second before --from.
run --sounds "$TMP/sounds.cul" --wav "$TMP/part.wav" --from Again --to 6 "$TMP/marked.kau"
check "part: status" 0 "$rc"
check "part: stdout" "$TMP/marked.kau: 0:03 written to $TMP/part.wav" "$out"
check "part: stderr" "" "$err"

# A song's mistake names its file and line, on stderr.
sed 's/tempo 120/tempo fast/' "$TMP/duo.kau" > "$TMP/bad.kau"
run --sounds "$TMP/sounds.cul" --check "$TMP/bad.kau"
check "mistake: status" 1 "$rc"
check "mistake: stdout" "" "$out"
[[ "$err" == "KauaiError: "*"bad.kau:9:"* ]] || {
  echo "FAIL mistake: stderr [$err]"; fail=1; }

run --sounds "$TMP/sounds.cul" --check "$TMP/absent.kau"
check "absent: status" 1 "$rc"
check "absent: stderr" "IOError: Kauai: no song file $TMP/absent.kau" "$err"

# An external sound the bundled instruments lack: the song's error, then where
# sounds come from.
run --check "$TMP/external.kau"
check "external, no sounds: status" 1 "$rc"
[[ "$err" == "ValueError: Kauai: "*'plays Ep, which the mixer has not'*$'\n'"play: --sounds "* ]] || {
  echo "FAIL external, no sounds: stderr [$err]"; fail=1; }

run --check --sounds "$TMP/sounds.cul" "$TMP/external.kau"
check "external, sounds: status" 0 "$rc"
check "external, sounds: stdout" "$TMP/external.kau: 0:04 once through" "$out"
check "external, sounds: stderr" "" "$err"
run --check --sounds="$TMP/sounds.cul" "$TMP/external.kau"
check "external, --sounds=: status" 0 "$rc"
# A folder names its main.cul, which finds its own files from wherever the
# player was started.
run --check --sounds "$TMP/instrument" "$TMP/external.kau"
check "external, a folder: status" 0 "$rc"
check "external, a folder: stdout" "$TMP/external.kau: 0:04 once through" "$out"
check "external, a folder: stderr" "" "$err"

# A module that exports one of the two leaves the other empty, and the hint
# is not repeated to someone who did name a module.
run --check --sounds "$TMP/voices_only.cul" "$TMP/external.kau"
check "external, voices only: status" 1 "$rc"
check "external, voices only: stderr" \
  "ValueError: Kauai: $TMP/external.kau:4: drum Kick plays Kick, which the mixer has not, and drums: has no \`kick\` (it has none)" "$err"

# --watch reads the song again whenever a file in its folder changes, says
# which, and watches on through a mistake until Ctrl+C.
mkdir -p "$TMP/watch"
cp "$TMP/duo.kau" "$TMP/watch/duo.kau"
song="$TMP/watch/duo.kau"
"$CULEBRA" --vm "$TOOL" --check --watch --sounds "$TMP/instrument" "$song" > "$TMP/watch.out" 2>&1 &
pid=$!
seen 1 "$song: 0:04 once through"
retempo 60
seen 1 "$song: 0:08 once through"
retempo fast
seen 1 "KauaiError: $song:9: "
retempo 120
seen 2 "$song: 0:04 once through"
# A .json file in a folder of the module's: its data, a recipe's drums/.
printf '{"bright": 1}\n' > "$TMP/watch/next.tmp"
mv "$TMP/watch/next.tmp" "$TMP/instrument/data/style.json"
seen 1 "$TMP/instrument/data/style.json changed"
seen 3 "$song: 0:04 once through"
kill -INT "$pid"
wait "$pid"; rc=$?
check "watch: status" 130 "$rc"
check "watch: changes named" 3 "$(grep -c -F "$song changed" "$TMP/watch.out")"
check "watch: last line" "interrupted" "$(tail -n 1 "$TMP/watch.out")"

# A mark's name, seconds, or minutes and seconds name the same point; a
# part that is not in the song is the song's mistake, status 1.
run --sounds "$TMP/sounds.cul" --wav "$TMP/part2.wav" --from=0:04 --to=6.0 "$TMP/marked.kau"
check "part, 0:04: same bytes" "$(od -An -tx1 "$TMP/part.wav" | md5sum)" "$(od -An -tx1 "$TMP/part2.wav" | md5sum)"
run --sounds "$TMP/sounds.cul" --wav "$TMP/part.wav" --from Twice "$TMP/marked.kau"
check "part, no mark: status" 1 "$rc"
check "part, no mark: stderr" "play: $TMP/marked.kau has no mark Twice (it has Again)" "$err"
run --sounds "$TMP/sounds.cul" --wav "$TMP/part.wav" --from 8 "$TMP/marked.kau"
check "part, past the end: stderr" "play: --from 8 is past the song's end (0:08)" "$err"
run --sounds "$TMP/sounds.cul" --wav "$TMP/part.wav" --from 0:05 --to Again "$TMP/marked.kau"
check "part, backwards: stderr" "play: --to Again is not after --from 0:05" "$err"

# The command line's own mistakes are status 2 and reach no engine.
run
check "no song: status" 2 "$rc"
[[ "$err" == "Usage: culebra kauai/tools/play.cul "* ]] || { echo "FAIL no song: stderr [$err]"; fail=1; }
run --check
check "only options: status" 2 "$rc"
check "only options: stderr" "play: missing required argument 'song'" "$err"
run "$TMP/duo.kau" "$TMP/external.kau"
check "two songs: status" 2 "$rc"
check "two songs: stderr" "play: unexpected positional argument '$TMP/external.kau'" "$err"
run --loud "$TMP/duo.kau"
check "unknown option: status" 2 "$rc"
check "unknown option: stderr" "play: unknown option '--loud'" "$err"
run "$TMP/duo.kau" --wav
check "wav, no file: status" 2 "$rc"
check "wav, no file: stderr" "play: option '--wav' expects a value" "$err"
run "$TMP/duo.kau" --wav=
check "wav=, no file: status" 2 "$rc"
check "wav=, no file: stderr" "play: option '--wav' expects a value" "$err"
run --wav "$TMP/x.wav" --check "$TMP/duo.kau"
check "wav and check: status" 2 "$rc"
check "wav and check: stderr" "play: --wav writes the song and plays nothing, so not with --check" "$err"
run --to 2 "$TMP/duo.kau"
check "part, no wav: status" 2 "$rc"
check "part, no wav: stderr" "play: --to chooses the part --wav writes, so it needs --wav" "$err"
run --wav "$TMP/x.wav" --from 1:5 "$TMP/duo.kau"
check "part, 1:5: status" 2 "$rc"
check "part, 1:5: stderr" "play: --from takes seconds (83.5), minutes and seconds (1:23.5) or a mark's name, not '1:5'" "$err"
run --wav "$TMP/x.wav" "$TMP/duo.kau" --to
check "part, no point: stderr" "play: option '--to' expects a value" "$err"
run "$TMP/duo.kau" --sounds
check "sounds, no file: status" 2 "$rc"
check "sounds, no file: stderr" "play: option '--sounds' expects a value" "$err"
run --sounds "$TMP/absent.cul" "$TMP/duo.kau"
check "absent sounds: status" 2 "$rc"
check "absent sounds: stderr" "play: no sounds file '$TMP/absent.cul'" "$err"
run --help
check "help: status" 0 "$rc"
[[ "$out" == "Usage: culebra kauai/tools/play.cul "* ]] || { echo "FAIL help: stdout [$out]"; fail=1; }

if [[ $fail -eq 0 ]]; then echo "kauai_play_test OK"; exit 0; fi
echo "kauai_play_test FAILED"; exit 1
