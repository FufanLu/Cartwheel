#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "cluster/siblings.h"
#include "common/clock.h"
#include "common/histogram.h"
#include "common/status.h"
#include "storage/engine.h"

namespace cartwheel::kv {

// One node's replica of the keyspace: the DVV sibling logic on top of a
// storage engine. M1 serves client requests straight from here. In M3 the
// quorum coordinator calls the same Coordinate*/Merge* operations on N
// replicas.
//
// Concurrency: an update is a read-modify-write of the key's SiblingSet.
// Writers to the *same* key are serialized by one of kLockStripes mutexes,
// picked by key hash. Writers to different keys proceed in parallel, so a
// hot cart blocks only the few keys that hash to its stripe.
class LocalStore {
 public:
  static constexpr size_t kLockStripes = 1024;

  LocalStore(std::string node_id, std::unique_ptr<storage::Engine> engine,
             const Clock* clock = SystemClock::Default());

  struct ReadResult {
    cluster::SiblingSet siblings;  // may contain tombstones
  };

  struct WriteResult {
    // Full set after the write. M3 replicates it whole, and the client gets
    // it back (like Riak's return_body), so the context it receives, the
    // join, covers only versions it has actually been shown.
    cluster::SiblingSet siblings;
  };

  // Storage key for (table, key): "<table>\0<key>". Tables share one engine.
  [[nodiscard]] static std::string StorageKey(std::string_view table, std::string_view key);

  [[nodiscard]] Result<ReadResult> Read(std::string_view table, std::string_view key) const;

  // Coordinator-side write: assigns a new dot from this node.
  //
  // Note: the new version's own clock (context + dot) must NOT be handed to
  // the client as its next context. Collapsing dot (n,3) into a vector entry
  // n:3 would also "cover" a concurrent sibling (n,2) the client never saw.
  [[nodiscard]] Result<WriteResult> CoordinateWrite(std::string_view table, std::string_view key,
                                                    const cluster::VectorClock& context,
                                                    std::string value, bool tombstone);

  // Replica-side merge of a sibling set received from a peer (M3).
  [[nodiscard]] Result<cluster::SiblingSet> Merge(std::string_view table, std::string_view key,
                                                  const cluster::SiblingSet& remote);

  [[nodiscard]] const std::string& node_id() const noexcept { return node_id_; }
  [[nodiscard]] const storage::Engine& engine() const noexcept { return *engine_; }

  [[nodiscard]] HistogramSnapshot ReadLatency() const { return read_latency_.Snapshot(); }
  [[nodiscard]] HistogramSnapshot WriteLatency() const { return write_latency_.Snapshot(); }
  [[nodiscard]] uint64_t reads() const noexcept { return reads_.load(std::memory_order_relaxed); }
  [[nodiscard]] uint64_t writes() const noexcept { return writes_.load(std::memory_order_relaxed); }

 private:
  [[nodiscard]] std::mutex& StripeFor(std::string_view storage_key) const;
  [[nodiscard]] Result<cluster::SiblingSet> LoadLocked(const std::string& storage_key) const;

  const std::string node_id_;
  std::unique_ptr<storage::Engine> engine_;
  const Clock* clock_;
  mutable std::array<std::mutex, kLockStripes> stripes_;

  mutable Histogram read_latency_;
  Histogram write_latency_;
  mutable std::atomic<uint64_t> reads_{0};
  std::atomic<uint64_t> writes_{0};
};

}  // namespace cartwheel::kv
