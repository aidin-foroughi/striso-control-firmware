# Striso firmware build environment.
# Ubuntu 22.04 ships faust 2.37.3, the exact version upstream used to generate synth.cpp.
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib \
        binutils make git python3 faust g++ ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && git config --system --add safe.directory '*'
WORKDIR /src
