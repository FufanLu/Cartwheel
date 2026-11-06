#include "kv/cluster_node.h"

#include <algorithm>

#include <spdlog/spdlog.h>

#include "cartwheel/cluster/v1/cluster.pb.h"
#include "common/coding.h"

namespace cartwheel::kv {

using cluster::NodeId;
using cluster::SiblingSet;
namespace v1 = cluster::v1;

std::string EncodeHintKey(std::string_view home, std::string_view table, std::string_view key) {
  std::string out;
  PutLengthPrefixed(&out, home);
  PutLengthPrefixed(&out, table);
  out.append(key);
  return out;
}

bool DecodeHintKey(std::string_view encoded, std::string* home, std::string* table,
                   std::string* key) {
  Decoder d(encoded);
  std::string_view h;
  std::string_view t;
  if (!d.GetLengthPrefixed(&h) || !d.GetLengthPrefixed(&t)) return false;
  *home = std::string(h);
  *table = std::string(t);
  *key = std::string(d.rest());
  return true;
}

ClusterNode::ClusterNode(NodeId id, std::string address, uint64_t incarnation, LocalStore* store,
                         cluster::Runtime rt, ClusterOptions options)
    : id_(std::move(id)),
      store_(store),
      rt_(rt),
      options_(options),
      membership_(id_, std::move(address), incarnation, rt, options.membership),
      ring_(options.vnodes),
      rng_(rt.seed ^ 0x636c7573ULL) {
  ring_.AddNode(id_);
  membership_.SetListener([this](const NodeId&, v1::Member::State) { RefreshRing(); });
}

ClusterNode::~ClusterNode() {
  Stop();
}

void ClusterNode::AddSeed(const NodeId& id, const std::string& address) {
  membership_.AddSeed(id, address);
  RefreshRing();
}

void ClusterNode::Start() {
  alive_->store(true);
  membership_.Start();
  ScheduleHints();
  ScheduleAntiEntropy();
}

void ClusterNode::Stop() {
  alive_->store(false);
  membership_.Stop();
}

void ClusterNode::RefreshRing() {
  const auto nodes = membership_.AllNodes();
  std::lock_guard lock(mu_);
  for (const auto& n : nodes) {
    if (!ring_.Contains(n)) ring_.AddNode(n);
  }
  for (const auto& n : std::set<std::string>(ring_.nodes())) {
    if (!nodes.contains(n)) ring_.RemoveNode(n);  // LEFT nodes give up their ranges
  }
}

cluster::Ring ClusterNode::RingSnapshot() const {
  std::lock_guard lock(mu_);
  return ring_;
}

std::vector<NodeId> ClusterNode::HomeReplicas(std::string_view table, std::string_view key) const {
  std::lock_guard lock(mu_);
  return ring_.PreferenceList(LocalStore::StorageKey(table, key), options_.n);
}

ClusterNode::Targets ClusterNode::ComputeTargets(std::string_view storage_key) const {
  const auto dead = membership_.DeadNodes();
  Targets t;
  std::vector<NodeId> home;
  {
    std::lock_guard lock(mu_);
    home = ring_.PreferenceList(storage_key, options_.n);
    t.nodes = ring_.PreferenceList(storage_key, options_.n, dead);
  }
  // Each stand-in (in the sloppy list but not a home replica) covers one
  // dead home replica and keeps a hint for it.
  std::vector<NodeId> missing;
  for (const auto& h : home) {
    if (std::find(t.nodes.begin(), t.nodes.end(), h) == t.nodes.end()) missing.push_back(h);
  }
  size_t next_missing = 0;
  for (const auto& n : t.nodes) {
    if (std::find(home.begin(), home.end(), n) == home.end() && next_missing < missing.size()) {
      t.hint_for[n] = missing[next_missing++];
    }
  }
  t.includes_self = std::find(t.nodes.begin(), t.nodes.end(), id_) != t.nodes.end();
  return t;
}

void ClusterNode::SetTableLimits(const std::string& table, double reads_per_sec,
                                 double writes_per_sec) {
  std::lock_guard lock(mu_);
  auto make = [this](double rate) -> std::unique_ptr<TokenBucket> {
    if (rate <= 0) return nullptr;
    return std::make_unique<TokenBucket>(rate, std::max(1.0, rate), rt_.clock);
  };
  limits_[table] = {make(reads_per_sec), make(writes_per_sec)};
}

bool ClusterNode::Throttle(const std::string& table, bool write) {
  std::lock_guard lock(mu_);
  auto it = limits_.find(table);
  if (it == limits_.end()) return false;
  TokenBucket* b = write ? it->second.second.get() : it->second.first.get();
  if (b == nullptr || b->TryAcquire()) return false;
  counters_.throttled.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// ---- client entry points -------------------------------------------------------------

void ClusterNode::Put(const std::string& table, const std::string& key, std::string value,
                      const cluster::VectorClock& context, bool tombstone, uint32_t w, Done done) {
  if (Throttle(table, /*write=*/true)) {
    done(Status::Throttled("table '" + table + "' is over its provisioned write rate"));
    return;
  }
  Targets targets = ComputeTargets(LocalStore::StorageKey(table, key));
  const uint32_t want = w == 0 ? static_cast<uint32_t>(options_.w) : w;
  if (targets.nodes.empty()) {
    done(Status::Unavailable("no live replicas"));
    return;
  }
  if (!targets.includes_self) {
    v1::ForwardRequest f;
    f.set_table(table);
    f.set_key(key);
    f.set_value(value);
    f.set_context(context.Encode());
    f.set_tombstone(tombstone);
    f.set_w(want);
    counters_.forwarded.fetch_add(1, std::memory_order_relaxed);
    Forward(targets.nodes.front(), v1::MESSAGE_TYPE_FORWARD_PUT, f.SerializeAsString(),
            std::move(done));
    return;
  }
  CoordinatePut(table, key, std::move(value), context, tombstone, want, targets, std::move(done));
}

void ClusterNode::Get(const std::string& table, const std::string& key, uint32_t r, Done done) {
  if (Throttle(table, /*write=*/false)) {
    done(Status::Throttled("table '" + table + "' is over its provisioned read rate"));
    return;
  }
  Targets targets = ComputeTargets(LocalStore::StorageKey(table, key));
  const uint32_t want = r == 0 ? static_cast<uint32_t>(options_.r) : r;
  if (targets.nodes.empty()) {
    done(Status::Unavailable("no live replicas"));
    return;
  }
  if (!targets.includes_self) {
    v1::ForwardRequest f;
    f.set_table(table);
    f.set_key(key);
    f.set_r(want);
    counters_.forwarded.fetch_add(1, std::memory_order_relaxed);
    Forward(targets.nodes.front(), v1::MESSAGE_TYPE_FORWARD_GET, f.SerializeAsString(),
            std::move(done));
    return;
  }
  CoordinateGet(table, key, want, targets, std::move(done));
}

void ClusterNode::Forward(const NodeId& to, v1::MessageType type, std::string payload,
                          Done done) const {
  // The remote coordinator needs its own replica round trips; allow for them.
  rt_.transport->Call(to, type, std::move(payload), options_.replica_timeout_ns * 2,
                      [done = std::move(done)](Result<std::string> resp) {
                        if (!resp.ok()) {
                          done(resp.status().WithContext("forward"));
                          return;
                        }
                        v1::ForwardResponse fr;
                        if (!fr.ParseFromString(*resp)) {
                          done(Status::Internal("forward: bad response"));
                          return;
                        }
                        auto set = SiblingSet::Decode(fr.siblings());
                        if (!set.ok()) {
                          done(set.status());
                          return;
                        }
                        done(CoordinatorResult{std::move(set).value(), fr.replicas()});
                      });
}

// ---- coordination -------------------------------------------------------------------------

void ClusterNode::SendReplicaWrite(const NodeId& to, const std::string& table,
                                   const std::string& key, const std::string& siblings,
                                   const std::string& hint_for,
                                   std::function<void(Status)> done) const {
  v1::ReplicaWriteRequest req;
  req.set_table(table);
  req.set_key(key);
  req.set_siblings(siblings);
  req.set_hint_for(hint_for);
  rt_.transport->Call(to, v1::MESSAGE_TYPE_REPLICA_WRITE, req.SerializeAsString(),
                      options_.replica_timeout_ns,
                      [done = std::move(done)](const Result<std::string>& r) { done(r.status()); });
}

void ClusterNode::StoreHint(const NodeId& home, std::string_view table, std::string_view key,
                            const SiblingSet& set) {
  if (store_->Merge(kHintTable, EncodeHintKey(home, table, key), set).ok()) {
    counters_.hints_stored.fetch_add(1, std::memory_order_relaxed);
  }
}

void ClusterNode::CoordinatePut(const std::string& table, const std::string& key, std::string value,
                                const cluster::VectorClock& context, bool tombstone, uint32_t w,
                                const Targets& targets, Done done) {
  counters_.puts.fetch_add(1, std::memory_order_relaxed);
  auto written = store_->CoordinateWrite(table, key, context, std::move(value), tombstone);
  if (!written.ok()) {
    done(written.status());
    return;
  }
  if (auto it = targets.hint_for.find(id_); it != targets.hint_for.end()) {
    StoreHint(it->second, table, key, written->siblings);  // the coordinator is itself a stand-in
  }
  struct State {
    std::mutex mu;
    uint32_t acks = 1;  // the local write
    uint32_t responses = 1;
    bool finished = false;
  };
  auto state = std::make_shared<State>();
  auto result = std::make_shared<CoordinatorResult>(CoordinatorResult{written->siblings, 1});
  const auto total = static_cast<uint32_t>(targets.nodes.size());
  auto shared_done = std::make_shared<Done>(std::move(done));
  auto maybe_finish = [state, result, shared_done, w, total, this]() {
    // Caller holds state->mu.
    if (state->finished) return;
    if (state->acks >= w) {
      state->finished = true;
      result->replicas = state->acks;
      (*shared_done)(*result);
    } else if (state->responses == total) {
      state->finished = true;
      counters_.quorum_failures.fetch_add(1, std::memory_order_relaxed);
      (*shared_done)(Status::Unavailable("write quorum not met: " + std::to_string(state->acks) +
                                         " of " + std::to_string(w) +
                                         " acks (the write is stored and will propagate)"));
    }
  };
  {
    std::lock_guard lock(state->mu);
    maybe_finish();  // w == 1
  }
  const std::string encoded = written->siblings.Encode();
  for (const auto& node : targets.nodes) {
    if (node == id_) continue;
    const auto hint = targets.hint_for.contains(node) ? targets.hint_for.at(node) : std::string();
    SendReplicaWrite(node, table, key, encoded, hint, [state, maybe_finish](const Status& s) {
      std::lock_guard lock(state->mu);
      ++state->responses;
      if (s.ok()) ++state->acks;
      maybe_finish();
    });
  }
}

void ClusterNode::CoordinateGet(const std::string& table, const std::string& key, uint32_t r,
                                const Targets& targets, Done done) {
  counters_.gets.fetch_add(1, std::memory_order_relaxed);
  auto local = store_->Read(table, key);
  if (!local.ok()) {
    done(local.status());
    return;
  }
  struct State {
    std::mutex mu;
    uint32_t ok = 1;
    uint32_t responses = 1;
    bool finished = false;
    SiblingSet merged;
    std::map<NodeId, std::string> seen;  // node -> encoded set, for read repair
  };
  auto state = std::make_shared<State>();
  state->merged = local->siblings;
  state->seen[id_] = local->siblings.Encode();
  const auto total = static_cast<uint32_t>(targets.nodes.size());
  auto shared_done = std::make_shared<Done>(std::move(done));
  auto alive = alive_;

  auto on_response = [this, state, shared_done, r, total, table, key, alive]() {
    // Caller holds state->mu.
    if (!state->finished && state->ok >= r) {
      state->finished = true;
      (*shared_done)(CoordinatorResult{state->merged, state->ok});
    } else if (!state->finished && state->responses == total) {
      state->finished = true;
      counters_.quorum_failures.fetch_add(1, std::memory_order_relaxed);
      (*shared_done)(Status::Unavailable("read quorum not met: " + std::to_string(state->ok) +
                                         " of " + std::to_string(r) + " replicas answered"));
    }
    if (state->responses != total || !alive->load()) return;
    // Every replica answered or timed out: repair the stale ones.
    const std::string merged = state->merged.Encode();
    for (const auto& [node, enc] : state->seen) {
      if (enc == merged) continue;
      counters_.read_repairs.fetch_add(1, std::memory_order_relaxed);
      if (node == id_) {
        static_cast<void>(store_->Merge(table, key, state->merged));
      } else {
        SendReplicaWrite(node, table, key, merged, "", [](const Status&) {});
      }
    }
  };
  {
    std::lock_guard lock(state->mu);
    on_response();  // r == 1 or a single target
  }
  v1::ReplicaReadRequest req;
  req.set_table(table);
  req.set_key(key);
  const std::string payload = req.SerializeAsString();
  for (const auto& node : targets.nodes) {
    if (node == id_) continue;
    rt_.transport->Call(node, v1::MESSAGE_TYPE_REPLICA_READ, payload, options_.replica_timeout_ns,
                        [state, on_response, node](Result<std::string> resp) {
                          std::lock_guard lock(state->mu);
                          ++state->responses;
                          if (resp.ok()) {
                            v1::ReplicaReadResponse rr;
                            if (rr.ParseFromString(*resp)) {
                              SiblingSet set;
                              if (!rr.siblings().empty()) {
                                auto decoded = SiblingSet::Decode(rr.siblings());
                                if (decoded.ok()) set = std::move(decoded).value();
                              }
                              state->merged.Sync(set);
                              state->seen[node] = set.Encode();
                              ++state->ok;
                            }
                          }
                          on_response();
                        });
  }
}

// ---- request handlers ----------------------------------------------------------------------

void ClusterNode::HandleRequest(const NodeId& /*from*/, v1::MessageType type, std::string payload,
                                Reply reply) {
  switch (type) {
    case v1::MESSAGE_TYPE_GOSSIP: reply(membership_.HandleGossip(payload)); return;
    case v1::MESSAGE_TYPE_REPLICA_WRITE: reply(HandleReplicaWrite(payload)); return;
    case v1::MESSAGE_TYPE_REPLICA_READ: reply(HandleReplicaRead(payload)); return;
    case v1::MESSAGE_TYPE_FORWARD_PUT:
    case v1::MESSAGE_TYPE_FORWARD_GET: HandleForward(type, payload, std::move(reply)); return;
    case v1::MESSAGE_TYPE_MERKLE_LEVEL: reply(HandleMerkleLevel(payload)); return;
    case v1::MESSAGE_TYPE_MERKLE_KEYS: reply(HandleMerkleKeys(payload)); return;
    default: reply(Status::Unimplemented("unknown message type")); return;
  }
}

Result<std::string> ClusterNode::HandleReplicaWrite(std::string_view payload) {
  v1::ReplicaWriteRequest req;
  if (!req.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
    return Status::InvalidArgument("replica write: bad payload");
  }
  CW_ASSIGN_OR_RETURN(SiblingSet set, SiblingSet::Decode(req.siblings()));
  // Stand-ins also keep the data in their main store, so sloppy reads find
  // it. A stale copy left behind later is harmless: DVV merges make it obsolete.
  CW_RETURN_IF_ERROR(store_->Merge(req.table(), req.key(), set).status());
  if (!req.hint_for().empty()) StoreHint(req.hint_for(), req.table(), req.key(), set);
  return v1::ReplicaWriteResponse{}.SerializeAsString();
}

Result<std::string> ClusterNode::HandleReplicaRead(std::string_view payload) {
  v1::ReplicaReadRequest req;
  if (!req.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
    return Status::InvalidArgument("replica read: bad payload");
  }
  CW_ASSIGN_OR_RETURN(auto read, store_->Read(req.table(), req.key()));
  v1::ReplicaReadResponse resp;
  if (!read.siblings.empty()) resp.set_siblings(read.siblings.Encode());
  return resp.SerializeAsString();
}

void ClusterNode::HandleForward(v1::MessageType type, std::string_view payload, Reply reply) {
  v1::ForwardRequest f;
  if (!f.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
    reply(Status::InvalidArgument("forward: bad payload"));
    return;
  }
  auto respond = [reply = std::move(reply)](Result<CoordinatorResult> r) {
    if (!r.ok()) {
      reply(r.status());
      return;
    }
    v1::ForwardResponse resp;
    resp.set_siblings(r->siblings.Encode());
    resp.set_replicas(r->replicas);
    reply(resp.SerializeAsString());
  };
  Targets targets = ComputeTargets(LocalStore::StorageKey(f.table(), f.key()));
  if (!targets.includes_self) {
    // Our views of the ring disagree; one forward hop only, never a loop.
    respond(Status::Unavailable("forward target is not a replica in its own view"));
    return;
  }
  if (type == v1::MESSAGE_TYPE_FORWARD_PUT) {
    auto ctx = cluster::VectorClock::Decode(f.context());
    if (!ctx.ok()) {
      respond(Status::InvalidArgument("forward: bad context"));
      return;
    }
    CoordinatePut(f.table(), f.key(), f.value(), *ctx, f.tombstone(), f.w(), targets,
                  std::move(respond));
  } else {
    CoordinateGet(f.table(), f.key(), f.r(), targets, std::move(respond));
  }
}

// ---- hinted handoff -------------------------------------------------------------------------

void ClusterNode::ScheduleHints() {
  auto alive = alive_;
  rt_.scheduler->After(options_.hint_interval_ns, [this, alive] {
    if (!alive->load()) return;
    DeliverHintsOnce();
    ScheduleHints();
  });
}

void ClusterNode::DeliverHintsOnce() {
  struct Hint {
    std::string storage_key;
    std::string home;
    std::string table;
    std::string key;
    std::string value;
  };
  std::vector<Hint> due;
  store_->ForEach(
      [&](std::string_view, std::string_view hint_key, std::string_view value) {
        Hint h;
        if (!DecodeHintKey(hint_key, &h.home, &h.table, &h.key)) return;
        if (!membership_.IsAlive(h.home)) return;
        h.storage_key = std::string(hint_key);
        h.value = std::string(value);
        due.push_back(std::move(h));
      },
      kHintTable);
  auto alive = alive_;
  for (auto& h : due) {
    SendReplicaWrite(h.home, h.table, h.key, h.value, "", [this, alive, h](const Status& s) {
      if (!s.ok() || !alive->load()) return;  // retried next round
      // Delete only if the hint did not grow while in flight.
      auto now = store_->Read(kHintTable, h.storage_key);
      if (now.ok() && now->siblings.Encode() == h.value) {
        static_cast<void>(store_->DeleteKey(kHintTable, h.storage_key));
        counters_.hints_delivered.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
}

// ---- anti-entropy ---------------------------------------------------------------------------

void ClusterNode::ScheduleAntiEntropy() {
  auto alive = alive_;
  rt_.scheduler->After(options_.anti_entropy_interval_ns, [this, alive] {
    if (!alive->load()) return;
    AntiEntropyOnce();
    ScheduleAntiEntropy();
  });
}

std::shared_ptr<const cluster::MerkleTree> ClusterNode::TreeFor(uint32_t partition) {
  const int64_t now = rt_.clock->NowNanos();
  {
    std::lock_guard lock(mu_);
    // A round asks for up to three levels within milliseconds; reuse one scan.
    if (auto it = trees_.find(partition);
        it != trees_.end() && now - it->second.first < 100'000'000) {
      return it->second.second;
    }
  }
  const cluster::Ring ring = RingSnapshot();
  auto tree = std::make_shared<cluster::MerkleTree>();
  store_->ForEach([&](std::string_view table, std::string_view key, std::string_view value) {
    if (table == kHintTable) return;
    const std::string skey = LocalStore::StorageKey(table, key);
    if (ring.PartitionOf(skey) == partition) tree->Add(skey, value);
  });
  tree->Finalize();
  std::lock_guard lock(mu_);
  trees_[partition] = {now, tree};
  return tree;
}

void ClusterNode::AntiEntropyOnce() {
  const cluster::Ring ring = RingSnapshot();
  if (ring.partitions() == 0) return;
  // Walk this node's partitions round-robin, so every range is compared
  // within (partitions / rate) rounds. The peer rotates too.
  std::vector<uint32_t> mine;
  for (uint32_t p = 0; p < ring.partitions(); ++p) {
    const auto replicas = ring.PartitionReplicas(p, options_.n);
    if (std::find(replicas.begin(), replicas.end(), id_) != replicas.end()) mine.push_back(p);
  }
  if (mine.empty()) return;
  uint32_t partition = 0;
  NodeId peer;
  {
    std::lock_guard lock(mu_);
    partition = mine[ae_cursor_++ % mine.size()];
    auto replicas = ring.PartitionReplicas(partition, options_.n);
    std::erase(replicas, id_);
    std::erase_if(replicas, [this](const NodeId& n) { return !membership_.IsAlive(n); });
    if (replicas.empty()) return;
    peer = replicas[(ae_cursor_ / mine.size()) % replicas.size()];
  }
  counters_.ae_rounds.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard lock(mu_);
    trees_.erase(partition);  // compare against a fresh scan
  }
  CompareLevel(peer, partition, 0, {0}, TreeFor(partition));
}

void ClusterNode::CompareLevel(const NodeId& peer, uint32_t partition, int level,
                               const std::vector<uint32_t>& indexes,
                               const std::shared_ptr<const cluster::MerkleTree>& local) {
  v1::MerkleLevelRequest req;
  req.set_partition(partition);
  req.set_level(static_cast<uint32_t>(level));
  for (const uint32_t i : indexes) req.add_indexes(i);
  auto alive = alive_;
  rt_.transport->Call(
      peer, v1::MESSAGE_TYPE_MERKLE_LEVEL, req.SerializeAsString(), options_.replica_timeout_ns,
      [this, alive, peer, partition, level, indexes, local](Result<std::string> resp) {
        if (!resp.ok() || !alive->load()) return;
        v1::MerkleLevelResponse r;
        if (!r.ParseFromString(*resp) || r.hashes_size() != static_cast<int>(indexes.size()))
          return;
        std::vector<uint32_t> differing;
        for (size_t i = 0; i < indexes.size(); ++i) {
          if (r.hashes(static_cast<int>(i)) != local->Hash(level, indexes[i])) {
            differing.push_back(indexes[i]);
          }
        }
        if (differing.empty()) {
          if (level == 0) counters_.ae_in_sync.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        if (level == cluster::MerkleTree::kLevels - 1) {
          SyncBuckets(peer, partition, differing);
          return;
        }
        std::vector<uint32_t> children;
        for (const uint32_t d : differing) {
          for (uint32_t c = 0; c < cluster::MerkleTree::kFanout; ++c) {
            children.push_back(d * cluster::MerkleTree::kFanout + c);
          }
        }
        CompareLevel(peer, partition, level + 1, children, local);
      });
}

void ClusterNode::SyncBuckets(const NodeId& peer, uint32_t partition,
                              const std::vector<uint32_t>& buckets) {
  v1::MerkleKeysRequest req;
  req.set_partition(partition);
  for (const uint32_t b : buckets) req.add_buckets(b);
  auto alive = alive_;
  rt_.transport->Call(
      peer, v1::MESSAGE_TYPE_MERKLE_KEYS, req.SerializeAsString(), options_.replica_timeout_ns,
      [this, alive, peer, partition, buckets](Result<std::string> resp) {
        if (!resp.ok() || !alive->load()) return;
        v1::MerkleKeysResponse r;
        if (!r.ParseFromString(*resp)) return;
        // Pull: merge the peer's versions of every key in the differing buckets.
        for (const auto& e : r.entries()) {
          auto set = SiblingSet::Decode(e.siblings());
          if (!set.ok()) continue;
          auto before = store_->Read(e.table(), e.key());
          auto after = store_->Merge(e.table(), e.key(), *set);
          if (before.ok() && after.ok() && before->siblings.Encode() != after->Encode()) {
            counters_.ae_repaired.fetch_add(1, std::memory_order_relaxed);
          }
        }
        // Push: send ours for the same buckets, so the peer converges too.
        const cluster::Ring ring = RingSnapshot();
        const std::set<uint32_t> wanted(buckets.begin(), buckets.end());
        store_->ForEach([&](std::string_view table, std::string_view key, std::string_view value) {
          if (table == kHintTable) return;
          const std::string skey = LocalStore::StorageKey(table, key);
          if (ring.PartitionOf(skey) != partition ||
              !wanted.contains(cluster::MerkleTree::BucketOf(skey))) {
            return;
          }
          SendReplicaWrite(peer, std::string(table), std::string(key), std::string(value), "",
                           [](const Status&) {});
        });
        std::lock_guard lock(mu_);
        trees_.erase(partition);
      });
}

Result<std::string> ClusterNode::HandleMerkleLevel(std::string_view payload) {
  v1::MerkleLevelRequest req;
  if (!req.ParseFromArray(payload.data(), static_cast<int>(payload.size())) ||
      req.level() >= static_cast<uint32_t>(cluster::MerkleTree::kLevels)) {
    return Status::InvalidArgument("merkle: bad request");
  }
  const auto tree = TreeFor(req.partition());
  v1::MerkleLevelResponse resp;
  const uint32_t nodes = cluster::MerkleTree::NodesAt(static_cast<int>(req.level()));
  for (const uint32_t i : req.indexes()) {
    resp.add_hashes(i < nodes ? tree->Hash(static_cast<int>(req.level()), i) : 0);
  }
  return resp.SerializeAsString();
}

Result<std::string> ClusterNode::HandleMerkleKeys(std::string_view payload) {
  v1::MerkleKeysRequest req;
  if (!req.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
    return Status::InvalidArgument("merkle keys: bad request");
  }
  const std::set<uint32_t> wanted(req.buckets().begin(), req.buckets().end());
  const cluster::Ring ring = RingSnapshot();
  v1::MerkleKeysResponse resp;
  store_->ForEach([&](std::string_view table, std::string_view key, std::string_view value) {
    if (table == kHintTable) return;
    const std::string skey = LocalStore::StorageKey(table, key);
    if (ring.PartitionOf(skey) != req.partition() ||
        !wanted.contains(cluster::MerkleTree::BucketOf(skey))) {
      return;
    }
    auto* e = resp.add_entries();
    e->set_table(std::string(table));
    e->set_key(std::string(key));
    e->set_siblings(std::string(value));
  });
  return resp.SerializeAsString();
}

ClusterStats ClusterNode::Stats() const {
  ClusterStats s;
  s.puts = counters_.puts.load();
  s.gets = counters_.gets.load();
  s.forwarded = counters_.forwarded.load();
  s.quorum_failures = counters_.quorum_failures.load();
  s.throttled = counters_.throttled.load();
  s.read_repairs = counters_.read_repairs.load();
  s.hints_stored = counters_.hints_stored.load();
  s.hints_delivered = counters_.hints_delivered.load();
  s.anti_entropy_rounds = counters_.ae_rounds.load();
  s.anti_entropy_in_sync = counters_.ae_in_sync.load();
  s.anti_entropy_keys_repaired = counters_.ae_repaired.load();
  store_->ForEach([&](std::string_view, std::string_view, std::string_view) { ++s.hints_pending; },
                  kHintTable);
  return s;
}

}  // namespace cartwheel::kv
