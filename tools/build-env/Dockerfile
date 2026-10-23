# Striso firmware build environment (tools/build.sh, tools/test.sh, CI).
#   Arm GNU Toolchain 14.2.rel1 (GCC 14.2, newlib 4.4) for the firmware, pinned and checksum
#     verified (the sha256 Arm publishes next to the archive)
#   CMake 3.31.6 (pinned, checksum verified) and Ninja
#   Ubuntu 22.04's faust 2.37.3 (the version upstream used for synth.cpp), g++ 11 for the host
#     tests and renderers (the golden renders in tests/golden are this compiler's), clang-format
#     and clang-tidy 15, QEMU 6.2 (tools/target_render: the DSP as the board computes it)
FROM ubuntu:22.04@sha256:5ec03bb3441e8b0bf3b4f9cd4629a1ae763010dc3035bb8da3ae6cf026486401
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        make git python3 faust g++ ca-certificates curl xz-utils ninja-build \
        clang-format-15 clang-tidy-15 qemu-system-arm \
    && rm -rf /var/lib/apt/lists/* \
    && ln -s /usr/bin/clang-format-15 /usr/local/bin/clang-format \
    && ln -s /usr/bin/clang-tidy-15 /usr/local/bin/clang-tidy \
    && ln -s /usr/bin/run-clang-tidy-15 /usr/local/bin/run-clang-tidy \
    && git config --system --add safe.directory '*'

ARG ARM_TOOLCHAIN_VERSION=14.2.rel1
ARG ARM_TOOLCHAIN_SHA256=62a63b981fe391a9cbad7ef51b17e49aeaa3e7b0d029b36ca1e9c3b2a9b78823
RUN curl -fsSL -o /tmp/arm.tar.xz \
        "https://developer.arm.com/-/media/Files/downloads/gnu/${ARM_TOOLCHAIN_VERSION}/binrel/arm-gnu-toolchain-${ARM_TOOLCHAIN_VERSION}-x86_64-arm-none-eabi.tar.xz" \
    && echo "${ARM_TOOLCHAIN_SHA256}  /tmp/arm.tar.xz" | sha256sum -c - \
    && mkdir -p /opt/arm-gnu-toolchain \
    && tar -C /opt/arm-gnu-toolchain --strip-components=1 -xJf /tmp/arm.tar.xz \
    && rm -rf /tmp/arm.tar.xz /opt/arm-gnu-toolchain/share/doc
ENV PATH=/opt/arm-gnu-toolchain/bin:$PATH

ARG CMAKE_VERSION=3.31.6
ARG CMAKE_SHA256=5a1133ff103c71eb5120e2cc3de922733e7d8a26a98ae716397e8676adb367bf
RUN curl -fsSL -o /tmp/cmake.tar.gz \
        "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz" \
    && echo "${CMAKE_SHA256}  /tmp/cmake.tar.gz" | sha256sum -c - \
    && tar -C /opt -xzf /tmp/cmake.tar.gz && rm /tmp/cmake.tar.gz \
    && ln -s /opt/cmake-${CMAKE_VERSION}-linux-x86_64/bin/* /usr/local/bin/
WORKDIR /src
