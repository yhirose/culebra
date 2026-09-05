# The Ubuntu toolchain ci.yml's Linux jobs build with, for reproducing a
# failure that only shows there (.github/actions/setup-linux-toolchain).
# Not part of the build: `docker build -f misc/repro-ubuntu.dockerfile .`
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
      ccache cmake g++-14 libblas-dev mold wget git \
      lsb-release software-properties-common gnupg \
      zlib1g-dev libssl-dev pkg-config gdb libzstd-dev \
 && wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh \
 && chmod +x /tmp/llvm.sh && /tmp/llvm.sh 22 \
 && apt-get install -y llvm-22-dev \
 && wget -qO /tmp/just.tar.gz https://github.com/casey/just/releases/download/1.36.0/just-1.36.0-x86_64-unknown-linux-musl.tar.gz \
 && tar -xzf /tmp/just.tar.gz -C /usr/local/bin just \
 && rm -rf /var/lib/apt/lists/*

# The same four the toolchain action exports. CMAKE_PREFIX_PATH and
# LLVM_CONFIG both: without them cmake silently configures the driver with no
# JIT, and every `--jit` run reads the flag as a file name.
ENV CC=gcc-14 CXX=g++-14
ENV CMAKE_PREFIX_PATH=/usr/lib/llvm-22
ENV LLVM_CONFIG=/usr/lib/llvm-22/bin/llvm-config
ENV CULEBRA_CANVAS_WINDOW_DEFAULT=OFF
WORKDIR /src
