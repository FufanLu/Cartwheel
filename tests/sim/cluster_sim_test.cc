// Five-node CartwheelKV cluster under a seeded storm of faults:
// message loss, latency jitter, partitions, node crashes and restarts.
// After healing, the invariants that make it a Dynamo-style store must hold:
//
//   1. No acknowledged write is lost: its dot is still a sibling, or a
//      surviving sibling's causal context covers it (a later write saw it).
//   2. Convergence: the N home replicas of every key hold identical
//      sibling sets, via hinted handoff, read repair and Merkle anti-entropy.
//   3. Every hint is delivered and deleted.
//   4. Determinism: the same seed yields a byte-identical final state.

#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gtest/gtest.h>

#include "kv/cluster_node.h"
#include "kv/local_store.h"
#include "sim.h"
#include "storage/engine.h"

namespace cartwheel::sim {
namespace {

using kv::ClusterNode;
using kv::LocalStore;

struct AckedWrite {
  std::string key;
  std::string value;
  cluster::Dot dot;
};

struct RunReport {
  std::string final_state;  // concatenated encodings, for the determinism check
  uint64_t acked = 0;
  uint64_t failed = 0;
  uint64_t crashes = 0;
  uint64_t partitions = 0;
  uint64_t messages = 0;
  uint64_t dropped = 0;
  kv::ClusterStats totals;
};

class SimCluster {
 public:
  SimCluster(uint64_t seed, int nodes) : world_(seed), net_(&world_) {
    for (int i = 1; i <= nodes; ++i) ids_.push_back(fmt::format("n{}", i));
    for (const auto& id : ids_) {
      stores_[id] = std::make_unique<LocalStore>(id, storage::NewMemEngine(), &world_.clock);
      endpoints_[id] = std::make_unique<Network::Endpoint>(&net_, id);
      Boot(id);
    }
  }

  kv::ClusterOptions Options() const {
    kv::ClusterOptions o;
    o.n = 3;
    o.r = 2;
    o.w = 2;
    o.vnodes = 16;
    o.replica_timeout_ns = 60'000'000;
    o.hint_interval_ns = 250'000'000;
    o.anti_entropy_interval_ns = 40'000'000;
    o.membership.gossip_interval_ns = 100'000'000;
    o.membership.suspect_after_ns = 500'000'000;
    o.membership.dead_after_ns = 1'200'000'000;
    o.membership.rpc_timeout_ns = 60'000'000;
    return o;
  }

  // (Re)start a node on its durable store with a new incarnation.
  void Boot(const NodeId& id) {
    auto alive = std::make_shared<bool>(true);
    alive_[id] = alive;
    schedulers_.push_back(std::make_unique<NodeScheduler>(&world_, alive));
    cluster::Runtime rt{endpoints_[id].get(), schedulers_.back().get(), &world_.clock,
                        world_.rng()};
    auto node =
        std::make_unique<ClusterNode>(id, id, ++incarnation_[id], stores_[id].get(), rt, Options());
    for (const auto& peer : ids_) {
      if (peer != id) node->AddSeed(peer, peer);
    }
    net_.Register(id, node.get());
    node->Start();
    nodes_[id] = node.get();
    graveyard_.push_back(std::move(node));  // never destroyed mid-run: callbacks may still hold it
  }

  void Crash(const NodeId& id) {
    *alive_[id] = false;
    nodes_[id]->Stop();
    net_.SetUp(id, false);
    down_.insert(id);
  }

  void Restart(const NodeId& id) {
    net_.SetUp(id, true);
    down_.erase(id);
    Boot(id);
  }

  World& world() { return world_; }
  Network& net() { return net_; }
  const std::vector<NodeId>& ids() const { return ids_; }
  const std::set<NodeId>& down() const { return down_; }
  ClusterNode* node(const NodeId& id) { return nodes_.at(id); }
  LocalStore* store(const NodeId& id) { return stores_.at(id).get(); }

 private:
  World world_;
  Network net_;
  std::vector<NodeId> ids_;
  std::map<NodeId, std::unique_ptr<LocalStore>> stores_;
  std::map<NodeId, std::unique_ptr<Network::Endpoint>> endpoints_;
  std::map<NodeId, std::shared_ptr<bool>> alive_;
  std::map<NodeId, uint64_t> incarnation_;
  std::vector<std::unique_ptr<NodeScheduler>> schedulers_;
  std::map<NodeId, ClusterNode*> nodes_;
  std::vector<std::unique_ptr<ClusterNode>> graveyard_;
  std::set<NodeId> down_;
};

RunReport RunScenario(uint64_t seed, bool verbose = false) {
  SimCluster c(seed, 5);
  World& w = c.world();
  RunReport report;
  std::vector<AckedWrite> acked;
  constexpr int kKeys = 40;
  constexpr int kClients = 6;
  std::map<std::pair<int, std::string>, std::string> client_context;  // (client, key) -> context

  w.RunFor(1'000'000'000);  // let gossip converge

  // ---- workload with faults: 12 simulated seconds ----
  uint64_t op = 0;
  bool workload = true;
  std::function<void(int)> client = [&](int id) {
    if (!workload) return;
    const std::string key = fmt::format("k{}", w.Uniform(kKeys));
    // Coordinator: any live node (clients sit behind a load balancer).
    std::vector<NodeId> live;
    for (const auto& n : c.ids()) {
      if (!c.down().contains(n)) live.push_back(n);
    }
    ClusterNode* coord = c.node(live[w.Uniform(live.size())]);
    if (w.Chance(0.55)) {
      const std::string value = fmt::format("c{}-op{}", id, op++);
      cluster::VectorClock ctx;
      if (!w.Chance(0.2)) {  // usually read-modify-write; sometimes blind
        auto it = client_context.find({id, key});
        if (it != client_context.end()) ctx = cluster::VectorClock::Decode(it->second).value();
      }
      coord->Put("t", key, value, ctx, false, 0,
                 [&, id, key, value](Result<kv::CoordinatorResult> r) {
                   if (!r.ok()) {
                     ++report.failed;
                     return;
                   }
                   for (const auto& s : r->siblings.siblings()) {
                     if (s.value == value) acked.push_back({key, value, s.dot});
                   }
                   client_context[{id, key}] = r->siblings.Join().Encode();
                   ++report.acked;
                 });
    } else {
      coord->Get("t", key, 0, [&, id, key](Result<kv::CoordinatorResult> r) {
        if (r.ok()) client_context[{id, key}] = r->siblings.Join().Encode();
      });
    }
    w.After(2'000'000 + static_cast<int64_t>(w.Uniform(8'000'000)), [&client, id] { client(id); });
  };
  for (int i = 0; i < kClients; ++i) {
    w.After(static_cast<int64_t>(w.Uniform(5'000'000)), [&client, i] { client(i); });
  }

  // Faults: every 1-2 s pick one.
  std::function<void()> chaos = [&] {
    if (!workload) return;
    const uint64_t dice = w.Uniform(100);
    if (dice < 30 && c.down().size() < 2) {
      const NodeId victim = c.ids()[w.Uniform(c.ids().size())];
      if (!c.down().contains(victim)) {
        c.Crash(victim);
        ++report.crashes;
        if (verbose) fmt::print("t={:.2f}s crash {}\n", static_cast<double>(w.now()) / 1e9, victim);
      }
    } else if (dice < 55 && !c.down().empty()) {
      const NodeId back = *c.down().begin();
      c.Restart(back);
      if (verbose) fmt::print("t={:.2f}s restart {}\n", static_cast<double>(w.now()) / 1e9, back);
    } else if (dice < 75 && !c.net().Partitioned()) {
      std::set<NodeId> side;
      for (const auto& n : c.ids()) {
        if (w.Chance(0.4)) side.insert(n);
      }
      if (!side.empty() && side.size() < c.ids().size()) {
        c.net().Partition(side);
        ++report.partitions;
        if (verbose)
          fmt::print("t={:.2f}s partition {} | rest\n", static_cast<double>(w.now()) / 1e9,
                     fmt::join(side, ","));
      }
    } else if (c.net().Partitioned()) {
      c.net().Heal();
      if (verbose) fmt::print("t={:.2f}s heal\n", static_cast<double>(w.now()) / 1e9);
    }
    c.net().drop_rate = w.Chance(0.5) ? 0.02 : 0.0;
    w.After(1'000'000'000 + static_cast<int64_t>(w.Uniform(1'000'000'000)), chaos);
  };
  w.After(500'000'000, chaos);
  w.RunFor(12'000'000'000);

  // ---- heal everything and let the repair mechanisms work ----
  workload = false;
  c.net().Heal();
  c.net().drop_rate = 0;
  for (const auto& id : std::set<NodeId>(c.down())) c.Restart(id);
  w.RunFor(15'000'000'000);

  // ---- invariants ----
  for (int k = 0; k < kKeys; ++k) {
    const std::string key = fmt::format("k{}", k);
    const auto home = c.node(c.ids().front())->HomeReplicas("t", key);
    std::string first;
    for (const auto& replica : home) {
      const std::string enc = c.store(replica)->Read("t", key).value().siblings.Encode();
      if (replica == home.front()) {
        first = enc;
        report.final_state += enc;
      } else {
        EXPECT_EQ(enc, first) << "seed " << seed << ": replicas of " << key << " diverged ("
                              << home.front() << " vs " << replica << ")";
      }
    }
  }
  for (const auto& a : acked) {
    const auto set = c.store(c.node(c.ids().front())->HomeReplicas("t", a.key).front())
                         ->Read("t", a.key)
                         .value()
                         .siblings;
    bool present = false;
    bool covered = false;
    for (const auto& s : set.siblings()) {
      if (s.dot == a.dot) present = true;
      if (a.dot.node == s.dot.node && a.dot.counter == s.dot.counter) present = true;
      if (s.context.Get(a.dot.node) >= a.dot.counter) covered = true;
    }
    EXPECT_TRUE(present || covered)
        << "seed " << seed << ": acknowledged write " << a.value << " (" << a.dot.node << ","
        << a.dot.counter << ") on " << a.key << " was lost: " << set.ToString();
  }
  for (const auto& id : c.ids()) {
    const auto s = c.node(id)->Stats();
    EXPECT_EQ(s.hints_pending, 0U) << "seed " << seed << ": " << id << " still holds hints";
    report.totals.read_repairs += s.read_repairs;
    report.totals.hints_stored += s.hints_stored;
    report.totals.hints_delivered += s.hints_delivered;
    report.totals.anti_entropy_rounds += s.anti_entropy_rounds;
    report.totals.anti_entropy_keys_repaired += s.anti_entropy_keys_repaired;
    report.totals.forwarded += s.forwarded;
    report.totals.quorum_failures += s.quorum_failures;
  }
  report.messages = c.net().sent;
  report.dropped = c.net().dropped;
  return report;
}

uint64_t SeedFromEnv() {
  const char* s = std::getenv("CW_SIM_SEED");  // NOLINT(concurrency-mt-unsafe)
  return s != nullptr ? std::strtoull(s, nullptr, 10) : 0;
}

TEST(ClusterSimTest, InvariantsHoldUnderFaults) {
  const uint64_t pinned = SeedFromEnv();
  const std::vector<uint64_t> seeds =
      pinned != 0 ? std::vector<uint64_t>{pinned} : std::vector<uint64_t>{1, 2, 3, 4, 5};
  for (const uint64_t seed : seeds) {
    const RunReport r = RunScenario(seed, pinned != 0);
    fmt::print(
        "seed {}: {} acked / {} failed writes, {} crashes, {} partitions, {} msgs ({} dropped);"
        " {} hint writes handed off as {} entries, {} read repairs, {} AE rounds repaired {} "
        "keys\n",
        seed, r.acked, r.failed, r.crashes, r.partitions, r.messages, r.dropped,
        r.totals.hints_stored, r.totals.hints_delivered, r.totals.read_repairs,
        r.totals.anti_entropy_rounds, r.totals.anti_entropy_keys_repaired);
    EXPECT_GT(r.acked, 300U) << "seed " << seed << ": workload barely ran";
    if (::testing::Test::HasFailure()) {
      fmt::print("replay with: CW_SIM_SEED={} ctest -R ClusterSim --output-on-failure\n", seed);
      return;
    }
  }
}

TEST(ClusterSimTest, SameSeedSameOutcome) {
  const RunReport a = RunScenario(42);
  const RunReport b = RunScenario(42);
  EXPECT_EQ(a.final_state, b.final_state);
  EXPECT_EQ(a.messages, b.messages);
  EXPECT_EQ(a.acked, b.acked);
}

}  // namespace
}  // namespace cartwheel::sim
