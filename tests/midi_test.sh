#!/usr/bin/env bash
# MIDI input end to end, on each engine: a virtual CoreMIDI source
# (tests/midi_sender.cc) sends a fixed run of messages, and a script reads
# them through MIDI.Input as the kinds and values culebra gives them. macOS
# only, where a program can make a source of its own; elsewhere it skips
# (MIDI.inputs() is [] with no device, which tests/test_midi.cul covers).
# Usage: midi_test.sh <path-to-culebra>
set -u
CULEBRA="${1:?usage: midi_test.sh <culebra-binary>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ "$(uname -s)" != Darwin ]]; then
  echo "midi_test: skipped (a virtual MIDI source needs macOS)"
  exit 0
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

c++ -std=c++17 -o "$TMP/sender" "$HERE/midi_sender.cc" -framework CoreMIDI -framework CoreFoundation || {
  echo "FAIL: cannot build the sender"; exit 1; }

cat > "$TMP/read.cul" <<'EOF'
# Open the test source once it is listed, say so on the FIFO, and print the
# messages it sends, rounded.
let fifo = Sys.argv[0]
mut port = nil
let until = Time.monotonic() + 5.0
while port == nil && Time.monotonic() < until {
  port = MIDI.inputs().find(|p| p.name == "culebra midi test")
  Time.sleep(0.01) if port == nil
}
let input = MIDI.Input.new(port.id)
FS.write(fifo, "ready\n")
let r3 = |x| Math.round(x * 1000) / 1000.0
mut seen = 0
while seen < 10 && input.wait(5.0) {
  for m in input.read() {
    seen += 1
    cond {
      m.kind == "note_on" || m.kind == "note_off" => println("{m.kind} {m.note} {r3(m.velocity)} {m.bytes}"),
      m.kind == "cc" => println("cc {m.cc} {r3(m.value)}"),
      m.kind == "bend" => println("bend {r3(m.bend)}"),
      m.kind == "pressure" => println("pressure {r3(m.value)}"),
      m.kind == "program" => println("program {m.program}"),
      _ => println("other {m}"),
    }
    assert_true(m.at <= Time.monotonic() && m.at > Time.monotonic() - 5.0)
  }
}
println("breath {r3(input.control(2))}, dropped {input.dropped()}")
input.drop()
# Closed, it neither waits nor reads.
let after = try { input.wait(0.1); "waited" } catch e { e.kind }
println("after drop: {after}")
EOF

want='cc 2 0.504
cc 34 0.126
cc 88 0.268
note_on 60 0.627 [144, 60, 80]
note_on 62 0.703 [144, 62, 90]
note_off 60 0.0 [128, 60, 0]
bend 1.0
pressure 0.504
program 5
note_off 62 0.0 [144, 62, 0]
breath 0.501, dropped 0
after drop: TypeError'

for engine in --vm --jit; do
  mkfifo "$TMP/ready"
  "$TMP/sender" "$TMP/ready" &
  sender=$!
  got=$(CULEBRA_AUDIO=on "$CULEBRA" "$engine" "$TMP/read.cul" "$TMP/ready" 2>&1)
  # A reader that failed before it opened the FIFO leaves the sender waiting.
  kill $sender 2>/dev/null
  wait $sender 2>/dev/null
  rm -f "$TMP/ready"
  [[ "$got" == "$want" ]] || { echo "FAIL $engine: got [$got]"; fail=1; }
done

[[ $fail == 0 ]] && echo "midi_test OK"
exit $fail
