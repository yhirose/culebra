# Linux dev/test image for culebra, mirroring .github/workflows/ci.yml's
# Ubuntu dependency install. Lets the CULEBRA_NEW_GC build + suite be validated
# on Linux locally (the conservative GC's stack_base path is glibc-specific).
# On Apple Silicon this runs as native arm64 Linux; pass --platform linux/amd64
# to match CI's x86_64 (slower, qemu-emulated).
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
      ccache \
      cmake \
      gcc-14 \
      g++-14 \
      libblas-dev \
      mold \
      wget \
      curl \
      lsb-release \
      software-properties-common \
      gnupg \
      git \
      ca-certificates \
      python3 \
 && rm -rf /var/lib/apt/lists/*

# just (Ubuntu 24.04's apt `just` is too old for [group(...)]).
RUN curl --proto '=https' --tlsv1.2 -sSf https://just.systems/install.sh \
      | bash -s -- --to /usr/local/bin

# LLVM 22 from apt.llvm.org (Ubuntu's llvm-dev is too old for the JIT).
RUN wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh \
 && chmod +x /tmp/llvm.sh \
 && /tmp/llvm.sh 22 \
 && apt-get update && apt-get install -y llvm-22-dev \
 && rm -rf /var/lib/apt/lists/*

# LLVM 22's exported cmake targets pull in zlib/zstd/terminfo/xml2, which the
# GitHub `ubuntu-latest` runner image provides implicitly but a clean
# ubuntu:24.04 does not (find_package(LLVM) errors on ZLIB::ZLIB otherwise).
RUN apt-get update && apt-get install -y \
      zlib1g-dev \
      libzstd-dev \
      libncurses-dev \
      libxml2-dev \
 && rm -rf /var/lib/apt/lists/*

ENV CC=gcc-14 \
    CXX=g++-14 \
    CMAKE_PREFIX_PATH=/usr/lib/llvm-22

WORKDIR /workspace
