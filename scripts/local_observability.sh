#!/usr/bin/env bash
# Prometheus, Grafana and Jaeger in Docker for the natively running stack
# (local_cluster.sh + local_infra.sh). Containers are prefixed cw- and share
# the network cw-obs; ports stay clear of other local stacks:
#   prometheus :19090   grafana :13000 (admin/admin)   jaeger UI :26686
#   OTLP gRPC for the services :24317
#
#   scripts/local_observability.sh start | stop
#   then run services with CARTWHEEL_TRACING_OTLP_ENDPOINT=127.0.0.1:24317
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OBS=$ROOT/deploy/observability
DATA=${CARTWHEEL_OBS_DATA:-$ROOT/data/observability}
H=host.docker.internal

write_prometheus_config() {
  mkdir -p "$DATA"
  cat >"$DATA/prometheus.yml" <<CFG
global: { scrape_interval: 5s, evaluation_interval: 5s }
rule_files: [/etc/prometheus/alerts.yml]
scrape_configs:
  - job_name: wheel-node
    static_configs: [{ targets: ["$H:9101", "$H:9102", "$H:9103", "$H:9104", "$H:9105"] }]
  - job_name: cart-svc
    static_configs: [{ targets: ["$H:9200"] }]
  - job_name: deal-svc
    static_configs: [{ targets: ["$H:9300"] }]
  - job_name: order-worker
    static_configs: [{ targets: ["$H:9400", "$H:9401"] }]
  - job_name: gateway
    static_configs: [{ targets: ["$H:8090"] }]
CFG
}

case "${1:-}" in
  start)
    write_prometheus_config
    docker network inspect cw-obs >/dev/null 2>&1 || docker network create cw-obs >/dev/null
    docker run -d --name cw-jaeger --network cw-obs --network-alias jaeger \
      -p 26686:16686 -p 24317:4317 jaegertracing/all-in-one:latest >/dev/null
    docker run -d --name cw-prometheus --network cw-obs --network-alias prometheus \
      -p 19090:9090 --add-host host.docker.internal:host-gateway \
      -v "$DATA/prometheus.yml:/etc/prometheus/prometheus.yml:ro" \
      -v "$OBS/alerts.yml:/etc/prometheus/alerts.yml:ro" prom/prometheus:v3.16.0 >/dev/null
    docker run -d --name cw-grafana --network cw-obs -p 13000:3000 \
      -e GF_AUTH_ANONYMOUS_ENABLED=true -e GF_AUTH_ANONYMOUS_ORG_ROLE=Viewer \
      -v "$OBS/grafana/provisioning:/etc/grafana/provisioning:ro" \
      -v "$OBS/grafana/dashboards:/var/lib/grafana/dashboards:ro" grafana/grafana:13.3.0 >/dev/null
    echo "prometheus http://127.0.0.1:19090  grafana http://127.0.0.1:13000/d/cartwheel  jaeger http://127.0.0.1:26686"
    echo "export CARTWHEEL_TRACING_OTLP_ENDPOINT=127.0.0.1:24317"
    ;;
  stop)
    docker rm -f cw-grafana cw-prometheus cw-jaeger >/dev/null 2>&1 || true
    ;;
  *) echo "usage: $0 start|stop"; exit 2 ;;
esac
