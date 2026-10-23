# Host unit-test environment for the OXOS core runtime: a 32-bit (i386) build of the kernel's core sources with
# GoogleTest. Used by tools/unit-tests.sh when the host has no 32-bit (multilib) toolchain.
FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        clang llvm lld g++-multilib cmake ninja-build googletest libclang-rt-18-dev python3 gdb \
    && rm -rf /var/lib/apt/lists/*
