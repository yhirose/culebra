#!/usr/bin/env bash
# MIDI end to end, on each engine: a virtual CoreMIDI source and destination
# (tests/midi_sender.cc). The source sends a fixed run of messages, a sysex
# among them, and a script reads them through MIDI.Input as the kinds and
# values culebra gives them; the script then sends to the destination through
# MIDI.Output, which prints what came. macOS only, where a program can make
# ports of its own; elsewhere it skips (MIDI.inputs() and MIDI.outputs() are
# [] with no device, which tests/test_midi.cul covers).
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
# messages it sends, rounded; then send to the test destination.
let fifo = Sys.argv[0]
mut port = nil
let until = Time.monotonic() + 5.0
while port == nil && Time.monotonic() < until {
  port = MIDI.inputs().find(|p| p.name == "culebra midi test")
  Time.sleep(0.01) if port == nil
}
let input = MIDI.Input.new(port.id, sysex: true)
FS.write(fifo, "ready\n")
let r3 = |x| Math.round(x * 1000) / 1000.0
mut seen = 0
while seen < 11 && input.wait(5.0) {
  # What has come and is not yet read needs no waiting for.
  assert_true(input.wait(0))
  # The run comes all at once: once it has all come, each came before it
  # is read.
  Time.sleep(0.05)
  let before = Time.monotonic()
  for m in input.read() {
    seen += 1
    cond {
      m.kind == "note_on" || m.kind == "note_off" => println("{m.kind} {m.note} {r3(m.velocity)} {m.bytes}"),
      m.kind == "cc" => println("cc {m.cc} {r3(m.value)}"),
      m.kind == "bend" => println("bend {r3(m.bend)}"),
      m.kind == "pressure" => println("pressure {r3(m.value)}"),
      m.kind == "program" => println("program {m.program}"),
      m.kind == "sysex" => println("sysex {m.bytes}"),
      _ => println("other {m}"),
    }
    assert_true(m.at <= before && m.at > before - 5.0)
  }
}
println("breath {r3(input.control(2))}, dropped {input.dropped()}")
input.drop()
# Closed, it neither waits nor reads.
let after = try { input.wait(0.1); "waited" } catch e { e.kind }
println("after drop: {after}")

let refused = |f| try { f(); "sent" } catch e { "{e.kind}: {e.message}" }
let dest = MIDI.outputs().find(|p| p.name == "culebra midi test")
let no_output = refused(|| MIDI.Output.new(port.id))
println(no_output.starts_with("ValueError: MIDI.Output: no output {port.id}; there are ") && no_output.contains("{dest.id} (culebra midi test)"))
let out = MIDI.Output.new(dest.id)
out.send([0x90, 61, 100, 0xF0, 0x7D, 1, 2, 3, 4, 5, 6, 7, 0xF7, 0xC0, 3])
println(refused(|| out.send([0x90, 61])))
println(refused(|| out.send([0x90, 61, 300])))
println(refused(|| out.send([0x90, 61, true])))
out.send([0x90, 0, 1])
out.drop()
println("send after drop: {refused(|| out.send([0x90, 1, 1])).split(":")[0]}")
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
sysex [240, 71, 127, 109, 66, 0, 0, 0, 7, 247]
note_off 62 0.0 [144, 62, 0]
breath 0.501, dropped 0
after drop: TypeError
true
ValueError: MIDI.Output.send: the message at byte 0 is cut short
TypeError: MIDI.Output.send: bytes are Longs from 0 to 255
TypeError: MIDI.Output.send: bytes are Longs from 0 to 255
send after drop: TypeError'
want_sent='90 3D 64 F0 7D 01 02 03 04 05 06 07 F7 C0 03 90 00 01'

for engine in --vm --jit; do
  mkfifo "$TMP/ready"
  "$TMP/sender" "$TMP/ready" > "$TMP/sent" &
  sender=$!
  got=$(CULEBRA_AUDIO=on "$CULEBRA" "$engine" "$TMP/read.cul" "$TMP/ready" 2>&1)
  # The sender ends within 3 s of the reader saying it is ready; a reader
  # that failed before that leaves it waiting on the FIFO.
  for _ in $(seq 40); do kill -0 $sender 2>/dev/null || break; sleep 0.1; done
  kill $sender 2>/dev/null
  wait $sender 2>/dev/null
  rm -f "$TMP/ready"
  sent=$(cat "$TMP/sent")
  [[ "$got" == "$want" ]] || { echo "FAIL $engine: got [$got]"; fail=1; }
  [[ "$sent" == "$want_sent" ]] || { echo "FAIL $engine: the destination got [$sent]"; fail=1; }
done

# One Ctrl+C stops a program waiting for a message that does not come (the
# second press kills it, and then neither the defer nor "interrupted" prints).
cat > "$TMP/wait.cul" <<'EOF'
defer { IO.eprint("DEFER\n") }
mut port = nil
let until = Time.monotonic() + 5.0
while port == nil && Time.monotonic() < until {
  port = MIDI.inputs().find(|p| p.name == "culebra midi test")
  Time.sleep(0.01) if port == nil
}
let input = MIDI.Input.new(port.id)
FS.write(Sys.argv[0], "ready\n")
IO.eprint("waiting\n")
while true {
  input.wait()
  input.read()
}
EOF

for engine in --vm --jit; do
  mkfifo "$TMP/ready"
  "$TMP/sender" "$TMP/ready" > /dev/null &
  sender=$!
  CULEBRA_AUDIO=on "$CULEBRA" "$engine" "$TMP/wait.cul" "$TMP/ready" > "$TMP/waited" 2>&1 &
  pid=$!
  for _ in $(seq 50); do grep -q waiting "$TMP/waited" && break; sleep 0.1; done
  sleep 0.3
  kill -INT $pid
  for _ in $(seq 20); do kill -0 $pid 2>/dev/null || break; sleep 0.1; done
  kill -9 $pid 2>/dev/null
  wait $pid 2>/dev/null
  kill $sender 2>/dev/null
  wait $sender 2>/dev/null
  rm -f "$TMP/ready"
  waited=$(tr '\n' '|' < "$TMP/waited")
  [[ "$waited" == 'waiting|DEFER|interrupted|' ]] || { echo "FAIL $engine: one Ctrl+C left [$waited]"; fail=1; }
done

[[ $fail == 0 ]] && echo "midi_test OK"
exit $fail
