# syntax=docker/dockerfile:1.4
FROM --platform=linux/amd64 debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN dpkg --add-architecture armhf \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
      ca-certificates curl git \
      build-essential cmake ninja-build pkg-config python3 protobuf-compiler \
      squashfs-tools \
      crossbuild-essential-armhf \
      libeigen3-dev \
      libssl-dev:armhf zlib1g-dev:armhf \
      libwebrtc-audio-processing-dev:armhf \
    && rm -rf /var/lib/apt/lists/*

# Distribution ARM headers/libraries suffice for a fully static build.
RUN mkdir -p /opt/sysroot/usr/include /opt/sysroot/usr/lib/pkgconfig /opt/sysroot/lib \
    && cp -a /usr/arm-linux-gnueabihf/include/. /opt/sysroot/usr/include/ \
    && cp -a /usr/arm-linux-gnueabihf/lib/. /opt/sysroot/usr/lib/ \
    && cp -a /usr/include/openssl /usr/include/zlib.h /usr/include/zconf.h /usr/include/webrtc_audio_processing /opt/sysroot/usr/include/ \
    && cp -a /usr/include/arm-linux-gnueabihf/openssl/. /opt/sysroot/usr/include/openssl/ \
    && cp -a /usr/lib/arm-linux-gnueabihf/*.a /opt/sysroot/usr/lib/ \
    && cp -a /usr/arm-linux-gnueabihf/lib/ld-linux-armhf.so.3 /opt/sysroot/lib/

ENV PKG_CONFIG_ALLOW_CROSS=1
ENV PKG_CONFIG_SYSROOT_DIR=/opt/sysroot
ENV PKG_CONFIG_LIBDIR=/opt/sysroot/usr/lib/pkgconfig:/opt/sysroot/lib/pkgconfig

WORKDIR /workspace
CMD ["/bin/bash"]
