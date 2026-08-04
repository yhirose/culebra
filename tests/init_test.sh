#!/usr/bin/env bash
# Regression test for `culebra init` (src/init_cmd.cc). Every case runs with
# HOME and cwd redirected into a throwaway tmpdir — this must never touch the
# machine's real ~/.vim, ~/.vscode, or any real project file. Every invocation
# also redirects stdin from /dev/null: init now shows a plan and, at an
# interactive terminal, asks to confirm before applying it — without this
# redirect, running this script directly from a real terminal (stdin still a
# tty) would hang at that prompt instead of applying non-interactively like
# CI does. Usage: init_test.sh <path-to-culebra>
set -u
CULEBRA="${1:?usage: init_test.sh <culebra-binary>}"
# Resolved once up front: every case below `cd`s elsewhere before invoking it.
case "$CULEBRA" in
  /*) : ;;
  *) CULEBRA="$PWD/$CULEBRA" ;;
esac
# $0 is this script's own path (ctest passes it absolute), so this is stable
# regardless of ctest's working directory — unlike CULEBRA above, nothing
# here depends on how the caller invoked us.
REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
CULEBRA_VERSION=$(sed -n 's/^#define CULEBRA_VERSION "\([^"]*\)"/\1/p' \
  "$REPO_ROOT/include/culebra.h")
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

# A fake `zed` lives in its own dir, not $BIN — $BIN is on every run_init()
# case's PATH, and adding zed there would make every one of them also detect
# and set up Zed, an unrelated behavior change for tests that don't care.
# Only the Zed-specific cases below opt into $BIN_ZED.
BIN_ZED="$WORK/bin-zed"
mkdir -p "$BIN_ZED"
cat > "$BIN_ZED/zed" <<'SH'
#!/bin/sh
exit 0
SH
chmod +x "$BIN_ZED/zed"

run_init() {  # <home> <proj> [extra `culebra init` args...]
  local home=$1 proj=$2
  mkdir -p "$home" "$proj"
  ( cd "$proj" && env HOME="$home" PATH="$BIN:/usr/bin:/bin" \
      CODE_LOG="$WORK/code.log" SAVE_VSIX="$WORK/saved.vsix" \
      "$CULEBRA" init "${@:3}" < /dev/null )
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
if ! grep -q 'culebra init will:' <<<"$out"; then
  echo "FAIL: no preview ('culebra init will:') printed before applying"
  fail=1
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
    "$CULEBRA" init >/dev/null 2>&1 < /dev/null )
rc=$?
if [[ $rc != 0 ]]; then
  echo "FAIL: init exits nonzero just because no editor was found (got $rc)"
  fail=1
fi

# --- VSCode: PATH with no editor CLI is skipped silently, not a failure ---
p5="$WORK/p5" h5="$WORK/h5"
mkdir -p "$p5" "$h5"
( cd "$p5" && env HOME="$h5" PATH="/usr/bin:/bin" "$CULEBRA" init >/dev/null 2>&1 < /dev/null )
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

# --- --yes behaves the same as the default non-interactive path -----------
p7="$WORK/p7" h7="$WORK/h7"
run_init "$h7" "$p7" --yes >/dev/null
if [[ ! -f "$p7/AGENTS.md" ]]; then
  echo "FAIL: --yes did not apply the planned changes"; fail=1
fi

# --- Zed: detected -> extension + .zed/debug.json written ------------------
p8="$WORK/p8" h8="$WORK/h8"
mkdir -p "$p8" "$h8"
out8=$( cd "$p8" && env HOME="$h8" PATH="$BIN_ZED:/usr/bin:/bin" \
    "$CULEBRA" init < /dev/null )
ext8="$h8/.local/share/culebra-zed-extension"
if ! grep -q 'Zed: extension written to' <<<"$out8"; then
  echo "FAIL: Zed setup message not printed when zed is on PATH"; fail=1
fi
if [[ ! -f "$ext8/extension.toml" ]]; then
  echo "FAIL: extension.toml not written for detected Zed"; fail=1
else
  if ! grep -q 'repository = "https://github.com/yhirose/culebra"' \
      "$ext8/extension.toml"; then
    echo "FAIL: extension.toml repository is not the public GitHub URL"
    fail=1
  fi
  if ! grep -q 'path = "misc/zed/tree-sitter-culebra"' "$ext8/extension.toml"
  then
    echo "FAIL: extension.toml grammar path is wrong"; fail=1
  fi
  if ! grep -q "rev = \"v${CULEBRA_VERSION}\"" "$ext8/extension.toml"; then
    echo "FAIL: extension.toml rev doesn't match CULEBRA_VERSION" \
         "($CULEBRA_VERSION)"
    fail=1
  fi
fi
for f in Cargo.toml src/culebra.rs debug_adapter_schemas/culebra.json \
         languages/culebra/config.toml languages/culebra/highlights.scm; do
  if [[ ! -s "$ext8/$f" ]]; then
    echo "FAIL: Zed asset $f missing or empty"; fail=1
  fi
done
if [[ -f "$ext8/debug.json" ]]; then
  echo "FAIL: debug.json leaked into the extension dir (belongs in .zed/ only)"
  fail=1
fi
if [[ ! -f "$p8/.zed/debug.json" ]]; then
  echo "FAIL: .zed/debug.json not written"; fail=1
elif ! grep -q '"adapter": "culebra"' "$p8/.zed/debug.json"; then
  echo "FAIL: .zed/debug.json missing the adapter field"; fail=1
fi

# --- Zed: rerun backs up the existing .zed/debug.json -----------------------
( cd "$p8" && env HOME="$h8" PATH="$BIN_ZED:/usr/bin:/bin" \
    "$CULEBRA" init >/dev/null < /dev/null )
if [[ ! -f "$p8/.zed/debug.json.bak" ]]; then
  echo "FAIL: rerun did not back up the existing .zed/debug.json"; fail=1
fi

# --- Zed: not on PATH and no app-support dir -> skipped, not a failure -----
p9="$WORK/p9" h9="$WORK/h9"
mkdir -p "$p9" "$h9"
( cd "$p9" && env HOME="$h9" PATH="/usr/bin:/bin" \
    "$CULEBRA" init >/dev/null 2>&1 < /dev/null )
if [[ $? != 0 ]]; then
  echo "FAIL: init exits nonzero when Zed is not detected"; fail=1
fi
if [[ -d "$h9/.local/share/culebra-zed-extension" ]]; then
  echo "FAIL: Zed extension dir created even though Zed wasn't detected"
  fail=1
fi

# --- nothing detected + already up to date -> "Nothing to do." -------------
p10="$WORK/p10" h10="$WORK/h10"
mkdir -p "$p10" "$h10"
( cd "$p10" && env HOME="$h10" PATH="/usr/bin:/bin" \
    "$CULEBRA" init >/dev/null 2>&1 < /dev/null )
out10=$( cd "$p10" && env HOME="$h10" PATH="/usr/bin:/bin" \
    "$CULEBRA" init < /dev/null )
if ! grep -q 'Nothing to do.' <<<"$out10"; then
  echo "FAIL: second no-op run didn't print 'Nothing to do.'"; fail=1
fi

# --- usage / bad usage -----------------------------------------------------
"$CULEBRA" init -h >/dev/null 2>&1
if [[ $? != 0 ]]; then echo "FAIL: init -h should exit 0"; fail=1; fi

"$CULEBRA" init --nonsense >/dev/null 2>&1
if [[ $? != 2 ]]; then echo "FAIL: unknown init flag should exit 2"; fail=1; fi

if [[ $fail == 0 ]]; then echo "init_test OK"; fi
exit $fail
