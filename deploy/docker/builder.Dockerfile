# syntax=docker/dockerfile:1.7
#
# Cartwheel build image: toolchain + every vcpkg dependency prebuilt.
# Service images build FROM this so a code change never recompiles gRPC.
#
#   docker build -f deploy/docker/builder.Dockerfile -t cartwheel-builder .
#
# The dependency layer is keyed on vcpkg.json only; a BuildKit cache mount
# keeps vcpkg's binary cache across rebuilds when the manifest changes.

FROM ubuntu:24.04 AS toolchain
ARG TARGETARCH
ARG CMAKE_VERSION=3.31.6
ARG VCPKG_COMMIT=054bd01bec7f31492ec9ac063740cc984a0fe59e
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential clang-18 clang-tidy-18 lld-18 llvm-18 \
      ninja-build git curl ca-certificates zip unzip tar pkg-config \
      autoconf automake autoconf-archive libtool python3 linux-libc-dev \
    && rm -rf /var/lib/apt/lists/*

# Ubuntu's CMake (3.28) predates our presets' features; use Kitware's build.
RUN case "${TARGETARCH}" in \
      amd64) arch=x86_64 ;; arm64) arch=aarch64 ;; *) echo "unsupported ${TARGETARCH}"; exit 1 ;; \
    esac \
    && curl -fsSL "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-${arch}.tar.gz" \
       | tar -xz --strip-components=1 -C /usr/local

ENV CC=clang-18 \
    CXX=clang++-18 \
    VCPKG_ROOT=/opt/vcpkg \
    VCPKG_FORCE_SYSTEM_BINARIES=1 \
    VCPKG_DISABLE_METRICS=1

RUN git clone https://github.com/microsoft/vcpkg.git "${VCPKG_ROOT}" \
    && git -C "${VCPKG_ROOT}" checkout "${VCPKG_COMMIT}" \
    && "${VCPKG_ROOT}/bootstrap-vcpkg.sh" -disableMetrics

FROM toolchain AS builder
# Each clang job compiling gRPC can take ~1 GB; Docker Desktop defaults to ~8 GB.
ARG VCPKG_MAX_CONCURRENCY=6
ENV VCPKG_MAX_CONCURRENCY=${VCPKG_MAX_CONCURRENCY}
WORKDIR /deps
COPY vcpkg.json .
RUN --mount=type=cache,target=/root/.cache/vcpkg \
    "${VCPKG_ROOT}/vcpkg" install --x-install-root=/opt/vcpkg_installed \
    && rm -rf "${VCPKG_ROOT}/buildtrees" "${VCPKG_ROOT}/downloads" "${VCPKG_ROOT}/packages"

WORKDIR /src
# Configure against the prebuilt tree without re-running vcpkg:
#   cmake --preset release -DVCPKG_INSTALLED_DIR=/opt/vcpkg_installed -DVCPKG_MANIFEST_INSTALL=OFF
