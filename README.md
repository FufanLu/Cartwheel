**English** | [中文](README.zh.md)

# Cartwheel

**A Dynamo-style distributed key-value store and a high-concurrency commerce stack, written in C++20.**

The 2007 Dynamo paper describes a shopping cart that must accept writes even
while servers fail and the network partitions. Cartwheel builds that system end
to end. **CartwheelKV** is a leaderless, quorum-replicated KV store with its own
LSM storage engine. On top of it run a shopping-cart service, a flash-sale
("lightning deal") pipeline that must never oversell, a C++ edge gateway, and a
React storefront with an operations console.

The cart and the deal inventory sit on opposite sides of the CAP trade-off, so
both models live in the same system:

| Data | Requirement | Mechanism |
|---|---|---|
| Shopping cart | Always writable, even during partitions (AP) | CartwheelKV: N/R/W quorum, sloppy quorum + hinted handoff, vector clocks, OR-Set CRDT merge |
| Deal inventory | Never oversell (CP) | Redis Lua atomic gate → Redpanda (Kafka) to absorb the spike → idempotent Postgres writes |

## Architecture

```mermaid
flowchart LR
  web[React storefront + ops console] --> nginx[Nginx]
  nginx --> gw["gateway<br/>Beast + Asio coroutines<br/>JWT · GCRA rate limit · load shedding"]
  gw -->|gRPC| cart[cart-svc]
  gw -->|gRPC| deal[deal-svc]
  cart -->|CartwheelKV SDK| kv
  deal --> pg[(Postgres)]
  deal --> redis[(Redis Lua gate)]
  deal --> rp[[Redpanda]]
  rp --> worker[order-worker] --> pg
  subgraph kv[CartwheelKV cluster]
    n1((wheel-node)) --- n2((wheel-node)) --- n3((wheel-node)) --- n4((wheel-node)) --- n5((wheel-node)) --- n1
  end
```

## Status

| Milestone | Scope | State |
|---|---|---|
| M0 | Toolchain, build system, overload primitives (GCRA limiter, jittered backoff, histogram, bounded pool), CI | ✅ |
| M1 | End-to-end skeleton: single node, gateway, cart-svc, minimal UI, docker-compose | ⏳ |
| M2 | LSM storage engine (WAL, skiplist memtable, SSTables, bloom filters, compaction) | |
| M3 | Distribution: consistent hashing, gossip, quorum, vector clocks, hinted handoff, Merkle anti-entropy, deterministic simulation | |
| M4 | Lightning-deal pipeline: Redis gate, Redpanda, order-worker, Postgres; gateway auth and rate limiting | |
| M5 | Storefront and ops console (hash ring, live QPS/p99, fault injection) | |
| M6 | Observability (Prometheus, Grafana, OpenTelemetry), k6 load tests, chaos experiments | |
| M7 | Helm/kind deployment, PRFAQ, runbooks | |

## Build

Prerequisites: CMake ≥ 3.25, Ninja, a C++20 compiler (Apple Clang 17 / Clang 18 / GCC 13), and
[vcpkg](https://github.com/microsoft/vcpkg) cloned to `~/vcpkg` or pointed to by `$VCPKG_ROOT`.

```bash
cmake --preset dev          # first run builds dependencies through vcpkg (gRPC takes a while)
cmake --build --preset dev
ctest --preset dev

cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan   # ThreadSanitizer
```

Presets: `dev` (Debug), `release` (RelWithDebInfo), `asan` (Address + UB), `tsan` (Thread).

## Documentation

- [Illustrated guide](docs/illustrated/README.md): walks one request at a time through the
  system, using traces captured from real runs.
- [Architecture decision records](docs/adr/)
