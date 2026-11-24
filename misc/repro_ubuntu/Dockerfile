# The Ubuntu toolchain ci.yml's Linux jobs build with, for reproducing a
# failure that only shows there (.github/actions/setup-linux-toolchain).
# Not part of the build: `docker build -f misc/repro_ubuntu/Dockerfile .` (the
# context is the checkout's root: the build copies two files from misc/).
#
# Also the base image for .devcontainer/devcontainer.json (GitHub Codespaces
# / VS Code Dev Containers) — a toolchain bump here reaches both consumers.
#
# It also builds a Linux arm64 culebra, which no release ships, on an Apple
# Silicon Mac: Docker builds the image natively there. With the window and
# Scene, as a release has them:
#
#   docker build --build-arg WINDOW=ON -t culebra-ubuntu:window \
#     -f misc/repro_ubuntu/Dockerfile .
#   docker run --rm -v "$PWD":/src -v culebra-ccache:/root/.cache/ccache \
#     culebra-ubuntu:window misc/repro_ubuntu/build.sh
#
# The named volume keeps ccache between runs (the container's own copy goes
# with --rm); the source is always /src, so a new worktree hits it too.
#
# build-linux/culebra then runs on a Linux whose glibc and libstdc++ are at
# least the versions build.sh prints at the end (Ubuntu 24.04's toolchain).
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
# g++ on top of g++-14: `culebra build` shells out to the stock `cc`/`c++`
# (never $CC/$CXX — LLVM is linked into the binary), and the base image has
# `cc` but no `c++`, so the AOT lane's embedded-asset step finds nothing to
# compile with. A CI runner has both; this makes the image agree.
RUN apt-get update && apt-get install -y \
      ccache cmake curl g++ g++-14 libblas-dev mold wget git \
      lsb-release software-properties-common gnupg \
      zlib1g-dev libssl-dev pkg-config gdb libzstd-dev \
 && wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh \
 && chmod +x /tmp/llvm.sh && /tmp/llvm.sh 22 \
 && apt-get install -y llvm-22-dev \
 && rm -rf /var/lib/apt/lists/*

# A separate layer: just's version and checksum change on their own, and a
# just-only bump shouldn't invalidate the apt+LLVM layer above and force a
# multi-minute reinstall of it. misc/install_just.sh holds the pin (CI and
# setup_ubuntu.sh run it too) and picks the asset for this machine's
# architecture, so the image builds natively on arm64 (an Apple Silicon Mac's
# Docker) as well as on x86-64.
COPY misc/install_just.sh /tmp/install_just.sh
RUN /tmp/install_just.sh && rm /tmp/install_just.sh

# WINDOW=ON adds SDL3's build dependencies, so the build carries the Canvas
# window and Scene the way a release does; the default stays the display-less
# build of ci.yml's Linux jobs. A build arg on a later layer rather than a
# second image: the two share every layer above and cannot drift apart. The
# list is the one CI installs, Wayland's included (the backend a handheld
# running Sway opens).
ARG WINDOW=OFF
COPY misc/linux_sdl3_deps.txt /tmp/linux_sdl3_deps.txt
RUN if [ "$WINDOW" = ON ]; then \
      apt-get update \
      && grep -v -e '^#' -e '^$' /tmp/linux_sdl3_deps.txt | xargs apt-get install -y \
      && rm -rf /var/lib/apt/lists/*; \
    fi \
 && rm /tmp/linux_sdl3_deps.txt

# The same four the toolchain action exports. CMAKE_PREFIX_PATH and
# LLVM_CONFIG both: without them cmake silently configures the driver with no
# JIT, and every `--jit` run reads the flag as a file name.
ENV CC=gcc-14 CXX=g++-14
ENV CMAKE_PREFIX_PATH=/usr/lib/llvm-22
ENV LLVM_CONFIG=/usr/lib/llvm-22/bin/llvm-config
ENV CULEBRA_CANVAS_WINDOW_DEFAULT=$WINDOW
WORKDIR /src
