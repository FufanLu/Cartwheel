# ADR 0006: A purpose-built LSM engine, benchmarked against RocksDB

- Status: accepted
- Date: 2026-11-06

## Context

A wheel-node needs an ordered, durable key-value engine. The obvious choice is RocksDB: it is
production-proven, fast, and extremely tunable. But Cartwheel exists to demonstrate the mechanisms
an engine like that is built from, and an interviewer can fairly ask "how does your storage layer
work?" Answering "RocksDB does it" ends that conversation.

## Decision

Cartwheel uses its own leveled LSM tree in `src/storage/lsm/`, behind the same `storage::Engine`
interface as the in-memory reference engine.

| component | design |
|---|---|
| WAL | LevelDB record format: 32 KiB blocks, masked CRC-32C (ARMv8/SSE4.2), FIRST/MIDDLE/LAST fragments, torn-tail tolerance |
| writes | group commit. A leader merges queued batches into one WAL record and one fsync, then inserts into the memtable outside the mutex |
| memtable | arena-allocated skiplist with a single writer and acquire/release readers |
| SSTables | 4 KiB prefix-compressed blocks with restart points, a whole-table bloom filter, an index of last-key → handle entries, and a 48-byte footer with magic `CARTWHL\x01` |
| metadata | VersionEdit records in a MANIFEST (fsync before install), with `CURRENT` replaced atomically |
| compaction | leveled (L0 trigger 4 files, L1 10 MiB, ×10 per level), round-robin file choice, trivial moves, dropping shadowed versions and bottom-level tombstones |
| concurrency | readers pin `{memtable, immutable memtable, Version}` and the snapshot sequence, then read lock-free |
| back-pressure | 1 ms slowdown per write at 8 L0 files, stop at 12, wait while the immutable memtable flushes |

RocksDB is still in the repo as an optional vcpkg feature (`bench-rocksdb`), linked only into
`lsm-bench`. It is the yardstick, not a dependency.

## Consequences

- With identical settings on 1M keys, this engine reaches 62–92% of RocksDB's throughput
  (`docs/illustrated/02`). The largest gap is random fill (62%), caused by a single compaction
  thread. The numbers are published as measured, and the known causes are listed with them.
- Correctness carries the burden RocksDB would otherwise carry. The engine is covered by:
  - a differential test against `std::map` with random reopen, flush and compaction;
  - a power-loss test, where `FaultInjectionEnv` drops unsynced bytes;
  - a 6-round SIGKILL test with 150k acknowledged keys;
  - fuzzing of every decoder, which has already found and fixed one out-of-bounds read;
  - TSan and ASan runs of all of the above.
- Out of scope, and documented as such: snapshots exposed through the API (iterators are
  snapshots), backward iteration, compression, column families, and parallel compaction.
