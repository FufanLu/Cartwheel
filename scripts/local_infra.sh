#!/usr/bin/env bash
# Native Postgres, Redis and Kafka (KRaft) for the lightning-deal pipeline,
# on ports that stay clear of other local stacks:
#   postgres 25432   redis 26379   kafka 29092 (controller 29093)
#
#   scripts/local_infra.sh start | stop | status | env
#
# Needs Homebrew postgresql@17, redis and kafka. `env` prints the variables
# deal-svc, order-worker and the infra tests read.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DATA=${CARTWHEEL_INFRA_DATA:-$ROOT/data/infra}
PG_BIN=${PG_BIN:-/opt/homebrew/opt/postgresql@17/bin}
PG_PORT=25432 REDIS_PORT=26379 KAFKA_PORT=29092 KAFKA_CTRL=29093

print_env() {
  cat <<EOF
export CARTWHEEL_POSTGRES_URL=postgresql://cartwheel@127.0.0.1:$PG_PORT/cartwheel
export CARTWHEEL_REDIS_URL=redis://127.0.0.1:$REDIS_PORT
export CARTWHEEL_KAFKA_BROKERS=127.0.0.1:$KAFKA_PORT
export CW_TEST_POSTGRES=postgresql://cartwheel@127.0.0.1:$PG_PORT/cartwheel_test
export CW_TEST_REDIS=redis://127.0.0.1:$REDIS_PORT
export CW_TEST_KAFKA=127.0.0.1:$KAFKA_PORT
EOF
}

start_pg() {
  if [ ! -f "$DATA/pg/PG_VERSION" ]; then
    mkdir -p "$DATA/pg"
    "$PG_BIN/initdb" -D "$DATA/pg" -U cartwheel -A trust -E UTF8 >/dev/null
  fi
  "$PG_BIN/pg_ctl" -D "$DATA/pg" -l "$DATA/pg.log" -w \
    -o "-p $PG_PORT -k $DATA -c listen_addresses=127.0.0.1 -c max_connections=200" start >/dev/null
  for db in cartwheel cartwheel_test; do
    "$PG_BIN/psql" -h 127.0.0.1 -p $PG_PORT -U cartwheel -d postgres -tAc \
      "SELECT 1 FROM pg_database WHERE datname='$db'" | grep -q 1 ||
      "$PG_BIN/createdb" -h 127.0.0.1 -p $PG_PORT -U cartwheel "$db"
  done
}

start_redis() {
  mkdir -p "$DATA/redis"
  redis-server --port $REDIS_PORT --bind 127.0.0.1 --dir "$DATA/redis" --daemonize yes \
    --appendonly yes --appendfsync everysec --pidfile "$DATA/redis.pid" \
    --logfile "$DATA/redis.log" >/dev/null
}

start_kafka() {
  local props="$DATA/kafka.properties"
  mkdir -p "$DATA/kafka"
  cat >"$props" <<EOF
process.roles=broker,controller
node.id=1
controller.quorum.voters=1@127.0.0.1:$KAFKA_CTRL
listeners=PLAINTEXT://127.0.0.1:$KAFKA_PORT,CONTROLLER://127.0.0.1:$KAFKA_CTRL
advertised.listeners=PLAINTEXT://127.0.0.1:$KAFKA_PORT
controller.listener.names=CONTROLLER
listener.security.protocol.map=PLAINTEXT:PLAINTEXT,CONTROLLER:PLAINTEXT
log.dirs=$DATA/kafka
num.partitions=12
offsets.topic.replication.factor=1
transaction.state.log.replication.factor=1
transaction.state.log.min.isr=1
group.initial.rebalance.delay.ms=0
EOF
  if [ ! -f "$DATA/kafka/meta.properties" ]; then
    kafka-storage format -t "$(kafka-storage random-uuid)" -c "$props" >/dev/null
  fi
  KAFKA_HEAP_OPTS="-Xms256m -Xmx768m" nohup kafka-server-start "$props" >"$DATA/kafka.log" 2>&1 &
  echo $! >"$DATA/kafka.pid"
  for _ in $(seq 1 60); do
    nc -z 127.0.0.1 $KAFKA_PORT 2>/dev/null && return 0
    sleep 0.5
  done
  echo "kafka did not start; see $DATA/kafka.log" >&2
  return 1
}

stop_all() {
  "$PG_BIN/pg_ctl" -D "$DATA/pg" -m fast stop >/dev/null 2>&1 || true
  [ -f "$DATA/redis.pid" ] && kill "$(cat "$DATA/redis.pid")" 2>/dev/null || true
  [ -f "$DATA/kafka.pid" ] && kill "$(cat "$DATA/kafka.pid")" 2>/dev/null || true
  rm -f "$DATA/redis.pid" "$DATA/kafka.pid"
}

status() {
  for p in $PG_PORT $REDIS_PORT $KAFKA_PORT; do
    if nc -z 127.0.0.1 "$p" 2>/dev/null; then echo "127.0.0.1:$p up"; else echo "127.0.0.1:$p down"; fi
  done
}

case "${1:-}" in
  start) start_pg; start_redis; start_kafka; status; echo; print_env ;;
  stop) stop_all ;;
  status) status ;;
  env) print_env ;;
  *) echo "usage: $0 start|stop|status|env"; exit 2 ;;
esac
