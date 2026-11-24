# The Ubuntu toolchain ci.yml's Linux jobs build with, for reproducing a
# failure that only shows there (.github/actions/setup-linux-toolchain).
# Not part of the build: `docker build -f misc/repro_ubuntu/Dockerfile .`, from
# the checkout's root (it copies from misc/).
#
# Also the base image for .devcontainer/devcontainer.json (GitHub Codespaces
# / VS Code Dev Containers) — a toolchain bump here reaches both consumers.
#
# On an Apple Silicon Mac it builds a Linux arm64 driver natively (no release
# ships one), with the window and Scene; build.sh prints what it needs to run.
# The volumes keep this checkout's build dir and a ccache every checkout shares:
#
#   docker build --build-arg WINDOW=ON -t culebra-ubuntu:window \
#     -f misc/repro_ubuntu/Dockerfile .
#   docker run --rm -v "$PWD":/src -v "culebra-build-$(basename "$PWD")":/build \
#     -v culebra-ccache:/root/.cache/ccache \
#     culebra-ubuntu:window misc/repro_ubuntu/build.sh
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

# Its own layer, so a just bump keeps the apt+LLVM one. The script holds the
# pin for every architecture.
COPY misc/install_just.sh /tmp/install_just.sh
RUN /tmp/install_just.sh && rm /tmp/install_just.sh

# WINDOW=ON adds SDL3's build dependencies (CI's list) for the window and
# Scene; a later layer, so both images share the ones above.
ARG WINDOW=OFF
COPY misc/linux_sdl3_deps.txt /tmp/linux_sdl3_deps.txt
RUN case "$WINDOW" in ON|OFF) ;; *) echo "WINDOW is ON or OFF" >&2; exit 1 ;; esac \
 && if [ "$WINDOW" = ON ]; then \
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
