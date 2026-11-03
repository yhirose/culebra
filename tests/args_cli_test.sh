#!/usr/bin/env bash
# Regression test for `Args.parse` (src/preambles/args.cul), whose contract is
# a process one: help on stdout + exit 0, `error: <message>` on stderr + exit 2
# (docs/stdlib.md §Args.parse). test_args.cul can only reach `try_parse` — it
# raises instead of exiting — so the streams, the exit codes and the raw (never
# quoted) formatting are only observable from a subprocess.
# Usage: args_cli_test.sh <path-to-culebra>
set -u
CULEBRA="${1:?usage: args_cli_test.sh <culebra-binary>}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

cat > "$TMP/greet.cul" <<'EOF'
let spec = {
  name: "greet",
  doc: "say hello",
  args: [
    {name: "who", doc: "who to greet"},
    {name: "loud", short: "l", type: "Bool", doc: "shout it"}
  ]
}
let a = Args.parse(Sys.argv, spec)
IO.println("who={a.who} loud={a.loud}")
EOF

# --help: stdout, exit 0, and the text itself — not an inspect() rendering of
# it, which wrapped the whole help in quotes.
out=$("$CULEBRA" --vm "$TMP/greet.cul" --help 2>"$TMP/err"); rc=$?
[[ $rc -eq 0 ]] || { echo "FAIL help: expected exit 0, got $rc"; fail=1; }
[[ -s "$TMP/err" ]] && { echo "FAIL help: wrote to stderr: $(cat "$TMP/err")"; fail=1; }
[[ "$out" == "greet - say hello"* ]] || { echo "FAIL help: unexpected first line: $out"; fail=1; }
[[ "$out" == *"'"* ]] && { echo "FAIL help: output is quoted: $out"; fail=1; }

# A parse error: stderr, exit 2, `error: <message>` unquoted.
out=$("$CULEBRA" --vm "$TMP/greet.cul" 2>"$TMP/err"); rc=$?
err=$(cat "$TMP/err")
[[ $rc -eq 2 ]] || { echo "FAIL error: expected exit 2, got $rc"; fail=1; }
[[ -z "$out" ]] || { echo "FAIL error: wrote to stdout: $out"; fail=1; }
[[ "$err" == "error: missing required argument 'who'" ]] || {
  echo "FAIL error: unexpected stderr: $err"; fail=1; }

# The happy path still parses.
out=$("$CULEBRA" --vm "$TMP/greet.cul" world -l 2>&1); rc=$?
[[ $rc -eq 0 && "$out" == "who=world loud=true" ]] || {
  echo "FAIL parse: rc=$rc out=$out"; fail=1; }

# A program with subcommands run with nothing: its help, which names them, on
# stderr, and exit 2, as an error is; with a subcommand, it runs.
cat > "$TMP/tool.cul" <<'EOF'
let spec = {
  name: "tool",
  subcommands: [{name: "show", doc: "shows them", args: []}],
}
let a = Args.parse(Sys.argv, spec)
IO.println("ran {a.subcommand}")
EOF
out=$("$CULEBRA" --vm "$TMP/tool.cul" 2>"$TMP/err"); rc=$?
err=$(cat "$TMP/err")
[[ $rc -eq 2 ]] || { echo "FAIL no command: expected exit 2, got $rc"; fail=1; }
[[ -z "$out" ]] || { echo "FAIL no command: wrote to stdout: $out"; fail=1; }
[[ "$err" == "Usage: tool <command>"*"Commands:"*"show  shows them"* ]] || {
  echo "FAIL no command: unexpected stderr: $err"; fail=1; }
out=$("$CULEBRA" --vm "$TMP/tool.cul" show 2>&1); rc=$?
[[ $rc -eq 0 && "$out" == "ran show" ]] || { echo "FAIL command: rc=$rc out=$out"; fail=1; }

if [[ $fail -eq 0 ]]; then echo "args_cli_test OK"; exit 0; fi
echo "args_cli_test FAILED"; exit 1
