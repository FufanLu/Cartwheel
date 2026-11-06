# Cartwheel, Illustrated

Each chapter follows one concrete request or mechanism through the code. The
tables, hex dumps, hash positions, and latencies come from running the real
binaries; none of them are made up. When a chapter names a command, that
command produced the data shown.

| # | Chapter | What you will see | Regenerate with |
|---|---|---|---|
| 00 | [The overload toolbox](00-overload-toolbox.md) | GCRA admitting and rejecting requests step by step, a retry storm with and without jitter, histogram bucket geometry and its error against exact percentiles, and a bounded queue shedding load | `build/dev/src/tools/cw-tour` |
| 01 | [Life of an add-to-cart](01-life-of-an-add-to-cart.md) | HTTP → gateway coroutine → cart-svc pool → wheel-node, the stored bytes, DVV vs vector clocks, the OR-Set fold, and real load, idempotency and shedding numbers | `build/release/src/tools/cw-trace`, `wheelctl`, `ab` |
| 02 | [Inside the LSM engine](02-inside-the-lsm-engine.md) | WAL bytes, skiplist level ratios, SSTable anatomy, live flush and compaction events, group commit, SIGKILL and power-loss tests, a fuzzer-found bug, and a head-to-head benchmark against RocksDB | `build/dev/src/tools/lsm-tour`, `lsm-bench` |
| 03 | Life of a PUT on the ring *(M3)* | key hash → preference list → quorum fan-out → hinted handoff when a node is down | |
| 04 | Life of a lightning-deal purchase *(M4)* | Redis Lua gate → Redpanda → order-worker → Postgres → WebSocket push | |

Conventions:

- Times are nanoseconds (`ns`) unless a unit is shown.
- Hardware for every measured number: Apple M3 Pro (12 cores), macOS 15, Apple
  Clang 17. Docker numbers come from Docker Desktop on the same machine.
