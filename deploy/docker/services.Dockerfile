# syntax=docker/dockerfile:1.7
#
# One image carrying every C++ service (wheel-node, cart-svc, deal-svc,
# order-worker, gateway); compose
# picks the binary with `command:`. Builds on the prebuilt dependency image, so
# a code change recompiles only Cartwheel.
#
#   docker build -f deploy/docker/builder.Dockerfile -t cartwheel-builder:dev .
#   docker build -f deploy/docker/services.Dockerfile -t cartwheel:dev .

ARG BUILDER=cartwheel-builder:dev

FROM ${BUILDER} AS build
WORKDIR /src
COPY . .
RUN cmake --preset release \
      -DVCPKG_INSTALLED_DIR=/opt/vcpkg_installed \
      -DVCPKG_MANIFEST_INSTALL=OFF \
      -DCARTWHEEL_BUILD_TESTS=OFF \
      -DCARTWHEEL_BUILD_BENCH=OFF \
    && cmake --build --preset release \
         --target wheel-node cart-svc deal-svc order-worker gateway wheelctl \
    && install -D -m 0755 -t /out \
         build/release/src/kv/wheel-node \
         build/release/src/services/cart/cart-svc \
         build/release/src/services/deal/deal-svc \
         build/release/src/services/order_worker/order-worker \
         build/release/src/gateway/gateway \
         build/release/src/tools/wheelctl

FROM ubuntu:24.04 AS runtime
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --no-create-home cartwheel \
    && mkdir -p /data && chown cartwheel /data
COPY --from=build /out/ /usr/local/bin/
USER cartwheel
# Structured logs for aggregation; override per service in compose.
ENV CARTWHEEL_LOG_JSON=true
