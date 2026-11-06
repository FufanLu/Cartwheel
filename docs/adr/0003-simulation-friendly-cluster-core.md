# ADR 0003: Cluster logic runs inside a deterministic simulator

- Status: accepted
- Date: 2026-11-06

## Context

Distributed-systems bugs, such as a lost write during a partition, or a hint
replayed twice, or a stale vector clock winning, appear only under specific
interleavings of messages, timeouts, and crashes. Tests on real sockets and
real clocks hit those interleavings by luck and cannot replay them.

FoundationDB and TigerBeetle avoid this by running the whole cluster in one
process. Inside that process, the network, the clock, and the scheduler are
simulated and driven by a seeded PRNG. Any failure reproduces exactly from its
seed.

## Decision

Code in `src/cluster/` depends only on three interfaces:

| Interface   | Production                         | Simulation                                        |
|-------------|------------------------------------|---------------------------------------------------|
| `Clock`     | `SystemClock` (steady/system time) | `ManualClock`, advanced by the simulator          |
| `Transport` | gRPC client stubs and services     | `SimNetwork`: in-memory queues with delay, drop, partition, and reorder |
| `Executor`  | Asio / thread pool                 | Single-threaded event loop in timestamp order     |

`Clock` already exists in `src/common/clock.h`. Every time-dependent primitive
(`TokenBucket`, `Backoff`, and later failure detection and hint expiry) takes a
`Clock*`, and every random choice takes a seed.

The simulation tests in `tests/sim/` run thousands of seeded scenarios and check
these invariants:

- An acknowledged write with W + R > N is never lost.
- After the network heals, replicas converge, which Merkle comparison verifies.
- Hints are delivered at most once and then deleted.

## Consequences

- Cluster code cannot call `std::chrono::now()`, spawn threads, or touch
  sockets directly. Reviews enforce this, and the simulator catches violations
  because they make runs non-deterministic.
- The gRPC adapter (`src/kv/grpc_transport.*`) is thin. Its own correctness is
  covered by integration tests on a real docker-compose cluster.
- A failing seed is a complete bug report: `ctest -R sim` prints it, and
  `CW_SIM_SEED=<seed>` replays it.
