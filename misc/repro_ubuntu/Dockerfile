# The Ubuntu toolchain ci.yml's Linux jobs build with, for reproducing a
# failure that only shows there (.github/actions/setup-linux-toolchain).
# Not part of the build: `docker build -f misc/repro_ubuntu/Dockerfile .`
#
# Also the base image for .devcontainer/devcontainer.json (GitHub Codespaces
# / VS Code Dev Containers) — a toolchain bump here reaches both consumers.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
# g++ on top of g++-14: `culebra build` shells out to the stock `cc`/`c++`
# (never $CC/$CXX — LLVM is linked into the binary), and the base image has
# `cc` but no `c++`, so the AOT lane's embedded-asset step finds nothing to
# compile with. A CI runner has both; this makes the image agree.
RUN apt-get update && apt-get install -y \
      ccache cmake g++ g++-14 libblas-dev mold wget git \
      lsb-release software-properties-common gnupg \
      zlib1g-dev libssl-dev pkg-config gdb libzstd-dev \
 && wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh \
 && chmod +x /tmp/llvm.sh && /tmp/llvm.sh 22 \
 && apt-get install -y llvm-22-dev \
 && rm -rf /var/lib/apt/lists/*

# A separate layer: just's version and checksum change on their own (see
# .github/actions/install-just/action.yml, the source of truth this pins to,
# so this image and CI never install different just binaries), and a
# just-only bump shouldn't invalidate the apt+LLVM layer above and force a
# multi-minute reinstall of it. A `#` comment cannot live inside a RUN's
# backslash-continued lines — the shell joins them onto one logical line,
# where `#` would silence everything after it — so this explanation stays
# above the instruction, not inside it.
RUN wget -qO /tmp/just.tar.gz https://github.com/casey/just/releases/download/1.58.0/just-1.58.0-x86_64-unknown-linux-musl.tar.gz \
 && echo "4a5cc2f53e6f0f8c59092a6cc38291eb729d46a7dd95d3ae582008881b84931d  /tmp/just.tar.gz" | sha256sum -c - \
 && tar -xzf /tmp/just.tar.gz -C /usr/local/bin just \
 && rm -f /tmp/just.tar.gz

# The same four the toolchain action exports. CMAKE_PREFIX_PATH and
# LLVM_CONFIG both: without them cmake silently configures the driver with no
# JIT, and every `--jit` run reads the flag as a file name.
ENV CC=gcc-14 CXX=g++-14
ENV CMAKE_PREFIX_PATH=/usr/lib/llvm-22
ENV LLVM_CONFIG=/usr/lib/llvm-22/bin/llvm-config
ENV CULEBRA_CANVAS_WINDOW_DEFAULT=OFF
WORKDIR /src
