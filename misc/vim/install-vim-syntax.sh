#!/bin/sh
# Install cul.vim into existing vim / neovim configurations.
# Targets ~/.vim and ~/.config/nvim — whichever already exists.

set -eu

SRC_DIR=$(cd "$(dirname "$0")" && pwd)
SRC="$SRC_DIR/cul.vim"

if [ ! -f "$SRC" ]; then
  echo "error: $SRC not found" >&2
  exit 1
fi

FTDETECT='autocmd BufRead,BufNewFile *.cul set filetype=cul'

install_into() {
  root=$1
  name=$2
  mkdir -p "$root/syntax" "$root/ftdetect"
  cp "$SRC" "$root/syntax/cul.vim"
  printf '%s\n' "$FTDETECT" > "$root/ftdetect/cul.vim"
  echo "installed into $name ($root)"
}

installed=0

if [ -d "$HOME/.vim" ]; then
  install_into "$HOME/.vim" vim
  installed=1
fi

if [ -d "$HOME/.config/nvim" ]; then
  install_into "$HOME/.config/nvim" neovim
  installed=1
fi

if [ "$installed" -eq 0 ]; then
  echo "no ~/.vim or ~/.config/nvim found — nothing to install" >&2
  exit 1
fi
