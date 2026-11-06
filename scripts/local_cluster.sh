#!/usr/bin/env bash
# Start (or stop) a 5-node CartwheelKV cluster on localhost.
#   scripts/local_cluster.sh start                 ports 7101-7105, metrics 9101-9105
#   scripts/local_cluster.sh stop
#   scripts/local_cluster.sh kill <i> | restart <i>
#   CARTWHEEL_BUILD=build/dev scripts/local_cluster.sh start   (default build/release)
set -euo pipefail
BUILD=${CARTWHEEL_BUILD:-build/release}
DATA=${CARTWHEEL_DATA:-./data/local-cluster}
SEEDS=$(for i in 1 2 3 4 5; do printf "wheel-%d=127.0.0.1:710%d," "$i" "$i"; done)
SEEDS=${SEEDS%,}

start_node() {
  local i=$1
  mkdir -p "$DATA/logs"
  CARTWHEEL_NODE_ID=wheel-$i \
  CARTWHEEL_NODE_ADVERTISE=127.0.0.1:710$i \
  CARTWHEEL_SERVER_LISTEN=127.0.0.1:710$i \
  CARTWHEEL_METRICS_LISTEN=127.0.0.1:910$i \
  CARTWHEEL_STORAGE_DIR="$DATA/wheel-$i" \
  CARTWHEEL_CLUSTER_ENABLED=true \
  CARTWHEEL_CLUSTER_SEEDS="$SEEDS" \
  CARTWHEEL_SERVER_DRAIN_GRACE_MS=200 \
  CARTWHEEL_DEBUG_FAULT_INJECTION=${CARTWHEEL_FAULT_INJECTION:-true} \
    "$BUILD/src/kv/wheel-node" >>"$DATA/logs/wheel-$i.log" 2>&1 &
  echo $! >"$DATA/wheel-$i.pid"
}

case "${1:-}" in
  start) for i in 1 2 3 4 5; do start_node "$i"; done; sleep 1; echo "started 5 nodes (logs in $DATA/logs)" ;;
  stop)  for f in "$DATA"/wheel-*.pid; do kill "$(cat "$f")" 2>/dev/null || true; rm -f "$f"; done ;;
  kill)  kill -9 "$(cat "$DATA/wheel-$2.pid")"; echo "SIGKILLed wheel-$2" ;;
  restart) start_node "$2"; echo "restarted wheel-$2" ;;
  *) echo "usage: $0 start|stop|kill <i>|restart <i>"; exit 2 ;;
esac
