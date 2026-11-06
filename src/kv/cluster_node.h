#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "cluster/membership.h"
#include "cluster/merkle.h"
#include "cluster/ring.h"
#include "cluster/runtime.h"
#include "cluster/siblings.h"
#include "common/token_bucket.h"
#include "kv/local_store.h"

namespace cartwheel::kv {

struct ClusterOptions {
  size_t n = 3;  // replicas per key
  size_t r = 2;  // default read quorum
  size_t w = 2;  // default write quorum
  int vnodes = 64;
  int64_t replica_timeout_ns = 300'000'000;
  int64_t hint_interval_ns = 500'000'000;
  int64_t anti_entropy_interval_ns = 2'000'000'000;
  cluster::MembershipOptions membership;
};

struct CoordinatorResult {
  cluster::SiblingSet siblings;
  uint32_t replicas = 0;  // acks (writes) or responses (reads) that formed the quorum
};

struct ClusterStats {
  uint64_t puts = 0;
  uint64_t gets = 0;
  uint64_t forwarded = 0;
  uint64_t quorum_failures = 0;
  uint64_t throttled = 0;
  uint64_t read_repairs = 0;
  uint64_t hints_stored = 0;
  uint64_t hints_delivered = 0;
  uint64_t anti_entropy_rounds = 0;
  uint64_t anti_entropy_in_sync = 0;
  uint64_t anti_entropy_keys_repaired = 0;
  size_t hints_pending = 0;
};

// One CartwheelKV node in a Dynamo-style cluster.
//
// Any node accepts a client request. It forwards the request to a node in
// the key's preference list, because the coordinator must also be a replica:
// DVV dots are assigned against the coordinator's local copy. The
// coordinator writes locally, ships the full sibling set to the other N-1
// targets, and answers as soon as W acks (R responses) are in, never waiting
// for the slowest replica.
//
// Failures are handled in three layers:
//   sloppy quorum   a dead home replica is replaced by the next healthy node
//                   on the ring, which keeps a hint for it
//   hinted handoff  hints are replayed to the home node once it is back
//   read repair     a read that sees a stale replica writes the merge back
//   anti-entropy    periodic Merkle-tree comparison of each partition with
//                   a random peer replica, which fixes whatever the other
//                   layers missed
//
// All I/O goes through cluster::Runtime, so the deterministic simulator
// (tests/sim) can run five of these in one thread under injected faults.
class ClusterNode final : public cluster::RequestHandler {
 public:
  ClusterNode(cluster::NodeId id, std::string address, uint64_t incarnation, LocalStore* store,
              cluster::Runtime rt, ClusterOptions options);
  ~ClusterNode() override;

  void AddSeed(const cluster::NodeId& id, const std::string& address);
  void Start();
  void Stop();  // stops timers; in-flight callbacks become no-ops

  using Done = std::function<void(Result<CoordinatorResult>)>;
  void Put(const std::string& table, const std::string& key, std::string value,
           const cluster::VectorClock& context, bool tombstone, uint32_t w, Done done);
  void Get(const std::string& table, const std::string& key, uint32_t r, Done done);

  void HandleRequest(const cluster::NodeId& from, cluster::MessageType type, std::string payload,
                     Reply reply) override;

  // Per-table provisioned throughput (DynamoDB-style); 0 = unlimited.
  void SetTableLimits(const std::string& table, double reads_per_sec, double writes_per_sec);

  [[nodiscard]] const cluster::NodeId& id() const { return id_; }
  [[nodiscard]] cluster::Membership& membership() { return membership_; }
  [[nodiscard]] ClusterStats Stats() const;
  [[nodiscard]] std::vector<cluster::NodeId> HomeReplicas(std::string_view table,
                                                          std::string_view key) const;
  [[nodiscard]] cluster::Ring RingSnapshot() const;
  [[nodiscard]] const ClusterOptions& options() const { return options_; }

  // Background work, exposed so the simulator and tests can drive it.
  void DeliverHintsOnce();
  void AntiEntropyOnce();

  static constexpr std::string_view kHintTable = "__hints";

 private:
  struct Targets {
    std::vector<cluster::NodeId> nodes;                   // sloppy preference list
    std::map<cluster::NodeId, cluster::NodeId> hint_for;  // stand-in -> dead home node
    bool includes_self = false;
  };
  [[nodiscard]] Targets ComputeTargets(std::string_view storage_key) const;
  void RefreshRing();
  bool Throttle(const std::string& table, bool write);

  void CoordinatePut(const std::string& table, const std::string& key, std::string value,
                     const cluster::VectorClock& context, bool tombstone, uint32_t w,
                     const Targets& targets, Done done);
  void CoordinateGet(const std::string& table, const std::string& key, uint32_t r,
                     const Targets& targets, Done done);
  void Forward(const cluster::NodeId& to, cluster::MessageType type, std::string payload,
               Done done) const;
  void SendReplicaWrite(const cluster::NodeId& to, const std::string& table, const std::string& key,
                        const std::string& siblings, const std::string& hint_for,
                        std::function<void(Status)> done) const;
  void StoreHint(const cluster::NodeId& home, std::string_view table, std::string_view key,
                 const cluster::SiblingSet& set);

  Result<std::string> HandleReplicaWrite(std::string_view payload);
  Result<std::string> HandleReplicaRead(std::string_view payload);
  void HandleForward(cluster::MessageType type, std::string_view payload, Reply reply);
  Result<std::string> HandleMerkleLevel(std::string_view payload);
  Result<std::string> HandleMerkleKeys(std::string_view payload);

  std::shared_ptr<const cluster::MerkleTree> TreeFor(uint32_t partition);
  void CompareLevel(const cluster::NodeId& peer, uint32_t partition, int level,
                    const std::vector<uint32_t>& indexes,
                    const std::shared_ptr<const cluster::MerkleTree>& local);
  void SyncBuckets(const cluster::NodeId& peer, uint32_t partition,
                   const std::vector<uint32_t>& buckets);

  void ScheduleHints();
  void ScheduleAntiEntropy();

  const cluster::NodeId id_;
  LocalStore* const store_;
  const cluster::Runtime rt_;
  const ClusterOptions options_;
  cluster::Membership membership_;

  mutable std::mutex mu_;  // ring_, rng_, limits_, trees_
  cluster::Ring ring_;
  std::mt19937_64 rng_;
  uint64_t ae_cursor_ = 0;
  std::map<std::string, std::pair<std::unique_ptr<TokenBucket>, std::unique_ptr<TokenBucket>>>
      limits_;
  std::map<uint32_t, std::pair<int64_t, std::shared_ptr<const cluster::MerkleTree>>> trees_;
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(false);

  struct Counters {
    std::atomic<uint64_t> puts{0}, gets{0}, forwarded{0}, quorum_failures{0}, throttled{0},
        read_repairs{0}, hints_stored{0}, hints_delivered{0}, ae_rounds{0}, ae_in_sync{0},
        ae_repaired{0};
  };
  mutable Counters counters_;
};

// "<len>target<len>table" + key, so arbitrary bytes in any part stay unambiguous.
std::string EncodeHintKey(std::string_view home, std::string_view table, std::string_view key);
bool DecodeHintKey(std::string_view encoded, std::string* home, std::string* table,
                   std::string* key);

}  // namespace cartwheel::kv
