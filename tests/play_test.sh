#!/usr/bin/env bash
# `culebra play`'s process contract: what it prints, on which stream, and the
# status it leaves, for a song that reads, one that does not, and one whose
# `host` sounds it has or lacks. The device stays closed (CULEBRA_AUDIO=off),
# so nothing here sounds and the play path itself ends at "no audio device";
# the .cul sweeps cover what a song plays (tests/test_audio_kauai*.cul).
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
# A band with a `host` voice and a `host` drum.
cat > "$TMP/host.kau" <<'EOF'
band Duo {
  voice Tune  pulse     plays melody            vol 20
  voice Keys  host Ep bright=0.8  plays chords from C4  poly  vol 40%
  drum Kick   host Kick  vol 80%
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

# A host sound nobody supplies: the song's error, then where sounds come from.
run --check "$TMP/host.kau"
check "$ENGINE host, no sounds: status" 1 "$rc"
[[ "$err" == "ValueError: Audio.Kauai: "*'plays `host '*$'\n'"culebra play: --sounds "* ]] || {
  echo "FAIL $ENGINE host, no sounds: stderr [$err]"; fail=1; }

run --check --sounds "$TMP/sounds.cul" "$TMP/host.kau"
check "$ENGINE host, sounds: status" 0 "$rc"
check "$ENGINE host, sounds: stdout" "$TMP/host.kau: 0:04 once through" "$out"
check "$ENGINE host, sounds: stderr" "" "$err"
run --check --sounds="$TMP/sounds.cul" "$TMP/host.kau"
check "$ENGINE host, --sounds=: status" 0 "$rc"

# A module that exports one of the two leaves the other empty, and the hint
# is not repeated to someone who did name a module.
run --check --sounds "$TMP/voices_only.cul" "$TMP/host.kau"
check "$ENGINE host, voices only: status" 1 "$rc"
check "$ENGINE host, voices only: stderr" \
  'ValueError: Audio.Kauai: drum Kick plays `host Kick`, and drums: has no `kick`' "$err"

done

# The command line's own mistakes are status 2 and reach no engine.
ENGINE=--vm
run
check "no song: status" 2 "$rc"
[[ "$err" == "Usage: culebra play "* ]] || { echo "FAIL no song: stderr [$err]"; fail=1; }
run "$TMP/duo.kau" "$TMP/host.kau"
check "two songs: status" 2 "$rc"
run --loud "$TMP/duo.kau"
check "unknown option: status" 2 "$rc"
check "unknown option: stderr" "culebra play: unknown option '--loud'" "$err"
run "$TMP/duo.kau" --sounds
check "sounds, no file: status" 2 "$rc"
check "sounds, no file: stderr" "culebra play: --sounds needs a file" "$err"
run --sounds "$TMP/absent.cul" "$TMP/duo.kau"
check "absent sounds: status" 2 "$rc"
check "absent sounds: stderr" "culebra play: no sounds file '$TMP/absent.cul'" "$err"
run --help
check "help: status" 0 "$rc"
[[ "$out" == "Usage: culebra play "* ]] || { echo "FAIL help: stdout [$out]"; fail=1; }

if [[ $fail -eq 0 ]]; then echo "play_test OK"; exit 0; fi
echo "play_test FAILED"; exit 1
