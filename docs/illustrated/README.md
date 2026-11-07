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
| 03 | [Life of a PUT on the ring](03-life-of-a-put-on-the-ring.md) | key hash → tokens → preference list, a forwarded quorum write traced message by message, sloppy quorum and a hint while a node is dead, hinted handoff, Merkle anti-entropy, a real 5-node kill/restart under YCSB load, and 5 simulator seeds of crashes and partitions | `build/dev/tests/sim/ring-tour`, `scripts/local_cluster.sh` + `wheel-bench` |
| 04 | [Life of a lightning-deal purchase](04-life-of-a-lightning-deal.md) | a decoded JWT, the Redis gate's keys and Lua script, why a hold is never released (and the bug that proved it), the batch transaction and its constraints of last resort, a 10k-buyer spike, and the same spike with a dead worker and a frozen broker | `scripts/local_infra.sh` + `deal-rush` |
| 05 | [Observing the system](05-observing-the-system.md) | an add-to-cart and a deal claim as Jaeger traces across four services, the Grafana dashboard during a partition, an alert going pending → firing → resolved, and k6 runs with a fault injected mid-run | `scripts/local_observability.sh` + `k6` |

Conventions:

- Times are nanoseconds (`ns`) unless a unit is shown.
- Hardware for every measured number: Apple M3 Pro (12 cores), macOS 15, Apple
  Clang 17. Docker numbers come from Docker Desktop on the same machine.
- The laptop was shared with other heavy workloads during the runs (a VM, an
  Android emulator, Gradle and rustc builds; load average 25–40 on 12 cores).
  Absolute throughput is therefore conservative. Comparisons run back-to-back
  in the same session, such as Cartwheel vs RocksDB, or `max_inflight=16` vs
  2048, are the meaningful part.
