# Cartwheel, Illustrated

Each chapter follows one concrete request or mechanism through the code. The
tables, hex dumps, hash positions, and latencies come from running the real
binaries; none of them are made up. When a chapter names a command, that
command produced the data shown.

| # | Chapter | What you will see | Regenerate with |
|---|---|---|---|
| 00 | [The overload toolbox](00-overload-toolbox.md) | GCRA admitting and rejecting requests step by step, a retry storm with and without jitter, histogram bucket geometry and its error against exact percentiles, and a bounded queue shedding load | `build/dev/src/tools/cw-tour` |
| 01 | Life of a PUT on one node *(M1)* | HTTP → gateway → gRPC → wheel-node → storage engine | |
| 02 | Inside the LSM engine *(M2)* | WAL record bytes, the memtable skiplist, SSTable layout, a compaction | |
| 03 | Life of a PUT on the ring *(M3)* | key hash → preference list → quorum fan-out → hinted handoff when a node is down | |
| 04 | Life of a lightning-deal purchase *(M4)* | Redis Lua gate → Redpanda → order-worker → Postgres → WebSocket push | |

Conventions:

- Times are nanoseconds (`ns`) unless a unit is shown.
- Hardware for every measured number: Apple M3 Pro (12 cores), macOS 15, Apple
  Clang 17. Docker numbers come from Docker Desktop on the same machine.
