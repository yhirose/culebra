#!/usr/bin/env bash
# The Kauai command line (kauai/cli/main.cul) against `culebra play`: the same
# streams, statuses and WAV bytes for a song that reads, one that does not, a
# part of one, and one that plays a bundled instrument. The program runs as a
# script, and with --aot again as the binary `culebra build` makes of it (which
# takes a minute, the instruments' recordings being embedded, and needs the
# runtime archive a dev build lacks). The device stays closed
# (CULEBRA_AUDIO=off): a WAV needs none.
# Usage: kauai_cli_test.sh <path-to-culebra> [--aot]
set -u
CULEBRA="${1:?usage: kauai_cli_test.sh <culebra-binary> [--aot]}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLI="$HERE/../kauai/cli/main.cul"
export CULEBRA_AUDIO=off
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

check() {  # check <label> <expected> <actual>
  [[ "$3" == "$2" ]] || {
    echo "FAIL $1: expected [$2], got [$3]"; fail=1; }
}
# run <command...>: stdout in $out, stderr in $err, the status in $rc.
run() {
  out=$("$@" 2> "$TMP/err"); rc=$?
  err=$(cat "$TMP/err")
}
# cli <args...>: the program, as the lane under test runs it.
cli() {
  if [[ $lane == aot ]]; then "$TMP/kauai" "$@"; else "$CULEBRA" --vm "$CLI" "$@"; fi
}
play() { "$CULEBRA" play --vm "$@"; }
same_wav() {  # same_wav <label> <wav> <wav>
  cmp -s "$2" "$3" || { echo "FAIL $1: $2 and $3 differ"; fail=1; }
}

lanes=(script)
if [[ "${2:-}" == "--aot" ]]; then
  "$CULEBRA" build "$CLI" -o "$TMP/kauai" > /dev/null || { echo "FAIL aot: culebra build"; exit 1; }
  lanes+=(aot)
fi

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
# A song that does not read.
printf 'band Duo {\n  voice Tune  nosuchwave  plays melody  vol 20\n}\n' > "$TMP/bad.kau"
# One note on a bundled instrument.
cat > "$TMP/alto.kau" <<'EOF'
band Solo {
  voice Lead  external AltoSax  plays melody  vol 40
}
song Tune {
  tempo 120
  band Solo
  melody in C4..B4
  Theme
}
section Theme {
  C  c4 e g c
}
EOF
# The same notes on a bundled recipe.
sed -e 's/external AltoSax/external MellowFmBrass/' "$TMP/alto.kau" > "$TMP/brass.kau"
# An instrument the program does not have.
sed -e 's/external AltoSax/external Organ/' "$TMP/alto.kau" > "$TMP/organ.kau"

for lane in "${lanes[@]}"; do
  # What the two print and leave for a song that reads, one that is not
  # there and one that does not read.
  for song in "$TMP/duo.kau" "$TMP/nope.kau" "$TMP/bad.kau"; do
    run play --check "$song"
    want_out=$out; want_err=$err; want_rc=$rc
    run cli "$song" --check
    check "$lane ${song##*/}: stdout" "$want_out" "$out"
    check "$lane ${song##*/}: stderr" "$want_err" "$err"
    check "$lane ${song##*/}: status" "$want_rc" "$rc"
  done

  run cli
  check "$lane no song: status" 2 "$rc"
  [[ "$err" == "Usage: kauai "* ]] || { echo "FAIL $lane no song: stderr [$err]"; fail=1; }
  run cli --help
  check "$lane --help: status" 0 "$rc"
  [[ "$out" == "Usage: kauai "* ]] || { echo "FAIL $lane --help: stdout [$out]"; fail=1; }
  run cli "$TMP/duo.kau" --wav "$TMP/x.wav" --check
  check "$lane wav and check: status" 2 "$rc"
  run cli "$TMP/duo.kau" --from 1
  check "$lane part, no wav: status" 2 "$rc"
  run cli "$TMP/marked.kau" --wav "$TMP/x.wav" --from Nope
  check "$lane part, no mark: status" 1 "$rc"
  check "$lane part, no mark: stderr" "kauai: $TMP/marked.kau has no mark Nope (it has Again)" "$err"
  # The instruments it has, and `live` refusing what it cannot play (the
  # device is closed, so there is no MIDI input).
  run cli instruments
  check "$lane instruments: status" 0 "$rc"
  [[ "$out" == *$'\n''FmBrass          a recipe; bright 0.5 (0.0 to 1.0)'$'\n'* &&
     "$out" == 'AltoSax          an alto saxophone, from recordings'* ]] || {
    echo "FAIL $lane instruments: stdout [$out]"; fail=1; }
  run cli live AltoSax --midi EWI
  check "$lane live, from recordings: stderr" "kauai: live needs one MIDI input named like EWI; there are none" "$err"
  run cli live Nope
  check "$lane live, no such: status" 2 "$rc"
  check "$lane live, no such: stderr" 'kauai: no instrument `Nope`; `kauai instruments` lists them' "$err"
  run cli live fmbrass --midi EWI
  check "$lane live, no input: status" 1 "$rc"
  check "$lane live, no input: stderr" "kauai: live needs one MIDI input named like EWI; there are none" "$err"
  # An option needs its value.
  run cli live FmBrass --breath
  check "$lane option, no value: status" 2 "$rc"
  check "$lane option, no value: stderr" "kauai: --breath needs a value" "$err"
  run cli "$TMP/duo.kau" --wav
  check "$lane wav, no value: status" 2 "$rc"
  check "$lane wav, no value: stderr" "kauai: --wav needs a value" "$err"
  # An instrument it does not have: the song's line and the ones it has.
  run cli "$TMP/organ.kau" --check
  check "$lane no instrument: status" 1 "$rc"
  [[ "$err" == "ValueError: Audio.Kauai: $TMP/organ.kau:2: voice Lead plays \`external Organ\`, and voices: has no \`organ\` (it has "*"altosax, "*"mellowfmbrass"*")" ]] || {
    echo "FAIL $lane no instrument: stderr [$err]"; fail=1; }

  # The WAV, whole and in part, is the one `culebra play` writes.
  run play --wav "$TMP/play.wav" "$TMP/duo.kau"
  run cli "$TMP/duo.kau" --wav "$TMP/cli.wav"
  check "$lane wav: status" 0 "$rc"
  same_wav "$lane wav" "$TMP/play.wav" "$TMP/cli.wav"
  run play --wav "$TMP/play.wav" --from Again --to 6 "$TMP/marked.kau"
  run cli "$TMP/marked.kau" --wav "$TMP/cli.wav" --from Again --to 6
  check "$lane part: status" 0 "$rc"
  same_wav "$lane part" "$TMP/play.wav" "$TMP/cli.wav"

  # A bundled instrument is built into the program: no --sounds, same bytes.
  run play --sounds "$HERE/../kauai/sounds/alto_sax" --wav "$TMP/play.wav" "$TMP/alto.kau"
  run cli "$TMP/alto.kau" --wav "$TMP/cli.wav"
  check "$lane bundled instrument: status" 0 "$rc"
  same_wav "$lane bundled instrument" "$TMP/play.wav" "$TMP/cli.wav"
  run play --sounds "$HERE/../kauai/recipes" --wav "$TMP/play.wav" "$TMP/brass.kau"
  run cli "$TMP/brass.kau" --wav "$TMP/cli.wav"
  check "$lane bundled recipe: status" 0 "$rc"
  same_wav "$lane bundled recipe" "$TMP/play.wav" "$TMP/cli.wav"
done

exit $fail
