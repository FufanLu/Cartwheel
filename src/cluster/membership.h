#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "cartwheel/cluster/v1/cluster.pb.h"
#include "cluster/runtime.h"

namespace cartwheel::cluster {

struct MembershipOptions {
  int64_t gossip_interval_ns = 200'000'000;  // 200 ms
  int fanout = 2;                            // peers contacted per tick
  int64_t suspect_after_ns = 1'000'000'000;  // heartbeat silent for 1 s
  int64_t dead_after_ns = 3'000'000'000;     // ... for 3 s: stop routing to it
  int64_t rpc_timeout_ns = 150'000'000;
};

// Gossip membership with heartbeat failure detection (Cassandra/Dynamo style).
//
// Every tick a node bumps its own heartbeat and push-pulls its member table
// with `fanout` random peers. A member whose heartbeat has not advanced for
// suspect_after is SUSPECT, and after dead_after it is DEAD. Each node judges
// liveness from what it hears itself; gossip carries heartbeats, never
// accusations, so one node's flaky link cannot get a healthy peer evicted
// cluster-wide. A restarted node bumps its incarnation, so its fresh
// heartbeat counter is not mistaken for an old one.
//
// Liveness only affects routing. Ring membership is everything ever seen,
// minus nodes that LEFT, so a dead node's ranges are served by sloppy-quorum
// stand-ins (hinted handoff) rather than being re-assigned.
class Membership {
 public:
  using Listener = std::function<void(const NodeId&, v1::Member::State)>;

  Membership(NodeId self, std::string address, uint64_t incarnation, Runtime rt,
             MembershipOptions options);

  void AddSeed(const NodeId& id, const std::string& address);
  void Start();
  void Stop();
  void SetListener(Listener l);

  // PeerService handler for MESSAGE_TYPE_GOSSIP.
  Result<std::string> HandleGossip(std::string_view payload);

  [[nodiscard]] std::set<NodeId> AllNodes() const;   // ring membership
  [[nodiscard]] std::set<NodeId> DeadNodes() const;  // routing exclusions
  [[nodiscard]] bool IsAlive(const NodeId& id) const;
  [[nodiscard]] std::string AddressOf(const NodeId& id) const;
  [[nodiscard]] std::vector<v1::Member> Snapshot() const;
  [[nodiscard]] const NodeId& self() const { return self_; }

 private:
  struct Entry {
    v1::Member member;
    int64_t last_heard_ns = 0;
  };

  void Tick();
  void Merge(const v1::Member& remote, std::vector<std::pair<NodeId, v1::Member::State>>* changes);
  void Evaluate(std::vector<std::pair<NodeId, v1::Member::State>>* changes);
  void Notify(const std::vector<std::pair<NodeId, v1::Member::State>>& changes);
  [[nodiscard]] v1::GossipRequest Digest() const;

  const NodeId self_;
  const Runtime rt_;
  const MembershipOptions options_;
  mutable std::mutex mu_;
  std::map<NodeId, Entry> members_;
  std::mt19937_64 rng_;
  Listener listener_;
  bool running_ = false;
};

}  // namespace cartwheel::cluster
