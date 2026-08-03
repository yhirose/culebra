#!/usr/bin/env bash
# Regression test for `culebra init` (src/init_cmd.cc). Every case runs with
# HOME and cwd redirected into a throwaway tmpdir — this must never touch the
# machine's real ~/.vim, ~/.vscode, or any real project file. Usage:
# init_test.sh <path-to-culebra>
set -u
CULEBRA="${1:?usage: init_test.sh <culebra-binary>}"
# Resolved once up front: every case below `cd`s elsewhere before invoking it.
case "$CULEBRA" in
  /*) : ;;
  *) CULEBRA="$PWD/$CULEBRA" ;;
esac
fail=0

WORK=$(mktemp -d -p "${TMPDIR:-/tmp}")
trap 'rm -rf "$WORK"' EXIT

# A fake `code` on PATH so the VSCode step has something to find without
# touching a real editor. Records its args; the caller decides what to do.
BIN="$WORK/bin"
mkdir -p "$BIN"
cat > "$BIN/code" <<'SH'
#!/bin/sh
echo "$@" >> "$CODE_LOG"
if [ -n "${SAVE_VSIX:-}" ]; then cp "$2" "$SAVE_VSIX"; fi
exit 0
SH
chmod +x "$BIN/code"

run_init() {  # <home> <proj> [extra PATH dirs...]
  local home=$1 proj=$2
  mkdir -p "$home" "$proj"
  ( cd "$proj" && env HOME="$home" PATH="$BIN:/usr/bin:/bin" \
      CODE_LOG="$WORK/code.log" SAVE_VSIX="$WORK/saved.vsix" \
      "$CULEBRA" init "${@:3}" )
}

# --- AI instructions: no destination file exists -> AGENTS.md is created ---
p1="$WORK/p1" h1="$WORK/h1"
mkdir -p "$h1"  # no .vim / nvim here — isolates this case to the agent step
out=$(run_init "$h1" "$p1")
if [[ ! -f "$p1/AGENTS.md" ]]; then
  echo "FAIL: AGENTS.md not created when nothing existed"; fail=1
fi
if ! grep -q 'culebra:agent:begin' "$p1/AGENTS.md" 2>/dev/null; then
  echo "FAIL: AGENTS.md missing the marker block"; fail=1
fi
if [[ -f "$p1/CLAUDE.md" ]]; then
  echo "FAIL: CLAUDE.md created when it shouldn't have been"; fail=1
fi

# --- AI instructions: an existing CLAUDE.md is updated, not replaced ------
p2="$WORK/p2" h2="$WORK/h2"
mkdir -p "$p2" "$h2"
printf '# My project notes\n' > "$p2/CLAUDE.md"
run_init "$h2" "$p2" >/dev/null
if [[ -f "$p2/AGENTS.md" ]]; then
  echo "FAIL: AGENTS.md created even though CLAUDE.md already existed"; fail=1
fi
if ! grep -q 'My project notes' "$p2/CLAUDE.md"; then
  echo "FAIL: existing CLAUDE.md content was lost"; fail=1
fi
if ! grep -q 'culebra:agent:begin' "$p2/CLAUDE.md"; then
  echo "FAIL: CLAUDE.md missing the marker block"; fail=1
fi

# --- idempotency: a second run does not duplicate the block ---------------
run_init "$h1" "$p1" >/dev/null
count=$(grep -c 'culebra:agent:begin' "$p1/AGENTS.md")
if [[ "$count" != 1 ]]; then
  echo "FAIL: marker block duplicated on rerun ($count copies)"; fail=1
fi

# --- Vim: only installs where a config dir already exists -----------------
p3="$WORK/p3" h3="$WORK/h3"
mkdir -p "$h3/.vim"
run_init "$h3" "$p3" >/dev/null
for f in syntax/culebra.vim ftplugin/culebra.vim ftdetect/culebra.vim; do
  if [[ ! -f "$h3/.vim/$f" ]]; then
    echo "FAIL: $f not installed into ~/.vim"; fail=1
  fi
done
if [[ -d "$h3/.config/nvim" ]]; then
  echo "FAIL: nvim dir created out of nowhere"; fail=1
fi

# --- neither ~/.vim nor ~/.config/nvim: not installed, but not a failure --
p4="$WORK/p4" h4="$WORK/h4"
mkdir -p "$h4"  # HOME exists but has no .vim / .config/nvim
( cd "$(mkdir -p "$p4" && echo "$p4")" && env HOME="$h4" PATH="/usr/bin:/bin" \
    "$CULEBRA" init >/dev/null 2>&1 )
rc=$?
if [[ $rc != 0 ]]; then
  echo "FAIL: init exits nonzero just because no editor was found (got $rc)"
  fail=1
fi

# --- VSCode: PATH with no editor CLI is skipped silently, not a failure ---
p5="$WORK/p5" h5="$WORK/h5"
mkdir -p "$p5" "$h5"
( cd "$p5" && env HOME="$h5" PATH="/usr/bin:/bin" "$CULEBRA" init >/dev/null 2>&1 )
if [[ $? != 0 ]]; then
  echo "FAIL: init exits nonzero when no VSCode-family CLI is on PATH"; fail=1
fi

# --- VSCode: a found CLI is invoked with a valid --install-extension zip --
p6="$WORK/p6" h6="$WORK/h6"
rm -f "$WORK/code.log" "$WORK/saved.vsix"
run_init "$h6" "$p6" >/dev/null
if ! grep -q -- '--install-extension' "$WORK/code.log" 2>/dev/null; then
  echo "FAIL: fake code CLI was not invoked with --install-extension"; fail=1
fi
if [[ ! -s "$WORK/saved.vsix" ]]; then
  echo "FAIL: no .vsix was handed to the editor CLI"; fail=1
elif ! unzip -l "$WORK/saved.vsix" >/dev/null 2>&1; then
  echo "FAIL: the .vsix is not a valid zip archive"; fail=1
elif ! unzip -p "$WORK/saved.vsix" extension/package.json 2>/dev/null \
       | grep -q '"culebra"\|"program"'; then
  echo "FAIL: extension/package.json missing from the .vsix"; fail=1
fi

# --- usage / bad usage -----------------------------------------------------
"$CULEBRA" init -h >/dev/null 2>&1
if [[ $? != 0 ]]; then echo "FAIL: init -h should exit 0"; fail=1; fi

"$CULEBRA" init --nonsense >/dev/null 2>&1
if [[ $? != 2 ]]; then echo "FAIL: unknown init flag should exit 2"; fail=1; fi

if [[ $fail == 0 ]]; then echo "init_test OK"; fi
exit $fail
