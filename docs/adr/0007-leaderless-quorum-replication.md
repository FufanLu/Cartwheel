# ADR 0007: Leaderless quorum replication (Dynamo), not consensus

- Status: accepted
- Date: 2026-11-06

## Context

CartwheelKV stores shopping carts. The requirement from the Dynamo paper is that **a customer can
always add to their cart**, even during node failures and network partitions. Two families of
replication fit a KV store:

- **Consensus per shard** (Raft or Multi-Paxos, as in TiKV, CockroachDB, and modern DynamoDB
  partitions). It gives linearizable reads and writes and conditional updates. A shard can write
  only while a majority is reachable and a leader has been elected, so a partition that cuts the
  leader off stalls writes for an election timeout or longer.
- **Leaderless quorums** (Dynamo, Riak, Cassandra). Any replica accepts writes, the quorums R and W
  are tunable, and conflicts are reconciled. Writes keep flowing on any side of a partition that
  can reach W nodes, or any W nodes at all under sloppy quorum.

## Decision

Leaderless replication with tunable N/R/W (default 3/2/2):

- consistent hashing with 64 vnodes per node; the preference list is the first N distinct nodes
  clockwise;
- a coordinator that is always a replica, with one forwarding hop if needed, writing locally first
  and answering at W acks or R responses without waiting for slower replicas;
- sloppy quorum plus hinted handoff for dead replicas;
- read repair after every read, and round-robin Merkle-tree anti-entropy per partition;
- DVV sibling sets (ADR 0004) as the unit of replication, merged with a commutative, associative,
  idempotent `Sync`;
- membership via heartbeat gossip with incarnations: liveness is judged locally and only
  heartbeats are gossiped (never accusations), and the ring includes dead nodes, because liveness
  only affects routing.

Data that needs linearizability does not go here. The lightning-deal inventory (M4) lives in Redis
and Postgres, behind an atomic Lua gate, because "never oversell" is a CP requirement.

## Consequences

- Writes survive partitions and node loss. In the 5-node test, killing a node costs only its
  detection window (189 failed operations out of 253k), and a restart is invisible to clients
  (`docs/illustrated/03`).
- Correctness is proved by the deterministic simulator. Across seeded crash, partition, and drop
  schedules, no acknowledged write is lost, replicas converge, and hints drain.
- Applications must reconcile siblings. The cart does this with an OR-Set (ADR 0005).
- Reads are not linearizable. With R + W > N and no failures, a read sees the latest acknowledged
  write. Under sloppy quorum it can miss a write temporarily, which is the AP trade-off chosen on
  purpose for carts.
- Accepted limitations: no streaming on join, full-scan Merkle builds, and no tombstone GC. These
  are listed in illustrated/03 §7.
