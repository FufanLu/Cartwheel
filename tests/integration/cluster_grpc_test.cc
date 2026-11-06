// Three wheel-nodes in one process, talking over real gRPC on localhost:
// the production PeerService transport, TimerScheduler and membership.
// Covers what the simulator cannot: protobuf over the wire, callback-API
// threading, reconnects after a node stops.

#include <chrono>
#include <future>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <grpcpp/server_builder.h>
#include <gtest/gtest.h>
#include <prometheus/registry.h>

#include "client/kv_client.h"
#include "kv/cluster_node.h"
#include "kv/kv_service.h"
#include "kv/peer_transport.h"
#include "kv/timer_scheduler.h"
#include "storage/engine.h"

namespace cartwheel::kv {
namespace {

kv::ClusterOptions FastOptions() {
  kv::ClusterOptions o;
  o.n = 3;
  o.r = 2;
  o.w = 2;
  o.vnodes = 16;
  o.replica_timeout_ns = 300'000'000;
  o.hint_interval_ns = 200'000'000;
  o.anti_entropy_interval_ns = 300'000'000;
  o.membership.gossip_interval_ns = 50'000'000;
  o.membership.suspect_after_ns = 300'000'000;
  o.membership.dead_after_ns = 700'000'000;
  return o;
}

// One node: store + cluster logic + gRPC server, on an ephemeral port.
class Node {
 public:
  Node(const std::string& id, int incarnation) : id_(id) {
    store_ = std::make_unique<LocalStore>(id, storage::NewMemEngine());
    Start(incarnation);
  }
  void Start(int incarnation) {
    transport_ = std::make_unique<GrpcPeerTransport>(id_, [this](const std::string& n) {
      return cluster_ ? cluster_->membership().AddressOf(n) : std::string();
    });
    scheduler_ = std::make_unique<TimerScheduler>();
    grpc::ServerBuilder b;
    b.AddListeningPort(port_ == 0 ? "127.0.0.1:0" : fmt::format("127.0.0.1:{}", port_),
                       grpc::InsecureServerCredentials(), &port_);
    cluster::Runtime rt{transport_.get(), scheduler_.get(), SystemClock::Default(),
                        static_cast<uint64_t>(incarnation)};
    // The address is only known after binding, so build the node after the
    // port is reserved: bind first with a placeholder service list.
    peer_proxy_ = std::make_unique<Proxy>();
    kv_proxy_registry_ = std::make_unique<prometheus::Registry>();
    b.RegisterService(&peer_proxy_->service);
    kv_ = std::make_unique<KvServiceImpl>(store_.get(), *kv_proxy_registry_, nullptr);
    server_ = b.BuildAndStart();
    cluster_ = std::make_unique<ClusterNode>(id_, address(), static_cast<uint64_t>(incarnation),
                                             store_.get(), rt, FastOptions());
    peer_proxy_->target = cluster_.get();
  }
  void Stop() {
    cluster_->Stop();
    server_->Shutdown(std::chrono::system_clock::now() + std::chrono::milliseconds(100));
    scheduler_->Stop();
    graveyard_.push_back(std::move(cluster_));
  }
  std::string address() const { return fmt::format("127.0.0.1:{}", port_); }
  ClusterNode* cluster() { return cluster_.get(); }
  LocalStore* store() { return store_.get(); }
  const std::string& id() const { return id_; }

 private:
  // Forwards PeerService calls to whatever ClusterNode is current.
  struct Proxy final : cluster::RequestHandler {
    ClusterNode* target = nullptr;
    PeerServiceImpl service{this};
    void HandleRequest(const cluster::NodeId& from, cluster::MessageType type, std::string payload,
                       Reply reply) override {
      if (target == nullptr) {
        reply(Status::Unavailable("starting"));
        return;
      }
      target->HandleRequest(from, type, std::move(payload), std::move(reply));
    }
  };

  std::string id_;
  int port_ = 0;
  std::unique_ptr<LocalStore> store_;
  std::unique_ptr<GrpcPeerTransport> transport_;
  std::unique_ptr<TimerScheduler> scheduler_;
  std::unique_ptr<Proxy> peer_proxy_;
  std::unique_ptr<prometheus::Registry> kv_proxy_registry_;
  std::unique_ptr<KvServiceImpl> kv_;
  std::unique_ptr<grpc::Server> server_;
  std::unique_ptr<ClusterNode> cluster_;
  std::vector<std::unique_ptr<ClusterNode>> graveyard_;
};

Result<CoordinatorResult> SyncPut(ClusterNode* n, const std::string& key, const std::string& v,
                                  const cluster::VectorClock& ctx = {}) {
  std::promise<Result<CoordinatorResult>> p;
  n->Put("t", key, v, ctx, false, 0,
         [&p](Result<CoordinatorResult> r) { p.set_value(std::move(r)); });
  return p.get_future().get();
}

Result<CoordinatorResult> SyncGet(ClusterNode* n, const std::string& key, uint32_t r = 0) {
  std::promise<Result<CoordinatorResult>> p;
  n->Get("t", key, r, [&p](Result<CoordinatorResult> res) { p.set_value(std::move(res)); });
  return p.get_future().get();
}

template <typename Pred>
bool WaitFor(Pred pred, int timeout_ms = 5000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return pred();
}

class ClusterGrpcTest : public ::testing::Test {
 protected:
  void SetUp() override {
    for (int i = 1; i <= 3; ++i) nodes_.push_back(std::make_unique<Node>(fmt::format("g{}", i), 1));
    for (auto& a : nodes_) {
      for (auto& b : nodes_) {
        if (a != b) a->cluster()->AddSeed(b->id(), b->address());
      }
    }
    for (auto& n : nodes_) n->cluster()->Start();
    ASSERT_TRUE(WaitFor([&] {
      for (auto& n : nodes_) {
        for (auto& m : nodes_) {
          if (!n->cluster()->membership().IsAlive(m->id())) return false;
        }
      }
      return true;
    }));
  }
  void TearDown() override {
    for (auto& n : nodes_) {
      if (n->cluster() != nullptr) n->Stop();
    }
  }
  std::vector<std::unique_ptr<Node>> nodes_;
};

TEST_F(ClusterGrpcTest, QuorumWriteIsReadableFromEveryNode) {
  auto w = SyncPut(nodes_[0]->cluster(), "cart:1", "kettle");
  ASSERT_TRUE(w.ok()) << w.status();
  EXPECT_GE(w->replicas, 2U);
  for (auto& n : nodes_) {
    auto r = SyncGet(n->cluster(), "cart:1");
    ASSERT_TRUE(r.ok()) << r.status();
    ASSERT_EQ(r->siblings.LiveCount(), 1U);
    EXPECT_EQ(r->siblings.siblings().front().value, "kettle");
  }
}

TEST_F(ClusterGrpcTest, WritesSurviveANodeStopAndReplicateAfterRestart) {
  // With N = 3 on 3 nodes every node is a replica of every key.
  nodes_[2]->Stop();
  ASSERT_TRUE(WaitFor([&] { return !nodes_[0]->cluster()->membership().IsAlive("g3"); }));
  auto w = SyncPut(nodes_[0]->cluster(), "cart:2", "mug");
  ASSERT_TRUE(w.ok()) << w.status();  // W = 2 still reachable
  EXPECT_TRUE(nodes_[2]->store()->Read("t", "cart:2")->siblings.empty());

  nodes_[2]->Start(2);  // same store and port, new incarnation
  nodes_[2]->cluster()->AddSeed("g1", nodes_[0]->address());
  nodes_[2]->cluster()->AddSeed("g2", nodes_[1]->address());
  nodes_[2]->cluster()->Start();
  // Anti-entropy (or a read repair) brings the restarted node up to date.
  EXPECT_TRUE(WaitFor(
      [&] {
        static_cast<void>(SyncGet(nodes_[0]->cluster(), "cart:2", 3));
        const auto set = nodes_[2]->store()->Read("t", "cart:2")->siblings;
        return !set.empty() && set.siblings().front().value == "mug";
      },
      8000));
}

TEST_F(ClusterGrpcTest, ConcurrentWritesThroughDifferentCoordinatorsBecomeSiblings) {
  auto base = SyncPut(nodes_[0]->cluster(), "cart:3", "base");
  ASSERT_TRUE(base.ok());
  const auto ctx = base->siblings.Join();
  std::promise<void> a_done;
  std::promise<void> b_done;
  nodes_[0]->cluster()->Put("t", "cart:3", "alice", ctx, false, 0,
                            [&](const auto&) { a_done.set_value(); });
  nodes_[1]->cluster()->Put("t", "cart:3", "bob", ctx, false, 0,
                            [&](const auto&) { b_done.set_value(); });
  a_done.get_future().wait();
  b_done.get_future().wait();
  auto r = SyncGet(nodes_[2]->cluster(), "cart:3", 3);
  ASSERT_TRUE(r.ok());
  std::set<std::string> values;
  for (const auto& s : r->siblings.siblings()) values.insert(s.value);
  EXPECT_EQ(values, (std::set<std::string>{"alice", "bob"})) << r->siblings.ToString();
}

}  // namespace
}  // namespace cartwheel::kv
