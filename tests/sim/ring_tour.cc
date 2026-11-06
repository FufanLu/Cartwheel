// ring-tour: one key's life on a simulated 5-node ring, every message traced.
// The simulator runs the production ClusterNode and LocalStore code; only the
// network and clock are simulated. Output feeds docs/illustrated/03.

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "cluster/ring.h"
#include "kv/cluster_node.h"
#include "kv/local_store.h"
#include "sim.h"
#include "storage/engine.h"

using namespace cartwheel;
using namespace cartwheel::sim;

namespace {

std::string TypeName(cluster::MessageType t) {
  std::string n = cluster::v1::MessageType_Name(t);
  return n.substr(std::string("MESSAGE_TYPE_").size());
}

struct Tour {
  World world{7};
  Network net{&world};
  std::vector<std::string> ids{"n1", "n2", "n3", "n4", "n5"};
  std::map<std::string, std::unique_ptr<kv::LocalStore>> stores;
  std::map<std::string, std::unique_ptr<Network::Endpoint>> endpoints;
  std::map<std::string, std::shared_ptr<bool>> alive;
  std::vector<std::unique_ptr<NodeScheduler>> schedulers;
  std::map<std::string, kv::ClusterNode*> nodes;
  std::vector<std::unique_ptr<kv::ClusterNode>> keep;
  bool tracing = false;
  uint64_t incarnation = 0;

  kv::ClusterOptions Options() {
    kv::ClusterOptions o;
    o.vnodes = 8;
    o.replica_timeout_ns = 50'000'000;
    o.hint_interval_ns = 200'000'000;
    o.anti_entropy_interval_ns = 3'600'000'000'000;  // off: show hints and read repair alone
    o.membership.gossip_interval_ns = 100'000'000;
    o.membership.suspect_after_ns = 400'000'000;
    o.membership.dead_after_ns = 900'000'000;
    return o;
  }

  void Boot(const std::string& id) {
    auto flag = std::make_shared<bool>(true);
    alive[id] = flag;
    schedulers.push_back(std::make_unique<NodeScheduler>(&world, flag));
    cluster::Runtime rt{endpoints[id].get(), schedulers.back().get(), &world.clock, 11};
    auto n =
        std::make_unique<kv::ClusterNode>(id, id, ++incarnation, stores[id].get(), rt, Options());
    for (const auto& p : ids) {
      if (p != id) n->AddSeed(p, p);
    }
    net.Register(id, n.get());
    n->Start();
    nodes[id] = n.get();
    keep.push_back(std::move(n));
  }

  Tour() {
    for (const auto& id : ids) {
      stores[id] = std::make_unique<kv::LocalStore>(id, storage::NewMemEngine(), &world.clock);
      endpoints[id] = std::make_unique<Network::Endpoint>(&net, id);
    }
    for (const auto& id : ids) Boot(id);
    net.trace = [this](const std::string& ev, const std::string& from, const std::string& to,
                       cluster::MessageType type) {
      if (!tracing || type == cluster::v1::MESSAGE_TYPE_GOSSIP) return;
      fmt::print("  {:>8.3f} ms  {:<5} {} -> {}  {}\n", Ms(), ev, from, to, TypeName(type));
    };
  }

  double t0 = 0;
  double Ms() const { return static_cast<double>(world.now()) / 1e6 - t0; }
  void Mark(const std::string& what) {
    t0 = static_cast<double>(world.now()) / 1e6;
    fmt::print("\n{}\n", what);
  }

  void Put(const std::string& coord, const std::string& value, const std::string& ctx_enc) {
    auto ctx =
        ctx_enc.empty() ? cluster::VectorClock{} : cluster::VectorClock::Decode(ctx_enc).value();
    nodes[coord]->Put(
        "carts", "ada", value, ctx, false, 0, [this](Result<kv::CoordinatorResult> r) {
          if (r.ok()) {
            fmt::print("  {:>8.3f} ms  ACK   client <- coordinator: {} acks, siblings {}\n", Ms(),
                       r->replicas, r->siblings.ToString());
          } else {
            fmt::print("  {:>8.3f} ms  FAIL  {}\n", Ms(), r.status().ToString());
          }
        });
  }

  void Dump(const std::string& label) {
    fmt::print("\n{}:\n", label);
    for (const auto& id : ids) {
      const auto set = stores[id]->Read("carts", "ada").value().siblings;
      size_t hints = 0;
      stores[id]->ForEach([&](std::string_view, std::string_view, std::string_view) { ++hints; },
                          kv::ClusterNode::kHintTable);
      fmt::print("  {}  {}{}\n", id, set.empty() ? std::string("(nothing)") : set.ToString(),
                 hints > 0 ? fmt::format("   + {} hint(s)", hints) : std::string());
    }
  }
};

}  // namespace

int main() {
  Tour t;
  t.world.RunFor(1'000'000'000);
  const std::string skey = kv::LocalStore::StorageKey("carts", "ada");
  const auto ring = t.nodes["n1"]->RingSnapshot();
  const uint64_t pos = cluster::RingHash(skey);
  fmt::print("## The key on the ring\n\n");
  fmt::print("storage key \"carts\\0ada\" -> RingHash {:#018x} ({:.4f} of the ring)\n", pos,
             static_cast<double>(pos) / 18446744073709551616.0);
  fmt::print("ring: 5 nodes x 8 vnodes = {} tokens; ownership:", ring.tokens().size());
  for (const auto& [n, share] : ring.Ownership()) fmt::print(" {} {:.1f}%", n, share * 100);
  const auto home = ring.PreferenceList(skey, 3);
  fmt::print("\ntokens clockwise from the key:\n");
  const auto& toks = ring.tokens();
  size_t start = ring.PartitionOf(skey);
  for (size_t i = 0; i < 6; ++i) {
    const auto& tok = toks[(start + i) % toks.size()];
    fmt::print("  {:#018x}  {}\n", tok.position, tok.node);
  }
  fmt::print("preference list (first 3 distinct nodes): {}\n", fmt::join(home, " -> "));

  std::string coordinator_outside;
  for (const auto& id : t.ids) {
    if (std::find(home.begin(), home.end(), id) == home.end()) {
      coordinator_outside = id;
      break;
    }
  }
  t.tracing = true;
  t.Mark(fmt::format("## 1. PUT arriving at {}, which is not a replica of the key",
                     coordinator_outside));
  t.Put(coordinator_outside, "kettle", "");
  t.world.RunFor(200'000'000);
  t.Dump("replicas after the write");

  const std::string& victim = home[1];
  t.Mark(fmt::format("## 2. {} (a home replica) crashes; the next write uses a stand-in", victim));
  t.tracing = false;
  *t.alive[victim] = false;
  t.nodes[victim]->Stop();
  t.net.SetUp(victim, false);
  t.world.RunFor(1'200'000'000);  // others declare it dead
  t.Mark("PUT (context = previous write) while it is down:");
  t.tracing = true;
  const auto ctx = t.stores[home[0]]->Read("carts", "ada").value().siblings.Join().Encode();
  t.Put(home[0], "kettle+mug", ctx);
  t.world.RunFor(150'000'000);
  t.Dump(fmt::format("replicas while {} is down", victim));

  t.Mark(fmt::format("## 3. {} restarts; the stand-in replays its hint", victim));
  t.net.SetUp(victim, true);
  t.Boot(victim);
  t.world.RunFor(800'000'000);
  t.tracing = false;
  t.Dump("replicas after hinted handoff");
  return 0;
}
