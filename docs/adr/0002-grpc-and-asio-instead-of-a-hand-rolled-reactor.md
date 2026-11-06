# ADR 0002: gRPC and Boost.Asio instead of a hand-rolled reactor

- Status: accepted
- Date: 2026-11-06

## Context

A common C++ portfolio project is a hand-written epoll reactor in the style of
muduo, with an HTTP parser and timer wheel on top. It shows that the author
knows `epoll_wait`. Production C++ shops rarely ship one: they use gRPC,
Boost.Asio, Folly/Wangle, Seastar, or an in-house framework, and spend their
engineering effort on the problem domain.

Cartwheel's hard problems are storage and distribution: an LSM engine, quorum
replication, failure detection, anti-entropy, and overload control. Building the
network layer too would add months of work and no differentiation.

## Decision

- **Node-to-node and service-to-service RPC use gRPC with the callback API.**
  This gives HTTP/2 multiplexing, deadlines propagated across hops,
  cancellation, and TLS. Protobuf schemas in `proto/` are the contract between
  services.
- **The edge gateway uses Boost.Beast on Boost.Asio with C++20 coroutines**
  (`asio::awaitable`). Each core runs one `io_context`, and each connection runs
  one coroutine. Asio uses kqueue on macOS and epoll on Linux, and we inherit
  both.
- **Where hand-written code goes instead:** the storage engine (WAL, concurrent
  skiplist, SSTables, compaction), the replication protocol, membership, and
  overload primitives (lock-free GCRA limiter, bounded queues, jittered backoff).

## Consequences

- Networking bugs and performance are the libraries' responsibility.
  Interview discussion moves to the design trade-offs that matter for a
  Dynamo-style system.
- gRPC pulls in a large dependency tree. ADR 0001 keeps the build cost
  manageable.
- Cluster logic must not depend on gRPC types directly, or it could not be
  tested deterministically. ADR 0003 handles this.
