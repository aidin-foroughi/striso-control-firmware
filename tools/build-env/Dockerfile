# Striso firmware build environment.
# Ubuntu 22.04 ships faust 2.37.3, the exact version upstream used to generate synth.cpp, and
# arm-gcc 10.3 (gcc-arm-none-eabi 15:10.3-2021.07-4), which reproduces the official v2.2.4 release.
FROM ubuntu:22.04@sha256:5ec03bb3441e8b0bf3b4f9cd4629a1ae763010dc3035bb8da3ae6cf026486401
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib \
        binutils make git python3 faust g++ ca-certificates curl ninja-build \
    && rm -rf /var/lib/apt/lists/* \
    && git config --system --add safe.directory '*'
# CMake, pinned and checksum verified (Ubuntu 22.04 has 3.22; the project needs >= 3.25)
ARG CMAKE_VERSION=3.31.6
ARG CMAKE_SHA256=5a1133ff103c71eb5120e2cc3de922733e7d8a26a98ae716397e8676adb367bf
RUN curl -fsSL -o /tmp/cmake.tar.gz \
        "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz" \
    && echo "${CMAKE_SHA256}  /tmp/cmake.tar.gz" | sha256sum -c - \
    && tar -C /opt -xzf /tmp/cmake.tar.gz && rm /tmp/cmake.tar.gz \
    && ln -s /opt/cmake-${CMAKE_VERSION}-linux-x86_64/bin/* /usr/local/bin/
WORKDIR /src
